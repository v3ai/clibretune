/* test_dash_fuzz.c - random truncation/mutation of .dash/.gauge input must not crash. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lt.h"
#include "dash/dash.h"
#include "gui/gui.h"
#include "lt_test.h"

static unsigned long rng_s = 12345UL;
static unsigned rnd(void)
{
    rng_s = (rng_s * 1103515245UL + 12345UL) & 0x7fffffffUL;
    return (unsigned)(rng_s >> 8);
}

static char *slurp(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    char *b;
    long sz;
    if (f == NULL) return NULL;
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    b = (char *)malloc((size_t)sz + 1);
    *n = fread(b, 1, (size_t)sz, f);
    b[*n] = 0;
    fclose(f);
    return b;
}

static void try_one(const char *s, size_t n)
{
    char err[120];
    lt_dash_file *d = lt_dash_parse(s, n, err, sizeof err);
    lt_gauge_file g;
    lt_buf b;
    if (d != NULL) {
        lt_dash_report r;
        lt_buf_init(&b);
        lt_dash_write(d, &b);
        lt_buf_free(&b);
        lt_dash_validate(d, NULL, 0, &r);
        lt_dash_report_free(&r);
        lt_dash_free(d);
    }
    memset(&g, 0, sizeof g);
    if (lt_gaugefile_parse(s, n, &g, err, sizeof err) == 0) {
        lt_buf_init(&b);
        lt_gaugefile_write(&g, &b);
        lt_buf_free(&b);
        lt_gaugefile_free(&g);
    }
}

LT_TEST(fuzz_dash)
{
    size_t n, i, k;
    char *src = slurp("tests/fixtures/dash/basic.dash", &n);
    char *m;
    lt_buf tb;
    lt_dash_file *t;
    LT_CHECK(src != NULL);
    if (src == NULL) return;
    /* also fuzz a written template (exercises more attributes) */
    t = lt_dash_template_telemetry();
    lt_buf_init(&tb);
    lt_dash_write(t, &tb);
    lt_dash_free(t);
    for (k = 0; k < 2; k++) {
        const char *base = k == 0 ? src : tb.data;
        size_t bn = k == 0 ? n : tb.len;
        m = (char *)malloc(bn + 1);
        for (i = 0; i < bn; i += 1 + bn / 600) { try_one(base, i); }
        for (i = 0; i < 1500; i++) {
            size_t j, nm = 1 + rnd() % 6;
            memcpy(m, base, bn);
            for (j = 0; j < nm; j++) {
                size_t p = rnd() % bn;
                switch (rnd() % 4) {
                case 0: m[p] = (char)rnd(); break;
                case 1: m[p] = "<>&\"'/=;"[rnd() % 8]; break;
                case 2: m[p] = (char)('0' + rnd() % 10); break;
                default: m[p] = ' '; break;
                }
            }
            try_one(m, bn - (rnd() % 3 == 0 ? rnd() % bn : 0));
        }
        free(m);
    }
    lt_buf_free(&tb);
    free(src);
    LT_CHECK(1);
}

/* extreme numeric property values must not trap in parse/validate/write/update/render */
LT_TEST(extreme_values)
{
    static const char *vals[] = { "99999999999999999999", "-99999999999999999999", "nan", "inf",
        "-inf", "1e999", "-1e999", "1e308", "2147483648", "-2147483649", "" };
    static const char *props[] = { "ValueDigits", "LabelDigits", "SweepAngle", "FaceAngle",
        "BorderWidth", "ShortestSize", "HistoryDelay", "Min", "Max", "Value", "RelativeX",
        "RelativeY", "RelativeWidth", "RelativeHeight", "HighCritical", "Hysteresis", "HistoryValue" };
    size_t i, j;
    lt_canvas cv;
    lt_gfx g;
    lt_canvas_init(&cv, 64, 64);
    lt_gfx_init_canvas(&g, &cv);
    for (i = 0; i < sizeof props / sizeof *props; i++)
        for (j = 0; j < sizeof vals / sizeof *vals; j++) {
            char xml[700], err[100];
            lt_dash_file *d;
            lt_dash_view *v;
            lt_dash_report r;
            lt_rect area;
            lt_buf b;
            lt_snprintf(xml, sizeof xml, "<dsh><gaugeCluster><dashComp type=\"Gauge\"><OutputChannel>rpm</OutputChannel>"
                "<ShowHistory>true</ShowHistory><%s>%s</%s></dashComp></gaugeCluster></dsh>",
                props[i], vals[j], props[i]);
            d = lt_dash_parse(xml, strlen(xml), err, sizeof err);
            LT_CHECK(d != NULL);
            if (d == NULL) continue;
            lt_buf_init(&b);
            lt_dash_write(d, &b);
            lt_buf_free(&b);
            lt_dash_validate(d, NULL, 0, &r);
            lt_dash_report_free(&r);
            v = lt_dash_view_new(d);
            lt_dash_view_update(v, 0.0, NULL);
            lt_dash_view_update(v, 5000.0, NULL);
            area.x = 0; area.y = 0; area.w = 64; area.h = 64;
            lt_dash_view_render(v, &g, area, NULL);
            (void)lt_dash_view_hit(v, 10, 10);
            lt_dash_view_free(v);
            lt_dash_free(d);
        }
    lt_canvas_free(&cv);
}

int main(void)
{
    LT_RUN(extreme_values);
    LT_RUN(fuzz_dash);
    LT_TEST_MAIN_END
}
