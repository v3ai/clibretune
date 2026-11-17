/* ui_gallery.h - one screen that exercises every widget of the toolkit.
 * Header-only (static functions) so both tests/test_ui.c (headless render to
 * build/ui_gallery.ppm and .txt) and tests/manual/ui_demo.c (interactive)
 * share it.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef UI_GALLERY_H
#define UI_GALLERY_H

#include <stdio.h>
#include <string.h>
#include <math.h>
#include "../src/gui/ui.h"

#define GAL_N 16

typedef struct gallery {
    int tab, sidebar_w, sidebar_on;
    char search[64], name[64];
    double gain, trim, level, opacity;
    int mode, check_a, check_b, radio, follow, shading, list_sel, tree_sel;
    double progress;
    lt_color color;
    int modal_open, about_open, win_open, theme_idx;
    lt_rect win_rect;
    lt_ui_filedlg fd;
    char picked[200];
    double ve[GAL_N][GAL_N];
    int live_r, live_c;
    double live_t;
    char status[80];
    int tab_open[3];
    int last_clicked;
} gallery;

static const char *gal_modes[] = { "Speed density", "Alpha-N", "MAF", "Hybrid" };
static const char *gal_list[] = { "Idle", "Cranking", "Warm-up", "Cruise", "Boost", "Overrun",
                                  "Launch", "Anti-lag", "Limp home" };

static void gallery_init(gallery *g)
{
    int r, c;
    memset(g, 0, sizeof(*g));
    g->sidebar_w = 240;
    g->sidebar_on = 1;
    g->tab = 0;
    lt_strlcpy(g->name, "Speeduino v202207", sizeof(g->name));
    g->gain = 1.25; g->trim = -3.5; g->level = 62.0; g->opacity = 40.0;
    g->mode = 0; g->check_a = 1; g->check_b = 0; g->radio = 1; g->follow = 1; g->shading = 1;
    g->list_sel = 3; g->tree_sel = 1;
    g->progress = 0.62;
    g->color = LT_RGB(0xFF, 0x98, 0x00);
    g->win_rect = lt_rect_make(640, 450, 380, 190);
    g->live_r = 6; g->live_c = 9;
    lt_ui_filedlg_init(&g->fd);
    g->tab_open[0] = g->tab_open[1] = g->tab_open[2] = 1;
    lt_strlcpy(g->status, "Ready", sizeof(g->status));
    for (r = 0; r < GAL_N; r++) {
        for (c = 0; c < GAL_N; c++) {
            double x = c / (double)(GAL_N - 1), y = r / (double)(GAL_N - 1);
            g->ve[r][c] = 28.0 + 62.0 * (0.25 + 0.75 * x) * (0.35 + 0.65 * sin(y * 2.4 + 0.3) * sin(y * 2.4 + 0.3))
                          + 6.0 * sin(c * 1.7 + r * 0.9);
            g->ve[r][c] = lt_round(g->ve[r][c] * 10.0) / 10.0;
        }
    }
}

static void gallery_free(gallery *g) { lt_ui_filedlg_free(&g->fd); }

static void gal_menus(lt_ui *ui, gallery *g)
{
    if (lt_ui_menubar_begin(ui)) {
        if (lt_ui_menu_begin(ui, "&File")) {
            if (lt_ui_menu_item(ui, "Open Tune...", "Ctrl+O", 0)) {
                lt_ui_filedlg_open(&g->fd, LT_UI_FILE_OPEN, NULL, "",
                                   "Tune files|*.msq;*.xml|All files|*");
            }
            lt_ui_menu_item(ui, "Save Tune", "Ctrl+S", 0);
            if (lt_ui_menu_item(ui, "Save Tune As...", "Ctrl+Shift+S", 0)) {
                lt_ui_filedlg_open(&g->fd, LT_UI_FILE_SAVE, NULL, "tune.msq",
                                   "Tune files|*.msq|All files|*");
            }
            lt_ui_menu_separator(ui);
            if (lt_ui_submenu_begin(ui, "Recent Projects", 0)) {
                lt_ui_menu_item(ui, "Speeduino demo", NULL, 0);
                lt_ui_menu_item(ui, "Miata NA6 turbo", NULL, 0);
                lt_ui_menu_item(ui, "RX-7 FD", NULL, LT_UI_MENU_DISABLED);
                lt_ui_submenu_end(ui);
            }
            lt_ui_menu_separator(ui);
            lt_ui_menu_item(ui, "Burn to ECU", "Ctrl+B", LT_UI_MENU_DISABLED);
            if (lt_ui_menu_item(ui, "Exit", "Ctrl+Q", 0)) { g->about_open = 0; }
            lt_ui_menu_end(ui);
        }
        if (lt_ui_menu_begin(ui, "&Edit")) {
            lt_ui_menu_item(ui, "Undo", "Ctrl+Z", LT_UI_MENU_DISABLED);
            lt_ui_menu_item(ui, "Redo", "Ctrl+Y", LT_UI_MENU_DISABLED);
            lt_ui_menu_separator(ui);
            lt_ui_menu_item(ui, "Reset to Defaults", NULL, 0);
            lt_ui_menu_end(ui);
        }
        if (lt_ui_menu_begin(ui, "&View")) {
            if (lt_ui_menu_item(ui, "Toggle Sidebar", "F9", g->sidebar_on ? LT_UI_MENU_CHECKED : 0)) {
                g->sidebar_on = !g->sidebar_on;
            }
            if (lt_ui_menu_item(ui, "Floating Window", NULL, g->win_open ? LT_UI_MENU_CHECKED : 0)) {
                g->win_open = !g->win_open;
            }
            lt_ui_menu_separator(ui);
            if (lt_ui_submenu_begin(ui, "Theme", 0)) {
                if (lt_ui_menu_item(ui, "Dark", NULL, g->theme_idx == 0 ? LT_UI_MENU_CHECKED : 0)) {
                    lt_theme_preset(lt_ui_theme(ui), "dark"); lt_ui_theme_changed(ui); g->theme_idx = 0;
                }
                if (lt_ui_menu_item(ui, "Light", NULL, g->theme_idx == 1 ? LT_UI_MENU_CHECKED : 0)) {
                    lt_theme_preset(lt_ui_theme(ui), "light"); lt_ui_theme_changed(ui); g->theme_idx = 1;
                }
                if (lt_ui_menu_item(ui, "Nord", NULL, g->theme_idx == 2 ? LT_UI_MENU_CHECKED : 0)) {
                    lt_theme_preset(lt_ui_theme(ui), "nord"); lt_ui_theme_changed(ui); g->theme_idx = 2;
                }
                if (lt_ui_menu_item(ui, "Dracula", NULL, g->theme_idx == 3 ? LT_UI_MENU_CHECKED : 0)) {
                    lt_theme_preset(lt_ui_theme(ui), "dracula"); lt_ui_theme_changed(ui); g->theme_idx = 3;
                }
                lt_ui_submenu_end(ui);
            }
            lt_ui_menu_end(ui);
        }
        if (lt_ui_menu_begin(ui, "&Tools")) {
            lt_ui_menu_item(ui, "AutoTune", "Ctrl+Shift+A", 0);
            lt_ui_menu_item(ui, "Data Logging", "Ctrl+L", 0);
            lt_ui_menu_separator(ui);
            lt_ui_menu_item(ui, "Settings...", "Ctrl+,", 0);
            lt_ui_menu_end(ui);
        }
        if (lt_ui_menu_begin(ui, "&Help")) {
            if (lt_ui_menu_item(ui, "About LibreTune", NULL, 0)) { g->about_open = 1; }
            lt_ui_menu_end(ui);
        }
        lt_ui_menubar_end(ui);
    }
}

static void gal_toolbar(lt_ui *ui, gallery *g)
{
    lt_ui_toolbar_begin(ui);
    lt_ui_icon_button(ui, LT_UI_ICON_MENU, "Toggle sidebar (F9)", 0);
    if (lt_ui_last_hovered(ui) && lt_ui_mouse_down(ui, LT_MB_LEFT)) { g->last_clicked = 2; }
    lt_ui_icon_button(ui, LT_UI_ICON_PLAY, "Start realtime data", 0);
    lt_ui_icon_button(ui, LT_UI_ICON_STOP, "Stop", 0);
    lt_ui_icon_button(ui, LT_UI_ICON_PLUS, "Increase selected cells", 0);
    lt_ui_icon_button(ui, LT_UI_ICON_MINUS, "Decrease selected cells", 0);
    lt_ui_toggle(ui, "Follow", &g->follow);
    lt_ui_tooltip(ui, "Follow the live ECU cursor");
    lt_ui_toggle(ui, "Shading", &g->shading);
    lt_ui_toolbar_end(ui);
}

static void gal_status(lt_ui *ui, gallery *g)
{
    const lt_theme *t = lt_ui_theme(ui);
    int w[5];
    lt_ui_statusbar_begin(ui);
    w[0] = 8 * 40; w[1] = -1; w[2] = 8 * 22; w[3] = 8 * 12; w[4] = 8 * 8;
    lt_ui_layout_row(ui, 5, w, 0);
    lt_ui_layout_indent(ui, 0);
    lt_ui_label_ex(ui, "  Connected: Speeduino @ /dev/ttyUSB0", t->success, LT_UI_ALIGN_LEFT);
    {
        lt_rect dr = lt_ui_last_rect(ui);
        lt_ui_draw_icon(ui, lt_rect_make(dr.x - 2, dr.y, 16, dr.h), LT_UI_ICON_DOT, t->success);
    }
    lt_ui_label_ex(ui, g->status, t->text2, LT_UI_ALIGN_LEFT);
    lt_ui_label_ex(ui, "Project: Miata turbo", t->text2, LT_UI_ALIGN_RIGHT);
    lt_ui_label_ex(ui, "Metric", t->muted, LT_UI_ALIGN_RIGHT);
    lt_ui_label_ex(ui, "v0.1.0", t->muted, LT_UI_ALIGN_RIGHT);
    lt_ui_statusbar_end(ui);
}

static void gal_tree_leaf(lt_ui *ui, gallery *g, const char *label, int icon, int id)
{
    int r = lt_ui_tree_node(ui, label, icon, LT_UI_TREE_LEAF | (g->tree_sel == id ? LT_UI_TREE_SELECTED : 0));
    if (r & LT_UI_TREE_CLICKED) { g->tree_sel = id; g->tab = 1; }
}

static void gal_sidebar(lt_ui *ui, gallery *g, lt_rect r)
{
    const lt_theme *t = lt_ui_theme(ui);
    int w1 = -1, f;
    lt_ui_region_begin(ui, r, t->sidebar);
    lt_ui_layout_row(ui, 1, &w1, 0);
    lt_ui_layout_space(ui, 4);
    lt_ui_label_ex(ui, " Speeduino v202207", t->text, LT_UI_ALIGN_LEFT);
    f = lt_ui_text(ui, "search", g->search, sizeof(g->search), 0);
    LT_UNUSED(f);
    if (g->search[0] == '\0' && !lt_ui_text_focused(ui)) {
        lt_rect sr = lt_ui_last_rect(ui);
        lt_ui_draw_icon(ui, lt_rect_make(sr.x + sr.w - 26, sr.y, 22, sr.h), LT_UI_ICON_SEARCH, t->muted);
        lt_ui_draw_text_in(ui, lt_rect_make(sr.x + t->pad_x, sr.y, sr.w - 40, sr.h), "Search settings",
                           t->disabled, 0);
    }
    lt_ui_layout_row(ui, 1, &w1, -1);
    if (lt_ui_scroll_begin(ui, "tree")) {
        lt_ui_layout_gap(ui, 0, 0);
        lt_ui_layout_row(ui, 1, &w1, 0);
        if (lt_ui_tree_node(ui, "Settings", LT_UI_ICON_FOLDER, LT_UI_TREE_DEFAULT_OPEN) & LT_UI_TREE_OPEN) {
            gal_tree_leaf(ui, g, "Engine Constants", LT_UI_ICON_FILE, 0);
            gal_tree_leaf(ui, g, "Injector Setup", LT_UI_ICON_FILE, 1);
            gal_tree_leaf(ui, g, "Ignition Setup", LT_UI_ICON_FILE, 2);
            lt_ui_tree_end(ui);
        }
        if (lt_ui_tree_node(ui, "Tuning", LT_UI_ICON_FOLDER, LT_UI_TREE_DEFAULT_OPEN) & LT_UI_TREE_OPEN) {
            if (lt_ui_tree_node(ui, "Fuel", LT_UI_ICON_FOLDER, LT_UI_TREE_DEFAULT_OPEN) & LT_UI_TREE_OPEN) {
                gal_tree_leaf(ui, g, "VE Table 1", LT_UI_ICON_TABLE, 3);
                gal_tree_leaf(ui, g, "AFR Targets", LT_UI_ICON_TABLE, 4);
                gal_tree_leaf(ui, g, "Warm-up Enrichment", LT_UI_ICON_TABLE, 5);
                lt_ui_tree_end(ui);
            }
            if (lt_ui_tree_node(ui, "Ignition", LT_UI_ICON_FOLDER, 0) & LT_UI_TREE_OPEN) {
                gal_tree_leaf(ui, g, "Spark Advance", LT_UI_ICON_TABLE, 6);
                lt_ui_tree_end(ui);
            }
            lt_ui_tree_end(ui);
        }
        if (lt_ui_tree_node(ui, "Tools", LT_UI_ICON_FOLDER, 0) & LT_UI_TREE_OPEN) {
            gal_tree_leaf(ui, g, "AutoTune", LT_UI_ICON_GAUGE, 7);
            lt_ui_tree_end(ui);
        }
        lt_ui_scroll_end(ui);
    }
    lt_ui_region_end(ui);
}

static void gal_buttons(lt_ui *ui, gallery *g)
{
    int w3[3], w2[2], w[3];
    const lt_theme *t = lt_ui_theme(ui);
    w3[0] = w3[1] = w3[2] = -1;
    lt_ui_layout_row(ui, 3, w3, 0);
    if (lt_ui_button(ui, "Default")) { lt_strlcpy(g->status, "Default clicked", sizeof(g->status)); g->last_clicked = 1; }
    lt_ui_tooltip(ui, "A standard button");
    if (lt_ui_button_ex(ui, "Primary", LT_UI_BTN_PRIMARY)) { g->modal_open = 1; }
    if (lt_ui_button_ex(ui, "Danger", LT_UI_BTN_DANGER)) { lt_ui_toast(ui, 3, "Danger button pressed"); }
    lt_ui_layout_row(ui, 3, w3, 0);
    lt_ui_button_ex(ui, "Disabled", LT_UI_BTN_DISABLED);
    lt_ui_button_ex(ui, "Flat", LT_UI_BTN_FLAT);
    if (lt_ui_button_ex(ui, "Accent", LT_UI_BTN_ACCENT)) { lt_ui_toast(ui, 1, "Saved tune to project"); }
    w2[0] = -1; w2[1] = -1;
    lt_ui_layout_row(ui, 2, w2, 0);
    lt_ui_checkbox(ui, "Boost control", &g->check_a);
    lt_ui_checkbox(ui, "Rev limiter", &g->check_b);
    w[0] = w[1] = w[2] = -1;
    lt_ui_layout_row(ui, 3, w, 0);
    lt_ui_radio(ui, "Metric", &g->radio, 0);
    lt_ui_radio(ui, "Imperial", &g->radio, 1);
    lt_ui_radio(ui, "SAE", &g->radio, 2);
    lt_ui_layout_row(ui, 3, w3, 0);
    if (lt_ui_button(ui, "Dialog...")) { g->about_open = 1; }
    if (lt_ui_button(ui, "Open file...")) {
        lt_ui_filedlg_open(&g->fd, LT_UI_FILE_OPEN, NULL, "", "Tune files|*.msq;*.xml|All files|*");
    }
    if (lt_ui_button(ui, "Window")) { g->win_open = 1; }
    lt_ui_layout_row(ui, 1, w, 0);
    lt_ui_label_ex(ui, g->picked[0] ? g->picked : "No file chosen yet", t->muted, LT_UI_ALIGN_LEFT);
}

static void gal_inputs(lt_ui *ui, gallery *g)
{
    int w[2], w3[3];
    w[0] = 8 * 8; w[1] = -1;
    lt_ui_layout_row(ui, 2, w, 0);
    lt_ui_label(ui, "Name");
    lt_ui_text(ui, "name", g->name, sizeof(g->name), 0);
    lt_ui_label(ui, "Gain");
    lt_ui_number(ui, "gain", &g->gain, 0.0, 10.0, 0.05, 2);
    lt_ui_label(ui, "Trim %");
    lt_ui_number(ui, "trim", &g->trim, -50.0, 50.0, 0.5, 1);
    lt_ui_label(ui, "Level");
    lt_ui_slider(ui, "level", &g->level, 0.0, 100.0, 0);
    lt_ui_label(ui, "Mode");
    lt_ui_combo(ui, "mode", &g->mode, gal_modes, 4);
    w3[0] = 8 * 8; w3[1] = 48; w3[2] = -1;
    lt_ui_layout_row(ui, 3, w3, 0);
    lt_ui_label(ui, "Colour");
    lt_ui_color_edit(ui, "color", &g->color);
    lt_ui_label_ex(ui, "click to edit", lt_ui_theme(ui)->muted, LT_UI_ALIGN_LEFT);
}

static void gal_lists(lt_ui *ui, gallery *g)
{
    int w1 = -1, w[2], w3[3];
    lt_ui_layout_row(ui, 1, &w1, lt_ui_row_height(ui) * 5);
    lt_ui_listbox(ui, "list", &g->list_sel, gal_list, 9);
    w[0] = 8 * 9; w[1] = -1;
    lt_ui_layout_row(ui, 2, w, 0);
    lt_ui_label(ui, "Progress");
    lt_ui_progress(ui, g->progress, NULL);
    lt_ui_label(ui, "Busy");
    lt_ui_progress(ui, -1.0, "Reading ECU...");
    w3[0] = 8 * 9; w3[1] = 28; w3[2] = -1;
    lt_ui_layout_row(ui, 3, w3, 0);
    lt_ui_label(ui, "Working");
    lt_ui_spinner(ui);
    lt_ui_label_ex(ui, "Syncing tune", lt_ui_theme(ui)->muted, LT_UI_ALIGN_LEFT);
}

static void gal_selection_info(lt_ui *ui, gallery *g)
{
    int r0, c0, r1, c1, n, r, c, cnt = 0;
    double mn = 1e9, mx = -1e9, sum = 0.0;
    int cr, cc, w[2];
    const lt_theme *t = lt_ui_theme(ui);
    n = lt_ui_grid_sel_bounds(ui, &r0, &c0, &r1, &c1);
    lt_ui_grid_cursor(ui, &cr, &cc);
    for (r = r0; r >= 0 && r <= r1; r++) {
        for (c = c0; c >= 0 && c <= c1; c++) {
            if (lt_ui_grid_selected(ui, r, c)) {
                double v = g->ve[r][c];
                if (v < mn) { mn = v; }
                if (v > mx) { mx = v; }
                sum += v;
                cnt++;
            }
        }
    }
    w[0] = 8 * 9; w[1] = -1;
    lt_ui_layout_row(ui, 2, w, 0);
    lt_ui_label_ex(ui, "Cells", t->text2, LT_UI_ALIGN_LEFT);
    lt_ui_labelf(ui, "%d", n);
    lt_ui_label_ex(ui, "Cursor", t->text2, LT_UI_ALIGN_LEFT);
    lt_ui_labelf(ui, "row %d, col %d", cr + 1, cc + 1);
    lt_ui_label_ex(ui, "Min", t->text2, LT_UI_ALIGN_LEFT);
    lt_ui_labelf(ui, "%.1f", cnt ? mn : 0.0);
    lt_ui_label_ex(ui, "Max", t->text2, LT_UI_ALIGN_LEFT);
    lt_ui_labelf(ui, "%.1f", cnt ? mx : 0.0);
    lt_ui_label_ex(ui, "Average", t->text2, LT_UI_ALIGN_LEFT);
    lt_ui_labelf(ui, "%.2f", cnt ? sum / cnt : 0.0);
    lt_ui_label_ex(ui, "Live", t->text2, LT_UI_ALIGN_LEFT);
    lt_ui_label_ex(ui, "row 7, col 10", t->cursor_live, LT_UI_ALIGN_LEFT);
}

static void gal_grid(lt_ui *ui, gallery *g, int show_info)
{
    int cell = lt_ui_cell_mode(ui);
    int cw = cell ? 48 : 46, ch = cell ? 16 : 20, rhw = cell ? 64 : 64, chh = cell ? 16 : 24;
    int gw = rhw + GAL_N * cw + lt_ui_theme(ui)->scrollbar_w + 2;
    int w[2];
    const lt_theme *t = lt_ui_theme(ui);
    w[0] = gw; w[1] = -1;
    if (show_info) { lt_ui_layout_row(ui, 2, w, -1); } else { lt_ui_layout_row(ui, 1, &w[1], -1); }
    if (lt_ui_grid_begin(ui, "ve", GAL_N, GAL_N, cw, ch, rhw, chh)) {
        int r0, r1, c0, c1, r, c, fl;
        double mn = 1e9, mx = -1e9;
        char b[32];
        for (r = 0; r < GAL_N; r++) {
            for (c = 0; c < GAL_N; c++) {
                if (g->ve[r][c] < mn) { mn = g->ve[r][c]; }
                if (g->ve[r][c] > mx) { mx = g->ve[r][c]; }
            }
        }
        lt_ui_grid_visible(ui, &r0, &r1, &c0, &c1);
        lt_ui_grid_corner(ui, "RPM/kPa");
        for (c = c0; c <= c1; c++) {
            lt_snprintf(b, sizeof(b), "%d", 20 + c * 10);
            lt_ui_grid_header(ui, 1, c, b);
        }
        for (r = r0; r <= r1; r++) {
            lt_snprintf(b, sizeof(b), "%d", 500 + r * 400);
            lt_ui_grid_header(ui, 0, r, b);
        }
        for (r = r0; r <= r1; r++) {
            for (c = c0; c <= c1; c++) {
                lt_color bg;
                lt_fmt_double(b, sizeof(b), g->ve[r][c], 1);
                if (g->shading) {
                    bg = lt_color_mix(lt_heat_color(0, (g->ve[r][c] - mn) / (mx - mn)), t->table_bg, 70);
                } else {
                    bg = t->table_cell;
                }
                lt_ui_grid_cell(ui, r, c, b, bg, g->shading ? lt_color_contrast(bg) : t->text);
            }
        }
        if (g->follow) { lt_ui_grid_mark(ui, g->live_r, g->live_c, t->cursor_live, 2); }
        fl = lt_ui_grid_end(ui);
        if ((fl & LT_UI_GRID_EDIT_BEGIN) && lt_ui_grid_edit_wants_text(ui)) {
            int er, ec;
            lt_ui_grid_editing(ui, &er, &ec);
            lt_fmt_double(b, sizeof(b), g->ve[er][ec], 1);
            lt_ui_grid_edit_set_text(ui, b);
        }
        if (fl & LT_UI_GRID_EDIT_COMMIT) {
            double v;
            if (lt_parse_double(lt_ui_grid_edit_text(ui), &v)) {
                int r0s, c0s, r1s, c1s;
                lt_ui_grid_sel_bounds(ui, &r0s, &c0s, &r1s, &c1s);
                for (r = r0s; r >= 0 && r <= r1s; r++) {
                    for (c = c0s; c >= 0 && c <= c1s; c++) {
                        if (lt_ui_grid_selected(ui, r, c)) { g->ve[r][c] = v; }
                    }
                }
            }
        }
    }
    if (show_info) {
        if (lt_ui_group_begin(ui, "Selection", 0)) {
            gal_selection_info(ui, g);
            lt_ui_group_end(ui);
        }
    }
}

static void gal_window(lt_ui *ui, gallery *g)
{
    int w[2], w1 = -1;
    if (lt_ui_window_begin(ui, "Live values", &g->win_rect, &g->win_open, LT_UI_WIN_RESIZE)) {
        w[0] = 8 * 8; w[1] = -1;
        lt_ui_layout_row(ui, 2, w, 0);
        lt_ui_label(ui, "RPM");
        lt_ui_progress(ui, 0.45, "3 050");
        lt_ui_label(ui, "MAP");
        lt_ui_progress(ui, 0.30, "78 kPa");
        lt_ui_label(ui, "AFR");
        lt_ui_progress(ui, 0.72, "14.7");
        lt_ui_layout_row(ui, 1, &w1, 0);
        lt_ui_label_ex(ui, "Drag the title bar; resize from the corner.", lt_ui_theme(ui)->muted,
                       LT_UI_ALIGN_LEFT);
        lt_ui_window_end(ui);
    }
}

static void gallery_frame(lt_ui *ui, gallery *g)
{
    const lt_theme *t = lt_ui_theme(ui);
    lt_rect cr, side, main_r, inner;
    int w1 = -1, pad = t->padding, w3[3], r;
    int cell = lt_ui_cell_mode(ui);

    gal_menus(ui, g);
    gal_toolbar(ui, g);
    gal_status(ui, g);
    cr = lt_ui_client_rect(ui);
    if (g->sidebar_on) {
        lt_ui_splitter(ui, "sidebar", cr, 1, &g->sidebar_w, 160, cr.w / 2, &side, &main_r);
        gal_sidebar(ui, g, side);
    } else {
        main_r = cr;
    }
    lt_ui_draw_fill(ui, main_r, t->bg_app);
    inner = lt_rect_inset(main_r, pad, cell ? 0 : pad);
    lt_ui_region_begin(ui, inner, 0);
    lt_ui_layout_row(ui, 1, &w1, 0);
    lt_ui_tabbar_begin(ui, "tabs", &g->tab);
    lt_ui_tab(ui, "Gallery", 0);
    r = lt_ui_tab(ui, "VE Table 1", LT_UI_TAB_CLOSABLE | LT_UI_TAB_DIRTY);
    if (r & LT_UI_TAB_CLOSED) { lt_ui_toast(ui, 2, "Tab close requested"); }
    lt_ui_tab(ui, "Settings", LT_UI_TAB_CLOSABLE);
    lt_ui_tabbar_end(ui);
    lt_ui_layout_space(ui, cell ? 0 : 8);
    if (g->tab == 0) {
        w3[0] = cell ? 328 : 336; w3[1] = cell ? 320 : 336; w3[2] = -1;
        lt_ui_layout_row(ui, 3, w3, cell ? 16 * 15 : 292);
        if (lt_ui_group_begin(ui, "Buttons", cell ? 16 * 15 : 292)) { gal_buttons(ui, g); lt_ui_group_end(ui); }
        if (lt_ui_group_begin(ui, "Inputs", cell ? 16 * 15 : 292)) { gal_inputs(ui, g); lt_ui_group_end(ui); }
        if (lt_ui_group_begin(ui, "Lists and status", cell ? 16 * 15 : 292)) { gal_lists(ui, g); lt_ui_group_end(ui); }
        gal_grid(ui, g, 1);
    } else if (g->tab == 1) {
        gal_grid(ui, g, 1);
    } else {
        lt_ui_layout_row(ui, 1, &w1, 0);
        lt_ui_heading(ui, "Settings");
        lt_ui_checkbox(ui, "Show ECU menus in menubar", &g->check_a);
        lt_ui_checkbox(ui, "Y axis zero at bottom", &g->check_b);
    }
    lt_ui_region_end(ui);

    gal_window(ui, g);
    if (g->modal_open) {
        int res = lt_ui_msgbox(ui, "burn", "Burn to ECU",
                               "Burn writes the current tune to permanent flash memory. "
                               "Make sure the engine is not running. Continue?",
                               "Burn|Cancel", &g->modal_open);
        if (res == 1) { lt_ui_toast(ui, 1, "Burn complete"); }
    }
    if (g->about_open) {
        lt_ui_msgbox(ui, "about", "About LibreTune",
                     "LibreTune - open source ECU tuning, In Universal C.", "OK", &g->about_open);
    }
    if (g->fd.open) {
        int res = lt_ui_file_dialog(ui, &g->fd, g->fd.mode == LT_UI_FILE_SAVE ? "Save tune" : "Open tune");
        if (res == 1) { lt_strlcpy(g->picked, g->fd.path, sizeof(g->picked)); }
    }
}

#endif
