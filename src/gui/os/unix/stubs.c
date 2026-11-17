/* stubs.c - Linux/BSD: constructors for backends that only exist on other
 * operating systems. The build system selects this file for TARGET=linux and
 * TARGET=bsd (tools/target.sh); there is no #ifdef anywhere.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "../../gui.h"

lt_backend *lt_backend_win32_create(void)
{
    return NULL;
}

lt_backend *lt_backend_cocoa_create(void)
{
    return NULL;
}
