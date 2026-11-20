/* fuzz_csv.c - CSV logs: parse, then what the log viewer does with the result. */
#include "fuzz_log.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    char *s = fz_cstr(data, size);
    lt_log l;
    lt_log_init(&l);
    (void)s;
    if (lt_csv_parse(s, size, &l) == 0) fz_log_use(&l);
    lt_log_free(&l);
    free(s);
    return 0;
}
