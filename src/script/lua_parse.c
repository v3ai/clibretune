/* lua_parse.c - lexer and recursive-descent parser: source text -> AST (lua_priv.h lt_lnode).
 * Names are resolved here: locals get a slot of their function, variables captured by an inner
 * function are marked (they live in heap cells at run time), everything else is a global.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include "lua_priv.h"

enum {
    TK_EOF = 256, TK_NAME, TK_NUMBER, TK_STRING,
    TK_AND, TK_BREAK, TK_DO, TK_ELSE, TK_ELSEIF, TK_END, TK_FALSE, TK_FOR, TK_FUNCTION, TK_IF,
    TK_IN, TK_LOCAL, TK_NIL, TK_NOT, TK_OR, TK_REPEAT, TK_RETURN, TK_THEN, TK_TRUE, TK_UNTIL,
    TK_WHILE, TK_EQ, TK_NE, TK_LE, TK_GE, TK_CONCAT, TK_DOTS, TK_IDIV
};

static const char *const kw_names[] = {
    "and", "break", "do", "else", "elseif", "end", "false", "for", "function", "if", "in",
    "local", "nil", "not", "or", "repeat", "return", "then", "true", "until", "while"
};

typedef struct lx_token {
    int type;
    int line;
    double num;
    lt_lstr *str;
    const char *raw;      /* source span for messages */
    size_t rawlen;
} lx_token;

#define MAX_UPS 100

typedef struct fstate {
    struct fstate *parent;
    lt_lproto *proto;
    lt_lvar *act[LL_MAX_VARS];
    int nact;
    int freeslot;
    int maxslot;
    const char *upnames[MAX_UPS];
    lt_lupdesc updesc[MAX_UPS];
    int nups;
    int loops;
} fstate;

typedef struct pstate {
    lt_lua *st;
    const char *p, *end;
    int line;
    lx_token cur, ahead;
    int has_ahead;
    int level;
    fstate *fs;
} pstate;

/* ------------------------------------------------------------------ errors */
static void lx_text(const pstate *ps, const lx_token *t, char *out, size_t cap)
{
    if (t->type == TK_EOF) { lt_strlcpy(out, "<eof>", cap); return; }
    {
        size_t n = t->rawlen < 40 ? t->rawlen : 40;
        size_t i;
        for (i = 0; i < n && i + 1 < cap; i++) {
            unsigned char ch = (unsigned char)t->raw[i];
            out[i] = (ch < 32 || ch > 126) ? '?' : (char)ch;
        }
        out[i < cap ? i : cap - 1] = '\0';
    }
    LT_UNUSED(ps);
}

static void syntax_error(pstate *ps, const char *msg)
{
    char nearbuf[48];
    lx_text(ps, &ps->cur, nearbuf, sizeof(nearbuf));
    if (ps->cur.type == TK_EOF) {
        lt_luai_error_plain(ps->st, "%s:%d: %s near <eof>", ps->st->chunk, ps->cur.line, msg);
    }
    lt_luai_error_plain(ps->st, "%s:%d: %s near '%s'", ps->st->chunk, ps->cur.line, msg, nearbuf);
}

static void lex_error(pstate *ps, const char *msg, const char *near_s, size_t near_n)
{
    char nearbuf[48];
    size_t i, n = near_n < 40 ? near_n : 40;
    size_t avail = (size_t)(ps->end - near_s);
    if (n > avail) { n = avail; }
    for (i = 0; i < n; i++) {
        unsigned char ch = (unsigned char)near_s[i];
        nearbuf[i] = (ch < 32 || ch > 126) ? '?' : (char)ch;
    }
    nearbuf[n] = '\0';
    if (n == 0) { lt_strlcpy(nearbuf, "<eof>", sizeof(nearbuf)); }
    lt_luai_error_plain(ps->st, "%s:%d: %s near '%s'", ps->st->chunk, ps->line, msg, nearbuf);
}

/* ------------------------------------------------------------------ lexer */
static int is_alpha(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
static int is_digit(int c) { return c >= '0' && c <= '9'; }
static int is_hex(int c)
{
    return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static int hexval(int c)
{
    if (is_digit(c)) { return c - '0'; }
    if (c >= 'a' && c <= 'f') { return c - 'a' + 10; }
    return c - 'A' + 10;
}

/* [=*[ at p: returns the level or -1 */
static int long_bracket(const char *p, const char *end)
{
    int lvl = 0;
    if (p >= end || *p != '[') { return -1; }
    p++;
    while (p < end && *p == '=') { lvl++; p++; }
    return (p < end && *p == '[') ? lvl : -1;
}

static void read_long(pstate *ps, int lvl, int is_comment)
{
    const char *start = ps->p;
    int sline = ps->line;
    ps->p += lvl + 2;
    if (ps->p < ps->end && *ps->p == '\r') { ps->p++; if (ps->p < ps->end && *ps->p == '\n') { ps->p++; } ps->line++; }
    else if (ps->p < ps->end && *ps->p == '\n') { ps->p++; if (ps->p < ps->end && *ps->p == '\r') { ps->p++; } ps->line++; }
    lt_buf_clear(&ps->st->scratch);
    for (;;) {
        if (ps->p >= ps->end) {
            ps->line = sline;
            lex_error(ps, is_comment ? "unfinished long comment" : "unfinished long string",
                      start, 12);
        }
        if (*ps->p == ']') {
            const char *q = ps->p + 1;
            int l = 0;
            while (q < ps->end && *q == '=') { l++; q++; }
            if (l == lvl && q < ps->end && *q == ']') {
                ps->p = q + 1;
                return;
            }
            lt_buf_putc(&ps->st->scratch, ']');
            ps->p++;
        } else {
            if (*ps->p == '\n') { ps->line++; }
            if (!is_comment) { lt_buf_putc(&ps->st->scratch, *ps->p); }
            ps->p++;
        }
    }
}

static void read_string(pstate *ps, lx_token *t, int quote)
{
    const char *start = ps->p;
    ps->p++;
    lt_buf_clear(&ps->st->scratch);
    for (;;) {
        int c;
        if (ps->p >= ps->end) { lex_error(ps, "unfinished string", start, 12); }
        c = (unsigned char)*ps->p;
        if (c == quote) { ps->p++; break; }
        if (c == '\n' || c == '\r') { lex_error(ps, "unfinished string", start, (size_t)(ps->p - start)); }
        if (c == '\\') {
            ps->p++;
            if (ps->p >= ps->end) { lex_error(ps, "unfinished string", start, 12); }
            c = (unsigned char)*ps->p;
            switch (c) {
            case 'n': lt_buf_putc(&ps->st->scratch, '\n'); ps->p++; break;
            case 't': lt_buf_putc(&ps->st->scratch, '\t'); ps->p++; break;
            case 'r': lt_buf_putc(&ps->st->scratch, '\r'); ps->p++; break;
            case 'a': lt_buf_putc(&ps->st->scratch, 7); ps->p++; break;
            case 'b': lt_buf_putc(&ps->st->scratch, 8); ps->p++; break;
            case 'f': lt_buf_putc(&ps->st->scratch, 12); ps->p++; break;
            case 'v': lt_buf_putc(&ps->st->scratch, 11); ps->p++; break;
            case '\\': case '"': case '\'': lt_buf_putc(&ps->st->scratch, c); ps->p++; break;
            case '\n': lt_buf_putc(&ps->st->scratch, '\n'); ps->line++; ps->p++; break;
            case 'x': {
                int v = 0, k;
                ps->p++;
                for (k = 0; k < 2; k++) {
                    if (ps->p >= ps->end || !is_hex((unsigned char)*ps->p)) {
                        lex_error(ps, "hexadecimal digit expected", start, (size_t)(ps->p - start));
                    }
                    v = v * 16 + hexval((unsigned char)*ps->p);
                    ps->p++;
                }
                lt_buf_putc(&ps->st->scratch, v);
                break;
            }
            case 'z':
                ps->p++;
                while (ps->p < ps->end && isspace((unsigned char)*ps->p)) {
                    if (*ps->p == '\n') { ps->line++; }
                    ps->p++;
                }
                break;
            default:
                if (is_digit(c)) {
                    int v = 0, k;
                    for (k = 0; k < 3 && ps->p < ps->end && is_digit((unsigned char)*ps->p); k++) {
                        v = v * 10 + (*ps->p - '0');
                        ps->p++;
                    }
                    if (v > 255) { lex_error(ps, "decimal escape too large", start, (size_t)(ps->p - start)); }
                    lt_buf_putc(&ps->st->scratch, v);
                } else {
                    lex_error(ps, "invalid escape sequence", start, (size_t)(ps->p - start) + 1u);
                }
                break;
            }
        } else {
            lt_buf_putc(&ps->st->scratch, c);
            ps->p++;
        }
    }
    t->type = TK_STRING;
    t->str = lt_luai_str_new(ps->st, ps->st->scratch.len > 0 ? ps->st->scratch.data : "", ps->st->scratch.len);
}

static void lex(pstate *ps, lx_token *t)
{
    for (;;) {
        int c;
        const char *start;
        if (ps->p >= ps->end) {
            t->type = TK_EOF; t->line = ps->line; t->raw = ""; t->rawlen = 0;
            return;
        }
        c = (unsigned char)*ps->p;
        if (c == '\n') { ps->line++; ps->p++; continue; }
        if (c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v') { ps->p++; continue; }
        if (c == '-' && ps->p + 1 < ps->end && ps->p[1] == '-') {
            int lvl;
            ps->p += 2;
            lvl = long_bracket(ps->p, ps->end);
            if (lvl >= 0) {
                read_long(ps, lvl, 1);
            } else {
                while (ps->p < ps->end && *ps->p != '\n') { ps->p++; }
            }
            continue;
        }
        start = ps->p;
        t->line = ps->line;
        t->raw = start;
        t->str = NULL;
        t->num = 0.0;
        if (is_alpha(c)) {
            size_t n, k;
            while (ps->p < ps->end && (is_alpha((unsigned char)*ps->p) || is_digit((unsigned char)*ps->p))) { ps->p++; }
            n = (size_t)(ps->p - start);
            t->type = TK_NAME;
            for (k = 0; k < sizeof(kw_names) / sizeof(kw_names[0]); k++) {
                if (strlen(kw_names[k]) == n && memcmp(kw_names[k], start, n) == 0) {
                    t->type = TK_AND + (int)k;
                    break;
                }
            }
            if (t->type == TK_NAME) { t->str = lt_luai_str_new(ps->st, start, n); }
            t->rawlen = n;
            return;
        }
        if (is_digit(c) || (c == '.' && ps->p + 1 < ps->end && is_digit((unsigned char)ps->p[1]))) {
            size_t n;
            int hex = c == '0' && ps->p + 1 < ps->end && (ps->p[1] == 'x' || ps->p[1] == 'X');
            if (hex) { ps->p += 2; }
            for (;;) {
                int d;
                if (ps->p >= ps->end) { break; }
                d = (unsigned char)*ps->p;
                if (is_alpha(d) || is_digit(d) || d == '.') {
                    if (!hex && (d == 'e' || d == 'E') && ps->p + 1 < ps->end &&
                        (ps->p[1] == '+' || ps->p[1] == '-')) { ps->p++; }
                    ps->p++;
                } else {
                    break;
                }
            }
            n = (size_t)(ps->p - start);
            t->rawlen = n;
            t->type = TK_NUMBER;
            if (!lt_lua_parse_number(start, n, &t->num)) {
                lex_error(ps, "malformed number", start, n);
            }
            return;
        }
        if (c == '"' || c == '\'') {
            read_string(ps, t, c);
            t->rawlen = (size_t)(ps->p - start);
            return;
        }
        if (c == '[') {
            int lvl = long_bracket(ps->p, ps->end);
            if (lvl >= 0) {
                read_long(ps, lvl, 0);
                t->type = TK_STRING;
                t->str = lt_luai_str_new(ps->st, ps->st->scratch.len > 0 ? ps->st->scratch.data : "", ps->st->scratch.len);
                t->rawlen = (size_t)(ps->p - start);
                return;
            }
        }
        ps->p++;
        t->type = c;
        t->rawlen = 1;
        {
            int c2 = ps->p < ps->end ? (unsigned char)*ps->p : 0;
            switch (c) {
            case '=': if (c2 == '=') { ps->p++; t->type = TK_EQ; t->rawlen = 2; } break;
            case '~': if (c2 == '=') { ps->p++; t->type = TK_NE; t->rawlen = 2; } break;
            case '<': if (c2 == '=') { ps->p++; t->type = TK_LE; t->rawlen = 2; } break;
            case '>': if (c2 == '=') { ps->p++; t->type = TK_GE; t->rawlen = 2; } break;
            case '/': if (c2 == '/') { ps->p++; t->type = TK_IDIV; t->rawlen = 2; } break;
            case '.':
                if (c2 == '.') {
                    ps->p++;
                    t->type = TK_CONCAT; t->rawlen = 2;
                    if (ps->p < ps->end && *ps->p == '.') { ps->p++; t->type = TK_DOTS; t->rawlen = 3; }
                }
                break;
            default: break;
            }
        }
        switch (t->type) {
        case '+': case '-': case '*': case '/': case '%': case '^': case '#': case '(': case ')':
        case '{': case '}': case '[': case ']': case ';': case ':': case ',': case '.': case '=':
        case '<': case '>': case TK_EQ: case TK_NE: case TK_LE: case TK_GE: case TK_CONCAT:
        case TK_DOTS: case TK_IDIV:
            return;
        default:
            lex_error(ps, "unexpected symbol", start, 1);
        }
    }
}

static void next_tok(pstate *ps)
{
    if (ps->has_ahead) {
        ps->cur = ps->ahead;
        ps->has_ahead = 0;
    } else {
        lex(ps, &ps->cur);
    }
}

static int peek_type(pstate *ps)
{
    if (!ps->has_ahead) {
        lex(ps, &ps->ahead);
        ps->has_ahead = 1;
    }
    return ps->ahead.type;
}

/* ------------------------------------------------------------------ helpers */
static lt_lnode *new_node(pstate *ps, int kind, int line)
{
    lt_lnode *n = (lt_lnode *)lt_luai_arena(ps->st, sizeof(lt_lnode));
    n->kind = kind;
    n->line = line;
    return n;
}

static void enter_level(pstate *ps)
{
    if (++ps->level > LL_MAX_LEVELS) { syntax_error(ps, "chunk has too many syntax levels"); }
}

static void leave_level(pstate *ps) { ps->level--; }

static int lx_is(pstate *ps, int t) { return ps->cur.type == t; }

static int lx_accept(pstate *ps, int t)
{
    if (ps->cur.type == t) { next_tok(ps); return 1; }
    return 0;
}

static const char *lx_name(int t, char *buf)
{
    if (t >= TK_AND && t <= TK_WHILE) { return kw_names[t - TK_AND]; }
    switch (t) {
    case TK_EOF: return "<eof>";
    case TK_NAME: return "<name>";
    case TK_STRING: return "<string>";
    case TK_NUMBER: return "<number>";
    case TK_EQ: return "==";
    case TK_NE: return "~=";
    case TK_LE: return "<=";
    case TK_GE: return ">=";
    case TK_CONCAT: return "..";
    case TK_DOTS: return "...";
    case TK_IDIV: return "//";
    default: buf[0] = (char)t; buf[1] = '\0'; return buf;
    }
}

static void expect(pstate *ps, int t)
{
    if (!lx_accept(ps, t)) {
        char b[2], msg[64];
        lt_snprintf(msg, sizeof(msg), "'%s' expected", lx_name(t, b));
        syntax_error(ps, msg);
    }
}

static void expect_match(pstate *ps, int what, int who, int line)
{
    if (!lx_accept(ps, what)) {
        char b1[2], b2[2], msg[96];
        if (line == ps->cur.line) {
            lt_snprintf(msg, sizeof(msg), "'%s' expected", lx_name(what, b1));
        } else {
            lt_snprintf(msg, sizeof(msg), "'%s' expected (to close '%s' at line %d)",
                        lx_name(what, b1), lx_name(who, b2), line);
        }
        syntax_error(ps, msg);
    }
}

static lt_lstr *expect_name(pstate *ps)
{
    lt_lstr *s;
    if (!lx_is(ps, TK_NAME)) { syntax_error(ps, "<name> expected"); }
    s = ps->cur.str;
    next_tok(ps);
    return s;
}

/* ------------------------------------------------------------------ scopes */
static lt_lvar *declare_var(pstate *ps, const char *name)
{
    fstate *fs = ps->fs;
    lt_lvar *v;
    if (fs->nact >= LL_MAX_VARS || fs->freeslot >= 250) {
        lt_luai_error_plain(ps->st, "%s:%d: too many local variables", ps->st->chunk, ps->cur.line);
    }
    v = (lt_lvar *)lt_luai_arena(ps->st, sizeof(lt_lvar));
    v->name = name;
    v->slot = fs->freeslot++;
    if (fs->freeslot > fs->maxslot) { fs->maxslot = fs->freeslot; }
    fs->act[fs->nact++] = v;
    return v;
}

static lt_lvar *find_local(const fstate *fs, const char *name)
{
    int i;
    for (i = fs->nact - 1; i >= 0; i--) {
        if (strcmp(fs->act[i]->name, name) == 0) { return fs->act[i]; }
    }
    return NULL;
}

static int add_up(pstate *ps, fstate *fs, const char *name, int from_local, int idx)
{
    if (fs->nups >= MAX_UPS) {
        lt_luai_error_plain(ps->st, "%s:%d: too many upvalues", ps->st->chunk, ps->cur.line);
    }
    fs->upnames[fs->nups] = name;
    fs->updesc[fs->nups].from_local = from_local;
    fs->updesc[fs->nups].idx = idx;
    return fs->nups++;
}

static int find_upvalue(pstate *ps, fstate *fs, const char *name)
{
    int i;
    lt_lvar *v;
    int pi;
    for (i = 0; i < fs->nups; i++) {
        if (strcmp(fs->upnames[i], name) == 0) { return i; }
    }
    if (fs->parent == NULL) { return -1; }
    v = find_local(fs->parent, name);
    if (v != NULL) {
        v->captured = 1;
        return add_up(ps, fs, name, 1, v->slot);
    }
    pi = find_upvalue(ps, fs->parent, name);
    if (pi < 0) { return -1; }
    return add_up(ps, fs, name, 0, pi);
}

static lt_lnode *name_expr(pstate *ps, lt_lstr *name, int line)
{
    lt_lnode *n;
    lt_lvar *v = find_local(ps->fs, name->s);
    int up;
    if (v != NULL) {
        n = new_node(ps, N_LOCAL, line);
        n->var = v;
        n->str = name;
        return n;
    }
    up = find_upvalue(ps, ps->fs, name->s);
    if (up >= 0) {
        n = new_node(ps, N_UPVAL, line);
        n->upidx = up;
        n->str = name;
        return n;
    }
    n = new_node(ps, N_GLOBAL, line);
    n->str = name;
    return n;
}

/* ------------------------------------------------------------------ expressions */
static lt_lnode *expr(pstate *ps);
static lt_lnode *block(pstate *ps);
static lt_lnode *funcbody(pstate *ps, int is_method, int line);

static void list_add(lt_lnode **head, lt_lnode **tail, lt_lnode *n)
{
    if (*tail == NULL) { *head = n; } else { (*tail)->next = n; }
    *tail = n;
}

static lt_lnode *explist(pstate *ps, int *count)
{
    lt_lnode *head = NULL, *tail = NULL;
    int n = 0;
    do {
        list_add(&head, &tail, expr(ps));
        n++;
    } while (lx_accept(ps, ','));
    *count = n;
    return head;
}

static lt_lnode *table_cons(pstate *ps)
{
    int line = ps->cur.line;
    lt_lnode *t = new_node(ps, N_TABLE, line);
    lt_lnode *head = NULL, *tail = NULL;   /* items: kind S_EXPR-like pair nodes */
    int n = 0;
    expect(ps, '{');
    while (!lx_is(ps, '}')) {
        lt_lnode *item = new_node(ps, N_INDEX, ps->cur.line);   /* a = key (NULL: positional), b = value */
        if (lx_is(ps, TK_NAME) && peek_type(ps) == '=') {
            lt_lnode *k = new_node(ps, N_STR, ps->cur.line);
            k->str = ps->cur.str;
            item->a = k;
            next_tok(ps);
            next_tok(ps);
            item->b = expr(ps);
        } else if (lx_is(ps, '[')) {
            next_tok(ps);
            item->a = expr(ps);
            expect(ps, ']');
            expect(ps, '=');
            item->b = expr(ps);
        } else {
            item->b = expr(ps);
        }
        list_add(&head, &tail, item);
        n++;
        if (!lx_accept(ps, ',') && !lx_accept(ps, ';')) { break; }
    }
    expect_match(ps, '}', '{', line);
    t->list = head;
    t->n = n;
    return t;
}

static lt_lnode *call_args(pstate *ps, lt_lnode *fn, lt_lstr *method, int line)
{
    lt_lnode *c = new_node(ps, method != NULL ? N_METHOD : N_CALL, line);
    c->a = fn;
    c->str = method;
    if (lx_is(ps, TK_STRING)) {
        lt_lnode *s = new_node(ps, N_STR, ps->cur.line);
        s->str = ps->cur.str;
        next_tok(ps);
        c->list = s;
        c->n = 1;
    } else if (lx_is(ps, '{')) {
        c->list = table_cons(ps);
        c->n = 1;
    } else if (lx_is(ps, '(')) {
        int open_line = ps->cur.line;
        next_tok(ps);
        if (!lx_is(ps, ')')) { c->list = explist(ps, &c->n); }
        expect_match(ps, ')', '(', open_line);
    } else {
        syntax_error(ps, "function arguments expected");
    }
    return c;
}

static lt_lnode *primary(pstate *ps)
{
    int line = ps->cur.line;
    if (lx_is(ps, TK_NAME)) {
        lt_lstr *nm = ps->cur.str;
        next_tok(ps);
        return name_expr(ps, nm, line);
    }
    if (lx_is(ps, '(')) {
        lt_lnode *e;
        next_tok(ps);
        enter_level(ps);
        e = expr(ps);
        leave_level(ps);
        expect_match(ps, ')', '(', line);
        if (e->kind == N_CALL || e->kind == N_METHOD || e->kind == N_VARARG) {
            lt_lnode *p = new_node(ps, N_PAREN, line);
            p->a = e;
            return p;
        }
        if (e->kind == N_LOCAL || e->kind == N_UPVAL || e->kind == N_GLOBAL || e->kind == N_INDEX) {
            lt_lnode *p = new_node(ps, N_PAREN, line);   /* (a) is not assignable */
            p->a = e;
            return p;
        }
        return e;
    }
    syntax_error(ps, "unexpected symbol");
    return NULL;
}

static lt_lnode *suffixed(pstate *ps)
{
    lt_lnode *e = primary(ps);
    for (;;) {
        int line = ps->cur.line;
        if (lx_is(ps, '.')) {
            lt_lnode *ix = new_node(ps, N_INDEX, line);
            lt_lnode *k;
            next_tok(ps);
            k = new_node(ps, N_STR, line);
            k->str = expect_name(ps);
            ix->a = e;
            ix->b = k;
            e = ix;
        } else if (lx_is(ps, '[')) {
            lt_lnode *ix = new_node(ps, N_INDEX, line);
            next_tok(ps);
            ix->a = e;
            ix->b = expr(ps);
            expect(ps, ']');
            e = ix;
        } else if (lx_is(ps, ':')) {
            lt_lstr *m;
            next_tok(ps);
            m = expect_name(ps);
            e = call_args(ps, e, m, line);
        } else if (lx_is(ps, '(') || lx_is(ps, TK_STRING) || lx_is(ps, '{')) {
            e = call_args(ps, e, NULL, line);
        } else {
            return e;
        }
    }
}

static lt_lnode *simple(pstate *ps)
{
    int line = ps->cur.line;
    lt_lnode *n;
    switch (ps->cur.type) {
    case TK_NUMBER:
        n = new_node(ps, N_NUM, line);
        n->num = ps->cur.num;
        next_tok(ps);
        return n;
    case TK_STRING:
        n = new_node(ps, N_STR, line);
        n->str = ps->cur.str;
        next_tok(ps);
        return n;
    case TK_NIL: next_tok(ps); return new_node(ps, N_NIL, line);
    case TK_TRUE: next_tok(ps); return new_node(ps, N_TRUE, line);
    case TK_FALSE: next_tok(ps); return new_node(ps, N_FALSE, line);
    case TK_DOTS:
        if (!ps->fs->proto->is_vararg) { syntax_error(ps, "cannot use '...' outside a vararg function"); }
        next_tok(ps);
        return new_node(ps, N_VARARG, line);
    case '{': return table_cons(ps);
    case TK_FUNCTION:
        next_tok(ps);
        return funcbody(ps, 0, line);
    default: return suffixed(ps);
    }
}

typedef struct binpri { int left, right, op, kind; } binpri;

static int binop_info(int t, binpri *bp)
{
    bp->kind = N_BIN;
    switch (t) {
    case '+': bp->left = 10; bp->right = 10; bp->op = OP_ADD; return 1;
    case '-': bp->left = 10; bp->right = 10; bp->op = OP_SUB; return 1;
    case '*': bp->left = 11; bp->right = 11; bp->op = OP_MUL; return 1;
    case '/': bp->left = 11; bp->right = 11; bp->op = OP_DIV; return 1;
    case '%': bp->left = 11; bp->right = 11; bp->op = OP_MOD; return 1;
    case TK_IDIV: bp->left = 11; bp->right = 11; bp->op = OP_IDIV; return 1;
    case '^': bp->left = 14; bp->right = 13; bp->op = OP_POW; return 1;
    case TK_CONCAT: bp->left = 9; bp->right = 8; bp->op = OP_CONCAT; return 1;
    case TK_EQ: bp->left = 3; bp->right = 3; bp->op = OP_EQ; return 1;
    case TK_NE: bp->left = 3; bp->right = 3; bp->op = OP_NE; return 1;
    case '<': bp->left = 3; bp->right = 3; bp->op = OP_LT; return 1;
    case TK_LE: bp->left = 3; bp->right = 3; bp->op = OP_LE; return 1;
    case '>': bp->left = 3; bp->right = 3; bp->op = OP_GT; return 1;
    case TK_GE: bp->left = 3; bp->right = 3; bp->op = OP_GE; return 1;
    case TK_AND: bp->left = 2; bp->right = 2; bp->op = 0; bp->kind = N_AND; return 1;
    case TK_OR: bp->left = 1; bp->right = 1; bp->op = 0; bp->kind = N_OR; return 1;
    default: return 0;
    }
}

static lt_lnode *subexpr(pstate *ps, int limit)
{
    lt_lnode *e;
    binpri bp;
    int uop = 0;
    enter_level(ps);
    if (lx_is(ps, TK_NOT)) { uop = OP_NOT; }
    else if (lx_is(ps, '-')) { uop = OP_NEG; }
    else if (lx_is(ps, '#')) { uop = OP_LEN; }
    if (uop != 0) {
        int line = ps->cur.line;
        lt_lnode *u = new_node(ps, N_UN, line);
        next_tok(ps);
        u->op = uop;
        u->a = subexpr(ps, 12);
        if (u->op == OP_NEG && u->a->kind == N_NUM) {   /* fold -number */
            u->a->num = -u->a->num;
            e = u->a;
        } else {
            e = u;
        }
    } else {
        e = simple(ps);
    }
    while (binop_info(ps->cur.type, &bp) && bp.left > limit) {
        int line = ps->cur.line;
        lt_lnode *b = new_node(ps, bp.kind, line);
        next_tok(ps);
        b->op = bp.op;
        b->a = e;
        b->b = subexpr(ps, bp.right);
        e = b;
    }
    leave_level(ps);
    return e;
}

static lt_lnode *expr(pstate *ps) { return subexpr(ps, 0); }

/* ------------------------------------------------------------------ statements */
static int block_follow(const pstate *ps)
{
    switch (ps->cur.type) {
    case TK_ELSE: case TK_ELSEIF: case TK_END: case TK_EOF: case TK_UNTIL: return 1;
    default: return 0;
    }
}

static void open_scope(pstate *ps, int *nact, int *slot)
{
    *nact = ps->fs->nact;
    *slot = ps->fs->freeslot;
}

static void close_scope(pstate *ps, int nact, int slot)
{
    ps->fs->nact = nact;
    ps->fs->freeslot = slot;
}

static lt_lnode *scoped_block(pstate *ps)
{
    int na, sl;
    lt_lnode *b;
    open_scope(ps, &na, &sl);
    b = block(ps);
    close_scope(ps, na, sl);
    return b;
}

static lt_lvar *append_var(lt_lvar **head, lt_lvar **tail, lt_lvar *v)
{
    if (*tail == NULL) { *head = v; } else { (*tail)->next = v; }
    *tail = v;
    return v;
}

static lt_lnode *if_stat(pstate *ps, int line)
{
    lt_lnode *n = new_node(ps, S_IF, line);
    lt_lnode *head = NULL, *tail = NULL;     /* chain of (cond a, body b) nodes */
    int cnt = 0;
    do {
        lt_lnode *arm = new_node(ps, S_IF, ps->cur.line);
        next_tok(ps);                         /* if / elseif */
        arm->a = expr(ps);
        expect(ps, TK_THEN);
        arm->b = scoped_block(ps);
        list_add(&head, &tail, arm);
        cnt++;
    } while (lx_is(ps, TK_ELSEIF));
    n->list = head;
    n->n = cnt;
    if (lx_accept(ps, TK_ELSE)) { n->b = scoped_block(ps); }
    expect_match(ps, TK_END, TK_IF, line);
    return n;
}

static lt_lnode *for_stat(pstate *ps, int line)
{
    lt_lnode *n;
    lt_lstr *name1;
    int na, sl;
    next_tok(ps);
    name1 = expect_name(ps);
    if (lx_is(ps, '=')) {
        lt_lnode *start, *limit, *step = NULL;
        n = new_node(ps, S_FORNUM, line);
        next_tok(ps);
        start = expr(ps);
        expect(ps, ',');
        limit = expr(ps);
        if (lx_accept(ps, ',')) { step = expr(ps); }
        expect(ps, TK_DO);
        open_scope(ps, &na, &sl);
        n->var = declare_var(ps, name1->s);
        n->a = start; n->b = limit; n->c = step;
        ps->fs->loops++;
        n->d = block(ps);
        ps->fs->loops--;
        close_scope(ps, na, sl);
    } else if (lx_is(ps, ',') || lx_is(ps, TK_IN)) {
        lt_lstr *names[16];
        int nn = 1, i;
        lt_lvar *vh = NULL, *vt = NULL;
        n = new_node(ps, S_FORIN, line);
        names[0] = name1;
        while (lx_accept(ps, ',')) {
            lt_lstr *extra = expect_name(ps);
            if (nn >= 16) { syntax_error(ps, "too many loop variables"); }
            else { names[nn++] = extra; }
        }
        expect(ps, TK_IN);
        n->list = explist(ps, &n->n);
        expect(ps, TK_DO);
        open_scope(ps, &na, &sl);
        for (i = 0; i < nn; i++) { append_var(&vh, &vt, declare_var(ps, names[i]->s)); }
        n->vars = vh;
        n->nvars = nn;
        ps->fs->loops++;
        n->d = block(ps);
        ps->fs->loops--;
        close_scope(ps, na, sl);
    } else {
        syntax_error(ps, "'=' or 'in' expected");
        return NULL;
    }
    expect_match(ps, TK_END, TK_FOR, line);
    return n;
}

static lt_lnode *local_stat(pstate *ps, int line)
{
    lt_lstr *names[16];
    int nn = 0, i;
    lt_lnode *n = new_node(ps, S_LOCAL, line);
    lt_lvar *vh = NULL, *vt = NULL;
    do {
        lt_lstr *extra = expect_name(ps);
        if (nn >= 16) { syntax_error(ps, "too many local variables in one statement"); }
        else { names[nn++] = extra; }
    } while (lx_accept(ps, ','));
    if (lx_accept(ps, '=')) { n->list = explist(ps, &n->n); }
    for (i = 0; i < nn; i++) { append_var(&vh, &vt, declare_var(ps, names[i]->s)); }
    n->vars = vh;
    n->nvars = nn;
    return n;
}

static lt_lnode *func_stat(pstate *ps, int line)
{
    lt_lnode *target, *fn, *a;
    lt_lstr *nm;
    int is_method = 0;
    next_tok(ps);
    nm = expect_name(ps);
    target = name_expr(ps, nm, line);
    while (lx_is(ps, '.') || lx_is(ps, ':')) {
        lt_lnode *ix = new_node(ps, N_INDEX, ps->cur.line);
        lt_lnode *k = new_node(ps, N_STR, ps->cur.line);
        is_method = lx_is(ps, ':');
        next_tok(ps);
        k->str = expect_name(ps);
        ix->a = target;
        ix->b = k;
        target = ix;
        if (is_method) { break; }
    }
    fn = funcbody(ps, is_method, line);
    a = new_node(ps, S_ASSIGN, line);
    a->list = target;
    a->n = 1;
    a->list2 = fn;
    a->n2 = 1;
    return a;
}

static lt_lnode *expr_stat(pstate *ps, int line)
{
    lt_lnode *e = suffixed(ps);
    if (lx_is(ps, '=') || lx_is(ps, ',')) {
        lt_lnode *a = new_node(ps, S_ASSIGN, line);
        lt_lnode *head = NULL, *tail = NULL;
        int cnt = 0;
        for (;;) {
            if (e->kind != N_LOCAL && e->kind != N_UPVAL && e->kind != N_GLOBAL && e->kind != N_INDEX) {
                syntax_error(ps, "syntax error");
            }
            list_add(&head, &tail, e);
            cnt++;
            if (!lx_accept(ps, ',')) { break; }
            e = suffixed(ps);
        }
        expect(ps, '=');
        a->list = head;
        a->n = cnt;
        a->list2 = explist(ps, &a->n2);
        return a;
    } else {
        lt_lnode *s;
        if (e->kind != N_CALL && e->kind != N_METHOD) { syntax_error(ps, "syntax error"); }
        s = new_node(ps, S_EXPR, line);
        s->a = e;
        return s;
    }
}

static lt_lnode *statement(pstate *ps)
{
    int line = ps->cur.line;
    lt_lnode *n;
    int na, sl;
    enter_level(ps);
    switch (ps->cur.type) {
    case ';': next_tok(ps); n = new_node(ps, S_BLOCK, line); break;
    case TK_IF: n = if_stat(ps, line); break;
    case TK_WHILE:
        n = new_node(ps, S_WHILE, line);
        next_tok(ps);
        n->a = expr(ps);
        expect(ps, TK_DO);
        ps->fs->loops++;
        n->b = scoped_block(ps);
        ps->fs->loops--;
        expect_match(ps, TK_END, TK_WHILE, line);
        break;
    case TK_DO:
        next_tok(ps);
        n = scoped_block(ps);
        expect_match(ps, TK_END, TK_DO, line);
        break;
    case TK_FOR: n = for_stat(ps, line); break;
    case TK_REPEAT:
        n = new_node(ps, S_REPEAT, line);
        next_tok(ps);
        open_scope(ps, &na, &sl);
        ps->fs->loops++;
        n->b = block(ps);
        ps->fs->loops--;
        expect_match(ps, TK_UNTIL, TK_REPEAT, line);
        n->a = expr(ps);                 /* sees the body's locals */
        close_scope(ps, na, sl);
        break;
    case TK_FUNCTION: n = func_stat(ps, line); break;
    case TK_LOCAL:
        next_tok(ps);
        if (lx_accept(ps, TK_FUNCTION)) {
            lt_lstr *nm = expect_name(ps);
            n = new_node(ps, S_LOCALFUNC, line);
            n->var = declare_var(ps, nm->s);
            n->a = funcbody(ps, 0, line);
        } else {
            n = local_stat(ps, line);
        }
        break;
    case TK_RETURN:
        n = new_node(ps, S_RETURN, line);
        next_tok(ps);
        if (!block_follow(ps) && !lx_is(ps, ';')) { n->list = explist(ps, &n->n); }
        (void)lx_accept(ps, ';');
        break;
    case TK_BREAK:
        if (ps->fs->loops == 0) { syntax_error(ps, "break outside a loop"); }
        next_tok(ps);
        n = new_node(ps, S_BREAK, line);
        break;
    default: n = expr_stat(ps, line); break;
    }
    leave_level(ps);
    return n;
}

static lt_lnode *block(pstate *ps)
{
    lt_lnode *b = new_node(ps, S_BLOCK, ps->cur.line);
    lt_lnode *head = NULL, *tail = NULL;
    int cnt = 0;
    while (!block_follow(ps)) {
        int was_return = lx_is(ps, TK_RETURN);
        list_add(&head, &tail, statement(ps));
        cnt++;
        if (was_return) { break; }
    }
    b->list = head;
    b->n = cnt;
    return b;
}

static lt_lnode *funcbody(pstate *ps, int is_method, int line)
{
    fstate fs;
    lt_lnode *n = new_node(ps, N_FUNC, line);
    lt_lproto *p = (lt_lproto *)lt_luai_arena(ps->st, sizeof(lt_lproto));
    lt_lvar *ph = NULL, *pt = NULL;
    size_t nb;
    memset(&fs, 0, sizeof(fs));
    fs.parent = ps->fs;
    fs.proto = p;
    p->line = line;
    /* cppcheck-suppress autoVariables */
    ps->fs = &fs;
    enter_level(ps);
    if (is_method) {
        append_var(&ph, &pt, declare_var(ps, "self"));
        p->nparams++;
    }
    expect(ps, '(');
    if (!lx_is(ps, ')')) {
        do {
            if (lx_is(ps, TK_DOTS)) {
                next_tok(ps);
                p->is_vararg = 1;
                break;
            }
            append_var(&ph, &pt, declare_var(ps, expect_name(ps)->s));
            p->nparams++;
        } while (lx_accept(ps, ','));
    }
    expect(ps, ')');
    p->params = ph;
    p->body = block(ps);
    expect_match(ps, TK_END, TK_FUNCTION, line);
    leave_level(ps);
    p->nslots = fs.maxslot;
    p->nups = fs.nups;
    nb = (size_t)fs.nups * sizeof(lt_lupdesc);
    if (nb > 0) {
        p->ups = (lt_lupdesc *)lt_luai_arena(ps->st, nb);
        memcpy(p->ups, fs.updesc, nb);
    }
    ps->fs = fs.parent;
    n->proto = p;
    return n;
}

lt_lproto *lt_luai_parse(lt_lua *st, const char *src, size_t len)
{
    pstate ps;
    fstate fs;
    lt_lproto *p = (lt_lproto *)lt_luai_arena(st, sizeof(lt_lproto));
    memset(&ps, 0, sizeof(ps));
    memset(&fs, 0, sizeof(fs));
    ps.st = st;
    ps.p = src;
    ps.end = src + len;
    ps.line = 1;
    lt_buf_clear(&st->scratch);
    p->is_vararg = 1;
    p->line = 0;
    fs.proto = p;
    /* cppcheck-suppress autoVariables */
    ps.fs = &fs;
    st->steps = 0;
    if (len > 0 && src[0] == '#') {      /* shebang line */
        while (ps.p < ps.end && *ps.p != '\n') { ps.p++; }
    }
    next_tok(&ps);
    p->body = block(&ps);
    if (!lx_is(&ps, TK_EOF)) { syntax_error(&ps, "'<eof>' expected"); }
    p->nslots = fs.maxslot;
    p->nups = 0;
    lt_buf_clear(&st->scratch);
    return p;
}
