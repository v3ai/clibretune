/* ui_priv.h - internals shared by the ui_*.c files. Not a public API.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef LT_UI_PRIV_H
#define LT_UI_PRIV_H

#include <string.h>
#include "ui.h"

#define UI_MAX_LAYERS 24
#define UI_MAX_ID 48
#define UI_MAX_LAYOUTS 24
#define UI_MAX_CLIPS 40
#define UI_MAX_POPUPS 6
#define UI_MAX_SCROLL 12
#define UI_MAX_LSTACK 12
#define UI_EVQ 64
#define UI_MAX_TOASTS 5
#define UI_TIP_DELAY_MS 500
#define UI_TOAST_MS 4000
#define UI_DBL_MS 400

/* layer kinds */
enum { UL_MAIN = 0, UL_WINDOW, UL_POPUP, UL_MODAL, UL_TIP };

/* draw command types */
enum { UC_CLIP = 0, UC_FILL, UC_RECT, UC_ROUND, UC_HLINE, UC_VLINE, UC_LINE, UC_TEXT,
       UC_VTEXT, UC_CFILL, UC_CIRCLE, UC_ARC, UC_POLY, UC_VGRAD, UC_BLIT, UC_CUSTOM,
       UC_ICON, UC_FRRECT, UC_SHADOW };

typedef struct ui_cmd {
    int type;
    lt_rect r;
    lt_color c, c2;
    int i[4];
    double d[6];
    long off;
    const lt_u32 *px;
    lt_ui_draw_fn fn;
    void *user;
} ui_cmd;

typedef struct ui_layer {
    lt_u32 id;
    int kind, z, seq;
    lt_rect rect;
    int blocked;
    int bg_cmd;
    lt_vec cmds;
} ui_layer;

typedef struct ui_prev_layer {
    lt_u32 id;
    int kind, z, seq;
    lt_rect rect;
} ui_prev_layer;

typedef struct ui_layout {
    lt_rect body;
    int x, row_y, row_h;
    int ncols, col;
    int widths[LT_UI_MAX_COLS];
    int spec_h;
    int started;
    int indent;
    int max_x, max_y;
    int has_override;
    lt_rect override_r;
    int sp_x, sp_y;
    int fill_slack;      /* height that fill rows (height < 0) took beyond one row */
} ui_layout;

/* persistent per-id state; pointers are stable until ui_state_gc */
typedef struct ui_state {
    lt_u32 id;
    lt_u32 frame;
    double d[4];
    int i[6];
} ui_state;

typedef struct ui_scroll {
    lt_u32 id;
    lt_rect view, inner;
    int sx, sy, cw, ch;
    int ovr, ovr_w, ovr_h;
    int vbar, hbar;
    int base_clip;
    int fill_top;        /* cell mode: a fill-height row of the enclosing modal's own layout */
} ui_scroll;

typedef struct ui_popup {
    lt_u32 id;
    int x, y;
    lt_rect anchor;
    int kind;       /* 0 menu 1 combo 2 context 3 colour */
    int right;      /* submenu: open to the right of anchor */
    int minw;
    int maxh;       /* visible height; taller content scrolls (state d[0] = offset) */
    int kbd;        /* the highlight moved by the keyboard: scroll it into view */
} ui_popup;

/* queued keyboard event (key or text), consumed by widgets in order */
typedef struct ui_evq {
    int is_text;
    int key, mods;
    char text[8];
    int used;
} ui_evq;

typedef struct ui_edit {
    lt_u32 id;
    char *buf;
    size_t cap;
    int cursor, anchor;   /* byte offsets; selection = [min,max) */
    int scroll;           /* pixels */
    int flags;
    char orig[512];
    int orig_ok;          /* orig holds the full original text */
    int dragging;
    double blink0;        /* time of last interaction (caret blink) */
    int numeric_cap;
} ui_edit;

typedef struct ui_toast {
    int kind;
    double t0;
    char text[160];
} ui_toast;

typedef struct ui_grid {
    lt_u32 id;
    int used;
    int rows, cols, cw, chh, rhw, chdr;
    lt_rect view, inner, parent_clip;
    int ox, oy, sx, sy;
    int cur_r, cur_c, anc_r, anc_c;
    lt_u8 *mask;
    int mask_cap;
    int serial, last_serial;
    int drag;
    int edit, edit_r, edit_c, edit_wants;
    char edit_buf[64];
    int hov_r, hov_c;
    int flags_out;
    int r0, r1, c0, c1;
    int in_frame;
    int lru;
} ui_grid;

typedef struct ui_grp {
    lt_u32 id;
    lt_rect r;
    int auto_h;
    int hdr;
} ui_grp;

struct lt_ui {
    lt_theme theme;
    int pct;                       /* UI scale in percent (100..300) */
    lt_bool cell;
    lt_backend *be;
    lt_gfx *g;
    double now, prev_now;
    int sw, sh;
    int frame_no;
    int in_frame;
    /* input */
    int mx, my;
    int mbtn, mpressed, mreleased, mpress_x, mpress_y;
    int dbl;
    double last_press_ms;
    int last_press_x, last_press_y;
    int wheel;
    int mods;
    ui_evq q[UI_EVQ];
    int nq;
    int quit;
    int dirty, extra, anim;
    double wake_in;                /* ms until next animation tick, <0 none */
    /* ids and interaction */
    lt_u32 idstack[UI_MAX_ID];
    int nid;
    lt_u32 hover, active, focus, last_id;
    lt_rect last_rect;
    int last_hover;
    int press_claimed;
    int popup_dismiss;
    int disabled;
    int kbd_nav;
    /* focus cycling */
    lt_u32 first_focusable, last_focusable, prev_focusable;
    int focus_seen, focus_miss, focus_req, focus_req_kind, focus_kind, wrap_back;
    lt_u32 focus_reveal;           /* focus moved by the keyboard: scroll this widget into view */
    int tab_taken;
    /* hover timing (tooltips) */
    lt_u32 tip_id, tip_id_prev;
    double tip_since;
    char tip_text[200];
    lt_rect tip_rect;
    int tip_want;
    /* layers */
    ui_layer layers[UI_MAX_LAYERS];
    int nlayers;
    int lstack[UI_MAX_LSTACK];
    int nlstack;
    ui_prev_layer prev[UI_MAX_LAYERS];
    int nprev;
    lt_u32 hover_layer;
    int modal_z_top, modal_seq, popup_seq;
    lt_u32 win_order[24];
    int nwin;
    /* clip / layout / scroll stacks */
    lt_rect clip[UI_MAX_CLIPS];
    int nclip;
    ui_layout lay[UI_MAX_LAYOUTS];
    int nlay;
    ui_scroll scr[UI_MAX_SCROLL];
    int nscr;
    /* text / double arenas for draw commands */
    lt_buf tbuf;
    lt_vec dpool;
    /* state table */
    ui_state **tab;
    int tab_cap, tab_count;
    /* popups */
    ui_popup popups[UI_MAX_POPUPS];
    int npopups;
    int menu_depth;
    int menu_hi[UI_MAX_POPUPS];
    int menu_cnt[UI_MAX_POPUPS];
    int menu_idx[UI_MAX_POPUPS];
    int menu_close_req;
    int bar_x, bar_y, bar_h, in_bar;
    int menu_activate;             /* Enter pressed in a menu */
    int lmx, lmy;                  /* mouse position at the previous frame */
    int pop_need[UI_MAX_POPUPS];   /* widest item per open popup depth */
    unsigned char menu_dis[UI_MAX_POPUPS][64];
    lt_u32 bar_ids[16];
    int nbar;
    char typeahead[32];
    double typeahead_t;
    /* default text style (lt_ui_text_style) */
    int tfont, tpx;
    /* shell regions */
    int used_top, bottom_used;
    /* text editing */
    ui_edit edit;
    lt_buf clipbuf;
    char numbuf[64];
    lt_u32 numedit;
    /* tab bar */
    lt_u32 tab_id;
    int tab_idx, tab_x, *tab_active_ptr;
    lt_rect tab_rect;
    /* tree */
    int tree_depth, tree_cmd, tree_cmd_left;
    /* msg_q */
    ui_toast msg_q[UI_MAX_TOASTS];
    int ntoasts;
    /* modal bookkeeping */
    int modal_depth;
    ui_grp grp[8];
    int ngrp;
    int *modal_stack[4];
    int nmodal;
    /* cell mode: fit a modal to its content (measured while it is drawn, used next frame) */
    lt_u32 modal_id[4];
    int modal_lay[4], modal_fill[4], modal_slackn[4], modal_slack[4];
    int modal_scr[4];              /* the modal's content did not fit: it is drawn in a scroll area */
    lt_u32 prev_tree;
    /* grids */
    ui_grid grids[4];
    int ngrids;
    ui_grid *grid;
};

/* ---- theme (ui_theme.c, ui_theme_data.c) ---- */
#define LT_THEME_NTOK 57
#define LT_THEME_NROWS 9
typedef struct lt_theme_row {
    const char *name;
    int dark;
    lt_u32 col[LT_THEME_NTOK];       /* 0xRRGGBB, 0xFFFFFFFF = unset */
} lt_theme_row;
extern const lt_theme_row lt_theme_rows[LT_THEME_NROWS];
void ui_theme_metrics(lt_theme *t, int cell, int pct);
void ui_layout_row_dev(lt_ui *ui, int n, const int *widths, int height);
lt_bool ui_modal_begin_dev(lt_ui *ui, const char *title, int w, int h, int *open);
void ui_layout_space_dev(lt_ui *ui, int px);
int  ui_bscale(const lt_ui *ui);
int  ui_text_box_h(const lt_ui *ui);

/* ---- core (ui.c) ---- */
ui_state *ui_state_get(lt_ui *ui, lt_u32 id);
ui_cmd   *ui_cmd_push(lt_ui *ui, int type);
lt_rect   ui_clip_cur(const lt_ui *ui);
int       ui_over(lt_ui *ui, lt_rect r);              /* mouse in r & clip & layer hover ok */
int       ui_layer_ok(const lt_ui *ui);               /* current layer receives input */
ui_layer *ui_layer_cur(lt_ui *ui);
ui_layer *ui_layer_begin(lt_ui *ui, lt_u32 id, int kind, int z, lt_rect rect);
void      ui_layer_end(lt_ui *ui);
int       ui_layer_z(const lt_ui *ui);
lt_u32    ui_hash(lt_u32 seed, const char *s);
const char *ui_label_visible(const char *label, char *out, size_t cap);
lt_u32    ui_id_label(lt_ui *ui, const char *label);
void      ui_set_last(lt_ui *ui, lt_u32 id, lt_rect r);
ui_layout *ui_lay(lt_ui *ui);
void      ui_layout_push(lt_ui *ui, lt_rect body);
void      ui_layout_pop(lt_ui *ui);
lt_rect   ui_next(lt_ui *ui, int pref_w);
lt_rect   ui_next_full(lt_ui *ui, int height);
int       ui_hover_only(lt_ui *ui, lt_u32 id, lt_rect r);
void      ui_text_in(lt_ui *ui, lt_rect r, const char *s, lt_color c, int align);
void      ui_text_in_f(lt_ui *ui, lt_rect r, const char *s, lt_color c, int align, int font, int px);
int       ui_fit_text(const lt_ui *ui, const char *s, int maxw, char *out, size_t cap, int font, int px);
int       ui_text_w(const lt_ui *ui, const char *s, int font, int px);   /* explicit face */
int       ui_line_h(const lt_ui *ui);       /* line pitch of the default text style */
int       ui_font_px(const lt_ui *ui, int px);  /* px (0 = default) scaled */
int       ui_prefix_w(const lt_ui *ui, const char *s, int nbytes);       /* width of s[0..nbytes) */
lt_rect   ui_text_rect(const lt_ui *ui, lt_rect r, int pad);
void      ui_field_box(lt_ui *ui, lt_rect r, lt_color fill, lt_color border, int focused);
void      ui_request(lt_ui *ui);       /* ask for an extra frame */
void      ui_cell_shadow(lt_ui *ui, lt_rect r);      /* cell mode: text-mode drop shadow below/right of r */
void      ui_cell_underline(lt_ui *ui, lt_rect r);   /* cell mode: underline the cells of r */
void      ui_cell_bold(lt_ui *ui, lt_rect r);        /* cell mode: bold text in the cells of r */
void      ui_animate(lt_ui *ui, int ms);

/* interaction */
#define UI_HOVER 1
#define UI_ACTIVE 2
#define UI_CLICK 4
#define UI_PRESS 8
#define UI_DBLCLICK 16
#define UI_FOCUSABLE 32
#define UI_RIGHT 64
#define UI_RELEASE 128
#define UI_KIND(k) ((k) << 8)
#define UI_K_BUTTON 1
#define UI_K_TEXT 2
#define UI_K_TREE 3
#define UI_K_LIST 4
#define UI_K_OTHER 5
int  ui_interact(lt_ui *ui, lt_u32 id, lt_rect r, int flags);
void ui_focusable(lt_ui *ui, lt_u32 id, int kind);
int  ui_key_take(lt_ui *ui, int key, int *mods_out);
int  ui_key_take_exact(lt_ui *ui, int key, int mods);
int  ui_enter_pressed(lt_ui *ui);
int  ui_pressed(const lt_ui *ui, int button);   /* press event available */
void ui_focus_ring(lt_ui *ui, lt_rect r, int radius);
int  ui_disabled(const lt_ui *ui);
lt_color ui_dim(lt_ui *ui, lt_color c);
lt_color ui_text_on(lt_color preferred, lt_color alt, lt_color bg);

/* ---- popups (ui_menu.c) ---- */
#define UI_ITEM_ACT 1
#define UI_ITEM_HI 2
#define UI_ITEM_OVER 4
int  ui_popup_item(lt_ui *ui, int depth, lt_rect r, int disabled);
void ui_popup_need(lt_ui *ui, int w);
int  ui_popup_find(const lt_ui *ui, lt_u32 id);
void ui_popup_open(lt_ui *ui, int depth, lt_u32 id, int kind, lt_rect anchor, int right, int x, int y);
void ui_popup_close_from(lt_ui *ui, int idx);
/* begin the popup layer for popups[idx]; returns inner layout started */
void ui_popup_begin(lt_ui *ui, int idx, int min_w, int est_h);
void ui_popup_end(lt_ui *ui, int idx);

/* ---- widgets / containers ---- */
int  ui_wrap_lines(const lt_ui *ui, const char *text, int maxw, const char **starts, int *lens, int cap);

/* ---- text (ui_text.c) ---- */
int  ui_edit_widget(lt_ui *ui, lt_u32 id, lt_rect r, char *buf, size_t cap, int flags, int draw_box);
void ui_edit_end(lt_ui *ui);
void ui_edit_select_all(lt_ui *ui);

/* ---- grids (ui_grid.c) ---- */
void ui_grid_frame_end(lt_ui *ui);

#endif
