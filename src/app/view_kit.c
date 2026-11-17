/* view_kit.c - see view_kit.h.
 * Drawing is recorded into the toolkit's draw list through lt_ui_draw_custom;
 * the callbacks receive a pointer into a small ring of parameter blocks that
 * stays valid until the end of the frame (a frame never records more than
 * VK_RING blocks of one kind).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <string.h>
#include <math.h>
#include "view_kit.h"

#define VK_RING_T 192
#define VK_RING_I 384
#define VK_TEXT_MAX 96

typedef struct vk_text {
    int font, align, y, mode;     /* mode 0: centred in rect, 1: baseline at y */
    double px;
    lt_color c;
    char s[VK_TEXT_MAX];
} vk_text;

typedef struct vk_icon {
    int icon, size;
    double line_w;
    lt_color c;
} vk_icon;

static vk_text vk_texts[VK_RING_T];
static vk_icon vk_icons[VK_RING_I];
static int vk_nt, vk_ni;

typedef char vk_check_text[(sizeof(vk_texts) <= 32000UL) ? 1 : -1];
typedef char vk_check_icon[(sizeof(vk_icons) <= 32000UL) ? 1 : -1];

static lt_font *vk_font(int id)
{
    if (id < 0 || id >= (int)LT_FONT_COUNT) { id = (int)LT_FONT_SANS; }
    return lt_font_get((lt_font_id)id);
}

int lt_vk_text_w(lt_ui *ui, int font, double px, const char *s)
{
    if (s == NULL) { return 0; }
    if (lt_ui_cell_mode(ui)) { return lt_ui_text_width(ui, s); }
    return (int)(lt_gfx_text_measure(vk_font(font), px, s) + 0.999);
}

/* copy s, eliding with "..." so that it fits max_w pixels */
static void fit_copy(lt_font *f, double px, const char *s, double max_w, char *out, size_t cap)
{
    size_t n;
    lt_strlcpy(out, s, cap);
    if (max_w <= 0.0 || lt_gfx_text_measure(f, px, out) <= max_w) { return; }
    n = lt_gfx_text_fit(f, px, out, max_w - lt_gfx_text_measure(f, px, "..."));
    if (n + 4 > cap) { n = cap > 4 ? cap - 4 : 0; }
    while (n > 0 && ((unsigned char)out[n] & 0xC0) == 0x80) { n--; }
    memcpy(out + n, "...", 4);
}

static void draw_text_cb(lt_gfx *g, lt_rect r, void *user)
{
    const vk_text *t = (const vk_text *)user;
    lt_font *f = vk_font(t->font);
    char buf[VK_TEXT_MAX + 4];
    double w;
    int x, y;
    if (lt_gfx_cell_mode(g) || f == NULL) {
        lt_gfx_text(g, r.x, r.y + (r.h - 16) / 2, t->s, t->c, 1);
        return;
    }
    fit_copy(f, t->px, t->s, (double)r.w, buf, sizeof(buf));
    w = lt_gfx_text_measure(f, t->px, buf);
    x = r.x;
    if (t->align == 1) { x = r.x + (int)((double)r.w - w) / 2; }
    else if (t->align == 2) { x = r.x + r.w - (int)(w + 0.5); }
    if (t->mode == 1) {
        lt_gfx_text_bl(g, f, t->px, (double)x, t->y, buf, t->c);
        return;
    }
    /* centre the cap/x-height band vertically in r */
    y = r.y + (r.h - lt_font_line_height(f, t->px)) / 2;
    lt_gfx_text_ex(g, f, t->px, x, y, buf, t->c);
}

void lt_vk_text(lt_ui *ui, lt_rect r, int font, double px, const char *s, lt_color c, int align)
{
    vk_text *t;
    lt_rect vis;
    if (s == NULL || s[0] == '\0' || r.w <= 0 || r.h <= 0) { return; }
    if (lt_ui_cell_mode(ui)) { lt_ui_draw_text_in(ui, r, s, c, align); return; }
    vis = lt_rect_intersect(r, lt_ui_clip_get(ui));
    if (vis.w <= 0 || vis.h <= 0) { return; }       /* scrolled out of view: keep the ring short */
    t = &vk_texts[vk_nt++ % VK_RING_T];
    t->font = font; t->px = px; t->c = c; t->align = align; t->mode = 0; t->y = 0;
    lt_strlcpy(t->s, s, sizeof(t->s));
    lt_ui_draw_custom(ui, r, draw_text_cb, t);
}

void lt_vk_text_at(lt_ui *ui, int x, int y, int font, double px, const char *s, lt_color c)
{
    vk_text *t;
    lt_rect r;
    if (s == NULL || s[0] == '\0') { return; }
    if (lt_ui_cell_mode(ui)) { lt_ui_draw_text(ui, x, y - 12, s, c); return; }
    {
        lt_rect cl = lt_ui_clip_get(ui);
        if (y < cl.y - 40 || y > cl.y + cl.h + 40 || x > cl.x + cl.w || x < cl.x - 400) { return; }
    }
    t = &vk_texts[vk_nt++ % VK_RING_T];
    t->font = font; t->px = px; t->c = c; t->align = 0; t->mode = 1; t->y = y;
    lt_strlcpy(t->s, s, sizeof(t->s));
    r = lt_rect_make(x, y - (int)(px * 1.2) - 2, lt_vk_text_w(ui, font, px, s) + 4, (int)(px * 1.6) + 4);
    lt_ui_draw_custom(ui, r, draw_text_cb, t);
}

static void draw_icon_cb(lt_gfx *g, lt_rect r, void *user)
{
    const vk_icon *ic = (const vk_icon *)user;
    if (lt_gfx_cell_mode(g)) { return; }
    lt_gfx_icon(g, ic->icon, r.x + (r.w - ic->size) / 2, r.y + (r.h - ic->size) / 2, ic->size,
                ic->line_w, ic->c);
}

void lt_vk_icon(lt_ui *ui, lt_rect r, int icon, int size_px, double line_w, lt_color c)
{
    vk_icon *ic;
    lt_rect vis;
    if (icon == 0 || lt_ui_cell_mode(ui) || r.w <= 0 || r.h <= 0) { return; }
    vis = lt_rect_intersect(r, lt_ui_clip_get(ui));
    if (vis.w <= 0 || vis.h <= 0) { return; }
    ic = &vk_icons[vk_ni++ % VK_RING_I];
    ic->icon = icon; ic->size = size_px; ic->line_w = line_w; ic->c = c;
    lt_ui_draw_custom(ui, r, draw_icon_cb, ic);
}

/* ---- shapes ---- */
void lt_vk_card(lt_ui *ui, lt_rect r, lt_color fill, lt_color border, int radius)
{
    if (lt_ui_cell_mode(ui)) { lt_ui_draw_box(ui, r, fill, border, 0); return; }
    /* soft shadow: two translucent offset outlines under the card */
    lt_ui_draw_round(ui, lt_rect_make(r.x - 1, r.y + 1, r.w + 2, r.h + 2), radius + 1, LT_RGBA(0, 0, 0, 40));
    lt_ui_draw_round(ui, lt_rect_make(r.x, r.y + 2, r.w, r.h), radius, LT_RGBA(0, 0, 0, 30));
    lt_ui_draw_box(ui, r, fill, border, radius);
}

void lt_vk_pill(lt_ui *ui, lt_rect r, lt_color fill, lt_color border)
{
    lt_ui_draw_box(ui, r, fill, border, lt_ui_cell_mode(ui) ? 0 : r.h / 2);
}

void lt_vk_caption(lt_ui *ui, lt_rect r, const char *s, lt_color c)
{
    char up[80];
    size_t i, n = 0;
    for (i = 0; s != NULL && s[i] != '\0' && n + 1 < sizeof(up); i++) {
        char ch = s[i];
        up[n++] = (ch >= 'a' && ch <= 'z') ? (char)(ch - 'a' + 'A') : ch;
    }
    up[n] = '\0';
    lt_vk_text(ui, r, (int)LT_FONT_SANS_SEMIBOLD, 11.0, up, c, 0);
}

void lt_vk_bar(lt_ui *ui, lt_rect r, double frac, lt_color fill, lt_color track)
{
    int fw;
    int rad = lt_ui_cell_mode(ui) ? 0 : r.h / 2;
    if (frac < 0.0 || !(frac == frac)) { frac = 0.0; }
    if (frac > 1.0) { frac = 1.0; }
    lt_ui_draw_round(ui, r, rad, track);
    fw = (int)((double)r.w * frac + 0.5);
    if (fw > 0) {
        if (fw < r.h) { fw = r.h < r.w ? r.h : r.w; }
        lt_ui_draw_round(ui, lt_rect_make(r.x, r.y, fw, r.h), rad, fill);
    }
}

void lt_vk_ring(lt_ui *ui, double cx, double cy, double radius, double w, double frac,
                lt_color color, lt_color track)
{
    const double pi = 3.14159265358979323846;
    if (frac < 0.0 || !(frac == frac)) { frac = 0.0; }
    if (frac > 1.0) { frac = 1.0; }
    lt_ui_draw_circle(ui, cx, cy, radius, w, track);
    if (frac > 0.0) {
        lt_ui_draw_arc(ui, cx, cy, radius, -pi / 2.0, -pi / 2.0 + 2.0 * pi * frac, w, color);
    }
}

lt_color lt_vk_score_color(int score)
{
    if (score >= 80) { return LT_RGB(0x4C, 0xAF, 0x50); }
    if (score >= 60) { return LT_RGB(0xFF, 0xC1, 0x07); }
    if (score >= 40) { return LT_RGB(0xFF, 0x98, 0x00); }
    return LT_RGB(0xF4, 0x43, 0x36);
}

/* ---- toolbar ---- */
#define VK_PAD 6
#define VK_MAX_ITEMS 40

static int item_label_w(lt_ui *ui, const lt_vk_item *it)
{
    return (it->label != NULL && it->label[0] != '\0')
               ? lt_vk_text_w(ui, (int)LT_FONT_SANS_MEDIUM, 12.0, it->label) : 0;
}

static int item_key_w(lt_ui *ui, const lt_vk_item *it)
{
    return (it->key != NULL && it->key[0] != '\0')
               ? lt_vk_text_w(ui, (int)LT_FONT_MONO, 10.0, it->key) + 6 : 0;
}

/* width of the content group: icon, label, key chip */
static int item_content_w(lt_ui *ui, const lt_vk_item *it)
{
    int w = 0, lw = item_label_w(ui, it), kw = item_key_w(ui, it);
    if (it->icon != 0) { w += 14; }
    if (lw > 0) { w += (w > 0 ? 5 : 0) + lw; }
    if (kw > 0) { w += (w > 0 ? 4 : 0) + kw; }
    return w;
}

static int item_w(lt_ui *ui, const lt_vk_item *it)
{
    int w = item_content_w(ui, it) + VK_PAD * 2;
    return w < 28 ? 28 : w;
}

static int item_flags(const lt_vk_item *it)
{
    int fl = 0;
    if (it->on) { fl |= LT_UI_BTN_SELECTED; }
    if (it->disabled) { fl |= LT_UI_BTN_DISABLED; }
    if (it->accent) { fl |= LT_UI_BTN_ACCENT; }
    if (it->fill != 0) { fl = (fl & ~(LT_UI_BTN_SELECTED | LT_UI_BTN_ACCENT)) | LT_UI_BTN_FLAT; }
    return fl;
}

static void item_face(lt_ui *ui, const lt_vk_item *it, lt_rect r)
{
    const lt_theme *t = lt_ui_theme(ui);
    lt_color fg = it->disabled ? t->disabled : (it->on ? t->primary_text :
                  (it->accent ? t->accent_text : t->text));
    int cw = item_content_w(ui, it), x = r.x + (r.w - cw) / 2;
    int lw = item_label_w(ui, it), kw = item_key_w(ui, it);
    if (it->fill != 0 && !it->disabled) {
        lt_color f = lt_ui_last_hovered(ui) ? lt_color_mix(it->fill, LT_RGB(255, 255, 255), 40) : it->fill;
        lt_ui_draw_box(ui, r, f, f, t->radius);
        fg = lt_color_contrast(it->fill);
    }
    if (it->icon != 0) {
        lt_vk_icon(ui, lt_rect_make(x, r.y, 14, r.h), it->icon, 14, 2.0, fg);
        x += 14 + 5;
    }
    if (lw > 0) {
        lt_vk_text(ui, lt_rect_make(x, r.y, lw + 2, r.h), (int)LT_FONT_SANS_MEDIUM, 12.0, it->label, fg, 0);
        x += lw + 4;
    }
    if (kw > 0) {
        lt_rect kr = lt_rect_make(x, r.y + (r.h - 16) / 2, kw, 16);
        lt_ui_draw_round(ui, kr, 2, it->on ? LT_RGBA(255, 255, 255, 60) : LT_RGBA(255, 255, 255, 26));
        lt_vk_text(ui, kr, (int)LT_FONT_MONO, 10.0, it->key, it->on ? fg : t->text2, 1);
    }
}

int lt_vk_toolbar(lt_ui *ui, const lt_vk_item *items, int h)
{
    int one = -1;
    lt_ui_layout_row(ui, 1, &one, h);
    return lt_vk_toolbar_in(ui, items, lt_ui_layout_next(ui));
}

int lt_vk_toolbar_in(lt_ui *ui, const lt_vk_item *items, lt_rect row)
{
    const lt_theme *t = lt_ui_theme(ui);
    int cell = lt_ui_cell_mode(ui), n = 0, i, x, clicked = -1, spare = 0, nspacer = 0;
    int wid[VK_MAX_ITEMS], total = 0, gap = cell ? 8 : 2;
    int h = row.h;
    while (n < VK_MAX_ITEMS && items[n].kind != LT_VK_END) { n++; }
    for (i = 0; i < n; i++) {
        if (items[i].kind == LT_VK_BTN) {
            const lt_vk_item *it = &items[i];
            const char *txt = (it->label != NULL && it->label[0] != '\0') ? it->label :
                              (it->key != NULL && it->key[0] != '\0' ? it->key : it->tip);
            wid[i] = cell ? lt_ui_text_width(ui, txt != NULL ? txt : "?") + 8 : item_w(ui, it);
        } else if (items[i].kind == LT_VK_SEP) { wid[i] = cell ? 8 : 13; }
        else { wid[i] = 0; nspacer++; }
        total += wid[i] + gap;
    }
    spare = row.w - total;
    if (spare < 0) { spare = 0; }
    x = row.x;
    for (i = 0; i < n; i++) {
        lt_rect r;
        if (items[i].kind == LT_VK_SPACER) { x += nspacer > 0 ? spare / nspacer : 0; continue; }
        r = lt_rect_make(x, row.y + (cell ? 0 : (row.h - (h > 28 ? 28 : h)) / 2), wid[i],
                         cell ? row.h : (h > 28 ? 28 : h));
        if (items[i].kind == LT_VK_SEP) {
            if (!cell) { lt_ui_draw_vline(ui, r.x + r.w / 2, r.y + 4, r.y + r.h - 5, t->border); }
        } else {
            const lt_vk_item *it = &items[i];
            const char *txt = (it->label != NULL && it->label[0] != '\0') ? it->label :
                              (it->key != NULL && it->key[0] != '\0' ? it->key : it->tip);
            lt_ui_push_id_int(ui, i);
            lt_ui_layout_set_rect(ui, r);
            if (cell) {
                if (lt_ui_button_ex(ui, txt != NULL ? txt : "?", item_flags(it) | LT_UI_BTN_FLAT)) { clicked = i; }
            } else {
                if (lt_ui_button_ex(ui, "##vk", item_flags(it))) { clicked = i; }
                item_face(ui, it, r);
            }
            if (it->tip != NULL) { lt_ui_tooltip(ui, it->tip); }
            lt_ui_pop_id(ui);
        }
        x += wid[i] + gap;
    }
    return clicked;
}

lt_bool lt_vk_icon_btn(lt_ui *ui, int icon, const char *tip, int on, int w, int h)
{
    lt_vk_item it;
    lt_rect r;
    lt_bool hit;
    int ww = w > 0 ? w : 28;
    memset(&it, 0, sizeof(it));
    it.kind = LT_VK_BTN; it.icon = icon; it.on = on; it.tip = tip;
    r = lt_ui_layout_next(ui);
    r.w = ww < r.w ? ww : r.w;
    if (h > 0 && h < r.h) { r.y += (r.h - h) / 2; r.h = h; }
    lt_ui_layout_set_rect(ui, r);
    if (lt_ui_cell_mode(ui)) {
        hit = lt_ui_button_ex(ui, tip != NULL ? tip : "?", on ? LT_UI_BTN_SELECTED : 0);
    } else {
        hit = lt_ui_button_ex(ui, "##vkib", (on ? LT_UI_BTN_SELECTED : 0));
        item_face(ui, &it, r);
    }
    if (tip != NULL) { lt_ui_tooltip(ui, tip); }
    return hit;
}
