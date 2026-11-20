/* be_tty.c - terminal (ANSI/VT100) cell-mode backend.
 * Uses only lt_term_* from plat.h plus standard C.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include "gui.h"
#include "../plat/plat.h"

#define TTY_CELL_W 8
#define TTY_CELL_H 16
#define TTY_INBUF 512
#define TTY_QCAP 64
#define TTY_ESC_MS 30
#define TTY_SLICE_MS 100

#define TTY_D_MONO 1
#define TTY_D_16 16
#define TTY_D_256 256
#define TTY_D_TRUE 24

/* charset levels (tty.utf8): 0 ASCII only, 1 full Unicode, 2 basic Unicode (the
 * Linux console: box drawing, half/full blocks, shades and CP437 symbols only) */
#define TTY_CS_ASCII 0
#define TTY_CS_FULL 1
#define TTY_CS_BASIC 2

typedef struct tty_out {
    char *p;
    size_t len, cap;
} tty_out;

typedef struct tty {
    int cols, rows;
    int depth;
    int utf8;
    int sync;          /* emit synchronized-output (mode 2026) brackets */
    int opened;
    int eof_sent;
    lt_cell *prev;
    int pcols, prows;
    int have_prev;
    /* current terminal style state */
    int cur_valid;
    lt_u32 cur_fg, cur_bg;
    int cur_attr;
    tty_out out;
    unsigned char in[TTY_INBUF];
    int inlen;
    lt_event q[TTY_QCAP];
    int qh, qn;
} tty;

/* ------------------------------------------------------------------ */
/* output buffer */
static void ob_reserve(tty_out *o, size_t extra)
{
    size_t need = o->len + extra;
    if (need > o->cap) {
        size_t nc = o->cap ? o->cap : 256;
        while (nc < need) {
            nc *= 2;
        }
        o->p = (char *)lt_realloc(o->p, nc);
        o->cap = nc;
    }
}

static void ob_puts(tty_out *o, const char *s)
{
    size_t n = strlen(s);
    ob_reserve(o, n);
    memcpy(o->p + o->len, s, n);
    o->len += n;
}

static void ob_putc(tty_out *o, int c)
{
    ob_reserve(o, 1);
    o->p[o->len++] = (char)c;
}

static void ob_num(tty_out *o, unsigned long v)
{
    char tmp[24];
    int n = 0;
    if (v == 0) {
        ob_putc(o, '0');
        return;
    }
    while (v > 0 && n < 23) {
        tmp[n++] = (char)('0' + (int)(v % 10));
        v /= 10;
    }
    while (n > 0) {
        ob_putc(o, tmp[--n]);
    }
}

static void ob_utf8(tty_out *o, lt_u32 cp)
{
    if (cp < 0x80) {
        ob_putc(o, (int)cp);
    } else if (cp < 0x800) {
        ob_putc(o, (int)(0xC0 | (cp >> 6)));
        ob_putc(o, (int)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000UL) {
        ob_putc(o, (int)(0xE0 | (cp >> 12)));
        ob_putc(o, (int)(0x80 | ((cp >> 6) & 0x3F)));
        ob_putc(o, (int)(0x80 | (cp & 0x3F)));
    } else {
        ob_putc(o, (int)(0xF0 | ((cp >> 18) & 0x07)));
        ob_putc(o, (int)(0x80 | ((cp >> 12) & 0x3F)));
        ob_putc(o, (int)(0x80 | ((cp >> 6) & 0x3F)));
        ob_putc(o, (int)(0x80 | (cp & 0x3F)));
    }
}

/* ------------------------------------------------------------------ */
/* environment detection (pure, testable) */
static int str_has_i(const char *s, const char *sub)
{
    size_t n = strlen(sub), i, j;
    if (s == NULL) {
        return 0;
    }
    for (i = 0; s[i] != '\0'; i++) {
        for (j = 0; j < n; j++) {
            int a = (unsigned char)s[i + j];
            int b = (unsigned char)sub[j];
            if (a == 0) {
                return 0;
            }
            if (a >= 'A' && a <= 'Z') {
                a += 32;
            }
            if (b >= 'A' && b <= 'Z') {
                b += 32;
            }
            if (a != b) {
                break;
            }
        }
        if (j == n) {
            return 1;
        }
    }
    return 0;
}

static int tty_detect_depth(const char *colorterm, const char *term)
{
    if (colorterm != NULL && (str_has_i(colorterm, "truecolor") || str_has_i(colorterm, "24bit"))) {
        return TTY_D_TRUE;
    }
    if (term != NULL) {
        if (str_has_i(term, "truecolor") || str_has_i(term, "direct")) {
            return TTY_D_TRUE;
        }
        if (str_has_i(term, "256color")) {
            return TTY_D_256;
        }
        if (strcmp(term, "dumb") == 0 || strcmp(term, "vt100") == 0 || strcmp(term, "vt52") == 0) {
            return TTY_D_MONO;
        }
    }
    return TTY_D_16;
}

static int tty_detect_utf8(const char *lc_all, const char *lc_ctype, const char *lang)
{
    const char *v = NULL;
    if (lc_all != NULL && lc_all[0] != '\0') {
        v = lc_all;
    } else if (lc_ctype != NULL && lc_ctype[0] != '\0') {
        v = lc_ctype;
    } else if (lang != NULL && lang[0] != '\0') {
        v = lang;
    }
    if (v == NULL) {
        return 1; /* nothing set: modern terminals (and Windows) are UTF-8 */
    }
    return str_has_i(v, "utf-8") || str_has_i(v, "utf8");
}

/* TERM=linux (the kernel console) has a 256/512 glyph font: no rounded corners,
 * no partial blocks, no braille */
static int tty_detect_charset(const char *lc_all, const char *lc_ctype, const char *lang,
                              const char *term)
{
    if (!tty_detect_utf8(lc_all, lc_ctype, lang)) {
        return TTY_CS_ASCII;
    }
    if (term != NULL && (strcmp(term, "linux") == 0 || strcmp(term, "vt220") == 0 ||
                         strcmp(term, "vt100") == 0 || strcmp(term, "dumb") == 0)) {
        return (strcmp(term, "linux") == 0) ? TTY_CS_BASIC : TTY_CS_ASCII;
    }
    return TTY_CS_FULL;
}

/* ------------------------------------------------------------------ */
/* color quantizers */
static int sq(int a)
{
    return a * a;
}

static int q_abs(int a)
{
    return a < 0 ? -a : a;
}

static int tty_rgb_to_256(int r, int g, int b)
{
    static const int lv[6] = { 0, 95, 135, 175, 215, 255 };
    int idx[3], v[3], k, i, best, bd, d, cd, gd, avg, gi, gl;
    v[0] = r;
    v[1] = g;
    v[2] = b;
    for (k = 0; k < 3; k++) {
        best = 0;
        bd = 1000;
        for (i = 0; i < 6; i++) {
            d = q_abs(v[k] - lv[i]);
            if (d < bd) {
                bd = d;
                best = i;
            }
        }
        idx[k] = best;
    }
    cd = sq(r - lv[idx[0]]) + sq(g - lv[idx[1]]) + sq(b - lv[idx[2]]);
    avg = (r + g + b) / 3;
    gi = (avg - 3) / 10;
    if (gi < 0) {
        gi = 0;
    }
    if (gi > 23) {
        gi = 23;
    }
    gl = 8 + 10 * gi;
    gd = sq(r - gl) + sq(g - gl) + sq(b - gl);
    if (gd < cd) {
        return 232 + gi;
    }
    return 16 + 36 * idx[0] + 6 * idx[1] + idx[2];
}

/* nlim = 16 for foreground, 8 for background (bright backgrounds are not
 * portable: the Linux console and many terminals only have 8) */
static int tty_rgb_to_n(int r, int g, int b, int nlim)
{
    static const int pal[16][3] = {
        { 0, 0, 0 }, { 170, 0, 0 }, { 0, 170, 0 }, { 170, 85, 0 },
        { 0, 0, 170 }, { 170, 0, 170 }, { 0, 170, 170 }, { 170, 170, 170 },
        { 85, 85, 85 }, { 255, 85, 85 }, { 85, 255, 85 }, { 255, 255, 85 },
        { 85, 85, 255 }, { 255, 85, 255 }, { 85, 255, 255 }, { 255, 255, 255 }
    };
    int i, best = 0, bd = 0x7FFFFFF, d, rm, dr, dg, db;
    for (i = 0; i < nlim; i++) {
        rm = (r + pal[i][0]) / 2;
        dr = r - pal[i][0];
        dg = g - pal[i][1];
        db = b - pal[i][2];
        /* "redmean" weighted distance: closer to perception than plain RGB */
        d = ((512 + rm) * dr * dr) / 256 + 4 * dg * dg + ((767 - rm) * db * db) / 256;
        if (d < bd) {
            bd = d;
            best = i;
        }
    }
    return best;
}

static int tty_luma(lt_color c)
{
    return (LT_COLOR_R(c) * 299 + LT_COLOR_G(c) * 587 + LT_COLOR_B(c) * 114) / 1000;
}

/* quantize a color to the code that is emitted at this depth */
static lt_u32 tty_quant(int depth, lt_color c, int is_bg)
{
    int r = LT_COLOR_R(c), g = LT_COLOR_G(c), b = LT_COLOR_B(c);
    if (depth == TTY_D_TRUE) {
        return (lt_u32)(c & 0xFFFFFFUL);
    }
    if (depth == TTY_D_256) {
        return (lt_u32)tty_rgb_to_256(r, g, b);
    }
    if (depth == TTY_D_16) {
        return (lt_u32)tty_rgb_to_n(r, g, b, is_bg ? 8 : 16);
    }
    return 0;
}

/* monochrome: cells whose background is a "highlight" (neither the base dark
 * nor a very light surface) are drawn reversed so selections stay visible */
static int tty_mono_reverse(const lt_cell *c)
{
    int l = tty_luma(c->bg);
    return (l >= 50 && l < 170) ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/* glyph mapping */
/* partial-block glyphs: how many eighths are filled (0 = not a partial block) */
static int tty_block_eighths(lt_u32 cp)
{
    if (cp >= 0x2581UL && cp <= 0x2587UL) {
        return (int)(cp - 0x2580UL);
    }
    if (cp >= 0x2589UL && cp <= 0x258FUL) {
        return (int)(0x2590UL - cp);
    }
    return 0;
}

static lt_u32 tty_ascii_fallback(lt_u32 cp)
{
    int e;
    if (cp >= 0x2500 && cp <= 0x257F) {
        switch (cp) {
        case 0x2500: case 0x2501: case 0x2504: case 0x2505: case 0x2508: case 0x2509:
        case 0x254C: case 0x254D: case 0x2550: case 0x2574: case 0x2576: case 0x2578:
        case 0x257A: case 0x257C: case 0x257E:
            return '-';
        case 0x2502: case 0x2503: case 0x2506: case 0x2507: case 0x250A: case 0x250B:
        case 0x254E: case 0x254F: case 0x2551: case 0x2575: case 0x2577: case 0x2579:
        case 0x257B: case 0x257D: case 0x257F:
            return '|';
        case 0x2571: return '/';
        case 0x2572: return '\\';
        case 0x2573: return 'X';
        default:
            return '+';
        }
    }
    if (cp >= 0x2580 && cp <= 0x259F) {
        if (cp == 0x2591) {
            return '.';
        }
        if (cp == 0x2592) {
            return ':';
        }
        e = tty_block_eighths(cp);
        if (e > 0) {
            return e <= 2 ? '.' : (e <= 5 ? ':' : '#');
        }
        if (cp == 0x2580) {
            return '"';
        }
        if (cp == 0x2594) {
            return '^';
        }
        if (cp == 0x2595) {
            return '|';
        }
        return '#';
    }
    if (cp >= 0x2800 && cp <= 0x28FF) {
        return cp == 0x2800 ? ' ' : '*';
    }
    switch (cp) {
    case 0xB0: return 'o';
    case 0xB7: case 0x2026: return '.';
    case 0x2022: case 0x25CF: case 0x25CB: case 0x25C9: case 0x2605: case 0x2606: return '*';
    case 0x2212: return '-';
    case 0x2190: case 0x25C4: case 0x25C0: case 0x25C2: case 0x25C1: return '<';
    case 0x2191: case 0x25B2: case 0x25B4: return '^';
    case 0x2192: case 0x25BA: case 0x25B6: case 0x25B8: case 0x25B7: return '>';
    case 0x2193: case 0x25BC: case 0x25BE: return 'v';
    case 0x2713: case 0x2714: return 'x';
    case 0x2717: case 0x2718: case 0xD7: return 'x';
    case 0x2264: return '<';
    case 0x2265: return '>';
    case 0x25A0: case 0x25AA: return '#';
    default: break;
    }
    if (cp >= 0xC0 && cp <= 0xFF) {
        return '?';
    }
    return '?';
}

/* what the Linux console can show: box drawing (no rounded corners), full/half
 * blocks and shades, a few CP437 symbols */
static lt_u32 tty_basic_fallback(lt_u32 cp)
{
    int e;
    if (cp >= 0x256D && cp <= 0x2570) {
        static const lt_u32 sq[4] = { 0x250C, 0x2510, 0x2518, 0x2514 };
        return sq[cp - 0x256D];
    }
    if (cp >= 0x2571 && cp <= 0x2573) {
        return cp == 0x2571 ? '/' : (cp == 0x2572 ? '\\' : 'X');
    }
    if (cp >= 0x2574 && cp <= 0x257F) {
        return (cp & 1) ? 0x2502 : 0x2500;
    }
    if (cp >= 0x2504 && cp <= 0x250B) {
        return (cp & 2) ? 0x2502 : 0x2500;
    }
    if (cp >= 0x254C && cp <= 0x254F) {
        return (cp & 2) ? 0x2502 : 0x2500;
    }
    if (cp >= 0x2500 && cp <= 0x257F) {
        return cp;      /* the console has all single/double combos it needs */
    }
    if (cp >= 0x2580 && cp <= 0x259F) {
        e = tty_block_eighths(cp);
        if (cp >= 0x2581 && cp <= 0x2587) {
            return e <= 2 ? '_' : (e <= 5 ? 0x2584 : 0x2588);
        }
        if (cp >= 0x2589 && cp <= 0x258F) {
            return e <= 2 ? ' ' : (e <= 5 ? 0x258C : 0x2588);
        }
        switch (cp) {
        case 0x2594: return 0x2580;
        case 0x2595: return 0x2590;
        case 0x2596: case 0x2599: return 0x258C;
        case 0x2597: case 0x259F: return 0x2590;
        case 0x2598: case 0x259B: return 0x2580;
        case 0x259A: case 0x259C: case 0x259D: case 0x259E: return 0x2588;
        default: return cp;
        }
    }
    if (cp >= 0x2800 && cp <= 0x28FF) {
        return cp == 0x2800 ? ' ' : 0xB7;
    }
    switch (cp) {
    case 0x25B6: case 0x25B8: case 0x25B7: return 0x25BA;
    case 0x25C0: case 0x25C2: case 0x25C1: return 0x25C4;
    case 0x25BE: return 0x25BC;
    case 0x25B4: return 0x25B2;
    case 0x25CF: case 0x25C9: case 0x2605: case 0x2606: return 0x2022;
    case 0x2713: case 0x2714: return 0x221A;
    case 0x2717: case 0x2718: return 'x';
    case 0x2026: return '.';
    case 0x2212: return '-';
    case 0x2022: case 0xB0: case 0xB7: case 0x2190: case 0x2191: case 0x2192: case 0x2193:
    case 0x25B2: case 0x25BA: case 0x25BC: case 0x25C4: case 0x2264: case 0x2265:
    case 0x25A0: case 0x25CB:
        return cp;
    default: break;
    }
    if (cp < 0x250 && cp >= 0xA0) {
        return cp;      /* Latin-1 / Latin Extended: the console font covers it */
    }
    return tty_ascii_fallback(cp);
}

static lt_u32 tty_glyph(const tty *t, lt_u32 cp)
{
    if (cp < 0x20 || cp == 0x7F || (cp >= 0x80 && cp < 0xA0)) {
        return ' ';
    }
    if (cp > 0x10FFFFUL || (cp >= 0xD800UL && cp <= 0xDFFFUL)) {
        return '?';
    }
    if (t->utf8 == TTY_CS_ASCII && cp > 0x7E) {
        return tty_ascii_fallback(cp);
    }
    if (t->utf8 == TTY_CS_BASIC && cp > 0x7E) {
        return tty_basic_fallback(cp);
    }
    return cp;
}

/* ------------------------------------------------------------------ */
/* frame rendering */
static int tty_cell_eq(const lt_cell *a, const lt_cell *b)
{
    return a->ch == b->ch && a->fg == b->fg && a->bg == b->bg && a->attr == b->attr;
}

static void tty_emit_color(tty *t, tty_out *o, int bg, lt_u32 code)
{
    if (t->depth == TTY_D_TRUE) {
        ob_puts(o, bg ? ";48;2;" : ";38;2;");
        ob_num(o, (code >> 16) & 0xFF);
        ob_putc(o, ';');
        ob_num(o, (code >> 8) & 0xFF);
        ob_putc(o, ';');
        ob_num(o, code & 0xFF);
    } else if (t->depth == TTY_D_256) {
        ob_puts(o, bg ? ";48;5;" : ";38;5;");
        ob_num(o, code);
    } else if (t->depth == TTY_D_16) {
        ob_putc(o, ';');
        if (code < 8) {
            ob_num(o, (bg ? 40UL : 30UL) + code);
        } else {
            ob_num(o, (bg ? 100UL : 90UL) + (code - 8));
        }
    }
}

static void tty_set_style(tty *t, tty_out *o, const lt_cell *c)
{
    lt_u32 fg = 0, bg = 0;
    int attr = c->attr & 7;
    if (t->depth == TTY_D_MONO) {
        if (tty_mono_reverse(c)) {
            attr ^= LT_CELL_REVERSE;
        }
    } else {
        fg = tty_quant(t->depth, c->fg, 0);
        bg = tty_quant(t->depth, c->bg, 1);
    }
    if (t->cur_valid && t->cur_fg == fg && t->cur_bg == bg && t->cur_attr == attr) {
        return;
    }
    ob_puts(o, "\033[0");
    if (attr & LT_CELL_BOLD) {
        ob_puts(o, ";1");
    }
    if (attr & LT_CELL_UNDERLINE) {
        ob_puts(o, ";4");
    }
    if (attr & LT_CELL_REVERSE) {
        ob_puts(o, ";7");
    }
    if (t->depth != TTY_D_MONO) {
        tty_emit_color(t, o, 0, fg);
        tty_emit_color(t, o, 1, bg);
    }
    ob_putc(o, 'm');
    t->cur_valid = 1;
    t->cur_fg = fg;
    t->cur_bg = bg;
    t->cur_attr = attr;
}

static void tty_free_bufs(tty *t);

/* Build the escape stream for one frame into t->out (does not write). */
static void tty_render(tty *t, const lt_cells *g)
{
    int ncols, nrows, x, y, cx = -1, cy = -1, full = 0;
    size_t total;

    t->out.len = 0;
    if (g == NULL || g->cells == NULL || g->cols <= 0 || g->rows <= 0) {
        return;
    }
    ncols = g->cols;
    nrows = g->rows;
    if (t->cols > 0 && ncols > t->cols) {
        ncols = t->cols;
    }
    if (t->rows > 0 && nrows > t->rows) {
        nrows = t->rows;
    }
    if (t->prev == NULL || t->pcols != g->cols || t->prows != g->rows || !t->have_prev) {
        if (t->prev != NULL) {
            lt_free(t->prev);
        }
        t->pcols = g->cols;
        t->prows = g->rows;
        total = (size_t)g->cols * (size_t)g->rows;
        t->prev = (lt_cell *)lt_calloc(total, sizeof(lt_cell));
        full = 1;
        t->cur_valid = 0;
        ob_puts(&t->out, "\033[0m\033[2J");
    }
    for (y = 0; y < nrows; y++) {
        for (x = 0; x < ncols; x++) {
            const lt_cell *c = &g->cells[(size_t)y * (size_t)g->cols + (size_t)x];
            lt_cell *p = &t->prev[(size_t)y * (size_t)t->pcols + (size_t)x];
            if (!full && tty_cell_eq(c, p)) {
                continue;
            }
            if (cy != y || cx != x) {
                ob_puts(&t->out, "\033[");
                ob_num(&t->out, (unsigned long)(y + 1));
                ob_putc(&t->out, ';');
                ob_num(&t->out, (unsigned long)(x + 1));
                ob_putc(&t->out, 'H');
                cy = y;
                cx = x;
            }
            tty_set_style(t, &t->out, c);
            ob_utf8(&t->out, tty_glyph(t, c->ch));
            *p = *c;
            cx++;
            if (cx >= ncols) {
                cx = -1; /* pending wrap: position unknown */
                cy = -1;
            }
        }
    }
    if (t->sync && t->out.len > 0) {
        /* synchronized output: the terminal shows the frame atomically (no tearing);
         * terminals that do not know mode 2026 ignore the sequence */
        size_t n = t->out.len;
        ob_reserve(&t->out, 16 + 8);
        memmove(t->out.p + 8, t->out.p, n);
        memcpy(t->out.p, "\033[?2026h", 8);
        memcpy(t->out.p + 8 + n, "\033[?2026l", 8);
        t->out.len = n + 16;
    }
    t->have_prev = 1;
}

/* ------------------------------------------------------------------ */
/* previews for the snapshot harness and tests */
static lt_color tty_code_rgb(int depth, lt_u32 code)
{
    static const int pal16[16][3] = {
        { 0, 0, 0 }, { 170, 0, 0 }, { 0, 170, 0 }, { 170, 85, 0 },
        { 0, 0, 170 }, { 170, 0, 170 }, { 0, 170, 170 }, { 170, 170, 170 },
        { 85, 85, 85 }, { 255, 85, 85 }, { 85, 255, 85 }, { 255, 255, 85 },
        { 85, 85, 255 }, { 255, 85, 255 }, { 85, 255, 255 }, { 255, 255, 255 }
    };
    static const int lv[6] = { 0, 95, 135, 175, 215, 255 };
    int c = (int)code;
    if (depth == TTY_D_TRUE) {
        return (lt_color)(0xFF000000UL | code);
    }
    if (depth == TTY_D_16 || (depth == TTY_D_256 && c < 16)) {
        return LT_RGB(pal16[c & 15][0], pal16[c & 15][1], pal16[c & 15][2]);
    }
    if (c >= 232) {
        int v = 8 + 10 * (c - 232);
        return LT_RGB(v, v, v);
    }
    c -= 16;
    return LT_RGB(lv[(c / 36) % 6], lv[(c / 6) % 6], lv[c % 6]);
}

void lt_tty_apply_mode(lt_cells *g, int depth, int utf8)
{
    int i, n;
    if (g == NULL || g->cells == NULL) {
        return;
    }
    if (depth != TTY_D_TRUE && depth != TTY_D_256 && depth != TTY_D_16) {
        depth = TTY_D_MONO;
    }
    n = g->cols * g->rows;
    for (i = 0; i < n; i++) {
        lt_cell *c = &g->cells[i];
        if (depth == TTY_D_MONO) {
            int rev = tty_mono_reverse(c);
            c->fg = LT_RGB(230, 230, 230);
            c->bg = LT_RGB(0, 0, 0);
            if (rev || (c->attr & LT_CELL_REVERSE)) {
                c->attr = (lt_u8)(c->attr ^ LT_CELL_REVERSE);
            }
        } else {
            c->fg = tty_code_rgb(depth, tty_quant(depth, c->fg, 0));
            c->bg = tty_code_rgb(depth, tty_quant(depth, c->bg, 1));
        }
        if (c->ch < 0x20UL || c->ch == 0x7FUL) {
            c->ch = ' ';
        } else if (utf8 == TTY_CS_ASCII && c->ch > 0x7EUL) {
            c->ch = tty_ascii_fallback(c->ch);
        } else if (utf8 == TTY_CS_BASIC && c->ch > 0x7EUL) {
            c->ch = tty_basic_fallback(c->ch);
        }
    }
}

char *lt_tty_ansi_dump(const lt_cells *g, int depth, int utf8)
{
    tty t;
    char *r;
    memset(&t, 0, sizeof(t));
    t.cols = g->cols;
    t.rows = g->rows;
    t.depth = (depth == TTY_D_TRUE || depth == TTY_D_256 || depth == TTY_D_16) ? depth : TTY_D_MONO;
    t.utf8 = utf8;
    tty_render(&t, g);
    r = (char *)lt_malloc(t.out.len + 1);
    if (t.out.len > 0) {
        memcpy(r, t.out.p, t.out.len);
    }
    r[t.out.len] = '\0';
    tty_free_bufs(&t);
    return r;
}

/* ------------------------------------------------------------------ */
/* input parsing */
static lt_event *q_push(tty *t, lt_ev_type type)
{
    lt_event *e;
    int idx = (t->qh + t->qn) % TTY_QCAP;
    e = &t->q[idx];
    memset(e, 0, sizeof(*e));
    e->type = type;
    t->qn++;
    return e;
}

static void push_key(tty *t, int key, int mods)
{
    lt_event *e = q_push(t, LT_EV_KEY_DOWN);
    e->key = key;
    e->mods = mods;
}

static int utf8_len(unsigned char c)
{
    if (c < 0x80) {
        return 1;
    }
    if (c >= 0xC2 && c <= 0xDF) {
        return 2;
    }
    if (c >= 0xE0 && c <= 0xEF) {
        return 3;
    }
    if (c >= 0xF0 && c <= 0xF4) {
        return 4;
    }
    return 0;
}

/* parse one plain (non-ESC) key; returns bytes consumed, 0 = need more */
static int parse_plain(tty *t, const unsigned char *p, int n, int mods)
{
    unsigned char c = p[0];
    int len, i;
    lt_u32 cp;
    lt_event *e;

    if (c < 0x20 || c == 0x7F) {
        if (c == 0x09) {
            push_key(t, LT_KEY_TAB, mods);
        } else if (c == 0x0D || c == 0x0A) {
            push_key(t, LT_KEY_ENTER, mods);
        } else if (c == 0x08 || c == 0x7F) {
            push_key(t, LT_KEY_BACKSPACE, mods);
        } else if (c == 0x03 && !(mods & LT_MOD_ALT)) {
            q_push(t, LT_EV_QUIT);      /* Ctrl-C: ask the app to quit (it may prompt) */
        } else if (c == 0x00) {
            push_key(t, ' ', mods | LT_MOD_CTRL);
        } else if (c >= 1 && c <= 26) {
            push_key(t, 'a' + (int)c - 1, mods | LT_MOD_CTRL);
        } else if (c >= 0x1C && c <= 0x1F) {
            push_key(t, (int)c + 0x40, mods | LT_MOD_CTRL);
        }
        return 1;
    }
    if (c < 0x80) {
        int key = c, m = mods;
        if (c >= 'A' && c <= 'Z') {
            key = c + 32;
            m |= LT_MOD_SHIFT;
        }
        push_key(t, key, m);
        if (!(mods & LT_MOD_ALT)) {
            e = q_push(t, LT_EV_TEXT);
            e->text[0] = (char)c;
            e->text[1] = '\0';
        }
        return 1;
    }
    len = utf8_len(c);
    if (len == 0) {
        return 1; /* stray byte: skip */
    }
    if (n < len) {
        return 0;
    }
    cp = (lt_u32)(c & (len == 2 ? 0x1F : (len == 3 ? 0x0F : 0x07)));
    for (i = 1; i < len; i++) {
        if ((p[i] & 0xC0) != 0x80) {
            return 1; /* invalid: skip lead byte */
        }
        cp = (cp << 6) | (lt_u32)(p[i] & 0x3F);
    }
    push_key(t, (int)cp, mods);
    if (!(mods & LT_MOD_ALT)) {
        e = q_push(t, LT_EV_TEXT);
        for (i = 0; i < len; i++) {
            e->text[i] = (char)p[i];
        }
        e->text[len] = '\0';
    }
    return len;
}

static int csi_mods(int m)
{
    int r = 0;
    if (m > 0) {
        m -= 1;
    }
    if (m & 1) {
        r |= LT_MOD_SHIFT;
    }
    if (m & 2) {
        r |= LT_MOD_ALT;
    }
    if (m & 4) {
        r |= LT_MOD_CTRL;
    }
    if (m & 8) {
        r |= LT_MOD_ALT;
    }
    return r;
}

static void parse_mouse(tty *t, int b, int x, int y, int release)
{
    lt_event *e;
    int mods = 0, btn = b & 3, px, py;
    if (b & 4) {
        mods |= LT_MOD_SHIFT;
    }
    if (b & 8) {
        mods |= LT_MOD_ALT;
    }
    if (b & 16) {
        mods |= LT_MOD_CTRL;
    }
    px = (x - 1) * TTY_CELL_W + TTY_CELL_W / 2;
    py = (y - 1) * TTY_CELL_H + TTY_CELL_H / 2;
    if (px < 0) {
        px = TTY_CELL_W / 2;
    }
    if (py < 0) {
        py = TTY_CELL_H / 2;
    }
    if (b & 64) {
        if (release) {
            return;
        }
        e = q_push(t, LT_EV_WHEEL);
        e->x = px;
        e->y = py;
        e->mods = mods;
        if (btn == 0) {
            e->wheel = 1;
        } else if (btn == 1) {
            e->wheel = -1;
        } else if (btn == 2) {
            e->wheel = 0;
            e->button = -1; /* horizontal: dx in button */
        } else {
            e->wheel = 0;
            e->button = 1;
        }
        return;
    }
    if (b & 32) {
        e = q_push(t, LT_EV_MOUSE_MOVE);
        e->x = px;
        e->y = py;
        e->mods = mods;
        e->button = (btn == 3) ? 0 : btn + 1;
        return;
    }
    if (btn == 3) {
        return;
    }
    e = q_push(t, release ? LT_EV_MOUSE_UP : LT_EV_MOUSE_DOWN);
    e->x = px;
    e->y = py;
    e->mods = mods;
    e->button = btn + 1;
}

static int fkey_from_tilde(int n)
{
    switch (n) {
    case 1: case 7: return LT_KEY_HOME;
    case 2: return LT_KEY_INSERT;
    case 3: return LT_KEY_DELETE;
    case 4: case 8: return LT_KEY_END;
    case 5: return LT_KEY_PAGEUP;
    case 6: return LT_KEY_PAGEDOWN;
    case 11: case 12: case 13: case 14: case 15: return LT_KEY_F1 + (n - 11);
    case 17: case 18: case 19: case 20: case 21: return LT_KEY_F6 + (n - 17);
    case 23: return LT_KEY_F11;
    case 24: return LT_KEY_F12;
    default: return 0;
    }
}

/* p[0]=ESC p[1]='['. returns consumed or 0 for incomplete */
static int parse_csi(tty *t, const unsigned char *p, int n)
{
    int i = 2, prefix = 0, np = 0, have = 0, f, key = 0, mods, nparams;
    long params[8];
    int k;

    for (k = 0; k < 8; k++) {
        params[k] = 0;
    }
    if (i >= n) {
        return 0;
    }
    if (p[i] == '[') { /* linux console: ESC [ [ A..E = F1..F5 */
        if (n < 4) {
            return 0;
        }
        if (p[3] >= 'A' && p[3] <= 'E') {
            push_key(t, LT_KEY_F1 + (p[3] - 'A'), 0);
        }
        return 4;
    }
    if (p[i] == '<' || p[i] == '=' || p[i] == '>' || p[i] == '?') {
        prefix = p[i];
        i++;
    }
    while (i < n && p[i] >= 0x30 && p[i] <= 0x3F) {
        if (p[i] >= '0' && p[i] <= '9') {
            if (params[np] < 100000L) {
                params[np] = params[np] * 10 + (p[i] - '0');
            }
            have = 1;
        } else if (p[i] == ';') {
            if (np < 7) {
                np++;
            }
        }
        i++;
    }
    while (i < n && p[i] >= 0x20 && p[i] <= 0x2F) {
        i++;
    }
    if (i >= n) {
        return 0;
    }
    f = p[i];
    if (f < 0x40 || f > 0x7E) {
        return i; /* malformed: drop what we saw */
    }
    nparams = (have || np > 0) ? np + 1 : 0;
    mods = csi_mods(nparams >= 2 ? (int)params[1] : 0);

    if (prefix == '<' && (f == 'M' || f == 'm')) {
        if (nparams >= 3) {
            parse_mouse(t, (int)params[0], (int)params[1], (int)params[2], f == 'm');
        }
        return i + 1;
    }
    if (prefix != 0) {
        return i + 1;
    }
    switch (f) {
    case 'A': key = LT_KEY_UP; break;
    case 'B': key = LT_KEY_DOWN; break;
    case 'C': key = LT_KEY_RIGHT; break;
    case 'D': key = LT_KEY_LEFT; break;
    case 'H': key = LT_KEY_HOME; break;
    case 'F': key = LT_KEY_END; break;
    case 'P': key = LT_KEY_F1; break;
    case 'Q': key = LT_KEY_F2; break;
    case 'R': key = LT_KEY_F3; break;
    case 'S': key = LT_KEY_F4; break;
    case 'Z':
        push_key(t, LT_KEY_TAB, LT_MOD_SHIFT);
        return i + 1;
    case '~':
        key = fkey_from_tilde(nparams >= 1 ? (int)params[0] : 0);
        break;
    case 'I':
        return i + 1;
    case 'O':
        q_push(t, LT_EV_FOCUS_LOST);
        return i + 1;
    default:
        break;
    }
    if (key != 0) {
        push_key(t, key, mods);
    }
    return i + 1;
}

static int parse_ss3(tty *t, const unsigned char *p, int n)
{
    int key = 0;
    if (n < 3) {
        return 0;
    }
    switch (p[2]) {
    case 'A': key = LT_KEY_UP; break;
    case 'B': key = LT_KEY_DOWN; break;
    case 'C': key = LT_KEY_RIGHT; break;
    case 'D': key = LT_KEY_LEFT; break;
    case 'H': key = LT_KEY_HOME; break;
    case 'F': key = LT_KEY_END; break;
    case 'P': key = LT_KEY_F1; break;
    case 'Q': key = LT_KEY_F2; break;
    case 'R': key = LT_KEY_F3; break;
    case 'S': key = LT_KEY_F4; break;
    case 'M': key = LT_KEY_ENTER; break;
    default: break;
    }
    if (key != 0) {
        push_key(t, key, 0);
    }
    return 3;
}

/* returns consumed (>0) or 0 if more bytes are needed */
static int parse_one(tty *t, const unsigned char *p, int n, int flush)
{
    int r;
    if (p[0] != 0x1B) {
        r = parse_plain(t, p, n, 0);
        if (r == 0 && flush) {
            return n;
        }
        return r;
    }
    if (n == 1) {
        if (flush) {
            push_key(t, LT_KEY_ESCAPE, 0);
            return 1;
        }
        return 0;
    }
    if (p[1] == '[') {
        r = parse_csi(t, p, n);
    } else if (p[1] == 'O') {
        r = parse_ss3(t, p, n);
    } else if (p[1] == 0x1B) {
        push_key(t, LT_KEY_ESCAPE, 0);
        return 1;
    } else {
        r = parse_plain(t, p + 1, n - 1, LT_MOD_ALT);
        if (r > 0) {
            r += 1;
        }
    }
    if (r == 0 && flush) {
        push_key(t, LT_KEY_ESCAPE, 0);
        return 1;
    }
    return r;
}

/* parse as much buffered input as possible into the event queue */
static void tty_feed(tty *t, int flush)
{
    while (t->inlen > 0 && t->qn < TTY_QCAP - 4) {
        int r = parse_one(t, t->in, t->inlen, flush);
        if (r <= 0) {
            break;
        }
        if (r > t->inlen) {
            r = t->inlen;
        }
        memmove(t->in, t->in + r, (size_t)(t->inlen - r));
        t->inlen -= r;
    }
}

/* ------------------------------------------------------------------ */
/* terminal setup / restore */
static tty *g_active = NULL;
static int g_hooks = 0;

static const char tty_seq_open[] =
    "\033[?1049h\033[?25l\033[?2004l\033[?1000h\033[?1002h\033[?1003h\033[?1006h\033[?1004h"
    "\033[0m\033[2J";
static const char tty_seq_close[] =
    "\033[0m\033[?2026l\033[?1004l\033[?1006l\033[?1003l\033[?1002l\033[?1000l\033[?25h"
    "\033[?1049l\033[0m";

static void tty_restore(void)
{
    tty *t = g_active;
    if (t == NULL || !t->opened) {
        return;
    }
    t->opened = 0;
    g_active = NULL;
    lt_term_write(tty_seq_close, (int)(sizeof(tty_seq_close) - 1));
    lt_term_raw_end();
}

#ifdef SIGTERM
static void tty_on_signal(int sig)
{
    tty_restore();
    signal(sig, SIG_DFL);
    raise(sig);
}
#endif

static void tty_install_hooks(void)
{
    if (g_hooks) {
        return;
    }
    g_hooks = 1;
    atexit(tty_restore);
#ifdef SIGTERM
    if (signal(SIGTERM, tty_on_signal) != SIG_DFL) {
        /* someone else owns it: leave theirs in place */
    }
#endif
#ifdef SIGHUP
    signal(SIGHUP, tty_on_signal);
#endif
#ifdef SIGINT
    signal(SIGINT, tty_on_signal);
#endif
#ifdef SIGQUIT
    signal(SIGQUIT, tty_on_signal);
#endif
#ifdef SIGSEGV
    signal(SIGSEGV, tty_on_signal);
#endif
#ifdef SIGABRT
    signal(SIGABRT, tty_on_signal);
#endif
}

static void tty_query_size(tty *t)
{
    int c = 0, r = 0;
    lt_term_size(&c, &r);
    if (c <= 0) {
        c = 80;
    }
    if (r <= 0) {
        r = 24;
    }
    t->cols = c;
    t->rows = r;
}

/* ------------------------------------------------------------------ */
/* backend methods */
static void tty_free_bufs(tty *t)
{
    if (t->prev != NULL) {
        lt_free(t->prev);
        t->prev = NULL;
    }
    if (t->out.p != NULL) {
        lt_free(t->out.p);
        t->out.p = NULL;
    }
    t->out.len = t->out.cap = 0;
    t->have_prev = 0;
}

/* BEL: the terminal rings (or flashes, per its settings) */
static void tty_beep(lt_backend *b)
{
    tty *t = (tty *)b->impl;
    static const char bel[1] = { 7 };
    if (t == NULL || !t->opened) {
        return;
    }
    lt_term_write(bel, 1);
}

static void tty_set_title(lt_backend *b, const char *title)
{
    tty *t = (tty *)b->impl;
    tty_out o;
    const char *s;
    if (t == NULL || !t->opened || title == NULL) {
        return;
    }
    o.p = NULL;
    o.len = o.cap = 0;
    ob_puts(&o, "\033]0;");
    for (s = title; *s != '\0'; s++) {
        if ((unsigned char)*s >= 0x20 && *s != 0x7F) {
            ob_putc(&o, *s);
        }
    }
    ob_putc(&o, 7);
    lt_term_write(o.p, (int)o.len);
    lt_free(o.p);
}

static int tty_open(lt_backend *b, const char *title, int w, int h)
{
    tty *t = (tty *)b->impl;
    const char *ct, *tm;
    LT_UNUSED(w);
    LT_UNUSED(h);
    if (t->opened) {
        return 0;
    }
    ct = getenv("COLORTERM");
    tm = getenv("TERM");
    t->depth = tty_detect_depth(ct, tm);
    t->utf8 = tty_detect_charset(getenv("LC_ALL"), getenv("LC_CTYPE"), getenv("LANG"), tm);
    if (lt_term_raw_begin() != 0) {
        lt_err_set("tty: cannot enter raw mode");
        return -1;
    }
    t->opened = 1;
    t->inlen = 0;
    t->qh = t->qn = 0;
    t->eof_sent = 0;
    t->cur_valid = 0;
    t->have_prev = 0;
    t->sync = (t->depth != TTY_D_MONO && (tm == NULL || strcmp(tm, "linux") != 0)) ? 1 : 0;
    g_active = t;
    tty_install_hooks();
    tty_query_size(t);
    lt_term_write(tty_seq_open, (int)(sizeof(tty_seq_open) - 1));
    tty_set_title(b, title);
    return 0;
}

static void tty_close(lt_backend *b)
{
    tty *t = (tty *)b->impl;
    if (t == NULL) {
        return;
    }
    if (t->opened) {
        g_active = t;
        tty_restore();
    }
    tty_free_bufs(t);
}

static void tty_present(lt_backend *b, lt_canvas *canvas, lt_cells *cells, lt_rect dirty)
{
    tty *t = (tty *)b->impl;
    LT_UNUSED(canvas);
    LT_UNUSED(dirty);
    if (t == NULL || !t->opened || cells == NULL) {
        return;
    }
    tty_render(t, cells);
    if (t->out.len > 0) {
        lt_term_write(t->out.p, (int)t->out.len);
    }
}

static void tty_size(lt_backend *b, int *w, int *h)
{
    tty *t = (tty *)b->impl;
    if (w != NULL) {
        *w = t->cols;
    }
    if (h != NULL) {
        *h = t->rows;
    }
}

static int q_pop(tty *t, lt_event *ev)
{
    if (t->qn <= 0) {
        return 0;
    }
    *ev = t->q[t->qh];
    t->qh = (t->qh + 1) % TTY_QCAP;
    t->qn--;
    return 1;
}

static int tty_poll(lt_backend *b, lt_event *ev, int timeout_ms)
{
    tty *t = (tty *)b->impl;
    char buf[128];
    int n, wait, room, remaining = timeout_ms;

    memset(ev, 0, sizeof(*ev));
    for (;;) {
        if (q_pop(t, ev)) {
            return 1;
        }
        if (lt_term_resized()) {
            tty_query_size(t);
            t->have_prev = 0;
            memset(ev, 0, sizeof(*ev));
            ev->type = LT_EV_RESIZE;
            ev->x = t->cols;
            ev->y = t->rows;
            return 1;
        }
        tty_feed(t, 0);
        if (t->qn > 0) {
            continue;
        }
        wait = remaining;
        if (wait < 0 || wait > TTY_SLICE_MS) {
            wait = TTY_SLICE_MS;        /* wake regularly: console resizes arrive as no input */
        }
        if (t->inlen > 0) {
            wait = TTY_ESC_MS; /* partial sequence: always give it time to finish */
        }
        room = TTY_INBUF - t->inlen;
        if (room <= 0) {
            t->inlen = 0; /* garbage overflow: drop */
            room = TTY_INBUF;
        }
        if (room > (int)sizeof(buf)) {
            room = (int)sizeof(buf);
        }
        n = lt_term_read(buf, room, wait);
        if (n < 0) {
            if (!t->eof_sent) {
                t->eof_sent = 1;
                ev->type = LT_EV_QUIT;
                return 1;
            }
            return 0;
        }
        if (n > 0) {
            memcpy(t->in + t->inlen, buf, (size_t)n);
            t->inlen += n;
            continue;
        }
        /* timed out */
        if (t->inlen > 0) {
            tty_feed(t, 1); /* lone ESC or truncated sequence */
            if (t->qn > 0) {
                continue;
            }
        }
        if (remaining < 0) {
            continue;
        }
        remaining -= wait;
        if (remaining <= 0) {
            return 0;
        }
    }
}

lt_backend *lt_backend_tty_create(void)
{
    lt_backend *b;
    tty *t;
    if (!lt_term_isatty()) {
        return NULL;
    }
    b = (lt_backend *)lt_calloc(1, sizeof(lt_backend));
    t = (tty *)lt_calloc(1, sizeof(tty));
    b->name = "tty";
    b->cell_mode = LT_TRUE;
    b->open = tty_open;
    b->close = tty_close;
    b->poll = tty_poll;
    b->present = tty_present;
    b->size = tty_size;
    b->set_title = tty_set_title;
    b->set_clipboard = NULL;
    b->get_clipboard = NULL;
    b->beep = tty_beep;
    b->impl = t;
    t->cols = 80;
    t->rows = 24;
    t->depth = TTY_D_16;
    t->utf8 = 1;
    return b;
}
