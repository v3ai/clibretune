/* tables_io.c - load / save a single table as a TunerStudio .table file.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <string.h>
#include "tables_priv.h"

void lt_tabs_io_free(lt_tabs_io_dlg *d)
{
    if (d->have) { lt_table_file_clear(&d->tf); }
    d->have = 0;
}

int lt_tabs_io_open_file(lt_tabs_io_dlg *d, const char *path, size_t cols, size_t rows)
{
    size_t len = 0;
    char *txt = lt_file_read(path, &len);
    int rc;
    if (txt == NULL) {
        lt_err_set("cannot read '%s'", path);
        return LT_EIO;
    }
    lt_tabs_io_free(d);
    rc = lt_table_file_parse(txt, len, &d->tf);
    lt_free(txt);
    if (rc < 0) { return rc; }
    d->have = 1;
    d->match = d->tf.cols == cols && d->tf.rows == rows;
    d->use_axes = d->match;
    lt_strlcpy(d->path, path, sizeof(d->path));
    d->open = 1;
    return 0;
}

int lt_tabs_io_fit(const lt_table_file *tf, const double *xb, size_t cols, const double *yb,
                   size_t rows, double *out_z)
{
    if (tf->cols == cols && tf->rows == rows) {
        memcpy(out_z, tf->z, cols * rows * sizeof(double));
        return 0;
    }
    return lt_tbl_rebin(tf->x, tf->cols, tf->y, tf->rows, tf->z, xb, cols, yb, rows, LT_TRUE,
                        out_z);
}

int lt_tabs_io_draw(lt_app *app, lt_tabs_io_dlg *d, size_t cols, size_t rows)
{
    lt_ui *ui = app->ui;
    const lt_theme *th = lt_ui_theme(ui);
    int w1 = -1, wb[3], ok = 0;
    char b[200];
    if (!d->open || !d->have) { return 0; }
    if (!lt_ui_modal_begin(ui, "Load table file", 480, 230, &d->open)) { return 0; }
    lt_ui_layout_row(ui, 1, &w1, 0);
    lt_ui_label_ex(ui, d->path, th->muted, LT_UI_ALIGN_LEFT);
    lt_snprintf(b, sizeof(b), "File: %lu x %lu     Table: %lu x %lu", (unsigned long)d->tf.cols,
                (unsigned long)d->tf.rows, (unsigned long)cols, (unsigned long)rows);
    lt_ui_label(ui, b);
    if (d->match) {
        lt_ui_checkbox(ui, "Also load the axis values from the file", &d->use_axes);
    } else {
        lt_ui_label_ex(ui, "The sizes differ: values are interpolated onto this table's axes.",
                       th->warning, LT_UI_ALIGN_LEFT);
        d->use_axes = 0;
    }
    wb[0] = -1; wb[1] = 90; wb[2] = 90;
    lt_ui_layout_row(ui, 3, wb, 0);
    lt_ui_label(ui, "");
    if (lt_ui_button_ex(ui, "Load", LT_UI_BTN_DEFAULT)) { ok = 1; d->open = 0; }
    if (lt_ui_button(ui, "Cancel")) { d->open = 0; }
    lt_ui_modal_end(ui);
    return ok;
}

int lt_tabs_io_save(const char *path, const char *xname, const char *yname, const double *x,
                    size_t cols, const double *y, size_t rows, const double *z)
{
    char *xml = lt_table_file_write(xname, yname, x, cols, y, rows, z);
    int rc;
    if (xml == NULL) {
        lt_err_set("cannot encode the table");
        return LT_EDATA;
    }
    rc = lt_file_write(path, xml, strlen(xml));
    lt_free(xml);
    if (rc < 0) { lt_err_set("cannot write '%s'", path); }
    return rc;
}
