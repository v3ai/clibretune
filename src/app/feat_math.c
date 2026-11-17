/* feat_math.c - Math Channels editor (G-60): user-defined computed log channels (name, units,
 * expression) validated with the INI expression engine and stored per project
 * (math_channels.json).  Tools -> Math Channels...   (dialog kind "math-channels")
 * The channels are appended to loaded logs in the viewer and to finished recordings.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <string.h>
#include "feat.h"
#include "feat_log.h"

#define MC_MAX 32

typedef struct mc_row {
    char name[48], units[16], expr[256];
    char err[160];
    int  checked, ok;
} mc_row;

typedef struct mc_dlg {
    mc_row *rows; int n, sel;
    char msg[160]; int msg_err;
    int pick;                    /* channel combo */
    char **chans; size_t n_chans;
} mc_dlg;

static void row_check(mc_row *r)
{
    lt_math_channel ch;
    lt_math_hooks h = LT_MATH_HOOKS_INI;
    lt_math_set_hooks(&h);
    r->checked = 1;
    r->err[0] = '\0';
    if (r->name[0] == '\0') { lt_strlcpy(r->err, "Name is required", sizeof(r->err)); r->ok = 0; return; }
    if (r->expr[0] == '\0') { lt_strlcpy(r->err, "Expression is required", sizeof(r->err)); r->ok = 0; return; }
    memset(&ch, 0, sizeof(ch));
    ch.name = r->name; ch.units = r->units; ch.expression = r->expr;
    r->ok = lt_math_compile(&ch, r->err, sizeof(r->err)) == 0;
    if (!r->ok && r->err[0] == '\0') { lt_strlcpy(r->err, "Invalid expression", sizeof(r->err)); }
    if (ch.ast != NULL && h.free_expr != NULL) { h.free_expr(ch.ast); }
}

static void rows_from_set(mc_dlg *s, lt_math_set *ms)
{
    size_t i;
    s->rows = (mc_row *)lt_calloc(MC_MAX, sizeof(mc_row));
    for (i = 0; i < ms->n && i < MC_MAX; i++) {
        lt_strlcpy(s->rows[i].name, ms->ch[i].name != NULL ? ms->ch[i].name : "", sizeof(s->rows[i].name));
        lt_strlcpy(s->rows[i].units, ms->ch[i].units != NULL ? ms->ch[i].units : "", sizeof(s->rows[i].units));
        lt_strlcpy(s->rows[i].expr, ms->ch[i].expression != NULL ? ms->ch[i].expression : "",
                   sizeof(s->rows[i].expr));
        s->n++;
    }
}

/* validate every row; returns the number of invalid ones */
static int check_all(mc_dlg *s)
{
    int i, bad = 0, j;
    for (i = 0; i < s->n; i++) {
        row_check(&s->rows[i]);
        for (j = 0; j < i; j++) {
            if (strcmp(s->rows[i].name, s->rows[j].name) == 0 && s->rows[i].ok) {
                lt_strlcpy(s->rows[i].err, "Duplicate name", sizeof(s->rows[i].err));
                s->rows[i].ok = 0;
            }
        }
        if (!s->rows[i].ok) { bad++; }
    }
    return bad;
}

static int do_save(lt_app *app, mc_dlg *s)
{
    lt_math_set *ms = lt_logx_math(app);
    int i, bad = check_all(s);
    if (bad > 0) {
        lt_snprintf(s->msg, sizeof(s->msg), "%d channel%s invalid: fix before saving", bad, bad == 1 ? " is" : "s are");
        s->msg_err = 1;
        return -1;
    }
    lt_math_set_free(ms);
    ms->ch = (lt_math_channel *)lt_calloc((size_t)(s->n > 0 ? s->n : 1), sizeof(lt_math_channel));
    ms->n = (size_t)s->n;
    for (i = 0; i < s->n; i++) {
        ms->ch[i].name = lt_strdup(s->rows[i].name);
        ms->ch[i].units = lt_strdup(s->rows[i].units);
        ms->ch[i].expression = lt_strdup(s->rows[i].expr);
    }
    if (lt_logx_math_save(app) < 0) {
        lt_snprintf(s->msg, sizeof(s->msg), "Save failed: %s", lt_err_get());
        s->msg_err = 1;
        return -1;
    }
    lt_snprintf(s->msg, sizeof(s->msg), "Saved %d math channel%s", s->n, s->n == 1 ? "" : "s");
    s->msg_err = 0;
    return 0;
}

static const char *row_get(void *ud, int i) { return ((mc_dlg *)ud)->rows[i].name[0] ? ((mc_dlg *)ud)->rows[i].name : "(unnamed)"; }
static const char *chan_get(void *ud, int i) { return ((mc_dlg *)ud)->chans[i]; }

static void mc_draw(lt_app *app, lt_app_dialog *d)
{
    lt_ui *ui = app->ui;
    const lt_theme *t = lt_ui_theme(ui);
    mc_dlg *s = (mc_dlg *)d->data;
    int open = d->open, w1 = -1, wc[2];
    if (s == NULL) {
        char **u = NULL;
        s = (mc_dlg *)lt_calloc(1, sizeof(*s));
        d->data = s;
        rows_from_set(s, lt_logx_math(app));
        if (app->ini != NULL) { s->n_chans = lt_datalog_channels_from_ini(app->ini, &s->chans, &u); }
        if (u != NULL) { lt_strv_free(u, s->n_chans); }
        check_all(s);
    }
    if (lt_ui_modal_begin(ui, "Math Channels", 860, 560, &open)) {
        int wl[2];
        mc_row *r = s->n > 0 ? &s->rows[s->sel] : NULL;
        wl[0] = 230; wl[1] = -1;
        lt_ui_layout_row(ui, 2, wl, -(lt_ui_cell_mode(ui) ? 18 : 40));
        lt_ui_layout_column_begin(ui);
        wc[0] = 105; wc[1] = -1;
        lt_ui_layout_row(ui, 2, wc, 0);
        if (lt_ui_button_ex(ui, "Add", s->n < MC_MAX ? LT_UI_BTN_PRIMARY : LT_UI_BTN_DISABLED) && s->n < MC_MAX) {
            memset(&s->rows[s->n], 0, sizeof(mc_row));
            lt_snprintf(s->rows[s->n].name, sizeof(s->rows[s->n].name), "math%d", s->n + 1);
            s->sel = s->n++;
            s->msg[0] = '\0';
        }
        if (lt_ui_button_ex(ui, "Delete", r != NULL ? LT_UI_BTN_DANGER : LT_UI_BTN_DISABLED) && r != NULL) {
            memmove(&s->rows[s->sel], &s->rows[s->sel + 1], (size_t)(s->n - s->sel - 1) * sizeof(mc_row));
            s->n--;
            if (s->sel >= s->n && s->sel > 0) { s->sel--; }
            check_all(s);
        }
        lt_ui_layout_row(ui, 1, &w1, -1);
        if (s->n > 0) { (void)lt_ui_listbox_cb(ui, "mcl", &s->sel, s->n, row_get, s); }
        else { lt_ui_label_wrap(ui, "No math channels. Add one to compute a new log channel from existing ones.", t->muted); }
        lt_ui_layout_column_end(ui);

        lt_ui_layout_column_begin(ui);
        if (r != NULL) {
            int wf[2], was_ok = r->ok;
            wf[0] = 80; wf[1] = -1;
            lt_ui_layout_row(ui, 2, wf, 0);
            lt_ui_label(ui, "Name");
            if (lt_ui_text(ui, "mcn", r->name, sizeof(r->name), 0) & LT_UI_CHANGED) { row_check(r); }
            lt_ui_label(ui, "Units");
            if (lt_ui_text(ui, "mcu", r->units, sizeof(r->units), 0) & LT_UI_CHANGED) { row_check(r); }
            lt_ui_label(ui, "Expression");
            if (lt_ui_text(ui, "mce", r->expr, sizeof(r->expr), 0) & LT_UI_CHANGED) { row_check(r); }
            LT_UNUSED(was_ok);
            if (s->n_chans > 0) {
                int wp[3];
                wp[0] = 80; wp[1] = 260; wp[2] = 90;
                lt_ui_layout_row(ui, 3, wp, 0);
                lt_ui_label(ui, "Channel");
                (void)lt_ui_combo_cb(ui, "mcp", &s->pick, (int)s->n_chans, chan_get, s);
                if (lt_ui_button(ui, "Insert") && s->pick >= 0 && s->pick < (int)s->n_chans) {
                    size_t len = strlen(r->expr);
                    lt_strlcpy(r->expr + len, s->chans[s->pick], sizeof(r->expr) - len);
                    row_check(r);
                }
            }
            lt_ui_layout_row(ui, 1, &w1, 0);
            if (r->checked && r->ok) { lt_ui_label_ex(ui, "Expression is valid", t->success, LT_UI_ALIGN_LEFT); }
            else if (r->checked) { lt_ui_label_wrap(ui, r->err, t->error); }
            lt_ui_label_wrap(ui, "Operators: + - * / % & | ^ << >> < > == != && || ! ?:   "
                             "Functions: min max abs round sqrt log exp sin cos tan if isNaN.  "
                             "Variables are the log's channels (and other math channels).", t->muted);
            lt_ui_label_wrap(ui, "Examples:  rpm * 0.00595   (afr - 14.7) / 14.7   "
                             "map > 100 ? 1 : 0", t->text2);
        } else {
            lt_ui_layout_row(ui, 1, &w1, 0);
            lt_ui_label(ui, "");
        }
        lt_ui_layout_column_end(ui);

        {
            int wb[4];
            wb[0] = 110; wb[1] = 110; wb[2] = -1; wb[3] = 110;
            lt_ui_layout_row(ui, 4, wb, 0);
            if (lt_ui_button(ui, "Validate all")) {
                int bad = check_all(s);
                lt_snprintf(s->msg, sizeof(s->msg), bad == 0 ? "All channels are valid" : "%d invalid", bad);
                s->msg_err = bad > 0;
            }
            if (lt_ui_button_ex(ui, "Save", LT_UI_BTN_PRIMARY)) { (void)do_save(app, s); }
            lt_ui_label_ex(ui, s->msg, s->msg_err ? t->error : t->success, LT_UI_ALIGN_LEFT);
            if (lt_ui_button(ui, "Close")) { open = 0; }
        }
        lt_ui_modal_end(ui);
    }
    d->open = open;
}

static void mc_close(lt_app *app, lt_app_dialog *d)
{
    mc_dlg *s = (mc_dlg *)d->data;
    LT_UNUSED(app);
    if (s != NULL) {
        lt_free(s->rows);
        if (s->chans != NULL) { lt_strv_free(s->chans, s->n_chans); }
        lt_free(s);
    }
    d->data = NULL;
}

static void act_math(lt_app *app) { (void)lt_app_dialog_push(app, "math-channels", "", "Math Channels"); }

void lt_feat_math_register(lt_app_ext *reg)
{
    lt_app_dialog_kind dk;
    dk.kind = "math-channels"; dk.draw = mc_draw; dk.close = mc_close;
    lt_ext_add_dialog_kind(reg, &dk);
    lt_ext_add_menu_item(reg, LT_EXT_MENU_TOOLS, "&Math Channels...", NULL, act_math, lt_ext_pred_ini);
}
