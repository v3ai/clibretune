/* tables_curve.c - curve editor tab: line plot with draggable points plus an editable list.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <string.h>
#include <math.h>
#include "tables_priv.h"
#include "view_kit.h"

typedef struct crv_tab {
    lt_tblview v;
    int loaded;
    char msg[160];
    const lt_ini_curve *c;
    int xdig, ydig;
    double ylo, yhi, xlo, xhi;     /* constant limits */
    double px0, px1, py0, py1;     /* plot ranges */
    int drag, drag_moved, prev_down;
    int frame;
    double live_x, live_y;
    int live_ok;
} crv_tab;

static void crv_load(lt_app *app, lt_app_tab *tab, crv_tab *st)
{
    lt_pagesrc src = lt_app_pages(app);
    const lt_ini_const *xc, *yc;
    lt_tblview_free(&st->v);
    lt_tblview_init(&st->v);
    st->loaded = 0;
    st->c = lt_ini_find_curve(app->ini, tab->target);
    if (st->c == NULL) { lt_snprintf(st->msg, sizeof(st->msg), "Unknown curve '%s'", tab->target); return; }
    if (lt_tblview_load_curve(&st->v, app->ini, st->c, &src) < 0) {
        lt_snprintf(st->msg, sizeof(st->msg), "Cannot load curve: %s", lt_err_get());
        return;
    }
    xc = st->c->x_const != NULL ? lt_ini_find_const(app->ini, st->c->x_const) : NULL;
    yc = st->c->y_const != NULL ? lt_ini_find_const(app->ini, st->c->y_const) : NULL;
    st->xdig = xc != NULL ? xc->digits : 0;
    st->ydig = yc != NULL ? yc->digits : 0;
    st->xlo = xc != NULL ? xc->min : 0.0; st->xhi = xc != NULL ? xc->max : 0.0;
    st->ylo = yc != NULL ? yc->min : 0.0; st->yhi = yc != NULL ? yc->max : 0.0;
    st->loaded = 1;
}

static void crv_ranges(crv_tab *st)
{
    size_t i;
    double xl = 1e300, xh = -1e300, yl = 1e300, yh = -1e300, pad;
    for (i = 0; i < st->v.cols; i++) {
        if (st->v.x[i] < xl) { xl = st->v.x[i]; }
        if (st->v.x[i] > xh) { xh = st->v.x[i]; }
        if (st->v.z[i] < yl) { yl = st->v.z[i]; }
        if (st->v.z[i] > yh) { yh = st->v.z[i]; }
    }
    if (st->c->has_xaxis && st->c->xaxis[1] > st->c->xaxis[0]) { st->px0 = st->c->xaxis[0]; st->px1 = st->c->xaxis[1]; }
    else { pad = (xh - xl) * 0.05; if (pad <= 0) { pad = 1; } st->px0 = xl - pad; st->px1 = xh + pad; }
    if (st->c->has_yaxis && st->c->yaxis[1] > st->c->yaxis[0]) { st->py0 = st->c->yaxis[0]; st->py1 = st->c->yaxis[1]; }
    else { pad = (yh - yl) * 0.05; if (pad <= 0) { pad = 1; } st->py0 = yl - pad; st->py1 = yh + pad; }
}

static void crv_commit(lt_app *app, lt_app_tab *tab, crv_tab *st)
{
    lt_pagesrc dst = lt_app_pages(app);
    int n = lt_tblview_commit(&st->v, app->ini, &dst);
    if (n > 0) { lt_app_edited(app); tab->dirty = 1; }
    else if (n < 0) { lt_app_toast(app, 3, "Write failed: %s", lt_err_get()); }
}

static void crv_check_external(lt_app *app, lt_app_tab *tab, crv_tab *st)
{
    const lt_ini_const *yc;
    lt_pagesrc src;
    double *tmp;
    size_t i, n = st->v.cols;
    int diff = 0;
    if (st->drag || lt_tblview_is_dirty(&st->v) || st->c->y_const == NULL) { return; }
    yc = lt_ini_find_const(app->ini, st->c->y_const);
    if (yc == NULL) { return; }
    tmp = (double *)lt_calloc(n + 1, sizeof(double));
    src = lt_app_pages(app);
    if (lt_ini_const_get_array(app->ini, yc, &src, tmp, n) != (int)n) { diff = 1; }
    for (i = 0; i < n && !diff; i++) { if (fabs(tmp[i] - st->v.z[i]) > 1e-9) { diff = 1; } }
    lt_free(tmp);
    if (diff) { crv_load(app, tab, st); }
}

static double clampy(const crv_tab *st, double v)
{
    if (st->yhi > st->ylo) { v = v < st->ylo ? st->ylo : (v > st->yhi ? st->yhi : v); }
    return lt_tabs_round(v, st->ydig);
}

typedef struct plot_ctx { crv_tab *st; lt_rect r; lt_color line, grid, pt, sel, live, axis, bg; int hot, cell; } plot_ctx;

#define PL_L 54
#define PL_R 20
#define PL_T 18
#define PL_B 38

static double sx_of(const plot_ctx *p, double x)
{
    return p->r.x + PL_L + (x - p->st->px0) / (p->st->px1 - p->st->px0) * (p->r.w - PL_L - PL_R);
}
static double sy_of(const plot_ctx *p, double y)
{
    return p->r.y + p->r.h - PL_B - (y - p->st->py0) / (p->st->py1 - p->st->py0) * (p->r.h - PL_T - PL_B);
}

static void chip(lt_gfx *g, double cx, double ytop, const char *s, lt_color fg)
{
    lt_font *f = lt_font_get(LT_FONT_MONO_MEDIUM);
    double tw = f != NULL ? lt_gfx_text_measure(f, 11.0, s) : (double)strlen(s) * 8.0;
    lt_rect c = lt_rect_make((int)(cx - tw / 2.0) - 6, (int)ytop, (int)tw + 12, 18);
    lt_gfx_fill_round_rect(g, c, 4, LT_RGBA(0, 0, 0, 215));
    lt_gfx_rect(g, c, LT_RGBA(255, 255, 255, 40));
    lt_gfx_text_bl(g, f, 11.0, (double)c.x + 6.0, c.y + 13, s, fg);
}

static void draw_plot(lt_gfx *g, lt_rect r, void *user)
{
    plot_ctx *p = (plot_ctx *)user;
    crv_tab *st = p->st;
    lt_font *fm = lt_font_get(LT_FONT_MONO), *fs = lt_font_get(LT_FONT_SANS_MEDIUM);
    size_t i;
    int k, x0, x1, y0, y1;
    char b[48];
    x0 = (int)sx_of(p, st->px0); x1 = (int)sx_of(p, st->px1);
    y0 = (int)sy_of(p, st->py0); y1 = (int)sy_of(p, st->py1);
    /* plot area */
    lt_gfx_fill_rect_vgrad(g, lt_rect_make(x0, y1, x1 - x0, y0 - y1), p->bg,
                           lt_color_mix(p->bg, LT_RGB(0, 0, 0), 60));
    for (k = 0; k <= 4; k++) {
        double fy = st->py0 + (st->py1 - st->py0) * k / 4.0, fx = st->px0 + (st->px1 - st->px0) * k / 4.0;
        double yy = sy_of(p, fy), xx = sx_of(p, fx);
        lt_gfx_hline(g, x0, x1, (int)yy, p->grid);
        lt_gfx_vline(g, (int)xx, y1, y0, p->grid);
        lt_snprintf(b, sizeof(b), "%g", fy);
        lt_gfx_text_bl(g, fm, 10.0, (double)x0 - 8.0 - (fm != NULL ? lt_gfx_text_measure(fm, 10.0, b) : 0.0),
                       (int)yy + 4, b, p->axis);
        lt_snprintf(b, sizeof(b), "%g", fx);
        lt_gfx_text_bl(g, fm, 10.0, xx - (fm != NULL ? lt_gfx_text_measure(fm, 10.0, b) / 2.0 : 0.0), y0 + 16, b, p->axis);
    }
    lt_gfx_rect(g, lt_rect_make(x0, y1, x1 - x0 + 1, y0 - y1 + 1), LT_RGBA(255, 255, 255, 45));
    /* axis captions */
    if (st->c->col_x != NULL) {
        lt_gfx_text_bl(g, fs, 11.0, (double)(x0 + x1) / 2.0 - lt_gfx_text_measure(fs, 11.0, st->c->col_x) / 2.0,
                       r.y + r.h - 6, st->c->col_x, p->axis);
    }
    if (st->c->col_y != NULL) {
        lt_gfx_text_rot(g, fs, 11.0, r.x + 12, (double)(y0 + y1) / 2.0, st->c->col_y, -1.57079632679, 1, 2, p->axis);
    }
    if (st->v.cols >= 2 && st->v.cols <= 128 && !p->cell) {
        double poly[2 * 132];
        int n = 0;
        for (i = 0; i < st->v.cols; i++) {
            poly[n * 2] = sx_of(p, st->v.x[i]); poly[n * 2 + 1] = sy_of(p, st->v.z[i]); n++;
        }
        poly[n * 2] = sx_of(p, st->v.x[st->v.cols - 1]); poly[n * 2 + 1] = (double)y0; n++;
        poly[n * 2] = sx_of(p, st->v.x[0]); poly[n * 2 + 1] = (double)y0; n++;
        lt_gfx_fill_poly(g, poly, n, (p->line & 0x00FFFFFFUL) | 0x28000000UL);
    }
    for (i = 0; i + 1 < st->v.cols; i++) {
        double ax = sx_of(p, st->v.x[i]), ay = sy_of(p, st->v.z[i]);
        double bx = sx_of(p, st->v.x[i + 1]), by = sy_of(p, st->v.z[i + 1]);
        lt_gfx_line(g, ax, ay, bx, by, 6.0, (p->line & 0x00FFFFFFUL) | 0x30000000UL);
        lt_gfx_line(g, ax, ay, bx, by, 2.2, p->line);
    }
    for (i = 0; i < st->v.cols; i++) {
        double px = sx_of(p, st->v.x[i]), py = sy_of(p, st->v.z[i]);
        int hot = (int)i == p->hot;
        if (hot) { lt_gfx_fill_circle(g, px, py, 11.0, (p->sel & 0x00FFFFFFUL) | 0x30000000UL); }
        lt_gfx_fill_circle(g, px, py, hot ? 6.5 : 5.0, LT_RGBA(0, 0, 0, 120));
        lt_gfx_fill_circle(g, px, py, hot ? 5.5 : 4.0, hot ? p->sel : p->pt);
        lt_gfx_circle(g, px, py, hot ? 5.5 : 4.0, 1.2, LT_RGB(255, 255, 255));
    }
    if (p->hot >= 0 && (size_t)p->hot < st->v.cols) {
        char vb[48];
        lt_tabs_fmt(b, sizeof(b), st->v.x[p->hot], st->xdig);
        lt_tabs_fmt(vb, sizeof(vb), st->v.z[p->hot], st->ydig);
        lt_snprintf(b + strlen(b), sizeof(b) - strlen(b), ", %s", vb);
        chip(g, sx_of(p, st->v.x[p->hot]), sy_of(p, st->v.z[p->hot]) - 30.0, b, LT_RGB(255, 255, 255));
    }
    if (st->live_ok) {
        double lx = sx_of(p, st->live_x), ly = sy_of(p, st->live_y);
        lt_gfx_line(g, lx, (double)y1, lx, (double)y0, 1.2, (p->live & 0x00FFFFFFUL) | 0x90000000UL);
        lt_gfx_fill_circle(g, lx, ly, 10.0, (p->live & 0x00FFFFFFUL) | 0x30000000UL);
        lt_gfx_fill_circle(g, lx, ly, 5.0, p->live);
        lt_gfx_circle(g, lx, ly, 5.5, 1.2, LT_RGB(255, 255, 255));
        lt_tabs_fmt(b, sizeof(b), st->live_y, st->ydig);
        chip(g, lx, ly + 12.0, b, p->live);
    }
}

static void crv_live(lt_app *app, crv_tab *st)
{
    double v;
    size_t i;
    st->live_ok = 0;
    if (st->c->x_channel == NULL || st->v.cols < 2 || app->rt == NULL) { return; }
    if (app->conn != LT_APP_ONLINE && app->conn != LT_APP_DEMO) { return; }
    if (!lt_app_channel(app, st->c->x_channel, &v)) { return; }
    for (i = 0; i + 2 < st->v.cols && v > st->v.x[i + 1]; i++) { }
    {
        double x0 = st->v.x[i], x1 = st->v.x[i + 1], f = x1 > x0 ? (v - x0) / (x1 - x0) : 0.0;
        f = f < 0.0 ? 0.0 : (f > 1.0 ? 1.0 : f);
        st->live_x = v;
        st->live_y = st->v.z[i] + (st->v.z[i + 1] - st->v.z[i]) * f;
        st->live_ok = 1;
    }
}

static void crv_draw(lt_app *app, lt_app_tab *tab, lt_rect rr)
{
    crv_tab *st = (crv_tab *)tab->data;
    lt_ui *ui = app->ui;
    const lt_theme *th = lt_ui_theme(ui);
    int w1 = -1, w2[2], wt[4];
    static plot_ctx pc;
    lt_rect pr;
    lt_point m;
    int down, inside, i;
    LT_UNUSED(rr);
    if (st == NULL) {
        st = (crv_tab *)lt_calloc(1, sizeof(*st));
        tab->data = st;
        st->drag = -1;
        crv_load(app, tab, st);
    }
    st->frame++;
    if (!st->loaded) {
        lt_ui_layout_row(ui, 1, &w1, 0);
        lt_ui_heading(ui, tab->title);
        lt_ui_label_ex(ui, st->msg, th->error, LT_UI_ALIGN_LEFT);
        return;
    }
    if (st->frame % 15 == 0) { crv_check_external(app, tab, st); }
    if (!st->loaded) { return; }
    crv_live(app, st);
    crv_ranges(st);
    if (!lt_ui_text_focused(ui)) {
        if (lt_ui_key_pressed(ui, 'z', LT_MOD_CTRL)) {
            if (lt_tblview_undo(&st->v)) { crv_commit(app, tab, st); }
        }
        if (lt_ui_key_pressed(ui, 'y', LT_MOD_CTRL) || lt_ui_key_pressed(ui, 'z', LT_MOD_CTRL | LT_MOD_SHIFT)) {
            if (lt_tblview_redo(&st->v)) { crv_commit(app, tab, st); }
        }
    }
    /* header + toolbar */
    if (lt_ui_cell_mode(ui)) {
        wt[0] = wt[1] = wt[2] = 0; wt[3] = 0;
        lt_ui_layout_row(ui, 4, wt, 0);
        if (lt_ui_button_ex(ui, "Undo", LT_UI_BTN_FLAT)) { if (lt_tblview_undo(&st->v)) { crv_commit(app, tab, st); } }
        if (lt_ui_button_ex(ui, "Redo", LT_UI_BTN_FLAT)) { if (lt_tblview_redo(&st->v)) { crv_commit(app, tab, st); } }
        if (lt_ui_button_ex(ui, "Burn", LT_UI_BTN_ACCENT | (lt_app_can_burn(app) ? 0 : LT_UI_BTN_DISABLED))) { lt_app_burn(app); tab->dirty = 0; }
        lt_ui_label_ex(ui, "  Drag a point vertically; edit X values in the list", th->muted, LT_UI_ALIGN_LEFT);
    } else {
        lt_vk_item it[6];
        lt_rect hr;
        int hit;
        lt_ui_layout_row(ui, 1, &w1, 40);
        hr = lt_ui_layout_next(ui);
        lt_ui_draw_fill(ui, hr, th->bg_app);
        lt_ui_draw_hline(ui, hr.x, hr.x + hr.w - 1, hr.y + hr.h - 1, th->border_subtle);
        lt_vk_text(ui, lt_rect_make(hr.x + 14, hr.y, hr.w / 2, hr.h), (int)LT_FONT_SANS_SEMIBOLD, 14.0, tab->title, th->text, 0);
        memset(it, 0, sizeof(it));
        it[0].kind = LT_VK_SPACER;
        it[1].kind = LT_VK_BTN; it[1].icon = LT_ICON_UNDO_2; it[1].label = "Undo"; it[1].tip = "Undo (Ctrl+Z)";
        it[2].kind = LT_VK_BTN; it[2].icon = LT_ICON_REDO_2; it[2].label = "Redo"; it[2].tip = "Redo (Ctrl+Y)";
        it[3].kind = LT_VK_SEP;
        it[4].kind = LT_VK_BTN; it[4].icon = LT_ICON_FLAME; it[4].label = "Burn"; it[4].tip = "Write the changes to ECU flash";
        it[4].accent = 1; it[4].disabled = lt_app_can_burn(app) ? 0 : 1;
        it[5].kind = LT_VK_END;
        hit = lt_vk_toolbar_in(ui, it, lt_rect_make(hr.x + hr.w / 2, hr.y + 5, hr.w / 2 - 12, 30));
        if (hit == 1) { if (lt_tblview_undo(&st->v)) { crv_commit(app, tab, st); } }
        else if (hit == 2) { if (lt_tblview_redo(&st->v)) { crv_commit(app, tab, st); } }
        else if (hit == 4) { lt_app_burn(app); tab->dirty = 0; }
        lt_ui_layout_row(ui, 1, &w1, 22);
        lt_ui_label_ex(ui, "  Drag a point vertically; edit X values in the list", th->muted, LT_UI_ALIGN_LEFT);
    }
    w2[0] = -3; w2[1] = -2;
    lt_ui_layout_row(ui, 2, w2, -1);
    pr = lt_ui_layout_next(ui);
    /* interaction */
    m = lt_ui_mouse(ui);
    down = lt_ui_mouse_down(ui, 0) ? 1 : 0;
    inside = m.x >= pr.x && m.x < pr.x + pr.w && m.y >= pr.y && m.y < pr.y + pr.h;
    pc.st = st; pc.r = pr;
    pc.line = th->primary_hover; pc.grid = LT_RGBA(255, 255, 255, th->dark ? 22 : 0);
    if (!th->dark) { pc.grid = LT_RGBA(0, 0, 0, 28); }
    pc.pt = th->accent; pc.sel = th->warning; pc.live = th->cursor_live; pc.axis = th->text2;
    pc.bg = th->table_bg; pc.cell = lt_ui_cell_mode(ui);
    pc.hot = st->drag;
    if (st->drag < 0) {
        double best = 100.0;
        pc.hot = -1;
        for (i = 0; i < (int)st->v.cols && inside; i++) {
            double dx = sx_of(&pc, st->v.x[i]) - m.x, dy = sy_of(&pc, st->v.z[i]) - m.y, d = dx * dx + dy * dy;
            if (d < best) { best = d; pc.hot = i; }
        }
        if (down && !st->prev_down && pc.hot >= 0) {
            st->drag = pc.hot; st->drag_moved = 0;
            lt_tblview_push_undo(&st->v);
        }
    }
    if (st->drag >= 0) {
        if (!down) {
            if (st->drag_moved) { crv_commit(app, tab, st); }
            st->drag = -1;
        } else {
            double ny = st->py0 + (sy_of(&pc, st->py0) - m.y) / (sy_of(&pc, st->py0) - sy_of(&pc, st->py1)) * (st->py1 - st->py0);
            lt_tblview_set(&st->v, 0, (size_t)st->drag, clampy(st, ny));
            st->drag_moved = 1;
            pc.hot = st->drag;
        }
    }
    st->prev_down = down;
    lt_ui_draw_box(ui, pr, th->table_bg, th->border_subtle, 0);
    lt_ui_draw_custom(ui, pr, draw_plot, &pc);
    /* value list */
    {
        int rhw = 40, cw = 78, ch = lt_ui_cell_mode(ui) ? 16 : 22;
        if (lt_ui_grid_begin(ui, "list", (int)st->v.cols, 2, cw, ch, rhw, lt_ui_cell_mode(ui) ? 16 : 24)) {
            int r0, r1, c0, c1, r, c, fl;
            char b[32];
            lt_ui_grid_visible(ui, &r0, &r1, &c0, &c1);
            lt_ui_grid_corner(ui, "#");
            lt_ui_grid_header(ui, 1, 0, st->c->col_x != NULL ? st->c->col_x : "X");
            lt_ui_grid_header(ui, 1, 1, st->c->col_y != NULL ? st->c->col_y : "Y");
            for (r = r0; r <= r1; r++) {
                lt_snprintf(b, sizeof(b), "%d", r + 1);
                lt_ui_grid_header(ui, 0, r, b);
                for (c = 0; c < 2; c++) {
                    double v = c == 0 ? st->v.x[r] : st->v.z[r];
                    lt_color bg = (r == st->drag || r == pc.hot) ? th->table_selected : th->table_cell;
                    lt_tabs_fmt(b, sizeof(b), v, c == 0 ? st->xdig : st->ydig);
                    lt_ui_grid_cell(ui, r, c, b, bg, th->text);
                }
            }
            fl = lt_ui_grid_end(ui);
            if ((fl & LT_UI_GRID_EDIT_BEGIN) && lt_ui_grid_edit_wants_text(ui)) {
                int er, ec;
                lt_ui_grid_editing(ui, &er, &ec);
                lt_tabs_fmt(b, sizeof(b), ec == 0 ? st->v.x[er] : st->v.z[er], ec == 0 ? st->xdig : st->ydig);
                lt_ui_grid_edit_set_text(ui, b);
            }
            if (fl & LT_UI_GRID_EDIT_COMMIT) {
                double v;
                int er, ec;
                lt_ui_grid_cursor(ui, &er, &ec);
                if (lt_parse_double(lt_ui_grid_edit_text(ui), &v) && er >= 0 && ec >= 0) {
                    lt_tblview_push_undo(&st->v);
                    if (ec == 0) {
                        if (st->xhi > st->xlo) { v = v < st->xlo ? st->xlo : (v > st->xhi ? st->xhi : v); }
                        lt_tblview_set_x(&st->v, (size_t)er, lt_tabs_round(v, st->xdig));
                    } else { lt_tblview_set(&st->v, 0, (size_t)er, clampy(st, v)); }
                    crv_commit(app, tab, st);
                } else { lt_app_toast(app, 2, "Not a number"); }
            }
        }
    }
}

static void crv_close(lt_app *app, lt_app_tab *tab)
{
    crv_tab *st = (crv_tab *)tab->data;
    LT_UNUSED(app);
    if (st == NULL) { return; }
    lt_tblview_free(&st->v);
    lt_free(st);
    tab->data = NULL;
}

const lt_app_tab_kind lt_tabs_kind_curve = { "curve", crv_draw, crv_close };
