/* ui.c - immediate-mode toolkit core: context, input, ids, layers, draw
 * command lists, layout, interaction and focus handling.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "ui_priv.h"
#include "icons.h"

/* ------------------------------------------------------------------ */
/* create / destroy                                                    */
lt_ui *lt_ui_create(void)
{
    lt_ui *ui = (lt_ui *)lt_calloc(1, sizeof(*ui));
    int i;
    lt_theme_dark(&ui->theme);
    ui->pct = 100;
    ui->wake_in = -1.0;
    lt_buf_init(&ui->tbuf);
    lt_buf_init(&ui->clipbuf);
    lt_vec_init(&ui->dpool, sizeof(double));
    for (i = 0; i < UI_MAX_LAYERS; i++) {
        lt_vec_init(&ui->layers[i].cmds, sizeof(ui_cmd));
    }
    ui->tab_cap = 512;
    ui->tab = (ui_state **)lt_calloc((size_t)ui->tab_cap, sizeof(ui_state *));
    ui->extra = 2;
    ui->dirty = 1;
    ui->hover_layer = 1;
    return ui;
}

void lt_ui_destroy(lt_ui *ui)
{
    int i;
    if (ui == NULL) { return; }
    for (i = 0; i < UI_MAX_LAYERS; i++) { lt_vec_free(&ui->layers[i].cmds); }
    for (i = 0; i < ui->tab_cap; i++) { lt_free(ui->tab[i]); }
    for (i = 0; i < 4; i++) { lt_free(ui->grids[i].mask); }
    lt_free(ui->tab);
    lt_buf_free(&ui->tbuf);
    lt_buf_free(&ui->clipbuf);
    lt_vec_free(&ui->dpool);
    lt_free(ui);
}

lt_theme *lt_ui_theme(lt_ui *ui) { return &ui->theme; }

void lt_ui_theme_changed(lt_ui *ui)
{
    ui_theme_metrics(&ui->theme, ui->cell, ui->pct);
    ui->extra = 2;
}

void lt_ui_set_theme(lt_ui *ui, const lt_theme *t)
{
    ui->theme = *t;
    lt_ui_theme_changed(ui);
}

void lt_ui_set_scale_pct(lt_ui *ui, int pct)
{
    ui->pct = LT_CLAMP(pct, 100, 300);
    lt_ui_theme_changed(ui);
}

void lt_ui_set_scale(lt_ui *ui, int scale)
{
    lt_ui_set_scale_pct(ui, LT_CLAMP(scale, 1, 3) * 100);
}

int lt_ui_scale_pct(const lt_ui *ui) { return ui->pct; }

/* px at 100% -> device pixels, rounded half away from zero; 64-bit
 * intermediate so large px cannot overflow. Identity in cell mode. */
int lt_ui_px_pct(int px, int pct)
{
    lt_s64 v;
    if (pct <= 0) { pct = 100; }
    v = (lt_s64)px * (lt_s64)pct;
    v = v >= 0 ? (v + 50) / 100 : -((-v + 50) / 100);
    if (v > 2147483647) { return 2147483647; }
    if (v < -2147483647) { return -2147483647; }
    return (int)v;
}

int lt_ui_px(const lt_ui *ui, int px)
{
    return ui->cell ? px : lt_ui_px_pct(px, ui->pct);
}

/* inverse of lt_ui_px (device px -> 100% px, rounded down) */
int lt_ui_unpx(const lt_ui *ui, int px)
{
    return ui->cell ? px : (int)(((lt_s64)px * 100) / ui->pct);
}

/* integer scale for the bitmap-font fallback */
int ui_bscale(const lt_ui *ui)
{
    return ui->cell ? 1 : LT_MAX(1, (ui->pct + 50) / 100);
}

/* height of one text line box (16 at 100%) for whichever font path is active */
int ui_text_box_h(const lt_ui *ui)
{
    if (ui->cell) { return 16; }
    if (ui->g != NULL && (ui->g->bitmap_text || ui->g->cells != NULL)) { return 16 * ui_bscale(ui); }
    return lt_ui_px(ui, 16);
}

void lt_ui_set_backend(lt_ui *ui, lt_backend *b) { ui->be = b; }
lt_bool lt_ui_cell_mode(const lt_ui *ui) { return ui->cell; }
int lt_ui_row_height(const lt_ui *ui) { return ui->theme.row_h; }
double lt_ui_now(const lt_ui *ui) { return ui->now; }
lt_bool lt_ui_quit_requested(const lt_ui *ui) { return ui->quit ? LT_TRUE : LT_FALSE; }
lt_rect lt_ui_surface(const lt_ui *ui) { return lt_rect_make(0, 0, ui->sw, ui->sh); }

lt_rect lt_ui_client_rect(const lt_ui *ui)
{
    return lt_rect_make(0, ui->used_top, ui->sw, LT_MAX(0, ui->sh - ui->used_top - ui->bottom_used));
}

/* ------------------------------------------------------------------ */
/* persistent state table                                              */
static void tab_insert(ui_state **tab, int cap, ui_state *s)
{
    int i = (int)(s->id % (lt_u32)cap);
    while (tab[i] != NULL) { i = (i + 1) % cap; }
    tab[i] = s;
}

static void tab_grow(lt_ui *ui, int newcap, lt_u32 min_frame)
{
    ui_state **nt = (ui_state **)lt_calloc((size_t)newcap, sizeof(ui_state *));
    int i, n = 0;
    for (i = 0; i < ui->tab_cap; i++) {
        ui_state *s = ui->tab[i];
        if (s == NULL) { continue; }
        if (s->frame < min_frame) {
            lt_free(s);
        } else {
            tab_insert(nt, newcap, s);
            n++;
        }
    }
    lt_free(ui->tab);
    ui->tab = nt;
    ui->tab_cap = newcap;
    ui->tab_count = n;
}

ui_state *ui_state_get(lt_ui *ui, lt_u32 id)
{
    int i = (int)(id % (lt_u32)ui->tab_cap);
    ui_state *s;
    while (ui->tab[i] != NULL) {
        if (ui->tab[i]->id == id) {
            ui->tab[i]->frame = (lt_u32)ui->frame_no;
            return ui->tab[i];
        }
        i = (i + 1) % ui->tab_cap;
    }
    if ((ui->tab_count + 1) * 2 > ui->tab_cap) {
        tab_grow(ui, ui->tab_cap * 2, 0);
        return ui_state_get(ui, id);
    }
    s = (ui_state *)lt_calloc(1, sizeof(*s));
    s->id = id;
    s->frame = (lt_u32)ui->frame_no;
    ui->tab[i] = s;
    ui->tab_count++;
    return s;
}

/* ------------------------------------------------------------------ */
/* ids                                                                 */
lt_u32 ui_hash(lt_u32 seed, const char *s)
{
    lt_u32 h = seed ? seed : 0x811C9DC5UL;
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        h ^= *p++;
        h = (h * 16777619UL) & LT_U32_MASK;
    }
    return h ? h : 1;
}

lt_u32 lt_ui_id(lt_ui *ui, const char *label)
{
    lt_u32 seed = ui->nid > 0 ? ui->idstack[ui->nid - 1] : 0;
    const char *p = strstr(label, "###");
    if (p != NULL) { label = p + 3; }
    return ui_hash(seed, label);
}

void lt_ui_push_id(lt_ui *ui, const char *s)
{
    lt_u32 id = lt_ui_id(ui, s);
    if (ui->nid < UI_MAX_ID) { ui->idstack[ui->nid++] = id; }
}

void lt_ui_push_id_int(lt_ui *ui, int n)
{
    char b[24];
    lt_snprintf(b, sizeof(b), "#%d", n);
    lt_ui_push_id(ui, b);
}

void lt_ui_pop_id(lt_ui *ui)
{
    if (ui->nid > 0) { ui->nid--; }
}

lt_u32 ui_id_label(lt_ui *ui, const char *label) { return lt_ui_id(ui, label); }

/* visible part of a label: up to "##" */
const char *ui_label_visible(const char *label, char *out, size_t cap)
{
    const char *p = strstr(label, "##");
    size_t n;
    if (p == NULL) { return label; }
    n = (size_t)(p - label);
    if (n >= cap) { n = cap - 1; }
    memcpy(out, label, n);
    out[n] = '\0';
    return out;
}

/* ------------------------------------------------------------------ */
/* input                                                               */
static int norm_mods(int m)
{
    if (m & LT_MOD_SUPER) { m |= LT_MOD_CTRL; }
    return m & (LT_MOD_SHIFT | LT_MOD_CTRL | LT_MOD_ALT);
}

static ui_evq *q_push(lt_ui *ui)
{
    ui_evq *e;
    if (ui->nq >= UI_EVQ) { return NULL; }
    e = &ui->q[ui->nq++];
    memset(e, 0, sizeof(*e));
    return e;
}

void lt_ui_input(lt_ui *ui, const lt_event *ev)
{
    ui_evq *e;
    ui->dirty = 1;
    switch (ev->type) {
    case LT_EV_QUIT: ui->quit = 1; break;
    case LT_EV_MOUSE_MOVE:
        ui->mx = ev->x; ui->my = ev->y;
        break;
    case LT_EV_MOUSE_DOWN:
        ui->mx = ev->x; ui->my = ev->y;
        if (ev->button >= 1 && ev->button <= 3) {
            ui->mbtn |= 1 << ev->button;
            ui->mpressed |= 1 << ev->button;
            ui->mpress_x = ev->x; ui->mpress_y = ev->y;
        }
        ui->mods = norm_mods(ev->mods);
        break;
    case LT_EV_MOUSE_UP:
        ui->mx = ev->x; ui->my = ev->y;
        if (ev->button >= 1 && ev->button <= 3) {
            ui->mbtn &= ~(1 << ev->button);
            ui->mreleased |= 1 << ev->button;
        }
        break;
    case LT_EV_WHEEL:
        ui->mx = ev->x; ui->my = ev->y;
        ui->wheel += ev->wheel;
        ui->mods = norm_mods(ev->mods);
        break;
    case LT_EV_KEY_DOWN:
        ui->mods = norm_mods(ev->mods);
        e = q_push(ui);
        if (e != NULL) { e->key = ev->key; e->mods = norm_mods(ev->mods); }
        break;
    case LT_EV_KEY_UP:
        ui->mods = norm_mods(ev->mods);
        break;
    case LT_EV_TEXT:
        e = q_push(ui);
        if (e != NULL) {
            e->is_text = 1;
            lt_strlcpy(e->text, ev->text, sizeof(e->text));
            e->mods = ui->mods;
        }
        break;
    case LT_EV_FOCUS_LOST:
        ui->mbtn = 0;
        ui->mreleased |= (1 << LT_MB_LEFT);
        break;
    default: break;
    }
}

int ui_key_take(lt_ui *ui, int key, int *mods_out)
{
    int i;
    for (i = 0; i < ui->nq; i++) {
        ui_evq *e = &ui->q[i];
        if (!e->is_text && !e->used && e->key == key) {
            e->used = 1;
            if (mods_out != NULL) { *mods_out = e->mods; }
            return 1;
        }
    }
    return 0;
}

int ui_key_take_exact(lt_ui *ui, int key, int mods)
{
    int i;
    for (i = 0; i < ui->nq; i++) {
        ui_evq *e = &ui->q[i];
        if (!e->is_text && !e->used && e->key == key && e->mods == mods) {
            e->used = 1;
            return 1;
        }
    }
    return 0;
}

int ui_enter_pressed(lt_ui *ui) { return ui_key_take(ui, LT_KEY_ENTER, NULL); }

int ui_pressed(const lt_ui *ui, int button)
{
    return (ui->mpressed & (1 << button)) && !ui->popup_dismiss;
}

lt_bool lt_ui_key_pressed(lt_ui *ui, int key, int mods)
{
    ui_layer *L;
    if (ui->edit.id != 0) {
        int fkey = key >= LT_KEY_F1 && key <= LT_KEY_F12;
        int used_by_field = (mods & LT_MOD_CTRL) &&
                            (key == 'a' || key == 'c' || key == 'v' || key == 'x' || key == 'z');
        int plain = !(mods & (LT_MOD_CTRL | LT_MOD_ALT));
        if (!fkey && (plain || used_by_field)) { return LT_FALSE; }
    }
    L = ui->nlstack > 0 ? &ui->layers[ui->lstack[ui->nlstack - 1]] : NULL;
    if (L != NULL && L->blocked) { return LT_FALSE; }
    return ui_key_take_exact(ui, key, norm_mods(mods)) ? LT_TRUE : LT_FALSE;
}

lt_bool lt_ui_text_focused(const lt_ui *ui) { return ui->edit.id != 0 ? LT_TRUE : LT_FALSE; }
lt_point lt_ui_mouse(const lt_ui *ui) { lt_point p; p.x = ui->mx; p.y = ui->my; return p; }
int lt_ui_mods(const lt_ui *ui) { return ui->mods; }
lt_bool lt_ui_mouse_down(const lt_ui *ui, int b) { return (ui->mbtn & (1 << b)) ? LT_TRUE : LT_FALSE; }
lt_u32 lt_ui_focus(const lt_ui *ui) { return ui->focus; }
void lt_ui_set_focus(lt_ui *ui, lt_u32 id) { ui->focus = id; ui_request(ui); }
lt_rect lt_ui_last_rect(const lt_ui *ui) { return ui->last_rect; }
lt_u32 lt_ui_last_id(const lt_ui *ui) { return ui->last_id; }
lt_bool lt_ui_last_hovered(const lt_ui *ui) { return ui->last_hover ? LT_TRUE : LT_FALSE; }
lt_bool lt_ui_last_focused(const lt_ui *ui) { return ui->last_id != 0 && ui->focus == ui->last_id; }

void lt_ui_clipboard_set(lt_ui *ui, const char *utf8)
{
    lt_buf_clear(&ui->clipbuf);
    lt_buf_puts(&ui->clipbuf, utf8);
    if (ui->be != NULL && ui->be->set_clipboard != NULL) { ui->be->set_clipboard(ui->be, utf8); }
}

char *lt_ui_clipboard_get(lt_ui *ui)
{
    char *s = NULL;
    if (ui->be != NULL && ui->be->get_clipboard != NULL) { s = ui->be->get_clipboard(ui->be); }
    if (s == NULL && ui->clipbuf.len > 0) { s = lt_strdup(ui->clipbuf.data); }
    return s;
}

void lt_ui_disabled_begin(lt_ui *ui, lt_bool disabled)
{
    if (disabled) { ui->disabled++; }
}

void lt_ui_disabled_end(lt_ui *ui)
{
    if (ui->disabled > 0) { ui->disabled--; }
}

int ui_disabled(const lt_ui *ui) { return ui->disabled > 0; }

lt_color ui_dim(lt_ui *ui, lt_color c)
{
    return ui->disabled ? lt_color_mix(c, ui->theme.bg_app, 150) : c;
}

void ui_request(lt_ui *ui) { ui->extra = 2; }

void ui_animate(lt_ui *ui, int ms)
{
    if (ms < 1) { ms = 1; }
    if (ui->wake_in < 0.0 || (double)ms < ui->wake_in) { ui->wake_in = (double)ms; }
}

lt_bool lt_ui_needs_redraw(const lt_ui *ui) { return (ui->dirty || ui->extra > 0) ? LT_TRUE : LT_FALSE; }
void lt_ui_request_redraw(lt_ui *ui) { ui->dirty = 1; }

int lt_ui_wait_hint_ms(const lt_ui *ui)
{
    if (ui->wake_in < 0.0) { return -1; }
    return (int)ui->wake_in;
}

/* ------------------------------------------------------------------ */
/* layers and draw commands                                            */
ui_layer *ui_layer_cur(lt_ui *ui)
{
    if (ui->nlstack == 0) { return &ui->layers[0]; }
    return &ui->layers[ui->lstack[ui->nlstack - 1]];
}

int ui_layer_z(const lt_ui *ui)
{
    if (ui->nlstack == 0) { return 0; }
    return ui->layers[ui->lstack[ui->nlstack - 1]].z;
}

ui_layer *ui_layer_begin(lt_ui *ui, lt_u32 id, int kind, int z, lt_rect rect)
{
    ui_layer *L;
    ui_cmd *c;
    int idx;
    if (ui->nlayers >= UI_MAX_LAYERS || ui->nlstack >= UI_MAX_LSTACK) {
        /* out of layers: keep drawing into the current one (stack stays balanced) */
        idx = ui->nlstack > 0 ? ui->lstack[ui->nlstack - 1] : 0;
        if (ui->nlstack < UI_MAX_LSTACK) {
            ui->lstack[ui->nlstack++] = idx;
            if (ui->nclip < UI_MAX_CLIPS) { ui->clip[ui->nclip] = ui->clip[ui->nclip - 1]; ui->nclip++; }
        }
        return &ui->layers[idx];
    }
    idx = ui->nlayers++;
    L = &ui->layers[idx];
    L->id = id; L->kind = kind; L->z = z; L->seq = idx; L->rect = rect;
    L->bg_cmd = -1;
    L->blocked = (ui->modal_z_top > 0 && z < ui->modal_z_top) ? 1 : 0;
    lt_vec_clear(&L->cmds);
    ui->lstack[ui->nlstack++] = idx;
    /* each layer starts with an independent clip */
    if (ui->nclip < UI_MAX_CLIPS) { ui->clip[ui->nclip++] = lt_rect_make(0, 0, ui->sw, ui->sh); }
    c = ui_cmd_push(ui, UC_CLIP);
    c->r = lt_rect_make(0, 0, ui->sw, ui->sh);
    return L;
}

void ui_layer_end(lt_ui *ui)
{
    if (ui->nlstack > 0) { ui->nlstack--; }
    if (ui->nclip > 1) { ui->nclip--; }
}

ui_cmd *ui_cmd_push(lt_ui *ui, int type)
{
    static ui_cmd dummy;
    ui_layer *L = ui_layer_cur(ui);
    ui_cmd *c;
    if (!ui->in_frame) { memset(&dummy, 0, sizeof(dummy)); return &dummy; }
    c = (ui_cmd *)lt_vec_push(&L->cmds);
    c->type = type;
    return c;
}

lt_rect ui_clip_cur(const lt_ui *ui)
{
    if (ui->nclip == 0) { return lt_rect_make(0, 0, ui->sw, ui->sh); }
    return ui->clip[ui->nclip - 1];
}

static int rect_vis(const lt_ui *ui, lt_rect r)
{
    lt_rect c = ui_clip_cur(ui);
    return r.x < c.x + c.w && r.x + r.w > c.x && r.y < c.y + c.h && r.y + r.h > c.y;
}

void lt_ui_clip_push(lt_ui *ui, lt_rect r)
{
    ui_cmd *c;
    lt_rect nr = lt_rect_intersect(r, ui_clip_cur(ui));
    if (ui->nclip >= UI_MAX_CLIPS) { return; }
    ui->clip[ui->nclip++] = nr;
    c = ui_cmd_push(ui, UC_CLIP);
    c->r = nr;
}

void lt_ui_clip_pop(lt_ui *ui)
{
    ui_cmd *c;
    if (ui->nclip <= 1) { return; }
    ui->nclip--;
    c = ui_cmd_push(ui, UC_CLIP);
    c->r = ui_clip_cur(ui);
}

lt_rect lt_ui_clip_get(const lt_ui *ui) { return ui_clip_cur(ui); }

static long arena_text(lt_ui *ui, const char *s)
{
    long off = (long)ui->tbuf.len;
    lt_buf_append(&ui->tbuf, s, strlen(s) + 1);
    return off;
}

void lt_ui_draw_fill(lt_ui *ui, lt_rect r, lt_color c)
{
    ui_cmd *k;
    if (LT_COLOR_A(c) == 0 || r.w <= 0 || r.h <= 0 || !rect_vis(ui, r)) { return; }
    k = ui_cmd_push(ui, UC_FILL);
    k->r = r; k->c = c;
}

void lt_ui_draw_rect(lt_ui *ui, lt_rect r, lt_color c)
{
    ui_cmd *k;
    if (LT_COLOR_A(c) == 0 || r.w <= 0 || r.h <= 0 || !rect_vis(ui, r)) { return; }
    k = ui_cmd_push(ui, UC_RECT);
    k->r = r; k->c = c;
}

void lt_ui_draw_round(lt_ui *ui, lt_rect r, int radius, lt_color c)
{
    ui_cmd *k;
    if (LT_COLOR_A(c) == 0 || r.w <= 0 || r.h <= 0 || !rect_vis(ui, r)) { return; }
    k = ui_cmd_push(ui, UC_ROUND);
    k->r = r; k->c = c; k->i[0] = ui->cell ? 0 : radius;
}

void lt_ui_draw_hline(lt_ui *ui, int x0, int x1, int y, lt_color c)
{
    ui_cmd *k;
    if (LT_COLOR_A(c) == 0) { return; }
    k = ui_cmd_push(ui, UC_HLINE);
    k->i[0] = x0; k->i[1] = x1; k->i[2] = y; k->c = c;
}

void lt_ui_draw_vline(lt_ui *ui, int x, int y0, int y1, lt_color c)
{
    ui_cmd *k;
    if (LT_COLOR_A(c) == 0) { return; }
    k = ui_cmd_push(ui, UC_VLINE);
    k->i[0] = x; k->i[1] = y0; k->i[2] = y1; k->c = c;
}

void lt_ui_draw_line(lt_ui *ui, double x0, double y0, double x1, double y1, double w, lt_color c)
{
    ui_cmd *k = ui_cmd_push(ui, UC_LINE);
    k->d[0] = x0; k->d[1] = y0; k->d[2] = x1; k->d[3] = y1; k->d[4] = w; k->c = c;
}

void lt_ui_draw_circle_fill(lt_ui *ui, double cx, double cy, double r, lt_color c)
{
    ui_cmd *k = ui_cmd_push(ui, UC_CFILL);
    k->d[0] = cx; k->d[1] = cy; k->d[2] = r; k->c = c;
}

void lt_ui_draw_circle(lt_ui *ui, double cx, double cy, double r, double w, lt_color c)
{
    ui_cmd *k = ui_cmd_push(ui, UC_CIRCLE);
    k->d[0] = cx; k->d[1] = cy; k->d[2] = r; k->d[3] = w; k->c = c;
}

void lt_ui_draw_arc(lt_ui *ui, double cx, double cy, double r, double a0, double a1,
                    double w, lt_color c)
{
    ui_cmd *k = ui_cmd_push(ui, UC_ARC);
    k->d[0] = cx; k->d[1] = cy; k->d[2] = r; k->d[3] = a0; k->d[4] = a1; k->d[5] = w; k->c = c;
}

void lt_ui_draw_poly(lt_ui *ui, const double *xy, int n, lt_color c)
{
    ui_cmd *k;
    int i;
    if (n < 3) { return; }
    k = ui_cmd_push(ui, UC_POLY);
    k->off = (long)ui->dpool.len;
    k->i[0] = n;
    k->c = c;
    for (i = 0; i < 2 * n; i++) {
        double *d = (double *)lt_vec_push(&ui->dpool);
        *d = xy[i];
    }
}

void lt_ui_draw_vgrad(lt_ui *ui, lt_rect r, lt_color top, lt_color bottom)
{
    ui_cmd *k;
    if (r.w <= 0 || r.h <= 0 || !rect_vis(ui, r)) { return; }
    k = ui_cmd_push(ui, UC_VGRAD);
    k->r = r; k->c = top; k->c2 = bottom;
}

void lt_ui_draw_blit(lt_ui *ui, int x, int y, const lt_u32 *px, int w, int h, int pitch)
{
    ui_cmd *k = ui_cmd_push(ui, UC_BLIT);
    k->r = lt_rect_make(x, y, w, h);
    k->px = px; k->i[0] = pitch;
}

/* ---- text: proportional IBM Plex in pixel mode, 8x16 cells in cell mode ---- */
static lt_font *ui_ttf(const lt_ui *ui, int font)
{
    if (ui->cell || (ui->g != NULL && (ui->g->bitmap_text || ui->g->cells != NULL))) { return NULL; }
    return lt_font_get((lt_font_id)font);
}

int ui_font_px(const lt_ui *ui, int px)
{
    if (px <= 0) { px = ui->tpx > 0 ? ui->tpx : LT_UI_FS_BASE; }
    return lt_ui_px_pct(px, ui->cell ? 100 : ui->pct);
}

int ui_line_h(const lt_ui *ui)
{
    if (ui->cell) { return 16; }
    return (ui_font_px(ui, 0) * 7 + 2) / 5;      /* 1.4 line height */
}

int ui_text_w(const lt_ui *ui, const char *s, int font, int px)
{
    lt_font *f;
    if (s == NULL || s[0] == '\0') { return 0; }
    f = ui_ttf(ui, font);
    if (f == NULL) { return lt_gfx_text_width(s, ui_bscale(ui)); }
    return (int)ceil(lt_gfx_text_measure(f, (double)ui_font_px(ui, px), s));
}

int ui_prefix_w(const lt_ui *ui, const char *s, int nbytes)
{
    char tmp[512];
    if (nbytes <= 0) { return 0; }
    if ((size_t)nbytes >= sizeof(tmp)) { nbytes = (int)sizeof(tmp) - 1; }
    memcpy(tmp, s, (size_t)nbytes);
    tmp[nbytes] = '\0';
    if (ui_ttf(ui, ui->tfont) == NULL) { return lt_gfx_text_width(tmp, ui_bscale(ui)); }
    return (int)floor(lt_gfx_text_measure(ui_ttf(ui, ui->tfont), (double)ui_font_px(ui, 0), tmp) + 0.5);
}

int lt_ui_text_width(const lt_ui *ui, const char *s) { return ui_text_w(ui, s, ui->tfont, 0); }
int lt_ui_text_width_f(const lt_ui *ui, const char *s, int font, int px) { return ui_text_w(ui, s, font, px); }

void lt_ui_text_style(lt_ui *ui, int font, int px) { ui->tfont = font; ui->tpx = px; }
void lt_ui_text_style_reset(lt_ui *ui) { ui->tfont = (int)LT_FONT_SANS; ui->tpx = 0; }

int lt_ui_draw_text_f(lt_ui *ui, int x, int y, const char *s, lt_color c, int font, int px)
{
    ui_cmd *k;
    int sc = ui_bscale(ui);
    int w = ui_text_w(ui, s, font, px);
    if (LT_COLOR_A(c) == 0 || s[0] == '\0') { return w; }
    if (!rect_vis(ui, lt_rect_make(x, y, w, ui_text_box_h(ui)))) { return w; }
    k = ui_cmd_push(ui, UC_TEXT);
    k->r = lt_rect_make(x, y, w, ui_text_box_h(ui));
    k->c = c; k->i[0] = sc; k->i[1] = font; k->i[2] = ui_font_px(ui, px);
    k->off = arena_text(ui, s);
    return w;
}

int lt_ui_draw_text(lt_ui *ui, int x, int y, const char *s, lt_color c)
{
    return lt_ui_draw_text_f(ui, x, y, s, c, ui->tfont, 0);
}

void lt_ui_draw_vtext(lt_ui *ui, double x, double y, const char *s, double height,
                      double rotation, int align, int valign, double stroke, lt_color c)
{
    ui_cmd *k = ui_cmd_push(ui, UC_VTEXT);
    k->d[0] = x; k->d[1] = y; k->d[2] = height; k->d[3] = rotation; k->d[4] = stroke;
    k->i[0] = align; k->i[1] = valign; k->c = c; k->off = arena_text(ui, s);
}

static int cell_attr_underline = LT_CELL_UNDERLINE;
static int cell_attr_bold = LT_CELL_BOLD;

static void cell_attr_cb(lt_gfx *g, lt_rect r, void *user)
{
    lt_gfx_cell_attr(g, r, *(const int *)user, 1);
}

static void cell_shadow_cb(lt_gfx *g, lt_rect r, void *user)
{
    LT_UNUSED(user);
    lt_gfx_cell_shadow(g, r, 2, 1);
}

void ui_cell_shadow(lt_ui *ui, lt_rect r)
{
    if (ui->cell) { lt_ui_draw_custom(ui, r, cell_shadow_cb, NULL); }
}

void ui_cell_underline(lt_ui *ui, lt_rect r)
{
    if (ui->cell) { lt_ui_draw_custom(ui, r, cell_attr_cb, &cell_attr_underline); }
}

void ui_cell_bold(lt_ui *ui, lt_rect r)
{
    if (ui->cell) { lt_ui_draw_custom(ui, r, cell_attr_cb, &cell_attr_bold); }
}

void lt_ui_draw_custom(lt_ui *ui, lt_rect r, lt_ui_draw_fn fn, void *user)
{
    ui_cmd *k = ui_cmd_push(ui, UC_CUSTOM);
    k->r = lt_rect_intersect(r, ui_clip_cur(ui));
    k->fn = fn; k->user = user;
    k->i[0] = r.x; k->i[1] = r.y; k->i[2] = r.w; k->i[3] = r.h;
}

/* truncate with an ellipsis so the result fits in maxw pixels */
int ui_fit_text(const lt_ui *ui, const char *s, int maxw, char *out, size_t cap, int font, int px)
{
    lt_font *f = ui_ttf(ui, font);
    size_t o;
    if (cap == 0) { return 0; }
    if (f != NULL) {
        double fpx = (double)ui_font_px(ui, px);
        double ell = lt_gfx_text_measure(f, fpx, "\xE2\x80\xA6");
        size_t n;
        if (lt_gfx_text_measure(f, fpx, s) <= (double)maxw) { lt_strlcpy(out, s, cap); return 0; }
        n = lt_gfx_text_fit(f, fpx, s, (double)maxw - ell);
        if (n + 4 > cap) { n = cap > 4 ? cap - 4 : 0; }
        while (n > 0 && (((unsigned char)s[n]) & 0xC0) == 0x80) { n--; }
        memcpy(out, s, n);
        o = n;
        if (maxw >= (int)ell && o + 4 <= cap) {
            memcpy(out + o, "\xE2\x80\xA6", 3); o += 3;
        }
        out[o] = '\0';
        return 1;
    }
    {
        int fw = 8;
        int n = 0, fit;
        const char *p = s;
        o = 0;
        while (lt_utf8_next(&p) != 0) { n++; }
        if (n * fw <= maxw) { lt_strlcpy(out, s, cap); return 0; }
        fit = maxw / fw - 1;
        if (fit < 0) { fit = 0; }
        p = s;
        while (fit > 0 && o + 5 < cap) {
            const char *q = p;
            lt_u32 cp = lt_utf8_next(&p);
            if (cp == 0) { break; }
            while (q < p && o + 5 < cap) { out[o++] = *q++; }
            fit--;
        }
        if (maxw >= 2 * fw && o + 4 <= cap) {
            memcpy(out + o, "\xE2\x80\xA6", 3); o += 3;
        }
        out[o] = '\0';
    }
    return 1;
}

void ui_text_in_f(lt_ui *ui, lt_rect r, const char *s, lt_color c, int align, int font, int px)
{
    char tmp[256];
    int sc = ui_bscale(ui);
    int tw, th = ui_text_box_h(ui), x, y, dy;
    if (s == NULL || s[0] == '\0') { return; }
    tw = ui_text_w(ui, s, font, px);
    if (tw > r.w) {
        ui_fit_text(ui, s, ui->cell ? r.w / sc : r.w, tmp, sizeof(tmp), font, px);
        s = tmp;
        tw = ui_text_w(ui, s, font, px);
    }
    x = align == 1 ? r.x + (r.w - tw) / 2 : (align == 2 ? r.x + r.w - tw : r.x);
    dy = (r.h - th) / 2;
    if (ui->cell) { dy = (dy / 16) * 16; }
    y = r.y + dy;
    lt_ui_draw_text_f(ui, x, y, s, c, font, px);
}

void ui_text_in(lt_ui *ui, lt_rect r, const char *s, lt_color c, int align)
{
    ui_text_in_f(ui, r, s, c, align, ui->tfont, 0);
}

void lt_ui_draw_text_in_f(lt_ui *ui, lt_rect r, const char *s, lt_color c, int align, int font, int px)
{
    ui_text_in_f(ui, r, s, c, align, font, px);
}

void lt_ui_draw_text_in(lt_ui *ui, lt_rect r, const char *s, lt_color c, int align)
{
    ui_text_in(ui, r, s, c, align);
}

void lt_ui_draw_box(lt_ui *ui, lt_rect r, lt_color fill, lt_color border, int radius)
{
    if (ui->cell) {
        lt_ui_draw_fill(ui, r, fill);
        if (LT_COLOR_A(border) != 0 && r.h >= 32 && r.w >= 16) { lt_ui_draw_rect(ui, r, border); }
        return;
    }
    if (LT_COLOR_A(border) != 0) {
        lt_ui_draw_round(ui, r, radius, border);
        lt_ui_draw_round(ui, lt_rect_inset(r, 1, 1), LT_MAX(0, radius - 1), fill);
    } else {
        lt_ui_draw_round(ui, r, radius, fill);
    }
}

void lt_ui_draw_shadow(lt_ui *ui, lt_rect r, int radius, int spread, int alpha)
{
    int i, n;
    if (ui->cell || spread <= 0 || alpha <= 0) { return; }
    n = spread;
    for (i = n; i >= 1; i--) {
        /* quadratic falloff: outermost rings nearly transparent */
        int a = alpha * (n - i + 1) * (n - i + 1) / (n * n * 3 / 2 + 1);
        lt_rect q = lt_rect_make(r.x - i, r.y - i + n / 2, r.w + 2 * i, r.h + 2 * i);
        if (a < 1) { a = 1; }
        lt_ui_draw_round(ui, q, radius + i, LT_RGBA(0, 0, 0, a));
    }
}

/* input-like box: fill + border, with the stock 2px translucent focus ring */
void ui_field_box(lt_ui *ui, lt_rect r, lt_color fill, lt_color border, int focused)
{
    if (focused && !ui->cell) {
        lt_color rc = (ui->theme.primary & 0x00FFFFFFUL) | 0x44000000UL;
        lt_ui_draw_round(ui, lt_rect_inset(r, -2, -2), ui->theme.radius + 2, rc);
    }
    lt_ui_draw_box(ui, r, fill, border, ui->theme.radius);
}

/* ------------------------------------------------------------------ */
/* icons                                                               */
void lt_ui_draw_lucide(lt_ui *ui, lt_rect r, int lucide_id, int px, double stroke, lt_color c)
{
    ui_cmd *k;
    if (ui->cell) {
        ui_text_in_f(ui, r, "*", c, 1, ui->tfont, 0);
        return;
    }
    if (lucide_id <= 0 || LT_COLOR_A(c) == 0 || !rect_vis(ui, r)) { return; }
    k = ui_cmd_push(ui, UC_ICON);
    k->r = r; k->c = c; k->i[0] = lucide_id;
    k->d[0] = (double)(px > 0 ? px : 14) * (double)ui->pct / 100.0;
    k->d[1] = stroke > 0.0 ? stroke * (double)ui->pct / 100.0 : 0.0;
}

/* legacy glyph ids -> lucide */
static int legacy_lucide(int icon)
{
    switch (icon) {
    case LT_UI_ICON_CHECK: return LT_ICON_CHECK;
    case LT_UI_ICON_CLOSE: return LT_ICON_X;
    case LT_UI_ICON_FOLDER: return LT_ICON_FOLDER;
    case LT_UI_ICON_FILE: return LT_ICON_FILE;
    case LT_UI_ICON_PLUS: return LT_ICON_PLUS;
    case LT_UI_ICON_MINUS: return LT_ICON_MINUS;
    case LT_UI_ICON_MENU: return LT_ICON_MENU;
    case LT_UI_ICON_PLAY: return LT_ICON_PLAY;
    case LT_UI_ICON_STOP: return LT_ICON_SQUARE;
    case LT_UI_ICON_WARN: return LT_ICON_TRIANGLE_ALERT;
    case LT_UI_ICON_INFO: return LT_ICON_INFO;
    case LT_UI_ICON_SEARCH: return LT_ICON_SEARCH;
    case LT_UI_ICON_TABLE: return LT_ICON_TABLE_2;
    case LT_UI_ICON_GAUGE: return LT_ICON_GAUGE;
    default: return 0;
    }
}

void lt_ui_draw_icon(lt_ui *ui, lt_rect r, int icon, lt_color c)
{
    double cx = r.x + r.w / 2.0, cy = r.y + r.h / 2.0;
    double k = (double)LT_MIN(r.w, r.h) / 16.0;
    double pts[16];
    if (icon >= 1000) {
        int px = LT_MIN(LT_MIN(r.w, r.h) - 2, lt_ui_px(ui, 14));
        px = lt_ui_unpx(ui, px);
        lt_ui_draw_lucide(ui, r, icon - 1000, px, 0.0, c);
        return;
    }
    if (!ui->cell) {
        int lid = legacy_lucide(icon);
        if (lid != 0) {
            int sz = LT_MIN(LT_MIN(r.w, r.h) - 2, lt_ui_px(ui, 14));
            double sw = (icon == LT_UI_ICON_CHECK) ? 2.6 : 2.0;
            lt_ui_draw_lucide(ui, r, lid, lt_ui_unpx(ui, sz), sw, c);
            return;
        }
    }
    if (ui->cell) {
        const char *ch;
        switch (icon) {
        case LT_UI_ICON_ARROW_RIGHT: ch = "\xE2\x96\xB8"; break;        /* U+25B8 */
        case LT_UI_ICON_ARROW_DOWN: ch = "\xE2\x96\xBE"; break;         /* U+25BE */
        case LT_UI_ICON_ARROW_UP: ch = "\xE2\x96\xB4"; break;           /* U+25B4 */
        case LT_UI_ICON_ARROW_LEFT: ch = "\xE2\x97\x82"; break;         /* U+25C2 */
        case LT_UI_ICON_CHECK: ch = "\xE2\x9C\x93"; break;              /* U+2713 */
        case LT_UI_ICON_CLOSE: ch = "\xC3\x97"; break;                      /* U+00D7 */
        case LT_UI_ICON_FOLDER: ch = "\xE2\x96\xB8"; break;
        case LT_UI_ICON_FILE: ch = "\xC2\xB7"; break;
        case LT_UI_ICON_PLUS: ch = "+"; break;
        case LT_UI_ICON_MINUS: ch = "-"; break;
        case LT_UI_ICON_DOT: ch = "\xE2\x97\x8F"; break;                /* U+25CF */
        case LT_UI_ICON_MENU: ch = "\xE2\x89\xA1"; break;               /* U+2261 */
        case LT_UI_ICON_PLAY: ch = "\xE2\x96\xB6"; break;
        case LT_UI_ICON_STOP: ch = "\xE2\x96\xA0"; break;
        case LT_UI_ICON_WARN: ch = "!"; break;
        case LT_UI_ICON_INFO: ch = "i"; break;
        case LT_UI_ICON_SEARCH: ch = "/"; break;
        case LT_UI_ICON_TABLE: ch = "\xE2\x96\xA6"; break;
        case LT_UI_ICON_GAUGE: ch = "\xE2\x97\x8B"; break;
        default: return;
        }
        ui_text_in(ui, r, ch, c, 1);
        return;
    }
    switch (icon) {
    case LT_UI_ICON_ARROW_RIGHT:
        pts[0] = cx - 2.5 * k; pts[1] = cy - 4.5 * k; pts[2] = cx + 3.0 * k; pts[3] = cy;
        pts[4] = cx - 2.5 * k; pts[5] = cy + 4.5 * k;
        lt_ui_draw_poly(ui, pts, 3, c);
        break;
    case LT_UI_ICON_ARROW_DOWN:
        pts[0] = cx - 4.5 * k; pts[1] = cy - 2.5 * k; pts[2] = cx + 4.5 * k; pts[3] = cy - 2.5 * k;
        pts[4] = cx; pts[5] = cy + 3.0 * k;
        lt_ui_draw_poly(ui, pts, 3, c);
        break;
    case LT_UI_ICON_ARROW_UP:
        pts[0] = cx - 4.5 * k; pts[1] = cy + 2.5 * k; pts[2] = cx + 4.5 * k; pts[3] = cy + 2.5 * k;
        pts[4] = cx; pts[5] = cy - 3.0 * k;
        lt_ui_draw_poly(ui, pts, 3, c);
        break;
    case LT_UI_ICON_ARROW_LEFT:
        pts[0] = cx + 2.5 * k; pts[1] = cy - 4.5 * k; pts[2] = cx - 3.0 * k; pts[3] = cy;
        pts[4] = cx + 2.5 * k; pts[5] = cy + 4.5 * k;
        lt_ui_draw_poly(ui, pts, 3, c);
        break;
    case LT_UI_ICON_DOT:
        lt_ui_draw_circle_fill(ui, cx, cy, 2.5 * k, c);
        break;
    default: break;
    }
}

/* ------------------------------------------------------------------ */
/* layout                                                              */
ui_layout *ui_lay(lt_ui *ui) { return &ui->lay[ui->nlay > 0 ? ui->nlay - 1 : 0]; }

/* cell mode: layouts live on the character grid (8x16 px cells) */
static int cell_snap_h(int h) { return LT_MAX(16, (h + 4) / 16 * 16); }
static int cell_snap_w(int w) { return LT_MAX(8, (w + 3) / 8 * 8); }

void ui_layout_push(lt_ui *ui, lt_rect body)
{
    ui_layout *L;
    if (ui->nlay >= UI_MAX_LAYOUTS) { return; }
    if (ui->cell) {
        int x0 = (body.x + 7) / 8 * 8, y0 = (body.y + 15) / 16 * 16;
        int x1 = (body.x + body.w) / 8 * 8, y1 = (body.y + body.h) / 16 * 16;
        if (body.x < 0 || body.y < 0) { x0 = body.x; y0 = body.y; x1 = body.x + body.w; y1 = body.y + body.h; }
        body = lt_rect_make(x0, y0, LT_MAX(0, x1 - x0), LT_MAX(0, y1 - y0));
    }
    L = &ui->lay[ui->nlay++];
    memset(L, 0, sizeof(*L));
    L->body = body;
    L->sp_x = ui->theme.spacing_x;
    L->sp_y = ui->theme.spacing_y;
    L->ncols = 1;
    L->widths[0] = -1;
    L->col = 1;
    L->x = body.x;
    L->max_x = body.x;
    L->max_y = body.y;
}

void ui_layout_pop(lt_ui *ui)
{
    if (ui->nlay > 1) { ui->nlay--; }
}

/* device-pixel layout row (internal): widths > 0 and height > 0 are device px,
 * widths < 0 are weights, height < 0 reserves -height-1 px at the bottom */
void ui_layout_row_dev(lt_ui *ui, int n, const int *widths, int height)
{
    ui_layout *L = ui_lay(ui);
    int i;
    if (n < 1) { n = 1; }
    if (n > LT_UI_MAX_COLS) { n = LT_UI_MAX_COLS; }
    L->ncols = n;
    for (i = 0; i < n; i++) { L->widths[i] = widths != NULL ? widths[i] : -1; }
    L->spec_h = height;
    L->col = n;
}

/* public layout row: widths/height in px at 100% scale */
void lt_ui_layout_row(lt_ui *ui, int n, const int *widths, int height)
{
    int w[LT_UI_MAX_COLS], i;
    if (n < 1) { n = 1; }
    if (n > LT_UI_MAX_COLS) { n = LT_UI_MAX_COLS; }
    if (widths != NULL) {
        for (i = 0; i < n; i++) { w[i] = widths[i] > 0 ? lt_ui_px(ui, widths[i]) : widths[i]; }
    }
    if (height > 0) { height = lt_ui_px(ui, height); }
    else if (height < 0) { height = -(lt_ui_px(ui, -height - 1) + 1); }
    ui_layout_row_dev(ui, n, widths != NULL ? w : NULL, height);
}

static void row_start(lt_ui *ui, ui_layout *L)
{
    if (L->started) { L->row_y += L->row_h + L->sp_y; } else { L->row_y = L->body.y; }
    L->started = 1;
    if (L->spec_h > 0) {
        L->row_h = ui->cell ? cell_snap_h(L->spec_h) : L->spec_h;
    } else if (L->spec_h == 0) {
        L->row_h = ui->theme.row_h;
    } else {
        int avail = L->body.y + L->body.h - L->row_y - (-L->spec_h - 1);
        if (ui->cell && ui->nmodal > 0 && ui->nlay - 1 == ui->modal_lay[ui->nmodal - 1]) {
            ui->modal_fill[ui->nmodal - 1]++;
        }
        L->row_h = LT_MAX(avail, ui->theme.row_h);
        if (ui->cell) { L->row_h = LT_MAX(16, L->row_h / 16 * 16); }
    }
    L->x = L->body.x + L->indent;
    L->col = 0;
}

lt_rect ui_next(lt_ui *ui, int pref_w)
{
    ui_layout *L = ui_lay(ui);
    lt_rect r;
    int w, cw, right, later, k;
    if (L->has_override) {
        L->has_override = 0;
        r = L->override_r;
        if (r.x + r.w > L->max_x) { L->max_x = r.x + r.w; }
        if (r.y + r.h > L->max_y) { L->max_y = r.y + r.h; }
        return r;
    }
    if (L->col >= L->ncols) { row_start(ui, L); }
    cw = L->widths[L->col];
    right = L->body.x + L->body.w;
    if (cw > 0) {
        w = cw;
    } else if (cw == 0) {
        w = pref_w > 0 ? pref_w : right - L->x;
    } else {
        int wsum = 0;
        later = 0;
        for (k = L->col; k < L->ncols; k++) {
            if (L->widths[k] < 0) { wsum += -L->widths[k]; }
        }
        for (k = L->col + 1; k < L->ncols; k++) {
            later += L->sp_x;
            if (L->widths[k] > 0) { later += L->widths[k]; }
        }
        w = (right - L->x - later) * (-cw) / (wsum > 0 ? wsum : 1);
        if (L->col + 1 == L->ncols) { w = right - L->x; }
    }
    if (w < 1) { w = 1; }
    if (ui->cell) {
        w = cell_snap_w(w);
        if (L->col + 1 == L->ncols && cw <= 0 && right - L->x >= 8) { w = LT_MAX(8, (right - L->x) / 8 * 8); }
    }
    r = lt_rect_make(L->x, L->row_y, w, L->row_h);
    L->x += w + L->sp_x;
    L->col++;
    if (r.x + r.w > L->max_x) { L->max_x = r.x + r.w; }
    if (r.y + r.h > L->max_y) { L->max_y = r.y + r.h; }
    return r;
}

lt_rect lt_ui_layout_next(lt_ui *ui) { return ui_next(ui, 0); }

/* full-width cell of the given height without disturbing the row spec */
lt_rect ui_next_full(lt_ui *ui, int height)
{
    ui_layout *L = ui_lay(ui);
    int w[LT_UI_MAX_COLS], n = L->ncols, h = L->spec_h;
    lt_rect r;
    memcpy(w, L->widths, sizeof(w));
    ui_layout_row_dev(ui, 1, NULL, height);
    r = ui_next(ui, 0);
    L->ncols = n;
    memcpy(L->widths, w, sizeof(w));
    L->spec_h = h;
    L->col = n;
    return r;
}

/* hover without claiming presses (labels, passive widgets) */
int ui_hover_only(lt_ui *ui, lt_u32 id, lt_rect r)
{
    if (ui->active == 0 && ui_over(ui, r) && ui->hover == 0) { ui->hover = id; }
    ui_set_last(ui, id, r);
    return ui->hover == id;
}

void lt_ui_layout_set_rect(lt_ui *ui, lt_rect r)
{
    ui_layout *L = ui_lay(ui);
    L->has_override = 1;
    L->override_r = r;
}

void ui_layout_space_dev(lt_ui *ui, int px)
{
    ui_layout *L = ui_lay(ui);
    if (ui->cell) { px = (px + 4) / 16 * 16; }
    if (L->started) {
        L->row_h += px;
    } else {
        L->row_y = L->body.y;
        L->row_h = px - L->sp_y;
        L->started = 1;
    }
    L->col = L->ncols;
    if (L->row_y + L->row_h > L->max_y) { L->max_y = L->row_y + L->row_h; }
}

void lt_ui_layout_space(lt_ui *ui, int px) { ui_layout_space_dev(ui, lt_ui_px(ui, px)); }

void lt_ui_layout_indent(lt_ui *ui, int dx)
{
    ui_layout *L = ui_lay(ui);
    L->indent += lt_ui_px(ui, dx);
    if (L->indent < 0) { L->indent = 0; }
}

void lt_ui_layout_gap(lt_ui *ui, int x, int y)
{
    ui_layout *L = ui_lay(ui);
    L->sp_x = lt_ui_px(ui, x);
    L->sp_y = lt_ui_px(ui, y);
}

int lt_ui_layout_width(const lt_ui *ui) { return ui->lay[ui->nlay > 0 ? ui->nlay - 1 : 0].body.w; }
lt_rect lt_ui_layout_body(const lt_ui *ui) { return ui->lay[ui->nlay > 0 ? ui->nlay - 1 : 0].body; }

void lt_ui_layout_column_begin(lt_ui *ui)
{
    lt_rect r = ui_next(ui, 0);
    ui_layout_push(ui, r);
}

void lt_ui_layout_column_end(lt_ui *ui) { ui_layout_pop(ui); }

/* ------------------------------------------------------------------ */
/* hit testing, interaction, focus                                     */
int ui_layer_ok(const lt_ui *ui)
{
    const ui_layer *L;
    if (ui->nlstack == 0) { return 0; }
    L = &ui->layers[ui->lstack[ui->nlstack - 1]];
    return L->id == ui->hover_layer && !L->blocked && !ui->disabled;
}

int ui_over(lt_ui *ui, lt_rect r)
{
    lt_rect c = lt_rect_intersect(r, ui_clip_cur(ui));
    return lt_rect_contains(c, ui->mx, ui->my) && ui_layer_ok(ui);
}

void ui_set_last(lt_ui *ui, lt_u32 id, lt_rect r)
{
    ui->last_id = id;
    ui->last_rect = r;
    ui->last_hover = (ui->hover == id || ui->active == id) ? 1 : 0;
}

void ui_focusable(lt_ui *ui, lt_u32 id, int kind)
{
    ui_layer *L = ui_layer_cur(ui);
    int shift = 0;
    if (L->blocked || ui->disabled) { return; }
    if (ui->first_focusable == 0) { ui->first_focusable = id; }
    if (ui->focus_req > 0 && (ui->focus_req_kind == 0 || ui->focus_req_kind == kind)) {
        ui->focus = id;
        ui->focus_req = 0;
        ui->kbd_nav = 1;
        ui->focus_kind = kind;
        ui_request(ui);
    }
    if (ui->focus == id) {
        ui->focus_seen = 1;
        ui->focus_kind = kind;
        if (ui_key_take(ui, LT_KEY_TAB, &shift) && !(shift & (LT_MOD_CTRL | LT_MOD_ALT))) {
            ui->kbd_nav = 1;
            if (shift & LT_MOD_SHIFT) {
                if (ui->prev_focusable != 0) { ui->focus = ui->prev_focusable; }
                else { ui->wrap_back = 1; }
            } else {
                ui->focus_req = 1;
                ui->focus_req_kind = 0;
            }
            ui_request(ui);
        }
    }
    ui->prev_focusable = id;
    ui->last_focusable = id;
}

int ui_interact(lt_ui *ui, lt_u32 id, lt_rect r, int flags)
{
    int res = 0;
    int over = ui_over(ui, r);
    if (flags & UI_FOCUSABLE) {
        int kind = (flags >> 8) & 0xFF;
        ui_focusable(ui, id, kind ? kind : 1);
    }
    if (ui->active == id) {
        if (over) { res |= UI_HOVER; }
        if (ui->mreleased & (1 << LT_MB_LEFT)) {
            res |= UI_RELEASE;
            if (over) { res |= UI_CLICK; }
            ui->active = 0;
        } else {
            res |= UI_ACTIVE;
        }
        return res;
    }
    if (ui->active != 0 || !over) { return res; }
    if (ui->hover == 0) { ui->hover = id; }
    if (ui->hover != id) { return res; }
    res |= UI_HOVER;
    if (ui_pressed(ui, LT_MB_LEFT)) {
        ui->active = id;
        ui->press_claimed = 1;
        res |= UI_PRESS | UI_ACTIVE;
        if (ui->dbl) { res |= UI_DBLCLICK; }
        if (flags & UI_FOCUSABLE) { ui->focus = id; ui->kbd_nav = 0; }
        if (ui->mreleased & (1 << LT_MB_LEFT)) {
            res |= UI_CLICK | UI_RELEASE;
            res &= ~UI_ACTIVE;
            ui->active = 0;
        }
    }
    if ((flags & UI_RIGHT) && ui_pressed(ui, LT_MB_RIGHT)) {
        res |= UI_RIGHT;
        ui->press_claimed = 1;
    }
    return res;
}

void ui_focus_ring(lt_ui *ui, lt_rect r, int radius)
{
    LT_UNUSED(radius);
    if (ui->cell) { return; }
    lt_ui_draw_box(ui, lt_rect_inset(r, -1, -1), ui->theme.focus, 0, radius + 1);
}

/* ------------------------------------------------------------------ */
/* frame                                                               */
static void sort_layers(lt_ui *ui, int *order)
{
    int i, j;
    for (i = 0; i < ui->nlayers; i++) { order[i] = i; }
    for (i = 1; i < ui->nlayers; i++) {
        int v = order[i];
        j = i - 1;
        while (j >= 0 && (ui->layers[order[j]].z > ui->layers[v].z ||
                          (ui->layers[order[j]].z == ui->layers[v].z &&
                           ui->layers[order[j]].seq > ui->layers[v].seq))) {
            order[j + 1] = order[j];
            j--;
        }
        order[j + 1] = v;
    }
}

static void replay_text(lt_ui *ui, lt_gfx *g, const ui_cmd *c)
{
    const char *s = ui->tbuf.data + c->off;
    lt_font *f = (g->cells != NULL || g->bitmap_text) ? NULL : lt_font_get((lt_font_id)c->i[1]);
    if (f == NULL) {
        lt_gfx_text(g, c->r.x, c->r.y, s, c->c, c->i[0]);
    } else {
        double px = (double)c->i[2];
        int base = c->r.y + (c->r.h + (int)floor(lt_font_cap_height(f, px) + 0.5)) / 2;
        lt_gfx_text_bl(g, f, px, (double)c->r.x, base, s, c->c);
    }
}

static void replay_layer(lt_ui *ui, ui_layer *L)
{
    lt_gfx *g = ui->g;
    size_t i;
    for (i = 0; i < L->cmds.len; i++) {
        const ui_cmd *c = &LT_VEC_AT(&L->cmds, ui_cmd, i);
        switch (c->type) {
        case UC_CLIP: lt_gfx_set_clip(g, c->r); break;
        case UC_FILL: lt_gfx_fill_rect(g, c->r, c->c); break;
        case UC_RECT: lt_gfx_rect(g, c->r, c->c); break;
        case UC_ROUND:
            if (c->i[0] > 0) { lt_gfx_fill_round_rect(g, c->r, c->i[0], c->c); }
            else { lt_gfx_fill_rect(g, c->r, c->c); }
            break;
        case UC_HLINE: lt_gfx_hline(g, c->i[0], c->i[1], c->i[2], c->c); break;
        case UC_VLINE: lt_gfx_vline(g, c->i[0], c->i[1], c->i[2], c->c); break;
        case UC_LINE: lt_gfx_line(g, c->d[0], c->d[1], c->d[2], c->d[3], c->d[4], c->c); break;
        case UC_TEXT: replay_text(ui, g, c); break;
        case UC_VTEXT:
            lt_gfx_vtext(g, c->d[0], c->d[1], ui->tbuf.data + c->off, c->d[2], c->d[3], c->i[0],
                         c->i[1], c->d[4], c->c);
            break;
        case UC_CFILL: lt_gfx_fill_circle(g, c->d[0], c->d[1], c->d[2], c->c); break;
        case UC_CIRCLE: lt_gfx_circle(g, c->d[0], c->d[1], c->d[2], c->d[3], c->c); break;
        case UC_ARC:
            lt_gfx_arc(g, c->d[0], c->d[1], c->d[2], c->d[3], c->d[4], c->d[5], c->c);
            break;
        case UC_POLY:
            lt_gfx_fill_poly(g, (const double *)ui->dpool.data + c->off, c->i[0], c->c);
            break;
        case UC_VGRAD: lt_gfx_fill_rect_vgrad(g, c->r, c->c, c->c2); break;
        case UC_ICON: {
            double sz = c->d[0];
            lt_gfx_icon(g, c->i[0], (double)c->r.x + ((double)c->r.w - sz) / 2.0,
                        (double)c->r.y + ((double)c->r.h - sz) / 2.0, sz, c->d[1], c->c);
            break;
        }
        case UC_BLIT:
            if (g->cells == NULL) {
                lt_gfx_blit(g, c->r.x, c->r.y, c->px, c->r.w, c->r.h, c->i[0]);
            }
            break;
        case UC_CUSTOM:
            if (c->fn != NULL) {
                c->fn(g, lt_rect_make(c->i[0], c->i[1], c->i[2], c->i[3]), c->user);
            }
            break;
        default: break;
        }
    }
}

void lt_ui_begin(lt_ui *ui, lt_gfx *g, double now_ms)
{
    int i, cell;
    ui->g = g;
    cell = lt_gfx_cell_mode(g) ? 1 : 0;
    g->ui_pct = cell ? 100 : ui->pct;
    if (cell != ui->cell || ui->frame_no == 0) {
        ui->cell = cell;
        ui_theme_metrics(&ui->theme, cell, ui->pct);
    }
    ui->sw = lt_gfx_width(g);
    ui->sh = lt_gfx_height(g);
    ui->frame_no++;
    ui->in_frame = 1;
    ui->prev_now = ui->now;
    ui->now = now_ms;
    ui->wake_in = -1.0;
    /* double click detection */
    ui->dbl = 0;
    if (ui->mpressed & (1 << LT_MB_LEFT)) {
        if (now_ms - ui->last_press_ms < UI_DBL_MS &&
            abs(ui->mpress_x - ui->last_press_x) < 6 && abs(ui->mpress_y - ui->last_press_y) < 6) {
            ui->dbl = 1;
            ui->last_press_ms = -1.0e9;
        } else {
            ui->last_press_ms = now_ms;
            ui->last_press_x = ui->mpress_x;
            ui->last_press_y = ui->mpress_y;
        }
    }
    /* hover layer from the previous frame */
    ui->hover_layer = 1;
    {
        int bestz = -1, bests = -1;
        ui->modal_z_top = 0;
        for (i = 0; i < ui->nprev; i++) {
            const ui_prev_layer *p = &ui->prev[i];
            if (p->kind == UL_TIP) { continue; }
            if (p->kind == UL_MODAL && p->z > ui->modal_z_top) { ui->modal_z_top = p->z; }
            if (lt_rect_contains(p->rect, ui->mx, ui->my) &&
                (p->z > bestz || (p->z == bestz && p->seq > bests))) {
                bestz = p->z; bests = p->seq; ui->hover_layer = p->id;
            }
        }
        ui->popup_dismiss = 0;
        if (ui->npopups > 0 && (ui->mpressed & (1 << LT_MB_LEFT | 1 << LT_MB_RIGHT))) {
            int on_popup = 0;
            for (i = 0; i < ui->nprev; i++) {
                if (ui->prev[i].id == ui->hover_layer && ui->prev[i].kind == UL_POPUP) { on_popup = 1; }
            }
            if (!on_popup) { ui->popup_dismiss = 1; }
        }
    }
    if (ui->active != 0 && !(ui->mbtn & (1 << LT_MB_LEFT)) &&
        !(ui->mreleased & (1 << LT_MB_LEFT))) {
        ui->active = 0;
    }
    ui->hover = 0;
    ui->press_claimed = 0;
    ui->tip_want = 0;
    ui->first_focusable = ui->last_focusable = ui->prev_focusable = 0;
    ui->focus_seen = 0;
    ui->nlayers = 0;
    ui->nlstack = 0;
    ui->nclip = 0;
    ui->nlay = 0;
    ui->nscr = 0;
    ui->nid = 0;
    ui->disabled = 0;
    ui->menu_depth = 0;
    ui->menu_close_req = 0;
    ui->menu_activate = 0;
    ui->in_bar = 0;
    ui->used_top = ui->bottom_used = 0;
    ui->modal_seq = 0;
    ui->popup_seq = 0;
    ui->modal_depth = 0;
    ui->tree_depth = 0;
    if (ui->tree_cmd_left > 0) { ui->tree_cmd_left--; ui_request(ui); if (ui->tree_cmd_left == 0) { ui->tree_cmd = 0; } }
    ui->tab_id = 0;
    lt_buf_clear(&ui->tbuf);
    lt_vec_clear(&ui->dpool);
    if (ui->tab_count > 2500) { tab_grow(ui, ui->tab_cap, (lt_u32)(ui->frame_no > 3000 ? ui->frame_no - 3000 : 0)); }
    for (i = 0; i < UI_MAX_POPUPS; i++) { ui->menu_idx[i] = 0; }
    if (ui->edit.id != 0 && ui->focus != ui->edit.id) { ui_edit_end(ui); }
    ui->clip[ui->nclip++] = lt_rect_make(0, 0, ui->sw, ui->sh);
    ui_layer_begin(ui, 1, UL_MAIN, 0, lt_rect_make(0, 0, ui->sw, ui->sh));
    ui_layout_push(ui, lt_rect_make(0, 0, ui->sw, ui->sh));
}

static void draw_tooltip(lt_ui *ui)
{
    char line[200];
    const char *p = ui->tip_text;
    int lines = 0, maxw = 0, x, y, w, h, i = 0;
    lt_rect r;
    while (*p) {
        const char *e = strchr(p, '\n');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        int tw;
        if (n >= sizeof(line)) { n = sizeof(line) - 1; }
        memcpy(line, p, n); line[n] = '\0';
        tw = lt_ui_text_width(ui, line);
        if (tw > maxw) { maxw = tw; }
        lines++;
        if (!e) { break; }
        p = e + 1;
    }
    w = maxw + 2 * ui->theme.pad_x; h = lines * ui_line_h(ui) + (ui->cell ? 0 : lt_ui_px(ui, 2) * ui->theme.padding);
    x = ui->mx + (ui->cell ? 8 : lt_ui_px(ui, 12));
    y = ui->my + (ui->cell ? 16 : lt_ui_px(ui, 20));
    if (x + w > ui->sw) { x = ui->sw - w; }
    if (y + h > ui->sh) { y = ui->my - h - 4; }
    if (x < 0) { x = 0; }
    if (y < 0) { y = 0; }
    r = lt_rect_make(x, y, w, h);
    ui_layer_begin(ui, 0x7001, UL_TIP, 5000, r);
    lt_ui_draw_box(ui, lt_rect_make(r.x + 2, r.y + 3, r.w, r.h), LT_RGBA(0, 0, 0, 70), 0,
                   ui->theme.radius);
    lt_ui_draw_box(ui, r, ui->theme.elevated, ui->theme.border_strong, ui->theme.radius);
    p = ui->tip_text;
    while (*p) {
        const char *e = strchr(p, '\n');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        if (n >= sizeof(line)) { n = sizeof(line) - 1; }
        memcpy(line, p, n); line[n] = '\0';
        lt_ui_draw_text(ui, r.x + ui->theme.pad_x, r.y + (ui->cell ? 0 : ui->theme.padding) + i * ui_line_h(ui),
                        line, ui->theme.text);
        i++;
        if (!e) { break; }
        p = e + 1;
    }
    ui_layer_end(ui);
}

static void draw_toasts(lt_ui *ui)
{
    int i, y, keep = 0;
    ui_toast tmp[UI_MAX_TOASTS];
    for (i = 0; i < ui->ntoasts; i++) {
        if (ui->now - ui->msg_q[i].t0 < UI_TOAST_MS) { tmp[keep++] = ui->msg_q[i]; }
    }
    for (i = 0; i < keep; i++) { ui->msg_q[i] = tmp[i]; }
    ui->ntoasts = keep;
    if (keep == 0) { return; }
    ui_layer_begin(ui, 0x7002, UL_TIP, 5001, lt_rect_make(0, 0, 0, 0));
    y = ui->sh - ui->theme.statusbar_h - ui->theme.padding;
    if (ui->cell) { y = ui->sh - 16; }
    for (i = keep - 1; i >= 0; i--) {
        const ui_toast *t = &ui->msg_q[i];
        lt_color bg, fg;
        int tw = lt_ui_text_width(ui, t->text), w, h = ui->theme.row_h + (ui->cell ? 0 : lt_ui_px(ui, 8));
        double age = ui->now - t->t0;
        lt_rect r;
        switch (t->kind) {
        case 1: bg = ui->theme.success_bg; break;
        case 2: bg = ui->theme.warning_bg; break;
        case 3: bg = ui->theme.error_bg; break;
        default: bg = ui->theme.info_bg; break;
        }
        fg = lt_color_contrast(bg);
        w = tw + 3 * ui->theme.pad_x + (ui->cell ? 8 : lt_ui_px(ui, 16));
        y -= h;
        r = lt_rect_make(ui->sw - w - ui->theme.padding, y, w, h);
        if (!ui->cell) {
            lt_ui_draw_box(ui, lt_rect_make(r.x + 2, r.y + 3, r.w, r.h), LT_RGBA(0, 0, 0, 80), 0, 4);
        }
        lt_ui_draw_box(ui, r, bg, lt_color_mix(bg, fg, 60), ui->theme.radius + 1);
        lt_ui_draw_icon(ui, lt_rect_make(r.x + ui->theme.pad_x / 2, r.y, ui->cell ? 8 : lt_ui_px(ui, 20), h),
                        t->kind == 3 || t->kind == 2 ? LT_UI_ICON_WARN :
                        (t->kind == 1 ? LT_UI_ICON_CHECK : LT_UI_ICON_INFO), fg);
        ui_text_in(ui, lt_rect_make(r.x + ui->theme.pad_x + (ui->cell ? 8 : lt_ui_px(ui, 18)), r.y, r.w, r.h),
                   t->text, fg, 0);
        y -= ui->cell ? 0 : lt_ui_px(ui, 6);
        if (age > UI_TOAST_MS - 400) { ui_animate(ui, 50); } else { ui_animate(ui, (int)(UI_TOAST_MS - 400 - age)); }
    }
    ui_layer_end(ui);
}

void lt_ui_toast(lt_ui *ui, int kind, const char *text)
{
    ui_toast *t;
    if (ui->ntoasts >= UI_MAX_TOASTS) {
        memmove(&ui->msg_q[0], &ui->msg_q[1], sizeof(ui_toast) * (UI_MAX_TOASTS - 1));
        ui->ntoasts = UI_MAX_TOASTS - 1;
    }
    t = &ui->msg_q[ui->ntoasts++];
    t->kind = kind;
    t->t0 = ui->now;
    lt_strlcpy(t->text, text, sizeof(t->text));
    ui_request(ui);
}

void lt_ui_tooltip(lt_ui *ui, const char *text)
{
    if (ui->last_id == 0 || !ui->last_hover || ui->disabled) { return; }
    if (ui->active != 0 && ui->active != ui->last_id) { return; }
    ui->tip_want = 1;
    ui->tip_id = ui->last_id;
    ui->tip_rect = ui->last_rect;
    lt_strlcpy(ui->tip_text, text, sizeof(ui->tip_text));
}

void lt_ui_end(lt_ui *ui)
{
    int order[UI_MAX_LAYERS];
    int i;
    lt_u32 cur_tip;
    if (!ui->in_frame) { return; }
    while (ui->nlstack > 1) { ui_layer_end(ui); }
    /* keyboard focus fallbacks */
    {
        int mods = 0;
        if (ui->focus_req > 0 && ui->first_focusable != 0) {
            ui->focus = ui->first_focusable;
            ui->focus_req = 0;
            ui->kbd_nav = 1;
            ui->focus_seen = 1;
        } else if (ui->wrap_back && ui->last_focusable != 0) {
            ui->focus = ui->last_focusable;
            ui->kbd_nav = 1;
            ui->focus_seen = 1;
        } else if (ui->focus == 0 && ui_key_take(ui, LT_KEY_TAB, &mods) &&
                   !(mods & (LT_MOD_CTRL | LT_MOD_ALT))) {
            ui->focus = (mods & LT_MOD_SHIFT) ? ui->last_focusable : ui->first_focusable;
            ui->kbd_nav = 1;
            ui->focus_seen = 1;
        }
        ui->focus_req = 0;
        ui->wrap_back = 0;
    }
    if (ui->focus != 0 && !ui->focus_seen) {
        /* a focused widget that vanished for two frames loses the focus */
        if (++ui->focus_miss >= 2) { ui->focus = 0; ui->focus_miss = 0; }
    } else {
        ui->focus_miss = 0;
    }
    if (ui->edit.id != 0 && !ui->focus_seen) { ui_edit_end(ui); }
    /* clicking on nothing clears the focus */
    if ((ui->mpressed & (1 << LT_MB_LEFT)) && !ui->press_claimed) {
        ui->focus = 0;
        ui->kbd_nav = 0;
    }
    /* popups: dismissal by outside click or Esc */
    if (ui->popup_dismiss && !ui->press_claimed) {
        ui->npopups = 0;
        ui_request(ui);
    }
    if (ui->npopups > 0 && ui_key_take(ui, LT_KEY_ESCAPE, NULL)) {
        ui->npopups--;
        ui_request(ui);
    }
    if (ui->menu_close_req) { ui->npopups = 0; ui_request(ui); }
    /* tooltip */
    cur_tip = ui->tip_want ? ui->tip_id : 0;
    if (cur_tip != ui->tip_id_prev || ui->mbtn != 0 || ui->npopups > 0) {
        ui->tip_since = ui->now;
        if (cur_tip != ui->tip_id_prev) { ui_request(ui); }
    }
    ui->tip_id_prev = cur_tip;
    if (cur_tip != 0 && ui->npopups == 0 && ui->mbtn == 0) {
        double el = ui->now - ui->tip_since;
        if (el >= UI_TIP_DELAY_MS) { draw_tooltip(ui); }
        else { ui_animate(ui, (int)(UI_TIP_DELAY_MS - el) + 1); }
    }
    draw_toasts(ui);
    ui_layer_end(ui);
    /* replay */
    lt_gfx_set_clip(ui->g, lt_rect_make(0, 0, ui->sw, ui->sh));
    lt_gfx_clear(ui->g, ui->theme.bg_app);
    sort_layers(ui, order);
    for (i = 0; i < ui->nlayers; i++) { replay_layer(ui, &ui->layers[order[i]]); }
    lt_gfx_set_clip(ui->g, lt_rect_make(0, 0, ui->sw, ui->sh));
    /* remember layer rects for next frame's hit testing */
    ui->nprev = 0;
    for (i = 0; i < ui->nlayers; i++) {
        ui_layer *L = &ui->layers[i];
        ui_prev_layer *p;
        if (ui->nprev >= UI_MAX_LAYERS) { break; }
        p = &ui->prev[ui->nprev++];
        p->id = L->id; p->kind = L->kind; p->z = L->z; p->seq = L->seq; p->rect = L->rect;
    }
    /* clear per-frame input latches */
    ui->lmx = ui->mx; ui->lmy = ui->my;
    ui->mpressed = ui->mreleased = 0;
    ui->wheel = 0;
    ui->nq = 0;
    ui->dbl = 0;
    ui->dirty = 0;
    if (ui->extra > 0) { ui->extra--; }
    ui->in_frame = 0;
}

void lt_ui_layout_row_px(lt_ui *ui, int n, const int *widths, int height)
{
    ui_layout_row_dev(ui, n, widths, height);
}

void lt_ui_layout_gap_px(lt_ui *ui, int x, int y)
{
    ui_layout *L = ui_lay(ui);
    L->sp_x = x;
    L->sp_y = y;
}

void lt_ui_layout_space_px(lt_ui *ui, int pixels) { ui_layout_space_dev(ui, pixels); }
