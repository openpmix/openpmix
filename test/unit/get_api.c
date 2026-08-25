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
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

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

static volatile bool regdone = false;

static void regcbfunc(pmix_status_t status, void *cbdata)
{
    (void) status;
    (void) cbdata;
    regdone = true;
}

static pmix_status_t register_job(void)
{
    pmix_info_t info[4];
    pmix_info_t *ja;
    pmix_data_array_t da;
    pmix_nspace_t ns;
    pmix_proc_t p0;
    pmix_status_t rc;
    char *noderegex = NULL, *ppnregex = NULL;
    uint32_t topval = GET_UT_TOPVAL;
    uint32_t arrval = GET_UT_ARRVAL;
    int n;

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
    if (PMIX_SUCCESS != rc) {
        return rc;
    }

    PMIX_LOAD_PROCID(&p0, GET_UT_NSPACE, 0);
    rc = PMIx_server_register_client(&p0, geteuid(), getegid(), NULL, regcbfunc, NULL);
    if (PMIX_OPERATION_SUCCEEDED == rc) {
        return PMIX_SUCCESS;
    }
    if (PMIX_SUCCESS != rc) {
        return rc;
    }
    /* do not fork the client before it is registered */
    for (n = 0; n < 400 && !regdone; n++) {
        usleep(50000);
    }
    return PMIX_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* the client half                                                    */
/* ------------------------------------------------------------------ */

/* Ask with a NULL proc, which process_request() resolves to
 * PMIX_RANK_UNDEF - the shape the datastore has to answer out of its
 * job-level store rather than out of any rank's. This runs in a real
 * client so it reaches whichever gds module the server offered: "hash"
 * everywhere, "shmem3" where that is built. Both had the same hole, and
 * neither is exercised by the server-side cases above, which only ever
 * reach "hash".
 *
 * Do not "simplify" this into the parent by asking with an explicit
 * PMIX_RANK_UNDEF from the server: the server's own gds is always hash,
 * so shmem3 would go untested and the case would say so nowhere.
 *
 * PMIX_OPTIONAL is load-bearing. The question is whether the module can
 * answer out of what it already holds, and without the directive a miss
 * is sent to the server instead - which here is the parent process, with
 * a stub host module that cannot answer either, so the request parks and
 * the whole test wedges rather than failing. Confined to the datastore,
 * a regression is an immediate FAIL line. */
static int check_client_scalar(const char *name, const char *key, uint32_t expect)
{
    pmix_value_t *val = NULL;
    pmix_status_t rc;
    pmix_info_t optional;
    int ok;

    PMIX_INFO_LOAD(&optional, PMIX_OPTIONAL, NULL, PMIX_BOOL);
    rc = PMIx_Get(NULL, key, &optional, 1, &val);
    PMIX_INFO_DESTRUCT(&optional);
    ok = (PMIX_SUCCESS == rc && NULL != val && PMIX_UINT32 == val->type
          && expect == val->data.uint32);
    fprintf(stdout, "  %s: %s (rc=%s type=%d)\n", ok ? "PASS" : "FAIL", name,
            PMIx_Error_string(rc), (NULL == val) ? -1 : (int) val->type);
    if (NULL != val) {
        PMIX_VALUE_RELEASE(val);
    }
    return ok ? 0 : 1;
}

/* ------------------------------------------------------------------ */
/* two outstanding requests, one of which is coalesced onto the other  */
/* ------------------------------------------------------------------ */

/* pmix_client_globals.pending_requests is both the table get_data()
 * consults to avoid asking the server twice for one proc's data, and the
 * table _getnb_cbfunc() walks to deliver a reply. The two asked it
 * different questions: the coalescing scan matched with PMIX_CHECK_NAMES,
 * which reports a match whenever *either* rank is PMIX_RANK_WILDCARD,
 * while delivery compares ranks exactly. So a get for a specific rank
 * issued while a get at WILDCARD for the same namespace was outstanding
 * was folded onto it, never sent, and then never matched by the reply -
 * and nothing else drains that list, so the callback simply never came.
 *
 * Both requests carry PMIX_IMMEDIATE, which is what makes this bounded:
 * without it the server parks a request it cannot satisfy and neither
 * call would complete, fixed or not. With it, a miss is answered at once,
 * so an unfixed library shows exactly one arrival and this case fails on
 * the timeout rather than wedging the suite.
 *
 * Issue order matters. The WILDCARD request has to be the one already on
 * the list when the second arrives. */

#define GET_UT_ABSENT_A "get.ut.absent.a"
#define GET_UT_ABSENT_B "get.ut.absent.b"

static volatile int narrived = 0;
static volatile bool a_done = false;
static volatile bool b_done = false;

static void arrival_cb(pmix_status_t status, pmix_value_t *kv, void *cbdata)
{
    volatile bool *flag = (volatile bool *) cbdata;

    (void) status;
    /* the caller owns the value on this callback - see PMIx_Get(3) */
    if (NULL != kv) {
        PMIX_VALUE_RELEASE(kv);
    }
    *flag = true;
    ++narrived;
}

static int check_coalescing(void)
{
    pmix_info_t immediate;
    pmix_proc_t p;
    pmix_status_t rc;
    int n, ok;

    PMIX_INFO_LOAD(&immediate, PMIX_IMMEDIATE, NULL, PMIX_BOOL);

    PMIX_LOAD_PROCID(&p, GET_UT_NSPACE, PMIX_RANK_WILDCARD);
    rc = PMIx_Get_nb(&p, GET_UT_ABSENT_A, &immediate, 1, arrival_cb, (void *) &a_done);
    if (PMIX_SUCCESS != rc) {
        fprintf(stdout, "  FAIL: client: wildcard request refused (%s)\n",
                PMIx_Error_string(rc));
        PMIX_INFO_DESTRUCT(&immediate);
        return 1;
    }

    /* rank 1 of this namespace never connects, so the server answers this
     * out of what it holds rather than from a peer */
    PMIX_LOAD_PROCID(&p, GET_UT_NSPACE, 1);
    rc = PMIx_Get_nb(&p, GET_UT_ABSENT_B, &immediate, 1, arrival_cb, (void *) &b_done);
    if (PMIX_SUCCESS != rc) {
        fprintf(stdout, "  FAIL: client: ranked request refused (%s)\n",
                PMIx_Error_string(rc));
        PMIX_INFO_DESTRUCT(&immediate);
        return 1;
    }
    PMIX_INFO_DESTRUCT(&immediate);

    /* both are answered in a single round trip apiece; ten seconds is a
     * timeout, not a settling time */
    for (n = 0; n < 200 && 2 > narrived; n++) {
        usleep(50000);
    }
    ok = (a_done && b_done);
    fprintf(stdout, "  %s: client: a ranked get is not swallowed by an "
                    "outstanding wildcard one (wildcard=%s ranked=%s)\n",
            ok ? "PASS" : "FAIL", a_done ? "answered" : "NO ANSWER",
            b_done ? "answered" : "NO ANSWER");
    return ok ? 0 : 1;
}

static int run_client(void)
{
    pmix_proc_t me;
    pmix_status_t rc;
    int bad = 0;

    rc = PMIx_Init(&me, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "client PMIx_Init failed: %s\n", PMIx_Error_string(rc));
        return 1;
    }
    bad += check_client_scalar("client: top-level key with a NULL proc",
                               GET_UT_TOPLEVEL, GET_UT_TOPVAL);
    bad += check_client_scalar("client: job-array key with a NULL proc",
                               GET_UT_JOBARRAY, GET_UT_ARRVAL);
    bad += check_coalescing();
    rc = PMIx_Finalize(NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "client PMIx_Finalize failed: %s\n", PMIx_Error_string(rc));
        return 1;
    }
    return bad;
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

int main(int argc, char **argv)
{
    pmix_status_t rc;
    char **client_env = NULL;
    char *client_argv[3];
    pmix_proc_t p0;
    pid_t child;
    int status = 0;

    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    /* re-executed as our own client - see the comment on run_client() */
    if (1 < argc && 0 == strcmp(argv[1], "client")) {
        return run_client();
    }

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

    /* Now the same two job-level shapes from a real client, which is the
     * only way to reach a gds module other than "hash".
     *
     * exec rather than run in the fork: this process has an initialized
     * PMIx server in it, and PMIx_Init in a forked child finds the
     * one-time-init latch already set. And seed the child's environment
     * with our own *before* PMIx_server_setup_fork adds its variables -
     * handing execve only the PMIx variables strands the child without
     * the library search path libtool's wrapper script set, so it would
     * silently run against the installed PMIx rather than this one. */
    PMIX_LOAD_PROCID(&p0, GET_UT_NSPACE, 0);
    client_env = PMIx_Argv_copy(environ);
    rc = PMIx_server_setup_fork(&p0, &client_env);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "PMIx_server_setup_fork failed: %s\n", PMIx_Error_string(rc));
        report("client: job-level gets", 0);
    } else {
        child = fork();
        if (0 > child) {
            fprintf(stderr, "fork() failed\n");
            report("client: job-level gets", 0);
        } else if (0 == child) {
            client_argv[0] = argv[0];
            client_argv[1] = (char *) "client";
            client_argv[2] = NULL;
            execve(argv[0], client_argv, client_env);
            fprintf(stderr, "exec of %s failed\n", argv[0]);
            _exit(127);
        } else {
            waitpid(child, &status, 0);
            if (WIFEXITED(status)) {
                status = WEXITSTATUS(status);
            } else {
                fprintf(stderr, "the client died on a signal\n");
                status = 1;
            }
            /* the client prints its own PASS/FAIL lines; count the run */
            report("client: job-level gets and request coalescing", 0 == status);
        }
    }
    PMIx_Argv_free(client_env);

    PMIx_server_finalize();

    fprintf(stdout, "\n%d passed, %d failed\n", npass, nfail);
    return (0 == nfail) ? 0 : 1;
}
