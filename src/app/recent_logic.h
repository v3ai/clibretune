/* recent_logic.h - what the start screen shows about recent projects (name, ECU, when it was
 * last opened), kept in recent.conf of the configuration folder next to the plain list of
 * paths in settings.conf (which stays the order and File > Open Recent). No UI; unit-tested
 * in tests/test_start.c.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef LT_RECENT_LOGIC_H
#define LT_RECENT_LOGIC_H

#include "../lt.h"

typedef struct lt_recent_ent {
    char path[512];
    char name[96];
    char signature[96];
    char opened[24];           /* local "YYYY-MM-DD HH:MM:SS", "" unknown */
} lt_recent_ent;

typedef struct lt_recent {
    lt_recent_ent *e;
    int n;
} lt_recent;

void lt_recent_init(lt_recent *r);
void lt_recent_free(lt_recent *r);
int  lt_recent_load(lt_recent *r, const char *config_dir);       /* missing file = empty */
int  lt_recent_save(const lt_recent *r, const char *config_dir);
const lt_recent_ent *lt_recent_find(const lt_recent *r, const char *path);
/* the project at path was opened at `stamp` (adds or updates; newest first, at most 32) */
void lt_recent_touch(lt_recent *r, const char *path, const char *name, const char *signature,
                     const char *stamp);
/* forget path; 1 when it was known */
int  lt_recent_remove(lt_recent *r, const char *path);
/* Name and signature of a project from its project.json (for entries opened by an older
 * build); 0 ok. */
int  lt_recent_read_project(const char *dir, char *name, size_t ncap, char *sig, size_t scap);

/* "rusEFI (f407-discovery)", "Speeduino 2024.02", "MegaSquirt-III", or the signature itself
 * shortened; "" for "" */
void lt_recent_ecu_label(const char *signature, char *out, size_t cap);
/* When `stamp` was, seen from `now` (both "YYYY-MM-DD HH:MM:SS"): "Today 14:02",
 * "Yesterday 09:30", "3 days ago", "12 Mar 2025"; "" when a stamp is not valid. */
void lt_recent_age(const char *stamp, const char *now, char *out, size_t cap);

#endif
