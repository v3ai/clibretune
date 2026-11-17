/* online_logic.h - assisted "Search INI Online" (G-17) without any network code: the INI source
 * pages of the Speeduino, FOME, rusEFI and epicEFI projects are opened in the browser and the
 * user's Downloads folder is watched for a new *.ini, which is then offered for import into the
 * INI library.  Unit-tested in tests/test_online.c.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef LT_ONLINE_LOGIC_H
#define LT_ONLINE_LOGIC_H

#include "../lt.h"

typedef struct lt_ionline_src {
    const char *id, *name, *url, *hint;
} lt_ionline_src;

int lt_ionline_n(void);
const lt_ionline_src *lt_ionline_get(int i);
/* source whose firmware the signature belongs to ("speeduino 202305" -> Speeduino, "rusEFI ..."
 * -> rusEFI, "FOME ..." -> FOME, "epicEFI ..." -> epicEFI); -1 when unknown */
int lt_ionline_pick(const char *signature);

/* directory to watch: $LT_DOWNLOADS_DIR, else <home>/Downloads when it exists; malloc'd or NULL */
char *lt_ionline_downloads_dir(void);

/* A directory watch: names present when the watch started are ignored, later *.ini files are
 * reported once. */
typedef struct lt_ionline_watch {
    char *dir;
    char **seen;
    size_t n_seen, cap_seen;
} lt_ionline_watch;
void lt_ionline_watch_init(lt_ionline_watch *w);
void lt_ionline_watch_free(lt_ionline_watch *w);
/* remember the current *.ini files of dir; 0 ok, -1 when dir cannot be read */
int  lt_ionline_watch_start(lt_ionline_watch *w, const char *dir);
/* new *.ini files since the last call, as full paths (lt_strv_free-able: malloc'd array of
 * malloc'd strings).  Files that are empty or do not look like an ECU definition (no
 * signature) are skipped and will be looked at again next time (a download in progress). */
size_t lt_ionline_watch_poll(lt_ionline_watch *w, char ***paths);

/* every ECU definition (*.ini with a signature) directly inside dir, as full paths */
size_t lt_ionline_list(const char *dir, char ***paths);

#endif
