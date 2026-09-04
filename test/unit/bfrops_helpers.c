/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Degenerate inputs to the public helper APIs that src/mca/bfrops
 * implements.
 *
 * A surprising amount of the installed PMIx C API lives in
 * bfrops/base - the whole PMIx_Argv_* family, the PMIx_Info_list_*
 * builders, the construct/create/load/free helpers for every PMIx
 * struct - because it is all fundamentally data manipulation. It is
 * also the part of the API applications call most casually, which makes
 * "what happens when the caller passes NULL" a question with a large
 * blast radius and no obvious owner.
 *
 * This asserts survival on that question, not any particular return
 * code. Refusing with PMIX_ERR_BAD_PARAM and quietly doing nothing are
 * both defensible answers for most of these, and which one a given
 * helper picks is not the subject; the subject is that the answer is
 * not a signal.
 *
 * Two cases here do assert behaviour, because a caller can tell the
 * difference and does depend on it:
 *   - PMIx_Info_list_release(NULL) must be a no-op, like every other
 *     release in that file. A caller whose PMIx_Info_list_start()
 *     failed holds exactly that NULL.
 *   - a zero-length create must not hand back something a matching free
 *     will choke on.
 */

#include "src/include/pmix_config.h"
#include "include/pmix.h"
#include "include/pmix_server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static pmix_server_module_t mymodule = {
    .client_connected = NULL
};

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
/* the argv family                                                     */
/* ------------------------------------------------------------------ */

static void test_argv_degenerate(void)
{
    char **a = NULL;
    char *s;

    (void) PMIx_Argv_count(NULL);
    PMIx_Argv_free(NULL);

    s = PMIx_Argv_join(NULL, ',');
    if (NULL != s) {
        free(s);
    }

    a = PMIx_Argv_split(NULL, ',');
    PMIx_Argv_free(a);
    a = PMIx_Argv_split("", ',');
    PMIx_Argv_free(a);
    a = PMIx_Argv_split(",,,", ',');
    PMIx_Argv_free(a);
    a = PMIx_Argv_copy(NULL);
    PMIx_Argv_free(a);

    /* a NULL string is nothing to append, and a NULL array pointer is
     * nowhere to append it to */
    a = NULL;
    (void) PMIx_Argv_append_nosize(&a, NULL);
    (void) PMIx_Argv_prepend_nosize(&a, NULL);
    (void) PMIx_Argv_append_unique_nosize(&a, NULL);
    (void) PMIx_Argv_append_nosize(NULL, "x");
    PMIx_Argv_free(a);

    report("the argv helpers survive NULL arrays and NULL strings", 1);
}

static void test_argv_still_works(void)
{
    char **a = NULL;
    char *joined;
    int ok = 1;

    /* the guards must not have cost the ordinary path anything */
    (void) PMIx_Argv_append_nosize(&a, "one");
    (void) PMIx_Argv_append_nosize(&a, "two");
    (void) PMIx_Argv_prepend_nosize(&a, "zero");
    (void) PMIx_Argv_append_unique_nosize(&a, "two");
    ok = ok && (3 == PMIx_Argv_count(a));

    joined = PMIx_Argv_join(a, ':');
    ok = ok && (NULL != joined) && (0 == strcmp(joined, "zero:one:two"));
    if (NULL != joined) {
        free(joined);
    }
    PMIx_Argv_free(a);

    report("the argv helpers still do their job", ok);
}

/* ------------------------------------------------------------------ */
/* proc and key helpers                                                */
/* ------------------------------------------------------------------ */

static void test_procid_degenerate(void)
{
    pmix_proc_t p;
    pmix_key_t k;
    pmix_nspace_t n, cluster, nspace;

    PMIx_Load_procid(NULL, "ns", 0);
    (void) PMIx_Check_procid(NULL, NULL);
    (void) PMIx_Check_procid_strict(NULL, NULL);
    (void) PMIx_Procid_invalid(NULL);
    PMIx_Xfer_procid(NULL, NULL);

    PMIx_Load_key(k, NULL);
    (void) PMIx_Check_key(NULL, NULL);
    (void) PMIx_Check_nspace(NULL, NULL);
    (void) PMIx_Check_nspace_strict(NULL, NULL);
    PMIx_Load_nspace(n, NULL);

    PMIx_Multicluster_nspace_parse(NULL, cluster, nspace);
    PMIx_Load_procid(&p, NULL, 0);

    report("the proc and key helpers survive NULL arguments", 1);
}

/* The two namespace comparisons answer opposite questions, and the whole
 * point of having both is that they disagree about an unset namespace.
 * This is one of the few things in this file that asserts behaviour
 * rather than survival, because a caller can tell the difference and the
 * difference is the reason the strict form exists. */
static void test_nspace_comparison(void)
{
    int ok = 1;

    /* two real namespaces: both routines agree, both ways */
    ok = ok && PMIx_Check_nspace("jobA", "jobA");
    ok = ok && PMIx_Check_nspace_strict("jobA", "jobA");
    ok = ok && !PMIx_Check_nspace("jobA", "jobB");
    ok = ok && !PMIx_Check_nspace_strict("jobA", "jobB");

    /* an unset namespace is a WILDCARD to the ordinary form... */
    ok = ok && PMIx_Check_nspace("", "jobA");
    ok = ok && PMIx_Check_nspace("jobA", "");
    ok = ok && PMIx_Check_nspace(NULL, "jobA");

    /* ...and matches NOTHING in the strict form, including another
     * unset namespace: "unset" is not a value two things can agree on */
    ok = ok && !PMIx_Check_nspace_strict("", "jobA");
    ok = ok && !PMIx_Check_nspace_strict("jobA", "");
    ok = ok && !PMIx_Check_nspace_strict(NULL, "jobA");
    ok = ok && !PMIx_Check_nspace_strict("jobA", NULL);
    ok = ok && !PMIx_Check_nspace_strict("", "");
    ok = ok && !PMIx_Check_nspace_strict(NULL, NULL);

    report("an unset namespace is a wildcard to one comparison and to "
           "neither side of the other", ok);
}

/* Same question one level up: PMIX_CHECK_PROCID wildcards twice over,
 * through the namespace and again through PMIX_RANK_WILDCARD, and the
 * strict form does neither.  Note the rank half is strict in a different
 * way from the namespace half - it compares literally rather than
 * refusing any particular value - because PMIX_RANK_WILDCARD is a real
 * participant designation, not an unset field. */
static void test_procid_comparison(void)
{
    pmix_proc_t a, b;
    int ok = 1;

    PMIx_Load_procid(&a, "jobA", 0);
    PMIx_Load_procid(&b, "jobA", 0);
    ok = ok && PMIx_Check_procid(&a, &b);
    ok = ok && PMIx_Check_procid_strict(&a, &b);

    /* a different rank of the same job is a different proc to both */
    PMIx_Load_procid(&b, "jobA", 1);
    ok = ok && !PMIx_Check_procid(&a, &b);
    ok = ok && !PMIx_Check_procid_strict(&a, &b);

    /* the same rank of a different job, likewise */
    PMIx_Load_procid(&b, "jobB", 0);
    ok = ok && !PMIx_Check_procid(&a, &b);
    ok = ok && !PMIx_Check_procid_strict(&a, &b);

    /* a wildcard rank stands for every rank to the ordinary form, and
     * for itself alone to the strict one - "jobA/0" and "jobA/WILDCARD"
     * are different participant lists */
    PMIx_Load_procid(&b, "jobA", PMIX_RANK_WILDCARD);
    ok = ok && PMIx_Check_procid(&a, &b);
    ok = ok && !PMIx_Check_procid_strict(&a, &b);

    /* ...but it does name the same thing as another wildcard: the strict
     * form declines to treat one rank as standing for the others, it does
     * not refuse any particular rank */
    PMIx_Load_procid(&a, "jobA", PMIX_RANK_WILDCARD);
    ok = ok && PMIx_Check_procid_strict(&a, &b);

    /* an unset namespace is a wildcard to one and matches nothing in the
     * other, exactly as for the bare namespace comparison */
    PMIx_Load_procid(&a, NULL, 0);
    PMIx_Load_procid(&b, "jobA", 0);
    ok = ok && PMIx_Check_procid(&a, &b);
    ok = ok && !PMIx_Check_procid_strict(&a, &b);

    /* two absent procIDs are the same proc to one and neither to the
     * other: an absent procID names no process, so it matches none */
    ok = ok && PMIx_Check_procid(NULL, NULL);
    ok = ok && !PMIx_Check_procid_strict(NULL, NULL);
    ok = ok && !PMIx_Check_procid(NULL, &b);
    ok = ok && !PMIx_Check_procid_strict(NULL, &b);

    report("neither an unset namespace nor a wildcard rank stands for "
           "anything in the strict procID comparison", ok);
}

/* The two halves of a multi-cluster nspace are written element by
 * element and the nspace half is never terminated, so both outputs have
 * to be cleared before anything is written into them - otherwise
 * whatever the caller's buffer held before shows through. */
static void test_multicluster_parse(void)
{
    pmix_nspace_t target, cluster, nspace;
    int ok = 1;

    /* the parameter is declared as a pmix_nspace_t, so hand it one */
    PMIx_Load_nspace(target, "clusterA:job17");
    memset(cluster, 'X', sizeof(cluster));
    memset(nspace, 'X', sizeof(nspace));
    PMIx_Multicluster_nspace_parse(target, cluster, nspace);
    ok = ok && (0 == strcmp(cluster, "clusterA"))
            && (0 == strcmp(nspace, "job17"));

    /* no separator at all: there is no nspace half */
    PMIx_Load_nspace(target, "nocolonhere");
    memset(cluster, 'X', sizeof(cluster));
    memset(nspace, 'X', sizeof(nspace));
    PMIx_Multicluster_nspace_parse(target, cluster, nspace);
    ok = ok && (0 == strcmp(cluster, "nocolonhere")) && ('\0' == nspace[0]);

    report("a multi-cluster nspace parses into cleared halves", ok);
}

/* ------------------------------------------------------------------ */
/* create / free at zero, and free of nothing                          */
/* ------------------------------------------------------------------ */

static void test_zero_and_null_lifecycle(void)
{
    pmix_info_t *i;
    pmix_proc_t *p;
    pmix_value_t *v;
    pmix_query_t *q;
    pmix_app_t *ap;
    pmix_data_array_t *d;

    i = PMIx_Info_create(0);
    PMIx_Info_free(i, 0);
    PMIx_Info_free(NULL, 3);

    p = PMIx_Proc_create(0);
    PMIx_Proc_free(p, 0);
    PMIx_Proc_free(NULL, 3);

    v = PMIx_Value_create(0);
    PMIx_Value_free(v, 0);

    q = PMIx_Query_create(0);
    PMIx_Query_free(q, 0);

    ap = PMIx_App_create(0);
    PMIx_App_free(ap, 0);

    d = PMIx_Data_array_create(0, PMIX_INT);
    PMIx_Data_array_free(d);
    PMIx_Data_array_free(NULL);

    report("creating zero of a thing, and freeing nothing", 1);
}

static void test_load_helpers_with_no_data(void)
{
    pmix_info_t info;
    pmix_value_t v;
    pmix_envar_t e;
    pmix_byte_object_t b;
    pmix_pdata_t pd;
    pmix_regattr_t r;

    PMIx_Info_load(&info, NULL, NULL, PMIX_STRING);
    PMIX_INFO_DESTRUCT(&info);
    PMIx_Info_load(&info, "k", NULL, PMIX_STRING);
    PMIX_INFO_DESTRUCT(&info);

    PMIX_VALUE_CONSTRUCT(&v);
    (void) PMIx_Value_load(&v, NULL, PMIX_STRING);
    PMIX_VALUE_DESTRUCT(&v);
    (void) PMIx_Value_true(NULL);

    PMIx_Envar_construct(&e);
    PMIx_Envar_load(&e, NULL, NULL, ':');
    PMIx_Envar_destruct(&e);

    PMIx_Byte_object_construct(&b);
    PMIx_Byte_object_load(&b, NULL, 0);
    PMIx_Byte_object_destruct(&b);

    PMIx_Pdata_construct(&pd);
    PMIx_Pdata_load(&pd, NULL, NULL, NULL, PMIX_STRING);
    PMIx_Pdata_destruct(&pd);

    PMIx_Regattr_construct(&r);
    PMIx_Regattr_load(&r, NULL, NULL, PMIX_STRING, NULL);
    PMIx_Regattr_destruct(&r);

    report("the load helpers survive absent data", 1);
}

/* ------------------------------------------------------------------ */
/* the info list builders                                              */
/* ------------------------------------------------------------------ */

static void test_info_list_degenerate(void)
{
    pmix_info_t info;
    pmix_data_array_t d;
    void *lst;
    void *nxt = NULL;

    /* every entry point with no list at all */
    (void) PMIx_Info_list_add(NULL, "k", "v", PMIX_STRING);
    (void) PMIx_Info_list_add_unique(NULL, "k", "v", PMIX_STRING, true);
    (void) PMIx_Info_list_prepend(NULL, "k", "v", PMIX_STRING);
    (void) PMIx_Info_list_convert(NULL, &d);
    (void) PMIx_Info_list_get_size(NULL);
    (void) PMIx_Info_list_get_info(NULL, NULL, &nxt);
    PMIx_Info_list_release(NULL);

    PMIx_Info_load(&info, "k", "v", PMIX_STRING);
    (void) PMIx_Info_list_insert(NULL, &info);
    (void) PMIx_Info_list_xfer(NULL, &info);

    /* a real but empty list */
    lst = PMIx_Info_list_start();
    if (NULL != lst) {
        (void) PMIx_Info_list_add(lst, NULL, NULL, PMIX_STRING);
        (void) PMIx_Info_list_get_size(lst);
        nxt = NULL;
        (void) PMIx_Info_list_get_info(lst, NULL, &nxt);
        (void) PMIx_Info_list_convert(lst, &d);
        PMIx_Info_list_release(lst);
    }
    PMIX_INFO_DESTRUCT(&info);

    report("the info list builders survive a NULL or empty list", 1);
}

static void test_info_list_still_works(void)
{
    void *lst;
    pmix_data_array_t d;
    pmix_status_t rc;
    int ok = 1;

    lst = PMIx_Info_list_start();
    if (NULL == lst) {
        report("the info list builders still do their job", 0);
        return;
    }
    rc = PMIx_Info_list_add(lst, "first", "one", PMIX_STRING);
    ok = ok && (PMIX_SUCCESS == rc);
    rc = PMIx_Info_list_add(lst, "second", "two", PMIX_STRING);
    ok = ok && (PMIX_SUCCESS == rc);
    ok = ok && (2 == PMIx_Info_list_get_size(lst));

    memset(&d, 0, sizeof(d));
    rc = PMIx_Info_list_convert(lst, &d);
    ok = ok && (PMIX_SUCCESS == rc) && (2 == d.size) && (NULL != d.array);
    if (PMIX_SUCCESS == rc) {
        pmix_info_t *ip = (pmix_info_t *) d.array;
        ok = ok && (0 == strcmp(ip[0].key, "first"))
                && (0 == strcmp(ip[1].key, "second"));
        PMIX_DATA_ARRAY_DESTRUCT(&d);
    }
    PMIx_Info_list_release(lst);

    report("the info list builders still do their job", ok);
}

/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    pmix_status_t rc;

    (void) argc;
    (void) argv;

    rc = PMIx_server_init(&mymodule, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "PMIx_server_init failed: %s\n", PMIx_Error_string(rc));
        return 1;
    }

    fprintf(stdout, "\n=== bfrops public helper unit tests ===\n\n");

    test_argv_degenerate();
    test_argv_still_works();
    test_procid_degenerate();
    test_nspace_comparison();
    test_procid_comparison();
    test_multicluster_parse();
    test_zero_and_null_lifecycle();
    test_load_helpers_with_no_data();
    test_info_list_degenerate();
    test_info_list_still_works();

    fprintf(stdout, "\nResults: %d passed, %d failed\n\n", npass, nfail);

    PMIx_server_finalize();

    return (nfail > 0) ? 1 : 0;
}
