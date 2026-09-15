/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Regression test: a tool attaching to a server must not stop its own
 * progress thread while it waits for that server.
 *
 * PMIx_tool_attach_to_server does its work in a thread-shift handler,
 * pmix_tool_retry_attach, and that handler used to run the whole connect
 * with blocking calls: the connect() itself and every read of the
 * server's replies. So for as long as the server took to answer, nothing
 * else in the tool ran - no event, no callback, no other connection - and
 * with ptl_base_handshake_wait_time at its old default of 0, a server that
 * accepted the connection and never answered held the thread for good.
 *
 * The attach is now event-driven. Each case runs in its own child with an
 * alarm armed, so a regression is a failure rather than a hung "make
 * check":
 *
 *   silent server   a listener that lets the connection complete but never
 *                   reads or replies. While the attach waits, a blocking
 *                   event-handler registration - which can only finish on
 *                   the progress thread - must come back promptly; and the
 *                   attach must then fail with PMIX_ERR_TIMEOUT once the
 *                   handshake wait expires, not before and not never.
 *   closed server   a listener that accepts and closes at once. The attach
 *                   must fail promptly, not wait out the timer.
 *
 * Attaching to a real server is covered by the existing tool tests
 * (tool_api, tool_rndz, run_toolswitch.pl), which go through the same
 * path.
 */

#include "src/include/pmix_config.h"

#include "include/pmix.h"
#include "include/pmix_tool.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/* the handshake wait the children run with, in seconds */
#define ATTACH_WAIT 4

/* child exit codes */
#define ATT_OK      0
#define ATT_FAILED  1
#define ATT_SETUP   2
#define ATT_HUNG    3

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

static void on_alarm(int sig)
{
    (void) sig;
    _exit(ATT_HUNG);
}

static double since(const struct timeval *start)
{
    struct timeval now;

    gettimeofday(&now, NULL);
    return (double) (now.tv_sec - start->tv_sec)
           + ((double) (now.tv_usec - start->tv_usec) / 1000000.0);
}

/* a listening socket on the loopback interface, on a port of the
 * kernel's choosing */
static int make_listener(int *port)
{
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    int sd;

    sd = socket(AF_INET, SOCK_STREAM, 0);
    if (0 > sd) {
        return -1;
    }
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (0 != bind(sd, (struct sockaddr *) &addr, sizeof(addr)) ||
        0 != listen(sd, 8) ||
        0 != getsockname(sd, (struct sockaddr *) &addr, &len)) {
        close(sd);
        return -1;
    }
    *port = ntohs(addr.sin_port);
    return sd;
}

typedef struct {
    int port;
    pmix_status_t rc;
    double secs;
    volatile bool done;
} attach_t;

static void *attach_thread(void *arg)
{
    attach_t *at = (attach_t *) arg;
    pmix_info_t info[1];
    pmix_proc_t server;
    struct timeval start;
    char uri[128];

    snprintf(uri, sizeof(uri), "fakeserver.0;tcp4://127.0.0.1:%d", at->port);
    PMIX_INFO_LOAD(&info[0], PMIX_SERVER_URI, uri, PMIX_STRING);
    gettimeofday(&start, NULL);
    at->rc = PMIx_tool_attach_to_server(NULL, &server, info, 1);
    at->secs = since(&start);
    PMIX_INFO_DESTRUCT(&info[0]);
    at->done = true;
    return NULL;
}

static void noop_handler(size_t evhdlr_registration_id, pmix_status_t status,
                         const pmix_proc_t *source, pmix_info_t info[], size_t ninfo,
                         pmix_info_t *results, size_t nresults,
                         pmix_event_notification_cbfunc_fn_t cbfunc, void *cbdata)
{
    (void) evhdlr_registration_id;
    (void) status;
    (void) source;
    (void) info;
    (void) ninfo;
    (void) results;
    (void) nresults;
    if (NULL != cbfunc) {
        cbfunc(PMIX_SUCCESS, NULL, 0, NULL, NULL, cbdata);
    }
}

static int tool_up(void)
{
    pmix_proc_t myproc;
    pmix_info_t tinfo;
    pmix_status_t rc;
    char val[16];

    snprintf(val, sizeof(val), "%d", ATTACH_WAIT);
    setenv("PMIX_MCA_ptl_base_handshake_wait_time", val, 1);
    PMIX_INFO_LOAD(&tinfo, PMIX_TOOL_DO_NOT_CONNECT, NULL, PMIX_BOOL);
    rc = PMIx_tool_init(&myproc, &tinfo, 1);
    PMIX_INFO_DESTRUCT(&tinfo);
    return (PMIX_SUCCESS == rc) ? 0 : -1;
}

/* the server lets the connection complete - the kernel accepts it into
 * the backlog - and then says nothing */
static int silent_child(int port)
{
    attach_t at;
    pthread_t tid;
    struct timeval start;
    pmix_status_t code = PMIX_EVENT_JOB_END;
    pmix_status_t rc;
    double probe;
    bool pending;

    signal(SIGALRM, on_alarm);
    alarm(4 * ATTACH_WAIT);
    if (0 != tool_up()) {
        return ATT_SETUP;
    }
    memset(&at, 0, sizeof(at));
    at.port = port;
    if (0 != pthread_create(&tid, NULL, attach_thread, &at)) {
        return ATT_SETUP;
    }

    /* let the attach connect and start waiting on the reply */
    usleep(500000);

    /* this can only complete on the progress thread */
    gettimeofday(&start, NULL);
    rc = PMIx_Register_event_handler(&code, 1, NULL, 0, noop_handler, NULL, NULL);
    probe = since(&start);
    pending = !at.done;
    if (0 <= rc) {
        (void) PMIx_Deregister_event_handler((size_t) rc, NULL, NULL);
    }

    pthread_join(tid, NULL);
    alarm(0);

    if (0 > rc) {
        fprintf(stdout, "    event registration failed: %s\n", PMIx_Error_string(rc));
        return ATT_FAILED;
    }
    if (!pending) {
        fprintf(stdout, "    the attach finished before the probe did (%.1fs) - "
                        "the progress thread was held for it\n", at.secs);
        return ATT_FAILED;
    }
    if (1.5 < probe) {
        fprintf(stdout, "    event registration took %.1fs while the attach waited\n", probe);
        return ATT_FAILED;
    }
    if (PMIX_ERR_TIMEOUT != at.rc) {
        fprintf(stdout, "    the attach returned %s, expected PMIX_ERR_TIMEOUT\n",
                PMIx_Error_string(at.rc));
        return ATT_FAILED;
    }
    if ((double) ATTACH_WAIT - 0.5 > at.secs || (double) (3 * ATTACH_WAIT) < at.secs) {
        fprintf(stdout, "    the attach timed out after %.1fs with a %ds wait\n",
                at.secs, ATTACH_WAIT);
        return ATT_FAILED;
    }
    PMIx_tool_finalize();
    return ATT_OK;
}

/* the server accepts the connection and closes it at once */
static int closed_child(int port)
{
    attach_t at;

    signal(SIGALRM, on_alarm);
    alarm(4 * ATTACH_WAIT);
    if (0 != tool_up()) {
        return ATT_SETUP;
    }
    memset(&at, 0, sizeof(at));
    at.port = port;
    (void) attach_thread(&at);
    alarm(0);
    if (PMIX_SUCCESS == at.rc) {
        fprintf(stdout, "    the attach to a closed connection succeeded\n");
        return ATT_FAILED;
    }
    if ((double) ATTACH_WAIT - 1.0 < at.secs) {
        fprintf(stdout, "    the attach took %.1fs to notice the close - it waited out "
                        "the timer\n", at.secs);
        return ATT_FAILED;
    }
    PMIx_tool_finalize();
    return ATT_OK;
}

static void run_case(const char *name, int (*fn)(int), bool closer)
{
    int lsd, port = 0, status = 0, csd;
    pid_t child, acceptor = -1;

    lsd = make_listener(&port);
    if (0 > lsd) {
        report(name, 0, "could not create a listener");
        return;
    }
    if (closer) {
        /* a separate process accepts and closes, so the child's attach is
         * answered while this one waits on it */
        acceptor = fork();
        if (0 == acceptor) {
            alarm(4 * ATTACH_WAIT);
            csd = accept(lsd, NULL, NULL);
            if (0 <= csd) {
                close(csd);
            }
            _exit(0);
        }
    }
    child = fork();
    if (0 > child) {
        report(name, 0, "fork failed");
        close(lsd);
        return;
    }
    if (0 == child) {
        close(lsd);
        _exit(fn(port));
    }
    waitpid(child, &status, 0);
    close(lsd);
    if (0 < acceptor) {
        kill(acceptor, SIGTERM);
        waitpid(acceptor, NULL, 0);
    }
    if (!WIFEXITED(status)) {
        report(name, 0, "the tool died on a signal");
    } else if (ATT_HUNG == WEXITSTATUS(status)) {
        report(name, 0, "the attach never returned");
    } else if (ATT_SETUP == WEXITSTATUS(status)) {
        report(name, 0, "the child could not set up");
    } else {
        report(name, ATT_OK == WEXITSTATUS(status), "see above");
    }
}

int main(int argc, char **argv)
{
    (void) argc;
    (void) argv;

    /* the children report their details and then _exit(), which does not
     * flush a buffered stream */
    setvbuf(stdout, NULL, _IONBF, 0);

    fprintf(stdout, "\n=== tool attach, event-driven ===\n\n");
    run_case("progress thread runs while an attach waits on a silent server", silent_child,
             false);
    run_case("attach to a server that closes the connection fails promptly", closed_child, true);

    fprintf(stdout, "\n%d passed, %d failed\n", npass, nfail);
    return (0 == nfail) ? 0 : 1;
}
