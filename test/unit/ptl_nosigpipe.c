/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Unit test: no PMIx write raises SIGPIPE.
 *
 * Writing to a socket whose peer has reset, or a pipe whose reader has
 * gone, raises SIGPIPE, and the default action of that signal ends the
 * process. The process is the host's or the application's, so its signal
 * disposition is not the library's to set: a reader that vanishes must
 * cost PMIx an error return, never the process. Each case runs in a
 * forked child with SIGPIPE at its default action and must come back
 * with an error rather than die on the signal. Each protection is held to
 * account on its own:
 *
 *   - the library's socket writes pass MSG_NOSIGNAL - tested on a socket
 *     with no per-socket protection;
 *   - a socket the library makes is set SO_NOSIGPIPE where the platform
 *     has it - tested by writing it with a plain send();
 *   - pmix_fd_write_nosigpipe(), which writes the pipe a host may hand a
 *     server for its URI, writes a broken pipe and leaves the thread's
 *     signal mask as it was, as does pmix_fd_write_once_nosigpipe();
 *   - output forwarding to a pipe of the library's own - a child's stdin -
 *     whose reader has gone, in the write handler and in the last flush.
 *
 * And one that must raise it: output forwarded to the process's own
 * stdout is written as the process writes it, so a reader of that which
 * goes ends the process as any other write there would.
 */

#include "src/include/pmix_config.h"

#include "include/pmix.h"
#include "include/pmix_server.h"
#include "src/include/pmix_globals.h"
#include "src/common/pmix_iof.h"
#include "src/mca/ptl/base/base.h"
#include "src/util/pmix_fd.h"

#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/wait.h>
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

/* A connected TCP socket whose peer has reset: the peer closes with a
 * zero linger, which sends a reset instead of a FIN. Returns the
 * surviving end, or -1. With `library`, the surviving end is made by
 * pmix_ptl_base_connect(), as every connection PMIx opens is. */
static int reset_peer_socket(bool library)
{
    struct sockaddr_in addr;
    struct sockaddr_storage ss;
    socklen_t len = sizeof(addr);
    struct linger lg;
    int lsd, csd = -1, asd;

    lsd = socket(AF_INET, SOCK_STREAM, 0);
    if (0 > lsd) {
        return -1;
    }
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (0 != bind(lsd, (struct sockaddr *) &addr, sizeof(addr)) || 0 != listen(lsd, 1) ||
        0 != getsockname(lsd, (struct sockaddr *) &addr, &len)) {
        close(lsd);
        return -1;
    }
    if (library) {
        memset(&ss, 0, sizeof(ss));
        memcpy(&ss, &addr, sizeof(addr));
        if (PMIX_SUCCESS != pmix_ptl_base_connect(&ss, sizeof(addr), &csd)) {
            close(lsd);
            return -1;
        }
    } else {
        csd = socket(AF_INET, SOCK_STREAM, 0);
        if (0 > csd || 0 != connect(csd, (struct sockaddr *) &addr, sizeof(addr))) {
            close(lsd);
            return -1;
        }
    }
    asd = accept(lsd, NULL, NULL);
    close(lsd);
    if (0 > asd) {
        close(csd);
        return -1;
    }
    lg.l_onoff = 1;
    lg.l_linger = 0;
    (void) setsockopt(asd, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg));
    close(asd);
    /* let the reset arrive */
    usleep(200000);
    return csd;
}

/* What each child does: 0 = every write failed with an error, as it
 * should; anything else = a write it could not account for */
static int write_blocking(int sd)
{
    char buf[1024];
    int i;

    memset(buf, 'x', sizeof(buf));
    /* the first write after a reset may still be accepted by the kernel,
     * so write until one fails */
    for (i = 0; i < 64; i++) {
        if (PMIX_SUCCESS != pmix_ptl_base_send_blocking(sd, buf, sizeof(buf))) {
            return 0;
        }
    }
    return 2;
}

static int write_vector(int sd)
{
    char hdr[16], body[1024];
    struct iovec iov[2];
    int i;

    memset(hdr, 'h', sizeof(hdr));
    memset(body, 'b', sizeof(body));
    for (i = 0; i < 64; i++) {
        iov[0].iov_base = hdr;
        iov[0].iov_len = sizeof(hdr);
        iov[1].iov_base = body;
        iov[1].iov_len = sizeof(body);
        if (0 > pmix_ptl_base_writev(sd, iov, 2)) {
            return 0;
        }
    }
    return 2;
}

#ifdef SO_NOSIGPIPE
/* a plain send() - only the socket's own setting protects it, and only a
 * platform with SO_NOSIGPIPE has one */
static int write_plain(int sd)
{
    char buf[1024];
    int i;

    memset(buf, 'p', sizeof(buf));
    for (i = 0; i < 64; i++) {
        if (0 > send(sd, buf, sizeof(buf), 0)) {
            return 0;
        }
    }
    return 2;
}
#endif

/* a pipe whose reader has gone */
static bool broken_pipe(int *wfd)
{
    int p[2];

    if (0 != pipe(p)) {
        return false;
    }
    close(p[0]);
    *wfd = p[1];
    return true;
}

static int pipe_from_this_thread(int unused)
{
    sigset_t before, after;
    int fd;
    PMIX_HIDE_UNUSED_PARAMS(unused);

    if (!broken_pipe(&fd)) {
        return 3;
    }
    pthread_sigmask(SIG_SETMASK, NULL, &before);
    if (PMIX_ERR_IN_ERRNO != pmix_fd_write_nosigpipe(fd, 1, "x") || EPIPE != errno) {
        return 2;
    }
    pthread_sigmask(SIG_SETMASK, NULL, &after);
    /* the mask is as it was - SIGPIPE unblocked again */
    return (sigismember(&before, SIGPIPE) == sigismember(&after, SIGPIPE)) ? 0 : 4;
}

static int pipe_once_from_this_thread(int unused)
{
    sigset_t before, after;
    int fd;
    PMIX_HIDE_UNUSED_PARAMS(unused);

    if (!broken_pipe(&fd)) {
        return 3;
    }
    pthread_sigmask(SIG_SETMASK, NULL, &before);
    if (-1 != pmix_fd_write_once_nosigpipe(fd, "x", 1) || EPIPE != errno) {
        return 2;
    }
    pthread_sigmask(SIG_SETMASK, NULL, &after);
    return (sigismember(&before, SIGPIPE) == sigismember(&after, SIGPIPE)) ? 0 : 4;
}

/* a sink writing `fd`, with one line queued */
static void sink_on(pmix_iof_sink_t *snk, int fd)
{
    pmix_iof_write_output_t *output;

    PMIX_CONSTRUCT(snk, pmix_iof_sink_t);
    snk->wev.fd = fd;
    output = PMIX_NEW(pmix_iof_write_output_t);
    output->data = strdup("forwarded\n");
    output->numbytes = (int) strlen(output->data);
    pmix_list_append(&snk->wev.outputs, &output->super);
}

/* the write handler, and then the last flush, on a pipe of our own */
static int iof_own_pipe(int unused)
{
    pmix_iof_sink_t snk;
    int fd;
    PMIX_HIDE_UNUSED_PARAMS(unused);

    if (!broken_pipe(&fd)) {
        return 3;
    }
    if (2 >= fd) {
        return 3;
    }
    sink_on(&snk, fd);
    snk.wev.pending = true;
    pmix_iof_write_handler(fd, 0, &snk);
    if (snk.wev.pending) {
        /* the write was not taken as failed */
        return 2;
    }
    sink_on(&snk, fd);
    pmix_iof_static_dump_output(&snk);
    return 0;
}

/* the write handler on our stdout, whose reader has gone */
static int iof_own_stdout(int unused)
{
    pmix_iof_sink_t snk;
    int fd;
    PMIX_HIDE_UNUSED_PARAMS(unused);

    if (!broken_pipe(&fd) || 0 > dup2(fd, 1)) {
        return 3;
    }
    sink_on(&snk, 1);
    snk.wev.pending = true;
    pmix_iof_write_handler(1, 0, &snk);
    /* still here: it did not raise SIGPIPE */
    return 2;
}

/* a case that must end on SIGPIPE */
static void run_dies_case(const char *name, int (*fn)(int))
{
    pid_t child;
    int status = 0;
    char detail[128];

    child = fork();
    if (0 > child) {
        report(name, 0, strerror(errno));
        return;
    }
    if (0 == child) {
        signal(SIGPIPE, SIG_DFL);
        alarm(30);
        _exit(fn(-1));
    }
    if (child != waitpid(child, &status, 0)) {
        report(name, 0, "waitpid failed");
        return;
    }
    if (WIFSIGNALED(status)) {
        snprintf(detail, sizeof(detail), "the process died on signal %d", WTERMSIG(status));
        report(name, SIGPIPE == WTERMSIG(status), detail);
        return;
    }
    snprintf(detail, sizeof(detail), "the process exited %d",
             WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    report(name, 0, detail);
}

static void run_case(const char *name, int (*fn)(int), int kind)
{
    pid_t child;
    int status = 0, sd = -1;
    char detail[128];

    child = fork();
    if (0 > child) {
        report(name, 0, strerror(errno));
        return;
    }
    if (0 == child) {
        signal(SIGPIPE, SIG_DFL);
        alarm(30);
        if (0 != kind) {
            /* a socket, made by the test (1) or by the library (2) */
            sd = reset_peer_socket(2 == kind);
            if (0 > sd) {
                _exit(3);
            }
        }
        _exit(fn(sd));
    }
    if (child != waitpid(child, &status, 0)) {
        report(name, 0, "waitpid failed");
        return;
    }
    if (WIFSIGNALED(status)) {
        snprintf(detail, sizeof(detail), "the process died on signal %d%s", WTERMSIG(status),
                 (SIGPIPE == WTERMSIG(status)) ? " (SIGPIPE)" : "");
        report(name, 0, detail);
        return;
    }
    if (3 == WEXITSTATUS(status)) {
        report(name, 0, "could not set up a reset connection");
        return;
    }
    report(name, 0 == WEXITSTATUS(status),
           (4 == WEXITSTATUS(status)) ? "the thread's signal mask was changed"
                                      : "the write did not fail as it should");
}

int main(int argc, char **argv)
{
    static pmix_server_module_t mymodule = {0};
    pmix_status_t rc;
    PMIX_HIDE_UNUSED_PARAMS(argc, argv);

    setvbuf(stdout, NULL, _IONBF, 0);

    /* the routines emit verbose output through the ptl framework */
    rc = PMIx_server_init(&mymodule, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "PMIx_server_init failed: %s\n", PMIx_Error_string(rc));
        return 1;
    }

    fprintf(stdout, "\n=== ptl writes never raise SIGPIPE ===\n\n");

    run_case("blocking send to a reset peer returns an error", write_blocking, 1);
    run_case("vector write to a reset peer returns an error", write_vector, 1);
#ifdef SO_NOSIGPIPE
    run_case("a socket the library made does not raise SIGPIPE on a plain send", write_plain, 2);
#endif
    run_case("pmix_fd_write_nosigpipe writes a broken pipe and leaves the mask as it was",
             pipe_from_this_thread, 0);
    run_case("pmix_fd_write_once_nosigpipe writes a broken pipe and leaves the mask as it was",
             pipe_once_from_this_thread, 0);
    run_case("output forwarded to a pipe of ours whose reader has gone returns an error",
             iof_own_pipe, 0);
    run_dies_case("output forwarded to our own stdout whose reader has gone raises SIGPIPE",
                  iof_own_stdout);

    PMIx_server_finalize();

    fprintf(stdout, "\n%d passed, %d failed\n", npass, nfail);
    return (0 == nfail) ? 0 : 1;
}
