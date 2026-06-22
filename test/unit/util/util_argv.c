/*
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Unit tests for internal argv utility functions:
 *   pmix_argv_append, pmix_argv_join_range, pmix_argv_len,
 *   pmix_argv_delete, pmix_argv_insert_element.
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
/* pmix_argv_append                                                    */
/* ------------------------------------------------------------------ */

static void test_append_to_null(void)
{
    char **argv = NULL;
    int argc = 0;
    pmix_status_t rc = pmix_argv_append(&argc, &argv, "first");
    report("append to NULL: returns PMIX_SUCCESS", PMIX_SUCCESS == rc);
    report("append to NULL: argc == 1", 1 == argc);
    report("append to NULL: argv[0] matches", argv && 0 == strcmp(argv[0], "first"));
    report("append to NULL: argv[1] is NULL", argv && NULL == argv[1]);
    PMIx_Argv_free(argv);
}

static void test_append_multiple(void)
{
    char **argv = NULL;
    int argc = 0;
    pmix_argv_append(&argc, &argv, "a");
    pmix_argv_append(&argc, &argv, "b");
    pmix_argv_append(&argc, &argv, "c");
    report("append multiple: argc == 3", 3 == argc);
    report("append multiple: argv[0] == \"a\"", argv && 0 == strcmp(argv[0], "a"));
    report("append multiple: argv[1] == \"b\"", argv && 0 == strcmp(argv[1], "b"));
    report("append multiple: argv[2] == \"c\"", argv && 0 == strcmp(argv[2], "c"));
    report("append multiple: argv[3] is NULL", argv && NULL == argv[3]);
    PMIx_Argv_free(argv);
}

/* ------------------------------------------------------------------ */
/* pmix_argv_join_range                                                */
/* ------------------------------------------------------------------ */

static void test_join_full_range(void)
{
    char **argv = NULL;
    PMIx_Argv_append_nosize(&argv, "foo");
    PMIx_Argv_append_nosize(&argv, "bar");
    PMIx_Argv_append_nosize(&argv, "baz");

    char *joined = pmix_argv_join_range(argv, 0, 3, ',');
    report("join full range", joined && 0 == strcmp(joined, "foo,bar,baz"));
    free(joined);
    PMIx_Argv_free(argv);
}

static void test_join_partial_range(void)
{
    char **argv = NULL;
    PMIx_Argv_append_nosize(&argv, "foo");
    PMIx_Argv_append_nosize(&argv, "bar");
    PMIx_Argv_append_nosize(&argv, "baz");

    char *joined = pmix_argv_join_range(argv, 1, 3, ':');
    report("join partial range [1,3)", joined && 0 == strcmp(joined, "bar:baz"));
    free(joined);
    PMIx_Argv_free(argv);
}

static void test_join_single_element(void)
{
    char **argv = NULL;
    PMIx_Argv_append_nosize(&argv, "only");
    PMIx_Argv_append_nosize(&argv, "other");

    char *joined = pmix_argv_join_range(argv, 0, 1, ',');
    report("join single element", joined && 0 == strcmp(joined, "only"));
    free(joined);
    PMIx_Argv_free(argv);
}

static void test_join_empty_range(void)
{
    char **argv = NULL;
    PMIx_Argv_append_nosize(&argv, "foo");

    char *joined = pmix_argv_join_range(argv, 0, 0, ',');
    report("join empty range returns \"\"", joined && 0 == strcmp(joined, ""));
    free(joined);
    PMIx_Argv_free(argv);
}

static void test_join_null_argv(void)
{
    char *joined = pmix_argv_join_range(NULL, 0, 1, ',');
    report("join NULL argv returns \"\"", joined && 0 == strcmp(joined, ""));
    free(joined);
}

/* ------------------------------------------------------------------ */
/* pmix_argv_len                                                       */
/* ------------------------------------------------------------------ */

static void test_len_null(void)
{
    size_t n = pmix_argv_len(NULL);
    report("argv_len(NULL) == 0", 0 == n);
}

static void test_len_nonempty(void)
{
    char **argv = NULL;
    PMIx_Argv_append_nosize(&argv, "abc");
    PMIx_Argv_append_nosize(&argv, "de");

    size_t n = pmix_argv_len(argv);
    /* must account for at least the two strings plus their null terminators */
    report("argv_len non-empty > strlen sum", n > (3 + 1 + 2 + 1));
    PMIx_Argv_free(argv);
}

/* ------------------------------------------------------------------ */
/* pmix_argv_delete                                                    */
/* ------------------------------------------------------------------ */

static void test_delete_middle(void)
{
    char **argv = NULL;
    int argc = 0;
    pmix_argv_append(&argc, &argv, "a");
    pmix_argv_append(&argc, &argv, "b");
    pmix_argv_append(&argc, &argv, "c");
    pmix_argv_append(&argc, &argv, "d");

    pmix_status_t rc = pmix_argv_delete(&argc, &argv, 1, 2);
    report("delete middle: returns PMIX_SUCCESS", PMIX_SUCCESS == rc);
    report("delete middle: argc == 2", 2 == argc);
    report("delete middle: argv[0] == \"a\"", argv && 0 == strcmp(argv[0], "a"));
    report("delete middle: argv[1] == \"d\"", argv && 0 == strcmp(argv[1], "d"));
    report("delete middle: argv[2] is NULL", argv && NULL == argv[2]);
    PMIx_Argv_free(argv);
}

static void test_delete_from_end(void)
{
    char **argv = NULL;
    int argc = 0;
    pmix_argv_append(&argc, &argv, "x");
    pmix_argv_append(&argc, &argv, "y");
    pmix_argv_append(&argc, &argv, "z");

    pmix_argv_delete(&argc, &argv, 1, 10); /* delete more than available */
    report("delete from end: argc == 1", 1 == argc);
    report("delete from end: argv[0] == \"x\"", argv && 0 == strcmp(argv[0], "x"));
    PMIx_Argv_free(argv);
}

static void test_delete_zero_elements(void)
{
    char **argv = NULL;
    int argc = 0;
    pmix_argv_append(&argc, &argv, "p");
    pmix_argv_append(&argc, &argv, "q");

    pmix_status_t rc = pmix_argv_delete(&argc, &argv, 0, 0);
    report("delete zero elements: returns PMIX_SUCCESS", PMIX_SUCCESS == rc);
    report("delete zero elements: argc unchanged", 2 == argc);
    PMIx_Argv_free(argv);
}

/* ------------------------------------------------------------------ */
/* pmix_argv_insert_element                                            */
/* ------------------------------------------------------------------ */

static void test_insert_middle(void)
{
    char **argv = NULL;
    int argc = 0;
    pmix_argv_append(&argc, &argv, "a");
    pmix_argv_append(&argc, &argv, "c");

    pmix_status_t rc = pmix_argv_insert_element(&argv, 1, "b");
    report("insert middle: returns PMIX_SUCCESS", PMIX_SUCCESS == rc);
    report("insert middle: argv[0] == \"a\"", argv && 0 == strcmp(argv[0], "a"));
    report("insert middle: argv[1] == \"b\"", argv && 0 == strcmp(argv[1], "b"));
    report("insert middle: argv[2] == \"c\"", argv && 0 == strcmp(argv[2], "c"));
    report("insert middle: argv[3] is NULL", argv && NULL == argv[3]);
    PMIx_Argv_free(argv);
}

static void test_insert_at_start(void)
{
    char **argv = NULL;
    int argc = 0;
    pmix_argv_append(&argc, &argv, "b");
    pmix_argv_append(&argc, &argv, "c");

    pmix_argv_insert_element(&argv, 0, "a");
    report("insert at start: argv[0] == \"a\"", argv && 0 == strcmp(argv[0], "a"));
    report("insert at start: argv[1] == \"b\"", argv && 0 == strcmp(argv[1], "b"));
    PMIx_Argv_free(argv);
}

/* ------------------------------------------------------------------ */
/* pmix_argv_append_unique_idx                                         */
/* ------------------------------------------------------------------ */

static void test_append_unique_idx_new(void)
{
    char **argv = NULL;
    int idx = -1;
    pmix_argv_append_unique_idx(&idx, &argv, "foo");
    report("append_unique_idx new: idx == 0", 0 == idx);
    report("append_unique_idx new: argv[0] matches", argv && 0 == strcmp(argv[0], "foo"));
    PMIx_Argv_free(argv);
}

static void test_append_unique_idx_duplicate(void)
{
    char **argv = NULL;
    int idx = -1;
    pmix_argv_append_unique_idx(&idx, &argv, "foo");
    pmix_argv_append_unique_idx(&idx, &argv, "bar");

    int idx2 = -1;
    pmix_argv_append_unique_idx(&idx2, &argv, "foo"); /* duplicate */
    report("append_unique_idx dup: returns existing index 0", 0 == idx2);
    report("append_unique_idx dup: count stays at 2", 2 == PMIx_Argv_count(argv));
    PMIx_Argv_free(argv);
}

/* ------------------------------------------------------------------ */
/* pmix_argv_insert                                                    */
/* ------------------------------------------------------------------ */

static void test_insert_array_middle(void)
{
    char **target = NULL;
    int argc = 0;
    char **source = NULL;

    pmix_argv_append(&argc, &target, "a");
    pmix_argv_append(&argc, &target, "d");
    PMIx_Argv_append_nosize(&source, "b");
    PMIx_Argv_append_nosize(&source, "c");

    pmix_status_t rc = pmix_argv_insert(&target, 1, source);
    report("insert array middle: returns PMIX_SUCCESS", PMIX_SUCCESS == rc);
    report("insert array middle: argv[0] == \"a\"", target && 0 == strcmp(target[0], "a"));
    report("insert array middle: argv[1] == \"b\"", target && 0 == strcmp(target[1], "b"));
    report("insert array middle: argv[2] == \"c\"", target && 0 == strcmp(target[2], "c"));
    report("insert array middle: argv[3] == \"d\"", target && 0 == strcmp(target[3], "d"));
    report("insert array middle: argv[4] is NULL", target && NULL == target[4]);
    /* source must be left unaffected */
    report("insert array middle: source preserved", source && 0 == strcmp(source[0], "b"));
    PMIx_Argv_free(target);
    PMIx_Argv_free(source);
}

static void test_insert_array_past_end(void)
{
    char **target = NULL;
    int argc = 0;
    char **source = NULL;

    pmix_argv_append(&argc, &target, "a");
    PMIx_Argv_append_nosize(&source, "b");
    PMIx_Argv_append_nosize(&source, "c");

    /* start beyond the end appends */
    pmix_argv_insert(&target, 10, source);
    report("insert array past end: argv[0] == \"a\"", target && 0 == strcmp(target[0], "a"));
    report("insert array past end: argv[1] == \"b\"", target && 0 == strcmp(target[1], "b"));
    report("insert array past end: argv[2] == \"c\"", target && 0 == strcmp(target[2], "c"));
    report("insert array past end: argv[3] is NULL", target && NULL == target[3]);
    PMIx_Argv_free(target);
    PMIx_Argv_free(source);
}

/* ------------------------------------------------------------------ */
/* pmix_argv_copy_strip                                                */
/* ------------------------------------------------------------------ */

static void test_copy_strip_quotes(void)
{
    char **argv = NULL;
    PMIx_Argv_append_nosize(&argv, "\"quoted\"");
    PMIx_Argv_append_nosize(&argv, "plain");

    char **dupv = pmix_argv_copy_strip(argv);
    report("copy_strip: non-NULL result", NULL != dupv);
    report("copy_strip: quotes removed", dupv && 0 == strcmp(dupv[0], "quoted"));
    report("copy_strip: plain unchanged", dupv && 0 == strcmp(dupv[1], "plain"));
    report("copy_strip: NULL terminated", dupv && NULL == dupv[2]);
    /* original must be restored to its quoted form */
    report("copy_strip: original restored", 0 == strcmp(argv[0], "\"quoted\""));
    PMIx_Argv_free(dupv);
    PMIx_Argv_free(argv);
}

static void test_copy_strip_empty_element(void)
{
    char **argv = NULL;
    /* an empty element must not trigger an out-of-bounds access */
    PMIx_Argv_append_nosize(&argv, "");
    PMIx_Argv_append_nosize(&argv, "x");

    char **dupv = pmix_argv_copy_strip(argv);
    report("copy_strip empty: non-NULL result", NULL != dupv);
    report("copy_strip empty: argv[0] == \"\"", dupv && 0 == strcmp(dupv[0], ""));
    report("copy_strip empty: argv[1] == \"x\"", dupv && 0 == strcmp(dupv[1], "x"));
    report("copy_strip empty: NULL terminated", dupv && NULL == dupv[2]);
    PMIx_Argv_free(dupv);
    PMIx_Argv_free(argv);
}

static void test_copy_strip_null(void)
{
    char **dupv = pmix_argv_copy_strip(NULL);
    report("copy_strip NULL returns NULL", NULL == dupv);
}

/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    PMIX_HIDE_UNUSED_PARAMS(argc, argv);

    fprintf(stdout, "\n=== pmix_argv internal functions unit tests ===\n\n");

    test_append_to_null();
    test_append_multiple();

    test_join_full_range();
    test_join_partial_range();
    test_join_single_element();
    test_join_empty_range();
    test_join_null_argv();

    test_len_null();
    test_len_nonempty();

    test_delete_middle();
    test_delete_from_end();
    test_delete_zero_elements();

    test_insert_middle();
    test_insert_at_start();

    test_insert_array_middle();
    test_insert_array_past_end();

    test_copy_strip_quotes();
    test_copy_strip_empty_element();
    test_copy_strip_null();

    test_append_unique_idx_new();
    test_append_unique_idx_duplicate();

    fprintf(stdout, "\nResults: %d passed, %d failed\n\n", npass, nfail);
    return (nfail > 0) ? 1 : 0;
}
