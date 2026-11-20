/* fuzz_msq.c - .msq tune files: parse, sanitize, re-serialize, apply to the pages of the demo
 * INI and capture back (what opening a tune in a project does). The first input byte picks
 * a sibling format sharing the XML layer: .table, .tuneView, tune CSV import. */
#include "fuzz_common.h"
#include "tune/tune.h"
#include "tune/tune_csv.h"

static void do_msq(const char *s, size_t n)
{
    lt_tune *t = lt_tune_parse_msq(s, n);
    if (t) {
        lt_buf b;
        lt_tune *c;
        lt_tune_sanitize(t);
        lt_buf_init(&b);
        lt_tune_to_msq(t, &b);
        lt_buf_free(&b);
        c = lt_tune_clone(t);
        lt_tune_apply_to_pages(c, fz_small_ini());
        lt_tune_capture_from_pages(c, fz_small_ini());
        {
            lt_tune_diff *d = lt_tune_diff_compare(t, c);
            if (d) lt_tune_diff_free(d);
        }
        lt_tune_free(c);
        lt_tune_free(t);
    }
}

static void do_table(const char *s, size_t n)
{
    lt_table_file t;
    memset(&t, 0, sizeof t);
    if (lt_table_file_parse(s, n, &t) == 0) lt_table_file_clear(&t);
}

static void do_view(const char *s, size_t n)
{
    lt_tune_view v;
    memset(&v, 0, sizeof v);
    if (lt_tune_view_parse(s, n, &v) == 0) {
        char *w = lt_tune_view_write(&v);
        lt_free(w);
        lt_tune_view_clear(&v);
    }
}

static void do_csv(const char *s, size_t n)
{
    fz_pages pg;
    lt_pagesrc src;
    lt_csv_import r;
    memset(&r, 0, sizeof r);
    fz_pages_init(&pg, fz_small_ini(), 0);
    src = fz_pagesrc(&pg);
    lt_tune_csv_import(fz_small_ini(), &src, s, n, &r);
    lt_csv_import_free(&r);
    fz_pages_free(&pg);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    char *s;
    int mode;
    if (size < 1) return 0;
    mode = data[0] & 3;
    s = fz_cstr(data + 1, size - 1);
    switch (mode) {
    case 0: do_msq(s, size - 1); break;
    case 1: do_table(s, size - 1); break;
    case 2: do_view(s, size - 1); break;
    default: do_csv(s, size - 1); break;
    }
    free(s);
    return 0;
}
