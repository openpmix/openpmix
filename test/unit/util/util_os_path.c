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

/* The length test is applied to an estimate, and the estimate used to
 * count the separator ahead of each element twice - once while measuring
 * and again in the num_elements term - so a name that fits was refused.
 * Three elements totalling 1021 characters assemble to exactly
 * PMIX_PATH_MAX - 1 (1024) plus the terminator, which is the largest
 * name this function will build; the old estimate came to 1028 and
 * answered NULL. One character more really is too long.
 *
 * The two cases share a builder so that the only difference between the
 * accepted and the rejected one is that single character. */
static char *assemble_of_total(size_t total)
{
    size_t first = total / 3, second = total / 3;
    size_t third = total - first - second;
    char *a, *b, *c, *got;

    a = malloc(first + 1);
    b = malloc(second + 1);
    c = malloc(third + 1);
    if (NULL == a || NULL == b || NULL == c) {
        free(a);
        free(b);
        free(c);
        return NULL;
    }
    memset(a, 'a', first);
    a[first] = '\0';
    memset(b, 'b', second);
    b[second] = '\0';
    memset(c, 'c', third);
    c[third] = '\0';

    got = pmix_os_path(false, a, b, c, NULL);
    free(a);
    free(b);
    free(c);
    return got;
}

static void test_length_boundary(void)
{
    char *got;

    got = assemble_of_total(PMIX_PATH_MAX - 4);
    report("longest name that fits is assembled",
           NULL != got && (PMIX_PATH_MAX - 1) == (int) strlen(got));
    free(got);

    got = assemble_of_total(PMIX_PATH_MAX - 3);
    report("one character longer is refused", NULL == got);
    free(got);
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

    /* an empty element contributes nothing but still gets its separator */
    check("empty element", pmix_os_path(false, "a", "", "b", NULL), "/a//b");

    test_length_boundary();

    fprintf(stdout, "\nResults: %d passed, %d failed\n\n", npass, nfail);
    return (nfail > 0) ? 1 : 0;
}
