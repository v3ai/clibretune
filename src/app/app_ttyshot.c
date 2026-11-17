/* app_ttyshot.c - cell-mode (terminal UI) snapshot harness helpers.
 *
 *   LT_SHOT_TTY=out.ppm     render the app headlessly as a character grid and
 *                           write an image (glyphs drawn with Plex Mono + procedural
 *                           box/block characters)
 *   LT_SHOT_TEXT=out.txt    also/instead: plain text dump of the grid
 *   LT_SHOT_ANSI=out.ans    also/instead: the escape stream a real terminal would get
 *   LT_SHOT_SIZE=COLSxROWS  grid size (default 120x40)
 *   LT_SHOT_COLORS=24|256|16|1   color depth to preview (default 24 = truecolor)
 *   LT_SHOT_ASCII=1         preview the ASCII fallback (no Unicode glyphs)
 *   LT_SHOT_BASIC=1         preview the Linux-console glyph subset (no rounded corners etc.)
 *   LT_SHOT_KEYS="alt+f,down,enter,text:abc,click:10/3,wheel:20/10/-1"
 *                           scripted input (separated by commas or spaces) fed
 *                           after the first frames; coordinates are cells col/row
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app.h"
#include "app_ttyshot.h"

static int env_set(const char *name)
{
    char *v = lt_plat_getenv(name);
    int r = v != NULL && v[0] != '\0';
    lt_free(v);
    return r;
}

lt_bool lt_app_shot_is_tty(void)
{
    return (env_set("LT_SHOT_TTY") || env_set("LT_SHOT_TEXT") || env_set("LT_SHOT_ANSI")) ? LT_TRUE
                                                                                          : LT_FALSE;
}

static int key_by_name(const char *s)
{
    static const struct { const char *n; int k; } tab[] = {
        { "enter", LT_KEY_ENTER }, { "return", LT_KEY_ENTER }, { "esc", LT_KEY_ESCAPE },
        { "tab", LT_KEY_TAB }, { "bksp", LT_KEY_BACKSPACE }, { "del", LT_KEY_DELETE },
        { "up", LT_KEY_UP }, { "down", LT_KEY_DOWN }, { "left", LT_KEY_LEFT },
        { "right", LT_KEY_RIGHT }, { "home", LT_KEY_HOME }, { "end", LT_KEY_END },
        { "pgup", LT_KEY_PAGEUP }, { "pgdn", LT_KEY_PAGEDOWN }, { "ins", LT_KEY_INSERT },
        { "space", ' ' }, { "comma", ',' }
    };
    size_t i;
    for (i = 0; i < LT_COUNTOF(tab); i++) {
        if (strcmp(s, tab[i].n) == 0) { return tab[i].k; }
    }
    if (s[0] == 'f' && s[1] >= '1' && s[1] <= '9') {
        int n = atoi(s + 1);
        if (n >= 1 && n <= 12) { return LT_KEY_F1 + n - 1; }
    }
    if (s[0] != '\0' && s[1] == '\0') { return (unsigned char)s[0]; }
    return 0;
}

static lt_event *ev_new(lt_event *out, int *n, lt_ev_type type, int col, int row)
{
    lt_event *e = &out[(*n)++];
    memset(e, 0, sizeof(*e));
    e->type = type;
    e->x = col * 8 + 4;
    e->y = row * 16 + 8;
    return e;
}

int lt_app_shot_parse(const char *script, lt_event *out, int max)
{
    int n = 0;
    const char *p = script;
    char tok[96];
    if (script == NULL) { return 0; }
    while (*p != '\0' && n < max - 8) {
        size_t len = 0;
        int mods = 0, a = 0, b = 0, c = 0, d = 0;
        const char *s;
        while (*p == ',' || *p == ' ') { p++; }
        while (*p != '\0' && *p != ' ' && *p != ',' && len + 1 < sizeof(tok)) { tok[len++] = *p++; }
        tok[len] = '\0';
        if (len == 0) { break; }
        if (strncmp(tok, "text:", 5) == 0) {
            const char *t = tok + 5;
            for (; *t != '\0' && n < max - 8; t++) {
                lt_event *e = ev_new(out, &n, LT_EV_KEY_DOWN, 0, 0);
                e->key = (*t >= 'A' && *t <= 'Z') ? *t + 32 : (unsigned char)*t;
                e = ev_new(out, &n, LT_EV_TEXT, 0, 0);
                e->text[0] = *t;
            }
        } else if (strncmp(tok, "click:", 6) == 0 || strncmp(tok, "rclick:", 7) == 0) {
            int bt = tok[0] == 'r' ? LT_MB_RIGHT : LT_MB_LEFT;
            lt_event *e;
            sscanf(tok + (tok[0] == 'r' ? 7 : 6), "%d/%d", &a, &b);
            ev_new(out, &n, LT_EV_MOUSE_MOVE, a, b);
            e = ev_new(out, &n, LT_EV_MOUSE_DOWN, a, b);
            e->button = bt;
            e = ev_new(out, &n, LT_EV_MOUSE_UP, a, b);
            e->button = bt;
        } else if (strncmp(tok, "move:", 5) == 0) {
            sscanf(tok + 5, "%d/%d", &a, &b);
            ev_new(out, &n, LT_EV_MOUSE_MOVE, a, b);
        } else if (strncmp(tok, "drag:", 5) == 0) {
            lt_event *e;
            sscanf(tok + 5, "%d/%d/%d/%d", &a, &b, &c, &d);
            ev_new(out, &n, LT_EV_MOUSE_MOVE, a, b);
            e = ev_new(out, &n, LT_EV_MOUSE_DOWN, a, b);
            e->button = LT_MB_LEFT;
            e = ev_new(out, &n, LT_EV_MOUSE_MOVE, c, d);
            e->button = LT_MB_LEFT;
            e = ev_new(out, &n, LT_EV_MOUSE_UP, c, d);
            e->button = LT_MB_LEFT;
        } else if (strncmp(tok, "wheel:", 6) == 0) {
            lt_event *e;
            sscanf(tok + 6, "%d/%d/%d", &a, &b, &c);
            e = ev_new(out, &n, LT_EV_WHEEL, a, b);
            e->wheel = c;
        } else {
            lt_event *e;
            int key;
            s = tok;
            for (;;) {
                if (strncmp(s, "alt+", 4) == 0) { mods |= LT_MOD_ALT; s += 4; }
                else if (strncmp(s, "ctrl+", 5) == 0) { mods |= LT_MOD_CTRL; s += 5; }
                else if (strncmp(s, "shift+", 6) == 0) { mods |= LT_MOD_SHIFT; s += 6; }
                else { break; }
            }
            key = key_by_name(s);
            if (key != 0) {
                e = ev_new(out, &n, LT_EV_KEY_DOWN, 0, 0);
                e->key = key;
                e->mods = mods;
            }
        }
    }
    return n;
}

int lt_app_shot_write(const lt_cells *g)
{
    char *v;
    lt_cells tmp;
    int depth = 24, charset, rc = 0;
    size_t total;
    v = lt_plat_getenv("LT_SHOT_COLORS");
    if (v != NULL && v[0] != '\0') { depth = atoi(v); }
    lt_free(v);
    charset = env_set("LT_SHOT_ASCII") ? 0 : (env_set("LT_SHOT_BASIC") ? 2 : 1);
    tmp.cols = g->cols;
    tmp.rows = g->rows;
    total = (size_t)g->cols * (size_t)g->rows;
    tmp.cells = (lt_cell *)lt_malloc(total * sizeof(lt_cell));
    memcpy(tmp.cells, g->cells, total * sizeof(lt_cell));
    lt_tty_apply_mode(&tmp, depth, charset);
    v = lt_plat_getenv("LT_SHOT_TTY");
    if (v != NULL && v[0] != '\0') {
        if (lt_cells_write_ppm(&tmp, v) != 0) { rc = 1; }
    }
    lt_free(v);
    v = lt_plat_getenv("LT_SHOT_TEXT");
    if (v != NULL && v[0] != '\0') {
        char *txt = lt_cells_to_text(&tmp);
        FILE *fp = fopen(v, "wb");
        if (fp != NULL) { fwrite(txt, 1, strlen(txt), fp); fclose(fp); } else { rc = 1; }
        lt_free(txt);
    }
    lt_free(v);
    v = lt_plat_getenv("LT_SHOT_ANSI");
    if (v != NULL && v[0] != '\0') {
        char *an = lt_tty_ansi_dump(g, depth, charset);
        FILE *fp = fopen(v, "wb");
        if (fp != NULL) { fwrite(an, 1, strlen(an), fp); fclose(fp); } else { rc = 1; }
        lt_free(an);
    }
    lt_free(v);
    lt_free(tmp.cells);
    return rc;
}
