/* test_iniext.c - INI driven dialog extras: components embedded in INI dialogs (curves,
 * tables, indicator panels, gauges, border layout), help topics, the programmable output
 * port editor, project INI settings ([SettingGroups] symbols), the pin conflict burn gate
 * and the "power cycle the ECU" notice.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "lt_test.h"
#include "../src/app/app.h"
#include "../src/app/iniext.h"
#include "../src/app/dialogs_embed.h"
#include <string.h>
#include <stdlib.h>

#define MS2_INI "tests/fixtures/ini/ms2_outputs.ini"

static int render(lt_app *app, const char *name)
{
    char path[200];
    lt_snprintf(path, sizeof(path), "build/test_iniext_%s.ppm", name);
    return lt_app_screenshot(app, path, 1400, 900, 3);
}

static int chan_index(const lt_ini *ini, const char *name)
{
    size_t i;
    for (i = 0; i < ini->n_channels; i++) {
        if (ini->channels[i]->name != NULL && strcmp(ini->channels[i]->name, name) == 0) { return (int)i; }
    }
    return -1;
}

/* open an INI dialog, draw it, return what it embedded */
static int dlg_counts(lt_app *app, const char *name, int *counts)
{
    memset(counts, 0, sizeof(int) * LT_EMB_KINDS);
    if (lt_app_dialog_open(app, name) != 0) { return -1; }
    if (render(app, name) != 0) { return -1; }
    return lt_dlg_ini_counts(app, name, counts);
}

LT_TEST(embedded_components_demo)
{
    char buf[512];
    int c[LT_EMB_KINDS];
    lt_app *app = lt_app_new();
    LT_CHECK(lt_app_start_demo(app, lt_app_find_demo_ini(buf, sizeof(buf))) == 0);
    /* curve dialog: the curve is an editable graph, no longer a button */
    LT_CHECK(dlg_counts(app, "cltFuelCorrCurveDialog", c) == 0);
    LT_CHECK(c[LT_EMB_CURVE] >= 1);
    /* border layout: settings panel West, knock threshold curve Center */
    LT_CHECK(dlg_counts(app, "softwareKnock", c) == 0);
    LT_CHECK(c[LT_EMB_CURVE] >= 1);
    LT_CHECK(c[LT_EMB_DIALOG] >= 1);
    /* indicator panel */
    LT_CHECK(dlg_counts(app, "fuel_computerDialog", c) == 0);
    LT_CHECK(c[LT_EMB_IND] >= 1);
    /* sensor dialog with a gauge panel East */
    LT_CHECK(dlg_counts(app, "auxTempSensor1Sensor", c) == 0);
    LT_CHECK(c[LT_EMB_GAUGE] >= 1);
    /* drawing an embedded editor does not edit anything */
    LT_CHECK(app->modified == 0);
    LT_CHECK_INT(lt_emb_kind(app, "no_such_panel"), LT_EMB_NONE);
    lt_app_free(app);
}

LT_TEST(embedded_curve_edit_and_tui)
{
    lt_app *app = lt_app_new();
    int c[LT_EMB_KINDS];
    lt_tblview v;
    lt_pagesrc ps;
    const lt_ini_curve *cv;
    LT_CHECK(lt_app_start_demo(app, MS2_INI) == 0);
    LT_CHECK(dlg_counts(app, "warmupDialog", c) == 0);
    LT_CHECK_INT(c[LT_EMB_CURVE], 1);
    LT_CHECK_INT(c[LT_EMB_IND], 1);
    LT_CHECK_INT(c[0], 0);
    LT_CHECK(dlg_counts(app, "engineSetup", c) == 0);
    LT_CHECK_INT(c[0], 1);                                  /* topicHelp: help button */
    LT_CHECK(app->modified == 0);
    /* an edit made elsewhere shows up in the embedded editor (same page source) */
    cv = lt_ini_find_curve(app->ini, "wueCurve");
    LT_CHECK(cv != NULL);
    ps = lt_app_pages(app);
    LT_CHECK(lt_ini_set(app->ini, &ps, "wueRates", 140.0) >= 0);
    lt_tblview_init(&v);
    LT_CHECK(lt_tblview_load_curve(&v, app->ini, cv, &ps) == 0);
    LT_CHECK_NEAR(v.z[0], 140.0, 1e-9);
    lt_tblview_free(&v);
    LT_CHECK(lt_app_dialog_open(app, "warmupDialog") == 0);
    lt_app_free(app);
}

LT_TEST(help_topics)
{
    char buf[512];
    lt_ini *ini = lt_ini_load_file(lt_app_find_demo_ini(buf, sizeof(buf)), NULL);
    lt_help_view hv;
    char *txt;
    const lt_ini_help *h;
    LT_CHECK(ini != NULL);
    if (ini == NULL) { return; }
    LT_CHECK_INT(lt_help_resolve(ini, "fuelHelp", &hv), LT_HELP_INI);
    LT_CHECK_INT(hv.kind, LT_HELP_INI);
    LT_CHECK_STR(hv.title, "Fuel Control");
    LT_CHECK(hv.url != NULL && strstr(hv.url, "rusefi.com") != NULL);
    LT_CHECK_INT(lt_help_resolve(ini, "https://example.org/x.html", &hv), LT_HELP_URL);
    LT_CHECK_INT(lt_help_resolve(ini, "noSuchHelpTopic", &hv), LT_HELP_NONE);
    lt_ini_free(ini);
    ini = lt_ini_load_file(MS2_INI, NULL);
    LT_CHECK(ini != NULL);
    if (ini == NULL) { return; }
    h = lt_ini_find_help(ini, "portHelp");
    LT_CHECK(h != NULL);
    if (h != NULL) {
        txt = lt_help_plain_text(h);
        LT_CHECK(txt != NULL && strstr(txt, "<br>") == NULL);
        LT_CHECK(txt != NULL && strstr(txt, "condition is true.\n") != NULL);
        LT_CHECK(txt != NULL && strstr(txt, "hysteresis") != NULL);
        lt_free(txt);
    }
    LT_CHECK(lt_ini_find_dialog(ini, "engineSetup")->help_topic != NULL);
    LT_CHECK(lt_ini_needs_power_cycle(ini, "nCylinders"));
    LT_CHECK(!lt_ini_needs_power_cycle(ini, "reqFuel"));
    lt_ini_free(ini);
    {   /* the viewer opens for a known topic, refuses an unknown one */
        lt_app *app = lt_app_new();
        LT_CHECK(lt_app_start_demo(app, MS2_INI) == 0);
        LT_CHECK_INT(lt_app_help_open(app, "portHelp"), 0);
        LT_CHECK(render(app, "help") == 0);
        LT_CHECK(lt_app_help_open(app, "noSuchHelpTopic") < 0);
        LT_CHECK_INT(lt_app_help_text(app, "Field", "Some help text"), 0);
        lt_app_free(app);
    }
}

LT_TEST(port_editor_parse)
{
    lt_ini *ini = lt_ini_load_file(MS2_INI, NULL);
    const lt_ini_port_editor *pe;
    LT_CHECK(ini != NULL);
    if (ini == NULL) { return; }
    pe = lt_ini_find_port_editor(ini, "std_port_edit");
    LT_CHECK(pe != NULL);
    if (pe != NULL) {
        LT_CHECK_STR(pe->label, "Output Port Settings");
        LT_CHECK_STR(pe->help_topic, "portHelp");
        LT_CHECK_STR(pe->enabled_const, "psEnabled");
        LT_CHECK_STR(pe->offset_const, "psOutOffset");
        LT_CHECK_STR(pe->threshold2_const, "psThreshold2");
        LT_CHECK_STR(pe->connector_const, "psConnector");
        LT_CHECK_INT((int)pe->n_port_names, 8);
        LT_CHECK_STR(pe->port_names[7], "PT4 - Nitrous");
        LT_CHECK_INT((int)pe->n_op_labels, 4);
        LT_CHECK_INT((int)pe->n_conn_labels, 3);
        LT_CHECK_INT(lt_port_count(ini, pe), 8);
    }
    lt_ini_free(ini);
}

LT_TEST(port_editor_round_trip)
{
    lt_app *app = lt_app_new();
    const lt_ini_port_editor *pe;
    lt_port_cfg in, out;
    lt_pagesrc ps;
    double raw;
    int n;
    LT_CHECK(lt_app_start_demo(app, MS2_INI) == 0);
    pe = lt_ini_find_port_editor(app->ini, NULL);
    LT_CHECK(pe != NULL);
    if (pe == NULL) { lt_app_free(app); return; }
    memset(&in, 0, sizeof(in));
    in.enabled = 1;
    in.c1.channel = chan_index(app->ini, "coolant");
    in.c1.op = '>';
    in.c1.threshold = 90.0;
    in.c1.hysteresis = 5.0;
    in.connector = '&';
    in.c2.channel = chan_index(app->ini, "rpm");
    in.c2.op = '<';
    in.c2.threshold = 3000.0;
    in.c2.hysteresis = 100.0;
    in.power_on = 0;
    in.trigger = 1;
    LT_CHECK(in.c1.channel >= 0 && in.c2.channel >= 0);
    ps = lt_app_pages(app);
    n = lt_port_write(app->ini, pe, &ps, 2, &in);
    LT_CHECK(n > 0);
    lt_app_edited(app);
    LT_CHECK(app->modified != 0);
    /* raw layout: MS2 character codes, channel offset/size, raw thresholds */
    LT_CHECK(lt_ini_const_get_raw(app->ini, lt_ini_find_const(app->ini, "psCondition"), &ps, 2, &raw) == 0);
    LT_CHECK_NEAR(raw, (double)'>', 1e-9);
    LT_CHECK(lt_ini_const_get_raw(app->ini, lt_ini_find_const(app->ini, "psConnector"), &ps, 2, &raw) == 0);
    LT_CHECK_NEAR(raw, (double)'&', 1e-9);
    LT_CHECK(lt_ini_const_get_raw(app->ini, lt_ini_find_const(app->ini, "psOutOffset"), &ps, 2, &raw) == 0);
    LT_CHECK_NEAR(raw, 22.0, 1e-9);
    LT_CHECK(lt_ini_const_get_raw(app->ini, lt_ini_find_const(app->ini, "psOutSize"), &ps, 2, &raw) == 0);
    LT_CHECK_NEAR(raw, 2.0, 1e-9);
    LT_CHECK(lt_ini_const_get_raw(app->ini, lt_ini_find_const(app->ini, "psThreshold"), &ps, 2, &raw) == 0);
    LT_CHECK_NEAR(raw, 900.0, 1e-9);
    LT_CHECK(lt_ini_const_get_raw(app->ini, lt_ini_find_const(app->ini, "psOutOffset2"), &ps, 2, &raw) == 0);
    LT_CHECK_NEAR(raw, 6.0, 1e-9);
    /* other ports untouched */
    LT_CHECK(lt_ini_const_get_raw(app->ini, lt_ini_find_const(app->ini, "psEnabled"), &ps, 1, &raw) == 0);
    LT_CHECK_NEAR(raw, 0.0, 1e-9);
    memset(&out, 0, sizeof(out));
    LT_CHECK(lt_port_read(app->ini, pe, &ps, 2, &out) == 0);
    LT_CHECK_INT(out.enabled, 1);
    LT_CHECK_INT(out.c1.channel, in.c1.channel);
    LT_CHECK_INT(out.c1.op, '>');
    LT_CHECK_NEAR(out.c1.threshold, 90.0, 1e-6);
    LT_CHECK_NEAR(out.c1.hysteresis, 5.0, 1e-6);
    LT_CHECK_INT(out.connector, '&');
    LT_CHECK_INT(out.c2.channel, in.c2.channel);
    LT_CHECK_INT(out.c2.op, '<');
    LT_CHECK_NEAR(out.c2.threshold, 3000.0, 1e-6);
    LT_CHECK_INT(out.power_on, 0);
    LT_CHECK_INT(out.trigger, 1);
    /* writing the same configuration again changes nothing */
    LT_CHECK_INT(lt_port_write(app->ini, pe, &ps, 2, &out), 0);
    /* a second port, then the editor screen */
    in.enabled = 1; in.connector = ' '; in.c1.channel = chan_index(app->ini, "rpm"); in.c1.op = '>';
    in.c1.threshold = 6500.0; in.c1.hysteresis = 200.0;
    in.connector = '|'; in.c2.channel = chan_index(app->ini, "map"); in.c2.op = '>'; in.c2.threshold = 180.0;
    LT_CHECK(lt_port_write(app->ini, pe, &ps, 0, &in) > 0);
    lt_app_edited(app);
    LT_CHECK_INT(lt_app_port_editor_open(app, NULL), 0);
    LT_CHECK(lt_app_tab_find(app, "porteditor", "std_port_edit") != NULL);
    LT_CHECK(render(app, "porteditor") == 0);
    /* through the menu target as well */
    LT_CHECK_INT(lt_app_open_target(app, "std_port_edit", "Output Port Settings"), 0);
    lt_app_free(app);
}

static const char sym_ini[] =
    "[SettingGroups]\n"
    "  settingGroup = pressure_units, \"Pressure\"\n"
    "  settingOption = DEFAULT, \"kPa\"\n"
    "  settingOption = pressure_bar, \"bar\"\n"
    "  settingGroup = enablehardware_test, \"Hardware test\"\n"
    "[MegaTune]\n"
    "  signature = \"symtest\"\n"
    "#unset enablehardware_test\n"
    "[Constants]\n"
    "page = 1\n"
    "#if pressure_bar\n"
    "  boost = scalar, U16, 0, \"bar\", 0.01, 0, 0, 3, 2\n"
    "#else\n"
    "  boost = scalar, U16, 0, \"kPa\", 1, 0, 0, 300, 0\n"
    "#endif\n"
    "#if enablehardware_test\n"
    "  hwTest = scalar, U08, 2, \"\", 1, 0, 0, 1, 0\n"
    "#endif\n";

LT_TEST(project_symbols)
{
    lt_ini *ini;
    lt_ini_options o;
    lt_proj_symbols s, back;
    const lt_ini_const *c;
    int sel[2];
    const char *dir = "build/test_iniext_proj";
    /* INI defaults */
    ini = lt_ini_load_string(sym_ini, strlen(sym_ini), NULL, NULL);
    LT_CHECK(ini != NULL);
    if (ini == NULL) { return; }
    c = lt_ini_find_const(ini, "boost");
    LT_CHECK(c != NULL && strcmp(c->units, "kPa") == 0);
    LT_CHECK(lt_ini_find_const(ini, "hwTest") == NULL);
    LT_CHECK_INT((int)ini->n_setting_groups, 2);
    /* choose bar + hardware test */
    sel[0] = 1; sel[1] = 1;
    lt_proj_symbols_init(&s);
    lt_proj_symbols_from_choice(ini, sel, &s);
    lt_ini_free(ini);
    LT_CHECK_INT((int)s.n_on, 2);
    LT_CHECK_INT((int)s.n_managed, 2);           /* pressure_bar, enablehardware_test */
    lt_proj_symbols_options(&s, &o);
    ini = lt_ini_load_string(sym_ini, strlen(sym_ini), NULL, &o);
    LT_CHECK(ini != NULL);
    if (ini != NULL) {
        c = lt_ini_find_const(ini, "boost");
        LT_CHECK(c != NULL && strcmp(c->units, "bar") == 0);
        /* the file's "#unset enablehardware_test" does not override the project */
        LT_CHECK(lt_ini_find_const(ini, "hwTest") != NULL);
        LT_CHECK_INT(lt_ini_setting_active(ini, ini->setting_groups[0]), 1);
        LT_CHECK_INT(lt_ini_setting_active(ini, ini->setting_groups[1]), 0);
        lt_ini_free(ini);
    }
    /* stored TunerStudio style and read back */
    lt_plat_mkdirs("build/test_iniext_proj/projectCfg");
    LT_CHECK_INT(lt_proj_symbols_save(dir, &s), 0);
    lt_proj_symbols_init(&back);
    LT_CHECK_INT(lt_proj_symbols_load(dir, &back), 0);
    LT_CHECK(back.present);
    LT_CHECK_INT((int)back.n_on, 2);
    LT_CHECK_INT((int)back.n_managed, (int)s.n_managed);
    lt_proj_symbols_free(&back);
    lt_proj_symbols_free(&s);
    /* back to the defaults: DEFAULT option and flag off = no symbols */
    sel[0] = 0; sel[1] = 0;
    ini = lt_ini_load_string(sym_ini, strlen(sym_ini), NULL, NULL);
    lt_proj_symbols_init(&s);
    lt_proj_symbols_from_choice(ini, sel, &s);
    LT_CHECK_INT((int)s.n_on, 0);
    lt_ini_free(ini);
    lt_proj_symbols_free(&s);
}

LT_TEST(reload_keeps_tune)
{
    lt_app *app = lt_app_new();
    lt_pagesrc ps;
    double v = 0.0;
    LT_CHECK(lt_app_start_demo(app, MS2_INI) == 0);
    ps = lt_app_pages(app);
    LT_CHECK(lt_ini_set(app->ini, &ps, "reqFuel", 7.5) >= 0);
    lt_app_edited(app);
    LT_CHECK(lt_app_dialog_open(app, "engineSetup") == 0);
    LT_CHECK(render(app, "reload1") == 0);
    LT_CHECK_INT(lt_app_reload_ini(app), 0);
    LT_CHECK(app->ini != NULL);
    ps = lt_app_pages(app);
    LT_CHECK(lt_ini_get(app->ini, &ps, "reqFuel", &v) == 0);
    LT_CHECK_NEAR(v, 7.5, 1e-3);
    LT_CHECK(lt_app_tab_find(app, "dialog", "engineSetup") != NULL);
    LT_CHECK(render(app, "reload2") == 0);
    lt_app_free(app);
}

static const lt_pin_report *pin_dialog_report(lt_app *app)
{
    int i;
    for (i = 0; i < app->n_dialogs; i++) {
        if (strcmp(app->dialogs[i]->kind, "pin-report") == 0) { return (const lt_pin_report *)app->dialogs[i]->data; }
    }
    return NULL;
}

LT_TEST(pin_conflict_gate_and_power_cycle)
{
    lt_app *app = lt_app_new();
    lt_pagesrc ps;
    lt_pin_report r;
    int i, open = 0;
    LT_CHECK(lt_app_start_demo(app, MS2_INI) == 0);
    LT_CHECK_INT(lt_app_pin_conflicts(app, NULL), 0);
    /* idle valve and tacho on the same pin */
    ps = lt_app_pages(app);
    LT_CHECK(lt_ini_set(app->ini, &ps, "idlePin", 1.0) >= 0);
    LT_CHECK(lt_ini_set(app->ini, &ps, "tachoPin", 1.0) >= 0);
    LT_CHECK(lt_ini_set(app->ini, &ps, "nCylinders", 6.0) >= 0);    /* needs a power cycle */
    lt_app_edited(app);
    LT_CHECK_INT(lt_app_pin_conflicts(app, &r), 1);
    if (r.n == 1) {
        LT_CHECK_STR(r.c[0].pin_label, "PM3");
        LT_CHECK_INT((int)r.c[0].n, 2);
    }
    lt_pin_report_free(&r);
    LT_CHECK(lt_app_burn(app) < 0);                 /* held back */
    for (i = 0; i < app->n_dialogs; i++) {
        if (strcmp(app->dialogs[i]->kind, "pin-report") == 0 && app->dialogs[i]->open) { open = 1; }
    }
    LT_CHECK(open);
    LT_CHECK(render(app, "pins") == 0);
    /* scanned once per opening, not every frame (~1M bit options in a large INI: 230 ms a frame) */
    {
        const lt_pin_report *shown = pin_dialog_report(app);
        LT_CHECK(shown != NULL && shown->n == 1);
        LT_CHECK(render(app, "pins") == 0);
        LT_CHECK(pin_dialog_report(app) == shown);
    }
    LT_CHECK_STR(lt_app_pc_pending(app), "");
    lt_app_pin_ack(app);                            /* "Burn anyway" */
    for (i = 0; i < app->n_dialogs; i++) { app->dialogs[i]->open = 0; }
    LT_CHECK_INT(lt_app_burn(app), 0);
    LT_CHECK(strstr(lt_app_pc_pending(app), "nCylinders") != NULL);
    LT_CHECK(render(app, "powercycle") == 0);
    /* a new conflict set needs a new acknowledgement */
    ps = lt_app_pages(app);
    LT_CHECK(lt_ini_set(app->ini, &ps, "idlePin", 2.0) >= 0);
    LT_CHECK(lt_ini_set(app->ini, &ps, "tachoPin", 2.0) >= 0);
    lt_app_edited(app);
    LT_CHECK(lt_app_burn(app) < 0);
    /* resolved: burns without asking */
    ps = lt_app_pages(app);
    LT_CHECK(lt_ini_set(app->ini, &ps, "tachoPin", 0.0) >= 0);
    lt_app_edited(app);
    for (i = 0; i < app->n_dialogs; i++) { app->dialogs[i]->open = 0; }
    LT_CHECK_INT(lt_app_burn(app), 0);
    lt_app_free(app);
}

int main(void)
{
    LT_RUN(embedded_components_demo);
    LT_RUN(embedded_curve_edit_and_tui);
    LT_RUN(help_topics);
    LT_RUN(port_editor_parse);
    LT_RUN(port_editor_round_trip);
    LT_RUN(project_symbols);
    LT_RUN(reload_keeps_tune);
    LT_RUN(pin_conflict_gate_and_power_cycle);
    LT_TEST_MAIN_END
}
