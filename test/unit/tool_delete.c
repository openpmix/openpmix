/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Regression test: a tool acts on the key-deletion notice its server
 * sends it.
 *
 * pmix_server_notify_deleted() walks pmix_server_globals.clients and
 * sends PMIX_PTL_TAG_DATA_DELETE to every peer there that is not
 * finalized and is not earlier than 7.0.0. A tool that attached to the
 * server is in that array and reports its real version, so it is sent
 * the notice - but only PMIx_Init posts a receive for that tag.
 *
 * The shape here is the smallest one that shows it:
 *
 *   parent - a PMIx server with tool support that hands the connecting
 *            tool an identity, then announces a deletion of one of that
 *            tool's own keys;
 *   child  - a plain tool that PMIx_Put's the key into its own store
 *            before the announcement and reads it back afterwards.
 *
 * A pass is the tool no longer being able to read the key. The tool
 * also registers a handler for PMIX_ERROR, because the failure mode is
 * not silent: a message nothing is posted for is discarded by
 * pmix_ptl_base_process_msg() and raises PMIX_ERROR on the way out.
 */

#include "src/include/pmix_config.h"

#include "include/pmix.h"
#include "include/pmix_server.h"
#include "include/pmix_tool.h"
#include "src/include/pmix_globals.h"
#include "src/mca/ptl/base/base.h"
#include "src/server/pmix_server_ops.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define TOOL_NSPACE "tool-delete-test"
#define DELKEY      "tool.delete.key"

/* how the tool reports back through its exit status */
#define T_OK          0  /* the key went away, as it should */
#define T_STALE       2  /* the key is still readable - the defect */
#define T_SETUP       1  /* the test could not be set up */
#define T_NOTSTORED   3  /* the key was not readable to begin with */

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

static void tool_connected_fn(pmix_info_t *info, size_t ninfo,
                              pmix_tool_connection_cbfunc_t cbfunc, void *cbdata)
{
    pmix_proc_t proc;
    PMIX_HIDE_UNUSED_PARAMS(info, ninfo);

    PMIX_LOAD_PROCID(&proc, TOOL_NSPACE, 0);
    cbfunc(PMIX_SUCCESS, &proc, cbdata);
}

static pmix_server_module_t mymodule = {
    .tool_connected = tool_connected_fn
};

/* ---------------- the tool half ---------------- */

static volatile int nerrors = 0;

static void errh(size_t evhdlr_registration_id, pmix_status_t status,
                 const pmix_proc_t *source, pmix_info_t info[], size_t ninfo,
                 pmix_info_t results[], size_t nresults,
                 pmix_event_notification_cbfunc_fn_t cbfunc, void *cbdata)
{
    PMIX_HIDE_UNUSED_PARAMS(evhdlr_registration_id, status, source, info, ninfo,
                            results, nresults);
    ++nerrors;
    if (NULL != cbfunc) {
        cbfunc(PMIX_EVENT_ACTION_COMPLETE, NULL, 0, NULL, NULL, cbdata);
    }
}

/* Can this process still read the key out of its own store? Optional, so
 * a miss comes straight back rather than going upstream. */
static bool can_read(pmix_proc_t *p)
{
    pmix_value_t *val = NULL;
    pmix_info_t opt;
    pmix_status_t rc;
    bool flag = true;
    bool found;

    PMIX_INFO_LOAD(&opt, PMIX_OPTIONAL, &flag, PMIX_BOOL);
    rc = PMIx_Get(p, DELKEY, &opt, 1, &val);
    PMIX_INFO_DESTRUCT(&opt);
    found = (PMIX_SUCCESS == rc && NULL != val);
    if (NULL != val) {
        PMIX_VALUE_RELEASE(val);
    }
    return found;
}

static int run_tool(int urifd, int readyfd, int gofd)
{
    char uri[2048];
    ssize_t n;
    pmix_proc_t myproc;
    pmix_info_t tinfo;
    pmix_value_t val;
    pmix_status_t rc, code = PMIX_ERROR;
    char c = 'r';
    int i, ret = T_STALE;

    n = read(urifd, uri, sizeof(uri) - 1);
    if (0 >= n) {
        return T_SETUP;
    }
    uri[n] = '\0';

    PMIX_INFO_LOAD(&tinfo, PMIX_SERVER_URI, uri, PMIX_STRING);
    rc = PMIx_tool_init(&myproc, &tinfo, 1);
    PMIX_INFO_DESTRUCT(&tinfo);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "  tool: PMIx_tool_init failed: %s\n", PMIx_Error_string(rc));
        return T_SETUP;
    }
    PMIx_Register_event_handler(&code, 1, NULL, 0, errh, NULL, NULL);

    /* cache a key of our own. PMIX_INTERNAL never leaves the process, so
     * this needs no commit and no server round trip - it is exactly the
     * shape of the cached copy the notice exists to correct. */
    PMIX_VALUE_LOAD(&val, "before", PMIX_STRING);
    rc = PMIx_Put(PMIX_INTERNAL, DELKEY, &val);
    PMIX_VALUE_DESTRUCT(&val);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "  tool: PMIx_Put failed: %s\n", PMIx_Error_string(rc));
        PMIx_tool_finalize();
        return T_SETUP;
    }
    if (!can_read(&myproc)) {
        PMIx_tool_finalize();
        return T_NOTSTORED;
    }

    if (1 != write(readyfd, &c, 1)) {
        PMIx_tool_finalize();
        return T_SETUP;
    }
    if (1 != read(gofd, &c, 1)) {
        PMIx_tool_finalize();
        return T_SETUP;
    }

    /* the notice is one-way, so give it a bounded chance to arrive */
    for (i = 0; i < 100; i++) {
        if (!can_read(&myproc)) {
            ret = T_OK;
            break;
        }
        usleep(50000);
    }
    fprintf(stderr, "  tool: %d PMIX_ERROR event(s) seen\n", nerrors);
    PMIx_tool_finalize();
    return ret;
}

/* ---------------- the server half ---------------- */

int main(int argc, char **argv)
{
    pmix_status_t rc;
    pmix_info_t sinfo[2];
    pmix_proc_t toolproc;
    char *uri;
    int uripipe[2], readypipe[2], gopipe[2];
    pid_t child;
    int status = 0;
    char c = 'g';
    bool flag = true;
    PMIX_HIDE_UNUSED_PARAMS(argc, argv);

    fprintf(stdout, "\n=== tool key-deletion notice unit test ===\n\n");

    if (0 != pipe(uripipe) || 0 != pipe(readypipe) || 0 != pipe(gopipe)) {
        fprintf(stderr, "pipe() failed\n");
        return 1;
    }

    child = fork();
    if (0 > child) {
        fprintf(stderr, "fork() failed\n");
        return 1;
    }
    if (0 == child) {
        close(uripipe[1]);
        close(readypipe[0]);
        close(gopipe[1]);
        _exit(run_tool(uripipe[0], readypipe[1], gopipe[0]));
    }

    close(uripipe[0]);
    close(readypipe[1]);
    close(gopipe[0]);

    PMIX_INFO_LOAD(&sinfo[0], PMIX_SERVER_TOOL_SUPPORT, &flag, PMIX_BOOL);
    PMIX_INFO_LOAD(&sinfo[1], PMIX_SERVER_NSPACE, "tool-delete-server", PMIX_STRING);
    rc = PMIx_server_init(&mymodule, sinfo, 2);
    PMIX_INFO_DESTRUCT(&sinfo[0]);
    PMIX_INFO_DESTRUCT(&sinfo[1]);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "PMIx_server_init failed: %s\n", PMIx_Error_string(rc));
        goto reap;
    }

    uri = pmix_ptl_base.listener.uri;
    if (NULL == uri) {
        report("server published its URI", 0, "listener has no URI");
        goto done;
    }
    if ((ssize_t) strlen(uri) != write(uripipe[1], uri, strlen(uri))) {
        report("tool received the URI", 0, "short write");
        goto done;
    }
    close(uripipe[1]);
    uripipe[1] = -1;

    if (1 != read(readypipe[0], &c, 1)) {
        report("tool connected and cached the key", 0, "tool never reported ready");
        goto done;
    }
    report("tool connected and cached the key", 1, NULL);

    /* announce the deletion the way any removal on this server does */
    PMIX_LOAD_PROCID(&toolproc, TOOL_NSPACE, 0);
    pmix_server_notify_deleted(&toolproc, PMIX_DEL_INTERNAL, DELKEY, NULL);

done:
    if (0 <= gopipe[1]) {
        if (1 != write(gopipe[1], &c, 1)) {
            ;
        }
        close(gopipe[1]);
        gopipe[1] = -1;
    }
    waitpid(child, &status, 0);
    child = -1;
    if (!WIFEXITED(status)) {
        report("the tool applied the deletion", 0, "tool died");
    } else {
        switch (WEXITSTATUS(status)) {
        case T_OK:
            report("the tool applied the deletion", 1, NULL);
            break;
        case T_STALE:
            report("the tool applied the deletion", 0,
                   "the tool still answers with the deleted key");
            break;
        case T_NOTSTORED:
            report("the tool applied the deletion", 0,
                   "test setup: the key was never readable");
            break;
        default:
            report("the tool applied the deletion", 0, "test setup failed");
            break;
        }
    }
    PMIx_server_finalize();

reap:
    if (0 <= uripipe[1]) {
        close(uripipe[1]);
    }
    if (0 <= gopipe[1]) {
        close(gopipe[1]);
    }
    close(readypipe[0]);
    if (0 < child) {
        waitpid(child, &status, 0);
    }

    fprintf(stdout, "\n%d passed, %d failed\n", npass, nfail);
    return (0 == nfail) ? 0 : 1;
}
