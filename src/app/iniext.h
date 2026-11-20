/* iniext.h - INI driven extras of the dialogs owner: help topics, pin conflict report and the
 * burn gate, the "power cycle the ECU" notice, project INI settings (preprocessor symbols)
 * and the programmable output port editor.  Implemented in feat_iniext.c, dlg_portedit.c
 * and app.c (INI reload).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef LT_INIEXT_H
#define LT_INIEXT_H

#include "app.h"

/* ---- help topics (feat_iniext.c) ---- */
/* What a help topic resolves to: an INI [UserDefined] help block (title, text lines, optional
 * web page), a plain URL, or nothing. */
typedef struct lt_help_view {
    int kind;                       /* LT_HELP_NONE / _INI / _URL / _TEXT */
    const char *title;              /* points into the INI or the caller's strings */
    const char *url;                /* NULL when none */
    const lt_ini_help *help;        /* LT_HELP_INI */
} lt_help_view;
#define LT_HELP_NONE 0
#define LT_HELP_INI  1
#define LT_HELP_URL  2
#define LT_HELP_TEXT 3
int  lt_help_resolve(const lt_ini *ini, const char *topic, lt_help_view *out);
/* Help text of an INI help block as plain text (simple HTML tags such as <br> and <p> turned into
 * line breaks, other tags dropped, entities decoded). malloc'd. */
char *lt_help_plain_text(const lt_ini_help *h);
/* open the help viewer for a topic (INI help name or URL); 0 ok, -1 when the topic is unknown */
int  lt_app_help_open(lt_app *app, const char *topic);
/* open the help viewer on a piece of text (field help) */
int  lt_app_help_text(lt_app *app, const char *title, const char *text);

/* ---- pin conflicts (feat_iniext.c) ---- */
/* Pin conflicts of the current tune/ECU image. Selector constants that merely share an option
 * label such as "Zero" are not reported (only real pin labels or "*pin*" constants). Returns
 * the number of conflicts; out (may be NULL) receives them (lt_pin_report_free). */
int  lt_app_pin_conflicts(lt_app *app, lt_pin_report *out);
/* Burn gate used by lt_app_burn: LT_TRUE when the burn may go ahead. With conflicts that were
 * not acknowledged yet it opens the conflict dialog (Burn anyway / Cancel) and returns LT_FALSE. */
lt_bool lt_app_burn_gate(lt_app *app);
/* acknowledge the current conflict set (what "Burn anyway" does); tests and scripts */
void lt_app_pin_ack(lt_app *app);
/* open the whole-tune pin conflict report */
void lt_app_pin_report_open(lt_app *app);

/* ---- requires power cycle (feat_iniext.c) ---- */
/* remember the values of the requiresPowerCycle constants (on connect / after a burn) */
void lt_app_pc_snapshot(lt_app *app);
/* after a successful burn: names of power-cycle constants that changed since the snapshot are
 * collected, the notice is shown and the snapshot is renewed. Returns the number changed. */
int  lt_app_pc_after_burn(lt_app *app);
/* the notice of the last burn: changed constants as "a, b, c" ("" = none pending) */
const char *lt_app_pc_pending(lt_app *app);

/* ---- project INI settings (feat_iniext.c, reload in app.c) ---- */
/* Symbols a project chose ([SettingGroups]), stored TunerStudio style in
 * <project>/projectCfg/project.properties as ecuSettings=A|B|C, plus the symbols LibreTune
 * manages for the project (ltManagedSettings=...). */
typedef struct lt_proj_symbols {
    char **on; size_t n_on;             /* symbols set */
    char **managed; size_t n_managed;   /* symbols decided by the project (#set/#unset ignored) */
    int present;                        /* the project declares ecuSettings */
} lt_proj_symbols;
void lt_proj_symbols_init(lt_proj_symbols *s);
void lt_proj_symbols_free(lt_proj_symbols *s);
int  lt_proj_symbols_load(const char *project_dir, lt_proj_symbols *s);    /* 0, also when absent */
int  lt_proj_symbols_save(const char *project_dir, const lt_proj_symbols *s);
/* options for lt_ini_load_* pointing into s (valid while s lives) */
void lt_proj_symbols_options(const lt_proj_symbols *s, lt_ini_options *o);
/* The symbols for a choice of setting group options: sel[i] is the option index of group i
 * (flags: 0 off, 1 on; option groups: -1 or the DEFAULT option = none). managed gets every
 * flag name and every non-DEFAULT option value of the INI. */
void lt_proj_symbols_from_choice(const lt_ini *ini, const int *sel, lt_proj_symbols *out);
/* Re-parse the open INI with the project's symbols, keeping the tune values, tabs and the
 * connection (app.c). 0 ok. */
int  lt_app_reload_ini(lt_app *app);

/* ---- port editor (dlg_portedit.c) ---- */
#define LT_PORT_MAX 32
typedef struct lt_port_cond {
    int channel;                    /* index into ini->channels, -1 = none / unknown offset */
    int op;                         /* operator character ('<', '=', '>', '&', ...), 0 = none */
    double threshold, hysteresis;   /* display units of the channel */
} lt_port_cond;
typedef struct lt_port_cfg {
    int enabled;
    int can_id;
    lt_port_cond c1, c2;
    int connector;                  /* ' ' none, '&' and, '|' or */
    int power_on, trigger;          /* pin state at power on / while the condition holds */
} lt_port_cfg;
/* number of ports of the editor (from the array sizes / port names) */
int  lt_port_count(const lt_ini *ini, const lt_ini_port_editor *pe);
/* name of port i (INI label or "Port N") */
void lt_port_name(const lt_ini_port_editor *pe, int i, char *buf, size_t cap);
int  lt_port_read(const lt_ini *ini, const lt_ini_port_editor *pe, const lt_pagesrc *src,
                  int port, lt_port_cfg *out);
/* write a port back; only elements that change are written (the pagesrc marks them dirty) */
int  lt_port_write(const lt_ini *ini, const lt_ini_port_editor *pe, lt_pagesrc *src, int port,
                   const lt_port_cfg *cfg);
/* output channels a condition can use (raw, non computed channels), in INI order */
int  lt_port_channel_ok(const lt_ini_channel *ch);
/* operator / connector labels of the editor (INI labels or the MS2 defaults) */
int  lt_port_ops(const lt_ini_port_editor *pe, const char **labels, int cap);
int  lt_port_conns(const lt_ini_port_editor *pe, const char **labels, int cap);
/* open the editor tab for an INI port editor name (NULL = the first one) */
int  lt_app_port_editor_open(lt_app *app, const char *name);
extern const lt_app_tab_kind lt_portedit_tab_kind;      /* "porteditor" */

#endif
