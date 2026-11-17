/* tableops_units.c - unit preference layer: detect an INI unit string, convert between the
 * native (ECU) unit and the preferred display unit.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <string.h>
#include "tableops.h"

static int g_temp = 0, g_press = 0, g_afr = 0, g_metric = 1;
static unsigned g_gen = 1;

void lt_units_set(int temp, int press, int afr, int metric)
{
    temp = LT_CLAMP(temp, 0, 2);
    press = LT_CLAMP(press, 0, 3);
    afr = LT_CLAMP(afr, 0, 1);
    metric = metric ? 1 : 0;
    if (temp != g_temp || press != g_press || afr != g_afr || metric != g_metric) {
        g_temp = temp; g_press = press; g_afr = afr; g_metric = metric;
        g_gen++;
    }
}

void lt_units_get(int *temp, int *press, int *afr, int *metric)
{
    if (temp) *temp = g_temp;
    if (press) *press = g_press;
    if (afr) *afr = g_afr;
    if (metric) *metric = g_metric;
}

unsigned lt_units_generation(void) { return g_gen; }

/* Lower-case the string, dropping spaces, underscores and degree signs (UTF-8 U+00B0). */
static void norm_units(const char *in, char *out, size_t cap)
{
    size_t n = 0;
    while (*in && n + 1 < cap) {
        unsigned char c = (unsigned char)*in;
        if (c == ' ' || c == '\t' || c == '_') { in++; continue; }
        if (c == 0xC2 && (unsigned char)in[1] == 0xB0) { in += 2; continue; }
        if (c >= 'A' && c <= 'Z') c = (unsigned char)(c - 'A' + 'a');
        out[n++] = (char)c;
        in++;
    }
    out[n] = '\0';
}

static const char *skip_deg(const char *s)
{
    if (strncmp(s, "degrees", 7) == 0) return s + 7;
    if (strncmp(s, "degree", 6) == 0) return s + 6;
    if (strncmp(s, "deg", 3) == 0) return s + 3;
    return s;
}

int lt_unit_detect(const char *units, int *src)
{
    char n[48];
    const char *t;
    int s = -1;
    if (src) *src = 0;
    if (!units || !*units || units[0] == '{') return LT_UK_NONE;
    norm_units(units, n, sizeof(n));
    t = skip_deg(n);
    if (!strcmp(t, "c") || !strcmp(t, "celsius")) s = 0;
    else if (!strcmp(t, "f") || !strcmp(t, "fahrenheit")) s = 1;
    else if (!strcmp(t, "k") || !strcmp(t, "kelvin")) s = 2;
    if (s >= 0) {
        if (src) *src = s;
        return LT_UK_TEMP;
    }
    if (!strcmp(n, "kpa") || !strcmp(n, "kpag") || !strcmp(n, "kpaa")) s = 0;
    else if (!strcmp(n, "psi") || !strcmp(n, "psig") || !strcmp(n, "psia")) s = 1;
    else if (!strcmp(n, "bar")) s = 2;
    else if (!strcmp(n, "inhg")) s = 3;
    if (s >= 0) {
        if (src) *src = s;
        return LT_UK_PRESS;
    }
    if (!strcmp(n, "afr") || !strcmp(n, "a/f") || !strcmp(n, "a:f") || !strcmp(n, "air:fuel")
        || !strcmp(n, "airfuelratio")) {
        if (src) *src = 0;
        return LT_UK_AFR;
    }
    if (!strcmp(n, "lambda") || !strcmp(n, "\xCE\xBB")) {
        if (src) *src = 1;
        return LT_UK_AFR;
    }
    if (!strcmp(n, "km/h") || !strcmp(n, "kph") || !strcmp(n, "kmh")) {
        if (src) *src = 0;
        return LT_UK_SPEED;
    }
    if (!strcmp(n, "mph")) {
        if (src) *src = 1;
        return LT_UK_SPEED;
    }
    return LT_UK_NONE;
}

/* to base: C, kPa, AFR, km/h */
static double to_base(int kind, int u, double v)
{
    switch (kind) {
    case LT_UK_TEMP:
        return u == 1 ? lt_f_to_c(v) : (u == 2 ? v - 273.15 : v);
    case LT_UK_PRESS:
        if (u == 1) return lt_psi_to_kpa(v);
        if (u == 2) return lt_psi_to_kpa(lt_bar_to_psi(v));
        if (u == 3) return v * 3.386389;
        return v;
    case LT_UK_AFR:
        return u == 1 ? lt_lambda_to_afr(v, NULL) : v;
    case LT_UK_SPEED:
        return u == 1 ? lt_mph_to_kmh(v) : v;
    default:
        return v;
    }
}

static double from_base(int kind, int u, double v)
{
    switch (kind) {
    case LT_UK_TEMP:
        return u == 1 ? lt_c_to_f(v) : (u == 2 ? v + 273.15 : v);
    case LT_UK_PRESS:
        if (u == 1) return lt_kpa_to_psi(v);
        if (u == 2) return lt_psi_to_bar(lt_kpa_to_psi(v));
        if (u == 3) return v / 3.386389;
        return v;
    case LT_UK_AFR:
        return u == 1 ? lt_afr_to_lambda(v, NULL) : v;
    case LT_UK_SPEED:
        return u == 1 ? lt_kmh_to_mph(v) : v;
    default:
        return v;
    }
}

double lt_unit_convert(int kind, int from, int to, double v)
{
    if (from == to || kind == LT_UK_NONE) return v;
    return from_base(kind, to, to_base(kind, from, v));
}

const char *lt_unit_name(int kind, int unit)
{
    static const char *const t[] = { "C", "F", "K" };
    static const char *const p[] = { "kPa", "psi", "bar", "inHg" };
    static const char *const a[] = { "AFR", "lambda" };
    static const char *const s[] = { "km/h", "mph" };
    if (unit < 0) unit = 0;
    switch (kind) {
    case LT_UK_TEMP:  return t[unit > 2 ? 2 : unit];
    case LT_UK_PRESS: return p[unit > 3 ? 3 : unit];
    case LT_UK_AFR:   return a[unit > 1 ? 1 : unit];
    case LT_UK_SPEED: return s[unit > 1 ? 1 : unit];
    default:          return "";
    }
}

static int pref_for(int kind)
{
    switch (kind) {
    case LT_UK_TEMP:  return g_temp;
    case LT_UK_PRESS: return g_press;
    case LT_UK_AFR:   return g_afr;
    case LT_UK_SPEED: return g_metric ? 0 : 1;
    default:          return 0;
    }
}

void lt_uconv_make(lt_uconv *c, const char *units)
{
    c->src = 0;
    c->kind = lt_unit_detect(units, &c->src);
    c->dst = c->kind ? pref_for(c->kind) : 0;
}

lt_bool lt_uconv_active(const lt_uconv *c) { return c->kind != LT_UK_NONE && c->src != c->dst; }

lt_bool lt_uconv_stale(const lt_uconv *c)
{
    return c->kind != LT_UK_NONE && c->dst != pref_for(c->kind);
}

double lt_uconv_to_disp(const lt_uconv *c, double ecu)
{
    return lt_unit_convert(c->kind, c->src, c->dst, ecu);
}

double lt_uconv_from_disp(const lt_uconv *c, double disp)
{
    return lt_unit_convert(c->kind, c->dst, c->src, disp);
}

const char *lt_uconv_label(const lt_uconv *c, const char *units)
{
    if (!lt_uconv_active(c)) return units ? units : "";
    return lt_unit_name(c->kind, c->dst);
}

double lt_uconv_retarget(lt_uconv *c, double disp)
{
    int nd;
    double r;
    if (c->kind == LT_UK_NONE) return disp;
    nd = pref_for(c->kind);
    r = lt_unit_convert(c->kind, c->dst, nd, disp);
    c->dst = nd;
    return r;
}

void lt_uconv_range(const lt_uconv *c, double *lo, double *hi)
{
    double a = lt_uconv_to_disp(c, *lo), b = lt_uconv_to_disp(c, *hi);
    *lo = a < b ? a : b;
    *hi = a < b ? b : a;
}

int lt_uconv_digits(const lt_uconv *c, int digits)
{
    double r;
    if (digits < 0) digits = 0;
    if (!lt_uconv_active(c)) return digits;
    r = lt_uconv_to_disp(c, 1.0) - lt_uconv_to_disp(c, 0.0);
    if (r < 0.0) r = -r;
    while (r > 0.0 && r < 0.99 && digits < 6) {
        r *= 10.0;
        digits++;
    }
    return digits;
}
