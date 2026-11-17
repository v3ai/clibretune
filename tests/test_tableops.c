/* test_tableops.c - pure math of src/tableops (spec 08 sections 1-3). */
#include <string.h>
#include <float.h>
#include "lt.h"
#include "tableops/tableops.h"
#include "lt_test.h"

#define TOPS_EPS 0.01

static void same(const double *a, const double *b, size_t n)
{
    size_t i;
    lt_bool ok = LT_TRUE;
    for (i = 0; i < n; i++)
        if (a[i] != b[i]) ok = LT_FALSE;
    LT_CHECK(ok);
}

/* ---- units ---- */
LT_TEST(units_temp)
{
    LT_CHECK(lt_c_to_f(0.0) == 32.0);
    LT_CHECK(lt_f_to_c(32.0) == 0.0);
    LT_CHECK(lt_c_to_f(100.0) == 212.0);
    LT_CHECK(lt_f_to_c(212.0) == 100.0);
    LT_CHECK(lt_c_to_f(-40.0) == -40.0);
    LT_CHECK(lt_f_to_c(-40.0) == -40.0);
}

LT_TEST(units_pressure)
{
    LT_CHECK_NEAR(lt_kpa_to_psi(100.0), 14.504, TOPS_EPS);
    LT_CHECK_NEAR(lt_psi_to_kpa(14.504), 100.0, TOPS_EPS);
    LT_CHECK_NEAR(lt_kpa_to_psi(101.325), 14.696, TOPS_EPS);
    LT_CHECK_NEAR(lt_psi_to_bar(lt_bar_to_psi(1.7)), 1.7, 1e-12);
    LT_CHECK_NEAR(lt_bar_to_psi(1.0), 14.503773773020923, 1e-12);
}

LT_TEST(units_misc)
{
    LT_CHECK_NEAR(lt_kmh_to_mph(100.0), 62.14, TOPS_EPS);
    LT_CHECK_NEAR(lt_mph_to_kmh(62.14), 100.0, TOPS_EPS);
    LT_CHECK_NEAR(lt_l_to_gal_us(3.78541), 1.0, 1e-4);
    LT_CHECK_NEAR(lt_gal_us_to_l(1.0), 3.78541, 1e-4);
    LT_CHECK_NEAR(lt_l_to_gal_imp(4.54609), 1.0, 1e-4);
    LT_CHECK_NEAR(lt_gal_imp_to_l(1.0), 4.54609, 1e-4);
    LT_CHECK_NEAR(lt_l_to_gal(3.78541, LT_FALSE), 1.0, 1e-4);
    LT_CHECK_NEAR(lt_l_to_gal(4.54609, LT_TRUE), 1.0, 1e-4);
    LT_CHECK_NEAR(lt_gal_to_l(1.0, LT_FALSE), 3.78541, 1e-4);
    LT_CHECK_NEAR(lt_gal_to_l(1.0, 1), 4.54609, 1e-4);
    LT_CHECK_NEAR(lt_lbs_to_kg(100.0), 45.36, TOPS_EPS);
    LT_CHECK_NEAR(lt_kg_to_lbs(45.36), 100.0, TOPS_EPS);
}

LT_TEST(units_afr)
{
    LT_CHECK_NEAR(lt_lambda_to_afr(1.0, "gasoline"), 14.7, 1e-12);
    LT_CHECK_NEAR(lt_afr_to_lambda(14.7, "gasoline"), 1.0, 1e-12);
    LT_CHECK_NEAR(lt_lambda_to_afr(1.0, "E85"), 9.8, 1e-12);
    LT_CHECK_NEAR(lt_lambda_to_afr(1.0, "Petrol"), 14.7, 1e-12);
    LT_CHECK_NEAR(lt_lambda_to_afr(1.0, "methanol"), 6.4, 1e-12);   /* basemap uses 6.5 */
    LT_CHECK_NEAR(lt_fuel_stoich("ethanol"), 9.0, 1e-12);
    LT_CHECK_NEAR(lt_fuel_stoich("lpg"), 15.5, 1e-12);          /* basemap uses 15.7 */
    LT_CHECK_NEAR(lt_fuel_stoich("PROPANE"), 15.5, 1e-12);
    LT_CHECK_NEAR(lt_fuel_stoich("cng"), 17.2, 1e-12);
    LT_CHECK_NEAR(lt_fuel_stoich("natural_gas"), 17.2, 1e-12);
    LT_CHECK_NEAR(lt_fuel_stoich("diesel"), 14.5, 1e-12);
    LT_CHECK_NEAR(lt_fuel_stoich("unobtainium"), 14.7, 1e-12);
    LT_CHECK_NEAR(lt_fuel_stoich(NULL), 14.7, 1e-12);
    LT_CHECK_NEAR(lt_afr_to_lambda(lt_lambda_to_afr(0.85, "e85"), "e85"), 0.85, 1e-12);
}

/* ---- simple ops ---- */
LT_TEST(scale_offset_equal)
{
    double z[6] = { 1, 2, 3, 4, 5, 6 };
    lt_tcell sel[3];
    sel[0].y = 0; sel[0].x = 1;
    sel[1].y = 1; sel[1].x = 2;
    sel[2].y = 9; sel[2].x = 9;      /* out of bounds: skipped */
    lt_tbl_scale(z, 2, 3, sel, 3, 2.0);
    LT_CHECK(z[1] == 4.0 && z[5] == 12.0 && z[0] == 1.0);
    lt_tbl_offset(z, 2, 3, sel, 3, -1.5);
    LT_CHECK(z[1] == 2.5 && z[5] == 10.5);
    lt_tbl_set_equal(z, 2, 3, sel, 3, 7.0);
    LT_CHECK(z[1] == 7.0 && z[5] == 7.0 && z[2] == 3.0);
}

LT_TEST(scale_duplicates)
{
    double z[2] = { 3, 3 };
    lt_tcell sel[2];
    sel[0].y = 0; sel[0].x = 0;
    sel[1].y = 0; sel[1].x = 0;
    lt_tbl_scale(z, 1, 2, sel, 2, 2.0);
    LT_CHECK(z[0] == 12.0 && z[1] == 3.0);
}

LT_TEST(bbox)
{
    lt_tcell sel[3];
    size_t a, b, c, d;
    sel[0].y = 2; sel[0].x = 5;
    sel[1].y = 1; sel[1].x = 7;
    sel[2].y = 4; sel[2].x = 3;
    LT_CHECK(lt_tbl_bbox(sel, 3, &a, &b, &c, &d));
    LT_CHECK(a == 1 && b == 4 && c == 3 && d == 7);
    LT_CHECK(!lt_tbl_bbox(sel, 0, &a, &b, &c, &d));
    LT_CHECK(lt_tbl_sel_has(sel, 3, 4, 3));
    LT_CHECK(!lt_tbl_sel_has(sel, 3, 4, 4));
}

static size_t all_cells(lt_tcell *sel, size_t rows, size_t cols)
{
    size_t y, x, n = 0;
    for (y = 0; y < rows; y++)
        for (x = 0; x < cols; x++) {
            sel[n].y = y;
            sel[n].x = x;
            n++;
        }
    return n;
}

/* ---- interp2d ---- */
LT_TEST(interp2d_3x3)
{
    double z[9] = { 10, 0, 40, 0, 0, 0, 20, 0, 80 };
    lt_tcell sel[9];
    size_t n = all_cells(sel, 3, 3);
    lt_tbl_interp2d(z, 3, 3, sel, n);
    LT_CHECK(z[0] == 10.0 && z[2] == 40.0 && z[6] == 20.0 && z[8] == 80.0);
    LT_CHECK_NEAR(z[4], 37.5, 1e-12);
}

LT_TEST(interp2d_row_col)
{
    double r[4] = { 10, 0, 0, 40 };
    double c[4] = { 10, 0, 0, 40 };
    lt_tcell sel[4];
    size_t n = all_cells(sel, 1, 4);
    lt_tbl_interp2d(r, 1, 4, sel, n);
    LT_CHECK_NEAR(r[0], 10.0, 1e-9);
    LT_CHECK_NEAR(r[1], 20.0, 1e-9);
    LT_CHECK_NEAR(r[2], 30.0, 1e-9);
    LT_CHECK_NEAR(r[3], 40.0, 1e-9);
    n = all_cells(sel, 4, 1);
    lt_tbl_interp2d(c, 4, 1, sel, n);
    LT_CHECK_NEAR(c[1], 20.0, 1e-9);
    LT_CHECK_NEAR(c[2], 30.0, 1e-9);
}

LT_TEST(interp2d_rejects)
{
    double z[4] = { 1, 2, 3, 4 }, o[4] = { 1, 2, 3, 4 };
    lt_tcell sel[4];
    sel[0].y = 0; sel[0].x = 0;
    sel[1].y = 0; sel[1].x = 5;
    sel[2].y = 9; sel[2].x = 0;
    sel[3].y = 9; sel[3].x = 5;
    lt_tbl_interp2d(z, 2, 2, sel, 4);
    same(z, o, 4);
    /* fewer than 4 cells: unchanged */
    {
        double w[4] = { 1, 0, 0, 5 };
        double w0[4] = { 1, 0, 0, 5 };
        lt_tbl_interp2d(w, 2, 2, sel, 3);
        same(w, w0, 4);
    }
}

LT_TEST(interp2d_partial_box_touches_all_box_cells)
{
    /* 3 wide box, selection 4 corners only: interior cell is overwritten too */
    double z[9] = { 0, 5, 10, 5, 99, 5, 20, 5, 30 };
    lt_tcell sel[4];
    sel[0].y = 0; sel[0].x = 0;
    sel[1].y = 0; sel[1].x = 2;
    sel[2].y = 2; sel[2].x = 0;
    sel[3].y = 2; sel[3].x = 2;
    lt_tbl_interp2d(z, 3, 3, sel, 4);
    LT_CHECK_NEAR(z[4], 15.0, 1e-12);
    LT_CHECK_NEAR(z[1], 5.0, 1e-12);
}

/* ---- interp_linear ---- */
LT_TEST(interp_linear_row)
{
    double z[8] = { 10, 0, 0, 50, 20, 0, 0, 80 };
    lt_tcell sel[4];
    size_t n = all_cells(sel, 1, 4);
    lt_tbl_interp_linear(z, 2, 4, sel, n, LT_TBL_AXIS_ROW);
    LT_CHECK_NEAR(z[0], 10.0, 1e-9);
    LT_CHECK_NEAR(z[1], 23.3333333333, 1e-6);
    LT_CHECK_NEAR(z[2], 36.6666666667, 1e-6);
    LT_CHECK_NEAR(z[3], 50.0, 1e-9);
    LT_CHECK(z[4] == 20.0 && z[5] == 0.0 && z[6] == 0.0 && z[7] == 80.0);
}

LT_TEST(interp_linear_col)
{
    double z[8] = { 10, 20, 0, 0, 0, 0, 40, 80 };
    lt_tcell sel[4];
    size_t i;
    for (i = 0; i < 4; i++) {
        sel[i].y = i;
        sel[i].x = 1;
    }
    lt_tbl_interp_linear(z, 4, 2, sel, 4, LT_TBL_AXIS_COL);
    LT_CHECK_NEAR(z[1], 20.0, 1e-9);
    LT_CHECK_NEAR(z[3], 40.0, 1e-9);
    LT_CHECK_NEAR(z[5], 60.0, 1e-9);
    LT_CHECK_NEAR(z[7], 80.0, 1e-9);
    LT_CHECK(z[0] == 10.0 && z[2] == 0.0 && z[4] == 0.0 && z[6] == 40.0);
}

LT_TEST(interp_linear_partial_and_oob)
{
    double z[4] = { 10, 0, 0, 40 };
    double o[4] = { 10, 0, 0, 40 };
    lt_tcell sel[2];
    sel[0].y = 0; sel[0].x = 1;
    sel[1].y = 0; sel[1].x = 2;
    lt_tbl_interp_linear(z, 1, 4, sel, 2, LT_TBL_AXIS_ROW);
    same(z, o, 4);      /* anchors are cells 1 and 2 (both 0) */
    sel[1].x = 9;
    lt_tbl_interp_linear(z, 1, 4, sel, 2, LT_TBL_AXIS_ROW);
    same(z, o, 4);
    lt_tbl_interp_linear(z, 1, 4, sel, 0, LT_TBL_AXIS_ROW);
    same(z, o, 4);
}

LT_TEST(interp_linear_unselected_gap_cell_kept)
{
    double z[5] = { 0, 9, 9, 9, 40 };
    lt_tcell sel[4];
    sel[0].y = 0; sel[0].x = 0;
    sel[1].y = 0; sel[1].x = 1;
    sel[2].y = 0; sel[2].x = 3;
    sel[3].y = 0; sel[3].x = 4;
    lt_tbl_interp_linear(z, 1, 5, sel, 4, LT_TBL_AXIS_ROW);
    LT_CHECK_NEAR(z[1], 10.0, 1e-9);
    LT_CHECK(z[2] == 9.0);      /* not in selection */
    LT_CHECK_NEAR(z[3], 30.0, 1e-9);
}

/* ---- fill ---- */
LT_TEST(fill_right)
{
    double z[6] = { 10, 0, 0, 20, 0, 0 };
    lt_tcell sel[6];
    size_t n = all_cells(sel, 2, 3);
    lt_tbl_fill(z, 2, 3, sel, n, LT_TBL_FILL_RIGHT);
    LT_CHECK(z[0] == 10 && z[1] == 10 && z[2] == 10);
    LT_CHECK(z[3] == 20 && z[4] == 20 && z[5] == 20);
}

LT_TEST(fill_down)
{
    double z[9] = { 10, 20, 30, 0, 0, 0, 0, 0, 0 };
    lt_tcell sel[9];
    size_t n = all_cells(sel, 3, 3);
    lt_tbl_fill(z, 3, 3, sel, n, LT_TBL_FILL_DOWN);
    LT_CHECK(z[3] == 10 && z[4] == 20 && z[5] == 30);
    LT_CHECK(z[6] == 10 && z[7] == 20 && z[8] == 30);
}

LT_TEST(fill_oob_rejects_whole_op)
{
    double z[3] = { 10, 0, 0 }, o[3] = { 10, 0, 0 };
    lt_tcell sel[4];
    sel[0].y = 0; sel[0].x = 0;
    sel[1].y = 0; sel[1].x = 1;
    sel[2].y = 0; sel[2].x = 2;
    sel[3].y = 0; sel[3].x = 99;
    lt_tbl_fill(z, 1, 3, sel, 4, LT_TBL_FILL_RIGHT);
    same(z, o, 3);
}

/* ---- smooth ---- */
static void grid3(double *z, double center)
{
    size_t i;
    for (i = 0; i < 9; i++) z[i] = 10.0;
    z[4] = center;
}

LT_TEST(smooth_basic)
{
    double z[9], o[9];
    lt_tcell sel[1];
    double expect, w1, w2, wsum;
    grid3(z, 50.0);
    sel[0].y = 1; sel[0].x = 1;
    lt_tbl_smooth(z, 3, 3, sel, 1, 1.0);
    LT_CHECK(z[4] > 10.0 && z[4] < 50.0);
    w1 = 0.6065306597126334;
    w2 = 0.36787944117144233;
    wsum = 1.0 + 4.0 * w1 + 4.0 * w2;
    expect = (50.0 + 10.0 * (wsum - 1.0)) / wsum;
    LT_CHECK_NEAR(z[4], expect, 1e-9);
    LT_CHECK_NEAR(expect, 18.17, 0.01);
    /* neighbours untouched */
    grid3(o, 50.0);
    LT_CHECK(z[0] == 10.0 && z[1] == 10.0 && z[8] == 10.0);
}

LT_TEST(smooth_corner_and_edge)
{
    double z[9];
    lt_tcell sel[1];
    grid3(z, 10.0);
    z[0] = 50.0;
    sel[0].y = 0; sel[0].x = 0;
    lt_tbl_smooth(z, 3, 3, sel, 1, 1.0);
    LT_CHECK(z[0] > 10.0 && z[0] < 50.0);
    grid3(z, 10.0);
    z[1] = 50.0;
    sel[0].y = 0; sel[0].x = 1;
    lt_tbl_smooth(z, 3, 3, sel, 1, 1.0);
    LT_CHECK(z[1] > 10.0 && z[1] < 50.0);
}

LT_TEST(smooth_factor_and_reject)
{
    double a[9], b[9], z[9], o[9];
    lt_tcell sel[2];
    sel[0].y = 1; sel[0].x = 1;
    grid3(z, 50.0);
    lt_tbl_smooth(z, 3, 3, sel, 1, 0.0);
    LT_CHECK(z[4] == 50.0);
    lt_tbl_smooth(z, 3, 3, sel, 1, -1.0);
    LT_CHECK(z[4] == 50.0);
    grid3(a, 50.0);
    grid3(b, 50.0);
    lt_tbl_smooth(a, 3, 3, sel, 1, 2.0);
    lt_tbl_smooth(b, 3, 3, sel, 1, 0.5);
    LT_CHECK(a[4] < b[4]);
    /* out-of-bounds cell rejects the whole op */
    grid3(z, 50.0);
    grid3(o, 50.0);
    sel[0].y = 3; sel[0].x = 3;
    lt_tbl_smooth(z, 3, 3, sel, 1, 1.0);
    same(z, o, 9);
    {
        double q[4] = { 1, 2, 3, 4 }, q0[4] = { 1, 2, 3, 4 };
        sel[0].y = 0; sel[0].x = 1;
        sel[1].y = 2; sel[1].x = 2;
        lt_tbl_smooth(q, 2, 2, sel, 2, 1.0);
        same(q, q0, 4);
    }
}

LT_TEST(smooth_order_independent)
{
    double a[9], b[9];
    lt_tcell s1[2], s2[2];
    size_t i;
    for (i = 0; i < 9; i++) a[i] = b[i] = (double)(i * i);
    s1[0].y = 0; s1[0].x = 0; s1[1].y = 1; s1[1].x = 1;
    s2[0] = s1[1]; s2[1] = s1[0];
    lt_tbl_smooth(a, 3, 3, s1, 2, 1.0);
    lt_tbl_smooth(b, 3, 3, s2, 2, 1.0);
    same(a, b, 9);
}

/* ---- copy / paste ---- */
LT_TEST(copy_paste)
{
    double z[9] = { 1, 2, 3, 4, 5, 6, 7, 8, 9 };
    double buf[16], dst[4] = { 0, 0, 0, 0 };
    lt_tcell sel[2];
    size_t h, w;
    sel[0].y = 1; sel[0].x = 1;
    sel[1].y = 2; sel[1].x = 2;
    lt_tbl_copy(z, 3, 3, sel, 2, buf, 16, &h, &w);
    LT_CHECK(h == 2 && w == 2);
    LT_CHECK(buf[0] == 5 && buf[1] == 6 && buf[2] == 8 && buf[3] == 9);
    lt_tbl_copy(z, 3, 3, sel, 2, buf, 3, &h, &w);   /* too small */
    LT_CHECK(h == 0 && w == 0);
    lt_tbl_paste(dst, 2, 2, 1, 1, z, 2, 2);        /* clipped to the single cell */
    LT_CHECK(dst[3] == 1.0 && dst[0] == 0.0);
    lt_tbl_paste(z, 3, 3, 0, 1, buf, 0, 0);
    LT_CHECK(z[1] == 2.0);
}

/* ---- bilinear / rebin ---- */
LT_TEST(bilinear)
{
    double xb[2] = { 0, 10 }, yb[2] = { 0, 10 };
    double z[4] = { 0, 10, 100, 110 };
    LT_CHECK_NEAR(lt_tbl_bilinear(5, 5, xb, 2, yb, 2, z), 55.0, 1e-12);
    LT_CHECK_NEAR(lt_tbl_bilinear(-5, -5, xb, 2, yb, 2, z), 0.0, 1e-12);
    LT_CHECK_NEAR(lt_tbl_bilinear(99, 99, xb, 2, yb, 2, z), 110.0, 1e-12);
    LT_CHECK_NEAR(lt_tbl_bilinear(10, 0, xb, 2, yb, 2, z), 10.0, 1e-12);
    LT_CHECK_NEAR(lt_tbl_bilinear(1, 1, xb, 0, yb, 2, z), 0.0, 1e-12);
}

LT_TEST(rebin_same_and_shrink)
{
    double xb[4] = { 500, 1500, 2500, 3500 }, yb[4] = { 20, 40, 60, 80 };
    double z[16], out[16], nx[2] = { 500, 3000 }, ny[2] = { 20, 80 };
    size_t i;
    for (i = 0; i < 16; i++) z[i] = (double)i * 3.0;
    LT_CHECK(lt_tbl_rebin(xb, 4, yb, 4, z, xb, 4, yb, 4, LT_TRUE, out) == 0);
    for (i = 0; i < 16; i++) LT_CHECK_NEAR(out[i], z[i], TOPS_EPS);
    LT_CHECK(lt_tbl_rebin(xb, 4, yb, 4, z, nx, 2, ny, 2, LT_TRUE, out) == 0);
    LT_CHECK_NEAR(out[0], z[0], TOPS_EPS);
    LT_CHECK_NEAR(out[1], lt_tbl_bilinear(3000, 20, xb, 4, yb, 4, z), 1e-9);
    LT_CHECK_NEAR(out[3], lt_tbl_bilinear(3000, 80, xb, 4, yb, 4, z), 1e-9);
}

LT_TEST(rebin_clamp)
{
    double xb[2] = { 0, 10 }, yb[2] = { 0, 10 };
    double z[4] = { 0, 10, 100, 110 };
    double nx[3] = { -5, 0, 15 }, ny[3] = { -2, 0, 12 }, out[9];
    LT_CHECK(lt_tbl_rebin(xb, 2, yb, 2, z, nx, 3, ny, 3, LT_TRUE, out) == 0);
    LT_CHECK(out[0] == 0.0);
    LT_CHECK(out[4] == 0.0);
    LT_CHECK(out[8] == 110.0);
}

LT_TEST(rebin_no_interp)
{
    double xb[3] = { 1, 2, 3 }, yb[3] = { 1, 2, 3 };
    double z[9] = { 1, 2, 3, 4, 5, 6, 7, 8, 9 };
    double out[16], nxb[4] = { 1, 2, 3, 4 }, nyb[4] = { 1, 2, 3, 4 }, o2[4];
    double x4[4] = { 1, 2, 3, 4 }, y4[4] = { 1, 2, 3, 4 };
    double z4[16], sx[2] = { 1, 2 }, sy[2] = { 1, 2 };
    size_t i;
    /* same size is identical */
    LT_CHECK(lt_tbl_rebin(xb, 3, yb, 3, z, xb, 3, yb, 3, LT_FALSE, out) == 0);
    same(out, z, 9);
    /* grow 3x3 -> 4x4 */
    LT_CHECK(lt_tbl_rebin(xb, 3, yb, 3, z, nxb, 4, nyb, 4, LT_FALSE, out) == 0);
    LT_CHECK(out[12] == z[6]);        /* [3][0] = old[2][0] */
    LT_CHECK(out[3] == z[2]);         /* [0][3] = old[0][2] */
    LT_CHECK(out[15] == z[8]);
    LT_CHECK(out[0] == 1 && out[1] == 2 && out[2] == 3 && out[4] == 4 && out[10] == 9);
    /* shrink 4x4 -> 2x2 keeps the top-left block */
    for (i = 0; i < 16; i++) z4[i] = (double)(i + 1);
    LT_CHECK(lt_tbl_rebin(x4, 4, y4, 4, z4, sx, 2, sy, 2, LT_FALSE, o2) == 0);
    LT_CHECK(o2[0] == 1 && o2[1] == 2 && o2[2] == 5 && o2[3] == 6);
    /* empty old grid: zeros, no crash */
    o2[0] = o2[1] = o2[2] = o2[3] = 7.0;
    LT_CHECK(lt_tbl_rebin(NULL, 0, NULL, 0, NULL, sx, 2, sy, 2, LT_FALSE, o2) == 0);
    LT_CHECK(o2[0] == 0 && o2[1] == 0 && o2[2] == 0 && o2[3] == 0);
    LT_CHECK(lt_tbl_rebin(xb, 3, yb, 3, z, sx, 2, sy, 2, LT_FALSE, NULL) == LT_EINVAL);
}

/* ---- VE-style lookup ---- */
LT_TEST(lookup_ve)
{
    double rpm[2] = { 1000, 2000 }, load[2] = { 40, 80 };
    double ve[4] = { 50, 60, 70, 80 };
    double r = -1;
    LT_CHECK(lt_tbl_lookup(1500, 60, rpm, 2, load, 2, ve, &r)); LT_CHECK_NEAR(r, 65.0, 1e-12);
    LT_CHECK(lt_tbl_lookup(1000, 60, rpm, 2, load, 2, ve, &r)); LT_CHECK_NEAR(r, 60.0, 1e-12);
    LT_CHECK(lt_tbl_lookup(1500, 40, rpm, 2, load, 2, ve, &r)); LT_CHECK_NEAR(r, 55.0, 1e-12);
    LT_CHECK(lt_tbl_lookup(0, 0, rpm, 2, load, 2, ve, &r));     LT_CHECK_NEAR(r, 50.0, 1e-12);
    LT_CHECK(lt_tbl_lookup(9000, 300, rpm, 2, load, 2, ve, &r)); LT_CHECK_NEAR(r, 80.0, 1e-12);
    LT_CHECK(!lt_tbl_lookup(1, 1, rpm, 0, load, 2, ve, &r));
    LT_CHECK(!lt_tbl_lookup(1, 1, rpm, 2, NULL, 2, ve, &r));
}

LT_TEST(lookup_single_bins)
{
    double xb[1] = { 5 }, yb[1] = { 5 }, z[1] = { 42 }, r;
    LT_CHECK(lt_tbl_lookup(100, -4, xb, 1, yb, 1, z, &r));
    LT_CHECK_NEAR(r, 42.0, 1e-12);
}

LT_TEST(nearest_linspace)
{
    double b[4] = { 0, 10, 20, 30 }, ls[5], e[2] = { 1, 3 };
    LT_CHECK(lt_tbl_nearest(b, 4, 14.0) == 1);
    LT_CHECK(lt_tbl_nearest(b, 4, 15.0) == 1);     /* tie: first wins */
    LT_CHECK(lt_tbl_nearest(b, 4, 99.0) == 3);
    LT_CHECK(lt_tbl_nearest(b, 0, 5.0) == 0);
    LT_CHECK(lt_tbl_nearest(e, 2, 2.0) == 0);
    lt_tbl_linspace(0, 100, 5, ls);
    LT_CHECK(ls[0] == 0 && ls[1] == 25 && ls[2] == 50 && ls[3] == 75 && ls[4] == 100);
    lt_tbl_linspace(7, 9, 1, ls);
    LT_CHECK(ls[0] == 7);
}

/* ---- dynamic tables ---- */
LT_TEST(dyn_pack_unpack)
{
    double z[6] = { 1, 2, 3, 4, 5, 6 }, flat[16], back[6];
    size_t i;
    for (i = 0; i < 16; i++) flat[i] = 9.0;
    lt_dyn_pack(flat, 16, z, 2, 3);
    for (i = 0; i < 6; i++) LT_CHECK(flat[i] == (double)(i + 1));
    LT_CHECK(flat[6] == 9.0 && flat[15] == 9.0);
    lt_dyn_unpack(flat, 16, 3, 2, back);
    same(back, z, 6);
    lt_dyn_unpack(flat, 4, 3, 2, back);       /* short source -> zeros */
    LT_CHECK(back[3] == 4.0 && back[4] == 0.0 && back[5] == 0.0 && back[0] == 1.0);
    lt_dyn_pack(flat, 4, z, 2, 3);            /* small alloc: no overflow */
    LT_CHECK(flat[4] == 5.0);
}

LT_TEST(dyn_resolve_axis_count)
{
    LT_CHECK(lt_dyn_resolve_axis_count(LT_TRUE, 0, 8, 64, LT_TRUE, 32) == 32);
    LT_CHECK(lt_dyn_resolve_axis_count(LT_TRUE, 1, 8, 64, LT_TRUE, 32) == 32);
    LT_CHECK(lt_dyn_resolve_axis_count(LT_FALSE, 0, 8, 64, LT_TRUE, 32) == 32);
    LT_CHECK(lt_dyn_resolve_axis_count(LT_TRUE, 24, 8, 64, LT_TRUE, 32) == 24);
    LT_CHECK(lt_dyn_resolve_axis_count(LT_TRUE, 100, 8, 64, LT_TRUE, 32) == 64);
    LT_CHECK(lt_dyn_resolve_axis_count(LT_FALSE, 0, 8, 64, LT_FALSE, 0) == 8);
    LT_CHECK(lt_dyn_resolve_axis_count(LT_TRUE, 23.6, 8, 64, LT_TRUE, 32) == 24);
    LT_CHECK(lt_dyn_resolve_axis_count(LT_FALSE, 0, 8, 64, LT_TRUE, 500) == 64);
}

LT_TEST(dyn_slice_bins)
{
    double b[3] = { 1, 2, 3 }, out[6];
    lt_dyn_slice_bins(b, 3, 2, out);
    LT_CHECK(out[0] == 1 && out[1] == 2);
    lt_dyn_slice_bins(b, 3, 5, out);
    LT_CHECK(out[2] == 3 && out[3] == 3 && out[4] == 3);
    out[0] = 5;
    lt_dyn_slice_bins(NULL, 0, 0, out);       /* writes max(active,1) zeros */
    LT_CHECK(out[0] == 0.0);
    out[0] = out[1] = out[2] = 5;
    lt_dyn_slice_bins(NULL, 0, 3, out);
    LT_CHECK(out[0] == 0 && out[1] == 0 && out[2] == 0);
}

LT_TEST(dyn_allocated)
{
    lt_shape s;
    memset(&s, 0, sizeof s);
    LT_CHECK(lt_dyn_allocated_elements(&s) == 1);
    s.kind = LT_SH_1D; s.n = 0;
    LT_CHECK(lt_dyn_allocated_elements(&s) == 1);
    s.n = 12;
    LT_CHECK(lt_dyn_allocated_elements(&s) == 12);
    s.kind = LT_SH_2D; s.rows = 16; s.cols = 16;
    LT_CHECK(lt_dyn_allocated_elements(&s) == 256);
}

LT_TEST(table_size_allows_and_clamp)
{
    lt_table_size s;
    memset(&s, 0, sizeof s);
    s.min_cols = 4; s.max_cols = 16; s.min_rows = 4; s.max_rows = 16; s.max_elements = 100;
    LT_CHECK(lt_table_size_allows(&s, 8, 8) == 0);
    LT_CHECK(lt_table_size_allows(&s, 3, 8) == LT_ERANGE);
    LT_CHECK_STR(lt_err_get(), "Columns 3 out of range 4..16");
    LT_CHECK(lt_table_size_allows(&s, 17, 8) == LT_ERANGE);
    LT_CHECK(lt_table_size_allows(&s, 8, 17) == LT_ERANGE);
    LT_CHECK_STR(lt_err_get(), "Rows 17 out of range 4..16");
    LT_CHECK(lt_table_size_allows(&s, 16, 16) == LT_ERANGE);
    LT_CHECK_STR(lt_err_get(), "16x16 exceeds cell budget 100");
    LT_CHECK(lt_table_size_allows(&s, 10, 10) == 0);
    s.active_cols = 16; s.active_rows = 16;
    lt_table_size_clamp_to_budget(&s);
    LT_CHECK(s.active_cols == 6 && s.active_rows == 16);   /* 6*16=96 */
    s.min_cols = 12; s.active_cols = 16; s.active_rows = 16;
    lt_table_size_clamp_to_budget(&s);
    LT_CHECK(s.active_cols == 12 && s.active_rows == 8);   /* 12*8=96 */
}

LT_TEST(dyn_resolve_nonfinite)
{
    double inf = 1e308 * 10.0, nan;
    nan = inf - inf;
    LT_CHECK(lt_dyn_resolve_axis_count(LT_TRUE, inf, 2, 16, LT_FALSE, 0.0) == 16);
    LT_CHECK(lt_dyn_resolve_axis_count(LT_TRUE, nan, 2, 16, LT_TRUE, 8.0) == 8);
    LT_CHECK(lt_dyn_resolve_axis_count(LT_FALSE, 0.0, 2, 16, LT_TRUE, nan) == 2);
    LT_CHECK(lt_dyn_resolve_axis_count(LT_TRUE, 1e30, 2, 16, LT_TRUE, 8.0) == 16);
    LT_CHECK(lt_dyn_resolve_axis_count(LT_TRUE, -inf, 2, 16, LT_TRUE, 8.0) == 8);
}

/* random selections (including out of range) and shapes must never crash */
LT_TEST(ops_random_fuzz)
{
    unsigned long seed = 12345UL;
    int it;
    for (it = 0; it < 3000; it++) {
        double z[6 * 7], out[64], xb[7], yb[6];
        lt_tcell sel[12];
        size_t rows, cols, n, i, bh, bw;
        seed = (seed * 1103515245UL + 12345UL) & 0x7FFFFFFFUL;
        rows = 1 + (seed >> 8) % 6;
        cols = 1 + (seed >> 12) % 7;
        n = (seed >> 4) % 13;
        for (i = 0; i < rows * cols; i++) z[i] = (double)(i * 3 % 17);
        for (i = 0; i < cols; i++) xb[i] = (double)i;
        for (i = 0; i < rows; i++) yb[i] = (double)i;
        for (i = 0; i < n; i++) {
            seed = (seed * 1103515245UL + 12345UL) & 0x7FFFFFFFUL;
            sel[i].y = (seed >> 6) % 9;
            sel[i].x = (seed >> 10) % 10;
        }
        lt_tbl_scale(z, rows, cols, sel, n, 1.5);
        lt_tbl_interp2d(z, rows, cols, sel, n);
        lt_tbl_interp_linear(z, rows, cols, sel, n, (int)(seed & 1));
        lt_tbl_fill(z, rows, cols, sel, n, (int)((seed >> 1) & 1));
        lt_tbl_smooth(z, rows, cols, sel, n, 1.0);
        lt_tbl_copy(z, rows, cols, sel, n, out, 64, &bh, &bw);
        lt_tbl_paste(z, rows, cols, seed % 9, seed % 10, out, bh, bw);
        LT_CHECK(bh * bw <= 64);
        (void)lt_tbl_bilinear((double)(seed % 11) - 2.0, (double)(seed % 7) - 2.0, xb, cols, yb,
                              rows, z);
        LT_CHECK(lt_tbl_lookup((double)(seed % 11) - 2.0, (double)(seed % 7) - 2.0, xb, cols,
                               yb, rows, z, &out[0]));
        LT_CHECK(lt_tbl_rebin(xb, cols, yb, rows, z, xb, cols, yb, rows, (lt_bool)(seed & 1),
                              out) == 0 || rows * cols > 64);
    }
}

int main(void)
{
    LT_RUN(units_temp);
    LT_RUN(units_pressure);
    LT_RUN(units_misc);
    LT_RUN(units_afr);
    LT_RUN(scale_offset_equal);
    LT_RUN(scale_duplicates);
    LT_RUN(bbox);
    LT_RUN(interp2d_3x3);
    LT_RUN(interp2d_row_col);
    LT_RUN(interp2d_rejects);
    LT_RUN(interp2d_partial_box_touches_all_box_cells);
    LT_RUN(interp_linear_row);
    LT_RUN(interp_linear_col);
    LT_RUN(interp_linear_partial_and_oob);
    LT_RUN(interp_linear_unselected_gap_cell_kept);
    LT_RUN(fill_right);
    LT_RUN(fill_down);
    LT_RUN(fill_oob_rejects_whole_op);
    LT_RUN(smooth_basic);
    LT_RUN(smooth_corner_and_edge);
    LT_RUN(smooth_factor_and_reject);
    LT_RUN(smooth_order_independent);
    LT_RUN(copy_paste);
    LT_RUN(bilinear);
    LT_RUN(rebin_same_and_shrink);
    LT_RUN(rebin_clamp);
    LT_RUN(rebin_no_interp);
    LT_RUN(lookup_ve);
    LT_RUN(lookup_single_bins);
    LT_RUN(nearest_linspace);
    LT_RUN(dyn_pack_unpack);
    LT_RUN(dyn_resolve_axis_count);
    LT_RUN(dyn_slice_bins);
    LT_RUN(dyn_allocated);
    LT_RUN(dyn_resolve_nonfinite);
    LT_RUN(ops_random_fuzz);
    LT_RUN(table_size_allows_and_clamp);
    LT_TEST_MAIN_END
}
