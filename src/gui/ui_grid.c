/* ui_grid.c - table-editor grid: pinned headers, scrolling cell area,
 * selection model (click, shift, ctrl, drag, keyboard), inline editing.
 * All interaction runs in lt_ui_grid_begin so the cells drawn afterwards
 * already reflect the new selection; the inline editor is drawn in
 * lt_ui_grid_end so it sits on top of the cells.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdlib.h>
#include <string.h>
#include "ui_priv.h"

static ui_grid *grid_find(lt_ui *ui, lt_u32 id)
{
    int i, victim = 0, best = 0x7FFFFFFF;
    for (i = 0; i < 4; i++) {
        if (ui->grids[i].used && ui->grids[i].id == id) { return &ui->grids[i]; }
    }
    for (i = 0; i < 4; i++) {
        if (!ui->grids[i].used) { victim = i; best = -1; break; }
        if (ui->grids[i].lru < best) { best = ui->grids[i].lru; victim = i; }
    }
    {
        ui_grid *G = &ui->grids[victim];
        lt_u8 *m = G->mask;
        int mc = G->mask_cap;
        memset(G, 0, sizeof(*G));
        G->mask = m;
        G->mask_cap = mc;
        G->id = id;
        G->used = 1;
        G->hov_r = G->hov_c = -1;
        return G;
    }
}

static void mask_clear(ui_grid *G)
{
    if (G->mask != NULL) { memset(G->mask, 0, (size_t)G->mask_cap); }
}

static void mask_rect(ui_grid *G, int r0, int c0, int r1, int c1, int val)
{
    int r, c, t;
    if (r0 > r1) { t = r0; r0 = r1; r1 = t; }
    if (c0 > c1) { t = c0; c0 = c1; c1 = t; }
    r0 = LT_MAX(r0, 0); c0 = LT_MAX(c0, 0);
    r1 = LT_MIN(r1, G->rows - 1); c1 = LT_MIN(c1, G->cols - 1);
    for (r = r0; r <= r1; r++) {
        for (c = c0; c <= c1; c++) { G->mask[r * G->cols + c] = (lt_u8)val; }
    }
}

static void sel_set_rect(ui_grid *G, int r0, int c0, int r1, int c1)
{
    mask_clear(G);
    mask_rect(G, r0, c0, r1, c1, 1);
    G->serial++;
}

static lt_rect cell_rect(const ui_grid *G, int r, int c)
{
    return lt_rect_make(G->ox + c * G->cw, G->oy + r * G->chh, G->cw, G->chh);
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

lt_bool lt_ui_grid_selected(const lt_ui *ui, int r, int c)
{
    const ui_grid *G = ui->grid;
    if (G == NULL || G->mask == NULL || r < 0 || c < 0 || r >= G->rows || c >= G->cols) { return LT_FALSE; }
    return G->mask[r * G->cols + c] ? LT_TRUE : LT_FALSE;
}

void lt_ui_grid_cursor(const lt_ui *ui, int *r, int *c)
{
    const ui_grid *G = ui->grid;
    if (r != NULL) { *r = G ? G->cur_r : 0; }
    if (c != NULL) { *c = G ? G->cur_c : 0; }
}

int lt_ui_grid_sel_bounds(const lt_ui *ui, int *r0, int *c0, int *r1, int *c1)
{
    const ui_grid *G = ui->grid;
    int r, c, n = 0, a = 1 << 30, b = -1, d = 1 << 30, e = -1;
    if (G != NULL && G->mask != NULL) {
        for (r = 0; r < G->rows; r++) {
            for (c = 0; c < G->cols; c++) {
                if (G->mask[r * G->cols + c]) {
                    n++;
                    if (r < a) { a = r; }
                    if (r > b) { b = r; }
                    if (c < d) { d = c; }
                    if (c > e) { e = c; }
                }
            }
        }
    }
    if (n == 0) { a = b = d = e = -1; }
    if (r0 != NULL) { *r0 = a; }
    if (r1 != NULL) { *r1 = b; }
    if (c0 != NULL) { *c0 = d; }
    if (c1 != NULL) { *c1 = e; }
    return n;
}

void lt_ui_grid_select(lt_ui *ui, int r0, int c0, int r1, int c1)
{
    ui_grid *G = ui->grid;
    if (G == NULL || G->mask == NULL) { return; }
    sel_set_rect(G, r0, c0, r1, c1);
    G->anc_r = clampi(r0, 0, G->rows - 1); G->anc_c = clampi(c0, 0, G->cols - 1);
    G->cur_r = clampi(r1, 0, G->rows - 1); G->cur_c = clampi(c1, 0, G->cols - 1);
    G->last_serial = G->serial;
}

void lt_ui_grid_set_cursor(lt_ui *ui, int r, int c)
{
    lt_ui_grid_select(ui, r, c, r, c);
}

lt_bool lt_ui_grid_hover(const lt_ui *ui, int *r, int *c)
{
    const ui_grid *G = ui->grid;
    if (G == NULL || G->hov_r < 0) { return LT_FALSE; }
    if (r != NULL) { *r = G->hov_r; }
    if (c != NULL) { *c = G->hov_c; }
    return LT_TRUE;
}

lt_bool lt_ui_grid_editing(const lt_ui *ui, int *r, int *c)
{
    const ui_grid *G = ui->grid;
    if (G == NULL) { return LT_FALSE; }
    if (r != NULL) { *r = G->edit_r; }
    if (c != NULL) { *c = G->edit_c; }
    return G->edit ? LT_TRUE : LT_FALSE;
}

const char *lt_ui_grid_edit_text(const lt_ui *ui)
{
    return ui->grid != NULL ? ui->grid->edit_buf : "";
}

lt_bool lt_ui_grid_edit_wants_text(const lt_ui *ui)
{
    return (ui->grid != NULL && ui->grid->edit_wants) ? LT_TRUE : LT_FALSE;
}

void lt_ui_grid_edit_set_text(lt_ui *ui, const char *text)
{
    ui_grid *G = ui->grid;
    if (G == NULL) { return; }
    lt_strlcpy(G->edit_buf, text, sizeof(G->edit_buf));
    G->edit_wants = 0;
    if (ui->edit.id != 0) {
        ui->edit.cursor = (int)strlen(G->edit_buf);
        ui->edit.anchor = 0;
    }
}

void lt_ui_grid_edit_begin(lt_ui *ui, int r, int c, const char *text)
{
    ui_grid *G = ui->grid;
    if (G == NULL) { return; }
    G->edit = 1;
    G->edit_r = clampi(r, 0, G->rows - 1);
    G->edit_c = clampi(c, 0, G->cols - 1);
    lt_strlcpy(G->edit_buf, text != NULL ? text : "", sizeof(G->edit_buf));
    G->edit_wants = (text == NULL);
    ui->focus = G->id ^ 0xED17UL;
    ui->kbd_nav = 0;
    ui_request(ui);
}

lt_rect lt_ui_grid_cell_rect(const lt_ui *ui, int r, int c)
{
    if (ui->grid == NULL) { return lt_rect_make(0, 0, 0, 0); }
    return cell_rect(ui->grid, r, c);
}

void lt_ui_grid_visible(const lt_ui *ui, int *r0, int *r1, int *c0, int *c1)
{
    const ui_grid *G = ui->grid;
    if (G == NULL) {
        if (r0) { *r0 = 0; } if (r1) { *r1 = -1; } if (c0) { *c0 = 0; } if (c1) { *c1 = -1; }
        return;
    }
    if (r0) { *r0 = G->r0; }
    if (r1) { *r1 = G->r1; }
    if (c0) { *c0 = G->c0; }
    if (c1) { *c1 = G->c1; }
}

/* ------------------------------------------------------------------ */
static void move_cursor(lt_ui *ui, ui_grid *G, int nr, int nc, int extend)
{
    nr = clampi(nr, 0, G->rows - 1);
    nc = clampi(nc, 0, G->cols - 1);
    if (!extend) {
        G->anc_r = nr; G->anc_c = nc;
    }
    G->cur_r = nr; G->cur_c = nc;
    sel_set_rect(G, G->anc_r, G->anc_c, nr, nc);
    lt_ui_scroll_ensure_visible(ui, cell_rect(G, nr, nc));
}

static void edit_start(lt_ui *ui, ui_grid *G, const char *typed)
{
    G->edit = 1;
    G->edit_r = G->cur_r;
    G->edit_c = G->cur_c;
    G->edit_wants = (typed == NULL);
    lt_strlcpy(G->edit_buf, typed != NULL ? typed : "", sizeof(G->edit_buf));
    G->flags_out |= LT_UI_GRID_EDIT_BEGIN;
    ui->focus = G->id ^ 0xED17UL;
    ui->kbd_nav = 0;
    ui_request(ui);
}

static void edit_commit(lt_ui *ui, ui_grid *G)
{
    if (!G->edit) { return; }
    G->edit = 0;
    G->flags_out |= LT_UI_GRID_EDIT_COMMIT;
    if (ui->focus == (G->id ^ 0xED17UL)) { ui->focus = G->id; }
    if (ui->edit.id == (G->id ^ 0xED17UL)) { ui_edit_end(ui); }
}

lt_bool lt_ui_grid_begin(lt_ui *ui, const char *label, int rows, int cols, int cell_w, int cell_h,
                         int row_header_w, int col_header_h)
{
    lt_u32 gid;
    ui_grid *G;
    lt_rect r, data;
    ui_scroll *S;
    int res, mods = 0, k, sbw = ui->theme.scrollbar_w;
    int dr = 0, dc = 0, extend = 0, handled = 0;
    lt_rect rowstrip, colstrip;
    int over_rowh, over_colh;

    if (rows < 1 || cols < 1 || cell_w < 1 || cell_h < 1) { return LT_FALSE; }
    /* sizes are given in px at 100% */
    cell_w = lt_ui_px(ui, cell_w); cell_h = lt_ui_px(ui, cell_h);
    row_header_w = lt_ui_px(ui, row_header_w); col_header_h = lt_ui_px(ui, col_header_h);
    lt_ui_push_id(ui, label);
    gid = lt_ui_id(ui, "##grid");
    G = grid_find(ui, gid);
    ui->grid = G;
    if (ui->cell && (G->lru == 0 || G->lru < ui->frame_no - 1) && ui->npopups == 0 && ui->focus == 0) {
        /* terminal: a table that just appeared takes the keyboard (arrows work at once) */
        ui->focus = gid;
        ui->kbd_nav = 1;
    }
    G->lru = ui->frame_no;
    G->in_frame = 1;
    if (G->rows != rows || G->cols != cols || G->mask == NULL) {
        int need = rows * cols;
        if (G->mask == NULL || G->mask_cap < need) {
            G->mask = (lt_u8 *)lt_realloc(G->mask, (size_t)need);
            G->mask_cap = need;
        }
        G->rows = rows; G->cols = cols;
        memset(G->mask, 0, (size_t)G->mask_cap);
        G->cur_r = clampi(G->cur_r, 0, rows - 1); G->cur_c = clampi(G->cur_c, 0, cols - 1);
        G->anc_r = G->cur_r; G->anc_c = G->cur_c;
        G->mask[G->cur_r * cols + G->cur_c] = 1;
        G->edit = 0;
    }
    G->cw = cell_w; G->chh = cell_h; G->rhw = row_header_w; G->chdr = col_header_h;
    G->flags_out = 0;
    r = ui_next(ui, 0);
    G->view = r;
    G->parent_clip = ui_clip_cur(ui);
    data = lt_rect_make(r.x + row_header_w, r.y + col_header_h, r.w - row_header_w, r.h - col_header_h);
    lt_ui_layout_set_rect(ui, data);
    if (!lt_ui_scroll_begin(ui, "##data")) {
        G->in_frame = 0;
        ui->grid = NULL;
        lt_ui_pop_id(ui);
        return LT_FALSE;
    }
    lt_ui_scroll_set_content(ui, cols * cell_w, rows * cell_h);
    S = &ui->scr[ui->nscr - 1];
    G->inner = S->inner;
    G->sx = S->sx; G->sy = S->sy;
    G->ox = S->inner.x - S->sx;
    G->oy = S->inner.y - S->sy;
    G->r0 = clampi(S->sy / cell_h, 0, rows - 1);
    G->r1 = clampi((S->sy + S->inner.h - 1) / cell_h, 0, rows - 1);
    G->c0 = clampi(S->sx / cell_w, 0, cols - 1);
    G->c1 = clampi((S->sx + S->inner.w - 1) / cell_w, 0, cols - 1);
    G->hov_r = G->hov_c = -1;
    LT_UNUSED(sbw);

    /* ---- header strips: hit test with the parent clip ---- */
    rowstrip = lt_rect_make(r.x, G->inner.y, row_header_w, G->inner.h);
    colstrip = lt_rect_make(G->inner.x, r.y, G->inner.w, col_header_h);
    over_rowh = row_header_w > 0 && lt_rect_contains(lt_rect_intersect(rowstrip, G->parent_clip), ui->mx, ui->my) &&
                ui_layer_ok(ui);
    over_colh = col_header_h > 0 && lt_rect_contains(lt_rect_intersect(colstrip, G->parent_clip), ui->mx, ui->my) &&
                ui_layer_ok(ui);
    if ((ui->mpressed & (1 << LT_MB_LEFT)) && !ui->popup_dismiss && ui->active == 0 &&
        (over_rowh || over_colh ||
         (row_header_w > 0 && col_header_h > 0 &&
          lt_rect_contains(lt_rect_make(r.x, r.y, row_header_w, col_header_h), ui->mx, ui->my) &&
          ui_layer_ok(ui)))) {
        int shift = (ui->mods & LT_MOD_SHIFT) != 0;
        edit_commit(ui, G);
        ui->focus = gid; ui->kbd_nav = 0; ui->press_claimed = 1;
        if (over_rowh) {
            int rr = clampi((ui->my - G->oy) / cell_h, 0, rows - 1);
            if (!shift) { G->anc_r = rr; }
            sel_set_rect(G, G->anc_r, 0, rr, cols - 1);
            G->cur_r = rr; G->cur_c = 0;
        } else if (over_colh) {
            int cc = clampi((ui->mx - G->ox) / cell_w, 0, cols - 1);
            if (!shift) { G->anc_c = cc; }
            sel_set_rect(G, 0, G->anc_c, rows - 1, cc);
            G->cur_c = cc; G->cur_r = 0;
        } else {
            sel_set_rect(G, 0, 0, rows - 1, cols - 1);
            G->anc_r = 0; G->anc_c = 0; G->cur_r = 0; G->cur_c = 0;
        }
        G->flags_out |= LT_UI_GRID_CLICKED;
    }

    /* ---- data area mouse ---- */
    res = ui_interact(ui, gid, G->inner, UI_FOCUSABLE | UI_KIND(UI_K_OTHER));
    {
        int mr = (ui->my - G->oy) / cell_h, mc = (ui->mx - G->ox) / cell_w;
        if (ui->my < G->oy) { mr = -1; }
        if (ui->mx < G->ox) { mc = -1; }
        if ((res & UI_HOVER) && mr >= 0 && mc >= 0 && mr < rows && mc < cols) {
            G->hov_r = mr; G->hov_c = mc;
        }
        if (res & UI_PRESS) {
            int cr = clampi(mr, 0, rows - 1), cc = clampi(mc, 0, cols - 1);
            int ctrl = (ui->mods & LT_MOD_CTRL) != 0, shift = (ui->mods & LT_MOD_SHIFT) != 0;
            if (G->edit) { edit_commit(ui, G); }
            if (ctrl) {
                G->mask[cr * cols + cc] = (lt_u8)!G->mask[cr * cols + cc];
                G->serial++;
                G->anc_r = G->cur_r = cr; G->anc_c = G->cur_c = cc;
            } else if (shift) {
                G->cur_r = cr; G->cur_c = cc;
                sel_set_rect(G, G->anc_r, G->anc_c, cr, cc);
            } else {
                G->anc_r = G->cur_r = cr; G->anc_c = G->cur_c = cc;
                sel_set_rect(G, cr, cc, cr, cc);
            }
            G->drag = !ctrl;
            G->flags_out |= LT_UI_GRID_CLICKED;
            if (res & UI_DBLCLICK) {
                G->flags_out |= LT_UI_GRID_ACTIVATED;
                edit_start(ui, G, NULL);
            }
        } else if ((res & UI_ACTIVE) && G->drag) {
            int cr = clampi(mr, 0, rows - 1), cc = clampi(mc, 0, cols - 1);
            if (cr != G->cur_r || cc != G->cur_c) {
                G->cur_r = cr; G->cur_c = cc;
                sel_set_rect(G, G->anc_r, G->anc_c, cr, cc);
            }
            /* auto-scroll when dragging outside */
            if (ui->my < G->inner.y || ui->my >= G->inner.y + G->inner.h ||
                ui->mx < G->inner.x || ui->mx >= G->inner.x + G->inner.w) {
                int nx = G->sx, ny = G->sy;
                if (ui->my < G->inner.y) { ny -= cell_h / 2; }
                if (ui->my >= G->inner.y + G->inner.h) { ny += cell_h / 2; }
                if (ui->mx < G->inner.x) { nx -= cell_w / 2; }
                if (ui->mx >= G->inner.x + G->inner.w) { nx += cell_w / 2; }
                lt_ui_scroll_set(ui, nx, ny);
            }
        } else if (!(res & UI_ACTIVE)) {
            G->drag = 0;
        }
    }

    /* ---- keyboard ---- */
    if (ui->focus == gid && !G->edit && !ui->disabled) {
        int page = LT_MAX(1, G->r1 - G->r0);
        extend = 0;
        for (k = 0; k < ui->nq && !handled; k++) {
            ui_evq *q = &ui->q[k];
            if (q->used) { continue; }
            if (q->is_text) {
                char ch = q->text[0];
                if ((ch >= '0' && ch <= '9') || ch == '.') {
                    q->used = 1;
                    edit_start(ui, G, q->text);
                    handled = 1;
                }
                continue;
            }
            mods = q->mods;
            extend = (mods & LT_MOD_SHIFT) != 0;
            switch (q->key) {
            case LT_KEY_UP: dr = -1; break;
            case LT_KEY_DOWN: dr = 1; break;
            case LT_KEY_LEFT: dc = -1; break;
            case LT_KEY_RIGHT: dc = 1; break;
            case LT_KEY_PAGEUP: dr = -page; break;
            case LT_KEY_PAGEDOWN: dr = page; break;
            case LT_KEY_HOME:
                if (mods & LT_MOD_CTRL) { dr = -rows; }
                dc = -cols;
                break;
            case LT_KEY_END:
                if (mods & LT_MOD_CTRL) { dr = rows; }
                dc = cols;
                break;
            case LT_KEY_ENTER: case LT_KEY_F2:
                q->used = 1;
                edit_start(ui, G, NULL);
                handled = 1;
                break;
            case 'a':
                if (mods & LT_MOD_CTRL) {
                    q->used = 1;
                    sel_set_rect(G, 0, 0, rows - 1, cols - 1);
                    G->anc_r = 0; G->anc_c = 0; G->cur_r = rows - 1; G->cur_c = cols - 1;
                    handled = 1;
                }
                break;
            default: break;
            }
            if (dr != 0 || dc != 0) {
                q->used = 1;
                if ((mods & LT_MOD_CTRL) && (q->key <= LT_KEY_RIGHT && q->key >= LT_KEY_UP)) {
                    dr *= rows; dc *= cols;
                }
                move_cursor(ui, G, G->cur_r + dr, G->cur_c + dc, extend);
                dr = dc = 0;
                handled = 1;
                /* process further queued keys in later frames only if needed */
                handled = 0;
            }
        }
    }
    return LT_TRUE;
}

void lt_ui_grid_header(lt_ui *ui, int kind, int index, const char *text)
{
    ui_grid *G = ui->grid;
    const lt_theme *t = &ui->theme;
    lt_rect hr, strip;
    lt_color bg = t->table_header;
    int sel = 0, r, c;
    if (G == NULL) { return; }
    if (index < 0 || index >= (kind == 0 ? G->rows : G->cols)) { return; }
    if (kind == 0) {
        hr = lt_rect_make(G->view.x, G->oy + index * G->chh, G->rhw, G->chh);
        strip = lt_rect_make(G->view.x, G->inner.y, G->rhw, G->inner.h);
        for (c = 0; c < G->cols && !sel; c++) {
            if (!G->mask[index * G->cols + c]) { break; }
            if (c == G->cols - 1) { sel = 1; }
        }
    } else {
        hr = lt_rect_make(G->ox + index * G->cw, G->view.y, G->cw, G->chdr);
        strip = lt_rect_make(G->inner.x, G->view.y, G->inner.w, G->chdr);
        for (r = 0; r < G->rows && !sel; r++) {
            if (!G->mask[r * G->cols + index]) { break; }
            if (r == G->rows - 1) { sel = 1; }
        }
    }
    strip = lt_rect_intersect(strip, G->parent_clip);
    {
        lt_rect vis = lt_rect_intersect(hr, strip);
        if (vis.w <= 0 || vis.h <= 0) { return; }
    }
    if (ui->nclip < UI_MAX_CLIPS) {
        ui_cmd *k;
        ui->clip[ui->nclip++] = strip;
        k = ui_cmd_push(ui, UC_CLIP);
        k->r = strip;
    }
    if (sel) { bg = lt_color_mix(bg, t->primary, 90); }
    lt_ui_draw_fill(ui, hr, bg);
    if (!ui->cell) {
        lt_ui_draw_vline(ui, hr.x + hr.w - 1, hr.y, hr.y + hr.h - 1, t->table_grid);
        lt_ui_draw_hline(ui, hr.x, hr.x + hr.w - 1, hr.y + hr.h - 1, t->table_grid);
    }
    ui_text_in(ui, hr, text, sel ? ui_text_on(t->primary_text, t->text, bg) : t->text2, 1);
    lt_ui_clip_pop(ui);
}

void lt_ui_grid_corner(lt_ui *ui, const char *text)
{
    ui_grid *G = ui->grid;
    const lt_theme *t = &ui->theme;
    lt_rect cr;
    if (G == NULL) { return; }
    cr = lt_rect_make(G->view.x, G->view.y, G->rhw, G->chdr);
    if (ui->nclip < UI_MAX_CLIPS) {
        ui_cmd *k;
        ui->clip[ui->nclip++] = lt_rect_intersect(cr, G->parent_clip);
        k = ui_cmd_push(ui, UC_CLIP);
        k->r = ui->clip[ui->nclip - 1];
    }
    lt_ui_draw_fill(ui, cr, t->table_axis);
    if (!ui->cell) {
        lt_ui_draw_vline(ui, cr.x + cr.w - 1, cr.y, cr.y + cr.h - 1, t->table_grid);
        lt_ui_draw_hline(ui, cr.x, cr.x + cr.w - 1, cr.y + cr.h - 1, t->table_grid);
    }
    if (text != NULL) { ui_text_in(ui, cr, text, t->muted, 1); }
    lt_ui_clip_pop(ui);
}

void lt_ui_grid_cell(lt_ui *ui, int r, int c, const char *text, lt_color bg, lt_color fg)
{
    ui_grid *G = ui->grid;
    const lt_theme *t = &ui->theme;
    lt_rect cr;
    int sel, cur;
    if (G == NULL || r < 0 || c < 0 || r >= G->rows || c >= G->cols) { return; }
    cr = cell_rect(G, r, c);
    sel = G->mask[r * G->cols + c];
    cur = (r == G->cur_r && c == G->cur_c);
    if (ui->cell) {
        /* heat-mapped text grid: the cursor cell is inverted, the rest of the selection tinted */
        lt_color fill = bg, tf = fg;
        int editing = G->edit && G->edit_r == r && G->edit_c == c;
        if (G->hov_r == r && G->hov_c == c && !sel) { fill = lt_color_mix(bg, LT_RGB(255, 255, 255), 40); }
        if (sel) {
            fill = lt_color_mix(bg, lt_color_contrast(bg) == LT_RGB(255, 255, 255) ? LT_RGB(255, 255, 255) : LT_RGB(0, 0, 0), 96);
            tf = lt_color_contrast(fill);
        }
        if (cur) {
            fill = lt_color_contrast(bg) == LT_RGB(255, 255, 255) ? LT_RGB(255, 255, 255) : LT_RGB(16, 16, 16);
            tf = lt_color_contrast(fill);
        }
        lt_ui_draw_fill(ui, cr, fill);
        if (!editing && text != NULL) {
            ui_text_in(ui, cr, text, tf, 1);
            if (cur || sel) { ui_cell_bold(ui, cr); }
        }
        return;
    }
    lt_ui_draw_fill(ui, cr, bg);
    if (!ui->cell) {
        lt_ui_draw_vline(ui, cr.x + cr.w - 1, cr.y, cr.y + cr.h - 1, t->table_grid);
        lt_ui_draw_hline(ui, cr.x, cr.x + cr.w - 1, cr.y + cr.h - 1, t->table_grid);
    }
    if (G->hov_r == r && G->hov_c == c && !sel) { lt_ui_draw_fill(ui, cr, LT_RGBA(255, 255, 255, 28)); }
    if (sel) { lt_ui_draw_fill(ui, cr, ui->cell ? LT_RGBA(255, 255, 255, 90) : LT_RGBA(255, 255, 255, 70)); }
    if (!(G->edit && G->edit_r == r && G->edit_c == c) && text != NULL) {
        if (sel && ui->cell) { fg = LT_RGB(0, 0, 0); }
        ui_text_in(ui, cr, text, fg, 1);
    }
    if (cur && !ui->cell) {
        lt_ui_draw_rect(ui, cr, t->primary_hover);
        lt_ui_draw_rect(ui, lt_rect_inset(cr, 1, 1), t->primary_hover);
    } else if (sel && !ui->cell) {
        lt_ui_draw_rect(ui, cr, lt_color_mix(bg, LT_RGB(255, 255, 255), 150));
    }
}

void lt_ui_grid_mark(lt_ui *ui, int r, int c, lt_color color, int thickness)
{
    ui_grid *G = ui->grid;
    lt_rect cr;
    int i;
    if (G == NULL || r < 0 || c < 0 || r >= G->rows || c >= G->cols) { return; }
    cr = cell_rect(G, r, c);
    if (ui->cell) {
        /* brackets around the value, in the mark color */
        lt_ui_draw_text(ui, cr.x, cr.y, "[", color);
        lt_ui_draw_text(ui, cr.x + cr.w - 8, cr.y, "]", color);
        return;
    }
    for (i = 0; i < thickness; i++) { lt_ui_draw_rect(ui, lt_rect_inset(cr, i, i), color); }
}

int lt_ui_grid_end(lt_ui *ui)
{
    ui_grid *G = ui->grid;
    const lt_theme *t = &ui->theme;
    int flags;
    if (G == NULL || !G->in_frame) { return 0; }
    if (G->edit) {
        lt_u32 eid = G->id ^ 0xED17UL;
        lt_rect cr = cell_rect(G, G->edit_r, G->edit_c);
        int fl, tabdir = 0, mods = 0;
        if (ui->focus != eid) { ui->focus = eid; }
        if (ui_key_take(ui, LT_KEY_TAB, &mods)) { tabdir = (mods & LT_MOD_SHIFT) ? -1 : 1; }
        fl = ui_edit_widget(ui, eid, lt_rect_inset(cr, 0, 0), G->edit_buf, sizeof(G->edit_buf),
                            LT_UI_TEXT_NUMERIC | (G->edit_wants ? LT_UI_TEXT_SELECTALL : 0), 1);
        if (fl & LT_UI_CANCEL) {
            G->edit = 0;
            G->flags_out |= LT_UI_GRID_EDIT_CANCEL;
            ui->focus = G->id;
        } else if ((fl & LT_UI_SUBMIT) || tabdir != 0) {
            edit_commit(ui, G);
            if (tabdir != 0) {
                int nc = G->edit_c + tabdir, nr = G->edit_r;
                if (nc >= G->cols) { nc = 0; nr++; }
                if (nc < 0) { nc = G->cols - 1; nr--; }
                if (nr < 0 || nr >= G->rows) { nr = G->edit_r; nc = G->edit_c; }
                move_cursor(ui, G, nr, clampi(nc, 0, G->cols - 1), 0);
            }
        } else if (ui->focus != eid) {
            /* focus moved elsewhere: commit (click-away) */
            G->edit = 0;
            G->flags_out |= LT_UI_GRID_EDIT_COMMIT;
        }
    }
    lt_ui_scroll_end(ui);
    if (!ui->cell) { lt_ui_draw_rect(ui, G->view, ui->focus == G->id && ui->kbd_nav ? t->focus : t->border_subtle); }
    lt_ui_pop_id(ui);
    if (G->serial != G->last_serial) {
        G->flags_out |= LT_UI_GRID_SELECTION;
        G->last_serial = G->serial;
    }
    flags = G->flags_out;
    G->flags_out = 0;
    G->in_frame = 0;
    return flags;
}
