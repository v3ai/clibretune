/* sim_cmd.c - the demo ECU's command decoder: the INI's command templates compiled into byte
 * patterns (see sim_cmd.h).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "sim_cmd.h"

#include <string.h>
#include "../proto/proto.h"

#define TK_LIT 0      /* one fixed byte */
#define TK_OFF 1      /* %2o */
#define TK_CNT 2      /* %2c */
#define TK_DATA 3     /* %v */

typedef struct sim_tok {
    lt_u8 type, byte;
} sim_tok;

typedef struct sim_tpl {
    lt_simcmd_kind kind;
    int page;
    sim_tok *tok;
    size_t n;
} sim_tpl;

struct lt_simcmds {
    sim_tpl *t;
    size_t n, cap;
    lt_bool le;
};

lt_u32 lt_simcmds_page_id(const lt_ini *ini, size_t idx)
{
    const lt_ini_protocol *pr = &ini->proto;

    if (idx < pr->n_page_identifiers && pr->page_identifiers) {
        const lt_ini_blob *b = &pr->page_identifiers[idx];
        if (b->n >= 2) {
            return ini->endian == LT_ENDIAN_LITTLE ? lt_rd_le((const lt_u8 *)b->p, 2)
                                                   : lt_rd_be((const lt_u8 *)b->p, 2);
        }
        if (b->n == 1) {
            return (lt_u8)b->p[0];
        }
    }
    return (lt_u32)idx;
}

static void put_tok(lt_buf *toks, lt_u8 type, lt_u8 byte)
{
    sim_tok k;

    k.type = type;
    k.byte = byte;
    lt_buf_append(toks, &k, sizeof(k));
}

static void put_lits(lt_buf *toks, const char *s, size_t n)
{
    lt_buf d;
    size_t i;

    if (n == 0) {
        return;
    }
    lt_buf_init(&d);
    lt_cmd_parse_string(s, n, &d);
    for (i = 0; i < d.len; i++) {
        put_tok(toks, TK_LIT, (lt_u8)d.data[i]);
    }
    lt_buf_free(&d);
}

/* compile like lt_cmd_build: literal runs (escapes decoded), %2i / %2o / %2c, %v */
static void add(lt_simcmds *c, lt_simcmd_kind kind, int page, const char *f, size_t n,
                lt_u32 page_id)
{
    lt_buf toks;
    size_t i = 0, lit = 0;
    sim_tpl *t;

    if (f == NULL || n == 0) {
        return;
    }
    lt_buf_init(&toks);
    while (i < n) {
        if (f[i] == '%' && i + 2 < n && f[i + 1] == '2' &&
            (f[i + 2] == 'i' || f[i + 2] == 'o' || f[i + 2] == 'c')) {
            put_lits(&toks, f + lit, i - lit);
            if (f[i + 2] == 'i') {
                lt_u8 b[2];
                if (c->le) {
                    lt_wr_le(b, 2, page_id);
                } else {
                    lt_wr_be(b, 2, page_id);
                }
                put_tok(&toks, TK_LIT, b[0]);
                put_tok(&toks, TK_LIT, b[1]);
            } else {
                put_tok(&toks, f[i + 2] == 'o' ? TK_OFF : TK_CNT, 0);
            }
            i += 3;
            lit = i;
        } else if (f[i] == '%' && i + 1 < n && f[i + 1] == 'v') {
            put_lits(&toks, f + lit, i - lit);
            if (kind == LT_SIMCMD_WRITE) {     /* elsewhere the host has no data to put there */
                put_tok(&toks, TK_DATA, 0);    /* (Speeduino 2017: "p%2i%2o%2c%v") */
            }
            i += 2;
            lit = i;
        } else {
            i++;
        }
    }
    put_lits(&toks, f + lit, n - lit);
    if (toks.len == 0 || ((const sim_tok *)toks.data)[0].type != TK_LIT) {
        lt_buf_free(&toks);           /* nothing to recognise it by */
        return;
    }
    if (c->n == c->cap) {
        size_t nc = c->cap ? c->cap * 2 : 16;
        sim_tpl *nt = (sim_tpl *)lt_calloc(nc, sizeof(sim_tpl));
        if (c->n) {
            memcpy(nt, c->t, c->n * sizeof(sim_tpl));
        }
        lt_free(c->t);
        c->t = nt;
        c->cap = nc;
    }
    t = &c->t[c->n++];
    t->kind = kind;
    t->page = page;
    t->n = toks.len / sizeof(sim_tok);
    t->tok = (sim_tok *)lt_malloc(toks.len);
    memcpy(t->tok, toks.data, toks.len);
    lt_buf_free(&toks);
}

static void add_blob(lt_simcmds *c, lt_simcmd_kind kind, const lt_ini_blob *b)
{
    add(c, kind, -1, b->p, b->n, 0);
}

static void add_list(lt_simcmds *c, const lt_ini *ini, lt_simcmd_kind kind,
                     const lt_ini_cmdlist *l, size_t n_pages)
{
    size_t i;

    for (i = 0; i < l->n && i < n_pages; i++) {
        add(c, kind, (int)i, l->v[i].p, l->v[i].n, lt_simcmds_page_id(ini, i));
    }
}

lt_simcmds *lt_simcmds_new(const lt_ini *ini, size_t n_pages)
{
    lt_simcmds *c = (lt_simcmds *)lt_calloc(1, sizeof(*c));
    const lt_ini_protocol *pr;

    if (ini == NULL) {
        return c;
    }
    pr = &ini->proto;
    c->le = ini->endian == LT_ENDIAN_LITTLE ? LT_TRUE : LT_FALSE;
    /* first match wins: the handshake first, then realtime, then the page commands */
    add_blob(c, LT_SIMCMD_QUERY, &pr->query_command);
    if (ini->version_info != NULL) {
        add(c, LT_SIMCMD_VERSION, -1, ini->version_info, strlen(ini->version_info), 0);
    }
    add_blob(c, LT_SIMCMD_OCH, &pr->och_get_command);
    add_blob(c, LT_SIMCMD_BURST, &pr->burst_get_command);
    add_list(c, ini, LT_SIMCMD_READ, &pr->page_read_commands, n_pages);
    add_list(c, ini, LT_SIMCMD_WRITE, &pr->page_chunk_write_commands, n_pages);
    add_list(c, ini, LT_SIMCMD_BURN, &pr->burn_commands, n_pages);
    add_list(c, ini, LT_SIMCMD_CRC, &pr->crc32_check_commands, n_pages);
    return c;
}

void lt_simcmds_free(lt_simcmds *c)
{
    size_t i;

    if (c == NULL) {
        return;
    }
    for (i = 0; i < c->n; i++) {
        lt_free(c->t[i].tok);
    }
    lt_free(c->t);
    lt_free(c);
}

/* 1 match, 0 no, -1 need more */
static int match_one(const lt_simcmds *c, const sim_tpl *t, const lt_u8 *b, size_t n,
                     lt_bool exact, lt_simcmd_match *m)
{
    size_t pos = 0, k, len;

    memset(m, 0, sizeof(*m));
    m->kind = t->kind;
    m->page = t->page;
    for (k = 0; k < t->n; k++) {
        const sim_tok *tk = &t->tok[k];
        switch (tk->type) {
        case TK_LIT:
            if (pos >= n) {
                return -1;
            }
            if (b[pos] != tk->byte) {
                return 0;
            }
            pos++;
            break;
        case TK_OFF:
        case TK_CNT:
            if (pos + 2 > n) {
                return -1;
            }
            len = (size_t)(c->le ? lt_rd_le(b + pos, 2) : lt_rd_be(b + pos, 2));
            if (tk->type == TK_OFF) {
                m->off = len;
                m->has_off = LT_TRUE;
            } else {
                m->cnt = len;
                m->has_cnt = LT_TRUE;
            }
            pos += 2;
            break;
        default:   /* TK_DATA: count bytes; without a count all of a frame, else one */
            len = m->has_cnt ? m->cnt : (exact ? n - pos : 1);
            if (len > n - pos) {
                return -1;
            }
            m->data = b + pos;
            m->ndata = len;
            pos += len;
            break;
        }
    }
    if (exact && pos != n) {
        return 0;
    }
    m->len = pos;
    return 1;
}

int lt_simcmds_match(const lt_simcmds *c, const lt_u8 *b, size_t n, lt_bool exact,
                     lt_simcmd_match *m)
{
    size_t i;
    int r, need = 0;
    lt_simcmd_match tmp;

    memset(m, 0, sizeof(*m));
    m->page = -1;
    if (c == NULL || n == 0) {
        return 0;
    }
    for (i = 0; i < c->n; i++) {
        r = match_one(c, &c->t[i], b, n, exact, &tmp);
        if (r > 0) {
            *m = tmp;
            return 1;
        }
        if (r < 0) {
            need = 1;
        }
    }
    return need ? -1 : 0;
}
