/* test_tableedit.c - editor-level table operations: restore, clear, paste modes, resize maths.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <string.h>
#include "lt.h"
#include "tableops/tableops.h"
#include "app/tables_priv.h"
#include "lt_test.h"

LT_TEST(restore_selection)
{
    double z[6] = { 1, 2, 3, 4, 5, 6 };
    const double orig[6] = { 10, 20, 30, 40, 50, 60 };
    lt_tcell sel[2];
    sel[0].y = 0; sel[0].x = 1;
    sel[1].y = 1; sel[1].x = 2;
    lt_tbl_restore(z, 2, 3, sel, 2, orig);
    LT_CHECK_NEAR(z[0], 1.0, 1e-12);
    LT_CHECK_NEAR(z[1], 20.0, 1e-12);
    LT_CHECK_NEAR(z[5], 60.0, 1e-12);
    sel[0].y = 9;
    lt_tbl_restore(z, 2, 3, sel, 1, orig);   /* out of range is skipped */
    LT_CHECK_NEAR(z[2], 3.0, 1e-12);
}

LT_TEST(clear_clamps)
{
    double z[4] = { 5, 6, 7, 8 };
    lt_tcell sel[1];
    sel[0].y = 0; sel[0].x = 1;
    lt_tbl_clear(z, 2, 2, sel, 1, 0.0, 100.0);
    LT_CHECK_NEAR(z[1], 0.0, 1e-12);
    lt_tbl_clear(z, 2, 2, sel, 1, 10.0, 100.0);
    LT_CHECK_NEAR(z[1], 10.0, 1e-12);
    lt_tbl_clear(z, 2, 2, sel, 1, -50.0, -10.0);
    LT_CHECK_NEAR(z[1], -10.0, 1e-12);
    LT_CHECK_NEAR(z[0], 5.0, 1e-12);
}

LT_TEST(paste_modes)
{
    double z[4], src[2] = { 2.0, 3.0 };
    int m;
    static const double want[6][2] = { { 2, 3 }, { 12, 23 }, { 8, 17 }, { 20, 60 }, { 2, 3 },
                                       { 10, 20 } };
    for (m = 0; m < 6; m++) {
        z[0] = 10; z[1] = 20; z[2] = 7; z[3] = 8;
        lt_tbl_paste_op(z, 2, 2, 0, 0, src, 1, 2, m);
        LT_CHECK_NEAR(z[0], want[m][0], 1e-12);
        LT_CHECK_NEAR(z[1], want[m][1], 1e-12);
        LT_CHECK_NEAR(z[2], 7.0, 1e-12);
    }
    /* clipped at the edge */
    z[0] = 1; z[1] = 1; z[2] = 1; z[3] = 1;
    lt_tbl_paste_op(z, 2, 2, 1, 1, src, 1, 2, LT_TBL_PASTE_ADD);
    LT_CHECK_NEAR(z[3], 3.0, 1e-12);
    LT_CHECK_NEAR(z[2], 1.0, 1e-12);
}

LT_TEST(resize_maths)
{
    double xb[2] = { 0, 10 }, yb[2] = { 0, 100 }, z[4] = { 0, 10, 100, 110 };
    double ox[3], oy[3], oz[9];
    LT_CHECK(lt_tbl_resize(xb, 2, yb, 2, z, 3, 3, ox, oy, oz) == 0);
    LT_CHECK_NEAR(ox[1], 5.0, 1e-9);
    LT_CHECK_NEAR(oy[1], 50.0, 1e-9);
    LT_CHECK_NEAR(oz[0], 0.0, 1e-9);
    LT_CHECK_NEAR(oz[4], 55.0, 1e-9);    /* centre: bilinear */
    LT_CHECK_NEAR(oz[8], 110.0, 1e-9);
    LT_CHECK(lt_tbl_resize(NULL, 0, yb, 2, z, 3, 3, ox, oy, oz) == LT_EINVAL);
    LT_CHECK(lt_tbl_resize(xb, 2, yb, 2, z, 0, 3, ox, oy, oz) == LT_EINVAL);
}

LT_TEST(clamp_between)
{
    double x[4] = { 1, 2, 5, 9 };
    LT_CHECK_NEAR(lt_tbl_clamp_between(x, 4, 1, 3.0), 3.0, 1e-12);
    LT_CHECK_NEAR(lt_tbl_clamp_between(x, 4, 1, 0.0), 1.0, 1e-12);
    LT_CHECK_NEAR(lt_tbl_clamp_between(x, 4, 1, 7.0), 5.0, 1e-12);
    LT_CHECK_NEAR(lt_tbl_clamp_between(x, 4, 0, -9.0), -9.0, 1e-12);
    LT_CHECK_NEAR(lt_tbl_clamp_between(x, 4, 3, 99.0), 99.0, 1e-12);
    LT_CHECK_NEAR(lt_tbl_clamp_between(x, 4, 9, 4.0), 4.0, 1e-12);
}

LT_TEST(generate_values)
{
    lt_engine_spec e;
    double xb[4] = { 1000, 2000, 3000, 4000 }, yb[3] = { 30, 60, 90 }, ox[4], oy[3], oz[12];
    double ax[4], ay[3];
    size_t i;
    lt_engine_spec_default(&e);
    LT_CHECK(lt_tabs_gen_values(&e, LT_ROLE_VE, 0, xb, 4, yb, 3, ox, oy, oz) == 0);
    LT_CHECK_NEAR(ox[2], 3000.0, 1e-12);
    for (i = 0; i < 12; i++) { LT_CHECK(oz[i] > 0.0 && oz[i] < 200.0); }
    LT_CHECK(lt_tabs_gen_values(&e, LT_ROLE_IGNITION, 1, xb, 4, yb, 3, ax, ay, oz) == 0);
    LT_CHECK_NEAR(ax[0], (double)e.idle_rpm, 1.0);
    LT_CHECK_NEAR(ax[3], (double)e.redline_rpm, 1.0);
    LT_CHECK(lt_tabs_gen_values(&e, LT_ROLE_OTHER, 0, xb, 4, yb, 3, ox, oy, oz) == LT_EINVAL);
}

LT_TEST(table_file_roundtrip_and_fit)
{
    double x[3] = { 1, 2, 3 }, y[2] = { 10, 20 }, z[6] = { 1, 2, 3, 4, 5, 6 };
    double nxb[2] = { 1, 3 }, nyb[2] = { 10, 20 }, fit[4];
    lt_table_file tf;
    char *xml = lt_table_file_write("RPM", "MAP", x, 3, y, 2, z);
    LT_CHECK(xml != NULL);
    LT_CHECK(lt_table_file_parse(xml, strlen(xml), &tf) == 0);
    lt_free(xml);
    LT_CHECK(tf.cols == 3 && tf.rows == 2);
    LT_CHECK(lt_tabs_io_fit(&tf, x, 3, y, 2, z) == 0);
    LT_CHECK_NEAR(z[5], 6.0, 1e-9);
    LT_CHECK(lt_tabs_io_fit(&tf, nxb, 2, nyb, 2, fit) == 0);    /* 3x2 file onto a 2x2 table */
    LT_CHECK_NEAR(fit[0], 1.0, 1e-9);
    LT_CHECK_NEAR(fit[1], 3.0, 1e-9);
    LT_CHECK_NEAR(fit[3], 6.0, 1e-9);
    lt_table_file_clear(&tf);
}

int main(void)
{
    LT_RUN(restore_selection);
    LT_RUN(clear_clamps);
    LT_RUN(paste_modes);
    LT_RUN(resize_maths);
    LT_RUN(clamp_between);
    LT_RUN(generate_values);
    LT_RUN(table_file_roundtrip_and_fit);
    LT_TEST_MAIN_END
}
