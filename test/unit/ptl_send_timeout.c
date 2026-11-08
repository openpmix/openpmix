/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Unit test: the blocking sends of a connect end at
 * ptl_base_handshake_wait_time, as its receives do.
 *
 * A process connecting to a server writes its offer, then its credential
 * (which carries the directives it passes the server), with
 * pmix_ptl_base_send_blocking on a socket in blocking mode. Once a
 * message is larger than the socket can buffer, the send waits for the
 * server to read.
 *
 *   connect   - a tool connects to a stand-in server on the local socket
 *               that answers the offer and then reads nothing more. The
 *               tool's credential carries a directive far larger than any
 *               socket buffer. Its init must end once the handshake wait
 *               has passed.
 *   timed out - pmix_ptl_base_send_blocking on a blocking socket with a
 *               send timeout, to a peer that never reads, returns
 *               PMIX_ERR_TIMEOUT.
 *   no bound  - with no send timeout - as a server's replies are sent - a
 *               send to a peer that reads late completes.
 */

#include "src/include/pmix_config.h"

#include "include/pmix.h"
#include "include/pmix_server.h"
#include "include/pmix_tool.h"
#include "src/include/pmix_globals.h"
#include "src/mca/ptl/base/base.h"
#include "src/mca/ptl/ptl_types.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define BIG (16 * 1024 * 1024)

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
    static const char msg[] = "  FAIL: send_blocking kept waiting after its send timeout "
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

static bool read_all(int sd, void *buf, size_t len)
{
    size_t got = 0;
    ssize_t n;

    while (got < len) {
        n = read(sd, (char *) buf + got, len - got);
        if (0 >= n) {
            return false;
        }
        got += (size_t) n;
    }
    return true;
}

/* ---------- connect: a tool, against a server that stops reading ---------- */

/* the tool: exits 0 if init failed (as it must), 1 if it succeeded, and
 * dies on its alarm if it never returns */
static int run_tool(const char *uri)
{
    pmix_proc_t me;
    pmix_info_t info[2];
    pmix_status_t rc;
    char *big;

    alarm(30);
    big = malloc(BIG + 1);
    if (NULL == big) {
        return 2;
    }
    memset(big, 'x', BIG);
    big[BIG] = '\0';
    PMIX_INFO_LOAD(&info[0], PMIX_SERVER_URI, uri, PMIX_STRING);
    PMIX_INFO_LOAD(&info[1], "ptl.send.timeout.big", big, PMIX_STRING);
    free(big);
    rc = PMIx_tool_init(&me, info, 2);
    fprintf(stdout, "  (tool init: %s)\n", PMIx_Error_string(rc));
    return (PMIX_SUCCESS == rc) ? 1 : 0;
}

/* the server: take the offer, answer it, then read nothing */
static void check_connect(void)
{
    char dir[] = "/tmp/pmix-sndto.XXXXXX";
    char path[256], uri[512], choice[64];
    struct sockaddr_un sa;
    pmix_ptl_hdr_t hdr;
    char *offer = NULL;
    int lsd = -1, sd = -1, status = 0;
    size_t len, clen;
    uint32_t u32;
    pid_t child;
    double start;
    char detail[256];

    if (NULL == mkdtemp(dir)) {
        report("connect: make a directory", 0, strerror(errno));
        return;
    }
    snprintf(path, sizeof(path), "%s/s", dir);
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof(sa.sun_path)) {
        report("connect: a socket path that fits", 0, path);
        goto cleanup;
    }
    memcpy(sa.sun_path, path, strlen(path) + 1);
    lsd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (0 > lsd || 0 != bind(lsd, (struct sockaddr *) &sa, sizeof(sa)) || 0 != listen(lsd, 4)) {
        report("connect: listen on a local socket", 0, strerror(errno));
        goto cleanup;
    }
    snprintf(uri, sizeof(uri), "ptl.send.timeout.0;usock://%s", path);

    start = now();
    child = fork();
    if (0 > child) {
        report("connect: fork", 0, strerror(errno));
        goto cleanup;
    }
    if (0 == child) {
        close(lsd);
        setenv("PMIX_MCA_ptl_base_handshake_wait_time", "2", 1);
        setenv("PMIX_BFROP_BUFFER_TYPE", "PMIX_BFROP_BUFFER_FULLY_DESC", 1);
        _exit(run_tool(uri));
    }

    alarm(40);
    sd = accept(lsd, NULL, NULL);
    if (0 > sd) {
        report("connect: the tool connects", 0, strerror(errno));
        goto reap;
    }
    /* the offer: a header, then its payload */
    if (!read_all(sd, &hdr, sizeof(hdr)) || 0 == hdr.nbytes || 65536 < hdr.nbytes ||
        NULL == (offer = malloc(hdr.nbytes)) || !read_all(sd, offer, hdr.nbytes)) {
        report("connect: the tool sends its offer", 0, "no offer");
        goto reap;
    }
    /* the choice: native, v70, a fully described buffer, hash */
    len = 0;
    memcpy(choice + 8 + len, "native", 7);
    len += 7;
    memcpy(choice + 8 + len, "v70", 4);
    len += 4;
    choice[8 + len] = (char) PMIX_BFROP_BUFFER_FULLY_DESC;
    len += 1;
    memcpy(choice + 8 + len, "hash", 5);
    len += 5;
    u32 = htonl((uint32_t) PMIX_SUCCESS);
    memcpy(choice, &u32, 4);
    u32 = htonl((uint32_t) len);
    memcpy(choice + 4, &u32, 4);
    clen = 8 + len;
    if ((ssize_t) clen != write(sd, choice, clen)) {
        report("connect: answer the offer", 0, strerror(errno));
        goto reap;
    }
    /* and read nothing more */

reap:
    waitpid(child, &status, 0);
    alarm(0);
    snprintf(detail, sizeof(detail), "%s after %.1f s",
             WIFSIGNALED(status) ? "the tool was still waiting"
             : (WIFEXITED(status) && 1 == WEXITSTATUS(status)) ? "init succeeded"
             : (WIFEXITED(status) && 0 == WEXITSTATUS(status)) ? "init failed"
                                                               : "the tool could not set up",
             now() - start);
    report("connect: a tool's init ends when a server stops reading its credential",
           WIFEXITED(status) && 0 == WEXITSTATUS(status) && now() - start < 25.0, detail);

cleanup:
    free(offer);
    if (0 <= sd) {
        close(sd);
    }
    if (0 <= lsd) {
        close(lsd);
    }
    unlink(path);
    rmdir(dir);
}

/* ---------- send_blocking itself ---------- */

static bool set_sndtimeo(int sd, int sec)
{
    struct timeval tv;

    tv.tv_sec = sec;
    tv.tv_usec = 0;
    return 0 == setsockopt(sd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

static void check_send_blocking(void)
{
    pmix_status_t rc;
    int sv[2], status;
    pid_t child;
    char *buf, *rbuf;
    size_t len = 4 * 1024 * 1024;
    double start;

    buf = calloc(1, len);
    if (NULL == buf) {
        report("allocate a message", 0, "no memory");
        return;
    }

    /* a peer that never reads */
    if (0 != socketpair(AF_UNIX, SOCK_STREAM, 0, sv) || !set_sndtimeo(sv[0], 1)) {
        report("socketpair with a send timeout", 0, strerror(errno));
        free(buf);
        return;
    }
    signal(SIGALRM, stuck);
    alarm(20);
    start = now();
    rc = pmix_ptl_base_send_blocking(sv[0], buf, len);
    alarm(0);
    report("timed out: the send times out", PMIX_ERR_TIMEOUT == rc, PMIx_Error_string(rc));
    report("timed out: after about the timeout", now() - start < 10.0, "took far too long");
    close(sv[0]);
    close(sv[1]);

    /* no send timeout, and a peer that reads late */
    if (0 != socketpair(AF_UNIX, SOCK_STREAM, 0, sv)) {
        report("second socketpair", 0, strerror(errno));
        free(buf);
        return;
    }
    child = fork();
    if (0 > child) {
        report("fork", 0, strerror(errno));
        free(buf);
        return;
    }
    if (0 == child) {
        close(sv[0]);
        rbuf = malloc(len);
        usleep(300000);
        _exit((NULL != rbuf && read_all(sv[1], rbuf, len)) ? 0 : 1);
    }
    close(sv[1]);
    alarm(20);
    rc = pmix_ptl_base_send_blocking(sv[0], buf, len);
    alarm(0);
    report("no bound: a send to a peer that reads late completes", PMIX_SUCCESS == rc,
           PMIx_Error_string(rc));
    close(sv[0]);
    waitpid(child, &status, 0);
    free(buf);
}

int main(int argc, char **argv)
{
    static pmix_server_module_t mymodule = {0};
    pmix_status_t rc;
    PMIX_HIDE_UNUSED_PARAMS(argc, argv);

    setvbuf(stdout, NULL, _IONBF, 0);
    signal(SIGPIPE, SIG_IGN);

    fprintf(stdout, "\n=== ptl blocking send timeout unit test ===\n\n");

    /* forks a tool, so before anything here initializes */
    check_connect();

    /* the routine emits verbose output through the ptl framework */
    rc = PMIx_server_init(&mymodule, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "PMIx_server_init failed: %s\n", PMIx_Error_string(rc));
        return 1;
    }
    check_send_blocking();
    PMIx_server_finalize();

    fprintf(stdout, "\n%d passed, %d failed\n", npass, nfail);
    return (0 == nfail) ? 0 : 1;
}
