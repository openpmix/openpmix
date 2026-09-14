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
 * A server reads each incoming connect-ack on its progress thread, before
 * any credential has been checked, from a port anything able to reach it
 * can open. It used to start reading the moment a connection was accepted,
 * with blocking recvs and no receive timeout - so a peer that connected and
 * then stayed silent, or sent a few bytes and stopped, held the progress
 * thread, and with it every client of that server, for as long as it cared
 * to. A tool suspended between its connect() and its first send does
 * exactly that by accident; so does a port probe.
 *
 * The listener now hands a connection to the handler only once it is
 * readable, and the handler reads the connect-ack as its bytes arrive,
 * keeping its place between read events. So every stall case below runs
 * with ptl_base_connect_ack_timeout DISABLED: a timeout can bound a stall,
 * but only never waiting can make these pass with none. The timeout's own
 * job - dropping a connection that never finishes - has a case of its own.
 *
 * Each case runs in its own child with an alarm armed, so a regression is
 * reported as a failure rather than hanging "make check". A stalled server
 * shows up as a blocking server call that never returns, since that call
 * can only complete through the progress thread.
 */

#include "src/include/pmix_config.h"

#include "include/pmix.h"
#include "include/pmix_server.h"
#include "src/mca/ptl/base/base.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
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
#define STALL_FAILED   1
#define STALL_HUNG     2
#define STALL_SETUP    3

static int npass = 0;
static int nfail = 0;
static int probes = 0;

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

/* bring up a server and return its listener's loopback port, or -1 */
static int start_server(void)
{
    pmix_status_t rc;
    char *p;

    rc = PMIx_server_init(&mymodule, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        return -1;
    }
    if (NULL == pmix_ptl_base.listener.uri ||
        NULL == (p = strrchr(pmix_ptl_base.listener.uri, ':'))) {
        return -1;
    }
    return atoi(p + 1);
}

static int dial(int port)
{
    struct sockaddr_in sa;
    int sd;

    sd = socket(AF_INET, SOCK_STREAM, 0);
    if (0 > sd) {
        return -1;
    }
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (0 != connect(sd, (struct sockaddr *) &sa, sizeof(sa))) {
        close(sd);
        return -1;
    }
    return sd;
}

static bool send_all(int sd, const char *buf, size_t len)
{
    return (ssize_t) len == send(sd, buf, len, 0);
}

/* true if the server has closed its end of sd within msec */
static bool closed_by_server(int sd, int msec)
{
    struct pollfd pfd;
    char c;

    pfd.fd = sd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    if (0 >= poll(&pfd, 1, msec)) {
        return false;
    }
    return 0 == recv(sd, &c, 1, 0);
}

/* A blocking call that cannot complete unless the progress thread is free
 * to run it. A hung call is caught by the alarm. */
static bool server_answers(void)
{
    char ns[PMIX_MAX_NSLEN + 1];
    pmix_status_t rc;

    snprintf(ns, sizeof(ns), "ptl-stalled-probe-%d", probes++);
    rc = PMIx_server_register_nspace(ns, 0, NULL, 0, NULL, NULL);
    if (PMIX_SUCCESS != rc && PMIX_OPERATION_SUCCEEDED != rc) {
        return false;
    }
    PMIx_server_deregister_nspace(ns, NULL, NULL);
    return true;
}

#define CHILD_CHECK(cond)       \
    do {                        \
        if (!(cond)) {          \
            ok = false;         \
        }                       \
    } while (0)

/* a peer that connects and sends `nsent` bytes of a header - none, one, or
 * less than a header - and then nothing more */
static int stall_child(const char *bytes, size_t nsent)
{
    int port, sd;
    bool ok = true;

    if (0 > (port = start_server()) || 0 > (sd = dial(port))) {
        return STALL_SETUP;
    }
    if (0 < nsent && !send_all(sd, bytes, nsent)) {
        return STALL_SETUP;
    }
    /* give the server time to accept and act on what it has */
    usleep(200000);
    CHILD_CHECK(server_answers());
    CHILD_CHECK(!closed_by_server(sd, 100));

    close(sd);
    PMIx_server_finalize();
    return ok ? STALL_OK : STALL_FAILED;
}

/* A connect-ack delivered in pieces is waited for, not refused, and is
 * parsed once the last piece lands - here a payload that is not a valid
 * connect-ack, so the server refuses it and closes. A header naming more
 * than the server will ever accept is refused as soon as the header is
 * complete, with nothing allocated for it and no payload waited for. A
 * peer that departs mid-header costs nothing. */
static int pieces_child(void)
{
    pmix_ptl_hdr_t hdr;
    char payload[64];
    int port, sd;
    bool ok = true;

    if (0 > (port = start_server())) {
        return STALL_SETUP;
    }

    memset(&hdr, 0, sizeof(hdr));
    hdr.nbytes = sizeof(payload);  // host order, as construct_message writes it
    memset(payload, 0xff, sizeof(payload));
    if (0 > (sd = dial(port))) {
        return STALL_SETUP;
    }
    CHILD_CHECK(send_all(sd, (char *) &hdr, sizeof(hdr) / 2));
    CHILD_CHECK(server_answers());
    CHILD_CHECK(!closed_by_server(sd, 200));
    CHILD_CHECK(send_all(sd, (char *) &hdr + sizeof(hdr) / 2, sizeof(hdr) - sizeof(hdr) / 2));
    CHILD_CHECK(send_all(sd, payload, sizeof(payload) / 2));
    CHILD_CHECK(server_answers());
    CHILD_CHECK(!closed_by_server(sd, 200));
    CHILD_CHECK(send_all(sd, payload + sizeof(payload) / 2,
                         sizeof(payload) - sizeof(payload) / 2));
    CHILD_CHECK(closed_by_server(sd, 5000));
    close(sd);

    memset(&hdr, 0, sizeof(hdr));
    hdr.nbytes = PMIX_MAX_CRED_SIZE + 1;
    if (0 > (sd = dial(port))) {
        return STALL_SETUP;
    }
    CHILD_CHECK(send_all(sd, (char *) &hdr, sizeof(hdr)));
    CHILD_CHECK(closed_by_server(sd, 5000));
    close(sd);

    if (0 > (sd = dial(port))) {
        return STALL_SETUP;
    }
    CHILD_CHECK(send_all(sd, "xyz", 3));
    close(sd);
    CHILD_CHECK(server_answers());

    PMIx_server_finalize();
    return ok ? STALL_OK : STALL_FAILED;
}

/* with the timeout disabled, a connection that never finishes its
 * connect-ack is still closed when the server finalizes - not left holding
 * a descriptor for the life of the process */
static int finalize_child(void)
{
    int port, silent, partial;
    bool ok = true;

    if (0 > (port = start_server()) || 0 > (silent = dial(port)) ||
        0 > (partial = dial(port)) || !send_all(partial, "x", 1)) {
        return STALL_SETUP;
    }
    CHILD_CHECK(server_answers());
    PMIx_server_finalize();
    CHILD_CHECK(closed_by_server(silent, 5000));
    CHILD_CHECK(closed_by_server(partial, 5000));
    close(silent);
    close(partial);
    return ok ? STALL_OK : STALL_FAILED;
}

/* with a one-second timeout, a connection that has not delivered its whole
 * connect-ack in that time is dropped - and a complete one that arrives
 * after the others is unaffected by their timers */
static int timeout_child(void)
{
    int port, silent, partial;
    bool ok = true;

    if (0 > (port = start_server()) || 0 > (silent = dial(port)) ||
        0 > (partial = dial(port)) || !send_all(partial, "abc", 3)) {
        return STALL_SETUP;
    }
    CHILD_CHECK(!closed_by_server(silent, 300));
    CHILD_CHECK(closed_by_server(silent, 3000));
    CHILD_CHECK(closed_by_server(partial, 3000));
    CHILD_CHECK(server_answers());
    close(silent);
    close(partial);
    PMIx_server_finalize();
    return ok ? STALL_OK : STALL_FAILED;
}

enum { CASE_IDLE, CASE_ONE_BYTE, CASE_PARTIAL, CASE_PIECES, CASE_FINALIZE, CASE_TIMEOUT };

static void run_case(const char *name, int which, const char *timeout)
{
    pid_t child;
    int status = 0, rc;

    child = fork();
    if (0 > child) {
        report(name, 0, "fork failed");
        return;
    }
    if (0 == child) {
        setenv("PMIX_MCA_ptl_base_connect_ack_timeout", timeout, 1);
        signal(SIGPIPE, SIG_IGN);
        signal(SIGALRM, on_alarm);
        alarm(20);
        switch (which) {
        case CASE_IDLE:
            rc = stall_child(NULL, 0);
            break;
        case CASE_ONE_BYTE:
            rc = stall_child("x", 1);
            break;
        case CASE_PARTIAL:
            rc = stall_child("abc", 3);
            break;
        case CASE_PIECES:
            rc = pieces_child();
            break;
        case CASE_FINALIZE:
            rc = finalize_child();
            break;
        default:
            rc = timeout_child();
            break;
        }
        _exit(rc);
    }
    waitpid(child, &status, 0);
    if (!WIFEXITED(status)) {
        report(name, 0, "the server died on a signal");
    } else if (STALL_HUNG == WEXITSTATUS(status)) {
        report(name, 0, "the progress thread stopped servicing requests");
    } else if (STALL_SETUP == WEXITSTATUS(status)) {
        report(name, 0, "the child could not set up the case");
    } else if (STALL_OK != WEXITSTATUS(status)) {
        report(name, 0, "the server did not treat the connection as expected");
    } else {
        report(name, 1, NULL);
    }
}

int main(int argc, char **argv)
{
    PMIX_HIDE_UNUSED_PARAMS(argc, argv);

    fprintf(stdout, "\n=== PTL stalled-peer unit test ===\n\n");

    run_case("a server keeps running while a peer connects and says nothing",
             CASE_IDLE, "0");
    run_case("a server keeps running while a peer sends one byte and stops",
             CASE_ONE_BYTE, "0");
    run_case("a server keeps running while a peer sends part of a header",
             CASE_PARTIAL, "0");
    run_case("a connect-ack in pieces is waited for, an oversized one refused at once",
             CASE_PIECES, "0");
    run_case("finalize closes connections that never finished their connect-ack",
             CASE_FINALIZE, "0");
    run_case("the connect-ack timeout drops a connection that never finishes",
             CASE_TIMEOUT, "1");

    fprintf(stdout, "\n%d passed, %d failed\n", npass, nfail);
    return (0 == nfail) ? 0 : 1;
}
