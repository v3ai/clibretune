/* be_win32.c - Win32 GDI pixel backend.
 *
 * Two build modes share one code path (every user32/gdi32/kernel32 function
 * is resolved at runtime with LoadLibraryA + GetProcAddress):
 *   - Cosmopolitan (x86-64 only): active only when IsWindows() at runtime;
 *     function pointers use the Microsoft x64 ABI.
 *   - Native Windows compilers: WINAPI calling convention.
 * Everywhere else this file compiles to a stub returning NULL.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "gui.h"

#if (defined(__COSMOPOLITAN__) && defined(__x86_64__)) || \
    (defined(_WIN32) && !defined(__COSMOPOLITAN__))

#include <stddef.h>
#include <string.h>

#if defined(__COSMOPOLITAN__)
#include <cosmo.h>
#include <libc/nt/dll.h>
#define NT_CALL __attribute__((__ms_abi__))
#define NT_COSMO 1
#else
#include <windows.h>
#define NT_CALL WINAPI
#define NT_COSMO 0
#endif

/* ---- basic Win32 types (pointer-sized values are ptrdiff_t) ---- */
typedef ptrdiff_t nt_ptr;          /* HWND, HDC, HANDLE, WPARAM-ish, LRESULT */
typedef size_t nt_uptr;            /* WPARAM */
typedef unsigned int nt_u32;       /* DWORD, UINT */
typedef unsigned short nt_u16;     /* WCHAR, WORD */

typedef struct nt_point { int x, y; } nt_point;
typedef struct nt_rect { int left, top, right, bottom; } nt_rect;

typedef nt_ptr (NT_CALL *nt_wndproc)(nt_ptr hwnd, nt_u32 msg, nt_uptr wp, nt_ptr lp);

typedef struct nt_wndclassex {
    nt_u32 cbSize;
    nt_u32 style;
    nt_wndproc lpfnWndProc;
    int cbClsExtra;
    int cbWndExtra;
    nt_ptr hInstance;
    nt_ptr hIcon;
    nt_ptr hCursor;
    nt_ptr hbrBackground;
    const nt_u16 *lpszMenuName;
    const nt_u16 *lpszClassName;
    nt_ptr hIconSm;
} nt_wndclassex;

typedef struct nt_msg {
    nt_ptr hwnd;
    nt_u32 message;
    nt_uptr wParam;
    nt_ptr lParam;
    nt_u32 time;
    nt_point pt;
} nt_msg;

typedef struct nt_paintstruct {
    nt_ptr hdc;
    int fErase;
    nt_rect rcPaint;
    int fRestore;
    int fIncUpdate;
    unsigned char rgbReserved[32];
} nt_paintstruct;

typedef struct nt_bmih {
    nt_u32 biSize;
    int biWidth;
    int biHeight;
    nt_u16 biPlanes;
    nt_u16 biBitCount;
    nt_u32 biCompression;
    nt_u32 biSizeImage;
    int biXPelsPerMeter;
    int biYPelsPerMeter;
    nt_u32 biClrUsed;
    nt_u32 biClrImportant;
} nt_bmih;

typedef struct nt_bmi {
    nt_bmih hdr;
    nt_u32 colors[1];              /* RGBQUAD bmiColors[1] */
} nt_bmi;

/* compile-time layout checks (Win64 / LP64-like pointer size only) */
#define NT_ASSERT_SIZE(name, expr) typedef char name[(expr) ? 1 : -1]
NT_ASSERT_SIZE(nt_chk_int, sizeof(int) == 4);
NT_ASSERT_SIZE(nt_chk_u16, sizeof(nt_u16) == 2);
NT_ASSERT_SIZE(nt_chk_u32, sizeof(nt_u32) == 4);
NT_ASSERT_SIZE(nt_chk_point, sizeof(nt_point) == 8);
NT_ASSERT_SIZE(nt_chk_rect, sizeof(nt_rect) == 16);
NT_ASSERT_SIZE(nt_chk_bmih, sizeof(nt_bmih) == 40);
NT_ASSERT_SIZE(nt_chk_bmi, sizeof(nt_bmi) == 44);
#if defined(__x86_64__) || defined(_WIN64)
NT_ASSERT_SIZE(nt_chk_ptr, sizeof(nt_ptr) == 8);
NT_ASSERT_SIZE(nt_chk_wc, sizeof(nt_wndclassex) == 80);
NT_ASSERT_SIZE(nt_chk_wc_proc, offsetof(nt_wndclassex, lpfnWndProc) == 8);
NT_ASSERT_SIZE(nt_chk_wc_inst, offsetof(nt_wndclassex, hInstance) == 24);
NT_ASSERT_SIZE(nt_chk_wc_cls, offsetof(nt_wndclassex, lpszClassName) == 64);
NT_ASSERT_SIZE(nt_chk_msg, sizeof(nt_msg) == 48);
NT_ASSERT_SIZE(nt_chk_msg_time, offsetof(nt_msg, time) == 32);
NT_ASSERT_SIZE(nt_chk_msg_pt, offsetof(nt_msg, pt) == 36);
NT_ASSERT_SIZE(nt_chk_ps, sizeof(nt_paintstruct) == 72);
NT_ASSERT_SIZE(nt_chk_ps_rc, offsetof(nt_paintstruct, rcPaint) == 12);
#endif

/* ---- constants ---- */
#define NT_WM_SIZE 0x0005
#define NT_WM_KILLFOCUS 0x0008
#define NT_WM_PAINT 0x000F
#define NT_WM_CLOSE 0x0010
#define NT_WM_ERASEBKGND 0x0014
#define NT_WM_KEYDOWN 0x0100
#define NT_WM_CHAR 0x0102
#define NT_WM_SYSKEYDOWN 0x0104
#define NT_WM_MOUSEMOVE 0x0200
#define NT_WM_LBUTTONDOWN 0x0201
#define NT_WM_LBUTTONUP 0x0202
#define NT_WM_RBUTTONDOWN 0x0204
#define NT_WM_RBUTTONUP 0x0205
#define NT_WM_MBUTTONDOWN 0x0207
#define NT_WM_MBUTTONUP 0x0208
#define NT_WM_MOUSEWHEEL 0x020A
#define NT_WM_KEYUP 0x0101
#define NT_WM_SYSKEYUP 0x0105
#define NT_SIZE_MINIMIZED 1
#define NT_WS_OVERLAPPEDWINDOW 0x00CF0000UL
#define NT_CW_USEDEFAULT 0x80000000UL
#define NT_SW_SHOW 5
#define NT_PM_REMOVE 1
#define NT_QS_ALLINPUT 0x04FF
#define NT_INFINITE 0xFFFFFFFFUL
#define NT_IDC_ARROW 32512
#define NT_CS_HREDRAW 0x0002
#define NT_CS_VREDRAW 0x0001
#define NT_DIB_RGB_COLORS 0
#define NT_BI_RGB 0
#define NT_SRCCOPY 0x00CC0020UL
#define NT_CF_UNICODETEXT 13
#define NT_GMEM_MOVEABLE 0x0002
#define NT_VK_BACK 0x08
#define NT_VK_TAB 0x09
#define NT_VK_RETURN 0x0D
#define NT_VK_SHIFT 0x10
#define NT_VK_CONTROL 0x11
#define NT_VK_MENU 0x12
#define NT_VK_ESCAPE 0x1B
#define NT_VK_SPACE 0x20
#define NT_VK_PRIOR 0x21
#define NT_VK_NEXT 0x22
#define NT_VK_END 0x23
#define NT_VK_HOME 0x24
#define NT_VK_LEFT 0x25
#define NT_VK_UP 0x26
#define NT_VK_RIGHT 0x27
#define NT_VK_DOWN 0x28
#define NT_VK_INSERT 0x2D
#define NT_VK_DELETE 0x2E
#define NT_VK_LWIN 0x5B
#define NT_VK_RWIN 0x5C
#define NT_VK_NUMPAD0 0x60
#define NT_VK_NUMPAD9 0x69
#define NT_VK_F1 0x70
#define NT_VK_F12 0x7B

#define EVQ_CAP 128
#define CLASS_NAME_LEN 13

/* ---- runtime-resolved API ---- */
typedef struct nt_api {
    nt_ptr (NT_CALL *GetModuleHandleW)(const nt_u16 *name);
    nt_ptr (NT_CALL *GlobalAlloc)(nt_u32 flags, size_t n);
    void *(NT_CALL *GlobalLock)(nt_ptr h);
    int (NT_CALL *GlobalUnlock)(nt_ptr h);
    nt_ptr (NT_CALL *GlobalFree)(nt_ptr h);

    nt_u16 (NT_CALL *RegisterClassExW)(const nt_wndclassex *wc);
    int (NT_CALL *UnregisterClassW)(const nt_u16 *name, nt_ptr inst);
    nt_ptr (NT_CALL *CreateWindowExW)(nt_u32 ex, const nt_u16 *cls, const nt_u16 *title,
        nt_u32 style, int x, int y, int w, int h, nt_ptr parent, nt_ptr menu,
        nt_ptr inst, void *param);
    int (NT_CALL *DestroyWindow)(nt_ptr hwnd);
    int (NT_CALL *ShowWindow)(nt_ptr hwnd, int cmd);
    int (NT_CALL *AdjustWindowRectEx)(nt_rect *r, nt_u32 style, int menu, nt_u32 ex);
    nt_ptr (NT_CALL *DefWindowProcW)(nt_ptr hwnd, nt_u32 msg, nt_uptr wp, nt_ptr lp);
    nt_ptr (NT_CALL *LoadCursorW)(nt_ptr inst, const nt_u16 *name);
    int (NT_CALL *PeekMessageW)(nt_msg *m, nt_ptr hwnd, nt_u32 lo, nt_u32 hi, nt_u32 rm);
    int (NT_CALL *TranslateMessage)(const nt_msg *m);
    nt_ptr (NT_CALL *DispatchMessageW)(const nt_msg *m);
    nt_u32 (NT_CALL *MsgWaitForMultipleObjects)(nt_u32 n, const nt_ptr *h, int all,
        nt_u32 ms, nt_u32 mask);
    nt_ptr (NT_CALL *GetDC)(nt_ptr hwnd);
    int (NT_CALL *ReleaseDC)(nt_ptr hwnd, nt_ptr dc);
    nt_ptr (NT_CALL *BeginPaint)(nt_ptr hwnd, nt_paintstruct *ps);
    int (NT_CALL *EndPaint)(nt_ptr hwnd, const nt_paintstruct *ps);
    int (NT_CALL *ScreenToClient)(nt_ptr hwnd, nt_point *p);
    nt_ptr (NT_CALL *SetCapture)(nt_ptr hwnd);
    int (NT_CALL *ReleaseCapture)(void);
    short (NT_CALL *GetKeyState)(int vk);
    int (NT_CALL *SetWindowTextW)(nt_ptr hwnd, const nt_u16 *text);
    int (NT_CALL *SetProcessDPIAware)(void);
    int (NT_CALL *OpenClipboard)(nt_ptr hwnd);
    int (NT_CALL *CloseClipboard)(void);
    int (NT_CALL *EmptyClipboard)(void);
    nt_ptr (NT_CALL *SetClipboardData)(nt_u32 fmt, nt_ptr h);
    nt_ptr (NT_CALL *GetClipboardData)(nt_u32 fmt);
    int (NT_CALL *StretchDIBits)(nt_ptr dc, int xd, int yd, int wd, int hd, int xs, int ys,
        int ws, int hs, const void *bits, const nt_bmi *bmi, nt_u32 usage, nt_u32 rop);
} nt_api;

typedef struct w32 {
    nt_api f;
    nt_ptr hwnd;
    nt_ptr hinst;
    int cw, ch;                 /* client size */
    int opened;
    int class_ok;
    lt_canvas *canvas;          /* last presented canvas */
    nt_u16 hi_surr;             /* pending UTF-16 high surrogate */
    lt_event q[EVQ_CAP];
    int qh, qn;
} w32;

static w32 *g_w;                /* single instance; WndProc has no user data */

static const nt_u16 g_class[CLASS_NAME_LEN] = {
    'L', 'i', 'b', 'r', 'e', 'T', 'u', 'n', 'e', 'W', 'i', 'n', 0
};

/* ---- loading ---- */
static void *nt_sym(nt_ptr lib, const char *name)
{
    if (!lib) return NULL;
    return (void *)GetProcAddress((
#if NT_COSMO
        (int64_t)
#else
        (HMODULE)
#endif
        lib), name);
}

#define LOADFN(lib, name) \
    do { *(void **)&w->f.name = nt_sym((lib), #name); } while (0)

static int nt_load(w32 *w)
{
    nt_ptr k, u, g;
    k = (nt_ptr)LoadLibraryA("kernel32.dll");
    u = (nt_ptr)LoadLibraryA("user32.dll");
    g = (nt_ptr)LoadLibraryA("gdi32.dll");
    if (!k || !u || !g) return -1;
    LOADFN(k, GetModuleHandleW);
    LOADFN(k, GlobalAlloc);
    LOADFN(k, GlobalLock);
    LOADFN(k, GlobalUnlock);
    LOADFN(k, GlobalFree);
    LOADFN(u, RegisterClassExW);
    LOADFN(u, UnregisterClassW);
    LOADFN(u, CreateWindowExW);
    LOADFN(u, DestroyWindow);
    LOADFN(u, ShowWindow);
    LOADFN(u, AdjustWindowRectEx);
    LOADFN(u, DefWindowProcW);
    LOADFN(u, LoadCursorW);
    LOADFN(u, PeekMessageW);
    LOADFN(u, TranslateMessage);
    LOADFN(u, DispatchMessageW);
    LOADFN(u, MsgWaitForMultipleObjects);
    LOADFN(u, GetDC);
    LOADFN(u, ReleaseDC);
    LOADFN(u, BeginPaint);
    LOADFN(u, EndPaint);
    LOADFN(u, ScreenToClient);
    LOADFN(u, SetCapture);
    LOADFN(u, ReleaseCapture);
    LOADFN(u, GetKeyState);
    LOADFN(u, SetWindowTextW);
    LOADFN(u, SetProcessDPIAware);
    LOADFN(u, OpenClipboard);
    LOADFN(u, CloseClipboard);
    LOADFN(u, EmptyClipboard);
    LOADFN(u, SetClipboardData);
    LOADFN(u, GetClipboardData);
    LOADFN(g, StretchDIBits);
    /* required set; clipboard, DPI, capture and GetModuleHandleW are optional */
    if (!w->f.RegisterClassExW || !w->f.CreateWindowExW || !w->f.DestroyWindow ||
        !w->f.ShowWindow || !w->f.AdjustWindowRectEx || !w->f.DefWindowProcW ||
        !w->f.PeekMessageW || !w->f.TranslateMessage || !w->f.DispatchMessageW ||
        !w->f.GetDC || !w->f.ReleaseDC || !w->f.BeginPaint || !w->f.EndPaint ||
        !w->f.StretchDIBits)
        return -1;
    return 0;
}

/* ---- UTF helpers ---- */
/* UTF-8 -> UTF-16 (NUL terminated, malloc'd) */
static nt_u16 *utf8_to_16(const char *s)
{
    nt_u16 *o;
    size_t n = 0;
    const char *p;
    lt_u32 cp;
    if (!s) s = "";
    o = (nt_u16 *)lt_malloc((strlen(s) * 2 + 2) * sizeof(nt_u16));
    p = s;
    while (*p) {
        cp = lt_utf8_next(&p);
        if (cp >= 0x10000UL) {
            cp -= 0x10000UL;
            o[n++] = (nt_u16)(0xD800UL + (cp >> 10));
            o[n++] = (nt_u16)(0xDC00UL + (cp & 0x3FF));
        } else {
            o[n++] = (nt_u16)cp;
        }
    }
    o[n] = 0;
    return o;
}

static int put_utf8(char *d, lt_u32 cp)
{
    if (cp < 0x80UL) { d[0] = (char)cp; return 1; }
    if (cp < 0x800UL) {
        d[0] = (char)(0xC0 | (cp >> 6)); d[1] = (char)(0x80 | (cp & 0x3F)); return 2;
    }
    if (cp < 0x10000UL) {
        d[0] = (char)(0xE0 | (cp >> 12)); d[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        d[2] = (char)(0x80 | (cp & 0x3F)); return 3;
    }
    d[0] = (char)(0xF0 | (cp >> 18)); d[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    d[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); d[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/* ---- event queue ---- */
static lt_event *q_new(w32 *w, lt_ev_type t)
{
    lt_event *e;
    if (w->qn >= EVQ_CAP) return NULL;
    e = &w->q[(w->qh + w->qn) % EVQ_CAP];
    w->qn++;
    memset(e, 0, sizeof(*e));
    e->type = t;
    return e;
}

static int cur_mods(w32 *w)
{
    int m = 0;
    if (!w->f.GetKeyState) return 0;
    if (w->f.GetKeyState(NT_VK_SHIFT) & 0x8000) m |= LT_MOD_SHIFT;
    if (w->f.GetKeyState(NT_VK_CONTROL) & 0x8000) m |= LT_MOD_CTRL;
    if (w->f.GetKeyState(NT_VK_MENU) & 0x8000) m |= LT_MOD_ALT;
    if ((w->f.GetKeyState(NT_VK_LWIN) & 0x8000) || (w->f.GetKeyState(NT_VK_RWIN) & 0x8000))
        m |= LT_MOD_SUPER;
    return m;
}

static int map_vk(nt_uptr vk)
{
    if (vk >= 'A' && vk <= 'Z') return (int)vk + 32;
    if (vk >= '0' && vk <= '9') return (int)vk;
    if (vk >= NT_VK_NUMPAD0 && vk <= NT_VK_NUMPAD9) return '0' + (int)(vk - NT_VK_NUMPAD0);
    if (vk >= NT_VK_F1 && vk <= NT_VK_F12) return LT_KEY_F1 + (int)(vk - NT_VK_F1);
    switch (vk) {
    case NT_VK_BACK: return LT_KEY_BACKSPACE;
    case NT_VK_TAB: return LT_KEY_TAB;
    case NT_VK_RETURN: return LT_KEY_ENTER;
    case NT_VK_ESCAPE: return LT_KEY_ESCAPE;
    case NT_VK_SPACE: return ' ';
    case NT_VK_DELETE: return LT_KEY_DELETE;
    case NT_VK_UP: return LT_KEY_UP;
    case NT_VK_DOWN: return LT_KEY_DOWN;
    case NT_VK_LEFT: return LT_KEY_LEFT;
    case NT_VK_RIGHT: return LT_KEY_RIGHT;
    case NT_VK_HOME: return LT_KEY_HOME;
    case NT_VK_END: return LT_KEY_END;
    case NT_VK_PRIOR: return LT_KEY_PAGEUP;
    case NT_VK_NEXT: return LT_KEY_PAGEDOWN;
    case NT_VK_INSERT: return LT_KEY_INSERT;
    case 0xBA: return ';';
    case 0xBB: return '=';
    case 0xBC: return ',';
    case 0xBD: return '-';
    case 0xBE: return '.';
    case 0xBF: return '/';
    case 0xC0: return '`';
    case 0xDB: return '[';
    case 0xDC: return '\\';
    case 0xDD: return ']';
    case 0xDE: return '\'';
    case 0x6A: return '*';
    case 0x6B: return '+';
    case 0x6D: return '-';
    case 0x6E: return '.';
    case 0x6F: return '/';
    default: break;
    }
    return LT_KEY_NONE;
}

static int s16lo(nt_ptr lp) { return (short)(unsigned short)((nt_uptr)lp & 0xFFFF); }
static int s16hi(nt_ptr lp) { return (short)(unsigned short)(((nt_uptr)lp >> 16) & 0xFFFF); }

/* copy a canvas region to the window DC */
static void blit(w32 *w, nt_ptr dc, int x0, int y0, int x1, int y1)
{
    nt_bmi bmi;
    lt_canvas *c = w->canvas;
    if (!c || !c->px || c->w <= 0 || c->h <= 0) return;
    /* Always blit the whole canvas: partial top-down source-y is unreliable. */
    x0 = 0; y0 = 0; x1 = c->w; y1 = c->h;
    memset(&bmi, 0, sizeof(bmi));
    bmi.hdr.biSize = sizeof(nt_bmih);
    bmi.hdr.biWidth = c->stride;
    bmi.hdr.biHeight = -c->h;           /* top-down */
    bmi.hdr.biPlanes = 1;
    bmi.hdr.biBitCount = 32;
    bmi.hdr.biCompression = NT_BI_RGB;
    w->f.StretchDIBits(dc, x0, y0, x1 - x0, y1 - y0, x0, y0, x1 - x0, y1 - y0,
                       c->px, &bmi, NT_DIB_RGB_COLORS, NT_SRCCOPY);
}

/* ---- window procedure ---- */
static nt_ptr NT_CALL wndproc(nt_ptr hwnd, nt_u32 msg, nt_uptr wp, nt_ptr lp)
{
    w32 *w = g_w;
    lt_event *e;
    nt_point pt;
    nt_paintstruct ps;
    int btn, down;
    nt_u16 u;
    lt_u32 cp;

    if (!w) return 0;
    switch (msg) {
    case NT_WM_CLOSE:
        e = q_new(w, LT_EV_QUIT);
        (void)e;
        return 0;
    case NT_WM_ERASEBKGND:
        return 1;
    case NT_WM_SIZE:
        if (wp != NT_SIZE_MINIMIZED) {
            w->cw = (int)((nt_uptr)lp & 0xFFFF);
            w->ch = (int)(((nt_uptr)lp >> 16) & 0xFFFF);
            e = q_new(w, LT_EV_RESIZE);
            if (e) { e->x = w->cw; e->y = w->ch; e->mods = 0; }
            if (e) { e->button = 0; }
        }
        return 0;
    case NT_WM_PAINT:
        memset(&ps, 0, sizeof(ps));
        w->f.BeginPaint(hwnd, &ps);
        blit(w, ps.hdc, ps.rcPaint.left, ps.rcPaint.top, ps.rcPaint.right, ps.rcPaint.bottom);
        w->f.EndPaint(hwnd, &ps);
        return 0;
    case NT_WM_KILLFOCUS:
        q_new(w, LT_EV_FOCUS_LOST);
        return 0;
    case NT_WM_MOUSEMOVE:
        e = q_new(w, LT_EV_MOUSE_MOVE);
        if (e) { e->x = s16lo(lp); e->y = s16hi(lp); e->mods = cur_mods(w); }
        return 0;
    case NT_WM_LBUTTONDOWN: case NT_WM_RBUTTONDOWN: case NT_WM_MBUTTONDOWN:
    case NT_WM_LBUTTONUP: case NT_WM_RBUTTONUP: case NT_WM_MBUTTONUP:
        down = (msg == NT_WM_LBUTTONDOWN || msg == NT_WM_RBUTTONDOWN ||
                msg == NT_WM_MBUTTONDOWN);
        if (msg == NT_WM_LBUTTONDOWN || msg == NT_WM_LBUTTONUP) btn = LT_MB_LEFT;
        else if (msg == NT_WM_RBUTTONDOWN || msg == NT_WM_RBUTTONUP) btn = LT_MB_RIGHT;
        else btn = LT_MB_MIDDLE;
        if (down) { if (w->f.SetCapture) w->f.SetCapture(hwnd); }
        else { if (w->f.ReleaseCapture) w->f.ReleaseCapture(); }
        e = q_new(w, down ? LT_EV_MOUSE_DOWN : LT_EV_MOUSE_UP);
        if (e) { e->x = s16lo(lp); e->y = s16hi(lp); e->button = btn; e->mods = cur_mods(w); }
        return 0;
    case NT_WM_MOUSEWHEEL:
        pt.x = s16lo(lp);
        pt.y = s16hi(lp);
        if (w->f.ScreenToClient) w->f.ScreenToClient(hwnd, &pt);
        e = q_new(w, LT_EV_WHEEL);
        if (e) {
            int d = s16hi((nt_ptr)wp);
            e->x = pt.x; e->y = pt.y; e->mods = cur_mods(w);
            e->wheel = d / 120;
            if (e->wheel == 0) e->wheel = d > 0 ? 1 : -1;
        }
        return 0;
    case NT_WM_KEYDOWN: case NT_WM_SYSKEYDOWN: case NT_WM_KEYUP: case NT_WM_SYSKEYUP:
        btn = map_vk(wp);
        if (btn != LT_KEY_NONE) {
            e = q_new(w, (msg == NT_WM_KEYDOWN || msg == NT_WM_SYSKEYDOWN)
                         ? LT_EV_KEY_DOWN : LT_EV_KEY_UP);
            if (e) { e->key = btn; e->mods = cur_mods(w); }
        }
        if (msg == NT_WM_SYSKEYDOWN || msg == NT_WM_SYSKEYUP)
            return w->f.DefWindowProcW(hwnd, msg, wp, lp);  /* Alt+F4, menu keys */
        return 0;
    case NT_WM_CHAR:
        u = (nt_u16)(wp & 0xFFFF);
        if (u >= 0xD800 && u < 0xDC00) { w->hi_surr = u; return 0; }
        if (u >= 0xDC00 && u < 0xE000) {
            if (!w->hi_surr) return 0;
            cp = 0x10000UL + (((lt_u32)w->hi_surr - 0xD800UL) << 10) + ((lt_u32)u - 0xDC00UL);
            w->hi_surr = 0;
        } else {
            w->hi_surr = 0;
            if (u < 32 || u == 127) return 0;
            cp = u;
        }
        e = q_new(w, LT_EV_TEXT);
        if (e) {
            int n = put_utf8(e->text, cp);
            e->text[n] = '\0';
            e->mods = cur_mods(w);
        }
        return 0;
    default:
        break;
    }
    return w->f.DefWindowProcW(hwnd, msg, wp, lp);
}

/* ---- backend methods ---- */
static int be_open(lt_backend *b, const char *title, int cw, int ch)
{
    w32 *w = (w32 *)b->impl;
    nt_wndclassex wc;
    nt_rect r;
    nt_u16 *t;
    nt_u32 style = NT_WS_OVERLAPPEDWINDOW;

    if (w->opened) return 0;
    g_w = w;
    if (w->f.SetProcessDPIAware) w->f.SetProcessDPIAware();
    w->hinst = w->f.GetModuleHandleW ? w->f.GetModuleHandleW(NULL) : 0;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = NT_CS_HREDRAW | NT_CS_VREDRAW;
    wc.lpfnWndProc = wndproc;
    wc.hInstance = w->hinst;
    wc.hCursor = w->f.LoadCursorW ? w->f.LoadCursorW(0, (const nt_u16 *)(nt_ptr)NT_IDC_ARROW) : 0;
    wc.lpszClassName = g_class;
    if (!w->class_ok) {
        if (!w->f.RegisterClassExW(&wc)) {
            lt_err_set("win32: RegisterClassExW failed");
            return -1;
        }
        w->class_ok = 1;
    }
    r.left = 0; r.top = 0; r.right = cw; r.bottom = ch;
    w->f.AdjustWindowRectEx(&r, style, 0, 0);
    w->cw = cw;
    w->ch = ch;
    t = utf8_to_16(title);
    w->hwnd = w->f.CreateWindowExW(0, g_class, t, style,
                                   (int)NT_CW_USEDEFAULT, (int)NT_CW_USEDEFAULT,
                                   r.right - r.left, r.bottom - r.top, 0, 0, w->hinst, NULL);
    lt_free(t);
    if (!w->hwnd) {
        lt_err_set("win32: CreateWindowExW failed");
        return -1;
    }
    w->opened = 1;
    w->f.ShowWindow(w->hwnd, NT_SW_SHOW);
    /* drop the initial resize chatter; announce a full repaint instead */
    w->qn = 0;
    w->qh = 0;
    q_new(w, LT_EV_EXPOSE);
    return 0;
}

static void be_close(lt_backend *b)
{
    w32 *w = (w32 *)b->impl;
    if (w->opened) {
        w->f.DestroyWindow(w->hwnd);
        w->hwnd = 0;
        w->opened = 0;
        if (w->class_ok && w->f.UnregisterClassW) {
            w->f.UnregisterClassW(g_class, w->hinst);
            w->class_ok = 0;
        }
    }
    w->canvas = NULL;
}

static void pump(w32 *w)
{
    nt_msg m;
    while (w->qn < EVQ_CAP - 8 && w->f.PeekMessageW(&m, 0, 0, 0, NT_PM_REMOVE)) {
        w->f.TranslateMessage(&m);
        w->f.DispatchMessageW(&m);
    }
}

static int be_poll(lt_backend *b, lt_event *ev, int timeout_ms)
{
    w32 *w = (w32 *)b->impl;
    nt_u32 ms;
    if (!w->opened) return 0;
    for (;;) {
        if (w->qn == 0) {
            pump(w);
            if (w->qn == 0 && timeout_ms != 0) {
                ms = timeout_ms < 0 ? (nt_u32)NT_INFINITE : (nt_u32)timeout_ms;
                if (w->f.MsgWaitForMultipleObjects)
                    w->f.MsgWaitForMultipleObjects(0, NULL, 0, ms, NT_QS_ALLINPUT);
                pump(w);
                if (w->qn == 0 && timeout_ms < 0) continue;
            }
        }
        break;
    }
    if (w->qn == 0) return 0;
    *ev = w->q[w->qh];
    w->qh = (w->qh + 1) % EVQ_CAP;
    w->qn--;
    return 1;
}

static void be_present(lt_backend *b, lt_canvas *canvas, lt_cells *cells, lt_rect dirty)
{
    w32 *w = (w32 *)b->impl;
    nt_ptr dc;
    (void)cells;
    if (!w->opened || !canvas) return;
    w->canvas = canvas;
    if (dirty.w <= 0 || dirty.h <= 0) return;
    dc = w->f.GetDC(w->hwnd);
    if (!dc) return;
    blit(w, dc, dirty.x, dirty.y, dirty.x + dirty.w, dirty.y + dirty.h);
    w->f.ReleaseDC(w->hwnd, dc);
}

static void be_size(lt_backend *b, int *pw, int *ph)
{
    w32 *w = (w32 *)b->impl;
    *pw = w->cw;
    *ph = w->ch;
}

static void be_set_title(lt_backend *b, const char *title)
{
    w32 *w = (w32 *)b->impl;
    nt_u16 *t;
    if (!w->opened || !w->f.SetWindowTextW) return;
    t = utf8_to_16(title);
    w->f.SetWindowTextW(w->hwnd, t);
    lt_free(t);
}

static void be_set_clipboard(lt_backend *b, const char *utf8)
{
    w32 *w = (w32 *)b->impl;
    nt_u16 *t;
    nt_ptr h;
    void *p;
    size_t n;
    if (!w->opened || !w->f.OpenClipboard || !w->f.SetClipboardData || !w->f.EmptyClipboard ||
        !w->f.GlobalAlloc || !w->f.GlobalLock || !w->f.GlobalUnlock || !w->f.CloseClipboard)
        return;
    t = utf8_to_16(utf8);
    for (n = 0; t[n]; n++) {}
    h = w->f.GlobalAlloc(NT_GMEM_MOVEABLE, (n + 1) * sizeof(nt_u16));
    if (h) {
        p = w->f.GlobalLock(h);
        if (p) {
            memcpy(p, t, (n + 1) * sizeof(nt_u16));
            w->f.GlobalUnlock(h);
            if (w->f.OpenClipboard(w->hwnd)) {
                w->f.EmptyClipboard();
                if (!w->f.SetClipboardData(NT_CF_UNICODETEXT, h) && w->f.GlobalFree)
                    w->f.GlobalFree(h);   /* ownership transfers only on success */
                w->f.CloseClipboard();
            } else if (w->f.GlobalFree) {
                w->f.GlobalFree(h);
            }
        } else if (w->f.GlobalFree) {
            w->f.GlobalFree(h);
        }
    }
    lt_free(t);
}

static char *be_get_clipboard(lt_backend *b)
{
    w32 *w = (w32 *)b->impl;
    nt_ptr h;
    const nt_u16 *p;
    char *out = NULL;
    size_t n, o = 0;
    lt_u32 cp;
    if (!w->opened || !w->f.OpenClipboard || !w->f.GetClipboardData || !w->f.GlobalLock ||
        !w->f.GlobalUnlock || !w->f.CloseClipboard)
        return NULL;
    if (!w->f.OpenClipboard(w->hwnd)) return NULL;
    h = w->f.GetClipboardData(NT_CF_UNICODETEXT);
    if (h) {
        p = (const nt_u16 *)w->f.GlobalLock(h);
        if (p) {
            for (n = 0; p[n]; n++) {}
            out = (char *)lt_malloc(n * 4 + 1);
            for (n = 0; p[n]; n++) {
                cp = p[n];
                if (cp >= 0xD800 && cp < 0xDC00 && p[n + 1] >= 0xDC00 && p[n + 1] < 0xE000) {
                    cp = 0x10000UL + ((cp - 0xD800UL) << 10) + ((lt_u32)p[n + 1] - 0xDC00UL);
                    n++;
                } else if (cp >= 0xD800 && cp < 0xE000) {
                    cp = 0xFFFD;
                }
                o += (size_t)put_utf8(out + o, cp);
            }
            out[o] = '\0';
            w->f.GlobalUnlock(h);
        }
    }
    w->f.CloseClipboard();
    return out;
}

lt_backend *lt_backend_win32_create(void)
{
    lt_backend *b;
    w32 *w;
#if NT_COSMO
    if (!IsWindows()) return NULL;
#endif
    w = (w32 *)lt_calloc(1, sizeof(*w));
    if (nt_load(w) != 0) {
        lt_free(w);
        return NULL;
    }
    b = (lt_backend *)lt_calloc(1, sizeof(*b));
    b->name = "win32";
    b->cell_mode = LT_FALSE;
    b->open = be_open;
    b->close = be_close;
    b->poll = be_poll;
    b->present = be_present;
    b->size = be_size;
    b->set_title = be_set_title;
    b->set_clipboard = be_set_clipboard;
    b->get_clipboard = be_get_clipboard;
    b->impl = w;
    return b;
}

#else /* not Windows-capable */

lt_backend *lt_backend_win32_create(void)
{
    return NULL;
}

#endif
