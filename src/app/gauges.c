/* gauges.c - framebuffer gauge painters on lt_gfx (spec 05 section 6, 05b).
 * Every painter takes the gauge rect, the config and the runtime state; sizes
 * follow the spec numbers, gradients are approximated with rings and strips.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "dash_gauges.h"

static lt_expr_cache *g_cache = NULL;
static const lt_expr_env *g_env = NULL;

void lt_gauges_set_env(lt_expr_cache *cache, const lt_expr_env *env)
{
    g_cache = cache;
    g_env = env;
}

/* ---- painter context ---- */
typedef struct gp {
    lt_gfx *g;
    lt_rect r;
    const lt_dash_gauge *c;
    const lt_dash_gstate *s;
    double now, W, H, v, mn, mx;
    lt_color bc, fc, tc, nc, wc, cc, vc;   /* back font trim needle warn crit value */
} gp;

#define VS 0.78   /* vector font em height -> requested size */
static lt_color opq(lt_color c) { return c | 0xFF000000UL; }
static double clamp01(double x) { return x < 0.0 ? 0.0 : (x > 1.0 ? 1.0 : x); }
static double rad(double deg) { return deg * LT_PI / 180.0; }

static lt_color mix(lt_color a, lt_color b, double t)
{
    int r, g, bl;
    t = clamp01(t);
    r = (int)(LT_COLOR_R(a) + (LT_COLOR_R(b) - LT_COLOR_R(a)) * t + 0.5);
    g = (int)(LT_COLOR_G(a) + (LT_COLOR_G(b) - LT_COLOR_G(a)) * t + 0.5);
    bl = (int)(LT_COLOR_B(a) + (LT_COLOR_B(b) - LT_COLOR_B(a)) * t + 0.5);
    return LT_RGB(r, g, bl);
}
static lt_color lighten(lt_color c, int pct) { return opq(lt_color_lighten(opq(c), pct)); }
static lt_color darken(lt_color c, int pct) { return opq(lt_color_darken(opq(c), pct)); }
static lt_color alpha(lt_color c, int a) { return (c & 0x00FFFFFFUL) | ((lt_u32)a << 24); }

static double pct_of(const gp *p, double v)
{
    double span = p->mx - p->mn;
    return span > 0.0 ? (v - p->mn) / span : 0.0;
}

static int ir(double x) { return (int)floor(x + 0.5); }

/* ---- local-coordinate drawing helpers ---- */
static void p_fill(gp *p, double x, double y, double w, double h, lt_color c)
{
    lt_gfx_fill_rect(p->g, lt_rect_make(p->r.x + ir(x), p->r.y + ir(y), ir(w), ir(h)), c);
}
static void p_round(gp *p, double x, double y, double w, double h, double rd, lt_color c)
{
    lt_gfx_fill_round_rect(p->g, lt_rect_make(p->r.x + ir(x), p->r.y + ir(y), ir(w), ir(h)),
                           ir(rd), c);
}
static void p_vgrad(gp *p, double x, double y, double w, double h, lt_color a, lt_color b)
{
    lt_gfx_fill_rect_vgrad(p->g, lt_rect_make(p->r.x + ir(x), p->r.y + ir(y), ir(w), ir(h)), a, b);
}
static void p_hgrad(gp *p, double x, double y, double w, double h, lt_color a, lt_color b)
{
    int n = ir(w), i;
    if (n < 1 || h < 1.0) { return; }
    if (n > 64) { n = 64; }
    for (i = 0; i < n; i++) {
        double x0 = x + w * i / n, x1 = x + w * (i + 1) / n;
        lt_gfx_fill_rect(p->g, lt_rect_make(p->r.x + ir(x0), p->r.y + ir(y),
                         ir(x1) - ir(x0) + (i + 1 < n ? 0 : 0), ir(h)), mix(a, b, (i + 0.5) / n));
    }
}
static void p_line(gp *p, double x0, double y0, double x1, double y1, double w, lt_color c)
{
    lt_gfx_line(p->g, p->r.x + x0, p->r.y + y0, p->r.x + x1, p->r.y + y1, w, c);
}
static void p_disc(gp *p, double cx, double cy, double r, lt_color c)
{
    lt_gfx_fill_circle(p->g, p->r.x + cx, p->r.y + cy, r, c);
}
static void p_ring(gp *p, double cx, double cy, double r, double w, lt_color c)
{
    lt_gfx_circle(p->g, p->r.x + cx, p->r.y + cy, r, w, c);
}
/* arc stroke between angles (radians, either order) with butt caps */
static void p_arc(gp *p, double cx, double cy, double r, double a0, double a1, double w, lt_color c)
{
    if (a1 < a0) { double t = a0; a0 = a1; a1 = t; }
    if (a1 - a0 < 1e-4 || w < 0.5) { return; }
    lt_gfx_arc(p->g, p->r.x + cx, p->r.y + cy, r, a0, a1, w, c);
}
/* arc stroke with round caps */
static void p_arc_round(gp *p, double cx, double cy, double r, double a0, double a1, double w,
                        lt_color c)
{
    p_arc(p, cx, cy, r, a0, a1, w, c);
    p_disc(p, cx + cos(a0) * r, cy + sin(a0) * r, w / 2.0, c);
    p_disc(p, cx + cos(a1) * r, cy + sin(a1) * r, w / 2.0, c);
}
static void p_sector(gp *p, double cx, double cy, double r0, double r1, double a0, double a1,
                     lt_color c)
{
    if (a1 < a0) { double t = a0; a0 = a1; a1 = t; }
    if (a1 - a0 < 1e-4) { return; }
    lt_gfx_fill_arc(p->g, p->r.x + cx, p->r.y + cy, r0, r1, a0, a1, c);
}
static void p_poly(gp *p, const double *xy, int n, lt_color c)
{
    double tmp[64] = { 0.0 };
    int i;
    if (n > 32) { n = 32; }
    for (i = 0; i < n; i++) { tmp[2 * i] = p->r.x + xy[2 * i]; tmp[2 * i + 1] = p->r.y + xy[2 * i + 1]; }
    lt_gfx_fill_poly(p->g, tmp, n, c);
}

/* text in Plex; h is the capital height in pixels; fitted into maxw when > 0.
 * style 0 sans regular, 1 sans medium, 2 mono medium (digital values) */
static double p_text_s(gp *p, double x, double y, const char *s, double h, int align, int style,
                       lt_color c, double maxw)
{
    static const int fid[3] = { (int)LT_FONT_SANS, (int)LT_FONT_SANS_MEDIUM, (int)LT_FONT_MONO_MEDIUM };
    lt_font *f;
    double w, ratio, px;
    if (s == NULL || s[0] == '\0' || h < 3.0) { return 0.0; }
    h *= VS;
    f = lt_font_get((lt_font_id)fid[style < 0 ? 0 : (style > 2 ? 2 : style)]);
    if (f == NULL) { return 0.0; }
    ratio = (double)lt_ttf_cap_height(f) / (double)lt_ttf_units_per_em(f);
    px = style == 2 ? h * 1.12 : h / ratio;
    w = lt_gfx_text_measure(f, px, s);
    if (maxw > 0.0 && w > maxw) { px = px * maxw / w; w = maxw; if (px * ratio < 3.0) { return 0.0; } }
    lt_gfx_text_rot(p->g, f, px, p->r.x + x, p->r.y + y, s, 0.0, align, 1, c);
    return w;
}

static double p_text(gp *p, double x, double y, const char *s, double h, int align, int bold,
                     lt_color c, double maxw)
{
    return p_text_s(p, x, y, s, h, align, bold ? 1 : 0, c, maxw);
}

/* digital value: Plex Mono */
static double p_val(gp *p, double x, double y, const char *s, double h, int align, int bold,
                    lt_color c, double maxw)
{
    LT_UNUSED(bold);
    return p_text_s(p, x, y, s, h, align, 2, c, maxw);
}

/* conic metallic ring between r0 and r1 over [a0,a1], light from upper-left */
static void p_metal(gp *p, double cx, double cy, double r0, double r1, double a0, double a1,
                    lt_color trim)
{
    int n = ir((a1 - a0) / (LT_PI / 36.0)), i;
    lt_color hi = lighten(trim, 55), lo = darken(trim, 55);
    if (n < 2) { n = 2; }
    if (n > 120) { n = 120; }
    for (i = 0; i < n; i++) {
        double s0 = a0 + (a1 - a0) * i / n, s1 = a0 + (a1 - a0) * (i + 1) / n;
        double am = (s0 + s1) / 2.0;
        double t = 0.5 + 0.5 * cos(am - rad(225.0));   /* 1 at upper-left */
        p_sector(p, cx, cy, r0, r1, s0, s1 + 0.012, mix(lo, hi, t));
    }
    p_ring(p, cx, cy, r1 - 0.5, 1.0, darken(trim, 70));
    p_ring(p, cx, cy, r0 + 0.5, 1.0, alpha(darken(trim, 70), 160));
}

static void fmt_val(const gp *p, double v, int digits, char *buf, size_t cap)
{
    int d = digits < 0 ? 0 : (digits > 8 ? 8 : digits);
    LT_UNUSED(p);
    lt_snprintf(buf, cap, "%.*f", d, v);
}

/* title / units with optional {expr} resolution */
static void resolve_text(const char *raw, char *buf, size_t cap)
{
    char *s;
    buf[0] = '\0';
    if (raw == NULL) { return; }
    if (strstr(raw, "\xCE\xBB") != NULL) {   /* lambda is not in the stroke font */
        const char *q = strstr(raw, "\xCE\xBB");
        lt_snprintf(buf, cap, "%.*slam%s", (int)(q - raw), raw, q + 2);
        return;
    }
    if (raw[0] == '{') {
        if (g_cache != NULL && g_env != NULL) {
            s = lt_expr_cache_display(g_cache, raw, g_env);
            if (s != NULL) { lt_snprintf(buf, cap, "%s", s); lt_free(s); }
        }
        return;
    }
    lt_snprintf(buf, cap, "%s", raw);
}

static void upper(char *s)
{
    for (; *s; s++) { if (*s >= 'a' && *s <= 'z') { *s = (char)(*s - 32); } }
}

static void init_ctx(gp *p, lt_gfx *g, lt_rect r, const lt_dash_gauge *c, const lt_dash_gstate *s,
                     double now)
{
    int z;
    p->g = g; p->r = r; p->c = c; p->s = s; p->now = now;
    p->W = r.w; p->H = r.h;
    p->v = s->display;
    p->mn = s->lim.min; p->mx = s->lim.max;
    if (!(p->mx > p->mn)) { p->mn = c->min; p->mx = c->max; }
    p->bc = opq(c->back_color); p->fc = opq(c->font_color); p->tc = opq(c->trim_color);
    p->nc = opq(c->needle_color); p->wc = opq(c->warn_color); p->cc = opq(c->critical_color);
    if (LT_COLOR_A(c->font_color) == 0 && (c->font_color & 0xFFFFFFUL) == 0) { p->fc = LT_RGB(255, 255, 255); }
    z = s->zone;
    p->vc = z >= 2 ? p->cc : (z == 1 ? p->wc : p->fc);
    if (lt_dash_gstate_spiking(s, now)) { p->vc = p->cc; }
}

/* colour of the "level" for bar-like painters */
static lt_color level_color(const gp *p)
{
    int z = p->s->zone;
    return z >= 2 ? p->cc : (z == 1 ? p->wc : p->nc);
}

/* ====================================================================== */
/* Readout panel pieces                                                    */

static void draw_title(gp *p, double x, double y, double h, int align, double maxw)
{
    char t[96];
    resolve_text(p->c->title, t, sizeof t);
    p_text(p, x, y, t, h, align, 1, p->tc, maxw);
}

/* ====================================================================== */
/* BasicReadout                                                            */
static void paint_readout(gp *p)
{
    double m = LT_MIN(p->W, p->H), fs = 1.0 + p->c->font_size_adjustment * 0.1;
    double corner = LT_MIN(8.0, p->W * 0.05), P = 5.0;
    char buf[64], u[48], t[96];
    double ts = LT_MAX(9.0, m * 0.14 * fs), vs = LT_MAX(14.0, m * 0.36 * fs), us;
    p_round(p, 0, 0, p->W, p->H, corner, LT_RGB(24, 24, 26));
    p_vgrad(p, 1, 1, p->W - 2, p->H - 2, LT_RGB(78, 78, 82), LT_RGB(34, 34, 38));
    p_round(p, P - 2, P - 2, p->W - 2 * P + 4, p->H - 2 * P + 4, corner - 1, darken(p->bc, 30));
    p_round(p, P, P, p->W - 2 * P, p->H - 2 * P, corner - 2, p->bc);
    p_vgrad(p, P + 1, P + 1, p->W - 2 * P - 2, (p->H - 2 * P) * 0.5,
            alpha(lighten(p->bc, 22), 150), alpha(p->bc, 0));
    resolve_text(p->c->title, t, sizeof t);
    resolve_text(p->c->units, u, sizeof u);
    fmt_val(p, p->v, p->c->value_digits, buf, sizeof buf);
    if (p->H < 40.0 || p->H < p->W * 0.4) {
        /* compact: title left, value right */
        double vh = LT_MIN(p->H - 2 * P - 2, vs);
        double tw = p_text(p, P + 4, p->H / 2, t, LT_MIN(ts, vh * 0.8), 0, 1, p->tc, p->W * 0.4);
        LT_UNUSED(tw);
        if (u[0]) { lt_snprintf(buf + strlen(buf), sizeof buf - strlen(buf), " %s", u); }
        p_val(p, p->W - P - 4, p->H / 2, buf, vh, 2, 1, p->vc, p->W * 0.62 - P);
        return;
    }
    p_text(p, P + 5, P + 2 + ts * 0.5, t, ts, 0, 1, p->tc, p->W - 2 * P - 10);
    p_val(p, p->W / 2, p->H / 2 + ts * 0.3, buf, vs, 1, 1, p->vc, p->W - 2 * P - 12);
    us = LT_MAX(8.0, m * 0.11);
    if (u[0]) { p_text(p, p->W / 2, p->H - P - us * 0.9, u, us, 1, 0, alpha(p->tc, 220), p->W - 2 * P); }
}

/* ====================================================================== */
/* Bars                                                                    */
static void bar_head(gp *p, double h)
{
    char t[96], u[48], v[64], buf[160];
    resolve_text(p->c->title, t, sizeof t);
    resolve_text(p->c->units, u, sizeof u);
    fmt_val(p, p->v, p->c->value_digits, v, sizeof v);
    lt_snprintf(buf, sizeof buf, "%s%s%s", v, u[0] ? " " : "", u);
    {
        double vw = p_val(p, p->W - 6, 3 + h * 0.55, buf, h, 2, 1, p->vc, p->W * 0.55);
        p_text(p, 6, 3 + h * 0.55, t, h, 0, 1, p->tc, p->W - 12 - vw - 8);
    }
}

static void paint_hbar(gp *p)
{
    double P = 6.0, barH = p->H * 0.35, barY = (p->H - barH) / 2 + p->H * 0.08;
    double barW = p->W - 12, pc = clamp01(pct_of(p, p->v)), f = barW * pc, r;
    lt_color lc = level_color(p);
    int i;
    p_vgrad(p, 0, 0, p->W, p->H, lighten(p->bc, 10), darken(p->bc, 15));
    lt_gfx_rect(p->g, p->r, alpha(p->tc, 80));
    bar_head(p, LT_MAX(9.0, p->H * 0.17));
    if (barH < 4) { barH = 4; }
    r = LT_MIN(4.0, barH * 0.3);
    p_round(p, P, barY, barW, barH, r, LT_RGB(20, 20, 22));
    p_vgrad(p, P + 1, barY + 1, barW - 2, barH - 2, LT_RGB(34, 34, 36), LT_RGB(52, 52, 56));
    if (f >= 2.0) {
        p_round(p, P, barY, f, barH, r, darken(lc, 20));
        p_vgrad(p, P + 1, barY + 1, f - 2, barH / 2.0, lighten(lc, 32), lighten(lc, 8));
        p_vgrad(p, P + 1, barY + barH / 2.0, f - 2, barH / 2.0 - 1, lc, darken(lc, 25));
    }
    for (i = 1; i < 10; i++) {
        double x = P + barW * i / 10.0;
        p_fill(p, x, barY + barH - 3, 1, 3, alpha(p->tc, 120));
    }
    {
        char a[32], b[32];
        double lh = LT_MAX(8.0, p->H * 0.10);
        fmt_val(p, p->mn, 0, a, sizeof a); fmt_val(p, p->mx, 0, b, sizeof b);
        p_text(p, P, barY + barH + lh * 0.9, a, lh, 0, 0, p->tc, 0);
        p_text(p, P + barW, barY + barH + lh * 0.9, b, lh, 2, 0, p->tc, 0);
    }
}

static void paint_vbar(gp *p)
{
    double P = 6.0, labelH = p->H * 0.12, barW = p->W * 0.45, barH = p->H - 2 * labelH - 3 * P;
    double barX = (p->W - barW) / 2, barY = labelH + 1.5 * P, pc = clamp01(pct_of(p, p->v));
    double f, r = LT_MIN(4.0, barW * 0.15);
    lt_color lc = level_color(p);
    char v[48];
    int i;
    p_vgrad(p, 0, 0, p->W, p->H, lighten(p->bc, 8), darken(p->bc, 15));
    lt_gfx_rect(p->g, p->r, alpha(p->tc, 80));
    draw_title(p, p->W / 2, 3 + labelH * 0.45, LT_MAX(9.0, labelH * 0.75), 1, p->W - 8);
    if (barH < 6) { return; }
    p_round(p, barX, barY, barW, barH, r, LT_RGB(18, 18, 20));
    p_hgrad(p, barX + 1, barY + 1, barW - 2, barH - 2, LT_RGB(30, 30, 33), LT_RGB(46, 46, 50));
    f = barH * pc;
    if (f >= 2.0) {
        p_round(p, barX, barY + barH - f, barW, f, r, darken(lc, 20));
        p_hgrad(p, barX + 1, barY + barH - f + 1, barW / 2.0, f - 2, darken(lc, 12), lighten(lc, 28));
        p_hgrad(p, barX + barW / 2.0, barY + barH - f + 1, barW / 2.0 - 1, f - 2, lighten(lc, 28),
                darken(lc, 15));
    }
    for (i = 0; i <= 5; i++) {
        double y = barY + barH - barH * i / 5.0;
        char tk[24];
        p_fill(p, barX + barW + 2, y, 4, 1, alpha(p->tc, 160));
        if (p->W - (barX + barW) > 28) {
            fmt_val(p, p->mn + (p->mx - p->mn) * i / 5.0, 0, tk, sizeof tk);
            p_text(p, barX + barW + 8, y, tk, LT_MAX(7.0, p->H * 0.055), 0, 0, p->tc, 0);
        }
    }
    fmt_val(p, p->v, p->c->value_digits, v, sizeof v);
    p_val(p, p->W / 2, p->H - labelH * 0.8, v, LT_MAX(10.0, labelH * 0.85), 1, 1, p->vc, p->W - 6);
}

static void paint_hline(gp *p)
{
    double P = 8.0, lineY = p->H * 0.55, lineW = p->W - 16, pc = clamp01(pct_of(p, p->v));
    lt_color lc = level_color(p);
    char a[32], b[32];
    double lh = LT_MAX(8.0, p->H * 0.14);
    p_vgrad(p, 0, 0, p->W, p->H, lighten(p->bc, 8), darken(p->bc, 8));
    bar_head(p, LT_MAX(9.0, p->H * 0.17));
    p_line(p, P, lineY, P + lineW, lineY, 6, LT_RGB(24, 24, 26));
    p_disc(p, P, lineY, 3, LT_RGB(24, 24, 26)); p_disc(p, P + lineW, lineY, 3, LT_RGB(24, 24, 26));
    p_line(p, P, lineY, P + lineW, lineY, 4, LT_RGB(60, 60, 64));
    if (pc > 0.0) { p_line(p, P, lineY, P + lineW * pc, lineY, 4, lc); p_disc(p, P, lineY, 2, lc); }
    p_disc(p, P + lineW * pc, lineY, 7, darken(lc, 45));
    p_disc(p, P + lineW * pc, lineY, 6, lc);
    p_disc(p, P + lineW * pc - 2, lineY - 2, 2.2, lighten(lc, 60));
    fmt_val(p, p->mn, 0, a, sizeof a); fmt_val(p, p->mx, 0, b, sizeof b);
    p_text(p, P, p->H - lh * 0.8, a, lh, 0, 0, p->tc, 0);
    p_text(p, P + lineW, p->H - lh * 0.8, b, lh, 2, 0, p->tc, 0);
}

static void paint_dashed(gp *p, int vertical)
{
    int N = vertical ? 12 : 14, i, filled;
    double pc = clamp01(pct_of(p, p->v)), P = 6.0, gap = 3.0, range = p->mx - p->mn;
    char v[48], t[96];
    double th = LT_MAX(9.0, p->H * (vertical ? 0.08 : 0.12));
    p_vgrad(p, 0, 0, p->W, p->H, darken(p->bc, 5), lighten(p->bc, 6));
    lt_gfx_rect(p->g, p->r, alpha(p->tc, 80));
    resolve_text(p->c->title, t, sizeof t);
    fmt_val(p, p->v, p->c->value_digits, v, sizeof v);
    p_text(p, p->W / 2, 3 + th * 0.6, t, th, 1, 1, p->tc, p->W - 8);
    filled = (int)ceil(pc * N - 1e-9);
    if (!vertical) {
        double titleH = p->H * 0.17, valueH = p->H * 0.17, barH = p->H * 0.38, barW = p->W - 12;
        double barY = titleH + (p->H - titleH - valueH - barH) / 2, segW = barW / N;
        for (i = 0; i < N; i++) {
            double sv = p->mn + (double)i / N * range;
            int on = i < filled;
            lt_color c = (p->s->lim.has_hc && sv >= p->s->lim.hc) ? (on ? p->cc : LT_RGB(64, 16, 16))
                : ((p->s->lim.has_hw && sv >= p->s->lim.hw) ? (on ? p->wc : LT_RGB(64, 48, 16))
                : (on ? p->nc : LT_RGB(48, 48, 50)));
            p_round(p, P + i * segW, barY, segW - gap, barH, 2, darken(c, on ? 25 : 10));
            if (on) {
                p_vgrad(p, P + i * segW + 1, barY + 1, segW - gap - 2, barH / 2, lighten(c, 35), c);
                p_vgrad(p, P + i * segW + 1, barY + barH / 2, segW - gap - 2, barH / 2 - 1, c, darken(c, 18));
            } else {
                p_round(p, P + i * segW + 1, barY + 1, segW - gap - 2, barH - 2, 2, c);
            }
        }
        p_val(p, p->W / 2, p->H - valueH * 0.55, v, LT_MAX(10.0, valueH * 0.95), 1, 1, p->vc, p->W - 8);
    } else {
        double labelH = th + 6, barW = p->W * 0.55, barH = p->H - 2 * labelH - 2 * P;
        double barX = (p->W - barW) / 2, barY = labelH + P, segH = barH / N;
        if (segH < 3) { return; }
        for (i = 0; i < N; i++) {
            double sv = p->mn + (double)i / N * range, y = barY + (N - 1 - i) * segH;
            int on = i < filled;
            lt_color c = (p->s->lim.has_hc && sv >= p->s->lim.hc) ? (on ? p->cc : LT_RGB(64, 16, 16))
                : ((p->s->lim.has_hw && sv >= p->s->lim.hw) ? (on ? p->wc : LT_RGB(64, 48, 16))
                : (on ? p->nc : LT_RGB(48, 48, 50)));
            p_round(p, barX, y, barW, segH - gap, 2, darken(c, on ? 25 : 10));
            if (on) { p_hgrad(p, barX + 1, y + 1, barW - 2, segH - gap - 2, lighten(c, 25), darken(c, 12)); }
            else { p_round(p, barX + 1, y + 1, barW - 2, segH - gap - 2, 2, c); }
        }
        p_val(p, p->W / 2, p->H - labelH * 0.5, v, LT_MAX(10.0, labelH * 0.8), 1, 1, p->vc, p->W - 6);
    }
}

/* ====================================================================== */
/* Histogram / graphs                                                      */
static void graph_panel(gp *p, double x, double y, double w, double h)
{
    int i;
    p_round(p, x - 2, y - 2, w + 4, h + 4, 4, LT_RGB(22, 22, 24));
    p_fill(p, x, y, w, h, LT_RGB(14, 15, 17));
    for (i = 1; i <= 3; i++) { p_fill(p, x, y + h * i / 4.0, w, 1, LT_RGB(44, 46, 50)); }
}

static void paint_histogram(gp *p)
{
    double P = 8.0, titleH = p->H * 0.14, valueH = p->H * 0.12;
    double gw = p->W - 16, gh = p->H - titleH - valueH - 16, gy = titleH + 8, bw;
    double pc = clamp01(pct_of(p, p->v)), c0 = floor(pc * 20.0), pw = pct_of(p, p->s->lim.hw);
    double pcr = pct_of(p, p->s->lim.hc);
    int i;
    char v[48];
    p_vgrad(p, 0, 0, p->W, p->H, lighten(p->bc, 5), darken(p->bc, 10));
    draw_title(p, P, 3 + titleH * 0.5, LT_MAX(9.0, titleH * 0.8), 0, p->W - 16);
    if (gh < 6) { return; }
    graph_panel(p, P, gy, gw, gh);
    bw = (gw - 19 * 2) / 20.0;
    for (i = 0; i < 20; i++) {
        double h = gh * exp(-((double)i - c0) * ((double)i - c0) / 20.0), pb = (double)i / 20.0;
        lt_color c = (p->s->lim.has_hc && pb >= pcr) ? p->cc : ((p->s->lim.has_hw && pb >= pw) ? p->wc : p->nc);
        if (h < gh * 0.05) { h = gh * 0.05; }
        p_vgrad(p, P + i * (bw + 2), gy + gh - h, bw, h, lighten(c, 25), darken(c, 15));
    }
    p_line(p, P + gw * pc, gy, P + gw * pc, gy + gh, 1.5, LT_RGB(255, 255, 255));
    fmt_val(p, p->v, p->c->value_digits, v, sizeof v);
    p_val(p, p->W / 2, p->H - valueH * 0.5 - 4, v, LT_MAX(10.0, valueH * 0.9), 1, 1, p->vc, p->W - 8);
}

static void paint_linegraph(gp *p)
{
    double P = 8.0, titleH = p->H * 0.14, gw = p->W - 16, gh = p->H - titleH - 16, gy = titleH + 8;
    double range = p->mx - p->mn;
    int n, i, step, k;
    char v[64], u[40], buf[110];
    double pts[2 * 400];
    const lt_dash_ring *rg = &p->s->ring[0];
    lt_color lc = p->s->zone >= 2 ? p->cc : (p->s->zone == 1 ? p->wc : p->fc);
    p_vgrad(p, 0, 0, p->W, p->H, lighten(p->bc, 5), darken(p->bc, 10));
    draw_title(p, P, 3 + titleH * 0.5, LT_MAX(9.0, titleH * 0.8), 0, p->W * 0.5);
    resolve_text(p->c->units, u, sizeof u);
    fmt_val(p, p->v, p->c->value_digits, v, sizeof v);
    lt_snprintf(buf, sizeof buf, "%s%s%s", v, u[0] ? " " : "", u);
    p_val(p, p->W - P, 3 + titleH * 0.5, buf, LT_MAX(10.0, titleH * 0.9), 2, 1, p->vc, p->W * 0.5);
    if (gh < 8 || gw < 8) { return; }
    graph_panel(p, P, gy, gw, gh);
    n = rg->n;
    if (n < 2 || range <= 0.0) {
        double y = gy + gh - clamp01(pct_of(p, p->v)) * gh;
        p_line(p, P, y, P + gw, y, 1.5, alpha(lc, 200));
    } else {
        double ema = 0.0;
        step = (n + 399) / 400;
        k = 0;
        for (i = 0; i < n; i += step) {
            double s = lt_dash_ring_get(rg, i);
            ema = (i == 0) ? s : ema + (s - ema) * 0.3;
            pts[2 * k] = P + gw * i / (double)(n - 1);
            pts[2 * k + 1] = gy + gh - clamp01((ema - p->mn) / range) * gh;
            k++;
        }
        if (k >= 2) {
            double poly[2 * 402];
            int j;
            for (j = 0; j < 2 * k; j++) { poly[j] = pts[j]; }
            poly[2 * k] = pts[2 * (k - 1)]; poly[2 * k + 1] = gy + gh;
            poly[2 * k + 2] = pts[0]; poly[2 * k + 3] = gy + gh;
            {
                double *q = poly; int m = k + 2, z;
                for (z = 0; z < m; z++) { q[2 * z] += p->r.x; q[2 * z + 1] += p->r.y; }
                lt_gfx_fill_poly(p->g, q, m, alpha(lc, 48));
            }
            for (j = 1; j < k; j++) {
                p_line(p, pts[2 * j - 2], pts[2 * j - 1], pts[2 * j], pts[2 * j + 1], 1.8, lc);
            }
            p_disc(p, pts[2 * k - 2], pts[2 * k - 1], 3.0, lc);
        }
    }
    {
        char a[24], b[24];
        double lh = LT_MAX(7.0, p->H * 0.08);
        fmt_val(p, p->mx, p->c->value_digits, a, sizeof a); fmt_val(p, p->mn, p->c->value_digits, b, sizeof b);
        p_text(p, P + 3, gy + lh * 0.8, a, lh, 0, 0, alpha(p->tc, 200), 0);
        p_text(p, P + 3, gy + gh - lh * 0.8, b, lh, 0, 0, alpha(p->tc, 200), 0);
    }
}

static void paint_multitrend(gp *p)
{
    int ns = p->s->n_series, i, cols, rows, k;
    double P = 6.0, titleH = LT_MAX(14.0, p->H * 0.08), rowH = LT_MAX(11.0, p->H * 0.045);
    double legendH, gy, gw = p->W - 12, gh;
    char t[96];
    if (ns < 1) { ns = 1; }
    cols = (int)((p->W - 12) / 110.0); if (cols < 2) { cols = 2; }
    rows = (ns + cols - 1) / cols;
    legendH = rows * rowH + 4;
    gy = titleH + legendH + 3; gh = p->H - gy - 6;
    p_vgrad(p, 0, 0, p->W, p->H, lighten(p->bc, 4), darken(p->bc, 8));
    resolve_text(p->c->title, t, sizeof t); upper(t);
    p_text(p, P, 2 + titleH * 0.5, t, LT_MAX(9.0, titleH * 0.85), 0, 1, p->tc, p->W - 12);
    if (gh < 8) { return; }
    graph_panel(p, P, gy, gw, gh);
    for (k = ns - 1; k >= -1; k--) {
        int s = k < 0 ? 0 : k;
        char key[48];
        const char *e;
        lt_color col = (s == 0) ? p->fc : LT_RGB(148, 163, 184);
        double mn = p->mn, mx = p->mx, ema = 0.0;
        const lt_dash_ring *rg = &p->s->ring[s];
        int n = rg->n, prevk = 0;
        double px = 0, py = 0;
        if (k < 0) { break; }
        if (s > 0) {
            lt_snprintf(key, sizeof key, "lt_series%d_color", s + 1);
            e = lt_dash_extras_get(&p->c->extras, key);
            if (e != NULL) { lt_color cc; if (lt_color_from_css(e, &cc)) { col = opq(cc); } }
            lt_snprintf(key, sizeof key, "lt_series%d_min", s + 1);
            e = lt_dash_extras_get(&p->c->extras, key); mn = e ? atof(e) : 0.0;
            lt_snprintf(key, sizeof key, "lt_series%d_max", s + 1);
            e = lt_dash_extras_get(&p->c->extras, key); mx = e ? atof(e) : 100.0;
        }
        if (mx <= mn) { mx = mn + 1.0; }
        for (i = 0; i < n; i++) {
            double sm = lt_dash_ring_get(rg, i), x, y;
            ema = (i == 0) ? sm : ema + (sm - ema) * 0.3;
            x = P + gw * i / (double)(n > 1 ? n - 1 : 1);
            y = gy + gh - clamp01((ema - mn) / (mx - mn)) * gh;
            if (prevk) { p_line(p, px, py, x, y, s == 0 ? 2.0 : 1.4, col); }
            px = x; py = y; prevk = 1;
        }
        {
            int col_i = s % cols, row_i = s / cols;
            double lx = P + col_i * (p->W - 12) / cols, ly = titleH + row_i * rowH + rowH / 2 + 2;
            const char *lab = p->s->series_ch[s] ? p->s->series_ch[s] : "";
            lt_snprintf(key, sizeof key, "lt_series%d_label", s + 1);
            e = lt_dash_extras_get(&p->c->extras, key);
            if (e != NULL && e[0]) { lab = e; }
            p_disc(p, lx + 4, ly, 3, col);
            lt_snprintf(t, sizeof t, "%s %.*f", lab, p->c->value_digits, p->s->series_latest[s]);
            p_text(p, lx + 11, ly, t, LT_MAX(7.0, rowH * 0.8), 0, 0, p->tc,
                   (p->W - 12) / cols - 14);
        }
        if (s == 0) { break; }
    }
}

/* ====================================================================== */
/* Analog dial family                                                      */
static double dial_angle(const gp *p, const lt_gauge_arc *a, double pc)
{
    double d = a->start_deg + (a->ccw ? -1.0 : 1.0) * pc * a->sweep_deg;
    LT_UNUSED(p);
    return rad(d);
}

static void paint_dial(gp *p)
{
    double size = LT_MIN(p->W, p->H), cx = p->W / 2, cy = p->H / 2;
    double radius = size / 2 - LT_MAX(3.0, size * 0.02), bezelW, faceR, zr, zw;
    lt_gauge_arc a;
    double fs, fe, mid, dirx, diry, pc, ang, len, w, major, range = p->mx - p->mn;
    int nMajor, i, minorPer, total;
    double tickR;
    char buf[64], t[96], u[48];
    lt_dash_limits *lim = (lt_dash_limits *)&p->s->lim;
    lt_color tc = p->tc;
    lt_gfx *g = p->g;
    double lh;

    lt_gauge_resolve_arc(p->c, &a);
    fs = rad(a.face_start_deg); fe = fs + rad(a.face_deg);
    mid = (fs + fe) / 2.0; dirx = -cos(mid); diry = -sin(mid);
    bezelW = p->c->border_width > 0 ? LT_MIN(radius * 0.3, (double)p->c->border_width)
                                      : LT_MAX(5.0, radius * 0.07);
    if (radius < 10) { return; }
    faceR = radius - bezelW - 1;
    /* face */
    if (a.full_circle) {
        p_disc(p, cx, cy, radius, LT_RGB(18, 18, 20));
        p_disc(p, cx, cy, faceR + 1, darken(p->bc, 10));
        p_disc(p, cx, cy, faceR, p->bc);
        /* subtle upper highlight */
        p_sector(p, cx, cy, faceR * 0.55, faceR, rad(200), rad(340), alpha(LT_RGB(255, 255, 255), 10));
        p_metal(p, cx, cy, faceR + 1, radius, 0, 2 * LT_PI, tc);
    } else {
        double poly[2 * 32];
        int k, np = 20;
        poly[0] = cx; poly[1] = cy;
        for (k = 0; k <= np; k++) {
            double aa = fs + (fe - fs) * k / np;
            poly[2 + 2 * k] = cx + cos(aa) * radius; poly[3 + 2 * k] = cy + sin(aa) * radius;
        }
        p_poly(p, poly, np + 2, LT_RGB(18, 18, 20));
        for (k = 0; k <= np; k++) {
            double aa = fs + (fe - fs) * k / np;
            poly[2 + 2 * k] = cx + cos(aa) * (faceR + 1); poly[3 + 2 * k] = cy + sin(aa) * (faceR + 1);
        }
        p_poly(p, poly, np + 2, p->bc);
        p_metal(p, cx, cy, faceR + 1, radius, fs, fe, tc);
    }
    /* zones */
    zr = faceR - 4; zw = LT_MAX(4.0, faceR * 0.06);
    if (range > 0.0) {
        double endp = dial_angle(p, &a, 1.0);
        if (lim->has_hw) { p_arc(p, cx, cy, zr, dial_angle(p, &a, clamp01(pct_of(p, lim->hw))), endp, zw, p->wc); }
        if (lim->has_hc) { p_arc(p, cx, cy, zr, dial_angle(p, &a, clamp01(pct_of(p, lim->hc))), endp, zw, p->cc); }
        if (lim->has_lw && lim->lw > p->mn) { p_arc(p, cx, cy, zr, dial_angle(p, &a, 0.0), dial_angle(p, &a, clamp01(pct_of(p, lim->lw))), zw, alpha(p->wc, 200)); }
        if (lim->has_lc && lim->lc > p->mn) { p_arc(p, cx, cy, zr, dial_angle(p, &a, 0.0), dial_angle(p, &a, clamp01(pct_of(p, lim->lc))), zw, p->cc); }
    }
    /* ticks */
    tickR = zr - zw - LT_MAX(2.0, faceR * 0.02);
    major = p->c->major_ticks > 0 ? p->c->major_ticks : range / 10.0;
    if (range > 0.0 && p->c->major_ticks > 0 && range / major < 3.0) { major = range / major; }   /* count, not step */
    if (major <= 0.0 || range <= 0.0) { major = 1.0; nMajor = 1; }
    else {
        nMajor = (int)floor(range / major + 1e-6) + 1;
        if (nMajor > 40) { nMajor = 40; major = range / 39.0; }
    }
    minorPer = p->c->minor_ticks > 0 ? (int)p->c->minor_ticks : 0;
    if (minorPer > 12) { minorPer = 12; }
    lh = LT_MAX(7.0, faceR * 0.115);
    if (minorPer > 0 && nMajor > 1) {
        total = (nMajor - 1) * minorPer;
        for (i = 0; i <= total; i++) {
            double pp, ax, ay;
            if (i % minorPer == 0) { continue; }
            pp = (double)i / total * ((nMajor - 1) * major / range);
            ang = dial_angle(p, &a, pp);
            ax = cos(ang); ay = sin(ang);
            p_line(p, cx + ax * (tickR - faceR * 0.05), cy + ay * (tickR - faceR * 0.05),
                   cx + ax * tickR, cy + ay * tickR, 1.0, darken(tc, 20));
        }
    }
    for (i = 0; i < nMajor; i++) {
        double val = p->mn + i * major, pp = range > 0.0 ? (val - p->mn) / range : 0.0, ax, ay, lr;
        char lab[32];
        if (pp > 1.0 + 1e-6) { break; }
        ang = dial_angle(p, &a, pp);
        ax = cos(ang); ay = sin(ang);
        p_line(p, cx + ax * (tickR - faceR * 0.10), cy + ay * (tickR - faceR * 0.10),
               cx + ax * tickR, cy + ay * tickR, 2.0, tc);
        lr = tickR - faceR * 0.10 - lh * 0.75;
        lt_snprintf(lab, sizeof lab, "%.*f", p->c->label_digits < 0 ? 0 : (p->c->label_digits > 4 ? 4 : p->c->label_digits), val);
        if (nMajor <= 14 || (i % 2) == 0) {
            lt_gfx_vtext(g, p->r.x + cx + ax * lr, p->r.y + cy + ay * lr, lab, lh * VS, 0.0, 1, 1,
                         LT_MAX(1.0, lh * VS / 10.0), p->fc);
        }
    }
    /* texts */
    resolve_text(p->c->title, t, sizeof t);
    resolve_text(p->c->units, u, sizeof u);
    fmt_val(p, p->v, p->c->value_digits, buf, sizeof buf);
    if (u[0]) { lt_snprintf(buf + strlen(buf), sizeof buf - strlen(buf), " %s", u); }
    {
        double tx, ty, vx, vy, vh = LT_MAX(11.0, faceR * 0.17), bw;
        if (a.full_circle) { tx = cx; ty = cy + faceR * 0.34; vx = cx; vy = cy + faceR * 0.64; }
        else { tx = cx + dirx * faceR * 0.1; ty = cy + diry * faceR * 0.1; vx = cx + dirx * faceR * 0.34; vy = cy + diry * faceR * 0.34; }
        p_text(p, tx, ty, t, LT_MAX(9.0, faceR * 0.12), 1, 1, p->fc, faceR * 0.72);
        bw = lt_gfx_vtext_width(buf, vh * VS);
        if (bw > faceR * 0.75) { vh = vh * faceR * 0.75 / bw; bw = faceR * 0.75; }
        p_round(p, vx - bw / 2 - 5, vy - vh / 2 - 3, bw + 10, vh + 6, 4, alpha(LT_RGB(0, 0, 0), 110));
        lt_gfx_rect(g, lt_rect_make(p->r.x + ir(vx - bw / 2 - 5), p->r.y + ir(vy - vh / 2 - 3), ir(bw + 10), ir(vh + 6)), alpha(tc, 60));
        p_val(p, vx, vy, buf, vh, 1, 1, p->vc, 0);
    }
    /* peak */
    if (p->c->peak_hold && p->s->peak.peak > p->mn && range > 0.0) {
        double pk = p->s->peak.peak, pa = dial_angle(p, &a, clamp01(pct_of(p, pk)));
        lt_color pcol = (lim->has_hc && pk >= lim->hc) ? p->cc : ((lim->has_hw && pk >= lim->hw) ? p->wc : tc);
        double ca = cos(pa), sa = sin(pa), r0 = faceR * 0.80, r1 = faceR * 0.93, hw2 = faceR * 0.035;
        double poly[6];
        poly[0] = cx + ca * r0; poly[1] = cy + sa * r0;
        poly[2] = cx + ca * r1 - sa * hw2; poly[3] = cy + sa * r1 + ca * hw2;
        poly[4] = cx + ca * r1 + sa * hw2; poly[5] = cy + sa * r1 - ca * hw2;
        p_poly(p, poly, 3, pcol);
    }
    /* needle */
    pc = range > 0.0 ? (p->v - p->mn) / range : 0.0;
    if (pc < 0.0) { pc = 0.0; } else if (pc > 1.0) { pc = 1.0; }
    ang = dial_angle(p, &a, pc);
    len = p->c->has_needle_length && p->c->needle_length > 0.0
        ? (p->c->needle_length <= 1.5 ? faceR * p->c->needle_length : LT_MIN(faceR, p->c->needle_length))
        : faceR * 0.80;
    w = LT_MAX(3.0, faceR * 0.045);
    {
        double ca = cos(ang), sa = sin(ang), poly[8];
        lt_color nc = p->nc;
        /* shadow */
        poly[0] = cx - ca * len * 0.16 - sa * w + 2; poly[1] = cy - sa * len * 0.16 + ca * w + 2;
        poly[2] = cx + ca * len + 2; poly[3] = cy + sa * len + 2;
        poly[4] = cx - ca * len * 0.16 + sa * w + 2; poly[5] = cy - sa * len * 0.16 - ca * w + 2;
        p_poly(p, poly, 3, alpha(LT_RGB(0, 0, 0), 90));
        /* two-tone body: lit half and dark half for a faceted look */
        poly[0] = cx - ca * len * 0.16 - sa * w; poly[1] = cy - sa * len * 0.16 + ca * w;
        poly[2] = cx + ca * len; poly[3] = cy + sa * len;
        poly[4] = cx; poly[5] = cy;
        p_poly(p, poly, 3, darken(nc, 25));
        poly[0] = cx - ca * len * 0.16 + sa * w; poly[1] = cy - sa * len * 0.16 - ca * w;
        poly[2] = cx + ca * len; poly[3] = cy + sa * len;
        poly[4] = cx; poly[5] = cy;
        p_poly(p, poly, 3, lighten(nc, 15));
        poly[0] = cx - ca * len * 0.16 - sa * w; poly[1] = cy - sa * len * 0.16 + ca * w;
        poly[2] = cx - ca * len * 0.16 + sa * w; poly[3] = cy - sa * len * 0.16 - ca * w;
        poly[4] = cx; poly[5] = cy;
        p_poly(p, poly, 3, nc);
    }
    {
        double cr = LT_MAX(5.0, faceR * 0.09);
        p_disc(p, cx, cy, cr + 1.5, LT_RGB(10, 10, 10));
        p_disc(p, cx, cy, cr, darken(tc, 30));
        p_disc(p, cx - cr * 0.15, cy - cr * 0.15, cr * 0.65, lighten(tc, 30));
        p_disc(p, cx - cr * 0.3, cy - cr * 0.3, cr * 0.25, alpha(LT_RGB(255, 255, 255), 150));
    }
}

/* ---- asymmetric sweep ---- */
static void paint_sweep(gp *p)
{
    double cx = p->W / 2 + p->c->needle_pivot_offset_x, cy = p->H * 0.62 + p->c->needle_pivot_offset_y;
    double radius = LT_MIN(p->W, p->H * 1.15) / 2 - 8, arcW = LT_MAX(14.0, radius * 0.18);
    double sd = p->c->sweep_begin_degree, sw = p->c->sweep_angle > 0 ? p->c->sweep_angle : 120.0;
    double pc = clamp01(pct_of(p, p->v)), a0, a1, ar, tipx, tipy;
    lt_color lc = level_color(p), hi;
    char buf[64], t[96], u[48];
    if (p->c->sweep_begin_degree == 0.0 && p->c->start_angle != 0) { sd = p->c->start_angle; }
    if (sd == 0.0) { sd = 210.0; }
    if (radius < 12) { return; }
    p_vgrad(p, 0, 0, p->W, p->H, lighten(p->bc, 8), darken(p->bc, 12));
    ar = radius - arcW / 2;
    a0 = rad(sd); a1 = a0 + rad(sw);
    p_arc_round(p, cx, cy, ar, a0, a1, arcW, LT_RGB(24, 24, 28));
    p_arc_round(p, cx, cy, ar, a0, a1, arcW - 3, LT_RGB(46, 46, 52));
    if (p->s->lim.has_hw) { p_arc(p, cx, cy, ar, a0 + rad(sw) * clamp01(pct_of(p, p->s->lim.hw)), a1, arcW - 3, alpha(p->wc, 120)); }
    if (p->s->lim.has_hc) { p_arc(p, cx, cy, ar, a0 + rad(sw) * clamp01(pct_of(p, p->s->lim.hc)), a1, arcW - 3, alpha(p->cc, 140)); }
    hi = lighten(lc, 25);
    if (pc > 0.002) {
        double e = a0 + rad(sw) * pc;
        p_arc_round(p, cx, cy, ar, a0, e, arcW - 3, darken(lc, 15));
        p_arc(p, cx, cy, ar + (arcW - 3) * 0.2, a0, e, (arcW - 3) * 0.45, hi);
        tipx = cx + cos(e) * ar; tipy = cy + sin(e) * ar;
        p_disc(p, tipx, tipy, arcW * 0.75, alpha(lc, 50));
        p_disc(p, tipx, tipy, arcW * 0.48, lighten(lc, 50));
        p_disc(p, tipx, tipy, arcW * 0.28, LT_RGB(255, 255, 255));
    }
    resolve_text(p->c->title, t, sizeof t); resolve_text(p->c->units, u, sizeof u);
    fmt_val(p, p->v, p->c->value_digits, buf, sizeof buf);
    p_val(p, cx, cy + radius * 0.08, buf, LT_MAX(14.0, radius * 0.42), 1, 1, p->vc, radius * 1.3);
    p_text(p, cx, cy + radius * 0.42, u, LT_MAX(8.0, radius * 0.14), 1, 0, p->tc, radius * 1.2);
    p_text(p, cx, LT_MAX(9.0, p->H * 0.08), t, LT_MAX(9.0, p->H * 0.11), 1, 1, p->tc, p->W - 10);
}

/* ---- tachometer ---- */
static void paint_tach(gp *p)
{
    double P = LT_MIN(p->W, p->H) * 0.05, radius = LT_MIN(p->W, p->H) / 2 - P, cx = p->W / 2, cy = p->H / 2;
    double sa = rad(135.0), sw = rad(270.0), range = p->mx - p->mn, pc, ang;
    int n, i;
    double step = 1000.0;
    char buf[48], t[96];
    if (radius < 12) { return; }
    p_disc(p, cx, cy, radius, LT_RGB(12, 12, 12));
    p_metal(p, cx, cy, radius * 0.93, radius, 0, 2 * LT_PI, p->tc);
    p_disc(p, cx, cy, radius * 0.93, LT_RGB(22, 22, 25));
    p_sector(p, cx, cy, radius * 0.5, radius * 0.93, rad(195), rad(345), alpha(LT_RGB(255, 255, 255), 9));
    if (range <= 0.0) { return; }
    if (range <= 4000.0) { step = 500.0; }
    if (range > 12000.0) { step = 2000.0; }
    n = (int)floor(p->mx / step + 1e-6);
    /* redline band */
    p_arc(p, cx, cy, radius * 0.88, sa + sw * 0.85, sa + sw, radius * 0.05, alpha(p->cc, 230));
    for (i = 0; i <= n; i++) {
        double tv = i * step, a, ca, sn, rr;
        char lab[16];
        if (tv > p->mx || tv < p->mn) { continue; }
        a = sa + sw * (tv - p->mn) / range; ca = cos(a); sn = sin(a);
        p_line(p, cx + ca * radius * 0.74, cy + sn * radius * 0.74, cx + ca * radius * 0.85,
               cy + sn * radius * 0.85, 2.5, tv >= 0.85 * p->mx ? p->cc : LT_RGB(240, 240, 240));
        rr = radius * 0.60;
        lt_snprintf(lab, sizeof lab, "%d", (int)(tv / (step >= 1000.0 ? 1000.0 : 1.0) + 0.5));
        if (step < 1000.0) { lt_snprintf(lab, sizeof lab, "%.1f", tv / 1000.0); }
        p_text(p, cx + ca * rr, cy + sn * rr, lab, LT_MAX(9.0, radius * 0.16), 1, 1,
               tv >= 0.85 * p->mx ? p->cc : LT_RGB(240, 240, 240), 0);
        if (i < n) {
            int k;
            for (k = 1; k < 5; k++) {
                double mv = tv + step * k / 5.0, ma;
                if (mv > p->mx) { break; }
                ma = sa + sw * (mv - p->mn) / range;
                p_line(p, cx + cos(ma) * radius * 0.80, cy + sin(ma) * radius * 0.80,
                       cx + cos(ma) * radius * 0.85, cy + sin(ma) * radius * 0.85, 1.0, alpha(LT_RGB(200, 200, 200), 170));
            }
        }
    }
    resolve_text(p->c->title, t, sizeof t);
    p_text(p, cx, cy + radius * 0.30, t, LT_MAX(8.0, radius * 0.11), 1, 1, p->tc, radius);
    fmt_val(p, p->v, p->c->value_digits, buf, sizeof buf);
    p_round(p, cx - radius * 0.34, cy + radius * 0.42, radius * 0.68, radius * 0.24, 4, LT_RGB(0, 0, 0));
    p_val(p, cx, cy + radius * 0.54, buf, LT_MAX(10.0, radius * 0.17), 1, 1, p->vc, radius * 0.6);
    pc = clamp01((p->v - p->mn) / range);
    ang = sa + sw * pc;
    {
        double ca = cos(ang), sn = sin(ang), len = radius * 0.80, w = LT_MAX(2.5, radius * 0.035), poly[6];
        poly[0] = cx - ca * radius * 0.12 - sn * w + 2; poly[1] = cy - sn * radius * 0.12 + ca * w + 2;
        poly[2] = cx + ca * len + 2; poly[3] = cy + sn * len + 2;
        poly[4] = cx - ca * radius * 0.12 + sn * w + 2; poly[5] = cy - sn * radius * 0.12 - ca * w + 2;
        p_poly(p, poly, 3, alpha(LT_RGB(0, 0, 0), 110));
        poly[0] = cx - ca * radius * 0.12 - sn * w; poly[1] = cy - sn * radius * 0.12 + ca * w;
        poly[2] = cx + ca * len; poly[3] = cy + sn * len;
        poly[4] = cx - ca * radius * 0.12 + sn * w; poly[5] = cy - sn * radius * 0.12 - ca * w;
        p_poly(p, poly, 3, LT_RGB(235, 40, 40));
    }
    p_disc(p, cx, cy, radius * 0.09, LT_RGB(8, 8, 8));
    p_disc(p, cx, cy, radius * 0.07, darken(p->tc, 20));
    p_disc(p, cx - radius * 0.015, cy - radius * 0.015, radius * 0.035, lighten(p->tc, 45));
}

/* ---- round / round dashed ---- */
static lt_color rz_color(const gp *p, double sv)
{
    double range = p->mx - p->mn;
    if (sv >= p->mx - 0.1 * range) { return p->cc; }
    if (sv >= p->mx - 0.25 * range) { return p->wc; }
    return p->tc;
}

static void paint_round(gp *p, int dashed)
{
    double P = LT_MIN(p->W, p->H) * 0.08, radius = LT_MIN(p->W, p->H) / 2 - P, cx = p->W / 2, cy = p->H / 2;
    double range = p->mx - p->mn;
    int N = dashed ? 30 : 60, i;
    char buf[48], u[40], t[96];
    if (radius < 10) { return; }
    p_disc(p, cx, cy, radius + 1, LT_RGB(10, 10, 10));
    if (dashed) {
        p_metal(p, cx, cy, radius * 0.9, radius, 0, 2 * LT_PI, p->tc);
        p_disc(p, cx, cy, radius * 0.9, p->bc);
    } else {
        p_metal(p, cx, cy, radius * 0.88, radius, 0, 2 * LT_PI, p->tc);
        p_disc(p, cx, cy, radius * 0.88, p->bc);
    }
    for (i = 0; i < N; i++) {
        double sv = p->mn + (dashed ? (double)i / (N - 1) : (double)i / N) * range;
        int lit = sv <= p->v + (dashed ? 0.0 : 0.0);
        lt_color c = rz_color(p, sv), col = lit ? c : darken(c, 62);
        if (dashed) {
            double a = rad(135.0) + (double)i / (N - 1) * rad(270.0), ca = cos(a), sn = sin(a);
            if (lit) { p_line(p, cx + ca * radius * 0.65, cy + sn * radius * 0.65, cx + ca * radius * 0.85, cy + sn * radius * 0.85, radius * 0.11, alpha(c, 60)); }
            p_line(p, cx + ca * radius * 0.65, cy + sn * radius * 0.65, cx + ca * radius * 0.85, cy + sn * radius * 0.85, radius * 0.07, col);
            p_disc(p, cx + ca * radius * 0.65, cy + sn * radius * 0.65, radius * 0.035, col);
            p_disc(p, cx + ca * radius * 0.85, cy + sn * radius * 0.85, radius * 0.035, col);
        } else {
            double a0 = (double)i / N * 2 * LT_PI - LT_PI / 2, a1 = (double)(i + 1) / N * 2 * LT_PI - LT_PI / 2 - rad(1.5);
            p_sector(p, cx, cy, radius * 0.55, radius * 0.85, a0, a1, col);
        }
    }
    resolve_text(p->c->title, t, sizeof t); resolve_text(p->c->units, u, sizeof u);
    fmt_val(p, p->v, p->c->value_digits, buf, sizeof buf);
    p_val(p, cx, cy, buf, LT_MAX(12.0, radius * 0.30), 1, 1, p->vc, radius * 1.0);
    p_text(p, cx, cy + radius * 0.30, u, LT_MAX(8.0, radius * 0.13), 1, 0, p->tc, radius);
    p_text(p, cx, cy - radius * 0.36, t, LT_MAX(8.0, radius * 0.12), 1, 1, p->tc, radius * 0.8);
}

/* ---- semicircle bar / moving bar / fuel meter ---- */
static void paint_analog_bar(gp *p)
{
    double cx = p->W / 2, cy = p->H * 0.85, radius = LT_MIN(p->W * 0.5 - 12, p->H * 0.75), barW = radius * 0.16;
    double pc = clamp01(pct_of(p, p->v));
    lt_color lc = level_color(p);
    char buf[64], t[96];
    int i;
    if (radius < 12) { return; }
    p_vgrad(p, 0, 0, p->W, p->H, lighten(p->bc, 6), darken(p->bc, 10));
    p_metal(p, cx, cy, radius - barW / 2 - 4, radius + barW / 2 + 4, LT_PI, 2 * LT_PI, p->tc);
    p_arc(p, cx, cy, radius, LT_PI, 2 * LT_PI, barW, darken(p->bc, 30));
    if (pc > 0.0) {
        p_arc(p, cx, cy, radius, LT_PI, LT_PI + LT_PI * pc, barW - 4, lc);
        p_arc(p, cx, cy, radius + (barW - 4) * 0.2, LT_PI, LT_PI + LT_PI * pc, (barW - 4) * 0.4, lighten(lc, 30));
    }
    for (i = 0; i <= 10; i++) {
        double a = LT_PI + LT_PI * i / 10.0;
        p_line(p, cx + cos(a) * (radius - barW * 0.9), cy + sin(a) * (radius - barW * 0.9),
               cx + cos(a) * (radius - barW * 0.55), cy + sin(a) * (radius - barW * 0.55), i % 5 ? 1.0 : 2.0, p->tc);
    }
    resolve_text(p->c->title, t, sizeof t);
    fmt_val(p, p->v, p->c->value_digits, buf, sizeof buf);
    p_val(p, cx, cy - radius * 0.28, buf, LT_MAX(12.0, radius * 0.30), 1, 1, p->vc, radius * 1.1);
    p_text(p, cx, cy - radius * 0.02, t, LT_MAX(8.0, radius * 0.12), 1, 1, p->tc, radius * 1.4);
}

static void paint_moving_bar(gp *p)
{
    double cx = p->W / 2, cy = p->H * 0.9, radius = LT_MIN(p->W * 0.5 - 14, p->H * 0.8), barW = radius * 0.09;
    double start = LT_PI + 0.2 * LT_PI, total = 1.4 * LT_PI, range = p->mx - p->mn;
    double wp = p->s->lim.has_hw ? pct_of(p, p->s->lim.hw) : 0.7, dp = p->s->lim.has_hc ? pct_of(p, p->s->lim.hc) : 0.9;
    double pc = clamp01(pct_of(p, p->v)), a;
    char buf[64], t[96];
    if (radius < 12 || range <= 0.0) { return; }
    p_vgrad(p, 0, 0, p->W, p->H, lighten(p->bc, 6), darken(p->bc, 10));
    /* angle(p) = start - p*total in the spec's math orientation; on screen angles grow
       clockwise, so map to start_s = -(...) */
    p_metal(p, cx, cy, radius - barW * 3, radius + barW + 4, LT_PI * 0.98, 2 * LT_PI + LT_PI * 0.02, p->tc);
    p_arc(p, cx, cy, radius - barW, LT_PI * 0.98, 2 * LT_PI + LT_PI * 0.02, barW * 2.0, darken(p->bc, 40));
    p_arc(p, cx, cy, radius - barW, LT_PI * 1.0, 1.0 * LT_PI + LT_PI * wp, barW * 0.9, alpha(p->nc, 220));
    p_arc(p, cx, cy, radius - barW, LT_PI + LT_PI * wp, LT_PI + LT_PI * dp, barW * 0.9, p->wc);
    p_arc(p, cx, cy, radius - barW, LT_PI + LT_PI * dp, 2 * LT_PI, barW * 0.9, p->cc);
    LT_UNUSED(start); LT_UNUSED(total);
    a = LT_PI + LT_PI * pc;
    {
        double ca = cos(a), sn = sin(a), len = radius - barW * 0.5, w = LT_MAX(2.5, radius * 0.03), poly[6];
        poly[0] = cx - sn * w; poly[1] = cy + ca * w;
        poly[2] = cx + ca * len; poly[3] = cy + sn * len;
        poly[4] = cx + sn * w; poly[5] = cy - ca * w;
        p_poly(p, poly, 3, lighten(p->nc, 20));
        p_disc(p, cx, cy, radius * 0.06, p->tc);
    }
    resolve_text(p->c->title, t, sizeof t);
    fmt_val(p, p->v, p->c->value_digits, buf, sizeof buf);
    p_val(p, cx, cy - radius * 0.25, buf, LT_MAX(12.0, radius * 0.26), 1, 1, p->vc, radius);
    p_text(p, cx, LT_MAX(9.0, p->H * 0.07), t, LT_MAX(9.0, p->H * 0.1), 1, 1, p->tc, p->W - 10);
}

static void paint_fuel(gp *p)
{
    double P = LT_MIN(p->W, p->H) * 0.1, radius = LT_MIN(p->W, p->H) / 2 - P, cx = p->W / 2, cy = p->H / 2;
    double a0 = 0.8 * LT_PI, sw = 1.4 * LT_PI, pc = clamp01(pct_of(p, p->v)), ar = radius * 0.7, w = radius * 0.12;
    char buf[48], t[96];
    int i;
    if (radius < 12) { return; }
    p_metal(p, cx, cy, radius * 0.88, radius, 0, 2 * LT_PI, p->tc);
    p_disc(p, cx, cy, radius * 0.88, LT_RGB(10, 10, 12));
    p_arc_round(p, cx, cy, ar, a0, a0 + sw, w, LT_RGB(50, 50, 54));
    if (pc > 0.01) {
        int n = (int)(pc * 28) + 1;
        for (i = 0; i < n; i++) {
            double t0 = pc * i / n, t1 = pc * (i + 1) / n;
            lt_color c = t0 < 0.25 ? mix(p->cc, p->wc, t0 / 0.25) : mix(p->wc, p->tc, (t0 - 0.25) / 0.25);
            p_arc(p, cx, cy, ar, a0 + sw * t0, a0 + sw * t1 + 0.01, w - 3, c);
        }
    }
    p_text(p, cx + cos(a0 + 0.05 * LT_PI) * radius * 0.52, cy + sin(a0 + 0.05 * LT_PI) * radius * 0.52 + 2, "E", LT_MAX(9.0, radius * 0.15), 1, 1, LT_RGB(255, 255, 255), 0);
    p_text(p, cx + cos(a0 + sw - 0.05 * LT_PI) * radius * 0.52, cy + sin(a0 + sw - 0.05 * LT_PI) * radius * 0.52 + 2, "F", LT_MAX(9.0, radius * 0.15), 1, 1, LT_RGB(255, 255, 255), 0);
    fmt_val(p, p->v, p->c->value_digits, buf, sizeof buf);
    resolve_text(p->c->title, t, sizeof t);
    p_val(p, cx, cy + radius * 0.05, buf, LT_MAX(11.0, radius * 0.24), 1, 1, p->vc, radius);
    p_text(p, cx, cy + radius * 0.50, t, LT_MAX(8.0, radius * 0.11), 1, 1, p->tc, radius * 0.6);
}

/* ---- telemetry stat ---- */
static void paint_stat(gp *p)
{
    int compact = p->H < 42.0 || p->H < p->W * 0.45;
    double corner = compact ? 2.0 : LT_MIN(6.0, LT_MIN(p->W * 0.04, p->H * 0.08));
    double accW = compact ? 0.0 : LT_MAX(3.0, p->W * 0.025), P = compact ? LT_MAX(4.0, p->W * 0.04) : LT_MAX(8.0, p->W * 0.06);
    double m = LT_MIN(p->W, p->H), fs = 1.0 + p->c->font_size_adjustment * 0.1, cx0;
    char t[96], u[48], buf[96];
    p_round(p, 0, 0, p->W, p->H, corner, p->bc);
    p_vgrad(p, 1, 1, p->W - 2, p->H - 2, p->bc, darken(p->bc, 8));
    resolve_text(p->c->title, t, sizeof t); upper(t);
    resolve_text(p->c->units, u, sizeof u);
    fmt_val(p, p->v, p->c->value_digits, buf, sizeof buf);
    cx0 = accW + P * (compact ? 0.4 : 0.6);
    if (compact) {
        if (u[0]) { lt_snprintf(buf + strlen(buf), sizeof buf - strlen(buf), " %s", u); }
        p_text(p, cx0, p->H / 2, t, LT_MAX(7.0, m * 0.30 * fs), 0, 1, p->tc, p->W * 0.4);
        p_val(p, p->W - P, p->H / 2, buf, LT_MAX(9.0, m * 0.42 * fs), 2, 1, p->vc, p->W * 0.6 - P);
        return;
    }
    p_fill(p, 0, corner, accW, p->H - 2 * corner, p->nc);
    p_text(p, cx0, P * 0.55 + m * 0.07, t, LT_MAX(9.0, m * 0.15 * fs), 0, 1, p->tc, p->W - cx0 - P);
    {
        double vs = LT_MAX(14.0, m * 0.42 * fs), vw = p_val(p, cx0, p->H * 0.52, buf, vs, 0, 1, p->vc, p->W - cx0 - P - 4);
        if (u[0]) { p_text(p, cx0 + vw + 4, p->H * 0.52 + vs * 0.18, u, LT_MAX(8.0, m * 0.13), 0, 1, p->tc, p->W - cx0 - vw - P - 6); }
    }
    {
        double barH = LT_MAX(3.0, p->H * 0.06), barY = p->H - P * 0.6 - barH, barW = p->W - cx0 - P;
        double pc = clamp01(pct_of(p, p->v));
        p_round(p, cx0, barY, barW, barH, barH / 2, LT_RGB(44, 46, 52));
        if (pc > 0.0) { p_round(p, cx0, barY, LT_MAX(barH, barW * pc), barH, barH / 2, p->nc); }
        if (p->s->lim.has_hw) { p_fill(p, cx0 + barW * clamp01(pct_of(p, p->s->lim.hw)), barY - 2, 1, barH + 4, p->wc); }
        if (p->s->lim.has_hc) { p_fill(p, cx0 + barW * clamp01(pct_of(p, p->s->lim.hc)), barY - 2, 1, barH + 4, p->cc); }
    }
}

/* ====================================================================== */
/* Cell-mode fallback: text readout with a block bar                       */
static void paint_cells(gp *p)
{
    char t[96], u[48], buf[160], bar[40];
    int n = (int)(clamp01(pct_of(p, p->v)) * 20.0 + 0.5), i, rows = p->r.h / 16;
    lt_color c = p->vc;
    resolve_text(p->c->title, t, sizeof t); resolve_text(p->c->units, u, sizeof u);
    lt_gfx_fill_rect(p->g, p->r, LT_RGB(0, 0, 0));
    for (i = 0; i < 20; i++) { bar[i] = i < n ? '#' : '-'; }
    bar[20] = '\0';
    lt_snprintf(buf, sizeof buf, "%.*f %s", p->c->value_digits < 0 ? 0 : p->c->value_digits, p->v, u);
    lt_gfx_text(p->g, p->r.x + 2, p->r.y, t, p->tc, 1);
    if (rows >= 2) { lt_gfx_text(p->g, p->r.x + 2, p->r.y + 16, buf, c, 1); }
    if (rows >= 3) { lt_gfx_text(p->g, p->r.x + 2, p->r.y + 32, bar, c, 1); }
}

/* ====================================================================== */
static void gauge_paint(void *ud, lt_gfx *g, lt_rect r, const lt_dash_gauge *cfg,
                        const lt_dash_gstate *st, double now_ms)
{
    gp p;
    lt_rect prev;
    LT_UNUSED(ud);
    if (r.w < 4 || r.h < 4) { return; }
    init_ctx(&p, g, r, cfg, st, now_ms);
    if (lt_gfx_cell_mode(g)) { paint_cells(&p); return; }
    prev = lt_gfx_push_clip(g, r);
    switch (cfg->painter) {
    case LT_GP_ANALOG: case LT_GP_BASIC_ANALOG: case LT_GP_CIRCLE_ANALOG: paint_dial(&p); break;
    case LT_GP_ASYM_SWEEP: paint_sweep(&p); break;
    case LT_GP_HBAR: paint_hbar(&p); break;
    case LT_GP_VBAR: paint_vbar(&p); break;
    case LT_GP_HLINE: paint_hline(&p); break;
    case LT_GP_HDASHED: paint_dashed(&p, 0); break;
    case LT_GP_VDASHED: paint_dashed(&p, 1); break;
    case LT_GP_ANALOG_BAR: paint_analog_bar(&p); break;
    case LT_GP_ANALOG_MOVING_BAR: paint_moving_bar(&p); break;
    case LT_GP_HISTOGRAM: paint_histogram(&p); break;
    case LT_GP_LINE_GRAPH: paint_linegraph(&p); break;
    case LT_GP_MULTI_TREND: paint_multitrend(&p); break;
    case LT_GP_ROUND: paint_round(&p, 0); break;
    case LT_GP_ROUND_DASHED: paint_round(&p, 1); break;
    case LT_GP_FUEL_METER: paint_fuel(&p); break;
    case LT_GP_TACHOMETER: paint_tach(&p); break;
    case LT_GP_TELEMETRY_STAT: paint_stat(&p); break;
    case LT_GP_BASIC_READOUT: default: paint_readout(&p); break;
    }
    lt_gfx_pop_clip(g, prev);
}

/* ---- indicators ---- */
static void indicator_paint(void *ud, lt_gfx *g, lt_rect r, const lt_dash_indicator *in, lt_bool on)
{
    lt_color bg = opq(on ? in->on_background_color : in->off_background_color);
    lt_color fg = opq(on ? in->on_text_color : in->off_text_color);
    const char *t = on ? in->on_text : in->off_text;
    lt_rect prev;
    gp p;
    double h;
    LT_UNUSED(ud);
    if (r.w < 3 || r.h < 3) { return; }
    memset(&p, 0, sizeof p);
    p.g = g; p.r = r; p.W = r.w; p.H = r.h;
    if (lt_gfx_cell_mode(g)) {
        lt_gfx_fill_rect(g, r, bg);
        if (t) { lt_gfx_text(g, r.x + 1, r.y, t, fg, 1); }
        return;
    }
    prev = lt_gfx_push_clip(g, r);
    h = LT_MAX(8.0, LT_MIN(p.H * 0.5, 15.0));
    if (in->painter == LT_IP_BULB) {
        double rr = LT_MIN(p.W, p.H) * 0.4;
        p_disc(&p, p.W / 2, p.H / 2, rr + 1.5, LT_RGB(20, 20, 20));
        p_disc(&p, p.W / 2, p.H / 2, rr, bg);
        if (on) { p_disc(&p, p.W / 2, p.H / 2, rr * 1.35, alpha(bg, 40)); }
        p_disc(&p, p.W / 2 - rr * 0.3, p.H / 2 - rr * 0.3, rr * 0.35, alpha(LT_RGB(255, 255, 255), on ? 150 : 60));
        if (t && t[0]) { p_text(&p, p.W / 2, p.H / 2, t, LT_MAX(7.0, rr * 0.6), 1, 1, fg, rr * 1.6); }
    } else {
        double cr = in->painter == LT_IP_LED ? LT_MIN(p.H / 2, 8.0) : 3.0;
        p_round(&p, 0, 0, p.W, p.H, cr, darken(bg, 55));
        p_round(&p, 1, 1, p.W - 2, p.H - 2, cr, bg);
        if (on) { p_vgrad(&p, 2, 2, p.W - 4, p.H * 0.4, alpha(lighten(bg, 40), 140), alpha(bg, 0)); }
        if (t && t[0]) { p_text(&p, p.W / 2, p.H / 2, t, h, 1, 1, fg, p.W - 8); }
    }
    lt_gfx_pop_clip(g, prev);
}

void lt_gauges_install(void)
{
    static const lt_dash_painters pt = { NULL, gauge_paint, indicator_paint };
    lt_dash_set_painters(&pt);
}
