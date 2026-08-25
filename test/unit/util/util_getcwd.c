/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Unit tests for pmix_getcwd().
 *
 * Two properties here have been broken before and are what this test
 * exists to pin down:
 *
 *  - the buffer-length test is ">=", not ">": a buffer exactly as long
 *    as the path leaves no room for the terminating NUL, and must be
 *    reported as a truncation rather than as success;
 *  - $PWD is *not* authoritative. It is used only when it is textually
 *    identical to getcwd(), which makes getcwd() the answer in every
 *    case. An implementation that trusted a stale or symlink-preserving
 *    $PWD would pass a naive test but hand callers a directory the
 *    process is not actually in.
 *
 * Exit 0 if all tests pass, 1 otherwise.
 */

#include "src/include/pmix_config.h"
#include "src/include/pmix_globals.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif

#include "src/util/pmix_basename.h"
#include "src/util/pmix_getcwd.h"

static int npass = 0;
static int nfail = 0;

/* The authoritative answer, captured once at startup. */
static char truth[PMIX_PATH_MAX];

/* A canary byte follows every destination buffer so that a copy which
 * runs one past the caller's length is caught rather than tolerated. */
#define CANARY 0x5a

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

/* ------------------------------------------------------------------ */
/* Bozo checks                                                         */
/* ------------------------------------------------------------------ */

static void test_null_buffer(void)
{
    int rc = pmix_getcwd(NULL, PMIX_PATH_MAX);
    report("getcwd(NULL, n) == PMIX_ERR_BAD_PARAM", PMIX_ERR_BAD_PARAM == rc);
}

static void test_absurd_size(void)
{
    char buf[PMIX_PATH_MAX];
    int rc;

    if (SIZE_MAX <= (size_t) INT_MAX) {
        /* size_t cannot express a value larger than INT_MAX here, so
         * there is nothing to reject */
        report("getcwd(buf, >INT_MAX) == PMIX_ERR_BAD_PARAM (n/a)", 1);
        return;
    }
    rc = pmix_getcwd(buf, (size_t) INT_MAX + 1);
    report("getcwd(buf, >INT_MAX) == PMIX_ERR_BAD_PARAM", PMIX_ERR_BAD_PARAM == rc);
}

/* ------------------------------------------------------------------ */
/* The happy path                                                      */
/* ------------------------------------------------------------------ */

static void test_ample_buffer(void)
{
    char buf[PMIX_PATH_MAX];
    int rc;

    memset(buf, CANARY, sizeof(buf));
    rc = pmix_getcwd(buf, sizeof(buf));
    report("getcwd(buf, PMIX_PATH_MAX) == PMIX_SUCCESS", PMIX_SUCCESS == rc);
    report("getcwd fills buf with the real cwd", 0 == strcmp(buf, truth));
}

static void test_exact_fit(void)
{
    size_t len = strlen(truth);
    char *buf = (char *) malloc(len + 2);
    int rc;

    if (NULL == buf) {
        report("getcwd exact-fit buffer succeeds (alloc failed)", 0);
        return;
    }
    memset(buf, CANARY, len + 2);
    /* len + 1 is precisely enough: the path plus its NUL */
    rc = pmix_getcwd(buf, len + 1);
    report("getcwd(buf, strlen+1) == PMIX_SUCCESS", PMIX_SUCCESS == rc);
    report("getcwd(buf, strlen+1) copies the whole path", 0 == strcmp(buf, truth));
    report("getcwd(buf, strlen+1) respects the buffer end", CANARY == buf[len + 1]);
    free(buf);
}

static void test_one_byte_short(void)
{
    size_t len = strlen(truth);
    char *buf = (char *) malloc(len + 1);
    char *base;
    int rc;

    if (NULL == buf) {
        report("getcwd one-byte-short buffer truncates (alloc failed)", 0);
        return;
    }
    memset(buf, CANARY, len + 1);
    /* len leaves no room for the NUL, so this is a truncation */
    rc = pmix_getcwd(buf, len);
    report("getcwd(buf, strlen) == PMIX_ERR_OUT_OF_RESOURCE", PMIX_ERR_OUT_OF_RESOURCE == rc);
    report("getcwd(buf, strlen) respects the buffer end", CANARY == buf[len]);

    /* what it hands back instead is (as much as fits of) the basename */
    base = pmix_basename(truth);
    if (NULL != base) {
        size_t keep = strlen(base);
        if (keep > len - 1) {
            keep = len - 1;
        }
        report("getcwd truncation yields the basename",
               0 == strncmp(buf, base, keep) && '\0' == buf[keep]);
        free(base);
    } else {
        report("getcwd truncation yields the basename (basename failed)", 0);
    }
    free(buf);
}

static void test_tiny_buffer(void)
{
    char buf[2];
    int rc;

    memset(buf, CANARY, sizeof(buf));
    rc = pmix_getcwd(buf, 1);
    report("getcwd(buf, 1) == PMIX_ERR_OUT_OF_RESOURCE", PMIX_ERR_OUT_OF_RESOURCE == rc);
    report("getcwd(buf, 1) writes only the NUL", '\0' == buf[0] && CANARY == buf[1]);
}

static void test_zero_size(void)
{
    char buf[1];
    int rc;

    memset(buf, CANARY, sizeof(buf));
    rc = pmix_getcwd(buf, 0);
    report("getcwd(buf, 0) == PMIX_ERR_OUT_OF_RESOURCE", PMIX_ERR_OUT_OF_RESOURCE == rc);
    report("getcwd(buf, 0) writes nothing", CANARY == buf[0]);
}

/* ------------------------------------------------------------------ */
/* $PWD is never authoritative                                         */
/* ------------------------------------------------------------------ */

static void test_pwd_unset(void)
{
    char buf[PMIX_PATH_MAX];
    int rc;

    unsetenv("PWD");
    rc = pmix_getcwd(buf, sizeof(buf));
    report("getcwd with no $PWD == PMIX_SUCCESS", PMIX_SUCCESS == rc);
    report("getcwd with no $PWD answers getcwd()", 0 == strcmp(buf, truth));
}

static void test_pwd_bogus(void)
{
    char buf[PMIX_PATH_MAX];
    int rc;

    setenv("PWD", "/no/such/directory/anywhere", 1);
    rc = pmix_getcwd(buf, sizeof(buf));
    report("getcwd with a stale $PWD == PMIX_SUCCESS", PMIX_SUCCESS == rc);
    report("getcwd ignores a stale $PWD", 0 == strcmp(buf, truth));
    unsetenv("PWD");
}

static void test_pwd_matching(void)
{
    char buf[PMIX_PATH_MAX];
    int rc;

    setenv("PWD", truth, 1);
    rc = pmix_getcwd(buf, sizeof(buf));
    report("getcwd with a matching $PWD == PMIX_SUCCESS", PMIX_SUCCESS == rc);
    report("getcwd with a matching $PWD answers getcwd()", 0 == strcmp(buf, truth));
    unsetenv("PWD");
}

int main(int argc, char **argv)
{
    (void) argc;
    (void) argv;

    if (NULL == getcwd(truth, sizeof(truth))) {
        fprintf(stderr, "cannot determine the working directory - skipping\n");
        return 77;
    }

    fprintf(stdout, "\n=== pmix_getcwd ===\n");
    test_null_buffer();
    test_absurd_size();
    test_ample_buffer();
    test_exact_fit();
    test_one_byte_short();
    test_tiny_buffer();
    test_zero_size();
    test_pwd_unset();
    test_pwd_bogus();
    test_pwd_matching();

    fprintf(stdout, "\nResults: %d passed, %d failed\n\n", npass, nfail);
    return (nfail > 0) ? 1 : 0;
}
