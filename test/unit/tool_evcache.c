/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * End-to-end coverage for the affected-process restriction on events
 * delivered to a tool.
 *
 * A tool receives events through pmix_tool_notify_recv (src/tool), which
 * unpacks them and hands them to the local event machinery. This drives
 * that path with two handlers that ask about different processes, and
 * asserts that each sees only what it asked for: handler B, registered
 * for process Z, must not be given the event about process Y that was
 * already in flight, and must be given the one about Z.
 *
 * The ordering is what makes the first half an assertion rather than a
 * race:
 *
 *   1. the tool registers handler A, restricted to process X. A PMIx
 *      server does not forward a tool codes it has registered no
 *      interest in, so without A the test code never leaves the server;
 *   2. the server notifies the code about process Y, and waits for its
 *      own completion callback so the message is really on the wire;
 *   3. the tool makes a blocking round trip to the server. The
 *      notification and the reply travel the same socket in order, so by
 *      the time the reply is in hand the notification has been dealt
 *      with. No sleep, no polling;
 *   4. the tool registers handler B, restricted to process Z, and B must
 *      not fire;
 *   5. the server notifies the code about Z, and B must fire - so the
 *      test cannot pass by never delivering anything at all.
 *
 * WHERE THE PARKED EVENT COMES FROM, since this file used to say the
 * opposite. A tool hands an event its server forwarded straight to
 * pmix_server_notify_client_of_event, which caches it in the
 * notification hotel, fans it out to any tools connected to us, and
 * walks our handlers against a chain of its own. So the caching this
 * test relies on is that one, not the copy of the parking code that
 * used to sit in the tool's own completion callback - which was
 * unreachable, because the chain the tool builds never visits a handler
 * list and its completion is therefore never told that nothing matched.
 * That dead copy is gone; the client keeps the live one
 * (pmix_event_notify_complete). See openpmix#4101, and
 * test/unit/event_forward.c for the client half.
 */

#include "src/include/pmix_config.h"

#include "include/pmix.h"
#include "include/pmix_server.h"
#include "include/pmix_tool.h"
#include "src/include/pmix_globals.h"
#include "src/mca/ptl/base/base.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/* a code with no meaning to the library, so nothing else reacts to it */
#define TEST_CODE   (PMIX_EXTERNAL_ERR_BASE - 42)
#define TOOL_NSPACE "tool-evcache-tool"
/* three processes that are not the tool and not each other */
#define NS_X "tool-evcache-x"
#define NS_Y "tool-evcache-y"
#define NS_Z "tool-evcache-z"

/* how long the tool waits to be sure a handler did NOT fire */
#define QUIET_SECS  2
/* and how long it will wait for the one that must */
#define LOUD_SECS   20

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
/* the server                                                         */
/* ------------------------------------------------------------------ */

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

/* ------------------------------------------------------------------ */
/* the tool                                                           */
/* ------------------------------------------------------------------ */

static volatile int nfired_a = 0;
static volatile int nfired_b = 0;

/* handler A exists only to make the server forward the code to us */
static void hdlr_a(size_t evhdlr_registration_id, pmix_status_t status,
                   const pmix_proc_t *source, pmix_info_t info[], size_t ninfo,
                   pmix_info_t results[], size_t nresults,
                   pmix_event_notification_cbfunc_fn_t cbfunc, void *cbdata)
{
    PMIX_HIDE_UNUSED_PARAMS(evhdlr_registration_id, status, source, info, ninfo, results,
                            nresults);

    ++nfired_a;
    if (NULL != cbfunc) {
        cbfunc(PMIX_EVENT_ACTION_COMPLETE, NULL, 0, NULL, NULL, cbdata);
    }
}

/* handler B is the one under test */
static void hdlr_b(size_t evhdlr_registration_id, pmix_status_t status,
                   const pmix_proc_t *source, pmix_info_t info[], size_t ninfo,
                   pmix_info_t results[], size_t nresults,
                   pmix_event_notification_cbfunc_fn_t cbfunc, void *cbdata)
{
    PMIX_HIDE_UNUSED_PARAMS(evhdlr_registration_id, status, source, info, ninfo, results,
                            nresults);

    ++nfired_b;
    if (NULL != cbfunc) {
        cbfunc(PMIX_EVENT_ACTION_COMPLETE, NULL, 0, NULL, NULL, cbdata);
    }
}

static void reg_cbfunc(pmix_status_t status, size_t refid, void *cbdata)
{
    pmix_status_t *sp = (pmix_status_t *) cbdata;
    PMIX_HIDE_UNUSED_PARAMS(refid);

    *sp = status;
}

/* wait up to secs for *counter to reach want; return what it reached */
static int wait_for_fires(volatile int *counter, int want, int secs)
{
    int i;

    for (i = 0; i < secs * 10; i++) {
        if (*counter >= want) {
            break;
        }
        usleep(100000);
    }
    return *counter;
}

/* register one handler for TEST_CODE restricted to "affected", and block
 * until the registration has been acknowledged */
static pmix_status_t register_for(pmix_proc_t *affected, pmix_notification_fn_t fn)
{
    pmix_info_t rinfo;
    pmix_status_t code = TEST_CODE;
    pmix_status_t regstatus = PMIX_ERR_WOULD_BLOCK;
    int i;

    PMIX_INFO_LOAD(&rinfo, PMIX_EVENT_AFFECTED_PROC, affected, PMIX_PROC);
    PMIx_Register_event_handler(&code, 1, &rinfo, 1, fn, reg_cbfunc, (void *) &regstatus);
    /* the acknowledgement arrives on the progress thread. Note the info
     * array stays alive across the wait: the library does not copy it */
    for (i = 0; i < 400 && PMIX_ERR_WOULD_BLOCK == regstatus; i++) {
        usleep(50000);
    }
    PMIX_INFO_DESTRUCT(&rinfo);
    return regstatus;
}

/* A blocking round trip to the server. Its only job is ordering: the
 * notification the server sent before this went out travels the same
 * socket, so it has been received and processed by the time we return.
 *
 * A query rather than a PMIx_Get: a get for a key nobody has posted does
 * not come back at all - it parks until the key appears, which is the
 * documented behaviour and would simply hang here. Our test server
 * implements no query interface, so this returns PMIX_ERR_NOT_SUPPORTED
 * as soon as the server has looked at it, which is exactly the round
 * trip we want. */
static void round_trip(void)
{
    pmix_query_t *query;
    pmix_info_t *results = NULL;
    size_t nresults = 0;
    pmix_status_t rc;

    PMIX_QUERY_CREATE(query, 1);
    PMIX_ARGV_APPEND(rc, query[0].keys, PMIX_QUERY_NAMESPACES);
    if (PMIX_SUCCESS != rc) {
        PMIX_QUERY_FREE(query, 1);
        return;
    }
    (void) PMIx_Query_info(query, 1, &results, &nresults);
    PMIX_QUERY_FREE(query, 1);
    if (NULL != results) {
        PMIX_INFO_FREE(results, nresults);
    }
}

static int run_tool(int urifd, int readyfd, int gofd)
{
    char uri[2048];
    ssize_t n;
    pmix_proc_t myproc, procx, procz;
    pmix_info_t tinfo;
    pmix_status_t rc;
    char c = 'r';
    int ret = 1, fires;

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

    /* Handler A, restricted to X. Its job is to make the server forward
     * TEST_CODE to us at all - a server does not send a tool codes it
     * has registered no interest in, and an event that never arrives is
     * never cached. */
    PMIX_LOAD_PROCID(&procx, NS_X, 0);
    rc = register_for(&procx, hdlr_a);
    if (0 > rc) {
        fprintf(stderr, "  tool: could not register handler A: %s\n", PMIx_Error_string(rc));
        goto done;
    }

    /* tell the server we are ready, and wait for it to say the
     * notification about Y has gone out */
    if (1 != write(readyfd, &c, 1) || 1 != read(gofd, &c, 1)) {
        goto done;
    }

    /* order ourselves behind that notification: when this returns, the
     * event about Y has been received, matched against handler A (which
     * wants X), found wanting, and parked */
    round_trip();

    if (0 != nfired_a) {
        fprintf(stderr, "  tool: handler A fired for an event about a process it did "
                "not ask about\n");
        goto done;
    }

    /* Handler B, restricted to Z. Registering it replays the cache. The
     * parked event is about Y, so B must not see it. */
    PMIX_LOAD_PROCID(&procz, NS_Z, 0);
    rc = register_for(&procz, hdlr_b);
    if (0 > rc) {
        fprintf(stderr, "  tool: could not register handler B: %s\n", PMIx_Error_string(rc));
        goto done;
    }

    fires = wait_for_fires(&nfired_b, 1, QUIET_SECS);
    if (0 != fires) {
        fprintf(stderr, "  tool: the cached event about %s was replayed to a handler that "
                "asked only about %s\n", NS_Y, NS_Z);
        goto done;
    }
    fprintf(stdout, "  tool: cached event kept its affected-process restriction\n");

    /* now ask for one that IS about Z, so a library that delivers
     * nothing at all cannot pass this test */
    if (1 != write(readyfd, &c, 1)) {
        goto done;
    }
    fires = wait_for_fires(&nfired_b, 1, LOUD_SECS);
    if (1 > fires) {
        fprintf(stderr, "  tool: the event about %s never reached handler B\n", NS_Z);
        goto done;
    }
    fprintf(stdout, "  tool: the event handler B asked for was delivered\n");
    ret = 0;

done:
    PMIx_tool_finalize();
    return ret;
}

/* ------------------------------------------------------------------ */

static volatile int notified = 0;

static void opcbfunc(pmix_status_t status, void *cbdata)
{
    PMIX_HIDE_UNUSED_PARAMS(status, cbdata);
    ++notified;
}

/* send one TEST_CODE event naming "affected" as the affected process,
 * and do not return until the library says it is done with it */
static void notify_about(pmix_proc_t *affected)
{
    pmix_info_t info;
    int i, was = notified;

    PMIX_INFO_LOAD(&info, PMIX_EVENT_AFFECTED_PROC, affected, PMIX_PROC);
    PMIx_Notify_event(TEST_CODE, &pmix_globals.myid, PMIX_RANGE_LOCAL, &info, 1,
                      opcbfunc, NULL);
    /* PMIx_Notify_event is non-blocking and does NOT copy the directives -
     * the caller has to keep them alive until the callback fires. Freeing
     * this array here is a use-after-free inside the library's own
     * progress thread, and it segfaults there rather than here. */
    for (i = 0; i < 200 && notified == was; i++) {
        usleep(50000);
    }
    PMIX_INFO_DESTRUCT(&info);
}

static int wait_readable(int fd, int secs)
{
    fd_set rd;
    struct timeval tv;

    FD_ZERO(&rd);
    FD_SET(fd, &rd);
    tv.tv_sec = secs;
    tv.tv_usec = 0;
    return (0 < select(fd + 1, &rd, NULL, NULL, &tv));
}

int main(int argc, char **argv)
{
    pmix_status_t rc;
    pmix_info_t sinfo[2];
    pmix_proc_t procy, procz;
    char *uri;
    int uripipe[2], readypipe[2], gopipe[2];
    pid_t child;
    int status = 0;
    bool flag = true;
    char c = 'g';
    PMIX_HIDE_UNUSED_PARAMS(argc, argv);

    fprintf(stdout, "\n=== tool cached-event restriction unit test ===\n\n");


    if (0 != pipe(uripipe) || 0 != pipe(readypipe) || 0 != pipe(gopipe)) {
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
        close(readypipe[0]);
        close(gopipe[1]);
        _exit(run_tool(uripipe[0], readypipe[1], gopipe[0]));
    }

    close(uripipe[0]);
    close(readypipe[1]);
    close(gopipe[0]);

    PMIX_INFO_LOAD(&sinfo[0], PMIX_SERVER_TOOL_SUPPORT, &flag, PMIX_BOOL);
    PMIX_INFO_LOAD(&sinfo[1], PMIX_SERVER_NSPACE, "tool-evcache-server", PMIX_STRING);
    rc = PMIx_server_init(&mymodule, sinfo, 2);
    PMIX_INFO_DESTRUCT(&sinfo[0]);
    PMIX_INFO_DESTRUCT(&sinfo[1]);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "PMIx_server_init failed: %s\n", PMIx_Error_string(rc));
        goto reap;
    }

    /* hand the tool our contact information - read straight off the
     * listener, as this test is white-box anyway */
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

    if (!wait_readable(readypipe[0], 30) || 1 != read(readypipe[0], &c, 1)) {
        report("tool connected", 0, "tool never reported ready");
        goto done;
    }
    report("tool connected", 1, NULL);

    /* the event the tool must not replay to handler B: it is about Y,
     * and by the time B registers it has been parked */
    PMIX_LOAD_PROCID(&procy, NS_Y, 0);
    notify_about(&procy);
    if (1 != write(gopipe[1], &c, 1)) {
        report("first notification released the tool", 0, "short write");
        goto done;
    }
    close(gopipe[1]);
    gopipe[1] = -1;
    report("server notified an event about a process no handler wants", 1, NULL);

    /* the tool tells us when it is satisfied nothing was replayed */
    if (!wait_readable(readypipe[0], 60) || 1 != read(readypipe[0], &c, 1)) {
        report("cached event keeps its affected-process restriction", 0,
               "the tool saw it replayed, or died");
        goto done;
    }
    report("cached event keeps its affected-process restriction", 1, NULL);

    /* and now one handler B must receive */
    PMIX_LOAD_PROCID(&procz, NS_Z, 0);
    notify_about(&procz);
    report("server notified an event about the process B asked about", 1, NULL);

done:
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
        report("tool saw exactly the events it asked for", 0, "tool exited non-zero");
    } else {
        report("tool saw exactly the events it asked for", 1, NULL);
    }

    fprintf(stdout, "\n%d passed, %d failed\n", npass, nfail);
    return (0 == nfail) ? 0 : 1;
}
