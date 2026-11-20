/* prefs.c - see prefs.h.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdlib.h>
#include <string.h>
#include "prefs.h"

static const char *const scheme_names[] = { "tunerstudio", "grayscale", "pastel", "thermal",
                                            "viridis", "plasma" };

void lt_prefs_defaults(lt_prefs *p)
{
    int i;
    memset(p, 0, sizeof(*p));
    p->packet_mode = LT_PKT_AUTO;
    p->adaptive_timing = 0;
    p->fome_fast_comms = 1;
    p->auto_burn_close = 0;
    p->change_alert = 0;
    p->change_abs = 5.0;
    p->change_pct = 10.0;
    p->help_icons = 1;
    p->y_bottom = 1;
    p->trail_fade_sec = 8;
    p->trail_on = 1;
    for (i = 0; i < LT_PREFS_HEAT_CTX; i++) {
        lt_strlcpy(p->heat_scheme[i], "tunerstudio", sizeof(p->heat_scheme[i]));
    }
    p->restore_session = 1;
    p->last_active = -1;
}

int lt_prefs_scheme_index(const char *name)
{
    int i;
    for (i = 0; i < 6; i++) {
        if (name != NULL && strcmp(name, scheme_names[i]) == 0) { return i; }
    }
    return 0;
}

static int hexval(int c)
{
    if (c >= '0' && c <= '9') { return c - '0'; }
    if (c >= 'a' && c <= 'f') { return c - 'a' + 10; }
    if (c >= 'A' && c <= 'F') { return c - 'A' + 10; }
    return -1;
}

lt_bool lt_prefs_parse_color(const char *s, lt_u32 *rgb)
{
    lt_u32 v = 0;
    int i, h;
    if (s == NULL || s[0] != '#' || strlen(s) != 7) { return LT_FALSE; }
    for (i = 1; i < 7; i++) {
        h = hexval((unsigned char)s[i]);
        if (h < 0) { return LT_FALSE; }
        v = (v << 4) | (lt_u32)h;
    }
    if (rgb != NULL) { *rgb = v; }
    return LT_TRUE;
}

void lt_prefs_format_color(lt_u32 rgb, char out[8])
{
    static const char hx[] = "0123456789abcdef";
    int i;
    out[0] = '#';
    for (i = 0; i < 6; i++) { out[1 + i] = hx[(rgb >> (20 - 4 * i)) & 15U]; }
    out[7] = '\0';
}

/* "#aaaaaa,#bbbbbb,..." -> stops */
static void parse_stops(lt_prefs *p, int ctx, const char *s)
{
    int n = 0;
    p->heat_n[ctx] = 0;
    while (s != NULL && *s != '\0' && n < LT_PREFS_HEAT_STOPS) {
        char tok[8];
        lt_u32 v = 0;
        size_t k = 0;
        while (*s == ',' || *s == ' ') { s++; }
        while (*s != '\0' && *s != ',' && k + 1 < sizeof(tok)) { tok[k++] = *s++; }
        tok[k] = '\0';
        while (*s != '\0' && *s != ',') { s++; }
        if (lt_prefs_parse_color(tok, &v)) { p->heat_stops[ctx][n++] = v; }
    }
    p->heat_n[ctx] = n;
}

static void format_stops(const lt_prefs *p, int ctx, char *out, size_t cap)
{
    int i;
    char c[8];
    out[0] = '\0';
    for (i = 0; i < p->heat_n[ctx]; i++) {
        lt_prefs_format_color(p->heat_stops[ctx][i], c);
        if (i > 0) { lt_strlcat(out, ",", cap); }
        lt_strlcat(out, c, cap);
    }
}

static void get_int(const lt_props *pr, const char *k, int *v, int lo, int hi)
{
    int x = 0;
    if (lt_props_get_i32(pr, k, &x)) { *v = LT_CLAMP(x, lo, hi); }
}

static void get_str(const lt_props *pr, const char *k, char *dst, size_t cap)
{
    const char *v = lt_props_get(pr, k);
    if (v != NULL) { lt_strlcpy(dst, v, cap); }
}

void lt_prefs_from_props(lt_prefs *p, const lt_props *pr)
{
    char key[24];
    const char *v;
    double d = 0.0;
    int i;
    get_int(pr, "packet_mode", &p->packet_mode, 0, 3);
    get_int(pr, "adaptive_timing", &p->adaptive_timing, 0, 1);
    get_int(pr, "fome_fast_comms", &p->fome_fast_comms, 0, 1);
    get_int(pr, "auto_burn_close", &p->auto_burn_close, 0, 1);
    get_int(pr, "change_alert", &p->change_alert, 0, 1);
    if (lt_props_get_f64(pr, "change_abs", &d) && d >= 0.0) { p->change_abs = d; }
    if (lt_props_get_f64(pr, "change_pct", &d) && d >= 0.0) { p->change_pct = d; }
    get_int(pr, "help_icons", &p->help_icons, 0, 1);
    get_int(pr, "onboarded", &p->onboarded, 0, 1);
    get_int(pr, "y_bottom", &p->y_bottom, 0, 1);
    get_str(pr, "cursor_color", p->cursor_color, sizeof(p->cursor_color));
    get_str(pr, "trail_color", p->trail_color, sizeof(p->trail_color));
    if (!lt_prefs_parse_color(p->cursor_color, NULL)) { p->cursor_color[0] = '\0'; }
    if (!lt_prefs_parse_color(p->trail_color, NULL)) { p->trail_color[0] = '\0'; }
    get_int(pr, "trail_fade_sec", &p->trail_fade_sec, 0, 600);
    get_int(pr, "trail_on", &p->trail_on, 0, 1);
    for (i = 0; i < LT_PREFS_HEAT_CTX; i++) {
        lt_snprintf(key, sizeof(key), "heat%d", i);
        get_str(pr, key, p->heat_scheme[i], sizeof(p->heat_scheme[i]));
        lt_snprintf(key, sizeof(key), "heat%d_stops", i);
        v = lt_props_get(pr, key);
        if (v != NULL) { parse_stops(p, i, v); }
    }
    get_int(pr, "restore_session", &p->restore_session, 0, 1);
    get_str(pr, "session_project", p->session_project, sizeof(p->session_project));
    v = lt_props_get(pr, "open_tabs");
    if (v != NULL) {
        size_t k;
        lt_strlcpy(p->open_tabs, v, sizeof(p->open_tabs));
        for (k = 0; p->open_tabs[k] != '\0'; k++) {     /* a properties value is one line */
            if (p->open_tabs[k] == ';') { p->open_tabs[k] = '\n'; }
        }
    }
    get_int(pr, "last_active", &p->last_active, -1, 1000);
    p->sb_n = 0;
    v = lt_props_get(pr, "sb_channels");
    while (v != NULL && *v != '\0' && p->sb_n < LT_PREFS_SB_MAX) {
        size_t k = 0;
        char *dst = p->sb_chan[p->sb_n];
        while (*v == ',') { v++; }
        while (*v != '\0' && *v != ',' && k + 1 < sizeof(p->sb_chan[0])) { dst[k++] = *v++; }
        dst[k] = '\0';
        while (*v != '\0' && *v != ',') { v++; }
        if (k > 0) { p->sb_n++; }
    }
}

void lt_prefs_to_props(const lt_prefs *p, lt_props *pr)
{
    char key[24], num[48], buf[LT_PREFS_SESSION_CAP];
    int i;
    size_t k;
    lt_snprintf(num, sizeof(num), "%d", p->packet_mode); lt_props_set(pr, "packet_mode", num);
    lt_snprintf(num, sizeof(num), "%d", p->adaptive_timing); lt_props_set(pr, "adaptive_timing", num);
    lt_snprintf(num, sizeof(num), "%d", p->fome_fast_comms); lt_props_set(pr, "fome_fast_comms", num);
    lt_snprintf(num, sizeof(num), "%d", p->auto_burn_close); lt_props_set(pr, "auto_burn_close", num);
    lt_snprintf(num, sizeof(num), "%d", p->change_alert); lt_props_set(pr, "change_alert", num);
    lt_snprintf(num, sizeof(num), "%g", p->change_abs); lt_props_set(pr, "change_abs", num);
    lt_snprintf(num, sizeof(num), "%g", p->change_pct); lt_props_set(pr, "change_pct", num);
    lt_snprintf(num, sizeof(num), "%d", p->help_icons); lt_props_set(pr, "help_icons", num);
    lt_snprintf(num, sizeof(num), "%d", p->onboarded); lt_props_set(pr, "onboarded", num);
    lt_snprintf(num, sizeof(num), "%d", p->y_bottom); lt_props_set(pr, "y_bottom", num);
    lt_props_set(pr, "cursor_color", p->cursor_color);
    lt_props_set(pr, "trail_color", p->trail_color);
    lt_snprintf(num, sizeof(num), "%d", p->trail_fade_sec); lt_props_set(pr, "trail_fade_sec", num);
    lt_snprintf(num, sizeof(num), "%d", p->trail_on); lt_props_set(pr, "trail_on", num);
    for (i = 0; i < LT_PREFS_HEAT_CTX; i++) {
        lt_snprintf(key, sizeof(key), "heat%d", i);
        lt_props_set(pr, key, p->heat_scheme[i]);
        lt_snprintf(key, sizeof(key), "heat%d_stops", i);
        format_stops(p, i, buf, sizeof(buf));
        lt_props_set(pr, key, buf);
    }
    lt_snprintf(num, sizeof(num), "%d", p->restore_session); lt_props_set(pr, "restore_session", num);
    lt_props_set(pr, "session_project", p->session_project);
    lt_strlcpy(buf, p->open_tabs, sizeof(buf));
    for (k = 0; buf[k] != '\0'; k++) { if (buf[k] == '\n') { buf[k] = ';'; } }
    lt_props_set(pr, "open_tabs", buf);
    lt_snprintf(num, sizeof(num), "%d", p->last_active); lt_props_set(pr, "last_active", num);
    buf[0] = '\0';
    for (i = 0; i < p->sb_n; i++) {
        if (i > 0) { lt_strlcat(buf, ",", sizeof(buf)); }
        lt_strlcat(buf, p->sb_chan[i], sizeof(buf));
    }
    lt_props_set(pr, "sb_channels", buf);
}

int lt_prefs_load(lt_prefs *p, const char *dir)
{
    lt_props pr;
    char *path;
    int rc = -1;
    lt_prefs_defaults(p);
    if (dir == NULL) { return -1; }
    path = lt_path_join(dir, "prefs.conf");
    lt_props_init(&pr);
    if (path != NULL && lt_file_exists(path) && lt_props_load(&pr, path) == 0) {
        lt_prefs_from_props(p, &pr);
        rc = 0;
    }
    lt_props_free(&pr);
    lt_free(path);
    return rc;
}

int lt_prefs_save(const lt_prefs *p, const char *dir)
{
    lt_props pr;
    char *path;
    int rc;
    if (dir == NULL) { return -1; }
    path = lt_path_join(dir, "prefs.conf");
    if (path == NULL) { return -1; }
    lt_props_init(&pr);
    lt_prefs_to_props(p, &pr);
    rc = lt_props_save(&pr, path);
    lt_props_free(&pr);
    lt_free(path);
    return rc;
}

lt_bool lt_prefs_heat(const lt_prefs *p, int ctx, double t, lt_u32 *out, int *scheme)
{
    int n, i, t256, r, g, bl;
    double f, seg;
    lt_u32 a, b;
    if (ctx < 0 || ctx >= LT_PREFS_HEAT_CTX) { ctx = 0; }
    if (strcmp(p->heat_scheme[ctx], "custom") != 0 || p->heat_n[ctx] < 2) {
        if (scheme != NULL) { *scheme = lt_prefs_scheme_index(p->heat_scheme[ctx]); }
        return LT_FALSE;
    }
    n = p->heat_n[ctx];
    f = LT_CLAMP(t, 0.0, 1.0) * (double)(n - 1);
    i = (int)f;
    if (i >= n - 1) { i = n - 2; }
    seg = f - (double)i;
    a = p->heat_stops[ctx][i];
    b = p->heat_stops[ctx][i + 1];
    t256 = (int)(seg * 256.0 + 0.5);
    r = (int)((a >> 16) & 255U) + ((int)((b >> 16) & 255U) - (int)((a >> 16) & 255U)) * t256 / 256;
    g = (int)((a >> 8) & 255U) + ((int)((b >> 8) & 255U) - (int)((a >> 8) & 255U)) * t256 / 256;
    bl = (int)(a & 255U) + ((int)(b & 255U) - (int)(a & 255U)) * t256 / 256;
    *out = ((lt_u32)r << 16) | ((lt_u32)g << 8) | (lt_u32)bl;
    return LT_TRUE;
}

lt_bool lt_prefs_change_exceeds(const lt_prefs *p, double old_v, double new_v)
{
    double d = new_v - old_v;
    double ad = d < 0.0 ? -d : d;
    double ao = old_v < 0.0 ? -old_v : old_v;
    if (!p->change_alert) { return LT_FALSE; }
    if (p->change_abs > 0.0 && ad > p->change_abs) { return LT_TRUE; }
    if (p->change_pct > 0.0 && ao > 0.0 && ad * 100.0 > p->change_pct * ao) { return LT_TRUE; }
    return LT_FALSE;
}

int lt_prefs_sb_has(const lt_prefs *p, const char *name)
{
    int i;
    for (i = 0; i < p->sb_n; i++) {
        if (strcmp(p->sb_chan[i], name) == 0) { return 1; }
    }
    return 0;
}

int lt_prefs_sb_toggle(lt_prefs *p, const char *name)
{
    int i, j;
    for (i = 0; i < p->sb_n; i++) {
        if (strcmp(p->sb_chan[i], name) == 0) {
            for (j = i; j < p->sb_n - 1; j++) {
                memcpy(p->sb_chan[j], p->sb_chan[j + 1], sizeof(p->sb_chan[0]));
            }
            p->sb_n--;
            p->sb_chan[p->sb_n][0] = '\0';
            return 0;
        }
    }
    if (p->sb_n >= LT_PREFS_SB_MAX || name[0] == '\0') { return -1; }
    lt_strlcpy(p->sb_chan[p->sb_n++], name, sizeof(p->sb_chan[0]));
    return 1;
}

void lt_prefs_session_clear(lt_prefs *p)
{
    p->open_tabs[0] = '\0';
}

int lt_prefs_session_add(lt_prefs *p, const char *kind, const char *target)
{
    size_t have = strlen(p->open_tabs), need = strlen(kind) + strlen(target) + 2;
    if (strchr(kind, '|') != NULL || strchr(target, '|') != NULL || strchr(target, '\n') != NULL
        || strchr(target, ';') != NULL || strchr(kind, ';') != NULL) { return -1; }
    if (have + need + 1 > sizeof(p->open_tabs)) { return -1; }
    lt_strlcat(p->open_tabs, kind, sizeof(p->open_tabs));
    lt_strlcat(p->open_tabs, "|", sizeof(p->open_tabs));
    lt_strlcat(p->open_tabs, target, sizeof(p->open_tabs));
    lt_strlcat(p->open_tabs, "\n", sizeof(p->open_tabs));
    return 0;
}

lt_bool lt_prefs_session_get(const lt_prefs *p, int n, char *kind, size_t kcap, char *target,
                             size_t tcap)
{
    const char *s = p->open_tabs, *e, *bar;
    int i = 0;
    while (*s != '\0') {
        e = strchr(s, '\n');
        if (e == NULL) { e = s + strlen(s); }
        if (i == n) {
            size_t len = (size_t)(e - s), kl;
            bar = (const char *)memchr(s, '|', len);
            if (bar == NULL) { return LT_FALSE; }
            kl = (size_t)(bar - s);
            if (kl + 1 > kcap) { kl = kcap - 1; }
            memcpy(kind, s, kl);
            kind[kl] = '\0';
            len = (size_t)(e - bar - 1);
            if (len + 1 > tcap) { len = tcap - 1; }
            memcpy(target, bar + 1, len);
            target[len] = '\0';
            return LT_TRUE;
        }
        i++;
        s = *e == '\n' ? e + 1 : e;
    }
    return LT_FALSE;
}
