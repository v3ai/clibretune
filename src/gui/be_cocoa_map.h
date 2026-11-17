/* be_cocoa_map.h - the pure (OS-independent) half of the macOS Cocoa backend:
 * virtual key codes, modifier flags, typed-text filtering and scroll
 * accumulation. It is universal C so the logic is unit-tested on every
 * platform (tests/test_cocoa_map.c); the Objective-C runtime glue lives in
 * os/macos/be_cocoa.c.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef LT_BE_COCOA_MAP_H
#define LT_BE_COCOA_MAP_H

#include "gui.h"

LT_BEGIN_DECLS

/* NSEvent modifierFlags bits (NSEventModifierFlag*) */
#define LT_COCOA_FLAG_SHIFT 0x20000UL
#define LT_COCOA_FLAG_CONTROL 0x40000UL
#define LT_COCOA_FLAG_OPTION 0x80000UL
#define LT_COCOA_FLAG_COMMAND 0x100000UL

/* NSEvent modifierFlags -> LT_MOD_* (Command = LT_MOD_SUPER, Option = ALT) */
int lt_cocoa_mods(unsigned long flags);

/* Non-printing keys by Carbon virtual key code (kVK_*): arrows, function
 * keys, return, tab, delete... Returns LT_KEY_NONE for every other code. */
int lt_cocoa_map_keycode(unsigned int keycode);

/* Printable key from the code point of NSEvent charactersIgnoringModifiers:
 * ASCII letters fold to lowercase and shifted US punctuation folds to the
 * unshifted key ('!' -> '1'), so LT_KEY_* matches the other backends.
 * Returns LT_KEY_NONE for anything that is not a printable ASCII key. */
int lt_cocoa_map_char(lt_u32 cp);

/* Copy the typed text of NSEvent characters (UTF-8) into dst (cap bytes
 * including the NUL). Control characters and the AppKit private-use code
 * points for function keys (U+F700..U+F8FF) are dropped; the copy never
 * splits a code point. Returns the number of bytes stored (0 = no text). */
int lt_cocoa_text(const char *utf8, char *dst, size_t cap);

/* Scroll accumulator: trackpads report points, wheels report lines. */
typedef struct lt_cocoa_scroll {
    double acc_y;
    double acc_x;
} lt_cocoa_scroll;

/* Feed one scroll event; returns the signed number of wheel notches to emit
 * on the vertical axis (positive = up) and sets *hnotches (positive = right)
 * for the horizontal axis. Each is limited to +-8 per event. */
int lt_cocoa_scroll_steps(lt_cocoa_scroll *s, double dx, double dy, lt_bool precise,
                          int *hnotches);

LT_END_DECLS

#endif /* LT_BE_COCOA_MAP_H */
