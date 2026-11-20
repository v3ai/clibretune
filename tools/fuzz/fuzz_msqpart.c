/* fuzz_msqpart.c - partial tunes (.msqpart): parse, list, apply to the pages of the demo INI
 * (with undo), apply the undo, extract / diff, and the pcVariableValues merge. */
#include "fuzz_common.h"
#include "tune/tune.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    char *s = fz_cstr(data, size);
    lt_tune *part = lt_tune_parse_msq(s, size);
    if (part) {
        const lt_ini *ini = fz_demo_ini();
        fz_pages pg;
        lt_pagesrc src;
        lt_tune *pcv = lt_tune_new("x"), *undo = NULL, *ex, *cap;
        char **names = NULL, **dn = NULL;
        size_t n, i, ap = 0, sk = 0, nd;
        fz_pages_init(&pg, ini, 0x11);
        src = fz_pagesrc(&pg);
        n = lt_tune_part_names(part, &names);
        lt_tune_part_apply(ini, &src, pcv, part, &undo, &ap, &sk);
        if (undo) {
            lt_tune_part_apply(ini, &src, pcv, undo, NULL, NULL, NULL);
            lt_tune_free(undo);
        }
        ex = lt_tune_part_extract(part, (const char *const *)names, n);
        cap = lt_tune_part_capture(ini, &src, pcv, (const char *const *)names, n);
        nd = lt_tune_part_diff_names(part, cap ? cap : ex, &dn);
        for (i = 0; i < nd; i++) lt_free(dn[i]);
        lt_free(dn);
        if (ex) {
            lt_buf b;
            lt_buf_init(&b);
            lt_tune_to_msq(ex, &b);
            lt_buf_free(&b);
            lt_tune_free(ex);
        }
        if (cap) lt_tune_free(cap);
        for (i = 0; i < n; i++) lt_free(names[i]);
        lt_free(names);
        fz_pages_free(&pg);
        lt_tune_free(pcv);
        lt_tune_free(part);
    }
    free(s);
    return 0;
}
