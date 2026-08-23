/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Regression test for the tool's launcher-rendezvous path - the block in
 * PMIx_tool_init that runs when PMIX_LAUNCHER_RNDZ_URI is set in the
 * environment.
 *
 * That is how a debugger drives a launcher it fork/exec'd: the launcher
 * attaches to the URI the debugger handed it, makes that connection its
 * primary server, pulls whatever job info the debugger has for it,
 * registers a one-shot handler for PMIX_DEBUGGER_RELEASE, and then blocks
 * inside init until the debugger fires that event. Only then does init
 * restore the launcher's original primary server and return.
 *
 * The registration in the middle of that sequence is the interesting
 * part, and it was broken. It does not go through
 * PMIx_Register_event_handler; it fills in a pmix_rshift_caddy_t by hand,
 * thread-shifts it to pmix_internal_reg_event_hdlr, and blocks on the
 * caddy's own lock. The registration machinery acknowledges by calling
 * cd->evregcbfn(status, index, cd->cbdata) - and cd->cbdata is the caddy.
 * The tool's callback nonetheless cast that pointer to a pmix_lock_t,
 * which
 *
 *   - wrote the status over the head of the object (a pmix_lock_t leads
 *     with its status field, a pmix_object_t with its class pointer and
 *     magic id), and took a "mutex" that was really the rest of that
 *     header, and
 *   - left the caddy's real lock - the one init blocks on - untouched,
 *     so PMIx_tool_init never came back.
 *
 * src/client and src/server both got this right (they use the caddy, not
 * a lock); only src/tool did not.
 *
 * The test therefore drives the whole flow for real: the parent is a PMIx
 * server standing in for the debugger, and the child is a tool that comes
 * up with PMIX_LAUNCHER_RNDZ_URI pointing at it. A hang inside the child's
 * init - the pre-fix behavior - is caught by the child's alarm and by the
 * parent's bounded wait, and shows up as a failed child rather than a
 * stuck "make check".
 *
 * The child also asserts what the rendezvous block is supposed to have
 * left behind: the debugger is a known server, PMIX_PARENT_ID has been
 * stored, and the primary server has been restored to what it was before
 * the rendezvous.
 */

#include "src/include/pmix_config.h"

#include "include/pmix.h"
#include "include/pmix_server.h"
#include "include/pmix_tool.h"
#include "src/include/pmix_globals.h"
#include "src/mca/ptl/base/base.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/* how long the child gives the whole rendezvous before declaring a hang */
#define CHILD_ALARM_SECS 40
/* how long the parent keeps re-firing the release before giving up */
#define PARENT_TRIES     60
#define PARENT_WAIT_USEC 500000

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

/* ------------------------------------------------------------------ */
/* the "debugger" - a plain PMIx server                               */
/* ------------------------------------------------------------------ */

static void tool_connected_fn(pmix_info_t *info, size_t ninfo,
                              pmix_tool_connection_cbfunc_t cbfunc, void *cbdata)
{
    pmix_proc_t proc;
    PMIX_HIDE_UNUSED_PARAMS(info, ninfo);

    /* accept the tool under a name of our choosing - the launcher does
     * not care what it is called here, only that the connection works */
    PMIX_LOAD_PROCID(&proc, "tool-rndz-launcher", 0);
    cbfunc(PMIX_SUCCESS, &proc, cbdata);
}

static pmix_server_module_t mymodule = {
    .tool_connected = tool_connected_fn
};

/* ------------------------------------------------------------------ */
/* the "launcher" - a tool that rendezvouses back to the debugger      */
/* ------------------------------------------------------------------ */

static int run_tool(int urifd, int readyfd)
{
    char uri[2048];
    ssize_t n;
    pmix_proc_t myproc, before, rndz, *servers = NULL;
    size_t nservers = 0, i;
    pmix_info_t tinfo[3];
    pmix_value_t *val = NULL;
    pmix_rank_t trank;
    pmix_status_t rc;
    int ret = 1;
    bool found;
    char c = 'r';

    n = read(urifd, uri, sizeof(uri) - 1);
    if (0 >= n) {
        fprintf(stderr, "  tool: could not read the server URI\n");
        return 1;
    }
    uri[n] = '\0';

    /* a hang in init is the failure mode we are testing for, so make
     * sure it terminates us instead of the harness */
    alarm(CHILD_ALARM_SECS);

    /* nothing in our environment may point us at a server: we want the
     * self-assigned identity of a do-not-connect tool, so that the only
     * connection init makes is the rendezvous one */
    unsetenv("PMIX_NAMESPACE");
    unsetenv("PMIX_RANK");
    unsetenv("PMIX_SERVER_URI");
    unsetenv("PMIX_SERVER_URI2");
    unsetenv("PMIX_SERVER_URI3");
    unsetenv("PMIX_SERVER_URI21");
    unsetenv("PMIX_SERVER_URI41");
    unsetenv("PMIX_SERVER_URI51");

    /* this is what a debugger sets before fork/exec'ing its launcher */
    setenv("PMIX_LAUNCHER_RNDZ_URI", uri, 1);

    /* Come up with an identity of our own as well as DO_NOT_CONNECT.
     * That combination is deliberate: it is the one case where the
     * stand-in peer "myserver" points at is left with no name at all
     * (init only fills one in when the caller supplied none), and the
     * rendezvous block has to restore our primary server by name once the
     * debugger releases us. An empty name is a wildcard to
     * PMIX_CHECK_NSPACE, so getting this wrong leaves the debugger as our
     * primary - which the connected check after init catches. */
    PMIX_INFO_LOAD(&tinfo[0], PMIX_TOOL_DO_NOT_CONNECT, NULL, PMIX_BOOL);
    PMIX_INFO_LOAD(&tinfo[1], PMIX_TOOL_NSPACE, "tool-rndz-launcher-self", PMIX_STRING);
    trank = 0;
    PMIX_INFO_LOAD(&tinfo[2], PMIX_TOOL_RANK, &trank, PMIX_PROC_RANK);
    rc = PMIx_tool_init(&myproc, tinfo, 3);
    PMIX_INFO_DESTRUCT(&tinfo[0]);
    PMIX_INFO_DESTRUCT(&tinfo[1]);
    PMIX_INFO_DESTRUCT(&tinfo[2]);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "  tool: PMIx_tool_init failed: %s\n", PMIx_Error_string(rc));
        return 1;
    }
    /* getting here at all is the point of the test: with the broken
     * registration callback, init never returned */
    alarm(0);
    PMIX_LOAD_PROCID(&before, myproc.nspace, myproc.rank);

    /* the rendezvous block made the debugger our primary server and then
     * had to put things back; we asked not to connect to anything, so
     * "back" means back to ourselves */
    if (PMIx_tool_is_connected()) {
        fprintf(stderr, "  tool: the debugger is still our primary server - init did "
                "not restore it\n");
        goto done;
    }

    /* the rendezvous server must be on our list of known servers, and
     * it must not be us */
    rc = PMIx_tool_get_servers(&servers, &nservers);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "  tool: get_servers failed: %s\n", PMIx_Error_string(rc));
        goto done;
    }
    found = false;
    for (i = 0; i < nservers; i++) {
        if (!PMIX_CHECK_PROCID(&servers[i], &before)) {
            found = true;
            PMIX_LOAD_PROCID(&rndz, servers[i].nspace, servers[i].rank);
        }
    }
    if (!found) {
        fprintf(stderr, "  tool: rendezvous server is not on the server list\n");
        goto done;
    }

    /* the rendezvous block stores PMIX_PARENT_ID on the way through, and
     * it has to name the process we rendezvoused with. The identity is
     * reported by the attach and nowhere else; before that was captured,
     * the key was stored from a static nothing ever assigned, so it read
     * back as an empty namespace with PMIX_RANK_UNDEF - a successful get
     * answering with nobody. */
    rc = PMIx_Get(&myproc, PMIX_PARENT_ID, NULL, 0, &val);
    if (PMIX_SUCCESS != rc || NULL == val || PMIX_PROC != val->type) {
        fprintf(stderr, "  tool: PMIX_PARENT_ID was not stored: %s\n",
                PMIx_Error_string(rc));
        goto done;
    }
    if (NULL == val->data.proc || 0 == strlen(val->data.proc->nspace) ||
        !PMIX_CHECK_PROCID(val->data.proc, &rndz)) {
        fprintf(stderr, "  tool: PMIX_PARENT_ID is %s:%u, expected %s:%u\n",
                (NULL == val->data.proc) ? "(null)" : val->data.proc->nspace,
                (NULL == val->data.proc) ? 0 : val->data.proc->rank,
                rndz.nspace, rndz.rank);
        goto done;
    }

    ret = 0;

done:
    if (NULL != val) {
        PMIX_VALUE_RELEASE(val);
    }
    if (NULL != servers) {
        PMIX_PROC_FREE(servers, nservers);
    }
    /* let the parent know we made it all the way through init */
    if (0 == ret && 1 != write(readyfd, &c, 1)) {
        ret = 1;
    }
    if (PMIX_SUCCESS != PMIx_tool_finalize()) {
        fprintf(stderr, "  tool: PMIx_tool_finalize failed\n");
        ret = 1;
    }
    return ret;
}

/* ------------------------------------------------------------------ */

static void opcbfunc(pmix_status_t status, void *cbdata)
{
    PMIX_HIDE_UNUSED_PARAMS(status, cbdata);
}

/* wait up to usec for the fd to become readable. Returns 1 if it did */
static int wait_readable(int fd, long usec)
{
    fd_set rd;
    struct timeval tv;

    FD_ZERO(&rd);
    FD_SET(fd, &rd);
    tv.tv_sec = usec / 1000000;
    tv.tv_usec = usec % 1000000;
    return (0 < select(fd + 1, &rd, NULL, NULL, &tv));
}

int main(int argc, char **argv)
{
    pmix_status_t rc;
    pmix_info_t sinfo[2];
    char *uri;
    int uripipe[2], readypipe[2];
    pid_t child;
    int status = 0, i;
    bool flag = true, ready = false;
    char c;
    PMIX_HIDE_UNUSED_PARAMS(argc, argv);

    fprintf(stdout, "\n=== tool launcher-rendezvous unit test ===\n\n");

    if (0 != pipe(uripipe) || 0 != pipe(readypipe)) {
        fprintf(stderr, "pipe() failed\n");
        return 1;
    }

    /* fork before touching PMIx so neither side inherits an initialized
     * library - a forked child of an initialized server has a copy of its
     * state but none of its threads */
    child = fork();
    if (0 > child) {
        fprintf(stderr, "fork() failed\n");
        return 1;
    }
    if (0 == child) {
        close(uripipe[1]);
        close(readypipe[0]);
        _exit(run_tool(uripipe[0], readypipe[1]));
    }

    close(uripipe[0]);
    close(readypipe[1]);

    PMIX_INFO_LOAD(&sinfo[0], PMIX_SERVER_TOOL_SUPPORT, &flag, PMIX_BOOL);
    PMIX_INFO_LOAD(&sinfo[1], PMIX_SERVER_NSPACE, "tool-rndz-debugger", PMIX_STRING);
    rc = PMIx_server_init(&mymodule, sinfo, 2);
    PMIX_INFO_DESTRUCT(&sinfo[0]);
    PMIX_INFO_DESTRUCT(&sinfo[1]);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "PMIx_server_init failed: %s\n", PMIx_Error_string(rc));
        goto reap;
    }

    /* hand the launcher our contact information. Read it straight off
     * the listener - this test is white-box anyway */
    uri = pmix_ptl_base.listener.uri;
    if (NULL == uri) {
        report("debugger published its URI", 0, "listener has no URI");
        goto done;
    }
    report("debugger published its URI", 1, NULL);
    if ((ssize_t) strlen(uri) != write(uripipe[1], uri, strlen(uri))) {
        report("launcher received the URI", 0, "short write");
        goto done;
    }
    close(uripipe[1]);
    uripipe[1] = -1;

    /* Fire the release until the launcher tells us it is through init.
     * We cannot observe the moment its handler is registered from out
     * here, and firing early is harmless: an event that arrives before
     * any handler matches is cached and replayed when one registers. */
    for (i = 0; i < PARENT_TRIES; i++) {
        PMIx_Notify_event(PMIX_DEBUGGER_RELEASE, &pmix_globals.myid, PMIX_RANGE_LOCAL,
                          NULL, 0, opcbfunc, NULL);
        if (wait_readable(readypipe[0], PARENT_WAIT_USEC)) {
            if (1 == read(readypipe[0], &c, 1)) {
                ready = true;
            }
            break;
        }
    }
    report("launcher completed PMIx_tool_init after the debugger release", ready,
           "init never returned - the rendezvous registration never woke it");

done:
    PMIx_server_finalize();

reap:
    if (0 <= uripipe[1]) {
        close(uripipe[1]);
    }
    close(readypipe[0]);
    if (!ready) {
        /* it is wedged in init - do not leave it behind */
        kill(child, SIGKILL);
    }
    waitpid(child, &status, 0);
    if (!WIFEXITED(status) || 0 != WEXITSTATUS(status)) {
        report("launcher checked its rendezvous state and finalized cleanly", 0,
               "tool exited non-zero");
    } else {
        report("launcher checked its rendezvous state and finalized cleanly", 1, NULL);
    }

    fprintf(stdout, "\n%d passed, %d failed\n", npass, nfail);
    return (0 == nfail) ? 0 : 1;
}
