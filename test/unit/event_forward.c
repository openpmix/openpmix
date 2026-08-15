/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * What a PMIx server decides to forward, and what the receiving client
 * does with an event none of its handlers accepted. Both halves of that
 * were wrong, and both need a real client behind a real socket - which
 * is why this program forks and execs itself rather than driving the
 * library in one process.
 *
 * Case 1 - the server's fan-out filter must not drop an event a local
 * handler wanted. The filter matches on the event code; it deliberately
 * does not consult the affected-proc list a registration carried,
 * because that list belongs to one registration *message* and a peer's
 * handlers do not agree on it. The client here registers a handler
 * restricted to one process and then a second handler with no
 * restriction at all - the second sends no message, since the code is
 * already active and it carries no directives - and the event names a
 * third process. Only the unrestricted handler may fire.
 *
 * Case 2 - an event that arrived and matched nothing must be parked, so
 * a handler registering a moment later still gets it. The client
 * registers a handler whose *source range* excludes the server, which
 * the server cannot know (a handler's range never leaves its own
 * process), so the event is forwarded, rejected locally, and has
 * nowhere to go but the cache. A second handler then registers and must
 * be given it.
 *
 * The ordering in case 2 is what makes it an assertion rather than a
 * race: the client makes a blocking round trip to the server before
 * registering the second handler, and the notification travels the same
 * socket ahead of that reply, so the event has been received and
 * rejected by the time the second handler exists. Without that, a
 * library that never parks anything would still pass by delivering the
 * event live.
 */

#include "src/include/pmix_config.h"

#include "include/pmix.h"
#include "include/pmix_server.h"
#include "src/include/pmix_globals.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

/* codes with no meaning to the library, so nothing else reacts to them */
#define CODE_DROP  (PMIX_EXTERNAL_ERR_BASE - 51)
#define CODE_PARK  (PMIX_EXTERNAL_ERR_BASE - 52)

#define NSPACE     "event-fwd"
/* processes that are neither the client nor each other */
#define NS_ASKED   "event-fwd-asked-about"
#define NS_HAPPENED "event-fwd-happened-to"

/* how long to wait for a handler that must fire, and for one that must not */
#define LOUD_SECS  20
#define QUIET_SECS 2

static int npass = 0;
static int nfail = 0;

static void report(const char *name, int passed, const char *detail)
{
    if (passed) {
        fprintf(stdout, "  PASS: %s\n", name);
        ++npass;
    } else {
        fprintf(stdout, "  FAIL: %s (%s)\n", name, NULL == detail ? "" : detail);
        ++nfail;
    }
}

/* ------------------------------------------------------------------ */
/* the client                                                         */
/* ------------------------------------------------------------------ */

static volatile int fired_restricted = 0;
static volatile int fired_open = 0;
static volatile int fired_ranged = 0;
static volatile int fired_late = 0;

static void count_hdlr(volatile int *counter, pmix_event_notification_cbfunc_fn_t cbfunc,
                       void *cbdata)
{
    ++(*counter);
    if (NULL != cbfunc) {
        cbfunc(PMIX_EVENT_ACTION_COMPLETE, NULL, 0, NULL, NULL, cbdata);
    }
}

static void hdlr_restricted(size_t id, pmix_status_t status, const pmix_proc_t *source,
                            pmix_info_t info[], size_t ninfo, pmix_info_t results[],
                            size_t nresults, pmix_event_notification_cbfunc_fn_t cbfunc,
                            void *cbdata)
{
    (void) id; (void) status; (void) source; (void) info; (void) ninfo;
    (void) results; (void) nresults;
    count_hdlr(&fired_restricted, cbfunc, cbdata);
}

static void hdlr_open(size_t id, pmix_status_t status, const pmix_proc_t *source,
                      pmix_info_t info[], size_t ninfo, pmix_info_t results[], size_t nresults,
                      pmix_event_notification_cbfunc_fn_t cbfunc, void *cbdata)
{
    (void) id; (void) status; (void) source; (void) info; (void) ninfo;
    (void) results; (void) nresults;
    count_hdlr(&fired_open, cbfunc, cbdata);
}

static void hdlr_ranged(size_t id, pmix_status_t status, const pmix_proc_t *source,
                        pmix_info_t info[], size_t ninfo, pmix_info_t results[], size_t nresults,
                        pmix_event_notification_cbfunc_fn_t cbfunc, void *cbdata)
{
    (void) id; (void) status; (void) source; (void) info; (void) ninfo;
    (void) results; (void) nresults;
    count_hdlr(&fired_ranged, cbfunc, cbdata);
}

static void hdlr_late(size_t id, pmix_status_t status, const pmix_proc_t *source,
                      pmix_info_t info[], size_t ninfo, pmix_info_t results[], size_t nresults,
                      pmix_event_notification_cbfunc_fn_t cbfunc, void *cbdata)
{
    (void) id; (void) status; (void) source; (void) info; (void) ninfo;
    (void) results; (void) nresults;
    count_hdlr(&fired_late, cbfunc, cbdata);
}

static void reg_cbfunc(pmix_status_t status, size_t refid, void *cbdata)
{
    pmix_status_t *sp = (pmix_status_t *) cbdata;
    (void) refid;
    *sp = status;
}

/* register one handler and block until the registration is acknowledged.
 * The info array must stay alive across the wait - the library does not
 * copy it - which is why it is the caller's and not built in here. */
static pmix_status_t register_hdlr(pmix_status_t code, pmix_info_t *info, size_t ninfo,
                                   pmix_notification_fn_t fn)
{
    pmix_status_t regstatus = PMIX_ERR_WOULD_BLOCK;
    int i;

    PMIx_Register_event_handler(&code, 1, info, ninfo, fn, reg_cbfunc, (void *) &regstatus);
    for (i = 0; i < 400 && PMIX_ERR_WOULD_BLOCK == regstatus; i++) {
        usleep(50000);
    }
    return regstatus;
}

static int wait_fires(volatile int *counter, int secs)
{
    int i;

    for (i = 0; i < secs * 10; i++) {
        if (0 < *counter) {
            break;
        }
        usleep(100000);
    }
    return *counter;
}

/* A blocking round trip to the server, used only for its ordering: the
 * notification the server sent before this went out travels the same
 * socket, so it has been received and processed by the time we return.
 *
 * A query rather than a get: a get for a key nobody has posted parks
 * until the key appears, which would simply hang. This server has no
 * host query interface, so the request comes back as soon as the server
 * has looked at it. */
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

static int sync_with_server(int readyfd, int gofd)
{
    char c = 'r';

    if (1 != write(readyfd, &c, 1)) {
        return 0;
    }
    if (1 != read(gofd, &c, 1)) {
        return 0;
    }
    return 1;
}

static int run_client(int readyfd, int gofd)
{
    pmix_proc_t myproc, asked, rngproc;
    pmix_info_t info[2];
    pmix_status_t rc;
    pmix_data_range_t range = PMIX_RANGE_CUSTOM;
    int fires;

    rc = PMIx_Init(&myproc, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "  client: PMIx_Init failed: %s\n", PMIx_Error_string(rc));
        return 1;
    }

    /* ---- case 1: an affected-restricted registration must not speak
     * for the handlers registered after it ---- */
    PMIX_LOAD_PROCID(&asked, NS_ASKED, 0);
    PMIX_INFO_LOAD(&info[0], PMIX_EVENT_AFFECTED_PROC, &asked, PMIX_PROC);
    rc = register_hdlr(CODE_DROP, info, 1, hdlr_restricted);
    PMIX_INFO_DESTRUCT(&info[0]);
    if (0 > rc) {
        fprintf(stderr, "  client: restricted registration failed: %s\n", PMIx_Error_string(rc));
        goto done;
    }
    /* no directives and the code is already active, so this registration
     * is never sent to the server - the server's record of what we want
     * still says only what the handler above asked for */
    rc = register_hdlr(CODE_DROP, NULL, 0, hdlr_open);
    if (0 > rc) {
        fprintf(stderr, "  client: unrestricted registration failed: %s\n",
                PMIx_Error_string(rc));
        goto done;
    }

    if (!sync_with_server(readyfd, gofd)) {
        goto done;
    }

    fires = wait_fires(&fired_open, LOUD_SECS);
    report("an unrestricted handler receives an event the server was told "
           "another handler did not want",
           1 == fires, "the server dropped it");
    report("the restricted handler is not given an event about a process it "
           "did not ask about",
           0 == fired_restricted, "it fired");

    /* ---- case 2: an event nothing accepted is parked for a handler
     * that registers later ---- */
    PMIX_LOAD_PROCID(&rngproc, NS_ASKED, PMIX_RANK_WILDCARD);
    PMIX_INFO_LOAD(&info[0], PMIX_RANGE, &range, PMIX_DATA_RANGE);
    PMIX_INFO_LOAD(&info[1], PMIX_EVENT_CUSTOM_RANGE, &rngproc, PMIX_PROC);
    rc = register_hdlr(CODE_PARK, info, 2, hdlr_ranged);
    PMIX_INFO_DESTRUCT(&info[0]);
    PMIX_INFO_DESTRUCT(&info[1]);
    if (0 > rc) {
        fprintf(stderr, "  client: range-restricted registration failed: %s\n",
                PMIx_Error_string(rc));
        goto done;
    }

    if (!sync_with_server(readyfd, gofd)) {
        goto done;
    }

    /* order ourselves behind the notification the server has now sent */
    round_trip();

    fires = wait_fires(&fired_ranged, QUIET_SECS);
    report("a handler is not given an event from a source outside the range "
           "it registered with",
           0 == fires, "it fired");

    /* registering this one replays anything parked that it matches. It
     * carries no directives and CODE_PARK is already active, so nothing
     * about this registration reaches the server: whatever it sees came
     * out of our own cache */
    rc = register_hdlr(CODE_PARK, NULL, 0, hdlr_late);
    if (0 > rc) {
        fprintf(stderr, "  client: late registration failed: %s\n", PMIx_Error_string(rc));
        goto done;
    }
    fires = wait_fires(&fired_late, LOUD_SECS);
    report("an event no handler accepted is parked and replayed to a handler "
           "that registers afterwards",
           1 == fires, "it was dropped");

done:
    PMIx_Finalize(NULL, 0);
    fprintf(stdout, "  client: %d passed, %d failed\n", npass, nfail);
    return (0 == nfail && 4 == npass) ? 0 : 1;
}

/* ------------------------------------------------------------------ */
/* the server                                                         */
/* ------------------------------------------------------------------ */

static volatile bool regdone = false;
static volatile int notified = 0;

static void regcbfunc(pmix_status_t status, void *cbdata)
{
    (void) status; (void) cbdata;
    regdone = true;
}

static void opcbfunc(pmix_status_t status, void *cbdata)
{
    (void) status; (void) cbdata;
    ++notified;
}

static pmix_server_module_t mymodule = {0};

static pmix_status_t register_job(void)
{
    pmix_info_t info[4];
    pmix_proc_t proc;
    pmix_nspace_t ns;
    pmix_status_t rc;
    char *noderegex = NULL, *ppnregex = NULL;
    uint32_t nprocs = 1;
    int i;

    PMIx_generate_regex(pmix_globals.hostname, &noderegex);
    PMIx_generate_ppn("0", &ppnregex);
    PMIX_INFO_LOAD(&info[0], PMIX_NODE_MAP, noderegex, PMIX_REGEX);
    PMIX_INFO_LOAD(&info[1], PMIX_PROC_MAP, ppnregex, PMIX_REGEX);
    PMIX_INFO_LOAD(&info[2], PMIX_JOB_SIZE, &nprocs, PMIX_UINT32);
    PMIX_INFO_LOAD(&info[3], PMIX_UNIV_SIZE, &nprocs, PMIX_UINT32);

    /* a pmix_nspace_t rather than the literal: the API takes a
     * fixed-size array, and gcc rejects a shorter literal outright */
    PMIX_LOAD_NSPACE(ns, NSPACE);
    rc = PMIx_server_register_nspace(ns, 1, info, 4, NULL, NULL);
    for (i = 0; i < 4; i++) {
        PMIX_INFO_DESTRUCT(&info[i]);
    }
    free(noderegex);
    free(ppnregex);
    if (PMIX_OPERATION_SUCCEEDED == rc) {
        rc = PMIX_SUCCESS;
    }
    if (PMIX_SUCCESS != rc) {
        return rc;
    }

    PMIX_LOAD_PROCID(&proc, NSPACE, 0);
    rc = PMIx_server_register_client(&proc, geteuid(), getegid(), NULL, regcbfunc, NULL);
    if (PMIX_OPERATION_SUCCEEDED == rc) {
        return PMIX_SUCCESS;
    }
    if (PMIX_SUCCESS != rc) {
        return rc;
    }
    /* don't fork the client until it is registered */
    for (i = 0; i < 400 && !regdone; i++) {
        usleep(50000);
    }
    return PMIX_SUCCESS;
}

/* send one event and do not return until the library is done with it */
static void notify(pmix_status_t code, pmix_info_t *info, size_t ninfo)
{
    int i, was = notified;

    PMIx_Notify_event(code, &pmix_globals.myid, PMIX_RANGE_LOCAL, info, ninfo, opcbfunc, NULL);
    /* PMIx_Notify_event does not copy the directives - the caller keeps
     * them alive until the callback fires */
    for (i = 0; i < 200 && notified == was; i++) {
        usleep(50000);
    }
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
    pmix_info_t sinfo, ainfo;
    pmix_proc_t proc, happened;
    char **client_env = NULL;
    char *client_argv[5];
    char fdbuf[64];
    int readypipe[2], gopipe[2];
    pid_t child;
    int status = 0;
    char c = 'g';
    bool flag = true;

    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    /* re-executed as our own client - the fd numbers ride in argv */
    if (3 < argc && 0 == strcmp(argv[1], "client")) {
        return run_client(atoi(argv[2]), atoi(argv[3]));
    }

    fprintf(stdout, "\n=== event forwarding and caching unit test ===\n\n");

    if (0 != pipe(readypipe) || 0 != pipe(gopipe)) {
        fprintf(stderr, "pipe() failed\n");
        return 1;
    }

    PMIX_INFO_LOAD(&sinfo, PMIX_SERVER_TOOL_SUPPORT, &flag, PMIX_BOOL);
    rc = PMIx_server_init(&mymodule, &sinfo, 1);
    PMIX_INFO_DESTRUCT(&sinfo);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "PMIx_server_init failed: %s\n", PMIx_Error_string(rc));
        return 1;
    }

    rc = register_job();
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "could not register the job: %s\n", PMIx_Error_string(rc));
        goto done;
    }

    PMIX_LOAD_PROCID(&proc, NSPACE, 0);
    /* Seed the child's environment with our own before the library adds
     * its variables. Handing execve only the PMIx variables strands the
     * child without the library search path this program was started
     * with - and libtool starts an uninstalled test through a wrapper
     * script that sets exactly that - so the child would silently run
     * against the *installed* PMIx rather than the one under test. */
    client_env = PMIx_Argv_copy(environ);
    rc = PMIx_server_setup_fork(&proc, &client_env);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "PMIx_server_setup_fork failed: %s\n", PMIx_Error_string(rc));
        goto done;
    }

    child = fork();
    if (0 > child) {
        fprintf(stderr, "fork() failed\n");
        goto done;
    }
    if (0 == child) {
        /* exec rather than run in the fork: this process has an
         * initialized PMIx server in it, and PMIx_Init would find the
         * one-time-init latch already set */
        close(readypipe[0]);
        close(gopipe[1]);
        client_argv[0] = argv[0];
        client_argv[1] = (char *) "client";
        snprintf(fdbuf, sizeof(fdbuf), "%d", readypipe[1]);
        client_argv[2] = strdup(fdbuf);
        snprintf(fdbuf, sizeof(fdbuf), "%d", gopipe[0]);
        client_argv[3] = strdup(fdbuf);
        client_argv[4] = NULL;
        execve(argv[0], client_argv, client_env);
        fprintf(stderr, "exec of %s failed\n", argv[0]);
        _exit(127);
    }
    close(readypipe[1]);
    close(gopipe[0]);

    /* case 1: the event names a process neither handler asked about */
    if (!wait_readable(readypipe[0], 60) || 1 != read(readypipe[0], &c, 1)) {
        fprintf(stderr, "the client never reported ready\n");
        goto reap;
    }
    PMIX_LOAD_PROCID(&happened, NS_HAPPENED, 0);
    PMIX_INFO_LOAD(&ainfo, PMIX_EVENT_AFFECTED_PROC, &happened, PMIX_PROC);
    notify(CODE_DROP, &ainfo, 1);
    PMIX_INFO_DESTRUCT(&ainfo);
    c = 'g';
    if (1 != write(gopipe[1], &c, 1)) {
        goto reap;
    }

    /* case 2: an event carrying nothing, from us - a source the client's
     * only handler for this code has excluded */
    if (!wait_readable(readypipe[0], 60) || 1 != read(readypipe[0], &c, 1)) {
        fprintf(stderr, "the client never reported ready a second time\n");
        goto reap;
    }
    notify(CODE_PARK, NULL, 0);
    c = 'g';
    if (1 != write(gopipe[1], &c, 1)) {
        goto reap;
    }

reap:
    waitpid(child, &status, 0);
    if (WIFEXITED(status)) {
        status = WEXITSTATUS(status);
    } else {
        fprintf(stderr, "the client died on a signal\n");
        status = 1;
    }

done:
    PMIx_server_finalize();
    if (NULL != client_env) {
        PMIx_Argv_free(client_env);
    }
    if (0 != status) {
        fprintf(stdout, "\n=== FAILED ===\n\n");
        return 1;
    }
    fprintf(stdout, "\n=== PASSED ===\n\n");
    return 0;
}
