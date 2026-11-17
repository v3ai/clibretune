/* plat_rand.c - POSIX secure random bytes from /dev/urandom.
 *
 * PLATFORM FILE (src/plat/posix): the device path is the OS-specific part.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "posix_priv.h"

#include <stdio.h>

int lt_plat_random(void *buf, size_t n)
{
    FILE *f = fopen("/dev/urandom", "rb");
    size_t got;
    if (f == NULL) {
        return -1;
    }
    got = fread(buf, 1, n, f);
    fclose(f);
    return got == n ? 0 : -1;
}
