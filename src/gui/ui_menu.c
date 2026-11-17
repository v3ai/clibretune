/* ui_menu.c - popup layers: menu bar, menus, submenus, context menus,
 * combo boxes, list boxes, colour editor.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "ui_priv.h"

/* ------------------------------------------------------------------ */
/* popup stack                                                         */
int ui_popup_find(const lt_ui *ui, lt_u32 id)
{
    int i;
    for (i = 0; i < ui->npopups; i++) {
        if (ui->popups[i].id == id) { return i; }
    }
    return -1;
}

void ui_popup_open(lt_ui *ui, int depth, lt_u32 id, int kind, lt_rect anchor, int right, int x, int y)
{
    ui_popup *p;
    if (depth < 0) { depth = 0; }
    if (depth >= UI_MAX_POPUPS) { return; }
    p = &ui->popups[depth];
    p->id = id; p->kind = kind; p->anchor = anchor; p->right = right; p->x = x; p->y = y;
    p->minw = 0;
    ui->npopups = depth + 1;
    ui->menu_hi[depth] = -1;
    memset(ui->menu_dis[depth], 0, sizeof(ui->menu_dis[depth]));
    ui_request(ui);
}

void ui_popup_close_from(lt_ui *ui, int idx)
{
    if (idx < 0) { idx = 0; }
    if (idx < ui->npopups) { ui->npopups = idx; ui_request(ui); }
}

void lt_ui_popups_close(lt_ui *ui) { ui->npopups = 0; ui_request(ui); }
lt_bool lt_ui_popup_open(const lt_ui *ui) { return ui->npopups > 0 ? LT_TRUE : LT_FALSE; }

void ui_popup_need(lt_ui *ui, int w)
{
    int d = ui->menu_depth - 1;
    if (d < 0) { d = 0; }
    if (w > ui->pop_need[d]) { ui->pop_need[d] = w; }
}

static int popup_ins(const lt_ui *ui) { return ui->cell ? 0 : lt_ui_px(ui, 3); }

void ui_popup_begin(lt_ui *ui, int idx, int min_w, int est_h)
{
    ui_popup *P = &ui->popups[idx];
    ui_state *st = ui_state_get(ui, P->id ^ 0x5BD1E995UL);
    int w, h, x, y, z, base, ins = popup_ins(ui);
    lt_rect rect;
    ui_layer *L;
    ui_cmd *c;
    const lt_theme *t = &ui->theme;

    P->minw = min_w;
    w = st->i[0] > 0 ? st->i[0] : min_w;
    if (w < min_w) { w = min_w; }
    h = st->i[1] > 0 ? st->i[1] : est_h;
    if (P->kind == 2 || P->kind == 3) {
        x = P->x + (ui->cell ? 0 : lt_ui_px(ui, 2)); y = P->y + (ui->cell ? 0 : lt_ui_px(ui, 2));
    } else if (P->right) {
        x = P->anchor.x + P->anchor.w - 2; y = P->anchor.y - ins;
    } else {
        x = P->anchor.x; y = P->anchor.y + P->anchor.h + (ui->cell ? 0 : lt_ui_px(ui, 2));
    }
    if (x + w > ui->sw) {
        x = P->right ? P->anchor.x - w + 2 : ui->sw - w;
    }
    if (x < 0) { x = 0; }
    if (y + h > ui->sh) {
        if (P->kind == 2 || P->kind == 3 || P->right) { y = ui->sh - h; }
        else if (P->anchor.y - h - 2 >= 0) { y = P->anchor.y - h - 2; }
        else { y = ui->sh - h; }
    }
    if (y < 0) { y = 0; }
    rect = lt_rect_make(x, y, w, h);
    base = ui_layer_z(ui);
    z = base < 1000 ? 900 + idx : base + 1 + idx;
    L = ui_layer_begin(ui, P->id ^ 0x1234567UL, UL_POPUP, z, rect);
    L->bg_cmd = (int)L->cmds.len;
    {
        int k;
        for (k = 0; k < 3; k++) {       /* soft shadow rings, resized in ui_popup_end */
            c = ui_cmd_push(ui, UC_ROUND);
            c->r = lt_rect_make(x - 6 + 2 * k, y - 3 + 2 * k, w + 12 - 4 * k, h + 12 - 4 * k);
            c->c = ui->cell ? 0 : LT_RGBA(0, 0, 0, 26 + 8 * k);
            c->i[0] = t->radius + 6 - 2 * k;
        }
    }
    c = ui_cmd_push(ui, UC_ROUND); c->r = rect; c->c = ui->cell ? t->elevated : t->border; c->i[0] = t->radius;
    c = ui_cmd_push(ui, UC_ROUND); c->r = lt_rect_inset(rect, ui->cell ? 0 : 1, ui->cell ? 0 : 1); c->c = t->elevated;
    c->i[0] = LT_MAX(0, t->radius - 1);
    ui->pop_need[idx] = 0;
    ui_layout_push(ui, lt_rect_make(x + ins, y + ins, w - 2 * ins, h - 2 * ins));
    ui_lay(ui)->sp_y = 0;
    lt_ui_clip_push(ui, rect);
}

static void popup_nav(lt_ui *ui, int idx)
{
    int cnt = ui->menu_idx[idx], hi = ui->menu_hi[idx], i, dir = 0, mods = 0;
    if (idx != ui->npopups - 1) { return; }
    if (idx > 0 && ui_key_take(ui, LT_KEY_LEFT, &mods)) {
        ui->npopups = idx;
        ui_request(ui);
        return;
    }
    if (cnt <= 0) { return; }
    if (ui_key_take(ui, LT_KEY_DOWN, &mods)) { dir = 1; }
    else if (ui_key_take(ui, LT_KEY_UP, &mods)) { dir = -1; }
    else if (ui_key_take(ui, LT_KEY_HOME, &mods)) { hi = -1; dir = 1; }
    else if (ui_key_take(ui, LT_KEY_END, &mods)) { hi = cnt; dir = -1; }
    if (dir != 0) {
        for (i = 0; i < cnt; i++) {
            hi += dir;
            if (hi < 0) { hi = cnt - 1; }
            if (hi >= cnt) { hi = 0; }
            if (hi >= 64 || !ui->menu_dis[idx][hi]) { break; }
        }
        ui->menu_hi[idx] = hi;
        ui_request(ui);
    }
}

void ui_popup_end(lt_ui *ui, int idx)
{
    ui_popup *P = &ui->popups[idx];
    ui_state *st = ui_state_get(ui, P->id ^ 0x5BD1E995UL);
    ui_layout *LY = ui_lay(ui);
    ui_layer *L = ui_layer_cur(ui);
    int ins = popup_ins(ui);
    int nw = LT_MAX(P->minw, ui->pop_need[idx] + 2 * ins);
    int nh = LY->max_y - LY->body.y + 2 * ins;
    lt_rect rect = L->rect;
    if (nh < 4) { nh = 4; }
    if (st->i[0] != nw || st->i[1] != nh) {
        st->i[0] = nw; st->i[1] = nh;
        ui_request(ui);
    }
    rect.w = nw; rect.h = nh;
    L->rect = rect;
    if (L->bg_cmd >= 0 && (size_t)L->bg_cmd + 4 < L->cmds.len) {
        ui_cmd *c = &LT_VEC_AT(&L->cmds, ui_cmd, L->bg_cmd);
        int k;
        for (k = 0; k < 3; k++) {
            c[k].r = lt_rect_make(rect.x - 6 + 2 * k, rect.y - 3 + 2 * k, nw + 12 - 4 * k, nh + 12 - 4 * k);
        }
        c[3].r = rect;
        c[4].r = lt_rect_inset(rect, ui->cell ? 0 : 1, ui->cell ? 0 : 1);
    }
    lt_ui_clip_pop(ui);
    ui_layout_pop(ui);
    ui_layer_end(ui);
    popup_nav(ui, idx);
}

/* generic item hit-testing/highlight for menu-like popups */
int ui_popup_item(lt_ui *ui, int depth, lt_rect r, int disabled)
{
    int idx = ui->menu_idx[depth]++;
    int moved = (ui->mx != ui->lmx || ui->my != ui->lmy);
    int over = ui_over(ui, r), res = 0;
    int mods = 0;
    if (idx < 64) { ui->menu_dis[depth][idx] = (unsigned char)(disabled ? 1 : 0); }
    if (over && !disabled && (moved || (ui->mpressed & (1 << LT_MB_LEFT)))) {
        ui->menu_hi[depth] = idx;
    }
    if (over) { res |= UI_ITEM_OVER; }
    if (ui->menu_hi[depth] == idx && !disabled) {
        res |= UI_ITEM_HI;
        if (depth == ui->npopups - 1 &&
            (ui_key_take(ui, LT_KEY_ENTER, &mods) || ui_key_take(ui, ' ', &mods))) {
            res |= UI_ITEM_ACT;
        }
    }
    if (over && !disabled && (ui->mreleased & (1 << LT_MB_LEFT))) { res |= UI_ITEM_ACT; }
    return res;
}

/* ------------------------------------------------------------------ */
/* menus                                                               */
static void amp_strip(const char *in, char *out, size_t cap, int *mn_pos, int *mn_ch)
{
    size_t o = 0;
    int pos = 0;
    *mn_pos = -1; *mn_ch = 0;
    while (*in && o + 1 < cap) {
        if (*in == '&') {
            if (in[1] == '&') { out[o++] = '&'; in += 2; pos++; continue; }
            if (*mn_pos < 0 && in[1] != '\0') {
                *mn_pos = pos;
                *mn_ch = (in[1] >= 'A' && in[1] <= 'Z') ? in[1] + 32 : in[1];
            }
            in++;
            continue;
        }
        out[o++] = *in++;
        pos++;
    }
    out[o] = '\0';
}

lt_bool lt_ui_menubar_begin(lt_ui *ui)
{
    const lt_theme *t = &ui->theme;
    lt_rect r = lt_rect_make(0, ui->used_top, ui->sw, t->menubar_h);
    ui->used_top += t->menubar_h;
    lt_ui_draw_fill(ui, r, t->menubar);
    if (!ui->cell) { lt_ui_draw_hline(ui, 0, ui->sw - 1, r.y + r.h - 1, t->border_subtle); }
    ui->in_bar = 1;
    ui->bar_x = ui->cell ? 0 : lt_ui_px(ui, 6);
    ui->bar_y = r.y;
    ui->bar_h = r.h;
    ui->nbar = 0;
    return LT_TRUE;
}

void lt_ui_menubar_end(lt_ui *ui)
{
    int mods = 0, i;
    ui->in_bar = 0;
    if (ui->npopups == 1 && ui->popups[0].kind == 0 && ui->nbar > 0) {
        int dir = 0;
        if (ui_key_take(ui, LT_KEY_RIGHT, &mods)) { dir = 1; }
        else if (ui_key_take(ui, LT_KEY_LEFT, &mods)) { dir = -1; }
        if (dir != 0) {
            for (i = 0; i < ui->nbar; i++) {
                if (ui->bar_ids[i] == ui->popups[0].id) {
                    int j = (i + dir + ui->nbar) % ui->nbar;
                    ui->popups[0].id = ui->bar_ids[j];
                    ui->popups[0].anchor = lt_rect_make(0, 0, 0, 0);
                    ui->menu_hi[0] = -1;
                    ui_request(ui);
                    break;
                }
            }
        }
    }
}

lt_bool lt_ui_menu_begin(lt_ui *ui, const char *title)
{
    char vis[128];
    int mn_pos, mn_ch, tw, open, over, raw_press, moved;
    lt_u32 id = lt_ui_id(ui, title);
    const lt_theme *t = &ui->theme;
    lt_rect r;
    int padx = ui->cell ? 8 : lt_ui_px(ui, 8);
    int idx;
    amp_strip(title, vis, sizeof(vis), &mn_pos, &mn_ch);
    tw = lt_ui_text_width(ui, vis);
    r = lt_rect_make(ui->bar_x, ui->bar_y, tw + 2 * padx, ui->bar_h - (ui->cell ? 0 : 1));
    ui->bar_x += r.w;
    if (ui->nbar < 16) { ui->bar_ids[ui->nbar++] = id; }
    idx = ui_popup_find(ui, id);
    open = (idx == 0 && ui->popups[0].kind == 0);
    over = ui_over(ui, r);
    raw_press = over && (ui->mpressed & (1 << LT_MB_LEFT));
    moved = (ui->mx != ui->lmx || ui->my != ui->lmy);
    if (raw_press) {
        ui->press_claimed = 1;
        if (open) { ui->npopups = 0; open = 0; }
        else { ui_popup_open(ui, 0, id, 0, r, 0, r.x, r.y + r.h); open = 1; }
        ui_request(ui);
    } else if (over && moved && ui->npopups > 0 && !open && ui->popups[0].kind == 0 &&
               ui->popups[0].anchor.h != 0) {
        ui_popup_open(ui, 0, id, 0, r, 0, r.x, r.y + r.h);
        open = 1;
    } else if (mn_ch != 0 && ui_key_take_exact(ui, mn_ch, LT_MOD_ALT)) {
        ui_popup_open(ui, 0, id, 0, r, 0, r.x, r.y + r.h);
        ui->menu_hi[0] = 0;
        open = 1;
    }
    if (open && ui->popups[0].anchor.h == 0) { ui->popups[0].anchor = r; }
    if (open) { ui->popups[0].anchor = r; }
    {
        lt_color bg = open ? t->active : ((over && !ui->disabled) ? t->hover : 0);
        if (bg != 0) { lt_ui_draw_fill(ui, r, bg); }
        ui_text_in(ui, r, vis, t->text, 1);
        if (mn_pos >= 0 && !ui->cell) {
            int x0 = ui_prefix_w(ui, vis, mn_pos), x1 = ui_prefix_w(ui, vis, mn_pos + 1);
            int ux = r.x + padx + x0;
            lt_ui_draw_hline(ui, ux, ux + LT_MAX(1, x1 - x0) - 1, r.y + (r.h + ui_font_px(ui, 0)) / 2 + 1, t->text);
        }
    }
    ui_set_last(ui, id, r);
    if (!open) { return LT_FALSE; }
    ui_popup_begin(ui, 0, ui->cell ? 24 : lt_ui_px(ui, 200), ui->theme.row_h * 3);
    ui->menu_depth = 1;
    ui->menu_idx[0] = 0;
    return LT_TRUE;
}

void lt_ui_menubar_separator(lt_ui *ui)
{
    if (ui->cell) { ui->bar_x += 8; return; }
    lt_ui_draw_vline(ui, ui->bar_x + 3, ui->bar_y + 4, ui->bar_y + ui->bar_h - 6, ui->theme.border_subtle);
    ui->bar_x += 7;
}

void lt_ui_menu_end(lt_ui *ui)
{
    ui_popup_end(ui, 0);
    ui->menu_depth = 0;
}

static void item_draw(lt_ui *ui, lt_rect r, int st, int disabled, const char *label,
                      const char *shortcut, int flags, int arrow)
{
    const lt_theme *t = &ui->theme;
    lt_color fg = disabled ? t->disabled : t->text;
    int pad = 8, chk = ui->cell ? 16 : lt_ui_px(ui, 26);
    lt_rect hr = ui->cell ? r : lt_rect_inset(r, 2, 1);
    int hi = (st & UI_ITEM_HI) && !disabled;
    if (hi) {
        lt_ui_draw_box(ui, hr, ui->cell ? t->primary_active : t->selected, 0, ui->cell ? 0 : t->radius);
        fg = ui->cell ? t->text : t->primary_text;
    }
    if (flags & LT_UI_MENU_CHECKED) {
        lt_ui_draw_icon(ui, lt_rect_make(r.x + (ui->cell ? 0 : lt_ui_px(ui, 4)), r.y, ui->cell ? 16 : lt_ui_px(ui, 18), r.h),
                        LT_UI_ICON_CHECK, disabled ? t->disabled : (hi ? fg : (ui->cell ? t->text : t->primary_hover)));
    }
    ui_text_in(ui, lt_rect_make(r.x + chk, r.y, r.w - chk - pad, r.h), label, fg, 0);
    if (shortcut != NULL && shortcut[0]) {
        ui_text_in_f(ui, lt_rect_make(r.x, r.y, r.w - pad - (arrow ? 16 : 0), r.h), shortcut,
                     disabled ? t->disabled : (hi ? fg : t->text2), 2, ui->tfont, LT_UI_FS_SM);
    }
    if (arrow) {
        lt_ui_draw_lucide(ui, lt_rect_make(r.x + r.w - 20, r.y, 16, r.h), LT_ICON_CHEVRON_RIGHT, 12, 2.0, fg);
        if (ui->cell) { lt_ui_draw_icon(ui, lt_rect_make(r.x + r.w - 20, r.y, 16, r.h), LT_UI_ICON_ARROW_RIGHT, fg); }
    }
}

lt_bool lt_ui_menu_item(lt_ui *ui, const char *label, const char *shortcut, int flags)
{
    char vis[128];
    int mn_pos, mn_ch, depth = ui->menu_depth - 1, st, dis = (flags & LT_UI_MENU_DISABLED) != 0;
    int need;
    lt_rect r;
    if (depth < 0) { return LT_FALSE; }
    amp_strip(label, vis, sizeof(vis), &mn_pos, &mn_ch);
    need = (ui->cell ? 16 : lt_ui_px(ui, 26)) + lt_ui_text_width(ui, vis) + 16;
    if (shortcut != NULL && shortcut[0]) { need += 24 + lt_ui_text_width_f(ui, shortcut, ui->tfont, LT_UI_FS_SM); }
    ui_popup_need(ui, need + (ui->cell ? 0 : lt_ui_px(ui, 6)));
    r = ui_next(ui, 0);
    st = ui_popup_item(ui, depth, r, dis);
    if ((st & UI_ITEM_OVER) && !dis && ui->npopups > depth + 1 &&
        (ui->mx != ui->lmx || ui->my != ui->lmy)) {
        ui->npopups = depth + 1;
    }
    item_draw(ui, r, st, dis, vis, shortcut, flags, 0);
    ui_set_last(ui, lt_ui_id(ui, label), r);
    if ((st & UI_ITEM_ACT) && !dis) {
        ui->menu_close_req = 1;
        return LT_TRUE;
    }
    return LT_FALSE;
}

void lt_ui_menu_separator(lt_ui *ui)
{
    lt_rect r = ui_next(ui, 0);
    int y = r.y + (ui->cell ? 8 : lt_ui_px(ui, 4));
    lt_ui_draw_hline(ui, r.x + (ui->cell ? 0 : lt_ui_px(ui, 6)), r.x + r.w - (ui->cell ? 1 : lt_ui_px(ui, 7)), y, ui->theme.border);
    if (!ui->cell) {
        /* separators are thinner than rows */
        ui_layout *L = ui_lay(ui);
        L->row_h = 9;
    }
}

lt_bool lt_ui_submenu_begin(lt_ui *ui, const char *label, int flags)
{
    char vis[128];
    int mn_pos, mn_ch, depth = ui->menu_depth - 1, st, dis = (flags & LT_UI_MENU_DISABLED) != 0;
    int need, open, moved;
    lt_u32 id = lt_ui_id(ui, label);
    lt_rect r;
    int keyopen = 0, mods = 0;
    if (depth < 0 || depth + 1 >= UI_MAX_POPUPS) { return LT_FALSE; }
    amp_strip(label, vis, sizeof(vis), &mn_pos, &mn_ch);
    need = (ui->cell ? 16 : lt_ui_px(ui, 24)) + lt_ui_text_width(ui, vis) + 40;
    ui_popup_need(ui, need + (ui->cell ? 0 : lt_ui_px(ui, 6)));
    r = ui_next(ui, 0);
    st = ui_popup_item(ui, depth, r, dis);
    moved = (ui->mx != ui->lmx || ui->my != ui->lmy);
    open = (ui_popup_find(ui, id) == depth + 1);
    if (!dis && !open && depth == ui->npopups - 1) {
        if (((st & UI_ITEM_OVER) && moved) || ((st & UI_ITEM_HI) && (ui->menu_hi[depth] >= 0) &&
            ui_key_take(ui, LT_KEY_RIGHT, &mods))) {
            ui_popup_open(ui, depth + 1, id, 0, r, 1, r.x, r.y);
            ui->menu_hi[depth + 1] = (st & UI_ITEM_OVER) ? -1 : 0;
            keyopen = 1;
            open = 1;
        }
    } else if (!dis && open && depth == ui->npopups - 1 - 0) {
        /* child popup closed itself */
    }
    LT_UNUSED(keyopen);
    if ((st & UI_ITEM_OVER) && !dis && moved && ui->npopups > depth + 1 &&
        ui_popup_find(ui, id) != depth + 1) {
        ui->npopups = depth + 1;
    }
    item_draw(ui, r, st | (open ? UI_ITEM_HI : 0), dis, vis, NULL, flags, 1);
    ui_set_last(ui, id, r);
    if (!open) { return LT_FALSE; }
    ui->popups[depth + 1].anchor = r;
    ui_popup_begin(ui, depth + 1, ui->cell ? 24 : lt_ui_px(ui, 160), ui->theme.row_h * 3);
    ui->menu_depth = depth + 2;
    ui->menu_idx[depth + 1] = 0;
    return LT_TRUE;
}

void lt_ui_submenu_end(lt_ui *ui)
{
    int depth = ui->menu_depth - 1;
    ui_popup_end(ui, depth);
    ui->menu_depth = depth;
}

lt_bool lt_ui_context_begin(lt_ui *ui, const char *id, lt_rect area)
{
    lt_u32 cid = lt_ui_id(ui, id);
    int open = (ui_popup_find(ui, cid) == 0 && ui->popups[0].kind == 2);
    if (ui_over(ui, area) && ui_pressed(ui, LT_MB_RIGHT) && !ui->press_claimed) {
        ui_popup_open(ui, 0, cid, 2, lt_rect_make(ui->mx, ui->my, 0, 0), 0, ui->mx, ui->my);
        ui->press_claimed = 1;
        open = 1;
    }
    if (!open) { return LT_FALSE; }
    ui_popup_begin(ui, 0, ui->cell ? 24 : lt_ui_px(ui, 160), ui->theme.row_h * 3);
    ui->menu_depth = 1;
    ui->menu_idx[0] = 0;
    return LT_TRUE;
}

void lt_ui_context_end(lt_ui *ui)
{
    ui_popup_end(ui, 0);
    ui->menu_depth = 0;
}

void lt_ui_context_open(lt_ui *ui, const char *id, int x, int y)
{
    ui_popup_open(ui, 0, lt_ui_id(ui, id), 2, lt_rect_make(x, y, 0, 0), 0, x, y);
}

/* ------------------------------------------------------------------ */
/* combo box                                                           */
static const char *combo_arr_get(void *user, int i)
{
    return ((const char *const *)user)[i];
}

/* type-ahead over items; returns matching index or -1 and consumes text */
static int typeahead(lt_ui *ui, int n, const char *(*get)(void *, int), void *user, int cur)
{
    int i, found = -1, any = 0;
    for (i = 0; i < ui->nq; i++) {
        ui_evq *q = &ui->q[i];
        size_t len;
        int k, start;
        if (!q->is_text || q->used) { continue; }
        if ((unsigned char)q->text[0] < 32) { continue; }
        q->used = 1;
        any = 1;
        if (ui->now - ui->typeahead_t > 900.0) { ui->typeahead[0] = '\0'; }
        ui->typeahead_t = ui->now;
        len = strlen(ui->typeahead);
        if (len + strlen(q->text) < sizeof(ui->typeahead)) { lt_strlcat(ui->typeahead, q->text, sizeof(ui->typeahead)); }
        len = strlen(ui->typeahead);
        /* repeated single letter cycles through items */
        start = (len == 1) ? cur + 1 : (cur < 0 ? 0 : cur);
        for (k = 0; k < n; k++) {
            int j = (start + k) % n;
            const char *s = get(user, j);
            if (s != NULL && lt_strnicmp(s, ui->typeahead, len) == 0) { found = j; break; }
        }
        if (found < 0 && len > 1) {
            /* "aa" == repeated a: retry with last char only */
            ui->typeahead[0] = q->text[0]; ui->typeahead[1] = '\0';
        }
        if (found >= 0) { cur = found; }
    }
    LT_UNUSED(any);
    return found;
}

lt_bool lt_ui_combo_cb(lt_ui *ui, const char *label, int *sel, int n,
                       const char *(*get)(void *user, int i), void *user)
{
    lt_u32 id = lt_ui_id(ui, label);
    const lt_theme *t = &ui->theme;
    lt_rect r = ui_next(ui, 160);
    int res = ui_interact(ui, id, r, UI_FOCUSABLE | UI_KIND(UI_K_OTHER));
    int open = (ui_popup_find(ui, id) == 0 && ui->popups[0].kind == 1);
    int changed = 0;
    lt_color border, fill;
    const char *cur = (*sel >= 0 && *sel < n) ? get(user, *sel) : "";
    int mods = 0;

    if (n < 1) { n = 0; }
    if ((res & UI_CLICK) && !ui->disabled) {
        if (open) { ui->npopups = 0; open = 0; }
        else {
            ui_popup_open(ui, 0, id, 1, r, 0, r.x, r.y + r.h);
            ui->menu_hi[0] = *sel;
            ui->focus = id;
            open = 1;
        }
    }
    if (ui->focus == id && !ui->disabled && n > 0) {
        if (!open) {
            int t_idx = typeahead(ui, n, get, user, *sel);
            if (ui_key_take(ui, LT_KEY_DOWN, &mods)) {
                if (mods & LT_MOD_ALT) {
                    ui_popup_open(ui, 0, id, 1, r, 0, r.x, r.y + r.h); ui->menu_hi[0] = *sel; open = 1;
                } else if (*sel < n - 1) { (*sel)++; changed = 1; }
            } else if (ui_key_take(ui, LT_KEY_UP, &mods)) {
                if (*sel > 0) { (*sel)--; changed = 1; }
            } else if (ui_key_take(ui, ' ', &mods) || ui_key_take(ui, LT_KEY_ENTER, &mods)) {
                ui_popup_open(ui, 0, id, 1, r, 0, r.x, r.y + r.h);
                ui->menu_hi[0] = *sel;
                open = 1;
            }
            if (t_idx >= 0 && t_idx != *sel) { *sel = t_idx; changed = 1; }
        } else {
            int t_idx = typeahead(ui, n, get, user, ui->menu_hi[0]);
            if (t_idx >= 0) { ui->menu_hi[0] = t_idx; }
        }
    }
    fill = ui->disabled ? t->surface : t->input;
    border = (ui->focus == id || open) ? t->focus : ((res & UI_HOVER) ? t->border_strong : t->border);
    if (ui->cell && (ui->focus == id || open)) { fill = lt_color_mix(t->input, t->primary, 70); }
    ui_field_box(ui, r, fill, border, ui->focus == id && ui->kbd_nav);
    ui_text_in(ui, lt_rect_make(r.x + t->pad_x, r.y, r.w - t->pad_x - (ui->cell ? 16 : lt_ui_px(ui, 26)), r.h), cur,
               ui->disabled ? t->disabled : t->text, 0);
    if (ui->cell) {
        lt_ui_draw_icon(ui, lt_rect_make(r.x + r.w - 16, r.y, 16, r.h), LT_UI_ICON_ARROW_DOWN,
                        ui->disabled ? t->disabled : t->text2);
    } else {
        lt_ui_draw_lucide(ui, lt_rect_make(r.x + r.w - lt_ui_px(ui, 22), r.y, lt_ui_px(ui, 18), r.h),
                          LT_ICON_CHEVRON_DOWN, 14, 2.0, ui->disabled ? t->disabled : t->text2);
    }
    ui_set_last(ui, id, r);
    if (open) {
        int rows = n < 10 ? n : 10, i;
        int hb = rows * t->row_h;
        ui_popup_begin(ui, 0, r.w, hb + lt_ui_px(ui, 8));
        ui->menu_depth = 1;
        ui->menu_idx[0] = 0;
        if (n > 10) {
            int w1 = -1;
            ui_layout_row_dev(ui, 1, &w1, hb);
            lt_ui_push_id(ui, "combo");
            lt_ui_scroll_begin(ui, "items");
        }
        for (i = 0; i < n; i++) {
            const char *s = get(user, i);
            lt_rect ir = ui_next(ui, 0);
            int st = ui_popup_item(ui, 0, ir, 0);
            lt_color fg = t->text;
            lt_rect hr = ui->cell ? ir : lt_rect_inset(ir, 2, 1);
            ui_popup_need(ui, lt_ui_text_width(ui, s) + 2 * t->pad_x + (n > 10 ? t->scrollbar_w : 0) + 8);
            if (st & UI_ITEM_HI) {
                lt_ui_draw_box(ui, hr, ui->cell ? t->primary_active : t->selected, 0, t->radius);
                if (!ui->cell) { fg = t->primary_text; }
                if (ui->menu_hi[0] == i && !(ui->mx != ui->lmx || ui->my != ui->lmy) && n > 10) {
                    lt_ui_scroll_ensure_visible(ui, ir);
                }
            } else if (i == *sel) {
                lt_ui_draw_box(ui, hr, ui->cell ? t->primary_active : t->hover, 0, t->radius);
            }
            ui_text_in(ui, lt_rect_make(ir.x + t->pad_x, ir.y, ir.w - 2 * t->pad_x, ir.h), s, fg, 0);
            if (st & UI_ITEM_ACT) {
                if (*sel != i) { *sel = i; changed = 1; }
                ui->npopups = 0;
                ui->focus = id;
                ui_request(ui);
            }
        }
        if (n > 10) {
            lt_ui_scroll_end(ui);
            lt_ui_pop_id(ui);
        }
        ui_popup_end(ui, 0);
        ui->menu_depth = 0;
    }
    return changed ? LT_TRUE : LT_FALSE;
}

lt_bool lt_ui_combo(lt_ui *ui, const char *id, int *sel, const char *const *items, int n)
{
    return lt_ui_combo_cb(ui, id, sel, n, combo_arr_get, (void *)items);
}

/* ------------------------------------------------------------------ */
/* list box                                                            */
int lt_ui_listbox_cb(lt_ui *ui, const char *label, int *sel, int n,
                     const char *(*get)(void *user, int i), void *user)
{
    lt_u32 lid;
    const lt_theme *t = &ui->theme;
    int res = 0, rh = t->row_h, i, first, last, focused, mods = 0;
    lt_rect view, body;
    int sy;

    lt_ui_push_id(ui, label);
    lid = lt_ui_id(ui, "##listfocus");
    if (!lt_ui_scroll_begin(ui, "##scroll")) {
        lt_ui_pop_id(ui);
        return 0;
    }
    view = lt_ui_scroll_view(ui);
    lt_ui_scroll_set_content(ui, 0, n * rh);
    ui_focusable(ui, lid, UI_K_LIST);
    focused = (ui->focus == lid);
    /* keyboard */
    if (focused && !ui->disabled && n > 0) {
        int old = *sel, page = LT_MAX(1, view.h / rh - 1);
        int tsel = typeahead(ui, n, get, user, *sel);
        if (ui_key_take(ui, LT_KEY_DOWN, &mods)) { *sel = old < 0 ? 0 : LT_MIN(n - 1, old + 1); }
        else if (ui_key_take(ui, LT_KEY_UP, &mods)) { *sel = old < 0 ? 0 : LT_MAX(0, old - 1); }
        else if (ui_key_take(ui, LT_KEY_PAGEDOWN, &mods)) { *sel = LT_MIN(n - 1, LT_MAX(0, old) + page); }
        else if (ui_key_take(ui, LT_KEY_PAGEUP, &mods)) { *sel = LT_MAX(0, old - page); }
        else if (ui_key_take(ui, LT_KEY_HOME, &mods)) { *sel = 0; }
        else if (ui_key_take(ui, LT_KEY_END, &mods)) { *sel = n - 1; }
        else if (tsel >= 0) { *sel = tsel; }
        if (*sel != old) { res |= LT_UI_CHANGED; }
        if (ui_key_take(ui, LT_KEY_ENTER, &mods) && *sel >= 0) { res |= LT_UI_ACTIVATED; }
        if (*sel != old && *sel >= 0) {
            lt_ui_scroll_get(ui, NULL, &sy);
            body = lt_ui_layout_body(ui);
            lt_ui_scroll_ensure_visible(ui, lt_rect_make(body.x, body.y + *sel * rh, body.w, rh));
        }
    }
    lt_ui_scroll_get(ui, NULL, &sy);
    body = lt_ui_layout_body(ui);
    first = (view.y - body.y) / rh;
    last = (view.y + view.h - body.y) / rh;
    if (first < 0) { first = 0; }
    if (last >= n) { last = n - 1; }
    /* background */
    lt_ui_draw_fill(ui, view, t->input);
    for (i = first; i <= last; i++) {
        lt_rect ir = lt_rect_make(body.x, body.y + i * rh, body.w, rh);
        lt_u32 rid;
        int ir_res;
        const char *s = get(user, i);
        lt_color bg = 0, fg = t->text;
        lt_ui_push_id_int(ui, i);
        rid = lt_ui_id(ui, "##row");
        ir_res = ui_interact(ui, rid, ir, 0);
        lt_ui_pop_id(ui);
        if (ir_res & UI_PRESS) {
            ui->focus = lid; ui->kbd_nav = 0;
            if (*sel != i) { *sel = i; res |= LT_UI_CHANGED; }
            if (ir_res & UI_DBLCLICK) { res |= LT_UI_ACTIVATED; }
        }
        if (i == *sel) {
            bg = focused ? t->selected : lt_color_mix(t->selected, t->input, 120);
            if (ui->cell) { bg = focused ? t->primary_active : t->selected; }
        } else if (ir_res & UI_HOVER) {
            bg = t->hover;
        }
        if (bg != 0) { lt_ui_draw_box(ui, lt_rect_inset(ir, ui->cell ? 0 : lt_ui_px(ui, 2), ui->cell ? 0 : 1), bg, 0, 3); }
        if (s[0] == '\x01' || s[0] == '\x02') {
            lt_ui_draw_icon(ui, lt_rect_make(ir.x + t->pad_x, ir.y, ui->cell ? 16 : lt_ui_px(ui, 20), ir.h),
                            s[0] == '\x01' ? LT_UI_ICON_FOLDER : LT_UI_ICON_FILE,
                            s[0] == '\x01' ? t->accent : t->text2);
            s++;
            ui_text_in(ui, lt_rect_make(ir.x + t->pad_x + (ui->cell ? 16 : lt_ui_px(ui, 24)), ir.y,
                                        ir.w - 2 * t->pad_x - (ui->cell ? 16 : lt_ui_px(ui, 24)), ir.h), s, fg, 0);
        } else {
            ui_text_in(ui, lt_rect_make(ir.x + t->pad_x, ir.y, ir.w - 2 * t->pad_x, ir.h), s, fg, 0);
        }
    }
    if (ui_over(ui, view) && ui->hover == 0 && ui_pressed(ui, LT_MB_LEFT)) {
        ui->focus = lid; ui->kbd_nav = 0; ui->press_claimed = 1;
    }
    lt_ui_scroll_end(ui);
    if (!ui->cell) { lt_ui_draw_rect(ui, view, focused ? t->focus : t->border_subtle); }
    lt_ui_pop_id(ui);
    ui_set_last(ui, lid, view);
    return res;
}

int lt_ui_listbox(lt_ui *ui, const char *id, int *sel, const char *const *items, int n)
{
    return lt_ui_listbox_cb(ui, id, sel, n, combo_arr_get, (void *)items);
}

/* ------------------------------------------------------------------ */
/* colour editor                                                       */
lt_bool lt_ui_color_edit(lt_ui *ui, const char *label, lt_color *c)
{
    lt_u32 id = lt_ui_id(ui, label);
    lt_rect r;
    int open, changed = 0, depth = 0;
    lt_ui_push_id(ui, label);
    if (lt_ui_color_swatch(ui, *c)) {
        ui_popup_open(ui, 0, id, 3, ui->last_rect, 0, ui->last_rect.x, ui->last_rect.y + ui->last_rect.h);
    }
    r = ui->last_rect;
    open = (ui_popup_find(ui, id) == 0 && ui->popups[0].kind == 3);
    if (open) {
        static const char *names[3] = { "R", "G", "B" };
        double v[3];
        int k, w[2];
        char hex[16];
        ui->popups[0].anchor = r;
        v[0] = LT_COLOR_R(*c); v[1] = LT_COLOR_G(*c); v[2] = LT_COLOR_B(*c);
        ui_popup_begin(ui, 0, ui->cell ? 200 : lt_ui_px(ui, 230), ui->theme.row_h * 4 + lt_ui_px(ui, 12));
        ui->menu_depth = 1;
        w[0] = ui->cell ? 16 : 18; w[1] = -1;
        for (k = 0; k < 3; k++) {
            lt_ui_layout_row(ui, 2, w, 0);
            lt_ui_label(ui, names[k]);
            if (lt_ui_slider(ui, names[k], &v[k], 0.0, 255.0, 0)) { changed = 1; }
        }
        lt_snprintf(hex, sizeof(hex), "#%02X%02X%02X", (int)v[0], (int)v[1], (int)v[2]);
        lt_ui_layout_row(ui, 1, NULL, 0);
        lt_ui_label_ex(ui, hex, ui->theme.muted, LT_UI_ALIGN_CENTER);
        ui_popup_need(ui, ui->cell ? 200 : lt_ui_px(ui, 230));
        ui_popup_end(ui, depth);
        ui->menu_depth = 0;
        if (changed) { *c = LT_RGB((int)v[0], (int)v[1], (int)v[2]); }
    }
    lt_ui_pop_id(ui);
    ui_set_last(ui, id, r);
    return changed ? LT_TRUE : LT_FALSE;
}
