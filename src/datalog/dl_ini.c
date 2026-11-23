/* dl_ini.c - glue between logs and the INI definition (channel lists, realtime values).
 * Uses only lt_ini data (no ini functions) so it links without the ini implementation.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <string.h>
#include "datalog.h"
#include "dl_view.h"

static lt_rt_set_fn g_rt_set;

/* virtual channels appended to the INI channel list (see dl_view.h) */
static char **g_vnames, **g_vunits;
static size_t g_vn;

void lt_datalog_set_virtual_channels(const char *const *names, const char *const *units, size_t n)
{
    size_t i;
    lt_strv_free(g_vnames, g_vn);
    lt_strv_free(g_vunits, g_vn);
    g_vnames = NULL;
    g_vunits = NULL;
    g_vn = 0;
    if (n == 0 || names == NULL) {
        return;
    }
    g_vnames = (char **)lt_calloc(n, sizeof(char *));
    g_vunits = (char **)lt_calloc(n, sizeof(char *));
    for (i = 0; i < n; i++) {
        g_vnames[i] = lt_strdup(names[i] != NULL ? names[i] : "");
        g_vunits[i] = lt_strdup(units != NULL && units[i] != NULL ? units[i] : "");
    }
    g_vn = n;
}

size_t lt_datalog_virtual_count(void)
{
    return g_vn;
}

const char *lt_datalog_virtual_name(size_t i)
{
    return i < g_vn ? g_vnames[i] : "";
}

const char *lt_datalog_virtual_units(size_t i)
{
    return i < g_vn ? g_vunits[i] : "";
}

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

    cap = (ini->n_dl_entries > 0 ? ini->n_dl_entries : ini->n_channels) + g_vn;
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
    for (i = 0; i < g_vn; i++) {
        size_t k;
        int dup = g_vnames[i][0] == '\0' || find_ch(ini, g_vnames[i]) != NULL;
        for (k = 0; k < n && !dup; k++) {
            if (strcmp(nv[k], g_vnames[i]) == 0) {
                dup = 1;
            }
        }
        if (dup) {
            continue;
        }
        nv[n] = lt_strdup(g_vnames[i]);
        uv[n] = lt_strdup(g_vunits[i]);
        n++;
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

int lt_log_use_channel_names(lt_log *log, const lt_ini *ini)
{
    int c, n = 0;
    size_t i;
    if (!log || !ini) {
        return 0;
    }
    for (c = 0; c < log->n_ch; c++) {
        const char *nm = log->names[c];
        if (!nm || find_ch(ini, nm)) {
            continue;
        }
        for (i = 0; i < ini->n_dl_entries; i++) {
            const lt_ini_dl_entry *e = &ini->dl_entries[i];
            if (!e->label || !e->channel || strcmp(e->label, nm) != 0 || !find_ch(ini, e->channel)) {
                continue;
            }
            if (lt_log_channel_index(log, e->channel) < 0) {
                lt_free(log->names[c]);
                log->names[c] = lt_strdup(e->channel);
                n++;
            }
            break;
        }
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
