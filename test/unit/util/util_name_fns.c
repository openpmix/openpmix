/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Unit tests for pmix_name_fns utility functions:
 *   pmix_util_print_rank, pmix_util_print_name_args,
 *   pmix_util_print_pname_args, pmix_util_compare_proc.
 *
 * These functions are self-initialising (TSD key created on first use)
 * and do not require PMIx_Init or PMIx_server_init.
 *
 * Exit 0 if all tests pass, 1 otherwise.
 */

#include "src/include/pmix_config.h"
#include "src/include/pmix_globals.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pmix.h"
#include "src/util/pmix_name_fns.h"

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
/* pmix_util_print_rank                                                */
/* ------------------------------------------------------------------ */

static void test_print_rank_zero(void)
{
    char *s = pmix_util_print_rank(0);
    report("print_rank_zero", s != NULL && 0 == strcmp(s, "0"));
}

static void test_print_rank_positive(void)
{
    char *s = pmix_util_print_rank(5);
    report("print_rank_positive", s != NULL && 0 == strcmp(s, "5"));
}

static void test_print_rank_undef(void)
{
    char *s = pmix_util_print_rank(PMIX_RANK_UNDEF);
    report("print_rank_undef", s != NULL && 0 == strcmp(s, "UNDEF"));
}

static void test_print_rank_wildcard(void)
{
    char *s = pmix_util_print_rank(PMIX_RANK_WILDCARD);
    report("print_rank_wildcard", s != NULL && 0 == strcmp(s, "WILDCARD"));
}

static void test_print_rank_invalid(void)
{
    char *s = pmix_util_print_rank(PMIX_RANK_INVALID);
    report("print_rank_invalid", s != NULL && 0 == strcmp(s, "INVALID"));
}

static void test_print_rank_local_node(void)
{
    char *s = pmix_util_print_rank(PMIX_RANK_LOCAL_NODE);
    report("print_rank_local_node", s != NULL && 0 == strcmp(s, "LOCAL_NODE"));
}

static void test_print_rank_local_peers(void)
{
    char *s = pmix_util_print_rank(PMIX_RANK_LOCAL_PEERS);
    report("print_rank_local_peers", s != NULL && 0 == strcmp(s, "LOCAL_PEERS"));
}

/* ------------------------------------------------------------------ */
/* pmix_util_print_name_args                                           */
/* ------------------------------------------------------------------ */

static void test_print_name_args_null(void)
{
    char *s = pmix_util_print_name_args(NULL);
    report("print_name_args_null", s != NULL && 0 == strcmp(s, "[NO-NAME]"));
}

static void test_print_name_args_basic(void)
{
    pmix_proc_t p;
    char *s;

    memset(&p, 0, sizeof(p));
    pmix_strncpy(p.nspace, "testns", PMIX_MAX_NSLEN);
    p.rank = 3;
    s = pmix_util_print_name_args(&p);
    report("print_name_args_basic", s != NULL && 0 == strcmp(s, "[testns,3]"));
}

/* ------------------------------------------------------------------ */
/* pmix_util_print_pname_args                                          */
/* ------------------------------------------------------------------ */

static void test_print_pname_args_null(void)
{
    char *s = pmix_util_print_pname_args(NULL);
    report("print_pname_args_null", s != NULL && 0 == strcmp(s, "[NO-NAME]"));
}

static void test_print_pname_args_basic(void)
{
    pmix_name_t pn;
    char *s;

    pn.nspace = "myns";
    pn.rank = 7;
    s = pmix_util_print_pname_args(&pn);
    report("print_pname_args_basic", s != NULL && 0 == strcmp(s, "[myns,7]"));
}

/* ------------------------------------------------------------------ */
/* pmix_util_compare_proc                                              */
/* ------------------------------------------------------------------ */

static void test_compare_proc_equal(void)
{
    pmix_proc_t a, b;

    memset(&a, 0, sizeof(a));
    memset(&b, 0, sizeof(b));
    pmix_strncpy(a.nspace, "ns", PMIX_MAX_NSLEN);
    pmix_strncpy(b.nspace, "ns", PMIX_MAX_NSLEN);
    a.rank = b.rank = 1;
    report("compare_proc_equal: returns 0", 0 == pmix_util_compare_proc(&a, &b));
}

static void test_compare_proc_nspace_diff(void)
{
    pmix_proc_t a, b;

    memset(&a, 0, sizeof(a));
    memset(&b, 0, sizeof(b));
    pmix_strncpy(a.nspace, "aaa", PMIX_MAX_NSLEN);
    pmix_strncpy(b.nspace, "bbb", PMIX_MAX_NSLEN);
    a.rank = b.rank = 0;
    report("compare_proc_nspace_diff: returns nonzero",
           0 != pmix_util_compare_proc(&a, &b));
}

static void test_compare_proc_rank_order(void)
{
    pmix_proc_t lo, hi;

    memset(&lo, 0, sizeof(lo));
    memset(&hi, 0, sizeof(hi));
    pmix_strncpy(lo.nspace, "ns", PMIX_MAX_NSLEN);
    pmix_strncpy(hi.nspace, "ns", PMIX_MAX_NSLEN);
    lo.rank = 2;
    hi.rank = 5;
    /* hi - lo > 0: hi comes after lo */
    report("compare_proc_rank_order: hi > lo gives positive",
           pmix_util_compare_proc(&hi, &lo) > 0);
    report("compare_proc_rank_order: lo < hi gives negative",
           pmix_util_compare_proc(&lo, &hi) < 0);
}

/* ------------------------------------------------------------------ */

/* Regression: pmix_rank_t is uint32_t and the special ranks live at the
 * top of the range. A plain "a->rank - b->rank" comparator reduces mod
 * 2^32 and returns the wrong sign when the ranks differ by more than
 * INT_MAX, mis-ordering a sort. A normal rank must always compare less
 * than PMIX_RANK_WILDCARD / PMIX_RANK_UNDEF. */
static void test_compare_proc_special_ranks(void)
{
    pmix_proc_t normal, wildcard, undef;

    memset(&normal, 0, sizeof(normal));
    memset(&wildcard, 0, sizeof(wildcard));
    memset(&undef, 0, sizeof(undef));
    pmix_strncpy(normal.nspace, "ns", PMIX_MAX_NSLEN);
    pmix_strncpy(wildcard.nspace, "ns", PMIX_MAX_NSLEN);
    pmix_strncpy(undef.nspace, "ns", PMIX_MAX_NSLEN);
    normal.rank = 0;
    wildcard.rank = PMIX_RANK_WILDCARD; /* 0xFFFFFFFE */
    undef.rank = PMIX_RANK_UNDEF;       /* 0xFFFFFFFF */

    report("compare_proc_special: 0 < WILDCARD",
           pmix_util_compare_proc(&normal, &wildcard) < 0);
    report("compare_proc_special: WILDCARD > 0",
           pmix_util_compare_proc(&wildcard, &normal) > 0);
    report("compare_proc_special: WILDCARD < UNDEF",
           pmix_util_compare_proc(&wildcard, &undef) < 0);
    report("compare_proc_special: identical special ranks equal",
           0 == pmix_util_compare_proc(&undef, &undef));
}

int main(int argc, char **argv)
{
    PMIX_HIDE_UNUSED_PARAMS(argc, argv);

    fprintf(stdout, "\n=== pmix_name_fns unit tests ===\n\n");

    test_print_rank_zero();
    test_print_rank_positive();
    test_print_rank_undef();
    test_print_rank_wildcard();
    test_print_rank_invalid();
    test_print_rank_local_node();
    test_print_rank_local_peers();

    test_print_name_args_null();
    test_print_name_args_basic();

    test_print_pname_args_null();
    test_print_pname_args_basic();

    test_compare_proc_equal();
    test_compare_proc_nspace_diff();
    test_compare_proc_rank_order();
    test_compare_proc_special_ranks();

    fprintf(stdout, "\nResults: %d passed, %d failed\n\n", npass, nfail);
    return (nfail > 0) ? 1 : 0;
}
