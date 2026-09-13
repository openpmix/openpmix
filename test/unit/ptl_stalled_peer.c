/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Regression test: a peer that stalls partway into connecting must not
 * stop the server.
 *
 * A server reads each incoming connect-ack with blocking recvs on its
 * progress thread, before any credential has been checked. It used to
 * start reading the moment a connection was accepted, with no receive
 * timeout at all - so anything that could reach the listener and then
 * stayed silent held the progress thread, and with it every client of
 * that server, for as long as it cared to. A tool suspended between its
 * connect() and its first send does exactly that by accident.
 *
 * Two changes closed it, and each case below pins one of them on its own:
 *
 *  - "idle": the peer connects and sends nothing. The listener now waits
 *    for the socket to become readable before handing it to the blocking
 *    handler, so an idle peer costs the progress thread nothing. This
 *    case runs with the connect-ack timeout DISABLED, so only that change
 *    can make it pass.
 *
 *  - "partial": the peer sends the first few bytes of a request and
 *    stops. Readability does not help here - the handler has to run - so
 *    the wait is bounded by a receive timeout on the socket. That in turn
 *    depends on pmix_ptl_base_recv_blocking reporting the expired timeout
 *    rather than retrying it, which it used to do. This case runs with a
 *    one-second timeout.
 *
 * Each case runs in its own child with an alarm armed, so a regression is
 * reported as a failure rather than hanging "make check". The child
 * brings up a server, opens a raw socket to its listener, stalls it, and
 * then makes a blocking server call that can only complete if the
 * progress thread is still running.
 */

#include "src/include/pmix_config.h"

#include "include/pmix.h"
#include "include/pmix_server.h"
#include "src/mca/ptl/base/base.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/* child exit codes */
#define STALL_OK       0
#define STALL_HUNG     2
#define STALL_SETUP    3

static int npass = 0;
static int nfail = 0;

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

static void on_alarm(int sig)
{
    PMIX_HIDE_UNUSED_PARAMS(sig);
    _exit(STALL_HUNG);
}

static int stall_child(bool partial)
{
    pmix_status_t rc;
    pmix_nspace_t ns;
    struct sockaddr_in sa;
    char *p;
    int port, sd;

    rc = PMIx_server_init(&mymodule, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        return STALL_SETUP;
    }
    if (NULL == pmix_ptl_base.listener.uri ||
        NULL == (p = strrchr(pmix_ptl_base.listener.uri, ':'))) {
        return STALL_SETUP;
    }
    port = atoi(p + 1);

    sd = socket(AF_INET, SOCK_STREAM, 0);
    if (0 > sd) {
        return STALL_SETUP;
    }
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (0 != connect(sd, (struct sockaddr *) &sa, sizeof(sa))) {
        return STALL_SETUP;
    }
    if (partial) {
        /* less than a message header, and then nothing more */
        if (3 != write(sd, "abc", 3)) {
            return STALL_SETUP;
        }
    }
    /* give the server time to accept and act on what it has */
    sleep(1);

    /* a blocking call completes only via the progress thread */
    signal(SIGALRM, on_alarm);
    alarm(8);
    PMIX_LOAD_NSPACE(ns, "ptl-stalled-peer");
    rc = PMIx_server_register_nspace(ns, 1, NULL, 0, NULL, NULL);
    alarm(0);

    close(sd);
    PMIx_server_finalize();
    return STALL_OK;
}

static void check_stall(const char *name, bool partial, const char *timeout)
{
    pid_t child;
    int status = 0;

    child = fork();
    if (0 > child) {
        report(name, 0, "fork failed");
        return;
    }
    if (0 == child) {
        setenv("PMIX_MCA_ptl_base_connect_ack_timeout", timeout, 1);
        _exit(stall_child(partial));
    }
    waitpid(child, &status, 0);
    if (!WIFEXITED(status)) {
        report(name, 0, "the server died on a signal");
    } else if (STALL_HUNG == WEXITSTATUS(status)) {
        report(name, 0, "the progress thread stopped servicing requests");
    } else if (STALL_OK != WEXITSTATUS(status)) {
        report(name, 0, "the child could not set up the case");
    } else {
        report(name, 1, NULL);
    }
}

int main(int argc, char **argv)
{
    PMIX_HIDE_UNUSED_PARAMS(argc, argv);

    fprintf(stdout, "\n=== PTL stalled-peer unit test ===\n\n");

    /* timeout disabled: only waiting for readability can pass this */
    check_stall("a server keeps running while a peer connects and says nothing",
                false, "0");
    /* one-second timeout: only a bounded, reported wait can pass this */
    check_stall("a server keeps running while a peer sends part of a request",
                true, "1");

    fprintf(stdout, "\n%d passed, %d failed\n", npass, nfail);
    return (0 == nfail) ? 0 : 1;
}
