/*
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Unit tests for pmix_environ utility functions:
 *   pmix_getenv, pmix_unsetenv, pmix_environ_merge,
 *   pmix_environ_merge_inplace, pmix_tmp_directory.
 *
 * Exit 0 if all tests pass, 1 otherwise.
 */

#include "src/include/pmix_config.h"
#include "src/include/pmix_globals.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pmix.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_environ.h"

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

/* Build a mutable NULL-terminated env array from a list of "KEY=VALUE" strings. */
static char **make_env(const char **vars)
{
    int argc = 0;
    char **env = NULL;
    int i;
    for (i = 0; NULL != vars[i]; i++) {
        pmix_argv_append(&argc, &env, vars[i]);
    }
    return env;
}

/* ------------------------------------------------------------------ */
/* pmix_getenv                                                         */
/* ------------------------------------------------------------------ */

static void test_getenv_found(void)
{
    char *env[] = {"FOO=bar", "BAZ=qux", NULL};
    char *v = pmix_getenv("FOO", env);
    report("getenv_found: returns value", NULL != v && 0 == strcmp(v, "bar"));
}

static void test_getenv_second_entry(void)
{
    char *env[] = {"A=1", "B=2", "C=3", NULL};
    char *v = pmix_getenv("C", env);
    report("getenv_second_entry: finds non-first key",
           NULL != v && 0 == strcmp(v, "3"));
}

static void test_getenv_not_found(void)
{
    char *env[] = {"FOO=bar", NULL};
    report("getenv_not_found: returns NULL", NULL == pmix_getenv("MISSING", env));
}

static void test_getenv_null_env(void)
{
    report("getenv_null_env: returns NULL", NULL == pmix_getenv("FOO", NULL));
}

/*
 * When name is "KEY=..." form the implementation matches on the key
 * portion and returns the value stored in env for that key.
 */
static void test_getenv_kv_name(void)
{
    char *env[] = {"FOO=real", NULL};
    char *v = pmix_getenv("FOO=ignored", env);
    report("getenv_kv_name: matches key, returns env value",
           NULL != v && 0 == strcmp(v, "real"));
}

/* ------------------------------------------------------------------ */
/* pmix_unsetenv                                                       */
/* ------------------------------------------------------------------ */

static void test_unsetenv_found(void)
{
    const char *vars[] = {"FOO=bar", "BAZ=qux", NULL};
    char **env = make_env(vars);

    pmix_status_t rc = pmix_unsetenv("FOO", &env);
    report("unsetenv_found: returns SUCCESS", PMIX_SUCCESS == rc);
    report("unsetenv_found: key removed", NULL == pmix_getenv("FOO", env));
    report("unsetenv_found: other key kept", NULL != pmix_getenv("BAZ", env));
    PMIx_Argv_free(env);
}

static void test_unsetenv_not_found(void)
{
    const char *vars[] = {"FOO=bar", NULL};
    char **env = make_env(vars);

    pmix_status_t rc = pmix_unsetenv("MISSING", &env);
    report("unsetenv_not_found: returns ERR_NOT_FOUND",
           PMIX_ERR_NOT_FOUND == rc);
    PMIx_Argv_free(env);
}

static void test_unsetenv_null_env(void)
{
    char **env = NULL;
    pmix_status_t rc = pmix_unsetenv("FOO", &env);
    report("unsetenv_null_env: returns SUCCESS", PMIX_SUCCESS == rc);
}

/* ------------------------------------------------------------------ */
/* pmix_environ_merge                                                  */
/* ------------------------------------------------------------------ */

static void test_merge_both_null(void)
{
    char **m = pmix_environ_merge(NULL, NULL);
    report("merge_both_null: returns NULL", NULL == m);
}

static void test_merge_minor_null(void)
{
    char *major[] = {"A=1", "B=2", NULL};
    char **m = pmix_environ_merge(NULL, major);
    char *v = (NULL != m) ? pmix_getenv("A", m) : NULL;
    report("merge_minor_null: non-NULL result", NULL != m);
    report("merge_minor_null: major key A present", NULL != v && 0 == strcmp(v, "1"));
    PMIx_Argv_free(m);
}

static void test_merge_major_null(void)
{
    char *minor[] = {"X=10", NULL};
    char **m = pmix_environ_merge(minor, NULL);
    report("merge_major_null: non-NULL result", NULL != m);
    report("merge_major_null: minor key X present",
           NULL != m && NULL != pmix_getenv("X", m));
    PMIx_Argv_free(m);
}

static void test_merge_major_wins(void)
{
    char *minor[] = {"K=minor", "ONLY_MINOR=yes", NULL};
    char *major[] = {"K=major", NULL};
    char **m = pmix_environ_merge(minor, major);
    char *kval = (NULL != m) ? pmix_getenv("K", m) : NULL;
    report("merge_major_wins: K has major value",
           NULL != kval && 0 == strcmp(kval, "major"));
    report("merge_major_wins: minor-only key present",
           NULL != m && NULL != pmix_getenv("ONLY_MINOR", m));
    PMIx_Argv_free(m);
}

/* ------------------------------------------------------------------ */
/* pmix_environ_merge_inplace                                         */
/* ------------------------------------------------------------------ */

static void test_merge_inplace_adds_missing(void)
{
    const char *vars[] = {"A=orig", NULL};
    char **env = make_env(vars);
    char *additions[] = {"B=new", "C=new2", NULL};

    pmix_status_t rc = pmix_environ_merge_inplace(&env, additions);
    report("merge_inplace_adds: returns SUCCESS", PMIX_SUCCESS == rc);
    report("merge_inplace_adds: B added", NULL != pmix_getenv("B", env));
    report("merge_inplace_adds: C added", NULL != pmix_getenv("C", env));
    PMIx_Argv_free(env);
}

static void test_merge_inplace_no_override(void)
{
    const char *vars[] = {"A=original", NULL};
    char **env = make_env(vars);
    char *additions[] = {"A=overwrite", NULL};
    pmix_status_t rc;
    char *v;

    rc = pmix_environ_merge_inplace(&env, additions);
    (void) rc;
    v = pmix_getenv("A", env);
    report("merge_inplace_no_override: original A unchanged",
           NULL != v && 0 == strcmp(v, "original"));
    PMIx_Argv_free(env);
}

/* ------------------------------------------------------------------ */
/* pmix_tmp_directory                                                  */
/* ------------------------------------------------------------------ */

static void test_tmp_directory_nonnull(void)
{
    const char *tmp = pmix_tmp_directory();
    report("tmp_directory: non-NULL", NULL != tmp);
}

/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    PMIX_HIDE_UNUSED_PARAMS(argc, argv);

    fprintf(stdout, "\n=== pmix_environ unit tests ===\n\n");

    test_getenv_found();
    test_getenv_second_entry();
    test_getenv_not_found();
    test_getenv_null_env();
    test_getenv_kv_name();

    test_unsetenv_found();
    test_unsetenv_not_found();
    test_unsetenv_null_env();

    test_merge_both_null();
    test_merge_minor_null();
    test_merge_major_null();
    test_merge_major_wins();

    test_merge_inplace_adds_missing();
    test_merge_inplace_no_override();

    test_tmp_directory_nonnull();

    fprintf(stdout, "\nResults: %d passed, %d failed\n\n", npass, nfail);
    return (nfail > 0) ? 1 : 0;
}
