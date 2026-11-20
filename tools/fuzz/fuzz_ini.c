/* fuzz_ini.c - INI definition parser: load, then touch what the app touches after loading
 * (constants through pages, realtime decode, dynamic scales, defaults tune, loggers). */
#include "fuzz_common.h"
#include "ini/expr.h"
#include "tune/tune.h"

static void exercise(lt_ini *ini)
{
    size_t i, k;
    fz_pages pg;
    lt_pagesrc src;
    lt_u8 frame[512];
    lt_ini_rt *rt;
    lt_varenv ve;
    double d, arr[64];
    char s[64];
    lt_tune *t;

    fz_pages_init(&pg, ini, 0x5a);
    src = fz_pagesrc(&pg);
    for (i = 0; i < sizeof frame; i++) frame[i] = (lt_u8)(i * 37u + 11u);
    for (i = 0; i < ini->n_constants && i < 2000; i++) {
        const lt_ini_const *c = ini->constants[i];
        lt_ini_const_get(ini, c, &src, 0, &d);
        lt_ini_const_get_raw(ini, c, &src, 3, &d);
        lt_ini_const_get_array(ini, c, &src, arr, 64);
        lt_ini_const_get_string(ini, c, &src, s, sizeof s);
        lt_ini_const_set(ini, c, &src, 0, 1.5);
        (void)lt_ini_bit_option(c, 1);
        (void)lt_const_in_range(c, 2.0);
    }
    for (i = 0; i < ini->n_channels && i < 4000; i++) {
        lt_ini_channel_decode(ini->channels[i], ini->endian, frame, sizeof frame, &d);
    }
    for (i = 0; i < ini->n_loggers; i++) {
        double vals[32];
        int present[32];
        if (ini->loggers[i].n_fields <= 32) {
            lt_ini_logger_decode(&ini->loggers[i], frame, sizeof frame, vals, present);
        }
        (void)lt_ini_logger_record_count(&ini->loggers[i], 400);
        (void)lt_ini_logger_is_empty(&ini->loggers[i], frame, 9);
    }
    for (i = 0; i < ini->n_gauges; i++) {
        (void)lt_ini_gauge_is_danger(ini->gauges[i], 50.0);
        (void)lt_ini_gauge_is_warning(ini->gauges[i], 50.0);
    }
    rt = lt_ini_rt_new(ini);
    lt_varenv_init(&ve, ini, &src, rt, NULL);
    if (rt) {
        lt_ini_rt_set_parent(rt, &ve.consts);
        lt_ini_rt_update(rt, frame, sizeof frame);
        lt_ini_rt_update(rt, frame, 7);
    }
    lt_ini_resolve_dynamic(ini, &ve.env);
    lt_ini_infer_table_roles(ini);
    for (i = 0; i < ini->n_dialogs && i < 500; i++) (void)lt_ini_find_help(ini, ini->dialogs[i]->name);
    for (i = 0; i < ini->n_menus; i++) {
        for (k = 0; k < ini->menus[i].n_items && k < 200; k++) {
            const char *tg = ini->menus[i].items[k]->target;
            if (tg) {
                (void)lt_ini_find_dialog(ini, tg);
                (void)lt_ini_find_table(ini, tg);
                (void)lt_ini_find_curve(ini, tg);
            }
        }
    }
    t = lt_tune_from_defaults(ini);
    if (t) {
        lt_buf b;
        lt_buf_init(&b);
        lt_tune_to_msq(t, &b);
        lt_buf_free(&b);
        lt_tune_free(t);
    }
    if (rt) lt_ini_rt_free(rt);
    fz_pages_free(&pg);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    size_t tl = 0;
    char *text = lt_ini_decode(data, size, &tl);
    lt_ini *ini = lt_ini_load_string(text, tl, "build/fuzz/no-such-dir", NULL);
    if (ini) {
        exercise(ini);
        lt_ini_free(ini);
    }
    lt_free(text);
    return 0;
}
