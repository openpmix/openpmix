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
 *   pmix_environ_merge_inplace, pmix_tmp_directory,
 *   pmix_util_harvest_envars.
 *
 * PMIx_Init is called first (the harvest builds pmix_kval_t objects);
 * PMIX_ERR_UNREACH is treated as normal.
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

/* The header states that orig cannot be environ - we extend it with
 * PMIx_Argv_append_nosize(), which reallocs, and environ may not be
 * realloc'd. That was enforced by an assert(), which -DNDEBUG compiles
 * out of exactly the build that has to be protected. */
static void test_merge_inplace_refuses_environ(void)
{
    const char *adds[] = {"PMIX_UT_MERGE_ZZZ=1", NULL};
    char **a = make_env(adds);
    char **before = environ;
    pmix_status_t rc = pmix_environ_merge_inplace(&environ, a);

    report("merge_inplace_environ: refused with ERR_BAD_PARAM",
           PMIX_ERR_BAD_PARAM == rc);
    report("merge_inplace_environ: environ untouched", before == environ);
    PMIx_Argv_free(a);
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
/* pmix_util_harvest_envars                                            */
/* ------------------------------------------------------------------ */

/* Harvest "inc" out of the environment and report how many entries
 * landed on the list. */
static int harvest_count(char **inc, char **exc, pmix_status_t *rc)
{
    pmix_list_t l;
    int n;

    PMIX_CONSTRUCT(&l, pmix_list_t);
    *rc = pmix_util_harvest_envars(inc, exc, &l);
    n = (int) pmix_list_get_size(&l);
    PMIX_LIST_DESTRUCT(&l);
    return n;
}

/* An include entry is the NAME of a variable unless it ends in '*', in
 * which case it is a prefix. Both directions are pinned here because
 * both have callers: a family of variables is spelled "FOO_*", and a
 * single one is spelled "FOO" and must not also take "FOOBAR".
 *
 * This is not academic - src/mca/pmdl/base spells its harvest of the
 * local MCA params "PMIX_MCA_", and while that read as a prefix it
 * worked; the day it became an exact name it silently matched nothing,
 * because no variable is called "PMIX_MCA_". */
static void test_harvest_exact_vs_wildcard(void)
{
    char *exact[] = {"PMIX_UT_HARV", NULL};
    char *wild[] = {"PMIX_UT_HARV*", NULL};
    pmix_status_t rc;

    setenv("PMIX_UT_HARV", "one", 1);
    setenv("PMIX_UT_HARV_EXTRA", "two", 1);

    report("harvest_exact: a bare name takes only that name",
           1 == harvest_count(exact, NULL, &rc) && PMIX_SUCCESS == rc);
    report("harvest_wild: a trailing '*' takes the family",
           2 == harvest_count(wild, NULL, &rc) && PMIX_SUCCESS == rc);

    unsetenv("PMIX_UT_HARV");
    unsetenv("PMIX_UT_HARV_EXTRA");
}

static void test_harvest_exclude(void)
{
    char *wild[] = {"PMIX_UT_HARV*", NULL};
    char *exc[] = {"PMIX_UT_HARV_EXTRA", NULL};
    pmix_status_t rc;

    setenv("PMIX_UT_HARV", "one", 1);
    setenv("PMIX_UT_HARV_EXTRA", "two", 1);
    report("harvest_exclude: an exact exclusion drops just that one",
           1 == harvest_count(wild, exc, &rc) && PMIX_SUCCESS == rc);
    unsetenv("PMIX_UT_HARV");
    unsetenv("PMIX_UT_HARV_EXTRA");
}

static void test_harvest_null_lists(void)
{
    pmix_list_t l;
    pmix_status_t rc;

    /* No include list is "nothing to harvest", the way no exclude list
     * is "nothing to drop". It used to walk the NULL array. */
    report("harvest_null_include: SUCCESS, harvests nothing",
           0 == harvest_count(NULL, NULL, &rc) && PMIX_SUCCESS == rc);

    PMIX_CONSTRUCT(&l, pmix_list_t);
    (void) l;
    PMIX_LIST_DESTRUCT(&l);
    report("harvest_null_ilist: ERR_BAD_PARAM",
           PMIX_ERR_BAD_PARAM == pmix_util_harvest_envars(NULL, NULL, NULL));
}

/* The list is the caller's, and an envar kval on it need not carry a
 * value - PMIx_Envar_load() leaves the value alone when it is handed
 * NULL. Harvesting a variable of the same name compared the two with
 * strcmp() regardless. */
static void test_harvest_existing_valueless_entry(void)
{
    char *inc[] = {"PMIX_UT_HARV", NULL};
    pmix_list_t l;
    pmix_kval_t *kv;
    pmix_status_t rc;

    setenv("PMIX_UT_HARV", "one", 1);
    PMIX_CONSTRUCT(&l, pmix_list_t);
    PMIX_KVAL_NEW(kv, PMIX_SET_ENVAR);
    kv->value->type = PMIX_ENVAR;
    kv->value->data.envar.envar = strdup("PMIX_UT_HARV");
    kv->value->data.envar.value = NULL;
    kv->value->data.envar.separator = ':';
    pmix_list_append(&l, &kv->super);

    rc = pmix_util_harvest_envars(inc, NULL, &l);
    report("harvest_valueless: returns SUCCESS", PMIX_SUCCESS == rc);
    report("harvest_valueless: the entry was filled in, not duplicated",
           1 == pmix_list_get_size(&l));
    kv = (pmix_kval_t *) pmix_list_get_first(&l);
    report("harvest_valueless: it now carries the value",
           NULL != kv->value->data.envar.value &&
           0 == strcmp(kv->value->data.envar.value, "one"));
    PMIX_LIST_DESTRUCT(&l);
    unsetenv("PMIX_UT_HARV");
}

/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    pmix_proc_t myproc;
    pmix_status_t irc;
    PMIX_HIDE_UNUSED_PARAMS(argc, argv);

    irc = PMIx_Init(&myproc, NULL, 0);
    if (PMIX_SUCCESS != irc && PMIX_ERR_UNREACH != irc) {
        fprintf(stderr, "PMIx_Init: %s\n", PMIx_Error_string(irc));
        return 1;
    }

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
    test_merge_inplace_refuses_environ();

    test_tmp_directory_nonnull();

    test_harvest_exact_vs_wildcard();
    test_harvest_exclude();
    test_harvest_null_lists();
    test_harvest_existing_valueless_entry();

    fprintf(stdout, "\nResults: %d passed, %d failed\n\n", npass, nfail);
    PMIx_Finalize(NULL, 0);
    return (nfail > 0) ? 1 : 0;
}
