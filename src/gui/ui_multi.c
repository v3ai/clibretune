/* ui_multi.c - multi-line text editor widget (monospaced, line numbers, optional syntax colours).
 *
 * The caller owns the text buffer (NUL terminated, capacity cap) and a lt_ui_multi state (caret,
 * selection, undo snapshots).  The widget fills the next layout cell, scrolls like a list box and
 * handles keyboard (arrows, Home/End, PageUp/Down, Backspace/Delete, Enter with auto indent, Tab,
 * Ctrl+A/C/X/V/Z) and mouse (click, drag, double click for a word).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdlib.h>
#include <string.h>
#include "ui_priv.h"

#define MU_TAB 4

/* ------------------------------------------------------------------ text helpers */
static int is_word_ch(char c)
{
    unsigned char u = (unsigned char)c;
    return (u >= '0' && u <= '9') || (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') || u == '_' ||
           u >= 0x80;
}

static size_t ln_start(const char *b, size_t pos)
{
    while (pos > 0 && b[pos - 1] != '\n') { pos--; }
    return pos;
}

static size_t ln_end(const char *b, size_t pos)
{
    while (b[pos] != '\0' && b[pos] != '\n') { pos++; }
    return pos;
}

static size_t prev_cp(const char *b, size_t pos)
{
    if (pos == 0) { return 0; }
    pos--;
    while (pos > 0 && ((unsigned char)b[pos] & 0xC0) == 0x80) { pos--; }
    return pos;
}

static size_t next_cp(const char *b, size_t pos)
{
    if (b[pos] == '\0') { return pos; }
    pos++;
    while (b[pos] != '\0' && ((unsigned char)b[pos] & 0xC0) == 0x80) { pos++; }
    return pos;
}

/* display column of byte offset pos on the line starting at ls (tabs expand) */
static int col_at(const char *b, size_t ls, size_t pos)
{
    int col = 0;
    size_t i = ls;
    while (i < pos && b[i] != '\0') {
        if (b[i] == '\t') { col = (col / MU_TAB + 1) * MU_TAB; i++; }
        else { col++; i = next_cp(b, i); }
    }
    return col;
}

/* byte offset on line [ls, le] for display column col (nearest at or before) */
static size_t pos_at(const char *b, size_t ls, size_t le, int col)
{
    int c = 0;
    size_t i = ls;
    while (i < le) {
        int nc = b[i] == '\t' ? (c / MU_TAB + 1) * MU_TAB : c + 1;
        if (nc > col) { return i; }
        c = nc;
        i = b[i] == '\t' ? i + 1 : next_cp(b, i);
    }
    return le;
}

static size_t word_left(const char *b, size_t pos)
{
    while (pos > 0 && !is_word_ch(b[pos - 1]) && b[pos - 1] != '\n') { pos--; }
    while (pos > 0 && is_word_ch(b[pos - 1])) { pos--; }
    return pos;
}

static size_t word_right(const char *b, size_t pos)
{
    while (b[pos] != '\0' && !is_word_ch(b[pos]) && b[pos] != '\n') { pos++; }
    while (b[pos] != '\0' && is_word_ch(b[pos])) { pos++; }
    return pos;
}

/* ------------------------------------------------------------------ state */
void lt_ui_multi_init(lt_ui_multi *m)
{
    memset(m, 0, sizeof(*m));
    m->want_col = -1;
}

void lt_ui_multi_free(lt_ui_multi *m)
{
    int i;
    for (i = 0; i < m->nsnap; i++) { lt_free(m->snap[i]); }
    memset(m, 0, sizeof(*m));
    m->want_col = -1;
}

void lt_ui_multi_reset(lt_ui_multi *m, const char *text)
{
    int i;
    for (i = 0; i < m->nsnap; i++) { lt_free(m->snap[i]); m->snap[i] = NULL; }
    m->nsnap = 0;
    m->cursor = m->anchor = 0;
    m->want_col = -1;
    m->last_kind = 0;
    m->follow = 1;
    LT_UNUSED(text);
}

static void snap_push(lt_ui_multi *m, const char *buf)
{
    if (m->nsnap == LT_UI_MULTI_UNDO) {
        int i;
        lt_free(m->snap[0]);
        for (i = 1; i < m->nsnap; i++) { m->snap[i - 1] = m->snap[i]; m->snap_cur[i - 1] = m->snap_cur[i]; }
        m->nsnap--;
    }
    m->snap[m->nsnap] = lt_strdup(buf);
    m->snap_cur[m->nsnap] = m->cursor;
    m->nsnap++;
}

static int sel_lo(const lt_ui_multi *m) { return m->cursor < m->anchor ? m->cursor : m->anchor; }
static int sel_hi(const lt_ui_multi *m) { return m->cursor < m->anchor ? m->anchor : m->cursor; }

/* replace [lo, hi) by ins; kind: 1 typing, 2 deleting, 3 other (always its own undo step) */
static int replace_range(lt_ui_multi *m, char *buf, size_t cap, int lo, int hi, const char *ins,
                         size_t inlen, int kind)
{
    size_t n = strlen(buf);
    size_t keep = n - (size_t)(hi - lo);
    if (keep + inlen + 1 > cap) {
        size_t room = cap > keep + 1 ? cap - keep - 1 : 0;
        while (inlen > room) { inlen = prev_cp(ins, inlen); }
        if (inlen == 0 && hi <= lo) { return 0; }
    }
    if (kind == 3 || kind != m->last_kind || m->cursor != m->last_pos) { snap_push(m, buf); }
    memmove(buf + lo + inlen, buf + hi, n - (size_t)hi + 1);
    memcpy(buf + lo, ins, inlen);
    m->cursor = m->anchor = lo + (int)inlen;
    m->last_kind = kind;
    m->last_pos = m->cursor;
    m->want_col = -1;
    m->follow = 1;
    return 1;
}

static int insert_text(lt_ui_multi *m, char *buf, size_t cap, const char *txt, int kind)
{
    char *tmp = (char *)lt_malloc(strlen(txt) + 1);
    size_t i, o = 0;
    int r;
    for (i = 0; txt[i] != '\0'; i++) {
        char c = txt[i];
        if (c == '\r') {
            if (txt[i + 1] == '\n') { continue; }
            c = '\n';
        }
        if ((unsigned char)c < 32 && c != '\n' && c != '\t') { continue; }
        if (c == 127) { continue; }
        tmp[o++] = c;
    }
    tmp[o] = '\0';
    r = o > 0 || sel_hi(m) > sel_lo(m) ? replace_range(m, buf, cap, sel_lo(m), sel_hi(m), tmp, o, kind) : 0;
    lt_free(tmp);
    return r;
}

static int do_undo(lt_ui_multi *m, char *buf, size_t cap)
{
    if (m->nsnap == 0) { return 0; }
    m->nsnap--;
    lt_strlcpy(buf, m->snap[m->nsnap], cap);
    m->cursor = m->anchor = m->snap_cur[m->nsnap];
    if (m->cursor > (int)strlen(buf)) { m->cursor = m->anchor = (int)strlen(buf); }
    lt_free(m->snap[m->nsnap]);
    m->snap[m->nsnap] = NULL;
    m->last_kind = 0;
    m->want_col = -1;
    m->follow = 1;
    return 1;
}

/* ------------------------------------------------------------------ keys */
static int multi_keys(lt_ui *ui, lt_ui_multi *m, char *buf, size_t cap, int flags, int page)
{
    int ro = (flags & LT_UI_MULTI_READONLY) != 0;
    int changed = 0, i;
    for (i = 0; i < ui->nq; i++) {
        ui_evq *q = &ui->q[i];
        int shift, ctrl;
        size_t n, c, ls, le;
        if (q->used) { continue; }
        n = strlen(buf);
        if (m->cursor > (int)n) { m->cursor = (int)n; }
        if (m->anchor > (int)n) { m->anchor = (int)n; }
        if (m->cursor < 0) { m->cursor = 0; }
        if (m->anchor < 0) { m->anchor = 0; }
        c = (size_t)m->cursor;
        if (q->is_text) {
            q->used = 1;
            if (!ro && insert_text(m, buf, cap, q->text, 1)) { changed = 1; }
            m->blink0 = ui->now;
            continue;
        }
        shift = (q->mods & LT_MOD_SHIFT) != 0;
        ctrl = (q->mods & LT_MOD_CTRL) != 0;
        if ((q->mods & LT_MOD_ALT) != 0) { continue; }
        ls = ln_start(buf, c);
        le = ln_end(buf, c);
        switch (q->key) {
        case LT_KEY_BACKSPACE:
            q->used = 1;
            if (ro) { break; }
            if (sel_hi(m) > sel_lo(m)) { changed |= replace_range(m, buf, cap, sel_lo(m), sel_hi(m), "", 0, 2); }
            else if (c > 0) {
                size_t lo = ctrl ? word_left(buf, c) : prev_cp(buf, c);
                changed |= replace_range(m, buf, cap, (int)lo, (int)c, "", 0, 2);
            }
            break;
        case LT_KEY_DELETE:
            q->used = 1;
            if (ro) { break; }
            if (sel_hi(m) > sel_lo(m)) { changed |= replace_range(m, buf, cap, sel_lo(m), sel_hi(m), "", 0, 2); }
            else if (c < n) {
                size_t hi = ctrl ? word_right(buf, c) : next_cp(buf, c);
                changed |= replace_range(m, buf, cap, (int)c, (int)hi, "", 0, 2);
            }
            break;
        case LT_KEY_ENTER: {
            char tmp[128];
            size_t k = 0, p = ls;
            if (ctrl) { break; }   /* Ctrl+Enter is left to the host (run) */
            q->used = 1;
            if (ro) { break; }
            tmp[k++] = '\n';
            while (k + 1 < sizeof(tmp) && p < c && (buf[p] == ' ' || buf[p] == '\t')) { tmp[k++] = buf[p++]; }
            tmp[k] = '\0';
            changed |= replace_range(m, buf, cap, sel_lo(m), sel_hi(m), tmp, k, 3);
            break;
        }
        case LT_KEY_TAB:
            if (ctrl || ro) { break; }
            q->used = 1;
            if (shift) {
                size_t k = 0;
                while (k < 2 && buf[ls + k] == ' ') { k++; }
                if (k > 0) {
                    int oc = m->cursor;
                    changed |= replace_range(m, buf, cap, (int)ls, (int)(ls + k), "", 0, 3);
                    m->cursor = m->anchor = (oc - (int)k < (int)ls) ? (int)ls : oc - (int)k;
                }
            } else {
                changed |= replace_range(m, buf, cap, sel_lo(m), sel_hi(m), "  ", 2, 3);
            }
            break;
        case LT_KEY_LEFT:
            q->used = 1;
            if (!shift && sel_hi(m) > sel_lo(m)) { m->cursor = m->anchor = sel_lo(m); }
            else {
                m->cursor = (int)(ctrl ? word_left(buf, c) : prev_cp(buf, c));
                if (!shift) { m->anchor = m->cursor; }
            }
            m->want_col = -1; m->last_kind = 0; m->follow = 1;
            break;
        case LT_KEY_RIGHT:
            q->used = 1;
            if (!shift && sel_hi(m) > sel_lo(m)) { m->cursor = m->anchor = sel_hi(m); }
            else {
                m->cursor = (int)(ctrl ? word_right(buf, c) : next_cp(buf, c));
                if (!shift) { m->anchor = m->cursor; }
            }
            m->want_col = -1; m->last_kind = 0; m->follow = 1;
            break;
        case LT_KEY_UP:
        case LT_KEY_DOWN:
        case LT_KEY_PAGEUP:
        case LT_KEY_PAGEDOWN: {
            int steps = (q->key == LT_KEY_PAGEUP || q->key == LT_KEY_PAGEDOWN) ? page : 1;
            int up = q->key == LT_KEY_UP || q->key == LT_KEY_PAGEUP;
            size_t cl = ls, ce = le;
            int col = m->want_col >= 0 ? m->want_col : col_at(buf, ls, c);
            q->used = 1;
            while (steps-- > 0) {
                if (up) {
                    if (cl == 0) { break; }
                    cl = ln_start(buf, cl - 1);
                    ce = ln_end(buf, cl);
                } else {
                    if (buf[ce] == '\0') { break; }
                    cl = ce + 1;
                    ce = ln_end(buf, cl);
                }
            }
            if (cl == ls && ((up && cl == 0) || (!up && buf[le] == '\0'))) {
                /* already on the first / last line: go to the start / end of text */
                m->cursor = up ? 0 : (int)n;
            } else {
                m->cursor = (int)pos_at(buf, cl, ce, col);
            }
            if (!shift) { m->anchor = m->cursor; }
            m->want_col = col; m->last_kind = 0; m->follow = 1;
            break;
        }
        case LT_KEY_HOME:
            q->used = 1;
            m->cursor = ctrl ? 0 : (int)ls;
            if (!ctrl) {   /* smart home: first non-blank, then column 0 */
                size_t k = ls;
                while (buf[k] == ' ' || buf[k] == '\t') { k++; }
                if (k < c || (k > c && k < le)) { m->cursor = (int)k; }
                if (k == c) { m->cursor = (int)ls; }
            }
            if (!shift) { m->anchor = m->cursor; }
            m->want_col = -1; m->last_kind = 0; m->follow = 1;
            break;
        case LT_KEY_END:
            q->used = 1;
            m->cursor = ctrl ? (int)n : (int)le;
            if (!shift) { m->anchor = m->cursor; }
            m->want_col = -1; m->last_kind = 0; m->follow = 1;
            break;
        case 'a':
            if (ctrl) { q->used = 1; m->anchor = 0; m->cursor = (int)n; m->last_kind = 0; }
            break;
        case 'c':
        case 'x':
            if (ctrl) {
                q->used = 1;
                if (sel_hi(m) > sel_lo(m)) {
                    int len = sel_hi(m) - sel_lo(m);
                    char *tmp = (char *)lt_malloc((size_t)len + 1);
                    memcpy(tmp, buf + sel_lo(m), (size_t)len);
                    tmp[len] = '\0';
                    lt_ui_clipboard_set(ui, tmp);
                    lt_free(tmp);
                    if (q->key == 'x' && !ro) {
                        changed |= replace_range(m, buf, cap, sel_lo(m), sel_hi(m), "", 0, 3);
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
                    changed |= insert_text(m, buf, cap, cb, 3);
                    lt_free(cb);
                }
            }
            break;
        case 'z':
            if (ctrl && !ro) {
                q->used = 1;
                changed |= do_undo(m, buf, cap);
            }
            break;
        default: break;
        }
        m->blink0 = ui->now;
    }
    return changed;
}

/* ------------------------------------------------------------------ drawing */
#define COLX(col) ((int)((long)(col) * (long)w100 / 100L))
static lt_color class_color(const lt_theme *t, int cls)
{
    switch (cls) {
    case 1: return t->accent;
    case 2: return t->success;
    case 3: return t->muted;
    case 4: return t->warning;
    case 5: return t->info;
    case 6: return t->error;
    default: return t->text;
    }
}

static void draw_run(lt_ui *ui, int x, int y, const char *s, size_t len, lt_color c)
{
    char tmp[256];
    while (len > 0) {
        size_t k = len < sizeof(tmp) - 1 ? len : sizeof(tmp) - 1;
        while (k < len && k > 0 && ((unsigned char)s[k] & 0xC0) == 0x80) { k--; }
        if (k == 0) { k = len < sizeof(tmp) - 1 ? len : sizeof(tmp) - 1; }
        memcpy(tmp, s, k);
        tmp[k] = '\0';
        lt_ui_draw_text_f(ui, x, y, tmp, c, LT_FONT_MONO, LT_UI_FS_BASE);
        x += ui_text_w(ui, tmp, LT_FONT_MONO, LT_UI_FS_BASE);
        s += k;
        len -= k;
    }
}

int lt_ui_text_multi(lt_ui *ui, const char *label, lt_ui_multi *m, char *buf, size_t cap, int flags,
                     lt_ui_classify_fn classify, void *user)
{
    const lt_theme *t = &ui->theme;
    int ro = (flags & LT_UI_MULTI_READONLY) != 0;
    lt_u32 fid;
    lt_rect view, body;
    int lh, cw, w100, gutter, nlines = 1, maxcol = 0, focused, changed = 0, page;
    int first, last, i, caret_line = 0, ir = 0;
    size_t n, p, ls;
    unsigned char *cls = NULL;
    size_t clen;

    lt_ui_push_id(ui, label);
    fid = lt_ui_id(ui, "##multifocus");
    if (!lt_ui_scroll_begin(ui, "##scroll")) {
        lt_ui_pop_id(ui);
        return 0;
    }
    view = lt_ui_scroll_view(ui);
    body = lt_ui_layout_body(ui);
    lh = ui_text_box_h(ui);
    /* the advance of a monospaced column is fractional: measure 100 of them and scale */
    {
        char hundred[101];
        memset(hundred, 'M', 100);
        hundred[100] = '\0';
        w100 = ui->cell ? 800 : ui_text_w(ui, hundred, LT_FONT_MONO, LT_UI_FS_BASE);
        if (w100 < 100) { w100 = 800; }
        cw = (w100 + 50) / 100;
    }
    if (lh < 1) { lh = 16; }
    page = view.h / lh - 1;
    if (page < 1) { page = 1; }
    focused = ui->focus == fid && !ui->disabled;

    /* mouse (before the keys so a click moves the caret first) */
    n = strlen(buf);
    if (m->cursor > (int)n) { m->cursor = (int)n; }
    if (m->anchor > (int)n) { m->anchor = (int)n; }
    {
        int gut_digits = 1, tmp_lines = 1;
        size_t k;
        for (k = 0; k < n; k++) { if (buf[k] == '\n') { tmp_lines++; } }
        while (tmp_lines >= 10) { tmp_lines /= 10; gut_digits++; }
        gutter = (flags & LT_UI_MULTI_LINENUMS) ? (gut_digits + 1) * cw + t->pad_x : 0;
    }
    {
        lt_rect hit = view;
        hit.w -= t->scrollbar_w;
        hit.h -= t->scrollbar_w;
        if (hit.w < 1) { hit.w = view.w; }
        if (hit.h < 1) { hit.h = view.h; }
        ir = ui_interact(ui, fid, hit, 0);
    }
    if (ir & UI_PRESS) { ui->focus = fid; ui->kbd_nav = 0; focused = !ui->disabled; }
    if (((ir & UI_PRESS) || ((ir & UI_ACTIVE) && m->dragging)) && !ui->disabled) {
        int row = (ui->my - body.y) / lh, col, ln = 0;
        size_t cl = 0;
        if (ui->my < body.y) { row = -1; }
        if (row < 0) { row = 0; }
        while (ln < row) {
            size_t e = ln_end(buf, cl);
            if (buf[e] == '\0') { break; }
            cl = e + 1;
            ln++;
        }
        col = ((ui->mx - (body.x + gutter)) * 100 + w100 / 2) / w100;
        if (ui->mx < body.x + gutter) { col = 0; }
        m->cursor = (int)pos_at(buf, cl, ln_end(buf, cl), col < 0 ? 0 : col);
        if (ir & UI_PRESS) {
            if (!(ui->mods & LT_MOD_SHIFT)) { m->anchor = m->cursor; }
            if (ir & UI_DBLCLICK) {
                size_t lo = (size_t)m->cursor, hi = (size_t)m->cursor;
                while (lo > 0 && is_word_ch(buf[lo - 1])) { lo--; }
                while (buf[hi] != '\0' && is_word_ch(buf[hi])) { hi++; }
                m->anchor = (int)lo;
                m->cursor = (int)hi;
            }
            m->dragging = 1;
        }
        m->want_col = -1;
        m->last_kind = 0;
        m->follow = 1;
        m->blink0 = ui->now;
    } else if (!(ir & UI_ACTIVE)) {
        m->dragging = 0;
    }
    if (focused) { changed = multi_keys(ui, m, buf, cap, flags, page); }
    ui_focusable(ui, fid, UI_K_TEXT);   /* after the keys: Tab belongs to the editor while it is focused */
    focused = ui->focus == fid && !ui->disabled;
    if (ui->focus != fid) { m->dragging = 0; }

    /* geometry: lines and the widest one */
    n = strlen(buf);
    if (m->cursor > (int)n) { m->cursor = (int)n; }
    if (m->anchor > (int)n) { m->anchor = (int)n; }
    ls = 0;
    for (p = 0;; p++) {
        if (buf[p] == '\n' || buf[p] == '\0') {
            int w = col_at(buf, ls, p);
            if (w > maxcol) { maxcol = w; }
            if (ls <= (size_t)m->cursor && (size_t)m->cursor <= p) { caret_line = nlines - 1; }
            if (buf[p] == '\0') { break; }
            ls = p + 1;
            nlines++;
        }
    }
    lt_ui_scroll_set_content(ui, gutter + COLX(maxcol + 3) + 2 * t->pad_x, nlines * lh + (ro ? 0 : lh));
    body = lt_ui_layout_body(ui);

    /* keep the caret visible after navigation or editing */
    if (m->follow && (focused || ro)) {
        size_t cl = ln_start(buf, (size_t)m->cursor);
        int cx = body.x + gutter + COLX(col_at(buf, cl, (size_t)m->cursor));
        lt_ui_scroll_ensure_visible(ui, lt_rect_make(cx - cw, body.y + caret_line * lh, 3 * cw, lh));
        body = lt_ui_layout_body(ui);
    }
    m->follow = 0;

    /* classification of the whole text (strings / comments may span lines) */
    clen = n;
    if (classify != NULL && n > 0) {
        cls = (unsigned char *)lt_calloc(n + 1, 1);
        classify(buf, n, cls, user);
    }

    lt_ui_draw_fill(ui, view, ro ? t->surface : t->input);
    first = (view.y - body.y) / lh;
    if (first < 0) { first = 0; }
    last = (view.y + view.h - body.y) / lh;
    if (last >= nlines) { last = nlines - 1; }
    /* find the start of line `first` */
    ls = 0;
    for (i = 0; i < first; i++) {
        size_t e = ln_end(buf, ls);
        if (buf[e] == '\0') { break; }
        ls = e + 1;
    }
    for (i = first; i <= last; i++) {
        size_t le = ln_end(buf, ls), a, b2;
        int y = body.y + i * lh, x0 = body.x + gutter;
        char num[16];
        if (i == caret_line && focused && !ro) {
            lt_ui_draw_fill(ui, lt_rect_make(body.x, y, LT_MAX(body.w, view.w), lh),
                            lt_color_mix(t->input, t->hover, 90));
        }
        /* selection */
        if (sel_hi(m) > sel_lo(m) && (size_t)sel_hi(m) > ls && (size_t)sel_lo(m) <= le) {
            int c0, c1;
            a = (size_t)sel_lo(m) > ls ? (size_t)sel_lo(m) : ls;
            b2 = (size_t)sel_hi(m) < le ? (size_t)sel_hi(m) : le;
            c0 = col_at(buf, ls, a);
            c1 = col_at(buf, ls, b2);
            if ((size_t)sel_hi(m) > le) { c1++; }
            lt_ui_draw_fill(ui, lt_rect_make(x0 + COLX(c0), y, COLX(c1) - COLX(c0), lh),
                            ui->cell ? t->primary_active : t->selected);
        }
        /* text, in runs of one colour */
        {
            size_t k = ls;
            int col = 0;
            while (k < le) {
                size_t e;
                int cc;
                if (buf[k] == '\t') { col = (col / MU_TAB + 1) * MU_TAB; k++; continue; }
                cc = cls != NULL ? cls[k] : 0;
                e = k;
                while (e < le && buf[e] != '\t' && (cls != NULL ? cls[e] : 0) == cc) { e++; }
                draw_run(ui, x0 + COLX(col), y, buf + k, e - k, ui->disabled ? t->disabled : class_color(t, cc));
                {
                    size_t q2 = k;
                    while (q2 < e) { col++; q2 = next_cp(buf, q2); }
                }
                k = e;
            }
        }
        if (gutter > 0) {
            lt_ui_draw_fill(ui, lt_rect_make(view.x, y, gutter - 2, lh), t->surface);
            lt_snprintf(num, sizeof(num), "%d", i + 1);
            lt_ui_draw_text_in_f(ui, lt_rect_make(view.x, y, gutter - t->pad_x / 2, lh), num,
                                 i == caret_line ? t->text : t->muted, LT_UI_ALIGN_RIGHT, LT_FONT_MONO,
                                 LT_UI_FS_BASE);
        }
        /* caret */
        if (focused && !ro && i == caret_line) {
            double ph = ui->now - m->blink0;
            int on, cx;
            while (ph >= 1000.0) { ph -= 1000.0; }
            if (ph < 0.0) { ph = 0.0; }
            on = ph < 530.0;
            ui_animate(ui, (int)(on ? 530.0 - ph : 1000.0 - ph) + 1);
            cx = x0 + COLX(col_at(buf, ls, (size_t)m->cursor));
            if (on && sel_hi(m) == sel_lo(m)) {
                if (ui->cell) {
                    char ch[8];
                    size_t cp = (size_t)m->cursor, ce = next_cp(buf, cp), k2 = ce - cp;
                    lt_ui_draw_fill(ui, lt_rect_make(cx, y, 8, 16), t->text);
                    if (buf[cp] != '\0' && buf[cp] != '\n' && buf[cp] != '\t') {
                        if (k2 > 7) { k2 = 7; }
                        memcpy(ch, buf + cp, k2);
                        ch[k2] = '\0';
                        lt_ui_draw_text(ui, cx, y, ch, t->input);
                    }
                } else {
                    lt_ui_draw_fill(ui, lt_rect_make(cx, y + 1, lt_ui_px(ui, 2), lh - 2), t->text);
                }
            }
        }
        if (buf[le] == '\0') { break; }
        ls = le + 1;
    }
    lt_free(cls);
    LT_UNUSED(clen);
    lt_ui_scroll_end(ui);
    if (!ui->cell) { lt_ui_draw_rect(ui, view, focused ? t->focus : t->border_subtle); }
    lt_ui_pop_id(ui);
    ui_set_last(ui, fid, view);
    return changed ? LT_UI_CHANGED : 0;
}
