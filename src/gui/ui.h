/* ui.h - LibreTune-C immediate-mode widget toolkit.
 *
 * Frame protocol (one frame per loop iteration, only when needed):
 *
 *     while (poll event) lt_ui_input(ui, &ev);
 *     lt_ui_begin(ui, &gfx, now_ms);
 *     ... widget calls (they return state: clicked / changed / ...) ...
 *     lt_ui_end(ui);                    // replays the draw lists into gfx
 *     backend->present(..., gfx.dirty);
 *
 * Idle loop contract (see tests/manual/ui_demo.c):
 *     timeout = lt_ui_needs_redraw(ui) ? 0 : lt_ui_wait_hint_ms(ui);   // -1 = forever
 *     got = backend->poll(be, &ev, timeout);  ... feed all pending events ...
 *     if (got || lt_ui_needs_redraw(ui) || timeout >= 0) run a frame;
 * i.e. an animation/tooltip/caret timer firing (poll timed out with a
 * non-negative hint) also runs a frame, so a fully idle app sleeps in poll.
 *
 * Design notes
 *  - Widgets never draw directly: every draw call is recorded into the draw
 *    list of the current *layer* (main, floating window, popup, modal,
 *    tooltip) and replayed at lt_ui_end in z order.  Overlays are therefore
 *    painted on top of content that was declared after them, and hit testing
 *    uses the layer rectangles of the previous frame so clicks never fall
 *    through popups or modals.  (Consequence: a popup that appears reacts
 *    one frame late; lt_ui_needs_redraw() asks for the extra frame.)
 *  - IDs are hashes of the label combined with the id stack.  "Label##x"
 *    shows "Label" but hashes the whole string; "Label###x" hashes only "x".
 *    Field widgets (text, number, slider, combo, list...) draw no caption:
 *    their label is only an id source; use lt_ui_label for captions.
 *  - Layout is computed in pixels with the 8x16 font.  In cell mode (tty)
 *    every theme metric snaps to multiples of 8x16 and borders are dropped.
 *  - Single threaded, no globals: any number of lt_ui contexts may coexist.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef LT_UI_H
#define LT_UI_H

#include "gui.h"

typedef struct lt_ui lt_ui;

#define LT_UI_MAX_COLS 16

/* result flags shared by several widgets */
#define LT_UI_CHANGED   1   /* value changed */
#define LT_UI_SUBMIT    2   /* Enter pressed / edit committed */
#define LT_UI_CANCEL    4   /* Esc pressed / edit cancelled */
#define LT_UI_ACTIVATED 8   /* double click or Enter on an item */

/* ------------------------------------------------------------------ */
/* Theme                                                               */
typedef struct lt_theme {
    const char *name;
    int dark;
    /* surfaces */
    lt_color bg_app, surface, elevated, input, hover, active, selected, dash_bg;
    lt_color menubar, bg_toolbar, statusbar, sidebar, tab, tab_active;
    /* text */
    lt_color text, text2, muted, disabled, inverse, link;
    /* borders */
    lt_color border, border_subtle, border_strong, focus;
    /* accents */
    lt_color primary, primary_hover, primary_active, primary_text;
    lt_color accent, accent_hover, accent_text;
    lt_color success, success_bg, warning, warning_bg, error, error_bg, info, info_bg;
    lt_color scroll_track, scroll_thumb, scroll_hover;
    /* table */
    lt_color table_bg, table_header, table_cell, table_hover, table_selected,
             table_editing, table_axis, table_grid;
    lt_color change_richer, change_leaner, change_same;
    lt_color cursor_live, cursor_trail, cursor_selected, cursor_active;
    /* metrics in pixels; derived from the 8x16 font by lt_ui_theme_changed() */
    int row_h, spacing_x, spacing_y, padding, pad_x, radius, scrollbar_w, indent;
    int menubar_h, tbar_h, tab_h, statusbar_h, splitter_w, title_h;
    int tree_row_h;     /* sidebar tree row (stock: 20px) */
    int radius_lg;      /* cards and dialogs */
} lt_theme;

/* Font sizes in px at scale 1 (stock LibreTune --font-size-* tokens). The
 * toolkit multiplies them by the UI scale. */
#define LT_UI_FS_XS   10
#define LT_UI_FS_SM   11
#define LT_UI_FS_BASE 12
#define LT_UI_FS_MD   13
#define LT_UI_FS_LG   14
#define LT_UI_FS_XL   16
#define LT_UI_FS_2XL  18
#define LT_UI_FS_3XL  24

void lt_theme_dark(lt_theme *t);
void lt_theme_light(lt_theme *t);
/* presets: dark light midnight carbon synthwave solarized nord dracula
 * highcontrast. Returns 0, or -1 if unknown (theme untouched). */
int  lt_theme_preset(lt_theme *t, const char *name);
/* fill every color from bg/primary/accent */
void lt_theme_derive(lt_theme *t, const char *name, lt_color bg, lt_color primary,
                     lt_color accent, lt_bool dark);
/* mix a->b, t256 = 0..256 */
lt_color lt_color_mix(lt_color a, lt_color b, int t256);
/* heat map: scheme 0 tunerstudio, 1 grayscale, 2 pastel, 3 thermal,
 * 4 viridis, 5 plasma; t in 0..1 (clamped). Opaque result. */
lt_color lt_heat_color(int scheme, double t);
/* black or white, whichever reads better on bg */
lt_color lt_color_contrast(lt_color bg);

/* ------------------------------------------------------------------ */
/* Context and frame                                                   */
lt_ui *lt_ui_create(void);
void   lt_ui_destroy(lt_ui *ui);
lt_theme *lt_ui_theme(lt_ui *ui);       /* editable; call lt_ui_theme_changed() after */
void   lt_ui_set_theme(lt_ui *ui, const lt_theme *t);
void   lt_ui_theme_changed(lt_ui *ui);  /* recompute metrics (cell mode, scale) */
void   lt_ui_set_scale(lt_ui *ui, int scale);   /* integer 1..3 (= 100..300 %) */
void   lt_ui_set_scale_pct(lt_ui *ui, int pct); /* UI scale in percent, clamped 100..300 */
int    lt_ui_scale_pct(const lt_ui *ui);
/* px at 100% -> device pixels, rounded (identity in cell mode). Use for every
 * hard-coded pixel metric. */
int    lt_ui_px(const lt_ui *ui, int px);
int    lt_ui_px_pct(int px, int pct);          /* same with an explicit percent */
int    lt_ui_unpx(const lt_ui *ui, int px);   /* device px -> px at 100% (rounded down) */
void   lt_ui_set_backend(lt_ui *ui, lt_backend *b); /* clipboard source; may be NULL */
/* feed one backend event (any time between frames) */
void   lt_ui_input(lt_ui *ui, const lt_event *ev);
void   lt_ui_begin(lt_ui *ui, lt_gfx *g, double now_ms);
void   lt_ui_end(lt_ui *ui);
/* LT_TRUE when a frame should be run now (input arrived, animation running,
 * or an overlay needs its settling frame). */
lt_bool lt_ui_needs_redraw(const lt_ui *ui);
/* poll timeout hint for the idle loop: ms until the next animation tick
 * (caret blink, tooltip delay, spinner, toast), or -1 = sleep until input */
int    lt_ui_wait_hint_ms(const lt_ui *ui);
void   lt_ui_request_redraw(lt_ui *ui);
/* Inside a frame: do not paint this frame (the surface keeps the previous one) and run another
 * right away. For when state that this frame's custom draw callbacks point at is freed before
 * lt_ui_end replays them (a tab or dialog closed after it was drawn). */
void   lt_ui_discard_frame(lt_ui *ui);
lt_bool lt_ui_quit_requested(const lt_ui *ui);  /* LT_EV_QUIT seen */
lt_bool lt_ui_cell_mode(const lt_ui *ui);
int    lt_ui_row_height(const lt_ui *ui);       /* 24 pixel mode, 16 cell mode (scaled) */
double lt_ui_now(const lt_ui *ui);
lt_rect lt_ui_surface(const lt_ui *ui);         /* whole drawing surface */
/* surface minus the menu bar / status bar already declared this frame */
lt_rect lt_ui_client_rect(const lt_ui *ui);

/* ------------------------------------------------------------------ */
/* IDs, focus, input queries                                           */
lt_u32 lt_ui_id(lt_ui *ui, const char *label);
void   lt_ui_push_id(lt_ui *ui, const char *s);
void   lt_ui_push_id_int(lt_ui *ui, int n);
void   lt_ui_pop_id(lt_ui *ui);

/* Global shortcut query: LT_TRUE once per physical key press matching key
 * (lowercase ASCII or LT_KEY_*) and exactly these modifiers (mods may be
 * LT_MOD_SHIFT|CTRL|ALT). The press is consumed. Returns LT_FALSE while a
 * text field is being edited (except F-keys and ctrl/alt combos that the
 * field does not use: ctrl+a/c/v/x/z) and while a modal is open and the
 * caller is outside it. */
lt_bool lt_ui_key_pressed(lt_ui *ui, int key, int mods);
lt_bool lt_ui_text_focused(const lt_ui *ui);    /* a text/number field is being edited */
lt_point lt_ui_mouse(const lt_ui *ui);
lt_bool lt_ui_mouse_down(const lt_ui *ui, int button);
/* LT_MOD_* held at the last mouse/key event */
int lt_ui_mods(const lt_ui *ui);
/* wheel notches (+1 up) received this frame and not consumed by a scroll area */
int lt_ui_wheel(const lt_ui *ui);
/* the same, and consumes them (a zoom gesture that a scroll area under the mouse must not see) */
int lt_ui_wheel_take(lt_ui *ui);
lt_u32 lt_ui_focus(const lt_ui *ui);
void   lt_ui_set_focus(lt_ui *ui, lt_u32 id);
lt_rect lt_ui_last_rect(const lt_ui *ui);       /* rect of the last widget */
lt_u32 lt_ui_last_id(const lt_ui *ui);
lt_bool lt_ui_last_hovered(const lt_ui *ui);
lt_bool lt_ui_last_focused(const lt_ui *ui);
/* clipboard: backend clipboard when available, internal buffer otherwise */
void   lt_ui_clipboard_set(lt_ui *ui, const char *utf8);
char  *lt_ui_clipboard_get(lt_ui *ui);          /* malloc'd (lt_free) or NULL */
/* grey out and ignore input for widgets between begin/end */
void   lt_ui_disabled_begin(lt_ui *ui, lt_bool disabled);
void   lt_ui_disabled_end(lt_ui *ui);

/* ------------------------------------------------------------------ */
/* Raw drawing into the current layer (clipped by the current clip)    */
void lt_ui_draw_fill(lt_ui *ui, lt_rect r, lt_color c);
void lt_ui_draw_rect(lt_ui *ui, lt_rect r, lt_color c);
void lt_ui_draw_round(lt_ui *ui, lt_rect r, int radius, lt_color c);
void lt_ui_draw_hline(lt_ui *ui, int x0, int x1, int y, lt_color c);
void lt_ui_draw_vline(lt_ui *ui, int x, int y0, int y1, lt_color c);
void lt_ui_draw_line(lt_ui *ui, double x0, double y0, double x1, double y1, double w, lt_color c);
void lt_ui_draw_circle_fill(lt_ui *ui, double cx, double cy, double r, lt_color c);
void lt_ui_draw_circle(lt_ui *ui, double cx, double cy, double r, double w, lt_color c);
void lt_ui_draw_arc(lt_ui *ui, double cx, double cy, double r, double a0, double a1,
                    double w, lt_color c);
void lt_ui_draw_poly(lt_ui *ui, const double *xy, int npoints, lt_color c);
void lt_ui_draw_vgrad(lt_ui *ui, lt_rect r, lt_color top, lt_color bottom);
/* pixels must stay valid until lt_ui_end */
void lt_ui_draw_blit(lt_ui *ui, int x, int y, const lt_u32 *px, int w, int h, int pitch);
int  lt_ui_draw_text(lt_ui *ui, int x, int y, const char *utf8, lt_color c);
/* Same with an explicit face (lt_font_id) and size (px at scale 1, e.g.
 * LT_UI_FS_SM). y is the top of a 16*scale px box in which the text is
 * centred, so rows can stay on the 8x16 grid in cell mode. Cell mode ignores
 * font and size. */
int  lt_ui_draw_text_f(lt_ui *ui, int x, int y, const char *utf8, lt_color c, int font, int px);
int  lt_ui_text_width_f(const lt_ui *ui, const char *utf8, int font, int px);
/* default face/size used by lt_ui_draw_text, lt_ui_text_width and the
 * widgets until reset (px 0 = LT_UI_FS_BASE) */
void lt_ui_text_style(lt_ui *ui, int font, int px);
void lt_ui_text_style_reset(lt_ui *ui);
/* aligned in a rect with explicit face/size (ellipsis when too long) */
void lt_ui_draw_text_in_f(lt_ui *ui, lt_rect r, const char *utf8, lt_color c, int align,
                          int font, int px);
/* aligned inside rect (0 left,1 center,2 right), vertically centered, with
 * an ellipsis when too long */
void lt_ui_draw_text_in(lt_ui *ui, lt_rect r, const char *utf8, lt_color c, int align);
void lt_ui_draw_vtext(lt_ui *ui, double x, double y, const char *utf8, double height,
                      double rotation, int align, int valign, double stroke, lt_color c);
int  lt_ui_text_width(const lt_ui *ui, const char *utf8);
/* callback executed at replay time with the clip set to r (for gauges etc.) */
typedef void (*lt_ui_draw_fn)(lt_gfx *g, lt_rect r, void *user);
void lt_ui_draw_custom(lt_ui *ui, lt_rect r, lt_ui_draw_fn fn, void *user);
/* the data behind this frame's custom callbacks is being freed mid-frame (a project closes from
 * a dialog or menu): they are skipped at replay and another frame is drawn */
void lt_ui_forget_custom(lt_ui *ui);
/* fn(user) once this frame's drawing has been replayed (at once outside a frame): frees state that
 * a queued lt_ui_draw_custom callback may still read, e.g. of a tab or dialog closed mid-frame */
void lt_ui_after_frame(lt_ui *ui, void (*fn)(void *user), void *user);
void lt_ui_clip_push(lt_ui *ui, lt_rect r);     /* intersected with current */
void lt_ui_clip_pop(lt_ui *ui);
lt_rect lt_ui_clip_get(const lt_ui *ui);
/* Filled box with 1px border (border omitted in cell mode for short boxes) */
void lt_ui_draw_box(lt_ui *ui, lt_rect r, lt_color fill, lt_color border, int radius);
/* simple glyph icons */
enum { LT_UI_ICON_NONE = 0, LT_UI_ICON_ARROW_RIGHT, LT_UI_ICON_ARROW_DOWN,
       LT_UI_ICON_ARROW_UP, LT_UI_ICON_ARROW_LEFT, LT_UI_ICON_CHECK, LT_UI_ICON_CLOSE,
       LT_UI_ICON_FOLDER, LT_UI_ICON_FILE, LT_UI_ICON_PLUS, LT_UI_ICON_MINUS,
       LT_UI_ICON_DOT, LT_UI_ICON_MENU, LT_UI_ICON_PLAY, LT_UI_ICON_STOP,
       LT_UI_ICON_WARN, LT_UI_ICON_INFO, LT_UI_ICON_SEARCH, LT_UI_ICON_TABLE,
       LT_UI_ICON_GAUGE };
void lt_ui_draw_icon(lt_ui *ui, lt_rect r, int icon, lt_color c);
/* Lucide icons (icons.h LT_ICON_*) can be passed wherever an icon id is
 * taken (tree nodes, icon buttons, lt_ui_draw_icon): wrap them with
 * LT_UI_LUCIDE(). Cell mode shows a placeholder glyph instead. */
#define LT_UI_LUCIDE(id) (1000 + (id))
/* Lucide icon centred in r, glyph size px (0 = 14 scaled), stroke 0 = default */
void lt_ui_draw_lucide(lt_ui *ui, lt_rect r, int lucide_id, int px, double stroke, lt_color c);
/* soft drop shadow under a rounded rect (pixel mode only) */
void lt_ui_draw_shadow(lt_ui *ui, lt_rect r, int radius, int spread, int alpha);

/* ------------------------------------------------------------------ */
/* Layout                                                              */
/* Start a row of n cells. widths[i]: >0 pixels, 0 = automatic (widget's
 * preferred width), <0 = share of the remaining space (-1 weight 1, -2 weight
 * 2...). height: >0 pixels, 0 = default row height, <0 = fill the remaining
 * height of the container (minus |height|-1). The row repeats until the next
 * lt_ui_layout_row call. widths may be NULL for n == 1 (full width). */
/* NOTE: layout row widths/height, layout_space/indent/gap, group_begin height,
 * modal_begin size and grid_begin cell/header sizes are all in px at 100% scale
 * and are scaled by the toolkit. */
/* same as lt_ui_layout_row/gap/space but with sizes already in device pixels
 * (values taken from rects, theme metrics or text measurements) */
void    lt_ui_layout_row_px(lt_ui *ui, int n, const int *widths, int height);
void    lt_ui_layout_gap_px(lt_ui *ui, int x, int y);
void    lt_ui_layout_space_px(lt_ui *ui, int pixels);
void    lt_ui_layout_row(lt_ui *ui, int n, const int *widths, int height);
lt_rect lt_ui_layout_next(lt_ui *ui);           /* rect of the next cell */
void    lt_ui_layout_set_rect(lt_ui *ui, lt_rect r); /* next widget uses exactly r */
void    lt_ui_layout_space(lt_ui *ui, int pixels);   /* vertical gap */
void    lt_ui_layout_indent(lt_ui *ui, int dx);
void    lt_ui_layout_gap(lt_ui *ui, int x, int y);   /* spacing between cells of this container */
int     lt_ui_layout_width(const lt_ui *ui);    /* body width of the current container */
lt_rect lt_ui_layout_body(const lt_ui *ui);
/* nested column: the next cell becomes a container with its own layout */
void    lt_ui_layout_column_begin(lt_ui *ui);
void    lt_ui_layout_column_end(lt_ui *ui);

/* ------------------------------------------------------------------ */
/* Containers                                                          */
/* Scrolling panel filling the next layout cell. Returns LT_TRUE when
 * visible (always call lt_ui_scroll_end when it returns true). The content
 * size is measured from the laid-out widgets (one frame late) unless
 * lt_ui_scroll_set_content overrides it (virtual lists / grids). */
lt_bool lt_ui_scroll_begin(lt_ui *ui, const char *id);
void    lt_ui_scroll_end(lt_ui *ui);
void    lt_ui_scroll_set_content(lt_ui *ui, int w, int h);
void    lt_ui_scroll_get(const lt_ui *ui, int *x, int *y);
void    lt_ui_scroll_set(lt_ui *ui, int x, int y);
/* scroll the innermost scroll container so that r (absolute coords) is visible */
void    lt_ui_scroll_ensure_visible(lt_ui *ui, lt_rect r);
lt_rect lt_ui_scroll_view(const lt_ui *ui);     /* viewport of the innermost scroll container */
/* Group box with a title. height>0 fixed, 0 = grow to fit content. */
lt_bool lt_ui_group_begin(lt_ui *ui, const char *title, int height);
void    lt_ui_group_end(lt_ui *ui);
/* Free rectangle with its own layout (no scroll) and optional background */
void    lt_ui_region_begin(lt_ui *ui, lt_rect r, lt_color bg);
void    lt_ui_region_end(lt_ui *ui);
/* Splitter: divides `area` into panes a and b and draws a draggable divider.
 * vertical != 0: side-by-side panes (divider is a vertical bar), *size is the
 * width of pane a; otherwise stacked, *size is the height of pane a.
 * Returns LT_TRUE while the divider moves. */
lt_bool lt_ui_splitter(lt_ui *ui, const char *id, lt_rect area, int vertical, int *size,
                       int min_a, int min_b, lt_rect *a, lt_rect *b);
/* Menubar / statusbar occupy the top / bottom edge of the surface */
lt_bool lt_ui_menubar_begin(lt_ui *ui);
void    lt_ui_menubar_end(lt_ui *ui);
void    lt_ui_statusbar_begin(lt_ui *ui);
void    lt_ui_statusbar_end(lt_ui *ui);
/* Toolbar occupying the next full-width row of the client area */
void    lt_ui_toolbar_begin(lt_ui *ui);
void    lt_ui_toolbar_end(lt_ui *ui);
void    lt_ui_toolbar_separator(lt_ui *ui);      /* 1px divider cell */

/* Floating windows. flags: */
#define LT_UI_WIN_RESIZE   1
#define LT_UI_WIN_NOCLOSE  2
#define LT_UI_WIN_NOMOVE   4
/* r is read and updated (drag/resize). *open may be NULL. Returns LT_TRUE
 * while visible; then call lt_ui_window_end. */
lt_bool lt_ui_window_begin(lt_ui *ui, const char *title, lt_rect *r, int *open, int flags);
void    lt_ui_window_end(lt_ui *ui);
/* Modal dialog centered on the surface; dims everything below; Esc or the
 * close button clears *open; Enter triggers the LT_UI_BTN_DEFAULT button. */
lt_bool lt_ui_modal_begin(lt_ui *ui, const char *title, int w, int h, int *open);
void    lt_ui_modal_end(lt_ui *ui);
/* Message box. buttons like "OK" or "Save|Discard|Cancel". Returns 0 while
 * undecided, 1..n for the chosen button, -1 if dismissed with Esc or the
 * close button. *open is cleared whenever a result is returned. The first
 * button is the default (Enter). */
int     lt_ui_msgbox(lt_ui *ui, const char *id, const char *title, const char *message,
                     const char *buttons, int *open);

/* ------------------------------------------------------------------ */
/* Basic widgets                                                       */
#define LT_UI_ALIGN_LEFT   0
#define LT_UI_ALIGN_CENTER 1
#define LT_UI_ALIGN_RIGHT  2
void lt_ui_label(lt_ui *ui, const char *text);
void lt_ui_label_ex(lt_ui *ui, const char *text, lt_color color, int align);
void lt_ui_labelf(lt_ui *ui, const char *fmt, ...);
void lt_ui_label_wrap(lt_ui *ui, const char *text, lt_color color); /* grows the row */
void lt_ui_heading(lt_ui *ui, const char *text);
void lt_ui_separator(lt_ui *ui);

#define LT_UI_BTN_PRIMARY  1    /* filled with the primary color */
#define LT_UI_BTN_DEFAULT  2    /* primary + activated by Enter inside a modal */
#define LT_UI_BTN_FLAT     4    /* no background until hovered (toolbars) */
#define LT_UI_BTN_DISABLED 8
#define LT_UI_BTN_DANGER   16
#define LT_UI_BTN_ACCENT   32
#define LT_UI_BTN_SELECTED 64   /* drawn pressed/on (toggle look) */
lt_bool lt_ui_button(lt_ui *ui, const char *label);
lt_bool lt_ui_button_ex(lt_ui *ui, const char *label, int flags);
lt_bool lt_ui_icon_button(lt_ui *ui, int icon, const char *tooltip, int flags);
lt_bool lt_ui_toggle(lt_ui *ui, const char *label, int *on);     /* returns changed */
lt_bool lt_ui_checkbox(lt_ui *ui, const char *label, int *v);    /* returns changed */
lt_bool lt_ui_radio(lt_ui *ui, const char *label, int *v, int value);

/* Single-line text input (edits buf in place). Returns LT_UI_CHANGED /
 * LT_UI_SUBMIT / LT_UI_CANCEL flags. Esc restores the text from focus time
 * and drops the focus; Esc on an untouched field is left for the enclosing
 * dialog/popup. Enter returns LT_UI_SUBMIT and is not consumed, so a
 * LT_UI_BTN_DEFAULT button in the same dialog fires as well. Ctrl+A/C/X/V/Z,
 * Shift+arrows, Home/End and Ctrl+arrows (words) work; the clipboard is the
 * backend's when available. */
#define LT_UI_TEXT_NUMERIC   1   /* digits + - . e only */
#define LT_UI_TEXT_READONLY  2
#define LT_UI_TEXT_SELECTALL 4   /* select everything when focused */
#define LT_UI_TEXT_PASSWORD  8
int lt_ui_text(lt_ui *ui, const char *id, char *buf, size_t cap, int flags);
/* Number field: click to type, drag horizontally to scrub, Up/Down arrows
 * step (Shift = x10), clamps to [min,max]; digits = max decimals shown. */
int lt_ui_number(lt_ui *ui, const char *id, double *v, double min, double max,
                 double step, int digits);
/* Horizontal slider with the value printed at its right end */
lt_bool lt_ui_slider(lt_ui *ui, const char *id, double *v, double min, double max, int digits);

/* Combo box: popup list, keyboard navigation and type-ahead. Returns changed. */
lt_bool lt_ui_combo(lt_ui *ui, const char *id, int *sel, const char *const *items, int n);
lt_bool lt_ui_combo_cb(lt_ui *ui, const char *id, int *sel, int n,
                       const char *(*get)(void *user, int i), void *user);
/* List box filling the next layout cell. Returns 0, LT_UI_CHANGED, and
 * LT_UI_ACTIVATED (double click / Enter) bits. Virtualized: only visible
 * rows are drawn. */
int lt_ui_listbox(lt_ui *ui, const char *id, int *sel, const char *const *items, int n);
int lt_ui_listbox_cb(lt_ui *ui, const char *id, int *sel, int n,
                     const char *(*get)(void *user, int i), void *user);

/* Tree view. flags in, result bits out. If the result has LT_UI_TREE_OPEN the
 * caller emits the children and then calls lt_ui_tree_end(). */
#define LT_UI_TREE_LEAF      1   /* in: no expander */
#define LT_UI_TREE_SELECTED  2   /* in: highlight row */
#define LT_UI_TREE_FORCE_OPEN 4  /* in: treat as open (search results) */
#define LT_UI_TREE_DEFAULT_OPEN 8 /* in: initial state open */
#define LT_UI_TREE_DISABLED  16  /* in: muted, hover ignored (still clickable) */
#define LT_UI_TREE_OPEN      1   /* out */
#define LT_UI_TREE_CLICKED   2   /* out: row clicked (or Enter) */
#define LT_UI_TREE_ACTIVATED 4   /* out: double click */
int  lt_ui_tree_node(lt_ui *ui, const char *label, int icon, int flags);
/* icon may be LT_UI_LUCIDE(x); icon_color tints it */
int  lt_ui_tree_node_ex(lt_ui *ui, const char *label, int icon, lt_color icon_color, int flags);
void lt_ui_tree_end(lt_ui *ui);
/* expand (open != 0) or collapse every tree node; takes a few frames to reach
 * nodes that only appear once their parent opens */
void lt_ui_tree_set_all(lt_ui *ui, int open);

/* Tab bar occupying the next row: begin, one lt_ui_tab per tab, end. */
#define LT_UI_TAB_CLOSABLE 1
#define LT_UI_TAB_DIRTY    2
#define LT_UI_TAB_CLOSED   2    /* result bit: close button / middle click */
#define LT_UI_TAB_SELECTED 1    /* result bit: tab was clicked */
void lt_ui_tabbar_begin(lt_ui *ui, const char *id, int *active);
int  lt_ui_tab(lt_ui *ui, const char *title, int flags);
int  lt_ui_tab_icon(lt_ui *ui, const char *title, int icon, int flags);
int  lt_ui_tabbar_end(lt_ui *ui);               /* returns the active index */
/* Full-width row of text tabs with the active one underlined (settings
 * dialogs). Returns true when *sel changed. */
lt_bool lt_ui_tabs_flat(lt_ui *ui, const char *id, const char *const *labels, int n, int *sel);

/* Menus. Titles may contain '&' (Alt+letter access key). */
#define LT_UI_MENU_DISABLED 1
#define LT_UI_MENU_CHECKED  2
lt_bool lt_ui_menu_begin(lt_ui *ui, const char *title);      /* inside a menubar */
void    lt_ui_menu_end(lt_ui *ui);
/* vertical divider between groups of top-level menus (menubar only) */
void    lt_ui_menubar_separator(lt_ui *ui);
lt_bool lt_ui_menu_item(lt_ui *ui, const char *label, const char *shortcut, int flags);
void    lt_ui_menu_separator(lt_ui *ui);
lt_bool lt_ui_submenu_begin(lt_ui *ui, const char *label, int flags);
void    lt_ui_submenu_end(lt_ui *ui);
/* Context menu opened by right click inside area (e.g. lt_ui_last_rect()).
 * Fill with menu items; end with lt_ui_context_end. */
lt_bool lt_ui_context_begin(lt_ui *ui, const char *id, lt_rect area);
void    lt_ui_context_end(lt_ui *ui);
void    lt_ui_context_open(lt_ui *ui, const char *id, int x, int y);
void    lt_ui_popups_close(lt_ui *ui);
lt_bool lt_ui_popup_open(const lt_ui *ui);       /* any menu/combo/context popup open */

/* Invisible interactive area for custom-drawn widgets (rows of a virtual table, strips):
 * LT_UI_HIT_* bits. Popups, modals and layers are respected like for any widget. */
#define LT_UI_HIT_HOVER  1
#define LT_UI_HIT_DOWN   2   /* pressed inside and still held */
#define LT_UI_HIT_PRESS  4   /* pressed this frame */
#define LT_UI_HIT_DOUBLE 8   /* the press is a double click */
int lt_ui_hit(lt_ui *ui, const char *id, lt_rect r);
/* Tooltip for the previous widget (shown after 500 ms hover) */
void lt_ui_tooltip(lt_ui *ui, const char *text);
/* Toast: kind 0 info, 1 success, 2 warning, 3 error; auto-dismiss after 4 s */
void lt_ui_toast(lt_ui *ui, int kind, const char *text);

/* Status widgets */
void    lt_ui_progress(lt_ui *ui, double frac, const char *text); /* frac<0: busy */
void    lt_ui_spinner(lt_ui *ui);
lt_bool lt_ui_color_swatch(lt_ui *ui, lt_color c);             /* returns clicked */
lt_bool lt_ui_color_edit(lt_ui *ui, const char *id, lt_color *c); /* swatch + RGB sliders popup */
void    lt_ui_image(lt_ui *ui, const lt_u32 *px, int w, int h, int pitch); /* cell mode: placeholder */
lt_bool lt_ui_link(lt_ui *ui, const char *text);

/* ------------------------------------------------------------------ */
/* Grid (table editor helper)                                          */
/* Flags returned by lt_ui_grid_end */
#define LT_UI_GRID_SELECTION  1  /* selection or cursor changed */
#define LT_UI_GRID_EDIT_BEGIN 2  /* inline edit started: see lt_ui_grid_edit_* */
#define LT_UI_GRID_EDIT_COMMIT 4 /* Enter/Tab/click-away committed the edit */
#define LT_UI_GRID_EDIT_CANCEL 8
#define LT_UI_GRID_CLICKED    16 /* cell clicked (even if selection unchanged) */
#define LT_UI_GRID_ACTIVATED  32 /* double click on a cell */
/* Takes the next layout cell as viewport. Row/column headers stay pinned.
 * Returns LT_TRUE if drawing is needed. Always call lt_ui_grid_end if true. */
lt_bool lt_ui_grid_begin(lt_ui *ui, const char *id, int rows, int cols, int cell_w,
                         int cell_h, int row_header_w, int col_header_h);
/* inclusive visible index ranges */
void    lt_ui_grid_visible(const lt_ui *ui, int *r0, int *r1, int *c0, int *c1);
lt_rect lt_ui_grid_cell_rect(const lt_ui *ui, int r, int c);
/* default cell: background, centered text, selection tint, cursor outline */
void    lt_ui_grid_cell(lt_ui *ui, int r, int c, const char *text, lt_color bg, lt_color fg);
/* kind 0 = row header (index r), 1 = column header (index c) */
void    lt_ui_grid_header(lt_ui *ui, int kind, int index, const char *text);
void    lt_ui_grid_corner(lt_ui *ui, const char *text);
/* overlay marker (live cursor): outline of the cell, thickness in px */
void    lt_ui_grid_mark(lt_ui *ui, int r, int c, lt_color color, int thickness);
int     lt_ui_grid_end(lt_ui *ui);
/* selection model (valid between begin/end) */
lt_bool lt_ui_grid_selected(const lt_ui *ui, int r, int c);
void    lt_ui_grid_cursor(const lt_ui *ui, int *r, int *c);
/* bounding box of the selection; returns the number of selected cells */
int     lt_ui_grid_sel_bounds(const lt_ui *ui, int *r0, int *c0, int *r1, int *c1);
void    lt_ui_grid_select(lt_ui *ui, int r0, int c0, int r1, int c1); /* replaces selection */
void    lt_ui_grid_set_cursor(lt_ui *ui, int r, int c);
lt_bool lt_ui_grid_hover(const lt_ui *ui, int *r, int *c);
/* inline editing */
lt_bool lt_ui_grid_editing(const lt_ui *ui, int *r, int *c);
const char *lt_ui_grid_edit_text(const lt_ui *ui);
lt_bool lt_ui_grid_edit_wants_text(const lt_ui *ui); /* started without a typed char */
void    lt_ui_grid_edit_begin(lt_ui *ui, int r, int c, const char *text);
void    lt_ui_grid_edit_set_text(lt_ui *ui, const char *text);

/* ------------------------------------------------------------------ */
/* File dialog (own implementation; lt_plat_list_dir / lt_plat_is_dir)  */
#define LT_UI_FILE_OPEN 0
#define LT_UI_FILE_SAVE 1
typedef struct lt_ui_filedlg {
    int  open;               /* LT_TRUE while shown */
    int  mode;
    char dir[512];
    char name[256];
    char path[768];          /* result after lt_ui_file_dialog returned 1 */
    char filters[384];       /* "Tune files|*.msq;*.xml|All files|*" */
    int  filter;
    int  show_hidden;
    /* private */
    char **names;
    int  *is_dir;
    int  n_names;
    int  sel;
    int  dirty;
    int  confirm;
    char pathbar[512];
    char err[96];
    int  fresh;
    int  list_filter;
} lt_ui_filedlg;
void lt_ui_filedlg_init(lt_ui_filedlg *d);
void lt_ui_filedlg_free(lt_ui_filedlg *d);
/* mode LT_UI_FILE_OPEN/SAVE; start_dir NULL = current directory */
void lt_ui_filedlg_open(lt_ui_filedlg *d, int mode, const char *start_dir,
                        const char *default_name, const char *filters);
/* Call every frame; draws a modal while d->open. Returns 1 when a path was
 * chosen (d->path), -1 when cancelled, 0 otherwise. */
int  lt_ui_file_dialog(lt_ui *ui, lt_ui_filedlg *d, const char *title);

/* ------------------------------------------------------------------ */
/* Multi-line text editor (ui_multi.c). The caller owns the NUL terminated buffer (capacity cap)
 * and a lt_ui_multi (caret, selection, undo snapshots). The widget fills the next layout cell,
 * so give the row a height (or <0 to fill). Returns LT_UI_CHANGED when the text was edited.
 * classify (optional) writes one class per byte of text: 0 text, 1 keyword, 2 string,
 * 3 comment, 4 number, 5 builtin name, 6 error. Ctrl+Enter is not used by the editor. */
#define LT_UI_MULTI_READONLY 1   /* selectable, not editable */
#define LT_UI_MULTI_LINENUMS 2   /* line number gutter */
#define LT_UI_MULTI_UNDO 24
typedef void (*lt_ui_classify_fn)(const char *text, size_t len, unsigned char *cls, void *user);
typedef struct lt_ui_multi {
    int cursor, anchor;          /* byte offsets, selection = [min, max) */
    int want_col;                /* column kept while moving up and down, -1 = none */
    int dragging, follow;
    int last_kind, last_pos;     /* undo grouping */
    int nsnap;
    char *snap[LT_UI_MULTI_UNDO];
    int snap_cur[LT_UI_MULTI_UNDO];
    double blink0;
} lt_ui_multi;
void lt_ui_multi_init(lt_ui_multi *m);
void lt_ui_multi_free(lt_ui_multi *m);
void lt_ui_multi_reset(lt_ui_multi *m, const char *text);   /* new text: caret to start, undo cleared */
int  lt_ui_text_multi(lt_ui *ui, const char *id, lt_ui_multi *m, char *buf, size_t cap, int flags,
                      lt_ui_classify_fn classify, void *user);

#endif /* LT_UI_H */
