/* ini_sect.c - section handlers other than identity/constants/channels.
 * Spec: docs/spec/01-ini-core.md section 7; docs/spec/02-ini-expr-ui.md sections 2-10.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdlib.h>
#include <string.h>
#include "ini_priv.h"

static int is(const char *a, const char *b)
{
    return lt_stricmp(a, b) == 0;
}

static char *dup_or_null(const char *s)
{
    return s ? lt_strdup(s) : NULL;
}

static char *lower_dup(const char *s)
{
    char *r = lt_strdup(s);
    size_t i;
    for (i = 0; r[i]; i++)
        if (r[i] >= 'A' && r[i] <= 'Z') r[i] = (char)(r[i] - 'A' + 'a');
    return r;
}

/* ------------------------------------------------------------------ */
/* gauges                                                              */
/* ------------------------------------------------------------------ */

/* numeric field or {expr}: value + deferred expr */
static void num_or_expr(const char *raw, double fallback, double *val, char **expr)
{
    double d;
    *val = fallback;
    *expr = NULL;
    if (!raw) return;
    if (ini_parse_f64(raw, &d)) *val = d;
    else *expr = ini_deferred_expr(raw);
}

lt_ini_gauge *ini_parse_gauge_line(const char *name, const char *value)
{
    char **p;
    size_t n = ini_split(value, &p);
    lt_ini_gauge *g;
    unsigned long u;
    char *ch;

    if (n == 0 || !p[0][0]) {
        ini_strv_free(p, n);
        return NULL;
    }
    ch = ini_dequote(p[0]);
    g = (lt_ini_gauge *)lt_calloc(1, sizeof(lt_ini_gauge));
    g->name = lt_strdup(name);
    g->channel = ch;
    g->title = n > 1 ? ini_dequote(p[1]) : lt_strdup("");
    g->units = n > 2 ? ini_dequote(p[2]) : lt_strdup("");
    num_or_expr(n > 3 ? p[3] : NULL, 0.0, &g->lo, &g->lo_expr);
    num_or_expr(n > 4 ? p[4] : NULL, 100.0, &g->hi, &g->hi_expr);
    num_or_expr(n > 5 ? p[5] : NULL, 0.0, &g->low_danger, &g->low_danger_expr);
    num_or_expr(n > 6 ? p[6] : NULL, 0.0, &g->low_warning, &g->low_warning_expr);
    num_or_expr(n > 7 ? p[7] : NULL, 100.0, &g->high_warning, &g->high_warning_expr);
    num_or_expr(n > 8 ? p[8] : NULL, 100.0, &g->high_danger, &g->high_danger_expr);
    if (n > 9 && ini_parse_uint(p[9], &u)) g->digits = (int)u;
    ini_strv_free(p, n);
    return g;
}

/* ------------------------------------------------------------------ */
/* menus                                                               */
/* ------------------------------------------------------------------ */

static lt_ini_menu_item *item_new(lt_menu_kind k, char *label, char *target, char *vis,
                                  char *en)
{
    lt_ini_menu_item *m = (lt_ini_menu_item *)lt_calloc(1, sizeof(lt_ini_menu_item));
    m->kind = k;
    m->label = label;
    m->target = target;
    m->vis_cond = vis;
    m->en_cond = en;
    return m;
}

static lt_menu_kind classify_target(const lt_ini *ini, const char *target)
{
    if (strcmp(target, "std_separator") == 0) return LT_MENU_SEPARATOR;
    if (lt_str_starts(target, "std_")) return LT_MENU_STD;
    if (lt_map_has(&ini->help_idx, target)) return LT_MENU_HELP;
    if (lt_map_has(&ini->table_idx, target) || lt_map_has(&ini->curve_idx, target))
        return LT_MENU_TABLE;
    return LT_MENU_DIALOG;
}

static void menu_add(lt_ini *ini, lt_ini_menu_item *it, int group_child)
{
    lt_ini_menu *m;
    if (ini->n_menus == 0) {
        ini_free_menu_item(it);
        return;
    }
    m = &ini->menus[ini->n_menus - 1];
    if (group_child && m->n_items > 0 && m->items[m->n_items - 1]->kind == LT_MENU_SUBMENU) {
        lt_ini_menu_item *parent = m->items[m->n_items - 1];
        parent->items = (lt_ini_menu_item **)lt_realloc(parent->items,
                                (parent->n_items + 1) * sizeof(lt_ini_menu_item *));
        parent->items[parent->n_items++] = it;
        return;
    }
    m->items = (lt_ini_menu_item **)lt_realloc(m->items,
                                               (m->n_items + 1) * sizeof(lt_ini_menu_item *));
    m->items[m->n_items++] = it;
}

static void handle_menu(lt_ini *ini, const char *lk, const char *value)
{
    char **p;
    size_t n;

    if (strcmp(lk, "menu") == 0) {
        lt_ini_menu *m;
        n = ini_split(value, &p);
        m = (lt_ini_menu *)ini_push((void **)&ini->menus, &ini->n_menus, sizeof(lt_ini_menu));
        m->name = ini_dequote(p[0]);
        m->title = lt_strdup(m->name);
        ini_strv_free(p, n);
    } else if (strcmp(lk, "submenu") == 0 || strcmp(lk, "groupchildmenu") == 0) {
        char *target, *label, *vis = NULL, *en = NULL;
        lt_ini_menu_item *it;
        n = ini_split(value, &p);
        if (strchr(p[0], '"')) {
            const char *q1 = strchr(p[0], '"');
            const char *q2 = strchr(q1 + 1, '"');
            target = lt_strndup(p[0], (size_t)(q1 - p[0]));
            lt_str_trim(target);
            label = q2 ? lt_strndup(q1 + 1, (size_t)(q2 - q1 - 1)) : ini_dequote(q1 + 1);
            if (n >= 3) {
                vis = ini_cond(p[1]);
                en = ini_cond(p[2]);
            } else if (n == 2) {
                en = ini_cond(p[1]);
            }
        } else if (n >= 2) {
            target = ini_dequote(p[0]);
            label = ini_dequote(p[1]);
            if (n >= 4) {
                vis = ini_cond(p[2]);
                en = ini_cond(p[3]);
            } else if (n == 3) {
                en = ini_cond(p[2]);
            }
        } else {
            target = ini_dequote(p[0]);
            label = lt_strdup(target);
        }
        ini_strv_free(p, n);
        if (!target[0]) {
            lt_free(target);
            lt_free(label);
            if (vis) lt_free(vis);
            if (en) lt_free(en);
            return;
        }
        it = item_new(classify_target(ini, target), label, target, vis, en);
        menu_add(ini, it, strcmp(lk, "groupchildmenu") == 0);
    } else if (strcmp(lk, "groupmenu") == 0) {
        char *vis = NULL, *en = NULL;
        n = ini_split(value, &p);
        if (n >= 3) {
            vis = ini_cond(p[1]);
            en = ini_cond(p[2]);
        } else if (n == 2) {
            en = ini_cond(p[1]);
        }
        {
            char *label = ini_dequote(p[0]);
            menu_add(ini, item_new(LT_MENU_SUBMENU, label, NULL, vis, en), 0);
        }
        ini_strv_free(p, n);
    } else if (strcmp(lk, "std_separator") == 0) {
        menu_add(ini, item_new(LT_MENU_SEPARATOR, NULL, lt_strdup("std_separator"), NULL, NULL),
                 0);
    }
}

/* ------------------------------------------------------------------ */
/* [UserDefined]                                                       */
/* ------------------------------------------------------------------ */

static char *trimmed(const char *s)
{
    char *r = lt_strdup(s);
    return lt_str_trim(r);
}

static lt_ini_comp *comp_add(lt_ini_dialog *d, lt_comp_kind k)
{
    lt_ini_comp *c = (lt_ini_comp *)ini_push((void **)&d->comps, &d->n_comps, sizeof(lt_ini_comp));
    c->kind = k;
    return c;
}

/* collect braced parts from index `from` (inner trimmed); returns count */
/* Some INIs omit the comma after the quoted caption of a "field" line
 * (field = "Caption"<tab>name).  Returns a copy of value with the comma
 * inserted, or NULL when the line is already well formed. */
static char *fix_field_comma(const char *value)
{
    const char *q, *r;
    lt_buf b;

    if (value[0] != '"') return NULL;
    q = strchr(value + 1, '"');
    if (!q) return NULL;
    r = q + 1;
    while (*r == ' ' || *r == '\t') r++;
    if (*r == '\0' || *r == ',' || *r == '{') return NULL;
    lt_buf_init(&b);
    lt_buf_append(&b, value, (size_t)(q + 1 - value));
    lt_buf_putc(&b, ',');
    lt_buf_puts(&b, r);
    return lt_buf_take(&b);
}

static size_t braces_from(char **p, size_t n, size_t from, char ***out)
{
    size_t i, cnt = 0;
    char **v = NULL;
    for (i = from; i < n; i++)
        if (ini_is_braced(p[i])) ini_str_push(&v, &cnt, ini_unbrace(p[i], 1));
    *out = v;
    return cnt;
}

/* Conditions of a field / checkbox / slider / displayOnlyField: the first is the enable condition
 * (greyed when false), a second one the visibility condition, as TunerStudio reads them. Speeduino
 * relies on it: "Maximum lambda" and "Maximum lambda deviation" edit the same constant with the
 * same {afrProtectEnabled} first and a different mode test second, only one of them shown. */
static void assign_vis_en(lt_ini_comp *c, char **b, size_t nb)
{
    if (nb >= 2) {
        if (b[0][0]) c->en_cond = lt_strdup(b[0]);
        if (b[1][0]) c->vis_cond = lt_strdup(b[1]);
    } else if (nb == 1) {
        if (b[0][0]) c->en_cond = lt_strdup(b[0]);
    }
}

static lt_ini_dialog *dialog_new(const char *name)
{
    lt_ini_dialog *d = (lt_ini_dialog *)lt_calloc(1, sizeof(lt_ini_dialog));
    d->name = lt_strdup(name);
    d->title = lt_strdup(name);
    return d;
}

static void handle_userdefined(lt_ini *ini, ini_pstate *st, const char *lk, const char *value)
{
    char **p;
    size_t n, i;
    lt_ini_dialog *d = st->dialog;

    if (strcmp(lk, "help") == 0) {
        lt_ini_help *h = (lt_ini_help *)lt_calloc(1, sizeof(lt_ini_help));
        n = ini_split(value, &p);
        h->name = lt_strdup(p[0]);
        h->title = n > 1 ? ini_dequote(p[1]) : lt_strdup(p[0]);
        ini_strv_free(p, n);
        st->help = (lt_ini_help *)ini_put((void ***)&ini->helps, &ini->n_helps, &ini->help_idx,
                                          h->name, h, ini_free_help);
        return;
    }
    if (strcmp(lk, "webhelp") == 0) {
        if (st->help) ini_set_str(&st->help->web_url, ini_dequote(value));
        return;
    }
    if (strcmp(lk, "text") == 0) {
        if (st->help) ini_str_push(&st->help->lines, &st->help->n_lines, ini_dequote(value));
        return;
    }
    if (strcmp(lk, "dialog") == 0) {
        lt_ini_dialog *nd;
        n = ini_split(value, &p);
        nd = dialog_new(p[0]);
        if (n > 1) ini_set_str(&nd->title, ini_dequote(p[1]));
        for (i = 2; i < n; i++) {
            if (p[i][0] && p[i][0] != '{') {
                nd->layout_hint = lt_strdup(p[i]);
                break;
            }
        }
        ini_strv_free(p, n);
        st->dialog = (lt_ini_dialog *)ini_put((void ***)&ini->dialogs, &ini->n_dialogs,
                                              &ini->dialog_idx, nd->name, nd,
                                              ini_freev_dialog);
        st->ipanel = NULL;
        st->rpanel = NULL;
        return;
    }
    if (strcmp(lk, "topichelp") == 0) {
        if (d) ini_set_str(&d->help_topic, ini_dequote(value));
        return;
    }
    if (strcmp(lk, "indicatorpanel") == 0) {
        lt_ini_indicator_panel *ip =
            (lt_ini_indicator_panel *)lt_calloc(1, sizeof(lt_ini_indicator_panel));
        unsigned long u;
        n = ini_split(value, &p);
        ip->name = lt_strdup(p[0]);
        ip->columns = (n > 1 && ini_parse_uint(p[1], &u)) ? (int)u : 2;
        for (i = 2; i < n; i++)
            if (ini_is_braced(p[i])) {
                ip->vis_cond = ini_unbrace(p[i], 1);
                break;
            }
        ini_strv_free(p, n);
        ini->ind_panels = (lt_ini_indicator_panel **)lt_realloc(ini->ind_panels,
                                (ini->n_ind_panels + 1) * sizeof(*ini->ind_panels));
        ini->ind_panels[ini->n_ind_panels++] = ip;
        st->ipanel = ip;
        st->rpanel = NULL;
        st->dialog = NULL;
        return;
    }
    if (strcmp(lk, "readoutpanel") == 0) {
        lt_ini_readout_panel *rp =
            (lt_ini_readout_panel *)lt_calloc(1, sizeof(lt_ini_readout_panel));
        unsigned long u;
        n = ini_split(value, &p);
        rp->name = lt_strdup(p[0]);
        rp->columns = 1;
        for (i = 1; i < n; i++) {
            if (p[i][0] != '{' && ini_parse_uint(p[i], &u)) {
                rp->columns = (int)u;
                break;
            }
        }
        for (i = 1; i < n; i++)
            if (ini_is_braced(p[i])) {
                rp->vis_cond = ini_unbrace(p[i], 1);
                break;
            }
        ini_strv_free(p, n);
        ini->readout_panels = (lt_ini_readout_panel **)lt_realloc(ini->readout_panels,
                                (ini->n_readout_panels + 1) * sizeof(*ini->readout_panels));
        ini->readout_panels[ini->n_readout_panels++] = rp;
        st->rpanel = rp;
        st->ipanel = NULL;
        st->dialog = NULL;
        return;
    }
    if (strcmp(lk, "readout") == 0) {
        lt_ini_readout *r;
        unsigned long u;
        if (!st->rpanel) return;
        n = ini_split(value, &p);
        if (n < 2) {
            ini_strv_free(p, n);
            return;
        }
        r = (lt_ini_readout *)ini_push((void **)&st->rpanel->readouts, &st->rpanel->n_readouts,
                                       sizeof(lt_ini_readout));
        r->channel = lt_strdup(p[0]);
        r->title = ini_dequote(p[1]);
        r->units = n > 2 ? ini_dequote(p[2]) : lt_strdup("");
        r->digits = ini_parse_uint(p[n - 2], &u) ? (int)u : 0;
        r->precision = ini_parse_uint(p[n - 1], &u) ? (int)u : 0;
        ini_strv_free(p, n);
        return;
    }
    if (strcmp(lk, "indicator") == 0) {
        n = ini_split(value, &p);
        if (n >= 3) {
            if (st->ipanel) {
                lt_ini_indicator *in = (lt_ini_indicator *)ini_push(
                    (void **)&st->ipanel->inds, &st->ipanel->n_inds, sizeof(lt_ini_indicator));
                in->expression = ini_unbrace(p[0], 1);
                in->label_off = ini_dequote(p[1]);
                in->label_on = ini_dequote(p[2]);
                if (n > 3 && p[3][0]) in->off_bg = lt_strdup(p[3]);   /* TS order: bg, fg */
                if (n > 4 && p[4][0]) in->off_fg = lt_strdup(p[4]);
                if (n > 5 && p[5][0]) in->on_bg = lt_strdup(p[5]);
                if (n > 6 && p[6][0]) in->on_fg = lt_strdup(p[6]);
            } else if (d) {
                lt_ini_comp *c = comp_add(d, LT_COMP_INDICATOR);
                c->expression = ini_unbrace(p[0], 1);
                c->label_off = ini_dequote(p[1]);
                c->label_on = ini_dequote(p[2]);
            }
        }
        ini_strv_free(p, n);
        return;
    }
    if (!d) return;

    if (strcmp(lk, "panel") == 0) {
        lt_ini_comp *c = comp_add(d, LT_COMP_PANEL);
        char **b;
        size_t nb, k;
        n = ini_split(value, &p);
        c->name = lt_strdup(p[0]);
        for (i = 1; i < n; i++)
            if (p[i][0] && p[i][0] != '{') {
                c->position = lt_strdup(p[i]);
                break;
            }
        nb = braces_from(p, n, 1, &b);
        for (k = 0; k < nb; k++) {
            if (!b[k][0] || strcmp(b[k], "0") == 0 || strcmp(b[k], "1") == 0) b[k][0] = '\0';
        }
        if (nb >= 2) {
            if (b[0][0]) c->en_cond = lt_strdup(b[0]);
            if (b[1][0]) c->vis_cond = lt_strdup(b[1]);
        } else if (nb == 1 && b[0][0]) {
            c->en_cond = lt_strdup(b[0]);
        }
        ini_strv_free(b, nb);
        ini_strv_free(p, n);
    } else if (strcmp(lk, "livegraph") == 0) {
        n = ini_split(value, &p);
        if (n >= 2) {
            lt_ini_comp *c = comp_add(d, LT_COMP_LIVEGRAPH);
            c->name = lt_strdup(p[0]);
            c->label = ini_dequote(p[1]);
            c->position = n > 2 && p[2][0] ? lt_strdup(p[2]) : lt_strdup("South");
        }
        ini_strv_free(p, n);
    } else if (strcmp(lk, "graphline") == 0) {
        if (d->n_comps > 0 && d->comps[d->n_comps - 1].kind == LT_COMP_LIVEGRAPH) {
            lt_ini_comp *c = &d->comps[d->n_comps - 1];
            ini_str_push(&c->channels, &c->n_channels, trimmed(value));
        }
    } else if (strcmp(lk, "runtimevalue") == 0) {
        n = ini_split(value, &p);
        if (n >= 2) {
            lt_ini_comp *c = comp_add(d, LT_COMP_RUNTIMEVALUE);
            c->label = ini_dequote(p[0]);
            c->name = lt_strdup(p[1]);
            for (i = 2; i < n; i++)
                if (ini_is_braced(p[i])) {
                    c->vis_cond = ini_unbrace(p[i], 1);
                    break;
                }
        }
        ini_strv_free(p, n);
    } else if (strcmp(lk, "field") == 0) {
        char *fixed = fix_field_comma(value);
        n = ini_split(fixed ? fixed : value, &p);
        lt_free(fixed);
        if (n >= 2) {
            /* no constant (`field = "!Note", {}, {}, { cond }`): a text line with conditions; `{}`
             * or nothing holds the constant's place, a non-empty brace there is the first condition */
            int text = !p[1][0] || ini_is_braced(p[1]);
            size_t from = 2;
            lt_ini_comp *c = comp_add(d, text ? LT_COMP_LABEL : LT_COMP_FIELD);
            char **b;
            size_t nb;
            c->label = ini_dequote(p[0]);
            if (!text) {
                c->name = lt_strdup(p[1]);
            } else {
                char *u = ini_unbrace(p[1], 1);
                if (u[0]) { from = 1; }
                lt_free(u);
            }
            nb = braces_from(p, n, from, &b);
            assign_vis_en(c, b, nb);
            ini_strv_free(b, nb);
        } else if (n == 1 && p[0][0]) {
            lt_ini_comp *c = comp_add(d, LT_COMP_LABEL);
            c->label = ini_dequote(p[0]);
        }
        ini_strv_free(p, n);
    } else if (strcmp(lk, "checkbox") == 0 || strcmp(lk, "slider") == 0) {
        int is_cb = lk[0] == 'c';
        n = ini_split(value, &p);
        if (n >= 2) {
            lt_ini_comp *c = comp_add(d, is_cb ? LT_COMP_CHECKBOX : LT_COMP_SLIDER);
            char **b = NULL;
            size_t nb = 0;
            c->label = ini_dequote(p[0]);
            c->name = lt_strdup(p[1]);
            for (i = 2; i < n; i++) {
                if (ini_is_braced(p[i])) {
                    ini_str_push(&b, &nb, ini_unbrace(p[i], 1));
                } else if (is_cb && is(p[i], "flipValue")) {
                    c->flip = LT_TRUE;
                } else if (!is_cb && (is(p[i], "horizontal") || is(p[i], "vertical"))) {
                    ini_set_str(&c->orientation, lower_dup(p[i]));
                }
            }
            assign_vis_en(c, b, nb);
            ini_strv_free(b, nb);
        }
        ini_strv_free(p, n);
    } else if (strcmp(lk, "displayonlyfield") == 0) {
        n = ini_split(value, &p);
        if (n >= 1) {
            lt_ini_comp *c = comp_add(d, LT_COMP_DISPLAYONLY);
            char **b = NULL;
            size_t nb = 0, from = 1, k;
            lt_buf lb;
            lt_buf_init(&lb);
            for (k = 0; p[0][k]; k++)
                if (p[0][k] != '"') lt_buf_putc(&lb, (unsigned char)p[0][k]);
            c->label = lb.data ? lt_buf_take(&lb) : lt_strdup("");
            if (n > 1 && p[1][0] && !is(p[1], "blankfield") && p[1][0] != '{') {
                c->name = lt_strdup(p[1]);
                from = 2;
            } else if (n > 1 && !p[1][0]) {
                from = 2;
            }
            for (i = from; i < n; i++)
                if (ini_is_braced(p[i])) {
                    char *u = ini_unbrace(p[i], 1);
                    if (u[0]) ini_str_push(&b, &nb, u);
                    else lt_free(u);
                }
            assign_vis_en(c, b, nb);
            ini_strv_free(b, nb);
        }
        ini_strv_free(p, n);
    } else if (strcmp(lk, "table") == 0) {
        lt_ini_comp *c = comp_add(d, LT_COMP_TABLE);
        c->name = trimmed(value);
    } else if (strcmp(lk, "settingselector") == 0) {
        lt_ini_comp *c = comp_add(d, LT_COMP_SETTINGSEL);
        n = ini_split(value, &p);
        c->label = ini_dequote(p[0]);
        for (i = 1; i < n; i++)
            if (p[i][0] == '{') {
                c->vis_cond = ini_unbrace(p[i], 1);
                break;
            }
        ini_strv_free(p, n);
    } else if (strcmp(lk, "settingoption") == 0) {
        if (d->n_comps > 0 && d->comps[d->n_comps - 1].kind == LT_COMP_SETTINGSEL) {
            lt_ini_comp *c = &d->comps[d->n_comps - 1];
            lt_ini_setting_opt *o;
            n = ini_split(value, &p);
            o = (lt_ini_setting_opt *)ini_push((void **)&c->options, &c->n_options,
                                               sizeof(lt_ini_setting_opt));
            o->label = ini_dequote(p[0]);
            for (i = 1; i < n; i++) {
                char *eq = strchr(p[i], '=');
                double v;
                if (eq) {
                    char *nm = lt_strndup(p[i], (size_t)(eq - p[i]));
                    lt_str_trim(nm);
                    if (ini_parse_f64(eq + 1, &v)) {
                        lt_ini_assign *a = (lt_ini_assign *)ini_push((void **)&o->assigns,
                                                &o->n_assigns, sizeof(lt_ini_assign));
                        a->name = nm;
                        a->value = v;
                    } else {
                        lt_free(nm);
                    }
                }
            }
            ini_strv_free(p, n);
        }
    } else if (strcmp(lk, "commandbutton") == 0) {
        n = ini_split(value, &p);
        if (n >= 2) {
            lt_ini_comp *c = comp_add(d, LT_COMP_CMDBUTTON);
            c->label = ini_dequote(p[0]);
            c->name = lt_strdup(p[1]);
            for (i = 2; i < n; i++) {
                if (ini_is_braced(p[i])) {
                    ini_set_str(&c->en_cond, ini_unbrace(p[i], 1));
                } else if (is(p[i], "clickOnClose")) {
                    c->close = LT_CLOSE_ALWAYS;
                } else if (is(p[i], "clickOnCloseIfEnabled")) {
                    c->close = LT_CLOSE_IF_ENABLED;
                } else if (is(p[i], "clickOnCloseIfDisabled")) {
                    c->close = LT_CLOSE_IF_DISABLED;
                }
            }
        }
        ini_strv_free(p, n);
    } else if (strcmp(lk, "gauge") == 0) {
        lt_ini_comp *c = comp_add(d, LT_COMP_GAUGE);
        n = ini_split(value, &p);
        c->name = lt_strdup(p[0]);
        if (n > 1 && p[1][0]) c->position = lt_strdup(p[1]);
        ini_strv_free(p, n);
    }
}

/* ------------------------------------------------------------------ */
/* [FrontPage]                                                         */
/* ------------------------------------------------------------------ */

static size_t split_frontpage(const char *v, char ***parts)
{
    lt_vec out;
    size_t i, st = 0, n = strlen(v);
    int in_q = 0;
    long depth = 0;
    char *last;

    lt_vec_init(&out, sizeof(char *));
    for (i = 0; i < n; i++) {
        char c = v[i];
        if (c == '"' && depth == 0) {
            in_q = !in_q;
        } else if (!in_q && c == '{') {
            depth++;
        } else if (!in_q && c == '}') {
            depth--;
        } else if (!in_q && depth == 0 && c == ',') {
            char *s = lt_strndup(v + st, i - st);
            lt_str_trim(s);
            *(char **)lt_vec_push(&out) = s;
            st = i + 1;
        }
    }
    last = lt_strdup(v + st);
    lt_str_trim(last);
    if (last[0]) *(char **)lt_vec_push(&out) = last;
    else lt_free(last);
    *parts = (char **)out.data;
    return out.len;
}

static void handle_frontpage(lt_ini *ini, const char *lk, const char *value)
{
    lt_ini_frontpage *fp;
    if (!ini->frontpage) ini->frontpage = (lt_ini_frontpage *)lt_calloc(1, sizeof(*fp));
    fp = ini->frontpage;
    if (lt_str_starts(lk, "gauge")) {
        unsigned long u;
        if (ini_parse_uint(lk + 5, &u) && u >= 1 && u <= 64) {
            while (fp->n_gauges < u) ini_str_push(&fp->gauges, &fp->n_gauges, lt_strdup(""));
            ini_set_str(&fp->gauges[u - 1], trimmed(value));
        }
    } else if (strcmp(lk, "indicator") == 0) {
        size_t i, depth = 0, np;
        char *expr, *rest, **p;
        if (value[0] != '{') return;
        for (i = 0; value[i]; i++) {
            if (value[i] == '{') depth++;
            else if (value[i] == '}') {
                depth--;
                if (depth == 0) break;
            }
        }
        if (!value[i]) return;
        expr = lt_strndup(value + 1, i - 1);
        lt_str_trim(expr);
        rest = lt_strdup(value + i + 1);
        lt_str_trim(rest);
        if (rest[0] == ',') {
            char *r2 = lt_strdup(rest + 1);
            lt_free(rest);
            rest = lt_str_trim(r2);
        }
        np = split_frontpage(rest, &p);
        if (np >= 6) {
            lt_ini_fp_indicator *in = (lt_ini_fp_indicator *)ini_push(
                (void **)&fp->inds, &fp->n_inds, sizeof(lt_ini_fp_indicator));
            in->expression = expr;
            expr = NULL;
            in->label_off = ini_dequote(p[0]);
            in->label_on = ini_dequote(p[1]);
            in->bg_off = lt_strdup(p[2]);
            in->fg_off = lt_strdup(p[3]);
            in->bg_on = lt_strdup(p[4]);
            in->fg_on = lt_strdup(p[5]);
        }
        if (expr) lt_free(expr);
        ini_strv_free(p, np);
        lt_free(rest);
    }
}

/* ------------------------------------------------------------------ */
/* tables / curves                                                     */
/* ------------------------------------------------------------------ */

static void handle_table(lt_ini *ini, ini_pstate *st, const char *lk, const char *value)
{
    char **p;
    size_t n;
    lt_ini_table *t = st->table;
    double d;

    if (strcmp(lk, "table") == 0) {
        unsigned long u;
        lt_ini_table *nt;
        n = ini_split_naive(value, &p);
        if (n >= 3) {
            nt = (lt_ini_table *)lt_calloc(1, sizeof(lt_ini_table));
            nt->name = lt_strdup(p[0]);
            nt->map_name = lt_strdup(p[1]);
            nt->title = ini_dequote(p[2]);
            nt->page = (n > 3 && ini_parse_uint(p[3], &u) && u < 256) ? (int)u : 0;
            nt->y_size = 1;
            st->table = (lt_ini_table *)ini_put((void ***)&ini->tables, &ini->n_tables,
                                                &ini->table_idx, nt->name, nt,
                                                ini_freev_table);
            lt_map_put(&ini->table_idx, nt->map_name, nt);
        }
        ini_strv_free(p, n);
        return;
    }
    if (!t) return;
    if (strcmp(lk, "xbins") == 0 || strcmp(lk, "ybins") == 0) {
        int is_y = lk[0] == 'y';
        n = ini_split_naive(value, &p);
        ini_set_str(is_y ? &t->y_const : &t->x_const, lt_strdup(p[0]));
        if (n > 1 && p[1][0]) ini_set_str(is_y ? &t->y_channel : &t->x_channel, lt_strdup(p[1]));
        if (is_y) t->is_3d = LT_TRUE;
        ini_strv_free(p, n);
    } else if (strcmp(lk, "zbins") == 0) {
        ini_set_str(&t->z_const, trimmed(value));
    } else if (strcmp(lk, "gridheight") == 0) {
        if (ini_parse_f64(value, &d)) {
            t->has_grid_height = LT_TRUE;
            t->grid_height = d;
        }
    } else if (strcmp(lk, "gridorient") == 0) {
        n = ini_split_naive(value, &p);
        if (n >= 3) {
            double a, b, c;
            if (ini_parse_f64(p[0], &a) && ini_parse_f64(p[1], &b) && ini_parse_f64(p[2], &c)) {
                t->has_orient = LT_TRUE;
                t->orient[0] = a;
                t->orient[1] = b;
                t->orient[2] = c;
            }
        }
        ini_strv_free(p, n);
    } else if (strcmp(lk, "topichelp") == 0) {
        ini_set_str(&t->help, ini_dequote(value));
    } else if (strcmp(lk, "xylabels") == 0) {
        n = ini_split(value, &p);
        ini_set_str(&t->x_label, ini_dequote(p[0]));
        if (n > 1) ini_set_str(&t->y_label, ini_dequote(p[1]));
        ini_strv_free(p, n);
    } else if (strcmp(lk, "updownlabel") == 0) {
        n = ini_split(value, &p);
        ini_set_str(&t->up_label, ini_dequote(p[0]));
        if (n > 1) ini_set_str(&t->down_label, ini_dequote(p[1]));
        ini_strv_free(p, n);
    }
}

static void handle_curve(lt_ini *ini, ini_pstate *st, const char *lk, const char *value)
{
    char **p;
    size_t n;
    lt_ini_curve *c = st->curve;

    if (strcmp(lk, "curve") == 0) {
        lt_ini_curve *nc = (lt_ini_curve *)lt_calloc(1, sizeof(lt_ini_curve));
        n = ini_split_naive(value, &p);
        nc->name = lt_strdup(p[0]);
        nc->title = n > 1 ? ini_dequote(p[1]) : lt_strdup("");
        nc->size = -1;
        ini_strv_free(p, n);
        st->curve = (lt_ini_curve *)ini_put((void ***)&ini->curves, &ini->n_curves,
                                            &ini->curve_idx, nc->name, nc,
                                            ini_freev_curve);
        return;
    }
    if (!c) return;
    if (strcmp(lk, "columnlabel") == 0) {
        n = ini_split_naive(value, &p);
        if (n >= 2) {
            ini_set_str(&c->col_x, ini_dequote(p[0]));
            ini_set_str(&c->col_y, ini_dequote(p[1]));
        }
        ini_strv_free(p, n);
    } else if (strcmp(lk, "xaxis") == 0 || strcmp(lk, "yaxis") == 0) {
        n = ini_split_naive(value, &p);
        if (n >= 3) {
            double a, b, s;
            if (ini_parse_f64(p[0], &a) && ini_parse_f64(p[1], &b) && ini_parse_f64(p[2], &s)) {
                double *dst = lk[0] == 'x' ? c->xaxis : c->yaxis;
                dst[0] = a;
                dst[1] = b;
                dst[2] = s;
                if (lk[0] == 'x') c->has_xaxis = LT_TRUE;
                else c->has_yaxis = LT_TRUE;
            }
        }
        ini_strv_free(p, n);
    } else if (strcmp(lk, "xbins") == 0) {
        n = ini_split(value, &p);
        ini_set_str(&c->x_const, lt_strdup(p[0]));
        if (n > 1 && p[1][0]) ini_set_str(&c->x_channel, lt_strdup(p[1]));
        ini_strv_free(p, n);
    } else if (strcmp(lk, "ybins") == 0) {
        n = ini_split(value, &p);
        if (!c->y_const) c->y_const = lt_strdup(p[0]);
        else ini_set_str(&c->y_const2, lt_strdup(p[0]));
        ini_strv_free(p, n);
    } else if (strcmp(lk, "size") == 0) {
        unsigned long u;
        if (ini_parse_uint(value, &u)) c->size = (int)u;
    } else if (strcmp(lk, "topichelp") == 0) {
        ini_set_str(&c->help, ini_dequote(value));
    } else if (strcmp(lk, "gauge") == 0) {
        ini_set_str(&c->gauge, trimmed(value));
    } else if (strcmp(lk, "linelabel") == 0) {
        ini_str_push(&c->line_labels, &c->n_line_labels, ini_dequote(value));
    }
}

/* ------------------------------------------------------------------ */
/* reference tables                                                    */
/* ------------------------------------------------------------------ */

void ini_flush_limits(lt_ini *ini, ini_pstate *st)
{
    size_t i, k;
    LT_UNUSED(ini);
    if (st->ref) {
        for (i = 0; i < st->limits.len; i++) {
            const ini_limit *l = &LT_VEC_AT(&st->limits, ini_limit, i);
            for (k = 0; k < st->ref->n_idents; k++) {
                lt_ini_ref_ident *id = &st->ref->idents[k];
                if (id->id == l->id) {
                    id->has_limits = LT_TRUE;
                    id->lim_min = l->lo;
                    id->lim_max = l->hi;
                    id->lim_default = l->dflt;
                }
            }
        }
    }
    lt_vec_clear(&st->limits);
}

static void handle_reftable(lt_ini *ini, ini_pstate *st, const char *lk, const char *value)
{
    char **p;
    size_t n, i;
    lt_ini_ref_table *t = st->ref;
    double d;
    unsigned long u;

    if (strcmp(lk, "tablewritecommand") == 0) {
        ini_set_str(&ini->proto.table_write_command, ini_dequote(value));
        return;
    }
    if (strcmp(lk, "tableblockingfactor") == 0) {
        if (ini_parse_uint(value, &u)) ini->proto.table_blocking_factor = (int)u;
        return;
    }
    if (strcmp(lk, "referencetable") == 0) {
        n = ini_split(value, &p);
        ini_flush_limits(ini, st);
        t = (lt_ini_ref_table *)lt_calloc(1, sizeof(lt_ini_ref_table));
        t->name = lt_strdup(p[0]);
        t->label = n > 1 ? ini_dequote(p[1]) : lt_strdup(p[0]);
        t->bytes_per_adc = 1;
        t->scale = 1.0;
        ini_strv_free(p, n);
        ini->ref_tables = (lt_ini_ref_table **)lt_realloc(ini->ref_tables,
                                (ini->n_ref_tables + 1) * sizeof(*ini->ref_tables));
        ini->ref_tables[ini->n_ref_tables++] = t;
        st->ref = t;
        return;
    }
    if (!t) return;
    if (strcmp(lk, "topichelp") == 0) {
        ini_set_str(&t->help_topic, ini_dequote(value));
    } else if (strcmp(lk, "tableidentifier") == 0) {
        n = ini_split(value, &p);
        for (i = 0; i + 1 < n + 1; i += 2) {
            lt_ini_ref_ident *id;
            if (i + 1 >= n) break;
            if (!ini_parse_uint(p[i], &u) || u > 255) continue;
            id = (lt_ini_ref_ident *)ini_push((void **)&t->idents, &t->n_idents,
                                              sizeof(lt_ini_ref_ident));
            id->id = (lt_u8)u;
            id->label = ini_dequote(p[i + 1]);
        }
        ini_strv_free(p, n);
    } else if (strcmp(lk, "adccount") == 0) {
        if (ini_parse_uint(value, &u)) t->adc_count = (size_t)u;
    } else if (strcmp(lk, "bytesperadc") == 0) {
        if (ini_parse_uint(value, &u)) t->bytes_per_adc = (unsigned)u;
    } else if (strcmp(lk, "scale") == 0) {
        if (ini_parse_f64(value, &d)) t->scale = d;
    } else if (strcmp(lk, "solutionslabel") == 0) {
        ini_set_str(&t->solutions_label, ini_dequote(value));
    } else if (strcmp(lk, "tablegenerator") == 0) {
        n = ini_split(value, &p);
        if (n >= 2) {
            lt_ini_generator *g = (lt_ini_generator *)ini_push((void **)&t->generators,
                                        &t->n_generators, sizeof(lt_ini_generator));
            double nums[4];
            size_t cnt = 0;
            g->kind = lt_strdup(p[0]);
            g->label = ini_dequote(p[1]);
            if (n > 2) g->x_units = ini_dequote(p[2]);
            if (n > 3) g->y_units = ini_dequote(p[3]);
            for (i = 4; i < n; i++) {
                if (ini_parse_f64(p[i], &d)) {
                    if (cnt < 4) nums[cnt] = d;
                    cnt++;
                }
            }
            if (cnt >= 4) {
                g->has_bounds = LT_TRUE;
                g->x_lo = nums[0];
                g->x_hi = nums[1];
                g->y_lo = nums[2];
                g->y_hi = nums[3];
            }
        }
        ini_strv_free(p, n);
    } else if (strcmp(lk, "thermoption") == 0) {
        n = ini_split(value, &p);
        if (n >= 8) {
            double v[7];
            int ok = 1;
            for (i = 0; i < 7; i++)
                if (!ini_parse_f64(p[i + 1], &v[i])) ok = 0;
            if (ok) {
                lt_ini_therm_opt *o = (lt_ini_therm_opt *)ini_push((void **)&t->therm_opts,
                                            &t->n_therm_opts, sizeof(lt_ini_therm_opt));
                o->name = ini_dequote(p[0]);
                o->bias_resistor = v[0];
                for (i = 0; i < 3; i++) {
                    o->t[i] = v[1 + 2 * i];
                    o->r[i] = v[2 + 2 * i];
                }
            }
        }
        ini_strv_free(p, n);
    } else if (strcmp(lk, "solution") == 0) {
        n = ini_split(value, &p);
        if (n >= 2) {
            char *body = lt_strdup(p[1]);
            if (ini_is_braced(body)) {
                char *e = ini_unbrace(body, 1);
                if (e[0]) {
                    lt_ini_solution *s = (lt_ini_solution *)ini_push((void **)&t->solutions,
                                                &t->n_solutions, sizeof(lt_ini_solution));
                    s->label = ini_dequote(p[0]);
                    s->expression = e;
                } else {
                    lt_free(e);
                }
            } else {
                lt_ini_solution *s = (lt_ini_solution *)ini_push((void **)&t->solutions,
                                            &t->n_solutions, sizeof(lt_ini_solution));
                s->label = ini_dequote(p[0]);
                s->generator = ini_dequote(body);
            }
            lt_free(body);
        }
        ini_strv_free(p, n);
    } else if (strcmp(lk, "tablelimits") == 0) {
        n = ini_split_naive(value, &p);
        if (n >= 4) {
            double a, b, c;
            unsigned long id;
            if (ini_parse_uint(p[0], &id) && ini_parse_f64(p[1], &a) &&
                ini_parse_f64(p[2], &b) && ini_parse_f64(p[3], &c)) {
                ini_limit *l = (ini_limit *)lt_vec_push(&st->limits);
                l->id = id;
                l->lo = a;
                l->hi = b;
                l->dflt = c;
            }
        }
        ini_strv_free(p, n);
    }
}

/* ------------------------------------------------------------------ */
/* analyze sections                                                    */
/* ------------------------------------------------------------------ */

static void parse_filter(lt_ini_analyze *a, const char *value)
{
    char **p;
    size_t n = ini_split(value, &p), i;
    lt_ini_filter *f;
    double d;

    if (n == 1) {
        if (lt_str_starts(p[0], "std_")) {
            f = (lt_ini_filter *)ini_push((void **)&a->filters, &a->n_filters, sizeof(*f));
            f->name = lt_strdup(p[0]);
            f->display = lt_strdup(p[0]);
            f->channel = lt_strdup("");
            f->op = LT_FOP_EQ;
        }
    } else if (n >= 5) {
        f = (lt_ini_filter *)ini_push((void **)&a->filters, &a->n_filters, sizeof(*f));
        f->name = lt_strdup(p[0]);
        f->display = ini_dequote(p[1]);
        f->channel = lt_strdup(p[2]);
        if (strcmp(p[3], "<") == 0) f->op = LT_FOP_LT;
        else if (strcmp(p[3], ">") == 0) f->op = LT_FOP_GT;
        else if (strcmp(p[3], "!=") == 0) f->op = LT_FOP_NE;
        else if (strcmp(p[3], "&") == 0) f->op = LT_FOP_BITAND;
        else if (strcmp(p[3], "|") == 0) f->op = LT_FOP_BITOR;
        else f->op = LT_FOP_EQ;
        f->dflt = ini_parse_f64(p[4], &d) ? d : 0.0;
        for (i = 5; i < n; i++) {
            if (p[i][0]) {
                f->adjustable = ini_parse_bool(p[i]) ? LT_TRUE : LT_FALSE;
                break;
            }
        }
    }
    ini_strv_free(p, n);
}

static void handle_analyze(lt_ini *ini, const char *section, const char *lk, const char *value)
{
    lt_ini_analyze *a;
    char **p;
    size_t n, i;
    double d;

    if (strcmp(section, "veanalyze") == 0) a = &ini->ve_analyze;
    else if (strcmp(section, "wueanalyze") == 0) a = &ini->wue_analyze;
    else a = &ini->gamma_e;
    a->present = LT_TRUE;

    if (strcmp(lk, "filter") == 0) {
        parse_filter(a, value);
    } else if (strcmp(lk, "option") == 0) {
        ini_str_push(&a->options, &a->n_options, trimmed(value));
    } else if (strcmp(lk, "lambdatargettables") == 0) {
        n = ini_split(value, &p);
        for (i = 0; i < n; i++)
            if (p[i][0]) ini_str_push(&a->lambda_target_tables, &a->n_lambda_target_tables,
                                      lt_strdup(p[i]));
        ini_strv_free(p, n);
    } else if (strcmp(section, "veanalyze") == 0 && strcmp(lk, "veanalyzemap") == 0) {
        n = ini_split(value, &p);
        if (n >= 4) {
            ini_set_str(&a->ve_table, lt_strdup(p[0]));
            ini_set_str(&a->target_table, lt_strdup(p[1]));
            ini_set_str(&a->lambda_channel, lt_strdup(p[2]));
            ini_set_str(&a->ego_channel, lt_strdup(p[3]));
            if (n > 4 && ini_is_braced(p[4])) ini_set_str(&a->active_cond, ini_unbrace(p[4], 1));
        }
        ini_strv_free(p, n);
    } else if (strcmp(section, "wueanalyze") == 0 && strcmp(lk, "wueanalyzemap") == 0) {
        n = ini_split(value, &p);
        if (n >= 7) {
            ini_set_str(&a->wue_curve, lt_strdup(p[0]));
            ini_set_str(&a->afr_comp_curve, lt_strdup(p[1]));
            ini_set_str(&a->target_table, lt_strdup(p[2]));
            ini_set_str(&a->lambda_channel, lt_strdup(p[3]));
            ini_set_str(&a->coolant_channel, lt_strdup(p[4]));
            ini_set_str(&a->wue_channel, lt_strdup(p[5]));
            ini_set_str(&a->ego_channel, lt_strdup(p[6]));
        }
        ini_strv_free(p, n);
    } else if (strcmp(section, "wueanalyze") == 0 && strcmp(lk, "wuepercentoffset") == 0) {
        a->wue_percent_offset = ini_parse_f64(value, &d) ? d : 0.0;
    } else if (strcmp(section, "gammae") == 0 &&
               (strcmp(lk, "gammaemap") == 0 || strcmp(lk, "gammaetable") == 0)) {
        n = ini_split(value, &p);
        if (n >= 3) {
            ini_set_str(&a->gamma_table, lt_strdup(p[0]));
            ini_set_str(&a->lambda_channel, lt_strdup(p[1]));
            ini_set_str(&a->target_table, lt_strdup(p[2]));
        }
        ini_strv_free(p, n);
    }
}

/* ------------------------------------------------------------------ */
/* defaults and extensions                                             */
/* ------------------------------------------------------------------ */

static void set_default(lt_ini *ini, const char *name, double v)
{
    double *d = (double *)lt_map_get(&ini->default_values, name);
    if (!d) {
        d = (double *)lt_malloc(sizeof(double));
        lt_map_put(&ini->default_values, name, d);
    }
    *d = v;
}

static void default_value_line(lt_ini *ini, const char *value)
{
    char **p;
    size_t n = ini_split(value, &p);
    if (n >= 2) {
        lt_buf b;
        char *tok;
        size_t i, k;
        double d;
        lt_buf_init(&b);
        for (i = 1; i < n; i++) {
            if (i > 1) lt_buf_putc(&b, ',');
            lt_buf_puts(&b, p[i]);
        }
        tok = b.data ? b.data : (char *)"";
        while (*tok == ' ' || *tok == '\t') tok++;
        for (k = 0; tok[k] && tok[k] != ' ' && tok[k] != '\t' && tok[k] != ','; k++) {}
        {
            char *t1 = lt_strndup(tok, k);
            if (ini_parse_f64(t1, &d)) set_default(ini, p[0], d);
            lt_free(t1);
        }
        lt_buf_free(&b);
    }
    ini_strv_free(p, n);
}

static void handle_extensions(lt_ini *ini, const char *lk, const char *value)
{
    char **p;
    size_t n;
    unsigned long u;

    if (strcmp(lk, "defaultvalue") == 0) {
        default_value_line(ini, value);
    } else if (strcmp(lk, "maintainconstantvalue") == 0) {
        const char *ob = strchr(value, '{');
        const char *cb = strrchr(value, '}');
        if (ob && cb && cb > ob) {
            char *name = lt_strndup(value, (size_t)(ob - value));
            char *expr = lt_strndup(ob + 1, (size_t)(cb - ob - 1));
            size_t l;
            lt_str_trim(name);
            l = strlen(name);
            while (l > 0 && (name[l - 1] == ',' || name[l - 1] == ' ')) name[--l] = '\0';
            lt_str_trim(expr);
            ini_str_push(&ini->maintain_constants, &ini->n_maintain_constants, name);
            {
                size_t n2 = ini->n_maintain_constants - 1;
                ini->maintain_exprs = (char **)lt_realloc(ini->maintain_exprs,
                                                          (n2 + 1) * sizeof(char *));
                ini->maintain_exprs[n2] = expr;
            }
        }
    } else if (strcmp(lk, "maximumelements") == 0) {
        n = ini_split(value, &p);
        if (n >= 2 && ini_parse_uint(p[1], &u) && u > 0)
            lt_map_put(&ini->maximum_elements, p[0], (void *)(size_t)(u > 65535UL ? 65535UL : u));
        ini_strv_free(p, n);
    } else if (strcmp(lk, "requirespowercycle") == 0) {
        char *t = trimmed(value);
        ini_str_push(&ini->requires_power_cycle, &ini->n_requires_power_cycle, t);
    }
}

/* ------------------------------------------------------------------ */
/* [PortEditor]                                                        */
/* ------------------------------------------------------------------ */
static lt_ini_port_editor *port_editor_new(lt_ini *ini, const char *name, char *label)
{
    lt_ini_port_editor *pe = (lt_ini_port_editor *)ini_push((void **)&ini->port_editors,
                                                            &ini->n_port_editors, sizeof(*pe));
    pe->name = lt_strdup(name);
    pe->label = label;
    return pe;
}

/* the editor the mapping keys attach to (the last one; a default std_port_edit if none) */
static lt_ini_port_editor *port_editor_cur(lt_ini *ini)
{
    if (ini->n_port_editors == 0)
        return port_editor_new(ini, "std_port_edit", lt_strdup("Output Port Settings"));
    return &ini->port_editors[ini->n_port_editors - 1];
}

static void port_labels(char **p, size_t n, size_t from, char ***out, size_t *nout)
{
    size_t i;
    for (i = from; i < n; i++) {
        if (ini_is_braced(p[i])) continue;
        ini_str_push(out, nout, ini_dequote(p[i]));
    }
}

static void port_editor_line(lt_ini *ini, const char *key, const char *lk, const char *value)
{
    static const char *const keys[] = {
        "enabledports", "outputcanid", "outputoffset", "outputsize", "operators", "threshold",
        "hysteresis", "poweronvalue", "triggervalue", "conditionrelationship", "outputoffset2",
        "outputsize2", "operators2", "threshold2", "hysteresis2"
    };
    char **p;
    size_t n, i, k;
    lt_ini_port_editor *pe;
    if (strcmp(lk, "topichelp") == 0) {
        if (ini->n_port_editors > 0) ini_set_str(&port_editor_cur(ini)->help_topic, ini_dequote(value));
        return;
    }
    n = ini_split(value, &p);
    if (n == 0) {
        ini_strv_free(p, n);
        return;
    }
    if (strcmp(lk, "portnames") == 0) {
        pe = port_editor_cur(ini);
        port_labels(p, n, 0, &pe->port_names, &pe->n_port_names);
        ini_strv_free(p, n);
        return;
    }
    {
        for (k = 0; k < sizeof(keys) / sizeof(keys[0]); k++)
            if (strcmp(lk, keys[k]) == 0) break;
        if (strcmp(lk, "porteditor") == 0 || k == sizeof(keys) / sizeof(keys[0])) {
            /* a new editor: "portEditor = name, \"Title\"" or "name = \"Title\"" */
            size_t first = 0;
            if (strcmp(lk, "porteditor") == 0) {
                pe = port_editor_new(ini, p[0], n > 1 ? ini_dequote(p[1]) : lt_strdup(p[0]));
                first = 2;
            } else {
                pe = port_editor_new(ini, key, ini_dequote(p[0]));
                first = 1;
            }
            for (i = first; i < n; i++)
                if (ini_is_braced(p[i])) {
                    pe->enable_cond = ini_unbrace(p[i], 1);
                    break;
                }
            ini_strv_free(p, n);
            return;
        }
        pe = port_editor_cur(ini);
        switch (k) {
        case 0: ini_set_str(&pe->enabled_const, lt_strdup(p[0]));
                port_labels(p, n, 1, &pe->port_names, &pe->n_port_names); break;
        case 1: ini_set_str(&pe->can_id_const, lt_strdup(p[0])); break;
        case 2: ini_set_str(&pe->offset_const, lt_strdup(p[0])); break;
        case 3: ini_set_str(&pe->size_const, lt_strdup(p[0])); break;
        case 4: ini_set_str(&pe->op_const, lt_strdup(p[0]));
                port_labels(p, n, 1, &pe->op_labels, &pe->n_op_labels); break;
        case 5: ini_set_str(&pe->threshold_const, lt_strdup(p[0])); break;
        case 6: ini_set_str(&pe->hysteresis_const, lt_strdup(p[0])); break;
        case 7: ini_set_str(&pe->power_on_const, lt_strdup(p[0])); break;
        case 8: ini_set_str(&pe->trigger_const, lt_strdup(p[0])); break;
        case 9: ini_set_str(&pe->connector_const, lt_strdup(p[0]));
                port_labels(p, n, 1, &pe->conn_labels, &pe->n_conn_labels); break;
        case 10: ini_set_str(&pe->offset2_const, lt_strdup(p[0])); break;
        case 11: ini_set_str(&pe->size2_const, lt_strdup(p[0])); break;
        case 12: ini_set_str(&pe->op2_const, lt_strdup(p[0]));
                 if (pe->n_op_labels == 0) port_labels(p, n, 1, &pe->op_labels, &pe->n_op_labels);
                 break;
        case 13: ini_set_str(&pe->threshold2_const, lt_strdup(p[0])); break;
        default: ini_set_str(&pe->hysteresis2_const, lt_strdup(p[0])); break;
        }
    }
    ini_strv_free(p, n);
}

/* ------------------------------------------------------------------ */
/* the rest                                                            */
/* ------------------------------------------------------------------ */

static void handle_commands(lt_ini *ini, const char *key, const char *value)
{
    char **p;
    size_t n = ini_split(value, &p), i;
    lt_ini_cmd *c = (lt_ini_cmd *)lt_calloc(1, sizeof(lt_ini_cmd));

    c->name = lt_strdup(key);
    for (i = 0; i < n; i++) {
        lt_ini_cmd_part *part;
        if (!p[i][0]) continue;
        if (ini_is_braced(p[i])) {
            ini_set_str(&c->enable_cond, ini_unbrace(p[i], 1));
            continue;
        }
        part = (lt_ini_cmd_part *)ini_push((void **)&c->parts, &c->n_parts, sizeof(*part));
        if (p[i][0] == '"' && p[i][strlen(p[i]) - 1] == '"' && strlen(p[i]) >= 1) {
            char *q = ini_dequote(p[i]);
            part->data = ini_blob_from_str(q);
            lt_free(q);
        } else {
            part->is_ref = LT_TRUE;
            part->data = ini_blob_from_str(p[i]);
        }
    }
    if (c->n_parts == 0) {
        lt_ini_cmd_part *part = (lt_ini_cmd_part *)ini_push((void **)&c->parts, &c->n_parts,
                                                            sizeof(*part));
        char *q = ini_dequote(p[0]);
        part->data = ini_blob_from_str(q);
        lt_free(q);
    }
    ini_strv_free(p, n);
    ini_put((void ***)&ini->commands, &ini->n_commands, &ini->command_idx, c->name, c,
            ini_freev_cmd);
}

void ini_section_line(lt_ini *ini, ini_ctx *cx, ini_pstate *st, const char *key,
                      const char *value)
{
    const char *s = st->section;
    char *lk = lower_dup(key);
    char **p;
    size_t n, i;

    LT_UNUSED(cx);
    if (strcmp(s, "gaugeconfigurations") == 0) {
        if (strcmp(lk, "gaugecategory") == 0) {
            ini_set_str(&st->category, ini_dequote(value));
        } else {
            lt_ini_gauge *g = ini_parse_gauge_line(key, value);
            if (g) {
                g->category = dup_or_null(st->category);
                ini_put((void ***)&ini->gauges, &ini->n_gauges, &ini->gauge_idx, g->name, g,
                        ini_freev_gauge);
            }
        }
    } else if (strcmp(s, "settinggroups") == 0) {
        n = ini_split_naive(value, &p);
        if (strcmp(lk, "settinggroup") == 0 && n >= 2) {
            lt_ini_setting_group *g = (lt_ini_setting_group *)lt_calloc(1, sizeof(*g));
            g->name = lt_strdup(p[0]);
            g->label = ini_dequote(p[1]);
            ini->setting_groups = (lt_ini_setting_group **)lt_realloc(ini->setting_groups,
                                    (ini->n_setting_groups + 1) * sizeof(*ini->setting_groups));
            ini->setting_groups[ini->n_setting_groups++] = g;
            st->group = g;
        } else if (strcmp(lk, "settingoption") == 0 && n >= 2 && st->group) {
            lt_ini_setting_group *g = st->group;
            g->options = lt_realloc(g->options, (g->n_options + 1) * sizeof(g->options[0]));
            g->options[g->n_options].value = lt_strdup(p[0]);
            g->options[g->n_options].label = ini_dequote(p[1]);
            g->n_options++;
        }
        ini_strv_free(p, n);
    } else if (strcmp(s, "datalog") == 0) {
        n = ini_split(value, &p);
        if (n > 0 && p[0][0]) {
            lt_ini_dl_entry *e = (lt_ini_dl_entry *)ini_push((void **)&ini->dl_entries,
                                        &ini->n_dl_entries, sizeof(lt_ini_dl_entry));
            char *f;
            e->channel = ini_dequote(p[0]);
            f = n > 1 ? ini_dequote(p[1]) : lt_strdup("");
            e->label = f;
            f = n > 2 ? ini_dequote(p[2]) : lt_strdup("");
            e->type = f[0] ? f : (lt_free(f), lt_strdup("float"));
            f = n > 3 ? ini_dequote(p[3]) : lt_strdup("");
            e->format = f[0] ? f : (lt_free(f), lt_strdup("%.2f"));
            if (n > 4 && ini_is_braced(p[4])) e->condition = ini_unbrace(p[4], 1);
            e->enabled = LT_TRUE;
        }
        ini_strv_free(p, n);
    } else if (strcmp(s, "defaults") == 0) {
        if (strcmp(lk, "defaultvalue") == 0) default_value_line(ini, value);
    } else if (strcmp(s, "menu") == 0) {
        handle_menu(ini, lk, value);
    } else if (strcmp(s, "userdefined") == 0) {
        handle_userdefined(ini, st, lk, value);
    } else if (strcmp(s, "settingcontexthelp") == 0) {
        lt_ini_const *c = (lt_ini_const *)lt_map_get(&ini->constant_idx, key);
        if (c) ini_set_str(&c->help, ini_dequote(value));
    } else if (strcmp(s, "frontpage") == 0) {
        handle_frontpage(ini, lk, value);
    } else if (strcmp(s, "controllercommands") == 0) {
        handle_commands(ini, key, value);
    } else if (strcmp(s, "porteditor") == 0) {
        port_editor_line(ini, key, lk, value);
    } else if (strcmp(s, "referencetables") == 0) {
        handle_reftable(ini, st, lk, value);
    } else if (strcmp(s, "datalogviews") == 0) {
        n = ini_split(value, &p);
        if (n >= 2) {
            lt_ini_dl_view *v = (lt_ini_dl_view *)ini_push((void **)&ini->dl_views,
                                                           &ini->n_dl_views, sizeof(*v));
            v->name = lt_strdup(key);
            v->label = ini_dequote(p[0]);
            for (i = 1; i < n; i++) ini_str_push(&v->channels, &v->n_channels, lt_strdup(p[i]));
        }
        ini_strv_free(p, n);
    } else if (strcmp(s, "keyactions") == 0) {
        lt_ini_key_action *ka;
        n = ini_split(value, &p);
        if (strcmp(lk, "showpanel") == 0 && n >= 2) {
            lt_buf b;
            ka = (lt_ini_key_action *)ini_push((void **)&ini->key_actions, &ini->n_key_actions,
                                               sizeof(*ka));
            ka->key = lt_strdup(p[0]);
            lt_buf_init(&b);
            lt_buf_printf(&b, "showPanel:%s", p[1]);
            ka->action = lt_buf_take(&b);
            lt_buf_init(&b);
            lt_buf_printf(&b, "Show %s", p[1]);
            ka->label = lt_buf_take(&b);
        } else if (n >= 2) {
            ka = (lt_ini_key_action *)ini_push((void **)&ini->key_actions, &ini->n_key_actions,
                                               sizeof(*ka));
            ka->key = lt_strdup(key);
            ka->action = lt_strdup(p[0]);
            ka->label = ini_dequote(p[1]);
            if (n > 2 && ini_is_braced(p[2])) ka->cond = ini_unbrace(p[2], 1);
        }
        ini_strv_free(p, n);
    } else if (strcmp(s, "veanalyze") == 0 || strcmp(s, "wueanalyze") == 0 ||
               strcmp(s, "gammae") == 0) {
        handle_analyze(ini, s, lk, value);
    } else if (strcmp(s, "constantsextensions") == 0) {
        handle_extensions(ini, lk, value);
    } else if (strstr(s, "tableeditor")) {
        handle_table(ini, st, lk, value);
    } else if (strstr(s, "curveeditor")) {
        handle_curve(ini, st, lk, value);
    }
    /* anything else (passive or unknown sections): ignored */
    lt_free(lk);
}
