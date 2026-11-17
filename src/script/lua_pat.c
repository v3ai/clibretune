/* lua_pat.c - Lua pattern matching (find, match, gmatch, gsub): character classes, sets,
 * quantifiers * + - ?, anchors, captures (also position captures), %b and %f. Backtracking is
 * bounded by a recursion limit and by the instruction budget.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <ctype.h>
#include <string.h>
#include "lua_priv.h"

#define L_ESC '%'
#define CAP_UNFINISHED (-1L)
#define CAP_POSITION   (-2L)
#define MAXCCALLS 200

static const char *do_match(lt_lmatch *ms, const char *s, const char *p);

static const char *class_end(lt_lmatch *ms, const char *p)
{
    if (p >= ms->pat_end) { lt_luai_error(ms->st, "malformed pattern"); }
    if (*p++ == L_ESC) {
        if (p >= ms->pat_end) { lt_luai_error(ms->st, "malformed pattern (ends with '%%')"); }
        return p + 1;
    }
    if (p[-1] == '[') {
        if (p < ms->pat_end && *p == '^') { p++; }
        do {
            if (p >= ms->pat_end) { lt_luai_error(ms->st, "malformed pattern (missing ']')"); }
            if (*(p++) == L_ESC && p < ms->pat_end) { p++; }
        } while (p >= ms->pat_end || *p != ']');
        return p + 1;
    }
    return p;
}

static int match_class(int c, int cl)
{
    int res;
    switch (tolower(cl)) {
    case 'a': res = isalpha(c); break;
    case 'c': res = iscntrl(c); break;
    case 'd': res = isdigit(c); break;
    case 'g': res = isgraph(c); break;
    case 'l': res = islower(c); break;
    case 'p': res = ispunct(c); break;
    case 's': res = isspace(c); break;
    case 'u': res = isupper(c); break;
    case 'w': res = isalnum(c); break;
    case 'x': res = isxdigit(c); break;
    default: return cl == c;
    }
    if (isupper(cl)) { res = !res; }
    return res;
}

static int match_bracket(int c, const char *p, const char *ec)
{
    int sig = 1;
    if (*(p + 1) == '^') { sig = 0; p++; }
    while (++p < ec) {
        if (*p == L_ESC) {
            p++;
            if (match_class(c, (unsigned char)*p)) { return sig; }
        } else if (*(p + 1) == '-' && p + 2 < ec) {
            p += 2;
            if ((unsigned char)*(p - 2) <= c && c <= (unsigned char)*p) { return sig; }
        } else if ((unsigned char)*p == c) {
            return sig;
        }
    }
    return !sig;
}

static int single_match(const lt_lmatch *ms, const char *s, const char *p, const char *ep)
{
    int c;
    if (s >= ms->src_end) { return 0; }
    c = (unsigned char)*s;
    switch (*p) {
    case '.': return 1;
    case L_ESC: return match_class(c, (unsigned char)*(p + 1));
    case '[': return match_bracket(c, p, ep - 1);
    default: return (unsigned char)*p == c;
    }
}

static const char *match_balance(lt_lmatch *ms, const char *s, const char *p)
{
    int b, e, cont = 1;
    if (p >= ms->pat_end - 1) { lt_luai_error(ms->st, "malformed pattern (missing arguments to '%%b')"); }
    if (s >= ms->src_end || *s != *p) { return NULL; }
    b = (unsigned char)*p;
    e = (unsigned char)*(p + 1);
    while (++s < ms->src_end) {
        if ((unsigned char)*s == e) {
            if (--cont == 0) { return s + 1; }
        } else if ((unsigned char)*s == b) {
            cont++;
        }
    }
    return NULL;
}

static const char *max_expand(lt_lmatch *ms, const char *s, const char *p, const char *ep)
{
    long i = 0;
    while (single_match(ms, s + i, p, ep)) { i++; }
    while (i >= 0) {
        const char *res = do_match(ms, s + i, ep + 1);
        if (res != NULL) { return res; }
        i--;
    }
    return NULL;
}

static const char *min_expand(lt_lmatch *ms, const char *s, const char *p, const char *ep)
{
    for (;;) {
        const char *res = do_match(ms, s, ep + 1);
        if (res != NULL) { return res; }
        if (single_match(ms, s, p, ep)) { s++; } else { return NULL; }
    }
}

static const char *start_capture(lt_lmatch *ms, const char *s, const char *p, long what)
{
    const char *res;
    int l = ms->level;
    if (l >= LL_MAXCAP) { lt_luai_error(ms->st, "too many captures"); return NULL; }
    ms->cap[l].init = s;
    ms->cap[l].len = what;
    ms->level = l + 1;
    res = do_match(ms, s, p);
    if (res == NULL) { ms->level--; }
    return res;
}

static int capture_to_close(lt_lmatch *ms)
{
    int level = ms->level;
    for (level--; level >= 0; level--) {
        if (ms->cap[level].len == CAP_UNFINISHED) { return level; }
    }
    lt_luai_error(ms->st, "invalid pattern capture");
    return 0;
}

static const char *end_capture(lt_lmatch *ms, const char *s, const char *p)
{
    int l = capture_to_close(ms);
    const char *res;
    ms->cap[l].len = (long)(s - ms->cap[l].init);
    res = do_match(ms, s, p);
    if (res == NULL) { ms->cap[l].len = CAP_UNFINISHED; }
    return res;
}

static const char *match_capture(lt_lmatch *ms, const char *s, int l)
{
    size_t len;
    l -= '1';
    if (l < 0 || l >= ms->level || ms->cap[l].len == CAP_UNFINISHED) {
        lt_luai_error(ms->st, "invalid capture index %%%d", l + 1);
        return NULL;
    }
    len = ms->cap[l].len < 0 ? 0u : (size_t)ms->cap[l].len;
    if ((size_t)(ms->src_end - s) >= len && memcmp(ms->cap[l].init, s, len) == 0) { return s + len; }
    return NULL;
}

static const char *do_match(lt_lmatch *ms, const char *s, const char *p)
{
    const char *res = NULL;
    lt_luai_step(ms->st);
    if (ms->depth++ > MAXCCALLS) { lt_luai_error(ms->st, "pattern too complex"); }
    for (;;) {
        const char *ep;
        int epc;
        int plain = 0;
        if (p == ms->pat_end) { res = s; break; }
        switch (*p) {
        case '(':
            res = p[1] == ')' ? start_capture(ms, s, p + 2, CAP_POSITION)
                              : start_capture(ms, s, p + 1, CAP_UNFINISHED);
            goto done;
        case ')':
            res = end_capture(ms, s, p + 1);
            goto done;
        case '$':
            if (p + 1 == ms->pat_end) {
                res = (s == ms->src_end) ? s : NULL;
                goto done;
            }
            plain = 1;
            break;
        case L_ESC:
            if (p + 1 < ms->pat_end && p[1] == 'b') {
                s = match_balance(ms, s, p + 2);
                if (s != NULL) { p += 4; continue; }
                res = NULL;
                goto done;
            }
            if (p + 1 < ms->pat_end && p[1] == 'f') {
                const char *prev_ep;
                int prev, cur;
                p += 2;
                if (p >= ms->pat_end || *p != '[') {
                    lt_luai_error(ms->st, "missing '[' after '%%f' in pattern");
                }
                prev_ep = class_end(ms, p);
                prev = s == ms->src ? 0 : (unsigned char)*(s - 1);
                cur = s < ms->src_end ? (unsigned char)*s : 0;
                if (!match_bracket(prev, p, prev_ep - 1) && match_bracket(cur, p, prev_ep - 1)) {
                    p = prev_ep;
                    continue;
                }
                res = NULL;
                goto done;
            }
            if (p + 1 < ms->pat_end && isdigit((unsigned char)p[1])) {
                s = match_capture(ms, s, (unsigned char)p[1]);
                if (s != NULL) { p += 2; continue; }
                res = NULL;
                goto done;
            }
            plain = 1;
            break;
        default:
            plain = 1;
            break;
        }
        if (!plain) { continue; }
        ep = class_end(ms, p);
        epc = ep < ms->pat_end ? (unsigned char)*ep : 0;
        if (!single_match(ms, s, p, ep)) {
            if (epc == '*' || epc == '?' || epc == '-') { p = ep + 1; continue; }
            res = NULL;
            goto done;
        }
        if (epc == '?') {
            const char *r2 = do_match(ms, s + 1, ep + 1);
            if (r2 != NULL) { res = r2; goto done; }
            p = ep + 1;
            continue;
        }
        if (epc == '+') { res = max_expand(ms, s + 1, p, ep); goto done; }
        if (epc == '*') { res = max_expand(ms, s, p, ep); goto done; }
        if (epc == '-') { res = min_expand(ms, s, p, ep); goto done; }
        s++;
        p = ep;
    }
done:
    ms->depth--;
    return res;
}

const char *lt_luai_pat_match(lt_lmatch *m, const char *s, const char *p)
{
    m->level = 0;
    m->depth = 0;
    return do_match(m, s, p);
}

lt_lv lt_luai_pat_capture(lt_lmatch *m, int i, const char *s, const char *e)
{
    if (i >= m->level) {
        if (i != 0) { lt_luai_error(m->st, "invalid capture index %%%d", i + 1); }
        return lt_luai_strv(lt_luai_str_new(m->st, s, (size_t)(e - s)));
    }
    if (m->cap[i].len == CAP_UNFINISHED) { lt_luai_error(m->st, "unfinished capture"); }
    if (m->cap[i].len == CAP_POSITION) {
        return lt_luai_num((double)(m->cap[i].init - m->src) + 1.0);
    }
    return lt_luai_strv(lt_luai_str_new(m->st, m->cap[i].init, (size_t)m->cap[i].len));
}

int lt_luai_pat_ncaps(const lt_lmatch *m, const char *s, int whole)
{
    LT_UNUSED(s);
    return (m->level == 0 && whole) ? 1 : m->level;
}
