/* sim_logger.c - demo ECU trigger logger and console.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <math.h>
#include <string.h>
#include "sim_logger.h"
#include "../proto/proto.h"

#define STEPS 144          /* 5-degree steps in a 720 degree cycle */

static const char *const fnames[6] = { "priLevel", "secLevel", "sync", "trigger", "coil", "inj" };

/* levels at one step: 36-1 crank wheel (tooth every 10 deg, 5 deg high), cam pulse at
 * 100..125 deg of the cycle, one coil dwell and injector pulse per 180 deg (4 cylinders) */
static void levels_at(int step, int synced, int lvl[6])
{
    int ang = step * 5, crank = ang % 360, tooth = crank / 10, in_cyc = ang % 180;
    lvl[0] = (tooth < 35 && (crank % 10) < 5) ? 1 : 0;
    lvl[1] = (ang >= 100 && ang < 125) ? 1 : 0;
    lvl[2] = synced;
    lvl[3] = lvl[0];
    lvl[4] = (in_cyc >= 125 && in_cyc < 170) ? 1 : 0;
    lvl[5] = (in_cyc >= 20 && in_cyc < 75) ? 1 : 0;
}

static lt_u32 rnd(lt_simlog *l)
{
    lt_u32 x = l->rng != 0 ? l->rng : 0x2545F491u;
    x ^= (x << 13) & 0xFFFFFFFFu;
    x ^= x >> 17;
    x ^= (x << 5) & 0xFFFFFFFFu;
    l->rng = x & 0xFFFFFFFFu;
    return l->rng;
}

static void put_bits(lt_u8 *rec, size_t len, unsigned start, unsigned count, lt_u32 v)
{
    unsigned i;
    for (i = 0; i < count && i < 32; i++) {
        unsigned bit = start + i;
        if (bit / 8 >= len) { return; }
        if ((v >> i) & 1u) { rec[len - 1 - bit / 8] |= (lt_u8)(1u << (bit % 8)); }
    }
}

static const lt_ini_logger *find_logger(const lt_ini *ini, const lt_u8 *cmd, size_t n, int *which)
{
    size_t i;
    for (i = 0; ini != NULL && i < ini->n_loggers; i++) {
        const lt_ini_logger *lg = &ini->loggers[i];
        const lt_ini_blob *b[3];
        int k;
        b[0] = &lg->start_command; b[1] = &lg->stop_command; b[2] = &lg->data_read_command;
        for (k = 0; k < 3; k++) {
            lt_buf c;
            int eq;
            if (b[k]->n == 0) { continue; }
            lt_buf_init(&c);
            lt_cmd_parse_string(b[k]->p, b[k]->n, &c);
            eq = c.len == n && memcmp(c.data, cmd, n) == 0;
            lt_buf_free(&c);
            if (eq) { *which = k; return lg; }
        }
    }
    return NULL;
}

static int is_trigger_logger(const lt_ini_logger *lg)
{
    return lg->kind != NULL && (lt_stricmp(lg->kind, "composite") == 0 ||
                                lt_stricmp(lg->kind, "tooth") == 0);
}

static void make_records(lt_simlog *l, const lt_ini_logger *lg, double rpm, lt_buf *out)
{
    int f[6], ft = -1, ftt = -1, k, made = 0;
    size_t i;
    double us_per_step;
    for (k = 0; k < 6; k++) { f[k] = -1; }
    for (i = 0; i < lg->n_fields; i++) {
        const char *nm = lg->fields[i].name;
        if (nm == NULL) { continue; }
        for (k = 0; k < 6; k++) { if (strcmp(nm, fnames[k]) == 0) { f[k] = (int)i; } }
        if (strcmp(nm, "time") == 0) { ft = (int)i; }
        if (strcmp(nm, "toothTime") == 0) { ftt = (int)i; }
    }
    if (rpm < 20.0) { return; }          /* engine stopped: nothing to log (empty buffer) */
    us_per_step = 60000000.0 / (rpm * 360.0) * 5.0;
    while (made < LT_SIMLOG_RECORDS) {
        int lvl[6], changed = 0;
        double dt = us_per_step * (1.0 + ((double)(rnd(l) % 1000) - 500.0) / 100000.0);
        l->t_us += dt;
        l->step = (l->step + 1) % STEPS;
        if (l->step * 5 == 125) { l->synced = 1; }
        levels_at(l->step, l->synced, lvl);
        for (k = 0; k < 6; k++) { if (!l->have_last || lvl[k] != l->last[k]) { changed = 1; } }
        if (!changed) { continue; }
        {
            lt_u8 rec[32];
            size_t len = lg->record_len < sizeof(rec) ? lg->record_len : sizeof(rec);
            memset(rec, 0, sizeof(rec));
            for (k = 0; k < 6; k++) {
                if (f[k] >= 0 && lvl[k]) {
                    put_bits(rec, len, lg->fields[f[k]].start_bit, lg->fields[f[k]].bit_count, 1u);
                }
                l->last[k] = lvl[k];
            }
            l->have_last = 1;
            if (ft >= 0 && lg->fields[ft].scale > 0.0) {
                double raw = fmod(l->t_us / 1000.0 / lg->fields[ft].scale, 4294967296.0);
                put_bits(rec, len, lg->fields[ft].start_bit, lg->fields[ft].bit_count, (lt_u32)raw);
            } else if (ftt >= 0) {
                put_bits(rec, len, lg->fields[ftt].start_bit, lg->fields[ftt].bit_count, (lt_u32)dt);
            }
            lt_buf_append(out, rec, len);
        }
        made++;
    }
}

int lt_simlog_command(lt_simlog *l, const lt_ini *ini, const lt_u8 *cmd, size_t n, double rpm,
                      lt_buf *out)
{
    int which = -1;
    const lt_ini_logger *lg = find_logger(ini, cmd, n, &which);
    if (lg == NULL) { return 0; }
    if (!is_trigger_logger(lg)) { return 1; }       /* other loggers: accepted, no data */
    if (which == 0) { l->on = 1; l->have_last = 0; l->step = 0; l->synced = 0; }
    else if (which == 1) { l->on = 0; }
    else if (l->on) { make_records(l, lg, rpm, out); }
    return 1;
}

void lt_sim_console(const char *text, double rpm, const char *sig, lt_buf *out)
{
    char b[96];
    while (*text == ' ') { text++; }
    if (strcmp(text, "help") == 0) {
        lt_buf_puts(out, "msg`Commands: help, hello, version, status, rpm, tooth`");
    } else if (strcmp(text, "hello") == 0) {
        lt_buf_puts(out, "msg`Hello from the demo ECU`");
    } else if (strcmp(text, "version") == 0) {
        lt_snprintf(b, sizeof(b), "msg`%s`", sig);
        lt_buf_puts(out, b);
    } else if (strcmp(text, "rpm") == 0) {
        lt_snprintf(b, sizeof(b), "msg`rpm=%d`", (int)rpm);
        lt_buf_puts(out, b);
    } else if (strcmp(text, "status") == 0) {
        lt_snprintf(b, sizeof(b), "msg`Demo ECU: engine %s, rpm=%d`", rpm > 20.0 ? "running" : "stopped",
                    (int)rpm);
        lt_buf_puts(out, b);
    } else if (strcmp(text, "tooth") == 0) {
        lt_buf_puts(out, "msg`trigger: 36-1 crank, cam sync`");
    } else if (text[0] != '\0') {
        lt_snprintf(b, sizeof(b), "msg`Unknown command: %.60s`", text);
        lt_buf_puts(out, b);
    }
}
