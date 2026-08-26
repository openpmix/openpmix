/*
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Unit tests for pmix_printf portability functions:
 *   pmix_snprintf, pmix_vsnprintf, pmix_asprintf, pmix_vasprintf.
 *
 * pmix_vsnprintf and pmix_vasprintf are exercised indirectly through
 * pmix_snprintf and pmix_asprintf respectively.
 *
 * Exit 0 if all tests pass, 1 otherwise.
 */

#include "src/include/pmix_config.h"
#include "src/include/pmix_globals.h"

#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/util/pmix_printf.h"

static int npass = 0;
static int nfail = 0;

static void report(const char *name, int passed)
{
    if (passed) {
        fprintf(stdout, "  PASS: %s\n", name);
        npass++;
    } else {
        fprintf(stdout, "  FAIL: %s\n", name);
        nfail++;
    }
}

/* Thin wrapper so we can exercise pmix_vsnprintf with a va_list. */
static int wrap_vsnprintf(char *buf, size_t size, const char *fmt, ...)
{
    int n;
    va_list ap;
    va_start(ap, fmt);
    n = pmix_vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return n;
}

/* ------------------------------------------------------------------ */
/* pmix_snprintf                                                       */
/* ------------------------------------------------------------------ */

static void test_snprintf_basic(void)
{
    char buf[64];
    int n = pmix_snprintf(buf, sizeof(buf), "hello %s %d", "world", 42);
    report("snprintf_basic: content", 0 == strcmp(buf, "hello world 42"));
    report("snprintf_basic: return value equals strlen",
           n == (int) strlen("hello world 42"));
}

static void test_snprintf_truncation(void)
{
    char buf[5];
    int n = pmix_snprintf(buf, sizeof(buf), "hello world");
    /* buffer holds 4 chars + NUL */
    report("snprintf_truncate: null terminated", '\0' == buf[4]);
    report("snprintf_truncate: prefix correct", 0 == strncmp(buf, "hell", 4));
    /* C99: return value is the full would-be length */
    report("snprintf_truncate: returns full length",
           n == (int) strlen("hello world"));
}

static void test_snprintf_null_buffer(void)
{
    /* NULL buf with size 0: return the would-be length without writing */
    int n = pmix_snprintf(NULL, 0, "test %d", 99);
    report("snprintf_null_buf: returns would-be length",
           n == (int) strlen("test 99"));
}

static void test_snprintf_multiple_conversions(void)
{
    char buf[128];
    pmix_snprintf(buf, sizeof(buf), "%s=%d", "rank", 3);
    report("snprintf_multiple: content", 0 == strcmp(buf, "rank=3"));
}

/* ------------------------------------------------------------------ */
/* pmix_vsnprintf (via wrap_vsnprintf)                                 */
/* ------------------------------------------------------------------ */

static void test_vsnprintf_basic(void)
{
    char buf[32];
    int n = wrap_vsnprintf(buf, sizeof(buf), "v=%d", 7);
    report("vsnprintf_basic: content", 0 == strcmp(buf, "v=7"));
    report("vsnprintf_basic: return value", n == (int) strlen("v=7"));
}

/* ------------------------------------------------------------------ */
/* pmix_asprintf                                                       */
/* ------------------------------------------------------------------ */

static void test_asprintf_basic(void)
{
    char *s = NULL;
    int n = pmix_asprintf(&s, "value=%d", 99);
    report("asprintf_basic: ptr non-NULL", NULL != s);
    report("asprintf_basic: content", NULL != s && 0 == strcmp(s, "value=99"));
    report("asprintf_basic: return length equals strlen",
           n == (int) strlen("value=99"));
    free(s);
}

static void test_asprintf_no_conversions(void)
{
    char *s = NULL;
    int n = pmix_asprintf(&s, "plain");
    report("asprintf_no_conversions: ptr non-NULL", NULL != s);
    report("asprintf_no_conversions: content", NULL != s && 0 == strcmp(s, "plain"));
    report("asprintf_no_conversions: return length", n == (int) strlen("plain"));
    free(s);
}

static void test_asprintf_multiple_args(void)
{
    char *s = NULL;
    pmix_asprintf(&s, "%s/%s", "a", "b");
    report("asprintf_multiple_args: content",
           NULL != s && 0 == strcmp(s, "a/b"));
    free(s);
}

static void test_asprintf_sets_ptr_on_success(void)
{
    char *s = (char *) 0xdeadbeef; /* poison */
    pmix_asprintf(&s, "ok");
    report("asprintf_sets_ptr: pointer updated on success",
           NULL != s && 0 == strcmp(s, "ok"));
    if (NULL != s && (void *) s != (void *) 0xdeadbeef) {
        free(s);
    }
}

/* ------------------------------------------------------------------ */
/* Conversion coverage                                                  */
/*                                                                      */
/* On a platform that has asprintf(3)/vsnprintf(3) these simply confirm */
/* that the wrappers pass everything through untouched.  On a platform  */
/* that has neither, pmix_vasprintf() has to size the result itself by  */
/* walking the format, and these are the cases that walk has to get     */
/* right: an argument it fails to recognize is an argument it fails to  */
/* consume, which makes every conversion after it read the wrong slot,  */
/* and a width or precision it fails to account for undersizes the      */
/* buffer that vsprintf() then writes into unbounded.                   */
/* ------------------------------------------------------------------ */

/* Compare pmix_asprintf against the platform's own rendering of the
 * same format, and confirm the documented "returns the length" contract
 * holds for both. */
#define CHECK_FMT(name, ...)                                                  \
    do {                                                                      \
        char expect[8192];                                                    \
        char *got = NULL;                                                     \
        int elen = snprintf(expect, sizeof(expect), __VA_ARGS__);             \
        int glen = pmix_asprintf(&got, __VA_ARGS__);                          \
        report(name ": content", NULL != got && 0 == strcmp(got, expect));    \
        report(name ": length is strlen", NULL != got                         \
                                              && glen == (int) strlen(got));  \
        report(name ": length matches platform", glen == elen);               \
        free(got);                                                            \
    } while (0)

static void test_conversions(void)
{
    /* a literal percent takes no argument: a walk that mistakes the
     * second '%' for the start of a conversion consumes one that was
     * never passed */
    CHECK_FMT("conv_percent", "100%% of %s", "it");

    /* the integer family, across every length modifier */
    CHECK_FMT("conv_int", "%d|%i|%u|%o|%x|%X", -1, 2, 3u, 8u, 255u, 255u);
    CHECK_FMT("conv_short", "%hd|%hhd|%hu", (short) -1, (signed char) -2,
              (unsigned short) 3);
    CHECK_FMT("conv_long", "%ld|%lu|%lx", -1L, 2UL, 255UL);
    CHECK_FMT("conv_longlong", "%lld|%llu", -1LL, 2ULL);
    CHECK_FMT("conv_intmax", "%jd|%ju", (intmax_t) -1, (uintmax_t) 2);
    CHECK_FMT("conv_size", "%zu|%zd", (size_t) 4096, (size_t) 4096);
    CHECK_FMT("conv_ptrdiff", "%td", (ptrdiff_t) -3);

    /* the floating family; %f of a large double is 300-odd digits, and
     * a precision widens it further */
    CHECK_FMT("conv_double", "%f|%e|%g", 1.5, 1.5, 1.5);
    CHECK_FMT("conv_double_big", "%f", 1.0e300);
    CHECK_FMT("conv_double_prec", "%.200f", 1.0e300);
    CHECK_FMT("conv_longdouble", "%Lf", (long double) 1.5);

    /* infinity and NaN render short, but a size estimate that divides
     * the value down to zero never terminates on either */
    CHECK_FMT("conv_inf", "%f|%f", (double) INFINITY, (double) -INFINITY);
    CHECK_FMT("conv_nan", "%f", (double) NAN);

    /* characters, strings and pointers */
    CHECK_FMT("conv_char", "%c%c", 'a', 'b');
    CHECK_FMT("conv_string", "%s|%.3s", "hello", "hello");
    CHECK_FMT("conv_pointer", "%p", (void *) &npass);

    /* flags, field widths and precisions - none of which are arguments,
     * but all of which change how much room the result needs */
    CHECK_FMT("conv_width", "%200d", 7);
    CHECK_FMT("conv_flags", "%-12.4x|%+ld|%08.3f", 255, 9L, 1.5);

    /* a '*' width or precision IS an argument, and an extra one */
    CHECK_FMT("conv_star_width", "%*d|%s", 60, 7, "tail");
    CHECK_FMT("conv_star_prec", "%.*f|%s", 12, 1.5, "tail");

    /* a mixture, in the order PMIx actually writes them */
    CHECK_FMT("conv_mixed", "[%s:%u] %zu bytes at %p (%d%%)", "nspace", 3u,
              (size_t) 64, (void *) &nfail, 50);
}

/* ------------------------------------------------------------------ */
/* Failure and edge behavior                                            */
/* ------------------------------------------------------------------ */

static void test_snprintf_zero_size(void)
{
    char buf[8];
    int n;

    memset(buf, 'Z', sizeof(buf));
    /* a non-NULL buffer with a zero size: nothing may be written, but
     * the would-be length is still reported */
    n = pmix_snprintf(buf, 0, "test %d", 99);
    report("snprintf_zero_size: returns would-be length",
           n == (int) strlen("test 99"));
    report("snprintf_zero_size: buffer untouched", 'Z' == buf[0]);
}

static void test_snprintf_size_one(void)
{
    char buf[8];
    int n;

    memset(buf, 'Z', sizeof(buf));
    n = pmix_snprintf(buf, 1, "test %d", 99);
    report("snprintf_size_one: returns would-be length",
           n == (int) strlen("test 99"));
    report("snprintf_size_one: only the terminator is written",
           '\0' == buf[0] && 'Z' == buf[1]);
}

static void test_vasprintf_null_string_arg(void)
{
    /* volatile so the compiler cannot see the NULL and reject the call
     * outright under -Wformat-overflow */
    static char *volatile nullstr = NULL;
    char *s = NULL;
    int n;

    /* a NULL %s argument is a caller bug, but it must not be a crash:
     * every implementation we build against renders it as some short
     * placeholder, and the length still has to describe what was
     * produced */
    n = pmix_asprintf(&s, "a%sb", nullstr);
    report("asprintf_null_arg: ptr non-NULL", NULL != s);
    report("asprintf_null_arg: length is strlen",
           NULL != s && n == (int) strlen(s));
    free(s);
}

/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    PMIX_HIDE_UNUSED_PARAMS(argc, argv);

    fprintf(stdout, "\n=== pmix_printf unit tests ===\n\n");

    test_snprintf_basic();
    test_snprintf_truncation();
    test_snprintf_null_buffer();
    test_snprintf_multiple_conversions();

    test_vsnprintf_basic();

    test_asprintf_basic();
    test_asprintf_no_conversions();
    test_asprintf_multiple_args();
    test_asprintf_sets_ptr_on_success();

    test_conversions();
    test_snprintf_zero_size();
    test_snprintf_size_one();
    test_vasprintf_null_string_arg();

    fprintf(stdout, "\nResults: %d passed, %d failed\n\n", npass, nfail);
    return (nfail > 0) ? 1 : 0;
}
