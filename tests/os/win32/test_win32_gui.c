/* test_win32_gui.c - native Win32 backend: window, present, input, close.
 *
 * PLATFORM FILE (tests/os/win32, built only for TARGET=windows): includes
 * <windows.h> to inject window messages into the backend's window.
 * Skips (exit 0) when no window can be created, e.g. wine without a display
 * or a service session without a desktop.
 */
#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0601
#include <windows.h>

#include "lt_test.h"
#include "../../../src/gui/gui.h"
#include <stdlib.h>
#include <string.h>

#define W32T_CLASS L"LibreTuneWindow"

static int w32t_next(lt_backend *b, lt_event *ev, int want)
{
    int i;
    for (i = 0; i < 50; i++) {
        if (b->poll(b, ev, 50) && ev->type == (lt_ev_type)want) {
            return 1;
        }
    }
    return 0;
}

LT_TEST(test_win32_window)
{
    lt_backend *b = lt_backend_win32_create();
    lt_canvas c;
    lt_event ev;
    HWND hw;
    HDC dc;
    COLORREF px;
    int w = 0, h = 0;
    char *clip;

    LT_CHECK(b != NULL);
    if (b == NULL) {
        return;
    }
    LT_CHECK(strcmp(b->name, "win32") == 0);
    if (b->open(b, "win32 test", 320, 200) != 0) {
        printf("test_win32_gui: cannot create a window (%s), skipped\n", lt_err_get());
        lt_backend_destroy(b);
        return;
    }
    b->size(b, &w, &h);
    LT_CHECK(w > 0 && h > 0);
    LT_CHECK(w == 320 && h == 200);

    LT_CHECK(w32t_next(b, &ev, LT_EV_EXPOSE));

    lt_canvas_init(&c, w, h);
    {
        int i;
        for (i = 0; i < c.pitch * c.h; i++) {
            c.px[i] = 0x00336699UL;     /* R=0x33 G=0x66 B=0x99 */
        }
    }
    b->present(b, &c, NULL, lt_rect_make(0, 0, w, h));
    hw = FindWindowW(W32T_CLASS, NULL);
    LT_CHECK(hw != NULL);
    if (hw != NULL) {
        dc = GetDC(hw);
        px = (dc != NULL) ? GetPixel(dc, 5, 5) : CLR_INVALID;
        if (px != CLR_INVALID) {
            LT_CHECK(GetRValue(px) == 0x33 && GetGValue(px) == 0x66 && GetBValue(px) == 0x99);
        } else {
            printf("test_win32_gui: GetPixel unavailable, pixel check skipped\n");
        }
        if (dc != NULL) {
            ReleaseDC(hw, dc);
        }

        /* injected input -> events */
        PostMessageW(hw, WM_KEYDOWN, (WPARAM)'A', 0);
        PostMessageW(hw, WM_CHAR, (WPARAM)'a', 0);
        PostMessageW(hw, WM_LBUTTONDOWN, 0, (LPARAM)((20 << 16) | 10));
        PostMessageW(hw, WM_LBUTTONUP, 0, (LPARAM)((20 << 16) | 10));
        PostMessageW(hw, WM_KEYDOWN, (WPARAM)VK_LEFT, 0);
        PostMessageW(hw, WM_CHAR, (WPARAM)0x00E9, 0);          /* e-acute */
        PostMessageW(hw, WM_CLOSE, 0, 0);
        LT_CHECK(w32t_next(b, &ev, LT_EV_KEY_DOWN) && ev.key == 'a');
        LT_CHECK(w32t_next(b, &ev, LT_EV_TEXT) && strcmp(ev.text, "a") == 0);
        LT_CHECK(w32t_next(b, &ev, LT_EV_MOUSE_DOWN) && ev.button == LT_MB_LEFT &&
                 ev.x == 10 && ev.y == 20);
        LT_CHECK(w32t_next(b, &ev, LT_EV_MOUSE_UP) && ev.button == LT_MB_LEFT);
        LT_CHECK(w32t_next(b, &ev, LT_EV_KEY_DOWN) && ev.key == LT_KEY_LEFT);
        LT_CHECK(w32t_next(b, &ev, LT_EV_TEXT) && strcmp(ev.text, "\xC3\xA9") == 0);
        LT_CHECK(w32t_next(b, &ev, LT_EV_QUIT));
    }

    b->set_title(b, "renamed");
    if (b->set_clipboard != NULL && b->get_clipboard != NULL) {
        b->set_clipboard(b, "hello \xC3\xA9 clip");
        clip = b->get_clipboard(b);
        if (clip != NULL) {
            LT_CHECK(strcmp(clip, "hello \xC3\xA9 clip") == 0);
            lt_free(clip);
        } else {
            printf("test_win32_gui: clipboard unavailable, skipped\n");
        }
    }
    lt_canvas_free(&c);
    b->close(b);
    b->close(b);                    /* must be safe twice */
    lt_backend_destroy(b);
}

LT_TEST(test_win32_not_opened)
{
    lt_backend *b = lt_backend_win32_create();
    lt_event ev;
    LT_CHECK(b != NULL);
    if (b == NULL) {
        return;
    }
    LT_CHECK(b->poll(b, &ev, 0) == 0);      /* never opened: no events, no crash */
    lt_backend_destroy(b);
}

int main(void)
{
    LT_RUN(test_win32_not_opened);
    LT_RUN(test_win32_window);
    LT_TEST_MAIN_END
}
