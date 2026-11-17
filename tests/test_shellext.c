/* test_shellext.c - interface language (G-95), customizable shortcuts and Edit routing (G-91,
 * G-105), the in-app manual (G-101) and tab management (G-103).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <string.h>
#include "lt_test.h"
#include "../src/app/app.h"
#include "../src/app/app_ext.h"
#include "../src/app/feat.h"
#include "../src/app/i18n.h"
#include "../src/app/hotkeys.h"
#include "../src/app/manual.h"
#include "../src/plat/plat.h"

static void sandbox(const char *tag)
{
    char cfg[160], ts[32];
    int i;
    lt_plat_timestamp(ts, sizeof(ts));
    for (i = 0; ts[i] != '\0'; i++) { if (ts[i] < '0' || ts[i] > '9') { ts[i] = '_'; } }
    lt_snprintf(cfg, sizeof(cfg), "build/test_shellext_%s_%s_%lu", tag, ts, (unsigned long)lt_plat_now_ms());
    (void)lt_plat_mkdirs(cfg);
    LT_CHECK(lt_plat_setenv("XDG_CONFIG_HOME", cfg) == 0);
}

/* ---- i18n ---- */
LT_TEST(i18n_lookup)
{
    int i, k;
    char buf[64];
    lt_i18n_set(0);
    LT_CHECK(lt_i18n_count() == 3);
    LT_CHECK(strcmp(lt_i18n_code(0), "en") == 0 && strcmp(lt_i18n_code(1), "pt-BR") == 0);
    LT_CHECK(lt_i18n_find("hu-HU") == 2 && lt_i18n_find("xx") < 0);
    LT_CHECK(strcmp(lt_tr("&File"), "&File") == 0);
    lt_i18n_set(lt_i18n_find("pt-BR"));
    LT_CHECK(strcmp(lt_tr("&File"), "&Arquivo") == 0);
    LT_CHECK(strcmp(lt_tr("Cancel"), "Cancelar") == 0);
    {   /* no translation: the very same English text comes back */
        const char *e = "A string nobody translated";
        LT_CHECK(lt_tr(e) == e);
    }
    LT_CHECK(strcmp(lt_tr_list("Burn|Cancel", buf, sizeof(buf)), "Gravar|Cancelar") == 0);
    lt_i18n_set(lt_i18n_find("hu-HU"));
    LT_CHECK(strcmp(lt_tr("Close"), "Bez\xC3\xA1r") == 0);
    LT_CHECK(strcmp(lt_tr("Language"), "Nyelv") == 0);
    lt_i18n_set(77);
    LT_CHECK(lt_i18n_get() == 0);
    /* the tables are sorted (binary search) and every key finds itself */
    for (i = 1; i < lt_i18n_count(); i++) {
        const lt_i18n_lang *L = &lt_i18n_langs[i];
        LT_CHECK(L->n > 100);
        for (k = 0; k < L->n; k++) {
            LT_CHECK(strcmp(lt_i18n_lookup(i, L->tab[k].en), L->tab[k].tr) == 0);
            if (k > 0) { LT_CHECK(strcmp(L->tab[k - 1].en, L->tab[k].en) < 0); }
        }
    }
}

LT_TEST(i18n_persist)
{
    char dir[160], ts[32];
    int i;
    lt_plat_timestamp(ts, sizeof(ts));
    for (i = 0; ts[i] != '\0'; i++) { if (ts[i] < '0' || ts[i] > '9') { ts[i] = '_'; } }
    lt_snprintf(dir, sizeof(dir), "build/test_i18n_%s_%lu", ts, (unsigned long)lt_plat_now_ms());
    (void)lt_plat_mkdirs(dir);
    lt_i18n_set(0);
    LT_CHECK(lt_i18n_load(dir) < 0 && lt_i18n_get() == 0);
    lt_i18n_set(lt_i18n_find("pt-BR"));
    LT_CHECK(lt_i18n_save(dir) == 0);
    lt_i18n_set(0);
    LT_CHECK(lt_i18n_load(dir) == 0);
    LT_CHECK(lt_i18n_get() == lt_i18n_find("pt-BR"));
    lt_i18n_set(0);
}

/* ---- hotkeys ---- */
LT_TEST(hotkey_notation)
{
    int i, k = 0, m = 0;
    char b[40];
    LT_CHECK(strcmp(lt_hk_format('s', LT_MOD_CTRL | LT_MOD_SHIFT, b, sizeof(b)), "Ctrl+Shift+S") == 0);
    LT_CHECK(strcmp(lt_hk_format(LT_KEY_F9, 0, b, sizeof(b)), "F9") == 0);
    LT_CHECK(strcmp(lt_hk_format(',', LT_MOD_CTRL, b, sizeof(b)), "Ctrl+,") == 0);
    LT_CHECK(strcmp(lt_hk_format(LT_HK_NONE, 0, b, sizeof(b)), "None") == 0);
    for (i = 0; i < lt_hk_core_count(); i++) {   /* every factory key survives a round trip */
        const lt_hk_def *d = lt_hk_core_def(i);
        LT_CHECK(lt_hk_parse(lt_hk_format(d->key, d->mods, b, sizeof(b)), &k, &m));
        LT_CHECK(k == d->key && m == d->mods);
    }
    LT_CHECK(lt_hk_parse("ctrl+alt+PageUp", &k, &m) && k == LT_KEY_PAGEUP && m == (LT_MOD_CTRL | LT_MOD_ALT));
    LT_CHECK(lt_hk_parse("Ctrl++", &k, &m) && k == '+' && m == LT_MOD_CTRL);
    LT_CHECK(lt_hk_parse("none", &k, &m) && k == LT_HK_NONE);
    LT_CHECK(!lt_hk_parse("Ctrl+Banana", &k, &m));
    LT_CHECK(!lt_hk_parse("Ctrl+", &k, &m));
    /* what may be bound */
    LT_CHECK(lt_hk_check('s', LT_MOD_CTRL) == NULL);
    LT_CHECK(lt_hk_check(LT_KEY_F5, 0) == NULL);
    LT_CHECK(lt_hk_check('s', 0) != NULL);
    LT_CHECK(lt_hk_check('s', LT_MOD_SHIFT) != NULL);
    LT_CHECK(lt_hk_check(LT_KEY_ESCAPE, LT_MOD_CTRL) != NULL);
    LT_CHECK(lt_hk_check('3', LT_MOD_CTRL) != NULL);
}

LT_TEST(hotkey_overrides)
{
    lt_hk_set s;
    lt_hk_def defs[40];
    int n = lt_hk_core_count(), i, k = 0, m = 0, c;
    char dir[160], ts[32];
    const lt_hk_def *save;
    for (i = 0; i < n; i++) { defs[i] = *lt_hk_core_def(i); }
    LT_CHECK(n > 15 && n < 40);
    /* no two factory bindings collide */
    lt_hk_set_clear(&s);
    for (i = 0; i < n; i++) {
        LT_CHECK(lt_hk_conflict(&s, defs, n, defs[i].key, defs[i].mods, defs[i].id) < 0);
    }
    save = lt_hk_core_find("file.save");
    LT_CHECK(save != NULL && lt_hk_core_find("nope") == NULL);
    LT_CHECK(!lt_hk_get(&s, save, &k, &m) && k == 's' && m == LT_MOD_CTRL);
    /* conflict: Ctrl+N is New project */
    c = lt_hk_conflict(&s, defs, n, 'n', LT_MOD_CTRL, "file.save");
    LT_CHECK(c >= 0 && strcmp(defs[c].id, "file.new") == 0);
    LT_CHECK(lt_hk_set_bind(&s, "file.save", 'u', LT_MOD_CTRL | LT_MOD_ALT) == 0);
    LT_CHECK(lt_hk_get(&s, save, &k, &m) && k == 'u' && m == (LT_MOD_CTRL | LT_MOD_ALT));
    /* the freed combination is no longer taken, the new one is */
    LT_CHECK(lt_hk_conflict(&s, defs, n, 's', LT_MOD_CTRL, "file.load_tune") < 0);
    c = lt_hk_conflict(&s, defs, n, 'u', LT_MOD_CTRL | LT_MOD_ALT, "file.new");
    LT_CHECK(c >= 0 && strcmp(defs[c].id, "file.save") == 0);
    LT_CHECK(lt_hk_set_bind(&s, "file.quit", LT_HK_NONE, 0) == 0);
    LT_CHECK(lt_hk_get(&s, lt_hk_core_find("file.quit"), &k, &m) && k == LT_HK_NONE);
    /* persistence */
    lt_plat_timestamp(ts, sizeof(ts));
    for (i = 0; ts[i] != '\0'; i++) { if (ts[i] < '0' || ts[i] > '9') { ts[i] = '_'; } }
    lt_snprintf(dir, sizeof(dir), "build/test_hk_%s_%lu", ts, (unsigned long)lt_plat_now_ms());
    (void)lt_plat_mkdirs(dir);
    LT_CHECK(lt_hk_save(&s, dir) == 0);
    {
        lt_hk_set t;
        lt_hk_set_clear(&t);
        LT_CHECK(lt_hk_load(&t, dir) == 0 && t.n == 2);
        LT_CHECK(lt_hk_get(&t, save, &k, &m) && k == 'u' && m == (LT_MOD_CTRL | LT_MOD_ALT));
        LT_CHECK(lt_hk_get(&t, lt_hk_core_find("file.quit"), &k, &m) && k == LT_HK_NONE);
    }
    /* reset */
    lt_hk_set_reset(&s, "file.save");
    LT_CHECK(!lt_hk_get(&s, save, &k, &m) && k == 's');
    LT_CHECK(s.n == 1);
    lt_hk_set_clear(&s);
    LT_CHECK(s.n == 0);
}

LT_TEST(hotkey_app_glue)
{
    char path[600];
    const char *txt = "# test\nfile.save=Ctrl+Alt+S\nview.sidebar=none\nbogus line\ntab.next=Ctrl+Banana\n";
    lt_app *app;
    lt_hk_def ext[16];
    int n, i, found = 0;
    sandbox("hk");
    app = lt_app_new();
    LT_CHECK(app != NULL && app->config_dir != NULL);
    (void)lt_plat_mkdirs(app->config_dir);
    lt_snprintf(path, sizeof(path), "%s/hotkeys.conf", app->config_dir);
    LT_CHECK(lt_file_write(path, txt, strlen(txt)) == 0);
    LT_CHECK(strcmp(lt_hk_text(app, "file.save"), "Ctrl+Alt+S") == 0);
    LT_CHECK(strcmp(lt_hk_text(app, "view.sidebar"), "") == 0);
    LT_CHECK(strcmp(lt_hk_text(app, "tab.next"), "Ctrl+Tab") == 0);     /* bad line ignored */
    LT_CHECK(strcmp(lt_hk_text(app, "file.new"), "Ctrl+N") == 0);
    LT_CHECK(strcmp(lt_hk_tip(app, "Save tune", "file.save"), "Save tune (Ctrl+Alt+S)") == 0);
    LT_CHECK(strcmp(lt_hk_tip(app, "Toggle sidebar", "view.sidebar"), "Toggle sidebar") == 0);
    /* feature shortcuts are rebindable too and show up next to the shell ones */
    n = lt_ext_shortcut_defs(app, ext, 16);
    for (i = 0; i < n; i++) { if (strcmp(ext[i].id, "tools.autotune") == 0) { found = 1; } }
    LT_CHECK(found);
    LT_CHECK(strcmp(lt_hk_text(app, "tools.autotune"), "Ctrl+Shift+A") == 0);
    LT_CHECK(strcmp(lt_hk_text(app, "help.manual"), "F1") == 0);
    LT_CHECK(!lt_hk_capturing(app));
    lt_app_free(app);
}

/* ---- Edit actions: queued as the tab's own key, delivered at the next service ---- */
LT_TEST(edit_routing)
{
    lt_app *app;
    sandbox("edit");
    app = lt_app_new();
    app->ui = lt_ui_create();
    lt_feat_edit_do(app, "edit.undo");                 /* no table tab: nothing is queued */
    lt_app_service(app);
    LT_CHECK(!lt_ui_key_pressed(app->ui, 'z', LT_MOD_CTRL));
    LT_CHECK(lt_app_tab_open(app, "table", "veTable1", "VE Table", 1) != NULL);
    lt_feat_edit_do(app, "edit.undo");
    lt_app_service(app);
    LT_CHECK(lt_ui_key_pressed(app->ui, 'z', LT_MOD_CTRL));
    lt_feat_edit_do(app, "edit.cut");
    lt_app_service(app);
    LT_CHECK(lt_ui_key_pressed(app->ui, 'x', LT_MOD_CTRL));
    LT_CHECK(lt_ui_key_pressed(app->ui, LT_KEY_DELETE, 0));
    lt_feat_edit_do(app, "edit.select_all");
    lt_app_service(app);
    LT_CHECK(lt_ui_key_pressed(app->ui, 'a', LT_MOD_CTRL));
    lt_app_free(app);
}

/* ---- tabs ---- */
static void open_tabs(lt_app *app, int n)
{
    int i;
    char t[16];
    for (i = 0; i < n; i++) {
        lt_snprintf(t, sizeof(t), "t%d", i);
        LT_CHECK(lt_app_tab_open(app, "table", t, t, 1) != NULL);
    }
}

LT_TEST(tabs_reorder)
{
    lt_app *app;
    lt_rect r[3];
    sandbox("tabs");
    app = lt_app_new();
    open_tabs(app, 4);                                   /* t0 t1 t2 t3, t3 active */
    LT_CHECK(app->n_tabs == 4 && app->active_tab == 3);
    LT_CHECK(lt_feat_tabs_move(app, 3, 0) == 0);         /* t3 t0 t1 t2 */
    LT_CHECK(strcmp(app->tabs[0]->target, "t3") == 0 && strcmp(app->tabs[3]->target, "t2") == 0);
    LT_CHECK(app->active_tab == 0);                      /* the active tab moved with it */
    LT_CHECK(lt_feat_tabs_move(app, 1, 2) == 0);         /* t3 t1 t0 t2 */
    LT_CHECK(strcmp(app->tabs[1]->target, "t1") == 0 && app->active_tab == 0);
    LT_CHECK(lt_feat_tabs_move(app, 0, 3) == 0);         /* t1 t0 t2 t3 */
    LT_CHECK(app->active_tab == 3 && strcmp(app->tabs[3]->target, "t3") == 0);
    LT_CHECK(lt_feat_tabs_move(app, 2, 2) < 0 && lt_feat_tabs_move(app, 0, 9) < 0);
    r[0] = lt_rect_make(0, 0, 100, 20); r[1] = lt_rect_make(100, 0, 100, 20); r[2] = lt_rect_make(200, 0, 100, 20);
    LT_CHECK(lt_feat_tabs_at(r, 3, 150) == 1 && lt_feat_tabs_at(r, 3, -40) == 0 &&
             lt_feat_tabs_at(r, 3, 900) == 2 && lt_feat_tabs_at(r, 0, 5) < 0);
    lt_app_free(app);
}

LT_TEST(tabs_close_protect)
{
    lt_app *app;
    sandbox("tabs2");
    app = lt_app_new();
    open_tabs(app, 5);                                   /* t0..t4 */
    lt_feat_tabs_protect(app, 1, 1);                     /* t1 protected */
    LT_CHECK(app->tabs[1]->pinned == 1);
    app->active_tab = 1;
    lt_feat_tabs_close_active(app);                      /* Ctrl+W on a protected tab: nothing */
    LT_CHECK(app->n_tabs == 5);
    LT_CHECK(lt_feat_tabs_close_many(app, 1, 3) == 1);   /* right of t3: t4 */
    LT_CHECK(app->n_tabs == 4);
    LT_CHECK(lt_feat_tabs_close_many(app, 0, 2) == 2);   /* all but t2; t1 stays */
    LT_CHECK(app->n_tabs == 2);
    LT_CHECK(strcmp(app->tabs[0]->target, "t1") == 0 && strcmp(app->tabs[1]->target, "t2") == 0);
    LT_CHECK(lt_feat_tabs_close_many(app, 2, 0) == 1);   /* all: only the unprotected one */
    LT_CHECK(app->n_tabs == 1 && app->tabs[0]->pinned);
    lt_feat_tabs_protect(app, 0, 0);
    app->active_tab = 0;
    lt_feat_tabs_close_active(app);
    LT_CHECK(app->n_tabs == 0);
    lt_app_free(app);
}

/* ---- manual ---- */
LT_TEST(manual_pages)
{
    int i, p;
    LT_CHECK(lt_man_count >= 30);
    for (i = 0; i < lt_man_count; i++) {
        lt_md_doc d;
        LT_CHECK(lt_man_line_count(i) > 3);
        LT_CHECK(lt_man_find(lt_man_pages[i].id) == i);
        lt_md_parse_page(&d, i);
        LT_CHECK(d.n_blocks > 3);
        lt_md_free(&d);
    }
    p = lt_man_find("features/autotune/setup");
    LT_CHECK(p >= 0);
    LT_CHECK(lt_man_resolve(p, "../autotune.md") == lt_man_find("features/autotune"));
    LT_CHECK(lt_man_resolve(p, "usage-guide.md#top") == lt_man_find("features/autotune/usage-guide"));
    LT_CHECK(lt_man_resolve(p, "./filters.md") == lt_man_find("features/autotune/filters"));
    LT_CHECK(lt_man_resolve(p, "../../faq.md") == lt_man_find("faq"));
    LT_CHECK(lt_man_resolve(p, "https://example.com/a.md") < 0);
    LT_CHECK(lt_man_resolve(p, "missing-page.md") < 0);
    LT_CHECK(lt_man_resolve(p, "#anchor") < 0 && lt_man_resolve(p, "") < 0);
    LT_CHECK(lt_man_find(lt_man_context("table", "veTable1")) >= 0);
    LT_CHECK(lt_man_find(lt_man_context("dashboard", "")) >= 0);
    LT_CHECK(lt_man_find(lt_man_context("autotune", "")) >= 0);
    LT_CHECK(strcmp(lt_man_context("nonsense", ""), "introduction") == 0);
    LT_CHECK(strcmp(lt_man_context(NULL, NULL), "introduction") == 0);
}

LT_TEST(manual_markdown)
{
    static const char *const src[] = {
        "# Title with `code`", "", "First line of a **bold** paragraph", "continues with *italic* and a",
        "[link](../other/page.md) plus [ext](https://x.org/y).", "",
        "- item one", "  continues here", "  - nested", "1. numbered", "",
        "```", "int x = 1;", "", "  indented", "```",
        "> quoted", "> more", "",
        "| A | B |", "|---|---|", "| 1 | [go](p.md) |", "",
        "---", "![shot](img.png) after"
    };
    lt_md_doc d;
    int i, heads = 0, codes = 0;
    lt_md_parse(&d, src, (int)LT_COUNTOF(src));
    LT_CHECK(d.n_blocks >= 12);
    LT_CHECK(d.blocks[0].kind == LT_MD_HEAD && d.blocks[0].level == 1 &&
             strcmp(d.blocks[0].text, "Title with code") == 0);
    LT_CHECK(d.blocks[1].kind == LT_MD_PARA);
    LT_CHECK(strstr(d.blocks[1].text, "bold paragraph continues with italic and a link plus") != NULL);
    LT_CHECK(strstr(d.blocks[1].text, "ext (https://x.org/y)") != NULL);
    LT_CHECK(d.blocks[1].nlinks == 1);
    LT_CHECK(strcmp(d.links[d.blocks[1].link0].label, "link") == 0 &&
             strcmp(d.links[d.blocks[1].link0].target, "../other/page.md") == 0);
    LT_CHECK(d.blocks[2].kind == LT_MD_BULLET && d.blocks[2].level == 0 &&
             strcmp(d.blocks[2].text, "item one continues here") == 0);
    LT_CHECK(d.blocks[3].kind == LT_MD_BULLET && d.blocks[3].level == 1);
    LT_CHECK(d.blocks[4].kind == LT_MD_BULLET && strncmp(d.blocks[4].text, "1. ", 3) == 0);
    for (i = 0; i < d.n_blocks; i++) {
        if (d.blocks[i].kind == LT_MD_HEAD) { heads++; }
        if (d.blocks[i].kind == LT_MD_CODE) { codes++; }
    }
    LT_CHECK(heads == 1 && codes == 3);
    for (i = 0; i < d.n_blocks; i++) {
        if (d.blocks[i].kind == LT_MD_QUOTE) { LT_CHECK(strcmp(d.blocks[i].text, "quoted more") == 0); }
        if (d.blocks[i].kind == LT_MD_TABLE && d.blocks[i].level == 1) {
            LT_CHECK(strcmp(d.blocks[i].text, "A  |  B") == 0);
        }
        if (d.blocks[i].kind == LT_MD_TABLE && d.blocks[i].level == 0) {
            LT_CHECK(strcmp(d.blocks[i].text, "1  |  go") == 0 && d.blocks[i].nlinks == 1);
        }
    }
    LT_CHECK(d.blocks[d.n_blocks - 1].kind == LT_MD_PARA &&
             strcmp(d.blocks[d.n_blocks - 1].text, "[shot] after") == 0);
    lt_md_free(&d);
}

LT_TEST(manual_search)
{
    lt_man_hit h[64];
    int n, i, found = 0;
    n = lt_man_search("autotune", h, 64);
    LT_CHECK(n > 3);
    for (i = 1; i < n; i++) { LT_CHECK(h[i - 1].score >= h[i].score); }
    LT_CHECK(strncmp(lt_man_pages[h[0].page].id, "features/autotune", 17) == 0);
    n = lt_man_search("TIMEOUT baud", h, 64);          /* case-insensitive, all words */
    for (i = 0; i < n; i++) { if (strcmp(lt_man_pages[h[i].page].id, "reference/troubleshooting") == 0) { found = 1; } }
    LT_CHECK(n > 0 && found);
    LT_CHECK(strstr(lt_man_line(h[0].page, h[0].line), "") != NULL);
    LT_CHECK(lt_man_search("zzzqqqxx", h, 64) == 0);
    LT_CHECK(lt_man_search("", h, 64) == 0 && lt_man_search("   ", h, 64) == 0);
}

LT_TEST(manual_tab)
{
    lt_app *app;
    lt_app_tab *t;
    sandbox("man");
    app = lt_app_new();
    LT_CHECK(lt_ext_has_target(app, "manual"));
    LT_CHECK(lt_ext_dispatch_target(app, "manual") == 0);
    t = lt_app_tab_active(app);
    LT_CHECK(t != NULL && strcmp(t->kind, "manual") == 0);
    lt_feat_manual_open(app, "faq");                    /* reopening reuses the tab */
    LT_CHECK(app->n_tabs == 1);
    lt_feat_manual_context(app);                        /* already on the manual: stays */
    LT_CHECK(app->n_tabs == 1);
    lt_app_free(app);
}

int main(void)
{
    LT_RUN(i18n_lookup);
    LT_RUN(i18n_persist);
    LT_RUN(hotkey_notation);
    LT_RUN(hotkey_overrides);
    LT_RUN(hotkey_app_glue);
    LT_RUN(edit_routing);
    LT_RUN(tabs_reorder);
    LT_RUN(tabs_close_protect);
    LT_RUN(manual_pages);
    LT_RUN(manual_markdown);
    LT_RUN(manual_search);
    LT_RUN(manual_tab);
    LT_TEST_MAIN_END
}
