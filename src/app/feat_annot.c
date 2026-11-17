/* feat_annot.c - change annotations (G-15): notes on constants, tables and table cells, kept in the
 * project (projectCfg/annotations.json, see project/annot.h).
 * Tools -> Annotations...   (dialog kind "annotations"): list of all notes (or only those of the
 * active table), add / edit / delete.  Tabs of annotated tables carry a bullet in their title.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <string.h>
#include "feat.h"
#include "../project/annot.h"

#define MARK "\xE2\x80\xA2"       /* bullet, appended to the title of annotated tabs */
#define MARK_SFX " " MARK

typedef struct annot_state {
    lt_annot_set set;
    char for_path[512];             /* project the set belongs to */
    int  loaded;
} annot_state;

static void st_free(lt_app *app, void *p)
{
    LT_UNUSED(app);
    lt_annot_free(&((annot_state *)p)->set);
}

static annot_state *get(lt_app *app)
{
    annot_state *s = (annot_state *)lt_ext_state(app, "annot", sizeof(*s), st_free);
    const char *path = app->has_project ? app->project.path : "";
    if (!s->loaded || strcmp(s->for_path, path) != 0) {
        lt_annot_free(&s->set);
        s->loaded = 1;
        lt_strlcpy(s->for_path, path, sizeof(s->for_path));
        if (app->has_project) {
            char *f = lt_annot_path(&app->project);
            if (lt_annot_load(&s->set, f) < 0) { lt_app_toast(app, 2, "Annotations: %s", lt_err_get()); }
            lt_free(f);
        }
    }
    return s;
}

static int persist(lt_app *app, annot_state *s)
{
    char *f;
    int rc;
    if (!app->has_project) { return -1; }
    f = lt_annot_path(&app->project);
    rc = lt_annot_save(&s->set, f);
    lt_free(f);
    if (rc < 0) { lt_app_toast(app, 3, "Cannot save annotations: %s", lt_err_get()); }
    return rc;
}

/* ---- public helpers (feat.h) ---- */
size_t lt_feat_annot_count(lt_app *app, const char *table)
{
    if (!app->has_project || table == NULL || table[0] == '\0') { return 0; }
    return lt_annot_count_table(&get(app)->set, table);
}

int lt_feat_annot_set(lt_app *app, const char *key, const char *text, int tag)
{
    annot_state *s;
    int rc;
    if (!app->has_project) { lt_err_set("no project open"); return -1; }
    s = get(app);
    rc = lt_annot_put(&s->set, key, text, tag, "");
    if (rc < 0) { return rc; }
    return persist(app, s);
}

int lt_feat_annot_delete(lt_app *app, const char *key)
{
    annot_state *s;
    if (!app->has_project) { return -1; }
    s = get(app);
    if (!lt_annot_delete(&s->set, key)) { return -1; }
    return persist(app, s);
}

/* ---- tab markers ---- */
static void tick(lt_app *app)
{
    int i;
    size_t mlen = strlen(MARK_SFX);
    annot_state *s;
    if (!app->has_project || app->n_tabs == 0) { return; }
    s = get(app);
    for (i = 0; i < app->n_tabs; i++) {
        lt_app_tab *t = app->tabs[i];
        size_t len;
        int marked, want;
        if (strcmp(t->kind, "table") != 0 && strcmp(t->kind, "curve") != 0) { continue; }
        len = strlen(t->title);
        marked = len >= mlen && strcmp(t->title + len - mlen, MARK_SFX) == 0;
        want = lt_annot_count_table(&s->set, t->target) > 0;
        if (want && !marked && len + mlen < sizeof(t->title)) {
            lt_strlcat(t->title, MARK_SFX, sizeof(t->title));
        } else if (!want && marked) {
            t->title[len - mlen] = '\0';
        }
    }
}

/* ---- dialog ---- */
typedef struct annot_dlg {
    char key[128], text[400];
    int  tag;                       /* index into the tag combo: 0 none, then LT_ANNOT_* + 1 */
    int  show_add, only_table;
    char table[128];                /* active table at open */
    char msg[160];
    int  confirm;                   /* row awaiting a second delete click, -1 */
} annot_dlg;

static lt_color tag_color(const lt_theme *t, int tag)
{
    switch (tag) {
    case LT_ANNOT_INFO: return t->info;
    case LT_ANNOT_WARNING: return t->warning;
    case LT_ANNOT_CRITICAL: return t->error;
    case LT_ANNOT_SUCCESS: return t->success;
    case LT_ANNOT_TODO: return t->accent;
    default: return t->muted;
    }
}

static void ad_init(lt_app *app, annot_dlg *s)
{
    s->confirm = -1;
    s->tag = LT_ANNOT_INFO + 1;
    if (app->active_tab >= 0 && app->active_tab < app->n_tabs) {
        const lt_app_tab *t = app->tabs[app->active_tab];
        if (strcmp(t->kind, "table") == 0 || strcmp(t->kind, "curve") == 0) {
            lt_strlcpy(s->table, t->target, sizeof(s->table));
            lt_strlcpy(s->key, t->target, sizeof(s->key));
            s->only_table = 1;
        }
    }
}

static void ad_draw(lt_app *app, lt_app_dialog *d)
{
    lt_ui *ui = app->ui;
    const lt_theme *t = lt_ui_theme(ui);
    annot_dlg *s = (annot_dlg *)d->data;
    annot_state *st;
    static const char *const tags[] = {"No tag", "Info", "Warning", "Critical", "Success", "Todo"};
    int open = d->open, w1 = -1, w2[2], w3[3], fh;
    size_t *idx, n, i;
    char line[300];
    if (s == NULL) { s = (annot_dlg *)lt_calloc(1, sizeof(*s)); d->data = s; ad_init(app, s); }
    st = get(app);
    fh = lt_ui_cell_mode(ui) ? 16 : 32;
    if (lt_ui_modal_begin(ui, "Annotations", 680, 520, &open)) {
        if (!app->has_project) {
            lt_ui_layout_row(ui, 1, &w1, 0);
            lt_ui_label_ex(ui, "Annotations belong to a project. Open or create a project first.", t->warning,
                           LT_UI_ALIGN_LEFT);
        } else {
            idx = (size_t *)lt_calloc(st->set.n + 1, sizeof(size_t));
            n = lt_annot_order(&st->set, s->only_table && s->table[0] != '\0' ? s->table : NULL, idx);
            w2[0] = -1; w2[1] = 220;
            lt_ui_layout_row(ui, 2, w2, 0);
            lt_snprintf(line, sizeof(line), "Notes (%lu)", (unsigned long)n);
            lt_ui_text_style(ui, (int)LT_FONT_SANS_SEMIBOLD, LT_UI_FS_MD);
            lt_ui_label_ex(ui, line, t->text, LT_UI_ALIGN_LEFT);
            lt_ui_text_style_reset(ui);
            if (s->table[0] != '\0') {
                lt_snprintf(line, sizeof(line), "Only %s", s->table);
                lt_ui_checkbox(ui, line, &s->only_table);
            } else {
                lt_ui_label(ui, "");
            }
            lt_ui_layout_row(ui, 1, &w1, 0);
            if (lt_ui_button(ui, s->show_add ? "Hide form" : "+ Add note")) { s->show_add = !s->show_add; }
            if (s->show_add) {
                int ok;
                lt_ui_layout_row(ui, 1, &w1, 0);
                lt_ui_label_ex(ui, "Key: constant, table, or table:row:col for one cell", t->muted,
                               LT_UI_ALIGN_LEFT);
                lt_ui_layout_row(ui, 1, &w1, 0);
                lt_ui_text(ui, "akey", s->key, sizeof(s->key), 0);
                lt_ui_layout_row(ui, 1, &w1, 0);
                lt_ui_text(ui, "atext", s->text, sizeof(s->text), 0);
                w2[0] = 160; w2[1] = 110;
                lt_ui_layout_row(ui, 2, w2, 0);
                lt_ui_combo(ui, "atag", &s->tag, tags, 6);
                ok = s->key[0] != '\0' && s->text[0] != '\0';
                if (lt_ui_button_ex(ui, "Save", ok ? LT_UI_BTN_PRIMARY : LT_UI_BTN_DISABLED) && ok) {
                    if (lt_feat_annot_set(app, s->key, s->text, s->tag - 1) == 0) {
                        s->text[0] = '\0';
                        s->show_add = 0;
                        s->msg[0] = '\0';
                    } else { lt_strlcpy(s->msg, lt_err_get(), sizeof(s->msg)); }
                }
                if (s->msg[0] != '\0') {
                    lt_ui_layout_row(ui, 1, &w1, 0);
                    lt_ui_label_ex(ui, s->msg, t->error, LT_UI_ALIGN_LEFT);
                }
            }
            lt_ui_layout_row(ui, 1, &w1, -(fh + 8));
            if (lt_ui_scroll_begin(ui, "alist")) {
                if (n == 0) {
                    lt_ui_layout_row(ui, 1, &w1, 0);
                    lt_ui_label_ex(ui, "No annotations yet", t->muted, LT_UI_ALIGN_CENTER);
                }
                for (i = 0; i < n; i++) {
                    const lt_annot *a = &st->set.v[idx[i]];
                    char when[40];
                    int del = 0, edit = 0;
                    lt_ui_push_id_int(ui, (int)i);
                    w3[0] = 80; w3[1] = -1; w3[2] = 120;
                    lt_ui_layout_row(ui, 3, w3, 0);
                    lt_ui_label_ex(ui, a->tag != LT_ANNOT_NONE ? lt_annot_tag_name(a->tag) : "",
                                   tag_color(t, a->tag), LT_UI_ALIGN_LEFT);
                    lt_ui_text_style(ui, (int)LT_FONT_SANS_SEMIBOLD, LT_UI_FS_BASE);
                    lt_ui_label_ex(ui, a->key, t->text, LT_UI_ALIGN_LEFT);
                    lt_ui_text_style_reset(ui);
                    lt_ui_label_ex(ui, "", t->muted, LT_UI_ALIGN_LEFT);
                    lt_ui_layout_row(ui, 1, &w1, 0);
                    lt_ui_label_wrap(ui, a->text, t->text2);
                    lt_snprintf(when, sizeof(when), "%.10s%s%s", a->created,
                                a->author[0] != '\0' ? "  -  " : "", a->author);
                    w3[0] = -1; w3[1] = 70; w3[2] = 110;
                    lt_ui_layout_row(ui, 3, w3, 0);
                    lt_ui_label_ex(ui, when, t->muted, LT_UI_ALIGN_LEFT);
                    if (lt_ui_button(ui, "Edit")) { edit = 1; }
                    if (lt_ui_button_ex(ui, s->confirm == (int)i ? "Confirm" : "Delete",
                                        s->confirm == (int)i ? LT_UI_BTN_DANGER : 0)) {
                        if (s->confirm == (int)i) { del = 1; } else { s->confirm = (int)i; }
                    }
                    lt_ui_layout_row(ui, 1, &w1, 4);
                    lt_ui_separator(ui);
                    lt_ui_pop_id(ui);
                    if (edit) {
                        lt_strlcpy(s->key, a->key, sizeof(s->key));
                        lt_strlcpy(s->text, a->text, sizeof(s->text));
                        s->tag = a->tag + 1;
                        s->show_add = 1;
                    }
                    if (del) {
                        char key[128];
                        lt_strlcpy(key, a->key, sizeof(key));
                        (void)lt_feat_annot_delete(app, key);
                        s->confirm = -1;
                        break;
                    }
                }
                lt_ui_scroll_end(ui);
            }
            lt_free(idx);
        }
        w2[0] = -1; w2[1] = 110;
        lt_ui_layout_row(ui, 2, w2, 0);
        lt_ui_label(ui, "");
        if (lt_ui_button_ex(ui, "Close", LT_UI_BTN_DEFAULT)) { open = 0; }
        lt_ui_modal_end(ui);
    }
    d->open = open;
}

static void ad_close(lt_app *app, lt_app_dialog *d)
{
    LT_UNUSED(app);
    lt_free(d->data);
    d->data = NULL;
}

static void open_dialog(lt_app *app)
{
    (void)lt_app_dialog_push(app, "annotations", "", "Annotations");
}

void lt_feat_annot_register(lt_app_ext *reg)
{
    lt_app_dialog_kind dk;
    lt_ext_add_menu_item(reg, LT_EXT_MENU_TOOLS, "&Annotations...", NULL, open_dialog, lt_ext_pred_project);
    lt_ext_add_tick(reg, tick);
    dk.kind = "annotations"; dk.draw = ad_draw; dk.close = ad_close;
    lt_ext_add_dialog_kind(reg, &dk);
}
