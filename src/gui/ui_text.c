/* ui_text.c - single-line text editing core, text input and number field.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "ui_priv.h"

/* ------------------------------------------------------------------ */
/* utf-8 helpers on byte offsets                                       */
static int cp_count(const char *s)
{
    int n = 0;
    while (lt_utf8_next(&s) != 0) { n++; }
    return n;
}

static int prev_char(const char *s, int pos)
{
    if (pos <= 0) { return 0; }
    pos--;
    while (pos > 0 && ((unsigned char)s[pos] & 0xC0) == 0x80) { pos--; }
    return pos;
}

static int next_char(const char *s, int pos)
{
    const char *p = s + pos;
    if (*p == '\0') { return pos; }
    lt_utf8_next(&p);
    return (int)(p - s);
}

static int is_word(char c)
{
    unsigned char u = (unsigned char)c;
    return (u >= '0' && u <= '9') || (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') || u == '_' ||
           u >= 0x80;
}

static int word_left(const char *s, int pos)
{
    while (pos > 0 && !is_word(s[pos - 1])) { pos--; }
    while (pos > 0 && is_word(s[pos - 1])) { pos--; }
    return pos;
}

static int word_right(const char *s, int pos)
{
    int n = (int)strlen(s);
    while (pos < n && !is_word(s[pos])) { pos++; }
    while (pos < n && is_word(s[pos])) { pos++; }
    return pos;
}

/* ------------------------------------------------------------------ */
/* session                                                             */
void ui_edit_end(lt_ui *ui)
{
    ui->edit.id = 0;
    ui->edit.buf = NULL;
    ui->edit.dragging = 0;
}

static void edit_begin(lt_ui *ui, lt_u32 id, char *buf, size_t cap, int flags, int select_all)
{
    ui_edit *e = &ui->edit;
    e->id = id;
    e->buf = buf;
    e->cap = cap;
    e->flags = flags;
    e->orig_ok = strlen(buf) < sizeof(e->orig);
    lt_strlcpy(e->orig, buf, sizeof(e->orig));
    e->cursor = (int)strlen(buf);
    e->anchor = select_all ? 0 : e->cursor;
    e->scroll = 0;
    e->dragging = 0;
    e->blink0 = ui->now;
}

void ui_edit_select_all(lt_ui *ui)
{
    if (ui->edit.id != 0 && ui->edit.buf != NULL) {
        ui->edit.anchor = 0;
        ui->edit.cursor = (int)strlen(ui->edit.buf);
    }
}

static int sel_lo(const ui_edit *e) { return e->cursor < e->anchor ? e->cursor : e->anchor; }
static int sel_hi(const ui_edit *e) { return e->cursor < e->anchor ? e->anchor : e->cursor; }

static void delete_range(ui_edit *e, int lo, int hi)
{
    size_t n = strlen(e->buf);
    if (lo < 0) { lo = 0; }
    if (hi > (int)n) { hi = (int)n; }
    if (hi <= lo) { return; }
    memmove(e->buf + lo, e->buf + hi, n - (size_t)hi + 1);
    e->cursor = e->anchor = lo;
}

static int insert_text(ui_edit *e, const char *txt)
{
    size_t n = strlen(e->buf), t = strlen(txt);
    int lo = sel_lo(e), hi = sel_hi(e);
    size_t keep = n - (size_t)(hi - lo);
    if (keep + t + 1 > e->cap) {
        /* truncate on a character boundary */
        size_t room = e->cap > keep + 1 ? e->cap - keep - 1 : 0;
        while (t > room) { t = (size_t)prev_char(txt, (int)t); }
        if (t == 0) { return 0; }
    }
    if (hi > lo) { delete_range(e, lo, hi); }
    n = strlen(e->buf);
    memmove(e->buf + e->cursor + t, e->buf + e->cursor, n - (size_t)e->cursor + 1);
    memcpy(e->buf + e->cursor, txt, t);
    e->cursor += (int)t;
    e->anchor = e->cursor;
    return 1;
}

static int numeric_ok(char c)
{
    return (c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.' || c == 'e' || c == 'E';
}

static int insert_filtered(lt_ui *ui, const char *txt, int flags)
{
    char tmp[512];
    size_t i, o = 0;
    for (i = 0; txt[i] && o + 1 < sizeof(tmp); i++) {
        char c = txt[i];
        if (c == '\n' || c == '\r' || c == '\t') { continue; }
        if ((flags & LT_UI_TEXT_NUMERIC) && !numeric_ok(c)) {
            if (c == ',') { c = '.'; } else { continue; }
        }
        tmp[o++] = c;
    }
    tmp[o] = '\0';
    LT_UNUSED(ui);
    if (o == 0) { return 0; }
    return insert_text(&ui->edit, tmp);
}

static void select_word(ui_edit *e)
{
    int lo = e->cursor, hi = e->cursor;
    int n = (int)strlen(e->buf);
    while (lo > 0 && is_word(e->buf[lo - 1])) { lo--; }
    while (hi < n && is_word(e->buf[hi])) { hi++; }
    e->anchor = lo;
    e->cursor = hi;
}

static int cursor_from_mouse(lt_ui *ui, lt_rect r, int px)
{
    ui_edit *e = &ui->edit;
    int x0 = r.x + ui->theme.pad_x - e->scroll;
    int best = 0, bestd = 1 << 30, pos = 0;
    const char *p = e->buf;
    if (px <= x0) { return 0; }
    for (;;) {
        int d = abs(px - (x0 + ui_prefix_w(ui, e->buf, pos)));
        if (d < bestd) { bestd = d; best = pos; }
        if (*p == '\0') { break; }
        lt_utf8_next(&p);
        pos = (int)(p - e->buf);
    }
    return best;
}

/* process queued keyboard events for the active edit; returns flags */
static int edit_keys(lt_ui *ui, int flags)
{
    ui_edit *e = &ui->edit;
    int res = 0, i;
    int ro = (flags & LT_UI_TEXT_READONLY) != 0;
    lt_u32 h0 = lt_hash_str(e->buf, LT_FALSE);
    for (i = 0; i < ui->nq; i++) {
        ui_evq *q = &ui->q[i];
        int shift, ctrl, n;
        if (q->used) { continue; }
        e->cursor = LT_CLAMP(e->cursor, 0, (int)strlen(e->buf));
        e->anchor = LT_CLAMP(e->anchor, 0, (int)strlen(e->buf));
        if (q->is_text) {
            q->used = 1;
            if (!ro && insert_filtered(ui, q->text, flags)) { res |= LT_UI_CHANGED; }
            e->blink0 = ui->now;
            continue;
        }
        shift = (q->mods & LT_MOD_SHIFT) != 0;
        ctrl = (q->mods & LT_MOD_CTRL) != 0;
        n = (int)strlen(e->buf);
        switch (q->key) {
        case LT_KEY_BACKSPACE:
            q->used = 1;
            if (ro) { break; }
            if (sel_hi(e) > sel_lo(e)) { delete_range(e, sel_lo(e), sel_hi(e)); }
            else if (e->cursor > 0) {
                int lo = ctrl ? word_left(e->buf, e->cursor) : prev_char(e->buf, e->cursor);
                delete_range(e, lo, e->cursor);
            }
            res |= LT_UI_CHANGED;
            break;
        case LT_KEY_DELETE:
            q->used = 1;
            if (ro) { break; }
            if (sel_hi(e) > sel_lo(e)) { delete_range(e, sel_lo(e), sel_hi(e)); }
            else if (e->cursor < n) {
                int hi = ctrl ? word_right(e->buf, e->cursor) : next_char(e->buf, e->cursor);
                delete_range(e, e->cursor, hi);
            }
            res |= LT_UI_CHANGED;
            break;
        case LT_KEY_LEFT:
            q->used = 1;
            if (!shift && sel_hi(e) > sel_lo(e)) { e->cursor = e->anchor = sel_lo(e); }
            else {
                e->cursor = ctrl ? word_left(e->buf, e->cursor) : prev_char(e->buf, e->cursor);
                if (!shift) { e->anchor = e->cursor; }
            }
            break;
        case LT_KEY_RIGHT:
            q->used = 1;
            if (!shift && sel_hi(e) > sel_lo(e)) { e->cursor = e->anchor = sel_hi(e); }
            else {
                e->cursor = ctrl ? word_right(e->buf, e->cursor) : next_char(e->buf, e->cursor);
                if (!shift) { e->anchor = e->cursor; }
            }
            break;
        case LT_KEY_HOME:
            q->used = 1;
            e->cursor = 0;
            if (!shift) { e->anchor = 0; }
            break;
        case LT_KEY_END:
            q->used = 1;
            e->cursor = n;
            if (!shift) { e->anchor = n; }
            break;
        case 'a':
            if (ctrl) { q->used = 1; e->anchor = 0; e->cursor = n; }
            break;
        case 'c':
        case 'x':
            if (ctrl) {
                q->used = 1;
                if (sel_hi(e) > sel_lo(e) && !(flags & LT_UI_TEXT_PASSWORD)) {
                    char tmp[512];
                    int len = sel_hi(e) - sel_lo(e);
                    if (len > (int)sizeof(tmp) - 1) {
                        len = prev_char(e->buf + sel_lo(e), (int)sizeof(tmp));
                    }
                    memcpy(tmp, e->buf + sel_lo(e), (size_t)len);
                    tmp[len] = '\0';
                    lt_ui_clipboard_set(ui, tmp);
                    if (q->key == 'x' && !ro) {
                        delete_range(e, sel_lo(e), sel_hi(e));
                        res |= LT_UI_CHANGED;
                    }
                }
            }
            break;
        case 'v':
            if (ctrl) {
                char *cb;
                q->used = 1;
                cb = ro ? NULL : lt_ui_clipboard_get(ui);
                if (cb != NULL) {
                    if (insert_filtered(ui, cb, flags)) { res |= LT_UI_CHANGED; }
                    lt_free(cb);
                }
            }
            break;
        case 'z':
            if (ctrl && !ro) {
                q->used = 1;
                if (e->orig_ok) {
                    lt_strlcpy(e->buf, e->orig, e->cap);
                    e->cursor = e->anchor = (int)strlen(e->buf);
                    res |= LT_UI_CHANGED;
                }
            }
            break;
        case LT_KEY_ENTER:
            res |= LT_UI_SUBMIT;
            break;
        case LT_KEY_ESCAPE:
            /* an untouched field leaves Esc for the enclosing dialog/popup */
            if (e->orig_ok && strcmp(e->buf, e->orig) != 0) { q->used = 1; }
            if (!ro && e->orig_ok) { lt_strlcpy(e->buf, e->orig, e->cap); }
            res |= LT_UI_CANCEL;
            break;
        default: break;
        }
        e->blink0 = ui->now;
    }
    if (lt_hash_str(e->buf, LT_FALSE) != h0) { res |= LT_UI_CHANGED; } else { res &= ~LT_UI_CHANGED; }
    return res;
}

/* Text editing widget used by text input, number field and grid editing.
 * buf is edited in place while id holds the focus. */
int ui_edit_widget(lt_ui *ui, lt_u32 id, lt_rect r, char *buf, size_t cap, int flags, int draw_box)
{
    const lt_theme *t = &ui->theme;
    ui_edit *e = &ui->edit;
    int res = 0, ir, focused;
    int padx = t->pad_x;
    lt_rect inner;
    int caret_on;
    char disp[512];
    const char *show;
    lt_color bg, border;

    ir = ui_interact(ui, id, r, 0);
    ui_focusable(ui, id, UI_K_TEXT);
    if (ir & UI_PRESS) {
        ui->focus = id;
        ui->kbd_nav = 0;
    }
    focused = (ui->focus == id) && !ui->disabled;
    if (focused && e->id != id) {
        edit_begin(ui, id, buf, cap, flags, ((flags & LT_UI_TEXT_SELECTALL) || ui->kbd_nav) ? 1 : 0);
        if (ir & UI_PRESS) { e->anchor = e->cursor; }
    }
    if (!focused && e->id == id) { ui_edit_end(ui); }
    e = &ui->edit;
    if (focused && e->id == id) {
        e->buf = buf;
        e->cap = cap;
        e->flags = flags;
        /* mouse */
        if (ir & UI_PRESS) {
            e->cursor = cursor_from_mouse(ui, r, ui->mx);
            if (!(ui->mods & LT_MOD_SHIFT)) { e->anchor = e->cursor; }
            if (ir & UI_DBLCLICK) { select_word(e); }
            e->dragging = 1;
            e->blink0 = ui->now;
        } else if ((ir & UI_ACTIVE) && e->dragging) {
            e->cursor = cursor_from_mouse(ui, r, ui->mx);
        } else {
            e->dragging = 0;
        }
        res |= edit_keys(ui, flags);
        e->cursor = LT_CLAMP(e->cursor, 0, (int)strlen(buf));
        e->anchor = LT_CLAMP(e->anchor, 0, (int)strlen(buf));
        if (res & LT_UI_CANCEL) {
            ui->focus = 0;
            ui_edit_end(ui);
            focused = 0;
        }
    }
    /* draw */
    bg = ui->disabled ? t->surface : t->input;
    border = focused ? t->focus : ((ir & UI_HOVER) ? t->border_strong : t->border);
    if (ui->cell && focused) { bg = lt_color_mix(t->input, t->primary, 70); }
    if (draw_box) { ui_field_box(ui, r, bg, border, focused && ui->kbd_nav); }
    inner = lt_rect_make(r.x + padx - (ui->cell ? 0 : lt_ui_px(ui, 2)), r.y, r.w - 2 * padx + (ui->cell ? 0 : lt_ui_px(ui, 4)), r.h);
    if (flags & LT_UI_TEXT_PASSWORD) {
        size_t i, n = (size_t)cp_count(buf);
        for (i = 0; i < n && i + 1 < sizeof(disp); i++) { disp[i] = '*'; }
        disp[i] = '\0';
        show = disp;
    } else {
        show = buf;
    }
    lt_ui_clip_push(ui, inner);
    {
        int tx, ty, th = ui_text_box_h(ui), dy = (r.h - th) / 2;
        int scroll = (focused && e->id == id) ? e->scroll : 0;
        if (ui->cell) { dy = (dy / 16) * 16; }
        if (focused && e->id == id) {
            int cx = ui_prefix_w(ui, show, e->cursor);
            int vis = r.w - 2 * padx;
            int cw = ui->cell ? 8 : lt_ui_px(ui, 2);
            if (cx - scroll > vis - cw) { scroll = cx - vis + cw; }
            if (cx - scroll < 0) { scroll = cx; }
            if (scroll < 0) { scroll = 0; }
            e->scroll = scroll;
        }
        tx = r.x + padx - scroll;
        ty = r.y + dy;
        if (focused && e->id == id && sel_hi(e) > sel_lo(e)) {
            int a = ui_prefix_w(ui, show, sel_lo(e)), b = ui_prefix_w(ui, show, sel_hi(e));
            lt_ui_draw_fill(ui, lt_rect_make(tx + a, ty + (ui->cell ? 0 : 1), b - a, th - (ui->cell ? 0 : lt_ui_px(ui, 2))),
                            ui->cell ? t->primary_active : t->selected);
        }
        lt_ui_draw_text(ui, tx, ty, show, ui->disabled ? t->disabled : t->text);
        if (focused && e->id == id) {
            double ph = fmod(ui->now - e->blink0, 1000.0);
            if (ph < 0.0) { ph = 0.0; }
            caret_on = ph < 530.0;
            ui_animate(ui, (int)(caret_on ? 530.0 - ph : 1000.0 - ph) + 1);
            if (caret_on && sel_hi(e) == sel_lo(e)) {
                int cx = tx + ui_prefix_w(ui, show, e->cursor);
                if (ui->cell) {
                    char ch[8];
                    const char *p = show + e->cursor;
                    const char *q = p;
                    size_t k;
                    lt_ui_draw_fill(ui, lt_rect_make(cx, ty, 8, 16), t->text);
                    if (*p) {
                        lt_utf8_next(&q);
                        k = (size_t)(q - p);
                        if (k > 7) { k = 7; }
                        memcpy(ch, p, k); ch[k] = '\0';
                        lt_ui_draw_text(ui, cx, ty, ch, t->input);
                    }
                } else {
                    {
                    int cph = ui_font_px(ui, 0) * 5 / 4 + 1;
                    lt_ui_draw_fill(ui, lt_rect_make(cx, ty + (th - cph) / 2, lt_ui_px(ui, 1), cph), t->text);
                }
                }
            }
        }
    }
    lt_ui_clip_pop(ui);
    ui_set_last(ui, id, r);
    return res;
}

/* ------------------------------------------------------------------ */
int lt_ui_text(lt_ui *ui, const char *label, char *buf, size_t cap, int flags)
{
    lt_u32 id = lt_ui_id(ui, label);
    lt_rect r = ui_next(ui, 160);
    return ui_edit_widget(ui, id, r, buf, cap, flags, 1);
}

/* ------------------------------------------------------------------ */
static double round_digits(double v, int digits)
{
    double sc = 1.0;
    int i;
    if (digits < 0) { return v; }
    for (i = 0; i < digits && i < 9; i++) { sc *= 10.0; }
    return lt_round(v * sc) / sc;
}

static void num_format(char *dst, size_t cap, double v, int digits)
{
    lt_fmt_double(dst, cap, v, digits < 0 ? 6 : digits);
}

int lt_ui_number(lt_ui *ui, const char *label, double *v, double mn, double mx, double step, int digits)
{
    lt_u32 id = lt_ui_id(ui, label);
    const lt_theme *t = &ui->theme;
    lt_rect r = ui_next(ui, 96);
    int res = 0, ir;
    int has_range = mx > mn;
    ui_state *st;
    double old = *v;
    char txt[64];

    if (step <= 0.0) { step = 1.0; }
    st = ui_state_get(ui, id);   /* d[0] = drag start value, i[0] = dragging, i[1] = start x */
    if (ui->focus == id && !ui->disabled) {
        /* editing */
        int fl, mods = 0;
        if (ui->numedit != id) {
            ui->numedit = id;
            num_format(ui->numbuf, sizeof(ui->numbuf), *v, digits);
            if (ui->edit.id == id) { ui_edit_end(ui); }
        }
        fl = ui_edit_widget(ui, id, r, ui->numbuf, sizeof(ui->numbuf),
                            LT_UI_TEXT_NUMERIC | LT_UI_TEXT_SELECTALL, 1);
        if (fl & LT_UI_CANCEL) {
            ui->numedit = 0;
            res |= LT_UI_CANCEL;
        } else {
            double nv = *v;
            int stepped = 0;
            if (ui_key_take(ui, LT_KEY_UP, &mods)) { stepped = 1; }
            else if (ui_key_take(ui, LT_KEY_DOWN, &mods)) { stepped = -1; }
            else if (ui_key_take(ui, LT_KEY_PAGEUP, &mods)) { stepped = 10; }
            else if (ui_key_take(ui, LT_KEY_PAGEDOWN, &mods)) { stepped = -10; }
            if (stepped != 0) {
                double s = step * (double)stepped;
                double cur = nv;
                if (lt_parse_double(ui->numbuf, &cur)) { nv = cur; }
                if (mods & LT_MOD_SHIFT) { s *= 10.0; }
                if (mods & LT_MOD_CTRL) { s *= 0.1; }
                nv = round_digits(nv + s, digits + (mods & LT_MOD_CTRL ? 1 : 0));
                if (has_range) { nv = LT_CLAMP(nv, mn, mx); }
                *v = nv;
                num_format(ui->numbuf, sizeof(ui->numbuf), nv, digits + (mods & LT_MOD_CTRL ? 1 : 0));
                ui->edit.cursor = (int)strlen(ui->numbuf);
                ui->edit.anchor = 0;
            }
            if (fl & LT_UI_SUBMIT) {
                double pv;
                if (lt_parse_double(ui->numbuf, &pv)) {
                    pv = round_digits(pv, digits < 0 ? 6 : digits + 2);
                    if (has_range) { pv = LT_CLAMP(pv, mn, mx); }
                    *v = pv;
                }
                ui->numedit = 0;
                ui->focus = 0;
                ui_edit_end(ui);
                res |= LT_UI_SUBMIT;
            }
        }
    } else {
        /* idle: display, hover, scrub, click-to-edit */
        lt_color fill, border, fg;
        if (ui->numedit == id) {
            double pv;
            if (lt_parse_double(ui->numbuf, &pv)) {
                if (has_range) { pv = LT_CLAMP(pv, mn, mx); }
                *v = pv;
            }
            ui->numedit = 0;
            res |= LT_UI_SUBMIT;
        }
        ui_focusable(ui, id, UI_K_TEXT);
        ir = ui_interact(ui, id, r, 0);
        if (ir & UI_PRESS) {
            st->i[0] = 1; st->i[1] = ui->mx; st->d[0] = *v;
        }
        if ((ir & UI_ACTIVE) && st->i[0] == 1) {
            int dx = ui->mx - st->i[1];
            if (abs(dx) > 3 || st->i[2]) {
                double s = step * (double)dx / 3.0;
                double nv;
                st->i[2] = 1;
                if (ui->mods & LT_MOD_SHIFT) { s *= 10.0; }
                if (ui->mods & LT_MOD_CTRL) { s *= 0.1; }
                nv = round_digits(st->d[0] + s, digits);
                if (has_range) { nv = LT_CLAMP(nv, mn, mx); }
                *v = nv;
            }
        }
        if (!(ir & UI_ACTIVE) && st->i[0] == 1) {
            if ((ir & UI_CLICK) && !st->i[2]) { ui->focus = id; ui->kbd_nav = 0; ui_request(ui); }
            st->i[0] = 0; st->i[2] = 0;
        }
        fill = ui->disabled ? t->surface : t->input;
        border = (ir & UI_HOVER) ? t->border_strong : t->border;
        fg = ui->disabled ? t->disabled : t->text;
        if (ir & UI_ACTIVE) { border = t->primary; }
        lt_ui_draw_box(ui, r, fill, border, t->radius);
        num_format(txt, sizeof(txt), *v, digits);
        ui_text_in(ui, lt_rect_make(r.x + t->pad_x, r.y, r.w - 2 * t->pad_x, r.h), txt, fg, 2);
        ui_set_last(ui, id, r);
    }
    /* every input path above clamps what the user enters; a stored value outside the range is
     * shown as it is, not rewritten just because the field was drawn */
    if (*v != old) { res |= LT_UI_CHANGED; }
    return res;
}
