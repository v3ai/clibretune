/* test_tableops_view.c - lt_tblview and dynamic sizes against a hand-built lt_ini
 * (no INI parser involved: constants/tables are filled in directly). */
#include <string.h>
#include "lt.h"
#include "tableops/tableops.h"
#include "lt_test.h"

#define PAGE_LEN 512

typedef struct fx {
    lt_ini ini;
    lt_u8 page[PAGE_LEN];
    lt_bool loaded;
    int writes;
    lt_pagesrc src;
} fx;

static const lt_u8 *fx_page(void *ud, int page, size_t *len)
{
    fx *f = (fx *)ud;
    if (page != 0 || !f->loaded) return NULL;
    *len = PAGE_LEN;
    return f->page;
}

static int fx_write(void *ud, int page, size_t off, const lt_u8 *data, size_t n)
{
    fx *f = (fx *)ud;
    if (page != 0 || !f->loaded) return LT_ENOENT;
    if (off + n > PAGE_LEN) return LT_ERANGE;
    memcpy(f->page + off, data, n);
    f->writes += (int)n;
    return 0;
}

static lt_ini_const *add_const(fx *f, const char *name, lt_u32 off, size_t rows, size_t cols,
                               double scale, double mn, double mx)
{
    lt_ini_const *c = (lt_ini_const *)lt_calloc(1, sizeof *c);
    c->name = lt_strdup(name);
    c->page = 0;
    c->offset = off;
    c->type = LT_DT_U08;
    c->endian_override = -1;
    if (rows > 1) {
        c->shape.kind = LT_SH_2D;
        c->shape.rows = rows;
        c->shape.cols = cols;
    } else if (cols > 1) {
        c->shape.kind = LT_SH_1D;
        c->shape.n = cols;
    } else {
        c->shape.kind = LT_SH_SCALAR;
    }
    c->scale = scale;
    c->min = mn;
    c->max = mx;
    c->range_resolved = LT_TRUE;
    c->size_bytes = rows > 1 ? rows * cols : cols;
    lt_map_put(&f->ini.constant_idx, name, c);
    return c;
}

static lt_ini_table *add_table(fx *f, const char *name, const char *x, const char *y,
                               const char *z, size_t cols, size_t rows)
{
    lt_ini_table *t = (lt_ini_table *)lt_calloc(1, sizeof *t);
    t->name = lt_strdup(name);
    t->map_name = lt_strdup(name);
    t->x_const = lt_strdup(x);
    t->y_const = lt_strdup(y);
    t->z_const = lt_strdup(z);
    t->is_3d = LT_TRUE;
    t->x_size = cols;
    t->y_size = rows;
    f->ini.tables = (lt_ini_table **)lt_realloc(f->ini.tables,
                                                (f->ini.n_tables + 1) * sizeof(lt_ini_table *));
    f->ini.tables[f->ini.n_tables++] = t;
    return t;
}

/* layout: rpmBins@0 (4), loadBins@4 (3), veTable@8 (3x4, scale 0.5), curve x@32 (5), y@40 (5),
 * dynamic: dynCols@100 dynRows@101, dynX@110 (8), dynY@120 (8), dynZ@130 (8x8). */
static void fx_init(fx *f)
{
    size_t i;
    memset(f, 0, sizeof *f);
    lt_map_init(&f->ini.constant_idx, LT_FALSE);
    lt_map_init(&f->ini.default_values, LT_FALSE);
    lt_map_init(&f->ini.maximum_elements, LT_FALSE);
    lt_map_init(&f->ini.table_idx, LT_FALSE);
    f->ini.n_pages = 1;
    f->loaded = LT_TRUE;
    f->src.ud = f;
    f->src.page = fx_page;
    f->src.write = fx_write;
    add_const(f, "rpmBins", 0, 1, 4, 100.0, 0, 25500);
    add_const(f, "loadBins", 4, 1, 3, 10.0, 0, 2550);
    add_const(f, "veTable", 8, 3, 4, 0.5, 0, 127.5);
    add_const(f, "cx", 32, 1, 5, 1.0, 0, 255);
    add_const(f, "cy", 40, 1, 5, 2.0, 0, 510);
    add_const(f, "dynCols", 100, 1, 1, 1.0, 2, 8);
    add_const(f, "dynRows", 101, 1, 1, 1.0, 2, 8);
    add_const(f, "dynX", 110, 1, 8, 1.0, 0, 255);
    add_const(f, "dynY", 120, 1, 8, 1.0, 0, 255);
    add_const(f, "dynZ", 130, 8, 8, 1.0, 0, 255);
    for (i = 0; i < 4; i++) f->page[i] = (lt_u8)(10 + 10 * i);      /* 1000..4000 */
    for (i = 0; i < 3; i++) f->page[4 + i] = (lt_u8)(4 + 2 * i);    /* 40,60,80 */
    for (i = 0; i < 12; i++) f->page[8 + i] = (lt_u8)(100 + i);     /* 50.0 .. 55.5 */
    for (i = 0; i < 5; i++) { f->page[32 + i] = (lt_u8)(i * 10); f->page[40 + i] = (lt_u8)(i * 5); }
    f->page[100] = 3;
    f->page[101] = 2;
    for (i = 0; i < 8; i++) { f->page[110 + i] = (lt_u8)(i + 1); f->page[120 + i] = (lt_u8)(i + 11); }
    for (i = 0; i < 64; i++) f->page[130 + i] = (lt_u8)(200 + i);
    add_table(f, "veTbl", "rpmBins", "loadBins", "veTable", 4, 3);
}

static void fx_free(fx *f)
{
    size_t i;
    for (i = 0; i < f->ini.constant_idx.cap; i++)
        if (f->ini.constant_idx.slots[i].used == 1) {
            lt_ini_const *c = (lt_ini_const *)f->ini.constant_idx.slots[i].val;
            lt_free(c->name);
            lt_free(c);
        }
    for (i = 0; i < f->ini.n_tables; i++) {
        lt_ini_table *t = f->ini.tables[i];
        lt_free(t->name); lt_free(t->map_name); lt_free(t->x_const); lt_free(t->y_const);
        lt_free(t->z_const); lt_free(t->rows_size_const); lt_free(t->cols_size_const);
        lt_free(t);
    }
    lt_free(f->ini.tables);
    lt_map_free(&f->ini.constant_idx);
    lt_map_free(&f->ini.default_values);
    lt_map_free(&f->ini.maximum_elements);
    lt_map_free(&f->ini.table_idx);
}

static lt_ini_table *add_dyn(fx *f)
{
    lt_ini_table *t = add_table(f, "dynTbl", "dynX", "dynY", "dynZ", 8, 8);
    t->cols_size_const = lt_strdup("dynCols");
    t->rows_size_const = lt_strdup("dynRows");
    t->max_elements = 40;
    return t;
}

LT_TEST(load_table)
{
    fx f;
    lt_tblview v;
    fx_init(&f);
    lt_tblview_init(&v);
    LT_CHECK(lt_tblview_load_table(&v, &f.ini, f.ini.tables[0], &f.src) == 0);
    LT_CHECK(v.rows == 3 && v.cols == 4);
    LT_CHECK_NEAR(v.x[0], 1000.0, 1e-9);
    LT_CHECK_NEAR(v.x[3], 4000.0, 1e-9);
    LT_CHECK_NEAR(v.y[2], 80.0, 1e-9);
    LT_CHECK_NEAR(lt_tblview_get(&v, 0, 0), 50.0, 1e-9);
    LT_CHECK_NEAR(lt_tblview_get(&v, 2, 3), 55.5, 1e-9);
    LT_CHECK_NEAR(lt_tblview_get(&v, 1, 0), 52.0, 1e-9);
    LT_CHECK(!lt_tblview_is_dirty(&v));
    LT_CHECK(lt_tblview_get(&v, 9, 9) == 0.0);
    lt_tblview_free(&v);
    fx_free(&f);
}

LT_TEST(load_not_loaded_page)
{
    fx f;
    lt_tblview v;
    fx_init(&f);
    f.loaded = LT_FALSE;
    lt_tblview_init(&v);
    LT_CHECK(lt_tblview_load_table(&v, &f.ini, f.ini.tables[0], &f.src) < 0);
    LT_CHECK(v.z == NULL);
    lt_tblview_free(&v);
    fx_free(&f);
}

LT_TEST(edit_dirty_commit)
{
    fx f;
    lt_tblview v;
    lt_tcell sel[2];
    lt_u8 before[PAGE_LEN];
    int n;
    fx_init(&f);
    memcpy(before, f.page, PAGE_LEN);
    lt_tblview_init(&v);
    lt_tblview_load_table(&v, &f.ini, f.ini.tables[0], &f.src);
    lt_tblview_set(&v, 1, 2, 70.0);
    LT_CHECK(lt_tblview_is_dirty(&v));
    LT_CHECK(lt_tblview_cell_dirty(&v, 1, 2));
    LT_CHECK(!lt_tblview_cell_dirty(&v, 0, 0));
    lt_tblview_set(&v, 1, 2, lt_tblview_get(&v, 1, 2));
    sel[0].y = 0; sel[0].x = 0;
    sel[1].y = 2; sel[1].x = 1;
    lt_tblview_offset(&v, sel, 2, 1.0);
    LT_CHECK_NEAR(lt_tblview_get(&v, 0, 0), 51.0, 1e-9);
    f.writes = 0;
    n = lt_tblview_commit(&v, &f.ini, &f.src);
    LT_CHECK(n == 3);
    LT_CHECK(f.writes == 3);
    LT_CHECK(!lt_tblview_is_dirty(&v));
    LT_CHECK(f.page[8 + 0] == 102);          /* 51.0 / 0.5 */
    LT_CHECK(f.page[8 + 6] == 140);          /* 70 / 0.5 */
    LT_CHECK(f.page[8 + 9] == 111);  /* original 109 + 1.0/0.5 */
    LT_CHECK(f.page[8 + 1] == before[8 + 1]);
    /* second commit has nothing to do */
    LT_CHECK(lt_tblview_commit(&v, &f.ini, &f.src) == 0);
    lt_tblview_free(&v);
    fx_free(&f);
}

LT_TEST(commit_quantises_and_axes)
{
    fx f;
    lt_tblview v;
    fx_init(&f);
    lt_tblview_init(&v);
    lt_tblview_load_table(&v, &f.ini, f.ini.tables[0], &f.src);
    lt_tblview_set(&v, 0, 0, 50.3);          /* quantum 0.5 -> 50.5 (round half up from 100.6) */
    lt_tblview_set_x(&v, 0, 1500.0);
    lt_tblview_set_y(&v, 1, 65.0);
    LT_CHECK(v.x_dirty && v.y_dirty && v.z_dirty);
    LT_CHECK(lt_tblview_commit(&v, &f.ini, &f.src) == 3);
    LT_CHECK_NEAR(lt_tblview_get(&v, 0, 0), 50.5, 1e-9);
    LT_CHECK(f.page[0] == 15);
    LT_CHECK(f.page[5] == 6 || f.page[5] == 7);   /* 6.5 rounds to 7 (half away from zero) */
    LT_CHECK_NEAR(v.x[0], 1500.0, 1e-9);
    LT_CHECK(!lt_tblview_is_dirty(&v));
    lt_tblview_free(&v);
    fx_free(&f);
}

LT_TEST(revert)
{
    fx f;
    lt_tblview v;
    fx_init(&f);
    lt_tblview_init(&v);
    lt_tblview_load_table(&v, &f.ini, f.ini.tables[0], &f.src);
    lt_tblview_set(&v, 0, 0, 99.0);
    LT_CHECK(lt_tblview_revert(&v, &f.ini, &f.src) == 0);
    LT_CHECK_NEAR(lt_tblview_get(&v, 0, 0), 50.0, 1e-9);
    LT_CHECK(!lt_tblview_is_dirty(&v));
    lt_tblview_free(&v);
    fx_free(&f);
}

LT_TEST(undo_redo)
{
    fx f;
    lt_tblview v;
    lt_tcell sel[1];
    size_t i;
    fx_init(&f);
    lt_tblview_init(&v);
    lt_tblview_load_table(&v, &f.ini, f.ini.tables[0], &f.src);
    sel[0].y = 0; sel[0].x = 0;
    LT_CHECK(!lt_tblview_undo(&v));
    LT_CHECK(!lt_tblview_redo(&v));
    lt_tblview_set_equal(&v, sel, 1, 60.0);
    lt_tblview_scale(&v, sel, 1, 2.0);
    LT_CHECK_NEAR(lt_tblview_get(&v, 0, 0), 120.0, 1e-9);
    LT_CHECK(lt_tblview_undo(&v));
    LT_CHECK_NEAR(lt_tblview_get(&v, 0, 0), 60.0, 1e-9);
    LT_CHECK(lt_tblview_undo(&v));
    LT_CHECK_NEAR(lt_tblview_get(&v, 0, 0), 50.0, 1e-9);
    LT_CHECK(!lt_tblview_is_dirty(&v));
    LT_CHECK(!lt_tblview_undo(&v));
    LT_CHECK(lt_tblview_redo(&v));
    LT_CHECK_NEAR(lt_tblview_get(&v, 0, 0), 60.0, 1e-9);
    LT_CHECK(lt_tblview_redo(&v));
    LT_CHECK_NEAR(lt_tblview_get(&v, 0, 0), 120.0, 1e-9);
    LT_CHECK(!lt_tblview_redo(&v));
    /* new edit after undo discards redo */
    lt_tblview_undo(&v);
    lt_tblview_offset(&v, sel, 1, 1.0);
    LT_CHECK_NEAR(lt_tblview_get(&v, 0, 0), 61.0, 1e-9);
    LT_CHECK(!lt_tblview_redo(&v));
    /* ring overflow: many ops never crash, oldest dropped */
    for (i = 0; i < 100; i++) lt_tblview_offset(&v, sel, 1, 1.0);
    for (i = 0; i < 100; i++)
        if (!lt_tblview_undo(&v)) break;
    LT_CHECK(i >= LT_TBLVIEW_UNDO_MAX - 2 && i <= LT_TBLVIEW_UNDO_MAX);
    lt_tblview_free(&v);
    fx_free(&f);
}

LT_TEST(undo_axis_edit)
{
    fx f;
    lt_tblview v;
    fx_init(&f);
    lt_tblview_init(&v);
    lt_tblview_load_table(&v, &f.ini, f.ini.tables[0], &f.src);
    lt_tblview_push_undo(&v);
    lt_tblview_set_x(&v, 2, 9999.0);
    LT_CHECK(v.x_dirty);
    LT_CHECK(lt_tblview_undo(&v));
    LT_CHECK_NEAR(v.x[2], 3000.0, 1e-9);
    LT_CHECK(!v.x_dirty);
    lt_tblview_free(&v);
    fx_free(&f);
}

LT_TEST(view_ops_and_range)
{
    fx f;
    lt_tblview v;
    lt_tcell sel[12];
    double lo, hi;
    size_t i;
    fx_init(&f);
    lt_tblview_init(&v);
    lt_tblview_load_table(&v, &f.ini, f.ini.tables[0], &f.src);
    for (i = 0; i < 12; i++) { sel[i].y = i / 4; sel[i].x = i % 4; }
    lt_tblview_interp2d(&v, sel, 12);
    LT_CHECK_NEAR(lt_tblview_get(&v, 0, 0), 50.0, 1e-9);
    LT_CHECK_NEAR(lt_tblview_get(&v, 2, 3), 55.5, 1e-9);
    lt_tblview_fill(&v, sel, 12, LT_TBL_FILL_DOWN);
    LT_CHECK_NEAR(lt_tblview_get(&v, 2, 1), lt_tblview_get(&v, 0, 1), 1e-12);
    lt_tblview_interp_linear(&v, sel, 12, LT_TBL_AXIS_ROW);
    lt_tblview_smooth(&v, sel, 12, 1.0);
    lt_tblview_z_range(&v, &f.ini, &lo, &hi);
    LT_CHECK_NEAR(lo, 0.0, 1e-12);
    LT_CHECK_NEAR(hi, 127.5, 1e-12);
    lt_tblview_z_range(&v, NULL, &lo, &hi);   /* falls back to data range */
    LT_CHECK(lo <= hi && hi < 60.0);
    lt_tblview_free(&v);
    fx_free(&f);
}

LT_TEST(import)
{
    fx f;
    lt_tblview v;
    double x[4] = { 1, 2, 3, 4 }, y[3] = { 5, 6, 7 }, z[12], bad[6] = { 0 };
    size_t i;
    fx_init(&f);
    for (i = 0; i < 12; i++) z[i] = 10.0 + (double)i;
    lt_tblview_init(&v);
    lt_tblview_load_table(&v, &f.ini, f.ini.tables[0], &f.src);
    LT_CHECK(lt_tblview_import(&v, 4, 3, x, y, z) == 0);
    LT_CHECK(lt_tblview_get(&v, 2, 3) == 21.0 && v.x[1] == 2.0 && v.y[0] == 5.0);
    LT_CHECK(lt_tblview_is_dirty(&v));
    LT_CHECK(lt_tblview_import(&v, 3, 2, x, y, bad) == LT_EINVAL);
    LT_CHECK(lt_tblview_undo(&v));
    LT_CHECK_NEAR(lt_tblview_get(&v, 2, 3), 55.5, 1e-9);
    lt_tblview_free(&v);
    fx_free(&f);
}

LT_TEST(curve)
{
    fx f;
    lt_tblview v;
    lt_ini_curve c;
    memset(&c, 0, sizeof c);
    c.name = (char *)"cv";
    c.x_const = (char *)"cx";
    c.y_const = (char *)"cy";
    c.size = -1;
    fx_init(&f);
    lt_tblview_init(&v);
    LT_CHECK(lt_tblview_load_curve(&v, &f.ini, &c, &f.src) == 0);
    LT_CHECK(v.rows == 1 && v.cols == 5 && v.y == NULL);
    LT_CHECK_NEAR(v.x[4], 40.0, 1e-9);
    LT_CHECK_NEAR(lt_tblview_get(&v, 0, 3), 30.0, 1e-9);    /* 15 * 2.0 */
    lt_tblview_set(&v, 0, 3, 100.0);
    LT_CHECK(lt_tblview_commit(&v, &f.ini, &f.src) == 1);
    LT_CHECK(f.page[43] == 50);
    c.size = 3;
    LT_CHECK(lt_tblview_load_curve(&v, &f.ini, &c, &f.src) == 0);
    LT_CHECK(v.cols == 3);
    c.y_const = (char *)"nope";
    LT_CHECK(lt_tblview_load_curve(&v, &f.ini, &c, &f.src) == LT_ENOENT);
    lt_tblview_free(&v);
    fx_free(&f);
}

LT_TEST(dynamic_size_info_and_load)
{
    fx f;
    lt_table_size s;
    lt_tblview v;
    lt_ini_table *t;
    fx_init(&f);
    t = add_dyn(&f);
    LT_CHECK(lt_table_size_info(&f.ini, f.ini.tables[0], &f.src, &s) == LT_ENOENT);
    LT_CHECK(lt_table_size_info(&f.ini, t, &f.src, &s) == 0);
    LT_CHECK_STR(s.cols_const, "dynCols");
    LT_CHECK_STR(s.rows_const, "dynRows");
    LT_CHECK(s.min_cols == 2 && s.max_cols == 8 && s.min_rows == 2 && s.max_rows == 8);
    LT_CHECK(s.max_elements == 40);
    LT_CHECK(s.active_cols == 3 && s.active_rows == 2);

    lt_tblview_init(&v);
    LT_CHECK(lt_tblview_load_table(&v, &f.ini, t, &f.src) == 0);
    LT_CHECK(v.cols == 3 && v.rows == 2);
    LT_CHECK(v.x[2] == 3.0 && v.y[1] == 12.0);
    LT_CHECK(v.z[0] == 200.0 && v.z[5] == 205.0);        /* stride = active cols */
    lt_tblview_set(&v, 1, 2, 7.0);
    LT_CHECK(lt_tblview_commit(&v, &f.ini, &f.src) == 1);
    LT_CHECK(f.page[130 + 5] == 7);
    LT_CHECK(f.page[130 + 6] == 206);                    /* unused tail preserved */
    lt_tblview_free(&v);
    fx_free(&f);
}

LT_TEST(dynamic_size_apply_and_budget)
{
    fx f;
    lt_table_size s;
    lt_ini_table *t;
    double d = 4;
    const lt_ini_table *sh[4];
    fx_init(&f);
    t = add_dyn(&f);
    LT_CHECK(lt_table_size_apply(&f.ini, t, &f.src, 5, 6) == 0);
    LT_CHECK(f.page[100] == 5 && f.page[101] == 6);
    LT_CHECK(lt_table_size_apply(&f.ini, t, &f.src, 8, 8) == LT_ERANGE);   /* 64 > 40 */
    LT_CHECK_STR(lt_err_get(), "8x8 exceeds cell budget 40");
    LT_CHECK(f.page[100] == 5);
    LT_CHECK(lt_table_size_apply(&f.ini, t, &f.src, 1, 6) == LT_ERANGE);
    LT_CHECK(lt_table_size_apply(&f.ini, f.ini.tables[0], &f.src, 5, 6) == LT_ENOENT);
    /* stored value out of budget is clamped on read */
    f.page[100] = 8;
    f.page[101] = 8;
    lt_table_size_info(&f.ini, t, &f.src, &s);
    LT_CHECK(s.active_cols * s.active_rows <= 40);
    LT_CHECK(s.active_cols == 5 && s.active_rows == 8);
    /* stored 0 falls back to INI default */
    lt_map_put(&f.ini.default_values, "dynCols", &d);
    f.page[100] = 0;
    f.page[101] = 3;
    lt_table_size_info(&f.ini, t, &f.src, &s);
    LT_CHECK(s.active_cols == 4 && s.active_rows == 3);
    /* page not loaded: defaults/min */
    f.loaded = LT_FALSE;
    lt_table_size_info(&f.ini, t, &f.src, &s);
    LT_CHECK(s.active_cols == 4 && s.active_rows == 2);
    f.loaded = LT_TRUE;
    /* sharing */
    {
        lt_ini_table *t2 = add_table(&f, "dynTbl2", "dynX", "dynY", "dynZ", 8, 8);
        t2->cols_size_const = lt_strdup("dynCols");
        t2->rows_size_const = lt_strdup("dynRows");
        LT_CHECK(lt_tables_sharing_size(&f.ini, "dynCols", "dynRows", sh, 4) == 2);
        LT_CHECK(sh[0] == t && sh[1] == t2);
        LT_CHECK(lt_tables_sharing_size(&f.ini, "dynCols", "dynRows", NULL, 0) == 2);
        LT_CHECK(lt_tables_sharing_size(&f.ini, "other", "dynRows", sh, 4) == 0);
    }
    fx_free(&f);
}

LT_TEST(dynamic_size_bad_bounds)
{
    fx f;
    lt_table_size s;
    lt_ini_table *t;
    lt_ini_const *cc;
    double nan;
    fx_init(&f);
    t = add_dyn(&f);
    cc = (lt_ini_const *)lt_ini_find_const(&f.ini, "dynCols");
    nan = 1e308 * 10.0;
    nan = nan - nan;
    cc->max = nan;
    cc->min = nan;
    LT_CHECK(lt_table_size_info(&f.ini, t, &f.src, &s) == 0);
    LT_CHECK(s.min_cols == 1 && s.max_cols == 1 && s.active_cols == 1);
    cc->max = 1e300;
    cc->min = -5.0;
    LT_CHECK(lt_table_size_info(&f.ini, t, &f.src, &s) == 0);
    LT_CHECK(s.min_cols == 1 && s.max_cols == 65536);
    fx_free(&f);
}

int main(void)
{
    LT_RUN(load_table);
    LT_RUN(load_not_loaded_page);
    LT_RUN(edit_dirty_commit);
    LT_RUN(commit_quantises_and_axes);
    LT_RUN(revert);
    LT_RUN(undo_redo);
    LT_RUN(undo_axis_edit);
    LT_RUN(view_ops_and_range);
    LT_RUN(import);
    LT_RUN(curve);
    LT_RUN(dynamic_size_info_and_load);
    LT_RUN(dynamic_size_apply_and_budget);
    LT_RUN(dynamic_size_bad_bounds);
    LT_TEST_MAIN_END
}
