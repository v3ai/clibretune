/* feat_i18n.c - interface language (G-95): Settings -> Language, and loading the saved choice at
 * start (env LT_LANG=<code> overrides it, for screenshots and tests).  The lookup itself is
 * lt_tr() in i18n.c.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <string.h>
#include "feat.h"
#include "../plat/plat.h"

typedef struct i18n_state { int loaded; } i18n_state;

static void lang_tick(lt_app *app)
{
    i18n_state *s = (i18n_state *)lt_ext_state(app, "i18n", sizeof(i18n_state), NULL);
    char *env;
    if (s->loaded) { return; }
    s->loaded = 1;
    (void)lt_i18n_load(app->config_dir);
    env = lt_plat_getenv("LT_LANG");
    if (env != NULL && env[0] != '\0' && lt_i18n_find(env) >= 0) { lt_i18n_set(lt_i18n_find(env)); }
}

static void lang_page(lt_app *app)
{
    lt_ui *ui = app->ui;
    const lt_theme *t = lt_ui_theme(ui);
    const char *names[LT_I18N_NLANG];
    int i, sel = lt_i18n_get(), w1 = -1;
    for (i = 0; i < LT_I18N_NLANG; i++) { names[i] = lt_i18n_name(i); }
    lt_ui_layout_row(ui, 1, &w1, 0);
    lt_ui_text_style(ui, (int)LT_FONT_SANS_SEMIBOLD, LT_UI_FS_BASE);
    lt_ui_label_ex(ui, lt_tr("Language"), t->text, LT_UI_ALIGN_LEFT);
    lt_ui_text_style_reset(ui);
    lt_ui_layout_row(ui, 1, &w1, 0);
    if (lt_ui_combo(ui, "lang", &sel, names, LT_I18N_NLANG)) {
        lt_i18n_set(sel);
        (void)lt_i18n_save(app->config_dir);
    }
    lt_ui_layout_row(ui, 1, &w1, 0);
    lt_ui_label_wrap(ui, lt_tr("Interface language. Restart not required."), t->muted);
    lt_ui_layout_row(ui, 1, &w1, 0);
    lt_ui_label_wrap(ui, lt_tr("Texts without a translation stay in English."), t->muted);
}

void lt_feat_i18n_register(lt_app_ext *reg)
{
    lt_ext_add_tick(reg, lang_tick);
    lt_ext_add_settings_page(reg, "Language", lang_page);
}
