/* lt_test.h - tiny header-only test framework (C89).
 *
 *   #include "lt_test.h"
 *   LT_TEST(my_test) { LT_CHECK(1 + 1 == 2); }
 *   int main(void) { LT_RUN(my_test); LT_TEST_MAIN_END }
 *
 * Counts every check; LT_TEST_MAIN_END prints "N passed, M failed" and
 * makes main() return nonzero when anything failed.
 */
#ifndef LT_TEST_H
#define LT_TEST_H

#include <stdio.h>
#include <string.h>
#include <math.h>

static int lt_t_pass;
static int lt_t_fail;

#define LT_TEST(name) static void name(void)

#define LT_RUN(name) \
    do { \
        int lt_before_ = lt_t_fail; \
        name(); \
        printf("%s %s\n", lt_before_ == lt_t_fail ? "[ ok ]" : "[FAIL]", #name); \
    } while (0)

#define LT_CHECK(cond) \
    do { \
        if (cond) { \
            lt_t_pass++; \
        } else { \
            lt_t_fail++; \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        } \
    } while (0)

#define LT_CHECK_NEAR(a, b, eps) \
    do { \
        double lt_a_ = (double)(a), lt_b_ = (double)(b), lt_e_ = (double)(eps); \
        if (fabs(lt_a_ - lt_b_) <= lt_e_) { \
            lt_t_pass++; \
        } else { \
            lt_t_fail++; \
            fprintf(stderr, "%s:%d: CHECK_NEAR failed: %s=%.17g vs %s=%.17g (eps %g)\n", \
                    __FILE__, __LINE__, #a, lt_a_, #b, lt_b_, lt_e_); \
        } \
    } while (0)

#define LT_CHECK_STR(a, b) \
    do { \
        const char *lt_sa_ = (a); \
        const char *lt_sb_ = (b); \
        if (lt_sa_ && lt_sb_ && strcmp(lt_sa_, lt_sb_) == 0) { \
            lt_t_pass++; \
        } else { \
            lt_t_fail++; \
            fprintf(stderr, "%s:%d: CHECK_STR failed: got \"%s\" expected \"%s\"\n", \
                    __FILE__, __LINE__, lt_sa_ ? lt_sa_ : "(null)", lt_sb_ ? lt_sb_ : "(null)"); \
        } \
    } while (0)

#define LT_CHECK_INT(a, b) \
    do { \
        long lt_ia_ = (long)(a), lt_ib_ = (long)(b); \
        if (lt_ia_ == lt_ib_) { \
            lt_t_pass++; \
        } else { \
            lt_t_fail++; \
            fprintf(stderr, "%s:%d: CHECK_INT failed: %s=%ld vs %s=%ld\n", \
                    __FILE__, __LINE__, #a, lt_ia_, #b, lt_ib_); \
        } \
    } while (0)

#define LT_TEST_MAIN_END \
    printf("%d passed, %d failed\n", lt_t_pass, lt_t_fail); \
    return lt_t_fail != 0;

#endif /* LT_TEST_H */
