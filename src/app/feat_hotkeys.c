/* feat_hotkeys.c - customizable keyboard shortcuts (G-91) and the Edit actions (G-105):
 *   Settings -> Keyboard Shortcuts   rebind every shell and feature shortcut, conflict check,
 *                                    reset one / all; saved to <config>/hotkeys.conf
 *   lt_hk_pressed / lt_hk_text       what the shell shortcuts and menu hints use
 *   lt_feat_edit_do                  Undo / Redo / Cut / Copy / Paste / Select all routed to the
 *                                    active tab (as the key press the tab itself understands)
 * The table of built-in actions and the override logic are in hotkeys.c.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <string.h>
#include "feat.h"

#define HK_RING 8
#define HK_MAX_DEFS 96

typedef struct hk_state {
    lt_hk_set set;
    int loaded;
    char ring[HK_RING][40];
    int ring_n;
    char tips[HK_RING][96];
    int tip_n;
    int capture;                    /* index into the defs being captured, -1 none */
    int conf_idx;                   /* conflicting capture: action index it collides with, -1 */
    int conf_key, conf_mods;
    char msg[96];
    int edit_pending;               /* queued edit action: 1 + index of the "edit.*" core entry */
} hk_state;

static hk_state *hk(lt_app *app)
{
    hk_state *s = (hk_state *)lt_ext_state(app, "hotkeys", sizeof(hk_state), NULL);
    if (!s->loaded) {
        s->loaded = 1;
        s->capture = -1;
        s->conf_idx = -1;
        if (app->config_dir != NULL) { (void)lt_hk_load(&s->set, app->config_dir); }
    }
    return s;
}

void lt_hk_effective(lt_app *app, const lt_hk_def *def, int *key, int *mods)
{
    (void)lt_hk_get(&hk(app)->set, def, key, mods);
}

int lt_hk_capturing(lt_app *app) { return hk(app)->capture >= 0; }

static int is_edit_id(const char *id) { return strncmp(id, "edit.", 5) == 0; }

int lt_hk_pressed(lt_app *app, const char *id)
{
    const lt_hk_def *d = lt_hk_core_find(id);
    hk_state *s = hk(app);
    int key = 0, mods = 0;
    lt_bool changed;
    if (d == NULL || s->capture >= 0 || app->ui == NULL) { return 0; }
    changed = lt_hk_get(&s->set, d, &key, &mods);
    if (key == LT_HK_NONE) { return 0; }
    /* the tabs and text fields handle the stock edit keys themselves */
    if (is_edit_id(id) && !changed) { return 0; }
    return lt_ui_key_pressed(app->ui, key, mods) ? 1 : 0;
}

const char *lt_hk_text(lt_app *app, const char *id)
{
    hk_state *s = hk(app);
    const lt_hk_def *d = lt_hk_core_find(id);
    lt_hk_def ext[HK_MAX_DEFS];
    char *buf = s->ring[s->ring_n];
    int key = 0, mods = 0, i, n;
    s->ring_n = (s->ring_n + 1) % HK_RING;
    buf[0] = '\0';
    if (d == NULL) {
        n = lt_ext_shortcut_defs(app, ext, HK_MAX_DEFS);
        for (i = 0; i < n; i++) {
            if (strcmp(ext[i].id, id) == 0) {
                (void)lt_hk_get(&s->set, &ext[i], &key, &mods);
                if (key != LT_HK_NONE) { (void)lt_hk_format(key, mods, buf, sizeof(s->ring[0])); }
            }
        }
        return buf;
    }
    (void)lt_hk_get(&s->set, d, &key, &mods);
    if (key != LT_HK_NONE) { (void)lt_hk_format(key, mods, buf, sizeof(s->ring[0])); }
    return buf;
}

/* "Save tune (Ctrl+S)": translated English action plus the current shortcut (valid for a few calls) */
const char *lt_hk_tip(lt_app *app, const char *english, const char *id)
{
    hk_state *s = hk(app);
    const char *key = lt_hk_text(app, id);
    char *buf = s->tips[s->tip_n];
    s->tip_n = (s->tip_n + 1) % HK_RING;
    if (key[0] == '\0') { lt_snprintf(buf, sizeof(s->tips[0]), "%s", lt_tr(english)); }
    else { lt_snprintf(buf, sizeof(s->tips[0]), "%s (%s)", lt_tr(english), key); }
    return buf;
}

/* ------------------------------------------------------------------ */
/* Edit actions                                                        */
static int edit_tab_ok(lt_app *app)
{
    lt_app_tab *t = lt_app_tab_active(app);
    return t != NULL && (strcmp(t->kind, "table") == 0 || strcmp(t->kind, "curve") == 0 ||
                         strcmp(t->kind, "autotune") == 0);
}

void lt_feat_edit_do(lt_app *app, const char *id)
{
    hk_state *s = hk(app);
    int i;
    if (!edit_tab_ok(app)) {
        lt_app_toast(app, 0, "%s", lt_tr("Open a table or curve tab to use the Edit actions"));
        return;
    }
    for (i = 0; i < lt_hk_core_count(); i++) {
        if (strcmp(lt_hk_core_def(i)->id, id) == 0) { s->edit_pending = 1 + i; }
    }
    if (app->ui != NULL) { lt_ui_request_redraw(app->ui); }
}

static void send_key(lt_app *app, int key, int mods)
{
    lt_event ev;
    if (app->ui == NULL) { return; }
    memset(&ev, 0, sizeof(ev));
    ev.type = LT_EV_KEY_DOWN;
    ev.key = key;
    ev.mods = mods;
    lt_ui_input(app->ui, &ev);
}

/* runs between frames, so the key waits in the input queue for the next frame's active tab */
static void edit_tick(lt_app *app)
{
    hk_state *s = hk(app);
    const lt_hk_def *d;
    if (s->edit_pending <= 0) { return; }
    d = lt_hk_core_def(s->edit_pending - 1);
    s->edit_pending = 0;
    if (d == NULL || !edit_tab_ok(app)) { return; }
    send_key(app, d->key, d->mods);                 /* the factory key the tab understands */
    if (strcmp(d->id, "edit.cut") == 0) { send_key(app, LT_KEY_DELETE, 0); }
}

/* user-chosen keys for the Edit actions (the stock keys work in the tab already) */
static void edit_keys(lt_app *app)
{
    static const char *const ids[] = { "edit.undo", "edit.redo", "edit.cut", "edit.copy",
                                       "edit.paste", "edit.select_all" };
    size_t i;
    for (i = 0; i < LT_COUNTOF(ids); i++) {
        if (lt_hk_pressed(app, ids[i])) { lt_feat_edit_do(app, ids[i]); }
    }
}


/* ------------------------------------------------------------------ */
/* editor page                                                         */
static int all_defs(lt_app *app, lt_hk_def *out, int cap)
{
    int n = 0, i;
    for (i = 0; i < lt_hk_core_count() && n < cap; i++) { out[n++] = *lt_hk_core_def(i); }
    n += lt_ext_shortcut_defs(app, out + n, cap - n);
    return n;
}

/* the key that went down this frame (any modifier combination), or 0 */
static int captured(lt_ui *ui, int *mods_out)
{
    static const char punct[] = ",./;'[]-=`\\";
    int mods, k, key;
    for (mods = 0; mods < 8; mods++) {
        int m = ((mods & 1) ? LT_MOD_CTRL : 0) | ((mods & 2) ? LT_MOD_SHIFT : 0) |
                ((mods & 4) ? LT_MOD_ALT : 0);
        for (k = 'a'; k <= 'z'; k++) { if (lt_ui_key_pressed(ui, k, m)) { *mods_out = m; return k; } }
        for (k = '0'; k <= '9'; k++) { if (lt_ui_key_pressed(ui, k, m)) { *mods_out = m; return k; } }
        for (k = 0; punct[k] != '\0'; k++) {
            if (lt_ui_key_pressed(ui, punct[k], m)) { *mods_out = m; return punct[k]; }
        }
        for (k = LT_KEY_F1; k <= LT_KEY_F12; k++) { if (lt_ui_key_pressed(ui, k, m)) { *mods_out = m; return k; } }
        key = LT_KEY_TAB;
        if (lt_ui_key_pressed(ui, key, m)) { *mods_out = m; return key; }
        for (key = LT_KEY_UP; key <= LT_KEY_INSERT; key++) {
            if (lt_ui_key_pressed(ui, key, m)) { *mods_out = m; return key; }
        }
        if (lt_ui_key_pressed(ui, ' ', m)) { *mods_out = m; return ' '; }
        if (lt_ui_key_pressed(ui, LT_KEY_ESCAPE, m)) { *mods_out = m; return LT_KEY_ESCAPE; }
        if (lt_ui_key_pressed(ui, LT_KEY_BACKSPACE, m)) { *mods_out = m; return LT_KEY_BACKSPACE; }
        if (lt_ui_key_pressed(ui, LT_KEY_DELETE, m)) { *mods_out = m; return LT_KEY_DELETE; }
    }
    return 0;
}

static void save_set(lt_app *app, hk_state *s)
{
    if (app->config_dir != NULL) { (void)lt_hk_save(&s->set, app->config_dir); }
}

static void keys_page(lt_app *app)
{
    lt_ui *ui = app->ui;
    hk_state *s = hk(app);
    const lt_theme *t = lt_ui_theme(ui);
    lt_hk_def defs[HK_MAX_DEFS];
    int n = all_defs(app, defs, HK_MAX_DEFS), i, w1 = -1, wr[3], wb[3];
    char txt[40];

    lt_ui_layout_row(ui, 1, &w1, 0);
    lt_ui_text_style(ui, (int)LT_FONT_SANS_SEMIBOLD, LT_UI_FS_BASE);
    lt_ui_label_ex(ui, lt_tr("Keyboard Shortcuts"), t->text, LT_UI_ALIGN_LEFT);
    lt_ui_text_style_reset(ui);
    lt_ui_layout_row(ui, 1, &w1, 0);
    lt_ui_label_wrap(ui, lt_tr("Click a shortcut, then press the new keys. Backspace removes it, Esc cancels."),
                     t->muted);

    if (s->capture >= n) { s->capture = -1; }
    if (s->capture >= 0 && s->conf_idx < 0) {
        int mods = 0, key = captured(ui, &mods);
        if (key == LT_KEY_ESCAPE && mods == 0) {
            s->capture = -1;
        } else if (key != 0) {
            const char *why;
            const lt_hk_def *d = &defs[s->capture];
            s->msg[0] = '\0';
            if ((key == LT_KEY_BACKSPACE || key == LT_KEY_DELETE) && mods == 0) {
                (void)lt_hk_set_bind(&s->set, d->id, LT_HK_NONE, 0);
                save_set(app, s);
                s->capture = -1;
            } else if ((why = lt_hk_check(key, mods)) != NULL) {
                lt_strlcpy(s->msg, lt_tr(why), sizeof(s->msg));
            } else {
                int c = lt_hk_conflict(&s->set, defs, n, key, mods, d->id);
                if (c >= 0) {
                    s->conf_idx = c; s->conf_key = key; s->conf_mods = mods;
                } else {
                    (void)lt_hk_set_bind(&s->set, d->id, key, mods);
                    save_set(app, s);
                    s->capture = -1;
                }
            }
        }
    }
    if (s->conf_idx >= 0 && s->capture >= 0 && s->conf_idx < n) {
        char kt[40], line[200];
        (void)lt_hk_format(s->conf_key, s->conf_mods, kt, sizeof(kt));
        lt_snprintf(line, sizeof(line), "%s %s: %s", kt, lt_tr("is already used by"),
                    lt_tr(defs[s->conf_idx].label));
        lt_ui_layout_row(ui, 1, &w1, 0);
        lt_ui_label_wrap(ui, line, t->warning);
        wb[0] = 120; wb[1] = 120; wb[2] = -1;
        lt_ui_layout_row(ui, 3, wb, 0);
        if (lt_ui_button_ex(ui, lt_tr("Reassign"), LT_UI_BTN_PRIMARY)) {
            (void)lt_hk_set_bind(&s->set, defs[s->conf_idx].id, LT_HK_NONE, 0);
            (void)lt_hk_set_bind(&s->set, defs[s->capture].id, s->conf_key, s->conf_mods);
            save_set(app, s);
            s->conf_idx = -1; s->capture = -1;
        }
        if (lt_ui_button(ui, lt_tr("Cancel"))) { s->conf_idx = -1; s->capture = -1; }
        lt_ui_label(ui, "");
    } else if (s->conf_idx >= 0) {
        s->conf_idx = -1;
    }
    if (s->msg[0] != '\0' && s->capture >= 0) {
        lt_ui_layout_row(ui, 1, &w1, 0);
        lt_ui_label_ex(ui, s->msg, t->warning, LT_UI_ALIGN_LEFT);
    }
    lt_ui_layout_space(ui, 6);

    wr[0] = -1; wr[1] = 170; wr[2] = 80;
    for (i = 0; i < n; i++) {
        int key = 0, mods = 0;
        lt_bool changed = lt_hk_get(&s->set, &defs[i], &key, &mods);
        int cap = (s->capture == i);
        lt_ui_push_id_int(ui, 100 + i);
        lt_ui_layout_row(ui, 3, wr, 0);
        lt_ui_label_ex(ui, lt_tr(defs[i].label), t->text, LT_UI_ALIGN_LEFT);
        if (cap) {
            lt_snprintf(txt, sizeof(txt), "%s", lt_tr("Press keys..."));
        } else {
            (void)lt_hk_format(key, mods, txt, sizeof(txt));
        }
        if (lt_ui_button_ex(ui, txt, cap ? LT_UI_BTN_SELECTED : 0) && !cap) {
            s->capture = i; s->conf_idx = -1; s->msg[0] = '\0';
        }
        if (changed) {
            if (lt_ui_button(ui, lt_tr("Reset"))) {
                lt_hk_set_reset(&s->set, defs[i].id);
                save_set(app, s);
                if (cap) { s->capture = -1; }
            }
        } else {
            lt_ui_label(ui, "");
        }
        lt_ui_pop_id(ui);
    }
    lt_ui_layout_space(ui, 8);
    wb[0] = 160; wb[1] = -1;
    lt_ui_layout_row(ui, 2, wb, 0);
    if (lt_ui_button(ui, lt_tr("Reset all"))) {
        lt_hk_set_clear(&s->set);
        save_set(app, s);
        s->capture = -1; s->conf_idx = -1;
    }
    lt_ui_label_ex(ui, lt_tr("Ctrl+1..9 always switch tabs."), t->muted, LT_UI_ALIGN_LEFT);
}

/* effective shortcut list for the Keyboard Shortcuts dialog: action / key pairs, two per row */
void lt_feat_hotkeys_grid(lt_app *app)
{
    lt_ui *ui = app->ui;
    const lt_theme *t = lt_ui_theme(ui);
    hk_state *s = hk(app);
    lt_hk_def defs[HK_MAX_DEFS];
    int n = all_defs(app, defs, HK_MAX_DEFS), i, w[4];
    char txt[40];
    w[0] = 150; w[1] = -1; w[2] = 150; w[3] = -1;
    for (i = 0; i < n; i += 2) {
        int j;
        lt_ui_layout_row(ui, 4, w, 0);
        for (j = i; j < i + 2; j++) {
            int key = 0, mods = 0;
            if (j >= n) { lt_ui_label(ui, ""); lt_ui_label(ui, ""); continue; }
            (void)lt_hk_get(&s->set, &defs[j], &key, &mods);
            lt_ui_label_ex(ui, key != LT_HK_NONE ? lt_hk_format(key, mods, txt, sizeof(txt)) : "-",
                           t->primary_hover, LT_UI_ALIGN_LEFT);
            lt_ui_label_ex(ui, lt_tr(defs[j].label), t->text, LT_UI_ALIGN_LEFT);
        }
    }
    lt_ui_layout_row(ui, 4, w, 0);
    lt_ui_label_ex(ui, "Ctrl+1..9", t->primary_hover, LT_UI_ALIGN_LEFT);
    lt_ui_label_ex(ui, lt_tr("Go to tab"), t->text, LT_UI_ALIGN_LEFT);
    lt_ui_label_ex(ui, "Ctrl+Left/Right", t->primary_hover, LT_UI_ALIGN_LEFT);
    lt_ui_label_ex(ui, lt_tr("Resize sidebar (terminal)"), t->text, LT_UI_ALIGN_LEFT);
}

void lt_feat_hotkeys_register(lt_app_ext *reg)
{
    lt_ext_add_tick(reg, edit_tick);
    lt_ext_add_overlay(reg, edit_keys);      /* drawn every frame: the key state is valid here */
    lt_ext_add_settings_page(reg, "Keyboard Shortcuts", keys_page);
}
