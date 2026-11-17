/* app_ext.c - shell extension registry: targets, menu items, shortcuts, settings pages,
 * tick hooks, overlays and per-feature state.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <string.h>
#include "feat.h"

typedef struct ext_target { char name[48]; lt_ext_target_fn fn; } ext_target;
typedef struct ext_item {
    int menu; const char *label; const char *shortcut; lt_ext_action_fn fn; lt_ext_pred_fn pred;
} ext_item;
typedef struct ext_key { int key, mods; lt_ext_action_fn fn; lt_ext_pred_fn pred; } ext_key;
typedef struct ext_page { const char *title; lt_ext_draw_fn draw; } ext_page;
typedef struct ext_hook { lt_ext_draw_fn fn; } ext_hook;
typedef struct ext_state { char key[48]; void *data; lt_ext_free_fn cleanup; } ext_state;

struct lt_app_ext {
    lt_app *app;
    ext_target *targets; int n_targets, cap_targets;
    ext_item *items; int n_items, cap_items;
    ext_key *keys; int n_keys, cap_keys;
    ext_page *pages; int n_pages, cap_pages;
    ext_hook *ticks; int n_ticks, cap_ticks;
    ext_hook *overlays; int n_overlays, cap_overlays;
    ext_hook *connects; int n_connects, cap_connects;
    ext_hook *losts; int n_losts, cap_losts;
    ext_hook *saves; int n_saves, cap_saves;
    ext_state *states; int n_states, cap_states;
};

/* grow a table by one element; returns the new slot */
static void *ext_push(void **arr, int *n, int *cap, size_t esz)
{
    if (*n == *cap) {
        size_t bytes = 0;
        *cap = *cap ? *cap * 2 : 8;
        if (!lt_mul_size(esz, (size_t)*cap, &bytes)) { bytes = (size_t)-1; }   /* OOM abort */
        *arr = lt_realloc(*arr, bytes);
    }
    (*n)++;
    return (char *)*arr + esz * (size_t)(*n - 1);
}

int lt_ext_pred_ini(lt_app *app) { return app->ini != NULL; }
int lt_ext_pred_project(lt_app *app) { return app->has_project; }
int lt_ext_pred_online(lt_app *app)
{
    return app->conn == LT_APP_ONLINE || app->conn == LT_APP_DEMO;
}

lt_app_ext *lt_ext_new(lt_app *app)
{
    lt_app_ext *reg = (lt_app_ext *)lt_calloc(1, sizeof(*reg));
    reg->app = app;
    app->ext = reg;
    lt_app_features_register(reg);
    return reg;
}

void lt_ext_free(lt_app *app)
{
    lt_app_ext *e = app->ext;
    int i;
    if (e == NULL) { return; }
    for (i = e->n_states - 1; i >= 0; i--) {
        if (e->states[i].cleanup != NULL) { e->states[i].cleanup(app, e->states[i].data); }
        lt_free(e->states[i].data);
    }
    lt_free(e->targets); lt_free(e->items); lt_free(e->keys); lt_free(e->pages);
    lt_free(e->connects); lt_free(e->losts); lt_free(e->saves);
    lt_free(e->ticks); lt_free(e->overlays); lt_free(e->states);
    lt_free(e);
    app->ext = NULL;
}

/* ---- targets ---- */
void lt_ext_add_target(lt_app_ext *reg, const char *name, lt_ext_target_fn open_fn)
{
    int i;
    ext_target *t;
    for (i = 0; i < reg->n_targets; i++) {
        if (strcmp(reg->targets[i].name, name) == 0) { reg->targets[i].fn = open_fn; return; }
    }
    t = (ext_target *)ext_push((void **)&reg->targets, &reg->n_targets, &reg->cap_targets,
                               sizeof(ext_target));
    lt_strlcpy(t->name, name, sizeof(t->name));
    t->fn = open_fn;
}

int lt_ext_has_target(const lt_app *app, const char *target)
{
    int i;
    if (app->ext == NULL || target == NULL) { return 0; }
    for (i = 0; i < app->ext->n_targets; i++) {
        if (strcmp(app->ext->targets[i].name, target) == 0) { return 1; }
    }
    return 0;
}

int lt_ext_dispatch_target(lt_app *app, const char *target)
{
    int i;
    if (app->ext == NULL || target == NULL) { return LT_EXT_NO_TARGET; }
    for (i = 0; i < app->ext->n_targets; i++) {
        if (strcmp(app->ext->targets[i].name, target) == 0) {
            return app->ext->targets[i].fn(app, target);
        }
    }
    return LT_EXT_NO_TARGET;
}

/* ---- menu items ---- */
void lt_ext_add_menu_item(lt_app_ext *reg, int menu, const char *label, const char *shortcut,
                          lt_ext_action_fn fn, lt_ext_pred_fn pred)
{
    ext_item *it = (ext_item *)ext_push((void **)&reg->items, &reg->n_items, &reg->cap_items,
                                        sizeof(ext_item));
    it->menu = menu; it->label = label; it->shortcut = shortcut; it->fn = fn; it->pred = pred;
}

int lt_ext_n_menu_items(const lt_app *app, int menu)
{
    int i, n = 0;
    if (app->ext == NULL) { return 0; }
    for (i = 0; i < app->ext->n_items; i++) { if (app->ext->items[i].menu == menu) { n++; } }
    return n;
}

const char *lt_ext_menu_item_label(const lt_app *app, int menu, int idx)
{
    int i, n = 0;
    if (app->ext == NULL) { return NULL; }
    for (i = 0; i < app->ext->n_items; i++) {
        if (app->ext->items[i].menu == menu) {
            if (n == idx) { return app->ext->items[i].label; }
            n++;
        }
    }
    return NULL;
}

int lt_ext_menu_draw(lt_app *app, int menu, int lead_sep)
{
    int i, shown = 0;
    lt_ui *ui = app->ui;
    if (app->ext == NULL) { return 0; }
    for (i = 0; i < app->ext->n_items; i++) {
        const ext_item *it = &app->ext->items[i];
        if (it->menu != menu) { continue; }
        if (it->pred != NULL && !it->pred(app)) { continue; }
        if (shown == 0 && lead_sep) { lt_ui_menu_separator(ui); }
        shown++;
        lt_ui_push_id_int(ui, 7000 + i);
        if (lt_ui_menu_item(ui, it->label, it->shortcut, 0)) { it->fn(app); }
        lt_ui_pop_id(ui);
    }
    return shown;
}

/* ---- shortcuts ---- */
void lt_ext_add_shortcut(lt_app_ext *reg, int key, int mods, lt_ext_action_fn fn,
                         lt_ext_pred_fn pred)
{
    ext_key *k = (ext_key *)ext_push((void **)&reg->keys, &reg->n_keys, &reg->cap_keys,
                                     sizeof(ext_key));
    k->key = key; k->mods = mods; k->fn = fn; k->pred = pred;
}

void lt_ext_shortcuts(lt_app *app)
{
    int i;
    if (app->ext == NULL) { return; }
    for (i = 0; i < app->ext->n_keys; i++) {
        const ext_key *k = &app->ext->keys[i];
        if (k->pred != NULL && !k->pred(app)) { continue; }
        if (lt_ui_key_pressed(app->ui, k->key, k->mods)) { k->fn(app); }
    }
}

/* ---- settings pages ---- */
void lt_ext_add_settings_page(lt_app_ext *reg, const char *title, lt_ext_draw_fn draw)
{
    ext_page *p = (ext_page *)ext_push((void **)&reg->pages, &reg->n_pages, &reg->cap_pages,
                                       sizeof(ext_page));
    p->title = title; p->draw = draw;
}

int lt_ext_n_settings_pages(const lt_app *app) { return app->ext != NULL ? app->ext->n_pages : 0; }

const char *lt_ext_settings_title(const lt_app *app, int idx)
{
    if (app->ext == NULL || idx < 0 || idx >= app->ext->n_pages) { return NULL; }
    return app->ext->pages[idx].title;
}

void lt_ext_settings_draw(lt_app *app, int idx)
{
    if (app->ext == NULL || idx < 0 || idx >= app->ext->n_pages) { return; }
    app->ext->pages[idx].draw(app);
}

/* ---- tick / overlay hooks ---- */
void lt_ext_add_tick(lt_app_ext *reg, lt_ext_draw_fn fn)
{
    ext_hook *h = (ext_hook *)ext_push((void **)&reg->ticks, &reg->n_ticks, &reg->cap_ticks,
                                       sizeof(ext_hook));
    h->fn = fn;
}

void lt_ext_add_overlay(lt_app_ext *reg, lt_ext_draw_fn fn)
{
    ext_hook *h = (ext_hook *)ext_push((void **)&reg->overlays, &reg->n_overlays,
                                       &reg->cap_overlays, sizeof(ext_hook));
    h->fn = fn;
}

void lt_ext_tick(lt_app *app)
{
    int i;
    if (app->ext == NULL) { return; }
    for (i = 0; i < app->ext->n_ticks; i++) { app->ext->ticks[i].fn(app); }
}

void lt_ext_overlays(lt_app *app)
{
    int i;
    if (app->ext == NULL) { return; }
    for (i = 0; i < app->ext->n_overlays; i++) { app->ext->overlays[i].fn(app); }
}

/* ---- connection hooks ---- */
void lt_ext_add_on_connect(lt_app_ext *reg, lt_ext_draw_fn fn)
{
    ext_hook *h = (ext_hook *)ext_push((void **)&reg->connects, &reg->n_connects,
                                       &reg->cap_connects, sizeof(ext_hook));
    h->fn = fn;
}

void lt_ext_add_on_link_lost(lt_app_ext *reg, lt_ext_draw_fn fn)
{
    ext_hook *h = (ext_hook *)ext_push((void **)&reg->losts, &reg->n_losts, &reg->cap_losts,
                                       sizeof(ext_hook));
    h->fn = fn;
}

void lt_ext_on_connect(lt_app *app)
{
    int i;
    if (app->ext == NULL) { return; }
    for (i = 0; i < app->ext->n_connects; i++) { app->ext->connects[i].fn(app); }
}

void lt_ext_on_link_lost(lt_app *app)
{
    int i;
    if (app->ext == NULL) { return; }
    for (i = 0; i < app->ext->n_losts; i++) { app->ext->losts[i].fn(app); }
}

void lt_ext_add_on_save(lt_app_ext *reg, lt_ext_draw_fn fn)
{
    ext_hook *h = (ext_hook *)ext_push((void **)&reg->saves, &reg->n_saves, &reg->cap_saves,
                                       sizeof(ext_hook));
    h->fn = fn;
}

void lt_ext_on_save(lt_app *app)
{
    int i;
    if (app->ext == NULL) { return; }
    for (i = 0; i < app->ext->n_saves; i++) { app->ext->saves[i].fn(app); }
}

void lt_ext_add_dialog_kind(lt_app_ext *reg, const lt_app_dialog_kind *k)
{
    lt_app_register_dialog_kind(reg->app, k);
}

void lt_ext_add_tab_kind(lt_app_ext *reg, const lt_app_tab_kind *k)
{
    lt_app_register_tab_kind(reg->app, k);
}

/* ---- feature state ---- */
void *lt_ext_state(lt_app *app, const char *key, size_t size, lt_ext_free_fn cleanup)
{
    lt_app_ext *e = app->ext;
    ext_state *s;
    int i;
    for (i = 0; i < e->n_states; i++) {
        if (strcmp(e->states[i].key, key) == 0) { return e->states[i].data; }
    }
    s = (ext_state *)ext_push((void **)&e->states, &e->n_states, &e->cap_states,
                              sizeof(ext_state));
    lt_strlcpy(s->key, key, sizeof(s->key));
    s->data = lt_calloc(1, size);
    s->cleanup = cleanup;
    return s->data;
}
