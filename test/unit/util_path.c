/*
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Unit tests for path utility functions:
 *   pmix_path_is_absolute, pmix_os_path.
 *
 * Exit 0 if all tests pass, 1 otherwise.
 */

#include "src/include/pmix_config.h"
#include "src/include/pmix_globals.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/util/pmix_path.h"
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

/* ------------------------------------------------------------------ */
/* pmix_path_is_absolute                                               */
/* ------------------------------------------------------------------ */

static void test_is_absolute_root(void)
{
    report("is_absolute(\"/\")", pmix_path_is_absolute("/"));
}

static void test_is_absolute_deep(void)
{
    report("is_absolute(\"/foo/bar/baz\")", pmix_path_is_absolute("/foo/bar/baz"));
}

static void test_is_absolute_relative(void)
{
    report("!is_absolute(\"foo\")", !pmix_path_is_absolute("foo"));
}

static void test_is_absolute_dot(void)
{
    report("!is_absolute(\".\")", !pmix_path_is_absolute("."));
}

static void test_is_absolute_dot_slash(void)
{
    report("!is_absolute(\"./foo\")", !pmix_path_is_absolute("./foo"));
}

static void test_is_absolute_dotdot(void)
{
    report("!is_absolute(\"../bar\")", !pmix_path_is_absolute("../bar"));
}

/* ------------------------------------------------------------------ */
/* pmix_os_path                                                        */
/* ------------------------------------------------------------------ */

static void test_os_path_absolute_two_components(void)
{
    char *r = pmix_os_path(0, "foo", "bar", NULL);
    /* On POSIX: "/foo/bar" */
    report("os_path(abs, foo, bar) starts with /", r && '/' == r[0]);
    report("os_path(abs, foo, bar) ends with /bar", r && NULL != strstr(r, "/bar"));
    free(r);
}

static void test_os_path_absolute_three_components(void)
{
    char *r = pmix_os_path(0, "a", "b", "c", NULL);
    report("os_path(abs, a, b, c) starts with /", r && '/' == r[0]);
    report("os_path(abs, a, b, c) contains /b/", r && NULL != strstr(r, "/b/"));
    free(r);
}

static void test_os_path_relative_two_components(void)
{
    char *r = pmix_os_path(1, "foo", "bar", NULL);
    /* On POSIX: "./foo/bar" */
    report("os_path(rel, foo, bar) starts with .", r && '.' == r[0]);
    report("os_path(rel, foo, bar) contains foo", r && NULL != strstr(r, "foo"));
    free(r);
}

static void test_os_path_no_components_absolute(void)
{
    char *r = pmix_os_path(0, NULL);
    /* On POSIX: "/" */
    report("os_path(abs, no components) == \"/\"", r && 0 == strcmp(r, "/"));
    free(r);
}

static void test_os_path_no_components_relative(void)
{
    char *r = pmix_os_path(1, NULL);
    /* On POSIX: "./" */
    report("os_path(rel, no components) == \"./\"", r && 0 == strcmp(r, "./"));
    free(r);
}

static void test_os_path_single_component(void)
{
    char *r = pmix_os_path(0, "single", NULL);
    report("os_path(abs, single) contains single", r && NULL != strstr(r, "single"));
    free(r);
}

/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    PMIX_HIDE_UNUSED_PARAMS(argc, argv);

    fprintf(stdout, "\n=== pmix_path_is_absolute / pmix_os_path unit tests ===\n\n");

    test_is_absolute_root();
    test_is_absolute_deep();
    test_is_absolute_relative();
    test_is_absolute_dot();
    test_is_absolute_dot_slash();
    test_is_absolute_dotdot();

    test_os_path_absolute_two_components();
    test_os_path_absolute_three_components();
    test_os_path_relative_two_components();
    test_os_path_no_components_absolute();
    test_os_path_no_components_relative();
    test_os_path_single_component();

    fprintf(stdout, "\nResults: %d passed, %d failed\n\n", npass, nfail);
    return (nfail > 0) ? 1 : 0;
}
