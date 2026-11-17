/* feat_manual.c - in-app user manual (G-101): Help -> User Manual and F1 (context help for the
 * active tab) open a "manual" tab with the embedded manual (manual.h): contents list, full-text
 * search, headings, paragraphs, lists, code, quotes and tables, links between pages, Back.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <string.h>
#include "feat.h"
#include "manual.h"
#include "../plat/plat.h"

#define MAN_HITS 64
#define MAN_HIST 16

typedef struct man_state {
    int page;                        /* page being shown */
    lt_md_doc doc;
    int doc_page;                    /* page the parsed doc belongs to (-1 none) */
    char q[64], last_q[64];
    lt_man_hit hits[MAN_HITS];
    int n_hits;
    int nav_sel, hit_sel;
    int hist[MAN_HIST], n_hist;
    int scroll_reset;
    lt_buf tmp;
    char label[4][200];
    int label_n;
    int env_done;
} man_state;

static void man_free(lt_app *app, void *p)
{
    man_state *s = (man_state *)p;
    LT_UNUSED(app);
    lt_md_free(&s->doc);
    lt_buf_free(&s->tmp);
}

static man_state *man(lt_app *app)
{
    man_state *s = (man_state *)lt_ext_state(app, "manual", sizeof(man_state), man_free);
    if (s->doc_page == 0 && s->doc.blocks == NULL && s->page == 0 && s->tmp.cap == 0) {
        s->doc_page = -1;
        lt_buf_init(&s->tmp);
    }
    return s;
}

/* show a page (history keeps the one left) */
static void man_go(man_state *s, int page, int push)
{
    if (page < 0 || page >= lt_man_count) { return; }
    if (push && s->page != page) {
        if (s->n_hist == MAN_HIST) {
            memmove(s->hist, s->hist + 1, sizeof(int) * (MAN_HIST - 1));
            s->n_hist--;
        }
        s->hist[s->n_hist++] = s->page;
    }
    s->page = page;
    s->nav_sel = page;
    s->scroll_reset = 1;
}

/* open the manual tab at `page_id` (NULL keeps the page shown last) */
void lt_feat_manual_open(lt_app *app, const char *page_id)
{
    man_state *s = man(app);
    if (page_id != NULL) {
        int p = lt_man_find(page_id);
        if (p >= 0) { man_go(s, p, 1); }
    }
    (void)lt_app_tab_open(app, "manual", "manual", lt_tr("User Manual"), 1);
}

/* F1: the manual page that belongs to the active tab */
void lt_feat_manual_context(lt_app *app)
{
    lt_app_tab *t = lt_app_tab_active(app);
    if (t != NULL && strcmp(t->kind, "manual") == 0) { return; }
    lt_feat_manual_open(app, lt_man_context(t != NULL ? t->kind : NULL, t != NULL ? t->target : NULL));
}

/* ---- rendering ---- */
static const char *nav_item(void *user, int i)
{
    man_state *s = (man_state *)user;
    char *b = s->label[s->label_n];
    s->label_n = (s->label_n + 1) % 4;
    if (s->n_hits > 0 || s->q[0] != '\0') {
        if (i >= 0 && i < s->n_hits) {
            lt_snprintf(b, sizeof(s->label[0]), "%s", lt_man_pages[s->hits[i].page].title);
        } else { b[0] = '\0'; }
    } else if (i >= 0 && i < lt_man_count) {
        lt_snprintf(b, sizeof(s->label[0]), "%*s%s", lt_man_pages[i].depth * 2, "",
                    lt_man_pages[i].title);
    } else { b[0] = '\0'; }
    return b;
}

static void run_search(man_state *s)
{
    lt_strlcpy(s->last_q, s->q, sizeof(s->last_q));
    s->n_hits = lt_man_search(s->q, s->hits, MAN_HITS);
    s->hit_sel = 0;
}

/* "12. text": a numbered list item keeps its own number instead of a bullet */
static int numbered(const char *t)
{
    int n = 0;
    while (t[n] >= '0' && t[n] <= '9') { n++; }
    return n > 0 && t[n] == '.' && t[n + 1] == ' ';
}

static void draw_block(lt_app *app, man_state *s, const lt_md_block *b, int cell, int bi, int *go)
{
    lt_ui *ui = app->ui;
    const lt_theme *t = lt_ui_theme(ui);
    int w1 = -1, i;
    static const int head_px[7] = { 13, 20, 16, 14, 13, 12, 12 };
    lt_ui_layout_row(ui, 1, &w1, 0);
    switch (b->kind) {
    case LT_MD_HEAD:
        lt_ui_layout_space(ui, b->level <= 2 ? 8 : 4);
        lt_ui_layout_row(ui, 1, &w1, 0);
        lt_ui_text_style(ui, (int)LT_FONT_SANS_SEMIBOLD, head_px[b->level]);
        lt_ui_label_ex(ui, b->text, b->level <= 2 ? t->text : t->primary_hover, LT_UI_ALIGN_LEFT);
        lt_ui_text_style_reset(ui);
        if (b->level <= 2) { lt_ui_separator(ui); }
        break;
    case LT_MD_BULLET:
        lt_buf_clear(&s->tmp);
        for (i = 0; i < b->level; i++) { lt_buf_puts(&s->tmp, "    "); }
        if (numbered(b->text)) { lt_buf_puts(&s->tmp, "  "); }
        else { lt_buf_puts(&s->tmp, cell ? "* " : "\xE2\x80\xA2 "); }
        lt_buf_puts(&s->tmp, b->text);
        lt_ui_label_wrap(ui, s->tmp.data != NULL ? s->tmp.data : "", t->text);
        break;
    case LT_MD_CODE:
        lt_ui_text_style(ui, (int)LT_FONT_MONO, LT_UI_FS_SM);
        {
            lt_rect r = lt_ui_layout_next(ui);
            lt_ui_draw_fill(ui, r, t->input);
            lt_ui_draw_text_in_f(ui, lt_rect_make(r.x + t->pad_x, r.y, r.w - 2 * t->pad_x, r.h),
                                 b->text, t->text2, 0, (int)LT_FONT_MONO, LT_UI_FS_SM);
        }
        lt_ui_text_style_reset(ui);
        break;
    case LT_MD_QUOTE:
        lt_buf_clear(&s->tmp);
        lt_buf_puts(&s->tmp, "| ");
        lt_buf_puts(&s->tmp, b->text);
        lt_ui_label_wrap(ui, s->tmp.data, t->muted);
        break;
    case LT_MD_RULE:
        lt_ui_separator(ui);
        break;
    case LT_MD_TABLE:
        if (b->level == 1) {
            lt_ui_text_style(ui, (int)LT_FONT_SANS_SEMIBOLD, LT_UI_FS_BASE);
            lt_ui_label_wrap(ui, b->text, t->text);
            lt_ui_text_style_reset(ui);
        } else {
            lt_ui_label_wrap(ui, b->text, t->text2);
        }
        break;
    default:
        lt_ui_label_wrap(ui, b->text, t->text);
        break;
    }
    /* links of this block that lead to another page */
    {
        int shown = 0;
        for (i = 0; i < b->nlinks && shown < 6; i++) {
            const lt_md_link *l = &s->doc.links[b->link0 + i];
            int target = lt_man_resolve(s->page, l->target);
            char lab[120];
            if (target < 0) { continue; }
            lt_snprintf(lab, sizeof(lab), "%s%s##l%d_%d", cell ? "> " : "\xE2\x86\x92 ", l->label, bi, i);
            w1 = 0;
            lt_ui_layout_row(ui, 1, &w1, 0);
            if (lt_ui_link(ui, lab)) { *go = target; }
            shown++;
        }
    }
}

static void man_draw(lt_app *app, lt_app_tab *tab, lt_rect r)
{
    lt_ui *ui = app->ui;
    man_state *s = man(app);
    const lt_theme *t = lt_ui_theme(ui);
    int w1 = -1, wc[2], wb[3], cell = lt_ui_cell_mode(ui), go = -1, i, nb;
    int nav_n, sel;
    LT_UNUSED(tab);
    LT_UNUSED(r);
    if (!s->env_done) {                       /* screenshot / test hooks */
        char *e = lt_plat_getenv("LT_SHOT_MANUAL");
        s->env_done = 1;
        if (e != NULL && lt_man_find(e) >= 0) { man_go(s, lt_man_find(e), 0); }
        e = lt_plat_getenv("LT_SHOT_MANUAL_Q");
        if (e != NULL) { lt_strlcpy(s->q, e, sizeof(s->q)); }
    }
    if (s->page < 0 || s->page >= lt_man_count) { s->page = 0; }
    if (s->doc_page != s->page) {
        lt_md_free(&s->doc);
        lt_md_parse_page(&s->doc, s->page);
        s->doc_page = s->page;
    }
    wc[0] = 250; wc[1] = -1;
    lt_ui_layout_row(ui, 2, wc, -1);
    /* ---- left: search + contents / results ---- */
    lt_ui_layout_column_begin(ui);
    lt_ui_layout_row(ui, 1, &w1, 0);
    (void)lt_ui_text(ui, "msearch", s->q, sizeof(s->q), 0);
    if (s->q[0] == '\0' && !lt_ui_text_focused(ui)) {
        lt_rect sr = lt_ui_last_rect(ui);
        lt_ui_draw_text_in_f(ui, lt_rect_make(sr.x + t->pad_x, sr.y, sr.w - 2 * t->pad_x, sr.h),
                             lt_tr("Search the manual..."), t->muted, 0, (int)LT_FONT_SANS, LT_UI_FS_SM);
    }
    if (strcmp(s->q, s->last_q) != 0) { run_search(s); }
    lt_ui_layout_row(ui, 1, &w1, -1);
    if (s->q[0] != '\0') {
        nav_n = s->n_hits;
        sel = s->hit_sel;
        for (i = 0; i < nav_n; i++) { if (s->hits[i].page == s->page) { sel = i; } }
        if (nav_n == 0) {
            lt_ui_label_ex(ui, lt_tr("No matching page"), t->muted, LT_UI_ALIGN_LEFT);
        } else if (lt_ui_listbox_cb(ui, "mhits", &sel, nav_n, nav_item, s) & LT_UI_CHANGED) {
            s->hit_sel = sel;
            if (sel >= 0 && sel < s->n_hits) { go = s->hits[sel].page; }
        }
    } else {
        sel = s->nav_sel;
        if (lt_ui_listbox_cb(ui, "mtoc", &sel, lt_man_count, nav_item, s) & LT_UI_CHANGED) {
            go = sel;
        }
    }
    lt_ui_layout_column_end(ui);
    /* ---- right: toolbar + page ---- */
    lt_ui_layout_column_begin(ui);
    wb[0] = 90; wb[1] = 110; wb[2] = -1;
    lt_ui_layout_row(ui, 3, wb, 0);
    if (lt_ui_button_ex(ui, lt_tr("Back"), s->n_hist > 0 ? 0 : LT_UI_BTN_DISABLED) && s->n_hist > 0) {
        s->page = s->hist[--s->n_hist];
        s->nav_sel = s->page;
        s->scroll_reset = 1;
    }
    if (lt_ui_button(ui, lt_tr("Contents"))) { go = 0; s->q[0] = '\0'; }
    lt_ui_text_style(ui, (int)LT_FONT_SANS_SEMIBOLD, LT_UI_FS_BASE);
    lt_ui_label_ex(ui, lt_tr(lt_man_pages[s->page].section), t->muted, LT_UI_ALIGN_LEFT);
    lt_ui_text_style_reset(ui);
    lt_ui_layout_row(ui, 1, &w1, -1);
    if (lt_ui_scroll_begin(ui, "mpage")) {
        if (s->scroll_reset) { lt_ui_scroll_set(ui, 0, 0); s->scroll_reset = 0; }
        nb = s->doc.n_blocks;
        for (i = 0; i < nb; i++) {
            lt_ui_push_id_int(ui, i);
            draw_block(app, s, &s->doc.blocks[i], cell, i, &go);
            lt_ui_pop_id(ui);
        }
        lt_ui_scroll_end(ui);
    }
    lt_ui_layout_column_end(ui);
    if (go >= 0) { man_go(s, go, 1); }
}

static void man_close(lt_app *app, lt_app_tab *tab)
{
    man_state *s = man(app);
    LT_UNUSED(tab);
    lt_md_free(&s->doc);
    s->doc_page = -1;
}

/* ---- registration ---- */
static void act_manual(lt_app *app) { lt_feat_manual_open(app, NULL); }
static void act_context(lt_app *app) { lt_feat_manual_context(app); }
static int  open_manual(lt_app *app, const char *t) { LT_UNUSED(t); lt_feat_manual_open(app, NULL); return 0; }

void lt_feat_manual_register(lt_app_ext *reg)
{
    lt_app_tab_kind k;
    k.kind = "manual"; k.draw = man_draw; k.close = man_close;
    lt_ext_add_tab_kind(reg, &k);
    lt_ext_add_target(reg, "manual", open_manual);
    lt_ext_add_menu_item(reg, LT_EXT_MENU_HELP, "&User Manual", "F1", act_manual, NULL);
    lt_ext_add_shortcut_named(reg, "help.manual", "User manual (context help)", LT_KEY_F1, 0,
                              act_context, NULL);
}
