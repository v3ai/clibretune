/* gui.h - LibreTune-C graphics contract: pixel canvas, rasterizer, fonts,
 * input events and the platform backend interface.
 *
 * Architecture
 *   app screens -> ui toolkit (ui.h, immediate mode)
 *               -> lt_gfx draw calls (this file)
 *               -> either a pixel lt_canvas (x11/win32/web backends)
 *                  or a character-cell lt_cells grid (tty backend)
 *   The toolkit never knows which backend is active, except through
 *   lt_gfx.cell_w/cell_h: in cell mode one "pixel" unit is still a pixel in
 *   layout terms but text is snapped to an 8x16 grid and curves are
 *   approximated. Layout is always computed in pixels with the base font
 *   8x16, so cell mode = pixel layout / (8,16).
 *
 * All colors are 0xAARRGGBB (lt_color). Alpha 0xFF = opaque.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef LT_GUI_H
#define LT_GUI_H

#include "../lt.h"

typedef lt_u32 lt_color;
#define LT_RGB(r, g, b) ((lt_color)(0xFF000000UL | ((lt_u32)(r) << 16) | ((lt_u32)(g) << 8) | (lt_u32)(b)))
#define LT_RGBA(r, g, b, a) ((lt_color)(((lt_u32)(a) << 24) | ((lt_u32)(r) << 16) | ((lt_u32)(g) << 8) | (lt_u32)(b)))
#define LT_COLOR_A(c) ((int)(((c) >> 24) & 0xFF))
#define LT_COLOR_R(c) ((int)(((c) >> 16) & 0xFF))
#define LT_COLOR_G(c) ((int)(((c) >> 8) & 0xFF))
#define LT_COLOR_B(c) ((int)((c) & 0xFF))

typedef struct lt_rect { int x, y, w, h; } lt_rect;
typedef struct lt_point { int x, y; } lt_point;

lt_rect lt_rect_make(int x, int y, int w, int h);
lt_rect lt_rect_intersect(lt_rect a, lt_rect b);
lt_bool lt_rect_contains(lt_rect r, int x, int y);
lt_rect lt_rect_inset(lt_rect r, int dx, int dy);

/* ------------------------------------------------------------------ */
/* Pixel canvas: 32-bit XRGB, row-major, stride in pixels.             */
typedef struct lt_canvas {
    lt_u32 *px;
    int w, h, stride;
} lt_canvas;
void lt_canvas_init(lt_canvas *c, int w, int h);   /* allocates */
void lt_canvas_resize(lt_canvas *c, int w, int h);
void lt_canvas_free(lt_canvas *c);

/* Character-cell grid for the tty backend. */
typedef struct lt_cell {
    lt_u32 ch;        /* Unicode code point (ASCII preferred; box chars allowed) */
    lt_color fg, bg;
    lt_u8 attr;       /* LT_CELL_BOLD | LT_CELL_UNDERLINE | LT_CELL_REVERSE */
} lt_cell;
#define LT_CELL_BOLD 1
#define LT_CELL_UNDERLINE 2
#define LT_CELL_REVERSE 4
typedef struct lt_cells {
    lt_cell *cells;
    int cols, rows;
} lt_cells;
void lt_cells_init(lt_cells *g, int cols, int rows);
void lt_cells_resize(lt_cells *g, int cols, int rows);
void lt_cells_free(lt_cells *g);

/* ------------------------------------------------------------------ */
/* Fonts: built-in 8x16 bitmap font (ASCII + Latin-1 + a few symbols like
 * degree sign), drawable at integer scale 1..4, plus a vector stroke font
 * (Hershey-style, public domain glyph data) for arbitrary sizes and
 * rotation, used by gauges. */
#define LT_FONT_W 8
#define LT_FONT_H 16
/* glyph rows for code point (16 bytes, MSB = leftmost). Unknown -> '?' */
const lt_u8 *lt_font_glyph(lt_u32 cp);

/* UTF-8 decode one code point; advances *s. Invalid bytes -> U+FFFD. */
lt_u32 lt_utf8_next(const char **s);

/* ------------------------------------------------------------------ */
/* Graphics context: the single drawing API everybody uses.            */
typedef struct lt_gfx {
    lt_canvas *canvas;    /* pixel mode if non-NULL */
    lt_cells  *cells;     /* cell mode if non-NULL */
    lt_rect    clip;      /* current clip in pixel coordinates */
    int        cell_w, cell_h; /* 1,1 in pixel mode; 8,16 in cell mode */
    int        ox, oy;    /* translation applied to all coordinates */
    lt_rect    dirty;     /* union of everything drawn since last reset */
} lt_gfx;

void lt_gfx_init_canvas(lt_gfx *g, lt_canvas *c);
void lt_gfx_init_cells(lt_gfx *g, lt_cells *cells);
lt_bool lt_gfx_cell_mode(const lt_gfx *g);
int  lt_gfx_width(const lt_gfx *g);   /* in pixel units (cells*8 in cell mode) */
int  lt_gfx_height(const lt_gfx *g);
void lt_gfx_set_clip(lt_gfx *g, lt_rect r);  /* intersected with surface */
lt_rect lt_gfx_push_clip(lt_gfx *g, lt_rect r); /* returns previous; intersects */
void lt_gfx_pop_clip(lt_gfx *g, lt_rect prev);
void lt_gfx_reset_dirty(lt_gfx *g);

/* primitives (alpha-blended when color alpha < 255) */
void lt_gfx_clear(lt_gfx *g, lt_color c);
void lt_gfx_fill_rect(lt_gfx *g, lt_rect r, lt_color c);
void lt_gfx_rect(lt_gfx *g, lt_rect r, lt_color c);           /* 1px outline */
void lt_gfx_fill_round_rect(lt_gfx *g, lt_rect r, int radius, lt_color c);
void lt_gfx_hline(lt_gfx *g, int x0, int x1, int y, lt_color c);
void lt_gfx_vline(lt_gfx *g, int x, int y0, int y1, lt_color c);
/* anti-aliased line, width in pixels (double) */
void lt_gfx_line(lt_gfx *g, double x0, double y0, double x1, double y1, double width, lt_color c);
void lt_gfx_fill_circle(lt_gfx *g, double cx, double cy, double r, lt_color c);
void lt_gfx_circle(lt_gfx *g, double cx, double cy, double r, double width, lt_color c);
/* angles in radians, 0 = +x (east), increasing clockwise on screen (y down) */
void lt_gfx_arc(lt_gfx *g, double cx, double cy, double r, double a0, double a1, double width, lt_color c);
/* filled annular sector (for sweep gauges / colored ranges) */
void lt_gfx_fill_arc(lt_gfx *g, double cx, double cy, double r_in, double r_out, double a0, double a1, lt_color c);
/* filled polygon, even-odd, anti-aliased edges */
void lt_gfx_fill_poly(lt_gfx *g, const double *xy, int npoints, lt_color c);
/* vertical gradient */
void lt_gfx_fill_rect_vgrad(lt_gfx *g, lt_rect r, lt_color top, lt_color bottom);
/* blit ARGB image (alpha blended) */
void lt_gfx_blit(lt_gfx *g, int x, int y, const lt_u32 *px, int w, int h, int stride);

/* bitmap text (UTF-8). scale 1..4. Returns advance width in pixels. */
int  lt_gfx_text(lt_gfx *g, int x, int y, const char *utf8, lt_color c, int scale);
int  lt_gfx_text_width(const char *utf8, int scale);
int  lt_gfx_text_height(int scale);
/* vector text: height in px, rotation radians, align: 0 left, 1 center, 2 right;
 * baseline-centered vertically when valign=1. */
void lt_gfx_vtext(lt_gfx *g, double x, double y, const char *utf8, double height,
                  double rotation, int align, int valign, double stroke, lt_color c);
double lt_gfx_vtext_width(const char *utf8, double height);

/* ------------------------------------------------------------------ */
/* Input events                                                        */
typedef enum lt_ev_type {
    LT_EV_NONE = 0,
    LT_EV_QUIT,
    LT_EV_RESIZE,      /* w,h = new size in pixels (cells*8/16 for tty) */
    LT_EV_EXPOSE,      /* need full repaint */
    LT_EV_MOUSE_MOVE,  /* x,y */
    LT_EV_MOUSE_DOWN,  /* x,y,button */
    LT_EV_MOUSE_UP,
    LT_EV_WHEEL,       /* x,y, wheel = +1 up / -1 down (dx in button) */
    LT_EV_KEY_DOWN,    /* key (LT_KEY_*), mods; repeats allowed */
    LT_EV_KEY_UP,
    LT_EV_TEXT,        /* text = UTF-8 of typed character(s) */
    LT_EV_FOCUS_LOST
} lt_ev_type;

/* mouse buttons */
#define LT_MB_LEFT 1
#define LT_MB_MIDDLE 2
#define LT_MB_RIGHT 3

/* modifiers */
#define LT_MOD_SHIFT 1
#define LT_MOD_CTRL  2
#define LT_MOD_ALT   4
#define LT_MOD_SUPER 8

/* keys: printable keys use their lowercase ASCII code ('a'..'z','0'..'9',
 * ' ', '-', '=', etc). Special keys: */
enum {
    LT_KEY_NONE = 0,
    LT_KEY_BACKSPACE = 8, LT_KEY_TAB = 9, LT_KEY_ENTER = 13, LT_KEY_ESCAPE = 27,
    LT_KEY_DELETE = 127,
    LT_KEY_UP = 0x100, LT_KEY_DOWN, LT_KEY_LEFT, LT_KEY_RIGHT,
    LT_KEY_HOME, LT_KEY_END, LT_KEY_PAGEUP, LT_KEY_PAGEDOWN, LT_KEY_INSERT,
    LT_KEY_F1, LT_KEY_F2, LT_KEY_F3, LT_KEY_F4, LT_KEY_F5, LT_KEY_F6,
    LT_KEY_F7, LT_KEY_F8, LT_KEY_F9, LT_KEY_F10, LT_KEY_F11, LT_KEY_F12
};

typedef struct lt_event {
    lt_ev_type type;
    int x, y;
    int button;
    int wheel;
    int key;
    int mods;
    char text[8];
} lt_event;

/* ------------------------------------------------------------------ */
/* Backend interface. Exactly one backend is active per run.          */
typedef struct lt_backend lt_backend;
struct lt_backend {
    const char *name;           /* "x11", "win32", "web", "tty" */
    lt_bool cell_mode;          /* tty: draws into lt_cells */
    /* open a window/session of the requested pixel size; 0 = ok */
    int  (*open)(lt_backend *b, const char *title, int w, int h);
    void (*close)(lt_backend *b);
    /* Wait up to timeout_ms (0 = no wait, -1 = forever) for one event.
     * Returns 1 and fills *ev if an event is available, 0 on timeout. */
    int  (*poll)(lt_backend *b, lt_event *ev, int timeout_ms);
    /* Present: pixel backends read canvas (only `dirty` changed); tty reads cells. */
    void (*present)(lt_backend *b, lt_canvas *canvas, lt_cells *cells, lt_rect dirty);
    /* current surface size (pixels, or cols/rows in cell mode) */
    void (*size)(lt_backend *b, int *w, int *h);
    void (*set_title)(lt_backend *b, const char *title);
    /* optional (may be NULL): system clipboard */
    void (*set_clipboard)(lt_backend *b, const char *utf8);
    char *(*get_clipboard)(lt_backend *b); /* malloc'd or NULL */
    void *impl;
};

/* Backend constructors. Each returns NULL if unavailable on this system
 * (e.g. no $DISPLAY, not running on Windows). Never abort. */
lt_backend *lt_backend_x11_create(void);
lt_backend *lt_backend_win32_create(void);
lt_backend *lt_backend_web_create(int port);   /* port 0 = default 8640 */
lt_backend *lt_backend_tty_create(void);
/* Pick by name ("auto" = win32 -> x11 -> web (macOS) -> tty). */
lt_backend *lt_backend_select(const char *name);
void lt_backend_destroy(lt_backend *b);

#endif /* LT_GUI_H */
