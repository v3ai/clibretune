/* test_basemap_apply.c - lt_basemap_apply logic against a stub INI codec.
 *
 * basemap_apply.c is #included with its ini calls renamed to local stubs, so
 * this test does not depend on (or clash with) src/ini/ini.c. The stubs model
 * U08 constants (scale 1) in a flat byte page. */
#include <stdlib.h>
#include "lt.h"
#include "lt_test.h"

#define lt_ini_find_const         stub_find_const
#define lt_ini_const_get_array    stub_get_array
#define lt_ini_const_set_array    stub_set_array
#define lt_ini_set                stub_set
#define lt_shape_count            stub_shape_count
#define lt_basemap_apply          bm_apply_under_test
#include "basemap/basemap_apply.c"
#undef lt_basemap_apply
#undef lt_ini_set

size_t stub_shape_count(const lt_shape *s)
{
    return s->kind == LT_SH_SCALAR ? 1 : (s->kind == LT_SH_1D ? s->n : s->rows * s->cols);
}

static lt_u8 page[1024];
static int sets_to_fail = 0;

const lt_ini_const *stub_find_const(const lt_ini *ini, const char *name)
{
    size_t i;
    for (i = 0; i < ini->n_constants; i++) {
        if (strcmp(ini->constants[i]->name, name) == 0) {
            return ini->constants[i];
        }
    }
    return NULL;
}

int stub_get_array(const lt_ini *ini, const lt_ini_const *c, const lt_pagesrc *src,
                          double *out, size_t cap)
{
    size_t i, n = lt_shape_count(&c->shape);
    (void)ini; (void)src;
    if (n > cap) {
        return LT_ERANGE;
    }
    for (i = 0; i < n; i++) {
        out[i] = page[c->offset + i];
    }
    return (int)n;
}

int stub_set_array(const lt_ini *ini, const lt_ini_const *c, lt_pagesrc *src,
                          const double *v, size_t n)
{
    size_t i;
    (void)ini; (void)src;
    if (sets_to_fail || n != lt_shape_count(&c->shape)) {
        return LT_ERANGE;
    }
    for (i = 0; i < n; i++) {
        double d = floor(v[i] + 0.5);
        page[c->offset + i] = (lt_u8)(d < 0 ? 0 : (d > 255 ? 255 : d));
    }
    return (int)n;
}

int stub_set(const lt_ini *ini, lt_pagesrc *src, const char *name, double v)
{
    const lt_ini_const *c = stub_find_const(ini, name);
    (void)src;
    if (!c) {
        return LT_ENOENT;
    }
    page[c->offset] = (lt_u8)(v < 0 ? 0 : (v > 255 ? 255 : floor(v + 0.5)));
    return 0;
}

static lt_ini ini;
static lt_ini_const consts[16];
static lt_ini_const *cptr[16];
static lt_ini_table tabs[4];
static lt_ini_table *tptr[4];
static lt_pagesrc src;

static void add_const(const char *name, unsigned off, size_t n, size_t rows, size_t cols)
{
    lt_ini_const *c = &consts[ini.n_constants];
    c->name = (char *)name;
    c->offset = off;
    c->scale = 1.0;
    if (rows) {
        c->shape.kind = LT_SH_2D; c->shape.rows = rows; c->shape.cols = cols;
    } else if (n > 1) {
        c->shape.kind = LT_SH_1D; c->shape.n = n;
    } else {
        c->shape.kind = LT_SH_SCALAR;
    }
    cptr[ini.n_constants] = c;
    ini.n_constants++;
}

static void add_table(const char *name, lt_table_role role, const char *z, const char *x,
                      const char *y)
{
    lt_ini_table *t = &tabs[ini.n_tables];
    t->name = (char *)name; t->role = role;
    t->z_const = (char *)z; t->x_const = (char *)x; t->y_const = (char *)y;
    tptr[ini.n_tables] = t;
    ini.n_tables++;
}

static void setup(void)
{
    memset(&ini, 0, sizeof(ini));
    memset(consts, 0, sizeof(consts));
    memset(tabs, 0, sizeof(tabs));
    memset(page, 0, sizeof(page));
    memset(&src, 0, sizeof(src));
    ini.constants = cptr;
    ini.tables = tptr;
    sets_to_fail = 0;
}

static void fill_axes(unsigned off, size_t n, unsigned lo, unsigned step)
{
    size_t i;
    for (i = 0; i < n; i++) {
        page[off + i] = (lt_u8)(lo + step * i);
    }
}

LT_TEST(apply_resamples_onto_own_axes)
{
    lt_engine_spec s;
    lt_basemap m;
    char *log = NULL;
    int n;
    setup();
    lt_engine_spec_default(&s);
    lt_basemap_generate(&s, &m);
    add_const("veTable", 0, 0, 4, 4);
    add_const("rpmBins", 100, 4, 0, 0);
    add_const("loadBins", 110, 4, 0, 0);
    add_const("reqFuel", 200, 1, 0, 0);
    add_const("nCylinders", 201, 1, 0, 0);
    add_table("ve", LT_ROLE_VE, "veTable", "rpmBins", "loadBins");
    add_table("ve2", LT_ROLE_VE, "veTable", "rpmBins", "loadBins");
    /* axes exactly at generated bin positions 0 / 5 / 10 / 15 */
    page[100] = 0; page[101] = 0; page[102] = 0; page[103] = 0;
    {
        double rx[4], ly[4];
        int i;
        for (i = 0; i < 4; i++) {
            rx[i] = m.rpm_bins[i * 5]; ly[i] = m.load_bins[i * 5];
            /* page holds U08: use load bins (<=105) and rpm/100 scaled by hand */
            page[110 + i] = (lt_u8)ly[i];
            page[100 + i] = (lt_u8)(rx[i] / 100.0);
        }
    }
    /* rpm axis is in 100s on the page: rebuild expectation with a rescaled copy */
    {
        lt_basemap m2 = m;
        int i;
        for (i = 0; i < 16; i++) {
            m2.rpm_bins[i] = m.rpm_bins[i] / 100.0;
        }
        n = bm_apply_under_test(&m2, &ini, &src, LT_FALSE, &log);
    }
    LT_CHECK(n == 1 + 2);   /* one table (not the duplicate role) + reqFuel + nCylinders */
    LT_CHECK(log != NULL && strstr(log, "table ve: 16 cells") != NULL);
    LT_CHECK(log != NULL && strstr(log, "const reqFuel") != NULL);
    {
        int r, c;
        for (r = 0; r < 4; r++) {
            for (c = 0; c < 4; c++) {
                LT_CHECK(page[r * 4 + c] == (lt_u8)m.ve[r * 5][c * 5]);
            }
        }
    }
    LT_CHECK(page[200] == 26 || page[200] == 25);   /* reqFuel 25.5 rounds in the stub */
    LT_CHECK(page[201] == 4);
    if (log) {
        lt_free(log);
    }
    lt_basemap_free(&m);
}

LT_TEST(apply_write_axes_and_roles)
{
    lt_engine_spec s;
    lt_basemap m;
    char *log = NULL;
    double rb[16], lb[16];
    int n, i;
    setup();
    lt_engine_spec_default(&s);
    lt_basemap_generate(&s, &m);
    add_const("ignTable", 0, 0, 16, 16);
    add_const("ignRpm", 300, 16, 0, 0);
    add_const("ignLoad", 320, 16, 0, 0);
    add_const("afrTable", 400, 0, 16, 16);
    add_const("afrRpm", 700, 16, 0, 0);
    add_const("afrLoad", 720, 16, 0, 0);
    add_table("ign", LT_ROLE_IGNITION, "ignTable", "ignRpm", "ignLoad");
    add_table("afr", LT_ROLE_AFR_TARGET, "afrTable", "afrRpm", "afrLoad");
    add_table("other", LT_ROLE_OTHER, "ignTable", "ignRpm", "ignLoad");
    /* afr is in tenths on U08: stub stores rounded values, just check non-zero + axes */
    n = bm_apply_under_test(&m, &ini, &src, LT_TRUE, &log);
    LT_CHECK(n == 6);   /* 2 tables x (z + 2 axes) */
    lt_basemap_load_bins(m.load_bins[15], 16, lb);
    lt_basemap_rpm_bins((unsigned)(m.rpm_bins[0] + 0.5), (unsigned)(m.rpm_bins[15] + 0.5), 16, rb);
    for (i = 0; i < 16; i++) {
        LT_CHECK(page[320 + i] == (lt_u8)lb[i]);
        LT_CHECK(page[300 + i] == (lt_u8)(rb[i] > 255 ? 255 : rb[i]));   /* U08 saturates */
    }
    LT_CHECK(page[0] == (lt_u8)m.ign[0][0]);
    LT_CHECK(page[15 * 16 + 15] == (lt_u8)m.ign[15][15]);
    LT_CHECK(page[400 + 15 * 16 + 15] == (lt_u8)floor(m.afr[15][15] + 0.5));
    if (log) {
        lt_free(log);
    }
    lt_basemap_free(&m);
}

LT_TEST(apply_rejects_mismatch_and_bad_args)
{
    lt_engine_spec s;
    lt_basemap m;
    char *log = NULL;
    int n;
    setup();
    lt_engine_spec_default(&s);
    lt_basemap_generate(&s, &m);
    /* z is 2x8 but axes are 4 / 4: element counts differ -> skipped untouched */
    add_const("zt", 0, 0, 2, 8);
    add_const("xa", 100, 4, 0, 0);
    add_const("ya", 110, 4, 0, 0);
    add_table("ve", LT_ROLE_VE, "zt", "xa", "ya");
    fill_axes(100, 4, 10, 10); fill_axes(110, 4, 20, 20);
    n = bm_apply_under_test(&m, &ini, &src, LT_FALSE, &log);
    LT_CHECK(n == 0);
    LT_CHECK(page[0] == 0 && page[15] == 0);
    /* 4x4 count but 2D layout rows/cols swapped relative to axes (2x8 vs 8x2 w/ axes 8,2) */
    add_const("z2", 0, 0, 4, 2);       /* rows 4 cols 2 = 8 cells */
    add_const("xb", 120, 4, 0, 0);
    add_const("yb", 130, 2, 0, 0);     /* nx*ny = 8 but cols(2) != nx(4) */
    tabs[0].z_const = (char *)"z2"; tabs[0].x_const = (char *)"xb"; tabs[0].y_const = (char *)"yb";
    n = bm_apply_under_test(&m, &ini, &src, LT_FALSE, &log);
    LT_CHECK(n == 0 && page[0] == 0);
    /* missing constants, null args */
    tabs[0].z_const = (char *)"nope";
    LT_CHECK(bm_apply_under_test(&m, &ini, &src, LT_FALSE, NULL) == 0);
    LT_CHECK(bm_apply_under_test(NULL, &ini, &src, LT_FALSE, &log) == LT_EINVAL);
    LT_CHECK(log == NULL);
    LT_CHECK(bm_apply_under_test(&m, NULL, &src, LT_FALSE, NULL) == LT_EINVAL);
    /* axis read failure (cap too small is not possible; make the axis a scalar-less const) */
    lt_basemap_free(&m);
}

LT_TEST(apply_scalar_shape_and_failed_write)
{
    lt_engine_spec s;
    lt_basemap m;
    char *log = NULL;
    int n;
    setup();
    lt_engine_spec_default(&s);
    lt_basemap_generate(&s, &m);
    add_const("nCylinders", 50, 4, 0, 0);   /* array: must not be written as a scalar */
    add_const("injType", 60, 1, 0, 0);
    n = bm_apply_under_test(&m, &ini, &src, LT_FALSE, &log);
    LT_CHECK(n == 1 && page[50] == 0 && page[60] == 1);
    if (log) {
        lt_free(log);
    }
    /* table z write failing is not counted */
    setup();
    add_const("z", 0, 0, 2, 2);
    add_const("x", 100, 2, 0, 0);
    add_const("y", 110, 2, 0, 0);
    add_table("ve", LT_ROLE_VE, "z", "x", "y");
    fill_axes(100, 2, 1, 1); fill_axes(110, 2, 20, 20);
    sets_to_fail = 1;
    log = NULL;
    LT_CHECK(bm_apply_under_test(&m, &ini, &src, LT_FALSE, &log) == 0);
    LT_CHECK(log == NULL);
    lt_basemap_free(&m);
}

LT_TEST(surround_edges)
{
    double b[3];
    size_t i0, i1;
    double f;
    b[0] = 10; b[1] = 20; b[2] = 20;
    surround(5, b, 3, &i0, &i1, &f);
    LT_CHECK(i0 == 0 && i1 == 0 && f == 0.0);
    surround(25, b, 3, &i0, &i1, &f);
    LT_CHECK(i0 == 2 && i1 == 2);
    surround(15, b, 3, &i0, &i1, &f);
    LT_CHECK(i0 == 0 && i1 == 1);
    LT_CHECK_NEAR(f, 0.5, 1e-12);
    surround(20, b, 3, &i0, &i1, &f);   /* duplicate bin: no division by zero */
    LT_CHECK(f == f);
}

int main(void)
{
    LT_RUN(apply_resamples_onto_own_axes);
    LT_RUN(apply_write_axes_and_roles);
    LT_RUN(apply_rejects_mismatch_and_bad_args);
    LT_RUN(apply_scalar_shape_and_failed_write);
    LT_RUN(surround_edges);
    LT_TEST_MAIN_END
}
