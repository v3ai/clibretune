/* search_index.c - the settings search: every field of every dialog, ranked per keystroke.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "search_index.h"

#include <ctype.h>
#include <string.h>

#define SX_MAX_DEPTH 5     /* panels inside panels: as deep as the dialog view draws them */
#define SX_MAX_TOKENS 8
#define SX_TOKEN 64

typedef struct sx_node {           /* a condition on the way to a field: menu entry or panel */
    const char *cond;
    int parent;                    /* -1 none */
} sx_node;

typedef struct sx_tab {            /* a dialog that opens as a tab */
    const char *target;
    const char *label;             /* as given to the tab (menu label, '&' included) */
    char *title;                   /* label without the '&' */
} sx_tab;

typedef struct sx_ent {
    const lt_ini_comp *comp;
    const char *caption, *name;
    int node;                      /* innermost condition node, -1 none */
    int tab_i;
} sx_ent;

struct lt_sidx {
    const lt_ini *ini;
    sx_tab *tabv; size_t n_tabs, cap_tabs;
    sx_node *nodes; size_t n_nodes, cap_nodes;
    sx_ent *ents; size_t n_ents, cap_ents;
    unsigned char *reached;        /* per ini->dialogs[i]: inside some tab_i */
    /* last query */
    char q[128];
    lt_sidx_hit *hits; size_t n_hits, cap_hits, n_match;
    int queried;
};

static void *grow(void *p, size_t *cap, size_t need, size_t sz)
{
    size_t c = *cap;
    if (need <= c) { return p; }
    while (c < need) { c = c ? c * 2 : 64; }
    p = lt_realloc(p, c * sz);
    *cap = c;
    return p;
}

static int add_node(lt_sidx *x, const char *cond, int parent)
{
    if (cond == NULL || cond[0] == '\0') { return parent; }
    x->nodes = (sx_node *)grow(x->nodes, &x->cap_nodes, x->n_nodes + 1, sizeof(sx_node));
    x->nodes[x->n_nodes].cond = cond;
    x->nodes[x->n_nodes].parent = parent;
    return (int)x->n_nodes++;
}

static void mark_reached(lt_sidx *x, const lt_ini_dialog *d)
{
    size_t i;
    for (i = 0; i < x->ini->n_dialogs; i++) {
        if (x->ini->dialogs[i] == d) { x->reached[i] = 1; return; }
    }
}

static int indexed_kind(lt_comp_kind k)
{
    return k == LT_COMP_FIELD || k == LT_COMP_DISPLAYONLY || k == LT_COMP_CHECKBOX ||
           k == LT_COMP_SLIDER || k == LT_COMP_SETTINGSEL;
}

static void add_field(lt_sidx *x, const lt_ini_comp *cm, int node, int tab_i, size_t first)
{
    const char *cap = cm->label, *name = cm->name;
    size_t i;
    sx_ent *e;
    if (cm->kind != LT_COMP_SETTINGSEL && (name == NULL || name[0] == '\0')) { return; }
    if (cap == NULL || cap[0] == '\0' || cap[0] == '{' || cap[0] == '#' || cap[0] == '!') { cap = name; }
    if (cap == NULL || cap[0] == '\0') { return; }
    for (i = first; i < x->n_ents; i++) {          /* once per dialog tab */
        const sx_ent *o = &x->ents[i];
        if (name != NULL ? (o->name != NULL && strcmp(o->name, name) == 0)
                         : (o->name == NULL && strcmp(o->caption, cap) == 0)) { return; }
    }
    x->ents = (sx_ent *)grow(x->ents, &x->cap_ents, x->n_ents + 1, sizeof(sx_ent));
    e = &x->ents[x->n_ents++];
    e->comp = cm;
    e->caption = cap;
    e->name = name;
    e->node = node;
    e->tab_i = tab_i;
}

static void walk_dialog(lt_sidx *x, const lt_ini_dialog *d, int node, int tab_i, size_t first,
                        const lt_ini_dialog **path, int depth)
{
    size_t i;
    int k;
    mark_reached(x, d);
    path[depth] = d;
    for (i = 0; i < d->n_comps; i++) {
        const lt_ini_comp *cm = &d->comps[i];
        if (indexed_kind(cm->kind)) {
            add_field(x, cm, add_node(x, cm->vis_cond, node), tab_i, first);
        } else if (cm->kind == LT_COMP_PANEL && cm->name != NULL && depth + 1 < SX_MAX_DEPTH) {
            const lt_ini_dialog *sub = lt_ini_find_dialog(x->ini, cm->name);
            int loop = 0;
            if (sub == NULL) { continue; }
            for (k = 0; k <= depth; k++) { if (path[k] == sub) { loop = 1; } }
            if (!loop) { walk_dialog(x, sub, add_node(x, cm->vis_cond, node), tab_i, first, path, depth + 1); }
        }
    }
}

static void clean_amp(char *dst, size_t cap, const char *src)
{
    size_t n = 0;
    while (*src != '\0' && n + 1 < cap) {
        if (*src == '&' && src[1] != '&' && src[1] != '\0') { src++; continue; }
        dst[n++] = *src++;
    }
    dst[n] = '\0';
}

static void add_top(lt_sidx *x, const lt_ini_dialog *d, const char *label, int node)
{
    const lt_ini_dialog *path[SX_MAX_DEPTH];
    char buf[200];
    size_t i;
    sx_tab *t;
    for (i = 0; i < x->n_tabs; i++) {
        if (strcmp(x->tabv[i].target, d->name) == 0) { return; }   /* in two menus: the first */
    }
    if (label == NULL || label[0] == '\0') { label = d->title != NULL && d->title[0] != '\0' ? d->title : d->name; }
    x->tabv = (sx_tab *)grow(x->tabv, &x->cap_tabs, x->n_tabs + 1, sizeof(sx_tab));
    t = &x->tabv[x->n_tabs++];
    t->target = d->name;
    t->label = label;
    clean_amp(buf, sizeof(buf), label);
    t->title = lt_strdup(buf);
    walk_dialog(x, d, node, (int)x->n_tabs - 1, x->n_ents, path, 0);
}

static void walk_menu(lt_sidx *x, lt_ini_menu_item *const *items, size_t n, int node)
{
    size_t i;
    for (i = 0; i < n; i++) {
        const lt_ini_menu_item *it = items[i];
        int nd = add_node(x, it->vis_cond, node);
        if (it->kind == LT_MENU_SUBMENU) {
            walk_menu(x, it->items, it->n_items, nd);
        } else if (it->kind != LT_MENU_SEPARATOR && it->target != NULL) {
            const lt_ini_dialog *d = lt_ini_find_dialog(x->ini, it->target);
            if (d != NULL && d->name != NULL) { add_top(x, d, it->label, nd); }
        }
    }
}

lt_sidx *lt_sidx_build(const lt_ini *ini)
{
    lt_sidx *x = (lt_sidx *)lt_calloc(1, sizeof(lt_sidx));
    size_t i;
    x->ini = ini;
    x->reached = (unsigned char *)lt_calloc(ini->n_dialogs + 1, 1);
    for (i = 0; i < ini->n_menus; i++) {
        walk_menu(x, ini->menus[i].items, ini->menus[i].n_items, -1);
    }
    for (i = 0; i < ini->n_dialogs; i++) {          /* dialogs no menu opens */
        const lt_ini_dialog *d = ini->dialogs[i];
        if (!x->reached[i] && d->name != NULL) { add_top(x, d, NULL, -1); }
    }
    return x;
}

void lt_sidx_free(lt_sidx *x)
{
    size_t i;
    if (x == NULL) { return; }
    for (i = 0; i < x->n_tabs; i++) { lt_free(x->tabv[i].title); }
    lt_free(x->tabv);
    lt_free(x->nodes);
    lt_free(x->ents);
    lt_free(x->reached);
    lt_free(x->hits);
    lt_free(x);
}

size_t lt_sidx_count(const lt_sidx *x) { return x != NULL ? x->n_ents : 0; }

/* ---- scoring ---- */
static int is_an(char c) { return isalnum((unsigned char)c) != 0; }

static int word_start(const char *s, size_t i)
{
    unsigned char c = (unsigned char)s[i], p;
    if (!isalnum(c)) { return 0; }
    if (i == 0) { return 1; }
    p = (unsigned char)s[i - 1];
    if (!isalnum(p)) { return 1; }
    if (islower(p) && isupper(c)) { return 1; }
    if (isalpha(p) && isdigit(c)) { return 1; }
    return 0;
}

static int word_score(const char *s, const char *t, size_t n)
{
    size_t i, h;
    int best = 0;
    if (s == NULL || n == 0) { return 0; }
    h = strlen(s);
    for (i = 0; i + n <= h && best < 3; i++) {
        int sc;
        if (lt_strnicmp(s + i, t, n) != 0) { continue; }
        if (word_start(s, i) || (i == 0 && !is_an(t[0]))) {
            sc = (s[i + n] == '\0' || !is_an(s[i + n]) || word_start(s, i + n)) ? 3 : 2;
        } else {
            sc = 1;
        }
        if (sc > best) { best = sc; }
    }
    return best;
}

static int split(const char *q, char tok[][SX_TOKEN], size_t *len)
{
    int n = 0;
    while (*q != '\0' && n < SX_MAX_TOKENS) {
        size_t k = 0;
        while (*q == ' ' || *q == '\t') { q++; }
        if (*q == '\0') { break; }
        while (*q != '\0' && *q != ' ' && *q != '\t') {
            if (k + 1 < SX_TOKEN) { tok[n][k++] = *q; }
            q++;
        }
        tok[n][k] = '\0';
        len[n] = k;
        n++;
    }
    return n;
}

int lt_sidx_score(const char *text, const char *q)
{
    char tok[SX_MAX_TOKENS][SX_TOKEN];
    size_t len[SX_MAX_TOKENS];
    int n = split(q != NULL ? q : "", tok, len), i, sum = 0;
    for (i = 0; i < n; i++) {
        int s = word_score(text, tok[i], len[i]);
        if (s == 0) { return 0; }
        sum += s;
    }
    return sum;
}

/* a before b in the list */
static int better(const lt_sidx_hit *a, const lt_sidx_hit *b)
{
    size_t la, lb;
    if (a->score != b->score) { return a->score > b->score; }
    if (a->in_caption != b->in_caption) { return a->in_caption > b->in_caption; }
    la = strlen(a->caption); lb = strlen(b->caption);
    if (la != lb) { return la < lb; }
    return a->entry < b->entry;
}

size_t lt_sidx_query(lt_sidx *x, const char *q, size_t max, const lt_sidx_hit **out, size_t *total)
{
    char tok[SX_MAX_TOKENS][SX_TOKEN];
    size_t len[SX_MAX_TOKENS], e;
    int n, i;
    if (q == NULL) { q = ""; }
    if (x->queried && strcmp(x->q, q) == 0 && x->cap_hits >= max) {
        *out = x->hits;
        if (total != NULL) { *total = x->n_match; }
        return x->n_hits < max ? x->n_hits : max;
    }
    lt_strlcpy(x->q, q, sizeof(x->q));
    x->queried = 1;
    x->n_hits = 0;
    x->n_match = 0;
    if (x->cap_hits < max) {
        lt_free(x->hits);
        x->hits = (lt_sidx_hit *)lt_calloc(max + 1, sizeof(lt_sidx_hit));
        x->cap_hits = max;
    }
    *out = x->hits;
    n = split(q, tok, len);
    for (e = 0; e < x->n_ents && n > 0; e++) {
        const sx_ent *en = &x->ents[e];
        lt_sidx_hit h;
        size_t p;
        int sum = 0, incap = 0;
        for (i = 0; i < n; i++) {
            int a = word_score(en->caption, tok[i], len[i]);
            int b = en->name != NULL && en->name != en->caption ? word_score(en->name, tok[i], len[i]) : 0;
            if (a > 0) { incap++; }
            if (b > a) { a = b; }
            if (a == 0) { sum = 0; break; }
            sum += a;
        }
        if (sum == 0) { continue; }
        if (lt_stricmp(en->caption, q) == 0) { sum++; }    /* the whole caption typed */
        x->n_match++;
        h.caption = en->caption;
        h.title = x->tabv[en->tab_i].title;
        h.target = x->tabv[en->tab_i].target;
        h.comp = en->comp;
        h.entry = e;
        h.score = sum;
        h.in_caption = incap;
        if (max == 0) { continue; }
        if (x->n_hits == max && !better(&h, &x->hits[max - 1])) { continue; }
        p = x->n_hits < max ? x->n_hits++ : max - 1;
        while (p > 0 && better(&h, &x->hits[p - 1])) { x->hits[p] = x->hits[p - 1]; p--; }
        x->hits[p] = h;
    }
    if (total != NULL) { *total = x->n_match; }
    return x->n_hits;
}

int lt_sidx_visible(const lt_sidx *x, size_t entry, lt_sidx_cond_fn fn, void *ud)
{
    int nd;
    if (entry >= x->n_ents) { return 0; }
    for (nd = x->ents[entry].node; nd >= 0; nd = x->nodes[nd].parent) {
        if (!fn(ud, x->nodes[nd].cond)) { return 0; }
    }
    return 1;
}
