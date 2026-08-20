/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Regression test for what a tool does with a spawn request sent to it by
 * ANOTHER tool - the one path through src/tool/pmix_tool_ops.c, and the
 * only case in which a tool's server switchyard is ever entered.
 *
 * A tool has no clients of its own, but another tool can connect to it as
 * that tool's primary server (a debugger fork/exec'ing a launcher is the
 * motivating case). When the downstream tool sends a command the
 * receiving tool cannot service itself, server_switchyard() hands it to
 * pmix_tool_relay_op().
 *
 * The rule is: relay it if we are attached to a server, service it
 * ourselves otherwise. PMIx_Spawn has always applied that rule to a
 * launcher's own requests - see the top of pmix_client_spawn.c, which
 * sets "forkexec" when it is not connected and we are a launcher. The
 * proxied path did not: pmix_tool_relay_op answered PMIX_ERR_UNREACH,
 * the switchyard returned it (it fell through to the logic tree only on
 * PMIX_ERR_NOT_SUPPORTED), and the downstream tool got an error instead
 * of a launched job - even though the receiving launcher had pfexec open
 * and was perfectly able to run it.
 *
 * So this test builds exactly that shape, with nothing attached upstream:
 *
 *   parent - a LAUNCHER tool with PMIX_TOOL_DO_NOT_CONNECT, so it starts
 *            a listener and opens pfexec but has no server of its own.
 *            It publishes its URI and then just waits;
 *   child  - a plain tool whose PMIX_SERVER_URI is the parent, which
 *            calls PMIx_Spawn.
 *
 * The child's spawn reaches the parent as PMIX_SPAWNNB_CMD, finds no
 * server to relay to, and must be fork/exec'd by the parent. A pass is
 * the child getting PMIX_SUCCESS and a namespace back; against the
 * unfixed library it gets PMIX_ERR_UNREACH.
 */

#include "src/include/pmix_config.h"

#include "include/pmix.h"
#include "include/pmix_tool.h"
#include "src/include/pmix_globals.h"
#include "src/mca/ptl/base/base.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/* the child must not outlive a wedged parent */
#define CHILD_ALARM_SECS 60

/* something that exists on every platform this builds on and exits at
 * once - the test is about who launches it, not what it does */
#define SPAWNEE "/usr/bin/true"
#define SPAWNEE_ALT "/bin/true"

/* and something that does NOT exit, so the launcher still has a pfexec
 * child when it finalizes - see the two-app comment in run_downstream().
 * "cat" with no argument blocks forever on the stdin pipe pfexec hands
 * it, and exits on the SIGTERM the kill sequence sends. */
#define LINGERER "/bin/cat"

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

static void clear_env(void)
{
    unsetenv("PMIX_NAMESPACE");
    unsetenv("PMIX_RANK");
    unsetenv("PMIX_SERVER_URI");
    unsetenv("PMIX_SERVER_URI2");
    unsetenv("PMIX_SERVER_URI3");
    unsetenv("PMIX_SERVER_URI21");
    unsetenv("PMIX_SERVER_URI41");
    unsetenv("PMIX_SERVER_URI51");
    unsetenv("PMIX_LAUNCHER_RNDZ_URI");
}

/* ------------------------------------------------------------------ */
/* the downstream tool                                                */
/* ------------------------------------------------------------------ */

/* child exit codes, so the parent can say what went wrong */
#define D_OK        0
#define D_NOURI     1
#define D_INITFAIL  2
#define D_UNREACH   3   /* the pre-fix answer */
#define D_SPAWNFAIL 4
#define D_NONSPACE  5

static int run_downstream(int urifd)
{
    char uri[2048];
    ssize_t n;
    pmix_proc_t myproc;
    pmix_info_t tinfo[3];
    pmix_rank_t trank;
    pmix_app_t *app;
    pmix_nspace_t child;
    pmix_status_t rc;
    int ret;

    n = read(urifd, uri, sizeof(uri) - 1);
    if (0 >= n) {
        return D_NOURI;
    }
    uri[n] = '\0';

    alarm(CHILD_ALARM_SECS);
    clear_env();

    /* Bring our own identity. A tool acting as a server has no
     * "tool_connected" upcall to hand one out - that is a host module's
     * job and a tool has no host - so the PTL accepts a downstream tool
     * only when it does not need an id (see process_tool_request in
     * ptl_base_connection_hdlr.c). Asking for one is refused with
     * PMIX_ERR_NOT_SUPPORTED before any of this test's subject is
     * reached. */
    PMIX_INFO_LOAD(&tinfo[0], PMIX_SERVER_URI, uri, PMIX_STRING);
    PMIX_INFO_LOAD(&tinfo[1], PMIX_TOOL_NSPACE, "tool-relay-downstream", PMIX_STRING);
    trank = 0;
    PMIX_INFO_LOAD(&tinfo[2], PMIX_TOOL_RANK, &trank, PMIX_PROC_RANK);
    rc = PMIx_tool_init(&myproc, tinfo, 3);
    PMIX_INFO_DESTRUCT(&tinfo[0]);
    PMIX_INFO_DESTRUCT(&tinfo[1]);
    PMIX_INFO_DESTRUCT(&tinfo[2]);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "  downstream: PMIx_tool_init failed: %s\n", PMIx_Error_string(rc));
        return D_INITFAIL;
    }

    /* Two apps, deliberately.
     *
     * The first exits at once, which is all the relay itself needs. The
     * second outlives us, so that when the upstream launcher finalizes it
     * still has a child on pmix_pfexec_globals.children and therefore
     * actually runs the kill sequence - kill_proc, SIGCONT, SIGTERM,
     * SIGKILL, kill_finish.
     *
     * That path is where a child is taken off the children list while a
     * completion for it may still be queued behind, and it used to
     * release the child twice (see child_delist() in pmix_pfexec.c). With
     * only the fast-exiting app the launcher usually had nothing left to
     * kill by the time it finalized, so the path ran in roughly one run
     * in fifteen and the double free surfaced as a rare abort in this
     * test rather than as a reproducible failure. */
    PMIX_APP_CREATE(app, 2);
    app[0].cmd = strdup((0 == access(SPAWNEE, X_OK)) ? SPAWNEE : SPAWNEE_ALT);
    app[0].argv = (char **) malloc(2 * sizeof(char *));
    app[0].argv[0] = strdup(app[0].cmd);
    app[0].argv[1] = NULL;
    app[0].maxprocs = 1;
    app[1].cmd = strdup(LINGERER);
    app[1].argv = (char **) malloc(2 * sizeof(char *));
    app[1].argv[0] = strdup(app[1].cmd);
    app[1].argv[1] = NULL;
    app[1].maxprocs = 1;

    memset(child, 0, sizeof(child));
    rc = PMIx_Spawn(NULL, 0, app, 2, child);
    PMIX_APP_FREE(app, 2);

    if (PMIX_ERR_UNREACH == rc) {
        /* this is the defect: our upstream tool has no server of its own,
         * so it refused rather than launching for us */
        fprintf(stderr, "  downstream: spawn refused with PMIX_ERR_UNREACH - the "
                "upstream launcher did not fall back to fork/exec\n");
        ret = D_UNREACH;
    } else if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "  downstream: PMIx_Spawn failed: %s\n", PMIx_Error_string(rc));
        ret = D_SPAWNFAIL;
    } else if (0 == strlen(child)) {
        fprintf(stderr, "  downstream: spawn succeeded but returned no namespace\n");
        ret = D_NONSPACE;
    } else {
        fprintf(stdout, "  downstream: spawn succeeded, job is %s\n", child);
        ret = D_OK;
    }

    PMIx_tool_finalize();
    return ret;
}

/* ------------------------------------------------------------------ */

static const char *why(int code)
{
    switch (code) {
    case D_NOURI:     return "never received the upstream URI";
    case D_INITFAIL:  return "could not connect to the upstream tool";
    case D_UNREACH:   return "spawn refused with PMIX_ERR_UNREACH - no fork/exec fallback";
    case D_SPAWNFAIL: return "spawn failed";
    case D_NONSPACE:  return "spawn returned no namespace";
    default:          return "tool died on a signal";
    }
}

int main(int argc, char **argv)
{
    pmix_proc_t myproc;
    pmix_info_t tinfo[2];
    pmix_status_t rc;
    char *uri;
    int uripipe[2];
    pid_t child;
    int status = 0;
    PMIX_HIDE_UNUSED_PARAMS(argc, argv);

    fprintf(stdout, "\n=== tool-to-tool spawn relay unit test ===\n\n");

    if (0 != pipe(uripipe)) {
        fprintf(stderr, "pipe() failed\n");
        return 1;
    }

    /* fork before touching PMIx so neither side inherits an initialized
     * library */
    child = fork();
    if (0 > child) {
        fprintf(stderr, "fork() failed\n");
        return 1;
    }
    if (0 == child) {
        close(uripipe[1]);
        _exit(run_downstream(uripipe[0]));
    }
    close(uripipe[0]);

    clear_env();

    /* A LAUNCHER, so we start a listener and open pfexec, and
     * DO_NOT_CONNECT so we have no server of our own - which is what
     * makes the downstream tool's spawn unrelayable. */
    PMIX_INFO_LOAD(&tinfo[0], PMIX_LAUNCHER, NULL, PMIX_BOOL);
    PMIX_INFO_LOAD(&tinfo[1], PMIX_TOOL_DO_NOT_CONNECT, NULL, PMIX_BOOL);
    rc = PMIx_tool_init(&myproc, tinfo, 2);
    PMIX_INFO_DESTRUCT(&tinfo[0]);
    PMIX_INFO_DESTRUCT(&tinfo[1]);
    if (PMIX_SUCCESS != rc) {
        report("upstream launcher initialized", 0, PMIx_Error_string(rc));
        goto reap;
    }
    report("upstream launcher initialized", 1, NULL);
    report("upstream launcher has no server of its own", !PMIx_tool_is_connected(),
           "it is connected to something - the relay would be taken instead");

    /* hand the downstream tool our contact information - read straight
     * off the listener, as this test is white-box anyway */
    uri = pmix_ptl_base.listener.uri;
    if (NULL == uri) {
        report("upstream launcher published its URI", 0,
               "no listener - a launcher must start one");
        goto done;
    }
    report("upstream launcher published its URI", 1, NULL);
    if ((ssize_t) strlen(uri) != write(uripipe[1], uri, strlen(uri))) {
        report("downstream tool received the URI", 0, "short write");
        goto done;
    }
    close(uripipe[1]);
    uripipe[1] = -1;

done:
    /* stay up until the downstream tool is finished: our progress thread
     * is what services its connection and its spawn */
    waitpid(child, &status, 0);
    child = -1;
    if (!WIFEXITED(status)) {
        report("downstream tool's spawn was serviced", 0, "tool died on a signal");
    } else if (D_OK != WEXITSTATUS(status)) {
        report("downstream tool's spawn was serviced", 0, why(WEXITSTATUS(status)));
    } else {
        report("downstream tool's spawn was serviced", 1, NULL);
    }

    rc = PMIx_tool_finalize();
    report("upstream launcher finalized cleanly", PMIX_SUCCESS == rc, PMIx_Error_string(rc));

reap:
    if (0 <= uripipe[1]) {
        close(uripipe[1]);
    }
    if (0 < child) {
        kill(child, SIGKILL);
        waitpid(child, &status, 0);
    }

    fprintf(stdout, "\n%d passed, %d failed\n", npass, nfail);
    return (0 == nfail) ? 0 : 1;
}
