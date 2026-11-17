/* feat_tabs.c - tab management (G-103): drag to reorder, tab context menu (Close, Close Others,
 * Close to the Right, Close All, Protect Tab) and protected tabs (no close button, immune to
 * Ctrl+W and to the bulk close commands).  The tab bar itself is drawn by the shell (tab_area in
 * app_shell.c), which hands the tab rectangles to lt_feat_tabs_bar.  The tab operations are plain
 * functions on lt_app so tests can run them headless.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <string.h>
#include "feat.h"

typedef struct tabs_state {
    int cand;               /* tab under the left button press, -1 none */
    int press_x;
    int dragging;
    int prev_down;
} tabs_state;

/* ---- operations ---- */
static int can_close(const lt_app_tab *t) { return t->closable && !t->pinned; }

void lt_feat_tabs_close_active(lt_app *app)
{
    lt_app_tab *a = lt_app_tab_active(app);
    if (a != NULL && can_close(a)) { lt_app_tab_close(app, a); }
}

/* move tab `from` to position `to`; the active tab stays the same tab */
int lt_feat_tabs_move(lt_app *app, int from, int to)
{
    lt_app_tab *t, *act;
    int i;
    if (from < 0 || from >= app->n_tabs || to < 0 || to >= app->n_tabs || from == to) { return -1; }
    act = (app->active_tab >= 0 && app->active_tab < app->n_tabs) ? app->tabs[app->active_tab] : NULL;
    t = app->tabs[from];
    if (from < to) { for (i = from; i < to; i++) { app->tabs[i] = app->tabs[i + 1]; } }
    else { for (i = from; i > to; i--) { app->tabs[i] = app->tabs[i - 1]; } }
    app->tabs[to] = t;
    if (act != NULL) {
        for (i = 0; i < app->n_tabs; i++) { if (app->tabs[i] == act) { app->active_tab = i; } }
    }
    return 0;
}

/* close tabs for which keep(i) is false, from the end; protected / permanent tabs stay.
 * mode 0 = all but `idx`, 1 = those right of `idx`, 2 = all.  Returns how many were closed. */
int lt_feat_tabs_close_many(lt_app *app, int mode, int idx)
{
    int i, closed = 0;
    for (i = app->n_tabs - 1; i >= 0; i--) {
        lt_app_tab *t = app->tabs[i];
        int hit = (mode == 2) || (mode == 0 && i != idx) || (mode == 1 && i > idx);
        if (hit && can_close(t)) { lt_app_tab_close(app, t); closed++; }
    }
    return closed;
}

void lt_feat_tabs_protect(lt_app *app, int idx, int on)
{
    if (idx >= 0 && idx < app->n_tabs) { app->tabs[idx]->pinned = on ? 1 : 0; }
}

/* index of the tab whose horizontal span holds x (clamped to the first / last); -1 when none */
int lt_feat_tabs_at(const lt_rect *rects, int n, int x)
{
    int i;
    if (n <= 0) { return -1; }
    for (i = 0; i < n; i++) {
        if (x >= rects[i].x && x < rects[i].x + rects[i].w) { return i; }
    }
    return x < rects[0].x ? 0 : n - 1;
}

/* ---- the bar: drag and context menu ---- */
void lt_feat_tabs_bar(lt_app *app, const lt_rect *rects, int n)
{
    lt_ui *ui = app->ui;
    tabs_state *st = (tabs_state *)lt_ext_state(app, "tabs", sizeof(tabs_state), NULL);
    lt_point m = lt_ui_mouse(ui);
    int down = lt_ui_mouse_down(ui, LT_MB_LEFT) ? 1 : 0;
    int i, act_mode = -1, act_idx = -1;
    if (n <= 0 || lt_ui_popup_open(ui)) { st->cand = -1; st->dragging = 0; }
    else {
        if (down && !st->prev_down) {
            st->cand = -1;
            st->dragging = 0;
            for (i = 0; i < n; i++) {
                lt_rect r = rects[i];
                int close_w = lt_ui_px(ui, 26);
                if (m.x >= r.x && m.x < r.x + r.w - (app->tabs[i]->closable ? close_w : 0) &&
                    m.y >= r.y && m.y < r.y + r.h) {
                    st->cand = i;
                    st->press_x = m.x;
                }
            }
        }
        if (down && st->cand >= 0 && st->cand < n) {
            int dx = m.x - st->press_x;
            if (!st->dragging && (dx > 8 || dx < -8)) { st->dragging = 1; }
            if (st->dragging) {
                int to = lt_feat_tabs_at(rects, n, m.x);
                if (to >= 0 && to != st->cand && lt_feat_tabs_move(app, st->cand, to) == 0) {
                    st->cand = to;
                    lt_ui_request_redraw(ui);
                }
            }
        }
        if (!down) { st->cand = -1; st->dragging = 0; }
    }
    st->prev_down = down;

    for (i = 0; i < n && i < app->n_tabs; i++) {
        lt_app_tab *t = app->tabs[i];
        lt_ui_push_id_int(ui, 5000 + i);
        if (lt_ui_context_begin(ui, "tabctx", rects[i])) {
            if (lt_ui_menu_item(ui, lt_tr("Close"), lt_hk_text(app, "tab.close"),
                                can_close(t) ? 0 : LT_UI_MENU_DISABLED)) { act_mode = 3; act_idx = i; }
            if (lt_ui_menu_item(ui, lt_tr("Close Others"), NULL,
                                app->n_tabs > 1 ? 0 : LT_UI_MENU_DISABLED)) { act_mode = 0; act_idx = i; }
            if (lt_ui_menu_item(ui, lt_tr("Close to the Right"), NULL,
                                i < app->n_tabs - 1 ? 0 : LT_UI_MENU_DISABLED)) { act_mode = 1; act_idx = i; }
            if (lt_ui_menu_item(ui, lt_tr("Close All"), NULL, 0)) { act_mode = 2; act_idx = i; }
            lt_ui_menu_separator(ui);
            if (lt_ui_menu_item(ui, lt_tr("Protect Tab"), NULL, t->pinned ? LT_UI_MENU_CHECKED : 0)) {
                act_mode = 4; act_idx = i;
            }
            if (lt_ui_menu_item(ui, lt_tr("Move Left"), NULL, i > 0 ? 0 : LT_UI_MENU_DISABLED)) {
                act_mode = 5; act_idx = i;
            }
            if (lt_ui_menu_item(ui, lt_tr("Move Right"), NULL,
                                i < app->n_tabs - 1 ? 0 : LT_UI_MENU_DISABLED)) { act_mode = 6; act_idx = i; }
            lt_ui_context_end(ui);
        }
        lt_ui_pop_id(ui);
    }
    if (act_idx >= 0) {
        switch (act_mode) {
        case 3: if (can_close(app->tabs[act_idx])) { lt_app_tab_close(app, app->tabs[act_idx]); } break;
        case 0: case 1: case 2: (void)lt_feat_tabs_close_many(app, act_mode, act_idx); break;
        case 4: lt_feat_tabs_protect(app, act_idx, !app->tabs[act_idx]->pinned); break;
        case 5: (void)lt_feat_tabs_move(app, act_idx, act_idx - 1); break;
        case 6: (void)lt_feat_tabs_move(app, act_idx, act_idx + 1); break;
        default: break;
        }
    }
}
