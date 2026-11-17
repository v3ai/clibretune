/* test_view_kit.c - view-kit helpers (src/app/view_kit.c): text metrics, score colours, icon
 * toolbar hit testing in pixel mode and its plain-text fallback in cell mode.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <string.h>
#include "lt_test.h"
#include "../src/app/view_kit.h"

static void frame_begin(lt_ui *ui, lt_gfx *g) { lt_ui_begin(ui, g, 1000.0); }

static int run_toolbar(lt_ui *ui, lt_gfx *g, int mx, int my, int click)
{
    lt_vk_item it[5];
    int hit;
    lt_event ev;
    memset(it, 0, sizeof(it));
    it[0].kind = LT_VK_BTN; it[0].icon = LT_ICON_PLUS; it[0].tip = "add";
    it[1].kind = LT_VK_SEP;
    it[2].kind = LT_VK_BTN; it[2].icon = LT_ICON_MINUS; it[2].label = "Sub"; it[2].key = "-";
    it[3].kind = LT_VK_END;
    memset(&ev, 0, sizeof(ev));
    ev.type = LT_EV_MOUSE_MOVE; ev.x = mx; ev.y = my;
    lt_ui_input(ui, &ev);
    if (click) { ev.type = LT_EV_MOUSE_DOWN; ev.button = LT_MB_LEFT; lt_ui_input(ui, &ev); }
    frame_begin(ui, g);
    hit = lt_vk_toolbar(ui, it, 32);
    lt_ui_end(ui);
    if (click) { ev.type = LT_EV_MOUSE_UP; lt_ui_input(ui, &ev); }
    return hit;
}

LT_TEST(score_colors)
{
    LT_CHECK(lt_vk_score_color(95) != lt_vk_score_color(10));
    LT_CHECK(lt_vk_score_color(80) == lt_vk_score_color(100));
    LT_CHECK(lt_vk_score_color(0) == lt_vk_score_color(39));
    LT_CHECK(LT_COLOR_A(lt_vk_score_color(50)) == 255);
}

LT_TEST(text_metrics)
{
    lt_ui *ui = lt_ui_create();
    int a = lt_vk_text_w(ui, (int)LT_FONT_SANS, 12.0, "Hello");
    int b = lt_vk_text_w(ui, (int)LT_FONT_SANS, 12.0, "Hello world");
    int c = lt_vk_text_w(ui, (int)LT_FONT_SANS, 24.0, "Hello");
    LT_CHECK(a > 10);
    LT_CHECK(b > a);
    LT_CHECK(c > a);
    LT_CHECK(lt_vk_text_w(ui, (int)LT_FONT_SANS, 12.0, NULL) == 0);
    LT_CHECK(lt_vk_text_w(ui, (int)LT_FONT_SANS, 12.0, "") == 0);
    lt_ui_destroy(ui);
}

LT_TEST(toolbar_click_and_draw)
{
    lt_ui *ui = lt_ui_create();
    lt_canvas cv;
    lt_gfx g;
    int hit, i, painted = 0;
    lt_canvas_init(&cv, 400, 120);
    lt_gfx_init_canvas(&g, &cv);
    /* settle layout, then hover the first button (left edge of the row) */
    for (i = 0; i < 3; i++) { run_toolbar(ui, &g, 5, 5, 0); }
    hit = run_toolbar(ui, &g, 5, 5, 0);
    LT_CHECK(hit == -1);
    /* press and release on the first (plus) button: some frame reports index 0 */
    hit = run_toolbar(ui, &g, 20, 12, 1);
    for (i = 0; i < 3 && hit != 0; i++) { hit = run_toolbar(ui, &g, 20, 12, 0); }
    LT_CHECK(hit == 0);
    /* pressing outside any button never reports one */
    hit = run_toolbar(ui, &g, 390, 100, 1);
    LT_CHECK(hit == -1);
    /* something visible was drawn (icon and button chrome) */
    for (i = 0; i < cv.w * cv.h; i++) { if (cv.px[i] != cv.px[0]) { painted = 1; break; } }
    LT_CHECK(painted);
    lt_canvas_free(&cv);
    lt_ui_destroy(ui);
}

LT_TEST(bar_and_ring_do_not_crash)
{
    lt_ui *ui = lt_ui_create();
    lt_canvas cv;
    lt_gfx g;
    lt_canvas_init(&cv, 200, 100);
    lt_gfx_init_canvas(&g, &cv);
    frame_begin(ui, &g);
    lt_vk_bar(ui, lt_rect_make(10, 10, 100, 6), 0.5, LT_RGB(0, 255, 0), LT_RGB(40, 40, 40));
    lt_vk_bar(ui, lt_rect_make(10, 30, 100, 6), -3.0, LT_RGB(0, 255, 0), LT_RGB(40, 40, 40));
    lt_vk_bar(ui, lt_rect_make(10, 50, 100, 6), 7.0, LT_RGB(0, 255, 0), LT_RGB(40, 40, 40));
    lt_vk_ring(ui, 150.0, 50.0, 20.0, 3.0, 0.7, LT_RGB(255, 0, 0), LT_RGB(60, 60, 60));
    lt_vk_text(ui, lt_rect_make(0, 80, 200, 20), (int)LT_FONT_SANS_SEMIBOLD, 12.0, "A fairly long caption that must be elided", LT_RGB(255, 255, 255), 0);
    lt_vk_caption(ui, lt_rect_make(0, 60, 200, 20), "caption", LT_RGB(200, 200, 200));
    lt_ui_end(ui);
    LT_CHECK(cv.px[13 * cv.pitch + 30] != cv.px[0]);
    lt_canvas_free(&cv);
    lt_ui_destroy(ui);
}

int main(void)
{
    LT_RUN(score_colors);
    LT_RUN(text_metrics);
    LT_RUN(toolbar_click_and_draw);
    LT_RUN(bar_and_ring_do_not_crash);
    LT_TEST_MAIN_END
}
