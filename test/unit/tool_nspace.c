/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Regression test: a connecting tool leaves exactly one namespace object
 * on the server's global namespace list.
 *
 * When a self-started tool connects, the server builds a
 * pmix_namespace_t for it - but it cannot put that object on
 * pmix_globals.nspaces right away, because for a tool asking to be given
 * an identity the namespace does not have a name until the host's
 * tool_connected upcall returns. The PTL therefore defers the append to
 * process_cbfunc.
 *
 * Two things can go wrong there, and both did.
 *
 * The append can be skipped entirely, which is what used to happen for
 * every tool the server had not already registered as a client. The
 * object was then reachable only through the peer, the reference held on
 * behalf of the list was stranded, and any later find-or-create for that
 * name (gds/hash does one) built a *second* namespace object - so the
 * server held two objects for one name and half the library resolved the
 * tool through the one with an empty rank list.
 *
 * Or the append can happen blindly, when the host has registered a
 * namespace of its own under that name during the very upcall that named
 * it - producing the same duplication from the other direction.
 *
 * This test drives a real tool connection and then asserts the property
 * that rules both out: after the tool is connected, exactly one entry on
 * pmix_globals.nspaces carries its name, and that entry is the one the
 * tool's peer is using.
 *
 * The fork happens before any PMIx call so neither side inherits an
 * initialized library, and the two pipes keep the check inside the
 * window where the tool is connected - so nothing here races the
 * teardown that a departing tool triggers on the progress thread.
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
#include <unistd.h>

#define TOOL_NSPACE "tool-nspace-test"

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

/* Hand the connecting tool an identity, which is what makes this the
 * "namespace has no name until the upcall returns" case */
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

/* the tool half - runs in the child, which has made no PMIx call yet */
static int run_tool(int urifd, int readyfd, int gofd)
{
    char uri[2048];
    ssize_t n;
    pmix_proc_t myproc;
    pmix_info_t tinfo;
    pmix_status_t rc;
    char c = 'r';

    n = read(urifd, uri, sizeof(uri) - 1);
    if (0 >= n) {
        return 1;
    }
    uri[n] = '\0';

    PMIX_INFO_LOAD(&tinfo, PMIX_SERVER_URI, uri, PMIX_STRING);
    rc = PMIx_tool_init(&myproc, &tinfo, 1);
    PMIX_INFO_DESTRUCT(&tinfo);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "  tool: PMIx_tool_init failed: %s\n", PMIx_Error_string(rc));
        return 1;
    }

    /* tell the server we are up, and stay connected until it has looked */
    if (1 != write(readyfd, &c, 1)) {
        PMIx_tool_finalize();
        return 1;
    }
    if (1 != read(gofd, &c, 1)) {
        PMIx_tool_finalize();
        return 1;
    }

    PMIx_tool_finalize();
    return 0;
}

/* count the entries on the server's global list carrying this name, and
 * report whether the peer's namespace is one of them */
static size_t count_nspace(const char *nspace, bool *is_peers)
{
    pmix_namespace_t *ns;
    pmix_peer_t *pr;
    size_t count = 0;
    int i;

    *is_peers = false;
    PMIX_LIST_FOREACH (ns, &pmix_globals.nspaces, pmix_namespace_t) {
        if (NULL != ns->nspace && 0 == strcmp(ns->nspace, nspace)) {
            ++count;
            for (i = 0; i < pmix_server_globals.clients.size; i++) {
                pr = (pmix_peer_t *) pmix_pointer_array_get_item(&pmix_server_globals.clients, i);
                if (NULL != pr && pr->nptr == ns) {
                    *is_peers = true;
                }
            }
        }
    }
    return count;
}

int main(int argc, char **argv)
{
    pmix_status_t rc;
    pmix_info_t sinfo[2];
    char *uri;
    int uripipe[2], readypipe[2], gopipe[2];
    pid_t child;
    int status = 0;
    char c = 'g';
    size_t count;
    bool is_peers = false;
    bool flag = true;
    PMIX_HIDE_UNUSED_PARAMS(argc, argv);

    fprintf(stdout, "\n=== tool namespace consolidation unit test ===\n\n");

    if (0 != pipe(uripipe) || 0 != pipe(readypipe) || 0 != pipe(gopipe)) {
        fprintf(stderr, "pipe() failed\n");
        return 1;
    }

    /* fork before touching PMIx so neither side inherits an initialized
     * library - a forked child of an initialized server has a copy of
     * its state but none of its threads */
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
    PMIX_INFO_LOAD(&sinfo[1], PMIX_SERVER_NSPACE, "tool-nspace-server", PMIX_STRING);
    rc = PMIx_server_init(&mymodule, sinfo, 2);
    PMIX_INFO_DESTRUCT(&sinfo[0]);
    PMIX_INFO_DESTRUCT(&sinfo[1]);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "PMIx_server_init failed: %s\n", PMIx_Error_string(rc));
        goto reap;
    }

    /* Hand the tool our contact information. Read it straight off the
     * listener rather than through PMIx_Get: the URI is stored against
     * our own rank under PMIX_INTERNAL scope, which is not what a
     * wildcard get resolves, and this test is white-box anyway. */
    uri = pmix_ptl_base.listener.uri;
    if (NULL == uri) {
        report("server published its URI", 0, "listener has no URI");
        goto done;
    }
    report("server published its URI", 1, NULL);
    if ((ssize_t) strlen(uri) != write(uripipe[1], uri, strlen(uri))) {
        report("tool received the URI", 0, "short write");
        goto done;
    }
    close(uripipe[1]);
    uripipe[1] = -1;

    /* wait for the tool to finish connecting. Its init does not return
     * until we have sent it the final status, which happens after the
     * namespace has been placed, so there is nothing left in flight */
    if (1 != read(readypipe[0], &c, 1)) {
        report("tool connected", 0, "tool never reported ready");
        goto done;
    }
    report("tool connected", 1, NULL);

    count = count_nspace(TOOL_NSPACE, &is_peers);
    report("tool namespace is on the server's global list", 1 <= count,
           "not found - the peer is the only thing holding it");
    report("only one list entry carries that name", 1 == count,
           "the list itself holds duplicates");
    /* This is the assertion that catches the defect, and it is worth
     * knowing why the two above do not. Before the fix, the count was
     * already 1: a find-or-create elsewhere in the library (gds/hash
     * does one while storing the tool's job info) had built its own
     * object under this name and put *that* on the list, while the
     * peer went on using the one the PTL built and never appended. So
     * the list looked healthy from the outside and the server was
     * nonetheless resolving this namespace two different ways -
     * whichever half reached the PTL's object saw the tool's rank list,
     * and whichever half reached the GDS's saw an empty one. */
    report("the listed object is the one the tool's peer uses", is_peers,
           "the peer resolves through a different object than the rest "
           "of the server");

done:
    /* release the tool and let it finalize cleanly */
    if (0 <= gopipe[1]) {
        if (1 != write(gopipe[1], &c, 1)) {
            /* the tool is gone already - nothing to do */
            ;
        }
        close(gopipe[1]);
        gopipe[1] = -1;
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
    waitpid(child, &status, 0);
    if (!WIFEXITED(status) || 0 != WEXITSTATUS(status)) {
        report("tool completed its own init/finalize", 0, "tool exited non-zero");
    } else {
        report("tool completed its own init/finalize", 1, NULL);
    }

    fprintf(stdout, "\n%d passed, %d failed\n", npass, nfail);
    return (0 == nfail) ? 0 : 1;
}
