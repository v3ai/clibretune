/* plat_shm.c - POSIX System V shared memory segments (the X11 backend's MIT-SHM images).
 *
 * PLATFORM FILE (src/plat/posix): includes <sys/ipc.h> and <sys/shm.h>. Feature-test macros
 * come from the build (tools/target.sh).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "posix_priv.h"

#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/shm.h>

struct lt_shm {
    int id;
    void *addr;
    size_t size;
    int unlinked;
};

lt_shm *lt_shm_create(size_t size)
{
    lt_shm *m;
    int id;
    void *addr;
    if (size == 0U) {
        return NULL;
    }
    id = shmget(IPC_PRIVATE, size, IPC_CREAT | 0600);
    if (id < 0) {
        lt_err_set("shmget failed");
        return NULL;
    }
    addr = shmat(id, NULL, 0);
    if (addr == (void *)-1) {
        (void)shmctl(id, IPC_RMID, NULL);
        lt_err_set("shmat failed");
        return NULL;
    }
    m = (lt_shm *)lt_malloc(sizeof *m);
    m->id = id;
    m->addr = addr;
    m->size = size;
    m->unlinked = 0;
    return m;
}

void *lt_shm_addr(lt_shm *m)
{
    return m != NULL ? m->addr : NULL;
}

size_t lt_shm_size(lt_shm *m)
{
    return m != NULL ? m->size : 0U;
}

long lt_shm_id(lt_shm *m)
{
    return m != NULL ? (long)m->id : -1L;
}

void lt_shm_unlink(lt_shm *m)
{
    if (m != NULL && !m->unlinked) {
        (void)shmctl(m->id, IPC_RMID, NULL);
        m->unlinked = 1;
    }
}

void lt_shm_destroy(lt_shm *m)
{
    if (m == NULL) {
        return;
    }
    (void)shmdt(m->addr);
    lt_shm_unlink(m);
    lt_free(m);
}
