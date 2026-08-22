/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Unit tests for the local half of PMIx_Get
 * (src/client/pmix_client_get.c) against job-level data.
 *
 * All of these are about one rank value. PMIx_Get(NULL, key, ...) - the
 * legacy-PMI form that a great deal of application code still uses -
 * resolves in process_request() to PMIX_RANK_UNDEF, which the datastore
 * reads as "any rank in this nspace could be the source" and answers by
 * searching the per-rank tables. Job-level data is not in those. It lives
 * either under PMIX_RANK_WILDCARD (a key given at the top level of the
 * PMIx_server_register_nspace info array) or on the job-info list (a key
 * given inside a PMIX_JOB_INFO_ARRAY), and neither is a per-rank table.
 *
 * Two defects followed, one on each side of that split:
 *
 *   the wildcard retry
 *      get_data() had no fallback of its own, so a key stored under
 *      PMIX_RANK_WILDCARD simply missed - PMIX_ERR_NOT_FOUND for a server
 *      or an unconnected client, and for a connected client a server round
 *      trip per call, rescued only by the retry _getnb_cbfunc() makes once
 *      the reply is in hand.
 *
 *   the job-info list
 *      a key given inside a PMIX_JOB_INFO_ARRAY used to be kept on a
 *      list of its own, trk->jobinfo, which only the undef-rank search
 *      consulted - and consulted once per scope, so it came back three
 *      times and PMIx_Get shaped that into an aggregate. The list is gone;
 *      such a key is now stored under PMIX_RANK_WILDCARD like any other
 *      job-level value, so one search finds it and both rank values that
 *      mean "job level" reach it.
 *
 * All of them are pinned below by asking for the *type* of what comes
 * back, not just for success: a data array of duplicates is what the
 * second defect produced, and it is a successful return.
 *
 * The process comes up as a PMIx server with a stub host module, which is
 * what keeps these on the local path - a server never asks anyone else
 * (see the role check in get_data), so what these calls reach is exactly
 * the datastore code under test. No client, no socket and no DVM are
 * involved.
 */

#include "src/include/pmix_config.h"

#include "include/pmix.h"
#include "include/pmix_server.h"

#include "src/include/pmix_globals.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GET_UT_NSPACE "get-ut-job"

/* a key handed to register_nspace at the top level lands under
 * PMIX_RANK_WILDCARD in the internal table */
#define GET_UT_TOPLEVEL "get.ut.toplevel"
/* a key handed to it inside a PMIX_JOB_INFO_ARRAY lands on trk->jobinfo */
#define GET_UT_JOBARRAY "get.ut.jobarray"

#define GET_UT_TOPVAL 11
#define GET_UT_ARRVAL 22

static int npass = 0;
static int nfail = 0;

static pmix_server_module_t mymodule = {0};

static void report(const char *name, int passed)
{
    if (passed) {
        fprintf(stdout, "  PASS: %s\n", name);
        ++npass;
    } else {
        fprintf(stdout, "  FAIL: %s\n", name);
        ++nfail;
    }
}

static pmix_status_t register_job(void)
{
    pmix_info_t info[4];
    pmix_info_t *ja;
    pmix_data_array_t da;
    pmix_nspace_t ns;
    pmix_status_t rc;
    char *noderegex = NULL, *ppnregex = NULL;
    uint32_t topval = GET_UT_TOPVAL;
    uint32_t arrval = GET_UT_ARRVAL;

    PMIx_generate_regex(pmix_globals.hostname, &noderegex);
    PMIx_generate_ppn("0,1", &ppnregex);

    PMIX_INFO_CREATE(ja, 1);
    if (NULL == ja) {
        return PMIX_ERR_NOMEM;
    }
    PMIX_INFO_LOAD(&ja[0], GET_UT_JOBARRAY, &arrval, PMIX_UINT32);
    da.type = PMIX_INFO;
    da.size = 1;
    da.array = ja;

    PMIX_INFO_LOAD(&info[0], PMIX_NODE_MAP, noderegex, PMIX_REGEX);
    PMIX_INFO_LOAD(&info[1], PMIX_PROC_MAP, ppnregex, PMIX_REGEX);
    PMIX_INFO_LOAD(&info[2], GET_UT_TOPLEVEL, &topval, PMIX_UINT32);
    PMIX_INFO_LOAD(&info[3], PMIX_JOB_INFO_ARRAY, &da, PMIX_DATA_ARRAY);

    PMIX_LOAD_NSPACE(ns, GET_UT_NSPACE);
    rc = PMIx_server_register_nspace(ns, 2, info, 4, NULL, NULL);
    if (PMIX_OPERATION_SUCCEEDED == rc) {
        rc = PMIX_SUCCESS;
    }

    PMIX_INFO_FREE(ja, 1);
    PMIX_INFO_DESTRUCT(&info[0]);
    PMIX_INFO_DESTRUCT(&info[1]);
    PMIX_INFO_DESTRUCT(&info[2]);
    PMIX_INFO_DESTRUCT(&info[3]);
    free(noderegex);
    free(ppnregex);
    return rc;
}

/* Ask for "key" at "rank" and require a scalar uint32 carrying "expect".
 * Checking the type is the point: the job-info defect returned success
 * with a PMIX_DATA_ARRAY of duplicates, which a status-only assertion
 * would have passed. */
static void check_scalar(const char *name, pmix_rank_t rank,
                         const char *key, uint32_t expect)
{
    pmix_proc_t p;
    pmix_value_t *val = NULL;
    pmix_status_t rc;
    int ok;

    PMIX_LOAD_PROCID(&p, GET_UT_NSPACE, rank);
    rc = PMIx_Get(&p, key, NULL, 0, &val);
    ok = (PMIX_SUCCESS == rc && NULL != val && PMIX_UINT32 == val->type
          && expect == val->data.uint32);
    if (!ok) {
        fprintf(stdout, "    (rc=%s type=%d)\n", PMIx_Error_string(rc),
                (NULL == val) ? -1 : (int) val->type);
    }
    report(name, ok);
    if (NULL != val) {
        PMIX_VALUE_RELEASE(val);
    }
}

/* A NULL key means "everything this proc put", which is answered as a
 * PMIX_DATA_ARRAY of pmix_info_t rather than as a scalar. It is legal for
 * any rank but PMIX_RANK_WILDCARD, and process_request() rejects only the
 * both-NULL and wildcard-plus-NULL-key forms. */
static void check_null_key(const char *name, pmix_rank_t rank)
{
    pmix_proc_t p;
    pmix_value_t *val = NULL;
    pmix_status_t rc;
    int ok;

    PMIX_LOAD_PROCID(&p, GET_UT_NSPACE, rank);
    rc = PMIx_Get(&p, NULL, NULL, 0, &val);
    ok = (PMIX_SUCCESS == rc && NULL != val && PMIX_DATA_ARRAY == val->type
          && NULL != val->data.darray && 0 < val->data.darray->size);
    if (!ok) {
        fprintf(stdout, "    (rc=%s type=%d)\n", PMIx_Error_string(rc),
                (NULL == val) ? -1 : (int) val->type);
    }
    report(name, ok);
    if (NULL != val) {
        PMIX_VALUE_RELEASE(val);
    }
}

/* Both parameter combinations process_request() refuses outright. */
static void check_bad_params(void)
{
    pmix_proc_t p;
    pmix_value_t *val = NULL;
    pmix_status_t rc;

    rc = PMIx_Get(NULL, NULL, NULL, 0, &val);
    report("both proc and key NULL is refused", PMIX_ERR_BAD_PARAM == rc);

    PMIX_LOAD_PROCID(&p, GET_UT_NSPACE, PMIX_RANK_WILDCARD);
    val = NULL;
    rc = PMIx_Get(&p, NULL, NULL, 0, &val);
    report("wildcard rank with a NULL key is refused", PMIX_ERR_BAD_PARAM == rc);
}

int main(void)
{
    pmix_status_t rc;

    rc = PMIx_server_init(&mymodule, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "PMIx_server_init failed: %s\n", PMIx_Error_string(rc));
        return 1;
    }

    rc = register_job();
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "register_nspace failed: %s\n", PMIx_Error_string(rc));
        PMIx_server_finalize();
        return 1;
    }

    fprintf(stdout, "PMIx_Get job-level data:\n");

    /* the shape that always worked, kept so a regression in it is not
     * mistaken for one of the two below */
    check_scalar("top-level key at wildcard rank", PMIX_RANK_WILDCARD,
                 GET_UT_TOPLEVEL, GET_UT_TOPVAL);

    /* get_data()'s wildcard retry */
    check_scalar("top-level key at undef rank", PMIX_RANK_UNDEF,
                 GET_UT_TOPLEVEL, GET_UT_TOPVAL);

    /* a job-array key now lands in the same store as a top-level one, so
     * both rank values that mean "job level" find it */
    check_scalar("job-array key at wildcard rank", PMIX_RANK_WILDCARD,
                 GET_UT_JOBARRAY, GET_UT_ARRVAL);
    check_scalar("job-array key at undef rank", PMIX_RANK_UNDEF,
                 GET_UT_JOBARRAY, GET_UT_ARRVAL);

    check_null_key("NULL key for a specific rank", 0);

    check_bad_params();

    PMIx_server_finalize();

    fprintf(stdout, "\n%d passed, %d failed\n", npass, nfail);
    return (0 == nfail) ? 0 : 1;
}
