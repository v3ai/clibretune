/* gui.h - clibretune-C graphics contract: pixel canvas, rasterizer, fonts,
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
#include "ttf.h"
#include "icons.h"

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
/* Pixel canvas: 32-bit XRGB, row-major, pitch in pixels.             */
typedef struct lt_canvas {
    lt_u32 *px;
    int w, h, pitch;
    lt_u32 gen;           /* content generation: changes with every draw call into the canvas
                           * (unique across canvases); retained rendering keys blits on it */
} lt_canvas;
void lt_canvas_init(lt_canvas *c, int w, int h);   /* allocates */
void lt_canvas_resize(lt_canvas *c, int w, int h);
void lt_canvas_free(lt_canvas *c);
/* call before writing c->px directly (outside the lt_gfx calls): a recorded frame that still
 * refers to the old pixels keeps a copy, and the generation changes */
void lt_canvas_touch(lt_canvas *c);

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
/* snapshots (cellshot.c): plain text (malloc'd, UTF-8, trailing blanks trimmed) and a PPM
 * image of the grid (10x20 px per cell); 0 = ok */
char *lt_cells_to_text(const lt_cells *g);
int lt_cells_write_ppm(const lt_cells *g, const char *path);
/* what a terminal of the given color depth (24, 256, 16, 1 = mono) and charset would show:
 * quantizes colors in place and replaces glyphs the charset (0 ASCII, 1 Unicode, 2 Linux
 * console subset) cannot show (be_tty.c) */
void lt_tty_apply_mode(lt_cells *g, int depth, int charset);
/* the full-frame escape stream for that terminal, malloc'd, NUL-terminated (be_tty.c) */
char *lt_tty_ansi_dump(const lt_cells *g, int depth, int charset);

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
    int        bitmap_text; /* nonzero: lt_gfx_text/vtext use the 8x16 bitmap/Hershey
                             * fonts even in pixel mode (default 0 = IBM Plex) */
    int        ui_pct;    /* UI scale percent for custom draw callbacks (0 = 100); set by lt_ui_begin */
    struct lt_rec *rec;   /* non-NULL while lt_tiles records this context: draw calls are
                           * stored, not rasterized, and g->canvas must not be read */
} lt_gfx;

/* px at 100% -> device px using g->ui_pct (identity in cell mode) */
int    lt_gfx_px(const lt_gfx *g, int px);
double lt_gfx_fpx(const lt_gfx *g, double px);

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
/* ---- frame differencing (gfx_diff.c): which parts of a canvas changed since the last call ---- */
typedef struct lt_diff { lt_u32 *prev; int w, h; } lt_diff;
void lt_diff_free(lt_diff *d);
void lt_diff_invalidate(lt_diff *d);      /* force the next lt_diff_rects to report everything */
/* Compare `cur` with the previous frame inside `area` per 64x64 tile; writes up to max_out merged
 * rectangles of changed tiles to out and returns their count (0 = nothing changed). The stored
 * copy is updated, so each change is reported once. */
int lt_diff_rects(lt_diff *d, const lt_canvas *cur, lt_rect area, lt_rect *out, int max_out);

/* ---- retained rendering (gfx_rec.c) ----
 * Between lt_tiles_begin and lt_tiles_end every pixel-mode draw call on g is recorded into a
 * display list (parameters, copied strings/points/pixels, clip, translation and a conservative
 * bounding box per call). lt_tiles_end hashes the calls touching each screen tile, compares with
 * the previous frame and rasterizes only the tiles whose hash changed, replaying their calls with
 * the clip narrowed to the tile, so the canvas ends up exactly as if every call had been drawn
 * directly. The frame must start with an opaque clear (lt_ui_end does). Offscreen canvases
 * stay immediate; a blit of one is keyed on its generation (lt_canvas.gen). Cell mode is never
 * recorded. Code that would read g->canvas pixels while recording must check
 * lt_gfx_recording. */
typedef struct lt_tiles lt_tiles;
lt_tiles *lt_tiles_new(void);
void lt_tiles_free(lt_tiles *t);
void lt_tiles_begin(lt_tiles *t, lt_gfx *g);  /* no-op (nothing recorded) in cell mode */
/* stop recording and rasterize the changed tiles into g->canvas; writes up to max_out merged
 * rectangles of changed pixels to out (bounding box of everything when more) and returns their
 * count; g->dirty becomes their union */
int  lt_tiles_end(lt_tiles *t, lt_gfx *g, lt_rect *out, int max_out);
void lt_tiles_invalidate(lt_tiles *t);        /* next frame re-renders every tile */
/* debug cross-check: render the last recorded frame from scratch into a private canvas and
 * compare with g->canvas; returns the number of differing pixels (first one in *x, *y) */
long lt_tiles_verify(lt_tiles *t, const lt_gfx *g, int *x, int *y);
/* statistics of the last frame: recorded calls, tiles, re-rendered tiles, re-rendered pixels */
void lt_tiles_stats(const lt_tiles *t, long *ncmd, int *ntiles, int *ndirty, double *pixels);
lt_bool lt_gfx_recording(const lt_gfx *g);

/* composite a premultiplied-ARGB canvas (alpha in the top byte) at (x, y); pixel mode only */
void lt_gfx_blit_premul(lt_gfx *g, int x, int y, const lt_canvas *src);
void lt_gfx_fill_rect(lt_gfx *g, lt_rect r, lt_color c);
void lt_gfx_rect(lt_gfx *g, lt_rect r, lt_color c);           /* 1px outline */
void lt_gfx_fill_round_rect(lt_gfx *g, lt_rect r, int radius, lt_color c);
/* soft drop shadow under r: for i = spread..1 a black round rect r grown by i (shifted down by
 * spread/2), radius radius+i, alpha alpha*(spread-i+1)^2/(1.5*spread^2+1) (at least 1) - drawn
 * in one pass with exactly the pixels of those calls */
void lt_gfx_shadow(lt_gfx *g, lt_rect r, int radius, int spread, int alpha);
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
void lt_gfx_blit(lt_gfx *g, int x, int y, const lt_u32 *px, int w, int h, int pitch);
/* blit an 8-bit coverage mask tinted with color c (alpha-blended). lut, if
 * non-NULL, is a 256-entry table remapping coverage (text contrast). No-op in
 * cell mode. */
void lt_gfx_blit_mask(lt_gfx *g, int x, int y, const lt_u8 *a, int w, int h, int pitch,
                      lt_color c, const lt_u8 *lut);

/* bitmap text (UTF-8). scale 1..4. Returns advance width in pixels. */
int  lt_gfx_text(lt_gfx *g, int x, int y, const char *utf8, lt_color c, int scale);
int  lt_gfx_text_width(const char *utf8, int scale);
int  lt_gfx_text_height(int scale);
/* vector text: height in px, rotation radians, align: 0 left, 1 center, 2 right;
 * baseline-centered vertically when valign=1. */
void lt_gfx_vtext(lt_gfx *g, double x, double y, const char *utf8, double height,
                  double rotation, int align, int valign, double stroke, lt_color c);
double lt_gfx_vtext_width(const char *utf8, double height);

/* Cell-mode drawing helpers (gfx_cell.c; no-ops in pixel mode). Coordinates are pixel units
 * (a cell is 8x16) like every other draw call; rects snap to whole cells. */
/* UTF-8 text with its own background (bg alpha 0 keeps the cell's) and attributes */
int  lt_gfx_cell_text(lt_gfx *g, int x, int y, const char *utf8, lt_color fg, lt_color bg, int attr);
void lt_gfx_cell_attr(lt_gfx *g, lt_rect r, int attr, int on);
/* horizontal / vertical bar filled to frac (0..1) with eighth-block resolution */
void lt_gfx_cell_hbar(lt_gfx *g, lt_rect r, double frac, lt_color fg, lt_color track);
void lt_gfx_cell_vbar(lt_gfx *g, lt_rect r, double frac, lt_color fg, lt_color track);
/* sparkline of the newest samples, 8 levels per row of r */
void lt_gfx_cell_spark(lt_gfx *g, lt_rect r, const double *v, int n, double lo, double hi,
                       lt_color fg, lt_color bg);
void lt_cell_ellipsize_mid(const char *s, int cols, char *out, size_t cap);
/* braille line plot (2x4 dots per cell); flags below */
#define LT_PLOT_CLEAR 1      /* reset the rect to bg first (else merge into existing braille) */
#define LT_PLOT_FILL 2       /* fill under the line */
#define LT_PLOT_STRETCH 4    /* spread the samples over the full width */
void lt_gfx_cell_plot(lt_gfx *g, lt_rect r, const double *v, int n, double lo, double hi,
                      lt_color fg, lt_color bg, int flags);
/* darken the cells under the dx,dy-cell offset copy of r that r does not cover */
void lt_gfx_cell_shadow(lt_gfx *g, lt_rect r, int dx, int dy);

/* ------------------------------------------------------------------ */
/* TrueType text (IBM Plex, embedded). Pixel mode only; in cell mode these
 * fall back to the 8x16 cell font (text snapped to the grid).
 *
 * Fonts are loaded lazily on first use. lt_font_get returns NULL if the
 * embedded data could not be parsed (never happens in a healthy build); all
 * text functions accept NULL and then use the bitmap font.
 *
 * Sizes are em heights in pixels (CSS font-size): 11..16 for UI text.
 * Stock clibretune: Plex Sans 12px body, Plex Mono for numbers, SemiBold
 * for headings.
 *
 * Vertical placement: lt_gfx_text_ex puts y at the TOP of the line box (the
 * line box is lt_font_line_height tall and the baseline lies lt_font_ascent
 * below y); lt_gfx_text_bl puts y at the baseline. Fractional x is honored
 * (glyphs are positioned on a 1/4 pixel grid). Kerning is applied. */
typedef lt_ttf lt_font;
typedef enum lt_font_id {
    LT_FONT_SANS = 0,
    LT_FONT_SANS_MEDIUM,
    LT_FONT_SANS_SEMIBOLD,
    LT_FONT_MONO,
    LT_FONT_MONO_MEDIUM,
    LT_FONT_COUNT
} lt_font_id;

lt_font *lt_font_get(lt_font_id id);
void lt_font_release_all(void);          /* free every loaded font (tests/exit) */
/* metrics at size px, rounded to whole pixels so line boxes tile exactly */
int lt_font_ascent(lt_font *f, double px);
int lt_font_descent(lt_font *f, double px);       /* positive */
int lt_font_line_height(lt_font *f, double px);   /* ascent + descent */
double lt_font_cap_height(lt_font *f, double px); /* unrounded */
double lt_font_x_height(lt_font *f, double px);

/* width of utf8 in pixels (kerning included); bitmap-font width if f is NULL */
double lt_gfx_text_measure(lt_font *f, double px, const char *utf8);
/* byte count of the longest prefix of utf8 that fits in max_w pixels */
size_t lt_gfx_text_fit(lt_font *f, double px, const char *utf8, double max_w);
/* draw; return the advance width in whole pixels (rounded) */
int lt_gfx_text_ex(lt_gfx *g, lt_font *f, double px, int x, int y, const char *utf8, lt_color c);
int lt_gfx_text_bl(lt_gfx *g, lt_font *f, double px, double x, int baseline, const char *utf8,
                   lt_color c);
/* rotated text (gauge labels): (x, y) is the anchor, rotation in radians
 * (0 = left-to-right, increasing clockwise on screen), align 0 left / 1
 * center / 2 right along the text direction, valign 0 = anchor at the cap
 * top, 1 = middle of the capital letters, 2 = baseline. */
void lt_gfx_text_rot(lt_gfx *g, lt_font *f, double px, double x, double y, const char *utf8,
                     double rotation, int align, int valign, lt_color c);

/* ------------------------------------------------------------------ */
/* Icons: the Lucide icon set (ISC, https://lucide.dev), 24x24 design grid,
 * stroked with round caps/joins. Ids are the LT_ICON_* macros from icons.h
 * (LT_ICON_FOLDER_OPEN, LT_ICON_SETTINGS, ...). Pixel mode only; cell mode
 * draws nothing. */
/* lucide name ("folder-open") or lucide-react name ("FolderOpen") -> id, or
 * LT_ICON_NONE (0) when unknown. Case-insensitive, ignores '-' and '_'. */
int lt_icon_find(const char *name);
const char *lt_icon_name(int icon);   /* kebab-case lucide name or NULL */
int lt_icon_count(void);              /* number of icons; ids are 1..count */
/* Draw icon with its top-left at (x, y), size_px wide/high, strokes stroke_px
 * wide (lucide default: 2 * size_px / 24; pass <= 0 for the default).
 * Fractional positions are anti-aliased exactly. */
void lt_gfx_icon(lt_gfx *g, int icon, double x, double y, double size_px, double stroke_px,
                 lt_color c);

/* ------------------------------------------------------------------ */
/* Input events                                                        */
typedef enum lt_ev_type {
    LT_EV_NONE = 0,
    LT_EV_QUIT,
    LT_EV_RESIZE,      /* w,h = new size in pixels (tty: columns and rows) */
    LT_EV_EXPOSE,      /* need full repaint */
    LT_EV_MOUSE_MOVE,  /* x,y */
    LT_EV_MOUSE_DOWN,  /* x,y,button */
    LT_EV_MOUSE_UP,
    LT_EV_WHEEL,       /* x,y, wheel = +1 up / -1 down (dx in button) */
    LT_EV_KEY_DOWN,    /* key (LT_KEY_*), mods; repeats allowed */
    LT_EV_KEY_UP,
    LT_EV_TEXT,        /* text = UTF-8 of typed character(s) */
    LT_EV_FOCUS_LOST,
    LT_EV_SCALE        /* display scale changed: x = new scale in percent (app re-queries) */
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
    lt_bool rect_present;       /* present() may be called several times per frame with disjoint
                                 * rectangles and sends only each rectangle (x11, win32) */
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
    /* ---- optional hooks (NULL = unsupported; call through the lt_backend_*
     * helpers below, which supply the defaults) ---- */
    /* display scale factor (1.0 = 96 dpi / unknown); may be called before open */
    double (*scale_hint)(lt_backend *b);
    /* minimum client size in pixels; may be called before open */
    void (*set_min_size)(lt_backend *b, int w, int h);
    /* desktop size in pixels; returns 1 if known */
    int  (*screen_size)(lt_backend *b, int *w, int *h);
    /* window position/maximized state to use when the window opens
     * (flags: LT_GEOM_POS = x,y valid, LT_GEOM_MAX = start maximized) */
    void (*set_initial_geometry)(lt_backend *b, int x, int y, int flags);
    /* current top-left of the client area and maximized state; returns 1 if known */
    int  (*get_geometry)(lt_backend *b, int *x, int *y, int *maximized);
    /* switch the window to/from full screen (kiosk dashboard); NULL = unsupported */
    void (*set_fullscreen)(lt_backend *b, int on);
    /* audible alert (X11 Bell, MessageBeep, NSBeep, terminal BEL, WebAudio); NULL = silent */
    void (*beep)(lt_backend *b);
    /* Present every changed rectangle of one frame in a single call (disjoint rectangles, the
     * output of lt_diff_rects): X11 sends them in one write, Win32 uses one DC, Cocoa one display
     * pass. NULL = present() once per rectangle; call through lt_backend_present_rects. */
    void (*present_rects)(lt_backend *b, lt_canvas *canvas, const lt_rect *rects, int n);
    /* Zero-copy canvas: pixel memory for a w x h canvas (pitch == w) that the backend can show
     * without copying it (X11: the MIT-SHM segment the server reads; Win32: a DIB section).
     * Call it before drawing every frame: it returns only once the window system no longer
     * reads the buffer, so the frame may be drawn into it. The memory stays valid until the next
     * call or close() and keeps its contents between calls with the same size; a different
     * pointer than last time means the contents are gone (redraw and present everything).
     * NULL = not available now (the app keeps its own canvas). Through lt_backend_canvas_pixels. */
    lt_u32 *(*canvas_pixels)(lt_backend *b, int w, int h);
    void *impl;
};

#define LT_GEOM_POS 1
#define LT_GEOM_MAX 2
/* NULL-safe wrappers for the optional hooks */
double lt_backend_scale_hint(lt_backend *b);                 /* 1.0 when unknown */
void   lt_backend_set_min_size(lt_backend *b, int w, int h);
int    lt_backend_screen_size(lt_backend *b, int *w, int *h);
void   lt_backend_set_initial_geometry(lt_backend *b, int x, int y, int flags);
int    lt_backend_get_geometry(lt_backend *b, int *x, int *y, int *maximized);
/* returns 1 when the backend supports full screen (and applied it), else 0 */
int    lt_backend_set_fullscreen(lt_backend *b, int on);
/* sound the backend's alert; returns 1 when a beep was requested, 0 when the backend (or a
 * NULL / offscreen backend) has no sound */
int    lt_backend_beep(lt_backend *b);
/* present rects[0..n) of one frame: present_rects when the backend has it, else present() per
 * rectangle (rect_present backends) or once with their bounding box (the others) */
void   lt_backend_present_rects(lt_backend *b, lt_canvas *canvas, const lt_rect *rects, int n);
/* backend-owned canvas memory (see canvas_pixels) or NULL when unsupported */
lt_u32 *lt_backend_canvas_pixels(lt_backend *b, int w, int h);
/* "Xft.dpi: N" from an X resource-manager string; 0.0 if absent (exposed for tests) */
double lt_x11_parse_xft_dpi(const char *resources);
/* XSETTINGS blob -> Xft/DPI (dpi) and Gdk/WindowScalingFactor; 0 ok, -1 malformed */
int lt_x11_parse_xsettings(const unsigned char *d, size_t n, double *dpi, int *wscale);
/* scale of a monitor from pixels and millimetres (dpi/120 in 25% steps; 0 = unknown) */
double lt_x11_dpi_scale(int px_w, int px_h, int mm_w, int mm_h);
/* display scale from every X11 source (largest wins; physical DPI as a last resort) */
double lt_x11_pick_scale(double xft_dpi, double xs_dpi, int xs_wscale, double gdk, double qt,
                         int root_w, int mm_w);

/* Backend constructors. Each returns NULL if unavailable on this system
 * (e.g. no $DISPLAY, not running on Windows). Never abort. */
lt_backend *lt_backend_x11_create(void);
lt_backend *lt_backend_win32_create(void);   /* NULL unless built for Windows */
lt_backend *lt_backend_cocoa_create(void);   /* NULL unless built for macOS */
lt_backend *lt_backend_web_create(int port);   /* port 0 = default 8640 */
lt_backend *lt_backend_tty_create(void);
/* Pick by name ("auto" = win32 -> x11 -> web (macOS) -> tty). */
lt_backend *lt_backend_select(const char *name);
void lt_backend_destroy(lt_backend *b);

#endif /* LT_GUI_H */
