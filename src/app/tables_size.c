/* tables_size.c - "Set table size" dialog for resizable (dynamic) tables.
 * The maths and the page writes live in lt_table_resize (tableops).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <string.h>
#include "tables_priv.h"

int lt_tabs_size_begin(lt_app *app, const lt_ini_table *t, lt_tabs_size_dlg *d)
{
    lt_pagesrc src = lt_app_pages(app);
    int rc = lt_table_size_info(app->ini, t, &src, &d->info);
    if (rc < 0) { return rc; }
    d->cols = (double)d->info.active_cols;
    d->rows = (double)d->info.active_rows;
    d->open = 1;
    return 0;
}

int lt_tabs_size_draw(lt_app *app, const lt_ini_table *t, lt_tabs_size_dlg *d)
{
    lt_ui *ui = app->ui;
    const lt_theme *th = lt_ui_theme(ui);
    int w1 = -1, w2[2], wb[3], applied = 0, rc = 0, bad;
    size_t cols, rows;
    char b[160];
    if (!d->open) { return 0; }
    if (!lt_ui_modal_begin(ui, "Set table size", 440, 230, &d->open)) { return 0; }
    lt_ui_layout_row(ui, 1, &w1, 0);
    lt_ui_label_ex(ui, "Resizes this table and any table sharing its size. Axes are spaced",
                   th->text2, LT_UI_ALIGN_LEFT);
    lt_ui_label_ex(ui, "evenly between the current end points; Z is interpolated.", th->text2,
                   LT_UI_ALIGN_LEFT);
    w2[0] = 150; w2[1] = -1;
    lt_ui_layout_row(ui, 2, w2, 0);
    lt_snprintf(b, sizeof(b), "Columns (%lu - %lu)", (unsigned long)d->info.min_cols,
                (unsigned long)d->info.max_cols);
    lt_ui_label(ui, b);
    lt_ui_number(ui, "sz_cols", &d->cols, (double)d->info.min_cols, (double)d->info.max_cols, 1.0, 0);
    lt_snprintf(b, sizeof(b), "Rows (%lu - %lu)", (unsigned long)d->info.min_rows,
                (unsigned long)d->info.max_rows);
    lt_ui_label(ui, b);
    lt_ui_number(ui, "sz_rows", &d->rows, (double)d->info.min_rows, (double)d->info.max_rows, 1.0, 0);
    cols = d->cols > 0.0 ? (size_t)(d->cols + 0.5) : 0;
    rows = d->rows > 0.0 ? (size_t)(d->rows + 0.5) : 0;
    bad = lt_table_size_allows(&d->info, cols, rows) < 0;
    lt_ui_layout_row(ui, 1, &w1, 0);
    if (bad) {
        lt_ui_label_ex(ui, lt_err_get(), th->error, LT_UI_ALIGN_LEFT);
    } else {
        lt_snprintf(b, sizeof(b), "%lu x %lu = %lu of %lu cells", (unsigned long)cols,
                    (unsigned long)rows, (unsigned long)(cols * rows),
                    (unsigned long)d->info.max_elements);
        lt_ui_label_ex(ui, b, th->text2, LT_UI_ALIGN_LEFT);
    }
    wb[0] = -1; wb[1] = 90; wb[2] = 90;
    lt_ui_layout_row(ui, 3, wb, 0);
    lt_ui_label(ui, "");
    if (lt_ui_button_ex(ui, "Apply", bad ? (LT_UI_BTN_DEFAULT | LT_UI_BTN_DISABLED) : LT_UI_BTN_DEFAULT)) {
        lt_pagesrc dst = lt_app_pages(app);
        rc = lt_table_resize(app->ini, t, &dst, cols, rows);
        if (rc < 0) {
            lt_app_toast(app, 3, "Resize failed: %s", lt_err_get());
        } else {
            lt_app_edited(app);
            applied = 1;
            d->open = 0;
        }
    }
    if (lt_ui_button(ui, "Cancel")) { d->open = 0; }
    lt_ui_modal_end(ui);
    return rc < 0 ? rc : applied;
}
