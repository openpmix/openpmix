/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Unit test: pmix_ptl_base_recv_blocking gives up when the socket's
 * receive timeout expires.
 *
 * A process connecting to a server reads the server's reply to its
 * connect-ack with pmix_ptl_base_recv_blocking, after
 * pmix_ptl_base_set_timeout has put a receive timeout of
 * ptl_base_handshake_wait_time seconds on the socket - so that a server
 * which accepts the connection and never answers does not hold the
 * connecting process forever. A tool attaching to a server does that on
 * its progress thread. A server's own blocking reads after an incoming
 * connect-ack - the psec handshake - are bounded the same way, by
 * ptl_base_connect_ack_timeout.
 *
 * When that timeout expires, recv() on the blocking socket fails with
 * EAGAIN/EWOULDBLOCK - the same errors a non-blocking socket gives for
 * "nothing yet" - and the loop treated both alike and went round again,
 * which simply started another wait. The timeout could never end the
 * wait it was set to bound. These cases pin the difference, over a
 * socketpair with no server involved.
 */

#include "src/include/pmix_config.h"

#include "include/pmix.h"
#include "include/pmix_server.h"
#include "src/include/pmix_globals.h"
#include "src/mca/ptl/base/base.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
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

static void stuck(int sig)
{
    static const char msg[] = "  FAIL: recv_blocking kept waiting after its receive timeout "
                              "expired\n";
    PMIX_HIDE_UNUSED_PARAMS(sig);
    if (0 > write(STDOUT_FILENO, msg, sizeof(msg) - 1)) {
        /* nothing more we can do */
    }
    _exit(1);
}

static double now(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double) ts.tv_sec + (double) ts.tv_nsec / 1.0e9;
}

static bool set_rcvtimeo(int sd, int sec)
{
    struct timeval tv;

    tv.tv_sec = sec;
    tv.tv_usec = 0;
    return 0 == setsockopt(sd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

int main(int argc, char **argv)
{
    static pmix_server_module_t mymodule = {0};
    pmix_status_t rc;
    int sv[2], flags, status;
    pid_t child;
    char buf[8];
    double start;
    PMIX_HIDE_UNUSED_PARAMS(argc, argv);

    setvbuf(stdout, NULL, _IONBF, 0);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGALRM, stuck);

    /* the routine emits verbose output through the ptl framework */
    rc = PMIx_server_init(&mymodule, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "PMIx_server_init failed: %s\n", PMIx_Error_string(rc));
        return 1;
    }

    fprintf(stdout, "\n=== ptl blocking receive timeout unit test ===\n\n");

    /* nothing ever arrives */
    if (0 != socketpair(AF_UNIX, SOCK_STREAM, 0, sv) || !set_rcvtimeo(sv[0], 1)) {
        report("socketpair with a receive timeout", 0, strerror(errno));
        goto done;
    }
    alarm(20);
    start = now();
    rc = pmix_ptl_base_recv_blocking(sv[0], buf, sizeof(buf));
    alarm(0);
    report("silent peer: the receive times out", PMIX_ERR_TIMEOUT == rc, PMIx_Error_string(rc));
    report("silent peer: after about the timeout", now() - start < 10.0, "took far too long");

    /* part of the message arrives, then nothing */
    if (1 != write(sv[1], "x", 1)) {
        report("partial message sent", 0, strerror(errno));
        goto done;
    }
    alarm(20);
    rc = pmix_ptl_base_recv_blocking(sv[0], buf, sizeof(buf));
    alarm(0);
    report("partial message: the receive times out", PMIX_ERR_TIMEOUT == rc,
           PMIx_Error_string(rc));

    /* all of it arrives - the timeout changes nothing. On a fresh pair:
     * what a timed-out MSG_WAITALL receive leaves in the socket is up to
     * the kernel (macOS hands the byte back to the next read), and the
     * caller closes a socket whose handshake timed out anyway */
    close(sv[0]);
    close(sv[1]);
    if (0 != socketpair(AF_UNIX, SOCK_STREAM, 0, sv) || !set_rcvtimeo(sv[0], 1)) {
        report("socketpair with a receive timeout", 0, strerror(errno));
        goto done;
    }
    if ((ssize_t) sizeof(buf) != write(sv[1], "abcdefgh", sizeof(buf))) {
        report("whole message sent", 0, strerror(errno));
        goto done;
    }
    memset(buf, 0, sizeof(buf));
    alarm(20);
    rc = pmix_ptl_base_recv_blocking(sv[0], buf, sizeof(buf));
    alarm(0);
    report("whole message: received", PMIX_SUCCESS == rc && 0 == memcmp(buf, "abcdefgh", 8),
           PMIx_Error_string(rc));

    /* the peer closes */
    close(sv[1]);
    sv[1] = -1;
    alarm(20);
    rc = pmix_ptl_base_recv_blocking(sv[0], buf, sizeof(buf));
    alarm(0);
    report("closed peer: unreachable, not a timeout", PMIX_ERR_UNREACH == rc,
           PMIx_Error_string(rc));
    close(sv[0]);

    /* On a non-blocking socket EAGAIN still means "not yet", so data
     * that arrives a moment later is still collected */
    if (0 != socketpair(AF_UNIX, SOCK_STREAM, 0, sv)) {
        report("second socketpair", 0, strerror(errno));
        goto done;
    }
    flags = fcntl(sv[0], F_GETFL, 0);
    if (0 > flags || 0 > fcntl(sv[0], F_SETFL, flags | O_NONBLOCK)) {
        report("non-blocking socket", 0, strerror(errno));
        goto done;
    }
    child = fork();
    if (0 > child) {
        report("fork", 0, strerror(errno));
        goto done;
    }
    if (0 == child) {
        usleep(200000);
        if ((ssize_t) sizeof(buf) != write(sv[1], "ijklmnop", sizeof(buf))) {
            _exit(1);
        }
        _exit(0);
    }
    close(sv[1]);
    memset(buf, 0, sizeof(buf));
    alarm(20);
    rc = pmix_ptl_base_recv_blocking(sv[0], buf, sizeof(buf));
    alarm(0);
    report("non-blocking socket: late data still received",
           PMIX_SUCCESS == rc && 0 == memcmp(buf, "ijklmnop", 8), PMIx_Error_string(rc));
    close(sv[0]);
    waitpid(child, &status, 0);

done:
    PMIx_server_finalize();
    fprintf(stdout, "\n%d passed, %d failed\n", npass, nfail);
    return (0 == nfail) ? 0 : 1;
}
