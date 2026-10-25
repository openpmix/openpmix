/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * What a tool or client makes of a server it reached over TCP on this
 * host, by what the kernel's table of TCP connections says of the
 * server's end (pmix_ptl_base_check_server).
 *
 * The check runs the moment our end is connected - and our end is
 * connected once the server's SYN-ACK arrives, a moment before the
 * server's kernel has taken our ACK and made its end an established
 * connection. A check that looked once refused a real server now and then
 * (on macOS CI). So: connect to a listener of our own,
 * check it the instant the socket is writable, many times over - it must
 * be ours every time.
 *
 * And the waiting is bounded: a connection whose server end is no longer
 * an established one - the server has closed it - is refused, and soon.
 *
 * Where there is no table to ask (pmix_util_getid_test_no_tcp_table), a
 * server is refused unless its file vouches for it, and taken if it does.
 */

#include "src/include/pmix_config.h"

#include "include/pmix_server.h"
#include "src/include/pmix_globals.h"
#include "src/mca/ptl/base/base.h"
#include "src/util/pmix_getid.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define NCONNECTS 2000

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

static pmix_server_module_t mymodule = {0};

/* a listener on 127.0.0.1, at a port of the kernel's choosing */
static int listen_lo(struct sockaddr_in *addr)
{
    socklen_t len = sizeof(*addr);
    int sd;

    sd = socket(AF_INET, SOCK_STREAM, 0);
    if (0 > sd) {
        return -1;
    }
    memset(addr, 0, sizeof(*addr));
    addr->sin_family = AF_INET;
    addr->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (0 != bind(sd, (struct sockaddr *) addr, sizeof(*addr)) ||
        0 != listen(sd, 128) ||
        0 != getsockname(sd, (struct sockaddr *) addr, &len)) {
        close(sd);
        return -1;
    }
    return sd;
}

/* connect without blocking, and return as soon as the socket is
 * writable - where cnct_connected checks the server */
static int connect_now(const struct sockaddr_in *addr)
{
    struct pollfd pfd;
    int sd, err = 0;
    socklen_t errlen = sizeof(err);

    sd = socket(AF_INET, SOCK_STREAM, 0);
    if (0 > sd) {
        return -1;
    }
    fcntl(sd, F_SETFL, fcntl(sd, F_GETFL) | O_NONBLOCK);
    if (0 != connect(sd, (const struct sockaddr *) addr, sizeof(*addr))) {
        if (EINPROGRESS != errno) {
            close(sd);
            return -1;
        }
        pfd.fd = sd;
        pfd.events = POLLOUT;
        pfd.revents = 0;
        if (1 != poll(&pfd, 1, 5000) ||
            0 != getsockopt(sd, SOL_SOCKET, SO_ERROR, &err, &errlen) || 0 != err) {
            close(sd);
            return -1;
        }
    }
    return sd;
}

static pmix_status_t check_sd_vouched(int sd, bool vouched)
{
    pmix_peer_t *peer;
    pmix_status_t rc;

    peer = PMIX_NEW(pmix_peer_t);
    peer->sd = sd; /* closed when the peer is released */
    peer->transport = NULL;
    rc = pmix_ptl_base_check_server(peer, false, 0, 0, vouched);
    PMIX_RELEASE(peer);
    return rc;
}

static pmix_status_t check_sd(int sd)
{
    return check_sd_vouched(sd, false);
}

/* with no table to ask: refused, unless vouched for */
static void test_no_table(const struct sockaddr_in *addr, int ls)
{
    pmix_status_t rc;
    int sd, as;

    pmix_util_getid_test_no_tcp_table = true;
    sd = connect_now(addr);
    as = accept(ls, NULL, NULL);
    rc = (0 > sd) ? PMIX_ERROR : check_sd_vouched(sd, false);
    report("no table: a server nothing vouches for is refused", PMIX_ERR_INVALID_CRED == rc,
           PMIx_Error_string(rc));
    if (0 <= as) {
        close(as);
    }
    sd = connect_now(addr);
    as = accept(ls, NULL, NULL);
    rc = (0 > sd) ? PMIX_ERROR : check_sd_vouched(sd, true);
    report("no table: a server its file vouches for is taken", PMIX_SUCCESS == rc,
           PMIx_Error_string(rc));
    if (0 <= as) {
        close(as);
    }
    pmix_util_getid_test_no_tcp_table = false;
}

static double now(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double) ts.tv_sec + (double) ts.tv_nsec / 1e9;
}

int main(void)
{
    struct sockaddr_in addr;
    struct pollfd pfd;
    char detail[128];
    int ls, sd, as, i, refused = 0, broken = 0;
    uid_t uid;
    pmix_status_t rc;
    double start, took;

    fprintf(stdout, "=== ptl check_server unit test ===\n\n");

    if (PMIX_SUCCESS != PMIx_server_init(&mymodule, NULL, 0)) {
        fprintf(stdout, "  FAIL: server init\n");
        return 1;
    }
    ls = listen_lo(&addr);
    if (0 > ls) {
        fprintf(stdout, "  FAIL: no listener on 127.0.0.1\n");
        PMIx_server_finalize();
        return 1;
    }

    test_no_table(&addr, ls);

    /* is there a table to ask? Without one there is nothing more to check */
    sd = connect_now(&addr);
    as = accept(ls, NULL, NULL);
    rc = (0 > sd) ? PMIX_ERROR : pmix_util_getid_tcp(sd, &uid);
    if (0 <= sd) {
        close(sd);
    }
    if (0 <= as) {
        close(as);
    }
    if (PMIX_ERR_NOT_SUPPORTED == rc) {
        fprintf(stdout, "this platform has no table of TCP connections - skipping\n");
        close(ls);
        PMIx_server_finalize();
        return 77;
    }

    for (i = 0; i < NCONNECTS; i++) {
        sd = connect_now(&addr);
        if (0 > sd) {
            ++broken;
            continue;
        }
        if (PMIX_SUCCESS != check_sd(sd)) {
            ++refused;
        }
        as = accept(ls, NULL, NULL);
        if (0 <= as) {
            close(as);
        }
    }
    snprintf(detail, sizeof(detail), "%d of %d refused, %d did not connect", refused,
             NCONNECTS, broken);
    report("a server of our own is ours the moment we are connected", 0 == refused && 0 == broken,
           detail);

    /* the server closes its end: ours is still connected, the server's is
     * no longer an established connection */
    sd = connect_now(&addr);
    as = accept(ls, NULL, NULL);
    if (0 > sd || 0 > as) {
        report("a server whose end has closed is refused, soon", 0, "could not connect");
    } else {
        close(as);
        pfd.fd = sd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        (void) poll(&pfd, 1, 5000); /* the server's FIN */
        start = now();
        rc = check_sd(sd);
        took = now() - start;
        snprintf(detail, sizeof(detail), "%s after %.2fs", PMIx_Error_string(rc), took);
        report("a server whose end has closed is refused, soon",
               PMIX_ERR_INVALID_CRED == rc && 10.0 > took, detail);
    }

    close(ls);
    PMIx_server_finalize();
    fprintf(stdout, "\n%d passed, %d failed\n", npass, nfail);
    return (0 == nfail) ? 0 : 1;
}
