/* tables_common.c - shared helpers for the table/curve/AutoTune screens and registration.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <string.h>
#include <ctype.h>
#include <math.h>
#include "tables_priv.h"

void lt_tabs_fmt(char *buf, size_t cap, double v, int digits)
{
    char f[8];
    if (digits < 0) { digits = 0; }
    if (digits > 6) { digits = 6; }
    lt_snprintf(f, sizeof(f), "%%.%df", digits);
    lt_snprintf(buf, cap, f, v);
}

static int scheme_of(const char *n)
{
    static const char *names[] = { "tunerstudio", "grayscale", "pastel", "thermal", "viridis",
                                   "plasma" };
    int i;
    for (i = 0; i < 6; i++) {
        if (n != NULL && strcmp(n, names[i]) == 0) { return i; }
    }
    return 0;
}

lt_color lt_tabs_heat(const lt_app *app, const lt_theme *t, double lo, double hi, double v)
{
    double f = hi > lo ? (v - lo) / (hi - lo) : 0.5;
    return lt_color_mix(lt_heat_color(scheme_of(app->settings.heatmap), f), t->table_bg, 70);
}

int lt_tabs_const_digits(const lt_ini *ini, const char *name)
{
    const lt_ini_const *c = name != NULL ? lt_ini_find_const(ini, name) : NULL;
    return c != NULL ? c->digits : 0;
}

double lt_tabs_round(double v, int digits)
{
    double m = 1.0, x;
    int i;
    for (i = 0; i < digits; i++) { m *= 10.0; }
    x = v * m;
    x = x >= 0.0 ? floor(x + 0.5) : -floor(-x + 0.5);
    return x / m;
}

int lt_tabs_key(lt_ui *ui, int ch)
{
    return lt_ui_key_pressed(ui, ch, 0) || lt_ui_key_pressed(ui, ch, LT_MOD_SHIFT);
}

int lt_tabs_parse_list(const char *s, double *out, int cap)
{
    int n = 0;
    char tok[48];
    while (*s != '\0') {
        size_t k = 0;
        while (*s != '\0' && (isspace((unsigned char)*s) || *s == ',' || *s == ';')) { s++; }
        if (*s == '\0') { break; }
        while (*s != '\0' && !isspace((unsigned char)*s) && *s != ',' && *s != ';') {
            if (k + 1 < sizeof(tok)) { tok[k++] = *s; }
            s++;
        }
        tok[k] = '\0';
        if (n >= cap || !lt_parse_double(tok, &out[n])) { return -1; }
        n++;
    }
    return n;
}

lt_bool lt_tabs_live_index(lt_app *app, const char *channel, const double *bins, size_t n,
                           double *value, size_t *idx)
{
    double v;
    if (channel == NULL || channel[0] == '\0' || n == 0 || app->rt == NULL) { return LT_FALSE; }
    if (app->conn != LT_APP_ONLINE && app->conn != LT_APP_DEMO) { return LT_FALSE; }
    if (!lt_app_channel(app, channel, &v)) { return LT_FALSE; }
    if (value != NULL) { *value = v; }
    *idx = lt_tbl_nearest(bins, n, v);
    return LT_TRUE;
}

void lt_app_register_tables(lt_app *app)
{
    lt_app_register_tab_kind(app, &lt_tabs_kind_table);
    lt_app_register_tab_kind(app, &lt_tabs_kind_curve);
    lt_app_register_tab_kind(app, &lt_tabs_kind_autotune);
}

int lt_app_table_editor_open(lt_app *app, const char *table_name)
{
    const lt_ini_table *t = lt_ini_find_table(app->ini, table_name);
    return lt_app_tab_open(app, "table", table_name,
                           t != NULL && t->title != NULL ? t->title : table_name, 1) != NULL
               ? 0 : -1;
}

int lt_app_curve_editor_open(lt_app *app, const char *curve_name)
{
    const lt_ini_curve *c = lt_ini_find_curve(app->ini, curve_name);
    return lt_app_tab_open(app, "curve", curve_name,
                           c != NULL && c->title != NULL ? c->title : curve_name, 1) != NULL
               ? 0 : -1;
}

int lt_app_autotune_open(lt_app *app)
{
    return lt_app_tab_open(app, "autotune", "autotune", "AutoTune", 1) != NULL ? 0 : -1;
}

void lt_app_tables_service(lt_app *app)
{
    lt_tabs_autotune_service(app);
}
