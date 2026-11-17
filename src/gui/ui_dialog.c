/* ui_dialog.c - file open/save dialog built from the toolkit's own widgets
 * (no native dialog is available). Uses lt_plat_list_dir / lt_plat_is_dir.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdlib.h>
#include <string.h>
#include "ui_priv.h"
#include "../plat/plat.h"

#define FD_MAX_FILTERS 12

static void free_names(lt_ui_filedlg *d)
{
    if (d->names != NULL) {
        lt_plat_free_list(d->names, d->n_names);
    }
    lt_free(d->is_dir);
    d->names = NULL;
    d->is_dir = NULL;
    d->n_names = 0;
}

void lt_ui_filedlg_init(lt_ui_filedlg *d)
{
    memset(d, 0, sizeof(*d));
    d->sel = -1;
}

void lt_ui_filedlg_free(lt_ui_filedlg *d)
{
    free_names(d);
}

void lt_ui_filedlg_open(lt_ui_filedlg *d, int mode, const char *start_dir, const char *default_name,
                        const char *filters)
{
    char *pwd = NULL;
    free_names(d);
    d->open = 1;
    d->mode = mode;
    d->sel = -1;
    d->dirty = 1;
    d->confirm = 0;
    d->fresh = 1;
    d->err[0] = '\0';
    d->path[0] = '\0';
    d->filter = 0;
    if (start_dir == NULL || start_dir[0] == '\0') {
        pwd = lt_plat_getenv("PWD");
        start_dir = pwd != NULL ? pwd : ".";
    }
    lt_strlcpy(d->dir, start_dir, sizeof(d->dir));
    lt_strlcpy(d->pathbar, d->dir, sizeof(d->pathbar));
    lt_strlcpy(d->name, default_name != NULL ? default_name : "", sizeof(d->name));
    lt_strlcpy(d->filters, filters != NULL ? filters : "All files|*", sizeof(d->filters));
    lt_free(pwd);
}

/* ---- filters ---- */
static int parse_filters(const char *f, char names[][64], char pats[][160], int cap)
{
    int n = 0;
    const char *p = f;
    while (*p && n < cap) {
        const char *e = strchr(p, '|');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        if (len >= 64) { len = 63; }
        memcpy(names[n], p, len);
        names[n][len] = '\0';
        if (e == NULL) { lt_strlcpy(pats[n], "*", 160); n++; break; }
        p = e + 1;
        e = strchr(p, '|');
        len = e ? (size_t)(e - p) : strlen(p);
        if (len >= 160) { len = 159; }
        memcpy(pats[n], p, len);
        pats[n][len] = '\0';
        n++;
        if (e == NULL) { break; }
        p = e + 1;
    }
    if (n == 0) { lt_strlcpy(names[0], "All files", 64); lt_strlcpy(pats[0], "*", 160); n = 1; }
    return n;
}

static int pattern_match(const char *name, const char *pats)
{
    const char *p = pats;
    while (*p) {
        const char *e = strchr(p, ';');
        char tok[64];
        size_t len = e ? (size_t)(e - p) : strlen(p);
        if (len >= sizeof(tok)) { len = sizeof(tok) - 1; }
        memcpy(tok, p, len);
        tok[len] = '\0';
        lt_str_trim(tok);
        if (strcmp(tok, "*") == 0 || strcmp(tok, "*.*") == 0) { return 1; }
        if (tok[0] == '*') {
            const char *ext = tok + 1;
            size_t nl = strlen(name), el = strlen(ext);
            if (nl >= el && lt_stricmp(name + nl - el, ext) == 0) { return 1; }
        } else if (tok[0] != '\0' && lt_stricmp(name, tok) == 0) {
            return 1;
        }
        if (!e) { break; }
        p = e + 1;
    }
    return 0;
}

/* ---- paths ---- */
static int is_sep(char c) { return c == '/' || c == '\\'; }

static char sep_for(const char *dir)
{
    return (strchr(dir, '\\') != NULL && strchr(dir, '/') == NULL) ? '\\' : '/';
}

static void join(char *out, size_t cap, const char *dir, const char *name)
{
    size_t n = strlen(dir);
    lt_strlcpy(out, dir, cap);
    if (n > 0 && !is_sep(dir[n - 1])) {
        char s[2];
        s[0] = sep_for(dir); s[1] = '\0';
        lt_strlcat(out, s, cap);
    }
    lt_strlcat(out, name, cap);
}

static int has_parent(const char *dir)
{
    size_t n = strlen(dir);
    while (n > 1 && is_sep(dir[n - 1])) { n--; }
    if (n <= 1) { return 0; }
    if (n == 3 && dir[1] == ':') { return 0; }
    return 1;
}

static void parent_dir(char *dir)
{
    size_t n = strlen(dir);
    while (n > 1 && is_sep(dir[n - 1])) { dir[--n] = '\0'; }
    while (n > 0 && !is_sep(dir[n - 1])) { n--; }
    if (n == 0) { lt_strlcpy(dir, ".", 2); return; }
    if (n > 1) { n--; }
    if (n == 2 && dir[1] == ':') { n = 3; }
    dir[n] = '\0';
}

static void rescan(lt_ui_filedlg *d)
{
    char **raw = NULL;
    int n, i, k = 0, nf;
    char fnames[FD_MAX_FILTERS][64], fpats[FD_MAX_FILTERS][160];
    char full[1024];
    int *isd;
    char **out;
    int up = has_parent(d->dir);
    free_names(d);
    nf = parse_filters(d->filters, fnames, fpats, FD_MAX_FILTERS);
    if (d->filter >= nf) { d->filter = 0; }
    n = lt_plat_list_dir(d->dir, &raw);
    if (n < 0) {
        lt_strlcpy(d->err, "Cannot read directory", sizeof(d->err));
        n = 0;
        raw = NULL;
    } else {
        d->err[0] = '\0';
    }
    out = (char **)lt_calloc((size_t)(n + 2), sizeof(char *));
    isd = (int *)lt_calloc((size_t)(n + 2), sizeof(int));
    if (up) { out[k] = lt_strdup("\x01.."); isd[k] = 1; k++; }
    /* directories first */
    for (i = 0; i < n; i++) {
        char tmp[300];
        if (raw[i][0] == '.' && !d->show_hidden) { continue; }
        join(full, sizeof(full), d->dir, raw[i]);
        if (lt_plat_is_dir(full)) {
            lt_snprintf(tmp, sizeof(tmp), "\x01%s", raw[i]);
            out[k] = lt_strdup(tmp); isd[k] = 1; k++;
        }
    }
    for (i = 0; i < n; i++) {
        char tmp[300];
        if (raw[i][0] == '.' && !d->show_hidden) { continue; }
        join(full, sizeof(full), d->dir, raw[i]);
        if (!lt_plat_is_dir(full) && pattern_match(raw[i], fpats[d->filter])) {
            lt_snprintf(tmp, sizeof(tmp), "\x02%s", raw[i]);
            out[k] = lt_strdup(tmp); isd[k] = 0; k++;
        }
    }
    if (raw != NULL) { lt_plat_free_list(raw, n); }
    d->names = out;
    d->is_dir = isd;
    d->n_names = k;
    d->sel = -1;
    d->dirty = 0;
}

static const char *fd_get(void *user, int i)
{
    lt_ui_filedlg *d = (lt_ui_filedlg *)user;
    return d->names[i];
}

static const char *filter_get(void *user, int i)
{
    static char names[FD_MAX_FILTERS][64];
    static char pats[FD_MAX_FILTERS][160];
    parse_filters((const char *)user, names, pats, FD_MAX_FILTERS);
    return names[i];
}

static void go_dir(lt_ui_filedlg *d, const char *dir)
{
    if (strlen(dir) >= sizeof(d->dir)) {
        lt_strlcpy(d->err, "Path too long", sizeof(d->err));
        return;
    }
    lt_strlcpy(d->dir, dir, sizeof(d->dir));
    lt_strlcpy(d->pathbar, d->dir, sizeof(d->pathbar));
    d->dirty = 1;
    d->sel = -1;
}

static int try_accept(lt_ui_filedlg *d)
{
    char full[1024];
    if (d->name[0] == '\0') { lt_strlcpy(d->err, "Enter a file name", sizeof(d->err)); return 0; }
    if (is_sep(d->name[0]) || (d->name[0] != '\0' && d->name[1] == ':')) {
        lt_strlcpy(full, d->name, sizeof(full));
    } else {
        join(full, sizeof(full), d->dir, d->name);
    }
    if (strlen(full) >= sizeof(d->path)) {
        lt_strlcpy(d->err, "Path too long", sizeof(d->err));
        return 0;
    }
    if (lt_plat_is_dir(full)) { go_dir(d, full); d->name[0] = '\0'; return 0; }
    if (d->mode == LT_UI_FILE_OPEN && !lt_file_exists(full)) {
        lt_strlcpy(d->err, "File does not exist", sizeof(d->err));
        return 0;
    }
    if (d->mode == LT_UI_FILE_SAVE && lt_file_exists(full) && !d->confirm) {
        d->confirm = 1;
        lt_strlcpy(d->path, full, sizeof(d->path));
        return 0;
    }
    lt_strlcpy(d->path, full, sizeof(d->path));
    return 1;
}

int lt_ui_file_dialog(lt_ui *ui, lt_ui_filedlg *d, const char *title)
{
    const lt_theme *t = &ui->theme;
    int w = 640, h = ui->cell ? 384 : 460, result = 0, open = d->open, nf, i;
    int widths[6], fl;
    char fnames[FD_MAX_FILTERS][64], fpats[FD_MAX_FILTERS][160];
    int footer, acted = 0;

    if (!d->open) { return 0; }
    if (d->dirty || d->names == NULL) { rescan(d); }
    nf = parse_filters(d->filters, fnames, fpats, FD_MAX_FILTERS);
    LT_UNUSED(nf);
    if (!lt_ui_modal_begin(ui, title, w, h, &open)) {
        d->open = 0;
        return -1;
    }
    lt_ui_push_id(ui, "filedlg");
    if (d->fresh) {
        /* start typing the file name right away */
        ui->focus_req = 0;
        ui->focus = lt_ui_id(ui, "name");
        ui->kbd_nav = 0;
        d->fresh = 0;
    }
    /* path bar */
    widths[0] = ui->cell ? 56 : 64; widths[1] = -1; widths[2] = ui->cell ? 72 : 80;
    lt_ui_layout_row(ui, 3, widths, 0);
    if (lt_ui_button_ex(ui, "Up##up", has_parent(d->dir) ? 0 : LT_UI_BTN_DISABLED)) {
        char tmp[512];
        lt_strlcpy(tmp, d->dir, sizeof(tmp));
        parent_dir(tmp);
        go_dir(d, tmp);
    }
    fl = lt_ui_text(ui, "path", d->pathbar, sizeof(d->pathbar), 0);
    if (fl & LT_UI_SUBMIT) {
        char tmp[512];
        lt_strlcpy(tmp, d->pathbar, sizeof(tmp));
        lt_str_trim(tmp);
        if (lt_plat_is_dir(tmp)) {
            size_t n = strlen(tmp);
            while (n > 1 && is_sep(tmp[n - 1])) { tmp[--n] = '\0'; }
            go_dir(d, tmp);
        } else {
            lt_strlcpy(d->err, "Not a directory", sizeof(d->err));
        }
    }
    {
        int hid = d->show_hidden;
        if (lt_ui_checkbox(ui, "Hidden##hid", &hid)) {
            d->show_hidden = hid;
            d->dirty = 1;
        }
    }
    /* listing */
    footer = 2 * (t->row_h + t->spacing_y) + 1 + (d->err[0] ? 0 : 0);
    {
        int wfull = -1;
        ui_layout_row_dev(ui, 1, &wfull, -footer);
    }
    fl = lt_ui_listbox_cb(ui, "files", &d->sel, d->n_names, fd_get, d);
    if (d->sel >= 0 && d->sel < d->n_names && (fl & LT_UI_CHANGED) && !d->is_dir[d->sel]) {
        lt_strlcpy(d->name, d->names[d->sel] + 1, sizeof(d->name));
        d->err[0] = '\0';
    }
    if ((fl & LT_UI_ACTIVATED) && d->sel >= 0 && d->sel < d->n_names) {
        if (d->is_dir[d->sel]) {
            char tmp[1024];
            if (strcmp(d->names[d->sel] + 1, "..") == 0) {
                lt_strlcpy(tmp, d->dir, sizeof(tmp));
                parent_dir(tmp);
            } else {
                join(tmp, sizeof(tmp), d->dir, d->names[d->sel] + 1);
            }
            go_dir(d, tmp);
            d->name[0] = '\0';
        } else {
            lt_strlcpy(d->name, d->names[d->sel] + 1, sizeof(d->name));
            if (!acted && try_accept(d)) { result = 1; }
            acted = 1;
        }
    }
    /* file name + filter */
    widths[0] = ui->cell ? 64 : 72; widths[1] = -1; widths[2] = ui->cell ? 200 : 200;
    lt_ui_layout_row(ui, 3, widths, 0);
    lt_ui_label_ex(ui, "Name", t->text2, LT_UI_ALIGN_LEFT);
    fl = lt_ui_text(ui, "name", d->name, sizeof(d->name), 0);
    if (fl & LT_UI_SUBMIT) {
        if (!acted) {
            d->err[0] = '\0';
            if (try_accept(d)) { result = 1; }
        }
        acted = 1;
    }
    {
        int f = d->filter;
        if (lt_ui_combo_cb(ui, "filter", &f, nf, filter_get, d->filters)) {
            d->filter = f;
            d->dirty = 1;
        }
    }
    /* buttons / status */
    widths[0] = -1; widths[1] = ui->cell ? 80 : 96; widths[2] = ui->cell ? 80 : 96;
    lt_ui_layout_row(ui, 3, widths, 0);
    if (d->err[0]) { lt_ui_label_ex(ui, d->err, t->error, LT_UI_ALIGN_LEFT); }
    else {
        char info[64];
        lt_snprintf(info, sizeof(info), "%d items", d->n_names);
        lt_ui_label_ex(ui, info, t->muted, LT_UI_ALIGN_LEFT);
    }
    if (lt_ui_button_ex(ui, d->mode == LT_UI_FILE_SAVE ? "Save##ok" : "Open##ok", LT_UI_BTN_DEFAULT) && !acted) {
        d->err[0] = '\0';
        if (d->sel >= 0 && d->sel < d->n_names && d->is_dir[d->sel] && d->name[0] == '\0') {
            char tmp[1024];
            if (strcmp(d->names[d->sel] + 1, "..") == 0) {
                lt_strlcpy(tmp, d->dir, sizeof(tmp));
                parent_dir(tmp);
            } else {
                join(tmp, sizeof(tmp), d->dir, d->names[d->sel] + 1);
            }
            go_dir(d, tmp);
        } else if (!acted && try_accept(d)) {
            result = 1;
        }
        acted = 1;
    }
    if (lt_ui_button(ui, "Cancel##cancel")) { open = 0; }
    lt_ui_pop_id(ui);
    lt_ui_modal_end(ui);
    /* overwrite confirmation */
    if (d->confirm) {
        int copen = 1;
        int r = lt_ui_msgbox(ui, "overwrite", "Confirm overwrite",
                             "The file already exists. Do you want to replace it?",
                             "Replace|Cancel", &copen);
        if (r == 1) { d->confirm = 0; result = 1; }
        else if (r != 0) { d->confirm = 0; }
    }
    (void)i;
    if (result == 1) {
        d->open = 0;
        ui_request(ui);
        return 1;
    }
    if (!open) {
        d->open = 0;
        ui_request(ui);
        return -1;
    }
    return 0;
}
