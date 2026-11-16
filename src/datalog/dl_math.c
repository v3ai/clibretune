/* dl_math.c - user math channels (expressions evaluated through hooks). Spec 06 section 8.
 * The expression engine (src/ini/expr) is reached only through lt_math_hooks so this
 * module has no link dependency on src/ini.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "datalog.h"

static lt_math_hooks g_hooks;

void lt_math_set_hooks(const lt_math_hooks *h)
{
    if (h) {
        g_hooks = *h;
    } else {
        memset(&g_hooks, 0, sizeof g_hooks);
    }
}

static double nan_val(void)
{
    volatile double z = 0.0;
    return z / z;
}

static void ch_clear_ast(lt_math_channel *ch)
{
    if (ch->ast && g_hooks.free_expr) {
        g_hooks.free_expr(ch->ast);
    }
    ch->ast = NULL;
}

void lt_math_set_free(lt_math_set *s)
{
    size_t i;
    for (i = 0; i < s->n; i++) {
        ch_clear_ast(&s->ch[i]);
        lt_free(s->ch[i].name);
        lt_free(s->ch[i].units);
        lt_free(s->ch[i].expression);
    }
    lt_free(s->ch);
    s->ch = NULL;
    s->n = 0;
}

int lt_math_compile(lt_math_channel *ch, char *err, size_t errlen)
{
    char msg[256];
    ch_clear_ast(ch);
    if (err && errlen) {
        err[0] = '\0';
    }
    if (!g_hooks.compile) {
        if (err && errlen) {
            lt_strlcpy(err, "no expression engine installed", errlen);
        }
        return LT_EUNSUPPORTED;
    }
    msg[0] = '\0';
    ch->ast = g_hooks.compile(ch->expression ? ch->expression : "", msg, sizeof msg);
    if (!ch->ast) {
        if (err && errlen) {
            lt_strlcpy(err, msg[0] ? msg : "invalid expression", errlen);
        }
        return LT_EDATA;
    }
    return 0;
}

static void ensure_compiled(lt_math_channel *ch)
{
    if (!ch->ast && g_hooks.compile && ch->expression && ch->expression[0]) {
        char e[8];
        lt_math_compile(ch, e, sizeof e);
    }
}

void lt_math_eval_order(lt_math_set *s, size_t *order)
{
    size_t n = s->n, i, j, placed_n = 0;
    lt_map names;
    char *placed;
    unsigned char *dep;
    int progress;

    if (n <= 1) {
        for (i = 0; i < n; i++) {
            order[i] = i;
        }
        return;
    }
    placed = (char *)lt_calloc(n, 1);
    dep = (unsigned char *)lt_calloc(n * n, 1);   /* dep[i*n + j]: i depends on j */
    lt_map_init(&names, LT_FALSE);
    for (i = 0; i < n; i++) {
        ensure_compiled(&s->ch[i]);
        lt_map_put(&names, s->ch[i].name ? s->ch[i].name : "", (void *)(size_t)(i + 1));
    }
    for (i = 0; i < n; i++) {
        lt_map vars;
        size_t k;
        if (!s->ch[i].ast || !g_hooks.collect_vars) {
            continue;
        }
        lt_map_init(&vars, LT_FALSE);
        g_hooks.collect_vars(s->ch[i].ast, &vars);
        for (k = 0; k < vars.cap; k++) {
            if (vars.slots[k].used == 1) {
                size_t idx = (size_t)lt_map_get(&names, vars.slots[k].key);
                if (idx > 0 && idx - 1 != i) {
                    dep[i * n + (idx - 1)] = 1;
                }
            }
        }
        lt_map_free(&vars);
    }
    do {
        progress = 0;
        for (i = 0; i < n; i++) {
            int ready = 1;
            if (placed[i]) {
                continue;
            }
            for (j = 0; j < n; j++) {
                if (dep[i * n + j] && !placed[j]) {
                    ready = 0;
                    break;
                }
            }
            if (ready) {
                placed[i] = 1;
                order[placed_n++] = i;
                progress = 1;
            }
        }
    } while (progress);
    for (i = 0; i < n; i++) {
        if (!placed[i]) {
            order[placed_n++] = i;
        }
    }
    lt_map_free(&names);
    lt_free(placed);
    lt_free(dep);
}

/* ---- evaluation with chained variables ---- */

typedef struct math_ctx {
    const lt_expr_env *parent;
    lt_math_set *s;
    const size_t *seq;     /* evaluation order of evaluated channels */
    size_t done;           /* number of channels evaluated so far */
    double *out;
} math_ctx;

static lt_bool mx_get_var(void *ud, const char *name, double *out)
{
    math_ctx *c = (math_ctx *)ud;
    size_t k;
    for (k = c->done; k > 0; k--) {
        size_t i = c->seq[k - 1];
        if (c->s->ch[i].name && strcmp(c->s->ch[i].name, name) == 0) {
            *out = c->out[i];
            return LT_TRUE;
        }
    }
    if (c->parent && c->parent->get_var) {
        return c->parent->get_var(c->parent->ud, name, out);
    }
    return LT_FALSE;
}
static const char *mx_get_string(void *ud, const char *n)
{
    const lt_expr_env *p = ((math_ctx *)ud)->parent;
    return p->get_string(p->ud, n);
}
static const char *const *mx_bit_opts(void *ud, const char *n, size_t *cnt)
{
    const lt_expr_env *p = ((math_ctx *)ud)->parent;
    return p->get_bit_options(p->ud, n, cnt);
}
static lt_bool mx_table(void *ud, const char *f, double x, double *o)
{
    const lt_expr_env *p = ((math_ctx *)ud)->parent;
    return p->table_lookup(p->ud, f, x, o);
}
static lt_bool mx_array(void *ud, const char *n, double i, double *o)
{
    const lt_expr_env *p = ((math_ctx *)ud)->parent;
    return p->array_value(p->ud, n, i, o);
}
static lt_bool mx_online(void *ud)
{
    const lt_expr_env *p = ((math_ctx *)ud)->parent;
    return p->is_online(p->ud);
}
static const char *mx_dir(void *ud, int w)
{
    const lt_expr_env *p = ((math_ctx *)ud)->parent;
    return p->get_dir(p->ud, w);
}
static double mx_time(void *ud)
{
    const lt_expr_env *p = ((math_ctx *)ud)->parent;
    return p->time_now(p->ud);
}

static void make_env(lt_expr_env *e, math_ctx *c)
{
    const lt_expr_env *p = c->parent;
    memset(e, 0, sizeof *e);
    e->ud = c;
    e->get_var = mx_get_var;
    if (p) {
        if (p->get_string) e->get_string = mx_get_string;
        if (p->get_bit_options) e->get_bit_options = mx_bit_opts;
        if (p->table_lookup) e->table_lookup = mx_table;
        if (p->array_value) e->array_value = mx_array;
        if (p->is_online) e->is_online = mx_online;
        if (p->get_dir) e->get_dir = mx_dir;
        if (p->time_now) e->time_now = mx_time;
    }
}

static int eval_with_order(lt_math_set *s, const lt_expr_env *env, const size_t *order,
                           double *out)
{
    math_ctx c;
    lt_expr_env e;
    size_t k;
    int bad = 0;
    c.parent = env;
    c.s = s;
    c.seq = order;
    c.done = 0;
    c.out = out;
    make_env(&e, &c);
    for (k = 0; k < s->n; k++) {
        size_t i = order[k];
        double v = 0.0;
        if (!s->ch[i].ast || !g_hooks.eval || g_hooks.eval(s->ch[i].ast, &e, &v) != 0) {
            v = nan_val();
            bad = 1;
        }
        out[i] = v;
        c.done = k + 1;
    }
    return bad ? LT_EDATA : 0;
}

int lt_math_eval_all(lt_math_set *s, const lt_expr_env *env, double *out)
{
    size_t *order;
    int rc;
    if (s->n == 0) {
        return 0;
    }
    order = (size_t *)lt_malloc(s->n * sizeof(size_t));
    lt_math_eval_order(s, order);
    rc = eval_with_order(s, env, order, out);
    lt_free(order);
    if (rc != 0) {
        lt_err_set("a math channel failed to evaluate");
    }
    return rc;
}

/* ---- apply to a log ---- */

typedef struct log_row_ctx {
    const lt_log *log;
    const lt_map *idx;
    const double *row;
} log_row_ctx;

static lt_bool lr_get_var(void *ud, const char *name, double *out)
{
    log_row_ctx *c = (log_row_ctx *)ud;
    size_t i = (size_t)lt_map_get(c->idx, name);
    if (i == 0) {
        return LT_FALSE;
    }
    *out = c->row[i - 1];
    return LT_TRUE;
}

int lt_math_apply_to_log(lt_math_set *s, lt_log *log)
{
    size_t m = s->n, *order, k;
    int c, nc;
    long i;
    lt_map idx;
    log_row_ctx lc;
    lt_expr_env env;
    double *out, *nv;
    char **nn, **nu;

    if (m == 0) {
        return 0;
    }
    if (!g_hooks.eval) {
        lt_err_set("no expression engine installed");
        return LT_EUNSUPPORTED;
    }
    lt_map_init(&idx, LT_FALSE);
    for (c = 0; c < log->n_ch; c++) {
        if (!lt_map_has(&idx, log->names[c])) {
            lt_map_put(&idx, log->names[c], (void *)(size_t)(c + 1));
        }
    }
    for (k = 0; k < m; k++) {
        ensure_compiled(&s->ch[k]);
    }
    order = (size_t *)lt_malloc(m * sizeof(size_t));
    lt_math_eval_order(s, order);
    lc.log = log;
    lc.idx = &idx;
    memset(&env, 0, sizeof env);
    env.ud = &lc;
    env.get_var = lr_get_var;
    nc = log->n_ch + (int)m;
    out = (double *)lt_malloc(m * sizeof(double));
    nv = (double *)lt_malloc((size_t)(log->n > 0 ? log->n : 1) * (size_t)nc * sizeof(double));
    for (i = 0; i < log->n; i++) {
        double *dst = nv + (size_t)i * (size_t)nc;
        lc.row = log->v + (size_t)i * (size_t)log->n_ch;
        eval_with_order(s, &env, order, out);
        for (c = 0; c < log->n_ch; c++) {
            dst[c] = lc.row[c];
        }
        for (k = 0; k < m; k++) {
            dst[log->n_ch + (int)k] = out[k];
        }
    }
    nn = (char **)lt_calloc((size_t)nc, sizeof(char *));
    nu = (char **)lt_calloc((size_t)nc, sizeof(char *));
    for (c = 0; c < log->n_ch; c++) {
        nn[c] = log->names[c];
        nu[c] = log->units[c];
    }
    for (k = 0; k < m; k++) {
        nn[log->n_ch + (int)k] = lt_strdup(s->ch[k].name ? s->ch[k].name : "");
        nu[log->n_ch + (int)k] = lt_strdup(s->ch[k].units ? s->ch[k].units : "");
    }
    lt_free(log->names);
    lt_free(log->units);
    lt_free(log->v);
    log->names = nn;
    log->units = nu;
    log->v = nv;
    log->n_ch = nc;
    log->cap = log->n;
    lt_map_free(&idx);
    lt_free(order);
    lt_free(out);
    return 0;
}

/* ---- JSON persistence (serde_json pretty layout; self-contained mini parser) ---- */

static void put_jstr(lt_buf *b, const char *s)
{
    const unsigned char *p = (const unsigned char *)(s ? s : "");
    lt_buf_putc(b, '"');
    for (; *p; p++) {
        switch (*p) {
        case '"':  lt_buf_puts(b, "\\\""); break;
        case '\\': lt_buf_puts(b, "\\\\"); break;
        case '\n': lt_buf_puts(b, "\\n"); break;
        case '\r': lt_buf_puts(b, "\\r"); break;
        case '\t': lt_buf_puts(b, "\\t"); break;
        case '\b': lt_buf_puts(b, "\\b"); break;
        case '\f': lt_buf_puts(b, "\\f"); break;
        default:
            if (*p < 0x20) {
                lt_buf_printf(b, "\\u%04x", (unsigned)*p);
            } else {
                lt_buf_putc(b, (int)*p);
            }
        }
    }
    lt_buf_putc(b, '"');
}

int lt_math_save(const char *path, const lt_math_set *s)
{
    lt_buf b;
    size_t i;
    int rc;
    lt_buf_init(&b);
    if (s->n == 0) {
        lt_buf_puts(&b, "[]");
    } else {
        lt_buf_puts(&b, "[\n");
        for (i = 0; i < s->n; i++) {
            lt_buf_puts(&b, "  {\n    \"name\": ");
            put_jstr(&b, s->ch[i].name);
            lt_buf_puts(&b, ",\n    \"units\": ");
            put_jstr(&b, s->ch[i].units);
            lt_buf_puts(&b, ",\n    \"expression\": ");
            put_jstr(&b, s->ch[i].expression);
            lt_buf_puts(&b, i + 1 < s->n ? "\n  },\n" : "\n  }\n");
        }
        lt_buf_puts(&b, "]");
    }
    rc = lt_file_write(path, b.data, b.len);
    lt_buf_free(&b);
    if (rc != 0) {
        return LT_EIO;
    }
    return 0;
}

typedef struct jp { const char *p, *end; } jp;

static void j_ws(jp *j)
{
    while (j->p < j->end && (*j->p == ' ' || *j->p == '\t' || *j->p == '\n' || *j->p == '\r')) {
        j->p++;
    }
}

static void put_utf8(lt_buf *b, unsigned long cp)
{
    if (cp < 0x80) {
        lt_buf_putc(b, (int)cp);
    } else if (cp < 0x800) {
        lt_buf_putc(b, (int)(0xC0 | (cp >> 6)));
        lt_buf_putc(b, (int)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000UL) {
        lt_buf_putc(b, (int)(0xE0 | (cp >> 12)));
        lt_buf_putc(b, (int)(0x80 | ((cp >> 6) & 0x3F)));
        lt_buf_putc(b, (int)(0x80 | (cp & 0x3F)));
    } else {
        lt_buf_putc(b, (int)(0xF0 | (cp >> 18)));
        lt_buf_putc(b, (int)(0x80 | ((cp >> 12) & 0x3F)));
        lt_buf_putc(b, (int)(0x80 | ((cp >> 6) & 0x3F)));
        lt_buf_putc(b, (int)(0x80 | (cp & 0x3F)));
    }
}

static int hex4(const char *p, unsigned long *out)
{
    int i;
    unsigned long v = 0;
    for (i = 0; i < 4; i++) {
        int c = (unsigned char)p[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (unsigned long)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (unsigned long)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (unsigned long)(c - 'A' + 10);
        else return 0;
    }
    *out = v;
    return 1;
}

/* parse a string at j->p (which is at the opening quote); malloc'd result or NULL */
static char *j_string(jp *j)
{
    lt_buf b;
    if (j->p >= j->end || *j->p != '"') {
        return NULL;
    }
    j->p++;
    lt_buf_init(&b);
    while (j->p < j->end && *j->p != '"') {
        char c = *j->p++;
        if (c != '\\') {
            lt_buf_putc(&b, (unsigned char)c);
            continue;
        }
        if (j->p >= j->end) {
            goto fail;
        }
        c = *j->p++;
        switch (c) {
        case 'n': lt_buf_putc(&b, '\n'); break;
        case 'r': lt_buf_putc(&b, '\r'); break;
        case 't': lt_buf_putc(&b, '\t'); break;
        case 'b': lt_buf_putc(&b, '\b'); break;
        case 'f': lt_buf_putc(&b, '\f'); break;
        case '"': case '\\': case '/': lt_buf_putc(&b, (unsigned char)c); break;
        case 'u': {
            unsigned long cp, lo;
            if (j->end - j->p < 4 || !hex4(j->p, &cp)) {
                goto fail;
            }
            j->p += 4;
            if (cp >= 0xD800 && cp < 0xDC00 && j->end - j->p >= 6 && j->p[0] == '\\' &&
                j->p[1] == 'u' && hex4(j->p + 2, &lo) && lo >= 0xDC00 && lo < 0xE000) {
                cp = 0x10000UL + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                j->p += 6;
            }
            put_utf8(&b, cp);
            break;
        }
        default: goto fail;
        }
    }
    if (j->p >= j->end) {
        goto fail;
    }
    j->p++;
    if (!b.data) {
        return lt_strdup("");
    }
    return lt_buf_take(&b);
fail:
    lt_buf_free(&b);
    return NULL;
}

/* skip any JSON value; returns 0 on success */
static int j_skip(jp *j, int depth)
{
    j_ws(j);
    if (j->p >= j->end || depth > 64) {
        return -1;
    }
    if (*j->p == '"') {
        char *s = j_string(j);
        if (!s) {
            return -1;
        }
        lt_free(s);
        return 0;
    }
    if (*j->p == '[' || *j->p == '{') {
        char close = *j->p == '[' ? ']' : '}';
        int obj = close == '}';
        j->p++;
        j_ws(j);
        if (j->p < j->end && *j->p == close) {
            j->p++;
            return 0;
        }
        for (;;) {
            j_ws(j);
            if (obj) {
                char *k = j_string(j);
                if (!k) {
                    return -1;
                }
                lt_free(k);
                j_ws(j);
                if (j->p >= j->end || *j->p != ':') {
                    return -1;
                }
                j->p++;
            }
            if (j_skip(j, depth + 1) != 0) {
                return -1;
            }
            j_ws(j);
            if (j->p >= j->end) {
                return -1;
            }
            if (*j->p == ',') {
                j->p++;
                continue;
            }
            if (*j->p == close) {
                j->p++;
                return 0;
            }
            return -1;
        }
    }
    /* number / true / false / null */
    {
        const char *s = j->p;
        while (j->p < j->end && strchr(",]} \t\r\n", *j->p) == NULL) {
            j->p++;
        }
        return j->p > s ? 0 : -1;
    }
}

int lt_math_load(const char *path, lt_math_set *out)
{
    size_t len = 0, cap = 0, i;
    char *buf;
    jp j;
    lt_math_set tmp;

    out->ch = NULL;
    out->n = 0;
    if (!lt_file_exists(path)) {
        return 0;
    }
    buf = lt_file_read(path, &len);
    if (!buf) {
        return LT_EIO;
    }
    memset(&tmp, 0, sizeof tmp);
    j.p = buf;
    j.end = buf + len;
    j_ws(&j);
    if (j.p >= j.end || *j.p != '[') {
        goto bad;
    }
    j.p++;
    j_ws(&j);
    if (j.p < j.end && *j.p == ']') {
        lt_free(buf);
        return 0;
    }
    for (;;) {
        lt_math_channel ch;
        memset(&ch, 0, sizeof ch);
        j_ws(&j);
        if (j.p >= j.end || *j.p != '{') {
            goto bad;
        }
        j.p++;
        j_ws(&j);
        if (j.p < j.end && *j.p == '}') {
            j.p++;
        } else {
            for (;;) {
                char *k, *v = NULL;
                j_ws(&j);
                k = j_string(&j);
                if (!k) {
                    goto badch;
                }
                j_ws(&j);
                if (j.p >= j.end || *j.p != ':') {
                    lt_free(k);
                    goto badch;
                }
                j.p++;
                j_ws(&j);
                if (j.p < j.end && *j.p == '"') {
                    v = j_string(&j);
                    if (!v) {
                        lt_free(k);
                        goto badch;
                    }
                } else if (j_skip(&j, 0) != 0) {
                    lt_free(k);
                    goto badch;
                }
                if (v && strcmp(k, "name") == 0) {
                    lt_free(ch.name); ch.name = v;
                } else if (v && strcmp(k, "units") == 0) {
                    lt_free(ch.units); ch.units = v;
                } else if (v && strcmp(k, "expression") == 0) {
                    lt_free(ch.expression); ch.expression = v;
                } else {
                    lt_free(v);
                }
                lt_free(k);
                j_ws(&j);
                if (j.p < j.end && *j.p == ',') {
                    j.p++;
                    continue;
                }
                if (j.p < j.end && *j.p == '}') {
                    j.p++;
                    break;
                }
                goto badch;
            }
        }
        if (!ch.name) ch.name = lt_strdup("");
        if (!ch.units) ch.units = lt_strdup("");
        if (!ch.expression) ch.expression = lt_strdup("");
        if (tmp.n == cap) {
            cap = cap ? cap * 2 : 8;
            tmp.ch = (lt_math_channel *)lt_realloc(tmp.ch, cap * sizeof(lt_math_channel));
        }
        tmp.ch[tmp.n++] = ch;
        j_ws(&j);
        if (j.p < j.end && *j.p == ',') {
            j.p++;
            continue;
        }
        if (j.p < j.end && *j.p == ']') {
            break;
        }
        goto bad;
badch:
        lt_free(ch.name); lt_free(ch.units); lt_free(ch.expression);
        goto bad;
    }
    lt_free(buf);
    for (i = 0; i < tmp.n; i++) {
        char e[8];
        lt_math_compile(&tmp.ch[i], e, sizeof e);   /* errors ignored: flagged at runtime */
    }
    *out = tmp;
    return 0;
bad:
    lt_free(buf);
    lt_math_set_free(&tmp);
    lt_err_set("math channels file '%s' is not valid JSON", path);
    return LT_EDATA;
}
