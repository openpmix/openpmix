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
 * Both entry points fan out to pnet and pgpu, whose active-module lists
 * a tool never opens. That used to be fatal: PMIX_LIST_STATIC_INIT left
 * the sentinel's next and prev NULL, so the walk dereferenced NULL
 * instead of finding the list empty, and a tool calling either one was
 * answered PMIX_SUCCESS and then died on the progress thread. The macro
 * now points a static sentinel at itself, so an unconstructed list is
 * an empty list.
 *
 * The tool cases therefore assert survival and nothing else. What the
 * library answers a role with no server behind it is not a contract -
 * calling these from a tool is a programming error PMIx does not try to
 * diagnose - so pinning a status here would pin something nobody
 * promised. They run in a forked child because the crash they guard
 * against lands after the call has already returned, so the parent
 * would die rather than report; a regression shows up as a signal.
 *
 * Test cases:
 *
 *   collect_inventory from a tool process   -> no crash
 *   deliver_inventory from a tool process   -> no crash
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

/* Either may or may not be called - what the entry point does for a role
 * that has no server library behind it is not a contract. They exist so
 * the request has somewhere to land; the child's verdict is whether it
 * is still alive at the end. */
static void tolerant_info(pmix_status_t status, pmix_info_t *info, size_t ninfo, void *cbdata,
                          pmix_release_cbfunc_t relfn, void *relcbdata)
{
    PMIX_HIDE_UNUSED_PARAMS(status, info, ninfo, cbdata);
    if (NULL != relfn) {
        relfn(relcbdata);
    }
}

static void tolerant_op(pmix_status_t status, void *cbdata)
{
    PMIX_HIDE_UNUSED_PARAMS(status, cbdata);
}

/* Runs in a child. Bring up a tool - which never stands up the server
 * library - and call one of the two entry points. A tool calling these
 * is a programming error the library does not try to diagnose, so what
 * comes back is not asserted; exiting 0 means the process was still
 * alive afterwards, which is the whole contract. Against a library whose
 * lists are only statically initialized, the shifted handler took the
 * process down after the call had already returned. */
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
        rc = PMIx_server_collect_inventory(NULL, 0, tolerant_info, NULL);
    } else {
        rc = PMIx_server_deliver_inventory(NULL, 0, NULL, 0, tolerant_op, NULL);
    }
    /* give the progress thread a turn: the shifted handler is what used
     * to crash, and it has not run yet */
    (void) PMIx_Get(&myproc, PMIX_UNIV_SIZE, NULL, 0, NULL);
    PMIx_tool_finalize();
    (void) rc;
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
    check_tool_refusal("collect_inventory from a tool is not fatal", 0);
    check_tool_refusal("deliver_inventory from a tool is not fatal", 1);

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
