/* test_ini_fixtures.c - parse the real INI fixtures and assert key counts.
 *
 * The counts were verified independently of the parser with an awk script that
 * follows the #if/#else arms (empty symbol seed) and counts distinct names:
 *   demo.ini: constants 1863 + pc variables 32, output channels 1129,
 *   tables 98, curves 70, menus 11, dialogs 444, gauges 331, datalog entries 1105.
 * Tests run from the project root (build.sh does that).
 */
#include <stdlib.h>
#include <string.h>
#include "lt.h"
#include "ini/ini.h"
#include "lt_test.h"

#define DEMO "tests/fixtures/ini/demo.ini"
#define SPEEDUINO "tests/fixtures/ini/speeduino_202501_na6.ini"
#define MS3 "tests/fixtures/ini/ms3.ini"
#define MS2X "tests/fixtures/ini/ms2extra.ini"

typedef struct fxstate { lt_u8 *p[3]; size_t len[3]; } fxstate;

static const lt_u8 *fx_page(void *ud, int page, size_t *len)
{
    fxstate *st = (fxstate *)ud;
    if (page < 0 || page > 2) return NULL;
    *len = st->len[page];
    return st->p[page];
}

static int fx_write(void *ud, int page, size_t off, const lt_u8 *d, size_t n)
{
    fxstate *st = (fxstate *)ud;
    if (page < 0 || page > 2 || off + n > st->len[page]) return LT_ERANGE;
    memcpy(st->p[page] + off, d, n);
    return 0;
}

static lt_ini *load(const char *path, const char *const *syms, size_t n)
{
    lt_ini_options o;
    lt_ini *ini;
    lt_ini_options_default(&o);
    o.symbols = syms;
    o.n_symbols = n;
    ini = lt_ini_load_file(path, &o);
    if (!ini) fprintf(stderr, "load %s failed: %s\n", path, lt_err_get());
    return ini;
}

LT_TEST(all_fixtures_parse)
{
    const char *files[4];
    size_t i;
    files[0] = DEMO;
    files[1] = SPEEDUINO;
    files[2] = MS3;
    files[3] = MS2X;
    for (i = 0; i < 4; i++) {
        lt_ini *ini = load(files[i], NULL, 0);
        LT_CHECK(ini != NULL);
        if (ini) {
            LT_CHECK_STR(ini->source_path, files[i]);
            lt_ini_free(ini);
        }
    }
}

LT_TEST(demo_identity_and_protocol)
{
    lt_ini *ini = load(DEMO, NULL, 0);
    const lt_ini_protocol *p;
    size_t i;
    if (!ini) return;
    p = &ini->proto;
    LT_CHECK_STR(ini->signature, "rusEFI 2025062409.2025.06.26.epicECU.2089451084");
    LT_CHECK(ini->ecu_type == LT_ECU_EPICEFI);   /* "epicecu" is tested before "rusefi" */
    LT_CHECK(ini->endian == LT_ENDIAN_LITTLE);
    LT_CHECK_INT(ini->n_pages, 3);
    LT_CHECK_INT(p->n_page_sizes, 3);
    LT_CHECK_INT(p->page_sizes[0], 40560);
    LT_CHECK_INT(p->page_sizes[1], 256);
    LT_CHECK_INT(p->page_sizes[2], 2048);
    LT_CHECK_STR(p->message_envelope_format, "msEnvelope_1.0");
    LT_CHECK(p->uses_modern);
    LT_CHECK_STR(p->query_command.p, "S");
    LT_CHECK_INT(p->n_page_identifiers, 3);
    for (i = 0; i < 3; i++) {
        LT_CHECK(p->page_identifiers[i].n == 2 && p->page_identifiers[i].p[0] == 0 &&
                 p->page_identifiers[i].p[1] == (char)i);
        LT_CHECK_STR(p->page_read_commands.v[i].p, "R%2i%2o%2c");
        LT_CHECK_STR(p->crc32_check_commands.v[i].p, "k%2i%2o%2c");
    }
    LT_CHECK_INT(p->burn_commands.n, 3);
    LT_CHECK_STR(p->burn_commands.v[0].p, "B%2i");
    LT_CHECK_INT(p->burn_commands.v[1].n, 0);
    LT_CHECK_INT(p->burn_commands.v[2].n, 0);
    LT_CHECK_STR(p->retrieve_config_error, "e");
    LT_CHECK_INT(p->page_activation_delay, 500);
    LT_CHECK_INT(p->write_blocks, 1);
    LT_CHECK_INT(p->inter_write_delay, 10);
    LT_CHECK_INT(p->block_read_timeout, 3000);
    LT_CHECK_INT(p->blocking_factor, 2560);
    LT_CHECK_STR(p->och_get_command.p, "O%2o%2c");
    LT_CHECK_INT(p->och_block_size, 2268);
    LT_CHECK_INT(lt_ini_realtime_size(ini), 2268);
    LT_CHECK_STR(p->burst_get_command.p, "A");
    LT_CHECK_INT(p->default_ip_port, 29001);
    LT_CHECK_INT(p->enable_can_id, 1);   /* the top-level 'enable2ndByteCanID = false' is ignored */
    LT_CHECK(lt_ini_supports_console(ini));
    LT_CHECK_INT(lt_ini_total_memory(ini), 40560 + 256 + 2048);
    LT_CHECK(lt_ini_symbol_tested(ini, "LAMBDA"));
    LT_CHECK(!lt_ini_symbol_active(ini, "LAMBDA"));
    lt_ini_free(ini);
}

LT_TEST(demo_counts)
{
    lt_ini *ini = load(DEMO, NULL, 0);
    size_t n_pc = 0, i;
    if (!ini) return;
    for (i = 0; i < ini->n_constants; i++)
        if (ini->constants[i]->is_pc_var) n_pc++;
    LT_CHECK_INT(ini->n_constants, 1863 + 32);
    LT_CHECK_INT(n_pc, 32);
    LT_CHECK_INT(ini->n_channels, 1129);
    LT_CHECK_INT(ini->n_tables, 98);
    LT_CHECK_INT(ini->n_curves, 70);
    LT_CHECK_INT(ini->n_menus, 11);
    LT_CHECK_INT(ini->n_dialogs, 444);
    LT_CHECK_INT(ini->n_gauges, 331);
    LT_CHECK_INT(ini->n_dl_entries, 1105);
    LT_CHECK_INT(ini->n_loggers, 2);
    LT_CHECK(ini->frontpage != NULL && ini->frontpage->n_gauges == 8);
    /* every constant/channel is reachable by name and indexes line up */
    for (i = 0; i < ini->n_constants; i++) {
        LT_CHECK(lt_ini_find_const(ini, ini->constants[i]->name) == ini->constants[i]);
        LT_CHECK(ini->constants[i]->order == i);
    }
    for (i = 0; i < ini->n_channels; i++) {
        LT_CHECK(lt_ini_find_channel(ini, ini->channels[i]->name) == ini->channels[i]);
        LT_CHECK(ini->channels[i]->index == i);
    }
    lt_ini_free(ini);
}

LT_TEST(demo_content)
{
    lt_ini *ini = load(DEMO, NULL, 0);
    const lt_ini_table *t;
    const lt_ini_const *c;
    const lt_ini_gauge *g;
    size_t i;
    if (!ini) return;

    /* tables: roles inferred from [VeAnalyze] */
    t = lt_ini_find_table(ini, "veTableTbl");
    LT_CHECK(t != NULL);
    if (t) {
        LT_CHECK(t->role == LT_ROLE_VE);
        LT_CHECK_INT(t->x_size, 32);
        LT_CHECK_INT(t->y_size, 32);
        LT_CHECK_STR(t->z_const, "veTable");
        LT_CHECK(lt_ini_find_table(ini, "veTableMap") == t);
    }
    t = lt_ini_find_table(ini, "afrTableTbl");
    LT_CHECK(t != NULL && t->role == LT_ROLE_AFR_TARGET);
    t = lt_ini_find_table(ini, "ignitionTableTbl");
    LT_CHECK(t != NULL && t->role == LT_ROLE_IGNITION && t->x_size == 32);
    LT_CHECK(ini->ve_analyze.present && ini->wue_analyze.present);
    LT_CHECK_STR(ini->ve_analyze.ve_table, "veTableTbl");

    c = lt_ini_find_const(ini, "veTable");
    LT_CHECK(c != NULL);
    if (c) {
        LT_CHECK(c->type == LT_DT_U16 && c->offset == 14868 && c->page == 0);
        LT_CHECK_INT(c->shape.rows, 32);
        LT_CHECK_INT(c->shape.cols, 32);
        LT_CHECK_NEAR(c->scale, 0.01, 1e-12);
        LT_CHECK_INT(c->size_bytes, 2048);
    }
    c = lt_ini_find_const(ini, "fuelAlgorithm");
    LT_CHECK(c != NULL && c->type == LT_DT_BITS && c->bit_pos == 0 && c->bit_size == 3);
    if (c) {
        LT_CHECK_INT(c->n_bit_options, 8);
        LT_CHECK_STR(c->bit_options[0], "Speed Density");
        LT_CHECK_STR(c->bit_options[2], "Alpha-N");
    }
    c = lt_ini_find_const(ini, "veLoadBins");
    LT_CHECK(c != NULL && c->shape.n == 32);

    /* gauge titles may be expressions (resolved by the caller) */
    g = lt_ini_find_gauge(ini, "gppwmGauge1");
    LT_CHECK(g != NULL && strstr(g->title, "stringValue") != NULL);

    /* menus: every item has a target or a label; item kinds were retyped */
    LT_CHECK_STR(ini->menus[0].name, "&Setup");
    for (i = 0; i < ini->n_menus; i++) {
        size_t k;
        LT_CHECK(ini->menus[i].n_items > 0);
        for (k = 0; k < ini->menus[i].n_items; k++) {
            const lt_ini_menu_item *m = ini->menus[i].items[k];
            if (m->kind == LT_MENU_TABLE)
                LT_CHECK(lt_ini_find_table(ini, m->target) != NULL ||
                         lt_ini_find_curve(ini, m->target) != NULL);
            if (m->kind == LT_MENU_HELP) LT_CHECK(lt_ini_find_help(ini, m->target) != NULL);
        }
    }

    /* rusEFI sparse bit option lists keep their positions */
    c = lt_ini_find_const(ini, "engineType");
    LT_CHECK(c != NULL && c->type == LT_DT_BITS && c->n_bit_options > 50);
    if (c) {
        LT_CHECK_STR(c->bit_options[0], "DEFAULT_FRANKENSO");
        LT_CHECK_STR(c->bit_options[22], "BMW_M52");
    }
    lt_ini_free(ini);
}

LT_TEST(demo_loggers)
{
    lt_ini *ini = load(DEMO, NULL, 0);
    size_t i;
    if (!ini) return;
    for (i = 0; i < ini->n_loggers; i++) {
        const lt_ini_logger *l = &ini->loggers[i];
        LT_CHECK(l->name[0] != '\0');
        LT_CHECK(l->start_command.n > 0);
        LT_CHECK(l->data_read_command.n > 0);
        LT_CHECK(strcmp(l->start_command.p, l->data_read_command.p) != 0);
        LT_CHECK(l->record_len > 0);
        LT_CHECK(l->n_fields > 0);
        LT_CHECK(l->continuous_read);
    }
    LT_CHECK_STR(ini->loggers[0].name, "compositeLogger");
    LT_CHECK_STR(ini->loggers[0].data_ready_condition, "toothLogReady");
    LT_CHECK_INT(ini->loggers[0].record_len, 5);
    lt_ini_free(ini);
}

LT_TEST(demo_realtime_frame)
{
    lt_ini *ini = load(DEMO, NULL, 0);
    lt_ini_rt *rt;
    lt_u8 *frame;
    size_t n, i, valid = 0, computed = 0;
    const lt_ini_channel *rpm, *ego;
    double v, stft;
    if (!ini) return;
    n = lt_ini_realtime_size(ini);
    frame = (lt_u8 *)lt_calloc(n, 1);
    rpm = lt_ini_find_channel(ini, "RPMValue");
    LT_CHECK(rpm != NULL);
    LT_CHECK(lt_dtype_write(rpm->type, frame, n, rpm->offset, ini->endian, 3000.0 / rpm->scale) == 0);
    frame[0] = 0x01;   /* sd_present */
    rt = lt_ini_rt_new(ini);
    LT_CHECK(lt_ini_rt_update(rt, frame, n) == 0);
    LT_CHECK(lt_ini_rt_get(rt, "RPMValue", &v) && v == 3000);
    LT_CHECK(lt_ini_rt_get(rt, "sd_present", &v) && v == 1);
    LT_CHECK(lt_ini_rt_get(rt, "sd_logging_internal", &v) && v == 0);
    ego = lt_ini_find_channel(ini, "egoCorrectionForVeAnalyze");
    LT_CHECK(ego != NULL && ego->expression != NULL);
    LT_CHECK(lt_ini_rt_get(rt, "stftCorrection1", &stft));
    LT_CHECK(lt_ini_rt_get(rt, "egoCorrectionForVeAnalyze", &v) && v == 100.0 + stft);
    for (i = 0; i < ini->n_channels; i++) {
        if (lt_ini_rt_valid(rt, i)) valid++;
        if (ini->channels[i]->expression) computed++;
    }
    LT_CHECK_INT(computed, 3);   /* time, egoCorrectionForVeAnalyze, wbo0_hasFault (grep) */
    LT_CHECK(valid > 1000);
    /* a frame shorter than the block still decodes what fits */
    LT_CHECK(lt_ini_rt_update(rt, frame, 100) == LT_ERANGE);
    LT_CHECK(lt_ini_rt_get(rt, "RPMValue", &v) && v == 3000);
    lt_ini_rt_free(rt);
    lt_free(frame);
    lt_ini_free(ini);
}

LT_TEST(demo_constant_codec)
{
    lt_ini *ini = load(DEMO, NULL, 0);
    lt_u8 *pages[3];
    size_t i;
    const lt_ini_const *c;
    double v;
    fxstate st;
    lt_pagesrc src;
    if (!ini) return;
    for (i = 0; i < 3; i++) {
        st.len[i] = ini->proto.page_sizes[i];
        pages[i] = (lt_u8 *)lt_calloc(st.len[i], 1);
        st.p[i] = pages[i];
    }
    src.ud = &st;
    src.page = fx_page;
    src.write = fx_write;
    c = lt_ini_find_const(ini, "veTable");
    LT_CHECK(c != NULL);
    if (c) {
        LT_CHECK(lt_ini_const_set(ini, c, &src, 33, 87.5) == 0);   /* element [1][1] */
        LT_CHECK(lt_ini_const_get(ini, c, &src, 33, &v) == 0);
        LT_CHECK_NEAR(v, 87.5, 1e-9);
        LT_CHECK(lt_ini_const_get_raw(ini, c, &src, 33, &v) == 0 && v == 8750);
        /* little endian page data */
        LT_CHECK(pages[0][14868 + 66] == (8750 & 0xFF) && pages[0][14868 + 67] == (8750 >> 8));
    }
    c = lt_ini_find_const(ini, "fuelAlgorithm");
    LT_CHECK(c != NULL);
    if (c) {
        LT_CHECK(lt_ini_const_set(ini, c, &src, 0, 4) == 0);
        LT_CHECK(lt_ini_const_get(ini, c, &src, 0, &v) == 0 && v == 4);
        LT_CHECK(pages[0][480] == 4);
    }
    for (i = 0; i < 3; i++) lt_free(pages[i]);
    lt_ini_free(ini);
}

LT_TEST(speeduino_reference_tables)
{
    lt_ini *ini = load(SPEEDUINO, NULL, 0);
    const lt_ini_ref_table *th, *afr;
    size_t i;
    int found_mazda = 0, found_vw = 0;
    if (!ini) return;
    LT_CHECK_INT(ini->n_ref_tables, 2);
    th = lt_ini_find_ref_table(ini, "std_ms2gentherm");
    afr = lt_ini_find_ref_table(ini, "std_ms2geno2");
    LT_CHECK(th != NULL && afr != NULL);
    LT_CHECK_INT(ini->proto.table_blocking_factor, 256);   /* mcu_stm32 / COMMS_COMPAT not set */
    LT_CHECK_INT(1024 / ini->proto.table_blocking_factor, 4);
    if (th) {
        LT_CHECK_STR(th->label, "Calibrate Thermistor Tables.");
        LT_CHECK_INT(th->adc_count, 32);
        LT_CHECK_INT(th->bytes_per_adc, 2);
        LT_CHECK_NEAR(th->scale, 10, 0);
        LT_CHECK_INT(lt_ini_ref_wire_bytes(th), 64);
        LT_CHECK_INT(th->n_idents, 2);
        LT_CHECK(th->idents[0].id == 0 && th->idents[1].id == 1);
        LT_CHECK_STR(th->idents[0].label, "Coolant Temperature Sensor");
        LT_CHECK_STR(th->idents[1].label, "Air Temperature Sensor");
        LT_CHECK(th->idents[0].has_limits && th->idents[0].lim_min == -40 &&
                 th->idents[0].lim_max == 350 && th->idents[0].lim_default == 180);
        LT_CHECK(th->idents[1].has_limits && th->idents[1].lim_default == 70);
        LT_CHECK(th->topic_help != NULL && strstr(th->topic_help, "wiki") != NULL);
        LT_CHECK_INT(th->n_therm_opts, 12);
        for (i = 0; i < th->n_therm_opts; i++) {
            const lt_ini_therm_opt *o = &th->therm_opts[i];
            if (strcmp(o->name, "Mazda") == 0) {
                found_mazda = 1;
                LT_CHECK_NEAR(o->bias_resistor, 50000, 0);
                LT_CHECK_NEAR(o->t[0], -40, 0);
                LT_CHECK_NEAR(o->r[0], 2022088, 0);
                LT_CHECK_NEAR(o->t[2], 99, 0);
                LT_CHECK_NEAR(o->r[2], 3715, 0);
            }
            if (strcmp(o->name, "VW L-Jet Cylinder Head Temp Sensor II") == 0) {
                found_vw = 1;
                LT_CHECK_NEAR(o->bias_resistor, 1100, 0);
                LT_CHECK_NEAR(o->t[0], -13.888, 1e-9);
                LT_CHECK_NEAR(o->r[0], 11600, 0);
            }
        }
        LT_CHECK(found_mazda && found_vw);
    }
    if (afr) {
        LT_CHECK_STR(afr->label, "Calibrate AFR Table...");
        LT_CHECK_INT(afr->adc_count, 1024);
        LT_CHECK_INT(afr->bytes_per_adc, 1);
        LT_CHECK_NEAR(afr->scale, 10, 0);
        LT_CHECK_INT(lt_ini_ref_wire_bytes(afr), 1024);
        LT_CHECK_INT(afr->n_idents, 1);
        LT_CHECK(afr->idents[0].id == 2 && !afr->idents[0].has_limits);
        LT_CHECK_STR(afr->solutions_label, "EGO Sensor");
        LT_CHECK(afr->n_solutions > 20);
        {
            int with_comma = 0;
            for (i = 0; i < afr->n_solutions; i++)
                if (afr->solutions[i].expression && strchr(afr->solutions[i].expression, ','))
                    with_comma++;
            LT_CHECK(with_comma > 0);   /* expressions keep their commas (e.g. table(x, "nb.inc")) */
        }
        for (i = 0; i < afr->n_generators; i++) {
            const lt_ini_generator *g = &afr->generators[i];
            if (strcmp(g->kind, "linearGenerator") == 0) {
                LT_CHECK_STR(g->label, "Custom Linear WB");
                LT_CHECK_STR(g->x_units, "Volts");
                LT_CHECK_STR(g->y_units, "AFR");
                LT_CHECK(g->has_bounds && g->x_lo == 1 && g->x_hi == 4 && g->y_lo == 9.7 &&
                         g->y_hi == 18.7);
            }
            if (strcmp(g->kind, "fileBrowseGenerator") == 0) LT_CHECK(!g->has_bounds);
        }
    }
    lt_ini_free(ini);
    {
        const char *syms[] = {"mcu_stm32"};
        ini = load(SPEEDUINO, syms, 1);
        if (ini) {
            LT_CHECK_INT(ini->proto.table_blocking_factor, 64);
            lt_ini_free(ini);
        }
    }
}

LT_TEST(ms3_reference_tables)
{
    lt_ini *ini = load(MS3, NULL, 0);
    const lt_ini_ref_table *t;
    if (!ini) return;
    t = lt_ini_find_ref_table(ini, "std_ms2gentherm");
    LT_CHECK(t != NULL);
    if (t) {
        LT_CHECK_INT(t->n_idents, 3);
        LT_CHECK(t->idents[0].id == 0 && t->idents[1].id == 1 && t->idents[2].id == 3);
        LT_CHECK_STR(t->idents[2].label, "Custom#1 Temperature Sensor");
        LT_CHECK_INT(t->adc_count, 1024);
        LT_CHECK_INT(t->bytes_per_adc, 2);
        LT_CHECK_INT(lt_ini_ref_wire_bytes(t), 2048);
        LT_CHECK(t->idents[0].has_limits && t->idents[0].lim_max == 350 &&
                 t->idents[0].lim_default == 180);
        LT_CHECK(t->idents[2].lim_max == 400 && t->idents[2].lim_default == 70);
    }
    LT_CHECK_INT(ini->proto.table_blocking_factor, 2048);
    LT_CHECK_STR(ini->proto.table_write_command, "w\\$tsCanId%2i%2o%2c%v");
    lt_ini_free(ini);
    {
        const char *syms[] = {"CAN_COMMANDS", "EXPANDED_CLT_TEMP"};
        ini = load(MS3, syms, 2);
        if (ini) {
            t = lt_ini_find_ref_table(ini, "std_ms2gentherm");
            LT_CHECK(t != NULL);
            if (t) {
                LT_CHECK(t->idents[0].lim_max == 400 && t->idents[0].lim_default == 350);
            }
            LT_CHECK_INT(ini->proto.table_blocking_factor, 256);
            lt_ini_free(ini);
        }
    }
}

LT_TEST(ms2extra_reference_tables)
{
    lt_ini *ini = load(MS2X, NULL, 0);
    const lt_ini_ref_table *t;
    if (!ini) return;
    LT_CHECK_INT(ini->n_ref_tables, 1);
    t = ini->ref_tables[0];
    LT_CHECK_STR(t->name, "mafTableBurner");
    LT_CHECK_INT(t->n_idents, 1);
    LT_CHECK_INT(t->idents[0].id, 3);
    LT_CHECK_INT(t->adc_count, 1024);
    LT_CHECK_INT(t->bytes_per_adc, 2);
    LT_CHECK_NEAR(t->scale, 1.0, 0);
    LT_CHECK(t->n_solutions >= 6);
    LT_CHECK_STR(t->solutions[0].expression, "table(adcValue, \"maffactor.inc\")");
    LT_CHECK_STR(t->solutions[t->n_solutions - 1].generator, "fileBrowseGenerator");
    LT_CHECK_INT(ini->proto.table_blocking_factor, 256);
    lt_ini_free(ini);
}

int main(void)
{
    LT_RUN(all_fixtures_parse);
    LT_RUN(demo_identity_and_protocol);
    LT_RUN(demo_counts);
    LT_RUN(demo_content);
    LT_RUN(demo_loggers);
    LT_RUN(demo_realtime_frame);
    LT_RUN(demo_constant_codec);
    LT_RUN(speeduino_reference_tables);
    LT_RUN(ms3_reference_tables);
    LT_RUN(ms2extra_reference_tables);
    LT_TEST_MAIN_END
}
