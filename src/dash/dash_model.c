/* dash_model.c - colours, painter names, extras and construction of the dash model.
 * Spec: docs/spec/05-dash-gauges.md sections 1.1 - 1.5.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <time.h>
#include "dash_priv.h"

/* ---------------------------------------------------------------- helpers */

void dash_set_str(char **p, const char *s)
{
    char *n = s ? lt_strdup(s) : NULL;
    lt_free(*p);
    *p = n;
}

void dash_now_str(char *dst, size_t cap, lt_bool with_time)
{
    time_t t = time(NULL);
    struct tm *tmv = localtime(&t);
    if (tmv == NULL ||
        strftime(dst, cap, with_time ? "%Y-%m-%d %H:%M:%S" : "%Y-%m-%d", tmv) == 0) {
        lt_strlcpy(dst, "1970-01-01", cap);
    }
}

lt_bool dash_parse_i32(const char *s, long *out)
{
    const char *p = s;
    int neg = 0;
    double acc = 0.0;
    int any = 0;

    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p == '+' || *p == '-') {
        neg = (*p == '-');
        p++;
    }
    while (*p >= '0' && *p <= '9') {
        acc = acc * 10.0 + (double)(*p - '0');
        if (acc > 4294967296.0) return LT_FALSE;
        any = 1;
        p++;
    }
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (!any || *p != '\0') return LT_FALSE;
    if (neg) acc = -acc;
    if (acc < -2147483648.0 || acc > 2147483647.0) return LT_FALSE;
    *out = (long)acc;
    return LT_TRUE;
}

/* multiply little-endian decimal digit array (base 10) by small factor f */
static int big_mul(unsigned char *dig, int n, long f)
{
    long carry = 0;
    int i;
    for (i = 0; i < n; i++) {
        long t = (long)dig[i] * f + carry;
        dig[i] = (unsigned char)(t % 10);
        carry = t / 10;
    }
    while (carry > 0) {
        dig[n++] = (unsigned char)(carry % 10);
        carry /= 10;
    }
    return n;
}

/* Shortest decimal text that parses back to v (like Rust's f64 Display, never exponent form).
 * Exact: expands the double to its full decimal value with a tiny bignum, then rounds to the
 * fewest significant digits that strtod maps back to v (lt_snprintf is not exact enough). */
void dash_fmt_f64(char *dst, size_t cap, double v)
{
    unsigned char dig[1200];
    char D[1200], cand[64], out[1400];
    double av, f, m;
    int e2, n, fd, i, p, nd, pos10, k, o;

    if (lt_isnan(v)) {
        lt_strlcpy(dst, "NaN", cap);
        return;
    }
    if (!lt_isfinite(v)) {
        lt_strlcpy(dst, v < 0 ? "-inf" : "inf", cap);
        return;
    }
    if (v == 0.0) {
        lt_strlcpy(dst, "0", cap);
        return;
    }
    av = fabs(v);
    f = frexp(av, &e2);
    m = ldexp(f, 53);
    e2 -= 53;
    n = 0;
    while (m >= 1.0) {
        double d = fmod(m, 10.0);
        dig[n++] = (unsigned char)d;
        m = (m - d) / 10.0;
    }
    fd = 0;
    if (e2 >= 0) {
        for (k = e2; k > 0;) {
            int st = k > 20 ? 20 : k;
            n = big_mul(dig, n, 1L << st);
            k -= st;
        }
    } else {
        fd = -e2;
        for (k = fd; k > 0;) {
            int st = k > 8 ? 8 : k;
            long f5 = 1;
            for (i = 0; i < st; i++) f5 *= 5;
            n = big_mul(dig, n, f5);
            k -= st;
        }
    }
    /* D = most significant first, no leading zeros */
    while (n > 1 && dig[n - 1] == 0) n--;
    for (i = 0; i < n; i++) D[i] = (char)('0' + dig[n - 1 - i]);
    D[n] = '\0';
    pos10 = n - 1 - fd;

    for (p = 1; p <= 17; p++) {
        int bump = 0, pe;
        char R[20];
        for (i = 0; i < p; i++) R[i] = (i < n) ? D[i] : '0';
        if (n > p && D[p] >= '5') {
            i = p - 1;
            while (i >= 0 && R[i] == '9') {
                R[i] = '0';
                i--;
            }
            if (i >= 0) {
                R[i]++;
            } else {
                R[0] = '1';
                for (i = 1; i < p; i++) R[i] = '0';
                bump = 1;
            }
        }
        pe = pos10 + bump;
        R[p] = '\0';
        lt_snprintf(cand, sizeof cand, "%c.%se%d", R[0], p > 1 ? R + 1 : "0", pe);
        if (strtod(cand, NULL) == av || p == 17) {
            nd = p;
            o = 0;
            if (v < 0) out[o++] = '-';
            if (pe >= nd - 1) {
                for (i = 0; i < nd; i++) out[o++] = R[i];
                for (i = 0; i < pe - (nd - 1); i++) out[o++] = '0';
            } else if (pe >= 0) {
                for (i = 0; i <= pe; i++) out[o++] = R[i];
                out[o++] = '.';
                for (i = pe + 1; i < nd; i++) out[o++] = R[i];
            } else {
                out[o++] = '0';
                out[o++] = '.';
                for (i = 0; i < -pe - 1; i++) out[o++] = '0';
                for (i = 0; i < nd; i++) out[o++] = R[i];
            }
            out[o] = '\0';
            lt_strlcpy(dst, out, cap);
            return;
        }
    }
}

/* ---------------------------------------------------------------- colours */

lt_color lt_color_from_argb(lt_s32 v)
{
    return (lt_color)((lt_u32)v & LT_U32_MASK);
}

lt_s32 lt_color_to_argb(lt_color c)
{
    lt_u32 u = (lt_u32)c & LT_U32_MASK;
    if (u >= 0x80000000UL) return (lt_s32)((long)u - 4294967296L);
    return (lt_s32)u;
}

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

lt_bool lt_color_from_css(const char *s, lt_color *out)
{
    size_t n, i;
    int d[8];

    if (s == NULL) return LT_FALSE;
    while (*s == '#') s++;
    n = strlen(s);
    if (n != 6 && n != 8) return LT_FALSE;
    for (i = 0; i < n; i++) {
        d[i] = hexval((unsigned char)s[i]);
        if (d[i] < 0) return LT_FALSE;
    }
    if (out) {
        int r = d[0] * 16 + d[1], g = d[2] * 16 + d[3], b = d[4] * 16 + d[5];
        int a = n == 8 ? d[6] * 16 + d[7] : 255;
        *out = LT_RGBA(r, g, b, a);
    }
    return LT_TRUE;
}

void lt_color_to_css(lt_color c, char out[10])
{
    if (LT_COLOR_A(c) == 255)
        lt_snprintf(out, 10, "#%02x%02x%02x", LT_COLOR_R(c), LT_COLOR_G(c), LT_COLOR_B(c));
    else
        lt_snprintf(out, 10, "#%02x%02x%02x%02x", LT_COLOR_R(c), LT_COLOR_G(c), LT_COLOR_B(c),
                    LT_COLOR_A(c));
}

static int clamp255(int v)
{
    return v < 0 ? 0 : (v > 255 ? 255 : v);
}

lt_color lt_color_lighten(lt_color c, int percent)
{
    int amt = (int)lt_round(2.55 * (double)percent);
    return LT_RGB(clamp255(LT_COLOR_R(c) + amt), clamp255(LT_COLOR_G(c) + amt),
                  clamp255(LT_COLOR_B(c) + amt));
}

lt_color lt_color_darken(lt_color c, int percent)
{
    int amt = (int)lt_round(2.55 * (double)percent);
    return LT_RGB(clamp255(LT_COLOR_R(c) - amt), clamp255(LT_COLOR_G(c) - amt),
                  clamp255(LT_COLOR_B(c) - amt));
}

/* ---------------------------------------------------------------- painters */

static const char *const gp_names[] = {
    "Analog Gauge", "Basic Analog Gauge", "Circle Analog Gauge", "Asymetric Sweep Gauge",
    "Basic Readout", "Horizontal Bar Gauge", "Vertical Bar Gauge", "Horizontal Line Gauge",
    "Horizontal Dashed Bar Gauge", "Vertical Dashed Bar Gauge", "Analog Bar Gauge",
    "Analog Moving Bar Gauge", "Histogram", "Line Graph", "Round Analog Gauge",
    "Round Dashed Gauge", "Fuel Meter", "Tachometer", "Telemetry Stat", "Multi Channel Trend"
};

const char *lt_painter_to_ts(lt_gauge_painter p)
{
    if ((int)p < 0 || (size_t)p >= LT_COUNTOF(gp_names)) return gp_names[LT_GP_BASIC_READOUT];
    return gp_names[p];
}

static lt_bool contains(const char *hay, const char *needle)
{
    return strstr(hay, needle) != NULL;
}

lt_gauge_painter lt_painter_from_ts(const char *s)
{
    char *t, *l, *p;
    lt_gauge_painter r = LT_GP_BASIC_READOUT;
    size_t i;
    static const struct { const char *a, *b; lt_gauge_painter p; } ex[] = {
        { "Analog Gauge", "AnalogGaugePainter", LT_GP_ANALOG },
        { "Basic Analog Gauge", NULL, LT_GP_BASIC_ANALOG },
        { "Circle Analog Gauge", "CircleAnalogGaugePainter", LT_GP_CIRCLE_ANALOG },
        { "Asymetric Sweep Gauge", "AsymetricSweepGaugePainter", LT_GP_ASYM_SWEEP },
        { "Basic Readout", "BasicReadoutGaugePainter", LT_GP_BASIC_READOUT },
        { "Horizontal Bar Gauge", "HorizontalBarPainter", LT_GP_HBAR },
        { "Vertical Bar Gauge", "VerticalBarPainter", LT_GP_VBAR },
        { "Horizontal Line Gauge", "HorizontalLinePainter", LT_GP_HLINE },
        { "Vertical Dashed Bar Gauge", "VerticalDashedBarPainter", LT_GP_VDASHED },
        { "Analog Bar Gauge", "AnalogBarPainter", LT_GP_ANALOG_BAR },
        { "Analog Moving Bar Gauge", "AnalogMovingBarGaugePainter", LT_GP_ANALOG_MOVING_BAR },
        { "Histogram", "HistogramPainter", LT_GP_HISTOGRAM },
        { "Line Graph", "LineGraphPainter", LT_GP_LINE_GRAPH },
        { "Telemetry Stat", NULL, LT_GP_TELEMETRY_STAT },
        { "Multi Channel Trend", NULL, LT_GP_MULTI_TREND }
    };
    static const struct { const char *sub; lt_gauge_painter p; } sf[] = {
        { "analoggaugepainter", LT_GP_ANALOG },
        { "basicreadoutgaugepainter", LT_GP_BASIC_READOUT },
        { "horizontalbarpainter", LT_GP_HBAR },
        { "verticalbarpainter", LT_GP_VBAR },
        { "horizontallinepainter", LT_GP_HLINE },
        { "verticaldashedbarpainter", LT_GP_VDASHED },
        { "analogbarpainter", LT_GP_ANALOG_BAR },
        { "analogmovingbargaugepainter", LT_GP_ANALOG_MOVING_BAR },
        { "histogrampainter", LT_GP_HISTOGRAM },
        { "linegraphpainter", LT_GP_LINE_GRAPH }
    };

    if (s == NULL) return r;
    t = lt_strdup(s);
    lt_str_trim(t);
    l = lt_strdup(t);
    for (p = l; *p; p++) *p = (char)tolower((unsigned char)*p);

    if (contains(l, "roundanaloggaugepainter") || strcmp(t, "Round Analog Gauge") == 0 ||
        strcmp(t, "Round Gauge") == 0) {
        r = LT_GP_ROUND;
    } else if (contains(l, "rounddashed") || strcmp(t, "Round Dashed Gauge") == 0) {
        r = LT_GP_ROUND_DASHED;
    } else if (contains(l, "fuelmeter") || strcmp(t, "Fuel Meter") == 0) {
        r = LT_GP_FUEL_METER;
    } else if (contains(l, "tachometer") || strcmp(t, "Tachometer") == 0) {
        r = LT_GP_TACHOMETER;
    } else if (contains(l, "horizontaldashedbar") || strcmp(t, "Horizontal Dashed Bar Gauge") == 0) {
        r = LT_GP_HDASHED;
    } else {
        lt_bool found = LT_FALSE;
        for (i = 0; i < LT_COUNTOF(ex) && !found; i++) {
            if (strcmp(t, ex[i].a) == 0 || (ex[i].b && strcmp(t, ex[i].b) == 0)) {
                r = ex[i].p;
                found = LT_TRUE;
            }
        }
        for (i = 0; i < LT_COUNTOF(sf) && !found; i++) {
            if (contains(l, sf[i].sub)) {
                r = sf[i].p;
                found = LT_TRUE;
            }
        }
    }
    lt_free(t);
    lt_free(l);
    return r;
}

const char *lt_indpainter_to_ts(lt_ind_painter p)
{
    switch (p) {
    case LT_IP_BULB: return "Bulb Indicator";
    case LT_IP_LED: return "LED";
    default: return "Basic Rectangle Indicator";
    }
}

lt_ind_painter lt_indpainter_from_ts(const char *s)
{
    char *t, *l, *p;
    lt_ind_painter r = LT_IP_RECTANGLE;

    if (s == NULL) return r;
    t = lt_strdup(s);
    lt_str_trim(t);
    l = lt_strdup(t);
    for (p = l; *p; p++) *p = (char)tolower((unsigned char)*p);
    if (strcmp(l, "led") == 0 || contains(l, "ledpainter") || contains(l, "ledindicator"))
        r = LT_IP_LED;
    else if (strcmp(t, "Bulb Indicator") == 0 || contains(l, "bulbindicator"))
        r = LT_IP_BULB;
    else
        r = LT_IP_RECTANGLE;
    lt_free(t);
    lt_free(l);
    return r;
}

/* ---------------------------------------------------------------- extras */

static size_t extras_find(const lt_dash_extras *e, const char *k, lt_bool *found)
{
    size_t lo = 0, hi = e->n;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        int c = strcmp(e->a[mid].k, k);
        if (c == 0) {
            *found = LT_TRUE;
            return mid;
        }
        if (c < 0) lo = mid + 1;
        else hi = mid;
    }
    *found = LT_FALSE;
    return lo;
}

void lt_dash_extras_set(lt_dash_extras *e, const char *k, const char *v)
{
    lt_bool found;
    size_t i = extras_find(e, k, &found);
    if (found) {
        char *nv = lt_strdup(v ? v : "");
        lt_free(e->a[i].v);
        e->a[i].v = nv;
        return;
    }
    e->a = (lt_dash_kv *)lt_realloc(e->a, (e->n + 1) * sizeof(lt_dash_kv));
    if (i < e->n) memmove(&e->a[i + 1], &e->a[i], (e->n - i) * sizeof(lt_dash_kv));
    e->a[i].k = lt_strdup(k);
    e->a[i].v = lt_strdup(v ? v : "");
    e->n++;
}

const char *lt_dash_extras_get(const lt_dash_extras *e, const char *k)
{
    lt_bool found;
    size_t i = extras_find(e, k, &found);
    return found ? e->a[i].v : NULL;
}

static void extras_free(lt_dash_extras *e)
{
    size_t i;
    for (i = 0; i < e->n; i++) {
        lt_free(e->a[i].k);
        lt_free(e->a[i].v);
    }
    lt_free(e->a);
    e->a = NULL;
    e->n = 0;
}

/* ---------------------------------------------------------------- init/free */

void lt_dash_gauge_init(lt_dash_gauge *g)
{
    memset(g, 0, sizeof(*g));
    g->id = lt_strdup("");
    g->painter = LT_GP_BASIC_READOUT;
    g->gauge_style = lt_strdup("Basic Readout");
    g->output_channel = lt_strdup("");
    g->title = lt_strdup("");
    g->units = lt_strdup("");
    g->min = 0.0;
    g->max = 100.0;
    g->peg_limits = LT_TRUE;
    g->back_color = LT_RGB(0, 0, 0);
    g->font_color = LT_RGB(255, 255, 255);
    g->trim_color = LT_RGB(102, 102, 153);
    g->warn_color = LT_RGB(255, 255, 0);
    g->critical_color = LT_RGB(255, 0, 0);
    g->needle_color = LT_RGB(255, 102, 0);
    g->font_family = lt_strdup("");
    g->sweep_angle = 270;
    g->start_angle = 135;
    g->face_angle = 360;
    g->sweep_begin_degree = 135;
    g->major_ticks = -1.0;
    g->minor_ticks = -1.0;
    g->relative_width = 0.25;
    g->relative_height = 0.25;
    g->border_width = 3;
    g->shortest_size = 50;
    g->antialiasing_on = LT_TRUE;
    g->history_delay = 15000;
    g->needle_smoothing = 1;
}

void lt_dash_gauge_free(lt_dash_gauge *g)
{
    lt_free(g->id);
    lt_free(g->gauge_style);
    lt_free(g->output_channel);
    lt_free(g->title);
    lt_free(g->units);
    lt_free(g->min_vp);
    lt_free(g->max_vp);
    lt_free(g->low_warning_vp);
    lt_free(g->high_warning_vp);
    lt_free(g->low_critical_vp);
    lt_free(g->high_critical_vp);
    lt_free(g->font_family);
    lt_free(g->background_image_file_name);
    lt_free(g->needle_image_file_name);
    lt_free(g->short_click_action);
    lt_free(g->long_click_action);
    lt_free(g->enabled_condition);
    extras_free(&g->extras);
    memset(g, 0, sizeof(*g));
}

void lt_dash_indicator_init(lt_dash_indicator *i)
{
    memset(i, 0, sizeof(*i));
    i->id = lt_strdup("");
    i->painter = LT_IP_RECTANGLE;
    i->output_channel = lt_strdup("");
    i->on_text = lt_strdup("ON");
    i->off_text = lt_strdup("OFF");
    i->on_text_color = LT_RGB(0, 0, 0);
    i->off_text_color = LT_RGB(51, 51, 51);
    i->on_background_color = LT_RGB(0, 255, 0);
    i->off_background_color = LT_RGBA(0, 0, 0, 0);
    i->relative_width = 0.1;
    i->relative_height = 0.05;
    i->font_family = lt_strdup("");
    i->antialiasing_on = LT_TRUE;
}

void lt_dash_indicator_free(lt_dash_indicator *i)
{
    lt_free(i->id);
    lt_free(i->output_channel);
    lt_free(i->on_text);
    lt_free(i->off_text);
    lt_free(i->on_image_file_name);
    lt_free(i->off_image_file_name);
    lt_free(i->font_family);
    lt_free(i->short_click_action);
    lt_free(i->long_click_action);
    lt_free(i->ecu_configuration_name);
    lt_free(i->enabled_condition);
    extras_free(&i->extras);
    memset(i, 0, sizeof(*i));
}

void dash_image_free(lt_dash_image *im)
{
    lt_free(im->file_name);
    lt_free(im->image_id);
    lt_free(im->data_base64);
    memset(im, 0, sizeof(*im));
}

void dash_comp_free(lt_dash_comp *c)
{
    if (c->gauge) {
        lt_dash_gauge_free(c->gauge);
        lt_free(c->gauge);
    }
    if (c->indicator) {
        lt_dash_indicator_free(c->indicator);
        lt_free(c->indicator);
    }
    c->gauge = NULL;
    c->indicator = NULL;
}

static void cluster_init(lt_dash_cluster *c)
{
    memset(c, 0, sizeof(*c));
    c->anti_aliasing = LT_TRUE;
    c->background_color = LT_RGB(0, 0, 0);
    c->background_image_style = LT_BG_TILE;
}

static void cluster_free(lt_dash_cluster *c)
{
    size_t i;
    for (i = 0; i < c->n_images; i++) dash_image_free(&c->images[i]);
    lt_free(c->images);
    for (i = 0; i < c->n_comps; i++) dash_comp_free(&c->comps[i]);
    lt_free(c->comps);
    lt_free(c->background_image_file_name);
    lt_free(c->layout);
    lt_free(c->enabled_condition);
    extras_free(&c->extras);
    memset(c, 0, sizeof(*c));
}

lt_dash_file *lt_dash_new(void)
{
    lt_dash_file *d = (lt_dash_file *)lt_calloc(1, sizeof(*d));
    char ts[32];
    cluster_init(&d->cluster);
    d->author = lt_strdup("LibreTune");
    d->company = lt_strdup("LibreTune Project");
    dash_now_str(ts, sizeof ts, LT_TRUE);
    d->write_date = lt_strdup(ts);
    d->file_format = lt_strdup("3.0");
    return d;
}

void lt_dash_free(lt_dash_file *d)
{
    if (d == NULL) return;
    lt_free(d->author);
    lt_free(d->company);
    lt_free(d->write_date);
    lt_free(d->file_format);
    lt_free(d->firmware_signature);
    cluster_free(&d->cluster);
    lt_free(d);
}

static lt_dash_comp *comp_push(lt_dash_file *d)
{
    lt_dash_cluster *c = &d->cluster;
    c->comps = (lt_dash_comp *)lt_realloc(c->comps, (c->n_comps + 1) * sizeof(lt_dash_comp));
    memset(&c->comps[c->n_comps], 0, sizeof(lt_dash_comp));
    return &c->comps[c->n_comps++];
}

lt_dash_gauge *lt_dash_add_gauge(lt_dash_file *d)
{
    lt_dash_comp *c = comp_push(d);
    c->gauge = (lt_dash_gauge *)lt_calloc(1, sizeof(lt_dash_gauge));
    lt_dash_gauge_init(c->gauge);
    return c->gauge;
}

lt_dash_indicator *lt_dash_add_indicator(lt_dash_file *d)
{
    lt_dash_comp *c = comp_push(d);
    c->indicator = (lt_dash_indicator *)lt_calloc(1, sizeof(lt_dash_indicator));
    lt_dash_indicator_init(c->indicator);
    return c->indicator;
}

void lt_dash_remove(lt_dash_file *d, size_t index)
{
    lt_dash_cluster *c = &d->cluster;
    if (index >= c->n_comps) return;
    dash_comp_free(&c->comps[index]);
    memmove(&c->comps[index], &c->comps[index + 1], (c->n_comps - index - 1) * sizeof(lt_dash_comp));
    c->n_comps--;
}

/* used by dash_io.c */
void dash_cluster_reset_attrs(lt_dash_cluster *c);
void dash_cluster_reset_attrs(lt_dash_cluster *c)
{
    /* keep images and components, reset every attribute-derived field */
    lt_dash_image *im = c->images;
    size_t ni = c->n_images;
    lt_dash_comp *cp = c->comps;
    size_t nc = c->n_comps;
    lt_free(c->background_image_file_name);
    lt_free(c->layout);
    lt_free(c->enabled_condition);
    extras_free(&c->extras);
    cluster_init(c);
    c->images = im;
    c->n_images = ni;
    c->comps = cp;
    c->n_comps = nc;
}
