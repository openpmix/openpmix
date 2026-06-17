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

#include <stdarg.h>
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

    fprintf(stdout, "\nResults: %d passed, %d failed\n\n", npass, nfail);
    return (nfail > 0) ? 1 : 0;
}
