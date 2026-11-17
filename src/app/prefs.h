/* prefs.h - application preferences beyond the core settings (G-93, G-94, G-96, G-06, G-104):
 * table display, heat-map contexts with custom colour stops, behaviour toggles, protocol tuning,
 * status-bar channels and the open-tabs session.  Stored in <config>/prefs.conf (Java
 * properties, like settings.conf); the struct lives in lt_app_settings.prefs.
 * Pure data and helpers, no UI.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef LT_PREFS_H
#define LT_PREFS_H

#include "../lt.h"
#include "../project/project.h"

#define LT_PREFS_HEAT_CTX 3          /* value, change, coverage */
#define LT_PREFS_HEAT_STOPS 6        /* max custom colour stops per context */
#define LT_PREFS_SB_MAX 8            /* status bar channels */
#define LT_PREFS_SESSION_CAP 1024

enum { LT_HEAT_VALUE = 0, LT_HEAT_CHANGE = 1, LT_HEAT_COVERAGE = 2 };
enum { LT_PKT_AUTO = 0, LT_PKT_BURST, LT_PKT_OCH, LT_PKT_DISABLED };

typedef struct lt_prefs {
    /* protocol tuning (applies at the next connect) */
    int  packet_mode;                /* LT_PKT_* */
    int  adaptive_timing;
    int  fome_fast_comms;
    /* behaviour */
    int  auto_burn_close;
    int  change_alert;               /* warn on large cell changes */
    double change_abs, change_pct;
    int  help_icons;
    int  onboarded;
    /* table display */
    int  y_bottom;                   /* Y axis zero at the bottom */
    char cursor_color[8];            /* "#rrggbb" or "" = theme */
    char trail_color[8];
    int  trail_fade_sec;             /* 0 = never */
    char heat_scheme[LT_PREFS_HEAT_CTX][16];   /* scheme name, or "custom" */
    lt_u32 heat_stops[LT_PREFS_HEAT_CTX][LT_PREFS_HEAT_STOPS];   /* 0xRRGGBB */
    int  heat_n[LT_PREFS_HEAT_CTX];  /* number of custom stops */
    /* session */
    int  restore_session;
    char session_project[512];       /* project the saved tabs belong to */
    char open_tabs[LT_PREFS_SESSION_CAP];   /* "kind|target" entries separated by '\n' */
    int  last_active;
    /* status bar */
    char sb_chan[LT_PREFS_SB_MAX][32];
    int  sb_n;
} lt_prefs;

void lt_prefs_defaults(lt_prefs *p);
/* load/save <dir>/prefs.conf; load keeps defaults for missing keys. 0 ok or negative */
int  lt_prefs_load(lt_prefs *p, const char *dir);
int  lt_prefs_save(const lt_prefs *p, const char *dir);
/* the same through a property set (tests) */
void lt_prefs_from_props(lt_prefs *p, const lt_props *pr);
void lt_prefs_to_props(const lt_prefs *p, lt_props *pr);

/* "#rrggbb" <-> 0xRRGGBB; parse returns LT_FALSE for anything else */
lt_bool lt_prefs_parse_color(const char *s, lt_u32 *rgb);
void    lt_prefs_format_color(lt_u32 rgb, char out[8]);

/* heat-map colour of context ctx at t in 0..1.  LT_TRUE when the context uses custom stops
 * (colour in *out, opaque RGB, linear interpolation); LT_FALSE otherwise and *scheme gets the
 * stock scheme index 0..5 */
lt_bool lt_prefs_heat(const lt_prefs *p, int ctx, double t, lt_u32 *out, int *scheme);
int     lt_prefs_scheme_index(const char *name);   /* stock names, 0 when unknown */

/* large-change alert: LT_TRUE when |new-old| exceeds the absolute or the percent threshold
 * (percent of |old|; a zero old value only uses the absolute one) */
lt_bool lt_prefs_change_exceeds(const lt_prefs *p, double old_v, double new_v);

/* status bar channels */
int  lt_prefs_sb_has(const lt_prefs *p, const char *name);
int  lt_prefs_sb_toggle(lt_prefs *p, const char *name);   /* 1 added, 0 removed, -1 list full */

/* session: serialise "kind|target" pairs; the helpers keep the field bounded */
void lt_prefs_session_clear(lt_prefs *p);
int  lt_prefs_session_add(lt_prefs *p, const char *kind, const char *target);   /* 0 ok, -1 full */
/* n-th entry (0-based) split into kind/target; LT_FALSE past the end */
lt_bool lt_prefs_session_get(const lt_prefs *p, int n, char *kind, size_t kcap, char *target,
                             size_t tcap);

#endif
