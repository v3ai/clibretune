/* ui_theme.c - theme palettes, metrics and color helpers.
 * Dark palette values come from docs/spec/10-ui.md section 1.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stddef.h>
#include <string.h>
#include "ui_priv.h"

#define H(x) ((lt_color)(0xFF000000UL | (x)))

lt_color lt_color_mix(lt_color a, lt_color b, int t256)
{
    int ch, out = 0;
    if (t256 < 0) { t256 = 0; }
    if (t256 > 256) { t256 = 256; }
    for (ch = 0; ch < 3; ch++) {
        int sh = ch * 8;
        int ca = (int)((a >> sh) & 0xFF), cb = (int)((b >> sh) & 0xFF);
        int v = (ca * (256 - t256) + cb * t256) / 256;
        out |= (v & 0xFF) << sh;
    }
    return (lt_color)(0xFF000000UL | (lt_u32)out);
}

lt_color lt_color_contrast(lt_color bg)
{
    int lum = (LT_COLOR_R(bg) * 299 + LT_COLOR_G(bg) * 587 + LT_COLOR_B(bg) * 114) / 1000;
    return lum > 140 ? H(0x101010) : H(0xFFFFFF);
}

/* ------------------------------------------------------------------ */
/* heat maps                                                           */
static const lt_u32 heat_ts[] = { 0x0000FF, 0x00FFFF, 0x00FF00, 0xFFFF00, 0xFF8000, 0xFF0000 };
static const lt_u32 heat_gray[] = { 0x000000, 0xFFFFFF };
static const lt_u32 heat_pastel[] = { 0x90CAF9, 0xA5D6A7, 0xFFF59D, 0xFFCC80, 0xEF9A9A };
static const lt_u32 heat_thermal[] = { 0x000000, 0x4B0082, 0xCC0000, 0xFF8000, 0xFFFF00, 0xFFFFFF };
static const lt_u32 heat_viridis[] = { 0x440154, 0x3B528B, 0x21918C, 0x5EC962, 0xFDE725 };
static const lt_u32 heat_plasma[] = { 0x0D0887, 0x7E03A8, 0xCC4678, 0xF89441, 0xF0F921 };

lt_color lt_heat_color(int scheme, double t)
{
    const lt_u32 *st;
    int n, i;
    double f;
    switch (scheme) {
    case 1: st = heat_gray; n = 2; break;
    case 2: st = heat_pastel; n = 5; break;
    case 3: st = heat_thermal; n = 6; break;
    case 4: st = heat_viridis; n = 5; break;
    case 5: st = heat_plasma; n = 5; break;
    default: st = heat_ts; n = 6; break;
    }
    if (!(t > 0.0)) { t = 0.0; }
    if (t > 1.0) { t = 1.0; }
    f = t * (double)(n - 1);
    i = (int)f;
    if (i >= n - 1) { return H(st[n - 1]); }
    return lt_color_mix(H(st[i]), H(st[i + 1]), (int)((f - (double)i) * 256.0 + 0.5));
}

/* ------------------------------------------------------------------ */
/* metrics                                                             */
void ui_theme_metrics(lt_theme *t, int cell, int scale)
{
    int s = scale < 1 ? 1 : (scale > 3 ? 3 : scale);
    if (cell) {
        t->row_h = 16; t->spacing_x = 8; t->spacing_y = 0; t->padding = 8; t->pad_x = 8;
        t->radius = 0; t->scrollbar_w = 8; t->indent = 16; t->menubar_h = 16;
        t->tbar_h = 16; t->tab_h = 16; t->statusbar_h = 16; t->splitter_w = 8;
        t->title_h = 16; t->tree_row_h = 16; t->radius_lg = 0;
        return;
    }
    t->row_h = 24 * s; t->spacing_x = 6 * s; t->spacing_y = 4 * s; t->padding = 8 * s;
    t->pad_x = 8 * s; t->radius = 3 * s; t->scrollbar_w = 10 * s; t->indent = 16 * s;
    t->menubar_h = 26 * s; t->tbar_h = 34 * s; t->tab_h = 28 * s; t->statusbar_h = 26 * s;
    t->splitter_w = 4 * s; t->title_h = 42 * s; t->tree_row_h = 20 * s; t->radius_lg = 4 * s;
}

/* ------------------------------------------------------------------ */
void lt_theme_dark(lt_theme *t)
{
    memset(t, 0, sizeof(*t));
    t->name = "dark"; t->dark = 1;
    t->bg_app = H(0x1A1A1A); t->surface = H(0x242424); t->elevated = H(0x2D2D2D);
    t->input = H(0x2A2A2A); t->hover = H(0x383838); t->active = H(0x424242);
    t->selected = H(0x0D47A1); t->dash_bg = H(0x000000);
    t->menubar = H(0x2D2D2D); t->bg_toolbar = H(0x262626); t->statusbar = H(0x1E1E1E);
    t->sidebar = H(0x1E1E1E); t->tab = H(0x2A2A2A); t->tab_active = H(0x1E1E1E);
    t->text = H(0xE8E8E8); t->text2 = H(0xB0B0B0); t->muted = H(0x909090);
    t->disabled = H(0x606060); t->inverse = H(0x1A1A1A); t->link = H(0x64B5F6);
    t->border = H(0x555555); t->border_subtle = H(0x444444); t->border_strong = H(0x666666);
    t->focus = H(0x1976D2);
    t->primary = H(0x1976D2); t->primary_hover = H(0x1E88E5); t->primary_active = H(0x1565C0);
    t->primary_text = H(0xFFFFFF);
    t->accent = H(0xFF9800); t->accent_hover = H(0xFFA726); t->accent_text = H(0x000000);
    t->success = H(0x4CAF50); t->success_bg = H(0x1B5E20);
    t->warning = H(0xFF9800); t->warning_bg = H(0xE65100);
    t->error = H(0xF44336); t->error_bg = H(0xB71C1C);
    t->info = H(0x2196F3); t->info_bg = H(0x0D47A1);
    t->scroll_track = H(0x1E1E1E); t->scroll_thumb = H(0x404040); t->scroll_hover = H(0x505050);
    t->table_bg = H(0x1E1E1E); t->table_header = H(0x2A2A2A); t->table_cell = H(0x252525);
    t->table_hover = H(0x333333); t->table_selected = H(0x0D47A1);
    t->table_editing = H(0x1565C0); t->table_axis = H(0x2D2D2D); t->table_grid = H(0x383838);
    t->change_richer = H(0x2196F3); t->change_leaner = H(0xF44336); t->change_same = H(0x808080);
    t->cursor_live = H(0x00FF00); t->cursor_trail = H(0xFFCC00);
    t->cursor_selected = H(0xFF0000); t->cursor_active = H(0x0088FF);
    ui_theme_metrics(t, 0, 1);
}

static void theme_base_light(lt_theme *t)
{
    memset(t, 0, sizeof(*t));
    t->name = "light"; t->dark = 0;
    t->bg_app = H(0xF5F5F5); t->surface = H(0xFFFFFF); t->elevated = H(0xFFFFFF);
    t->input = H(0xFFFFFF); t->hover = H(0xE8E8E8); t->active = H(0xDADADA);
    t->selected = H(0xBBDEFB); t->dash_bg = H(0x000000);
    t->menubar = H(0xEBEBEB); t->bg_toolbar = H(0xF0F0F0); t->statusbar = H(0xE6E6E6);
    t->sidebar = H(0xEEEEEE); t->tab = H(0xE4E4E4); t->tab_active = H(0xFFFFFF);
    t->text = H(0x1A1A1A); t->text2 = H(0x444444); t->muted = H(0x6B6B6B);
    t->disabled = H(0xAAAAAA); t->inverse = H(0xFFFFFF); t->link = H(0x1565C0);
    t->border = H(0xBDBDBD); t->border_subtle = H(0xD6D6D6); t->border_strong = H(0x9E9E9E);
    t->focus = H(0x1976D2);
    t->primary = H(0x1976D2); t->primary_hover = H(0x1E88E5); t->primary_active = H(0x1565C0);
    t->primary_text = H(0xFFFFFF);
    t->accent = H(0xF57C00); t->accent_hover = H(0xFB8C00); t->accent_text = H(0x000000);
    t->success = H(0x2E7D32); t->success_bg = H(0xC8E6C9);
    t->warning = H(0xEF6C00); t->warning_bg = H(0xFFE0B2);
    t->error = H(0xC62828); t->error_bg = H(0xFFCDD2);
    t->info = H(0x1565C0); t->info_bg = H(0xBBDEFB);
    t->scroll_track = H(0xEEEEEE); t->scroll_thumb = H(0xC4C4C4); t->scroll_hover = H(0xA8A8A8);
    t->table_bg = H(0xFFFFFF); t->table_header = H(0xE8E8E8); t->table_cell = H(0xFAFAFA);
    t->table_hover = H(0xE3F2FD); t->table_selected = H(0xBBDEFB);
    t->table_editing = H(0x90CAF9); t->table_axis = H(0xEEEEEE); t->table_grid = H(0xD0D0D0);
    t->change_richer = H(0x1565C0); t->change_leaner = H(0xC62828); t->change_same = H(0x808080);
    t->cursor_live = H(0x00A000); t->cursor_trail = H(0xE0A000);
    t->cursor_selected = H(0xD00000); t->cursor_active = H(0x0070D0);
    ui_theme_metrics(t, 0, 1);
}

/* field offsets in the order of tools/gen_themes.py TOKENS */
static const size_t field_off[LT_THEME_NTOK] = {
    offsetof(lt_theme, bg_app), offsetof(lt_theme, surface), offsetof(lt_theme, elevated),
    offsetof(lt_theme, input), offsetof(lt_theme, hover), offsetof(lt_theme, active),
    offsetof(lt_theme, selected), offsetof(lt_theme, dash_bg), offsetof(lt_theme, menubar),
    offsetof(lt_theme, bg_toolbar), offsetof(lt_theme, statusbar), offsetof(lt_theme, sidebar),
    offsetof(lt_theme, tab), offsetof(lt_theme, tab_active), offsetof(lt_theme, text),
    offsetof(lt_theme, text2), offsetof(lt_theme, muted), offsetof(lt_theme, disabled),
    offsetof(lt_theme, inverse), offsetof(lt_theme, link), offsetof(lt_theme, border),
    offsetof(lt_theme, border_subtle), offsetof(lt_theme, border_strong), offsetof(lt_theme, focus),
    offsetof(lt_theme, primary), offsetof(lt_theme, primary_hover), offsetof(lt_theme, primary_active),
    offsetof(lt_theme, primary_text), offsetof(lt_theme, accent), offsetof(lt_theme, accent_hover),
    offsetof(lt_theme, accent_text), offsetof(lt_theme, success), offsetof(lt_theme, success_bg),
    offsetof(lt_theme, warning), offsetof(lt_theme, warning_bg), offsetof(lt_theme, error),
    offsetof(lt_theme, error_bg), offsetof(lt_theme, info), offsetof(lt_theme, info_bg),
    offsetof(lt_theme, scroll_track), offsetof(lt_theme, scroll_thumb), offsetof(lt_theme, scroll_hover),
    offsetof(lt_theme, table_bg), offsetof(lt_theme, table_header), offsetof(lt_theme, table_cell),
    offsetof(lt_theme, table_hover), offsetof(lt_theme, table_selected), offsetof(lt_theme, table_editing),
    offsetof(lt_theme, table_axis), offsetof(lt_theme, table_grid), offsetof(lt_theme, change_richer),
    offsetof(lt_theme, change_leaner), offsetof(lt_theme, change_same), offsetof(lt_theme, cursor_live),
    offsetof(lt_theme, cursor_trail), offsetof(lt_theme, cursor_selected), offsetof(lt_theme, cursor_active)
};

/* stock theme row: start from the matching built-in base, then overlay tokens */
static void theme_from_row(lt_theme *t, const lt_theme_row *row)
{
    int i;
    if (row->dark) { lt_theme_dark(t); } else { theme_base_light(t); }
    for (i = 0; i < LT_THEME_NTOK; i++) {
        lt_color c;
        if (row->col[i] == 0xFFFFFFFFUL) { continue; }
        c = H(row->col[i]);
        memcpy((char *)t + field_off[i], &c, sizeof(c));
    }
    t->name = row->name;
    t->dark = row->dark;
    ui_theme_metrics(t, 0, 1);
}

void lt_theme_light(lt_theme *t) { theme_from_row(t, &lt_theme_rows[1]); }

void lt_theme_derive(lt_theme *t, const char *name, lt_color bg, lt_color primary,
                     lt_color accent, lt_bool dark)
{
    lt_color up = dark ? H(0xFFFFFF) : H(0x000000);
    lt_color fg = dark ? H(0xE8E8E8) : H(0x1A1A1A);
    if (dark) { lt_theme_dark(t); } else { lt_theme_light(t); }
    t->name = name; t->dark = dark ? 1 : 0;
    t->bg_app = bg;
    if (dark) {
        t->surface = lt_color_mix(bg, up, 11); t->elevated = lt_color_mix(bg, up, 21);
        t->input = lt_color_mix(bg, up, 18); t->hover = lt_color_mix(bg, up, 34);
        t->active = lt_color_mix(bg, up, 45);
        t->border = lt_color_mix(bg, up, 66); t->border_subtle = lt_color_mix(bg, up, 47);
        t->border_strong = lt_color_mix(bg, up, 80);
        t->sidebar = lt_color_mix(bg, up, 6); t->statusbar = lt_color_mix(bg, up, 6);
        t->tab_active = lt_color_mix(bg, up, 6);
    } else {
        t->surface = lt_color_mix(bg, H(0xFFFFFF), 200); t->elevated = t->surface;
        t->input = t->surface;
        t->hover = lt_color_mix(bg, up, 20); t->active = lt_color_mix(bg, up, 34);
        t->border = lt_color_mix(bg, up, 50); t->border_subtle = lt_color_mix(bg, up, 30);
        t->border_strong = lt_color_mix(bg, up, 80);
        t->sidebar = lt_color_mix(bg, up, 10); t->statusbar = lt_color_mix(bg, up, 16);
        t->tab_active = t->surface;
    }
    t->menubar = t->elevated; t->bg_toolbar = lt_color_mix(bg, t->elevated, 128);
    t->tab = t->input;
    t->text = fg; t->text2 = lt_color_mix(fg, bg, 60); t->muted = lt_color_mix(fg, bg, 110);
    t->disabled = lt_color_mix(fg, bg, 170); t->inverse = bg;
    t->primary = primary; t->focus = primary;
    t->primary_hover = lt_color_mix(primary, H(0xFFFFFF), 36);
    t->primary_active = lt_color_mix(primary, H(0x000000), 36);
    t->primary_text = lt_color_contrast(primary);
    t->accent = accent; t->accent_hover = lt_color_mix(accent, H(0xFFFFFF), 36);
    t->accent_text = lt_color_contrast(accent);
    t->selected = lt_color_mix(bg, primary, dark ? 150 : 80);
    t->link = lt_color_mix(primary, fg, 90);
    t->scroll_track = t->sidebar; t->scroll_thumb = t->active; t->scroll_hover = t->border;
    t->table_bg = t->sidebar; t->table_header = t->input; t->table_cell = t->surface;
    t->table_hover = t->hover; t->table_selected = t->selected;
    t->table_editing = primary; t->table_axis = t->elevated; t->table_grid = t->hover;
}

int lt_theme_preset(lt_theme *t, const char *name)
{
    int i;
    if (name == NULL) { return -1; }
    for (i = 0; i < LT_THEME_NROWS; i++) {
        if (lt_stricmp(name, lt_theme_rows[i].name) == 0) {
            if (i == 0) { lt_theme_dark(t); } else { theme_from_row(t, &lt_theme_rows[i]); }
            return 0;
        }
    }
    return -1;
}
