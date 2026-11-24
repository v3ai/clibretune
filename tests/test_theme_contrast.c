/* test_theme_contrast.c - WCAG 2.1 contrast audit of every stock theme.
 *
 * The table below lists which colour tokens the UI draws on which backgrounds (found in the
 * widget and app code: labels on panels, dialogs, menus, inputs, the status bar, the toolbar,
 * the sidebar tree, tabs and table headers; selected rows; button labels; control outlines).
 * Every pair must reach its target: 4.5:1 for text (WCAG 1.4.3 AA), 3:1 for disabled text,
 * coloured glyphs on hover/pressed rows and UI component boundaries (WCAG 1.4.11).
 *
 * tools/gen_themes.py reads the same table (between the spec markers) and nudges the stock
 * palette in OKLCH lightness until every pair passes, so this file is the single list of pairs.
 * Spec rows: { "fg tokens", target x10, "bg tokens" }. "a|b" passes when either a or b reaches
 * the target (the toolkit picks the readable one, see ui_text_on()); "@contrast" is
 * lt_color_contrast(bg), the black-or-white pick toasts and pills use.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdio.h>
#include <stddef.h>
#include <string.h>
#include <math.h>
#include "lt_test.h"
#include "../src/gui/ui.h"
#include "../src/gui/ui_priv.h"

typedef struct spec_row {
    const char *fg;
    int target10;
    const char *bg;
} spec_row;

/* theme-contrast-spec-begin */
static const spec_row spec[] = {
    /* body, secondary and muted text on every surface text sits on */
    { "text text2 muted", 45, "bg_app surface elevated input menubar bg_toolbar statusbar sidebar tab "
                              "tab_active hover table_header table_axis table_cell" },
    /* pressed buttons and open menus; the device banner and the unburned-changes pill */
    { "text", 45, "active success_bg warning_bg info_bg" },
    { "text2", 45, "success_bg info_bg" },
    /* links, accent labels and status colours used as text */
    { "link accent warning error success info", 45, "bg_app surface elevated input menubar bg_toolbar "
                                                   "statusbar sidebar tab tab_active table_header "
                                                   "table_axis table_cell" },
    /* the same colours as icons on hovered and pressed rows and toolbar buttons (a selected
     * toolbar button shows a link-coloured glyph on the active fill) */
    { "link accent warning error success info", 30, "hover active" },
    /* disabled labels (exempt from 1.4.3, kept legible) */
    { "disabled", 30, "bg_app surface elevated input menubar bg_toolbar statusbar sidebar tab table_cell" },
    /* selected rows, menu items and list entries */
    { "text|primary_text", 45, "selected" },
    /* button labels */
    { "primary_text", 45, "primary primary_hover primary_active" },
    { "accent_text", 45, "accent accent_hover" },
    /* toasts and pills pick black or white */
    { "@contrast", 45, "success_bg warning_bg error_bg info_bg" },
    /* control outlines (text fields, combos, checkboxes, radios) and the keyboard focus ring */
    { "border_strong focus", 30, "bg_app surface elevated input bg_toolbar sidebar" },
    /* checked checkboxes and radios */
    { "primary", 30, "bg_app surface input" },
    /* scrollbar thumbs (idle and hovered) on their track and in popups */
    { "scroll_thumb scroll_hover", 30, "scroll_track elevated" },
    /* grid lines of the table editor */
    { "table_grid", 30, "table_cell table_header table_axis" }
};
/* theme-contrast-spec-end */

#define F(n) { #n, offsetof(lt_theme, n) }
static const struct { const char *name; size_t off; } fields[] = {
    F(bg_app), F(surface), F(elevated), F(input), F(hover), F(active), F(selected), F(dash_bg),
    F(menubar), F(bg_toolbar), F(statusbar), F(sidebar), F(tab), F(tab_active), F(text), F(text2),
    F(muted), F(disabled), F(inverse), F(link), F(border), F(border_subtle), F(border_strong),
    F(focus), F(primary), F(primary_hover), F(primary_active), F(primary_text), F(accent),
    F(accent_hover), F(accent_text), F(success), F(success_bg), F(warning), F(warning_bg), F(error),
    F(error_bg), F(info), F(info_bg), F(scroll_track), F(scroll_thumb), F(scroll_hover), F(table_bg),
    F(table_header), F(table_cell), F(table_hover), F(table_selected), F(table_editing),
    F(table_axis), F(table_grid)
};
#undef F

static int field_of(const lt_theme *t, const char *name, size_t len, lt_color *out)
{
    size_t i;
    for (i = 0; i < LT_COUNTOF(fields); i++) {
        if (strlen(fields[i].name) == len && strncmp(fields[i].name, name, len) == 0) {
            memcpy(out, (const char *)t + fields[i].off, sizeof(*out));
            return 1;
        }
    }
    return 0;
}

static double chan(int v)
{
    double c = (double)v / 255.0;
    return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}

static double lum(lt_color c)
{
    return 0.2126 * chan(LT_COLOR_R(c)) + 0.7152 * chan(LT_COLOR_G(c)) + 0.0722 * chan(LT_COLOR_B(c));
}

static double ratio(lt_color a, lt_color b)
{
    double la = lum(a), lb = lum(b);
    return la > lb ? (la + 0.05) / (lb + 0.05) : (lb + 0.05) / (la + 0.05);
}

/* next space-separated word of *s; returns its length, 0 at the end */
static size_t word(const char **s, const char **w)
{
    size_t n = 0;
    while (**s == ' ') { (*s)++; }
    *w = *s;
    while ((*s)[n] != '\0' && (*s)[n] != ' ') { n++; }
    *s += n;
    return n;
}

/* best ratio of the fg alternatives ("a|b", "@contrast") against bg; -1 on an unknown name */
static double fg_ratio(const lt_theme *t, const char *fg, size_t len, lt_color bg)
{
    double best = 0.0;
    size_t a = 0;
    while (a < len) {
        size_t b = a;
        lt_color c;
        double r;
        while (b < len && fg[b] != '|') { b++; }
        if (b - a == 9 && strncmp(fg + a, "@contrast", 9) == 0) { c = lt_color_contrast(bg); }
        else if (!field_of(t, fg + a, b - a, &c)) { return -1.0; }
        r = ratio(c, bg);
        if (r > best) { best = r; }
        a = b + 1;
    }
    return best;
}

LT_TEST(stock_themes_meet_wcag_aa)
{
    int i;
    for (i = 0; i < LT_THEME_NROWS; i++) {
        lt_theme t;
        size_t k;
        int bad = 0;
        double worst_text = 99.0, worst_ui = 99.0;
        char wt[64] = "", wu[64] = "";
        LT_CHECK(lt_theme_preset(&t, lt_theme_rows[i].name) == 0);
        for (k = 0; k < LT_COUNTOF(spec); k++) {
            const char *fs = spec[k].fg, *f;
            size_t fl;
            double target = (double)spec[k].target10 / 10.0;
            while ((fl = word(&fs, &f)) > 0) {
                const char *bs = spec[k].bg, *b;
                size_t bl;
                while ((bl = word(&bs, &b)) > 0) {
                    lt_color bg;
                    double r;
                    if (!field_of(&t, b, bl, &bg)) {
                        printf("unknown token %.*s\n", (int)bl, b);
                        LT_CHECK(0);
                        continue;
                    }
                    r = fg_ratio(&t, f, fl, bg);
                    if (r < 0.0) {
                        printf("unknown token %.*s\n", (int)fl, f);
                        LT_CHECK(0);
                        continue;
                    }
                    if (r < target) {
                        printf("%s: %.*s on %.*s is %.2f:1, needs %.1f:1\n", t.name, (int)fl, f, (int)bl, b,
                               r, target);
                        bad++;
                    }
                    if (spec[k].target10 >= 45 && r < worst_text) {
                        worst_text = r;
                        lt_snprintf(wt, sizeof(wt), "%.*s/%.*s", (int)fl, f, (int)bl, b);
                    }
                    if (spec[k].target10 < 45 && r < worst_ui) {
                        worst_ui = r;
                        lt_snprintf(wu, sizeof(wu), "%.*s/%.*s", (int)fl, f, (int)bl, b);
                    }
                }
            }
        }
        printf("  %-12s worst text %.2f (%s), worst 3:1 pair %.2f (%s)\n", t.name, worst_text, wt, worst_ui, wu);
        LT_CHECK(bad == 0);
    }
}

int main(void)
{
    LT_RUN(stock_themes_meet_wcag_aa);
    LT_TEST_MAIN_END
}
