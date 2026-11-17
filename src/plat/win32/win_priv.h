/* win_priv.h - internal helpers for the Windows platform files.
 *
 * PLATFORM FILE EXCEPTION: includes <winsock2.h>/<windows.h>. These OS headers
 * are not guaranteed to pass -pedantic-errors, so Windows platform files are
 * compiled by tools/check_universal.sh with the same flags but are the only
 * place where Win32 headers may be used. The macros below must precede the
 * system headers, so this header is the first include of every Windows file.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef LT_WIN_PRIV_H
#define LT_WIN_PRIV_H

#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0601
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <shellapi.h>

#include "../plat_priv.h"

#endif /* LT_WIN_PRIV_H */
