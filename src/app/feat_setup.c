/* feat_setup.c - the new-engine setup checklist (Tools > Engine Setup Checklist): the eight
 * things every new engine needs, each linked to the page of this ECU definition that handles
 * it (setup_logic.h), ticked when that page was opened (or by hand), kept per project in
 * setup.conf. Opens by itself once for a project created today; "Hide" puts it away.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <string.h>
#include "feat.h"
#include "setup_logic.h"

typedef struct setup_feat {
    char proj[512];                  /* project the state belongs to */
    const lt_ini *ini;               /* definition the links were made for */
    lt_setup_state st;
    lt_setup_link links[LT_SETUP_STEPS];
    int n_links;
} setup_feat;

static setup_feat *sf(lt_app *app)
{
    return (setup_feat *)lt_ext_state(app, "setup", sizeof(setup_feat), NULL);
}

static void save(lt_app *app, setup_feat *s)
{
    if (app->has_project && !app->headless) { (void)lt_setup_save(&s->st, app->project.path); }
}

/* created today ("YYYY-MM-DD..." of project.json against the local date) */
static int created_today(lt_app *app)
{
    char now[24];
    const char *c = app->project.cfg.created;
    if (c == NULL || strlen(c) < 10) { return 0; }
    lt_plat_timestamp(now, sizeof(now));
    return strncmp(c, now, 10) == 0;
}

static int has_saved_tune(lt_app *app)
{
    char *f = lt_path_join(app->project.path, "CurrentTune.msq");
    int ok = f != NULL && lt_file_exists(f);
    lt_free(f);
    return ok;
}

static int open_setup(lt_app *app, const char *target)
{
    LT_UNUSED(target);
    return lt_app_tab_open(app, "setup", "setup", "Engine Setup", 1) != NULL ? 0 : -1;
}

static void act_open(lt_app *app)
{
    setup_feat *s = sf(app);
    s->st.dismissed = 0;
    save(app, s);
    (void)open_setup(app, NULL);
}

static void tick(lt_app *app)
{
    setup_feat *s = sf(app);
    int i, k, ch = 0;
    if (!app->has_project || app->ini == NULL || app->project.path == NULL) { s->proj[0] = '\0'; return; }
    if (strcmp(s->proj, app->project.path) != 0 || s->ini != app->ini) {
        int fresh, newp = strcmp(s->proj, app->project.path) != 0;
        lt_strlcpy(s->proj, app->project.path, sizeof(s->proj));
        s->ini = app->ini;
        s->n_links = lt_setup_match(app->ini, s->links);
        if (newp) {
            fresh = lt_setup_load(&s->st, app->project.path);
            /* a new engine: open the checklist once, for a project made today or without a tune */
            if (fresh == 1 && !s->st.shown && !app->headless && (created_today(app) || !has_saved_tune(app))) {
                s->st.shown = 1;
                save(app, s);
                if (lt_app_tab_open(app, "setup", "setup", "Engine Setup", 1) != NULL) {
                    lt_app_toast(app, 0, "New project: the Engine Setup checklist shows what to set first.");
                }
            }
        }
    }
    /* a page that was opened counts as visited */
    for (i = 0; i < LT_SETUP_STEPS; i++) {
        if (s->st.done[i] || s->links[i].target[0] == '\0') { continue; }
        for (k = 0; k < app->n_tabs; k++) {
            if (strcmp(app->tabs[k]->target, s->links[i].target) == 0 && strcmp(app->tabs[k]->kind, "setup") != 0) {
                s->st.done[i] = 1;
                ch = 1;
                break;
            }
        }
    }
    if (ch) { save(app, s); }
}

static void draw(lt_app *app, lt_app_tab *tab, lt_rect r)
{
    lt_ui *ui = app->ui;
    const lt_theme *t = lt_ui_theme(ui);
    setup_feat *s = sf(app);
    int cell = lt_ui_cell_mode(ui), w1 = -1, w2[2], i, done, cw;
    char buf[200];
    lt_rect inner;
    LT_UNUSED(tab);
    lt_ui_draw_fill(ui, r, t->bg_app);
    cw = LT_MIN(r.w - (cell ? 16 : lt_ui_px(ui, 48)), cell ? 100 * 8 : lt_ui_px(ui, 760));
    inner = lt_rect_make(r.x + (r.w - cw) / 2, r.y + (cell ? 16 : lt_ui_px(ui, 24)), cw,
                         r.h - (cell ? 16 : lt_ui_px(ui, 24)));
    lt_ui_region_begin(ui, inner, 0);
    lt_ui_layout_row(ui, 1, &w1, -1);
    if (!lt_ui_scroll_begin(ui, "setup_scroll")) { lt_ui_region_end(ui); return; }
    lt_ui_layout_row(ui, 1, &w1, 0);
    lt_ui_heading(ui, "Set up a new engine");
    lt_ui_label_wrap(ui, "Go through these once for a new engine or ECU. Each button opens the page of this "
                         "ECU's settings that handles it. Pages you open are ticked; untick what you still "
                         "want to check. Burn when you are done.", t->text2);
    done = lt_setup_count_done(&s->st);
    lt_snprintf(buf, sizeof(buf), "%d of %d done", done, LT_SETUP_STEPS);
    lt_ui_progress(ui, (double)done / (double)LT_SETUP_STEPS, buf);
    lt_ui_layout_space(ui, cell ? 16 : 8);
    w2[0] = -1;
    w2[1] = cell ? 12 * 8 : lt_ui_px(ui, 110);
    for (i = 0; i < LT_SETUP_STEPS; i++) {
        const lt_setup_step *st = lt_setup_step_at(i);
        int has = s->links[i].target[0] != '\0';
        lt_ui_push_id_int(ui, i);
        lt_ui_layout_row_px(ui, 2, w2, 0);
        if (lt_ui_checkbox(ui, st->title, &s->st.done[i])) { save(app, s); }
        if (lt_ui_button_ex(ui, has ? "Open" : "Open##none", has ? 0 : LT_UI_BTN_DISABLED) && has) {
            s->st.done[i] = 1;
            save(app, s);
            {   /* the tab is named like the menu entry ("Trigger"), not its path */
                const char *leaf = strrchr(s->links[i].where, '>');
                (void)lt_app_open_target(app, s->links[i].target, leaf != NULL ? leaf + 2 : s->links[i].where);
            }
        }
        if (has) { lt_ui_tooltip(ui, s->links[i].where); }
        lt_ui_layout_row(ui, 1, &w1, 0);
        lt_ui_layout_indent(ui, cell ? 32 : 22);
        if (has) { lt_snprintf(buf, sizeof(buf), "%s   (%s)", st->why, s->links[i].where); }
        else { lt_snprintf(buf, sizeof(buf), "%s   This ECU definition has no page for it in its menus.", st->why); }
        lt_ui_label_wrap(ui, buf, t->muted);
        lt_ui_layout_indent(ui, cell ? -32 : -22);
        lt_ui_layout_space(ui, cell ? 0 : 4);
        lt_ui_pop_id(ui);
    }
    lt_ui_layout_space(ui, cell ? 16 : 8);
    w2[0] = -1;
    w2[1] = cell ? 22 * 8 : lt_ui_px(ui, 190);
    lt_ui_layout_row_px(ui, 2, w2, 0);
    lt_ui_label_ex(ui, done == LT_SETUP_STEPS ? "All done. Reopen this list from Tools > Engine Setup Checklist."
                                              : "Reopen it any time from Tools > Engine Setup Checklist.",
                   t->muted, LT_UI_ALIGN_LEFT);
    if (lt_ui_button_ex(ui, "Hide this checklist", LT_UI_BTN_FLAT)) {
        s->st.dismissed = 1;
        save(app, s);
        lt_ui_scroll_end(ui);
        lt_ui_region_end(ui);
        lt_app_tab_close(app, tab);
        return;
    }
    lt_ui_scroll_end(ui);
    lt_ui_region_end(ui);
}

void lt_feat_setup_register(lt_app_ext *reg)
{
    lt_app_tab_kind tk;
    tk.kind = "setup";
    tk.draw = draw;
    tk.close = NULL;
    lt_ext_add_tab_kind(reg, &tk);
    lt_ext_add_target(reg, "setup_checklist", open_setup);
    lt_ext_add_menu_item(reg, LT_EXT_MENU_TOOLS, "Engine Setup &Checklist", NULL, act_open, lt_ext_pred_project);
    lt_ext_add_tick(reg, tick);
}
