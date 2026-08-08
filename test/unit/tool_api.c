/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Unit test for the parts of the tool API that a single, unconnected tool
 * can exercise on its own: the identity it publishes, the state of its
 * server list before anything has been attached, and the argument
 * handling of the entry points.
 *
 * The tool comes up with PMIX_TOOL_DO_NOT_CONNECT, so it self-assigns an
 * identity of "<hostname>:<pid>" rank 0, points its "server" back at
 * itself, and needs no launcher or rendezvous - which keeps this inside
 * "make check".
 *
 * Two of the properties checked here were broken.
 *
 * PMIx_Get(NULL, PMIX_PROCID) and PMIx_Get(NULL, PMIX_RANK) are answered
 * out of pmix_globals.myidval / myrankval when the caller asks for
 * PMIX_GET_POINTER_VALUES - the library hands back a pointer to those
 * pre-built values rather than allocating. pmix_rte_init creates them
 * carrying a NULL nspace and PMIX_RANK_INVALID, and it is each role's job
 * to fill them in once its identity is settled. src/client did; src/tool
 * never did, so a tool asking for its own process ID that way got
 * garbage, silently and only on that one code path.
 *
 * PMIx_tool_set_server with PMIX_WAIT_FOR_CONNECTION is required to poll
 * for the named server "up to any specified PMIX_TIMEOUT", where a zero
 * or absent timeout means never time out. The retry budget was loaded
 * from the timeout directly, so with no timeout it started at zero, the
 * first retry decremented it below zero, and the call reported failure
 * without ever having waited - and reported it as PMIX_ERR_NOT_FOUND
 * rather than the documented PMIX_ERR_TIMEOUT.
 */

#include "src/include/pmix_config.h"

#include "include/pmix.h"
#include "include/pmix_server.h"
#include "include/pmix_tool.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static int npass = 0;
static int nfail = 0;

/* an empty module is enough: nothing here calls through it, and the
 * point is whether the library accepts one at all */
static pmix_server_module_t mymodule = {0};

static void report(const char *name, int passed, const char *detail)
{
    if (passed) {
        fprintf(stdout, "  PASS: %s\n", name);
        ++npass;
    } else {
        fprintf(stdout, "  FAIL: %s (%s)\n", name, detail);
        ++nfail;
    }
}

/* A malformed directive must be refused, not dereferenced. This has to
 * run in a CHILD: PMIx_tool_init does not unwind, and a failed one leaves
 * the one-time-init latch set, so a process that has tried a bad init
 * cannot then do a good one. Exit codes: 0 = refused with
 * PMIX_ERR_BAD_PARAM, 1 = accepted, 2 = some other error. A crash shows
 * up as a signal, which is the case that used to happen. */
static int bad_directive_child(const char *key)
{
    pmix_proc_t myproc;
    pmix_info_t binfo;
    pmix_status_t rc;
    bool flag = true;

    /* the key names a string; hand it a bool instead */
    PMIX_INFO_LOAD(&binfo, key, &flag, PMIX_BOOL);
    rc = PMIx_tool_init(&myproc, &binfo, 1);
    if (PMIX_SUCCESS == rc) {
        PMIx_tool_finalize();
        return 1;
    }
    return (PMIX_ERR_BAD_PARAM == rc) ? 0 : 2;
}

static void check_bad_directive(const char *key)
{
    pid_t child;
    int status = 0;
    char name[128];

    snprintf(name, sizeof(name), "a malformed %s is refused, not dereferenced", key);
    child = fork();
    if (0 > child) {
        report(name, 0, "fork failed");
        return;
    }
    if (0 == child) {
        _exit(bad_directive_child(key));
    }
    waitpid(child, &status, 0);
    if (!WIFEXITED(status)) {
        report(name, 0, "the tool died on a signal");
    } else if (0 != WEXITSTATUS(status)) {
        report(name, 0, (1 == WEXITSTATUS(status)) ? "init accepted it"
                                                   : "init failed with the wrong status");
    } else {
        report(name, 1, NULL);
    }
}

/* elapsed seconds between two gettimeofday samples */
static double elapsed(struct timeval *start, struct timeval *end)
{
    return (double) (end->tv_sec - start->tv_sec)
           + ((double) (end->tv_usec - start->tv_usec) / 1000000.0);
}

int main(int argc, char **argv)
{
    pmix_proc_t myproc, bogus;
    pmix_proc_t *servers = NULL;
    size_t nservers = 0;
    pmix_info_t tinfo, dirs[2];
    pmix_value_t *val = NULL;
    pmix_status_t rc;
    struct timeval start, end;
    double secs;
    int itmo = 1;
    (void) argc;
    (void) argv;

    fprintf(stdout, "\n=== tool API unit test ===\n\n");

    /* keep the run hermetic no matter what the caller's environment
     * points at - with DO_NOT_CONNECT we will not dial out, but a
     * half-set identity pair in the environment is a hard error in init */
    unsetenv("PMIX_NAMESPACE");
    unsetenv("PMIX_RANK");
    unsetenv("PMIX_SERVER_URI");
    unsetenv("PMIX_SERVER_URI2");
    unsetenv("PMIX_SERVER_URI3");
    unsetenv("PMIX_SERVER_URI21");
    unsetenv("PMIX_SERVER_URI41");
    unsetenv("PMIX_SERVER_URI51");
    unsetenv("PMIX_LAUNCHER_RNDZ_URI");

    /* the entry points must refuse to run before init rather than
     * dereference the state init would have built */
    rc = PMIx_tool_set_server_module(NULL);
    report("set_server_module before init returns PMIX_ERR_INIT", PMIX_ERR_INIT == rc,
           PMIx_Error_string(rc));
    rc = PMIx_tool_get_servers(&servers, &nservers);
    report("get_servers before init returns PMIX_ERR_INIT", PMIX_ERR_INIT == rc,
           PMIx_Error_string(rc));

    /* these fork, so they must run before we bring the library up */
    check_bad_directive(PMIX_TOOL_NSPACE);
    check_bad_directive(PMIX_SERVER_TMPDIR);
    check_bad_directive(PMIX_SYSTEM_TMPDIR);

    PMIX_INFO_LOAD(&tinfo, PMIX_TOOL_DO_NOT_CONNECT, NULL, PMIX_BOOL);
    rc = PMIx_tool_init(&myproc, &tinfo, 1);
    PMIX_INFO_DESTRUCT(&tinfo);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "PMIx_tool_init failed: %s\n", PMIx_Error_string(rc));
        return 1;
    }
    report("tool init self-assigned an identity", 0 < strlen(myproc.nspace), "empty nspace");

    /* a do-not-connect tool points its server at itself and is not
     * "connected" in the sense the library reports */
    report("do-not-connect tool reports itself as not connected", !PMIx_tool_is_connected(),
           "reported as connected");

    /* ---------------------------------------------------------------
     * identity as reported through the pointer-value fast path
     * --------------------------------------------------------------- */
    PMIX_INFO_LOAD(&dirs[0], PMIX_GET_POINTER_VALUES, NULL, PMIX_BOOL);
    rc = PMIx_Get(NULL, PMIX_PROCID, dirs, 1, &val);
    if (PMIX_SUCCESS != rc || NULL == val) {
        report("PMIx_Get(PMIX_PROCID) with pointer values succeeds", 0,
               PMIx_Error_string(rc));
    } else {
        report("PMIx_Get(PMIX_PROCID) with pointer values succeeds", 1, NULL);
        report("that PROCID carries our own nspace and rank",
               PMIX_PROC == val->type && NULL != val->data.proc
                   && PMIX_CHECK_PROCID(val->data.proc, &myproc),
               "the pre-built value was never filled in by the tool");
        /* the library owns this one - it handed back a pointer to its
         * own static value, so there is nothing to release */
        val = NULL;
    }

    rc = PMIx_Get(NULL, PMIX_RANK, dirs, 1, &val);
    if (PMIX_SUCCESS != rc || NULL == val) {
        report("PMIx_Get(PMIX_RANK) with pointer values succeeds", 0, PMIx_Error_string(rc));
    } else {
        report("PMIx_Get(PMIX_RANK) with pointer values succeeds", 1, NULL);
        report("that RANK is our own rank",
               PMIX_PROC_RANK == val->type && val->data.rank == myproc.rank,
               "the pre-built value was never filled in by the tool");
        val = NULL;
    }
    PMIX_INFO_DESTRUCT(&dirs[0]);

    /* the same two queries taken the ordinary (allocating) way have
     * always worked - check them so a regression in one is told apart
     * from a regression in both */
    rc = PMIx_Get(NULL, PMIX_PROCID, NULL, 0, &val);
    report("PMIx_Get(PMIX_PROCID) without pointer values agrees",
           PMIX_SUCCESS == rc && NULL != val && PMIX_PROC == val->type
               && NULL != val->data.proc && PMIX_CHECK_PROCID(val->data.proc, &myproc),
           PMIx_Error_string(rc));
    if (NULL != val) {
        PMIX_VALUE_RELEASE(val);
        val = NULL;
    }

    /* ---------------------------------------------------------------
     * the server list of a tool that has not attached to anything
     * --------------------------------------------------------------- */
    rc = PMIx_tool_get_servers(NULL, &nservers);
    report("get_servers with a NULL array pointer is rejected", PMIX_ERR_BAD_PARAM == rc,
           PMIx_Error_string(rc));

    rc = PMIx_tool_get_servers(&servers, &nservers);
    report("get_servers on an unconnected tool reports no servers",
           PMIX_ERR_UNREACH == rc && 0 == nservers, PMIx_Error_string(rc));
    if (NULL != servers) {
        PMIX_PROC_FREE(servers, nservers);
        servers = NULL;
    }

    /* ---------------------------------------------------------------
     * set_server argument handling and wait semantics
     * --------------------------------------------------------------- */
    rc = PMIx_tool_set_server(NULL, NULL, 0);
    report("set_server(NULL) is rejected", PMIX_ERR_BAD_PARAM == rc, PMIx_Error_string(rc));

    /* switching to ourselves is always legal and needs no directives */
    rc = PMIx_tool_set_server(&myproc, NULL, 0);
    report("set_server(self) succeeds", PMIX_SUCCESS == rc, PMIx_Error_string(rc));

    /* a server we have never heard of, with no wait requested, fails
     * immediately */
    PMIX_LOAD_PROCID(&bogus, "no-such-server", 0);
    gettimeofday(&start, NULL);
    rc = PMIx_tool_set_server(&bogus, NULL, 0);
    gettimeofday(&end, NULL);
    secs = elapsed(&start, &end);
    report("set_server on an unknown server without a wait fails at once",
           PMIX_ERR_UNREACH == rc && 0.5 > secs, PMIx_Error_string(rc));

    /* asking to wait must actually wait, and must report the expiry as
     * PMIX_ERR_TIMEOUT. One second of budget is consumed in 0.25sec
     * retries, so allow generous slack on the upper bound and require
     * only that it did not return immediately */
    PMIX_INFO_LOAD(&dirs[0], PMIX_WAIT_FOR_CONNECTION, NULL, PMIX_BOOL);
    PMIX_INFO_LOAD(&dirs[1], PMIX_TIMEOUT, &itmo, PMIX_INT);
    gettimeofday(&start, NULL);
    rc = PMIx_tool_set_server(&bogus, dirs, 2);
    gettimeofday(&end, NULL);
    PMIX_INFO_DESTRUCT(&dirs[0]);
    PMIX_INFO_DESTRUCT(&dirs[1]);
    secs = elapsed(&start, &end);
    report("set_server with PMIX_WAIT_FOR_CONNECTION reports PMIX_ERR_TIMEOUT",
           PMIX_ERR_TIMEOUT == rc, PMIx_Error_string(rc));
    report("...and it waited for the timeout rather than failing at once", 0.5 <= secs,
           "returned immediately - the retry budget was never loaded");

    /* ---------------------------------------------------------------
     * disconnect of something we are not connected to
     * --------------------------------------------------------------- */
    rc = PMIx_tool_disconnect(&bogus);
    report("disconnect from an unknown server reports PMIX_ERR_NOT_FOUND",
           PMIX_ERR_NOT_FOUND == rc, PMIx_Error_string(rc));

    /* an unnamed server is a wildcard to PMIX_CHECK_NSPACE, so it would
     * otherwise drop whichever server the array held first */
    PMIX_LOAD_PROCID(&bogus, NULL, 0);
    rc = PMIx_tool_disconnect(&bogus);
    report("disconnect from an unnamed server is rejected", PMIX_ERR_BAD_PARAM == rc,
           PMIx_Error_string(rc));

    /* a tool may be handed a server module after the fact */
    rc = PMIx_tool_set_server_module(&mymodule);
    report("set_server_module accepted after init", PMIX_SUCCESS == rc, PMIx_Error_string(rc));

    rc = PMIx_tool_finalize();
    report("tool finalized cleanly", PMIX_SUCCESS == rc, PMIx_Error_string(rc));

    /* and the entry points refuse again once the library is down */
    rc = PMIx_tool_get_servers(&servers, &nservers);
    report("get_servers after finalize returns PMIX_ERR_INIT", PMIX_ERR_INIT == rc,
           PMIx_Error_string(rc));

    /* ---------------------------------------------------------------
     * a second cycle has to be able to do everything the first did.
     * The "module has been set" latch is a global that no finalize
     * resets, so a tool that cycles used to be refused its module on
     * every init after the first - while init had already zeroed the
     * table it would be called through.
     * --------------------------------------------------------------- */
    PMIX_INFO_LOAD(&tinfo, PMIX_TOOL_DO_NOT_CONNECT, NULL, PMIX_BOOL);
    rc = PMIx_tool_init(&myproc, &tinfo, 1);
    PMIX_INFO_DESTRUCT(&tinfo);
    report("second tool init succeeds", PMIX_SUCCESS == rc, PMIx_Error_string(rc));
    if (PMIX_SUCCESS == rc) {
        rc = PMIx_tool_set_server_module(&mymodule);
        report("set_server_module accepted again on the second cycle", PMIX_SUCCESS == rc,
               PMIx_Error_string(rc));
        rc = PMIx_tool_finalize();
        report("second tool finalize succeeds", PMIX_SUCCESS == rc, PMIx_Error_string(rc));
    }

    fprintf(stdout, "\n%d passed, %d failed\n", npass, nfail);
    return (0 == nfail) ? 0 : 1;
}
