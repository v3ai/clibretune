/* ini_post.c - post-processing passes, table roles, lookups and destruction.
 * Spec: docs/spec/01-ini-core.md sections 5.5, 7.11, 7.15, 7.17, 8, 9.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "ini_priv.h"

/* ------------------------------------------------------------------ */
/* destructors                                                         */
/* ------------------------------------------------------------------ */

static void fr(char *s)
{
    if (s) lt_free(s);
}

void ini_free_str_array(char **v, size_t n)
{
    size_t i;
    if (!v) return;
    for (i = 0; i < n; i++) fr(v[i]);
    lt_free(v);
}

void ini_free_blob_array(lt_ini_blob *v, size_t n)
{
    size_t i;
    if (!v) return;
    for (i = 0; i < n; i++) ini_blob_free(&v[i]);
    lt_free(v);
}

void ini_free_cmdlist(lt_ini_cmdlist *l)
{
    ini_free_blob_array(l->v, l->n);
    l->v = NULL;
    l->n = 0;
}

void ini_free_const(lt_ini_const *c)
{
    if (!c) return;
    fr(c->name);
    fr(c->units);
    fr(c->help);
    fr(c->scale_expr);
    fr(c->translate_expr);
    fr(c->min_expr);
    fr(c->max_expr);
    ini_free_str_array(c->bit_options, c->n_bit_options);
    if (c->dyn) {
        fr(c->dyn->cols_const);
        fr(c->dyn->rows_const);
        lt_free(c->dyn);
    }
    lt_free(c);
}

void ini_free_channel(lt_ini_channel *c)
{
    if (!c) return;
    fr(c->name);
    fr(c->units);
    fr(c->expression);
    lt_expr_free(c->ast);
    lt_free(c);
}

void ini_free_table(lt_ini_table *t)
{
    if (!t) return;
    fr(t->name); fr(t->map_name); fr(t->title); fr(t->z_const);
    fr(t->x_const); fr(t->x_channel); fr(t->y_const); fr(t->y_channel);
    fr(t->help); fr(t->x_label); fr(t->y_label); fr(t->up_label); fr(t->down_label);
    fr(t->rows_size_const); fr(t->cols_size_const);
    lt_free(t);
}

void ini_free_curve(lt_ini_curve *c)
{
    if (!c) return;
    fr(c->name); fr(c->title); fr(c->x_const); fr(c->x_channel); fr(c->y_const);
    fr(c->y_const2); fr(c->col_x); fr(c->col_y); fr(c->help); fr(c->gauge);
    ini_free_str_array(c->line_labels, c->n_line_labels);
    lt_free(c);
}

static void free_comp(lt_ini_comp *c)
{
    size_t i, k;
    fr(c->label); fr(c->name); fr(c->position); fr(c->vis_cond); fr(c->en_cond);
    fr(c->orientation); fr(c->expression); fr(c->label_on); fr(c->label_off);
    ini_free_str_array(c->channels, c->n_channels);
    for (i = 0; i < c->n_options; i++) {
        lt_ini_setting_opt *o = &c->options[i];
        fr(o->label);
        for (k = 0; k < o->n_assigns; k++) fr(o->assigns[k].name);
        if (o->assigns) lt_free(o->assigns);
    }
    if (c->options) lt_free(c->options);
}

void ini_free_dialog(lt_ini_dialog *d)
{
    size_t i;
    if (!d) return;
    fr(d->name); fr(d->title); fr(d->layout_hint); fr(d->help_topic);
    for (i = 0; i < d->n_comps; i++) free_comp(&d->comps[i]);
    if (d->comps) lt_free(d->comps);
    lt_free(d);
}

void ini_free_menu_item(lt_ini_menu_item *m)
{
    size_t i;
    if (!m) return;
    fr(m->label); fr(m->target); fr(m->vis_cond); fr(m->en_cond);
    for (i = 0; i < m->n_items; i++) ini_free_menu_item(m->items[i]);
    if (m->items) lt_free(m->items);
    lt_free(m);
}

void ini_free_help(void *p)
{
    lt_ini_help *h = (lt_ini_help *)p;
    fr(h->name); fr(h->title); fr(h->web_url);
    ini_free_str_array(h->lines, h->n_lines);
    lt_free(h);
}

void ini_free_cmd(lt_ini_cmd *c)
{
    size_t i;
    if (!c) return;
    fr(c->name); fr(c->enable_cond);
    for (i = 0; i < c->n_parts; i++) ini_blob_free(&c->parts[i].data);
    if (c->parts) lt_free(c->parts);
    lt_free(c);
}

void ini_free_gauge(lt_ini_gauge *g)
{
    if (!g) return;
    fr(g->name); fr(g->channel); fr(g->title); fr(g->units);
    fr(g->lo_expr); fr(g->hi_expr); fr(g->low_danger_expr); fr(g->low_warning_expr);
    fr(g->high_warning_expr); fr(g->high_danger_expr); fr(g->category);
    lt_free(g);
}

/* void * adapters for ini_put() free callbacks */
void ini_freev_const(void *p) { ini_free_const((lt_ini_const *)p); }
void ini_freev_channel(void *p) { ini_free_channel((lt_ini_channel *)p); }
void ini_freev_table(void *p) { ini_free_table((lt_ini_table *)p); }
void ini_freev_curve(void *p) { ini_free_curve((lt_ini_curve *)p); }
void ini_freev_dialog(void *p) { ini_free_dialog((lt_ini_dialog *)p); }
void ini_freev_cmd(void *p) { ini_free_cmd((lt_ini_cmd *)p); }
void ini_freev_gauge(void *p) { ini_free_gauge((lt_ini_gauge *)p); }

void ini_free_ref_table(lt_ini_ref_table *t)
{
    size_t i;
    if (!t) return;
    fr(t->name); fr(t->label); fr(t->help_topic); fr(t->solutions_label);
    for (i = 0; i < t->n_idents; i++) fr(t->idents[i].label);
    if (t->idents) lt_free(t->idents);
    for (i = 0; i < t->n_generators; i++) {
        fr(t->generators[i].kind); fr(t->generators[i].label);
        fr(t->generators[i].x_units); fr(t->generators[i].y_units);
    }
    if (t->generators) lt_free(t->generators);
    for (i = 0; i < t->n_therm_opts; i++) fr(t->therm_opts[i].name);
    if (t->therm_opts) lt_free(t->therm_opts);
    for (i = 0; i < t->n_solutions; i++) {
        fr(t->solutions[i].label); fr(t->solutions[i].expression);
        fr(t->solutions[i].generator);
    }
    if (t->solutions) lt_free(t->solutions);
    lt_free(t);
}

void ini_free_analyze(lt_ini_analyze *a)
{
    size_t i;
    fr(a->ve_table); fr(a->target_table); fr(a->lambda_channel); fr(a->ego_channel);
    fr(a->active_cond); fr(a->wue_curve); fr(a->afr_comp_curve); fr(a->wue_channel);
    fr(a->coolant_channel); fr(a->gamma_table);
    ini_free_str_array(a->lambda_target_tables, a->n_lambda_target_tables);
    for (i = 0; i < a->n_filters; i++) {
        fr(a->filters[i].name); fr(a->filters[i].display); fr(a->filters[i].channel);
    }
    if (a->filters) lt_free(a->filters);
    ini_free_str_array(a->options, a->n_options);
}

void ini_free_logger(lt_ini_logger *l)
{
    size_t i;
    fr(l->name); fr(l->label); fr(l->kind); fr(l->data_ready_condition);
    ini_blob_free(&l->start_command);
    ini_blob_free(&l->stop_command);
    ini_blob_free(&l->data_read_command);
    for (i = 0; i < l->n_fields; i++) {
        fr(l->fields[i].name); fr(l->fields[i].label); fr(l->fields[i].units);
    }
    if (l->fields) lt_free(l->fields);
}

void ini_free_contents(lt_ini *ini)
{
    size_t i;
    ini_ctx *cx = (ini_ctx *)ini->priv;
    lt_ini_protocol *p = &ini->proto;

    fr(ini->signature); fr(ini->signature_prefix); fr(ini->version_info);
    fr(ini->ini_spec_version); fr(ini->source_path);
    fr(p->message_envelope_format);
    ini_blob_free(&p->query_command);
    ini_blob_free(&p->och_get_command);
    ini_blob_free(&p->burst_get_command);
    ini_free_blob_array(p->page_identifiers, p->n_page_identifiers);
    ini_free_str_array(p->page_identifiers_raw, p->n_page_identifiers_raw);
    if (p->page_sizes) lt_free(p->page_sizes);
    ini_free_cmdlist(&p->page_read_commands);
    ini_free_cmdlist(&p->page_chunk_write_commands);
    ini_free_cmdlist(&p->page_value_write_commands);
    ini_free_cmdlist(&p->burn_commands);
    ini_free_cmdlist(&p->crc32_check_commands);
    ini_free_cmdlist(&p->page_activate_commands);
    fr(p->retrieve_config_error); fr(p->default_ip_address);
    fr(p->table_write_command); fr(p->table_crc_command);

    for (i = 0; i < ini->n_constants; i++) ini_free_const(ini->constants[i]);
    if (ini->constants) lt_free(ini->constants);
    for (i = 0; i < ini->n_channels; i++) ini_free_channel(ini->channels[i]);
    if (ini->channels) lt_free(ini->channels);
    for (i = 0; i < ini->n_tables; i++) ini_free_table(ini->tables[i]);
    if (ini->tables) lt_free(ini->tables);
    for (i = 0; i < ini->n_curves; i++) ini_free_curve(ini->curves[i]);
    if (ini->curves) lt_free(ini->curves);
    for (i = 0; i < ini->n_gauges; i++) ini_free_gauge(ini->gauges[i]);
    if (ini->gauges) lt_free(ini->gauges);
    for (i = 0; i < ini->n_menus; i++) {
        size_t k;
        fr(ini->menus[i].name);
        fr(ini->menus[i].title);
        for (k = 0; k < ini->menus[i].n_items; k++) ini_free_menu_item(ini->menus[i].items[k]);
        if (ini->menus[i].items) lt_free(ini->menus[i].items);
    }
    if (ini->menus) lt_free(ini->menus);
    for (i = 0; i < ini->n_dialogs; i++) ini_free_dialog(ini->dialogs[i]);
    if (ini->dialogs) lt_free(ini->dialogs);
    for (i = 0; i < ini->n_ind_panels; i++) {
        lt_ini_indicator_panel *ip = ini->ind_panels[i];
        size_t k;
        fr(ip->name); fr(ip->vis_cond);
        for (k = 0; k < ip->n_inds; k++) {
            lt_ini_indicator *in = &ip->inds[k];
            fr(in->expression); fr(in->label_off); fr(in->label_on);
            fr(in->off_fg); fr(in->off_bg); fr(in->on_fg); fr(in->on_bg);
        }
        if (ip->inds) lt_free(ip->inds);
        lt_free(ip);
    }
    if (ini->ind_panels) lt_free(ini->ind_panels);
    for (i = 0; i < ini->n_readout_panels; i++) {
        lt_ini_readout_panel *rp = ini->readout_panels[i];
        size_t k;
        fr(rp->name); fr(rp->vis_cond);
        for (k = 0; k < rp->n_readouts; k++) {
            fr(rp->readouts[k].channel); fr(rp->readouts[k].title); fr(rp->readouts[k].units);
        }
        if (rp->readouts) lt_free(rp->readouts);
        lt_free(rp);
    }
    if (ini->readout_panels) lt_free(ini->readout_panels);
    for (i = 0; i < ini->n_helps; i++) ini_free_help(ini->helps[i]);
    if (ini->helps) lt_free(ini->helps);
    for (i = 0; i < ini->n_setting_groups; i++) {
        lt_ini_setting_group *g = ini->setting_groups[i];
        size_t k;
        fr(g->name); fr(g->label);
        for (k = 0; k < g->n_options; k++) {
            fr(g->options[k].value);
            fr(g->options[k].label);
        }
        if (g->options) lt_free(g->options);
        lt_free(g);
    }
    if (ini->setting_groups) lt_free(ini->setting_groups);
    for (i = 0; i < ini->n_commands; i++) ini_free_cmd(ini->commands[i]);
    if (ini->commands) lt_free(ini->commands);
    for (i = 0; i < ini->n_dl_entries; i++) {
        lt_ini_dl_entry *e = &ini->dl_entries[i];
        fr(e->channel); fr(e->label); fr(e->type); fr(e->format); fr(e->condition);
    }
    if (ini->dl_entries) lt_free(ini->dl_entries);
    for (i = 0; i < ini->n_dl_views; i++) {
        fr(ini->dl_views[i].name);
        fr(ini->dl_views[i].label);
        ini_free_str_array(ini->dl_views[i].channels, ini->dl_views[i].n_channels);
    }
    if (ini->dl_views) lt_free(ini->dl_views);
    for (i = 0; i < ini->n_loggers; i++) ini_free_logger(&ini->loggers[i]);
    if (ini->loggers) lt_free(ini->loggers);
    for (i = 0; i < ini->n_port_editors; i++) {
        lt_ini_port_editor *pe = &ini->port_editors[i];
        fr(pe->name); fr(pe->label); fr(pe->enable_cond); fr(pe->help_topic);
        fr(pe->enabled_const); fr(pe->can_id_const); fr(pe->offset_const); fr(pe->size_const);
        fr(pe->op_const); fr(pe->threshold_const); fr(pe->hysteresis_const);
        fr(pe->power_on_const); fr(pe->trigger_const); fr(pe->connector_const);
        fr(pe->offset2_const); fr(pe->size2_const); fr(pe->op2_const);
        fr(pe->threshold2_const); fr(pe->hysteresis2_const);
        ini_free_str_array(pe->port_names, pe->n_port_names);
        ini_free_str_array(pe->op_labels, pe->n_op_labels);
        ini_free_str_array(pe->conn_labels, pe->n_conn_labels);
    }
    if (ini->port_editors) lt_free(ini->port_editors);
    for (i = 0; i < ini->n_key_actions; i++) {
        fr(ini->key_actions[i].key);
        fr(ini->key_actions[i].action);
        fr(ini->key_actions[i].label);
        fr(ini->key_actions[i].cond);
    }
    if (ini->key_actions) lt_free(ini->key_actions);
    for (i = 0; i < ini->n_ref_tables; i++) ini_free_ref_table(ini->ref_tables[i]);
    if (ini->ref_tables) lt_free(ini->ref_tables);
    if (ini->frontpage) {
        lt_ini_frontpage *fp = ini->frontpage;
        ini_free_str_array(fp->gauges, fp->n_gauges);
        for (i = 0; i < fp->n_inds; i++) {
            lt_ini_fp_indicator *in = &fp->inds[i];
            fr(in->expression); fr(in->label_off); fr(in->label_on);
            fr(in->bg_off); fr(in->fg_off); fr(in->bg_on); fr(in->fg_on);
        }
        if (fp->inds) lt_free(fp->inds);
        lt_free(fp);
    }
    ini_free_analyze(&ini->ve_analyze);
    ini_free_analyze(&ini->wue_analyze);
    ini_free_analyze(&ini->gamma_e);
    for (i = 0; i < ini->default_values.cap; i++)
        if (ini->default_values.slots[i].used == 1) lt_free(ini->default_values.slots[i].val);
    ini_free_str_array(ini->maintain_constants, ini->n_maintain_constants);
    ini_free_str_array(ini->maintain_exprs, ini->n_maintain_constants);
    ini_free_str_array(ini->requires_power_cycle, ini->n_requires_power_cycle);

    lt_map_free(&ini->constant_idx); lt_map_free(&ini->channel_idx);
    lt_map_free(&ini->table_idx); lt_map_free(&ini->curve_idx);
    lt_map_free(&ini->gauge_idx); lt_map_free(&ini->dialog_idx);
    lt_map_free(&ini->help_idx); lt_map_free(&ini->command_idx);
    lt_map_free(&ini->pc_variables); lt_map_free(&ini->default_values);
    lt_map_free(&ini->maximum_elements); lt_map_free(&ini->active_symbols);
    lt_map_free(&ini->tested_symbols);

    if (cx) {
        for (i = 0; i < cx->defines.cap; i++) {
            if (cx->defines.slots[i].used == 1) {
                ini_strlist *l = (ini_strlist *)cx->defines.slots[i].val;
                ini_strv_free(l->v, l->n);
                lt_free(l);
            }
        }
        lt_map_free(&cx->defines);
        lt_map_free(&cx->included);
        lt_map_free(&cx->managed);
        lt_buf_free(&cx->logger_raw);
        for (i = 0; i < cx->std_panels.len; i++)
            ini_free_dialog(LT_VEC_AT(&cx->std_panels, lt_ini_dialog *, i));
        lt_vec_free(&cx->std_panels);
        lt_free(cx);
    }
}

void lt_ini_free(lt_ini *ini)
{
    if (!ini) return;
    ini_free_contents(ini);
    lt_free(ini);
}

/* ------------------------------------------------------------------ */
/* lookups                                                             */
/* ------------------------------------------------------------------ */

const lt_ini_const *lt_ini_find_const(const lt_ini *ini, const char *name)
{
    return (const lt_ini_const *)lt_map_get(&ini->constant_idx, name);
}

const lt_ini_channel *lt_ini_find_channel(const lt_ini *ini, const char *name)
{
    return (const lt_ini_channel *)lt_map_get(&ini->channel_idx, name);
}

const lt_ini_table *lt_ini_find_table(const lt_ini *ini, const char *name)
{
    return (const lt_ini_table *)lt_map_get(&ini->table_idx, name);
}

const lt_ini_curve *lt_ini_find_curve(const lt_ini *ini, const char *name)
{
    return (const lt_ini_curve *)lt_map_get(&ini->curve_idx, name);
}

const lt_ini_gauge *lt_ini_find_gauge(const lt_ini *ini, const char *name)
{
    return (const lt_ini_gauge *)lt_map_get(&ini->gauge_idx, name);
}

const lt_ini_dialog *lt_ini_find_dialog(const lt_ini *ini, const char *name)
{
    return (const lt_ini_dialog *)lt_map_get(&ini->dialog_idx, name);
}

const lt_ini_help *lt_ini_find_help(const lt_ini *ini, const char *name)
{
    return (const lt_ini_help *)lt_map_get(&ini->help_idx, name);
}

const lt_ini_cmd *lt_ini_find_command(const lt_ini *ini, const char *name)
{
    return (const lt_ini_cmd *)lt_map_get(&ini->command_idx, name);
}

const lt_ini_ref_table *lt_ini_find_ref_table(const lt_ini *ini, const char *name)
{
    size_t i;
    if (ini == NULL || name == NULL) return NULL;           /* a dialog left open with no INI */
    for (i = 0; i < ini->n_ref_tables; i++)
        if (strcmp(ini->ref_tables[i]->name, name) == 0) return ini->ref_tables[i];
    return NULL;
}

const lt_ini_indicator_panel *lt_ini_find_ind_panel(const lt_ini *ini, const char *name)
{
    size_t i;
    if (name == NULL) return NULL;
    for (i = 0; i < ini->n_ind_panels; i++)
        if (ini->ind_panels[i]->name != NULL && strcmp(ini->ind_panels[i]->name, name) == 0)
            return ini->ind_panels[i];
    return NULL;
}

const lt_ini_readout_panel *lt_ini_find_readout_panel(const lt_ini *ini, const char *name)
{
    size_t i;
    if (name == NULL) return NULL;
    for (i = 0; i < ini->n_readout_panels; i++)
        if (ini->readout_panels[i]->name != NULL &&
            strcmp(ini->readout_panels[i]->name, name) == 0)
            return ini->readout_panels[i];
    return NULL;
}

const lt_ini_port_editor *lt_ini_find_port_editor(const lt_ini *ini, const char *name)
{
    size_t i;
    if (name == NULL) return ini->n_port_editors > 0 ? &ini->port_editors[0] : NULL;
    for (i = 0; i < ini->n_port_editors; i++)
        if (ini->port_editors[i].name != NULL && strcmp(ini->port_editors[i].name, name) == 0)
            return &ini->port_editors[i];
    return NULL;
}

lt_bool lt_ini_needs_power_cycle(const lt_ini *ini, const char *name)
{
    size_t i;
    if (name == NULL) return LT_FALSE;
    for (i = 0; i < ini->n_requires_power_cycle; i++)
        if (strcmp(ini->requires_power_cycle[i], name) == 0) return LT_TRUE;
    return LT_FALSE;
}

lt_bool lt_ini_setting_is_flag(const lt_ini_setting_group *g)
{
    return g != NULL && g->n_options == 0;
}

int lt_ini_setting_active(const lt_ini *ini, const lt_ini_setting_group *g)
{
    size_t i;
    if (g == NULL) return -1;
    if (g->n_options == 0) return lt_ini_symbol_active(ini, g->name) ? 0 : -1;
    for (i = 0; i < g->n_options; i++) {
        const char *v = g->options[i].value;
        if (v != NULL && v[0] != '\0' && strcmp(v, "DEFAULT") != 0 && lt_ini_symbol_active(ini, v))
            return (int)i;
    }
    return -1;
}

lt_bool lt_ini_symbol_active(const lt_ini *ini, const char *sym)
{
    return lt_map_has(&ini->active_symbols, sym);
}

lt_bool lt_ini_symbol_tested(const lt_ini *ini, const char *sym)
{
    return lt_map_has(&ini->tested_symbols, sym);
}

lt_bool lt_ini_default_value(const lt_ini *ini, const char *name, double *out)
{
    const double *d = (const double *)lt_map_get(&ini->default_values, name);
    if (!d) return LT_FALSE;
    if (out) *out = *d;
    return LT_TRUE;
}

size_t lt_ini_total_memory(const lt_ini *ini)
{
    size_t i, t = 0;
    for (i = 0; i < ini->proto.n_page_sizes; i++) t += ini->proto.page_sizes[i];
    return t;
}

/* ------------------------------------------------------------------ */
/* std panels                                                          */
/* ------------------------------------------------------------------ */

static const struct { const char *name, *label; } std_inj[] = {
    {"algorithm", "Control Algorithm"}, {"reqFuel", "Required Fuel"},
    {"nCylinders", "Number of Cylinders"}, {"twoStroke", "Engine Stroke"},
    {"engineType", "Engine Type"}, {"injType", "Injector Type"},
    {"divider", "Injector Divider"}, {"alternate", "Injection Timing"},
    {"nInjectors", "Number of Injectors"}, {"injOpen", "Injector Open Time"}
};

const lt_ini_dialog *lt_ini_std_panel(lt_ini *ini, const char *name)
{
    ini_ctx *cx = (ini_ctx *)ini->priv;
    size_t i;
    lt_ini_dialog *d;
    const char *title;
    const char *const *names;
    const char *const *labels;
    size_t n;
    static const char *const rtc_n[] = {"rtc_trim"};
    static const char *const rtc_l[] = {"RTC Trim (ppm)"};
    const char *inj_n[10], *inj_l[10];

    for (i = 0; i < cx->std_panels.len; i++) {
        d = LT_VEC_AT(&cx->std_panels, lt_ini_dialog *, i);
        if (strcmp(d->name, name) == 0) return d;
    }
    if (strcmp(name, "std_injection") == 0) {
        for (i = 0; i < 10; i++) {
            inj_n[i] = std_inj[i].name;
            inj_l[i] = std_inj[i].label;
        }
        names = inj_n;
        labels = inj_l;
        n = 10;
        title = "Injection Setup";
    } else if (strcmp(name, "std_ms3Rtc") == 0) {
        names = rtc_n;
        labels = rtc_l;
        n = 1;
        title = "Real Time Clock";
    } else {
        return NULL;
    }
    d = (lt_ini_dialog *)lt_calloc(1, sizeof(lt_ini_dialog));
    d->name = lt_strdup(name);
    d->title = lt_strdup(title);
    for (i = 0; i < n; i++) {
        if (lt_ini_find_const(ini, names[i])) {
            lt_ini_comp *c = (lt_ini_comp *)ini_push((void **)&d->comps, &d->n_comps,
                                                     sizeof(lt_ini_comp));
            c->kind = LT_COMP_FIELD;
            c->label = lt_strdup(labels[i]);
            c->name = lt_strdup(names[i]);
        }
    }
    if (d->n_comps == 0) {
        ini_free_dialog(d);
        return NULL;
    }
    *(lt_ini_dialog **)lt_vec_push(&cx->std_panels) = d;
    return d;
}

/* ------------------------------------------------------------------ */
/* post-processing                                                     */
/* ------------------------------------------------------------------ */

static lt_bool has_dollar(const lt_ini_blob *b)
{
    size_t i;
    for (i = 0; i < b->n; i++)
        if (b->p[i] == '$') return LT_TRUE;
    return LT_FALSE;
}

static void subst_blob(lt_ini_blob *b, const lt_map *pcv)
{
    lt_ini_blob nb;
    if (!has_dollar(b)) return;
    nb = ini_substitute(b->p, b->n, pcv);
    ini_blob_free(b);
    *b = nb;
}

static void subst_list(lt_ini_cmdlist *l, const lt_map *pcv)
{
    size_t i;
    for (i = 0; i < l->n; i++) subst_blob(&l->v[i], pcv);
}

static void post_substitution(lt_ini *ini)
{
    lt_ini_protocol *p = &ini->proto;
    size_t i;

    ini_resolve_pcvars(ini);
    if (ini->pc_variables.len > 0) {
        subst_blob(&p->query_command, &ini->pc_variables);
        subst_list(&p->page_read_commands, &ini->pc_variables);
        subst_list(&p->page_chunk_write_commands, &ini->pc_variables);
        subst_list(&p->page_value_write_commands, &ini->pc_variables);
        subst_list(&p->burn_commands, &ini->pc_variables);
        subst_list(&p->crc32_check_commands, &ini->pc_variables);
        subst_list(&p->page_activate_commands, &ini->pc_variables);
        subst_blob(&p->och_get_command, &ini->pc_variables);
        subst_blob(&p->burst_get_command, &ini->pc_variables);
    }
    /* page identifiers: substitute + decode escapes */
    ini_free_blob_array(p->page_identifiers, p->n_page_identifiers);
    p->n_page_identifiers = p->n_page_identifiers_raw;
    p->page_identifiers = NULL;
    if (p->n_page_identifiers) {
        p->page_identifiers = (lt_ini_blob *)lt_calloc(p->n_page_identifiers,
                                                       sizeof(lt_ini_blob));
        for (i = 0; i < p->n_page_identifiers; i++) {
            lt_ini_blob s = ini_substitute(p->page_identifiers_raw[i],
                                           strlen(p->page_identifiers_raw[i]),
                                           &ini->pc_variables);
            p->page_identifiers[i] = ini_parse_escape(s.p, s.n);
            ini_blob_free(&s);
        }
    }
}

static void retype_items(const lt_ini *ini, lt_ini_menu_item **items, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        lt_ini_menu_item *m = items[i];
        if (m->kind == LT_MENU_DIALOG && m->target) {
            if (lt_map_has(&ini->help_idx, m->target)) m->kind = LT_MENU_HELP;
            else if (lt_map_has(&ini->table_idx, m->target) ||
                     lt_map_has(&ini->curve_idx, m->target))
                m->kind = LT_MENU_TABLE;
        }
        if (m->kind == LT_MENU_SUBMENU) retype_items(ini, m->items, m->n_items);
    }
}

static long round_l(double d)
{
    d = lt_round(d);
    if (!(d > -32767.0)) return d != d ? 0 : -32767L; /* NaN -> 0; keeps products inside 32-bit long */
    if (d > 32767.0) return 32767L;
    return (long)d;
}

static void post_dynamic_shapes(lt_ini *ini)
{
    size_t i;
    for (i = 0; i < ini->n_constants; i++) {
        lt_ini_const *c = ini->constants[i];
        if (!c->dyn) continue;
        if (c->dyn->cols_const) {
            double d;
            long def_cols = 0, def_rows = 0, max_el;
            void *mx = lt_map_get(&ini->maximum_elements, c->name);
            size_t rows, cols;
            if (lt_ini_default_value(ini, c->dyn->cols_const, &d)) def_cols = round_l(d);
            if (lt_ini_default_value(ini, c->dyn->rows_const, &d)) def_rows = round_l(d);
            max_el = mx ? (long)(size_t)mx : def_cols * def_rows;
            if (def_rows > 0 && def_cols > 0 && def_rows * def_cols == max_el) {
                rows = (size_t)def_rows;
                cols = (size_t)def_cols;
            } else if (def_cols > 0 && max_el > 0 && max_el % def_cols == 0) {
                rows = (size_t)(max_el / def_cols);
                cols = (size_t)def_cols;
            } else if (max_el > 0) {
                rows = (size_t)max_el;
                cols = 1;
            } else {
                rows = def_rows > 1 ? (size_t)def_rows : 1;
                cols = def_cols > 1 ? (size_t)def_cols : 1;
            }
            c->shape.kind = LT_SH_2D;
            c->shape.rows = rows;
            c->shape.cols = cols;
        } else {
            const lt_ini_const *cnt = lt_ini_find_const(ini, c->dyn->rows_const);
            long alloc = 0;
            double d;
            if (cnt && round_l(cnt->max) > 0) alloc = round_l(cnt->max);
            else if (lt_ini_default_value(ini, c->dyn->rows_const, &d)) alloc = round_l(d);
            if (alloc < 1) alloc = 1;
            c->shape.kind = LT_SH_1D;
            c->shape.n = (size_t)alloc;
        }
        c->size_bytes = lt_dtype_size(c->type) * lt_shape_count(&c->shape);
        if (c->type == LT_DT_STRING) c->size_bytes = lt_shape_count(&c->shape);
    }
}

static void post_table_sizes(lt_ini *ini)
{
    size_t i;
    for (i = 0; i < ini->n_tables; i++) {
        lt_ini_table *t = ini->tables[i];
        const lt_ini_const *xb = t->x_const ? lt_ini_find_const(ini, t->x_const) : NULL;
        const lt_ini_const *yb = t->y_const ? lt_ini_find_const(ini, t->y_const) : NULL;
        const lt_ini_const *mp = t->z_const ? lt_ini_find_const(ini, t->z_const) : NULL;
        size_t xs = xb ? lt_shape_x_size(&xb->shape) : 0;
        size_t ys = (t->y_const && yb) ? lt_shape_x_size(&yb->shape) : 1;
        size_t fx = 0, fy = 0;
        int have_dyn = 0;

        if (mp && mp->dyn && mp->dyn->cols_const) {
            double dc, dr;
            if (lt_ini_default_value(ini, mp->dyn->cols_const, &dc) &&
                lt_ini_default_value(ini, mp->dyn->rows_const, &dr) &&
                round_l(dc) > 0 && round_l(dr) > 0) {
                fx = (size_t)round_l(dc);
                fy = (size_t)round_l(dr);
                have_dyn = 1;
            }
        }
        if (!have_dyn) {
            if (mp) {
                if (mp->shape.kind == LT_SH_2D) {
                    fx = mp->shape.cols;
                    fy = mp->shape.rows;
                } else if (mp->shape.kind == LT_SH_1D) {
                    fx = xs > 0 ? xs : mp->shape.n;
                    fy = 1;
                } else {
                    fx = xs;
                    fy = ys;
                }
            } else {
                fx = xs;
                fy = ys;
            }
        }
        if (t->x_size == 0 && fx > 0) t->x_size = fx;
        if (t->y_size <= 1 && fy > 1) t->y_size = fy;
        if (mp && mp->dyn) {
            if (mp->dyn->rows_const) ini_set_str(&t->rows_size_const, lt_strdup(mp->dyn->rows_const));
            if (mp->dyn->cols_const) ini_set_str(&t->cols_size_const, lt_strdup(mp->dyn->cols_const));
            {
                void *mx = lt_map_get(&ini->maximum_elements, mp->name);
                if (mx) t->max_elements = (size_t)mx;
            }
        }
    }
}

/* ---- table roles ---- */
static lt_bool split_digit_run(const char *s, size_t *pre, size_t *dig_start, size_t *dig_end)
{
    size_t n = strlen(s), i = n;
    while (i > 0 && !(s[i - 1] >= '0' && s[i - 1] <= '9')) i--;
    if (i == 0) return LT_FALSE;
    *dig_end = i;
    while (i > 0 && s[i - 1] >= '0' && s[i - 1] <= '9') i--;
    *dig_start = i;
    *pre = i;
    return LT_TRUE;
}

static lt_bool digit_sibling(const char *a, const char *b)
{
    size_t pa, sa, ea, pb, sb, eb;
    if (!split_digit_run(a, &pa, &sa, &ea) || !split_digit_run(b, &pb, &sb, &eb)) return LT_FALSE;
    if (sa != sb) return LT_FALSE;
    if (strncmp(a, b, sa) != 0) return LT_FALSE;
    if (strcmp(a + ea, b + eb) != 0) return LT_FALSE;
    if (ea - sa == eb - sb && strncmp(a + sa, b + sb, ea - sa) == 0) return LT_FALSE;
    return LT_TRUE;
}

static lt_bool role_match(const lt_ini_table *t, const char *cand)
{
    if (!cand || !cand[0]) return LT_FALSE;
    if (strcmp(t->name, cand) == 0 || strcmp(t->map_name, cand) == 0) return LT_TRUE;
    return digit_sibling(cand, t->name) || digit_sibling(cand, t->map_name);
}

static lt_bool role_in(const lt_ini_table *t, char **c, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++)
        if (role_match(t, c[i])) return LT_TRUE;
    return LT_FALSE;
}

void lt_ini_infer_table_roles(lt_ini *ini)
{
    size_t i;
    char **afr = NULL, **ve = NULL, **wue = NULL;
    size_t nafr = 0, nve = 0, nwue = 0;

    if (ini->ve_analyze.ve_table) ini_str_push(&ve, &nve, lt_strdup(ini->ve_analyze.ve_table));
    if (ini->ve_analyze.target_table)
        ini_str_push(&afr, &nafr, lt_strdup(ini->ve_analyze.target_table));
    for (i = 0; i < ini->ve_analyze.n_lambda_target_tables; i++)
        ini_str_push(&afr, &nafr, lt_strdup(ini->ve_analyze.lambda_target_tables[i]));
    if (ini->wue_analyze.target_table)
        ini_str_push(&afr, &nafr, lt_strdup(ini->wue_analyze.target_table));
    for (i = 0; i < ini->wue_analyze.n_lambda_target_tables; i++)
        ini_str_push(&afr, &nafr, lt_strdup(ini->wue_analyze.lambda_target_tables[i]));
    if (ini->wue_analyze.wue_curve) ini_str_push(&wue, &nwue, lt_strdup(ini->wue_analyze.wue_curve));

    for (i = 0; i < ini->n_tables; i++) {
        lt_ini_table *t = ini->tables[i];
        char *lname = lt_strdup(t->name);
        size_t k;
        for (k = 0; lname[k]; k++)
            if (lname[k] >= 'A' && lname[k] <= 'Z') lname[k] = (char)(lname[k] - 'A' + 'a');
        if (role_in(t, ve, nve)) t->role = LT_ROLE_VE;
        else if (role_in(t, afr, nafr)) t->role = LT_ROLE_AFR_TARGET;
        else if (role_in(t, wue, nwue)) t->role = LT_ROLE_WARMUP;
        else if (strstr(lname, "ign") || strstr(lname, "spark") || strstr(lname, "advance"))
            t->role = LT_ROLE_IGNITION;
        else t->role = LT_ROLE_OTHER;
        lt_free(lname);
    }
    ini_strv_free(ve, nve);
    ini_strv_free(afr, nafr);
    ini_strv_free(wue, nwue);
}

void ini_post_process(lt_ini *ini, const char *filename_hint)
{
    size_t i;
    ini_ctx *cx = (ini_ctx *)ini->priv;

    post_substitution(ini);
    for (i = 0; i < ini->n_menus; i++)
        retype_items(ini, ini->menus[i].items, ini->menus[i].n_items);
    for (i = 0; i < ini->n_constants; i++) ini->constants[i]->order = i;
    for (i = 0; i < ini->n_channels; i++) ini->channels[i]->index = i;
    post_dynamic_shapes(ini);
    post_table_sizes(ini);
    if (ini->n_pages < ini->proto.n_page_sizes) ini->n_pages = ini->proto.n_page_sizes;
    ini->ecu_type = lt_ecu_detect(ini->signature, filename_hint);
    lt_ini_infer_table_roles(ini);
    if (cx->logger_raw.len > 0) ini_parse_loggers(ini, cx->logger_raw.data);
}
