/* hotkeys.c - key notation, factory shortcut table, user overrides (see hotkeys.h).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <string.h>
#include "hotkeys.h"
#include "../gui/gui.h"

#define HK_C LT_MOD_CTRL
#define HK_S LT_MOD_SHIFT

static const lt_hk_def core_defs[] = {
    { "file.new",        "New project",          'n', HK_C },
    { "file.load_tune",  "Load tune",            'o', HK_C },
    { "file.save",       "Save tune",            's', HK_C },
    { "file.save_as",    "Save tune as",         's', HK_C | HK_S },
    { "file.burn",       "Burn to ECU",          'b', HK_C },
    { "file.quit",       "Quit",                 'q', HK_C },
    { "conn.connect",    "Connect",              'c', HK_C | HK_S },
    { "conn.disconnect", "Disconnect",           'd', HK_C | HK_S },
    { "view.dashboard",  "Dashboard",            'd', HK_C },
    { "view.sidebar",    "Toggle sidebar",       LT_KEY_F9, 0 },
    { "sidebar.search",  "Search the sidebar",   'k', HK_C },
    { "tools.log",       "Start/stop data log",  'l', HK_C },
    { "app.settings",    "Settings",             ',', HK_C },
    { "tab.close",       "Close tab",            'w', HK_C },
    { "tab.next",        "Next tab",             LT_KEY_TAB, HK_C },
    { "tab.prev",        "Previous tab",         LT_KEY_TAB, HK_C | HK_S },
    { "edit.undo",       "Undo",                 'z', HK_C },
    { "edit.redo",       "Redo",                 'y', HK_C },
    { "edit.cut",        "Cut",                  'x', HK_C },
    { "edit.copy",       "Copy",                 'c', HK_C },
    { "edit.paste",      "Paste",                'v', HK_C },
    { "edit.select_all", "Select all",           'a', HK_C }
};

int lt_hk_core_count(void) { return (int)LT_COUNTOF(core_defs); }
const lt_hk_def *lt_hk_core_def(int i)
{
    return (i >= 0 && i < (int)LT_COUNTOF(core_defs)) ? &core_defs[i] : NULL;
}

const lt_hk_def *lt_hk_core_find(const char *id)
{
    size_t i;
    if (id == NULL) { return NULL; }
    for (i = 0; i < LT_COUNTOF(core_defs); i++) {
        if (strcmp(core_defs[i].id, id) == 0) { return &core_defs[i]; }
    }
    return NULL;
}

/* ---- notation ---- */
static const struct { int key; const char *name; } key_names[] = {
    { LT_KEY_TAB, "Tab" }, { LT_KEY_ENTER, "Enter" }, { LT_KEY_ESCAPE, "Esc" },
    { LT_KEY_BACKSPACE, "Backspace" }, { LT_KEY_DELETE, "Delete" }, { ' ', "Space" },
    { LT_KEY_UP, "Up" }, { LT_KEY_DOWN, "Down" }, { LT_KEY_LEFT, "Left" },
    { LT_KEY_RIGHT, "Right" }, { LT_KEY_HOME, "Home" }, { LT_KEY_END, "End" },
    { LT_KEY_PAGEUP, "PageUp" }, { LT_KEY_PAGEDOWN, "PageDown" }, { LT_KEY_INSERT, "Insert" }
};

const char *lt_hk_format(int key, int mods, char *out, size_t cap)
{
    size_t i;
    char one[8];
    if (cap == 0) { return out; }
    out[0] = '\0';
    if (key == LT_HK_NONE) { (void)lt_strlcpy(out, "None", cap); return out; }
    if (mods & LT_MOD_CTRL) { (void)lt_strlcat(out, "Ctrl+", cap); }
    if (mods & LT_MOD_ALT) { (void)lt_strlcat(out, "Alt+", cap); }
    if (mods & LT_MOD_SHIFT) { (void)lt_strlcat(out, "Shift+", cap); }
    if (mods & LT_MOD_SUPER) { (void)lt_strlcat(out, "Super+", cap); }
    if (key >= LT_KEY_F1 && key <= LT_KEY_F12) {
        lt_snprintf(one, sizeof(one), "F%d", key - LT_KEY_F1 + 1);
        (void)lt_strlcat(out, one, cap);
        return out;
    }
    for (i = 0; i < LT_COUNTOF(key_names); i++) {
        if (key_names[i].key == key) { (void)lt_strlcat(out, key_names[i].name, cap); return out; }
    }
    one[0] = (char)((key >= 'a' && key <= 'z') ? key - 'a' + 'A' : key);
    one[1] = '\0';
    (void)lt_strlcat(out, one, cap);
    return out;
}

lt_bool lt_hk_parse(const char *s, int *key, int *mods)
{
    char tok[24];
    int m = 0, k = LT_HK_NONE;
    const char *p = s;
    size_t i;
    if (s == NULL) { return LT_FALSE; }
    while (*p == ' ') { p++; }
    if (*p == '\0' || lt_stricmp(p, "none") == 0) {
        *key = LT_HK_NONE; *mods = 0;
        return LT_TRUE;
    }
    for (;;) {
        size_t n = 0;
        int last;
        while (p[n] != '\0' && (p[n] != '+' || n == 0 || p[n + 1] == '\0') && n + 1 < sizeof(tok)) {
            tok[n] = p[n];
            n++;
        }
        tok[n] = '\0';
        p += n;
        last = (*p == '\0');
        if (!last) { p++; }               /* the '+' */
        if (lt_stricmp(tok, "ctrl") == 0 || lt_stricmp(tok, "control") == 0) { m |= LT_MOD_CTRL; }
        else if (lt_stricmp(tok, "shift") == 0) { m |= LT_MOD_SHIFT; }
        else if (lt_stricmp(tok, "alt") == 0) { m |= LT_MOD_ALT; }
        else if (lt_stricmp(tok, "super") == 0 || lt_stricmp(tok, "meta") == 0 ||
                 lt_stricmp(tok, "cmd") == 0) { m |= LT_MOD_SUPER; }
        else if (last) {
            k = 0;
            if ((tok[0] == 'F' || tok[0] == 'f') && tok[1] >= '1' && tok[1] <= '9') {
                int fn = tok[1] - '0';
                if (tok[2] >= '0' && tok[2] <= '9') { fn = fn * 10 + (tok[2] - '0'); }
                if (fn >= 1 && fn <= 12 && (tok[2] == '\0' || tok[3] == '\0')) { k = LT_KEY_F1 + fn - 1; }
            }
            for (i = 0; k == 0 && i < LT_COUNTOF(key_names); i++) {
                if (lt_stricmp(tok, key_names[i].name) == 0) { k = key_names[i].key; }
            }
            if (k == 0 && tok[0] != '\0' && tok[1] == '\0' && (unsigned char)tok[0] > 32 &&
                (unsigned char)tok[0] < 127) {
                k = (tok[0] >= 'A' && tok[0] <= 'Z') ? tok[0] - 'A' + 'a' : tok[0];
            }
            if (k == 0) { return LT_FALSE; }
        } else {
            return LT_FALSE;
        }
        if (last) { break; }
    }
    if (k == 0) { return LT_FALSE; }
    *key = k; *mods = m;
    return LT_TRUE;
}

const char *lt_hk_check(int key, int mods)
{
    int fkey = key >= LT_KEY_F1 && key <= LT_KEY_F12;
    if (key == LT_HK_NONE) { return NULL; }
    if (key == LT_KEY_ESCAPE || key == LT_KEY_ENTER) { return "Esc and Enter cannot be shortcuts"; }
    if (!fkey && !(mods & (LT_MOD_CTRL | LT_MOD_ALT))) {
        return "Use Ctrl or Alt with the key, or a function key";
    }
    if ((mods & LT_MOD_CTRL) && !(mods & ~LT_MOD_CTRL) && key >= '1' && key <= '9') {
        return "Ctrl+1..9 switch tabs";
    }
    return NULL;
}

/* ---- overrides ---- */
void lt_hk_set_clear(lt_hk_set *s) { s->n = 0; }

static int ov_find(const lt_hk_set *s, const char *id)
{
    int i;
    for (i = 0; i < s->n; i++) {
        if (strcmp(s->ov[i].id, id) == 0) { return i; }
    }
    return -1;
}

lt_bool lt_hk_get(const lt_hk_set *s, const lt_hk_def *def, int *key, int *mods)
{
    int i = ov_find(s, def->id);
    if (i >= 0) { *key = s->ov[i].key; *mods = s->ov[i].mods; return LT_TRUE; }
    *key = def->key; *mods = def->mods;
    return LT_FALSE;
}

int lt_hk_set_bind(lt_hk_set *s, const char *id, int key, int mods)
{
    int i = ov_find(s, id);
    if (i < 0) {
        if (s->n >= LT_HK_MAX_OV || strlen(id) >= LT_HK_ID_MAX) { return -1; }
        i = s->n++;
        lt_strlcpy(s->ov[i].id, id, sizeof(s->ov[i].id));
    }
    s->ov[i].key = key;
    s->ov[i].mods = (key == LT_HK_NONE) ? 0 : mods;
    return 0;
}

void lt_hk_set_reset(lt_hk_set *s, const char *id)
{
    int i = ov_find(s, id);
    if (i < 0) { return; }
    for (; i < s->n - 1; i++) { s->ov[i] = s->ov[i + 1]; }
    s->n--;
}

int lt_hk_conflict(const lt_hk_set *s, const lt_hk_def *defs, int n, int key, int mods,
                   const char *skip_id)
{
    int i, k = 0, m = 0;
    if (key == LT_HK_NONE) { return -1; }
    for (i = 0; i < n; i++) {
        if (skip_id != NULL && strcmp(defs[i].id, skip_id) == 0) { continue; }
        (void)lt_hk_get(s, &defs[i], &k, &m);
        if (k == key && m == mods) { return i; }
    }
    return -1;
}

int lt_hk_load(lt_hk_set *s, const char *dir)
{
    char path[600];
    char *txt, *line, *next;
    size_t len = 0;
    lt_hk_set_clear(s);
    if (dir == NULL) { return -1; }
    lt_snprintf(path, sizeof(path), "%s/hotkeys.conf", dir);
    txt = lt_file_read(path, &len);
    if (txt == NULL) { return -1; }
    for (line = txt; line != NULL && *line != '\0'; line = next) {
        char *eq;
        int key = 0, mods = 0;
        next = strchr(line, '\n');
        if (next != NULL) { *next++ = '\0'; }
        line = lt_str_trim(line);
        if (line[0] == '#' || line[0] == '\0') { continue; }
        eq = strchr(line, '=');
        if (eq == NULL) { continue; }
        *eq = '\0';
        line = lt_str_trim(line);
        if (lt_hk_parse(lt_str_trim(eq + 1), &key, &mods) && line[0] != '\0') {
            (void)lt_hk_set_bind(s, line, key, mods);
        }
    }
    lt_free(txt);
    return 0;
}

int lt_hk_save(const lt_hk_set *s, const char *dir)
{
    char path[600], buf[LT_HK_MAX_OV * 64 + 64], one[32], line[96];
    int i, rc;
    if (dir == NULL) { return -1; }
    lt_snprintf(path, sizeof(path), "%s/hotkeys.conf", dir);
    lt_strlcpy(buf, "# LibreTune keyboard shortcuts (edited in Settings > Keyboard Shortcuts)\n",
               sizeof(buf));
    for (i = 0; i < s->n; i++) {
        lt_snprintf(line, sizeof(line), "%s=%s\n", s->ov[i].id,
                    lt_hk_format(s->ov[i].key, s->ov[i].mods, one, sizeof(one)));
        lt_strlcat(buf, line, sizeof(buf));
    }
    rc = lt_file_write(path, buf, strlen(buf));
    return rc;
}
