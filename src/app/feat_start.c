/* feat_start.c - the start screen (no project open): one obvious "Connect to my ECU" action,
 * the ECU that is plugged in right now (named by its USB id, with the project it was used
 * with), recent projects with their ECU and when they were last opened, the other ways in
 * (demo, base tune, TunerStudio import, open, manual setup) and what a first connection needs.
 * Recent project details live in recent.conf (recent_logic.h).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <string.h>
#include "feat.h"
#include "recent_logic.h"

typedef struct start_state {
    lt_recent rec;
    int loaded;
    char seen_project[512];          /* project the "last opened" stamp was taken for */
} start_state;

static void start_cleanup(lt_app *app, void *p)
{
    LT_UNUSED(app);
    lt_recent_free(&((start_state *)p)->rec);
}

static start_state *st_get(lt_app *app)
{
    start_state *s = (start_state *)lt_ext_state(app, "start", sizeof(start_state), start_cleanup);
    if (!s->loaded) {
        s->loaded = 1;
        lt_recent_init(&s->rec);
        if (app->config_dir != NULL) { (void)lt_recent_load(&s->rec, app->config_dir); }
    }
    return s;
}

/* a project was opened: remember its name, ECU and the time */
static void tick(lt_app *app)
{
    start_state *s;
    char stamp[24];
    if (!app->has_project || app->project.path == NULL) { return; }
    s = st_get(app);
    if (strcmp(s->seen_project, app->project.path) == 0) { return; }
    lt_strlcpy(s->seen_project, app->project.path, sizeof(s->seen_project));
    lt_plat_timestamp(stamp, sizeof(stamp));
    lt_recent_touch(&s->rec, app->project.path, app->project.cfg.name, app->project.cfg.signature, stamp);
    if (app->config_dir != NULL && !app->headless) { (void)lt_recent_save(&s->rec, app->config_dir); }
}

/* name/signature/opened of a recent path (project.json read once for unknown ones) */
static const lt_recent_ent *details(lt_app *app, const char *path)
{
    start_state *s = st_get(app);
    const lt_recent_ent *e = lt_recent_find(&s->rec, path);
    char name[96], sig[96];
    if (e != NULL) { return e; }
    if (lt_recent_read_project(path, name, sizeof(name), sig, sizeof(sig)) != 0) { return NULL; }
    /* remember without a time stamp, at the end of the list order (order comes from settings) */
    lt_recent_touch(&s->rec, path, name, sig, "");
    return lt_recent_find(&s->rec, path);
}

static void forget(lt_app *app, int idx)
{
    start_state *s = st_get(app);
    lt_app_settings *set = &app->settings;
    int k;
    if (idx < 0 || idx >= set->n_recent) { return; }
    (void)lt_recent_remove(&s->rec, set->recent[idx]);
    for (k = idx; k < set->n_recent - 1; k++) {
        lt_strlcpy(set->recent[k], set->recent[k + 1], sizeof(set->recent[0]));
    }
    set->n_recent--;
    if (!app->headless) {
        lt_app_settings_save(app);
        if (app->config_dir != NULL) { (void)lt_recent_save(&s->rec, app->config_dir); }
    }
    lt_app_toast(app, 0, "Removed from the list. The project folder was not touched.");
}

int lt_feat_start_forget(lt_app *app, const char *dir)
{
    char *a = dir != NULL ? lt_plat_abspath(dir) : NULL;
    int i, found = 0;
    for (i = 0; a != NULL && i < app->settings.n_recent; i++) {
        if (lt_plat_path_same(app->settings.recent[i], a)) {
            forget(app, i);
            found = 1;
            break;
        }
    }
    if (!found && a != NULL) {           /* recent.conf may know it without the list */
        start_state *s = st_get(app);
        if (lt_recent_remove(&s->rec, a) && app->config_dir != NULL && !app->headless) {
            (void)lt_recent_save(&s->rec, app->config_dir);
        }
    }
    lt_free(a);
    return found;
}

static int folder_ok(const char *dir)
{
    char *p = lt_path_join(dir, "project.json");
    int ok = p != NULL && lt_file_exists(p);
    lt_free(p);
    return ok;
}

/* ------------------------------------------------------------------ */
/* actions                                                             */
static void act_open(lt_app *app)
{
    lt_app_file_dialog(app, LT_APP_FD_OPEN_PROJECT, LT_UI_FILE_OPEN, NULL, "project.json",
                       "Project|project.json|All files|*");
}

static void act_import(lt_app *app)
{
    lt_app_file_dialog(app, LT_APP_FD_IMPORT_TS, LT_UI_FILE_OPEN, NULL, "", "All files|*");
}

/* ------------------------------------------------------------------ */
/* pixel UI                                                            */
/* A clickable card: icon, title and up to two lines of text. Returns 1 when clicked. */
static int card(lt_app *app, lt_rect r, const char *id, int icon, const char *title, const char *l1,
                const char *l2, int primary)
{
    lt_ui *ui = app->ui;
    const lt_theme *t = lt_ui_theme(ui);
    int clicked, hv, big = primary, ic = lt_ui_px(ui, big ? 44 : 34), pad = lt_ui_px(ui, 16);
    int tx, ty;
    lt_color fg, fg2, fill;
    lt_rect ir;
    lt_ui_push_id(ui, id);
    lt_ui_layout_set_rect(ui, r);
    clicked = lt_ui_button_ex(ui, "##card", LT_UI_BTN_FLAT);
    hv = lt_ui_last_hovered(ui);
    if (primary) {
        fill = hv ? t->primary_hover : t->primary;
        fg = t->primary_text;
        fg2 = lt_color_mix(t->primary_text, t->primary, 60);
        lt_ui_draw_shadow(ui, r, t->radius_lg, lt_ui_px(ui, 6), 60);
        lt_ui_draw_box(ui, r, fill, fill, t->radius_lg);
    } else {
        fill = hv ? t->hover : t->elevated;
        fg = t->text;
        fg2 = t->muted;
        lt_ui_draw_box(ui, r, fill, hv ? t->border_strong : t->border, t->radius_lg);
    }
    ir = lt_rect_make(r.x + pad, r.y + (r.h - ic) / 2, ic, ic);
    lt_ui_draw_round(ui, ir, ic / 2, primary ? lt_color_mix(t->primary, t->primary_text, 40)
                                             : lt_color_mix(t->elevated, t->primary, 40));
    lt_ui_draw_lucide(ui, ir, icon, big ? 22 : 17, 2.0, primary ? t->primary_text : t->primary);
    tx = ir.x + ic + pad;
    ty = r.y + (r.h - lt_ui_px(ui, big ? 26 + 18 + 18 : 20 + 18)) / 2;
    if (l2 == NULL && big) { ty += lt_ui_px(ui, 9); }
    lt_ui_draw_text_in_f(ui, lt_rect_make(tx, ty, r.x + r.w - tx - pad, lt_ui_px(ui, big ? 26 : 20)), title,
                         fg, 0, (int)LT_FONT_SANS_SEMIBOLD, big ? LT_UI_FS_3XL - 4 : LT_UI_FS_LG);
    ty += lt_ui_px(ui, big ? 28 : 21);
    if (l1 != NULL) {
        lt_ui_draw_text_in_f(ui, lt_rect_make(tx, ty, r.x + r.w - tx - pad, lt_ui_px(ui, 18)), l1, fg2, 0,
                             (int)LT_FONT_SANS, big ? LT_UI_FS_MD : LT_UI_FS_BASE);
        ty += lt_ui_px(ui, 18);
    }
    if (l2 != NULL) {
        lt_ui_draw_text_in_f(ui, lt_rect_make(tx, ty, r.x + r.w - tx - pad, lt_ui_px(ui, 18)), l2, fg2, 0,
                             (int)LT_FONT_SANS, big ? LT_UI_FS_MD : LT_UI_FS_BASE);
    }
    lt_ui_pop_id(ui);
    return clicked;
}

static void small_caps(lt_app *app, lt_rect r, const char *text)
{
    const lt_theme *t = lt_ui_theme(app->ui);
    lt_ui_draw_text_in_f(app->ui, r, text, t->muted, 0, (int)LT_FONT_SANS_SEMIBOLD, LT_UI_FS_SM);
}

/* "rusEFI ECU is plugged in on /dev/ttyACM0" strip; returns its height (0 = nothing shown) */
static int detected_banner(lt_app *app, int x, int y, int w)
{
    lt_ui *ui = app->ui;
    const lt_theme *t = lt_ui_theme(ui);
    const lt_ecuport *ports = NULL;
    lt_ecuport port;
    char project[512], l1[300], l2[300], btn[160], pname[96], sig[96];
    int n = lt_feat_devices_ports(app, &ports, NULL), ecu, h = lt_ui_px(ui, 60), bw;
    lt_rect r, br;
    lt_color edge;
    if (n <= 0) { return 0; }
    ecu = lt_feat_devices_detected(app, &port, project, sizeof(project));
    if (!ecu) { port = ports[0]; project[0] = '\0'; }
    r = lt_rect_make(x, y, w, h);
    edge = ecu ? t->success : t->info;
    lt_ui_draw_box(ui, r, ecu ? t->success_bg : t->info_bg, edge, t->radius_lg);
    lt_ui_draw_lucide(ui, lt_rect_make(r.x + lt_ui_px(ui, 12), r.y, lt_ui_px(ui, 28), r.h), LT_ICON_USB, 20, 2.0,
                      edge);
    if (ecu) {
        lt_snprintf(l1, sizeof(l1), "%s is plugged in on %s", port.label, port.info.name);
    } else {
        lt_snprintf(l1, sizeof(l1), "A USB serial device is plugged in: %s on %s", port.label, port.info.name);
    }
    l2[0] = '\0';
    if (project[0] != '\0') {
        const lt_recent_ent *e = details(app, project);
        lt_strlcpy(pname, e != NULL && e->name[0] != '\0' ? e->name : lt_path_base(project), sizeof(pname));
        lt_snprintf(l2, sizeof(l2), "Last used with the project \"%s\"", pname);
        lt_snprintf(btn, sizeof(btn), "Open \"%s\" and connect", pname);
    } else {
        const lt_ecuport_seen *seen = lt_ecuport_mem_find(lt_feat_devices_mem(app), &port.info);
        sig[0] = '\0';
        if (seen != NULL) { lt_recent_ecu_label(seen->signature, sig, sizeof(sig)); }
        if (sig[0] != '\0') { lt_snprintf(l2, sizeof(l2), "Seen before: %s", sig); }
        else if (port.ids[0] != '\0') { lt_snprintf(l2, sizeof(l2), "USB id %s", port.ids); }
        lt_strlcpy(btn, ecu ? "Connect" : "Look for an ECU", sizeof(btn));
    }
    bw = LT_MAX(lt_ui_px(ui, 120), lt_ui_text_width(ui, btn) + lt_ui_px(ui, 32));
    lt_ui_draw_text_in_f(ui, lt_rect_make(r.x + lt_ui_px(ui, 48), r.y + lt_ui_px(ui, l2[0] ? 11 : 20),
                                          r.w - bw - lt_ui_px(ui, 72), lt_ui_px(ui, 20)),
                         l1, t->text, 0, (int)LT_FONT_SANS_SEMIBOLD, LT_UI_FS_LG);
    if (l2[0] != '\0') {
        lt_ui_draw_text_in_f(ui, lt_rect_make(r.x + lt_ui_px(ui, 48), r.y + lt_ui_px(ui, 32),
                                              r.w - bw - lt_ui_px(ui, 72), lt_ui_px(ui, 18)),
                             l2, t->text2, 0, (int)LT_FONT_SANS, LT_UI_FS_BASE);
    }
    br = lt_rect_make(r.x + r.w - bw - lt_ui_px(ui, 12), r.y + (r.h - lt_ui_px(ui, 32)) / 2, bw, lt_ui_px(ui, 32));
    lt_ui_layout_set_rect(ui, br);
    if (lt_ui_button_ex(ui, btn, ecu ? LT_UI_BTN_PRIMARY : 0)) { lt_feat_devices_connect(app); }
    return h;
}

/* returns the y below the last row drawn */
static int recent_list(lt_app *app, lt_rect col)
{
    lt_ui *ui = app->ui;
    const lt_theme *t = lt_ui_theme(ui);
    int i, y = col.y, rh = lt_ui_px(ui, 62), gap = lt_ui_px(ui, 8);
    char now[24], age[48], ecu[96], line[200];
    small_caps(app, lt_rect_make(col.x, y, col.w, lt_ui_px(ui, 18)), "RECENT PROJECTS");
    y += lt_ui_px(ui, 26);
    if (app->settings.n_recent == 0) {
        lt_rect r = lt_rect_make(col.x, y, col.w, lt_ui_px(ui, 96));
        lt_ui_draw_box(ui, r, t->bg_app, t->border, t->radius_lg);
        lt_ui_draw_lucide(ui, lt_rect_make(r.x, r.y + lt_ui_px(ui, 14), r.w, lt_ui_px(ui, 26)), LT_ICON_FOLDER_OPEN,
                          20, 1.5, t->muted);
        lt_ui_draw_text_in_f(ui, lt_rect_make(r.x, r.y + lt_ui_px(ui, 46), r.w, lt_ui_px(ui, 18)),
                             "No projects yet", t->text2, 1, (int)LT_FONT_SANS_SEMIBOLD, LT_UI_FS_MD);
        lt_ui_draw_text_in_f(ui, lt_rect_make(r.x, r.y + lt_ui_px(ui, 66), r.w, lt_ui_px(ui, 18)),
                             "Projects you create or open appear here.", t->muted, 1, (int)LT_FONT_SANS,
                             LT_UI_FS_BASE);
        return r.y + r.h;
    }
    lt_plat_timestamp(now, sizeof(now));
    for (i = 0; i < app->settings.n_recent; i++) {
        const char *path = app->settings.recent[i];
        const lt_recent_ent *e;
        lt_rect r = lt_rect_make(col.x, y, col.w, rh), xr;
        int ok, hv, clicked;
        if (r.y + r.h > col.y + col.h) { break; }
        ok = folder_ok(path);
        e = ok ? details(app, path) : NULL;
        lt_ui_push_id_int(ui, i);
        xr = lt_rect_make(r.x + r.w - lt_ui_px(ui, 36), r.y + (r.h - lt_ui_px(ui, 28)) / 2, lt_ui_px(ui, 28),
                          lt_ui_px(ui, 28));
        lt_ui_layout_set_rect(ui, lt_rect_make(r.x, r.y, r.w - lt_ui_px(ui, 44), r.h));
        clicked = lt_ui_button_ex(ui, "##recent", LT_UI_BTN_FLAT);
        hv = lt_ui_last_hovered(ui);
        lt_ui_draw_box(ui, r, hv ? t->hover : t->elevated, hv ? t->border_strong : t->border, t->radius_lg);
        lt_ui_draw_lucide(ui, lt_rect_make(r.x + lt_ui_px(ui, 10), r.y, lt_ui_px(ui, 28), r.h),
                          ok ? LT_ICON_FOLDER : LT_ICON_TRIANGLE_ALERT, 18, 1.75, ok ? t->primary : t->warning);
        lt_ui_draw_text_in_f(ui, lt_rect_make(r.x + lt_ui_px(ui, 46), r.y + lt_ui_px(ui, 11), r.w - lt_ui_px(ui, 96),
                                              lt_ui_px(ui, 20)),
                             e != NULL && e->name[0] != '\0' ? e->name : lt_path_base(path), t->text, 0,
                             (int)LT_FONT_SANS_SEMIBOLD, LT_UI_FS_LG);
        if (!ok) {
            lt_snprintf(line, sizeof(line), "Folder not found: %s", path);
        } else {
            ecu[0] = '\0';
            age[0] = '\0';
            if (e != NULL) {
                lt_recent_ecu_label(e->signature, ecu, sizeof(ecu));
                lt_recent_age(e->opened, now, age, sizeof(age));
            }
            lt_snprintf(line, sizeof(line), "%s%s%s", ecu[0] ? ecu : "ECU not known", age[0] ? "   \xC2\xB7   " : "",
                        age);
        }
        lt_ui_draw_text_in_f(ui, lt_rect_make(r.x + lt_ui_px(ui, 46), r.y + lt_ui_px(ui, 33), r.w - lt_ui_px(ui, 96),
                                              lt_ui_px(ui, 18)),
                             line, ok ? t->muted : t->warning, 0, (int)LT_FONT_SANS, LT_UI_FS_BASE);
        if (hv && ok) { lt_ui_tooltip(ui, path); }
        lt_ui_layout_set_rect(ui, xr);
        if (lt_ui_icon_button(ui, LT_UI_LUCIDE(LT_ICON_X), "Remove from this list (the project stays on disk)", 0)) {
            forget(app, i);
            lt_ui_pop_id(ui);
            break;
        }
        lt_ui_pop_id(ui);
        if (clicked && ok) {
            char dir[512];
            lt_strlcpy(dir, path, sizeof(dir));
            (void)lt_app_open_project(app, dir);
            break;
        }
        if (clicked && !ok) { lt_app_toast(app, 2, "The project folder is gone. Remove it from the list with x."); }
        y += rh + gap;
    }
    if (y + lt_ui_px(ui, 28) <= col.y + col.h) {
        lt_ui_layout_set_rect(ui, lt_rect_make(col.x, y + lt_ui_px(ui, 2), col.w, lt_ui_px(ui, 26)));
        if (lt_ui_button_ex(ui, "Open another project...", LT_UI_BTN_FLAT)) { act_open(app); }
        y += lt_ui_px(ui, 28);
    }
    return y;
}

static void needs_box(lt_app *app, lt_rect r)
{
    lt_ui *ui = app->ui;
    const lt_theme *t = lt_ui_theme(ui);
    char cmd[96], l3[200];
    int y = r.y + lt_ui_px(ui, 12), x = r.x + lt_ui_px(ui, 44), w = r.w - lt_ui_px(ui, 56);
    lt_ui_draw_box(ui, r, t->bg_app, t->border_subtle, t->radius_lg);
    lt_ui_draw_lucide(ui, lt_rect_make(r.x + lt_ui_px(ui, 10), r.y + lt_ui_px(ui, 10), lt_ui_px(ui, 24),
                                       lt_ui_px(ui, 24)), LT_ICON_INFO, 16, 1.75, t->info);
    lt_ui_draw_text_in_f(ui, lt_rect_make(x, y, w, lt_ui_px(ui, 18)), "Before you connect", t->text2, 0,
                         (int)LT_FONT_SANS_SEMIBOLD, LT_UI_FS_MD);
    y += lt_ui_px(ui, 22);
    lt_ui_draw_text_in_f(ui, lt_rect_make(x, y, w, lt_ui_px(ui, 18)),
                         "Use the ECU's USB data cable (some cables only charge) and switch the ignition on so "
                         "the ECU has power.", t->muted, 0, (int)LT_FONT_SANS, LT_UI_FS_BASE);
    y += lt_ui_px(ui, 18);
    if (!lt_serial_access_ok()) {
        /* the one step most first connections on Linux fail on: make it stand out */
        lt_ui_draw_lucide(ui, lt_rect_make(r.x + lt_ui_px(ui, 10), y, lt_ui_px(ui, 24), lt_ui_px(ui, 18)),
                          LT_ICON_TRIANGLE_ALERT, 14, 1.75, t->warning);
    }
    if (!lt_serial_access_ok() && lt_serial_perm_fix(cmd, sizeof(cmd))) {
        lt_snprintf(l3, sizeof(l3), "Your user cannot use serial ports yet. Run once:  %s  then log out and in.",
                    cmd);
    } else if (lt_plat_os() == LT_OS_WINDOWS) {
        lt_strlcpy(l3, "Windows: rusEFI and Speeduino need no driver. Close TunerStudio first: only one "
                       "program can use the port.", sizeof(l3));
    } else {
        lt_strlcpy(l3, "Only one program can use the ECU's port at a time: close TunerStudio first.", sizeof(l3));
    }
    lt_ui_draw_text_in_f(ui, lt_rect_make(x, y, w, lt_ui_px(ui, 18)), l3, lt_serial_access_ok() ? t->muted : t->text,
                         0, (int)LT_FONT_SANS, LT_UI_FS_BASE);
}

static void start_pixels(lt_app *app, lt_rect area)
{
    lt_ui *ui = app->ui;
    const lt_theme *t = lt_ui_theme(ui);
    int W = LT_MIN(area.w - lt_ui_px(ui, 64), lt_ui_px(ui, 1040));
    int x0 = area.x + (area.w - W) / 2, y, lw, rw, gap = lt_ui_px(ui, 40), ch = lt_ui_px(ui, 64);
    int cgap = lt_ui_px(ui, 10), bottom, hb;
    lt_rect lc, rc;
    lt_ui_draw_fill(ui, area, t->bg_app);
    y = area.y + LT_MAX(lt_ui_px(ui, 28), (area.h - lt_ui_px(ui, 720)) / 3);
    lt_ui_draw_text_in_f(ui, lt_rect_make(area.x, y, area.w, lt_ui_px(ui, 44)), "LibreTune", t->text, 1,
                         (int)LT_FONT_SANS_SEMIBOLD, 34);
    lt_ui_draw_text_in_f(ui, lt_rect_make(area.x, y + lt_ui_px(ui, 44), area.w, lt_ui_px(ui, 22)),
                         "Open-source ECU tuning for rusEFI, FOME, Speeduino and MegaSquirt", t->muted, 1,
                         (int)LT_FONT_SANS, LT_UI_FS_LG);
    y += lt_ui_px(ui, 88);
    hb = detected_banner(app, x0, y, W);
    if (hb > 0) { y += hb + lt_ui_px(ui, 20); }
    if (W < lt_ui_px(ui, 760)) {
        /* narrow window: one column, recent projects below */
        lw = W; rw = W;
    } else {
        lw = (W - gap) * 52 / 100;
        rw = W - gap - lw;
    }
    bottom = area.y + area.h - lt_ui_px(ui, 24) - lt_ui_px(ui, 80);
    lc = lt_rect_make(x0, y, lw, bottom - y);
    small_caps(app, lt_rect_make(lc.x, y, lw, lt_ui_px(ui, 18)), "GET STARTED");
    y += lt_ui_px(ui, 26);
    if (card(app, lt_rect_make(lc.x, y, lw, lt_ui_px(ui, 100)), "connect", LT_ICON_PLUG_ZAP, "Connect to my ECU",
             "Plug in the USB cable. LibreTune finds the ECU, picks its", "definition and sets up the project.", 1)) {
        lt_feat_devices_connect(app);
    }
    y += lt_ui_px(ui, 100) + cgap + lt_ui_px(ui, 4);
    if (card(app, lt_rect_make(lc.x, y, lw, ch), "demo", LT_ICON_GAUGE, "Try the demo",
             "A simulated engine: every screen works, nothing can break.", NULL, 0)) {
        lt_app_start_demo(app, NULL);
        return;
    }
    y += ch + cgap;
    if (card(app, lt_rect_make(lc.x, y, lw, ch), "base", LT_ICON_ROCKET, "Start from a base tune",
             "Speeduino base maps for common engines (Miata, BMW, Audi...).", NULL, 0)) {
        lt_feat_basetune_open(app, "new");
    }
    y += ch + cgap;
    if (card(app, lt_rect_make(lc.x, y, lw, ch), "import", LT_ICON_FILE_DOWN, "Import a TunerStudio project",
             "Bring an existing project, its tune and settings across.", NULL, 0)) {
        act_import(app);
    }
    y += ch + cgap;
    lt_ui_layout_set_rect(ui, lt_rect_make(lc.x, y, lw / 2 - lt_ui_px(ui, 4), lt_ui_px(ui, 28)));
    if (lt_ui_button_ex(ui, "Open a project folder...", LT_UI_BTN_FLAT)) { act_open(app); }
    lt_ui_layout_set_rect(ui, lt_rect_make(lc.x + lw / 2 + lt_ui_px(ui, 4), y, lw / 2 - lt_ui_px(ui, 4),
                                           lt_ui_px(ui, 28)));
    if (lt_ui_button_ex(ui, "Set up a project manually...", LT_UI_BTN_FLAT)) { lt_app_new_project_dialog(app); }
    lt_ui_tooltip(ui, "Choose the ECU definition (INI) and port yourself");
    y += lt_ui_px(ui, 36);
    if (lw == W) {
        rc = lt_rect_make(x0, y + lt_ui_px(ui, 8), rw, bottom - y - lt_ui_px(ui, 8));
    } else {
        rc = lt_rect_make(x0 + lw + gap, lc.y, rw, bottom - lc.y);
    }
    {
        int yr = recent_list(app, rc), yb = LT_MAX(lw == W ? yr : LT_MAX(y, yr), y) + lt_ui_px(ui, 16);
        /* once an ECU has answered on this computer the hints are old news */
        if (lt_feat_devices_mem(app)->n == 0 || !lt_serial_access_ok()) {
            needs_box(app, lt_rect_make(x0, LT_MIN(yb, bottom + lt_ui_px(ui, 8)), W, lt_ui_px(ui, 72)));
        }
    }
}

/* ------------------------------------------------------------------ */
/* terminal UI                                                         */
static void start_cells(lt_app *app, lt_rect area)
{
    lt_ui *ui = app->ui;
    const lt_theme *t = lt_ui_theme(ui);
    int cw = LT_MIN(area.w - 32, 640), i, w1 = -1, w2[2];
    lt_rect r;
    lt_ecuport port;
    char buf[640], ecu[96];
    r = lt_rect_make(area.x + (area.w - cw) / 2, area.y + 16, cw, area.h - 16);
    lt_ui_draw_fill(ui, area, t->bg_app);
    lt_ui_draw_text_in(ui, lt_rect_make(r.x, r.y, r.w, 16), "LibreTune", t->primary, 1);
    lt_ui_draw_text_in(ui, lt_rect_make(r.x, r.y + 16, r.w, 16), "Open-source ECU tuning", t->text2, 1);
    r.y += 48;
    r.h = area.y + area.h - r.y;
    lt_ui_region_begin(ui, r, 0);
    lt_ui_layout_row(ui, 1, &w1, 0);
    if (lt_feat_devices_detected(app, &port, NULL, 0)) {
        lt_snprintf(buf, sizeof(buf), "%s is plugged in on %s", port.label, port.info.name);
        lt_ui_label_ex(ui, buf, t->success, LT_UI_ALIGN_LEFT);
    }
    if (lt_ui_button_ex(ui, "Connect to my ECU", LT_UI_BTN_PRIMARY)) { lt_feat_devices_connect(app); }
    w2[0] = -1; w2[1] = -1;
    lt_ui_layout_row(ui, 2, w2, 0);
    if (lt_ui_button(ui, "Try the demo")) { lt_app_start_demo(app, NULL); lt_ui_region_end(ui); return; }
    if (lt_ui_button(ui, "Start from a base tune")) { lt_feat_basetune_open(app, "new"); }
    if (lt_ui_button(ui, "Import TunerStudio project")) { act_import(app); }
    if (lt_ui_button(ui, "Open project...")) { act_open(app); }
    lt_ui_layout_row(ui, 1, &w1, 0);
    if (lt_ui_button_ex(ui, "Set up a project manually...", LT_UI_BTN_FLAT)) { lt_app_new_project_dialog(app); }
    lt_ui_layout_space(ui, 16);
    lt_ui_heading(ui, "Recent Projects");
    if (app->settings.n_recent == 0) {
        lt_ui_label_ex(ui, "Projects you create or open appear here.", t->muted, LT_UI_ALIGN_LEFT);
    }
    for (i = 0; i < app->settings.n_recent; i++) {
        const lt_recent_ent *e = details(app, app->settings.recent[i]);
        int w3[2];
        w3[0] = -1; w3[1] = 8 * 10;
        lt_ui_layout_row(ui, 2, w3, 0);
        lt_ui_push_id_int(ui, i);
        ecu[0] = '\0';
        if (e != NULL) { lt_recent_ecu_label(e->signature, ecu, sizeof(ecu)); }
        lt_snprintf(buf, sizeof(buf), "%s   %s", e != NULL && e->name[0] ? e->name : lt_path_base(app->settings.recent[i]),
                    ecu);
        if (lt_ui_button_ex(ui, buf, LT_UI_BTN_FLAT)) {
            char dir[512];
            lt_strlcpy(dir, app->settings.recent[i], sizeof(dir));
            lt_ui_pop_id(ui);
            (void)lt_app_open_project(app, dir);
            break;
        }
        if (lt_ui_button_ex(ui, "Remove", LT_UI_BTN_FLAT)) { forget(app, i); lt_ui_pop_id(ui); break; }
        lt_ui_pop_id(ui);
    }
    lt_ui_region_end(ui);
}

void lt_feat_start_draw(lt_app *app, lt_rect area)
{
    if (lt_ui_cell_mode(app->ui)) { start_cells(app, area); }
    else { start_pixels(app, area); }
    /* notice an ECU being plugged in without any input: look again once a second */
    if (app->anim_ms <= 0.0 || app->anim_ms > 1000.0) { app->anim_ms = 1000.0; }
}

void lt_feat_start_register(lt_app_ext *reg)
{
    lt_ext_add_tick(reg, tick);
}
