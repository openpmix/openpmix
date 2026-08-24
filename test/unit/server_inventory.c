/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * White-box unit tests for src/server/pmix_server_inventory.c.
 *
 * The case that matters is who is allowed to call these two entry points.
 * Both fan out to pmix_pnet and pmix_pgpu, whose active-module lists are
 * only *statically* initialized until PMIx_server_init opens those two
 * frameworks - and PMIX_LIST_STATIC_INIT leaves the list sentinel's next
 * pointer NULL rather than pointing at itself, so the PMIX_LIST_FOREACH
 * inside each base fan-out dereferences NULL rather than finding the list
 * empty.
 *
 * Both entry points guarded on pmix_globals.initialized, which a client
 * or a tool sets just as readily as a server does. So a tool process that
 * called either one was answered PMIX_SUCCESS and then took a SIGSEGV on
 * the progress thread a moment later - where the documented answer is
 * PMIX_ERR_INIT, "the PMIx server library has not been initialized".
 * The process type is not a usable stand-in for that question either:
 * PMIX_PROC_LAUNCHER carries PMIX_PROC_SERVER, so a launcher that has
 * called only PMIx_tool_init reads as a server. The screen is
 * pmix_server_globals.initialized, set by PMIx_server_init and cleared by
 * server_teardown().
 *
 * The refusal cases run in a forked child: against an unfixed library the
 * crash lands on the progress thread after the call has already returned,
 * so the parent would die rather than report. A child that survives and
 * reports PMIX_ERR_INIT exits 0; one that is answered anything else exits
 * 1; a crash shows up as a signal, which is what used to happen.
 *
 * Test cases:
 *
 *   collect_inventory from a tool process   -> PMIX_ERR_INIT, no crash
 *   deliver_inventory from a tool process   -> PMIX_ERR_INIT, no crash
 *   collect_inventory in a server           -> accepted, callback fires
 *   the callback's answer                   -> success, empty inventory
 *   deliver_inventory (blocking) in a server -> PMIX_OPERATION_SUCCEEDED
 *   deliver_inventory (non-blocking)         -> accepted, callback fires
 */

#include "src/include/pmix_config.h"

#include "include/pmix.h"
#include "include/pmix_server.h"
#include "include/pmix_tool.h"

#include "src/include/pmix_globals.h"
#include "src/server/pmix_server_ops.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static int npass = 0;
static int nfail = 0;

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

/* what the collection callback saw */
static pmix_lock_t clock_;
static pmix_status_t cstatus = PMIX_ERR_NOT_SUPPORTED;
static size_t cninfo = SIZE_MAX;
static pmix_info_t *cinfo = (pmix_info_t *) 0x1;

static void infocbfunc(pmix_status_t status, pmix_info_t *info, size_t ninfo, void *cbdata,
                       pmix_release_cbfunc_t relfn, void *relcbdata)
{
    PMIX_HIDE_UNUSED_PARAMS(cbdata);

    cstatus = status;
    cinfo = info;
    cninfo = ninfo;
    /* the array belongs to the library - hand it back the way the man
     * page requires rather than freeing it ourselves */
    if (NULL != relfn) {
        relfn(relcbdata);
    }
    PMIX_WAKEUP_THREAD(&clock_);
}

static pmix_lock_t olock;
static pmix_status_t ostatus = PMIX_ERR_NOT_SUPPORTED;

static void opcbfunc(pmix_status_t status, void *cbdata)
{
    PMIX_HIDE_UNUSED_PARAMS(cbdata);

    ostatus = status;
    PMIX_WAKEUP_THREAD(&olock);
}

/* Neither of these may be called: the entry point must refuse before it
 * ever shifts. A child that sees one exits 2. */
static void must_not_fire_info(pmix_status_t status, pmix_info_t *info, size_t ninfo, void *cbdata,
                               pmix_release_cbfunc_t relfn, void *relcbdata)
{
    PMIX_HIDE_UNUSED_PARAMS(status, info, ninfo, cbdata, relfn, relcbdata);
    _exit(2);
}

static void must_not_fire_op(pmix_status_t status, void *cbdata)
{
    PMIX_HIDE_UNUSED_PARAMS(status, cbdata);
    _exit(2);
}

/* Runs in a child. Bring up a tool - which sets pmix_globals.initialized
 * without ever standing up the server library - and call one of the two
 * entry points. Exits 0 when refused with PMIX_ERR_INIT. */
static int tool_child(int which)
{
    pmix_proc_t myproc;
    pmix_info_t tinfo;
    pmix_status_t rc;

    PMIX_INFO_LOAD(&tinfo, PMIX_TOOL_DO_NOT_CONNECT, NULL, PMIX_BOOL);
    rc = PMIx_tool_init(&myproc, &tinfo, 1);
    PMIX_INFO_DESTRUCT(&tinfo);
    if (PMIX_SUCCESS != rc) {
        return 3;
    }

    if (0 == which) {
        rc = PMIx_server_collect_inventory(NULL, 0, must_not_fire_info, NULL);
    } else {
        rc = PMIx_server_deliver_inventory(NULL, 0, NULL, 0, must_not_fire_op, NULL);
    }
    if (PMIX_ERR_INIT != rc) {
        PMIx_tool_finalize();
        return 1;
    }
    /* give the progress thread a turn: against an unfixed library the
     * shifted handler is what crashes, and it has not run yet */
    (void) PMIx_Get(&myproc, PMIX_UNIV_SIZE, NULL, 0, NULL);
    PMIx_tool_finalize();
    return 0;
}

static void check_tool_refusal(const char *name, int which)
{
    pid_t child;
    int status = 0;

    fflush(stdout);
    child = fork();
    if (0 == child) {
        _exit(tool_child(which));
    }
    if (0 > child) {
        report(name, 0, "fork failed");
        return;
    }
    if (0 > waitpid(child, &status, 0)) {
        report(name, 0, "waitpid failed");
        return;
    }
    if (!WIFEXITED(status)) {
        report(name, 0, "child died on a signal");
        return;
    }
    switch (WEXITSTATUS(status)) {
    case 0:
        report(name, 1, NULL);
        break;
    case 1:
        report(name, 0, "not refused with PMIX_ERR_INIT");
        break;
    case 2:
        report(name, 0, "the callback fired");
        break;
    default:
        report(name, 0, "tool init failed");
        break;
    }
}

int main(int argc, char **argv)
{
    static pmix_server_module_t mymodule = {0};
    pmix_status_t rc;

    PMIX_HIDE_UNUSED_PARAMS(argc, argv);

    fprintf(stdout, "server_inventory: inventory collection unit tests\n");

    /* the refusal cases fork, so they run before the library is up */
    check_tool_refusal("collect_inventory from a tool is refused, not fatal", 0);
    check_tool_refusal("deliver_inventory from a tool is refused, not fatal", 1);

    rc = PMIx_server_init(&mymodule, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "PMIx_server_init failed: %s\n", PMIx_Error_string(rc));
        return 1;
    }

    /* --- collection in a real server --------------------------------- */
    PMIX_CONSTRUCT_LOCK(&clock_);
    rc = PMIx_server_collect_inventory(NULL, 0, infocbfunc, NULL);
    report("collect_inventory accepted in a server", PMIX_SUCCESS == rc, PMIx_Error_string(rc));
    if (PMIX_SUCCESS == rc) {
        PMIX_WAIT_THREAD(&clock_);
        /* no in-tree pnet/pgpu component contributes anything, so the
         * documented "nothing collected" answer is the one to expect:
         * PMIX_ERR_EMPTY out of PMIx_Info_list_convert mapped to success
         * with a NULL array */
        report("collection reports success", PMIX_SUCCESS == cstatus, PMIx_Error_string(cstatus));
        report("an empty inventory is NULL/0", NULL == cinfo && 0 == cninfo, "array not empty");
    }
    PMIX_DESTRUCT_LOCK(&clock_);

    /* --- delivery, both forms ---------------------------------------- */
    rc = PMIx_server_deliver_inventory(NULL, 0, NULL, 0, NULL, NULL);
    report("deliver_inventory blocking form completes", PMIX_OPERATION_SUCCEEDED == rc,
           PMIx_Error_string(rc));

    PMIX_CONSTRUCT_LOCK(&olock);
    rc = PMIx_server_deliver_inventory(NULL, 0, NULL, 0, opcbfunc, NULL);
    report("deliver_inventory non-blocking form accepted", PMIX_SUCCESS == rc,
           PMIx_Error_string(rc));
    if (PMIX_SUCCESS == rc) {
        PMIX_WAIT_THREAD(&olock);
        report("delivery reports success", PMIX_SUCCESS == ostatus, PMIx_Error_string(ostatus));
    }
    PMIX_DESTRUCT_LOCK(&olock);

    PMIx_server_finalize();

    fprintf(stdout, "server_inventory: %d passed, %d failed\n", npass, nfail);
    return (0 == nfail) ? 0 : 1;
}
