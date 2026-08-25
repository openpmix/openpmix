/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Unit tests for the pmix_fd utilities:
 *   pmix_fd_read / pmix_fd_write over a pipe (round trip, EOF, degenerate
 *   lengths), pmix_fd_set_cloexec, the fstat type predicates,
 *   pmix_fd_get_peer_name over real sockets, pmix_fd_dup2, and
 *   pmix_close_open_file_descriptors.
 *
 * The last of those closes every descriptor it can find, so it is
 * exercised in a forked child that reports its findings back over the
 * one descriptor it was told to protect - running it in-process would
 * take the test harness's own descriptors down with it.
 *
 * No PMIx_Init: everything here is a leaf utility over raw descriptors.
 *
 * Exit 0 if all tests pass, 1 otherwise.
 */

#include "src/include/pmix_config.h"
#include "src/include/pmix_globals.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#include "src/util/pmix_fd.h"

static int npass = 0;
static int nfail = 0;

static void report(const char *name, int passed)
{
    if (passed) {
        fprintf(stdout, "  PASS: %s\n", name);
        npass++;
    } else {
        fprintf(stdout, "  FAIL: %s\n", name);
        nfail++;
    }
}

/* ------------------------------------------------------------------ */
/* pmix_fd_read / pmix_fd_write                                        */
/* ------------------------------------------------------------------ */

static void test_read_write_roundtrip(void)
{
    /* larger than a pipe's atomic write, so both loops go around more
     * than once and the b += rc / l2 -= rc bookkeeping is actually
     * exercised rather than being a single pass */
    const int len = 256 * 1024;
    char *out, *in;
    int p[2];
    pid_t child;
    pmix_status_t rc;
    int n, ok = 1;

    out = malloc(len);
    in = calloc(1, len);
    if (NULL == out || NULL == in || 0 != pipe(p)) {
        report("read_write_roundtrip: fixture", 0);
        free(out);
        free(in);
        return;
    }
    for (n = 0; n < len; n++) {
        out[n] = (char) (n & 0xff);
    }

    /* a pipe holds far less than len, so the writer has to be a
     * separate process or it deadlocks against itself */
    child = fork();
    if (0 == child) {
        close(p[0]);
        rc = pmix_fd_write(p[1], len, out);
        close(p[1]);
        _exit(PMIX_SUCCESS == rc ? 0 : 1);
    }
    close(p[1]);
    rc = pmix_fd_read(p[0], len, in);
    close(p[0]);
    report("fd_read: full buffer returns SUCCESS", PMIX_SUCCESS == rc);

    if (0 < child) {
        int wstatus = 0;
        waitpid(child, &wstatus, 0);
        report("fd_write: full buffer returns SUCCESS",
               WIFEXITED(wstatus) && 0 == WEXITSTATUS(wstatus));
    }

    for (n = 0; n < len; n++) {
        if (in[n] != out[n]) {
            ok = 0;
            break;
        }
    }
    report("fd_read/fd_write: bytes survive the round trip", ok);

    free(out);
    free(in);
}

static void test_read_eof(void)
{
    int p[2];
    char buf[4];
    pmix_status_t rc;

    if (0 != pipe(p)) {
        report("read_eof: fixture", 0);
        return;
    }
    close(p[1]);
    /* the documented answer for "the fd closed before we had it all" */
    rc = pmix_fd_read(p[0], sizeof(buf), buf);
    close(p[0]);
    report("fd_read: short read reports PMIX_ERR_TIMEOUT", PMIX_ERR_TIMEOUT == rc);
}

static void test_read_partial_then_eof(void)
{
    int p[2];
    char buf[8];
    pmix_status_t rc;

    if (0 != pipe(p)) {
        report("read_partial_then_eof: fixture", 0);
        return;
    }
    if (2 != write(p[1], "hi", 2)) {
        report("read_partial_then_eof: fixture", 0);
        close(p[0]);
        close(p[1]);
        return;
    }
    close(p[1]);
    /* fewer bytes than asked for, then EOF - still TIMEOUT, not SUCCESS */
    rc = pmix_fd_read(p[0], sizeof(buf), buf);
    close(p[0]);
    report("fd_read: partial then EOF reports PMIX_ERR_TIMEOUT", PMIX_ERR_TIMEOUT == rc);
}

static void test_degenerate_lengths(void)
{
    char buf[1];

    /* both are answered without ever touching the descriptor, so a
     * closed fd is a fine thing to hand them here */
    report("fd_read: len 0 succeeds without touching the fd",
           PMIX_SUCCESS == pmix_fd_read(-1, 0, buf));
    report("fd_write: len 0 succeeds without touching the fd",
           PMIX_SUCCESS == pmix_fd_write(-1, 0, buf));
    report("fd_read: negative len is BAD_PARAM",
           PMIX_ERR_BAD_PARAM == pmix_fd_read(-1, -1, buf));
    report("fd_write: negative len is BAD_PARAM",
           PMIX_ERR_BAD_PARAM == pmix_fd_write(-1, -1, buf));
}

static void test_write_to_closed_fd(void)
{
    char buf[1] = {'x'};
    /* EBADF is not EAGAIN/EINTR, so this must come straight back out
     * rather than spinning */
    report("fd_write: a bad fd is IN_ERRNO, not a spin",
           PMIX_ERR_IN_ERRNO == pmix_fd_write(-1, 1, buf));
    report("fd_read: a bad fd is IN_ERRNO, not a spin",
           PMIX_ERR_IN_ERRNO == pmix_fd_read(-1, 1, buf));
}

/* ------------------------------------------------------------------ */
/* pmix_fd_set_cloexec                                                 */
/* ------------------------------------------------------------------ */

static void test_set_cloexec(void)
{
    int p[2];
    int flags;

    if (0 != pipe(p)) {
        report("set_cloexec: fixture", 0);
        return;
    }
    report("set_cloexec: returns SUCCESS", PMIX_SUCCESS == pmix_fd_set_cloexec(p[0]));
    flags = fcntl(p[0], F_GETFD, 0);
    report("set_cloexec: FD_CLOEXEC is actually set", 0 <= flags && 0 != (flags & FD_CLOEXEC));
    /* the other end must be untouched */
    flags = fcntl(p[1], F_GETFD, 0);
    report("set_cloexec: leaves other descriptors alone",
           0 <= flags && 0 == (flags & FD_CLOEXEC));
    close(p[0]);
    close(p[1]);

    report("set_cloexec: a bad fd is IN_ERRNO", PMIX_ERR_IN_ERRNO == pmix_fd_set_cloexec(-1));
}

/* ------------------------------------------------------------------ */
/* the fstat type predicates                                           */
/* ------------------------------------------------------------------ */

static void test_type_predicates(void)
{
    char tmpl[] = "/tmp/pmix_util_fd_XXXXXX";
    int fd, devnull, p[2];

    fd = mkstemp(tmpl);
    if (0 > fd) {
        report("type_predicates: fixture", 0);
        return;
    }
    unlink(tmpl);
    report("is_regular: true for a regular file", pmix_fd_is_regular(fd));
    report("is_chardev: false for a regular file", !pmix_fd_is_chardev(fd));
    report("is_blkdev: false for a regular file", !pmix_fd_is_blkdev(fd));
    close(fd);

    devnull = open("/dev/null", O_RDWR);
    if (0 <= devnull) {
        report("is_chardev: true for /dev/null", pmix_fd_is_chardev(devnull));
        report("is_regular: false for /dev/null", !pmix_fd_is_regular(devnull));
        report("is_blkdev: false for /dev/null", !pmix_fd_is_blkdev(devnull));
        close(devnull);
    }

    if (0 == pipe(p)) {
        report("is_regular: false for a pipe", !pmix_fd_is_regular(p[0]));
        report("is_chardev: false for a pipe", !pmix_fd_is_chardev(p[0]));
        close(p[0]);
        close(p[1]);
    }

    /* an fd that cannot be fstat'd answers false rather than reporting
     * the error - all three of them */
    report("is_regular: false for a bad fd", !pmix_fd_is_regular(-1));
    report("is_chardev: false for a bad fd", !pmix_fd_is_chardev(-1));
    report("is_blkdev: false for a bad fd", !pmix_fd_is_blkdev(-1));
}

/* ------------------------------------------------------------------ */
/* pmix_fd_get_peer_name                                               */
/* ------------------------------------------------------------------ */

static void test_peer_name_not_a_socket(void)
{
    int p[2];
    const char *name;

    if (0 != pipe(p)) {
        report("peer_name_not_a_socket: fixture", 0);
        return;
    }
    /* getpeername fails on a pipe.  The caller is promised a printable
     * string either way - a NULL here is a crash in whatever prints it */
    name = pmix_fd_get_peer_name(p[0]);
    report("get_peer_name: non-socket is named, not NULL", NULL != name);
    report("get_peer_name: non-socket reads as Unknown",
           NULL != name && 0 == strcmp("Unknown", name));
    close(p[0]);
    close(p[1]);

    name = pmix_fd_get_peer_name(-1);
    report("get_peer_name: bad fd is named, not NULL", NULL != name);
}

static void test_peer_name_unix_socket(void)
{
    int sv[2];
    const char *name;

    if (0 != socketpair(AF_UNIX, SOCK_STREAM, 0, sv)) {
        report("peer_name_unix_socket: fixture", 0);
        return;
    }
    /* getpeername succeeds, but AF_UNIX is not a family this renders -
     * the family fall-through must still produce a string */
    name = pmix_fd_get_peer_name(sv[0]);
    report("get_peer_name: unrendered family is named, not NULL", NULL != name);
    report("get_peer_name: unrendered family reads as Unknown",
           NULL != name && 0 == strcmp("Unknown", name));
    close(sv[0]);
    close(sv[1]);
}

static void test_peer_name_loopback(void)
{
    int lsock = -1, csock = -1, asock = -1;
    struct sockaddr_in sin;
    socklen_t slen = sizeof(sin);
    const char *name;

    lsock = socket(AF_INET, SOCK_STREAM, 0);
    if (0 > lsock) {
        report("peer_name_loopback: fixture (no AF_INET)", 0);
        return;
    }
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port = 0; /* let the kernel choose */
    sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (0 != bind(lsock, (struct sockaddr *) &sin, sizeof(sin)) || 0 != listen(lsock, 1)
        || 0 != getsockname(lsock, (struct sockaddr *) &sin, &slen)) {
        report("peer_name_loopback: fixture (bind/listen)", 0);
        close(lsock);
        return;
    }
    csock = socket(AF_INET, SOCK_STREAM, 0);
    if (0 > csock || 0 != connect(csock, (struct sockaddr *) &sin, sizeof(sin))) {
        report("peer_name_loopback: fixture (connect)", 0);
        close(lsock);
        if (0 <= csock) {
            close(csock);
        }
        return;
    }
    asock = accept(lsock, NULL, NULL);

    /* the one path that actually renders an address */
    name = pmix_fd_get_peer_name(csock);
    report("get_peer_name: loopback peer renders as 127.0.0.1",
           NULL != name && 0 == strcmp("127.0.0.1", name));

    if (0 <= asock) {
        name = pmix_fd_get_peer_name(asock);
        report("get_peer_name: accepted end renders as 127.0.0.1",
               NULL != name && 0 == strcmp("127.0.0.1", name));
        close(asock);
    }
    close(csock);
    close(lsock);
}

/* ------------------------------------------------------------------ */
/* pmix_fd_dup2                                                        */
/* ------------------------------------------------------------------ */

static void test_dup2(void)
{
    int p[2];
    char buf[4];
    int target = 37;

    if (0 != pipe(p)) {
        report("dup2: fixture", 0);
        return;
    }
    /* a target above 2 is passed straight to dup2 */
    report("fd_dup2: returns the target it was given", target == pmix_fd_dup2(p[0], target));
    if (2 != write(p[1], "ok", 2)) {
        report("dup2: fixture (write)", 0);
    } else {
        memset(buf, 0, sizeof(buf));
        report("fd_dup2: the duplicate reads the same pipe",
               2 == read(target, buf, 2) && 0 == strcmp("ok", buf));
    }
    close(target);
    close(p[0]);
    close(p[1]);

    report("fd_dup2: a bad source is -1", -1 == pmix_fd_dup2(-1, target));
}

/* ------------------------------------------------------------------ */
/* pmix_close_open_file_descriptors                                    */
/* ------------------------------------------------------------------ */

/* what the child reports back to us */
typedef struct {
    int nclosed;   /* how many of the fds it opened are now closed */
    int nopened;   /* how many it opened */
    int protected_alive;
    int std_alive; /* 0, 1 and 2 all still open */
} closed_report_t;

#define NEXTRA 12

static void run_close_child(int report_fd)
{
    int extra[NEXTRA];
    closed_report_t rep;
    int n;

    memset(&rep, 0, sizeof(rep));

    for (n = 0; n < NEXTRA; n++) {
        extra[n] = open("/dev/null", O_RDONLY);
        if (0 <= extra[n]) {
            rep.nopened++;
        }
    }

    /* Seed a stale errno of exactly the kind the scan tests for.  strtol
     * only ever sets errno, it never clears it, so a scan that reads
     * errno without zeroing it first abandons the directory listing here
     * and falls back to closing by bound instead.  Either route has to
     * end with the same descriptors closed. */
    errno = ERANGE;

    pmix_close_open_file_descriptors(report_fd);

    for (n = 0; n < NEXTRA; n++) {
        if (0 <= extra[n] && 0 > fcntl(extra[n], F_GETFD, 0)) {
            rep.nclosed++;
        }
    }
    rep.protected_alive = (0 <= fcntl(report_fd, F_GETFD, 0));
    rep.std_alive = (0 <= fcntl(0, F_GETFD, 0)) && (0 <= fcntl(1, F_GETFD, 0))
                    && (0 <= fcntl(2, F_GETFD, 0));

    (void) pmix_fd_write(report_fd, sizeof(rep), &rep);
    close(report_fd);
    _exit(0);
}

static void test_close_open_file_descriptors(void)
{
    int p[2];
    pid_t child;
    closed_report_t rep;
    pmix_status_t rc;
    int wstatus = 0;

    if (0 != pipe(p)) {
        report("close_open_fds: fixture", 0);
        return;
    }
    child = fork();
    if (0 > child) {
        report("close_open_fds: fixture (fork)", 0);
        close(p[0]);
        close(p[1]);
        return;
    }
    if (0 == child) {
        close(p[0]);
        run_close_child(p[1]);
        /* not reached */
    }
    close(p[1]);
    memset(&rep, 0, sizeof(rep));
    rc = pmix_fd_read(p[0], sizeof(rep), &rep);
    close(p[0]);
    waitpid(child, &wstatus, 0);

    if (PMIX_SUCCESS != rc) {
        report("close_open_fds: child reported back", 0);
        return;
    }
    report("close_open_fds: child opened its fixtures", NEXTRA == rep.nopened);
    /* the whole point of the call.  A bound that truncated to a
     * negative int, or a scan abandoned on a stale errno, shows up here
     * as descriptors still open */
    report("close_open_fds: every unprotected fd was closed", rep.nclosed == rep.nopened);
    report("close_open_fds: the protected fd survived", 0 != rep.protected_alive);
    report("close_open_fds: stdin/stdout/stderr survived", 0 != rep.std_alive);
    /* NOTE: this exercises whichever route the host OS offers, which on
     * both Linux and macOS is the directory scan.  The close-by-bound
     * fallback is only taken where there is no /proc/self/fd or /dev/fd
     * to read, so a unit test cannot reach it here; it was verified by
     * hand by pointing the opendir at a path that does not exist. */
}

/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    PMIX_HIDE_UNUSED_PARAMS(argc, argv);

    fprintf(stdout, "\n=== pmix_fd unit tests ===\n\n");

    test_read_write_roundtrip();
    test_read_eof();
    test_read_partial_then_eof();
    test_degenerate_lengths();
    test_write_to_closed_fd();
    test_set_cloexec();
    test_type_predicates();
    test_peer_name_not_a_socket();
    test_peer_name_unix_socket();
    test_peer_name_loopback();
    test_dup2();
    test_close_open_file_descriptors();

    fprintf(stdout, "\nResults: %d passed, %d failed\n\n", npass, nfail);

    return (nfail > 0) ? 1 : 0;
}
