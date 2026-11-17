/* plat_rand.c - Windows secure random bytes (RtlGenRandom).
 *
 * PLATFORM FILE: uses Win32 headers via win_priv.h (see the exception noted
 * there). RtlGenRandom is exported by advapi32 as SystemFunction036 and is
 * declared here because <ntsecapi.h> is not universally available. Built only
 * for TARGET=windows; link advapi32.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "win_priv.h"

extern BOOLEAN WINAPI SystemFunction036(PVOID buffer, ULONG length);

int lt_plat_random(void *buf, size_t n)
{
    unsigned char *p = (unsigned char *)buf;
    while (n > 0U) {
        ULONG chunk = n > 65536U ? 65536UL : (ULONG)n;
        if (!SystemFunction036(p, chunk)) {
            return -1;
        }
        p += chunk;
        n -= chunk;
    }
    return 0;
}
