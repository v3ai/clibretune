/* setup_logic.c - the new-engine setup checklist without UI (see setup_logic.h).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <string.h>
#include "setup_logic.h"
#include "../project/project.h"

static const lt_setup_step k_steps[LT_SETUP_STEPS] = {
    { "engine", "Engine: displacement and cylinders",
      "Displacement, number of cylinders, firing order, and how the engine is injected." },
    { "injectors", "Injectors: flow and dead time",
      "Injector flow rate at your fuel pressure and the dead time (opening time) per voltage." },
    { "trigger", "Trigger wheel and cam sensor",
      "The crank wheel (e.g. 36-1, 60-2) and cam input; check the timing with a timing light later." },
    { "map", "MAP sensor",
      "The manifold pressure sensor type, so load is measured right." },
    { "tps", "Throttle position sensor",
      "Calibrate closed and wide-open throttle with the engine off." },
    { "temps", "Coolant and intake air temperature sensors",
      "The sensor types (or their resistance curves) for CLT and IAT." },
    { "wideband", "Wideband O2 sensor",
      "The AFR / lambda input and its calibration, needed for AutoTune and closed loop." },
    { "ignition", "Ignition: spark output and dwell",
      "Coil type, output mode (wasted spark / sequential), dwell and spark polarity." }
};

/* What the menus call each step, in order of preference: label words first, then dialog
 * names. Lists are '|'-separated (no string concatenation in initializers). */
static const char *const k_labels[LT_SETUP_STEPS] = {
    "Base engine|Engine Constants|Engine and Sequential|Engine Configuration|Engine Settings",
    "Injector Characteristics|Injection hardware|Injector Setup|Injector Deadtime|Injectors",
    "Trigger Setup|Trigger|Wheel Decoder|Ignition Options",
    "Manifold Pressure Sensor|Calibrate Pressure Sensors|MAP sensor|MAP Sensor Settings",
    "Throttle Position Sensor|Calibrate TPS|TPS Calibration|TPS/Pedal",
    "Calibrate Temperature Sensors|Coolant Temperature|Thermistor|Temperature Sensors|CLT sensor",
    "AFR/O2|Analog O2 sensor|Wideband|O2 sensor|Calibrate AFR Sensor|EGO",
    "Spark Settings|Ignition Hardware|Ignition Settings|Ignition Options|Dwell settings"
};
static const char *const k_targets[LT_SETUP_STEPS] = {
    "engineChars|engine_constants|engineSetup|engineConstants",
    "injChars|injectionSettings|injectorsDeadTime|injectorChars",
    "triggerConfiguration|triggerSettings|trigger",
    "mapSettings|mapCal",
    "tpsSensor|std_ms2tps|tpsCal",
    "cltSensor|std_ms2gentherm",
    "analogEgoSettings|egoControl|std_ms2geno2|egoSettings",
    "sparkSettings|ignitionSettings|ignitionOptions"
};

const lt_setup_step *lt_setup_step_at(int i)
{
    return i >= 0 && i < LT_SETUP_STEPS ? &k_steps[i] : NULL;
}

/* case-insensitive substring */
static int has_ci(const char *s, const char *w, size_t wl)
{
    size_t i, n = strlen(s);
    if (wl == 0 || wl > n) { return 0; }
    for (i = 0; i + wl <= n; i++) {
        if (lt_strnicmp(s + i, w, wl) == 0) { return 1; }
    }
    return 0;
}

static void clean_label(const char *in, char *out, size_t cap)
{
    size_t k = 0;
    if (cap == 0) { return; }
    while (in != NULL && *in != '\0' && k + 1 < cap) {
        if (*in != '&') { out[k++] = *in; }
        in++;
    }
    out[k] = '\0';
}

/* depth-first search of a menu tree for the first item whose label (by_target 0) or target
 * (1) contains the word */
static const lt_ini_menu_item *find_in(lt_ini_menu_item **items, size_t n, const char *w, size_t wl,
                                       int by_target, char *where, size_t wcap)
{
    size_t i;
    for (i = 0; i < n; i++) {
        const lt_ini_menu_item *it = items[i];
        if (it == NULL) { continue; }
        if (it->kind == LT_MENU_SUBMENU) {
            char sub[160];
            const lt_ini_menu_item *r;
            clean_label(it->label, sub, sizeof(sub));
            r = find_in(it->items, it->n_items, w, wl, by_target, where, wcap);
            if (r != NULL) {
                char tmp[160];
                lt_snprintf(tmp, sizeof(tmp), "%s > %s", sub, where);
                lt_strlcpy(where, tmp, wcap);
                return r;
            }
            continue;
        }
        if (it->kind == LT_MENU_SEPARATOR || it->kind == LT_MENU_HELP || it->target == NULL ||
            it->target[0] == '\0' || strcmp(it->target, "std_separator") == 0) {
            continue;
        }
        if (has_ci(by_target ? it->target : (it->label != NULL ? it->label : ""), w, wl)) {
            clean_label(it->label != NULL ? it->label : it->target, where, wcap);
            return it;
        }
    }
    return NULL;
}

static int match_one(const lt_ini *ini, const char *words, int by_target, lt_setup_link *out)
{
    const char *w = words;
    while (*w != '\0') {
        const char *e = strchr(w, '|');
        size_t wl = e != NULL ? (size_t)(e - w) : strlen(w), m;
        for (m = 0; m < ini->n_menus; m++) {
            const lt_ini_menu *menu = &ini->menus[m];
            char where[160], top[96];
            const lt_ini_menu_item *it = find_in(menu->items, menu->n_items, w, wl, by_target, where,
                                                 sizeof(where));
            if (it != NULL) {
                clean_label(menu->title != NULL ? menu->title : menu->name, top, sizeof(top));
                lt_strlcpy(out->target, it->target, sizeof(out->target));
                if (top[0] != '\0') { lt_snprintf(out->where, sizeof(out->where), "%s > %s", top, where); }
                else { lt_strlcpy(out->where, where, sizeof(out->where)); }
                return 1;
            }
        }
        if (e == NULL) { break; }
        w = e + 1;
    }
    return 0;
}

int lt_setup_match(const lt_ini *ini, lt_setup_link links[LT_SETUP_STEPS])
{
    int i, n = 0;
    for (i = 0; i < LT_SETUP_STEPS; i++) {
        links[i].target[0] = '\0';
        links[i].where[0] = '\0';
        if (ini == NULL) { continue; }
        if (match_one(ini, k_labels[i], 0, &links[i]) || match_one(ini, k_targets[i], 1, &links[i])) { n++; }
    }
    return n;
}

int lt_setup_count_done(const lt_setup_state *s)
{
    int i, n = 0;
    for (i = 0; i < LT_SETUP_STEPS; i++) { n += s->done[i] ? 1 : 0; }
    return n;
}

/* ------------------------------------------------------------------ */
int lt_setup_load(lt_setup_state *s, const char *dir)
{
    lt_props p;
    char *path = lt_path_join(dir, "setup.conf"), key[48];
    int i, v, fresh = 1;
    memset(s, 0, sizeof(*s));
    if (path == NULL) { return -1; }
    lt_props_init(&p);
    if (lt_file_exists(path)) {
        fresh = 0;
        if (lt_props_load(&p, path) != 0) { lt_props_free(&p); lt_free(path); return -1; }
        v = 0; lt_props_get_i32(&p, "dismissed", &v); s->dismissed = v != 0;
        v = 0; lt_props_get_i32(&p, "shown", &v); s->shown = v != 0;
        for (i = 0; i < LT_SETUP_STEPS; i++) {
            lt_snprintf(key, sizeof(key), "done.%s", k_steps[i].id);
            v = 0;
            lt_props_get_i32(&p, key, &v);
            s->done[i] = v != 0;
        }
    }
    lt_props_free(&p);
    lt_free(path);
    return fresh;
}

int lt_setup_save(const lt_setup_state *s, const char *dir)
{
    lt_props p;
    char *path = lt_path_join(dir, "setup.conf"), key[48];
    int i, rc;
    if (path == NULL) { return -1; }
    lt_props_init(&p);
    lt_props_set(&p, "dismissed", s->dismissed ? "1" : "0");
    lt_props_set(&p, "shown", s->shown ? "1" : "0");
    for (i = 0; i < LT_SETUP_STEPS; i++) {
        lt_snprintf(key, sizeof(key), "done.%s", k_steps[i].id);
        lt_props_set(&p, key, s->done[i] ? "1" : "0");
    }
    rc = lt_props_save(&p, path);
    lt_props_free(&p);
    lt_free(path);
    return rc;
}
