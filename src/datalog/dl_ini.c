/* dl_ini.c - glue between logs and the INI definition (channel lists, realtime values).
 * Uses only lt_ini data (no ini functions) so it links without the ini implementation.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <string.h>
#include "datalog.h"

static lt_rt_set_fn g_rt_set;

void lt_datalog_set_rt_setter(lt_rt_set_fn fn)
{
    g_rt_set = fn;
}

static const lt_ini_channel *find_ch(const lt_ini *ini, const char *name)
{
    return (const lt_ini_channel *)lt_map_get(&ini->channel_idx, name);
}

size_t lt_datalog_channels_from_ini(const lt_ini *ini, char ***names, char ***units)
{
    size_t i, n = 0, cap;
    char **nv, **uv;

    cap = ini->n_dl_entries > 0 ? ini->n_dl_entries : ini->n_channels;
    nv = (char **)lt_calloc(cap + 1, sizeof(char *));
    uv = (char **)lt_calloc(cap + 1, sizeof(char *));
    if (ini->n_dl_entries > 0) {
        for (i = 0; i < ini->n_dl_entries; i++) {
            const lt_ini_dl_entry *e = &ini->dl_entries[i];
            const lt_ini_channel *ch;
            size_t k;
            int dup = 0;
            if (!e->enabled || !e->channel) {
                continue;
            }
            ch = find_ch(ini, e->channel);
            if (!ch) {
                continue;
            }
            for (k = 0; k < n; k++) {
                if (strcmp(nv[k], ch->name) == 0) {
                    dup = 1;
                    break;
                }
            }
            if (dup) {
                continue;
            }
            nv[n] = lt_strdup(ch->name);
            uv[n] = lt_strdup(ch->units ? ch->units : "");
            n++;
        }
    } else {
        for (i = 0; i < ini->n_channels; i++) {
            const lt_ini_channel *ch = ini->channels[i];
            nv[n] = lt_strdup(ch->name);
            uv[n] = lt_strdup(ch->units ? ch->units : "");
            n++;
        }
    }
    if (names) {
        *names = nv;
    } else {
        lt_strv_free(nv, n);
    }
    if (units) {
        *units = uv;
    } else {
        lt_strv_free(uv, n);
    }
    return n;
}

void lt_player_apply_to_rt(const lt_player *p, const lt_ini *ini, lt_ini_rt *rt)
{
    const double *row = lt_player_row(p);
    int c;
    if (!row || !g_rt_set || !ini || !rt) {
        return;
    }
    for (c = 0; c < p->log->n_ch; c++) {
        const lt_ini_channel *ch = find_ch(ini, p->log->names[c]);
        if (ch) {
            g_rt_set(rt, ch->index, row[c]);
        }
    }
}
