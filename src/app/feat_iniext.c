/* feat_iniext.c - INI driven extras: help topic viewer, pin conflict report + burn gate,
 * "power cycle the ECU" notice after burning requiresPowerCycle constants, project INI
 * settings ([SettingGroups] symbols, stored as TunerStudio ecuSettings), and the port editor
 * tab registration.  See iniext.h.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <string.h>
#include <ctype.h>
#include "feat.h"
#include "iniext.h"

#define PC_MAX 256

typedef struct ix_state {
    lt_u32 ack_hash; int has_ack;            /* acknowledged pin conflict set */
    lt_u32 pc_hash[PC_MAX]; int pc_n; int pc_have;   /* power cycle snapshot */
    char pc_pending[600]; int pc_open;
    int reload_pending;
} ix_state;

static ix_state *ix(lt_app *app)
{
    return (ix_state *)lt_ext_state(app, "iniext", sizeof(ix_state), NULL);
}

/* ================================================================== */
/* help topics                                                         */
static int is_url(const char *s)
{
    return s != NULL && (strstr(s, "://") != NULL || lt_str_istarts(s, "www."));
}

int lt_help_resolve(const lt_ini *ini, const char *topic, lt_help_view *out)
{
    const lt_ini_help *h;
    memset(out, 0, sizeof(*out));
    if (topic == NULL || topic[0] == '\0') { return LT_HELP_NONE; }
    h = ini != NULL ? lt_ini_find_help(ini, topic) : NULL;
    if (h != NULL) {
        out->kind = LT_HELP_INI;
        out->help = h;
        out->title = h->title != NULL ? h->title : h->name;
        out->url = h->web_url != NULL && h->web_url[0] != '\0' ? h->web_url : NULL;
        return out->kind;
    }
    if (is_url(topic)) {
        out->kind = LT_HELP_URL;
        out->title = topic;
        out->url = topic;
        return out->kind;
    }
    return LT_HELP_NONE;
}

/* append text with simple HTML handling */
static void html_plain(lt_buf *b, const char *s)
{
    while (*s != '\0') {
        if (*s == '<') {
            const char *e = strchr(s, '>');
            if (e != NULL) {
                char tag[16];
                size_t k = 0;
                const char *q = s + 1;
                if (*q == '/') { q++; }
                while (q < e && k + 1 < sizeof(tag) && isalpha((unsigned char)*q)) {
                    tag[k++] = (char)tolower((unsigned char)*q); q++;
                }
                tag[k] = '\0';
                if (strcmp(tag, "br") == 0 || strcmp(tag, "p") == 0 || strcmp(tag, "li") == 0 ||
                    strcmp(tag, "div") == 0 || (tag[0] == 'h' && tag[1] >= '1' && tag[1] <= '6')) {
                    lt_buf_putc(b, '\n');
                    if (strcmp(tag, "li") == 0 && s[1] != '/') { lt_buf_puts(b, "  - "); }
                }
                s = e + 1;
                continue;
            }
        }
        if (*s == '&') {
            static const char *const ent[][2] = {
                { "&amp;", "&" }, { "&lt;", "<" }, { "&gt;", ">" }, { "&quot;", "\"" },
                { "&nbsp;", " " }, { "&deg;", "deg" }, { "&#39;", "'" }
            };
            size_t i;
            int hit = 0;
            for (i = 0; i < sizeof(ent) / sizeof(ent[0]); i++) {
                size_t n = strlen(ent[i][0]);
                if (strncmp(s, ent[i][0], n) == 0) { lt_buf_puts(b, ent[i][1]); s += n; hit = 1; break; }
            }
            if (hit) { continue; }
        }
        lt_buf_putc(b, (unsigned char)*s);
        s++;
    }
}

char *lt_help_plain_text(const lt_ini_help *h)
{
    lt_buf b;
    size_t i;
    lt_buf_init(&b);
    for (i = 0; h != NULL && i < h->n_lines; i++) {
        if (h->lines[i] == NULL) { continue; }
        html_plain(&b, h->lines[i]);
        lt_buf_putc(&b, '\n');
    }
    if (b.data == NULL) { return lt_strdup(""); }
    return lt_buf_take(&b);
}

typedef struct help_dlg { char *text; char *url; char title[96]; } help_dlg;

int lt_app_help_open(lt_app *app, const char *topic)
{
    lt_help_view hv;
    if (lt_help_resolve(app->ini, topic, &hv) == LT_HELP_NONE) {
        lt_app_toast(app, 2, "No help available for %s", topic != NULL ? topic : "?");
        return -1;
    }
    return lt_app_dialog_push(app, "ini-help", topic, hv.title != NULL ? hv.title : "Help") != NULL ? 0 : -1;
}

int lt_app_help_text(lt_app *app, const char *title, const char *text)
{
    lt_app_dialog *d = lt_app_dialog_push(app, "ini-help", "", title != NULL ? title : "Help");
    help_dlg *h;
    if (d == NULL) { return -1; }
    h = (help_dlg *)lt_calloc(1, sizeof(*h));
    h->text = lt_strdup(text != NULL ? text : "");
    lt_strlcpy(h->title, title != NULL ? title : "Help", sizeof(h->title));
    d->data = h;
    return 0;
}

static void help_draw(lt_app *app, lt_app_dialog *d)
{
    lt_ui *ui = app->ui;
    const lt_theme *t = lt_ui_theme(ui);
    help_dlg *h = (help_dlg *)d->data;
    int open = d->open, w1 = -1, w3[3], fh = lt_ui_cell_mode(ui) ? 16 : 28;
    if (h == NULL) {
        lt_help_view hv;
        h = (help_dlg *)lt_calloc(1, sizeof(*h));
        d->data = h;
        switch (lt_help_resolve(app->ini, d->target, &hv)) {
        case LT_HELP_INI:
            h->text = lt_help_plain_text(hv.help);
            break;
        case LT_HELP_URL:
            h->text = lt_strdup("This help topic is a web page. Open it in your browser:");
            break;
        default:
            h->text = lt_strdup(d->target[0] != '\0' ? "This help topic is not defined in the loaded ECU definition."
                                                    : "No help text.");
            break;
        }
        h->url = hv.url != NULL ? lt_strdup(hv.url) : NULL;
        lt_strlcpy(h->title, hv.title != NULL ? hv.title : (d->title[0] != '\0' ? d->title : "Help"), sizeof(h->title));
    }
    if (lt_ui_modal_begin(ui, h->title, 620, 440, &open)) {
        lt_ui_layout_row(ui, 1, &w1, -(fh + 10 + (h->url != NULL ? fh : 0)));
        if (lt_ui_scroll_begin(ui, "helptext")) {
            const char *p = h->text;
            char para[1200];
            while (*p != '\0') {
                size_t k = 0;
                while (*p != '\0' && *p != '\n' && k + 1 < sizeof(para)) { para[k++] = *p++; }
                para[k] = '\0';
                if (*p == '\n') { p++; }
                lt_ui_layout_row(ui, 1, &w1, 0);
                if (para[0] == '\0') { lt_ui_layout_space(ui, 4); }
                else { lt_ui_label_wrap(ui, para, t->text); }
            }
            lt_ui_scroll_end(ui);
        }
        if (h->url != NULL) {
            lt_ui_layout_row(ui, 1, &w1, 0);
            lt_ui_label_ex(ui, h->url, t->link, LT_UI_ALIGN_LEFT);
        }
        w3[0] = -1; w3[1] = 160; w3[2] = 90;
        lt_ui_layout_row(ui, 3, w3, fh);
        lt_ui_label(ui, "");
        if (h->url != NULL) {
            if (lt_ui_button(ui, "Open in Browser")) {
                if (lt_plat_open_url(h->url) != 0) { lt_app_toast(app, 2, "Cannot open %s", h->url); }
            }
        } else { lt_ui_label(ui, ""); }
        if (lt_ui_button_ex(ui, "Close", LT_UI_BTN_DEFAULT)) { open = 0; }
        lt_ui_modal_end(ui);
    }
    d->open = open;
}

static void help_close(lt_app *app, lt_app_dialog *d)
{
    help_dlg *h = (help_dlg *)d->data;
    LT_UNUSED(app);
    if (h != NULL) { lt_free(h->text); lt_free(h->url); lt_free(h); }
    d->data = NULL;
}

/* ================================================================== */
/* pin conflicts                                                       */
static int pinlike_label(const char *s)
{
    size_t n, i;
    if (s == NULL) { return 0; }
    n = strlen(s);
    /* PA0, PD13, E12 style port pins */
    if (n >= 2 && n <= 6 && isalpha((unsigned char)s[0])) {
        int digits = 0, ok = 1;
        for (i = 1; i < n; i++) {
            if (isdigit((unsigned char)s[i])) { digits = 1; }
            else if (!isalpha((unsigned char)s[i]) || digits) { ok = 0; }
        }
        if (ok && digits) { return 1; }
    }
    for (i = 0; i < n; i++) {
        if (isdigit((unsigned char)s[i])) { return 1; }   /* "Output 3", "Injector 1", "Pin 22", "30" */
    }
    return 0;
}

static int name_has_pin(const char *s)
{
    size_t i;
    for (i = 0; s != NULL && s[i] != '\0' && s[i + 1] != '\0' && s[i + 2] != '\0'; i++) {
        if (tolower((unsigned char)s[i]) == 'p' && tolower((unsigned char)s[i + 1]) == 'i' &&
            tolower((unsigned char)s[i + 2]) == 'n') { return 1; }
    }
    return 0;
}

int lt_app_pin_conflicts(lt_app *app, lt_pin_report *out)
{
    lt_pagesrc ps;
    struct lt_pin_pages pp;
    lt_pin_report r;
    size_t i, j, keep = 0;
    if (out != NULL) { out->c = NULL; out->n = 0; }
    if (app->ini == NULL) { return 0; }
    ps = lt_app_pages(app);
    pp.ini = app->ini; pp.src = &ps;
    if (lt_pin_detect(app->ini, lt_pin_value_from_pages, &pp, &r) < 0) { return 0; }
    for (i = 0; i < r.n; i++) {
        int real = pinlike_label(r.c[i].pin_label), named = 1;
        for (j = 0; j < r.c[i].n; j++) { if (!name_has_pin(r.c[i].constants[j])) { named = 0; } }
        if (real || named) {
            if (keep != i) { r.c[keep] = r.c[i]; memset(&r.c[i], 0, sizeof(r.c[i])); }
            keep++;
        } else {
            lt_pin_conflict_free(&r.c[i]);
            memset(&r.c[i], 0, sizeof(r.c[i]));
        }
    }
    r.n = keep;
    if (out != NULL) { *out = r; } else { lt_pin_report_free(&r); }
    return (int)keep;
}

static lt_u32 report_hash(const lt_pin_report *r)
{
    lt_u32 h = 2166136261UL;
    size_t i, j;
    const char *s;
    for (i = 0; i < r->n; i++) {
        for (s = r->c[i].pin_label != NULL ? r->c[i].pin_label : ""; *s != '\0'; s++) { h = (h ^ (unsigned char)*s) * 16777619UL; }
        for (j = 0; j < r->c[i].n; j++) {
            h = (h ^ 0x1FU) * 16777619UL;
            for (s = r->c[i].constants[j]; *s != '\0'; s++) { h = (h ^ (unsigned char)*s) * 16777619UL; }
        }
        h &= 0xFFFFFFFFUL;
    }
    return h & 0xFFFFFFFFUL;
}

void lt_app_pin_ack(lt_app *app)
{
    lt_pin_report r;
    ix_state *s = ix(app);
    (void)lt_app_pin_conflicts(app, &r);
    s->ack_hash = report_hash(&r);
    s->has_ack = 1;
    lt_pin_report_free(&r);
}

lt_bool lt_app_burn_gate(lt_app *app)
{
    lt_pin_report r;
    ix_state *s = ix(app);
    int n, i;
    lt_bool ok;
    n = lt_app_pin_conflicts(app, &r);
    ok = n == 0 || (s->has_ack && s->ack_hash == report_hash(&r)) ? LT_TRUE : LT_FALSE;
    lt_pin_report_free(&r);
    if (ok) { return LT_TRUE; }
    for (i = 0; i < app->n_dialogs; i++) {
        if (strcmp(app->dialogs[i]->kind, "pin-report") == 0 && app->dialogs[i]->open) { return LT_FALSE; }
    }
    (void)lt_app_dialog_push(app, "pin-report", "burn", "Pin Conflicts");
    lt_app_toast(app, 2, "Burn held back: %d pin conflict%s need%s your confirmation", n, n == 1 ? "" : "s",
                 n == 1 ? "s" : "");
    return LT_FALSE;
}

void lt_app_pin_report_open(lt_app *app)
{
    if (app->ini == NULL) { return; }
    (void)lt_app_dialog_push(app, "pin-report", "", "Pin Conflict Report");
}

static void pins_draw(lt_app *app, lt_app_dialog *d)
{
    lt_ui *ui = app->ui;
    const lt_theme *t = lt_ui_theme(ui);
    int open = d->open, w1 = -1, w3[3], fh = lt_ui_cell_mode(ui) ? 16 : 28, burn = strcmp(d->target, "burn") == 0;
    lt_pin_report r;
    size_t i, j;
    int n = lt_app_pin_conflicts(app, &r);
    if (lt_ui_modal_begin(ui, burn ? "Pin Conflicts - Confirm Burn" : "Pin Conflict Report", 640, 440, &open)) {
        lt_ui_layout_row(ui, 1, &w1, 0);
        if (n == 0) {
            lt_ui_label_wrap(ui, "No pin conflicts: every output and input pin is assigned to one function only.",
                             t->success);
        } else {
            char line[160];
            lt_snprintf(line, sizeof(line), "%d pin%s %s assigned to more than one function:", n, n == 1 ? "" : "s",
                        n == 1 ? "is" : "are");
            lt_ui_label_wrap(ui, line, t->warning);
        }
        lt_ui_layout_row(ui, 1, &w1, -(fh * 2 + 18));
        if (lt_ui_scroll_begin(ui, "pins")) {
            for (i = 0; i < r.n; i++) {
                lt_buf b;
                char *s;
                lt_buf_init(&b);
                lt_buf_puts(&b, r.c[i].pin_label != NULL ? r.c[i].pin_label : "?");
                lt_buf_puts(&b, ": ");
                for (j = 0; j < r.c[i].n; j++) {
                    if (j > 0) { lt_buf_puts(&b, ", "); }
                    lt_buf_puts(&b, r.c[i].constants[j]);
                }
                s = lt_buf_take(&b);
                lt_ui_layout_row(ui, 1, &w1, 0);
                lt_ui_label_wrap(ui, s, t->text);
                lt_free(s);
            }
            lt_ui_scroll_end(ui);
        }
        lt_ui_layout_row(ui, 1, &w1, 0);
        lt_ui_label_wrap(ui, burn ? "Reassign one of the functions, or burn anyway if the shared pin is intended."
                                  : "Reassign one of the functions before burning. Burning with conflicts asks for "
                                    "confirmation.", t->muted);
        w3[0] = -1; w3[1] = 130; w3[2] = 90;
        lt_ui_layout_row(ui, 3, w3, fh);
        lt_ui_label(ui, "");
        if (burn && n > 0) {
            if (lt_ui_button_ex(ui, "Burn Anyway", LT_UI_BTN_DANGER)) {
                lt_app_pin_ack(app);
                open = 0;
                (void)lt_app_burn(app);
            }
        } else if (burn) {
            if (lt_ui_button_ex(ui, "Burn", LT_UI_BTN_ACCENT)) { open = 0; (void)lt_app_burn(app); }
        } else { lt_ui_label(ui, ""); }
        if (lt_ui_button_ex(ui, burn ? "Cancel" : "Close", LT_UI_BTN_DEFAULT)) { open = 0; }
        lt_ui_modal_end(ui);
    }
    lt_pin_report_free(&r);
    d->open = open;
}

static void pin_menu(lt_app *app) { lt_app_pin_report_open(app); }

/* ================================================================== */
/* requires power cycle                                                */
static lt_u32 const_hash(lt_app *app, const lt_ini_const *c)
{
    lt_pagesrc ps = lt_app_pages(app);
    const lt_u8 *pg;
    size_t len = 0, n, i;
    lt_u32 h = 2166136261UL;
    if (c->is_pc_var || ps.page == NULL) { return 0; }
    pg = ps.page(ps.ud, (int)c->page, &len);
    if (pg == NULL) { return 0; }
    n = c->size_bytes;
    if (c->type == LT_DT_BITS || n == 0) { n = (size_t)((c->bit_pos + c->bit_size + 7) / 8); if (n == 0) { n = 1; } }
    for (i = 0; i < n && c->offset + i < len; i++) { h = ((h ^ pg[c->offset + i]) * 16777619UL) & 0xFFFFFFFFUL; }
    return h;
}

void lt_app_pc_snapshot(lt_app *app)
{
    ix_state *s = ix(app);
    size_t i;
    s->pc_n = 0;
    s->pc_have = 0;
    if (app->ini == NULL) { return; }
    for (i = 0; i < app->ini->n_requires_power_cycle && i < PC_MAX; i++) {
        const lt_ini_const *c = lt_ini_find_const(app->ini, app->ini->requires_power_cycle[i]);
        s->pc_hash[i] = c != NULL ? const_hash(app, c) : 0;
        s->pc_n++;
    }
    s->pc_have = 1;
}

int lt_app_pc_after_burn(lt_app *app)
{
    ix_state *s = ix(app);
    size_t i;
    int changed = 0;
    if (app->ini == NULL) { return 0; }
    if (!s->pc_have || (size_t)s->pc_n != (app->ini->n_requires_power_cycle < PC_MAX ? app->ini->n_requires_power_cycle : PC_MAX)) {
        lt_app_pc_snapshot(app);
        return 0;
    }
    s->pc_pending[0] = '\0';
    for (i = 0; i < (size_t)s->pc_n; i++) {
        const lt_ini_const *c = lt_ini_find_const(app->ini, app->ini->requires_power_cycle[i]);
        if (c != NULL && const_hash(app, c) != s->pc_hash[i]) {
            size_t k = strlen(s->pc_pending);
            if (k + 4 < sizeof(s->pc_pending)) {
                lt_snprintf(s->pc_pending + k, sizeof(s->pc_pending) - k, "%s%s", changed ? ", " : "", c->name);
            }
            changed++;
        }
    }
    lt_app_pc_snapshot(app);
    if (changed > 0) { s->pc_open = 1; }
    return changed;
}

const char *lt_app_pc_pending(lt_app *app)
{
    ix_state *s = ix(app);
    return s->pc_open ? s->pc_pending : "";
}

static void pc_overlay(lt_app *app)
{
    ix_state *s = ix(app);
    char msg[800];
    if (!s->pc_open || app->ui == NULL) { return; }
    lt_snprintf(msg, sizeof(msg), "These settings only take effect after the ECU is power cycled (switch the "
                "ignition off, wait a few seconds and on again):\n\n%s", s->pc_pending);
    (void)lt_ui_msgbox(app->ui, "pcycle", "Power Cycle Required", msg, "OK", &s->pc_open);
}

static void ix_on_connect(lt_app *app) { lt_app_pc_snapshot(app); }

/* ================================================================== */
/* project INI settings                                                */
void lt_proj_symbols_init(lt_proj_symbols *s) { memset(s, 0, sizeof(*s)); }

void lt_proj_symbols_free(lt_proj_symbols *s)
{
    size_t i;
    for (i = 0; i < s->n_on; i++) { lt_free(s->on[i]); }
    for (i = 0; i < s->n_managed; i++) { lt_free(s->managed[i]); }
    lt_free(s->on); lt_free(s->managed);
    memset(s, 0, sizeof(*s));
}

static void sym_add(char ***v, size_t *n, const char *s, size_t len)
{
    size_t i;
    char *t = (char *)lt_malloc(len + 1);
    memcpy(t, s, len);
    t[len] = '\0';
    lt_str_trim(t);
    if (t[0] == '\0') { lt_free(t); return; }
    for (i = 0; i < *n; i++) { if (strcmp((*v)[i], t) == 0) { lt_free(t); return; } }
    *v = (char **)lt_realloc(*v, (*n + 1) * sizeof(char *));
    (*v)[(*n)++] = t;
}

static void sym_split(const char *s, char ***v, size_t *n)
{
    const char *p = s;
    while (p != NULL && *p != '\0') {
        const char *e = strchr(p, '|');
        size_t len = e != NULL ? (size_t)(e - p) : strlen(p);
        sym_add(v, n, p, len);
        p = e != NULL ? e + 1 : NULL;
    }
}

static char *props_path(const char *dir)
{
    char *a = lt_path_join(dir, "projectCfg"), *b = lt_path_join(a, "project.properties");
    lt_free(a);
    return b;
}

int lt_proj_symbols_load(const char *project_dir, lt_proj_symbols *s)
{
    lt_props pr;
    char *path;
    const char *v;
    lt_proj_symbols_free(s);
    if (project_dir == NULL) { return 0; }
    path = props_path(project_dir);
    lt_props_init(&pr);
    if (lt_file_exists(path) && lt_props_load(&pr, path) == 0) {
        v = lt_props_get(&pr, "ecuSettings");
        if (v != NULL) { s->present = 1; sym_split(v, &s->on, &s->n_on); }
        v = lt_props_get(&pr, "ltManagedSettings");
        if (v != NULL) { sym_split(v, &s->managed, &s->n_managed); }
    }
    lt_props_free(&pr);
    lt_free(path);
    return 0;
}

static char *sym_join(char **v, size_t n)
{
    lt_buf b;
    size_t i;
    lt_buf_init(&b);
    for (i = 0; i < n; i++) {
        if (i > 0) { lt_buf_putc(&b, '|'); }
        lt_buf_puts(&b, v[i]);
    }
    return b.data != NULL ? lt_buf_take(&b) : lt_strdup("");
}

int lt_proj_symbols_save(const char *project_dir, const lt_proj_symbols *s)
{
    lt_props pr;
    char *path, *dir, *j;
    int rc;
    path = props_path(project_dir);
    dir = lt_path_join(project_dir, "projectCfg");
    (void)lt_plat_mkdirs(dir);
    lt_free(dir);
    lt_props_init(&pr);
    if (lt_file_exists(path)) { (void)lt_props_load(&pr, path); }
    j = sym_join(s->on, s->n_on);
    lt_props_set(&pr, "ecuSettings", j);
    lt_free(j);
    j = sym_join(s->managed, s->n_managed);
    lt_props_set(&pr, "ltManagedSettings", j);
    lt_free(j);
    rc = lt_props_save(&pr, path);
    lt_props_free(&pr);
    lt_free(path);
    return rc;
}

void lt_proj_symbols_options(const lt_proj_symbols *s, lt_ini_options *o)
{
    lt_ini_options_default(o);
    o->symbols = (const char *const *)s->on;
    o->n_symbols = s->n_on;
    o->managed = (const char *const *)s->managed;
    o->n_managed = s->n_managed;
}

static int is_default(const char *v)
{
    return v == NULL || v[0] == '\0' || strcmp(v, "DEFAULT") == 0;
}

void lt_proj_symbols_from_choice(const lt_ini *ini, const int *sel, lt_proj_symbols *out)
{
    size_t i, k;
    lt_proj_symbols_free(out);
    out->present = 1;
    for (i = 0; i < ini->n_setting_groups; i++) {
        const lt_ini_setting_group *g = ini->setting_groups[i];
        if (g->name == NULL) { continue; }
        if (lt_ini_setting_is_flag(g)) {
            sym_add(&out->managed, &out->n_managed, g->name, strlen(g->name));
            if (sel[i] > 0) { sym_add(&out->on, &out->n_on, g->name, strlen(g->name)); }
            continue;
        }
        for (k = 0; k < g->n_options; k++) {
            const char *v = g->options[k].value;
            if (is_default(v)) { continue; }
            sym_add(&out->managed, &out->n_managed, v, strlen(v));
            if (sel[i] == (int)k) { sym_add(&out->on, &out->n_on, v, strlen(v)); }
        }
    }
}

static const char *opt_label(void *ud, int i)
{
    const lt_ini_setting_group *g = (const lt_ini_setting_group *)ud;
    if (i < 0 || (size_t)i >= g->n_options) { return ""; }
    return g->options[i].label != NULL ? g->options[i].label : g->options[i].value;
}

static void settings_page(lt_app *app)
{
    lt_ui *ui = app->ui;
    const lt_theme *t = lt_ui_theme(ui);
    int w1 = -1, w2[2], changed = 0;
    size_t i, k;
    int *sel;
    if (app->ini == NULL) {
        lt_ui_label_wrap(ui, "Open a project to choose its ECU definition settings.", t->muted);
        return;
    }
    lt_ui_label_wrap(ui, "Options the ECU definition offers in [SettingGroups] (TunerStudio's Project "
                     "Properties > Settings): units, sensor types, controller variant. Changing one reloads "
                     "the definition; the tune values are kept.", t->text2);
    if (!app->has_project) {
        lt_ui_layout_row(ui, 1, &w1, 0);
        lt_ui_label_wrap(ui, "No project is open: the choices below are shown for reference only.", t->warning);
    }
    if (app->ini->n_setting_groups == 0) {
        lt_ui_layout_row(ui, 1, &w1, 0);
        lt_ui_label_ex(ui, "This ECU definition has no project settings.", t->muted, LT_UI_ALIGN_LEFT);
        return;
    }
    sel = (int *)lt_calloc(app->ini->n_setting_groups + 1, sizeof(int));
    for (i = 0; i < app->ini->n_setting_groups; i++) {
        const lt_ini_setting_group *g = app->ini->setting_groups[i];
        int a = lt_ini_setting_active(app->ini, g);
        if (lt_ini_setting_is_flag(g)) { sel[i] = a >= 0 ? 1 : 0; continue; }
        sel[i] = a;
        if (a < 0) {               /* none chosen: the DEFAULT option when there is one */
            for (k = 0; k < g->n_options; k++) { if (is_default(g->options[k].value)) { sel[i] = (int)k; break; } }
        }
    }
    lt_ui_disabled_begin(ui, !app->has_project);
    w2[0] = lt_ui_cell_mode(ui) ? 36 * 8 : 280; w2[1] = -1;
    for (i = 0; i < app->ini->n_setting_groups; i++) {
        const lt_ini_setting_group *g = app->ini->setting_groups[i];
        lt_ui_push_id_int(ui, (int)i);
        if (lt_ini_setting_is_flag(g)) {
            lt_ui_layout_row(ui, 1, &w1, 0);
            if (lt_ui_checkbox(ui, g->label != NULL ? g->label : g->name, &sel[i])) { changed = 1; }
        } else {
            lt_ui_layout_row(ui, 2, w2, 0);
            lt_ui_label(ui, g->label != NULL ? g->label : g->name);
            if (lt_ui_combo_cb(ui, "opt", &sel[i], (int)g->n_options, opt_label, (void *)g)) { changed = 1; }
        }
        lt_ui_pop_id(ui);
    }
    lt_ui_disabled_end(ui);
    if (changed && app->has_project) {
        lt_proj_symbols sy;
        lt_proj_symbols_init(&sy);
        lt_proj_symbols_from_choice(app->ini, sel, &sy);
        if (lt_proj_symbols_save(app->project.path, &sy) < 0) {
            lt_app_toast(app, 3, "Cannot save the project settings: %s", lt_err_get());
        } else {
            ix(app)->reload_pending = 1;
        }
        lt_proj_symbols_free(&sy);
    }
    lt_free(sel);
}

static void ix_tick(lt_app *app)
{
    ix_state *s = ix(app);
    if (s->reload_pending) {
        s->reload_pending = 0;
        if (lt_app_reload_ini(app) == 0) { lt_app_toast(app, 1, "ECU definition reloaded with the new settings"); }
    }
}

/* ================================================================== */
void lt_feat_iniext_register(lt_app_ext *reg)
{
    lt_app_dialog_kind dk;
    dk.kind = "ini-help"; dk.draw = help_draw; dk.close = help_close;
    lt_ext_add_dialog_kind(reg, &dk);
    dk.kind = "pin-report"; dk.draw = pins_draw; dk.close = NULL;
    lt_ext_add_dialog_kind(reg, &dk);
    lt_ext_add_tab_kind(reg, &lt_portedit_tab_kind);
    lt_ext_add_menu_item(reg, LT_EXT_MENU_TOOLS, "Pin Conflict Report...", NULL, pin_menu, lt_ext_pred_ini);
    lt_ext_add_settings_page(reg, "Project INI Settings", settings_page);
    lt_ext_add_overlay(reg, pc_overlay);
    lt_ext_add_on_connect(reg, ix_on_connect);
    lt_ext_add_tick(reg, ix_tick);
}
