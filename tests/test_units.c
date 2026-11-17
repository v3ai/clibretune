/* test_units.c - unit preference layer (G-90): detection, conversion, round trips, and the
 * table/curve view model editing in the preferred unit while storing the ECU unit. */
#include <string.h>
#include "lt.h"
#include "tableops/tableops.h"
#include "lt_test.h"

#define PAGE_LEN 256

typedef struct fx {
    lt_ini ini;
    lt_u8 page[PAGE_LEN];
    lt_pagesrc src;
} fx;

static const lt_u8 *fx_page(void *ud, int page, size_t *len)
{
    fx *f = (fx *)ud;
    if (page != 0) return NULL;
    *len = PAGE_LEN;
    return f->page;
}

static int fx_write(void *ud, int page, size_t off, const lt_u8 *data, size_t n)
{
    fx *f = (fx *)ud;
    if (page != 0 || off + n > PAGE_LEN) return LT_ERANGE;
    memcpy(f->page + off, data, n);
    return 0;
}

static void add_const(fx *f, const char *name, lt_u32 off, size_t n, double scale, double tr,
                      double mn, double mx, const char *units)
{
    lt_ini_const *c = (lt_ini_const *)lt_calloc(1, sizeof *c);
    c->name = lt_strdup(name);
    c->offset = off;
    c->type = LT_DT_U08;
    c->endian_override = -1;
    c->shape.kind = LT_SH_1D;
    c->shape.n = n;
    c->scale = scale;
    c->translate = tr;
    c->min = mn;
    c->max = mx;
    c->range_resolved = LT_TRUE;
    c->units = lt_strdup(units);
    c->size_bytes = n;
    lt_map_put(&f->ini.constant_idx, name, c);
}

static lt_ini_curve *g_curve;

static void fx_init(fx *f)
{
    size_t i;
    memset(f, 0, sizeof *f);
    lt_map_init(&f->ini.constant_idx, LT_FALSE);
    f->ini.n_pages = 1;
    f->src.ud = f;
    f->src.page = fx_page;
    f->src.write = fx_write;
    /* CLT axis: stored C with a -40 offset: raw 0..255 -> -40..215 C. Pressure: kPa, scale 2. */
    add_const(f, "cltBins", 0, 5, 1.0, -40.0, -40, 215, "deg C");
    add_const(f, "mapVals", 16, 5, 2.0, 0.0, 0, 510, "kPa");
    for (i = 0; i < 5; i++) {
        f->page[i] = (lt_u8)(40 + 20 * i);        /* 0, 20, 40, 60, 80 C */
        f->page[16 + i] = (lt_u8)(50 + 10 * i);   /* 100 .. 140 kPa */
    }
    g_curve = (lt_ini_curve *)lt_calloc(1, sizeof *g_curve);
    g_curve->name = lt_strdup("c1");
    g_curve->x_const = lt_strdup("cltBins");
    g_curve->y_const = lt_strdup("mapVals");
    g_curve->size = 5;
}

static void fx_free(fx *f)
{
    size_t i;
    for (i = 0; i < f->ini.constant_idx.cap; i++)
        if (f->ini.constant_idx.slots[i].used == 1) {
            lt_ini_const *c = (lt_ini_const *)f->ini.constant_idx.slots[i].val;
            lt_free(c->name); lt_free(c->units); lt_free(c);
        }
    lt_map_free(&f->ini.constant_idx);
    lt_free(g_curve->name); lt_free(g_curve->x_const); lt_free(g_curve->y_const);
    lt_free(g_curve);
}

LT_TEST(detect_strings)
{
    int s = -1;
    LT_CHECK(lt_unit_detect("C", &s) == LT_UK_TEMP && s == 0);
    LT_CHECK(lt_unit_detect("deg C", &s) == LT_UK_TEMP && s == 0);
    LT_CHECK(lt_unit_detect("\xC2\xB0" "C", &s) == LT_UK_TEMP && s == 0);
    LT_CHECK(lt_unit_detect("degF", &s) == LT_UK_TEMP && s == 1);
    LT_CHECK(lt_unit_detect("Fahrenheit", &s) == LT_UK_TEMP && s == 1);
    LT_CHECK(lt_unit_detect("K", &s) == LT_UK_TEMP && s == 2);
    LT_CHECK(lt_unit_detect("kPa", &s) == LT_UK_PRESS && s == 0);
    LT_CHECK(lt_unit_detect("PSI", &s) == LT_UK_PRESS && s == 1);
    LT_CHECK(lt_unit_detect("bar", &s) == LT_UK_PRESS && s == 2);
    LT_CHECK(lt_unit_detect("inHg", &s) == LT_UK_PRESS && s == 3);
    LT_CHECK(lt_unit_detect("AFR", &s) == LT_UK_AFR && s == 0);
    LT_CHECK(lt_unit_detect("lambda", &s) == LT_UK_AFR && s == 1);
    LT_CHECK(lt_unit_detect("km/h", &s) == LT_UK_SPEED && s == 0);
    LT_CHECK(lt_unit_detect("mph", &s) == LT_UK_SPEED && s == 1);
    LT_CHECK(lt_unit_detect("%", &s) == LT_UK_NONE);
    LT_CHECK(lt_unit_detect("deg", &s) == LT_UK_NONE);      /* an angle */
    LT_CHECK(lt_unit_detect("rpm", &s) == LT_UK_NONE);
    LT_CHECK(lt_unit_detect("{expr}", &s) == LT_UK_NONE);
    LT_CHECK(lt_unit_detect(NULL, &s) == LT_UK_NONE);
    LT_CHECK(lt_unit_detect("", &s) == LT_UK_NONE);
}

LT_TEST(convert_values)
{
    LT_CHECK_NEAR(lt_unit_convert(LT_UK_TEMP, 0, 1, 100.0), 212.0, 1e-9);
    LT_CHECK_NEAR(lt_unit_convert(LT_UK_TEMP, 1, 0, 32.0), 0.0, 1e-9);
    LT_CHECK_NEAR(lt_unit_convert(LT_UK_TEMP, 0, 2, 0.0), 273.15, 1e-9);
    LT_CHECK_NEAR(lt_unit_convert(LT_UK_TEMP, 2, 1, 273.15), 32.0, 1e-9);
    LT_CHECK_NEAR(lt_unit_convert(LT_UK_PRESS, 0, 1, 100.0), 14.5037738, 1e-6);
    LT_CHECK_NEAR(lt_unit_convert(LT_UK_PRESS, 0, 2, 100.0), 1.0, 1e-9);
    LT_CHECK_NEAR(lt_unit_convert(LT_UK_PRESS, 2, 0, 1.0), 100.0, 1e-9);
    LT_CHECK_NEAR(lt_unit_convert(LT_UK_PRESS, 0, 3, 101.325), 29.9213, 1e-3);
    LT_CHECK_NEAR(lt_unit_convert(LT_UK_AFR, 0, 1, 14.7), 1.0, 1e-9);
    LT_CHECK_NEAR(lt_unit_convert(LT_UK_AFR, 1, 0, 0.85), 12.495, 1e-9);
    LT_CHECK_NEAR(lt_unit_convert(LT_UK_SPEED, 0, 1, 100.0), 62.1371, 1e-3);
    LT_CHECK_NEAR(lt_unit_convert(LT_UK_NONE, 0, 1, 5.0), 5.0, 1e-12);
}

LT_TEST(round_trip_no_drift)
{
    int kind, from, to, k;
    double v;
    for (kind = LT_UK_TEMP; kind <= LT_UK_SPEED; kind++) {
        for (from = 0; from < 2; from++) {
            for (to = 0; to < 4; to++) {
                if ((kind == LT_UK_AFR || kind == LT_UK_SPEED) && to > 1) continue;
                if (kind == LT_UK_TEMP && to > 2) continue;
                for (k = -40; k <= 300; k += 7) {
                    v = (double)k * 1.37;
                    LT_CHECK_NEAR(lt_unit_convert(kind, to, from,
                                  lt_unit_convert(kind, from, to, v)), v, 1e-9);
                }
            }
        }
    }
}

LT_TEST(preferences)
{
    lt_uconv c;
    unsigned g0;
    int t, p, a, m;
    lt_units_set(0, 0, 0, 1);
    g0 = lt_units_generation();
    lt_units_set(0, 0, 0, 1);
    LT_CHECK(lt_units_generation() == g0);      /* unchanged: no bump */
    lt_units_set(1, 1, 1, 0);
    LT_CHECK(lt_units_generation() != g0);
    lt_units_get(&t, &p, &a, &m);
    LT_CHECK(t == 1 && p == 1 && a == 1 && m == 0);
    lt_uconv_make(&c, "deg C");
    LT_CHECK(lt_uconv_active(&c));
    LT_CHECK_STR(lt_uconv_label(&c, "deg C"), "F");
    LT_CHECK_NEAR(lt_uconv_to_disp(&c, 90.0), 194.0, 1e-9);
    LT_CHECK_NEAR(lt_uconv_from_disp(&c, 194.0), 90.0, 1e-9);
    lt_uconv_make(&c, "rpm");
    LT_CHECK(!lt_uconv_active(&c));
    LT_CHECK_STR(lt_uconv_label(&c, "rpm"), "rpm");
    LT_CHECK(lt_uconv_digits(&c, 1) == 1);
    lt_uconv_make(&c, "kPa");
    LT_CHECK(lt_uconv_digits(&c, 0) == 1);           /* psi needs a decimal */
    lt_units_set(0, 2, 0, 1);
    LT_CHECK(lt_uconv_stale(&c));
    LT_CHECK_NEAR(lt_uconv_retarget(&c, 14.5037738), 1.0, 1e-6);
    LT_CHECK(!lt_uconv_stale(&c));
    LT_CHECK(lt_uconv_digits(&c, 0) == 2);           /* bar: two decimals */
    lt_units_set(0, 0, 0, 1);
}

LT_TEST(curve_edit_in_fahrenheit)
{
    fx f;
    lt_tblview v;
    lt_u8 before[PAGE_LEN];
    fx_init(&f);
    lt_units_set(1, 1, 0, 0);               /* F, psi */
    lt_tblview_init(&v);
    LT_CHECK(lt_tblview_load_curve(&v, &f.ini, g_curve, &f.src) == 0);
    LT_CHECK_NEAR(v.x[0], 32.0, 1e-9);                    /* 0 C -> 32 F */
    LT_CHECK_NEAR(v.x[4], 176.0, 1e-9);                   /* 80 C -> 176 F */
    LT_CHECK_NEAR(v.z[0], 100.0 * 0.14503773773, 1e-6);   /* 100 kPa -> psi */
    /* untouched commit writes nothing and leaves the page identical */
    memcpy(before, f.page, PAGE_LEN);
    LT_CHECK(lt_tblview_commit(&v, &f.ini, &f.src) == 0);
    LT_CHECK(memcmp(before, f.page, PAGE_LEN) == 0);
    /* type 140 F into the third axis point: stored as 60 C -> raw 100 */
    lt_tblview_set_x(&v, 2, 140.0);
    LT_CHECK(lt_tblview_commit(&v, &f.ini, &f.src) == 1);
    LT_CHECK(f.page[2] == 100);
    LT_CHECK_NEAR(v.x[2], 140.0, 1e-9);                   /* 60 C is exactly 140 F */
    /* type 20 psi: 137.895 kPa -> raw 69 (scale 2, rounded) -> 138 kPa */
    lt_tblview_set(&v, 0, 1, 20.0);
    LT_CHECK(lt_tblview_commit(&v, &f.ini, &f.src) == 1);
    LT_CHECK(f.page[16 + 1] == 69);
    LT_CHECK_NEAR(v.z[1], 138.0 * 0.14503773773, 1e-6);
    lt_tblview_free(&v);
    lt_units_set(0, 0, 0, 1);
    fx_free(&f);
}

LT_TEST(live_preference_change)
{
    fx f;
    lt_tblview v;
    fx_init(&f);
    lt_units_set(0, 0, 0, 1);
    lt_tblview_init(&v);
    LT_CHECK(lt_tblview_load_curve(&v, &f.ini, g_curve, &f.src) == 0);
    LT_CHECK_NEAR(v.x[1], 20.0, 1e-9);
    lt_tblview_push_undo(&v);
    lt_tblview_set_x(&v, 1, 25.0);             /* uncommitted edit in C */
    LT_CHECK(!lt_tblview_units_sync(&v));
    lt_units_set(1, 0, 0, 1);                   /* switch to F while editing */
    LT_CHECK(lt_tblview_units_sync(&v));
    LT_CHECK_NEAR(v.x[1], 77.0, 1e-9);          /* the edit survived, shown in F */
    LT_CHECK_NEAR(v.x[0], 32.0, 1e-9);
    LT_CHECK(lt_tblview_is_dirty(&v));
    LT_CHECK(!lt_tblview_cell_dirty(&v, 0, 1)); /* y untouched */
    LT_CHECK(!lt_tblview_units_sync(&v));       /* idempotent */
    LT_CHECK(lt_tblview_commit(&v, &f.ini, &f.src) == 1);
    LT_CHECK(f.page[1] == 65);                  /* 25 C -> raw 65 */
    LT_CHECK(lt_tblview_undo(&v));              /* undo snapshot was converted too */
    LT_CHECK_NEAR(v.x[1], 68.0, 1e-9);          /* 20 C in F */
    lt_tblview_free(&v);
    lt_units_set(0, 0, 0, 1);
    fx_free(&f);
}

LT_TEST(z_range_converted)
{
    fx f;
    lt_tblview v;
    double lo, hi;
    fx_init(&f);
    lt_units_set(0, 2, 0, 1);                   /* bar */
    lt_tblview_init(&v);
    LT_CHECK(lt_tblview_load_curve(&v, &f.ini, g_curve, &f.src) == 0);
    lt_tblview_z_range(&v, &f.ini, &lo, &hi);
    LT_CHECK_NEAR(lo, 0.0, 1e-9);
    LT_CHECK_NEAR(hi, 5.10, 1e-9);              /* 510 kPa */
    lt_tblview_free(&v);
    lt_units_set(0, 0, 0, 1);
    fx_free(&f);
}

int main(void)
{
    LT_RUN(detect_strings);
    LT_RUN(convert_values);
    LT_RUN(round_trip_no_drift);
    LT_RUN(preferences);
    LT_RUN(curve_edit_in_fahrenheit);
    LT_RUN(live_preference_change);
    LT_RUN(z_range_converted);
    LT_TEST_MAIN_END
}
