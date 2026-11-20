/* fuzz_expr.c - INI expression language: compile and evaluate (visibility conditions,
 * computed channels, dynamic scales all go through it). */
#include "fuzz_common.h"
#include "ini/expr.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    char err[160];
    char *s = fz_cstr(data, size);
    lt_expr *x = lt_expr_compile(s, err, sizeof err);
    if (x) {
        lt_value v;
        double d;
        lt_bool b;
        if (lt_expr_eval(x, NULL, &v) == 0) lt_value_clear(&v);
        (void)lt_expr_eval_f64(x, NULL, &d);
        (void)lt_expr_eval_bool(x, NULL, &b);
        lt_expr_free(x);
    }
    free(s);
    return 0;
}
