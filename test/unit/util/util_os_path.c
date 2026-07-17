/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Unit tests for src/util/pmix_os_path.c (pmix_os_path).
 *
 * Pure string assembly - no server/tool init required. Assumes a POSIX
 * '/' path separator (PMIX_PATH_SEP), which holds on every platform the
 * test suite runs on.
 *
 * Exit 0 if all tests pass, 1 otherwise.
 */

#include "src/include/pmix_config.h"
#include "src/include/pmix_globals.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/util/pmix_os_path.h"

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

static void check(const char *label, char *got, const char *expected)
{
    report(label, NULL != got && 0 == strcmp(got, expected));
    if (NULL != got) {
        free(got);
    }
}

int main(int argc, char **argv)
{
    PMIX_HIDE_UNUSED_PARAMS(argc, argv);

    fprintf(stdout, "\n=== pmix_os_path unit tests ===\n\n");

    check("absolute multi-element", pmix_os_path(false, "a", "b", "c", NULL), "/a/b/c");
    check("relative multi-element", pmix_os_path(true, "a", "b", NULL), "./a/b");
    check("absolute, no elements", pmix_os_path(false, NULL), "/");
    check("relative, no elements", pmix_os_path(true, NULL), "./");

    /* an element that already begins with the separator must not produce a
     * doubled slash */
    check("leading-separator element", pmix_os_path(false, "/a", "b", NULL), "/a/b");
    check("single element", pmix_os_path(false, "usr", NULL), "/usr");

    fprintf(stdout, "\nResults: %d passed, %d failed\n\n", npass, nfail);
    return (nfail > 0) ? 1 : 0;
}
