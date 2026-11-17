/* stubs.c - macOS: constructors for backends that only exist on other
 * operating systems (selected by the build system for TARGET=macos).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "../../gui.h"

lt_backend *lt_backend_win32_create(void)
{
    return NULL;
}
