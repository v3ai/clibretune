/* dialogs_ini.c - renderer for INI [UserDefined] dialogs (and std_* panels) as tabs.
 * Field widgets follow the constant type; visibility/enable conditions are
 * re-evaluated every frame through app->expr / app->varenv.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "dialogs.h"
#include "view_kit.h"
#include "feat.h"
#include "dialogs_embed.h"
#include "iniext.h"

#define MAX_DEPTH 5
#define N_STR 8
#define N_GRAPH 6
#define GRAPH_N 240
#define GRAPH_CH 4

typedef struct str_slot { const lt_ini_const *c; char buf[160]; int editing; } str_slot;
typedef struct graph_slot {
    const lt_ini_comp *comp;
    double h[GRAPH_CH][GRAPH_N];
    int n;
    double last_ms;
} graph_slot;

typedef struct ini_state {
    int *map; size_t map_cap;
    str_slot str[N_STR]; int n_str;
    graph_slot gr[N_GRAPH]; int n_gr;
    int close_req;
    lt_embeds emb;                  /* embedded curves/tables/gauges */
    int counts[LT_EMB_KINDS];       /* drawn this frame (tests); [0] = help buttons */
} ini_state;

typedef struct opt_ctx { const lt_ini_const *c; const int *map; } opt_ctx;

static int opt_visible(const lt_ini_const *c, size_t i)
{
    const char *s = lt_ini_bit_option(c, i);
    return s != NULL && s[0] != '\0' && strcmp(s, "INVALID") != 0;
}

static const char *opt_get(void *ud, int i)
{
    opt_ctx *o = (opt_ctx *)ud;
    return lt_ini_bit_option(o->c, (size_t)o->map[i]);
}

/* ---- value access (ECU/tune pages or PC variables) ---- */
static int val_get(lt_app *app, const lt_ini_const *c, double *v)
{
    lt_pagesrc ps = lt_app_pages(app);
    const lt_tune_entry *e;
    *v = 0.0;
    if (c->is_pc_var) {
        e = app->tune != NULL ? lt_tune_get_pc_variable(app->tune, c->name) : NULL;
        if (e != NULL && e->value.kind == LT_TV_SCALAR) { *v = e->value.scalar; return 0; }
        if (e != NULL && e->value.kind == LT_TV_BOOL) { *v = e->value.boolean ? 1.0 : 0.0; return 0; }
        return lt_ini_get(app->ini, &ps, c->name, v) == 0 ? 0 : -1;
    }
    if (c->type == LT_DT_BITS) { return lt_ini_const_get_raw(app->ini, c, &ps, 0, v); }
    return lt_ini_const_get(app->ini, c, &ps, 0, v);
}

static void val_set(lt_app *app, const lt_ini_const *c, double v)
{
    lt_pagesrc ps = lt_app_pages(app);
    int rc;
    double old_v = 0.0;
    int recording = lt_feat_actions_recording(app) && val_get(app, c, &old_v) == 0;
    if (c->is_pc_var) {
        lt_tune_value tv;
        if (app->tune == NULL) { return; }
        lt_tune_value_init(&tv);
        tv.scalar = v;
        lt_tune_set_pc_variable(app->tune, c->name, &tv);
        lt_tune_value_clear(&tv);
        app->modified = 1;
        rc = 0;
    } else {
        if (c->type == LT_DT_BITS) { rc = lt_ini_const_set_raw(app->ini, c, &ps, 0, v); }
        else { rc = lt_ini_const_set(app->ini, c, &ps, 0, v); }
        if (rc == 0) { lt_app_edited(app); }
    }
    if (rc < 0) { lt_app_toast(app, 3, "Cannot write %s: %s", c->name, lt_err_get()); return; }
    if (recording) { lt_feat_actions_note_constant(app, c->name, old_v, v); }
    if (lt_ini_feeds_dynamic_scale(app->ini, c->name)) {
        lt_ini_resolve_dynamic(app->ini, &app->varenv.env);
    }
}

static int cond(lt_app *app, const char *c)
{
    return lt_expr_cache_test(&app->expr, c, &app->varenv.env) ? 1 : 0;
}

/* label text that may be a {expr}; returns a buffer valid until the next call */
static const char *label_text(lt_app *app, const char *raw, char *buf, size_t cap)
{
    char *d;
    if (raw == NULL) { return ""; }
    if (raw[0] != '{') { return raw; }
    d = lt_expr_cache_display(&app->expr, raw, &app->varenv.env);
    if (d == NULL) { return raw; }
    lt_strlcpy(buf, d, cap);
    lt_free(d);
    return buf;
}

static void field_layout(lt_ui *ui, int *w3, int *n)
{
    int avail = lt_ui_unpx(ui, lt_ui_layout_width(ui));
    int cell = lt_ui_cell_mode(ui);
    if (avail >= 460) {
        w3[0] = LT_CLAMP(avail * (cell ? 42 : 46) / 100, 150, cell ? 340 : 400);
        w3[1] = 200; w3[2] = -1; *n = 3;
    } else {
        w3[0] = -3; w3[1] = -2; *n = 2;
    }
    lt_ui_layout_row(ui, *n, w3, cell ? 0 : 30);
}

/* label cell of a field row: uppercase caption, hint icon when the constant has help text (and
 * the "help icons" preference is on; clicking it opens the text), a power badge on constants
 * that need an ECU power cycle, hairline under the whole row (pixel mode); plain label in
 * cell mode */
static void field_label(lt_app *app, const char *label, const lt_ini_const *c, int enabled)
{
    lt_ui *ui = app->ui;
    const lt_theme *t = lt_ui_theme(ui);
    lt_rect r, body;
    char up[160];
    size_t i, n = 0;
    int tw, lw, x, help = c != NULL && c->help != NULL && app->settings.prefs.help_icons;
    int pcyc = c != NULL && lt_ini_needs_power_cycle(app->ini, c->name);
    if (lt_ui_cell_mode(ui)) {
        if (pcyc) {
            char lb[200];
            lt_snprintf(lb, sizeof(lb), "%s [pwr]", label != NULL ? label : "");
            lt_ui_label(ui, lb);
            lt_ui_tooltip(ui, "Takes effect after the ECU is power cycled");
        } else {
            lt_ui_label(ui, label);
        }
        return;
    }
    r = lt_ui_layout_next(ui);
    body = lt_ui_layout_body(ui);
    lt_ui_draw_hline(ui, body.x, body.x + body.w - 1, r.y + r.h - 1, LT_RGBA(255, 255, 255, t->dark ? 16 : 0));
    if (!t->dark) { lt_ui_draw_hline(ui, body.x, body.x + body.w - 1, r.y + r.h - 1, LT_RGBA(0, 0, 0, 20)); }
    for (i = 0; label != NULL && label[i] != '\0' && n + 1 < sizeof(up); i++) {
        char ch = label[i];
        up[n++] = (ch >= 'a' && ch <= 'z') ? (char)(ch - 'a' + 'A') : ch;
    }
    up[n] = '\0';
    lw = r.w - lt_ui_px(ui, 8 + (help ? 22 : 0) + (pcyc ? 22 : 0));
    lt_vk_text(ui, lt_rect_make(r.x + lt_ui_px(ui, 4), r.y, lw, r.h),
               (int)LT_FONT_SANS_SEMIBOLD, 11.0, up, enabled ? t->text2 : t->disabled, 0);
    tw = lt_vk_text_w(ui, (int)LT_FONT_SANS_SEMIBOLD, 11.0, up);
    if (tw > lw) {                  /* elided: full caption as a tooltip, icons after the "..." */
        char tid[180];
        tw = lw;
        lt_snprintf(tid, sizeof(tid), "##flbl%s", label);
        lt_ui_layout_set_rect(ui, lt_rect_make(r.x, r.y, lw + lt_ui_px(ui, 4), r.h));
        lt_ui_label(ui, tid);
        lt_ui_tooltip(ui, label);
    }
    x = r.x + lt_ui_px(ui, 4) + tw + lt_ui_px(ui, 6);
    if (help) {
        lt_rect hr = lt_rect_make(x, r.y, lt_ui_px(ui, 16), r.h);
        lt_point m = lt_ui_mouse(ui);
        int over = m.x >= hr.x && m.x < hr.x + hr.w && m.y >= r.y + r.h / 4 && m.y < r.y + r.h * 3 / 4;
        lt_ui_layout_set_rect(ui, lt_rect_make(hr.x, r.y + r.h / 4, hr.w, r.h / 2));
        if (lt_ui_button_ex(ui, "##fhelp", LT_UI_BTN_FLAT) && c != NULL) { (void)lt_app_help_text(app, label, c->help); }
        lt_vk_icon(ui, hr, LT_ICON_CIRCLE_HELP, 13, 2.0, enabled ? (over ? t->accent_hover : t->accent) : t->disabled);
        x += lt_ui_px(ui, 20);
    }
    if (pcyc) {
        lt_rect pr = lt_rect_make(x, r.y, lt_ui_px(ui, 16), r.h);
        lt_vk_icon(ui, pr, LT_ICON_POWER, 12, 2.0, enabled ? t->warning : t->disabled);
        lt_ui_layout_set_rect(ui, lt_rect_make(pr.x, r.y + r.h / 4, pr.w, r.h / 2));
        lt_ui_label(ui, "");
        lt_ui_tooltip(ui, "Changing this setting takes effect after the ECU is power cycled");
    }
}

static str_slot *str_slot_get(ini_state *st, const lt_ini_const *c)
{
    int i;
    for (i = 0; i < st->n_str; i++) { if (st->str[i].c == c) { return &st->str[i]; } }
    if (st->n_str >= N_STR) { return &st->str[N_STR - 1]; }
    st->str[st->n_str].c = c;
    st->str[st->n_str].editing = 0;
    return &st->str[st->n_str++];
}

static double step_for(int digits)
{
    double s = 1.0;
    int i;
    for (i = 0; i < digits && i < 6; i++) { s /= 10.0; }
    return s;
}

/* the value widget for a constant. readonly draws text only. */
static void value_widget(lt_app *app, ini_state *st, const lt_ini_const *c, int readonly)
{
    lt_ui *ui = app->ui;
    double v;
    char buf[64];
    if (c->type == LT_DT_STRING) {
        str_slot *sl = str_slot_get(st, c);
        int fl;
        lt_pagesrc ps = lt_app_pages(app);
        if (!sl->editing) { lt_ini_const_get_string(app->ini, c, &ps, sl->buf, sizeof(sl->buf)); }
        fl = lt_ui_text(ui, c->name, sl->buf, LT_MIN(sizeof(sl->buf), c->size_bytes + 1),
                        readonly ? LT_UI_TEXT_READONLY : 0);
        sl->editing = lt_ui_last_focused(ui);
        if ((fl & LT_UI_CHANGED) && !readonly) {
            if (lt_ini_const_set_string(app->ini, c, &ps, sl->buf) == 0) { lt_app_edited(app); }
        }
        return;
    }
    if (c->shape.kind != LT_SH_SCALAR) {
        const lt_ini_table *t = lt_ini_find_table(app->ini, c->name);
        if (t != NULL) {
            if (lt_ui_button(ui, "Edit table...")) { lt_app_table_editor_open(app, c->name); }
        } else {
            lt_ui_label_ex(ui, "(array)", lt_ui_theme(ui)->muted, LT_UI_ALIGN_LEFT);
        }
        return;
    }
    if (val_get(app, c, &v) < 0) {
        lt_ui_label_ex(ui, "n/a", lt_ui_theme(ui)->muted, LT_UI_ALIGN_LEFT);
        return;
    }
    if (c->type == LT_DT_BITS) {
        size_t i, n = 0;
        int sel = -1, ch;
        opt_ctx oc;
        if (c->n_bit_options == 0) {
            lt_fmt_double(buf, sizeof(buf), v, 0);
            lt_ui_label(ui, buf);
            return;
        }
        if (st->map_cap < c->n_bit_options) {
            st->map = (int *)lt_realloc(st->map, c->n_bit_options * sizeof(int));
            st->map_cap = c->n_bit_options;
        }
        for (i = 0; i < c->n_bit_options; i++) {
            if (opt_visible(c, i) || (size_t)v == i) {
                if ((size_t)v == i) { sel = (int)n; }
                st->map[n++] = (int)i;
            }
        }
        oc.c = c; oc.map = st->map;
        lt_ui_disabled_begin(ui, readonly);
        ch = lt_ui_combo_cb(ui, c->name, &sel, (int)n, opt_get, &oc);
        lt_ui_disabled_end(ui);
        if (ch && sel >= 0 && sel < (int)n) {
            if (c->name != NULL && lt_pin_is_assignment(c)) {
                lt_pin_conflict pc;
                lt_pagesrc ps = lt_app_pages(app);
                struct lt_pin_pages pp;
                pp.ini = app->ini; pp.src = &ps;
                if (lt_pin_conflict_if_assigning(app->ini, c->name, (size_t)st->map[sel],
                                                 lt_pin_value_from_pages, &pp, &pc) == 1) {
                    lt_app_toast(app, 2, "Pin %s is already used by %lu other setting(s)",
                                 pc.pin_label != NULL ? pc.pin_label : "?", (unsigned long)pc.n);
                    lt_pin_conflict_free(&pc);
                }
            }
            val_set(app, c, (double)st->map[sel]);
        }
        return;
    }
    {
        double lo = c->min, hi = c->max;
        int fl, dg;
        lt_uconv uc;
        lt_uconv_make(&uc, c->units);          /* shown and typed in the preferred unit (G-90) */
        if (!(hi > lo)) { lo = -1e12; hi = 1e12; }
        else { lt_uconv_range(&uc, &lo, &hi); }
        dg = lt_uconv_digits(&uc, c->digits);
        v = lt_uconv_to_disp(&uc, v);
        lt_ui_disabled_begin(ui, readonly);
        fl = lt_ui_number(ui, c->name, &v, lo, hi, step_for(dg), dg);
        lt_ui_disabled_end(ui);
        if ((fl & LT_UI_CHANGED) && !readonly) { val_set(app, c, lt_uconv_from_disp(&uc, v)); }
    }
}

static void units_cell(lt_app *app, const lt_ini_const *c, int has_cell)
{
    char buf[48];
    const char *u;
    lt_uconv uc;
    if (!has_cell) { return; }
    u = c != NULL && c->units != NULL ? label_text(app, c->units, buf, sizeof(buf)) : "";
    if (c != NULL) { lt_uconv_make(&uc, c->units); u = lt_uconv_label(&uc, u); }
    lt_ui_label_ex(app->ui, u, lt_ui_theme(app->ui)->muted, LT_UI_ALIGN_LEFT);
}

static void push_graph(lt_app *app, graph_slot *g, const lt_ini_comp *cm)
{
    double now = lt_ui_now(app->ui), v;
    size_t k;
    int i;
    if (now - g->last_ms < 50.0) { return; }
    g->last_ms = now;
    if (g->n < GRAPH_N) { g->n++; }
    else {
        for (k = 0; k < GRAPH_CH; k++) {
            memmove(&g->h[k][0], &g->h[k][1], (GRAPH_N - 1) * sizeof(double));
        }
    }
    i = g->n - 1;
    for (k = 0; k < GRAPH_CH; k++) {
        v = 0.0;
        if (k < cm->n_channels) { lt_app_channel_disp(app, cm->channels[k], &v, NULL); }
        g->h[k][i] = v;
    }
}

static void draw_graph(lt_app *app, ini_state *st, const lt_ini_comp *cm)
{
    lt_ui *ui = app->ui;
    const lt_theme *t = lt_ui_theme(ui);
    graph_slot *g = NULL;
    lt_rect r;
    int i, w1 = -1, lx, ly;
    size_t k;
    lt_color cols[GRAPH_CH];
    cols[0] = t->primary; cols[1] = t->accent; cols[2] = t->success; cols[3] = t->warning;
    for (i = 0; i < st->n_gr; i++) { if (st->gr[i].comp == cm) { g = &st->gr[i]; } }
    if (g == NULL && st->n_gr < N_GRAPH) {
        g = &st->gr[st->n_gr++];
        memset(g, 0, sizeof(*g));
        g->comp = cm;
    }
    lt_ui_layout_row(ui, 1, &w1, 0);
    if (cm->label != NULL && cm->label[0] != '\0') { lt_ui_label_ex(ui, cm->label, t->text2, LT_UI_ALIGN_LEFT); }
    lt_ui_layout_row(ui, 1, &w1, 8 * 16);
    r = lt_ui_layout_next(ui);
    lt_ui_draw_box(ui, r, t->table_bg, t->border, 0);
    lx = r.x + 6; ly = r.y + 2;
    if (g == NULL) { return; }
    push_graph(app, g, cm);
    lt_ui_request_redraw(ui);
    for (k = 0; k < GRAPH_CH && k < cm->n_channels; k++) {
        double lo = 1e300, hi = -1e300, x0, y0, x1, y1;
        char lab[96];
        for (i = 0; i < g->n; i++) {
            if (g->h[k][i] < lo) { lo = g->h[k][i]; }
            if (g->h[k][i] > hi) { hi = g->h[k][i]; }
        }
        if (hi - lo < 1e-9) { hi = lo + 1.0; }
        for (i = 1; i < g->n; i++) {
            x0 = r.x + 4 + (double)(i - 1) * (r.w - 8) / (GRAPH_N - 1);
            x1 = r.x + 4 + (double)i * (r.w - 8) / (GRAPH_N - 1);
            y0 = r.y + r.h - 4 - (g->h[k][i - 1] - lo) / (hi - lo) * (r.h - 8);
            y1 = r.y + r.h - 4 - (g->h[k][i] - lo) / (hi - lo) * (r.h - 8);
            lt_ui_draw_line(ui, x0, y0, x1, y1, 1.5, cols[k]);
        }
        lt_snprintf(lab, sizeof(lab), "%s %.1f", cm->channels[k], g->n > 0 ? g->h[k][g->n - 1] : 0.0);
        if (lx + lt_ui_text_width(ui, lab) > r.x + r.w - 4) { lx = r.x + 6; ly += lt_ui_row_height(ui) - 4; }
        lt_ui_draw_text(ui, lx, ly, lab, cols[k]);
        lx += lt_ui_text_width(ui, lab) + lt_ui_px(ui, 18);
    }
}

static void draw_dialog(lt_app *app, ini_state *st, const lt_ini_dialog *dlg, int depth);

static const lt_ini_dialog *find_panel(lt_app *app, const char *name)
{
    const lt_ini_dialog *d;
    if (name == NULL) { return NULL; }
    d = lt_ini_find_dialog(app->ini, name);
    if (d == NULL) { d = lt_ini_std_panel(app->ini, name); }
    return d;
}

/* group box with a title around an embedded component (pixel mode: card, cell mode: heading) */
static int titled_begin(lt_app *app, const char *title)
{
    if (title == NULL || title[0] == '\0') { return 1; }
    return lt_ui_group_begin(app->ui, title, 0) ? 2 : 0;
}

static void titled_end(lt_app *app, int g)
{
    if (g == 2) { lt_ui_group_end(app->ui); }
}

/* "panel = name": a dialog, curve, table, indicator panel, readout panel or gauge. Returns 1
 * when something was drawn. */
static int draw_panel_ref(lt_app *app, ini_state *st, const char *name, int enabled, int depth)
{
    lt_ui *ui = app->ui;
    int w1 = -1, g, kind = lt_emb_kind(app, name), drawn = 0;
    lt_ui_layout_row(ui, 1, &w1, 0);
    switch (kind) {
    case LT_EMB_DIALOG: {
        const lt_ini_dialog *sub = find_panel(app, name);
        if (sub == NULL || depth >= MAX_DEPTH) { return 0; }
        /* an untitled dialog (title defaults to its name) gets no titled border, as in TunerStudio */
        g = titled_begin(app, sub->title != NULL && sub->name != NULL && strcmp(sub->title, sub->name) == 0
                              ? NULL : sub->title);
        if (g) { draw_dialog(app, st, sub, depth + 1); titled_end(app, g); }
        st->counts[LT_EMB_DIALOG]++;
        return 1; }
    case LT_EMB_CURVE: {
        const lt_ini_curve *c = lt_ini_find_curve(app->ini, name);
        g = titled_begin(app, c->title != NULL ? c->title : c->name);
        if (g) { drawn = lt_emb_curve(app, &st->emb, c, enabled); titled_end(app, g); }
        break; }
    case LT_EMB_TABLE: {
        const lt_ini_table *t = lt_ini_find_table(app->ini, name);
        g = titled_begin(app, t->title != NULL ? t->title : t->name);
        if (g) { drawn = lt_emb_table(app, &st->emb, t, enabled); titled_end(app, g); }
        break; }
    case LT_EMB_IND:
        drawn = lt_emb_indicators(app, lt_ini_find_ind_panel(app->ini, name));
        break;
    case LT_EMB_READOUT:
        drawn = lt_emb_readouts(app, lt_ini_find_readout_panel(app->ini, name));
        break;
    case LT_EMB_GAUGE:
        drawn = lt_emb_gauge(app, &st->emb, lt_ini_find_gauge(app->ini, name));
        break;
    default:
        return 0;
    }
    if (drawn) { st->counts[kind]++; }
    return drawn;
}

static void draw_comp(lt_app *app, ini_state *st, const lt_ini_dialog *dlg, size_t idx, int depth)
{
    lt_ui *ui = app->ui;
    const lt_theme *t = lt_ui_theme(ui);
    const lt_ini_comp *cm = &dlg->comps[idx];
    const lt_ini_const *c = NULL;
    char lb[160];
    const char *label;
    int w3[3], n, w1 = -1, enabled = 1;
    double v;

    if (!cond(app, cm->vis_cond)) { return; }
    enabled = cond(app, cm->en_cond);
    if (cm->name != NULL) { c = lt_ini_find_const(app->ini, cm->name); }
    label = label_text(app, cm->label != NULL ? cm->label : (c != NULL ? c->name : ""), lb, sizeof(lb));
    lt_ui_push_id_int(ui, (int)idx);
    lt_ui_disabled_begin(ui, !enabled);
    switch (cm->kind) {
    case LT_COMP_LABEL:
        lt_ui_layout_row(ui, 1, &w1, 0);
        if (label[0] == '#') {
            if (lt_ui_cell_mode(ui)) {
                lt_ui_layout_space(ui, 4);
                lt_ui_heading(ui, label + 1);
            } else {
                lt_rect hr;
                lt_ui_layout_row(ui, 1, &w1, 30);
                hr = lt_ui_layout_next(ui);
                lt_vk_caption(ui, lt_rect_make(hr.x + lt_ui_px(ui, 4), hr.y + lt_ui_px(ui, 6), hr.w - lt_ui_px(ui, 8), hr.h - lt_ui_px(ui, 6)), label + 1, t->text2);
                lt_ui_draw_hline(ui, hr.x, hr.x + hr.w - 1, hr.y + hr.h - 2, t->border_subtle);
            }
        } else if (label[0] == '!') {
            lt_ui_label_wrap(ui, label + 1, t->warning);
        } else if (label[0] != '\0') {
            lt_ui_label_wrap(ui, label, t->text2);
        } else {
            lt_ui_layout_space(ui, 6);
        }
        break;
    case LT_COMP_PANEL:
        (void)draw_panel_ref(app, st, cm->name, enabled, depth);
        break;
    case LT_COMP_FIELD:
    case LT_COMP_DISPLAYONLY:
        field_layout(ui, w3, &n);
        field_label(app, label, c, enabled);
        if (c != NULL && c->help != NULL) { lt_ui_tooltip(ui, c->help); }
        if (c != NULL) {
            value_widget(app, st, c, cm->kind == LT_COMP_DISPLAYONLY);
            if (c->help != NULL) { lt_ui_tooltip(ui, c->help); }
            units_cell(app, c, n == 3);
        } else {
            if (cm->name != NULL && lt_app_channel_disp(app, cm->name, &v, NULL)) {
                char b[48];
                lt_fmt_double(b, sizeof(b), v, 3);
                lt_ui_label(ui, b);
            } else { lt_ui_label_ex(ui, "-", t->muted, LT_UI_ALIGN_LEFT); }
            units_cell(app, NULL, n == 3);
        }
        break;
    case LT_COMP_CHECKBOX:
        lt_ui_layout_row(ui, 1, &w1, 0);
        if (c != NULL && val_get(app, c, &v) == 0) {
            int on = ((v != 0.0) ? 1 : 0) ^ (cm->flip ? 1 : 0);
            if (lt_ui_checkbox(ui, label, &on)) { val_set(app, c, (double)(on ^ (cm->flip ? 1 : 0))); }
            if (c->help != NULL) { lt_ui_tooltip(ui, c->help); }
        } else {
            lt_ui_label_ex(ui, label, t->muted, LT_UI_ALIGN_LEFT);
        }
        break;
    case LT_COMP_SLIDER:
        field_layout(ui, w3, &n);
        field_label(app, label, c, enabled);
        if (c != NULL && c->shape.kind == LT_SH_SCALAR && c->type != LT_DT_BITS && val_get(app, c, &v) == 0) {
            double lo = c->min, hi = c->max;
            lt_uconv uc;
            lt_uconv_make(&uc, c->units);
            if (!(hi > lo)) { lo = 0; hi = 100; }
            lt_uconv_range(&uc, &lo, &hi);
            v = lt_uconv_to_disp(&uc, v);
            if (lt_ui_slider(ui, c->name, &v, lo, hi, lt_uconv_digits(&uc, c->digits))) {
                val_set(app, c, lt_uconv_from_disp(&uc, v));
            }
            units_cell(app, c, n == 3);
        } else {
            lt_ui_label_ex(ui, "n/a", t->muted, LT_UI_ALIGN_LEFT);
        }
        break;
    case LT_COMP_SETTINGSEL: {
        int sel = -1, ch;
        size_t o, a;
        field_layout(ui, w3, &n);
        field_label(app, label, NULL, enabled);
        /* match: every assignment equals the current value */
        for (o = 0; o < cm->n_options; o++) {
            int all = cm->options[o].n_assigns > 0;
            for (a = 0; a < cm->options[o].n_assigns && all; a++) {
                const lt_ini_const *ac = lt_ini_find_const(app->ini, cm->options[o].assigns[a].name);
                double cur;
                if (ac == NULL || val_get(app, ac, &cur) < 0 ||
                    fabs(cur - cm->options[o].assigns[a].value) > 1e-6) { all = 0; }
            }
            if (all) { sel = (int)o; break; }
        }
        {
            const char *items[32] = { NULL };
            size_t m = cm->n_options < 32 ? cm->n_options : 32;
            for (o = 0; o < m; o++) { items[o] = cm->options[o].label != NULL ? cm->options[o].label : ""; }
            ch = lt_ui_combo(ui, "sel", &sel, items, (int)m);
        }
        if (ch && sel >= 0 && (size_t)sel < cm->n_options) {
            for (a = 0; a < cm->options[sel].n_assigns; a++) {
                const lt_ini_const *ac = lt_ini_find_const(app->ini, cm->options[sel].assigns[a].name);
                if (ac != NULL) { val_set(app, ac, cm->options[sel].assigns[a].value); }
            }
        }
        break; }
    case LT_COMP_RUNTIMEVALUE: {
        char b[64];
        field_layout(ui, w3, &n);
        field_label(app, label, NULL, enabled);
        if (cm->name != NULL && lt_app_channel_disp(app, cm->name, &v, NULL)) { lt_fmt_double(b, sizeof(b), v, 2); }
        else { lt_strlcpy(b, "-", sizeof(b)); }
        lt_ui_label_ex(ui, b, t->accent, LT_UI_ALIGN_LEFT);
        if (n == 3) {
            const lt_ini_channel *ch = cm->name != NULL ? lt_ini_find_channel(app->ini, cm->name) : NULL;
            lt_uconv uc;
            lt_uconv_make(&uc, ch != NULL ? ch->units : NULL);
            lt_ui_label_ex(ui, lt_uconv_label(&uc, ch != NULL && ch->units != NULL ? ch->units : ""), t->muted,
                           LT_UI_ALIGN_LEFT);
        }
        break; }
    case LT_COMP_LIVEGRAPH:
        draw_graph(app, st, cm);
        break;
    case LT_COMP_TABLE: {
        const lt_ini_table *tb = cm->name != NULL ? lt_ini_find_table(app->ini, cm->name) : NULL;
        if (tb != NULL) {
            (void)draw_panel_ref(app, st, cm->name, enabled, depth);
        } else {
            lt_ui_layout_row(ui, 1, &w1, 0);
            lt_ui_label_ex(ui, cm->name != NULL ? cm->name : "?", t->muted, LT_UI_ALIGN_LEFT);
        }
        break; }
    case LT_COMP_INDICATOR: {
        int on = lt_expr_cache_f64(&app->expr, cm->expression, &app->varenv.env, 0.0) != 0.0;
        char l1[96], l2[96];
        const char *txt = label_text(app, on ? cm->label_on : cm->label_off, l1, sizeof(l1));
        lt_rect r;
        LT_UNUSED(l2);
        lt_ui_layout_row(ui, 1, &w1, 0);
        r = lt_ui_layout_next(ui);
        lt_ui_draw_box(ui, r, on ? t->success_bg : t->surface, on ? t->success : t->border, 3);
        lt_ui_draw_text_in(ui, r, txt, on ? t->success : t->muted, LT_UI_ALIGN_CENTER);
        break; }
    case LT_COMP_CMDBUTTON: {
        int online = (app->conn == LT_APP_ONLINE || app->conn == LT_APP_DEMO) && app->proto != NULL;
        lt_ui_layout_row(ui, 1, &w1, 0);
        if (lt_ui_button_ex(ui, label, enabled ? 0 : LT_UI_BTN_DISABLED) && enabled) {
            const lt_ini_cmd *cmd = cm->name != NULL ? lt_ini_find_command(app->ini, cm->name) : NULL;
            if (cmd == NULL) { lt_app_toast(app, 3, "Unknown command %s", cm->name != NULL ? cm->name : "?"); }
            else if (!online) { lt_app_toast(app, 2, "Connect to an ECU to run %s", label); }
            else if (lt_proto_run_command(app->proto, cmd) < 0) {
                lt_app_toast(app, 3, "%s failed: %s", label, lt_err_get());
            } else {
                lt_app_toast(app, 1, "%s sent", label);
                if (cm->close == LT_CLOSE_ALWAYS || cm->close == LT_CLOSE_IF_ENABLED) { st->close_req = 1; }
            }
        }
        break; }
    case LT_COMP_GAUGE: {
        const lt_ini_gauge *gg = cm->name != NULL ? lt_ini_find_gauge(app->ini, cm->name) : NULL;
        if (gg != NULL && lt_emb_gauge(app, &st->emb, gg)) { st->counts[LT_EMB_GAUGE]++; }
        break; }
    default:
        break;
    }
    lt_ui_disabled_end(ui);
    lt_ui_pop_id(ui);
}

static int is_xaxis(const lt_ini_dialog *d)
{
    return d->layout_hint != NULL && lt_str_istarts(d->layout_hint, "xaxis");
}

static int is_border(const lt_ini_dialog *d)
{
    return d->layout_hint != NULL && lt_stricmp(d->layout_hint, "border") == 0;
}

/* a component that can stand as a column: an embedded panel of any kind or a gauge */
static int columnable(lt_app *app, const lt_ini_comp *cm)
{
    if (cm->kind == LT_COMP_GAUGE) { return cm->name != NULL && lt_ini_find_gauge(app->ini, cm->name) != NULL; }
    return cm->kind == LT_COMP_PANEL && lt_emb_kind(app, cm->name) != LT_EMB_NONE;
}

/* draw groups of components side by side: col[k] lists n_in[k] component indices */
#define MAX_COLS 4
#define COL_CAP 64
static void draw_columns(lt_app *app, ini_state *st, const lt_ini_dialog *dlg, size_t col[][COL_CAP],
                         const int *n_in, const int *weight, int ncols, int depth)
{
    lt_ui *ui = app->ui;
    int ws[MAX_COLS], k, n = 0, idx[MAX_COLS], j;
    for (k = 0; k < ncols; k++) {
        int vis = 0;
        for (j = 0; j < n_in[k]; j++) {
            const lt_ini_comp *cm = &dlg->comps[col[k][j]];
            if (cond(app, cm->vis_cond)) { vis = 1; }
        }
        if (vis) { ws[n] = weight[k]; idx[n++] = k; }
    }
    if (n == 0) { return; }
    lt_ui_layout_row(ui, n, ws, 0);
    for (k = 0; k < n; k++) {
        lt_ui_push_id_int(ui, 1000 + idx[k]);
        lt_ui_layout_column_begin(ui);
        for (j = 0; j < n_in[idx[k]]; j++) { draw_comp(app, st, dlg, col[idx[k]][j], depth); }
        lt_ui_layout_column_end(ui);
        lt_ui_pop_id(ui);
    }
}

/* border layout: North, then West | Center | East side by side (when wide enough), then South */
static void draw_border(lt_app *app, ini_state *st, const lt_ini_dialog *dlg, int depth)
{
    size_t (*col)[COL_CAP];
    int n_in[5] = { 0, 0, 0, 0, 0 }, k, j, wide, used = 0;
    static const int weight[3] = { -2, -3, -2 };
    size_t i;
    /* 0 North, 1 West, 2 Center, 3 East, 4 South */
    col = (size_t (*)[COL_CAP])lt_calloc(5, sizeof(*col));
    for (i = 0; i < dlg->n_comps; i++) {
        const char *pos = dlg->comps[i].position;
        int r = 2;
        if (pos != NULL) {
            if (lt_stricmp(pos, "North") == 0) { r = 0; }
            else if (lt_stricmp(pos, "West") == 0) { r = 1; }
            else if (lt_stricmp(pos, "East") == 0) { r = 3; }
            else if (lt_stricmp(pos, "South") == 0) { r = 4; }
        }
        if (n_in[r] < COL_CAP) { col[r][n_in[r]++] = i; }
    }
    for (k = 1; k <= 3; k++) { if (n_in[k] > 0) { used++; } }
    wide = lt_ui_unpx(app->ui, lt_ui_layout_width(app->ui)) >= 700 && used >= 2;
    for (j = 0; j < n_in[0]; j++) { draw_comp(app, st, dlg, col[0][j], depth); }
    if (wide) {
        draw_columns(app, st, dlg, &col[1], &n_in[1], weight, 3, depth);
    } else {
        for (k = 1; k <= 3; k++) {
            for (j = 0; j < n_in[k]; j++) { draw_comp(app, st, dlg, col[k][j], depth); }
        }
    }
    for (j = 0; j < n_in[4]; j++) { draw_comp(app, st, dlg, col[4][j], depth); }
    lt_free(col);
}

static void draw_dialog(lt_app *app, ini_state *st, const lt_ini_dialog *dlg, int depth)
{
    lt_ui *ui = app->ui;
    size_t i = 0, j;
    lt_ui_push_id(ui, dlg->name != NULL ? dlg->name : "dlg");
    if (is_border(dlg) && depth < MAX_DEPTH) {
        draw_border(app, st, dlg, depth);
        lt_ui_pop_id(ui);
        return;
    }
    while (i < dlg->n_comps) {
        if (is_xaxis(dlg) && columnable(app, &dlg->comps[i]) && depth < MAX_DEPTH) {
            size_t (*col)[COL_CAP] = (size_t (*)[COL_CAP])lt_calloc(MAX_COLS, sizeof(*col));
            int n_in[MAX_COLS] = { 0, 0, 0, 0 }, ws[MAX_COLS] = { 0, 0, 0, 0 }, n = 0;
            for (j = i; j < dlg->n_comps && columnable(app, &dlg->comps[j]) && n < MAX_COLS; j++) {
                col[n][0] = j; n_in[n] = 1; ws[n] = -1; n++;
            }
            i = j;
            draw_columns(app, st, dlg, col, n_in, ws, n, depth);
            lt_free(col);
            continue;
        }
        draw_comp(app, st, dlg, i, depth);
        i++;
    }
    lt_ui_pop_id(ui);
}

/* the dialog has its own title (an untitled one carries its name as title) */
static int titled(const lt_ini_dialog *d)
{
    return d != NULL && d->title != NULL && d->title[0] != '\0' &&
           (d->name == NULL || strcmp(d->title, d->name) != 0);
}

static void ini_dialog_draw(lt_app *app, lt_app_tab *tab, lt_rect r)
{
    lt_ui *ui = app->ui;
    const lt_theme *t = lt_ui_theme(ui);
    const lt_ini_dialog *dlg;
    ini_state *st = (ini_state *)tab->data;
    int w1 = -1;
    LT_UNUSED(r);
    if (st == NULL) {
        st = (ini_state *)lt_calloc(1, sizeof(*st));
        tab->data = st;
    }
    st->close_req = 0;
    memset(st->counts, 0, sizeof(st->counts));
    if (app->ini == NULL) { return; }
    dlg = find_panel(app, tab->target);
    lt_ui_layout_row(ui, 1, &w1, -1);
    if (lt_ui_scroll_begin(ui, "inidlg")) {
        if (lt_ui_cell_mode(ui)) {
            int wh[2];
            wh[0] = -1; wh[1] = 8 * 8;
            lt_ui_layout_row(ui, dlg != NULL && dlg->help_topic != NULL ? 2 : 1, dlg != NULL && dlg->help_topic != NULL ? wh : &w1, 0);
            if (dlg == NULL || dlg->help_topic == NULL) { lt_ui_heading(ui, titled(dlg) ? dlg->title : tab->title); }
            else { lt_ui_label_ex(ui, titled(dlg) ? dlg->title : tab->title, t->text, LT_UI_ALIGN_LEFT); }
            if (dlg != NULL && dlg->help_topic != NULL) {
                st->counts[0]++;
                if (lt_ui_button(ui, "Help")) { (void)lt_app_help_open(app, dlg->help_topic); }
            }
        } else {
            lt_rect hr;
            char ttl[200];
            int tw;
            lt_ui_layout_row(ui, 1, &w1, 50);
            hr = lt_ui_layout_next(ui);
            lt_snprintf(ttl, sizeof(ttl), "%s", titled(dlg) ? dlg->title : tab->title);
            if (dlg != NULL && dlg->name != NULL && strcmp(dlg->name, ttl) != 0) {
                size_t k = strlen(ttl);
                lt_snprintf(ttl + k, sizeof(ttl) - k, " (%s)", dlg->name);
            }
            tw = lt_vk_text_w(ui, (int)LT_FONT_SANS_SEMIBOLD, 17.0, ttl);
            if (tw > hr.w - 40) { tw = hr.w - 40; }
            lt_vk_text(ui, lt_rect_make(hr.x + lt_ui_px(ui, 20), hr.y + lt_ui_px(ui, 6), hr.w - lt_ui_px(ui, 40), lt_ui_px(ui, 30)), (int)LT_FONT_SANS_SEMIBOLD, 17.0,
                       ttl, t->text, 1);
            lt_ui_draw_hline(ui, hr.x + (hr.w - tw) / 2 - 4, hr.x + (hr.w + tw) / 2 + 4, hr.y + 38, t->border_strong);
            if (dlg != NULL && dlg->help_topic != NULL) {     /* topicHelp: help button at the right */
                st->counts[0]++;
                lt_ui_layout_set_rect(ui, lt_rect_make(hr.x + hr.w - lt_ui_px(ui, 84), hr.y + lt_ui_px(ui, 8), lt_ui_px(ui, 72), lt_ui_px(ui, 26)));
                if (lt_ui_button_ex(ui, "Help##dlghelp", LT_UI_BTN_FLAT)) { (void)lt_app_help_open(app, dlg->help_topic); }
                lt_ui_tooltip(ui, dlg->help_topic);
            }
        }
        lt_ui_layout_row(ui, 1, &w1, 0);
        if (dlg == NULL) {
            lt_ui_label_ex(ui, "This dialog is not defined in the loaded ECU definition.",
                           t->warning, LT_UI_ALIGN_LEFT);
        } else {
            draw_dialog(app, st, dlg, 0);
        }
        lt_ui_scroll_end(ui);
    }
    if (st->close_req) { lt_app_tab_close(app, tab); }
}

static void ini_dialog_close(lt_app *app, lt_app_tab *tab)
{
    ini_state *st = (ini_state *)tab->data;
    LT_UNUSED(app);
    if (st != NULL) { lt_emb_free(&st->emb); lt_free(st->map); lt_free(st); }
    tab->data = NULL;
}

int lt_dlg_ini_counts(lt_app *app, const char *name, int *counts)
{
    lt_app_tab *tab = lt_app_tab_find(app, "dialog", name);
    const ini_state *st = tab != NULL ? (const ini_state *)tab->data : NULL;
    if (st == NULL) { return -1; }
    memcpy(counts, st->counts, sizeof(st->counts));
    return 0;
}

void lt_dlg_register_ini(lt_app *app)
{
    lt_app_tab_kind tk;
    tk.kind = "dialog"; tk.draw = ini_dialog_draw; tk.close = ini_dialog_close;
    lt_app_register_tab_kind(app, &tk);
}
