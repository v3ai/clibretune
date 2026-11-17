/* ui_container.c - scroll areas, groups, regions, splitters, floating
 * windows, modal dialogs, message boxes, tree view and tab bar.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "ui_priv.h"

/* ------------------------------------------------------------------ */
/* scroll areas                                                        */
lt_bool lt_ui_scroll_begin(lt_ui *ui, const char *id)
{
    lt_u32 sid;
    ui_state *st;
    ui_scroll *S;
    lt_rect r, inner;
    int sbw = ui->theme.scrollbar_w, maxx, maxy;
    int fill_top = 0;
    if (ui->nscr >= UI_MAX_SCROLL) { return LT_FALSE; }
    if (ui->cell && ui->nmodal > 0 && ui->nlay - 1 == ui->modal_lay[ui->nmodal - 1]) {
        const ui_layout *ML = ui_lay(ui);
        fill_top = ML->col >= ML->ncols ? ML->spec_h < 0 : 0;
    }
    r = ui_next(ui, 0);
    lt_ui_push_id(ui, id);
    sid = lt_ui_id(ui, "##scrollstate");
    st = ui_state_get(ui, sid);
    S = &ui->scr[ui->nscr++];
    memset(S, 0, sizeof(*S));
    S->fill_top = fill_top;
    S->id = sid;
    S->view = r;
    S->sx = (int)st->d[0]; S->sy = (int)st->d[1];
    S->cw = st->i[0]; S->ch = st->i[1];
    inner = r;
    S->vbar = S->ch > inner.h;
    if (S->vbar) { inner.w -= sbw; }
    S->hbar = S->cw > inner.w;
    if (S->hbar) {
        inner.h -= sbw;
        if (!S->vbar && S->ch > inner.h) { S->vbar = 1; inner.w -= sbw; }
    }
    if (inner.w < 1) { inner.w = 1; }
    if (inner.h < 1) { inner.h = 1; }
    maxx = LT_MAX(0, S->cw - inner.w);
    maxy = LT_MAX(0, S->ch - inner.h);
    S->sx = LT_CLAMP(S->sx, 0, maxx);
    S->sy = LT_CLAMP(S->sy, 0, maxy);
    S->inner = inner;
    lt_ui_clip_push(ui, inner);
    ui_layout_push(ui, lt_rect_make(inner.x - S->sx, inner.y - S->sy, inner.w, inner.h));
    return LT_TRUE;
}

static ui_scroll *scr_top(lt_ui *ui) { return ui->nscr > 0 ? &ui->scr[ui->nscr - 1] : NULL; }

void lt_ui_scroll_set_content(lt_ui *ui, int w, int h)
{
    ui_scroll *S = scr_top(ui);
    if (S == NULL) { return; }
    S->ovr = 1; S->ovr_w = w; S->ovr_h = h;
}

void lt_ui_scroll_get(const lt_ui *ui, int *x, int *y)
{
    const ui_scroll *S = ui->nscr > 0 ? &ui->scr[ui->nscr - 1] : NULL;
    if (x != NULL) { *x = S ? S->sx : 0; }
    if (y != NULL) { *y = S ? S->sy : 0; }
}

void lt_ui_scroll_set(lt_ui *ui, int x, int y)
{
    ui_scroll *S = scr_top(ui);
    if (S == NULL) { return; }
    S->sx = x; S->sy = y;
    ui_request(ui);
}

lt_rect lt_ui_scroll_view(const lt_ui *ui)
{
    if (ui->nscr == 0) { return lt_rect_make(0, 0, 0, 0); }
    return ui->scr[ui->nscr - 1].view;
}

void lt_ui_scroll_ensure_visible(lt_ui *ui, lt_rect r)
{
    ui_scroll *S = scr_top(ui);
    int dy = 0, dx = 0;
    if (S == NULL) { return; }
    if (r.y < S->inner.y) { dy = r.y - S->inner.y; }
    else if (r.y + r.h > S->inner.y + S->inner.h) { dy = r.y + r.h - (S->inner.y + S->inner.h); }
    if (r.x < S->inner.x) { dx = r.x - S->inner.x; }
    else if (r.x + r.w > S->inner.x + S->inner.w) { dx = r.x + r.w - (S->inner.x + S->inner.w); }
    if (dx != 0 || dy != 0) {
        S->sx += dx; S->sy += dy;
        if (S->sx < 0) { S->sx = 0; }
        if (S->sy < 0) { S->sy = 0; }
        ui_request(ui);
    }
}

/* one scroll bar; returns new scroll offset */
static int scrollbar(lt_ui *ui, lt_u32 sid, int vertical, lt_rect track, int pos, int content,
                     int view)
{
    const lt_theme *t = &ui->theme;
    ui_state *st = ui_state_get(ui, sid ^ (vertical ? 0x76u : 0x68u));
    int range = content - view, tlen = vertical ? track.h : track.w;
    int thumb = (int)((double)tlen * (double)view / (double)content);
    int tpos, maxpos, res;
    lt_rect tr;
    lt_u32 id = sid ^ (vertical ? 0xB076u : 0xB068u);
    lt_color col;
    if (thumb < 24) { thumb = ui->cell ? 16 : lt_ui_px(ui, 24); }
    if (thumb > tlen) { thumb = tlen; }
    maxpos = tlen - thumb;
    tpos = range > 0 ? (int)((double)pos / (double)range * (double)maxpos) : 0;
    tr = vertical ? lt_rect_make(track.x, track.y + tpos, track.w, thumb)
                  : lt_rect_make(track.x + tpos, track.y, thumb, track.h);
    res = ui_interact(ui, id, tr, 0);
    if (res & UI_PRESS) { st->d[0] = (double)((vertical ? ui->my : ui->mx) - (vertical ? tr.y : tr.x)); }
    if ((res & UI_ACTIVE) && maxpos > 0) {
        int np = (vertical ? ui->my : ui->mx) - (int)st->d[0] - (vertical ? track.y : track.x);
        np = LT_CLAMP(np, 0, maxpos);
        pos = (int)((double)np / (double)maxpos * (double)range);
        tpos = np;
        tr = vertical ? lt_rect_make(track.x, track.y + tpos, track.w, thumb)
                      : lt_rect_make(track.x + tpos, track.y, thumb, track.h);
    } else if (!(res & UI_ACTIVE) && ui_over(ui, track) && !(res & UI_HOVER) &&
               ui_pressed(ui, LT_MB_LEFT)) {
        /* click on the track: page */
        int m = vertical ? ui->my : ui->mx, tp = vertical ? tr.y : tr.x;
        pos += (m < tp ? -view : view);
        ui->press_claimed = 1;
        pos = LT_CLAMP(pos, 0, range);
    }
    lt_ui_draw_fill(ui, track, t->scroll_track);
    col = (res & UI_ACTIVE) ? t->muted : ((res & UI_HOVER) ? t->scroll_hover : t->scroll_thumb);
    if (ui->cell) { lt_ui_draw_fill(ui, tr, col); }
    else { lt_ui_draw_box(ui, lt_rect_inset(tr, vertical ? 1 : 0, vertical ? 0 : 1), col, 0, 2); }
    return pos;
}

void lt_ui_scroll_end(lt_ui *ui)
{
    ui_scroll *S = scr_top(ui);
    ui_layout *LY;
    ui_state *st;
    int cw, ch, maxx, maxy, wheel_step;
    const lt_theme *t = &ui->theme;
    lt_rect view;
    int oldx, oldy;
    if (S == NULL) { return; }
    LY = ui_lay(ui);
    cw = S->ovr ? S->ovr_w : LY->max_x - LY->body.x;
    ch = S->ovr ? S->ovr_h : LY->max_y - LY->body.y;
    view = S->view;
    if (S->fill_top && ui->nmodal > 0) {
        int m = ui->nmodal - 1, slack = S->inner.h - ch;
        ui->modal_slack[m] = ui->modal_slackn[m] == 0 ? slack : LT_MIN(ui->modal_slack[m], slack);
        ui->modal_slackn[m]++;
    }
    ui_layout_pop(ui);
    lt_ui_clip_pop(ui);
    oldx = S->sx; oldy = S->sy;
    maxx = LT_MAX(0, cw - S->inner.w);
    maxy = LT_MAX(0, ch - S->inner.h);
    /* wheel: only when the area can scroll that way */
    wheel_step = t->row_h * 2;
    if (ui->wheel != 0 && ui_over(ui, view)) {
        if ((ui->mods & LT_MOD_SHIFT) && maxx > 0) {
            int nx = LT_CLAMP(S->sx - ui->wheel * wheel_step, 0, maxx);
            if (nx != S->sx) { S->sx = nx; ui->wheel = 0; }
        } else if (maxy > 0) {
            int ny = LT_CLAMP(S->sy - ui->wheel * wheel_step, 0, maxy);
            if (ny != S->sy) { S->sy = ny; ui->wheel = 0; }
        }
    }
    if (S->vbar && ch > S->inner.h) {
        lt_rect tr = lt_rect_make(view.x + view.w - t->scrollbar_w, view.y, t->scrollbar_w, S->inner.h);
        S->sy = scrollbar(ui, S->id, 1, tr, S->sy, ch, S->inner.h);
    }
    if (S->hbar && cw > S->inner.w) {
        lt_rect tr = lt_rect_make(view.x, view.y + view.h - t->scrollbar_w, S->inner.w, t->scrollbar_w);
        S->sx = scrollbar(ui, S->id, 0, tr, S->sx, cw, S->inner.w);
    }
    if (S->vbar && S->hbar) {
        lt_ui_draw_fill(ui, lt_rect_make(view.x + view.w - t->scrollbar_w, view.y + view.h - t->scrollbar_w,
                                         t->scrollbar_w, t->scrollbar_w), t->scroll_track);
    }
    S->sx = LT_CLAMP(S->sx, 0, maxx);
    S->sy = LT_CLAMP(S->sy, 0, maxy);
    if (ui->cell) { S->sx = (S->sx / 8) * 8; S->sy = (S->sy / 16) * 16; }
    st = ui_state_get(ui, S->id);
    if (st->i[0] != cw || st->i[1] != ch || S->sx != oldx || S->sy != oldy) { ui_request(ui); }
    st->d[0] = S->sx; st->d[1] = S->sy; st->i[0] = cw; st->i[1] = ch;
    ui->nscr--;
    lt_ui_pop_id(ui);
}

/* ------------------------------------------------------------------ */
/* group / region / splitter                                           */
lt_bool lt_ui_group_begin(lt_ui *ui, const char *title, int height)
{
    const lt_theme *t = &ui->theme;
    ui_layout *L = ui_lay(ui);
    lt_rect r;
    ui_state *st;
    ui_grp *G;
    lt_u32 id;
    int hdr, pad;
    if (ui->ngrp >= 8) { return LT_FALSE; }
    id = lt_ui_id(ui, title);
    st = ui_state_get(ui, id ^ 0x6770UL);
    r = ui_next(ui, 0);
    if (height > 0) { height = lt_ui_px(ui, height); }
    if (height <= 0) {
        int prev = st->i[0] > 0 ? st->i[0] : t->row_h * 2;
        r.h = prev;
        if (L->row_h < prev) { L->row_h = prev; }
    } else {
        r.h = height;
        L->row_h = LT_MAX(L->row_h, height);
        if (L->col >= L->ncols) { L->row_h = height; }
    }
    if (r.y + r.h > L->max_y) { L->max_y = r.y + r.h; }
    G = &ui->grp[ui->ngrp++];
    G->id = id; G->r = r; G->auto_h = height <= 0; G->hdr = 0;
    pad = ui->cell ? 8 : t->padding + lt_ui_px(ui, 2);
    hdr = (title[0] != '\0' && strncmp(title, "##", 2) != 0) ? (ui->cell ? t->row_h : t->row_h + lt_ui_px(ui, 2)) : (ui->cell ? 0 : lt_ui_px(ui, 4));
    G->hdr = hdr;
    if (ui->cell) {
        lt_ui_draw_fill(ui, r, t->surface);
    } else {
        lt_ui_draw_box(ui, r, t->surface, t->border_subtle, t->radius_lg);
    }
    if (hdr > 4) {
        char vis[160], up[160];
        const char *v = ui_label_visible(title, vis, sizeof(vis));
        size_t k;
        lt_strlcpy(up, v, sizeof(up));
        if (!ui->cell) {
            for (k = 0; up[k] != '\0'; k++) {
                if (up[k] >= 'a' && up[k] <= 'z') { up[k] = (char)(up[k] - 32); }
            }
            lt_ui_draw_box(ui, lt_rect_make(r.x + 1, r.y + 1, r.w - 2, hdr - 1), lt_color_mix(t->surface, t->active, 128),
                           0, t->radius_lg - 1);
            lt_ui_draw_fill(ui, lt_rect_make(r.x + 1, r.y + hdr / 2, r.w - 2, hdr - hdr / 2), lt_color_mix(t->surface, t->active, 128));
            lt_ui_draw_hline(ui, r.x + 1, r.x + r.w - 2, r.y + hdr, t->border_subtle);
        }
        ui_text_in_f(ui, lt_rect_make(r.x + pad, r.y, r.w - 2 * pad, hdr), ui->cell ? v : up,
                     ui->cell ? t->primary_hover : t->text2, 0, (int)LT_FONT_SANS_SEMIBOLD, LT_UI_FS_SM);
    }
    lt_ui_push_id(ui, title);
    lt_ui_clip_push(ui, r);
    ui_layout_push(ui, lt_rect_make(r.x + pad, r.y + hdr + (ui->cell ? 0 : lt_ui_px(ui, 4)), r.w - 2 * pad,
                                    r.h - hdr - (ui->cell ? 0 : lt_ui_px(ui, 8))));
    return LT_TRUE;
}

void lt_ui_group_end(lt_ui *ui)
{
    ui_grp *G;
    ui_layout *LY;
    ui_state *st;
    int h;
    if (ui->ngrp <= 0) { return; }
    G = &ui->grp[--ui->ngrp];
    LY = ui_lay(ui);
    h = LY->max_y - LY->body.y + G->hdr + (ui->cell ? 0 : lt_ui_px(ui, 12));
    if (ui->cell) { h = ((h + 15) / 16) * 16; }
    ui_layout_pop(ui);
    lt_ui_clip_pop(ui);
    lt_ui_pop_id(ui);
    st = ui_state_get(ui, G->id ^ 0x6770UL);
    if (G->auto_h && st->i[0] != h) { st->i[0] = h; ui_request(ui); }
}

void lt_ui_region_begin(lt_ui *ui, lt_rect r, lt_color bg)
{
    if (LT_COLOR_A(bg) != 0) { lt_ui_draw_fill(ui, r, bg); }
    lt_ui_clip_push(ui, r);
    ui_layout_push(ui, r);
}

void lt_ui_region_end(lt_ui *ui)
{
    ui_layout_pop(ui);
    lt_ui_clip_pop(ui);
}

lt_bool lt_ui_splitter(lt_ui *ui, const char *label, lt_rect area, int vertical, int *size,
                       int min_a, int min_b, lt_rect *a, lt_rect *b)
{
    const lt_theme *t = &ui->theme;
    lt_u32 id = lt_ui_id(ui, label);
    ui_state *st = ui_state_get(ui, id);
    int sw = t->splitter_w, total = vertical ? area.w : area.h, changed = 0, res;
    lt_rect bar;
    int old = *size;
    int maxs = total - min_b - sw;
    if (maxs < min_a) { maxs = min_a; }
    *size = LT_CLAMP(*size, min_a, maxs);
    bar = vertical ? lt_rect_make(area.x + *size, area.y, sw, area.h)
                   : lt_rect_make(area.x, area.y + *size, area.w, sw);
    res = ui_interact(ui, id, bar, 0);
    if (res & UI_PRESS) { st->d[0] = *size; st->d[1] = vertical ? ui->mx : ui->my; }
    if (res & UI_ACTIVE) {
        double d = (vertical ? ui->mx : ui->my) - st->d[1];
        *size = LT_CLAMP((int)(st->d[0] + d), min_a, maxs);
        if (ui->cell) { *size = (*size / (vertical ? 8 : 16)) * (vertical ? 8 : 16); }
        bar = vertical ? lt_rect_make(area.x + *size, area.y, sw, area.h)
                       : lt_rect_make(area.x, area.y + *size, area.w, sw);
    }
    if (*size != old) { changed = 1; }
    {
        lt_color c = (res & UI_ACTIVE) ? t->primary : ((res & UI_HOVER) ? t->primary_hover : t->border_subtle);
        if (ui->cell) {
            if (res & (UI_HOVER | UI_ACTIVE)) { lt_ui_draw_fill(ui, bar, t->hover); }
            if (vertical) { lt_ui_draw_vline(ui, bar.x + bar.w / 2, bar.y, bar.y + bar.h - 1, c); }
            else { lt_ui_draw_hline(ui, bar.x, bar.x + bar.w - 1, bar.y + bar.h / 2, c); }
        } else if (vertical) {
            lt_ui_draw_fill(ui, lt_rect_make(bar.x + bar.w / 2 - 1, bar.y, (res & (UI_HOVER | UI_ACTIVE)) ? 2 : 1,
                                             bar.h), c);
        } else {
            lt_ui_draw_fill(ui, lt_rect_make(bar.x, bar.y + bar.h / 2 - 1, bar.w,
                                             (res & (UI_HOVER | UI_ACTIVE)) ? 2 : 1), c);
        }
    }
    if (a != NULL) {
        *a = vertical ? lt_rect_make(area.x, area.y, *size, area.h)
                      : lt_rect_make(area.x, area.y, area.w, *size);
    }
    if (b != NULL) {
        *b = vertical ? lt_rect_make(area.x + *size + sw, area.y, total - *size - sw, area.h)
                      : lt_rect_make(area.x, area.y + *size + sw, area.w, total - *size - sw);
    }
    ui_set_last(ui, id, bar);
    return changed ? LT_TRUE : LT_FALSE;
}

/* ------------------------------------------------------------------ */
/* floating windows                                                    */
static void draw_window_frame(lt_ui *ui, lt_rect r, const char *title, int show_close,
                              lt_rect *close_out, int title_h)
{
    const lt_theme *t = &ui->theme;
    lt_rect tb = lt_rect_make(r.x, r.y, r.w, title_h);
    int rl = t->radius_lg;
    if (!ui->cell) {
        lt_ui_draw_shadow(ui, r, rl, lt_ui_px(ui, 18), 90);
        lt_ui_draw_box(ui, r, t->surface, t->border, rl);
        lt_ui_draw_box(ui, lt_rect_make(r.x + 1, r.y + 1, r.w - 2, title_h - 1), t->elevated, 0, rl - 1);
        lt_ui_draw_fill(ui, lt_rect_make(r.x + 1, r.y + title_h - 6, r.w - 2, 6), t->elevated);
        lt_ui_draw_hline(ui, r.x + 1, r.x + r.w - 2, r.y + title_h, t->border_subtle);
    } else {
        /* text-mode window: rounded frame with the title set into the top edge */
        char cap[200];
        ui_cell_shadow(ui, r);
        lt_ui_draw_fill(ui, r, t->surface);
        lt_ui_draw_rect(ui, r, t->focus);
        lt_snprintf(cap, sizeof(cap), " %s ", title);
        ui_text_in_f(ui, lt_rect_make(tb.x + 16, tb.y, tb.w - 64, tb.h), cap, t->text, 0,
                     (int)LT_FONT_SANS_SEMIBOLD, LT_UI_FS_LG);
        ui_cell_bold(ui, lt_rect_make(tb.x + 16, tb.y, (int)strlen(cap) * 8, tb.h));
        if (show_close) { *close_out = lt_rect_make(tb.x + tb.w - 32, tb.y, 24, tb.h); }
        return;
    }
    ui_text_in_f(ui, lt_rect_make(tb.x + lt_ui_px(ui, 16), tb.y, tb.w - 2 * t->pad_x - 30, tb.h), title,
                 t->text, 0, (int)LT_FONT_SANS_SEMIBOLD, LT_UI_FS_LG);
    if (show_close) {
        int cs = lt_ui_px(ui, 24);
        *close_out = lt_rect_make(tb.x + tb.w - (cs + lt_ui_px(ui, 10)), tb.y + (tb.h - cs) / 2, cs, cs);
    }
}

lt_bool lt_ui_window_begin(lt_ui *ui, const char *title, lt_rect *r, int *open, int flags)
{
    const lt_theme *t = &ui->theme;
    char vis[160];
    const char *v = ui_label_visible(title, vis, sizeof(vis));
    lt_u32 id = lt_ui_id(ui, title) ^ 0x57494EUL;
    int i, rank = -1, z, pad = ui->cell ? 16 : t->padding;
    ui_layer *L;
    lt_rect close = lt_rect_make(0, 0, 0, 0), tb, grip;
    int res, th = t->title_h;
    if (open != NULL && !*open) { return LT_FALSE; }
    for (i = 0; i < ui->nwin; i++) {
        if (ui->win_order[i] == id) { rank = i; }
    }
    if (rank < 0 && ui->nwin < 24) { rank = ui->nwin; ui->win_order[ui->nwin++] = id; }
    if (rank >= 0 && ui->hover_layer == id && (ui->mpressed & (1 << LT_MB_LEFT)) && rank != ui->nwin - 1) {
        for (i = rank; i < ui->nwin - 1; i++) { ui->win_order[i] = ui->win_order[i + 1]; }
        ui->win_order[ui->nwin - 1] = id;
        rank = ui->nwin - 1;
    }
    z = 100 + 10 * (rank < 0 ? 0 : rank);
    L = ui_layer_begin(ui, id, UL_WINDOW, z, *r);
    LT_UNUSED(L);
    tb = lt_rect_make(r->x, r->y, r->w, th);
    /* title bar drag + close + resize before drawing so there is no lag */
    if (!(flags & LT_UI_WIN_NOCLOSE)) {
        int cs = lt_ui_px(ui, 24);
        close = lt_rect_make(tb.x + tb.w - (ui->cell ? 32 : cs + lt_ui_px(ui, 10)), tb.y + (ui->cell ? 0 : (tb.h - cs) / 2),
                             ui->cell ? 24 : cs, ui->cell ? tb.h : cs);
        res = ui_interact(ui, id ^ 0xC10EUL, close, 0);
        if ((res & UI_CLICK) && open != NULL) { *open = 0; ui_request(ui); }
    }
    if (!(flags & LT_UI_WIN_NOMOVE)) {
        ui_state *st = ui_state_get(ui, id ^ 0xD2A6UL);
        res = ui_interact(ui, id ^ 0xD2A6UL, tb, 0);
        if (res & UI_PRESS) { st->d[0] = ui->mx - r->x; st->d[1] = ui->my - r->y; }
        if (res & UI_ACTIVE) {
            r->x = LT_CLAMP(ui->mx - (int)st->d[0], -r->w + 64, ui->sw - 64);
            r->y = LT_CLAMP(ui->my - (int)st->d[1], 0, ui->sh - th);
            if (ui->cell) { r->x = (r->x / 8) * 8; r->y = (r->y / 16) * 16; }
        }
    }
    if (flags & LT_UI_WIN_RESIZE) {
        ui_state *st = ui_state_get(ui, id ^ 0x5E5UL);
        grip = lt_rect_make(r->x + r->w - 16, r->y + r->h - 16, 16, 16);
        res = ui_interact(ui, id ^ 0x5E5UL, grip, 0);
        if (res & UI_PRESS) { st->d[0] = r->x + r->w - ui->mx; st->d[1] = r->y + r->h - ui->my; }
        if (res & UI_ACTIVE) {
            r->w = LT_MAX(160, ui->mx - r->x + (int)st->d[0]);
            r->h = LT_MAX(th + 48, ui->my - r->y + (int)st->d[1]);
            if (ui->cell) { r->w = (r->w / 8) * 8; r->h = (r->h / 16) * 16; }
        }
    }
    ui_layer_cur(ui)->rect = *r;
    draw_window_frame(ui, *r, v, !(flags & LT_UI_WIN_NOCLOSE), &close, th);
    if (!(flags & LT_UI_WIN_NOCLOSE)) {
        int ch = ui_over(ui, close);
        if (ch && !ui->cell) { lt_ui_draw_box(ui, close, t->hover, 0, t->radius); }
        lt_ui_draw_icon(ui, close, LT_UI_ICON_CLOSE, ch ? t->text : t->muted);
    }
    if (flags & LT_UI_WIN_RESIZE) {
        int k;
        for (k = 0; k < 3 && !ui->cell; k++) {
            lt_ui_draw_line(ui, r->x + r->w - 4 - k * 4, r->y + r->h - 2, r->x + r->w - 2, r->y + r->h - 4 - k * 4,
                            1.0, t->muted);
        }
    }
    lt_ui_push_id(ui, title);
    lt_ui_clip_push(ui, ui->cell ? lt_rect_make(r->x + 8, r->y + th, r->w - 16, r->h - th - 16)
                                 : lt_rect_make(r->x + 1, r->y + th + 1, r->w - 2, r->h - th - 2));
    ui_layout_push(ui, lt_rect_make(r->x + pad, r->y + th + (ui->cell ? 0 : lt_ui_px(ui, 6)), r->w - 2 * pad,
                                    r->h - th - (ui->cell ? 16 : lt_ui_px(ui, 12))));
    return LT_TRUE;
}

void lt_ui_window_end(lt_ui *ui)
{
    ui_layout_pop(ui);
    lt_ui_clip_pop(ui);
    lt_ui_pop_id(ui);
    ui_layer_end(ui);
}

/* ------------------------------------------------------------------ */
/* modal dialogs                                                       */
lt_bool lt_ui_modal_begin(lt_ui *ui, const char *title, int w, int h, int *open)
{
    return ui_modal_begin_dev(ui, title, lt_ui_px(ui, w), lt_ui_px(ui, h), open);
}

lt_bool ui_modal_begin_dev(lt_ui *ui, const char *title, int w, int h, int *open)
{
    const lt_theme *t = &ui->theme;
    char vis[160];
    const char *v = ui_label_visible(title, vis, sizeof(vis));
    lt_u32 id = lt_ui_id(ui, title) ^ 0x4D4F44UL;
    ui_state *st;
    lt_rect dlg, close;
    int z, res, pad = ui->cell ? 16 : t->padding + lt_ui_px(ui, 8), th = t->title_h, fresh;
    if (open == NULL || !*open || ui->nmodal >= 4) { return LT_FALSE; }
    st = ui_state_get(ui, id);
    fresh = (st->i[0] != ui->frame_no - 1 && st->i[0] != ui->frame_no);
    st->i[0] = ui->frame_no;
    z = 2000 + 20 * ui->modal_seq++;
    if (ui->cell && !fresh && st->d[1] > 0.0) {
        /* shrink to the content measured on earlier frames (grows back if it needs more) */
        int need = (th + (int)st->d[0] + 32 + 15) / 16 * 16;
        if (need < th + 64) { need = th + 64; }
        if (need < h) { h = need; }
    }
    if (ui->cell) { w = LT_MIN(w * 6 / 5, ui->sw - 32); }    /* pixel designs assume proportional text */
    w = LT_MIN(w, ui->sw); h = LT_MIN(h, ui->sh);
    dlg = lt_rect_make((ui->sw - w) / 2, (ui->sh - h) / 2, w, h);
    if (ui->cell) { dlg.x = (dlg.x / 8) * 8; dlg.y = (dlg.y / 16) * 16; }
    ui_layer_begin(ui, id, UL_MODAL, z, lt_rect_make(0, 0, ui->sw, ui->sh));
    lt_ui_draw_fill(ui, lt_rect_make(0, 0, ui->sw, ui->sh), ui->cell ? LT_RGBA(0, 0, 0, 150) : LT_RGBA(0, 0, 0, 153));
    draw_window_frame(ui, dlg, v, 1, &close, th);
    res = ui_interact(ui, id ^ 0xC10EUL, close, 0);
    if (res & UI_CLICK) { *open = 0; ui_request(ui); }
    if (!ui->cell && (res & UI_HOVER)) { lt_ui_draw_box(ui, close, t->hover, 0, t->radius); }
    lt_ui_draw_icon(ui, close, LT_UI_ICON_CLOSE, (res & UI_HOVER) ? t->text : t->muted);
    ui->modal_stack[ui->nmodal] = open;
    ui->modal_id[ui->nmodal] = id;
    ui->modal_fill[ui->nmodal] = ui->modal_slackn[ui->nmodal] = ui->modal_slack[ui->nmodal] = 0;
    ui->nmodal++;
    ui->modal_depth++;
    if (fresh) {
        st->d[0] = 0.0; st->d[1] = 0.0;
        ui->focus_req = 1;
        ui->focus_req_kind = 0;
        ui->focus = 0;
        ui_request(ui);
    }
    lt_ui_push_id(ui, title);
    lt_ui_clip_push(ui, ui->cell ? lt_rect_make(dlg.x + 8, dlg.y + th, dlg.w - 16, dlg.h - th - 16)
                                 : lt_rect_make(dlg.x + 1, dlg.y + th + 1, dlg.w - 2, dlg.h - th - 2));
    ui_layout_push(ui, lt_rect_make(dlg.x + pad, dlg.y + th + (ui->cell ? 0 : lt_ui_px(ui, 12)), dlg.w - 2 * pad,
                                    dlg.h - th - (ui->cell ? 16 : lt_ui_px(ui, 24))));
    ui->modal_lay[ui->nmodal - 1] = ui->nlay - 1;
    return LT_TRUE;
}

void lt_ui_modal_end(lt_ui *ui)
{
    int *open;
    if (ui->cell && ui->nmodal > 0) {
        /* content height of this frame: all fill rows must be scroll areas to know the slack */
        int m = ui->nmodal - 1;
        const ui_layout *ML = ui_lay(ui);
        ui_state *ms = ui_state_get(ui, ui->modal_id[m]);
        int e = ML->max_y - ML->body.y;
        if (ui->modal_fill[m] == 0 || ui->modal_fill[m] == ui->modal_slackn[m]) {
            if (ui->modal_fill[m] > 0) { e -= ui->modal_slack[m]; }
            if (e > (int)ms->d[0]) { ms->d[0] = (double)e; ui_request(ui); }
            ms->d[1] = 1.0;
        } else {
            ms->d[0] = 1.0e6;
            ms->d[1] = 1.0;
        }
    }
    ui_layout_pop(ui);
    lt_ui_clip_pop(ui);
    lt_ui_pop_id(ui);
    if (ui->nmodal > 0) {
        open = ui->modal_stack[--ui->nmodal];
        if (open != NULL && !ui->popup_dismiss && ui->npopups == 0 &&
            ui_key_take(ui, LT_KEY_ESCAPE, NULL)) {
            *open = 0;
            ui_request(ui);
        }
    }
    ui->modal_depth--;
    ui_layer_end(ui);
}

int lt_ui_msgbox(lt_ui *ui, const char *id, const char *title, const char *message,
                 const char *buttons, int *open)
{
    char names[6][48];
    int n = 0, i, w, h, result = 0, nlines;
    const char *p = buttons;
    const char *starts[64];
    int lens[64];
    const lt_theme *t = &ui->theme;
    int bw, widths[8];
    LT_UNUSED(id);
    while (*p && n < 6) {
        const char *e = strchr(p, '|');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        if (len >= sizeof(names[0])) { len = sizeof(names[0]) - 1; }
        memcpy(names[n], p, len);
        names[n][len] = '\0';
        n++;
        if (!e) { break; }
        p = e + 1;
    }
    if (n == 0) { strcpy(names[0], "OK"); n = 1; }
    w = ui->cell ? LT_MIN(448, ui->sw - 16) : lt_ui_px(ui, 440);
    nlines = ui_wrap_lines(ui, message, w - 2 * (ui->cell ? 16 : t->padding), starts, lens, 64);
    h = t->title_h + nlines * ui_line_h(ui) + t->row_h + (ui->cell ? 48 : lt_ui_px(ui, 48));
    if (ui->cell) { h = ((h + 15) / 16) * 16; }
    if (!ui_modal_begin_dev(ui, title, w, h, open)) { return 0; }
    lt_ui_label_wrap(ui, message, t->text);
    lt_ui_layout_space(ui, ui->cell ? 16 : 8);
    bw = ui->cell ? 80 : 88;
    widths[0] = -1;
    for (i = 0; i < n; i++) { widths[i + 1] = bw; }
    lt_ui_layout_row(ui, n + 1, widths, 0);
    lt_ui_layout_next(ui);
    for (i = 0; i < n; i++) {
        char lab[64];
        lt_snprintf(lab, sizeof(lab), "%s##mb%d", names[i], i);
        if (lt_ui_button_ex(ui, lab, i == 0 ? LT_UI_BTN_DEFAULT : 0)) {
            result = i + 1;
        }
    }
    lt_ui_modal_end(ui);
    if (result > 0) {
        *open = 0;
        ui_request(ui);
    } else if (!*open) {
        result = -1;
    }
    return result;
}

/* ------------------------------------------------------------------ */
/* tree view                                                           */
int lt_ui_tree_node(lt_ui *ui, const char *label, int icon, int flags)
{
    return lt_ui_tree_node_ex(ui, label, icon, ui->theme.text2, flags);
}

int lt_ui_tree_node_ex(lt_ui *ui, const char *label, int icon, lt_color icon_color, int flags)
{
    const lt_theme *t = &ui->theme;
    char vis[200];
    const char *v = ui_label_visible(label, vis, sizeof(vis));
    lt_u32 id = lt_ui_id(ui, label);
    ui_layout *L = ui_lay(ui);
    lt_rect r = ui_next(ui, 0);
    lt_rect full = lt_rect_make(L->body.x, r.y, L->body.w, r.h);
    ui_state *st = ui_state_get(ui, id ^ 0x7472UL);
    int open, res, out = 0, leaf = (flags & LT_UI_TREE_LEAF) != 0, mods = 0, ex = ui->cell ? 16 : lt_ui_px(ui, 14);
    int dis = (flags & LT_UI_TREE_DISABLED) != 0;
    lt_color bg = 0, fg;
    lt_rect hr;
    if (st->i[1] == 0) {
        st->i[1] = 1;
        st->i[0] = (flags & LT_UI_TREE_DEFAULT_OPEN) ? 1 : 0;
    }
    if (ui->tree_cmd != 0 && !leaf) { st->i[0] = (ui->tree_cmd == 1) ? 1 : 0; }
    res = ui_interact(ui, id, full, UI_FOCUSABLE | UI_KIND(UI_K_TREE));
    open = st->i[0];
    if (res & UI_CLICK) {
        out |= LT_UI_TREE_CLICKED;
        if (!leaf) { st->i[0] = open = !open; }
    }
    if (res & UI_DBLCLICK) { out |= LT_UI_TREE_ACTIVATED; }
    if (ui->focus == id) {
        if (ui_key_take(ui, LT_KEY_DOWN, &mods)) {
            ui->focus_req = 1; ui->focus_req_kind = UI_K_TREE; ui_request(ui);
        } else if (ui_key_take(ui, LT_KEY_UP, &mods)) {
            if (ui->prev_tree != 0) { ui->focus = ui->prev_tree; ui_request(ui); }
        } else if (ui_key_take(ui, LT_KEY_RIGHT, &mods)) {
            if (!leaf && !open) { st->i[0] = open = 1; }
        } else if (ui_key_take(ui, LT_KEY_LEFT, &mods)) {
            if (!leaf && open) { st->i[0] = open = 0; }
        } else if (ui_key_take(ui, LT_KEY_ENTER, &mods) || ui_key_take(ui, ' ', &mods)) {
            out |= LT_UI_TREE_CLICKED | LT_UI_TREE_ACTIVATED;
            if (!leaf) { st->i[0] = open = !open; }
        }
    }
    ui->prev_tree = id;
    if (flags & LT_UI_TREE_FORCE_OPEN) { open = 1; }
    hr = full;
    if (flags & LT_UI_TREE_SELECTED) { bg = ui->cell ? t->primary_active : t->selected; }
    else if ((res & UI_HOVER) && !dis) { bg = t->hover; }
    if (bg != 0) { lt_ui_draw_fill(ui, hr, bg); }
    if ((flags & LT_UI_TREE_SELECTED) && !ui->cell) {
        lt_ui_draw_fill(ui, lt_rect_make(hr.x, hr.y, lt_ui_px(ui, 2), hr.h), t->primary);
    }
    if (ui->focus == id && ui->kbd_nav) {
        if (ui->cell) { lt_ui_draw_fill(ui, hr, t->hover); }
        else { lt_ui_draw_rect(ui, hr, t->focus); }
    }
    fg = dis ? t->muted : ((flags & LT_UI_TREE_SELECTED) ? t->primary_text : t->text);
    if (!leaf) {
        lt_rect er = lt_rect_make(r.x + (ui->cell ? 0 : lt_ui_px(ui, 2)), r.y, ex - (ui->cell ? 0 : lt_ui_px(ui, 2)), r.h);
        if (ui->cell) {
            lt_ui_draw_icon(ui, er, open ? LT_UI_ICON_ARROW_DOWN : LT_UI_ICON_ARROW_RIGHT,
                            (flags & LT_UI_TREE_SELECTED) ? t->primary_text : t->text2);
        } else {
            double cx = (double)er.x + (double)er.w / 2.0, cy = (double)er.y + (double)er.h / 2.0;
            double q = 3.0 * (double)ui->pct / 100.0, pts[6];
            if (open) {
                pts[0] = cx - q; pts[1] = cy - q * 0.5; pts[2] = cx + q; pts[3] = cy - q * 0.5;
                pts[4] = cx; pts[5] = cy + q * 0.9;
            } else {
                pts[0] = cx - q * 0.5; pts[1] = cy - q; pts[2] = cx + q * 0.9; pts[3] = cy;
                pts[4] = cx - q * 0.5; pts[5] = cy + q;
            }
            lt_ui_draw_poly(ui, pts, 3, (flags & LT_UI_TREE_SELECTED) ? t->primary_text : t->muted);
        }
    }
    {
        int tx = r.x + ex + (ui->cell ? 0 : lt_ui_px(ui, 2));
        char mid[200];
        if (icon != 0 && ui->cell) {
            if (leaf) {       /* a bullet in the node's kind color keeps leaves aligned and scannable */
                lt_ui_draw_icon(ui, lt_rect_make(tx, r.y, 8, r.h), LT_UI_ICON_DOT,
                                dis ? t->muted : ((flags & LT_UI_TREE_SELECTED) ? t->primary_text : icon_color));
                tx += 16;
            }
        } else if (icon != 0) {
            lt_ui_draw_icon(ui, lt_rect_make(tx, r.y, ui->cell ? 16 : lt_ui_px(ui, 14), r.h), icon,
                              dis ? t->muted : ((flags & LT_UI_TREE_SELECTED) ? t->primary_text : icon_color));
            tx += ui->cell ? 16 : lt_ui_px(ui, 18);
        }
        if (ui->cell) {
            int avail = (full.x + full.w - tx) / 8;
            if (ui_text_w(ui, v, ui->tfont, LT_UI_FS_SM) > avail * 8) {
                lt_cell_ellipsize_mid(v, avail, mid, sizeof(mid));
                v = mid;
            }
        }
        ui_text_in_f(ui, lt_rect_make(tx, r.y, full.x + full.w - tx - (ui->cell ? 0 : 4), r.h), v, fg, 0, ui->tfont, LT_UI_FS_SM);
        if (ui->cell && (!leaf || (flags & LT_UI_TREE_SELECTED))) {
            ui_cell_bold(ui, lt_rect_make(tx, r.y, ui_text_w(ui, v, ui->tfont, LT_UI_FS_SM), r.h));
        }
    }
    ui_set_last(ui, id, full);
    if (open) {
        out |= LT_UI_TREE_OPEN;
        L->indent += t->indent;
        ui->tree_depth++;
        lt_ui_push_id(ui, label);
    }
    return out;
}

void lt_ui_tree_set_all(lt_ui *ui, int open)
{
    ui->tree_cmd = open ? 1 : 2;
    ui->tree_cmd_left = 8;
    ui_request(ui);
}

void lt_ui_tree_end(lt_ui *ui)
{
    ui_layout *L = ui_lay(ui);
    L->indent -= ui->theme.indent;
    if (L->indent < 0) { L->indent = 0; }
    ui->tree_depth--;
    lt_ui_pop_id(ui);
}

/* ------------------------------------------------------------------ */
/* tab bar                                                             */
void lt_ui_tabbar_begin(lt_ui *ui, const char *id, int *active)
{
    const lt_theme *t = &ui->theme;
    lt_rect r = ui_next_full(ui, t->tab_h);
    ui->tab_id = lt_ui_id(ui, id);
    ui->tab_active_ptr = active;
    ui->tab_idx = 0;
    ui->tab_x = r.x;
    ui->tab_rect = r;
    lt_ui_draw_fill(ui, r, ui->cell ? t->tab : t->bg_toolbar);
    if (!ui->cell) { lt_ui_draw_hline(ui, r.x, r.x + r.w - 1, r.y + r.h - 1, t->border_subtle); }
    lt_ui_push_id(ui, id);
    lt_ui_clip_push(ui, r);
}

int lt_ui_tab(lt_ui *ui, const char *title, int flags)
{
    return lt_ui_tab_icon(ui, title, 0, flags);
}

int lt_ui_tab_icon(lt_ui *ui, const char *title, int icon, int flags)
{
    const lt_theme *t = &ui->theme;
    char vis[160];
    const char *v = ui_label_visible(title, vis, sizeof(vis));
    lt_u32 id = lt_ui_id(ui, title);
    int tw = ui_text_w(ui, v, ui->tfont, LT_UI_FS_SM), w, closable = (flags & LT_UI_TAB_CLOSABLE) != 0;
    int dirty = (flags & LT_UI_TAB_DIRTY) != 0, out = 0, res, cres = 0, active, idx = ui->tab_idx++;
    int padx = ui->cell ? 8 : lt_ui_px(ui, 8);
    int iw = icon != 0 ? (ui->cell ? 0 : lt_ui_px(ui, 18)) : 0;
    int cw = closable ? (ui->cell ? 16 : lt_ui_px(ui, 24)) : 0;
    int dw = dirty ? (ui->cell ? 8 : lt_ui_px(ui, 12)) : 0;
    lt_rect r, cb = lt_rect_make(0, 0, 0, 0);
    lt_color bg, fg;
    w = tw + 2 * padx + iw + cw + dw;
    if (!ui->cell && w < lt_ui_px(ui, 100)) { w = lt_ui_px(ui, 100); }
    if (w > (ui->cell ? 240 : lt_ui_px(ui, 200))) { w = ui->cell ? 240 : lt_ui_px(ui, 200); }
    {   /* too many tabs for the row: shrink them evenly (width of the last frame) */
        ui_state *bs = ui_state_get(ui, ui->tab_id ^ 0x7AB5UL);
        bs->i[1] += w;
        if (!ui->cell && bs->i[0] > ui->tab_rect.w && bs->i[0] > 0) {
            w = LT_MAX(w * ui->tab_rect.w / bs->i[0], lt_ui_px(ui, 56));
        }
    }
    r = lt_rect_make(ui->tab_x, ui->tab_rect.y, w, ui->tab_rect.h - (ui->cell ? 0 : 1));
    ui->tab_x += w;
    if (closable) {
        int cs = ui->cell ? 16 : lt_ui_px(ui, 18);
        cb = lt_rect_make(r.x + r.w - cs - (ui->cell ? 0 : lt_ui_px(ui, 4)), r.y + (r.h - cs) / 2, cs, cs);
        cres = ui_interact(ui, id ^ 0xC105UL, cb, 0);
    }
    res = ui_interact(ui, id, r, 0);
    if ((cres & UI_CLICK) || (ui_over(ui, r) && ui_pressed(ui, LT_MB_MIDDLE) && closable)) {
        out |= LT_UI_TAB_CLOSED;
        if (ui_over(ui, r)) { ui->press_claimed = 1; }
    } else if ((res & UI_CLICK) && !(cres & UI_HOVER)) {
        if (ui->tab_active_ptr != NULL) { *ui->tab_active_ptr = idx; }
        out |= LT_UI_TAB_SELECTED;
    }
    active = ui->tab_active_ptr != NULL && *ui->tab_active_ptr == idx;
    if (active) { bg = t->tab_active; fg = t->text; }
    else { bg = (res & UI_HOVER) ? t->hover : t->tab; fg = (res & UI_HOVER) ? t->text : t->text2; }
    if (ui->cell) {
        lt_ui_draw_fill(ui, r, active ? t->primary_active : bg);
        if (active) { fg = t->primary_text; }
        if (!active && idx > 0) { lt_ui_draw_vline(ui, r.x, r.y, r.y + r.h - 1, t->border_subtle); }
    } else {
        lt_ui_draw_fill(ui, r, bg);
        lt_ui_draw_vline(ui, r.x + r.w - 1, r.y, r.y + r.h - 1, t->border_subtle);
        if (active) { lt_ui_draw_fill(ui, lt_rect_make(r.x, r.y + r.h - lt_ui_px(ui, 2), r.w - 1, lt_ui_px(ui, 2)), t->primary); }
    }
    {
        int tx = r.x + padx;
        if (iw > 0) {
            lt_ui_draw_icon(ui, lt_rect_make(tx, r.y, lt_ui_px(ui, 14), r.h), icon, active ? t->primary : t->text2);
            tx += iw;
        }
        if (dirty) {
            lt_ui_draw_icon(ui, lt_rect_make(tx - 2, r.y, ui->cell ? 8 : lt_ui_px(ui, 10), r.h), LT_UI_ICON_DOT, t->warning);
            tx += dw;
        }
        ui_text_in_f(ui, lt_rect_make(tx, r.y, r.x + r.w - tx - (closable ? cw : padx), r.h),
                     v, fg, 0, ui->tfont, LT_UI_FS_SM);
        if (active) { ui_cell_bold(ui, lt_rect_make(tx, r.y, tw, r.h)); }
    }
    if (closable && (active || (res & UI_HOVER) || (cres & UI_HOVER) || ui->cell)) {
        if (cres & UI_HOVER) {
            lt_ui_draw_box(ui, cb, t->active, 0, t->radius);
        }
        lt_ui_draw_lucide(ui, cb, LT_ICON_X, 12, 2.0, (cres & UI_HOVER) ? t->text : t->muted);
        if (ui->cell) { lt_ui_draw_icon(ui, cb, LT_UI_ICON_CLOSE, (cres & UI_HOVER) ? t->text : t->muted); }
    }
    ui_set_last(ui, id, r);
    return out;
}

int lt_ui_tabbar_end(lt_ui *ui)
{
    ui_state *bs = ui_state_get(ui, ui->tab_id ^ 0x7AB5UL);
    if (bs->i[0] != bs->i[1]) { bs->i[0] = bs->i[1]; ui_request(ui); }
    bs->i[1] = 0;
    lt_ui_clip_pop(ui);
    lt_ui_pop_id(ui);
    return ui->tab_active_ptr != NULL ? *ui->tab_active_ptr : 0;
}

/* ------------------------------------------------------------------ */
/* flat tabs (settings dialogs): text tabs, active one underlined       */
lt_bool lt_ui_tabs_flat(lt_ui *ui, const char *label, const char *const *labels, int n, int *sel)
{
    const lt_theme *t = &ui->theme;
    int i, x, changed = 0;
    lt_rect r = ui_next_full(ui, ui->cell ? 16 : lt_ui_px(ui, 36));
    lt_ui_push_id(ui, label);
    if (!ui->cell) { lt_ui_draw_hline(ui, r.x, r.x + r.w - 1, r.y + r.h - 1, t->border_subtle); }
    x = r.x;
    for (i = 0; i < n; i++) {
        int tw = ui_text_w(ui, labels[i], (int)LT_FONT_SANS_MEDIUM, 0);
        int w = tw + (ui->cell ? 16 : lt_ui_px(ui, 32));
        lt_rect tr = lt_rect_make(x, r.y, w, r.h);
        lt_u32 id = lt_ui_id(ui, labels[i]);
        int res = ui_interact(ui, id, tr, UI_FOCUSABLE | UI_KIND(UI_K_BUTTON)), on = (*sel == i);
        lt_color fg = on ? (ui->cell ? t->primary_hover : t->primary) : ((res & UI_HOVER) ? t->text : t->text2);
        if (res & UI_CLICK) { if (*sel != i) { changed = 1; } *sel = i; on = 1; }
        if (ui->focus == id && (ui_key_take(ui, LT_KEY_ENTER, NULL) || ui_key_take(ui, ' ', NULL))) { *sel = i; changed = 1; }
        ui_text_in_f(ui, tr, labels[i], fg, 1, (int)LT_FONT_SANS_MEDIUM, 0);
        if (on && !ui->cell) { lt_ui_draw_fill(ui, lt_rect_make(x, r.y + r.h - lt_ui_px(ui, 2), w, lt_ui_px(ui, 2)), t->primary); }
        else if (on) { lt_ui_draw_hline(ui, x, x + w - 1, r.y + r.h - 1, fg); }
        ui_set_last(ui, id, tr);
        x += w;
    }
    lt_ui_pop_id(ui);
    return changed ? LT_TRUE : LT_FALSE;
}
