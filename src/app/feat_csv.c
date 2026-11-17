/* feat_csv.c - tune export / import as CSV (G-16): File -> Export Tune as CSV... / Import Tune
 * from CSV...  Format and logic: tune/tune_csv.c.  The import writes through the live page source
 * (ECU image when connected, tune pages offline) and reports clamped values and bad rows.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <string.h>
#include "feat.h"
#include "../tune/tune_csv.h"

#define FD_CSV_EXPORT (LT_APP_FD_USER0 + 46)
#define FD_CSV_IMPORT (LT_APP_FD_USER0 + 47)
#define CSV_FILTERS "CSV Files|*.csv|All Files|*"

typedef struct csv_state {
    lt_csv_import res;
    char title[64], summary[200];
    int  ready;                     /* a result is waiting for its dialog */
} csv_state;

static void st_free(lt_app *app, void *p)
{
    LT_UNUSED(app);
    lt_csv_import_free(&((csv_state *)p)->res);
}

static csv_state *get(lt_app *app)
{
    return (csv_state *)lt_ext_state(app, "csv", sizeof(csv_state), st_free);
}

/* ---- scripted entry points (tests, screenshots) ---- */
int lt_feat_csv_export(lt_app *app, const char *path, size_t *count)
{
    lt_pagesrc ps;
    if (app->ini == NULL || app->tune == NULL) { lt_err_set("Definition not loaded"); return -1; }
    ps = lt_app_pages(app);
    return lt_tune_csv_export_file(app->ini, &ps, path, count);
}

int lt_feat_csv_import(lt_app *app, const char *path, size_t *imported)
{
    lt_pagesrc ps;
    csv_state *s = get(app);
    int rc;
    if (app->ini == NULL || app->tune == NULL) { lt_err_set("No tune loaded"); return -1; }
    ps = lt_app_pages(app);
    lt_csv_import_free(&s->res);
    rc = lt_tune_csv_import_file(app->ini, &ps, path, &s->res);
    if (rc < 0) { return rc; }
    if (s->res.imported > 0) {
        (void)lt_tune_capture_from_pages(app->tune, app->ini);
        lt_app_edited(app);
    }
    if (imported != NULL) { *imported = s->res.imported; }
    lt_snprintf(s->title, sizeof(s->title), "Import Tune from CSV");
    lt_snprintf(s->summary, sizeof(s->summary),
                "Imported %lu constant%s from %s. %lu value%s clamped to the INI range, %lu unknown name%s skipped.",
                (unsigned long)s->res.imported, s->res.imported == 1 ? "" : "s", lt_path_base(path),
                (unsigned long)s->res.clamped, s->res.clamped == 1 ? "" : "s",
                (unsigned long)s->res.unknown, s->res.unknown == 1 ? "" : "s");
    s->ready = 1;
    return 0;
}

/* ---- menu + file dialog results ---- */
static void do_export(lt_app *app)
{
    lt_app_file_dialog(app, FD_CSV_EXPORT, LT_UI_FILE_SAVE, NULL, "tune.csv", CSV_FILTERS);
}

static void do_import(lt_app *app)
{
    lt_app_file_dialog(app, FD_CSV_IMPORT, LT_UI_FILE_OPEN, NULL, "", CSV_FILTERS);
}

static void tick(lt_app *app)
{
    const char *p;
    p = lt_app_file_result(app, FD_CSV_EXPORT);
    if (p != NULL) {
        size_t n = 0;
        if (lt_feat_csv_export(app, p, &n) == 0) {
            lt_app_toast(app, 1, "Exported %lu constants to %s", (unsigned long)n, lt_path_base(p));
        } else {
            lt_app_toast(app, 3, "CSV export failed: %s", lt_err_get());
        }
    }
    p = lt_app_file_result(app, FD_CSV_IMPORT);
    if (p != NULL) {
        char path[512];
        lt_strlcpy(path, p, sizeof(path));
        if (lt_feat_csv_import(app, path, NULL) == 0) {
            (void)lt_app_dialog_push(app, "csv_result", "", "Import Tune from CSV");
        } else {
            lt_app_toast(app, 3, "CSV import failed: %s", lt_err_get());
        }
    }
}

/* ---- result dialog ---- */
static void res_draw(lt_app *app, lt_app_dialog *d)
{
    lt_ui *ui = app->ui;
    const lt_theme *t = lt_ui_theme(ui);
    csv_state *s = get(app);
    int open = d->open, w1 = -1, w2[2];
    size_t i;
    if (lt_ui_modal_begin(ui, "Import Tune from CSV", 640, 340, &open)) {
        lt_ui_layout_row(ui, 1, &w1, 0);
        lt_ui_label_wrap(ui, s->summary, s->res.imported > 0 ? t->success : t->warning);
        if (s->res.n_messages > 0) {
            lt_ui_layout_row(ui, 1, &w1, 0);
            lt_ui_label_ex(ui, "Warnings", t->text, LT_UI_ALIGN_LEFT);
            lt_ui_layout_row(ui, 1, &w1, 190);
            if (lt_ui_scroll_begin(ui, "csvmsgs")) {
                for (i = 0; i < s->res.n_messages; i++) {
                    lt_ui_layout_row(ui, 1, &w1, 0);
                    lt_ui_label_ex(ui, s->res.messages[i], t->warning, LT_UI_ALIGN_LEFT);
                }
                lt_ui_scroll_end(ui);
            }
        }
        w2[0] = -1; w2[1] = 110;
        lt_ui_layout_row(ui, 2, w2, 0);
        lt_ui_label(ui, "");
        if (lt_ui_button_ex(ui, "OK", LT_UI_BTN_DEFAULT)) { open = 0; }
        lt_ui_modal_end(ui);
    }
    d->open = open;
}

void lt_feat_csv_register(lt_app_ext *reg)
{
    lt_app_dialog_kind dk;
    lt_ext_add_menu_item(reg, LT_EXT_MENU_FILE, "E&xport Tune as CSV...", NULL, do_export, lt_ext_pred_ini);
    lt_ext_add_menu_item(reg, LT_EXT_MENU_FILE, "Import Tune from &CSV...", NULL, do_import, lt_ext_pred_ini);
    lt_ext_add_tick(reg, tick);
    dk.kind = "csv_result"; dk.draw = res_draw; dk.close = NULL;
    lt_ext_add_dialog_kind(reg, &dk);
}
