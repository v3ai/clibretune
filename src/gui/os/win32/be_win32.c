/* be_win32.c - native Win32 GDI pixel backend.
 *
 * One top-level window; the app canvas (32-bit XRGB) is copied to the client
 * area with SetDIBitsToDevice (1:1, no scaling). The process is marked
 * per-monitor DPI aware, so the canvas is in physical pixels and Windows never
 * bitmap-scales it; scale_hint reports dpi/96 and WM_DPICHANGED resizes the
 * window to the suggested rectangle and raises LT_EV_SCALE. Input arrives through the window procedure into a small ring queue that
 * poll() drains.
 *
 * Built only for TARGET=windows (selected by the build system); other
 * operating systems get a NULL-returning stub from src/gui/os/<os>/stubs.c.
 * Links user32 and gdi32 directly (tools/target.sh).
 *
 * PLATFORM FILE: includes <windows.h> (see src/plat/win32/win_priv.h for the
 * header exception). The macros below must precede the system header.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0601
#include <windows.h>

#include <stddef.h>
#include <string.h>

#include "../../gui.h"
#include "../../../plat/plat.h"

#define WB_EVQ_CAP 128
#define WB_CLASS_NAME L"LibreTuneWindow"

typedef struct wb_state {
    HWND hwnd;
    HINSTANCE hinst;
    int cw, ch;                 /* client size in pixels */
    int opened;
    int class_ok;
    lt_canvas *canvas;          /* last presented canvas */
    WCHAR hi_surr;              /* pending UTF-16 high surrogate */
    lt_event q[WB_EVQ_CAP];
    int qh, qn;
    UINT dpi;                   /* current window/system dpi (96 = 100%) */
    int min_w, min_h;           /* minimum client size, 0 = none */
    int init_x, init_y, init_flags;
} wb_state;

/* ---- dynamically bound user32 DPI entry points (Win10+; absent earlier) ---- */
typedef UINT (WINAPI *wb_dpi_sys_fn)(void);
typedef UINT (WINAPI *wb_dpi_win_fn)(HWND);
typedef BOOL (WINAPI *wb_set_ctx_fn)(HANDLE);
typedef BOOL (WINAPI *wb_adjust_dpi_fn)(LPRECT, DWORD, BOOL, DWORD, UINT);

static void (*wb_user32_proc(const char *name))(void)
{
    HMODULE m = GetModuleHandleW(L"user32.dll");
    if (m == NULL) {
        return NULL;
    }
    return (void (*)(void))GetProcAddress(m, name);
}

static UINT wb_system_dpi(void)
{
    wb_dpi_sys_fn fn = (wb_dpi_sys_fn)wb_user32_proc("GetDpiForSystem");
    UINT d = 0;
    if (fn != NULL) {
        d = fn();
    } else {
        HDC dc = GetDC(NULL);
        if (dc != NULL) {
            d = (UINT)GetDeviceCaps(dc, LOGPIXELSX);
            (void)ReleaseDC(NULL, dc);
        }
    }
    return d >= 48 ? d : 96;
}

static UINT wb_window_dpi(const wb_state *w)
{
    wb_dpi_win_fn fn;
    UINT d = 0;
    if (w->hwnd != NULL) {
        fn = (wb_dpi_win_fn)wb_user32_proc("GetDpiForWindow");
        if (fn != NULL) {
            d = fn(w->hwnd);
        }
    }
    return d >= 48 ? d : wb_system_dpi();
}

/* client size -> outer window size at the given dpi */
static void wb_outer_size(int cw, int ch, DWORD style, UINT dpi, int *ow, int *oh)
{
    RECT r;
    wb_adjust_dpi_fn fn = (wb_adjust_dpi_fn)wb_user32_proc("AdjustWindowRectExForDpi");
    r.left = 0;
    r.top = 0;
    r.right = cw;
    r.bottom = ch;
    if (fn != NULL) {
        (void)fn(&r, style, FALSE, 0, dpi);
    } else {
        (void)AdjustWindowRectEx(&r, style, FALSE, 0);
    }
    *ow = (int)(r.right - r.left);
    *oh = (int)(r.bottom - r.top);
}

/* ---- UTF helpers ---- */
/* UTF-8 -> UTF-16 (NUL terminated, lt_malloc'd) */
static WCHAR *wb_utf8_to_16(const char *s)
{
    WCHAR *o;
    size_t n = 0;
    const char *p;
    lt_u32 cp;
    if (s == NULL) {
        s = "";
    }
    o = (WCHAR *)lt_malloc((strlen(s) * 2 + 2) * sizeof(WCHAR));
    p = s;
    while (*p != '\0') {
        cp = lt_utf8_next(&p);
        if (cp >= 0x10000UL) {
            cp -= 0x10000UL;
            o[n++] = (WCHAR)(0xD800UL + (cp >> 10));
            o[n++] = (WCHAR)(0xDC00UL + (cp & 0x3FFUL));
        } else {
            o[n++] = (WCHAR)cp;
        }
    }
    o[n] = 0;
    return o;
}

/* encode one code point as UTF-8; returns the byte count (1..4) */
static int wb_put_utf8(char *d, lt_u32 cp)
{
    if (cp < 0x80UL) {
        d[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800UL) {
        d[0] = (char)(0xC0UL | (cp >> 6));
        d[1] = (char)(0x80UL | (cp & 0x3FUL));
        return 2;
    }
    if (cp < 0x10000UL) {
        d[0] = (char)(0xE0UL | (cp >> 12));
        d[1] = (char)(0x80UL | ((cp >> 6) & 0x3FUL));
        d[2] = (char)(0x80UL | (cp & 0x3FUL));
        return 3;
    }
    d[0] = (char)(0xF0UL | (cp >> 18));
    d[1] = (char)(0x80UL | ((cp >> 12) & 0x3FUL));
    d[2] = (char)(0x80UL | ((cp >> 6) & 0x3FUL));
    d[3] = (char)(0x80UL | (cp & 0x3FUL));
    return 4;
}

/* ---- event queue ---- */
static lt_event *wb_new_event(wb_state *w, lt_ev_type t)
{
    lt_event *e;
    if (w->qn >= WB_EVQ_CAP) {
        return NULL;
    }
    e = &w->q[(w->qh + w->qn) % WB_EVQ_CAP];
    w->qn++;
    memset(e, 0, sizeof(*e));
    e->type = t;
    return e;
}

static int wb_key_down(int vk)
{
    return (GetKeyState(vk) & 0x8000) != 0;
}

static int wb_mods(void)
{
    int m = 0;
    if (wb_key_down(VK_SHIFT)) {
        m |= LT_MOD_SHIFT;
    }
    if (wb_key_down(VK_CONTROL)) {
        m |= LT_MOD_CTRL;
    }
    if (wb_key_down(VK_MENU)) {
        m |= LT_MOD_ALT;
    }
    if (wb_key_down(VK_LWIN) || wb_key_down(VK_RWIN)) {
        m |= LT_MOD_SUPER;
    }
    return m;
}

static int wb_map_vk(WPARAM vk)
{
    if (vk >= 'A' && vk <= 'Z') {
        return (int)vk + 32;
    }
    if (vk >= '0' && vk <= '9') {
        return (int)vk;
    }
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) {
        return '0' + (int)(vk - VK_NUMPAD0);
    }
    if (vk >= VK_F1 && vk <= VK_F12) {
        return LT_KEY_F1 + (int)(vk - VK_F1);
    }
    switch (vk) {
    case VK_BACK: return LT_KEY_BACKSPACE;
    case VK_TAB: return LT_KEY_TAB;
    case VK_RETURN: return LT_KEY_ENTER;
    case VK_ESCAPE: return LT_KEY_ESCAPE;
    case VK_SPACE: return ' ';
    case VK_DELETE: return LT_KEY_DELETE;
    case VK_UP: return LT_KEY_UP;
    case VK_DOWN: return LT_KEY_DOWN;
    case VK_LEFT: return LT_KEY_LEFT;
    case VK_RIGHT: return LT_KEY_RIGHT;
    case VK_HOME: return LT_KEY_HOME;
    case VK_END: return LT_KEY_END;
    case VK_PRIOR: return LT_KEY_PAGEUP;
    case VK_NEXT: return LT_KEY_PAGEDOWN;
    case VK_INSERT: return LT_KEY_INSERT;
    case VK_OEM_1: return ';';
    case VK_OEM_PLUS: return '=';
    case VK_OEM_COMMA: return ',';
    case VK_OEM_MINUS: return '-';
    case VK_OEM_PERIOD: return '.';
    case VK_OEM_2: return '/';
    case VK_OEM_3: return '`';
    case VK_OEM_4: return '[';
    case VK_OEM_5: return '\\';
    case VK_OEM_6: return ']';
    case VK_OEM_7: return '\'';
    case VK_MULTIPLY: return '*';
    case VK_ADD: return '+';
    case VK_SUBTRACT: return '-';
    case VK_DECIMAL: return '.';
    case VK_DIVIDE: return '/';
    default: break;
    }
    return LT_KEY_NONE;
}

/* signed 16-bit halves of an LPARAM / WPARAM (mouse coordinates, wheel delta) */
static int wb_lo16(LPARAM v)
{
    return (int)(short)(unsigned short)((ULONG_PTR)v & 0xFFFFUL);
}

static int wb_hi16(LPARAM v)
{
    return (int)(short)(unsigned short)(((ULONG_PTR)v >> 16) & 0xFFFFUL);
}

/* ---- painting ---- */
/* Copy the whole canvas to the window DC. Partial updates are not used:
 * SetDIBitsToDevice source-y handling of top-down DIBs differs between
 * Windows versions and wine, and a full 1:1 copy is cheap. */
static void wb_blit(wb_state *w, HDC dc)
{
    BITMAPINFO bmi;
    lt_canvas *c = w->canvas;
    if (c == NULL || c->px == NULL || c->w <= 0 || c->h <= 0) {
        return;
    }
    memset(&bmi, 0, sizeof(bmi));
    bmi.bmiHeader.biSize = (DWORD)sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = c->pitch;
    bmi.bmiHeader.biHeight = -c->h;         /* negative = top-down */
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    (void)SetDIBitsToDevice(dc, 0, 0, (DWORD)c->w, (DWORD)c->h, 0, 0, 0, (UINT)c->h,
                            c->px, &bmi, DIB_RGB_COLORS);
}

/* ---- window procedure ---- */
static LRESULT CALLBACK wb_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    wb_state *w;
    lt_event *e;
    PAINTSTRUCT ps;
    POINT pt;
    int btn, down, d;
    WCHAR u;
    lt_u32 cp;

    if (msg == WM_NCCREATE) {
        const CREATESTRUCTW *cs = (const CREATESTRUCTW *)lp;
        (void)SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    w = (wb_state *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (w == NULL) {
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    switch (msg) {
    case WM_CLOSE:
        (void)wb_new_event(w, LT_EV_QUIT);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        if (wp != SIZE_MINIMIZED) {
            w->cw = (int)((ULONG_PTR)lp & 0xFFFFUL);
            w->ch = (int)(((ULONG_PTR)lp >> 16) & 0xFFFFUL);
            e = wb_new_event(w, LT_EV_RESIZE);
            if (e != NULL) {
                e->x = w->cw;
                e->y = w->ch;
            }
        }
        return 0;
    case WM_GETMINMAXINFO:
        if (w->min_w > 0 && w->min_h > 0) {
            MINMAXINFO *mm = (MINMAXINFO *)lp;
            int ow, oh;
            wb_outer_size(w->min_w, w->min_h, (DWORD)GetWindowLongW(hwnd, GWL_STYLE),
                          wb_window_dpi(w), &ow, &oh);
            mm->ptMinTrackSize.x = ow;
            mm->ptMinTrackSize.y = oh;
        }
        return 0;
    case WM_DPICHANGED: {
        const RECT *sug = (const RECT *)lp;
        UINT nd = (UINT)(wp & 0xFFFFUL);
        if (nd >= 48) {
            w->dpi = nd;
        }
        if (sug != NULL) {
            (void)SetWindowPos(hwnd, NULL, (int)sug->left, (int)sug->top,
                               (int)(sug->right - sug->left), (int)(sug->bottom - sug->top),
                               SWP_NOZORDER | SWP_NOACTIVATE);
        }
        e = wb_new_event(w, LT_EV_SCALE);
        if (e != NULL) {
            e->x = (int)((w->dpi * 100UL + 48UL) / 96UL);
        }
        return 0;
    }
    case WM_PAINT:
        BeginPaint(hwnd, &ps);
        wb_blit(w, ps.hdc);
        EndPaint(hwnd, &ps);
        return 0;
    case WM_KILLFOCUS:
        (void)wb_new_event(w, LT_EV_FOCUS_LOST);
        return 0;
    case WM_MOUSEMOVE:
        e = wb_new_event(w, LT_EV_MOUSE_MOVE);
        if (e != NULL) {
            e->x = wb_lo16(lp);
            e->y = wb_hi16(lp);
            e->mods = wb_mods();
        }
        return 0;
    case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN:
    case WM_LBUTTONUP: case WM_RBUTTONUP: case WM_MBUTTONUP:
        down = (msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN || msg == WM_MBUTTONDOWN);
        if (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP) {
            btn = LT_MB_LEFT;
        } else if (msg == WM_RBUTTONDOWN || msg == WM_RBUTTONUP) {
            btn = LT_MB_RIGHT;
        } else {
            btn = LT_MB_MIDDLE;
        }
        if (down) {
            (void)SetCapture(hwnd);
        } else {
            (void)ReleaseCapture();
        }
        e = wb_new_event(w, down ? LT_EV_MOUSE_DOWN : LT_EV_MOUSE_UP);
        if (e != NULL) {
            e->x = wb_lo16(lp);
            e->y = wb_hi16(lp);
            e->button = btn;
            e->mods = wb_mods();
        }
        return 0;
    case WM_MOUSEWHEEL:
        /* lParam is in screen coordinates for wheel messages */
        pt.x = wb_lo16(lp);
        pt.y = wb_hi16(lp);
        (void)ScreenToClient(hwnd, &pt);
        e = wb_new_event(w, LT_EV_WHEEL);
        if (e != NULL) {
            d = wb_hi16((LPARAM)wp);
            e->x = (int)pt.x;
            e->y = (int)pt.y;
            e->mods = wb_mods();
            e->wheel = d / 120;
            if (e->wheel == 0) {
                e->wheel = d > 0 ? 1 : -1;
            }
        }
        return 0;
    case WM_KEYDOWN: case WM_SYSKEYDOWN: case WM_KEYUP: case WM_SYSKEYUP:
        btn = wb_map_vk(wp);
        if (btn != LT_KEY_NONE) {
            e = wb_new_event(w, (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN)
                                ? LT_EV_KEY_DOWN : LT_EV_KEY_UP);
            if (e != NULL) {
                e->key = btn;
                e->mods = wb_mods();
            }
        }
        if (msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP) {
            return DefWindowProcW(hwnd, msg, wp, lp);   /* Alt+F4, system menu */
        }
        return 0;
    case WM_CHAR:
        u = (WCHAR)(wp & 0xFFFFUL);
        if (u >= 0xD800 && u < 0xDC00) {
            w->hi_surr = u;
            return 0;
        }
        if (u >= 0xDC00 && u < 0xE000) {
            if (w->hi_surr == 0) {
                return 0;
            }
            cp = 0x10000UL + (((lt_u32)w->hi_surr - 0xD800UL) << 10) + ((lt_u32)u - 0xDC00UL);
            w->hi_surr = 0;
        } else {
            w->hi_surr = 0;
            if (u < 32 || u == 127) {
                return 0;
            }
            cp = (lt_u32)u;
        }
        e = wb_new_event(w, LT_EV_TEXT);
        if (e != NULL) {
            int n = wb_put_utf8(e->text, cp);
            e->text[n] = '\0';
            e->mods = wb_mods();
        }
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ---- backend methods ---- */
static int wb_open(lt_backend *b, const char *title, int cw, int ch)
{
    wb_state *w = (wb_state *)b->impl;
    WNDCLASSEXW wc;
    RECT r;
    WCHAR *t;
    DWORD style = WS_OVERLAPPEDWINDOW;
    int ow, oh;

    if (w->opened) {
        return 0;
    }
    w->hinst = GetModuleHandleW(NULL);
    if (!w->class_ok) {
        memset(&wc, 0, sizeof(wc));
        wc.cbSize = (UINT)sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = wb_wndproc;
        wc.hInstance = w->hinst;
        wc.hCursor = LoadCursorW(NULL, MAKEINTRESOURCEW(32512));   /* IDC_ARROW */
        wc.lpszClassName = WB_CLASS_NAME;
        if (RegisterClassExW(&wc) == 0) {
            lt_err_set("win32: RegisterClassExW failed (%lu)", (unsigned long)GetLastError());
            return -1;
        }
        w->class_ok = 1;
    }
    wb_outer_size(cw, ch, style, wb_system_dpi(), &ow, &oh);
    w->cw = cw;
    w->ch = ch;
    t = wb_utf8_to_16(title);
    w->hwnd = CreateWindowExW(0, WB_CLASS_NAME, t, style,
                              (w->init_flags & LT_GEOM_POS) ? w->init_x : CW_USEDEFAULT,
                              (w->init_flags & LT_GEOM_POS) ? w->init_y : CW_USEDEFAULT,
                              ow, oh, NULL, NULL, w->hinst, w);
    lt_free(t);
    if (w->hwnd == NULL) {
        lt_err_set("win32: CreateWindowExW failed (%lu)", (unsigned long)GetLastError());
        (void)UnregisterClassW(WB_CLASS_NAME, w->hinst);
        w->class_ok = 0;
        return -1;
    }
    w->opened = 1;
    w->dpi = wb_window_dpi(w);
    (void)ShowWindow(w->hwnd, (w->init_flags & LT_GEOM_MAX) ? SW_SHOWMAXIMIZED : SW_SHOW);
    /* The real client size may differ from the request (work-area clamping). */
    if (GetClientRect(w->hwnd, &r)) {
        w->cw = (int)(r.right - r.left);
        w->ch = (int)(r.bottom - r.top);
    }
    /* drop the initial resize chatter; announce a full repaint instead */
    w->qn = 0;
    w->qh = 0;
    (void)wb_new_event(w, LT_EV_EXPOSE);
    return 0;
}

static void wb_close(lt_backend *b)
{
    wb_state *w = (wb_state *)b->impl;
    if (w == NULL) {
        return;
    }
    if (w->opened) {
        (void)DestroyWindow(w->hwnd);
        w->hwnd = NULL;
        w->opened = 0;
    }
    if (w->class_ok) {
        (void)UnregisterClassW(WB_CLASS_NAME, w->hinst);
        w->class_ok = 0;
    }
    w->canvas = NULL;
}

static void wb_pump(wb_state *w)
{
    MSG m;
    while (w->qn < WB_EVQ_CAP - 8 && PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) {
        (void)TranslateMessage(&m);
        (void)DispatchMessageW(&m);
    }
}

static int wb_poll(lt_backend *b, lt_event *ev, int timeout_ms)
{
    wb_state *w = (wb_state *)b->impl;
    DWORD ms;
    if (!w->opened) {
        return 0;
    }
    if (w->qn == 0) {
        wb_pump(w);
    }
    while (w->qn == 0 && timeout_ms != 0) {
        double t0 = lt_plat_now_ms();
        ms = timeout_ms < 0 ? INFINITE : (DWORD)timeout_ms;
        (void)MsgWaitForMultipleObjects(0, NULL, FALSE, ms, QS_ALLINPUT);
        wb_pump(w);
        if (w->qn == 0 && timeout_ms > 0) {
            /* woke for a message that produced no event: wait out the rest */
            timeout_ms -= (int)(lt_plat_now_ms() - t0);
            if (timeout_ms <= 0) {
                break;
            }
        }
    }
    if (w->qn == 0) {
        return 0;
    }
    *ev = w->q[w->qh];
    w->qh = (w->qh + 1) % WB_EVQ_CAP;
    w->qn--;
    return 1;
}

static void wb_present(lt_backend *b, lt_canvas *canvas, lt_cells *cells, lt_rect dirty)
{
    wb_state *w = (wb_state *)b->impl;
    HDC dc;
    (void)cells;
    if (!w->opened || canvas == NULL) {
        return;
    }
    w->canvas = canvas;
    if (dirty.w <= 0 || dirty.h <= 0) {
        return;
    }
    dc = GetDC(w->hwnd);
    if (dc == NULL) {
        return;
    }
    wb_blit(w, dc);
    (void)ReleaseDC(w->hwnd, dc);
}

static void wb_size(lt_backend *b, int *pw, int *ph)
{
    wb_state *w = (wb_state *)b->impl;
    *pw = w->cw;
    *ph = w->ch;
}

static void wb_set_title(lt_backend *b, const char *title)
{
    wb_state *w = (wb_state *)b->impl;
    WCHAR *t;
    if (!w->opened) {
        return;
    }
    t = wb_utf8_to_16(title);
    (void)SetWindowTextW(w->hwnd, t);
    lt_free(t);
}

static void wb_set_clipboard(lt_backend *b, const char *utf8)
{
    wb_state *w = (wb_state *)b->impl;
    WCHAR *t;
    HGLOBAL h;
    void *p;
    size_t n;
    if (!w->opened) {
        return;
    }
    t = wb_utf8_to_16(utf8);
    for (n = 0; t[n] != 0; n++) {
    }
    h = GlobalAlloc(GMEM_MOVEABLE, (n + 1) * sizeof(WCHAR));
    if (h != NULL) {
        p = GlobalLock(h);
        if (p != NULL) {
            memcpy(p, t, (n + 1) * sizeof(WCHAR));
            (void)GlobalUnlock(h);
            if (OpenClipboard(w->hwnd)) {
                (void)EmptyClipboard();
                if (SetClipboardData(CF_UNICODETEXT, h) == NULL) {
                    (void)GlobalFree(h);    /* ownership moves only on success */
                }
                (void)CloseClipboard();
            } else {
                (void)GlobalFree(h);
            }
        } else {
            (void)GlobalFree(h);
        }
    }
    lt_free(t);
}

static char *wb_get_clipboard(lt_backend *b)
{
    wb_state *w = (wb_state *)b->impl;
    HANDLE h;
    const WCHAR *p;
    char *out = NULL;
    size_t n, o = 0;
    lt_u32 cp;
    if (!w->opened || !OpenClipboard(w->hwnd)) {
        return NULL;
    }
    h = GetClipboardData(CF_UNICODETEXT);
    if (h != NULL) {
        p = (const WCHAR *)GlobalLock(h);
        if (p != NULL) {
            for (n = 0; p[n] != 0; n++) {
            }
            out = (char *)lt_malloc(n * 4 + 1);
            for (n = 0; p[n] != 0; n++) {
                cp = (lt_u32)p[n];
                if (cp >= 0xD800UL && cp < 0xDC00UL && p[n + 1] >= 0xDC00 && p[n + 1] < 0xE000) {
                    cp = 0x10000UL + ((cp - 0xD800UL) << 10) + ((lt_u32)p[n + 1] - 0xDC00UL);
                    n++;
                } else if (cp >= 0xD800UL && cp < 0xE000UL) {
                    cp = 0xFFFDUL;
                }
                o += (size_t)wb_put_utf8(out + o, cp);
            }
            out[o] = '\0';
            (void)GlobalUnlock(h);
        }
    }
    (void)CloseClipboard();
    return out;
}

static double wb_scale_hint(lt_backend *b)
{
    wb_state *w = (wb_state *)b->impl;
    UINT d = (w != NULL && w->opened) ? w->dpi : wb_system_dpi();
    return (double)d / 96.0;
}

static void wb_set_min_size(lt_backend *b, int mw, int mh)
{
    wb_state *w = (wb_state *)b->impl;
    w->min_w = mw;
    w->min_h = mh;
}

static int wb_screen_size(lt_backend *b, int *pw, int *ph)
{
    RECT wa;
    wb_state *w = (wb_state *)b->impl;
    (void)w;
    if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0)) {
        return 0;
    }
    if (pw != NULL) {
        *pw = (int)(wa.right - wa.left);
    }
    if (ph != NULL) {
        *ph = (int)(wa.bottom - wa.top);
    }
    return 1;
}

static void wb_set_initial_geometry(lt_backend *b, int x, int y, int flags)
{
    wb_state *w = (wb_state *)b->impl;
    w->init_x = x;
    w->init_y = y;
    w->init_flags = flags;
}

static int wb_get_geometry(lt_backend *b, int *px, int *py, int *maxd)
{
    wb_state *w = (wb_state *)b->impl;
    WINDOWPLACEMENT wp;
    if (w == NULL || !w->opened) {
        return 0;
    }
    memset(&wp, 0, sizeof(wp));
    wp.length = (UINT)sizeof(wp);
    if (!GetWindowPlacement(w->hwnd, &wp)) {
        return 0;
    }
    if (px != NULL) {
        *px = (int)wp.rcNormalPosition.left;
    }
    if (py != NULL) {
        *py = (int)wp.rcNormalPosition.top;
    }
    if (maxd != NULL) {
        *maxd = IsZoomed(w->hwnd) ? 1 : 0;
    }
    return 1;
}

lt_backend *lt_backend_win32_create(void)
{
    lt_backend *b;
    wb_state *w;
    /* Per-monitor v2 awareness (Win10 1703+), else system aware (Vista+). Must
     * happen before any window exists; harmless if the manifest already says so. */
    {
        wb_set_ctx_fn ctx = (wb_set_ctx_fn)wb_user32_proc("SetProcessDpiAwarenessContext");
        if (ctx == NULL || !ctx((HANDLE)(LONG_PTR)-4)) {
            (void)SetProcessDPIAware();
        }
    }
    w = (wb_state *)lt_calloc(1, sizeof(*w));
    w->dpi = wb_system_dpi();
    b = (lt_backend *)lt_calloc(1, sizeof(*b));
    b->name = "win32";
    b->cell_mode = LT_FALSE;
    b->open = wb_open;
    b->close = wb_close;
    b->poll = wb_poll;
    b->present = wb_present;
    b->size = wb_size;
    b->set_title = wb_set_title;
    b->set_clipboard = wb_set_clipboard;
    b->get_clipboard = wb_get_clipboard;
    b->scale_hint = wb_scale_hint;
    b->set_min_size = wb_set_min_size;
    b->screen_size = wb_screen_size;
    b->set_initial_geometry = wb_set_initial_geometry;
    b->get_geometry = wb_get_geometry;
    b->impl = w;
    return b;
}
