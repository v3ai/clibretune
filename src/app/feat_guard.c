/* feat_guard.c - never lose work: how many changed settings are only in the ECU's RAM (status
 * bar chip with a Burn button), the question before quit / close project / disconnect when
 * something would be lost (guard_logic.h), a periodic autosave of the working tune to
 * <project>/recovery/ and the offer to restore it when a project is opened after a crash.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <string.h>
#include "feat.h"
#include "guard_logic.h"
#include "sync_logic.h"
#include "recent_logic.h"

#define AUTOSAVE_MS 60000.0
#define CHECK_MS 500.0

typedef struct guard_state {
    /* unburned */
    lt_tune *base;                   /* what the ECU's flash holds: taken when nothing was pending */
    unsigned long base_hash, hash, cmp_hash;   /* cmp_hash: memory the count was made for */
    int unburned;                    /* last count (0 none, LT_GUARD_SOME unknown) */
    double t_check;
    /* question */
    int open, what;
    lt_guard_q q;
    lt_feat_guard_fn next;           /* lt_feat_guard_switch: what to do instead of closing */
    char next_arg[512];
    /* autosave / recovery */
    char proj[512];
    double t_auto, dirty_since;
    int written;
    int rec_open, rec_diff;
    char rec_when[24];
} guard_state;

static void guard_cleanup(lt_app *app, void *p)
{
    guard_state *g = (guard_state *)p;
    LT_UNUSED(app);
    lt_tune_free(g->base);
    g->base = NULL;
}

static guard_state *gs(lt_app *app)
{
    return (guard_state *)lt_ext_state(app, "guard", sizeof(guard_state), guard_cleanup);
}

static unsigned long mem_hash(const lt_ecu_mem *m)
{
    unsigned long h = 2166136261UL;
    size_t pg, n = lt_ecu_mem_page_count(m), i, len;
    for (pg = 0; pg < n; pg++) {
        const lt_u8 *d = lt_ecu_mem_page(m, (int)pg, &len);
        if (d == NULL) { continue; }
        for (i = 0; i < len; i++) { h = ((h ^ d[i]) * 16777619UL) & 0xFFFFFFFFUL; }
        h = ((h ^ (unsigned long)pg) * 16777619UL) & 0xFFFFFFFFUL;
    }
    return h;
}

static void update_unburned(lt_app *app, guard_state *g)
{
    double now = lt_plat_now_ms();
    if ((app->conn != LT_APP_ONLINE && app->conn != LT_APP_DEMO) || app->mem == NULL || app->ini == NULL ||
        app->tune == NULL) {
        lt_tune_free(g->base);
        g->base = NULL;
        g->unburned = 0;
        return;
    }
    if (g->t_check != 0.0 && now - g->t_check < CHECK_MS && now >= g->t_check) { return; }
    g->t_check = now;
    if (!lt_ecu_mem_fully_loaded(app->mem)) { return; }
    g->hash = mem_hash(app->mem);
    if (!lt_app_can_burn(app)) {
        /* nothing pending: the ECU's flash holds what LibreTune shows */
        if (g->base == NULL || g->hash != g->base_hash) {
            lt_tune_free(g->base);
            g->base = lt_tune_clone(app->tune);
            if (g->base != NULL) { (void)lt_tune_from_mem(g->base, app->mem); }
            g->base_hash = g->hash;
        }
        g->unburned = 0;
        return;
    }
    if (g->base == NULL) { g->unburned = LT_GUARD_SOME; return; }
    if (g->unburned != 0 && g->hash == g->cmp_hash) { return; }    /* nothing changed since */
    g->cmp_hash = g->hash;
    {
        lt_sync_cmp *c = lt_sync_compare(app->ini, g->base, app->mem);
        int n = c != NULL && c->diff != NULL ? (int)c->diff->n : 0;
        g->unburned = n > 0 ? n : LT_GUARD_SOME;
        lt_sync_cmp_free(c);
    }
}

int lt_feat_guard_unburned(lt_app *app)
{
    guard_state *g = gs(app);
    if (!lt_app_can_burn(app)) { return 0; }
    return g->unburned != 0 ? g->unburned : LT_GUARD_SOME;
}

/* ------------------------------------------------------------------ */
/* autosave and recovery                                               */
char *lt_feat_autosave_path(lt_app *app)
{
    return app->has_project && app->project.path != NULL ? lt_guard_recovery_path(app->project.path) : NULL;
}

int lt_feat_autosave_now(lt_app *app)
{
    guard_state *g = gs(app);
    lt_tune *t;
    int rc;
    if (!app->has_project || app->tune == NULL || app->ini == NULL || !app->modified) { return 1; }
    t = lt_tune_clone(app->tune);
    if (t == NULL) { return -1; }
    if (app->mem != NULL) { (void)lt_tune_from_mem(t, app->mem); }
    (void)lt_tune_capture_from_pages(t, app->ini);
    lt_tune_touch(t);
    rc = lt_guard_recovery_write(app->project.path, t);
    lt_tune_free(t);
    if (rc == 0) { g->written = 1; }
    g->t_auto = lt_plat_now_ms();
    return rc;
}

void lt_feat_autosave_discard(lt_app *app)
{
    guard_state *g = gs(app);
    if (app->has_project && app->project.path != NULL) { (void)lt_guard_recovery_remove(app->project.path); }
    g->written = 0;
}

static void autosave_tick(lt_app *app, guard_state *g)
{
    double now = lt_plat_now_ms();
    if (!app->has_project || app->project.path == NULL) { g->proj[0] = '\0'; return; }
    if (strcmp(g->proj, app->project.path) != 0) {
        /* a project was just opened: did the last session end without saving? */
        lt_strlcpy(g->proj, app->project.path, sizeof(g->proj));
        g->written = 0;
        g->dirty_since = 0.0;
        g->t_auto = now;
        g->rec_open = lt_guard_recovery_pending(app->project.path, app->project.tune, g->rec_when,
                                                sizeof(g->rec_when), &g->rec_diff);
    }
    if (app->headless || g->rec_open) { return; }      /* never overwrite what is being offered */
    if (app->modified) {
        if (g->dirty_since == 0.0) { g->dirty_since = now; }
        if (now - g->t_auto >= AUTOSAVE_MS || now < g->t_auto) { (void)lt_feat_autosave_now(app); }
    } else {
        g->dirty_since = 0.0;
        g->t_auto = now;
        if (g->written) { lt_feat_autosave_discard(app); }  /* saved: the recovery copy is stale */
    }
}

static void tick(lt_app *app)
{
    guard_state *g = gs(app);
    update_unburned(app, g);
    autosave_tick(app, g);
}

/* ------------------------------------------------------------------ */
/* quit / close / disconnect                                           */
static void do_it(lt_app *app, int what)
{
    switch (what) {
    case LT_GUARD_QUIT:
        app->modified = 0;
        app->quit = 1;
        break;
    case LT_GUARD_CLOSE:
        lt_app_close_project(app);
        break;
    default:
        lt_app_disconnect(app);
        break;
    }
}

/* close / quit / disconnect, or the action lt_feat_guard_switch was given */
static void go_on(lt_app *app, guard_state *g, int what)
{
    lt_feat_guard_fn fn = g->next;
    char arg[512];
    g->next = NULL;
    if (fn == NULL) { do_it(app, what); return; }
    lt_strlcpy(arg, g->next_arg, sizeof(arg));
    fn(app, arg);
}

static void act(lt_app *app, guard_state *g, int a)
{
    int what = g->what;
    if (a == LT_GA_CANCEL) { g->next = NULL; return; }
    if (a == LT_GA_BURN) {
        if (lt_app_burn(app) != 0) { return; }             /* burn explains what went wrong */
        if (app->has_project && app->modified && lt_app_save_tune(app) != 0) { return; }
    } else if (a == LT_GA_SAVE) {
        if (lt_app_save_tune(app) != 0) { return; }
    }
    if (what != LT_GUARD_DISCONNECT) { lt_feat_autosave_discard(app); }
    go_on(app, g, what);
}

static void ask_then(lt_app *app, int what, lt_feat_guard_fn fn, const char *arg)
{
    guard_state *g = gs(app);
    int unb, unsaved;
    g->next = fn;
    lt_strlcpy(g->next_arg, arg != NULL ? arg : "", sizeof(g->next_arg));
    if (what != LT_GUARD_DISCONNECT) { (void)lt_feat_prefs_auto_burn(app); }   /* "burn on close" */
    update_unburned(app, g);
    unb = app->conn == LT_APP_ONLINE ? lt_feat_guard_unburned(app) : 0;
    unsaved = app->ini != NULL && app->has_project && app->modified;
    if (!lt_guard_question(what, unb, unsaved, &g->q)) {
        if (what != LT_GUARD_DISCONNECT && app->has_project) { lt_feat_autosave_discard(app); }
        go_on(app, g, what);
        return;
    }
    g->what = what;
    g->open = 1;
}

static void ask(lt_app *app, int what) { ask_then(app, what, NULL, NULL); }

void lt_feat_guard_quit(lt_app *app) { ask(app, LT_GUARD_QUIT); }
void lt_feat_guard_close_project(lt_app *app) { ask(app, LT_GUARD_CLOSE); }
void lt_feat_guard_disconnect(lt_app *app)
{
    if (app->conn == LT_APP_OFFLINE && app->proto == NULL) { return; }
    ask(app, LT_GUARD_DISCONNECT);
}
void lt_feat_guard_switch(lt_app *app, lt_feat_guard_fn fn, const char *arg)
{
    if (fn != NULL) { ask_then(app, LT_GUARD_CLOSE, fn, arg); }
}

/* A question with buttons sized to their labels (lt_ui_msgbox has fixed narrow ones).
 * Returns the 1-based button pressed, 0 while open, -1 when closed with x / Escape. */
static int qbox(lt_app *app, const char *title, const char *msg, const char *buttons, int *open)
{
    lt_ui *ui = app->ui;
    const lt_theme *t = lt_ui_theme(ui);
    int cell = lt_ui_cell_mode(ui), n = 0, i, lines = 0, w = cell ? 72 * 8 : 560, h, avail, res = 0;
    int widths[6];
    char names[5][48], lab[64];
    const char *p = buttons, *q;
    while (*p != '\0' && n < 5) {
        size_t len;
        q = strchr(p, '|');
        len = q != NULL ? (size_t)(q - p) : strlen(p);
        if (len >= sizeof(names[0])) { len = sizeof(names[0]) - 1; }
        memcpy(names[n], p, len);
        names[n++][len] = '\0';
        if (q == NULL) { break; }
        p = q + 1;
    }
    /* height: wrapped lines of each paragraph, estimated from the text width */
    avail = LT_MAX(1, lt_ui_px(ui, w - 48));
    p = msg;
    while (*p != '\0') {
        char para[480];
        size_t len;
        q = strchr(p, '\n');
        len = q != NULL ? (size_t)(q - p) : strlen(p);
        if (len >= sizeof(para)) { len = sizeof(para) - 1; }
        memcpy(para, p, len);
        para[len] = '\0';
        lines += 1 + lt_ui_text_width(ui, para) / avail;
        if (q == NULL) { break; }
        p = q + 1;
    }
    h = cell ? (lines + 7) * 16 : 96 + lines * 20 + 16;
    if (!lt_ui_modal_begin(ui, title, w, h, open)) { return *open ? 0 : -1; }
    lt_ui_label_wrap(ui, msg, t->text);
    lt_ui_layout_space(ui, cell ? 16 : 12);
    widths[0] = -1;
    for (i = 0; i < n; i++) { widths[i + 1] = lt_ui_text_width(ui, names[i]) + (cell ? 32 : lt_ui_px(ui, 28)); }
    lt_ui_layout_row_px(ui, n + 1, widths, 0);
    (void)lt_ui_layout_next(ui);
    for (i = 0; i < n; i++) {
        lt_snprintf(lab, sizeof(lab), "%s##gq%d", names[i], i);
        if (lt_ui_button_ex(ui, lab, i == 0 ? LT_UI_BTN_DEFAULT : 0)) { res = i + 1; }
    }
    lt_ui_modal_end(ui);
    if (res > 0) { *open = 0; lt_ui_request_redraw(ui); }
    else if (!*open) { res = -1; }
    return res;
}

static void overlay(lt_app *app)
{
    guard_state *g = gs(app);
    int res;
    if (g->open) {
        res = qbox(app, g->q.title, g->q.msg, g->q.buttons, &g->open);
        if (res > 0 && res <= g->q.n) {
            g->open = 0;
            act(app, g, g->q.act[res - 1]);
        }
    }
    if (g->rec_open && app->has_project) {
        char msg[400], *path, now[24], age[48], when[64];
        lt_plat_timestamp(now, sizeof(now));
        lt_recent_age(g->rec_when, now, age, sizeof(age));
        if (age[0] >= '0' && age[0] <= '9' && strstr(age, "ago") == NULL) {
            lt_snprintf(when, sizeof(when), "on %s", age);
        } else {
            lt_strlcpy(when, age, sizeof(when));
            if (when[0] >= 'A' && when[0] <= 'Z') { when[0] = (char)(when[0] - 'A' + 'a'); }
        }
        if (age[0] != '\0') {
            lt_snprintf(msg, sizeof(msg), "LibreTune did not close normally last time. It autosaved this project's "
                        "tune %s, with changes that were never saved%s.\n\nRestore that tune? Nothing is "
                        "final until you save it.", when,
                        app->conn == LT_APP_ONLINE ? " (restoring sends them to the ECU)" : "");
        } else {
            lt_snprintf(msg, sizeof(msg), "LibreTune did not close normally last time and autosaved this project's "
                        "tune with changes that were never saved%s.\n\nRestore that tune?",
                        app->conn == LT_APP_ONLINE ? " (restoring sends them to the ECU)" : "");
        }
        res = qbox(app, "Restore unsaved work?", msg, "Restore|Keep the saved tune|Decide later", &g->rec_open);
        if (res == 1) {
            path = lt_feat_autosave_path(app);
            g->rec_open = 0;
            {
                lt_tune *t = path != NULL ? lt_tune_load(path) : NULL;
                if (t == NULL) {
                    lt_app_toast(app, 3, "The autosaved tune cannot be read: %s", lt_err_get());
                } else if (lt_app_load_tune_mem(app, t, "the autosaved tune (save to keep it)") == 0) {
                    app->modified = 1;
                    g->written = 1;
                    g->dirty_since = lt_plat_now_ms();
                }
            }
            lt_free(path);
        } else if (res == 2) {
            g->rec_open = 0;
            lt_feat_autosave_discard(app);
        } else if (res == 3) {
            g->rec_open = 0;
            g->written = 1;                 /* keep the file; the next clean save replaces it */
        }
    }
}

/* ------------------------------------------------------------------ */
/* status bar chip                                                     */
static int chip_text(lt_app *app, char *out, size_t cap)
{
    int unb;
    out[0] = '\0';
    if (app->conn != LT_APP_ONLINE && app->conn != LT_APP_DEMO) { return 0; }
    unb = lt_feat_guard_unburned(app);
    if (unb == 0) { return 0; }
    lt_guard_unburned_text(unb, out, cap);
    return 1;
}

int lt_feat_guard_statusbar_width(lt_app *app)
{
    char txt[64];
    lt_ui *ui = app->ui;
    if (!chip_text(app, txt, sizeof(txt))) { return 0; }
    return lt_ui_px(ui, 22) + lt_ui_text_width(ui, txt) + lt_ui_px(ui, 10) + lt_ui_text_width(ui, "Burn") +
           lt_ui_px(ui, 24);
}

void lt_feat_guard_statusbar(lt_app *app, lt_rect r)
{
    lt_ui *ui = app->ui;
    const lt_theme *t = lt_ui_theme(ui);
    char txt[64];
    int bw, tw, ph;
    lt_rect pill, br;
    if (!chip_text(app, txt, sizeof(txt))) { return; }
    tw = lt_ui_text_width(ui, txt);
    bw = lt_ui_text_width(ui, "Burn") + lt_ui_px(ui, 18);
    ph = LT_MIN(r.h - lt_ui_px(ui, 2), lt_ui_px(ui, 20));
    pill = lt_rect_make(r.x, r.y + (r.h - ph) / 2, r.w - lt_ui_px(ui, 4), ph);
    lt_ui_draw_box(ui, pill, t->warning_bg, t->warning, ph / 2);
    lt_ui_draw_lucide(ui, lt_rect_make(pill.x + lt_ui_px(ui, 4), pill.y, lt_ui_px(ui, 16), pill.h), LT_ICON_FLAME, 11,
                      2.0, t->warning);
    lt_ui_draw_text_in(ui, lt_rect_make(pill.x + lt_ui_px(ui, 22), pill.y, tw + 2, pill.h), txt, t->text, 0);
    br = lt_rect_make(pill.x + pill.w - bw - lt_ui_px(ui, 2), pill.y + lt_ui_px(ui, 2), bw, pill.h - lt_ui_px(ui, 4));
    lt_ui_layout_set_rect(ui, br);
    if (lt_ui_button_ex(ui, "Burn##chip", LT_UI_BTN_ACCENT)) { app->burn_open = 1; }
    lt_ui_tooltip(ui, "These settings are in the ECU's working memory only. Burn writes them to flash so "
                      "they survive switching the ECU off.");
}

void lt_feat_guard_register(lt_app_ext *reg)
{
    lt_ext_add_tick(reg, tick);
    lt_ext_add_overlay(reg, overlay);
}
