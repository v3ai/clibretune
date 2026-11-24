/* dialogs_embed.h - components embedded in INI dialogs (dialogs_embed.c): compact curve and
 * table editors, indicator panels, readout panels and gauges, as TunerStudio shows them when a
 * dialog says "panel = someCurve" or "gauge = someGauge".  Internal to the dialogs owner.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef LT_DIALOGS_EMBED_H
#define LT_DIALOGS_EMBED_H

#include "app.h"

/* per dialog tab: state of its embedded editors/gauges (created on first draw) */
typedef struct lt_embeds { void **v; int n; } lt_embeds;
void lt_emb_free(lt_embeds *e);

/* what a "panel = name" refers to */
#define LT_EMB_NONE    0
#define LT_EMB_DIALOG  1
#define LT_EMB_CURVE   2
#define LT_EMB_TABLE   3
#define LT_EMB_IND     4
#define LT_EMB_READOUT 5
#define LT_EMB_GAUGE   6
#define LT_EMB_KINDS   7
int lt_emb_kind(lt_app *app, const char *name);

/* Each draws into the current layout container (full width) and returns 1 when drawn.
 * enabled = 0 shows the component read only. */
int lt_emb_curve(lt_app *app, lt_embeds *e, const lt_ini_curve *c, int enabled);
int lt_emb_table(lt_app *app, lt_embeds *e, const lt_ini_table *t, int enabled);
int lt_emb_indicators(lt_app *app, const lt_ini_indicator_panel *p);
int lt_emb_readouts(lt_app *app, const lt_ini_readout_panel *p);
int lt_emb_gauge(lt_app *app, lt_embeds *e, const lt_ini_gauge *g);

/* Test hook (dialogs_ini.c): embedded components drawn in the last frame of the dialog tab
 * `name`, by LT_EMB_* kind (counts[LT_EMB_KINDS]); also counts[0] = help buttons. 0 ok,
 * -1 when no such tab was drawn. */
int lt_dlg_ini_counts(lt_app *app, const char *name, int *counts);
/* Test hook: how many sets of side-by-side panels the dialog tab `name` drew one below the other
 * in its last frame because the window was too narrow for them; -1 when no such tab was drawn.
 * *overflow (may be NULL) = how many device px its content is wider than the tab, 0 none. */
int lt_dlg_ini_stacked(lt_app *app, const char *name, int *overflow);
/* Test hook: the dialog tab `name` outlined a field opened from the settings search in its last
 * frame */
int lt_dlg_ini_revealed(lt_app *app, const char *name);

#endif
