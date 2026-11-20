/* plat_shm.c - Windows has no System V shared memory (and no X server to share it with):
 * every call reports "unsupported", so the X11 backend uses plain PutImage requests.
 *
 * PLATFORM FILE: includes Win32 headers via win_priv.h (see the exception noted there).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "win_priv.h"

lt_shm *lt_shm_create(size_t size)
{
    (void)size;
    lt_err_set("shared memory segments are not supported on Windows");
    return NULL;
}

void *lt_shm_addr(lt_shm *m)
{
    (void)m;
    return NULL;
}

size_t lt_shm_size(lt_shm *m)
{
    (void)m;
    return 0U;
}

long lt_shm_id(lt_shm *m)
{
    (void)m;
    return -1L;
}

void lt_shm_unlink(lt_shm *m)
{
    (void)m;
}

void lt_shm_destroy(lt_shm *m)
{
    (void)m;
}
