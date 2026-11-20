/* backend.c - backend selection and common destruction.
 *
 * Ownership rule (single place): lt_backend_create functions return a
 * heap lt_backend whose `impl` is a heap object. close() releases every
 * resource owned by impl (window, sockets, buffers) and MAY free impl and
 * set it to NULL (x11, web do); it must be safe to call more than once and
 * on a backend that was never opened. lt_backend_destroy() therefore calls
 * close(), frees whatever impl is left, then frees the struct.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <string.h>
#include "gui.h"
#include "../plat/plat.h"

static lt_backend *try_name(const char *name)
{
    if (lt_stricmp(name, "x11") == 0) {
        return lt_backend_x11_create();
    }
    if (lt_stricmp(name, "win32") == 0) {
        return lt_backend_win32_create();
    }
    if (lt_stricmp(name, "cocoa") == 0) {
        return lt_backend_cocoa_create();
    }
    if (lt_stricmp(name, "web") == 0) {
        return lt_backend_web_create(0);
    }
    if (lt_stricmp(name, "tty") == 0) {
        return lt_backend_tty_create();
    }
    return NULL;
}

lt_backend *lt_backend_select(const char *name)
{
    lt_backend *b;
    char *env = lt_plat_getenv("LT_BACKEND");
    char *disp;
    if (env != NULL && env[0] != '\0') {
        b = try_name(env);
        lt_free(env);
        return b;
    }
    lt_free(env);
    if (name != NULL && name[0] != '\0' && lt_stricmp(name, "auto") != 0) {
        return try_name(name);
    }
    /* auto: win32 -> cocoa -> x11 -> (macOS: web) -> tty -> web.
     * Constructors for other operating systems are build-selected stubs. */
    b = lt_backend_win32_create();
    if (b != NULL) {
        return b;
    }
    b = lt_backend_cocoa_create();
    if (b != NULL) {
        return b;
    }
    disp = lt_plat_getenv("DISPLAY");
    if (disp != NULL && disp[0] != '\0') {
        lt_free(disp);
        b = lt_backend_x11_create();
        if (b != NULL) {
            return b;
        }
    } else {
        lt_free(disp);
    }
    if (lt_plat_os() == LT_OS_MACOS) {
        b = lt_backend_web_create(0);
        if (b != NULL) {
            return b;
        }
    }
    if (lt_term_isatty()) {
        b = lt_backend_tty_create();
        if (b != NULL) {
            return b;
        }
    }
    return lt_backend_web_create(0);
}

void lt_backend_destroy(lt_backend *b)
{
    if (b == NULL) {
        return;
    }
    if (b->close != NULL) {
        b->close(b);
    }
    if (b->impl != NULL) {
        lt_free(b->impl);
        b->impl = NULL;
    }
    lt_free(b);
}

/* ------------------------------------------------------------------ */
/* NULL-safe wrappers for the optional hooks                           */
double lt_backend_scale_hint(lt_backend *b)
{
    double v = 1.0;
    if (b != NULL && b->scale_hint != NULL) { v = b->scale_hint(b); }
    if (!(v >= 0.5) || v > 8.0) { v = 1.0; }
    return v;
}

void lt_backend_set_min_size(lt_backend *b, int w, int h)
{
    if (b != NULL && b->set_min_size != NULL) { b->set_min_size(b, w, h); }
}

int lt_backend_screen_size(lt_backend *b, int *w, int *h)
{
    if (b == NULL || b->screen_size == NULL) { return 0; }
    return b->screen_size(b, w, h);
}

void lt_backend_set_initial_geometry(lt_backend *b, int x, int y, int flags)
{
    if (b != NULL && b->set_initial_geometry != NULL) { b->set_initial_geometry(b, x, y, flags); }
}

int lt_backend_get_geometry(lt_backend *b, int *x, int *y, int *maximized)
{
    if (b == NULL || b->get_geometry == NULL) { return 0; }
    return b->get_geometry(b, x, y, maximized);
}

int lt_backend_set_fullscreen(lt_backend *b, int on)
{
    if (b == NULL || b->set_fullscreen == NULL) { return 0; }
    b->set_fullscreen(b, on);
    return 1;
}

int lt_backend_beep(lt_backend *b)
{
    if (b == NULL || b->beep == NULL) { return 0; }
    b->beep(b);
    return 1;
}
