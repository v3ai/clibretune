/* recent_logic.c - recent project details for the start screen (see recent_logic.h).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdlib.h>
#include <string.h>
#include "recent_logic.h"
#include "../project/project.h"
#include "../json/json.h"

#define RECENT_MAX 32

void lt_recent_init(lt_recent *r)
{
    r->e = NULL;
    r->n = 0;
}

void lt_recent_free(lt_recent *r)
{
    lt_free(r->e);
    lt_recent_init(r);
}

static char *recent_path(const char *dir)
{
    return dir != NULL ? lt_path_join(dir, "recent.conf") : NULL;
}

int lt_recent_load(lt_recent *r, const char *config_dir)
{
    lt_props p;
    char *path = recent_path(config_dir);
    char key[32];
    int i;
    lt_recent_free(r);
    if (path == NULL) { return -1; }
    r->e = (lt_recent_ent *)lt_calloc(RECENT_MAX, sizeof(lt_recent_ent));
    lt_props_init(&p);
    if (lt_file_exists(path) && lt_props_load(&p, path) == 0) {
        for (i = 0; i < RECENT_MAX; i++) {
            const char *v;
            lt_recent_ent *e;
            lt_snprintf(key, sizeof(key), "p%d.path", i);
            v = lt_props_get(&p, key);
            if (v == NULL || v[0] == '\0') { continue; }
            e = &r->e[r->n++];
            lt_strlcpy(e->path, v, sizeof(e->path));
            lt_snprintf(key, sizeof(key), "p%d.name", i);
            v = lt_props_get(&p, key);
            lt_strlcpy(e->name, v != NULL ? v : "", sizeof(e->name));
            lt_snprintf(key, sizeof(key), "p%d.signature", i);
            v = lt_props_get(&p, key);
            lt_strlcpy(e->signature, v != NULL ? v : "", sizeof(e->signature));
            lt_snprintf(key, sizeof(key), "p%d.opened", i);
            v = lt_props_get(&p, key);
            lt_strlcpy(e->opened, v != NULL ? v : "", sizeof(e->opened));
        }
    }
    lt_props_free(&p);
    lt_free(path);
    return 0;
}

int lt_recent_save(const lt_recent *r, const char *config_dir)
{
    lt_props p;
    char *path = recent_path(config_dir);
    char key[32];
    int i, rc;
    if (path == NULL) { return -1; }
    lt_props_init(&p);
    for (i = 0; i < r->n; i++) {
        lt_snprintf(key, sizeof(key), "p%d.path", i);
        lt_props_set(&p, key, r->e[i].path);
        lt_snprintf(key, sizeof(key), "p%d.name", i);
        lt_props_set(&p, key, r->e[i].name);
        lt_snprintf(key, sizeof(key), "p%d.signature", i);
        lt_props_set(&p, key, r->e[i].signature);
        lt_snprintf(key, sizeof(key), "p%d.opened", i);
        lt_props_set(&p, key, r->e[i].opened);
    }
    rc = lt_props_save(&p, path);
    lt_props_free(&p);
    lt_free(path);
    return rc;
}

const lt_recent_ent *lt_recent_find(const lt_recent *r, const char *path)
{
    int i;
    for (i = 0; i < r->n; i++) {
        if (strcmp(r->e[i].path, path) == 0) { return &r->e[i]; }
    }
    return NULL;
}

void lt_recent_touch(lt_recent *r, const char *path, const char *name, const char *signature,
                     const char *stamp)
{
    lt_recent_ent e;
    int i, at = -1;
    if (r->e == NULL) { r->e = (lt_recent_ent *)lt_calloc(RECENT_MAX, sizeof(lt_recent_ent)); }
    memset(&e, 0, sizeof(e));
    lt_strlcpy(e.path, path, sizeof(e.path));
    lt_strlcpy(e.name, name != NULL ? name : "", sizeof(e.name));
    lt_strlcpy(e.signature, signature != NULL ? signature : "", sizeof(e.signature));
    lt_strlcpy(e.opened, stamp != NULL ? stamp : "", sizeof(e.opened));
    for (i = 0; i < r->n; i++) {
        if (strcmp(r->e[i].path, path) == 0) { at = i; break; }
    }
    if (at < 0) { at = r->n < RECENT_MAX ? r->n++ : RECENT_MAX - 1; }
    for (i = at; i > 0; i--) { r->e[i] = r->e[i - 1]; }
    r->e[0] = e;
}

int lt_recent_remove(lt_recent *r, const char *path)
{
    int i, k;
    for (i = 0; i < r->n; i++) {
        if (strcmp(r->e[i].path, path) == 0) {
            for (k = i; k < r->n - 1; k++) { r->e[k] = r->e[k + 1]; }
            r->n--;
            return 1;
        }
    }
    return 0;
}

int lt_recent_read_project(const char *dir, char *name, size_t ncap, char *sig, size_t scap)
{
    char *p = lt_path_join(dir, "project.json");
    lt_json *j = p != NULL && lt_file_exists(p) ? lt_json_load(p, NULL, 0) : NULL;
    lt_free(p);
    if (ncap > 0) { name[0] = '\0'; }
    if (scap > 0) { sig[0] = '\0'; }
    if (j == NULL) { return -1; }
    lt_strlcpy(name, lt_json_get_str(j, "name", ""), ncap);
    lt_strlcpy(sig, lt_json_get_str(j, "signature", ""), scap);
    lt_json_free(j);
    return 0;
}

/* ------------------------------------------------------------------ */
static int starts_ci(const char *s, const char *p)
{
    return lt_strnicmp(s, p, strlen(p)) == 0;
}

/* The board of "rusEFI master.2024.05.03.proteus_f4.1234" / "FOME 2024.10.01.hellen72.1": the
 * last '.'-separated field after the first one that has a letter in it. */
static void board_field(const char *s, char *out, size_t cap)
{
    const char *f = strchr(s, '.');
    out[0] = '\0';
    while (f != NULL) {
        const char *e;
        size_t len, i;
        int alpha = 0;
        f++;
        e = strchr(f, '.');
        len = e != NULL ? (size_t)(e - f) : strlen(f);
        for (i = 0; i < len; i++) {
            if ((f[i] >= 'a' && f[i] <= 'z') || (f[i] >= 'A' && f[i] <= 'Z')) { alpha = 1; }
            if (f[i] == ' ') { len = i; break; }
        }
        if (alpha && len > 0) {
            if (len >= cap) { len = cap - 1; }
            memcpy(out, f, len);
            out[len] = '\0';
        }
        f = e;
    }
}

void lt_recent_ecu_label(const char *sig, char *out, size_t cap)
{
    char board[48];
    const char *fam = NULL;
    size_t n;
    if (cap == 0) { return; }
    out[0] = '\0';
    if (sig == NULL || sig[0] == '\0') { return; }
    if (starts_ci(sig, "rusEFI")) { fam = "rusEFI"; }
    else if (starts_ci(sig, "FOME")) { fam = "FOME"; }
    else if (starts_ci(sig, "epicEFI") || starts_ci(sig, "epicECU")) { fam = "epicEFI"; }
    if (fam != NULL) {
        board_field(sig, board, sizeof(board));
        if (board[0] != '\0') {
            lt_snprintf(out, cap, "%s (%s)", fam, board);
        } else {
            lt_strlcpy(out, fam, cap);
        }
        return;
    }
    if (starts_ci(sig, "speeduino")) {
        const char *v = sig + 9;
        while (*v == ' ') { v++; }
        n = 0;
        while (v[n] >= '0' && v[n] <= '9') { n++; }
        if (n == 6) {
            lt_snprintf(out, cap, "Speeduino %.4s.%.2s", v, v + 4);
        } else if (*v != '\0') {
            lt_snprintf(out, cap, "Speeduino %s", v);
        } else {
            lt_strlcpy(out, "Speeduino", cap);
        }
        return;
    }
    if (starts_ci(sig, "MS3")) { lt_strlcpy(out, "MegaSquirt-III", cap); return; }
    if (starts_ci(sig, "MS2Extra") || starts_ci(sig, "MS2/Extra")) { lt_strlcpy(out, "MS2/Extra", cap); return; }
    if (starts_ci(sig, "MSII") || starts_ci(sig, "MS2")) { lt_strlcpy(out, "MegaSquirt-II", cap); return; }
    if (starts_ci(sig, "MS1") || starts_ci(sig, "MSnS")) { lt_strlcpy(out, "MegaSquirt-I", cap); return; }
    lt_strlcpy(out, sig, cap);
    n = strlen(out);
    if (n > 40 && cap > 41) {
        out[37] = '.'; out[38] = '.'; out[39] = '.'; out[40] = '\0';
    }
}

/* "YYYY-MM-DD HH:MM:SS" -> day number and minutes; 0 ok */
static int parse_stamp(const char *s, long *day, int *hh, int *mm, int *y, int *mo, int *d)
{
    int i;
    long yy, m, dd, era, yoe, doy, doe;
    if (s == NULL || strlen(s) < 16) { return -1; }
    for (i = 0; i < 16; i++) {
        if (i == 4 || i == 7) { if (s[i] != '-') { return -1; } }
        else if (i == 10) { if (s[i] != ' ' && s[i] != 'T') { return -1; } }
        else if (i == 13) { if (s[i] != ':') { return -1; } }
        else if (s[i] < '0' || s[i] > '9') { return -1; }
    }
    yy = atol(s);
    m = atol(s + 5);
    dd = atol(s + 8);
    *hh = atoi(s + 11);
    *mm = atoi(s + 14);
    if (yy < 1970 || m < 1 || m > 12 || dd < 1 || dd > 31) { return -1; }
    *y = (int)yy; *mo = (int)m; *d = (int)dd;
    /* days from civil (proleptic Gregorian), non-negative years only */
    if (m <= 2) { yy -= 1; }
    era = yy / 400;
    yoe = yy - era * 400;
    doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + dd - 1;
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    *day = era * 146097L + doe;
    return 0;
}

void lt_recent_age(const char *stamp, const char *now, char *out, size_t cap)
{
    static const char *const mon[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                         "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    long d0 = 0, d1 = 0, diff;
    int h0 = 0, m0 = 0, h1, m1, y, mo, d, y1, mo1, dd1;
    if (cap == 0) { return; }
    out[0] = '\0';
    if (parse_stamp(stamp, &d0, &h0, &m0, &y, &mo, &d) != 0) { return; }
    if (parse_stamp(now, &d1, &h1, &m1, &y1, &mo1, &dd1) != 0) { d1 = d0; }
    diff = d1 - d0;
    if (diff <= 0) { lt_snprintf(out, cap, "Today %02d:%02d", h0, m0); }
    else if (diff == 1) { lt_snprintf(out, cap, "Yesterday %02d:%02d", h0, m0); }
    else if (diff < 7) { lt_snprintf(out, cap, "%ld days ago", diff); }
    else { lt_snprintf(out, cap, "%d %s %d", d, mon[mo - 1], y); }
}
