/* fuzz_mlg.c - MLG v1/v2 binary logs: parse, then what the log viewer does with the result. */
#include "fuzz_log.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    char *s = fz_cstr(data, size);
    lt_log l;
    lt_log_init(&l);
    (void)s;
    if (lt_mlg_read_buf(data, size, &l) == 0) fz_log_use(&l);
    lt_log_free(&l);
    free(s);
    return 0;
}
