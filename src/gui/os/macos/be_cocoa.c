/* be_cocoa.c - native macOS window backend, written in C against the
 * Objective-C runtime (no Objective-C source, no Apple headers).
 *
 * Structure
 *   - A window (NSWindow) holds one custom NSView subclass "LTCocoaView",
 *     created at run time with objc_allocateClassPair/class_addMethod. present
 *     invalidates only the dirty rectangles (setNeedsDisplayInRect:, one
 *     displayIfNeeded per frame); drawRect: copies just the rectangles AppKit
 *     asks for (getRectsBeingDrawn:count:) through a CGBitmapContext over that
 *     part of the canvas, so a small update never copies the whole canvas;
 *     its key/mouse/scroll overrides translate NSEvents into lt_event
 *     records pushed on a ring queue.
 *   - One delegate class "LTCocoaDelegate" serves as NSApplication delegate
 *     (Cmd-Q becomes a QUIT event instead of exiting) and NSWindow delegate
 *     (close button -> QUIT, resize -> RESIZE, focus loss -> FOCUS_LOST).
 *   - poll() pumps the AppKit event queue itself
 *     ([NSApp nextEventMatchingMask:untilDate:inMode:dequeue:] + sendEvent:);
 *     [NSApp run] is never used, so the app keeps its own main loop.
 *   - The pure parts (key codes, modifiers, text filtering, scroll
 *     accumulation) are in ../../be_cocoa_map.c and are unit-tested on every
 *     platform.
 *
 * HiDPI choice: the canvas is in logical points (1x). On a Retina display
 * Cocoa scales the image 2x with interpolation off, i.e. exact pixel
 * doubling: sharp, never blurred, and the UI keeps its layout units. The
 * canvas size is therefore already in logical points, so scale_hint reports
 * 1.0 (the OS does the Retina scaling; the app's own UI scale stays 100%).
 *
 * ABI notes (this file was written without access to a Mac):
 *   - objc_msgSend is declared as `void f(void)` and is only ever called
 *     through a function-pointer type that spells the exact signature of the
 *     method (the lc_msg_* wrappers below), which is what both x86_64 and
 *     arm64 require. Variadic prototypes would be wrong on arm64.
 *   - Methods that return a struct larger than 16 bytes (NSRect) need
 *     objc_msgSend_stret on x86_64 but not on arm64. To stay architecture
 *     neutral they are never sent through objc_msgSend: lc_imp_of() looks up
 *     the method implementation and the C compiler calls it with the
 *     correct struct-return convention. NSPoint/NSSize (two doubles) are
 *     handled the same way for uniformity.
 *   - Struct arguments (NSRect, NSPoint, NSSize) and doubles are passed by
 *     value through correctly typed function pointers; NSInteger/NSUInteger
 *     are long/unsigned long (LP64 is asserted); BOOL is read and written as
 *     signed char, which is correct for both ABIs (x86_64: signed char;
 *     arm64: bool with value 0/1).
 *
 * PLATFORM FILE: built only for TARGET=macos (tools/target.sh links
 * -framework Cocoa -framework CoreGraphics -framework CoreFoundation). The
 * Objective-C runtime and CoreGraphics symbols it uses are declared by hand
 * below instead of including Apple headers, so the file compiles under
 * -std=c89 -pedantic-errors like every other file and can be syntax-checked
 * on any host.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "../../gui.h"
#include "../../be_cocoa_map.h"
#include "../../../plat/plat.h"

LT_STATIC_ASSERT(sizeof(void *) == 8, cocoa_needs_lp64);
LT_STATIC_ASSERT(sizeof(long) == 8, cocoa_nsinteger_is_long);
LT_STATIC_ASSERT(sizeof(double) == 8, cocoa_cgfloat_is_double);

/* ---- Objective-C runtime (libobjc), declared by hand ------------------ */
typedef struct lc_object *lc_id;        /* id and Class */
typedef struct lc_selector *lc_sel;     /* SEL */
typedef struct lc_method *lc_method;    /* Method */
typedef void (*lc_imp)(void);           /* IMP, called only via exact casts */

extern lc_id objc_getClass(const char *name);
extern lc_sel sel_registerName(const char *name);
extern lc_id objc_allocateClassPair(lc_id super_class, const char *name, size_t extra);
extern void objc_registerClassPair(lc_id cls);
extern signed char class_addMethod(lc_id cls, lc_sel name, lc_imp imp, const char *types);
extern lc_method class_getInstanceMethod(lc_id cls, lc_sel name);
extern const char *method_getTypeEncoding(lc_method m);
extern lc_imp class_getMethodImplementation(lc_id cls, lc_sel name);
extern lc_id object_getClass(lc_id obj);
extern void objc_msgSend(void);
/* AppKit: plays the system alert sound */
extern void NSBeep(void);

/* Foundation / AppKit globals */
extern lc_id NSDefaultRunLoopMode;
extern lc_id NSPasteboardTypeString;

/* ---- CoreGraphics / CoreFoundation, declared by hand ------------------ */
typedef struct lc_cgpoint { double x, y; } lc_cgpoint;      /* CGPoint / NSPoint */
typedef struct lc_cgsize { double w, h; } lc_cgsize;        /* CGSize / NSSize */
typedef struct lc_cgrect { lc_cgpoint origin; lc_cgsize size; } lc_cgrect;  /* CGRect / NSRect */
typedef struct lc_cgcolorspace *lc_cgcs;
typedef struct lc_cgcontext *lc_cgctx;
typedef struct lc_cgimage *lc_cgimg;

LT_STATIC_ASSERT(sizeof(lc_cgpoint) == 16, cocoa_point_size);
LT_STATIC_ASSERT(sizeof(lc_cgsize) == 16, cocoa_size_size);
LT_STATIC_ASSERT(sizeof(lc_cgrect) == 32, cocoa_rect_size);

extern lc_cgcs CGColorSpaceCreateDeviceRGB(void);
extern void CGColorSpaceRelease(lc_cgcs cs);
extern lc_cgctx CGBitmapContextCreate(void *data, size_t width, size_t height,
                                      size_t bits_per_component, size_t bytes_per_row,
                                      lc_cgcs cs, lt_u32 bitmap_info);
extern lc_cgimg CGBitmapContextCreateImage(lc_cgctx ctx);
extern void CGContextRelease(lc_cgctx ctx);
extern void CGImageRelease(lc_cgimg img);
extern void CGContextDrawImage(lc_cgctx ctx, lc_cgrect rect, lc_cgimg img);
extern void CGContextSetInterpolationQuality(lc_cgctx ctx, int quality);
extern void CGContextSetRGBFillColor(lc_cgctx ctx, double r, double g, double b, double a);
extern void CGContextFillRect(lc_cgctx ctx, lc_cgrect rect);
extern void *CGSessionCopyCurrentDictionary(void);
extern void CFRelease(const void *cf);

/* kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little: a native-endian
 * 32-bit word 0x00RRGGBB, exactly the lt_canvas pixel format on the
 * little-endian Macs (x86_64, arm64). */
#define CO_BITMAP_INFO 0x2006UL
#define CO_INTERP_NONE 1

/* ---- AppKit constants -------------------------------------------------- */
#define CO_STYLE_MASK 15UL          /* titled | closable | miniaturizable | resizable */
#define CO_BACKING_BUFFERED 2UL
#define CO_POLICY_REGULAR 0L
#define CO_EVENT_MASK_ANY (~0UL)
#define CO_QUEUE_CAP 256
#define CO_MIN_W 320.0
#define CO_MIN_H 200.0

/* ---- message-send wrappers: one per distinct signature ---------------- */
typedef lc_id (*lc_f_id0)(lc_id, lc_sel);
typedef lc_id (*lc_f_id_id)(lc_id, lc_sel, lc_id);
typedef lc_id (*lc_f_id_cstr)(lc_id, lc_sel, const char *);
typedef lc_id (*lc_f_id_dbl)(lc_id, lc_sel, double);
typedef lc_id (*lc_f_id_rect)(lc_id, lc_sel, lc_cgrect);
typedef lc_id (*lc_f_id_rect_ul_ul_b)(lc_id, lc_sel, lc_cgrect, unsigned long, unsigned long,
                                       signed char);
typedef lc_id (*lc_f_id_ul_id_id_b)(lc_id, lc_sel, unsigned long, lc_id, lc_id, signed char);
typedef lc_id (*lc_f_id_id_sel_id)(lc_id, lc_sel, lc_id, lc_sel, lc_id);
typedef void (*lc_f_void0)(lc_id, lc_sel);
typedef void (*lc_f_void_id)(lc_id, lc_sel, lc_id);
typedef void (*lc_f_void_b)(lc_id, lc_sel, signed char);
typedef void (*lc_f_void_long)(lc_id, lc_sel, long);
typedef void (*lc_f_void_size)(lc_id, lc_sel, lc_cgsize);
typedef void (*lc_f_void_point)(lc_id, lc_sel, lc_cgpoint);
typedef void (*lc_f_void_rect)(lc_id, lc_sel, lc_cgrect);
typedef void (*lc_f_void_rects)(lc_id, lc_sel, const lc_cgrect **, long *);
typedef signed char (*lc_f_b0)(lc_id, lc_sel);
typedef signed char (*lc_f_b_id_id)(lc_id, lc_sel, lc_id, lc_id);
typedef unsigned long (*lc_f_ul0)(lc_id, lc_sel);
typedef long (*lc_f_long0)(lc_id, lc_sel);
typedef unsigned short (*lc_f_us0)(lc_id, lc_sel);
typedef double (*lc_f_dbl0)(lc_id, lc_sel);
typedef const char *(*lc_f_cstr0)(lc_id, lc_sel);
typedef void *(*lc_f_ptr0)(lc_id, lc_sel);

/* implementations obtained with lc_imp_of(); struct returns */
typedef lc_cgrect (*lc_i_rect0)(lc_id, lc_sel);
typedef lc_cgpoint (*lc_i_point0)(lc_id, lc_sel);
typedef lc_cgpoint (*lc_i_point_point_id)(lc_id, lc_sel, lc_cgpoint, lc_id);

/* objc_msgSend as a plain pointer, so the call sites below cast a pointer
 * value (never a function designator, which gcc rejects as an ABI mismatch). */
static lc_imp lc_msg_send(void)
{
    return (lc_imp)objc_msgSend;
}

#define CO_SEND(type) ((type)lc_msg_send())

static lc_sel lc_sel_of(const char *name)
{
    return sel_registerName(name);
}

static lc_id lc_cls(const char *name)
{
    return objc_getClass(name);
}

static lc_id lc_send0(lc_id o, const char *sel)
{
    return CO_SEND(lc_f_id0)(o, lc_sel_of(sel));
}

static lc_id lc_send_id(lc_id o, const char *sel, lc_id a)
{
    return CO_SEND(lc_f_id_id)(o, lc_sel_of(sel), a);
}

static void lc_send_void0(lc_id o, const char *sel)
{
    CO_SEND(lc_f_void0)(o, lc_sel_of(sel));
}

static void lc_send_void_id(lc_id o, const char *sel, lc_id a)
{
    CO_SEND(lc_f_void_id)(o, lc_sel_of(sel), a);
}

static void lc_send_void_b(lc_id o, const char *sel, int yes)
{
    CO_SEND(lc_f_void_b)(o, lc_sel_of(sel), (signed char)(yes ? 1 : 0));
}

/* Implementation lookup for struct-returning methods (see ABI notes). */
static lc_imp lc_imp_of(lc_id obj, const char *sel)
{
    return class_getMethodImplementation(object_getClass(obj), lc_sel_of(sel));
}

/* [obj bounds] / [event locationInWindow] / [view convertPoint:fromView:] */
static lc_cgrect lc_get_rect(lc_id obj, const char *sel)
{
    lc_sel s = lc_sel_of(sel);
    lc_i_rect0 f = (lc_i_rect0)class_getMethodImplementation(object_getClass(obj), s);
    return f(obj, s);
}

static lc_cgpoint lc_get_point(lc_id obj, const char *sel)
{
    lc_sel s = lc_sel_of(sel);
    lc_i_point0 f = (lc_i_point0)class_getMethodImplementation(object_getClass(obj), s);
    return f(obj, s);
}

static lc_cgpoint lc_convert_point(lc_id view, lc_cgpoint p)
{
    lc_sel s = lc_sel_of("convertPoint:fromView:");
    lc_i_point_point_id f = (lc_i_point_point_id)lc_imp_of(view, "convertPoint:fromView:");
    return f(view, s, p, NULL);
}

/* ---- autorelease pool (poll/present run outside any AppKit run loop) -- */
static lc_id lc_pool_new(void)
{
    return lc_send0(lc_send0(lc_cls("NSAutoreleasePool"), "alloc"), "init");
}

static void lc_pool_drain(lc_id pool)
{
    lc_send_void0(pool, "drain");
}

static lc_id lc_nsstring(const char *utf8)
{
    return CO_SEND(lc_f_id_cstr)(lc_cls("NSString"), lc_sel_of("stringWithUTF8String:"),
                                 utf8 != NULL ? utf8 : "");
}

/* ---- backend state ----------------------------------------------------- */
typedef struct lc_state {
    lc_id app;                      /* NSApplication */
    lc_id window;                   /* NSWindow */
    lc_id view;                     /* LTCocoaView */
    lc_id delegate;                 /* LTCocoaDelegate */
    int cw, ch;                     /* content size in points */
    int min_w, min_h;               /* requested minimum content size, 0 = default */
    int init_x, init_y, init_flags;
    int opened;
    int ctrl_click;                 /* a Control-click is being sent as a right click */
    lt_canvas *canvas;              /* last presented canvas */
    lc_cgcs cs;                     /* cached device RGB colour space */
    lt_cocoa_scroll scroll;
    lt_event q[CO_QUEUE_CAP];
    int qh, qn;
} lc_state;

/* The AppKit callbacks have no user-data slot, so the single open backend
 * is reachable through this pointer (set in open, cleared in close). */
static lc_state *g_cocoa = NULL;

static lt_event *lc_new_event(lc_state *s, lt_ev_type t)
{
    lt_event *e;
    if (s->qn >= CO_QUEUE_CAP) {
        return NULL;
    }
    e = &s->q[(s->qh + s->qn) % CO_QUEUE_CAP];
    s->qn++;
    memset(e, 0, sizeof(*e));
    e->type = t;
    return e;
}

/* double -> int, truncating toward -infinity for small magnitudes */
static int lc_to_int(double v)
{
    if (v > 1.0e6) {
        return 1000000;
    }
    if (v < -1.0e6) {
        return -1000000;
    }
    if (v >= 0.0) {
        return (int)v;
    }
    return -(int)(-v + 0.999999);
}

/* content-view size in points */
static void lc_refresh_size(lc_state *s)
{
    lc_cgrect r;
    if (s->view == NULL) {
        return;
    }
    r = lc_get_rect(s->view, "bounds");
    if (r.size.w >= 1.0 && r.size.h >= 1.0) {
        s->cw = lc_to_int(r.size.w);
        s->ch = lc_to_int(r.size.h);
    }
}

/* ---- event translation (called from the view/delegate IMPs) ----------- */
static int lc_event_mods(lc_id ev)
{
    return lt_cocoa_mods(CO_SEND(lc_f_ul0)(ev, lc_sel_of("modifierFlags")));
}

/* mouse position in canvas pixels: origin top-left, y down */
static void lc_event_pos(lc_state *s, lc_id ev, int *x, int *y)
{
    lc_cgpoint p = lc_get_point(ev, "locationInWindow");
    lc_cgpoint v = lc_convert_point(s->view, p);
    lc_cgrect b = lc_get_rect(s->view, "bounds");
    *x = lc_to_int(v.x);
    *y = lc_to_int(b.size.h - v.y);
}

static void lc_push_mouse(lc_state *s, lc_id ev, lt_ev_type t, int button)
{
    lt_event *e = lc_new_event(s, t);
    if (e == NULL) {
        return;
    }
    lc_event_pos(s, ev, &e->x, &e->y);
    e->button = button;
    e->mods = lc_event_mods(ev);
}

static void lc_on_mouse_down(lc_id self, lc_sel cmd, lc_id ev)
{
    int button = LT_MB_LEFT;
    (void)self;
    (void)cmd;
    if (g_cocoa == NULL) {
        return;
    }
    /* Control-click is the Mac way to ask for a context menu */
    if ((lc_event_mods(ev) & LT_MOD_CTRL) != 0) {
        button = LT_MB_RIGHT;
        g_cocoa->ctrl_click = 1;
    }
    lc_push_mouse(g_cocoa, ev, LT_EV_MOUSE_DOWN, button);
}

static void lc_on_mouse_up(lc_id self, lc_sel cmd, lc_id ev)
{
    int button = LT_MB_LEFT;
    (void)self;
    (void)cmd;
    if (g_cocoa == NULL) {
        return;
    }
    if (g_cocoa->ctrl_click) {
        button = LT_MB_RIGHT;
        g_cocoa->ctrl_click = 0;
    }
    lc_push_mouse(g_cocoa, ev, LT_EV_MOUSE_UP, button);
}

static void lc_on_right_down(lc_id self, lc_sel cmd, lc_id ev)
{
    (void)self;
    (void)cmd;
    if (g_cocoa != NULL) {
        lc_push_mouse(g_cocoa, ev, LT_EV_MOUSE_DOWN, LT_MB_RIGHT);
    }
}

static void lc_on_right_up(lc_id self, lc_sel cmd, lc_id ev)
{
    (void)self;
    (void)cmd;
    if (g_cocoa != NULL) {
        lc_push_mouse(g_cocoa, ev, LT_EV_MOUSE_UP, LT_MB_RIGHT);
    }
}

/* only the middle button (number 2) is reported; others are ignored */
static void lc_on_other_down(lc_id self, lc_sel cmd, lc_id ev)
{
    (void)self;
    (void)cmd;
    if (g_cocoa != NULL && CO_SEND(lc_f_long0)(ev, lc_sel_of("buttonNumber")) == 2L) {
        lc_push_mouse(g_cocoa, ev, LT_EV_MOUSE_DOWN, LT_MB_MIDDLE);
    }
}

static void lc_on_other_up(lc_id self, lc_sel cmd, lc_id ev)
{
    (void)self;
    (void)cmd;
    if (g_cocoa != NULL && CO_SEND(lc_f_long0)(ev, lc_sel_of("buttonNumber")) == 2L) {
        lc_push_mouse(g_cocoa, ev, LT_EV_MOUSE_UP, LT_MB_MIDDLE);
    }
}

/* moves and drags (with any button held) are both MOUSE_MOVE */
static void lc_on_mouse_move(lc_id self, lc_sel cmd, lc_id ev)
{
    (void)self;
    (void)cmd;
    if (g_cocoa != NULL) {
        lc_push_mouse(g_cocoa, ev, LT_EV_MOUSE_MOVE, 0);
    }
}

static void lc_on_scroll(lc_id self, lc_sel cmd, lc_id ev)
{
    int v, h, i, n, x, y, mods;
    lt_event *e;
    double dx, dy;
    lt_bool precise;
    (void)self;
    (void)cmd;
    if (g_cocoa == NULL) {
        return;
    }
    dx = CO_SEND(lc_f_dbl0)(ev, lc_sel_of("scrollingDeltaX"));
    dy = CO_SEND(lc_f_dbl0)(ev, lc_sel_of("scrollingDeltaY"));
    precise = CO_SEND(lc_f_b0)(ev, lc_sel_of("hasPreciseScrollingDeltas")) != 0 ? LT_TRUE : LT_FALSE;
    v = lt_cocoa_scroll_steps(&g_cocoa->scroll, dx, dy, precise, &h);
    lc_event_pos(g_cocoa, ev, &x, &y);
    mods = lc_event_mods(ev);
    n = v < 0 ? -v : v;
    for (i = 0; i < n; i++) {
        e = lc_new_event(g_cocoa, LT_EV_WHEEL);
        if (e != NULL) {
            e->x = x;
            e->y = y;
            e->mods = mods;
            e->wheel = v > 0 ? 1 : -1;
        }
    }
    n = h < 0 ? -h : h;
    for (i = 0; i < n; i++) {
        e = lc_new_event(g_cocoa, LT_EV_WHEEL);
        if (e != NULL) {
            e->x = x;
            e->y = y;
            e->mods = mods;
            e->wheel = 0;
            e->button = h > 0 ? 1 : -1;     /* horizontal: -1 left, +1 right */
        }
    }
}

/* key code for a key event: special keys by virtual key code, everything
 * else by the (layout-aware) character the key produces */
static int lc_key_of(lc_id ev)
{
    int key;
    lc_id str;
    const char *u;
    const char *p;
    key = lt_cocoa_map_keycode((unsigned int)CO_SEND(lc_f_us0)(ev, lc_sel_of("keyCode")));
    if (key != LT_KEY_NONE) {
        return key;
    }
    str = lc_send0(ev, "charactersIgnoringModifiers");
    if (str == NULL) {
        return LT_KEY_NONE;
    }
    u = CO_SEND(lc_f_cstr0)(str, lc_sel_of("UTF8String"));
    if (u == NULL || u[0] == '\0') {
        return LT_KEY_NONE;
    }
    p = u;
    return lt_cocoa_map_char(lt_utf8_next(&p));
}

static void lc_on_key(lc_id ev, int down)
{
    lt_event *e;
    int key, mods;
    lc_id str;
    const char *u;
    if (g_cocoa == NULL) {
        return;
    }
    mods = lc_event_mods(ev);
    key = lc_key_of(ev);
    if (key != LT_KEY_NONE) {
        e = lc_new_event(g_cocoa, down ? LT_EV_KEY_DOWN : LT_EV_KEY_UP);
        if (e != NULL) {
            e->key = key;
            e->mods = mods;
        }
    }
    /* typed text: only for key-down without Control/Command */
    if (down && (mods & (LT_MOD_CTRL | LT_MOD_SUPER)) == 0) {
        str = lc_send0(ev, "characters");
        if (str != NULL) {
            u = CO_SEND(lc_f_cstr0)(str, lc_sel_of("UTF8String"));
            if (u != NULL) {
                lt_event tmp;
                memset(&tmp, 0, sizeof(tmp));
                if (lt_cocoa_text(u, tmp.text, sizeof(tmp.text)) > 0) {
                    e = lc_new_event(g_cocoa, LT_EV_TEXT);
                    if (e != NULL) {
                        memcpy(e->text, tmp.text, sizeof(e->text));
                        e->mods = mods;
                    }
                }
            }
        }
    }
}

static void lc_on_key_down(lc_id self, lc_sel cmd, lc_id ev)
{
    (void)self;
    (void)cmd;
    lc_on_key(ev, 1);
}

static void lc_on_key_up(lc_id self, lc_sel cmd, lc_id ev)
{
    (void)self;
    (void)cmd;
    lc_on_key(ev, 0);
}

static signed char lc_yes(lc_id self, lc_sel cmd)
{
    (void)self;
    (void)cmd;
    return 1;
}

static signed char lc_yes_id(lc_id self, lc_sel cmd, lc_id arg)
{
    (void)self;
    (void)cmd;
    (void)arg;
    return 1;
}

/* ---- drawing ----------------------------------------------------------- */
/* view rectangle (points, y up, origin bottom-left) -> canvas rectangle (y down) */
static lt_rect lc_view_to_canvas(lc_cgrect r, double view_h)
{
    double x0 = r.origin.x, x1 = r.origin.x + r.size.w;
    double y0 = view_h - (r.origin.y + r.size.h), y1 = view_h - r.origin.y;
    int ix0, iy0, ix1, iy1;
    if (x0 < 0.0) { x0 = 0.0; }
    if (y0 < 0.0) { y0 = 0.0; }
    if (x1 > 65535.0) { x1 = 65535.0; }
    if (y1 > 65535.0) { y1 = 65535.0; }
    if (x1 <= x0 || y1 <= y0) {
        return lt_rect_make(0, 0, 0, 0);
    }
    ix0 = (int)x0;                          /* floor (non-negative) */
    iy0 = (int)y0;
    ix1 = (int)x1;                          /* ceil */
    if ((double)ix1 < x1) { ix1++; }
    iy1 = (int)y1;
    if ((double)iy1 < y1) { iy1++; }
    return lt_rect_make(ix0, iy0, ix1 - ix0, iy1 - iy0);
}

/* canvas rectangle -> view rectangle */
static lc_cgrect lc_canvas_to_view(lt_rect r, double view_h)
{
    lc_cgrect v;
    v.origin.x = (double)r.x;
    v.origin.y = view_h - (double)(r.y + r.h);
    v.size.w = (double)r.w;
    v.size.h = (double)r.h;
    return v;
}

/* copy canvas rectangle r (inside the canvas) to the same place in the view */
static void lc_draw_part(lc_state *s, lc_cgctx cg, const lt_canvas *c, lt_rect r, double view_h)
{
    lc_cgctx bmp;
    lc_cgimg img;
    /* a bitmap context over just this part of the canvas (row stride = canvas pitch): the
     * image made from it copies r, not the whole canvas */
    bmp = CGBitmapContextCreate(c->px + (size_t)r.y * (size_t)c->pitch + (size_t)r.x,
                                (size_t)r.w, (size_t)r.h, 8, (size_t)c->pitch * 4U, s->cs,
                                (lt_u32)CO_BITMAP_INFO);
    if (bmp == NULL) {
        return;
    }
    img = CGBitmapContextCreateImage(bmp);
    if (img != NULL) {
        CGContextDrawImage(cg, lc_canvas_to_view(r, view_h), img);
        CGImageRelease(img);
    }
    CGContextRelease(bmp);
}

static void lc_on_draw_rect(lc_id self, lc_sel cmd, lc_cgrect dirty)
{
    lc_state *s = g_cocoa;
    lc_id gctx;
    lc_cgctx cg;
    lc_cgrect bounds;
    const lc_cgrect *rects = NULL;
    long nrects = 0, i;
    lt_canvas *c;
    (void)cmd;
    if (s == NULL) {
        return;
    }
    gctx = lc_send0(lc_cls("NSGraphicsContext"), "currentContext");
    if (gctx == NULL) {
        return;
    }
    cg = (lc_cgctx)CO_SEND(lc_f_ptr0)(gctx, lc_sel_of("CGContext"));
    if (cg == NULL) {
        return;
    }
    bounds = lc_get_rect(self, "bounds");
    c = s->canvas;
    if (c == NULL || c->px == NULL || c->w <= 0 || c->h <= 0 ||
        (double)c->w < bounds.size.w || (double)c->h < bounds.size.h) {
        /* black where the canvas does not reach (AppKit clips this to the dirty region) */
        CGContextSetRGBFillColor(cg, 0.0, 0.0, 0.0, 1.0);
        CGContextFillRect(cg, dirty);
    }
    if (c == NULL || c->px == NULL || c->w <= 0 || c->h <= 0) {
        return;
    }
    if (s->cs == NULL) {
        s->cs = CGColorSpaceCreateDeviceRGB();
        if (s->cs == NULL) {
            return;
        }
    }
    CGContextSetInterpolationQuality(cg, CO_INTERP_NONE);
    /* the exact rectangles that need drawing (dirty is only their bounding box) */
    CO_SEND(lc_f_void_rects)(self, lc_sel_of("getRectsBeingDrawn:count:"), &rects, &nrects);
    if (rects == NULL || nrects <= 0) {
        rects = &dirty;
        nrects = 1;
    }
    for (i = 0; i < nrects; i++) {
        lt_rect r = lt_rect_intersect(lc_view_to_canvas(rects[i], bounds.size.h),
                                      lt_rect_make(0, 0, c->w, c->h));
        if (r.w > 0 && r.h > 0) {
            lc_draw_part(s, cg, c, r, bounds.size.h);
        }
    }
}

/* ---- delegate (NSApplication + NSWindow) ------------------------------- */
static unsigned long lc_app_should_terminate(lc_id self, lc_sel cmd, lc_id app)
{
    (void)self;
    (void)cmd;
    (void)app;
    if (g_cocoa != NULL) {
        (void)lc_new_event(g_cocoa, LT_EV_QUIT);
    }
    return 0UL;                     /* NSTerminateCancel: the app quits itself */
}

static signed char lc_window_should_close(lc_id self, lc_sel cmd, lc_id win)
{
    (void)self;
    (void)cmd;
    (void)win;
    if (g_cocoa != NULL) {
        (void)lc_new_event(g_cocoa, LT_EV_QUIT);
    }
    return 0;
}

static void lc_window_did_resize(lc_id self, lc_sel cmd, lc_id note)
{
    lt_event *e;
    (void)self;
    (void)cmd;
    (void)note;
    if (g_cocoa == NULL) {
        return;
    }
    lc_refresh_size(g_cocoa);
    e = lc_new_event(g_cocoa, LT_EV_RESIZE);
    if (e != NULL) {
        e->x = g_cocoa->cw;
        e->y = g_cocoa->ch;
    }
}

static void lc_window_resigned_key(lc_id self, lc_sel cmd, lc_id note)
{
    (void)self;
    (void)cmd;
    (void)note;
    if (g_cocoa != NULL) {
        (void)lc_new_event(g_cocoa, LT_EV_FOCUS_LOST);
    }
}

/* ---- class registration ------------------------------------------------- */
/* Add an override. The type encoding is copied from the superclass method
 * when it exists (so BOOL stays 'c' or 'B' as the platform defines it) and
 * falls back to `fallback` for protocol methods. */
static void lc_add(lc_id cls, lc_id super_class, const char *sel, void (*fn)(void),
                   const char *fallback)
{
    lc_sel s = lc_sel_of(sel);
    lc_method m = class_getInstanceMethod(super_class, s);
    const char *enc = (m != NULL) ? method_getTypeEncoding(m) : NULL;
    (void)class_addMethod(cls, s, (lc_imp)fn, enc != NULL ? enc : fallback);
}

static int lc_register_classes(lc_id *view_cls, lc_id *delegate_cls)
{
    lc_id nsview = lc_cls("NSView");
    lc_id nsobject = lc_cls("NSObject");
    lc_id v, d;
    if (nsview == NULL || nsobject == NULL) {
        return -1;
    }
    v = lc_cls("LTCocoaView");
    if (v == NULL) {
        v = objc_allocateClassPair(nsview, "LTCocoaView", 0U);
        if (v == NULL) {
            return -1;
        }
        lc_add(v, nsview, "drawRect:", (void (*)(void))lc_on_draw_rect, "v@:{CGRect={CGPoint=dd}{CGSize=dd}}");
        lc_add(v, nsview, "acceptsFirstResponder", (void (*)(void))lc_yes, "c@:");
        lc_add(v, nsview, "acceptsFirstMouse:", (void (*)(void))lc_yes_id, "c@:@");
        lc_add(v, nsview, "isOpaque", (void (*)(void))lc_yes, "c@:");
        lc_add(v, nsview, "keyDown:", (void (*)(void))lc_on_key_down, "v@:@");
        lc_add(v, nsview, "keyUp:", (void (*)(void))lc_on_key_up, "v@:@");
        lc_add(v, nsview, "mouseDown:", (void (*)(void))lc_on_mouse_down, "v@:@");
        lc_add(v, nsview, "mouseUp:", (void (*)(void))lc_on_mouse_up, "v@:@");
        lc_add(v, nsview, "mouseDragged:", (void (*)(void))lc_on_mouse_move, "v@:@");
        lc_add(v, nsview, "mouseMoved:", (void (*)(void))lc_on_mouse_move, "v@:@");
        lc_add(v, nsview, "rightMouseDown:", (void (*)(void))lc_on_right_down, "v@:@");
        lc_add(v, nsview, "rightMouseUp:", (void (*)(void))lc_on_right_up, "v@:@");
        lc_add(v, nsview, "rightMouseDragged:", (void (*)(void))lc_on_mouse_move, "v@:@");
        lc_add(v, nsview, "otherMouseDown:", (void (*)(void))lc_on_other_down, "v@:@");
        lc_add(v, nsview, "otherMouseUp:", (void (*)(void))lc_on_other_up, "v@:@");
        lc_add(v, nsview, "otherMouseDragged:", (void (*)(void))lc_on_mouse_move, "v@:@");
        lc_add(v, nsview, "scrollWheel:", (void (*)(void))lc_on_scroll, "v@:@");
        objc_registerClassPair(v);
    }
    d = lc_cls("LTCocoaDelegate");
    if (d == NULL) {
        d = objc_allocateClassPair(nsobject, "LTCocoaDelegate", 0U);
        if (d == NULL) {
            return -1;
        }
        lc_add(d, nsobject, "applicationShouldTerminate:", (void (*)(void))lc_app_should_terminate, "Q@:@");
        lc_add(d, nsobject, "windowShouldClose:", (void (*)(void))lc_window_should_close, "c@:@");
        lc_add(d, nsobject, "windowDidResize:", (void (*)(void))lc_window_did_resize, "v@:@");
        lc_add(d, nsobject, "windowDidResignKey:", (void (*)(void))lc_window_resigned_key, "v@:@");
        objc_registerClassPair(d);
    }
    *view_cls = v;
    *delegate_cls = d;
    return 0;
}

/* Minimal main menu so Cmd-Q and Cmd-W work and the app has a menu bar. */
static void lc_build_menu(lc_state *s)
{
    lc_id menu = lc_send0(lc_send0(lc_cls("NSMenu"), "alloc"), "init");
    lc_id item = lc_send0(lc_send0(lc_cls("NSMenuItem"), "alloc"), "init");
    lc_id sub = lc_send0(lc_send0(lc_cls("NSMenu"), "alloc"), "init");
    lc_f_id_id_sel_id add = CO_SEND(lc_f_id_id_sel_id);
    lc_send_void_id(menu, "addItem:", item);
    (void)add(sub, lc_sel_of("addItemWithTitle:action:keyEquivalent:"),
              lc_nsstring("Close Window"), lc_sel_of("performClose:"), lc_nsstring("w"));
    (void)add(sub, lc_sel_of("addItemWithTitle:action:keyEquivalent:"),
              lc_nsstring("Quit clibretune"), lc_sel_of("terminate:"), lc_nsstring("q"));
    lc_send_void_id(item, "setSubmenu:", sub);
    lc_send_void_id(s->app, "setMainMenu:", menu);
    lc_send_void0(sub, "release");
    lc_send_void0(item, "release");
    lc_send_void0(menu, "release");
}

/* ---- backend methods ------------------------------------------------------ */
static int lc_open(lt_backend *b, const char *title, int cw, int ch)
{
    lc_state *s = (lc_state *)b->impl;
    lc_id pool, view_cls, delegate_cls, win, view;
    lc_cgrect frame;
    lc_cgsize minsize;

    if (s->opened) {
        return 0;
    }
    if (cw < 1 || ch < 1) {
        lt_err_set("cocoa: bad window size %dx%d", cw, ch);
        return -1;
    }
    pool = lc_pool_new();
    if (lc_register_classes(&view_cls, &delegate_cls) != 0) {
        lc_pool_drain(pool);
        lt_err_set("cocoa: cannot register view classes");
        return -1;
    }
    s->app = lc_send0(lc_cls("NSApplication"), "sharedApplication");
    if (s->app == NULL) {
        lc_pool_drain(pool);
        lt_err_set("cocoa: NSApplication unavailable");
        return -1;
    }
    CO_SEND(lc_f_void_long)(s->app, lc_sel_of("setActivationPolicy:"), CO_POLICY_REGULAR);
    s->delegate = lc_send0(lc_send0(delegate_cls, "alloc"), "init");
    lc_send_void_id(s->app, "setDelegate:", s->delegate);
    lc_build_menu(s);

    frame.origin.x = 0.0;
    frame.origin.y = 0.0;
    frame.size.w = (double)cw;
    frame.size.h = (double)ch;
    win = CO_SEND(lc_f_id_rect_ul_ul_b)(lc_send0(lc_cls("NSWindow"), "alloc"),
              lc_sel_of("initWithContentRect:styleMask:backing:defer:"),
              frame, CO_STYLE_MASK, CO_BACKING_BUFFERED, (signed char)0);
    if (win == NULL) {
        lc_send_void_id(s->app, "setDelegate:", NULL);
        lc_send_void0(s->delegate, "release");
        s->delegate = NULL;
        lc_pool_drain(pool);
        lt_err_set("cocoa: cannot create window");
        return -1;
    }
    view = CO_SEND(lc_f_id_rect)(lc_send0(view_cls, "alloc"), lc_sel_of("initWithFrame:"), frame);
    s->window = win;
    s->view = view;
    s->scroll.acc_x = 0.0;
    s->scroll.acc_y = 0.0;
    s->ctrl_click = 0;
    s->cw = cw;
    s->ch = ch;
    g_cocoa = s;

    lc_send_void_b(win, "setReleasedWhenClosed:", 0);
    lc_send_void_id(win, "setTitle:", lc_nsstring(title));
    lc_send_void_id(win, "setContentView:", view);
    lc_send_void_id(win, "setDelegate:", s->delegate);
    lc_send_void_b(win, "setAcceptsMouseMovedEvents:", 1);
    minsize.w = s->min_w > 0 ? (double)s->min_w : CO_MIN_W;
    minsize.h = s->min_h > 0 ? (double)s->min_h : CO_MIN_H;
    CO_SEND(lc_f_void_size)(win, lc_sel_of("setContentMinSize:"), minsize);
    if (s->init_flags & LT_GEOM_POS) {
        /* init_y is measured from the top of the main screen */
        lc_cgrect sf = lc_get_rect(lc_send0(lc_cls("NSScreen"), "mainScreen"), "frame");
        lc_cgpoint tl;
        tl.x = (double)s->init_x;
        tl.y = sf.size.h - (double)s->init_y;
        CO_SEND(lc_f_void_point)(win, lc_sel_of("setFrameTopLeftPoint:"), tl);
    } else {
        lc_send_void0(win, "center");
    }
    (void)lc_send_id(win, "makeFirstResponder:", view);
    lc_send_void_id(win, "makeKeyAndOrderFront:", NULL);
    lc_send_void0(s->app, "finishLaunching");
    lc_send_void_b(s->app, "activateIgnoringOtherApps:", 1);
    if (s->init_flags & LT_GEOM_MAX) {
        lc_send_void_id(win, "zoom:", NULL);
    }
    lc_refresh_size(s);
    s->opened = 1;
    lc_pool_drain(pool);

    /* announce a full repaint; drop any resize chatter from the setup */
    s->qh = 0;
    s->qn = 0;
    (void)lc_new_event(s, LT_EV_EXPOSE);
    return 0;
}

static double lc_scale_hint(lt_backend *b)
{
    (void)b;
    return 1.0;      /* canvas is in points; AppKit scales it for Retina */
}

static void lc_set_min_size(lt_backend *b, int mw, int mh)
{
    lc_state *s = (lc_state *)b->impl;
    lc_cgsize m;
    s->min_w = mw;
    s->min_h = mh;
    if (s->opened && s->window != NULL) {
        m.w = (double)mw;
        m.h = (double)mh;
        CO_SEND(lc_f_void_size)(s->window, lc_sel_of("setContentMinSize:"), m);
    }
}

static int lc_screen_size(lt_backend *b, int *pw, int *ph)
{
    lc_cgrect r;
    lc_id scr;
    (void)b;
    scr = lc_send0(lc_cls("NSScreen"), "mainScreen");
    if (scr == NULL) {
        return 0;
    }
    r = lc_get_rect(scr, "visibleFrame");
    if (pw != NULL) {
        *pw = lc_to_int(r.size.w);
    }
    if (ph != NULL) {
        *ph = lc_to_int(r.size.h);
    }
    return 1;
}

static void lc_set_initial_geometry(lt_backend *b, int x, int y, int flags)
{
    lc_state *s = (lc_state *)b->impl;
    s->init_x = x;
    s->init_y = y;
    s->init_flags = flags;
}

static int lc_get_geometry(lt_backend *b, int *px, int *py, int *maxd)
{
    lc_state *s = (lc_state *)b->impl;
    lc_cgrect wf, sf;
    if (s == NULL || !s->opened || s->window == NULL) {
        return 0;
    }
    wf = lc_get_rect(s->window, "frame");
    sf = lc_get_rect(lc_send0(lc_cls("NSScreen"), "mainScreen"), "frame");
    if (px != NULL) {
        *px = lc_to_int(wf.origin.x);
    }
    if (py != NULL) {
        *py = lc_to_int(sf.size.h - (wf.origin.y + wf.size.h));
    }
    if (maxd != NULL) {
        *maxd = CO_SEND(lc_f_b0)(s->window, lc_sel_of("isZoomed")) ? 1 : 0;
    }
    return 1;
}

/* Full screen: NSWindow toggleFullScreen: toggles, so only send it when the state differs
 * (styleMask bit NSWindowStyleMaskFullScreen = 1 << 14). */
static void lc_set_fullscreen(lt_backend *b, int on)
{
    lc_state *s = (lc_state *)b->impl;
    unsigned long mask;
    int is_fs;
    if (s == NULL || !s->opened || s->window == NULL) {
        return;
    }
    mask = CO_SEND(lc_f_ul0)(s->window, lc_sel_of("styleMask"));
    is_fs = (mask & (1UL << 14)) != 0UL;
    if ((on ? 1 : 0) != is_fs) {
        lc_send_void_id(s->window, "toggleFullScreen:", NULL);
    }
}

static void lc_beep(lt_backend *b)
{
    LT_UNUSED(b);
    NSBeep();
}

static void lc_close(lt_backend *b)
{
    lc_state *s = (lc_state *)b->impl;
    lc_id pool;
    if (s == NULL) {
        return;
    }
    if (s->opened) {
        pool = lc_pool_new();
        lc_send_void_id(s->window, "setDelegate:", NULL);
        lc_send_void_id(s->app, "setDelegate:", NULL);
        lc_send_void_id(s->window, "orderOut:", NULL);
        lc_send_void0(s->window, "close");
        lc_send_void0(s->window, "release");     /* also releases the content view */
        lc_send_void0(s->view, "release");       /* our own reference from alloc */
        lc_send_void0(s->delegate, "release");
        s->window = NULL;
        s->view = NULL;
        s->delegate = NULL;
        s->opened = 0;
        lc_pool_drain(pool);
    }
    if (g_cocoa == s) {
        g_cocoa = NULL;
    }
    if (s->cs != NULL) {
        CGColorSpaceRelease(s->cs);
        s->cs = NULL;
    }
    s->canvas = NULL;
}

/* Run the AppKit event loop for up to timeout_ms (-1 = until an event is
 * queued), dispatching events into the view/delegate IMPs above. */
static void lc_pump(lc_state *s, int timeout_ms)
{
    double t0 = lt_plat_now_ms();
    int wait = timeout_ms;
    lc_id pool, date, ev;
    lc_f_id_ul_id_id_b next = CO_SEND(lc_f_id_ul_id_id_b);
    for (;;) {
        pool = lc_pool_new();
        if (wait < 0) {
            date = lc_send0(lc_cls("NSDate"), "distantFuture");
        } else if (wait == 0) {
            date = lc_send0(lc_cls("NSDate"), "distantPast");
        } else {
            date = CO_SEND(lc_f_id_dbl)(lc_cls("NSDate"),
                       lc_sel_of("dateWithTimeIntervalSinceNow:"), (double)wait / 1000.0);
        }
        ev = next(s->app, lc_sel_of("nextEventMatchingMask:untilDate:inMode:dequeue:"),
                  CO_EVENT_MASK_ANY, date, NSDefaultRunLoopMode, (signed char)1);
        while (ev != NULL && s->qn < CO_QUEUE_CAP - 16) {
            lc_send_void_id(s->app, "sendEvent:", ev);
            ev = next(s->app, lc_sel_of("nextEventMatchingMask:untilDate:inMode:dequeue:"),
                      CO_EVENT_MASK_ANY, lc_send0(lc_cls("NSDate"), "distantPast"),
                      NSDefaultRunLoopMode, (signed char)1);
        }
        lc_send_void0(s->app, "updateWindows");
        lc_pool_drain(pool);
        if (s->qn > 0 || timeout_ms == 0) {
            break;
        }
        if (timeout_ms > 0) {
            wait = timeout_ms - (int)(lt_plat_now_ms() - t0);
            if (wait <= 0) {
                break;
            }
        }
    }
}

static int lc_poll(lt_backend *b, lt_event *ev, int timeout_ms)
{
    lc_state *s = (lc_state *)b->impl;
    if (!s->opened) {
        return 0;
    }
    if (s->qn == 0) {
        lc_pump(s, timeout_ms);
    }
    if (s->qn == 0) {
        return 0;
    }
    *ev = s->q[s->qh];
    s->qh = (s->qh + 1) % CO_QUEUE_CAP;
    s->qn--;
    return 1;
}

/* invalidate the canvas rectangles (view coordinates), then draw them in one display pass */
static void lc_present_rects(lt_backend *b, lt_canvas *canvas, const lt_rect *rects, int n)
{
    lc_state *s = (lc_state *)b->impl;
    lc_id pool;
    lc_cgrect bounds;
    lc_f_void_rect inval = CO_SEND(lc_f_void_rect);
    int i, any = 0;
    if (!s->opened || canvas == NULL || rects == NULL) {
        return;
    }
    s->canvas = canvas;
    pool = lc_pool_new();
    bounds = lc_get_rect(s->view, "bounds");
    for (i = 0; i < n; i++) {
        if (rects[i].w > 0 && rects[i].h > 0) {
            inval(s->view, lc_sel_of("setNeedsDisplayInRect:"),
                  lc_canvas_to_view(rects[i], bounds.size.h));
            any = 1;
        }
    }
    if (any) {
        lc_send_void0(s->view, "displayIfNeeded");
    }
    lc_pool_drain(pool);
}

static void lc_present(lt_backend *b, lt_canvas *canvas, lt_cells *cells, lt_rect dirty)
{
    (void)cells;
    lc_present_rects(b, canvas, &dirty, 1);
}

static void lc_size(lt_backend *b, int *pw, int *ph)
{
    lc_state *s = (lc_state *)b->impl;
    *pw = s->cw;
    *ph = s->ch;
}

static void lc_set_title(lt_backend *b, const char *title)
{
    lc_state *s = (lc_state *)b->impl;
    lc_id pool;
    if (!s->opened) {
        return;
    }
    pool = lc_pool_new();
    lc_send_void_id(s->window, "setTitle:", lc_nsstring(title));
    lc_pool_drain(pool);
}

static void lc_set_clipboard(lt_backend *b, const char *utf8)
{
    lc_state *s = (lc_state *)b->impl;
    lc_id pool, pb;
    if (!s->opened || utf8 == NULL) {
        return;
    }
    pool = lc_pool_new();
    pb = lc_send0(lc_cls("NSPasteboard"), "generalPasteboard");
    if (pb != NULL) {
        (void)CO_SEND(lc_f_long0)(pb, lc_sel_of("clearContents"));
        (void)CO_SEND(lc_f_b_id_id)(pb, lc_sel_of("setString:forType:"), lc_nsstring(utf8),
                                    NSPasteboardTypeString);
    }
    lc_pool_drain(pool);
}

static char *lc_get_clipboard(lt_backend *b)
{
    lc_state *s = (lc_state *)b->impl;
    lc_id pool, pb, str;
    const char *u;
    char *out = NULL;
    if (!s->opened) {
        return NULL;
    }
    pool = lc_pool_new();
    pb = lc_send0(lc_cls("NSPasteboard"), "generalPasteboard");
    if (pb != NULL) {
        str = lc_send_id(pb, "stringForType:", NSPasteboardTypeString);
        if (str != NULL) {
            u = CO_SEND(lc_f_cstr0)(str, lc_sel_of("UTF8String"));
            if (u != NULL) {
                out = lt_strdup(u);
            }
        }
    }
    lc_pool_drain(pool);
    return out;
}

/* No window-server session (ssh without a console login, a daemon): AppKit
 * would abort the process, so report the backend as unavailable instead. */
static int lc_have_session(void)
{
    void *dict = CGSessionCopyCurrentDictionary();
    if (dict == NULL) {
        return 0;
    }
    CFRelease(dict);
    return 1;
}

lt_backend *lt_backend_cocoa_create(void)
{
    lt_backend *b;
    lc_state *s;
    if (!lc_have_session()) {
        return NULL;
    }
    s = (lc_state *)lt_calloc(1, sizeof(*s));
    b = (lt_backend *)lt_calloc(1, sizeof(*b));
    b->name = "cocoa";
    b->cell_mode = LT_FALSE;
    b->open = lc_open;
    b->close = lc_close;
    b->poll = lc_poll;
    b->present = lc_present;
    b->size = lc_size;
    b->set_title = lc_set_title;
    b->set_clipboard = lc_set_clipboard;
    b->get_clipboard = lc_get_clipboard;
    b->scale_hint = lc_scale_hint;
    b->set_min_size = lc_set_min_size;
    b->screen_size = lc_screen_size;
    b->set_initial_geometry = lc_set_initial_geometry;
    b->get_geometry = lc_get_geometry;
    b->set_fullscreen = lc_set_fullscreen;
    b->beep = lc_beep;
    b->present_rects = lc_present_rects;
    b->impl = s;
    return b;
}
