/* feat_online.c - Search for INI Online, assisted form (G-17).  clibretune has no TLS stack, so it
 * does not download anything itself: the dialog opens the project's INI source page in the
 * browser and watches the Downloads folder; a new *.ini that shows up is offered for import into
 * the INI library.  Reached from the signature-mismatch dialog ("Search Online") and from
 * File -> Search for INI Online...
 *   dialog "ini_online"   target = ECU signature (picks the source, flags matching files)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <string.h>
#include "feat.h"
#include "online_logic.h"
#include "../plat/plat.h"

#define FD_ONLINE_DIR (LT_APP_FD_USER0 + 30)
#define MAX_FOUND 16

typedef struct online_state { int generation; } online_state;

typedef struct found {
    char path[512], name[96], sig[96];
    int imported;
} found;

typedef struct online_dlg {
    lt_ionline_watch watch;
    int watching, src, sel, n_found;
    double next_poll;
    found f[MAX_FOUND];
    char note[256], dir[512], ecu_sig[96];
    int note_ok;
} online_dlg;

int lt_feat_online_generation(lt_app *app)
{
    return ((online_state *)lt_ext_state(app, "online", sizeof(online_state), NULL))->generation;
}

static void add_found(online_dlg *s, const char *path)
{
    found *f;
    char *txt, *sig = NULL, *nm = NULL;
    size_t len = 0;
    int i;
    if (s->n_found >= MAX_FOUND) { return; }
    for (i = 0; i < s->n_found; i++) {
        if (strcmp(s->f[i].path, path) == 0) { return; }
    }
    f = &s->f[s->n_found++];
    memset(f, 0, sizeof(*f));
    lt_strlcpy(f->path, path, sizeof(f->path));
    lt_strlcpy(f->name, lt_path_base(path), sizeof(f->name));
    txt = lt_file_read(path, &len);
    if (txt != NULL) { (void)lt_ini_extract_info(txt, &sig, &nm); }
    if (sig != NULL) { lt_strlcpy(f->sig, sig, sizeof(f->sig)); }
    lt_free(txt); lt_free(sig); lt_free(nm);
}

static void add_paths(online_dlg *s, char **paths, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) { add_found(s, paths[i]); lt_free(paths[i]); }
    lt_free(paths);
}

static void start_watch(online_dlg *s)
{
    char *d = lt_ionline_downloads_dir();
    s->watching = 0;
    s->dir[0] = '\0';
    if (d != NULL) {
        if (lt_ionline_watch_start(&s->watch, d) == 0) {
            s->watching = 1;
            lt_strlcpy(s->dir, d, sizeof(s->dir));
        }
        lt_free(d);
    }
}

static void do_import(lt_app *app, online_dlg *s, int idx)
{
    lt_ini_repo repo;
    char *id = NULL;
    if (idx < 0 || idx >= s->n_found) { return; }
    if (lt_ini_repo_open(&repo, NULL) != 0) {
        lt_snprintf(s->note, sizeof(s->note), "The INI library cannot be opened: %s", lt_err_get());
        s->note_ok = 0;
        return;
    }
    if (lt_ini_repo_import(&repo, s->f[idx].path, &id) == 0) {
        s->f[idx].imported = 1;
        s->note_ok = 1;
        lt_snprintf(s->note, sizeof(s->note), "Imported %s into the INI library.", s->f[idx].name);
        ((online_state *)lt_ext_state(app, "online", sizeof(online_state), NULL))->generation++;
        lt_free(id);
    } else {
        s->note_ok = 0;
        lt_snprintf(s->note, sizeof(s->note), "Import failed: %s", lt_err_get());
    }
    lt_ini_repo_close(&repo);
}

static const char *found_item(void *user, int i)
{
    static char buf[256];
    const online_dlg *s = (const online_dlg *)user;
    const found *f = &s->f[i];
    lt_snprintf(buf, sizeof(buf), "%s%s   %s", f->imported ? "[imported] " : "", f->name, f->sig);
    return buf;
}

static void online_draw(lt_app *app, lt_app_dialog *d)
{
    lt_ui *ui = app->ui;
    const lt_theme *t = lt_ui_theme(ui);
    online_dlg *s = (online_dlg *)d->data;
    int open = d->open, w1 = -1, w2[2], w3[3], i, nsrc = lt_ionline_n();
    const char *names[8] = { NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL };
    const char *picked;
    lt_ionline_src const *src;
    if (s == NULL) {
        s = (online_dlg *)lt_calloc(1, sizeof(*s));
        d->data = s;
        lt_ionline_watch_init(&s->watch);
        lt_strlcpy(s->ecu_sig, d->target, sizeof(s->ecu_sig));
        s->src = lt_ionline_pick(d->target);
        if (s->src < 0) { s->src = 0; }
        start_watch(s);
    }
    picked = lt_app_file_result(app, FD_ONLINE_DIR);
    if (picked != NULL) {
        char dir[512], **paths = NULL;
        size_t n, before = (size_t)s->n_found;
        const char *base = lt_path_base(picked);
        size_t dl = (size_t)(base - picked);
        if (dl >= sizeof(dir)) { dl = sizeof(dir) - 1; }
        memcpy(dir, picked, dl);
        dir[dl] = '\0';
        if (dl > 1) { dir[dl - 1] = '\0'; }          /* drop the trailing separator */
        n = lt_ionline_list(dir[0] != '\0' ? dir : ".", &paths);
        add_paths(s, paths, n);
        s->note_ok = s->n_found > (int)before;
        lt_snprintf(s->note, sizeof(s->note), "%lu ECU definition(s) found in %s",
                    (unsigned long)(s->n_found - (int)before), dir);
    }
    if (s->watching && lt_ui_now(ui) >= s->next_poll) {
        char **paths = NULL;
        size_t n = lt_ionline_watch_poll(&s->watch, &paths);
        s->next_poll = lt_ui_now(ui) + 1000.0;
        if (n > 0) {
            add_paths(s, paths, n);
            s->sel = s->n_found - 1;
            s->note_ok = 1;
            lt_snprintf(s->note, sizeof(s->note), "New INI file detected in your Downloads folder.");
        }
    }
    if (lt_ui_modal_begin(ui, "Search for INI Online", 600, 470, &open)) {
        lt_ui_layout_row(ui, 1, &w1, 0);
        lt_ui_label_wrap(ui, "clibretune opens the project's INI page in your browser. Download the "
                             "file for your firmware there: it is picked up from your Downloads "
                             "folder and can be imported into the INI library.", t->text2);
        if (s->ecu_sig[0] != '\0') {
            w2[0] = 128; w2[1] = -1;
            lt_ui_layout_row(ui, 2, w2, 0);
            lt_ui_label(ui, "ECU signature:");
            lt_ui_label_ex(ui, s->ecu_sig, t->warning, LT_UI_ALIGN_LEFT);
        }
        lt_ui_separator(ui);
        for (i = 0; i < nsrc && i < 8; i++) { names[i] = lt_ionline_get(i)->name; }
        w2[0] = 128; w2[1] = -1;
        lt_ui_layout_row(ui, 2, w2, 0);
        lt_ui_label(ui, "Firmware project:");
        (void)lt_ui_combo(ui, "onsrc", &s->src, names, nsrc);
        src = lt_ionline_get(s->src);
        lt_ui_layout_row(ui, 1, &w1, 0);
        lt_ui_label_ex(ui, src->url, t->primary, LT_UI_ALIGN_LEFT);
        lt_ui_label_wrap(ui, src->hint, t->muted);
        w2[0] = -1; w2[1] = -1;
        lt_ui_layout_row(ui, 2, w2, 0);
        if (lt_ui_button_ex(ui, "Open Source Page", LT_UI_BTN_PRIMARY)) {
            if (lt_plat_open_url(src->url) != 0) {
                s->note_ok = 0;
                lt_snprintf(s->note, sizeof(s->note), "Could not start the browser. Open %s yourself.",
                            src->url);
            }
        }
        if (lt_ui_button(ui, "Copy Address")) { lt_ui_clipboard_set(ui, src->url); }
        lt_ui_separator(ui);
        lt_ui_layout_row(ui, 1, &w1, 0);
        if (s->watching) {
            char line[600];
            lt_snprintf(line, sizeof(line), "Watching %s for new .ini files...", s->dir);
            lt_ui_label_ex(ui, line, t->text2, LT_UI_ALIGN_LEFT);
        } else {
            lt_ui_label_ex(ui, "No Downloads folder was found. Use \"Scan Folder...\" after downloading.",
                           t->warning, LT_UI_ALIGN_LEFT);
        }
        lt_ui_heading(ui, "INI files found");
        lt_ui_layout_row(ui, 1, &w1, 96);
        if (s->n_found > 0) {
            (void)lt_ui_listbox_cb(ui, "onfound", &s->sel, s->n_found, found_item, s);
        } else {
            lt_ui_label_ex(ui, "Nothing yet.", t->muted, LT_UI_ALIGN_LEFT);
        }
        if (s->n_found > 0 && s->sel >= 0 && s->sel < s->n_found && s->ecu_sig[0] != '\0') {
            const found *f = &s->f[s->sel];
            lt_ui_layout_row(ui, 1, &w1, 0);
            if (strcmp(f->sig, s->ecu_sig) == 0) {
                lt_ui_label_ex(ui, "This file matches the ECU signature exactly.", t->success,
                               LT_UI_ALIGN_LEFT);
            } else {
                lt_ui_label_ex(ui, "The signature of this file differs from the ECU's.", t->warning,
                               LT_UI_ALIGN_LEFT);
            }
        }
        if (s->note[0] != '\0') { lt_ui_label_wrap(ui, s->note, s->note_ok ? t->success : t->error); }
        w3[0] = -1; w3[1] = -1; w3[2] = -1;
        lt_ui_layout_row(ui, 3, w3, 0);
        if (lt_ui_button_ex(ui, "Import Selected",
                            s->n_found > 0 && !s->f[LT_CLAMP(s->sel, 0, MAX_FOUND - 1)].imported
                              ? LT_UI_BTN_PRIMARY : LT_UI_BTN_DISABLED)) {
            do_import(app, s, s->sel);
        }
        if (lt_ui_button(ui, "Scan Folder...")) {
            lt_app_file_dialog(app, FD_ONLINE_DIR, LT_UI_FILE_OPEN, s->dir[0] != '\0' ? s->dir : NULL,
                               "", "ECU definitions|*.ini|All files|*");
        }
        if (lt_ui_button_ex(ui, "Close", LT_UI_BTN_DEFAULT)) { open = 0; }
        lt_ui_modal_end(ui);
    }
    d->open = open;
}

static void online_close(lt_app *app, lt_app_dialog *d)
{
    online_dlg *s = (online_dlg *)d->data;
    LT_UNUSED(app);
    if (s != NULL) { lt_ionline_watch_free(&s->watch); lt_free(s); }
    d->data = NULL;
}

void lt_feat_online_open(lt_app *app, const char *signature)
{
    (void)lt_app_dialog_push(app, "ini_online", signature != NULL ? signature : "",
                             "Search for INI Online");
}

static void act_online(lt_app *app)
{
    lt_feat_online_open(app, app->signature);
}

void lt_feat_online_register(lt_app_ext *reg)
{
    lt_app_dialog_kind dk;
    dk.kind = "ini_online"; dk.draw = online_draw; dk.close = online_close;
    lt_ext_add_dialog_kind(reg, &dk);
    lt_ext_add_menu_item(reg, LT_EXT_MENU_FILE, "Search for INI &Online...", NULL, act_online, NULL);
}
