/*
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Unit tests for path utility functions:
 *   pmix_path_is_absolute, pmix_os_path, pmix_path_find,
 *   pmix_path_findv, pmix_path_df.
 *
 * Exit 0 if all tests pass, 1 otherwise.
 */

#include "src/include/pmix_config.h"
#include "src/include/pmix_globals.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

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
/* pmix_path_find / pmix_path_findv                                    */
/* ------------------------------------------------------------------ */

/* pathv belongs to the caller. Resolving a "$VAR" prefix used to mark
 * the end of the variable name by punching a temporary NUL into the
 * caller's string and putting it back, which faults outright on the
 * string literals an ordinary caller passes - and races any other
 * thread reading the same array. This case dies on a signal rather
 * than failing an assertion if that comes back. */
static void test_path_find_does_not_write_through_pathv(void)
{
    char *pathv[] = {(char *) "$PMIX_UTIL_PATH_TEST_UNSET/bin", NULL};
    char *r;

    unsetenv("PMIX_UTIL_PATH_TEST_UNSET");
    r = pmix_path_find((char *) "no-such-file-here", pathv, R_OK, NULL);
    report("path_find: undefined $VAR prefix finds nothing", NULL == r);
    report("path_find: leaves the caller's pathv intact",
           0 == strcmp(pathv[0], "$PMIX_UTIL_PATH_TEST_UNSET/bin"));
    free(r);
}

/* NULL used to reach pmix_path_is_absolute(), which dereferenced it */
static void test_path_null_arguments(void)
{
    report("is_absolute(NULL) is false", !pmix_path_is_absolute(NULL));
    report("path_access(NULL) is NULL", NULL == pmix_path_access(NULL, NULL, R_OK));
    report("find_absolute_path(NULL) is NULL", NULL == pmix_find_absolute_path(NULL));
}

/* The documented behavior of a wrkdir: it replaces every "." in the
 * search path, and when there was no "." it is appended, so a wrkdir is
 * always searched. Also pins that the search reads $PATH out of envv
 * without modifying it. */
static void test_path_findv_searches_wrkdir(void)
{
    char dir[] = "/tmp/pmix-util-path-XXXXXX";
    char *envv[] = {(char *) "PATH=/nonexistent-a:/nonexistent-b", NULL};
    char file[512];
    char *found;
    FILE *fp;

    if (NULL == mkdtemp(dir)) {
        report("path_findv: made a temp dir", 0);
        return;
    }
    snprintf(file, sizeof(file), "%s/pmix-util-path-probe", dir);
    fp = fopen(file, "w");
    if (NULL == fp) {
        report("path_findv: made a probe file", 0);
        rmdir(dir);
        return;
    }
    fclose(fp);

    found = pmix_path_findv((char *) "pmix-util-path-probe", R_OK, envv, dir);
    report("path_findv: wrkdir is searched even without a \".\" in PATH",
           NULL != found && 0 == strcmp(found, file));
    report("path_findv: leaves the caller's envv intact",
           0 == strcmp(envv[0], "PATH=/nonexistent-a:/nonexistent-b"));
    free(found);
    unlink(file);
    rmdir(dir);
}

/* ------------------------------------------------------------------ */
/* pmix_path_df                                                        */
/* ------------------------------------------------------------------ */

static void test_path_df(void)
{
    uint64_t avail = 12345;

    report("path_df: NULL path is an error", PMIX_SUCCESS != pmix_path_df(NULL, &avail));
    report("path_df: NULL out is an error", PMIX_SUCCESS != pmix_path_df("/", NULL));

    avail = 12345;
    report("path_df: a path that does not exist is an error",
           PMIX_SUCCESS != pmix_path_df("/no/such/path/at/all", &avail));
    report("path_df: out_avail is zeroed even on failure", 0 == avail);

    report("path_df: /tmp reports some free space",
           PMIX_SUCCESS == pmix_path_df("/tmp", &avail) && 0 < avail);
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

    test_path_null_arguments();
    test_path_find_does_not_write_through_pathv();
    test_path_findv_searches_wrkdir();
    test_path_df();
    test_os_path_single_component();

    fprintf(stdout, "\nResults: %d passed, %d failed\n\n", npass, nfail);
    return (nfail > 0) ? 1 : 0;
}
