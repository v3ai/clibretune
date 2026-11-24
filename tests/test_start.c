/* test_start.c - the start screen's recent-project store (recent.conf): touch / remove / order,
 * save and load, reading names from project.json, ECU names from signatures and "last opened"
 * texts.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "lt_test.h"
#include "../src/app/recent_logic.h"
#include "../src/plat/plat.h"

static void put(const char *path, const char *val)
{
    char *d = lt_strdup(path), *sl = strrchr(d, '/');
    if (sl != NULL) { *sl = '\0'; }
    lt_plat_mkdirs(d);
    LT_CHECK(lt_file_write(path, val, strlen(val)) == 0);
    lt_free(d);
}

LT_TEST(test_store)
{
    const char *dir = "build/test_start_cfg";
    lt_recent r, r2;
    char *f = lt_path_join(dir, "recent.conf");
    /* stored paths are absolute (lt_plat_abspath), so the test uses absolute ones too */
    char *pa = lt_plat_abspath("build/test_start_p/a"), *pb = lt_plat_abspath("build/test_start_p/b");
    lt_plat_mkdirs(dir);
    if (lt_file_exists(f)) { lt_plat_remove(f); }
    lt_recent_init(&r);
    LT_CHECK(lt_recent_load(&r, dir) == 0);          /* missing file: empty */
    LT_CHECK_INT(r.n, 0);
    lt_recent_touch(&r, pa, "Alpha", "rusEFI master.2024.05.03.proteus_f4.1", "2026-11-01 10:00:00");
    lt_recent_touch(&r, pb, "Beta", "speeduino 202402", "2026-11-02 10:00:00");
    lt_recent_touch(&r, pa, "Alpha 2", "rusEFI master.2024.05.03.proteus_f4.1", "2026-11-03 10:00:00");
    LT_CHECK_INT(r.n, 2);
    LT_CHECK_STR(r.e[0].path, pa);                   /* newest first, no duplicate */
    LT_CHECK_STR(r.e[0].name, "Alpha 2");
    LT_CHECK_STR(r.e[1].path, pb);
    LT_CHECK(lt_recent_save(&r, dir) == 0);
    lt_recent_init(&r2);
    LT_CHECK(lt_recent_load(&r2, dir) == 0);
    LT_CHECK_INT(r2.n, 2);
    LT_CHECK(lt_recent_find(&r2, pb) != NULL);
    if (lt_recent_find(&r2, pb) != NULL) {
        LT_CHECK_STR(lt_recent_find(&r2, pb)->opened, "2026-11-02 10:00:00");
        LT_CHECK_STR(lt_recent_find(&r2, pb)->signature, "speeduino 202402");
    }
    LT_CHECK_INT(lt_recent_remove(&r2, pa), 1);
    LT_CHECK_INT(lt_recent_remove(&r2, pa), 0);
    LT_CHECK_INT(r2.n, 1);
    LT_CHECK(lt_recent_find(&r2, pa) == NULL);
    lt_recent_free(&r2);
    lt_recent_free(&r);
    lt_free(f); lt_free(pa); lt_free(pb);
}

/* recent.conf of an older build: relative paths are resolved on load and two spellings of one
 * folder become one entry (the first, newest one keeps its details) */
LT_TEST(test_migrate)
{
    const char *dir = "build/test_start_mig";
    const char *conf = "p0.path=build/test_start_m/proj\n"
                       "p0.name=Newest\n"
                       "p0.opened=2026-11-03 10:00:00\n"
                       "p1.path=build/test_start_m/x/../proj/\n"
                       "p1.name=Older spelling\n"
                       "p2.path=build/test_start_m/./other\n"
                       "p2.name=Other\n";
    char *f = lt_path_join(dir, "recent.conf");
    char *want = lt_plat_abspath("build/test_start_m/proj"), *other = lt_plat_abspath("build/test_start_m/other");
    lt_recent r;
    lt_plat_mkdirs(dir);
    LT_CHECK(lt_file_write(f, conf, strlen(conf)) == 0);
    lt_recent_init(&r);
    LT_CHECK(lt_recent_load(&r, dir) == 0);
    LT_CHECK_INT(r.n, 2);
    if (r.n == 2) {
        LT_CHECK_STR(r.e[0].path, want);
        LT_CHECK_STR(r.e[0].name, "Newest");
        LT_CHECK_STR(r.e[0].opened, "2026-11-03 10:00:00");
        LT_CHECK_STR(r.e[1].path, other);
        LT_CHECK_STR(r.e[1].name, "Other");
    }
    lt_recent_free(&r);
    lt_free(f); lt_free(want); lt_free(other);
}

LT_TEST(test_cap)
{
    lt_recent r;
    char path[32];
    int i;
    lt_recent_init(&r);
    for (i = 0; i < 40; i++) {
        lt_snprintf(path, sizeof(path), "/p/%d", i);
        lt_recent_touch(&r, path, "", "", "");
    }
    LT_CHECK_INT(r.n, 32);
    LT_CHECK_STR(r.e[0].path, "/p/39");
    LT_CHECK_STR(r.e[31].path, "/p/8");
    lt_recent_free(&r);
}

LT_TEST(test_read_project)
{
    char name[96], sig[96];
    put("build/test_start_proj/project.json",
        "{\"version\":\"1.0\",\"name\":\"My Miata\",\"signature\":\"rusEFI master.2025.01.01.uaefi.9\"}");
    LT_CHECK(lt_recent_read_project("build/test_start_proj", name, sizeof(name), sig, sizeof(sig)) == 0);
    LT_CHECK_STR(name, "My Miata");
    LT_CHECK_STR(sig, "rusEFI master.2025.01.01.uaefi.9");
    LT_CHECK(lt_recent_read_project("build/test_start_nothing", name, sizeof(name), sig, sizeof(sig)) != 0);
    LT_CHECK_STR(name, "");
}

LT_TEST(test_ecu_label)
{
    char b[96];
    lt_recent_ecu_label("rusEFI master.2024.05.03.proteus_f4.1234", b, sizeof(b));
    LT_CHECK_STR(b, "rusEFI (proteus_f4)");
    lt_recent_ecu_label("rusEFI 2025062409.2025.06.26.epicECU.2089451084", b, sizeof(b));
    LT_CHECK_STR(b, "rusEFI (epicECU)");
    lt_recent_ecu_label("FOME 2024.10.01.hellen72.1", b, sizeof(b));
    LT_CHECK_STR(b, "FOME (hellen72)");
    lt_recent_ecu_label("speeduino 202402", b, sizeof(b));
    LT_CHECK_STR(b, "Speeduino 2024.02");
    lt_recent_ecu_label("speeduino 202501-dev", b, sizeof(b));
    LT_CHECK_STR(b, "Speeduino 2025.01");
    lt_recent_ecu_label("MS3 Format 0566.05", b, sizeof(b));
    LT_CHECK_STR(b, "MegaSquirt-III");
    lt_recent_ecu_label("MS2Extra comms342hP", b, sizeof(b));
    LT_CHECK_STR(b, "MS2/Extra");
    lt_recent_ecu_label("", b, sizeof(b));
    LT_CHECK_STR(b, "");
    lt_recent_ecu_label("Some Very Long Custom Firmware Signature 1.2.3 build 4567", b, sizeof(b));
    LT_CHECK_INT((int)strlen(b), 40);
    LT_CHECK(strcmp(b + 37, "...") == 0);
}

LT_TEST(test_age)
{
    char b[48];
    lt_recent_age("2026-11-20 14:02:11", "2026-11-20 18:00:00", b, sizeof(b));
    LT_CHECK_STR(b, "Today 14:02");
    lt_recent_age("2026-11-19 09:30:00", "2026-11-20 08:00:00", b, sizeof(b));
    LT_CHECK_STR(b, "Yesterday 09:30");
    lt_recent_age("2026-02-27 09:30:00", "2026-03-02 08:00:00", b, sizeof(b));   /* across February */
    LT_CHECK_STR(b, "3 days ago");
    lt_recent_age("2025-12-31 23:59:00", "2026-01-01 00:01:00", b, sizeof(b));   /* across a year */
    LT_CHECK_STR(b, "Yesterday 23:59");
    lt_recent_age("2025-03-12 10:00:00", "2026-11-20 08:00:00", b, sizeof(b));
    LT_CHECK_STR(b, "12 Mar 2025");
    lt_recent_age("", "2026-11-20 08:00:00", b, sizeof(b));
    LT_CHECK_STR(b, "");
    lt_recent_age("garbage", "2026-11-20 08:00:00", b, sizeof(b));
    LT_CHECK_STR(b, "");
}

int main(void)
{
    LT_RUN(test_store);
    LT_RUN(test_migrate);
    LT_RUN(test_cap);
    LT_RUN(test_read_project);
    LT_RUN(test_ecu_label);
    LT_RUN(test_age);
    LT_TEST_MAIN_END
}
