/* ui_widgets.c - basic widgets: labels, buttons, checkbox, radio, toggle,
 * slider, progress, spinner, swatch, image, link, separator, bars.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "ui_priv.h"

#define PAD(ui) ((ui)->theme.pad_x)

static int key_activate(lt_ui *ui, lt_u32 id)
{
    if (ui->focus != id || ui->disabled) { return 0; }
    if (ui_key_take(ui, LT_KEY_ENTER, NULL)) { return 1; }
    if (ui_key_take(ui, ' ', NULL)) { return 1; }
    return 0;
}

/* ------------------------------------------------------------------ */
/* labels                                                              */
void lt_ui_label_ex(lt_ui *ui, const char *text, lt_color color, int align)
{
    char buf[256];
    const char *vis = ui_label_visible(text, buf, sizeof(buf));
    lt_rect r = ui_next(ui, lt_ui_text_width(ui, vis));
    ui_hover_only(ui, lt_ui_id(ui, text), r);
    ui_text_in(ui, r, vis, ui->disabled ? ui->theme.disabled : color, align);
}

void lt_ui_label(lt_ui *ui, const char *text)
{
    lt_ui_label_ex(ui, text, ui->theme.text, LT_UI_ALIGN_LEFT);
}

void lt_ui_labelf(lt_ui *ui, const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    lt_vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    lt_ui_label(ui, buf);
}

void lt_ui_heading(lt_ui *ui, const char *text)
{
    lt_rect r = ui_next_full(ui, ui->theme.row_h);
    ui_text_in_f(ui, r, text, ui->theme.text, 0, (int)LT_FONT_SANS_SEMIBOLD, LT_UI_FS_MD);
    if (!ui->cell) {
        lt_ui_draw_hline(ui, r.x, r.x + r.w - 1, r.y + r.h - 1, ui->theme.border_subtle);
    }
}

int ui_wrap_lines(const lt_ui *ui, const char *text, int maxw, const char **starts, int *lens, int cap)
{
    const char *p = text;
    int n = 0;
    while (*p && n < cap) {
        const char *e = p;
        const char *brk = NULL;
        while (*e && *e != '\n') {
            const char *nx = e;
            lt_utf8_next(&nx);
            if (ui_prefix_w(ui, p, (int)(nx - p)) > maxw && e > p) { break; }
            if (*e == ' ') { brk = e; }
            e = nx;
        }
        if (*e && *e != '\n' && *e != ' ' && brk != NULL) { e = brk; }
        starts[n] = p;
        lens[n] = (int)(e - p);
        n++;
        p = e;
        if (*p == '\n' || *p == ' ') { p++; }
    }
    if (n == 0) { starts[0] = text; lens[0] = 0; n = 1; }
    return n;
}

void lt_ui_label_wrap(lt_ui *ui, const char *text, lt_color color)
{
    ui_layout *L = ui_lay(ui);
    int lh = ui_line_h(ui);
    int nlines, i;
    lt_rect r;
    const char *starts[64];
    int lens[64];
    nlines = ui_wrap_lines(ui, text, LT_MAX(32, L->body.w - L->indent), starts, lens, 64);
    r = ui_next_full(ui, nlines * lh + (ui->cell ? 0 : lt_ui_px(ui, 4)));
    for (i = 0; i < nlines; i++) {
        char line[256];
        int n = lens[i] < 255 ? lens[i] : 255;
        memcpy(line, starts[i], (size_t)n);
        line[n] = '\0';
        lt_ui_draw_text(ui, r.x, r.y + i * lh + (lh - ui_text_box_h(ui)) / 2, line, color);
    }
}

lt_bool lt_ui_link(lt_ui *ui, const char *text)
{
    char buf[256];
    const char *vis = ui_label_visible(text, buf, sizeof(buf));
    lt_u32 id = lt_ui_id(ui, text);
    lt_rect r = ui_next(ui, lt_ui_text_width(ui, vis));
    int res = ui_interact(ui, id, r, UI_FOCUSABLE | UI_KIND(UI_K_BUTTON));
    int tw = lt_ui_text_width(ui, vis);
    lt_color c = (res & UI_HOVER) ? ui->theme.primary_hover : ui->theme.link;
    ui_text_in(ui, r, vis, c, 0);
    if (res & UI_HOVER) {
        lt_ui_draw_hline(ui, r.x, r.x + tw - 1, r.y + (r.h + 16) / 2 - (ui->cell ? 0 : 1), c);
    }
    if (ui->focus == id && ui->kbd_nav && !ui->cell) {
        lt_ui_draw_rect(ui, lt_rect_inset(r, -1, 0), ui->theme.focus);
    }
    ui_set_last(ui, id, r);
    return ((res & UI_CLICK) || key_activate(ui, id)) ? LT_TRUE : LT_FALSE;
}

void lt_ui_separator(lt_ui *ui)
{
    lt_rect r = ui_next_full(ui, ui->cell ? 16 : lt_ui_px(ui, 9));
    int y = r.y + r.h / 2;
    if (ui->cell) {
        lt_ui_draw_hline(ui, r.x, r.x + r.w - 1, y, ui->theme.border_subtle);
    } else {
        lt_ui_draw_hline(ui, r.x, r.x + r.w - 1, y, ui->theme.border_subtle);
    }
}

/* ------------------------------------------------------------------ */
/* buttons                                                             */
static lt_bool button_impl(lt_ui *ui, const char *label, int icon, int flags)
{
    char buf[256];
    const char *vis = ui_label_visible(label, buf, sizeof(buf));
    lt_u32 id = lt_ui_id(ui, label);
    int fid = (int)LT_FONT_SANS_MEDIUM;
    int tw = ui_text_w(ui, vis, fid, 0);
    int pref = icon ? lt_ui_px(ui, 26) : tw + (ui->cell ? 20 : lt_ui_px(ui, 24));
    lt_rect r;
    int res, clicked = 0, dis = (flags & LT_UI_BTN_DISABLED) ? 1 : 0, focused;
    lt_color fill, border, fg;
    const lt_theme *t = &ui->theme;
    r = ui_next(ui, pref);
    if (dis) { ui->disabled++; }
    res = ui_interact(ui, id, r, (flags & LT_UI_BTN_NOTAB) ? 0 : (UI_FOCUSABLE | UI_KIND(UI_K_BUTTON)));
    focused = (ui->focus == id && !dis);
    if (res & UI_CLICK) { clicked = 1; }
    if (key_activate(ui, id)) { clicked = 1; }
    if ((flags & LT_UI_BTN_DEFAULT) && !dis && !ui_layer_cur(ui)->blocked &&
        (ui->focus == 0 || ui->focus == id || ui->focus_kind != UI_K_BUTTON)) {
        if (ui_key_take(ui, LT_KEY_ENTER, NULL)) { clicked = 1; }
    }
    /* colors */
    if (icon && (flags & LT_UI_BTN_FLAT) && !ui->cell) {
        /* toolbar button: text2 glyph, hover/active wash, selected = link-blue glyph on active
         * (link is the theme's accent colour made to read on surfaces; primary is a fill) */
        fill = (res & UI_ACTIVE) ? t->active : ((res & UI_HOVER) ? t->hover : 0);
        border = 0;
        fg = (flags & LT_UI_BTN_SELECTED) ? t->link : ((res & UI_HOVER) ? t->text : t->text2);
        if (flags & LT_UI_BTN_SELECTED) { fill = t->active; }
        if (flags & LT_UI_BTN_DANGER) {
            fg = t->error;
            fill = (res & UI_HOVER) ? LT_RGBA(255, 80, 80, 38) : fill;
        }
    } else if (flags & (LT_UI_BTN_PRIMARY | LT_UI_BTN_DEFAULT | LT_UI_BTN_SELECTED)) {
        fill = (res & UI_ACTIVE) ? t->primary_active : ((res & UI_HOVER) ? t->primary_hover : t->primary);
        border = fill; fg = t->primary_text;
    } else if (flags & LT_UI_BTN_DANGER) {
        fill = t->dark ? ((res & UI_HOVER) ? t->error : t->error_bg)
                       : ((res & UI_HOVER) ? lt_color_mix(t->error, LT_RGB(255, 255, 255), 40) : t->error);
        border = fill; fg = ui_text_on(LT_RGB(255, 255, 255), LT_RGB(16, 16, 16), fill);
    } else if (flags & LT_UI_BTN_ACCENT) {
        fill = (res & UI_HOVER) ? t->accent_hover : t->accent; border = fill; fg = t->accent_text;
    } else if (flags & LT_UI_BTN_FLAT) {
        fill = (res & UI_ACTIVE) ? t->active : ((res & UI_HOVER) ? t->hover : 0);
        border = 0; fg = (res & UI_HOVER) ? t->text : t->text2;
    } else {
        fill = (res & UI_ACTIVE) ? t->active : ((res & UI_HOVER) ? t->hover : t->elevated);
        border = (res & UI_HOVER) ? t->border_strong : t->border; fg = t->text;
    }
    if (dis) {
        fill = (flags & LT_UI_BTN_FLAT) ? 0 : t->surface;
        border = (flags & LT_UI_BTN_FLAT) ? 0 : t->border_subtle;
        fg = t->disabled;
    }
    if (ui->cell) {
        /* text-mode button: a tinted pill; the focused one is solid with a marker */
        int plain = !(flags & (LT_UI_BTN_PRIMARY | LT_UI_BTN_DEFAULT | LT_UI_BTN_SELECTED | LT_UI_BTN_DANGER |
                               LT_UI_BTN_ACCENT | LT_UI_BTN_FLAT));
        if (plain && !dis) {
            fill = (res & UI_ACTIVE) ? t->primary_active
                 : ((res & UI_HOVER) ? lt_color_mix(t->elevated, t->primary, 120) : lt_color_mix(t->elevated, t->primary, 64));
            fg = t->text;
        }
        if (focused && ui->kbd_nav) {
            fill = t->primary; fg = t->primary_text;
        }
        border = 0;
    } else if (focused && ui->kbd_nav) {
        border = t->focus;
    }
    if (fill != 0 || border != 0) { lt_ui_draw_box(ui, r, fill, border, t->radius); }
    if (icon) {
        if (icon >= 1000 && !ui->cell) {
            lt_ui_draw_lucide(ui, r, icon - 1000, r.h >= lt_ui_px(ui, 26) ? 18 : 14, 0.0, fg);
        } else {
            lt_ui_draw_icon(ui, r, icon, fg);
        }
    } else {
        ui_text_in_f(ui, r, vis, fg, 1, fid, 0);
        if (ui->cell && !dis && (flags & (LT_UI_BTN_PRIMARY | LT_UI_BTN_DEFAULT | LT_UI_BTN_SELECTED) ||
                                  (focused && ui->kbd_nav))) {
            ui_cell_bold(ui, r);
        }
    }
    if (dis) { ui->disabled--; }
    ui_set_last(ui, id, r);
    if (dis) { ui->last_hover = 0; }
    return clicked ? LT_TRUE : LT_FALSE;
}

lt_bool lt_ui_button(lt_ui *ui, const char *label) { return button_impl(ui, label, 0, 0); }
lt_bool lt_ui_button_ex(lt_ui *ui, const char *label, int flags) { return button_impl(ui, label, 0, flags); }

lt_bool lt_ui_icon_button(lt_ui *ui, int icon, const char *tooltip, int flags)
{
    char id[40];
    lt_bool r;
    lt_snprintf(id, sizeof(id), "##icon%d", icon);
    r = button_impl(ui, id, icon, flags | LT_UI_BTN_FLAT);
    if (tooltip != NULL) { lt_ui_tooltip(ui, tooltip); }
    return r;
}

lt_bool lt_ui_toggle(lt_ui *ui, const char *label, int *on)
{
    lt_bool c = button_impl(ui, label, 0, (*on ? LT_UI_BTN_SELECTED : 0));
    if (c) { *on = !*on; }
    return c;
}

lt_bool lt_ui_checkbox(lt_ui *ui, const char *label, int *v)
{
    char buf[256];
    const char *vis = ui_label_visible(label, buf, sizeof(buf));
    lt_u32 id = lt_ui_id(ui, label);
    const lt_theme *t = &ui->theme;
    int bw = ui->cell ? 24 : lt_ui_px(ui, 14), gap = ui->cell ? 8 : lt_ui_px(ui, 8);
    int tw = lt_ui_text_width(ui, vis);
    lt_rect r = ui_next(ui, bw + gap + tw + 2);
    lt_rect box;
    int res = ui_interact(ui, id, r, UI_FOCUSABLE | UI_KIND(UI_K_BUTTON));
    int changed = 0, on;
    lt_color fg = ui->disabled ? t->disabled : t->text;
    if ((res & UI_CLICK) || key_activate(ui, id)) { *v = !*v; changed = 1; }
    on = *v != 0;
    if (ui->cell) {
        lt_color c = on ? t->primary_hover : ((res & UI_HOVER) ? t->text : t->text2);
        box = lt_rect_make(r.x, r.y + (r.h - 16) / 2, 24, 16);
        if (ui->focus == id && ui->kbd_nav) { lt_ui_draw_fill(ui, r, t->hover); }
        lt_ui_draw_text(ui, box.x, box.y, on ? "[\xE2\x9C\x93]" : "[ ]", ui->disabled ? t->disabled : c);
    } else {
        lt_color fill, border;
        box = lt_rect_make(r.x, r.y + (r.h - bw) / 2, bw, bw);
        if (on) {
            fill = (res & UI_HOVER) ? t->primary_hover : t->primary; border = fill;
        } else {
            fill = t->input; border = (res & UI_HOVER) ? t->text2 : t->border_strong;
        }
        if (ui->disabled) { fill = t->surface; border = t->border_subtle; }
        if (ui->focus == id && ui->kbd_nav) { border = t->focus; }
        lt_ui_draw_box(ui, box, fill, border, lt_ui_px(ui, 2));
        if (on) { lt_ui_draw_lucide(ui, box, LT_ICON_CHECK, 11, 3.0, ui->disabled ? t->disabled : t->primary_text); }
    }
    ui_text_in(ui, lt_rect_make(r.x + bw + gap, r.y, r.w - bw - gap, r.h), vis, fg, 0);
    ui_set_last(ui, id, r);
    return changed ? LT_TRUE : LT_FALSE;
}

lt_bool lt_ui_radio(lt_ui *ui, const char *label, int *v, int value)
{
    char buf[256];
    const char *vis = ui_label_visible(label, buf, sizeof(buf));
    lt_u32 id = lt_ui_id(ui, label);
    const lt_theme *t = &ui->theme;
    int bw = ui->cell ? 24 : lt_ui_px(ui, 14), gap = 8;
    int tw = lt_ui_text_width(ui, vis);
    lt_rect r = ui_next(ui, bw + gap + tw + 2);
    int res = ui_interact(ui, id, r, UI_FOCUSABLE | UI_KIND(UI_K_BUTTON));
    int changed = 0, on;
    lt_color fg = ui->disabled ? t->disabled : t->text;
    if (((res & UI_CLICK) || key_activate(ui, id)) && *v != value) { *v = value; changed = 1; }
    on = (*v == value);
    if (ui->cell) {
        lt_color c = on ? t->primary_hover : ((res & UI_HOVER) ? t->text : t->text2);
        if (ui->focus == id && ui->kbd_nav) { lt_ui_draw_fill(ui, r, t->hover); }
        lt_ui_draw_text(ui, r.x, r.y + (r.h - 16) / 2, on ? "(\xE2\x97\x8F)" : "( )", c);
    } else {
        double cx = r.x + bw / 2.0, cy = r.y + r.h / 2.0, rr = bw / 2.0;
        lt_color border = (ui->focus == id && ui->kbd_nav) ? t->focus :
                          (on ? t->primary : ((res & UI_HOVER) ? t->text2 : t->border_strong));
        lt_ui_draw_circle_fill(ui, cx, cy, rr, border);
        lt_ui_draw_circle_fill(ui, cx, cy, rr - 1.0, ui->disabled ? t->surface : t->input);
        if (on) { lt_ui_draw_circle_fill(ui, cx, cy, rr * 0.45, ui->disabled ? t->disabled : t->primary); }
    }
    ui_text_in(ui, lt_rect_make(r.x + bw + gap, r.y, r.w - bw - gap, r.h), vis, fg, 0);
    ui_set_last(ui, id, r);
    return changed ? LT_TRUE : LT_FALSE;
}

/* ------------------------------------------------------------------ */
/* slider                                                              */
lt_bool lt_ui_slider(lt_ui *ui, const char *label, double *v, double mn, double mx, int digits)
{
    lt_u32 id = lt_ui_id(ui, label);
    const lt_theme *t = &ui->theme;
    lt_rect r = ui_next(ui, 160);
    lt_rect track;
    int res = ui_interact(ui, id, r, UI_FOCUSABLE | UI_KIND(UI_K_OTHER));
    int changed = 0, vw = 0;
    double frac, old = *v, step = (mx - mn) / 100.0;
    char txt[40];
    if (r.w >= 140) {
        lt_fmt_double(txt, sizeof(txt), *v, digits);
        vw = lt_ui_text_width(ui, "-0000.00") + (ui->cell ? 8 : lt_ui_px(ui, 10));
    }
    track = lt_rect_make(r.x + (ui->cell ? 0 : lt_ui_px(ui, 8)), r.y, r.w - vw - (ui->cell ? 0 : lt_ui_px(ui, 16)), r.h);
    if (track.w < 16) { track.w = 16; }
    if (digits == 0) { step = LT_MAX(1.0, step); }
    if (ui->active == id && mx > mn) {
        double f = (double)(ui->mx - track.x) / (double)(track.w > 1 ? track.w - 1 : 1);
        f = LT_CLAMP(f, 0.0, 1.0);
        *v = mn + f * (mx - mn);
    }
    if (ui->focus == id && !ui->disabled) {
        int mods = 0;
        double s = (ui->mods & LT_MOD_SHIFT) ? step * 5.0 : step;
        if (ui_key_take(ui, LT_KEY_LEFT, &mods) || ui_key_take(ui, LT_KEY_DOWN, &mods)) { *v -= s; }
        if (ui_key_take(ui, LT_KEY_RIGHT, &mods) || ui_key_take(ui, LT_KEY_UP, &mods)) { *v += s; }
        if (ui_key_take(ui, LT_KEY_HOME, &mods)) { *v = mn; }
        if (ui_key_take(ui, LT_KEY_END, &mods)) { *v = mx; }
    }
    *v = LT_CLAMP(*v, mn, mx);
    if (digits >= 0 && digits < 8) {
        double sc = 1.0;
        int i;
        for (i = 0; i < digits; i++) { sc *= 10.0; }
        if (res & UI_ACTIVE) { *v = lt_round(*v * sc) / sc; }
    }
    if (*v != old) { changed = 1; }
    frac = (mx > mn) ? (*v - mn) / (mx - mn) : 0.0;
    if (ui->cell) {
        int kx = track.x + (int)(frac * (double)(track.w - 8) / 8.0 + 0.5) * 8;
        lt_color tc = (ui->focus == id && ui->kbd_nav) ? t->primary_hover : t->primary;
        lt_ui_draw_hline(ui, track.x, track.x + track.w - 1, r.y + 8, t->border);
        if (kx > track.x) { lt_ui_draw_hline(ui, track.x, kx - 1, r.y + 8, tc); }
        lt_ui_draw_text(ui, kx, r.y, "\xE2\x97\x8F", (res & UI_ACTIVE) ? t->text : tc);
    } else {
        int ty = r.y + r.h / 2 - 2, kx = track.x + (int)(frac * (double)(track.w - 1));
        lt_color knob = (res & UI_ACTIVE) ? t->primary_hover : ((res & UI_HOVER) ? t->text : t->text2);
        lt_ui_draw_box(ui, lt_rect_make(track.x, ty, track.w, 4), ui->disabled ? t->surface : t->active, 0, 2);
        if (kx > track.x) {
            lt_ui_draw_box(ui, lt_rect_make(track.x, ty, kx - track.x, 4), ui->disabled ? t->disabled : t->primary, 0, 2);
        }
        if (ui->focus == id && ui->kbd_nav) {
            lt_ui_draw_circle_fill(ui, kx, r.y + r.h / 2.0, 9.0, t->focus);
        }
        lt_ui_draw_circle_fill(ui, kx, r.y + r.h / 2.0, 7.0, ui->disabled ? t->disabled : knob);
        lt_ui_draw_circle_fill(ui, kx, r.y + r.h / 2.0, 3.0, t->input);
    }
    if (vw > 0) {
        lt_ui_draw_text_in(ui, lt_rect_make(r.x + r.w - vw, r.y, vw, r.h), txt, t->text2, 2);
    }
    ui_set_last(ui, id, r);
    return changed ? LT_TRUE : LT_FALSE;
}

/* ------------------------------------------------------------------ */
/* progress / spinner / swatch / image                                 */
void lt_ui_progress(lt_ui *ui, double frac, const char *text)
{
    const lt_theme *t = &ui->theme;
    lt_rect r = ui_next(ui, 160);
    lt_rect bar = r;
    char tmp[16];
    if (!ui->cell) { bar = lt_rect_make(r.x, r.y + (r.h - 14) / 2, r.w, 14); }
    lt_ui_draw_box(ui, bar, t->input, ui->cell ? 0 : t->border_subtle, ui->cell ? 0 : lt_ui_px(ui, 7));
    if (frac < 0.0) {
        double ph = fmod(ui->now / 1100.0, 2.0);
        double pos = ph < 1.0 ? ph : 2.0 - ph;
        int sw = LT_MAX(bar.w / 4, 16), sx = bar.x + (int)(pos * (double)(bar.w - sw));
        lt_ui_clip_push(ui, lt_rect_inset(bar, ui->cell ? 0 : 1, ui->cell ? 0 : 1));
        lt_ui_draw_box(ui, lt_rect_make(sx, bar.y, sw, bar.h), t->primary, 0, ui->cell ? 0 : lt_ui_px(ui, 6));
        lt_ui_clip_pop(ui);
        ui_animate(ui, 33);
    } else {
        int fw;
        frac = LT_CLAMP(frac, 0.0, 1.0);
        fw = (int)(frac * (double)(bar.w - (ui->cell ? 0 : lt_ui_px(ui, 2))));
        if (fw > 0) {
            lt_ui_draw_box(ui, lt_rect_make(bar.x + (ui->cell ? 0 : 1), bar.y + (ui->cell ? 0 : 1), fw,
                                            bar.h - (ui->cell ? 0 : lt_ui_px(ui, 2))), t->primary, 0, ui->cell ? 0 : lt_ui_px(ui, 6));
        }
        if (text == NULL) {
            lt_snprintf(tmp, sizeof(tmp), "%d%%", (int)(frac * 100.0 + 0.5));
            text = tmp;
        }
    }
    if (text != NULL && text[0]) { ui_text_in(ui, bar, text, t->text, 1); }
    ui_set_last(ui, lt_ui_id(ui, "##progress"), r);
}

void lt_ui_spinner(lt_ui *ui)
{
    const lt_theme *t = &ui->theme;
    lt_rect r = ui_next(ui, ui->cell ? 8 : lt_ui_px(ui, 24));
    double cx = r.x + r.w / 2.0, cy = r.y + r.h / 2.0;
    if (ui->cell) {
        static const char frames[] = "|/-\\";
        char s[2];
        s[0] = frames[((long)(ui->now / 100.0)) & 3]; s[1] = '\0';
        ui_text_in(ui, r, s, t->primary_hover, 1);
    } else {
        double a = fmod(ui->now / 1000.0 * 5.0, 2.0 * LT_PI);
        double rad = LT_MIN(r.w, r.h) / 2.0 - 4.0;
        if (rad < 4.0) { rad = 4.0; }
        lt_ui_draw_circle(ui, cx, cy, rad, 2.0, t->active);
        lt_ui_draw_arc(ui, cx, cy, rad, a, a + 1.9 * LT_PI * 0.45 * 2.0 / 2.0, 2.5, t->primary_hover);
    }
    ui_animate(ui, ui->cell ? 100 : 33);
    ui_set_last(ui, lt_ui_id(ui, "##spinner"), r);
}

lt_bool lt_ui_color_swatch(lt_ui *ui, lt_color c)
{
    const lt_theme *t = &ui->theme;
    char b[24];
    lt_u32 id;
    lt_rect r, sw;
    int res;
    lt_snprintf(b, sizeof(b), "##sw%lx", (unsigned long)(c & 0xFFFFFFUL));
    id = lt_ui_id(ui, b);
    r = ui_next(ui, ui->cell ? 24 : lt_ui_px(ui, 40));
    sw = ui->cell ? r : lt_rect_make(r.x, r.y + 2, r.w, r.h - 4);
    res = ui_interact(ui, id, r, UI_FOCUSABLE | UI_KIND(UI_K_BUTTON));
    lt_ui_draw_box(ui, sw, c | 0xFF000000UL,
                   (res & UI_HOVER) ? t->text : ((ui->focus == id && ui->kbd_nav) ? t->focus : t->border),
                   t->radius);
    ui_set_last(ui, id, r);
    return (res & UI_CLICK) ? LT_TRUE : LT_FALSE;
}

void lt_ui_image(lt_ui *ui, const lt_u32 *px, int w, int h, int pitch)
{
    lt_rect r = ui_next(ui, w);
    char lab[48];
    ui_set_last(ui, lt_ui_id(ui, "##image"), r);
    if (ui->cell || px == NULL) {
        lt_ui_draw_box(ui, r, ui->theme.surface, ui->theme.border_subtle, ui->theme.radius);
        lt_snprintf(lab, sizeof(lab), "[image %dx%d]", w, h);
        ui_text_in(ui, r, lab, ui->theme.muted, 1);
        return;
    }
    lt_ui_clip_push(ui, r);
    lt_ui_draw_blit(ui, r.x, r.y, px, w, h, pitch);
    lt_ui_clip_pop(ui);
}

/* ------------------------------------------------------------------ */
/* shell bars                                                          */
static void bar_begin(lt_ui *ui, lt_rect r, lt_color bg, int top)
{
    const lt_theme *t = &ui->theme;
    int i, w[LT_UI_MAX_COLS];
    lt_ui_draw_fill(ui, r, bg);
    if (!ui->cell) {
        lt_ui_draw_hline(ui, r.x, r.x + r.w - 1, top ? r.y + r.h - 1 : r.y, t->border_subtle);
    }
    lt_ui_clip_push(ui, r);
    {
        int vp = ui->cell ? 0 : lt_ui_px(ui, 4);
        ui_layout_push(ui, lt_rect_make(r.x + t->padding, r.y + vp, r.w - 2 * t->padding, r.h - 2 * vp));
        for (i = 0; i < LT_UI_MAX_COLS; i++) { w[i] = 0; }
        ui_layout_row_dev(ui, LT_UI_MAX_COLS, w, r.h - 2 * vp);
    }
}

void lt_ui_statusbar_begin(lt_ui *ui)
{
    int h = ui->theme.statusbar_h;
    lt_rect r = lt_rect_make(0, ui->sh - ui->bottom_used - h, ui->sw, h);
    ui->bottom_used += h;
    bar_begin(ui, r, ui->theme.statusbar, 0);
}

void lt_ui_statusbar_end(lt_ui *ui)
{
    ui_layout_pop(ui);
    lt_ui_clip_pop(ui);
}

void lt_ui_toolbar_begin(lt_ui *ui)
{
    int h = ui->theme.tbar_h;
    lt_rect r = lt_rect_make(0, ui->used_top, ui->sw, h);
    ui->used_top += h;
    bar_begin(ui, r, ui->theme.bg_toolbar, 1);
    if (!ui->cell) { ui_lay(ui)->sp_x = lt_ui_px(ui, 2); }
}

void lt_ui_toolbar_end(lt_ui *ui)
{
    ui_layout_pop(ui);
    lt_ui_clip_pop(ui);
}

void lt_ui_toolbar_separator(lt_ui *ui)
{
    lt_rect r = ui_next(ui, ui->cell ? 8 : lt_ui_px(ui, 9));
    if (!ui->cell) {
        lt_ui_draw_vline(ui, r.x + r.w / 2, r.y + lt_ui_px(ui, 3), r.y + r.h - lt_ui_px(ui, 3) - 1, ui->theme.border);
    }
}
