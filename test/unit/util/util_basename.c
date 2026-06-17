/*
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Unit tests for pmix_basename() and pmix_dirname().
 *
 * Exit 0 if all tests pass, 1 otherwise.
 */

#include "src/include/pmix_config.h"
#include "src/include/pmix_globals.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/util/pmix_basename.h"

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
/* pmix_basename                                                       */
/* ------------------------------------------------------------------ */

static void test_basename_null(void)
{
    char *r = pmix_basename(NULL);
    report("basename(NULL) == NULL", NULL == r);
}

static void test_basename_empty(void)
{
    char *r = pmix_basename("");
    report("basename(\"\") == \"\"", r && 0 == strcmp(r, ""));
    free(r);
}

static void test_basename_root(void)
{
    char *r = pmix_basename("/");
    report("basename(\"/\") == \"/\"", r && 0 == strcmp(r, "/"));
    free(r);
}

static void test_basename_simple(void)
{
    char *r = pmix_basename("foo.txt");
    report("basename(\"foo.txt\") == \"foo.txt\"", r && 0 == strcmp(r, "foo.txt"));
    free(r);
}

static void test_basename_absolute(void)
{
    char *r = pmix_basename("/foo/bar/baz");
    report("basename(\"/foo/bar/baz\") == \"baz\"", r && 0 == strcmp(r, "baz"));
    free(r);
}

static void test_basename_single_component(void)
{
    char *r = pmix_basename("/yow.c");
    report("basename(\"/yow.c\") == \"yow.c\"", r && 0 == strcmp(r, "yow.c"));
    free(r);
}

static void test_basename_trailing_slash(void)
{
    char *r = pmix_basename("/foo/bar/");
    report("basename(\"/foo/bar/\") strips trailing slash", r && 0 == strcmp(r, "bar"));
    free(r);
}

static void test_basename_multi_trailing_slash(void)
{
    char *r = pmix_basename("/foo/bar//");
    report("basename(\"/foo/bar//\") strips multiple trailing slashes", r && 0 == strcmp(r, "bar"));
    free(r);
}

/* ------------------------------------------------------------------ */
/* pmix_dirname                                                        */
/* ------------------------------------------------------------------ */

static void test_dirname_absolute(void)
{
    char *r = pmix_dirname("/foo/bar/baz");
    report("dirname(\"/foo/bar/baz\") == \"/foo/bar\"", r && 0 == strcmp(r, "/foo/bar"));
    free(r);
}

static void test_dirname_single_component(void)
{
    char *r = pmix_dirname("/yow.c");
    report("dirname(\"/yow.c\") == \"/\"", r && 0 == strcmp(r, "/"));
    free(r);
}

static void test_dirname_local_file(void)
{
    char *r = pmix_dirname("foo.txt");
    report("dirname(\"foo.txt\") == \".\"", r && 0 == strcmp(r, "."));
    free(r);
}

static void test_dirname_deep_path(void)
{
    char *r = pmix_dirname("/a/b/c/d");
    report("dirname(\"/a/b/c/d\") == \"/a/b/c\"", r && 0 == strcmp(r, "/a/b/c"));
    free(r);
}

/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    PMIX_HIDE_UNUSED_PARAMS(argc, argv);

    fprintf(stdout, "\n=== pmix_basename / pmix_dirname unit tests ===\n\n");

    test_basename_null();
    test_basename_empty();
    test_basename_root();
    test_basename_simple();
    test_basename_absolute();
    test_basename_single_component();
    test_basename_trailing_slash();
    test_basename_multi_trailing_slash();

    test_dirname_absolute();
    test_dirname_single_component();
    test_dirname_local_file();
    test_dirname_deep_path();

    fprintf(stdout, "\nResults: %d passed, %d failed\n\n", npass, nfail);
    return (nfail > 0) ? 1 : 0;
}
