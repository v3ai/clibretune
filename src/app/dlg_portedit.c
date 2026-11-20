/* dlg_portedit.c - TunerStudio style programmable on/off output editor ("std_port_edit").
 *
 * The INI's [PortEditor] maps one array constant per property (one element per port):
 * enabled, output channel (offset/size in the realtime block), operator, threshold,
 * hysteresis, an optional second condition joined by and/or, the power-on value and the
 * value written while the condition holds.  The editor shows the ports on the left and the
 * condition of the selected port on the right; every change is written through the normal
 * page path (lt_app_pages + lt_app_edited: dirty, sent to the ECU RAM, burn) with an undo
 * stack local to the tab.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <string.h>
#include <math.h>
#include "iniext.h"
#include "view_kit.h"

/* ------------------------------------------------------------------ */
/* model                                                               */
static const lt_ini_const *pconst(const lt_ini *ini, const char *name)
{
    return name != NULL ? lt_ini_find_const(ini, name) : NULL;
}

static int arr_get(const lt_ini *ini, const char *name, const lt_pagesrc *src, int i, double *v)
{
    const lt_ini_const *c = pconst(ini, name);
    *v = 0.0;
    if (c == NULL || i < 0 || (size_t)i >= lt_shape_count(&c->shape)) { return -1; }
    return lt_ini_const_get(ini, c, src, (size_t)i, v) == 0 ? 0 : -1;
}

static int arr_set(const lt_ini *ini, const char *name, lt_pagesrc *src, int i, double v)
{
    const lt_ini_const *c = pconst(ini, name);
    double cur = 0.0;
    if (c == NULL || i < 0 || (size_t)i >= lt_shape_count(&c->shape)) { return 0; }
    if (lt_ini_const_get(ini, c, src, (size_t)i, &cur) == 0 && fabs(cur - v) < 1e-9) { return 0; }
    return lt_ini_const_set(ini, c, src, (size_t)i, v) == 0 ? 1 : -1;
}

int lt_port_count(const lt_ini *ini, const lt_ini_port_editor *pe)
{
    const lt_ini_const *c;
    size_t n = 0;
    if (pe == NULL) { return 0; }
    c = pconst(ini, pe->enabled_const);
    if (c == NULL) { c = pconst(ini, pe->offset_const); }
    if (c == NULL) { c = pconst(ini, pe->op_const); }
    if (c != NULL) { n = lt_shape_count(&c->shape); }
    if (n == 0 || (c != NULL && c->shape.kind == LT_SH_SCALAR && pe->n_port_names > 1)) { n = pe->n_port_names; }
    return (int)(n > LT_PORT_MAX ? LT_PORT_MAX : n);
}

void lt_port_name(const lt_ini_port_editor *pe, int i, char *buf, size_t cap)
{
    if (pe != NULL && i >= 0 && (size_t)i < pe->n_port_names && pe->port_names[i] != NULL &&
        pe->port_names[i][0] != '\0') {
        lt_strlcpy(buf, pe->port_names[i], cap);
    } else {
        lt_snprintf(buf, cap, "Port %d", i + 1);
    }
}

int lt_port_channel_ok(const lt_ini_channel *ch)
{
    return ch != NULL && ch->expression == NULL && ch->type != LT_DT_BITS && lt_ini_channel_size(ch) > 0;
}

int lt_port_ops(const lt_ini_port_editor *pe, const char **labels, int cap)
{
    static const char *const def[] = { "<", "=", ">", "&" };
    int i, n = 0;
    if (pe != NULL && pe->n_op_labels > 0) {
        for (i = 0; i < (int)pe->n_op_labels && n < cap; i++) { labels[n++] = pe->op_labels[i]; }
        return n;
    }
    for (i = 0; i < 4 && n < cap; i++) { labels[n++] = def[i]; }
    return n;
}

int lt_port_conns(const lt_ini_port_editor *pe, const char **labels, int cap)
{
    static const char *const def[] = { " ", "&", "|" };
    int i, n = 0;
    if (pe != NULL && pe->n_conn_labels > 0) {
        for (i = 0; i < (int)pe->n_conn_labels && n < cap; i++) { labels[n++] = pe->conn_labels[i]; }
        return n;
    }
    for (i = 0; i < 3 && n < cap; i++) { labels[n++] = def[i]; }
    return n;
}

/* stored operator/connector -> character; bits constants hold the index of the label */
static int code_get(const lt_ini *ini, const char *cname, const lt_pagesrc *src, int port,
                    const char **labels, int n)
{
    const lt_ini_const *c = pconst(ini, cname);
    double v;
    if (c == NULL || arr_get(ini, cname, src, port, &v) < 0) { return 0; }
    if (c->type == LT_DT_BITS) {
        int k = (int)v;
        return k >= 0 && k < n && labels[k] != NULL ? (unsigned char)labels[k][0] : 0;
    }
    return (int)v;
}

static int code_set(const lt_ini *ini, const char *cname, lt_pagesrc *src, int port, int ch,
                    const char **labels, int n)
{
    const lt_ini_const *c = pconst(ini, cname);
    int k;
    if (c == NULL) { return 0; }
    if (c->type == LT_DT_BITS) {
        for (k = 0; k < n; k++) {
            if (labels[k] != NULL && (unsigned char)labels[k][0] == ch) { return arr_set(ini, cname, src, port, (double)k); }
        }
        return 0;
    }
    return arr_set(ini, cname, src, port, (double)ch);
}

static int find_channel(const lt_ini *ini, double off, double size)
{
    size_t i;
    int loose = -1;
    for (i = 0; i < ini->n_channels; i++) {
        const lt_ini_channel *ch = ini->channels[i];
        if (!lt_port_channel_ok(ch) || fabs((double)ch->offset - off) > 0.5) { continue; }
        if (size <= 0.0 || fabs((double)lt_ini_channel_size(ch) - size) < 0.5) { return (int)i; }
        if (loose < 0) { loose = (int)i; }
    }
    return loose;
}

static void cond_read(const lt_ini *ini, const lt_pagesrc *src, int port, const char *offc,
                      const char *sizec, const char *opc, const char *thc, const char *hyc,
                      const lt_ini_port_editor *pe, lt_port_cond *out)
{
    const char *ops[16];
    int nops = lt_port_ops(pe, ops, 16);
    double off = 0.0, size = 0.0, th = 0.0, hy = 0.0;
    const lt_ini_channel *ch;
    out->channel = -1;
    out->op = code_get(ini, opc, src, port, ops, nops);
    if (arr_get(ini, offc, src, port, &off) == 0) {
        if (arr_get(ini, sizec, src, port, &size) < 0) { size = 0.0; }
        out->channel = find_channel(ini, off, size);
    }
    (void)arr_get(ini, thc, src, port, &th);
    (void)arr_get(ini, hyc, src, port, &hy);
    ch = out->channel >= 0 ? ini->channels[out->channel] : NULL;
    out->threshold = ch != NULL ? th * ch->scale + ch->translate : th;
    out->hysteresis = ch != NULL ? hy * ch->scale : hy;
}

static int cond_write(const lt_ini *ini, lt_pagesrc *src, int port, const char *offc,
                      const char *sizec, const char *opc, const char *thc, const char *hyc,
                      const lt_ini_port_editor *pe, const lt_port_cond *c)
{
    const char *ops[16];
    int nops = lt_port_ops(pe, ops, 16), n = 0, r;
    const lt_ini_channel *ch = c->channel >= 0 && (size_t)c->channel < ini->n_channels
                               ? ini->channels[c->channel] : NULL;
    double sc = ch != NULL && ch->scale != 0.0 ? ch->scale : 1.0, tr = ch != NULL ? ch->translate : 0.0;
    if (ch != NULL) {
        r = arr_set(ini, offc, src, port, (double)ch->offset); if (r < 0) { return r; } n += r;
        r = arr_set(ini, sizec, src, port, (double)lt_ini_channel_size(ch)); if (r < 0) { return r; } n += r;
    }
    if (c->op != 0) { r = code_set(ini, opc, src, port, c->op, ops, nops); if (r < 0) { return r; } n += r; }
    r = arr_set(ini, thc, src, port, floor((c->threshold - tr) / sc + 0.5)); if (r < 0) { return r; } n += r;
    r = arr_set(ini, hyc, src, port, floor(c->hysteresis / sc + 0.5)); if (r < 0) { return r; } n += r;
    return n;
}

int lt_port_read(const lt_ini *ini, const lt_ini_port_editor *pe, const lt_pagesrc *src,
                 int port, lt_port_cfg *out)
{
    const char *cn[8];
    int ncn = lt_port_conns(pe, cn, 8);
    double v;
    memset(out, 0, sizeof(*out));
    out->c1.channel = out->c2.channel = -1;
    if (pe == NULL || port < 0 || port >= lt_port_count(ini, pe)) { return -1; }
    out->enabled = arr_get(ini, pe->enabled_const, src, port, &v) == 0 && v != 0.0;
    out->can_id = arr_get(ini, pe->can_id_const, src, port, &v) == 0 ? (int)v : 0;
    out->power_on = arr_get(ini, pe->power_on_const, src, port, &v) == 0 && v != 0.0;
    out->trigger = arr_get(ini, pe->trigger_const, src, port, &v) == 0 && v != 0.0;
    out->connector = code_get(ini, pe->connector_const, src, port, cn, ncn);
    if (out->connector == 0) { out->connector = ' '; }
    cond_read(ini, src, port, pe->offset_const, pe->size_const, pe->op_const, pe->threshold_const,
              pe->hysteresis_const, pe, &out->c1);
    cond_read(ini, src, port, pe->offset2_const, pe->size2_const, pe->op2_const, pe->threshold2_const,
              pe->hysteresis2_const, pe, &out->c2);
    return 0;
}

int lt_port_write(const lt_ini *ini, const lt_ini_port_editor *pe, lt_pagesrc *src, int port,
                  const lt_port_cfg *cfg)
{
    const char *cn[8];
    int ncn = lt_port_conns(pe, cn, 8), n = 0, r;
    if (pe == NULL || port < 0 || port >= lt_port_count(ini, pe)) { return -1; }
    r = arr_set(ini, pe->enabled_const, src, port, cfg->enabled ? 1.0 : 0.0); if (r < 0) { return r; } n += r;
    r = arr_set(ini, pe->can_id_const, src, port, (double)cfg->can_id); if (r < 0) { return r; } n += r;
    r = arr_set(ini, pe->power_on_const, src, port, cfg->power_on ? 1.0 : 0.0); if (r < 0) { return r; } n += r;
    r = arr_set(ini, pe->trigger_const, src, port, cfg->trigger ? 1.0 : 0.0); if (r < 0) { return r; } n += r;
    r = code_set(ini, pe->connector_const, src, port, cfg->connector != 0 ? cfg->connector : ' ', cn, ncn);
    if (r < 0) { return r; } n += r;
    r = cond_write(ini, src, port, pe->offset_const, pe->size_const, pe->op_const, pe->threshold_const,
                   pe->hysteresis_const, pe, &cfg->c1);
    if (r < 0) { return r; } n += r;
    if (pe->offset2_const != NULL || pe->op2_const != NULL) {
        r = cond_write(ini, src, port, pe->offset2_const, pe->size2_const, pe->op2_const,
                       pe->threshold2_const, pe->hysteresis2_const, pe, &cfg->c2);
        if (r < 0) { return r; } n += r;
    }
    return n;
}

/* ------------------------------------------------------------------ */
/* tab                                                                 */
#define PE_UNDO 32
typedef struct pe_undo { int port; lt_port_cfg cfg; } pe_undo;
typedef struct pe_tab {
    int sel;
    int *chmap; int n_ch;           /* combo index -> channel index */
    char **names;                   /* port list labels */
    int n_names;
    pe_undo undo[PE_UNDO]; int n_undo;
} pe_tab;

static const lt_ini_port_editor *pe_of(lt_app *app, const lt_app_tab *tab)
{
    const lt_ini_port_editor *pe = lt_ini_find_port_editor(app->ini, tab->target);
    if (pe == NULL && app->ini->n_port_editors > 0) { pe = &app->ini->port_editors[0]; }
    return pe;
}

static void pe_build(lt_app *app, pe_tab *st)
{
    size_t i;
    int n = 0;
    lt_free(st->chmap);
    st->chmap = (int *)lt_calloc(app->ini->n_channels + 1, sizeof(int));
    for (i = 0; i < app->ini->n_channels; i++) {
        if (lt_port_channel_ok(app->ini->channels[i])) { st->chmap[n++] = (int)i; }
    }
    st->n_ch = n;
}

static const char *ch_get(void *ud, int i)
{
    lt_app *app = (lt_app *)((void **)ud)[0];
    const pe_tab *st = (const pe_tab *)((void **)ud)[1];
    if (i < 0 || i >= st->n_ch) { return "(none)"; }
    return app->ini->channels[st->chmap[i]]->name;
}

static int pe_apply(lt_app *app, pe_tab *st, const lt_ini_port_editor *pe, int port,
                    const lt_port_cfg *old, const lt_port_cfg *now)
{
    lt_pagesrc ps = lt_app_pages(app);
    int n = lt_port_write(app->ini, pe, &ps, port, now);
    if (n < 0) { lt_app_toast(app, 3, "Cannot write the port settings: %s", lt_err_get()); return n; }
    if (n > 0) {
        if (old != NULL) {
            if (st->n_undo == PE_UNDO) { memmove(&st->undo[0], &st->undo[1], (PE_UNDO - 1) * sizeof(pe_undo)); st->n_undo--; }
            st->undo[st->n_undo].port = port;
            st->undo[st->n_undo].cfg = *old;
            st->n_undo++;
        }
        lt_app_edited(app);
    }
    return n;
}

static const char *op_word(int op)
{
    switch (op) {
    case '<': return "is below";
    case '>': return "is above";
    case '=': return "equals";
    case '&': return "has bits of";
    case '!': return "differs from";
    default: return "?";
    }
}

/* live state of one condition (with the hysteresis the firmware applies) */
static int cond_live(lt_app *app, const lt_port_cond *c, int *known)
{
    double v = 0.0;
    *known = 0;
    if (c->channel < 0 || app->rt == NULL || (size_t)c->channel >= app->ini->n_channels) { return 0; }
    if (!lt_ini_rt_get(app->rt, app->ini->channels[c->channel]->name, &v)) { return 0; }
    *known = 1;
    switch (c->op) {
    case '<': return v < c->threshold;
    case '>': return v > c->threshold;
    case '=': return fabs(v - c->threshold) < 1e-9;
    case '&': return ((long)v & (long)c->threshold) != 0;
    case '!': return fabs(v - c->threshold) >= 1e-9;
    default: return 0;
    }
}

/* one condition row: channel, operator, threshold, hysteresis. Returns 1 when changed. */
static int cond_row(lt_app *app, pe_tab *st, const lt_ini_port_editor *pe, const char *id,
                    lt_port_cond *c, int has_hyst)
{
    lt_ui *ui = app->ui;
    const char *ops[16];
    int nops = lt_port_ops(pe, ops, 16), sel = -1, osel = -1, k, ch = 0, w[4];
    void *ud[2];
    const lt_ini_channel *chn = c->channel >= 0 ? app->ini->channels[c->channel] : NULL;
    int dg = 0;
    double v;
    ud[0] = app; ud[1] = st;
    for (k = 0; k < st->n_ch; k++) { if (st->chmap[k] == c->channel) { sel = k; } }
    for (k = 0; k < nops; k++) { if (ops[k] != NULL && (unsigned char)ops[k][0] == c->op) { osel = k; } }
    if (chn != NULL && chn->scale > 0.0 && chn->scale < 1.0) { dg = (int)ceil(-log10(chn->scale) - 1e-9); }
    lt_ui_push_id(ui, id);
    w[0] = -3; w[1] = 70; w[2] = -2; w[3] = -2;
    lt_ui_layout_row(ui, has_hyst ? 4 : 3, w, 0);
    lt_ui_label_ex(ui, "Channel", lt_ui_theme(ui)->muted, LT_UI_ALIGN_LEFT);
    lt_ui_label_ex(ui, "Operator", lt_ui_theme(ui)->muted, LT_UI_ALIGN_LEFT);
    lt_ui_label_ex(ui, "Threshold", lt_ui_theme(ui)->muted, LT_UI_ALIGN_LEFT);
    if (has_hyst) { lt_ui_label_ex(ui, "Hysteresis", lt_ui_theme(ui)->muted, LT_UI_ALIGN_LEFT); }
    if (lt_ui_combo_cb(ui, "ch", &sel, st->n_ch, ch_get, ud) && sel >= 0 && sel < st->n_ch) {
        c->channel = st->chmap[sel]; ch = 1;
    }
    if (lt_ui_combo(ui, "op", &osel, ops, nops) && osel >= 0 && osel < nops && ops[osel] != NULL) {
        c->op = (unsigned char)ops[osel][0]; ch = 1;
    }
    v = c->threshold;
    if (lt_ui_number(ui, "th", &v, -1e9, 1e9, dg > 0 ? 1.0 / pow(10.0, dg) : 1.0, dg) & LT_UI_CHANGED) {
        c->threshold = v; ch = 1;
    }
    if (has_hyst) {
        v = c->hysteresis;
        if (lt_ui_number(ui, "hy", &v, 0.0, 1e9, dg > 0 ? 1.0 / pow(10.0, dg) : 1.0, dg) & LT_UI_CHANGED) {
            c->hysteresis = v; ch = 1;
        }
    }
    lt_ui_pop_id(ui);
    return ch;
}

static void cond_text(lt_app *app, const lt_port_cond *c, char *buf, size_t cap)
{
    char th[32];
    const lt_ini_channel *ch = c->channel >= 0 ? app->ini->channels[c->channel] : NULL;
    lt_fmt_double(th, sizeof(th), c->threshold, 3);
    lt_snprintf(buf, cap, "%s %s %s%s%s", ch != NULL ? ch->name : "(no channel)", op_word(c->op), th,
                ch != NULL && ch->units != NULL && ch->units[0] != '\0' ? " " : "",
                ch != NULL && ch->units != NULL ? ch->units : "");
}

static void pe_editor(lt_app *app, pe_tab *st, const lt_ini_port_editor *pe)
{
    lt_ui *ui = app->ui;
    const lt_theme *t = lt_ui_theme(ui);
    lt_pagesrc ps = lt_app_pages(app);
    lt_port_cfg cfg, old;
    const char *cn[8];
    static const char *const vals[2] = { "0 (off)", "1 (on)" };
    int ncn = lt_port_conns(pe, cn, 8), w1 = -1, w2[2], changed = 0, k, csel = 0, has2;
    char name[96], line[300], a[160], b[160];
    if (lt_port_read(app->ini, pe, &ps, st->sel, &cfg) < 0) {
        lt_ui_layout_row(ui, 1, &w1, 0);
        lt_ui_label_ex(ui, "Port settings are not available (page not loaded).", t->warning, LT_UI_ALIGN_LEFT);
        return;
    }
    old = cfg;
    lt_port_name(pe, st->sel, name, sizeof(name));
    lt_ui_layout_row(ui, 1, &w1, 0);
    lt_ui_heading(ui, name);
    w2[0] = 170; w2[1] = -1;
    lt_ui_layout_row(ui, 1, &w1, 0);
    if (lt_ui_checkbox(ui, "Enabled", &cfg.enabled)) { changed = 1; }
    lt_ui_disabled_begin(ui, !cfg.enabled);
    if (pe->can_id_const != NULL) {
        double v = (double)cfg.can_id;
        lt_ui_layout_row(ui, 2, w2, 0);
        lt_ui_label(ui, "CAN ID");
        if (lt_ui_number(ui, "canid", &v, 0.0, 15.0, 1.0, 0) & LT_UI_CHANGED) { cfg.can_id = (int)v; changed = 1; }
    }
    if (pe->power_on_const != NULL) {
        int s = cfg.power_on ? 1 : 0;
        lt_ui_layout_row(ui, 2, w2, 0);
        lt_ui_label(ui, "Power on value");
        if (lt_ui_combo(ui, "pon", &s, vals, 2)) { cfg.power_on = s; changed = 1; }
    }
    if (pe->trigger_const != NULL) {
        int s = cfg.trigger ? 1 : 0;
        lt_ui_layout_row(ui, 2, w2, 0);
        lt_ui_label(ui, "Active value");
        if (lt_ui_combo(ui, "trig", &s, vals, 2)) { cfg.trigger = s; changed = 1; }
    }
    lt_ui_layout_space(ui, 6);
    lt_ui_layout_row(ui, 1, &w1, 0);
    if (lt_ui_group_begin(ui, "Condition", 0)) {
        if (cond_row(app, st, pe, "c1", &cfg.c1, pe->hysteresis_const != NULL)) { changed = 1; }
        has2 = pe->offset2_const != NULL || pe->op2_const != NULL;
        if (pe->connector_const != NULL && has2) {
            static const char *const words[3] = { "(single condition)", "AND", "OR" };
            const char *items[8] = { NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL };
            for (k = 0; k < ncn; k++) {
                items[k] = cn[k][0] == '&' ? words[1] : (cn[k][0] == '|' ? words[2] : words[0]);
                if ((unsigned char)cn[k][0] == cfg.connector) { csel = k; }
            }
            lt_ui_layout_row(ui, 2, w2, 0);
            lt_ui_label(ui, "Second condition");
            if (lt_ui_combo(ui, "conn", &csel, items, ncn) && csel >= 0 && csel < ncn) {
                cfg.connector = (unsigned char)cn[csel][0]; changed = 1;
            }
            if (cfg.connector == '&' || cfg.connector == '|') {
                if (cond_row(app, st, pe, "c2", &cfg.c2, pe->hysteresis2_const != NULL)) { changed = 1; }
            }
        }
        lt_ui_group_end(ui);
    }
    lt_ui_disabled_end(ui);
    /* summary + live state */
    cond_text(app, &cfg.c1, a, sizeof(a));
    if (cfg.connector == '&' || cfg.connector == '|') {
        cond_text(app, &cfg.c2, b, sizeof(b));
        lt_snprintf(line, sizeof(line), "%s is set to %d when %s %s %s; otherwise %d.", name, cfg.trigger, a,
                    cfg.connector == '&' ? "and" : "or", b, cfg.trigger ? 0 : 1);
    } else {
        lt_snprintf(line, sizeof(line), "%s is set to %d when %s; otherwise %d.", name, cfg.trigger, a,
                    cfg.trigger ? 0 : 1);
    }
    lt_ui_layout_space(ui, 6);
    lt_ui_layout_row(ui, 1, &w1, 0);
    lt_ui_label_wrap(ui, cfg.enabled ? line : "This output is disabled.", t->text2);
    if (cfg.enabled && (app->conn == LT_APP_ONLINE || app->conn == LT_APP_DEMO)) {
        int k1, k2, s1 = cond_live(app, &cfg.c1, &k1), s2 = cond_live(app, &cfg.c2, &k2), on;
        on = cfg.connector == '&' ? (s1 && s2) : (cfg.connector == '|' ? (s1 || s2) : s1);
        lt_ui_layout_row(ui, 1, &w1, 0);
        if (k1) {
            lt_snprintf(line, sizeof(line), "Live: condition %s (output %d)", on ? "met" : "not met",
                        on ? cfg.trigger : !cfg.trigger);
            lt_ui_label_ex(ui, line, on ? t->success : t->muted, LT_UI_ALIGN_LEFT);
        } else {
            lt_ui_label_ex(ui, "Live: channel not available", t->muted, LT_UI_ALIGN_LEFT);
        }
        lt_ui_request_redraw(ui);
    }
    if (changed) { (void)pe_apply(app, st, pe, st->sel, &old, &cfg); }
}

static void pe_draw(lt_app *app, lt_app_tab *tab, lt_rect r)
{
    lt_ui *ui = app->ui;
    const lt_theme *t = lt_ui_theme(ui);
    pe_tab *st = (pe_tab *)tab->data;
    const lt_ini_port_editor *pe;
    int w1 = -1, w2[2], i, n, sel;
    char tb[200];
    LT_UNUSED(r);
    if (app->ini == NULL) { return; }
    if (st == NULL) {
        st = (pe_tab *)lt_calloc(1, sizeof(*st));
        tab->data = st;
        pe_build(app, st);
    }
    pe = pe_of(app, tab);
    lt_ui_layout_row(ui, 1, &w1, 0);
    if (pe == NULL) {
        lt_ui_heading(ui, "Output Port Settings");
        lt_ui_label_wrap(ui, "The loaded ECU definition has no [PortEditor] section, so there are no "
                         "programmable outputs to edit.", t->muted);
        return;
    }
    n = lt_port_count(app->ini, pe);
    if (st->n_names != n) {
        for (i = 0; i < st->n_names; i++) { lt_free(st->names[i]); }
        lt_free(st->names);
        st->names = (char **)lt_calloc((size_t)n + 1, sizeof(char *));
        st->n_names = n;
        for (i = 0; i < n; i++) { st->names[i] = (char *)lt_calloc(1, 112); }
    }
    {   /* list labels with the enabled state */
        lt_pagesrc ps = lt_app_pages(app);
        for (i = 0; i < n; i++) {
            char nm[96];
            double v = 0.0;
            int on = arr_get(app->ini, pe->enabled_const, &ps, i, &v) == 0 && v != 0.0;
            lt_port_name(pe, i, nm, sizeof(nm));
            lt_snprintf(st->names[i], 112, "%s %s", on ? "[x]" : "[ ]", nm);
        }
    }
    w2[0] = -1; w2[1] = 110;
    lt_ui_layout_row(ui, pe->help_topic != NULL ? 2 : 1, pe->help_topic != NULL ? w2 : &w1, 0);
    lt_snprintf(tb, sizeof(tb), "%s", pe->label != NULL ? pe->label : "Output Port Settings");
    if (pe->help_topic != NULL) {           /* heading takes a full row: title and Help side by side */
        lt_ui_label_ex(ui, tb, t->text, LT_UI_ALIGN_LEFT);
        if (lt_ui_button(ui, "Help")) { (void)lt_app_help_open(app, pe->help_topic); }
    } else {
        lt_ui_heading(ui, tb);
    }
    if (pe->enable_cond != NULL && !lt_expr_cache_test(&app->expr, pe->enable_cond, &app->varenv.env)) {
        lt_ui_layout_row(ui, 1, &w1, 0);
        lt_ui_label_ex(ui, "The port editor is disabled by the current settings.", t->warning, LT_UI_ALIGN_LEFT);
    }
    w2[0] = lt_ui_cell_mode(ui) ? 30 * 8 : 260; w2[1] = -1;
    lt_ui_layout_row(ui, 2, w2, -1);
    lt_ui_layout_column_begin(ui);
    {
        int wb[2];
        lt_ui_layout_row(ui, 1, &w1, -(lt_ui_cell_mode(ui) ? 17 : 33));
        sel = st->sel;
        if (lt_ui_listbox(ui, "ports", &sel, (const char *const *)st->names, n) & LT_UI_CHANGED) { st->sel = sel; }
        wb[0] = -1; wb[1] = -1;
        lt_ui_layout_row(ui, 2, wb, 0);
        if (lt_ui_button_ex(ui, "Undo", st->n_undo > 0 ? 0 : LT_UI_BTN_DISABLED) && st->n_undo > 0) {
            lt_pagesrc ps = lt_app_pages(app);
            lt_port_cfg cur;
            pe_undo *u = &st->undo[--st->n_undo];
            if (lt_port_read(app->ini, pe, &ps, u->port, &cur) == 0) { (void)pe_apply(app, st, pe, u->port, NULL, &u->cfg); }
            st->sel = u->port;
        }
        if (lt_ui_button_ex(ui, "Burn", lt_app_can_burn(app) ? 0 : LT_UI_BTN_DISABLED) && lt_app_can_burn(app)) {
            (void)lt_app_burn(app);
        }
    }
    lt_ui_layout_column_end(ui);
    lt_ui_layout_column_begin(ui);
    if (st->sel >= n) { st->sel = n > 0 ? n - 1 : 0; }
    lt_ui_layout_row(ui, 1, &w1, -1);
    if (lt_ui_scroll_begin(ui, "pe_body")) {
        if (n > 0) { pe_editor(app, st, pe); }
        lt_ui_scroll_end(ui);
    }
    lt_ui_layout_column_end(ui);
}

static void pe_close(lt_app *app, lt_app_tab *tab)
{
    pe_tab *st = (pe_tab *)tab->data;
    int i;
    LT_UNUSED(app);
    if (st == NULL) { return; }
    for (i = 0; i < st->n_names; i++) { lt_free(st->names[i]); }
    lt_free(st->names);
    lt_free(st->chmap);
    lt_free(st);
    tab->data = NULL;
}

const lt_app_tab_kind lt_portedit_tab_kind = { "porteditor", pe_draw, pe_close };

int lt_app_port_editor_open(lt_app *app, const char *name)
{
    const lt_ini_port_editor *pe;
    if (app->ini == NULL) { return -1; }
    pe = name != NULL ? lt_ini_find_port_editor(app->ini, name) : NULL;
    if (pe == NULL && app->ini->n_port_editors > 0) { pe = &app->ini->port_editors[0]; }
    return lt_app_tab_open(app, "porteditor", pe != NULL ? pe->name : (name != NULL ? name : "std_port_edit"),
                           pe != NULL && pe->label != NULL ? pe->label : "Output Port Settings", 1) != NULL ? 0 : -1;
}
