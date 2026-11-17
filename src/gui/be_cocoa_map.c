/* be_cocoa_map.c - see be_cocoa_map.h. Universal C: no OS headers.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "be_cocoa_map.h"

#include <string.h>

#define LT_COCOA_PRECISE_NOTCH 12.0   /* trackpad points per wheel notch */
#define LT_COCOA_MAX_NOTCHES 8

int lt_cocoa_mods(unsigned long flags)
{
    int m = 0;
    if ((flags & LT_COCOA_FLAG_SHIFT) != 0UL) {
        m |= LT_MOD_SHIFT;
    }
    if ((flags & LT_COCOA_FLAG_CONTROL) != 0UL) {
        m |= LT_MOD_CTRL;
    }
    if ((flags & LT_COCOA_FLAG_OPTION) != 0UL) {
        m |= LT_MOD_ALT;
    }
    if ((flags & LT_COCOA_FLAG_COMMAND) != 0UL) {
        m |= LT_MOD_SUPER;
    }
    return m;
}

int lt_cocoa_map_keycode(unsigned int keycode)
{
    switch (keycode) {
    case 0x24U: case 0x4CU: return LT_KEY_ENTER;     /* Return, keypad Enter */
    case 0x30U: return LT_KEY_TAB;
    case 0x33U: return LT_KEY_BACKSPACE;             /* the "delete" key */
    case 0x35U: return LT_KEY_ESCAPE;
    case 0x75U: return LT_KEY_DELETE;                /* forward delete */
    case 0x73U: return LT_KEY_HOME;
    case 0x77U: return LT_KEY_END;
    case 0x74U: return LT_KEY_PAGEUP;
    case 0x79U: return LT_KEY_PAGEDOWN;
    case 0x72U: return LT_KEY_INSERT;                /* Help */
    case 0x7BU: return LT_KEY_LEFT;
    case 0x7CU: return LT_KEY_RIGHT;
    case 0x7DU: return LT_KEY_DOWN;
    case 0x7EU: return LT_KEY_UP;
    case 0x7AU: return LT_KEY_F1;
    case 0x78U: return LT_KEY_F2;
    case 0x63U: return LT_KEY_F3;
    case 0x76U: return LT_KEY_F4;
    case 0x60U: return LT_KEY_F5;
    case 0x61U: return LT_KEY_F6;
    case 0x62U: return LT_KEY_F7;
    case 0x64U: return LT_KEY_F8;
    case 0x65U: return LT_KEY_F9;
    case 0x6DU: return LT_KEY_F10;
    case 0x67U: return LT_KEY_F11;
    case 0x6FU: return LT_KEY_F12;
    default: break;
    }
    return LT_KEY_NONE;
}

int lt_cocoa_map_char(lt_u32 cp)
{
    static const char shifted[] = "!@#$%^&*()_+{}|:\"<>?~";
    static const char plain[] = "1234567890-=[]\\;',./`";
    const char *p;
    if (cp >= (lt_u32)'A' && cp <= (lt_u32)'Z') {
        return (int)cp + 32;
    }
    if (cp < 0x20UL || cp > 0x7EUL) {
        return LT_KEY_NONE;
    }
    p = strchr(shifted, (int)cp);
    if (p != NULL && *p != '\0') {
        return (int)(unsigned char)plain[p - shifted];
    }
    return (int)cp;
}

int lt_cocoa_text(const char *utf8, char *dst, size_t cap)
{
    const char *p;
    const char *q;
    lt_u32 cp;
    size_t n = 0, len;
    if (dst == NULL || cap == 0U) {
        return 0;
    }
    dst[0] = '\0';
    if (utf8 == NULL) {
        return 0;
    }
    p = utf8;
    while (*p != '\0') {
        q = p;
        cp = lt_utf8_next(&q);
        len = (size_t)(q - p);
        if (len == 0U) {
            break;
        }
        if (cp >= 0x20UL && cp != 0x7FUL && !(cp >= 0x80UL && cp < 0xA0UL) &&
            !(cp >= 0xF700UL && cp <= 0xF8FFUL)) {
            if (n + len + 1U > cap) {
                break;
            }
            memcpy(dst + n, p, len);
            n += len;
        }
        p = q;
    }
    dst[n] = '\0';
    return (int)n;
}

/* One axis: add delta, emit whole notches, keep the remainder. A direction
 * change drops the remainder so reversing feels immediate. */
static int scroll_axis(double *acc, double d, lt_bool precise)
{
    double unit = precise ? LT_COCOA_PRECISE_NOTCH : 1.0;
    int n = 0;
    if (d == 0.0) {
        return 0;
    }
    if ((*acc > 0.0 && d < 0.0) || (*acc < 0.0 && d > 0.0)) {
        *acc = 0.0;
    }
    *acc += d;
    while (*acc >= unit && n < LT_COCOA_MAX_NOTCHES) {
        *acc -= unit;
        n++;
    }
    while (*acc <= -unit && n > -LT_COCOA_MAX_NOTCHES) {
        *acc += unit;
        n--;
    }
    if (*acc > unit) {
        *acc = unit;
    } else if (*acc < -unit) {
        *acc = -unit;
    }
    return n;
}

int lt_cocoa_scroll_steps(lt_cocoa_scroll *s, double dx, double dy, lt_bool precise,
                          int *hnotches)
{
    int v = scroll_axis(&s->acc_y, dy, precise);
    int h = scroll_axis(&s->acc_x, -dx, precise);   /* +dx = content moves right */
    if (hnotches != NULL) {
        *hnotches = h;
    }
    return v;
}
