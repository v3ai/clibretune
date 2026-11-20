/* sim_ecu.c - virtual ECU: page memory + wire protocol responder + io vtable.
 * Spec: docs/spec/08-sim-basemap-tableops.md section 7.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "sim.h"

#include <string.h>
#include "sim_logger.h"

#define SIM_FRAME_TIMEOUT_MS 400.0
#define SIM_MAX_PACKET 65535UL
#define SIM_DEFAULT_SIG "speeduino 202504-dev"
#define SIM_DEFAULT_VER "Speeduino 2025.04-dev"
#define SIM_MAX_AXIS 4096
#define SIM_MAX_TICK_MS 600000.0
#define SIM_MAX_PENDING 140000UL /* > 2 max frames; garbage beyond this is dropped */

typedef struct sim_page {
    lt_u8 *ram, *flash;
    size_t size;
} sim_page;

typedef enum { TF_NEED = 0, TF_ENV, TF_BADCRC, TF_PLAIN } take_kind;

struct lt_ecusim {
    const lt_ini *ini;
    lt_simengine *eng;
    sim_page *pages;
    size_t n_pages;
    lt_u32 *page_ids;          /* NULL when no identifiers declared */
    size_t n_page_ids;
    char *sig, *ver;
    lt_u8 query_cmd;
    lt_bool cmd_le, enveloped;
    lt_env_order hint, order;
    lt_bool order_latched;
    lt_buf cmd, rsp;
    size_t rsp_pos;
    lt_bool have_partial;
    double partial_since;
    lt_buf och;
    lt_bool auto_tick, first_och_done, have_last_tick, dropped;
    double last_tick;
    unsigned long mem_gen, ve_gen;
    lt_bool ve_gen_valid;
    double (*now_fn)(void *ud);
    void *now_ud;
    lt_simlog trig;            /* trigger logger state */
    lt_buf con_out;            /* pending console reply text */
    double wbo_t0;             /* demo of the miata-ecus wideband flasher (wbo_uart_*), 0 = idle */
    int fault_burn, fault_write;   /* fault injection: commands left before the link drops */
    lt_bool fault_torn;            /* ... and that burn stores only half the page */
};

static double now_ms(const lt_ecusim *s)
{
    return s->now_fn ? s->now_fn(s->now_ud) : lt_plat_now_ms();
}

/* miata-ecus board console commands: wbo_uart_flash starts a ~14 s simulated flash of the
 * on-board wideband MCU, wbo_uart_status reports its progress like the real firmware */
static int wbo_console(lt_ecusim *s, const char *text, lt_buf *out)
{
    static const struct { double t; const char *st; } steps[5] = {
        { 600.0, "starting" }, { 2500.0, "erasing" }, { 10500.0, "writing" }, { 13500.0, "verifying" },
        { 0.0, "done: wideband flashed and verified" }
    };
    double t;
    int i;
    char b[120];
    while (*text == ' ') { text++; }
    if (strcmp(text, "wbo_uart_flash") == 0) {
        if (s->wbo_t0 > 0.0 && now_ms(s) - s->wbo_t0 < steps[3].t) {
            lt_buf_puts(out, "msg`WBO UART: already running`");
        } else {
            s->wbo_t0 = now_ms(s);
            lt_buf_puts(out, "msg`WBO UART: 26112 byte image, entering the F103 ROM bootloader`");
        }
        return 1;
    }
    if (strcmp(text, "wbo_uart_status") != 0) { return 0; }
    if (s->wbo_t0 <= 0.0) { lt_buf_puts(out, "msg`WBO UART: idle`"); return 1; }
    t = now_ms(s) - s->wbo_t0;
    for (i = 0; i < 4 && t >= steps[i].t; i++) { }
    lt_snprintf(b, sizeof(b), "msg`WBO UART: %s%s`", steps[i].st, i < 4 ? " (running)" : "");
    lt_buf_puts(out, b);
    return 1;
}

/* ------------------------------------------------------ memory */
static const lt_u8 *mem_page(void *ud, int page, size_t *len)
{
    lt_ecusim *s = (lt_ecusim *)ud;

    if (page < 0 || (size_t)page >= s->n_pages) {
        return NULL;
    }
    if (len) {
        *len = s->pages[page].size;
    }
    return s->pages[page].ram;
}

static int mem_write(void *ud, int page, size_t off, const lt_u8 *data, size_t n)
{
    lt_ecusim *s = (lt_ecusim *)ud;

    if (page < 0 || (size_t)page >= s->n_pages || off > s->pages[page].size ||
        n > s->pages[page].size - off) {
        return LT_ERANGE;
    }
    if (n) {
        memcpy(s->pages[page].ram + off, data, n);
        s->mem_gen++;
    }
    return 0;
}

static lt_pagesrc ram_src(lt_ecusim *s)
{
    lt_pagesrc p;

    p.ud = s;
    p.page = mem_page;
    p.write = mem_write;
    return p;
}

/* ------------------------------------------------------ och block */
static void refresh_ve(lt_ecusim *s)
{
    const lt_ini *ini = s->ini;
    const lt_ini_table *t;
    const lt_ini_const *cx, *cy, *cz;
    size_t nx, ny, nz;
    double *rpm, *load, *z;
    lt_pagesrc src;
    int a, b, c;

    if (s->ve_gen_valid && s->ve_gen == s->mem_gen) {
        return;
    }
    s->ve_gen = s->mem_gen;
    s->ve_gen_valid = LT_TRUE;
    lt_simengine_set_ve(s->eng, NULL, 0, NULL, 0, NULL);
    if (!ini || !ini->ve_analyze.present || !ini->ve_analyze.ve_table) {
        return;
    }
    t = lt_ini_find_table(ini, ini->ve_analyze.ve_table);
    if (!t || !t->x_const || !t->y_const || !t->z_const) {
        return;
    }
    cx = lt_ini_find_const(ini, t->x_const);
    cy = lt_ini_find_const(ini, t->y_const);
    cz = lt_ini_find_const(ini, t->z_const);
    if (!cx || !cy || !cz) {
        return;
    }
    nx = lt_shape_count(&cx->shape);
    ny = lt_shape_count(&cy->shape);
    nz = lt_shape_count(&cz->shape);
    if (nx < 2 || ny < 2 || nx > SIM_MAX_AXIS || ny > SIM_MAX_AXIS || nz != nx * ny) {
        return;
    }
    rpm = (double *)lt_malloc(nx * sizeof(double));
    load = (double *)lt_malloc(ny * sizeof(double));
    z = (double *)lt_malloc(nz * sizeof(double));
    src = ram_src(s);
    a = lt_ini_const_get_array(ini, cx, &src, rpm, nx);
    b = lt_ini_const_get_array(ini, cy, &src, load, ny);
    c = lt_ini_const_get_array(ini, cz, &src, z, nz);
    if (a == (int)nx && b == (int)ny && c == (int)nz) {
        lt_simengine_set_ve(s->eng, rpm, nx, load, ny, z);
    }
    lt_free(rpm);
    lt_free(load);
    lt_free(z);
}

static void refresh_block(lt_ecusim *s)
{
    lt_sim_snapshot snap;
    size_t n;

    if (!s->ini) {
        return;
    }
    n = lt_sim_block_size(s->ini);
    if (s->och.len != n) {
        lt_buf_clear(&s->och);
        if (n) {
            lt_buf_reserve(&s->och, n);
            memset(s->och.data, 0, n);
            s->och.len = n;
        }
    }
    if (n == 0) {
        return;
    }
    memset(s->och.data, 0, n);
    lt_simengine_snapshot(s->eng, &snap);
    lt_sim_encode_och(s->ini, &snap, (lt_u8 *)s->och.data, n);
}

static void on_och_request(lt_ecusim *s)
{
    if (!s->first_och_done) {
        s->first_och_done = LT_TRUE;
        lt_simengine_reset_secl(s->eng);
        refresh_block(s);
    }
}

static void do_tick(lt_ecusim *s, unsigned dt)
{
    refresh_ve(s);
    lt_simengine_tick(s->eng, dt);
    refresh_block(s);
}

static void auto_tick(lt_ecusim *s)
{
    double t, dt;

    if (!s->auto_tick) {
        return;
    }
    t = now_ms(s);
    dt = s->have_last_tick ? t - s->last_tick : 0.0;
    if (dt < 0.0) {
        dt = 0.0;
    }
    if (dt > SIM_MAX_TICK_MS) { /* clock jump: avoid out-of-range double->unsigned cast and endless stepping */
        dt = SIM_MAX_TICK_MS;
    }
    do_tick(s, (unsigned)dt);
    s->last_tick = t;
    s->have_last_tick = LT_TRUE;
}

/* ------------------------------------------------------ construction */
lt_ecusim *lt_ecusim_new(const lt_ini *ini)
{
    lt_ecusim *s = (lt_ecusim *)lt_calloc(1, sizeof(*s));
    size_t i;

    s->ini = ini;
    s->eng = lt_simengine_new();
    lt_buf_init(&s->cmd);
    lt_buf_init(&s->rsp);
    lt_buf_init(&s->och);
    lt_buf_init(&s->con_out);
    s->auto_tick = LT_TRUE;
    s->hint = LT_ENV_BIG;
    s->cmd_le = LT_FALSE;
    s->query_cmd = 'Q';
    s->sig = lt_strdup(SIM_DEFAULT_SIG);
    s->ver = lt_strdup(SIM_DEFAULT_VER);
    if (ini) {
        size_t np = ini->proto.n_page_sizes;

        if (ini->signature && ini->signature[0]) {
            lt_free(s->sig);
            s->sig = lt_strdup(ini->signature);
        }
        if (ini->version_info && ini->version_info[0]) {
            lt_free(s->ver);
            s->ver = lt_strdup(ini->version_info);
        }
        if (ini->proto.query_command.n > 0) {
            s->query_cmd = (lt_u8)ini->proto.query_command.p[0];
        }
        s->cmd_le = ini->endian == LT_ENDIAN_LITTLE;
        s->enveloped = ini->proto.uses_modern;
        s->hint = s->cmd_le ? LT_ENV_LITTLE : LT_ENV_BIG;
        s->order = s->hint;
        if (np == 0) {
            np = ini->n_pages;
        }
        s->n_pages = np;
        s->pages = (sim_page *)lt_calloc(np ? np : 1, sizeof(sim_page));
        for (i = 0; i < np; i++) {
            size_t sz = i < ini->proto.n_page_sizes ? (size_t)ini->proto.page_sizes[i] : 0;

            s->pages[i].size = sz;
            s->pages[i].ram = (lt_u8 *)lt_calloc(sz ? sz : 1, 1);
            s->pages[i].flash = (lt_u8 *)lt_calloc(sz ? sz : 1, 1);
        }
        if (ini->proto.n_page_identifiers > 0) {
            s->n_page_ids = ini->proto.n_page_identifiers;
            s->page_ids = (lt_u32 *)lt_calloc(s->n_page_ids, sizeof(lt_u32));
            for (i = 0; i < s->n_page_ids; i++) {
                const lt_ini_blob *b = &ini->proto.page_identifiers[i];

                if (b->n == 0) {
                    s->page_ids[i] = (lt_u32)i;
                } else if (b->n == 1) {
                    s->page_ids[i] = (lt_u8)b->p[0];
                } else {
                    s->page_ids[i] = (lt_u32)((lt_u8)b->p[0]) | ((lt_u32)((lt_u8)b->p[1]) << 8);
                }
            }
        }
        {
            lt_pagesrc src = ram_src(s);
            /* A blank page image disables everything gated on master enable
             * bits (rusEFI/FOME/epicEFI greys out every fuel and ignition
             * menu). Turn the common ones on so the demo behaves like a
             * configured ECU; INIs without them are unaffected. */
            static const char *const enables[] = {
                "isInjectionEnabled", "isIgnitionEnabled"
            };
            lt_bool seeded = LT_FALSE;
            size_t e;

            for (e = 0; e < LT_COUNTOF(enables); e++) {
                if (lt_ini_find_const(ini, enables[e]) != NULL &&
                    lt_ini_set(ini, &src, enables[e], 1.0) == 0) {
                    seeded = LT_TRUE;
                }
            }
            if (lt_sim_seed_ve_table(ini, &src)) {
                seeded = LT_TRUE;
            }
            if (lt_sim_seed_afr_target(ini, &src)) {
                seeded = LT_TRUE;
            }
            if (lt_sim_seed_axes(ini, &src)) {
                seeded = LT_TRUE;
            }
            if (seeded) {
                for (i = 0; i < s->n_pages; i++) {
                    memcpy(s->pages[i].flash, s->pages[i].ram, s->pages[i].size);
                }
            }
        }
    } else {
        s->pages = (sim_page *)lt_calloc(1, sizeof(sim_page));
    }
    refresh_ve(s);
    refresh_block(s);
    return s;
}

void lt_ecusim_free(lt_ecusim *s)
{
    size_t i;

    if (!s) {
        return;
    }
    for (i = 0; i < s->n_pages; i++) {
        lt_free(s->pages[i].ram);
        lt_free(s->pages[i].flash);
    }
    lt_free(s->pages);
    lt_free(s->page_ids);
    lt_free(s->sig);
    lt_free(s->ver);
    lt_buf_free(&s->cmd);
    lt_buf_free(&s->rsp);
    lt_buf_free(&s->och);
    lt_buf_free(&s->con_out);
    lt_simengine_free(s->eng);
    lt_free(s);
}

/* ------------------------------------------------------ responder */
static lt_u32 rd16(const lt_ecusim *s, const lt_u8 *p)
{
    return s->cmd_le ? lt_rd_le(p, 2) : lt_rd_be(p, 2);
}

static int resolve_page(const lt_ecusim *s, lt_u32 id)
{
    size_t i;

    if (s->page_ids) {
        for (i = 0; i < s->n_page_ids; i++) {
            if (s->page_ids[i] == id) {
                return i < s->n_pages ? (int)i : -1;
            }
        }
        return -1;
    }
    if (id > 255 || id >= s->n_pages) {
        return -1;
    }
    return (int)id;
}

/* zero padded read of RAM into out */
static void mem_read(const lt_ecusim *s, int page, size_t off, size_t count, lt_buf *out)
{
    const sim_page *p = &s->pages[page];
    size_t have = 0, i;

    if (off < p->size) {
        have = LT_MIN(count, p->size - off);
    }
    lt_buf_reserve(out, count);
    if (have) {
        lt_buf_append(out, p->ram + off, have);
    }
    for (i = have; i < count; i++) {
        lt_buf_putc(out, 0);
    }
}

static void put_status(lt_buf *out, lt_bool env, lt_u8 code)
{
    if (env) {
        lt_buf_putc(out, code);
    }
}

static void put_window(lt_ecusim *s, lt_buf *out, size_t off, size_t len)
{
    size_t blen = s->och.len;
    size_t start = LT_MIN(off, blen);
    size_t end = LT_MIN(start + len, blen);
    size_t i;

    lt_buf_append(out, s->och.data + start, end - start);
    for (i = end - start; i < len; i++) {
        lt_buf_putc(out, 0);
    }
}

static void respond(lt_ecusim *s, const lt_u8 *cmd, size_t n, lt_bool env, lt_buf *out)
{
    lt_u8 c = cmd[0];
    int pg;
    size_t off, cnt;
    lt_sim_snapshot sn;
    lt_buf lg_data;

    if (c == s->query_cmd) {
        put_status(out, env, LT_RC_OK);
        lt_buf_puts(out, s->sig);
        return;
    }
    switch (c) {
    case 'Q':
        put_status(out, env, LT_RC_OK);
        lt_buf_puts(out, s->sig);
        return;
    case 'S':
        put_status(out, env, LT_RC_OK);
        lt_buf_puts(out, s->ver);
        return;
    case 'A':
        on_och_request(s);
        auto_tick(s);
        put_status(out, env, LT_RC_OK);
        if (s->och.len == 0) {
            lt_simengine_snapshot(s->eng, &sn);
            lt_buf_putc(out, sn.secl & 0xFF);
        } else {
            lt_buf_append(out, s->och.data, s->och.len);
        }
        return;
    case 'p':
    case 'R':
    case 'k':
        pg = n >= 7 ? resolve_page(s, rd16(s, cmd + 1)) : -1;
        if (pg < 0) {
            put_status(out, env, LT_RC_OUT_OF_RANGE);
            return;
        }
        off = rd16(s, cmd + 3);
        cnt = rd16(s, cmd + 5);
        if (c == 'k') {
            lt_buf tmp;
            lt_u32 crc;

            lt_buf_init(&tmp);
            mem_read(s, pg, off, cnt, &tmp);
            crc = lt_proto_crc32(tmp.data, tmp.len);
            lt_buf_free(&tmp);
            put_status(out, env, LT_RC_OK);
            lt_buf_putc(out, (int)((crc >> 24) & 0xFF));
            lt_buf_putc(out, (int)((crc >> 16) & 0xFF));
            lt_buf_putc(out, (int)((crc >> 8) & 0xFF));
            lt_buf_putc(out, (int)(crc & 0xFF));
        } else {
            put_status(out, env, LT_RC_OK);
            mem_read(s, pg, off, cnt, out);
        }
        return;
    case 'M':
    case 'C': {
        int wrote = 0;

        pg = n >= 7 ? resolve_page(s, rd16(s, cmd + 1)) : -1;
        if (pg >= 0) {
            off = rd16(s, cmd + 3);
            cnt = rd16(s, cmd + 5);
            if (n >= 7 + cnt) {
                wrote = mem_write(s, pg, off, cmd + 7, cnt) == 0;
            }
        }
        if (s->fault_write > 0 && --s->fault_write == 0) {
            s->dropped = LT_TRUE;          /* written, but the link is gone before the ack */
        }
        if (env) {
            lt_buf_putc(out, wrote ? LT_RC_OK : LT_RC_OUT_OF_RANGE);
        }
        return;
    }
    case 'b':
    case 'B': {
        int ok = 0;

        pg = n >= 3 ? resolve_page(s, rd16(s, cmd + 1)) : -1;
        if (pg >= 0) {
            size_t len = s->pages[pg].size;
            if (s->fault_burn > 0 && --s->fault_burn == 0) {
                if (s->fault_torn) len /= 2;   /* power/link lost while the flash was written */
                s->dropped = LT_TRUE;
            }
            memcpy(s->pages[pg].flash, s->pages[pg].ram, len);
            ok = 1;
        }
        if (env) {
            lt_buf_putc(out, ok ? LT_RC_OK_BURN : LT_RC_OUT_OF_RANGE);
        }
        return;
    }
    case 'O':
    case 'r':
        put_status(out, env, LT_RC_OK);
        if (c == 'O') {
            if (n < 5) {
                return;
            }
            off = rd16(s, cmd + 1);
            cnt = rd16(s, cmd + 3);
        } else {
            if (n < 7 || cmd[2] != 0x30) {
                return;
            }
            off = rd16(s, cmd + 3);
            cnt = rd16(s, cmd + 5);
        }
        on_och_request(s);
        auto_tick(s);
        put_window(s, out, off, cnt);
        return;
    case 'E':      /* console line (rusEFI): reply text is queued for the 'G' polls */
        if (env && n >= 2) {
            auto_tick(s);
            lt_simengine_snapshot(s->eng, &sn);
            lt_buf_clear(&s->con_out);
            lt_buf_append(&s->con_out, cmd + 1, n - 1);
            lt_buf_putc(&s->con_out, 0);
            {
                lt_buf txt;
                lt_buf_init(&txt);
                if (!wbo_console(s, s->con_out.data, &txt)) {
                    lt_sim_console(s->con_out.data, (double)sn.rpm, s->sig, &txt);
                }
                lt_buf_clear(&s->con_out);
                lt_buf_append(&s->con_out, txt.data, txt.len);
                lt_buf_free(&txt);
            }
            put_status(out, env, LT_RC_OK);
            return;
        }
        put_status(out, env, LT_RC_UNKNOWN_CMD);
        return;
    case 'G':
        put_status(out, env, LT_RC_OK);
        lt_buf_append(out, s->con_out.data, s->con_out.len);
        lt_buf_clear(&s->con_out);
        return;
    default:
        auto_tick(s);
        lt_simengine_snapshot(s->eng, &sn);
        lt_buf_init(&lg_data);
        if (lt_simlog_command(&s->trig, s->ini, cmd, n, (double)sn.rpm, &lg_data)) {
            put_status(out, env, LT_RC_OK);
            lt_buf_append(out, lg_data.data, lg_data.len);
            lt_buf_free(&lg_data);
            return;
        }
        lt_buf_free(&lg_data);
        put_status(out, env, LT_RC_UNKNOWN_CMD);
        return;
    }
}

/* ------------------------------------------------------ framing */
static lt_bool is_cmd_byte(lt_u8 b)
{
    return b == 'Q' || b == 'S' || b == 'A' || b == 'p' || b == 'M' || b == 'b' || b == 'r' ||
           b == 'R' || b == 'C' || b == 'B' || b == 'k' || b == 'O';
}

/* 0 = need more bytes (or bad), else length */
static size_t plain_command_len(const lt_ecusim *s, const lt_u8 *b, size_t n)
{
    switch (b[0]) {
    case 'p': case 'r': case 'R': case 'k': return 7;
    case 'O': return 5;
    case 'b': case 'B': return 3;
    case 'M': case 'C':
        if (n < 7) {
            return 0;
        }
        return 7 + (size_t)rd16(s, b + 5);
    default: return 1;
    }
}

static lt_u32 env_rd16(lt_env_order o, const lt_u8 *p)
{
    return o == LT_ENV_LITTLE ? lt_rd_le(p, 2) : lt_rd_be(p, 2);
}

/* try to decode a frame at buffer start with `o`; returns total len or 0 */
static size_t try_decode(const lt_u8 *b, size_t n, lt_env_order o)
{
    size_t len, total;
    lt_u32 crc, want;

    if (n < 6) {
        return 0;
    }
    len = env_rd16(o, b);
    if (len == 0 || len > SIM_MAX_PACKET) {
        return 0;
    }
    total = 2 + len + 4;
    if (n < total) {
        return 0;
    }
    crc = lt_proto_crc32(b + 2, len);
    want = o == LT_ENV_LITTLE ? lt_rd_le(b + 2 + len, 4) : lt_rd_be(b + 2 + len, 4);
    return crc == want ? total : 0;
}

static void consume(lt_buf *b, size_t n)
{
    if (n >= b->len) {
        lt_buf_clear(b);
        return;
    }
    memmove(b->data, b->data + n, b->len - n);
    b->len -= n;
    b->data[b->len] = '\0';
}

static void push_frame(lt_ecusim *s, const lt_u8 *payload, size_t n, lt_env_order o)
{
    lt_env_encode(o, payload, n, &s->rsp);
}

static void process(lt_ecusim *s)
{
    if (s->dropped) {
        return;
    }
    while (s->cmd.len > 0) {
        const lt_u8 *b = (const lt_u8 *)s->cmd.data;
        size_t n = s->cmd.len, total = 0;
        take_kind kind = TF_NEED;
        lt_env_order used = s->order;
        lt_buf rsp;

        if (s->enveloped) {
            lt_env_order tries[2];
            int nt, i;

            if (s->order_latched) {
                tries[0] = s->order;
                nt = 1;
            } else {
                tries[0] = s->hint;
                tries[1] = s->hint == LT_ENV_BIG ? LT_ENV_LITTLE : LT_ENV_BIG;
                nt = 2;
            }
            for (i = 0; i < nt && kind == TF_NEED; i++) {
                total = try_decode(b, n, tries[i]);
                if (total) {
                    kind = TF_ENV;
                    used = tries[i];
                }
            }
            if (kind == TF_NEED && s->order_latched && n >= 2) {
                total = 2 + (size_t)env_rd16(s->order, b) + 4;
                if (n >= total) {
                    kind = TF_BADCRC;
                    used = s->order;
                }
            }
        } else if (is_cmd_byte(b[0])) {
            total = plain_command_len(s, b, n);
            if (total && n >= total) {
                kind = TF_PLAIN;
            }
        }
        if (kind == TF_NEED) {
            if (!s->have_partial) {
                s->have_partial = LT_TRUE;
                s->partial_since = now_ms(s);
            }
            return;
        }
        lt_buf_init(&rsp);
        if (kind == TF_ENV) {
            s->order = used;
            s->order_latched = LT_TRUE;
            respond(s, b + 2, total - 6, LT_TRUE, &rsp);
            push_frame(s, (const lt_u8 *)rsp.data, rsp.len, used);
        } else if (kind == TF_BADCRC) {
            lt_u8 code = LT_RC_CRC_MISMATCH;

            push_frame(s, &code, 1, used);
        } else {
            respond(s, b, total, LT_FALSE, &rsp);
            lt_buf_append(&s->rsp, rsp.data, rsp.len);
        }
        lt_buf_free(&rsp);
        consume(&s->cmd, total);
        s->have_partial = LT_FALSE;
    }
}

size_t lt_ecusim_write(lt_ecusim *s, const lt_u8 *data, size_t n)
{
    if (s->dropped) {
        return (size_t)-1;
    }
    if (s->have_partial && now_ms(s) - s->partial_since >= SIM_FRAME_TIMEOUT_MS) {
        lt_buf_clear(&s->cmd);
        s->have_partial = LT_FALSE;
    }
    if (n > 0) {
        s->have_partial = LT_FALSE;
        lt_buf_append(&s->cmd, data, n);
    }
    process(s);
    if (s->cmd.len > SIM_MAX_PENDING) { /* unframeable flood: bound memory */
        lt_buf_clear(&s->cmd);
        s->have_partial = LT_FALSE;
    }
    return n;
}

long lt_ecusim_read(lt_ecusim *s, lt_u8 *buf, size_t max)
{
    size_t avail, n;

    if (s->dropped) {
        return -1;
    }
    avail = s->rsp.len - s->rsp_pos;
    n = LT_MIN(avail, max);
    if (n) {
        memcpy(buf, s->rsp.data + s->rsp_pos, n);
        s->rsp_pos += n;
    }
    if (s->rsp_pos >= s->rsp.len) {
        lt_buf_clear(&s->rsp);
        s->rsp_pos = 0;
    }
    return (long)n;
}

size_t lt_ecusim_bytes_to_read(const lt_ecusim *s)
{
    return s->rsp.len - s->rsp_pos;
}

void lt_ecusim_clear_input(lt_ecusim *s)
{
    lt_buf_clear(&s->rsp);
    s->rsp_pos = 0;
}

void lt_ecusim_clear_output(lt_ecusim *s)
{
    lt_buf_clear(&s->cmd);
    s->have_partial = LT_FALSE;
}

void lt_ecusim_set_clock(lt_ecusim *s, double (*now)(void *ud), void *ud)
{
    s->now_fn = now;
    s->now_ud = ud;
}

void lt_ecusim_tick_engine(lt_ecusim *s, unsigned dt_ms)
{
    s->auto_tick = LT_FALSE;
    do_tick(s, dt_ms);
}

void lt_ecusim_set_mode(lt_ecusim *s, lt_sim_mode m)
{
    lt_simengine_set_mode(s->eng, m);
    refresh_block(s);
}

void lt_ecusim_set_link_dropped(lt_ecusim *s, lt_bool dropped)
{
    s->dropped = dropped ? LT_TRUE : LT_FALSE;
}

void lt_ecusim_fault_burn(lt_ecusim *s, int nth, lt_bool torn)
{
    s->fault_burn = nth > 0 ? nth : 0;
    s->fault_torn = torn ? LT_TRUE : LT_FALSE;
}

void lt_ecusim_fault_write(lt_ecusim *s, int nth)
{
    s->fault_write = nth > 0 ? nth : 0;
}

void lt_ecusim_advance_secl(lt_ecusim *s, unsigned d)
{
    lt_simengine_advance_secl(s->eng, d);
    refresh_block(s);
}

void lt_ecusim_reboot(lt_ecusim *s)
{
    size_t i;

    for (i = 0; i < s->n_pages; i++) {
        memcpy(s->pages[i].ram, s->pages[i].flash, s->pages[i].size);
    }
    s->mem_gen++;
    lt_simengine_reset_secl(s->eng);
    s->first_och_done = LT_FALSE;
    s->have_last_tick = LT_FALSE;
    lt_ecusim_clear_output(s);
    refresh_ve(s);
    refresh_block(s);
}

const lt_u8 *lt_ecusim_ram(const lt_ecusim *s, int page, size_t *len)
{
    if (page < 0 || (size_t)page >= s->n_pages) {
        return NULL;
    }
    if (len) {
        *len = s->pages[page].size;
    }
    return s->pages[page].ram;
}

const lt_u8 *lt_ecusim_flash(const lt_ecusim *s, int page, size_t *len)
{
    if (page < 0 || (size_t)page >= s->n_pages) {
        return NULL;
    }
    if (len) {
        *len = s->pages[page].size;
    }
    return s->pages[page].flash;
}

lt_simengine *lt_ecusim_engine(lt_ecusim *s)
{
    return s->eng;
}

/* ------------------------------------------------------ io vtable */
static int io_read(void *ud, void *buf, int n, int timeout_ms)
{
    lt_ecusim *s = (lt_ecusim *)ud;
    long r;

    LT_UNUSED(timeout_ms);
    if (n <= 0) {
        return s->dropped ? -1 : 0;
    }
    r = lt_ecusim_read(s, (lt_u8 *)buf, (size_t)n);
    return (int)r;
}

static int io_write(void *ud, const void *buf, int n)
{
    lt_ecusim *s = (lt_ecusim *)ud;
    size_t r = lt_ecusim_write(s, (const lt_u8 *)buf, (size_t)n);

    return r == (size_t)-1 ? -1 : n;
}

static void io_clear(void *ud)
{
    lt_ecusim_clear_input((lt_ecusim *)ud);
}

lt_proto_io lt_ecusim_io(lt_ecusim *s)
{
    lt_proto_io io;

    io.ud = s;
    io.read = io_read;
    io.write = io_write;
    io.flush = NULL;
    io.clear_input = io_clear;
    io.close = NULL;
    return io;
}
