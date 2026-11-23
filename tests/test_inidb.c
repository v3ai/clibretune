/* test_inidb.c - the ECU definitions bundled with the program (project/inidb.h): every file unpacks
 * byte for byte and parses, a sample per family opens in demo mode, signature parsing, resolve()
 * exact / near / none with real Speeduino, rusEFI, FOME and miata-ecus signatures, materialising into
 * the INI repository, the ECU's USB drive, and the headless app flows (connect -> project with the
 * right definition, the automatic switch on a signature mismatch, the New Project dialog).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdlib.h>
#include <string.h>
#include "lt_test.h"
#include "../src/project/inidb.h"
#include "../src/project/proj_priv.h"
#include "../src/app/app.h"
#include "../src/app/app_ext.h"
#include "../src/app/feat.h"
#include "../src/plat/plat.h"

#define TMP "build/test_tmp/inidb_"

static char *fresh(const char *name)
{
    char *p = (char *)lt_malloc(strlen(TMP) + strlen(name) + 1);
    strcpy(p, TMP);
    strcat(p, name);
    lt_proj_rmtree(p);
    lt_plat_mkdirs(p);
    return p;
}

/* every test runs against its own data/config/project directories */
static void use_test_dirs(void)
{
    char *d = fresh("defs");
    LT_CHECK(lt_plat_setenv("LIBRETUNE_DEFINITIONS", d) == 0);
    lt_free(d);
    d = fresh("projects");
    LT_CHECK(lt_plat_setenv("LIBRETUNE_PROJECTS", d) == 0);
    lt_free(d);
    LT_CHECK(lt_plat_setenv("LIBRETUNE_VOLUMES", "") == 0);     /* no real USB drives */
}

static const lt_inidb_def *find_def(const char *family, const char *board, const char *version)
{
    size_t i;
    for (i = 0; i < lt_inidb_count(); i++) {
        const lt_inidb_def *d = lt_inidb_at(i);
        if (strcmp(d->family, family) == 0 && strcmp(d->board, board) == 0 &&
            strncmp(d->version, version, strlen(version)) == 0) { return d; }
    }
    return NULL;
}

/* ---------------------------------------------------------------- catalog */
LT_TEST(catalog)
{
    size_t i, n = lt_inidb_count(), fam[4];
    memset(fam, 0, sizeof(fam));
    LT_CHECK(n >= 150);
    LT_CHECK(lt_inidb_at(n) == NULL);
    for (i = 0; i < n; i++) {
        const lt_inidb_def *d = lt_inidb_at(i);
        LT_CHECK(d->signature[0] != '\0' && d->file[0] != '\0' && d->version[0] != '\0');
        LT_CHECK(d->size > 1000UL && d->source[0] != '\0' && d->licence[0] != '\0');
        LT_CHECK(d->hist0 < (int)i && d->hist1 < (int)i);
        LT_CHECK(lt_inidb_index(d) == (long)i);
        if (strcmp(d->family, "speeduino") == 0) { fam[0]++; LT_CHECK(d->board[0] == '\0'); }
        else if (strcmp(d->family, "rusefi") == 0) { fam[1]++; LT_CHECK(d->board[0] != '\0'); }
        else if (strcmp(d->family, "fome") == 0) { fam[2]++; }
        else if (strcmp(d->family, "miata-ecus") == 0) { fam[3]++; }
        else { LT_CHECK(0); }
    }
    LT_CHECK(fam[0] >= 30 && fam[1] >= 100 && fam[2] >= 12 && fam[3] == 1);
    LT_CHECK(lt_inidb_index(NULL) == -1L);
    LT_CHECK_STR(lt_inidb_family_title("rusefi"), "rusEFI");
    LT_CHECK_STR(lt_inidb_family_title("fome"), "FOME");
    LT_CHECK(lt_inidb_filter(NULL, NULL, 0) == n);
    LT_CHECK(lt_inidb_filter("release_2026 proteus_f4", NULL, 0) == 2);    /* rusEFI, not FOME */
    LT_CHECK(lt_inidb_filter("proteus_f4", NULL, 0) == 4);
    LT_CHECK(lt_inidb_filter("FOME atlas", NULL, 0) == 1);
    LT_CHECK(lt_inidb_filter("no-such-board-anywhere", NULL, 0) == 0);
}

/* every bundled file unpacks to exactly what upstream ships (CRC; sha256 via the cache when
 * tools/fetch_definitions.py or --extract filled build/definitions/) and carries its signature */
LT_TEST(every_file_unpacks)
{
    size_t i, n = lt_inidb_count(), compared = 0;
    for (i = 0; i < n; i++) {
        const lt_inidb_def *d = lt_inidb_at(i);
        size_t len = 0, tl = 0;
        char *txt = lt_inidb_text(d, &len), *dec, *sig = NULL;
        LT_CHECK(txt != NULL && len == d->size);
        if (txt == NULL) { continue; }
        LT_CHECK(lt_crc32(0, txt, len) == d->crc32);
        dec = lt_ini_decode((const lt_u8 *)txt, len, &tl);
        LT_CHECK(dec != NULL && lt_ini_extract_info(dec, &sig, NULL) == LT_OK);
        if (sig != NULL) { LT_CHECK_STR(sig, d->signature); }
        lt_free(sig);
        lt_free(dec);
        {   /* byte for byte against the upstream copy, when present */
            char path[600];
            const char *base = d->board[0] != '\0' ? d->board : "";
            size_t fl = 0;
            char *f;
            if (strcmp(d->family, "speeduino") == 0) {
                lt_snprintf(path, sizeof(path), "build/definitions/speeduino/%s", d->file);
            } else if (strcmp(d->family, "miata-ecus") == 0) {
                lt_snprintf(path, sizeof(path), "build/definitions/miata-ecus/%s", d->file);
            } else {
                char ver[64], *sp;
                lt_strlcpy(ver, d->version, sizeof(ver));
                sp = strchr(ver, ' ');
                if (sp != NULL) { *sp = '\0'; }
                lt_snprintf(path, sizeof(path), "build/definitions/%s/%s/%s/%s", d->family, ver, base,
                            d->file);
            }
            f = lt_file_read(path, &fl);
            if (f != NULL) {
                LT_CHECK(fl == len && memcmp(f, txt, len) == 0);
                compared++;
            }
            lt_free(f);
        }
        lt_free(txt);
    }
    printf("  %lu of %lu files also compared with build/definitions/\n", (unsigned long)compared,
           (unsigned long)n);
}

/* every bundled file parses with the INI parser into a usable definition */
LT_TEST(every_file_parses)
{
    size_t i, n = lt_inidb_count();
    for (i = 0; i < n; i++) {
        const lt_inidb_def *d = lt_inidb_at(i);
        size_t len = 0, tl = 0;
        char *txt = lt_inidb_text(d, &len), *dec;
        lt_ini *ini;
        if (txt == NULL) { LT_CHECK(0); continue; }
        dec = lt_ini_decode((const lt_u8 *)txt, len, &tl);
        lt_free(txt);
        ini = dec != NULL ? lt_ini_load_string(dec, tl, NULL, NULL) : NULL;
        LT_CHECK(ini != NULL);
        if (ini == NULL) {
            fprintf(stderr, "  %s %s %s: %s\n", d->family, d->board, d->version, lt_err_get());
        } else {
            LT_CHECK(ini->signature != NULL && strcmp(ini->signature, d->signature) == 0);
            LT_CHECK(ini->n_constants > 50);
        }
        lt_ini_free(ini);
        lt_free(dec);
    }
}

LT_TEST(damaged_data_is_reported)
{
    lt_inidb_def bad;
    LT_CHECK(lt_inidb_text(NULL, NULL) == NULL);
    bad = *lt_inidb_at(0);
    LT_CHECK(lt_inidb_text(&bad, NULL) == NULL);        /* not a catalog entry */
}

/* ---------------------------------------------------------------- signatures */
LT_TEST(signature_parse)
{
    lt_sig_info si;
    LT_CHECK(lt_sig_parse("speeduino 202501", &si) == 0 && si.family == LT_SIGFAM_SPEEDUINO);
    LT_CHECK(si.when == 2025L * 12L);
    LT_CHECK(lt_sig_parse("speeduino 202402", &si) == 0 && si.when == 2024L * 12L + 1L);
    LT_CHECK(lt_sig_parse("rusEFI master.2026.08.13.proteus_f4.357168325", &si) == 0);
    LT_CHECK(si.family == LT_SIGFAM_RUSEFI && si.variant[0] == '\0');
    LT_CHECK_STR(si.branch, "master");
    LT_CHECK_STR(si.board, "proteus_f4");
    LT_CHECK(si.when == 2026L * 372L + 8L * 31L + 13L);
    LT_CHECK(lt_sig_parse("rusEFI (FOME) .2026.06.03.hellenNA8_96.3616644565", &si) == 0);
    LT_CHECK_STR(si.variant, "FOME");
    LT_CHECK_STR(si.branch, "");
    LT_CHECK_STR(si.board, "hellenNA8_96");
    LT_CHECK(lt_sig_parse("rusEFI main.2026.11.20.miata-ecu.282710243", &si) == 0);
    LT_CHECK_STR(si.board, "miata-ecu");
    LT_CHECK(lt_sig_parse("rusEFI 2021.07.18.proteus_f4.123", &si) == 0);   /* older: no branch */
    LT_CHECK_STR(si.board, "proteus_f4");
    LT_CHECK(lt_sig_parse("MS3 Format 0238.003 ", &si) == -1 && si.family == LT_SIGFAM_UNKNOWN);
    LT_CHECK(lt_sig_parse("rusEFI something", &si) == -1);
    LT_CHECK(lt_sig_parse("speeduino 2025", &si) == -1);
    LT_CHECK(lt_sig_parse(NULL, &si) == -1);
    LT_CHECK(lt_inidb_distance("speeduino 202501", "Speeduino  202501") == 0);
    LT_CHECK(lt_inidb_distance("speeduino 202404", "speeduino 202402") == 2);
    LT_CHECK(lt_inidb_distance("rusEFI master.2026.08.20.proteus_f4.1",
                               "rusEFI master.2026.08.13.proteus_f4.357168325") == 7);
    LT_CHECK(lt_inidb_distance("rusEFI master.2026.08.13.proteus_f7.1",
                               "rusEFI master.2026.08.13.proteus_f4.357168325") == -1);
    LT_CHECK(lt_inidb_distance("rusEFI (FOME) .2026.06.03.proteus_f4.1",
                               "rusEFI master.2026.06.03.proteus_f4.2") == -1);
    LT_CHECK(lt_inidb_distance("speeduino 202501", "rusEFI master.2026.08.13.x.1") == -1);
    LT_CHECK(lt_inidb_find("speeduino 202501", NULL, 0) == 3);   /* 202501.6, .5 and 202501 */
}

/* ---------------------------------------------------------------- resolve */
static int resolve(const char *sig, const lt_ini_repo *r, lt_inidb_pick *pk)
{
    return lt_inidb_resolve(sig, r, 0, pk);
}

static const lt_inidb_def *picked(const lt_inidb_pick *pk)
{
    return pk->src == LT_INIDB_SRC_BUNDLE ? lt_inidb_at((size_t)pk->bundle) : NULL;
}

LT_TEST(resolve_exact)
{
    lt_inidb_pick pk;
    const lt_inidb_def *d;
    LT_CHECK(resolve("speeduino 202501", NULL, &pk) == LT_INIDB_EXACT);
    d = picked(&pk);
    LT_CHECK(d != NULL && strcmp(d->version, "202501.6 to 202501.7") == 0);    /* newest first */
    LT_CHECK(pk.distance == 0 && strstr(pk.label, "Speeduino 202501.6") != NULL);
    LT_CHECK(resolve("speeduino 201711", NULL, &pk) == LT_INIDB_EXACT);          /* tag 201712 */
    LT_CHECK(picked(&pk) == find_def("speeduino", "", "201712"));
    LT_CHECK(resolve("rusEFI master.2026.08.13.proteus_f4.357168325", NULL, &pk) == LT_INIDB_EXACT);
    LT_CHECK(picked(&pk) == find_def("rusefi", "proteus_f4", "release_20260812"));
    LT_CHECK(resolve("rusEFI master.2025.10.18.proteus_f4.2530537040", NULL, &pk) == LT_INIDB_EXACT);
    LT_CHECK(picked(&pk) == find_def("rusefi", "proteus_f4", "release_20251018"));
    LT_CHECK(resolve("rusEFI (FOME) .2026.06.03.atlas.3685778166", NULL, &pk) == LT_INIDB_EXACT);
    LT_CHECK(picked(&pk) == find_def("fome", "atlas", "release_260501_02"));
    LT_CHECK(resolve("rusEFI main.2026.11.20.miata-ecu.282710243", NULL, &pk) == LT_INIDB_EXACT);
    LT_CHECK(picked(&pk) != NULL && strcmp(picked(&pk)->family, "miata-ecus") == 0);
    /* case and punctuation do not matter */
    LT_CHECK(resolve("SPEEDUINO 202402 ", NULL, &pk) == LT_INIDB_EXACT);
}

LT_TEST(resolve_near)
{
    lt_inidb_pick pk;
    /* a nightly build after the latest release: same board, closest date */
    LT_CHECK(resolve("rusEFI master.2026.09.30.proteus_f4.123456789", NULL, &pk) == LT_INIDB_NEAR);
    LT_CHECK(picked(&pk) == find_def("rusefi", "proteus_f4", "release_20260812"));
    LT_CHECK(pk.distance == 48L);
    /* between two releases: the closer one (Feb 20 is 62 days away, Oct 18 is 64) */
    LT_CHECK(resolve("rusEFI master.2025.12.20.uaefi.1", NULL, &pk) == LT_INIDB_NEAR);
    LT_CHECK(picked(&pk) == find_def("rusefi", "uaefi", "release_20260220_2"));
    LT_CHECK(resolve("rusEFI master.2025.11.01.uaefi.1", NULL, &pk) == LT_INIDB_NEAR);
    LT_CHECK(picked(&pk) == find_def("rusefi", "uaefi", "release_20251018"));
    /* an older build than anything bundled */
    LT_CHECK(resolve("rusEFI master.2024.01.05.hellen72.42", NULL, &pk) == LT_INIDB_NEAR);
    LT_CHECK(picked(&pk) == find_def("rusefi", "hellen72", "release_20251018"));
    /* FOME stays FOME */
    LT_CHECK(resolve("rusEFI (FOME) .2026.09.01.proteus_f4.5", NULL, &pk) == LT_INIDB_NEAR);
    LT_CHECK(picked(&pk) == find_def("fome", "proteus_f4", "release_260501_02"));
    LT_CHECK(resolve("rusEFI main.2026.12.24.miata-ecu.5", NULL, &pk) == LT_INIDB_NEAR);
    LT_CHECK(picked(&pk) != NULL && strcmp(picked(&pk)->family, "miata-ecus") == 0);
    /* Speeduino: closest release; equal distance goes to the newer one */
    LT_CHECK(resolve("speeduino 202404", NULL, &pk) == LT_INIDB_NEAR);
    LT_CHECK(picked(&pk) == find_def("speeduino", "", "202402.2"));
    LT_CHECK(resolve("speeduino 202602", NULL, &pk) == LT_INIDB_NEAR);
    LT_CHECK(picked(&pk) == find_def("speeduino", "", "202501.6"));
    LT_CHECK(resolve("speeduino 201601", NULL, &pk) == LT_INIDB_NEAR);
    LT_CHECK(picked(&pk) == find_def("speeduino", "", "201712"));
    LT_CHECK(resolve("speeduino 202206", NULL, &pk) == LT_INIDB_NEAR);     /* 202207 vs 202205? */
    LT_CHECK(picked(&pk) == find_def("speeduino", "", "202207"));
}

LT_TEST(resolve_none)
{
    lt_inidb_pick pk;
    LT_CHECK(resolve("MS3 Format 0238.003 ", NULL, &pk) == LT_INIDB_NONE);
    LT_CHECK(pk.src == LT_INIDB_SRC_NONE && pk.bundle == -1L && pk.label[0] == '\0');
    LT_CHECK(resolve("rusEFI master.2026.08.13.no_such_board.1", NULL, &pk) == LT_INIDB_NONE);
    LT_CHECK(resolve("rusEFI (FOME) .2026.06.03.uaefi.1", NULL, &pk) == LT_INIDB_NONE);
    LT_CHECK(resolve("", NULL, &pk) == LT_INIDB_NONE);
    LT_CHECK(resolve(NULL, NULL, &pk) == LT_INIDB_NONE);
}

/* ---------------------------------------------------------------- repository */
LT_TEST(materialise_and_repo_first)
{
    char *dir = fresh("repo"), *id = NULL, *id2 = NULL, *path, *f;
    const lt_inidb_def *d = find_def("rusefi", "hellen72", "release_20260812");
    lt_ini_repo r;
    lt_inidb_pick pk;
    size_t len = 0, fl = 0, tl = 0;
    char *txt, *dec;
    LT_CHECK(d != NULL);
    if (d == NULL) { lt_free(dir); return; }
    LT_CHECK(lt_ini_repo_open(&r, dir) == 0);
    LT_CHECK(lt_inidb_materialise(&r, d, &id) == 0 && id != NULL);
    LT_CHECK(r.n == 1 && strcmp(r.e[0].signature, d->signature) == 0);
    LT_CHECK(strstr(r.e[0].source, "bundled") != NULL);
    path = lt_ini_repo_path(&r, id);
    f = path != NULL ? lt_file_read(path, &fl) : NULL;
    txt = lt_inidb_text(d, &len);
    dec = txt != NULL ? lt_ini_decode((const lt_u8 *)txt, len, &tl) : NULL;
    LT_CHECK(f != NULL && dec != NULL && fl == tl && memcmp(f, dec, tl) == 0);
    lt_free(f); lt_free(txt); lt_free(dec); lt_free(path);
    /* a second time is a no-op that returns the same id */
    LT_CHECK(lt_inidb_materialise(&r, d, &id2) == 0 && id2 != NULL && strcmp(id, id2) == 0);
    LT_CHECK(r.n == 1);
    /* exact: the repository is asked before the bundle */
    LT_CHECK(lt_inidb_resolve(d->signature, &r, 0, &pk) == LT_INIDB_EXACT);
    LT_CHECK(pk.src == LT_INIDB_SRC_REPO && strcmp(pk.repo_id, id) == 0);
    LT_CHECK(strstr(pk.label, "INI library") != NULL);
    /* near: on a tie the repository wins too */
    LT_CHECK(lt_inidb_resolve("rusEFI master.2026.08.20.hellen72.1", &r, 0, &pk) == LT_INIDB_NEAR);
    LT_CHECK(pk.src == LT_INIDB_SRC_REPO && pk.distance == 7L);
    /* pick_path: repository entries as they are, bundled ones unpacked */
    LT_CHECK(lt_inidb_pick_path(&r, &pk, &path) == 0 && path != NULL && lt_file_exists(path));
    lt_free(path);
    LT_CHECK(lt_inidb_resolve("speeduino 202305", &r, 0, &pk) == LT_INIDB_EXACT);
    LT_CHECK(pk.src == LT_INIDB_SRC_BUNDLE);
    LT_CHECK(lt_inidb_pick_path(&r, &pk, &path) == 0 && path != NULL && lt_file_exists(path));
    LT_CHECK(r.n == 2);
    lt_free(path);
    LT_CHECK(lt_inidb_resolve("nothing", &r, 0, &pk) == LT_INIDB_NONE);
    LT_CHECK(lt_inidb_pick_path(&r, &pk, &path) == LT_ENOENT && path == NULL);
    lt_ini_repo_close(&r);
    /* the index on disk carries the new entries */
    LT_CHECK(lt_ini_repo_open(&r, dir) == 0 && r.n == 2);
    lt_ini_repo_close(&r);
    lt_free(id); lt_free(id2); lt_free(dir);
}

/* ---------------------------------------------------------------- USB drive */
LT_TEST(usb_drive)
{
    char *vol = fresh("vol_ini"), *vol7 = fresh("vol_7z"), *empty = fresh("vol_empty");
    char env[1600], path[512], *f, *repo_dir = fresh("repo_drive"), *got = NULL;
    const lt_inidb_def *d = find_def("rusefi", "proteus_f7", "release_20260812");
    lt_inidb_pick pk;
    lt_ini_repo r;
    size_t len = 0;
    char *txt;
    int comp = -1;
    LT_CHECK(d != NULL);
    if (d == NULL) { return; }
    txt = lt_inidb_text(d, &len);
    /* the compressed-image layout: the plain INI next to README and .url files */
    f = lt_path_join(vol, "rusefi_proteus_f7.ini");
    LT_CHECK(txt != NULL && lt_plat_write_atomic(f, txt, len) == 0);
    lt_free(f);
    f = lt_path_join(vol, "README-2026-08-13.txt");
    LT_CHECK(lt_plat_write_atomic(f, "Welcome to rusEFI!\n", 19) == 0);
    lt_free(f);
    /* the uncompressed image: only a 7-Zip archive */
    f = lt_path_join(vol7, "rusefi.ini.7z");
    LT_CHECK(lt_plat_write_atomic(f, "7z\274\257\047\034", 6) == 0);
    lt_free(f);
    lt_free(txt);

    lt_snprintf(env, sizeof(env), "%s;%s", empty, vol7);
    LT_CHECK(lt_plat_setenv("LIBRETUNE_VOLUMES", env) == 0);
    LT_CHECK(lt_inidb_drive_find(d->signature, path, sizeof(path), &comp) == 0 && comp == 1);
    LT_CHECK(lt_inidb_resolve(d->signature, NULL, LT_INIDB_USE_DRIVE, &pk) == LT_INIDB_EXACT);
    LT_CHECK(pk.src == LT_INIDB_SRC_BUNDLE && pk.drive_compressed == 1);

    lt_snprintf(env, sizeof(env), "%s;%s;%s", empty, vol7, vol);
    LT_CHECK(lt_plat_setenv("LIBRETUNE_VOLUMES", env) == 0);
    LT_CHECK(lt_inidb_drive_find(d->signature, path, sizeof(path), &comp) == 1 && comp == 0);
    LT_CHECK(strstr(path, "rusefi_proteus_f7.ini") != NULL);
    LT_CHECK(lt_inidb_drive_find("rusEFI master.2026.08.13.proteus_f4.357168325", path,
                                 sizeof(path), NULL) == 0);
    LT_CHECK(lt_inidb_resolve(d->signature, NULL, LT_INIDB_USE_DRIVE, &pk) == LT_INIDB_EXACT);
    LT_CHECK(pk.src == LT_INIDB_SRC_DRIVE && strstr(pk.label, "USB drive") != NULL);
    /* without the flag the drive is not looked at */
    LT_CHECK(lt_inidb_resolve(d->signature, NULL, 0, &pk) == LT_INIDB_EXACT &&
             pk.src == LT_INIDB_SRC_BUNDLE);
    /* the drive's INI is kept in the INI library */
    LT_CHECK(lt_inidb_resolve(d->signature, NULL, LT_INIDB_USE_DRIVE, &pk) == LT_INIDB_EXACT);
    LT_CHECK(lt_ini_repo_open(&r, repo_dir) == 0);
    LT_CHECK(lt_inidb_pick_path(&r, &pk, &got) == 0 && got != NULL && r.n == 1);
    LT_CHECK(r.n == 1 && strstr(r.e[0].source, "rusefi_proteus_f7.ini") != NULL);
    lt_ini_repo_close(&r);
    lt_free(got);
    LT_CHECK(lt_plat_setenv("LIBRETUNE_VOLUMES", "") == 0);
    LT_CHECK(lt_inidb_drive_find(d->signature, path, sizeof(path), &comp) == 0 && comp == 0);
    lt_free(vol); lt_free(vol7); lt_free(empty); lt_free(repo_dir);
}

/* ---------------------------------------------------------------- app flows */
static void frame(lt_app *app)
{
    if (app->ui == NULL) {          /* the first frame sets up a headless surface */
        (void)lt_app_screenshot(app, "build/test_inidb_frame.ppm", 1000, 760, 1);
        return;
    }
    lt_ui_begin(app->ui, &app->gfx, lt_plat_now_ms());
    lt_app_frame(app);
    lt_ui_end(app->ui);
}

static int n_dialogs_of(const lt_app *app, const char *kind)
{
    int i, n = 0;
    for (i = 0; i < app->n_dialogs; i++) {
        if (strcmp(app->dialogs[i]->kind, kind) == 0) { n++; }
    }
    return n;
}

/* the project's definition.ini is the bundled file (as the INI repository stores it) */
static lt_bool project_uses(const lt_app *app, const lt_inidb_def *d)
{
    char *p, *f, *txt, *dec;
    size_t fl = 0, len = 0, tl = 0;
    lt_bool same;
    if (!app->has_project || d == NULL) { return LT_FALSE; }
    p = lt_project_ini_path(&app->project);
    f = lt_file_read(p, &fl);
    txt = lt_inidb_text(d, &len);
    dec = txt != NULL ? lt_ini_decode((const lt_u8 *)txt, len, &tl) : NULL;
    same = f != NULL && dec != NULL && fl == tl && memcmp(f, dec, tl) == 0;
    lt_free(p); lt_free(f); lt_free(txt); lt_free(dec);
    return same;
}

/* one sample per family opens in demo mode */
LT_TEST(demo_per_family)
{
    static const char *const fams[] = { "speeduino", "rusefi", "fome", "miata-ecus" };
    char *dir = fresh("repo_demo");
    lt_ini_repo r;
    size_t k, i;
    LT_CHECK(lt_ini_repo_open(&r, dir) == 0);
    for (k = 0; k < LT_COUNTOF(fams); k++) {
        const lt_inidb_def *d = NULL;
        char *id = NULL, *path;
        lt_app *app;
        for (i = 0; i < lt_inidb_count() && d == NULL; i++) {
            if (strcmp(lt_inidb_at(i)->family, fams[k]) == 0) { d = lt_inidb_at(i); }
        }
        LT_CHECK(d != NULL && lt_inidb_materialise(&r, d, &id) == 0);
        path = id != NULL ? lt_ini_repo_path(&r, id) : NULL;
        app = lt_app_new();
        if (path == NULL || lt_app_start_demo(app, path) != 0) {
            fprintf(stderr, "  demo %s %s: %s\n", d != NULL ? d->family : "?",
                    path != NULL ? path : "(no path)", lt_err_get());
            LT_CHECK(0);
        }
        LT_CHECK(app->ini != NULL && app->conn == LT_APP_DEMO && app->mem != NULL);
        LT_CHECK(strcmp(app->signature, d->signature) == 0);
        (void)lt_app_service(app);
        lt_app_free(app);
        lt_free(path);
        lt_free(id);
    }
    lt_ini_repo_close(&r);
    lt_free(dir);
}

/* lt_feat_sync_connect_auto: connect (demo answering with a bundled signature), identify, pick
 * the definition, create the project; again: the same project is opened */
/* a release binary without demo.ini beside it: "Try the demo" runs on a bundled definition
 * instead of asking for an INI file the user does not have */
LT_TEST(demo_without_demo_ini)
{
    lt_app *app;
    char buf[512];
    const char *path;
    use_test_dirs();
    app = lt_app_new();
    lt_free(app->config_dir);
    app->config_dir = fresh("cfg");
    path = lt_app_bundled_demo_ini(app, buf, sizeof(buf));
    LT_CHECK(path != NULL && lt_file_exists(path));
    LT_CHECK(path != NULL && lt_app_start_demo(app, path) == 0);
    LT_CHECK(app->ini != NULL && app->conn == LT_APP_DEMO);
    LT_CHECK(app->ini != NULL && app->ini->signature != NULL &&
             strstr(app->ini->signature, "proteus_f4") != NULL);
    lt_free(app->config_dir);
    app->config_dir = NULL;            /* nowhere to write it: no path */
    LT_CHECK(lt_app_bundled_demo_ini(app, buf, sizeof(buf)) == NULL);
    lt_app_free(app);
}

LT_TEST(connect_auto_flow)
{
    lt_app *app;
    lt_feat_auto res;
    char first[512];
    const lt_inidb_def *d = find_def("speeduino", "", "202402.2");
    const lt_inidb_def *r0 = find_def("rusefi", "proteus_f4", "release_20260812");
    use_test_dirs();
    app = lt_app_new();
    LT_CHECK(lt_feat_sync_connect_auto(app, "demo", 0, "speeduino 202402", &res) == LT_INIDB_EXACT);
    LT_CHECK(res.quality == LT_INIDB_EXACT && res.created == 1);
    LT_CHECK_STR(res.signature, "speeduino 202402");
    LT_CHECK(app->has_project && app->conn == LT_APP_DEMO && project_uses(app, d));
    LT_CHECK_STR(app->ini->signature, "speeduino 202402");
    LT_CHECK_STR(app->project.cfg.name, "Speeduino 202402");
    LT_CHECK(n_dialogs_of(app, "sig_mismatch") == 0 && n_dialogs_of(app, "inidb_near") == 0);
    lt_strlcpy(first, res.project, sizeof(first));
    lt_app_free(app);

    /* the same ECU again: its project is opened, not a second one created */
    app = lt_app_new();
    LT_CHECK(lt_feat_sync_connect_auto(app, "demo", 0, "speeduino 202402", &res) == LT_INIDB_EXACT);
    LT_CHECK(res.created == 0 && strcmp(res.project, first) == 0);
    lt_app_free(app);

    /* a rusEFI nightly: the closest build of the board, with the warning and its action */
    app = lt_app_new();
    LT_CHECK(lt_feat_sync_connect_auto(app, "demo", 0, "rusEFI master.2026.09.30.proteus_f4.77",
                                       &res) == LT_INIDB_NEAR);
    LT_CHECK(res.created == 1 && project_uses(app, r0));
    LT_CHECK_STR(app->project.cfg.name, "rusEFI proteus_f4");
    LT_CHECK(n_dialogs_of(app, "inidb_near") == 1);
    LT_CHECK(strstr(res.label, "release_20260812") != NULL);
    frame(app);                     /* the warning draws */
    lt_app_free(app);

    /* nothing fits: no project, the wizard opens at its definition step */
    app = lt_app_new();
    LT_CHECK(lt_feat_sync_connect_auto(app, "demo", 0, "MS3 Format 0238.003", &res) == LT_ENOENT);
    LT_CHECK(!app->has_project && n_dialogs_of(app, "connect_wizard") == 1);
    frame(app);
    lt_app_free(app);

    app = lt_app_new();
    LT_CHECK(lt_feat_sync_connect_auto(app, "", 0, "speeduino 202402", &res) == LT_EINVAL);
    LT_CHECK(lt_feat_sync_connect_auto(app, "demo", 0, NULL, &res) == LT_EINVAL);
    lt_app_free(app);
}

/* the ECU's signature differs from the open project's definition */
LT_TEST(mismatch_switches_automatically)
{
    lt_app *app;
    lt_feat_auto res;
    char dir[512];
    const lt_inidb_def *d = find_def("speeduino", "", "202310");
    const lt_inidb_def *r0 = find_def("rusefi", "uaefi", "release_20260812");
    use_test_dirs();
    /* resolved for this ECU by connect_auto already: a later mismatch is left to the user */
    app = lt_app_new();
    LT_CHECK(lt_feat_sync_connect_auto(app, "demo", 0, "speeduino 202310", &res) == LT_INIDB_EXACT);
    LT_CHECK(project_uses(app, d));
    lt_free(app->ini->signature);
    app->ini->signature = lt_strdup("speeduino 201905");
    LT_CHECK(lt_proto_signature_mismatch(app->proto));
    app->n_dialogs = 0;
    lt_ext_on_connect(app);
    LT_CHECK(n_dialogs_of(app, "sig_mismatch") == 1);
    lt_strlcpy(dir, res.project, sizeof(dir));
    lt_app_free(app);

    /* exact: opening the project in a fresh session, the bundled definition replaces it silently */
    app = lt_app_new();
    LT_CHECK(lt_app_open_project(app, dir) == 0);
    LT_CHECK(lt_app_connect_demo(app) == 0);
    lt_free(app->ini->signature);
    app->ini->signature = lt_strdup("speeduino 201905");     /* as if the firmware was updated */
    app->n_dialogs = 0;
    lt_ext_on_connect(app);
    LT_CHECK(n_dialogs_of(app, "sig_mismatch") == 0 && n_dialogs_of(app, "inidb_near") == 0);
    LT_CHECK(app->ini != NULL && app->ini->signature != NULL &&
             strcmp(app->ini->signature, "speeduino 202310") == 0);
    LT_CHECK(project_uses(app, d) && app->conn == LT_APP_DEMO);
    LT_CHECK(!lt_proto_signature_mismatch(app->proto));
    LT_CHECK_STR(app->project.cfg.signature, "speeduino 202310");
    lt_app_free(app);

    /* near: the ECU runs a later nightly; the open definition is the nearest, it stays and warns */
    LT_CHECK(r0 != NULL);
    if (r0 == NULL) { return; }
    app = lt_app_new();
    LT_CHECK(lt_feat_sync_connect_auto(app, "demo", 0, r0->signature, &res) == LT_INIDB_EXACT);
    lt_strlcpy(dir, res.project, sizeof(dir));
    lt_app_free(app);
    app = lt_app_new();
    LT_CHECK(lt_app_open_project(app, dir) == 0);
    LT_CHECK(lt_app_connect_demo(app) == 0);
    lt_strlcpy(app->signature, "rusEFI master.2026.09.02.uaefi.11", sizeof(app->signature));
    lt_free(app->ini->signature);                      /* (so that the link sees a mismatch) */
    app->ini->signature = lt_strdup("rusEFI master.2026.08.13.uaefi.999");
    app->n_dialogs = 0;
    lt_ext_on_connect(app);
    LT_CHECK(n_dialogs_of(app, "sig_mismatch") == 0 && n_dialogs_of(app, "inidb_near") == 1);
    frame(app);
    lt_app_free(app);

    /* none: the mismatch dialog as before */
    app = lt_app_new();
    LT_CHECK(lt_app_open_project(app, dir) == 0);
    LT_CHECK(lt_app_connect_demo(app) == 0);
    lt_strlcpy(app->signature, "MS3 Format 0238.003", sizeof(app->signature));
    lt_free(app->ini->signature);
    app->ini->signature = lt_strdup("rusEFI master.2026.08.13.uaefi.999");
    app->n_dialogs = 0;
    lt_ext_on_connect(app);
    LT_CHECK(n_dialogs_of(app, "sig_mismatch") == 1);
    frame(app);
    lt_app_free(app);
}

/* the wizard opened on an identified ECU, the New Project dialog with its bundled tree */
LT_TEST(dialogs_draw)
{
    lt_app *app;
    use_test_dirs();
    app = lt_app_new();
    (void)lt_app_dialog_push(app, "connect_wizard", "demo\t0\trusEFI master.2026.09.30.proteus_f4.77",
                             "Connect ECU");
    (void)lt_app_screenshot(app, "build/test_inidb_wizard.ppm", 1000, 760, 3);
    LT_CHECK(n_dialogs_of(app, "connect_wizard") == 1);
    app->dialogs[0]->open = 0;
    frame(app);
    LT_CHECK(app->n_dialogs == 0);
    lt_app_free(app);
    app = lt_app_new();
    (void)lt_app_dialog_push(app, "newproject", "", "New Project");
    (void)lt_app_screenshot(app, "build/test_inidb_newproj.ppm", 1000, 760, 3);
    LT_CHECK(n_dialogs_of(app, "newproject") == 1);
    lt_app_free(app);
    app = lt_app_new();             /* with a search: the matching groups open */
    (void)lt_app_dialog_push(app, "newproject", "hellen nb", "New Project");
    (void)lt_app_screenshot(app, "build/test_inidb_newproj_search.ppm", 1000, 760, 3);
    LT_CHECK(n_dialogs_of(app, "newproject") == 1);
    lt_app_free(app);
}

int main(void)
{
    use_test_dirs();
    LT_RUN(catalog);
    LT_RUN(every_file_unpacks);
    LT_RUN(every_file_parses);
    LT_RUN(damaged_data_is_reported);
    LT_RUN(signature_parse);
    LT_RUN(resolve_exact);
    LT_RUN(resolve_near);
    LT_RUN(resolve_none);
    LT_RUN(materialise_and_repo_first);
    LT_RUN(usb_drive);
    LT_RUN(demo_per_family);
    LT_RUN(demo_without_demo_ini);
    LT_RUN(connect_auto_flow);
    LT_RUN(mismatch_switches_automatically);
    LT_RUN(dialogs_draw);
    LT_TEST_MAIN_END
}
