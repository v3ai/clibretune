/* view_kit.h - drawing and widget helpers shared by the view screens (table
 * editor, gauges, AutoTune, curve editor, data log, INI dialog content).
 *
 * They sit on top of the toolkit (ui.h) and the gfx text/icon API (gui.h):
 * Plex typography, Lucide icons, icon toolbars and card panels.  Every helper
 * degrades to the plain toolkit calls in cell (tty) mode, where there are no
 * icons and no TrueType text.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef LT_VIEW_KIT_H
#define LT_VIEW_KIT_H

#include "../gui/ui.h"

/* ---- text ---- */
/* width in pixels of s in font id (an lt_font_id) at px */
int  lt_vk_text_w(lt_ui *ui, int font, double px, const char *s);
/* draw s inside r (align 0 left, 1 center, 2 right), vertically centred and
 * elided with "..." when too long. */
void lt_vk_text(lt_ui *ui, lt_rect r, int font, double px, const char *s, lt_color c, int align);
/* same, but the baseline of the text sits at y (no vertical centring) */
void lt_vk_text_at(lt_ui *ui, int x, int y, int font, double px, const char *s, lt_color c);

/* ---- icons ---- */
/* Lucide icon (LT_ICON_*) of size px centred in r; no-op in cell mode */
void lt_vk_icon(lt_ui *ui, lt_rect r, int icon, int size_px, double stroke, lt_color c);

/* ---- shapes ---- */
/* card: rounded surface with a 1px border and a soft drop shadow */
void lt_vk_card(lt_ui *ui, lt_rect r, lt_color fill, lt_color border, int radius);
/* horizontal pill (radius = h/2) */
void lt_vk_pill(lt_ui *ui, lt_rect r, lt_color fill, lt_color border);
/* small uppercase section caption with letter spacing, e.g. "INJECTION" */
void lt_vk_caption(lt_ui *ui, lt_rect r, const char *s, lt_color c);

/* progress bar: rounded track with a fill of frac (0..1) */
void lt_vk_bar(lt_ui *ui, lt_rect r, double frac, lt_color fill, lt_color track);
/* ring gauge: arc of frac (0..1) clockwise from 12 o'clock over a faint track */
void lt_vk_ring(lt_ui *ui, double cx, double cy, double radius, double w, double frac,
                lt_color color, lt_color track);
/* traffic-light colour for a 0..100 score (green / amber / orange / red) */
lt_color lt_vk_score_color(int score);

/* ---- toolbar ---- */
#define LT_VK_END      0   /* terminator */
#define LT_VK_BTN      1   /* icon button with optional label / key hint */
#define LT_VK_SEP      2   /* vertical divider */
#define LT_VK_SPACER   3   /* flexible gap: following items are right-aligned */
typedef struct lt_vk_item {
    int kind;
    int icon;               /* LT_ICON_* or 0 */
    const char *label;      /* text next to the icon (may be NULL) */
    const char *key;        /* key hint chip, e.g. "=" (may be NULL) */
    const char *tip;        /* tooltip */
    int on;                 /* toggled on */
    int disabled;
    int accent;             /* LT_UI_BTN_ACCENT look (burn) */
    lt_color fill;          /* custom solid fill (0 = toolkit look): green Start button etc. */
    int pin;                /* stays on the bar when the row is too narrow (Burn) */
    const char *menu;       /* name in the overflow menu (NULL: label, else tip) */
} lt_vk_item;
/* Lays the items out in one row of height h taken from the layout. In cell
 * mode every button is drawn as plain text (label, else key, else tip).
 * When the row is too narrow, buttons that are not pinned are taken off the
 * bar from the right and listed in a "\xc2\xbb" menu at its right end; picking
 * one there counts as clicking it.
 * Returns the index of the clicked button, or -1. */
int lt_vk_toolbar(lt_ui *ui, const lt_vk_item *items, int h);

/* same, laid out inside row (does not touch the layout) */
int lt_vk_toolbar_in(lt_ui *ui, const lt_vk_item *items, lt_rect row);

/* standalone icon button of w x h taken from the layout (w <= 0: 28 px) */
lt_bool lt_vk_icon_btn(lt_ui *ui, int icon, const char *tip, int on, int w, int h);

#endif
