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
 *
 * And the decision itself (pmix_ptl_base_judge_server), apart from any
 * socket, with fake map files standing in for /proc/self/uid_map and
 * gid_map: an id mapped into our user namespace and one from outside it,
 * for the uid and the gid; with the server's identity given and not; and
 * with its source vouching and not.
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
#include <pthread.h>
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

/* Accept as a server does, from its own thread, while the main thread
 * connects and checks: a connection still in the accept queue has no
 * socket of the server's yet, which Linux lists with inode 0, and the
 * check waits for it to be accepted. Each accepted socket is held open
 * until the next arrives, so the server's end of the connection being
 * checked is still established */
static volatile bool accepting = true;

static void *acceptor(void *arg)
{
    struct pollfd pfd;
    int ls = *(int *) arg, as, prev = -1;

    pfd.fd = ls;
    pfd.events = POLLIN;
    while (accepting) {
        pfd.revents = 0;
        if (1 == poll(&pfd, 1, 100) && 0 <= (as = accept(ls, NULL, NULL))) {
            if (0 <= prev) {
                close(prev);
            }
            prev = as;
        }
    }
    if (0 <= prev) {
        close(prev);
    }
    return NULL;
}

static double now(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double) ts.tv_sec + (double) ts.tv_nsec / 1e9;
}

static char mapdir[256];

/* a map file of the form /proc/self/uid_map has */
static void write_map(char *path, size_t len, const char *name, const char *body)
{
    FILE *fp;

    snprintf(path, len, "%s/%s", mapdir, name);
    fp = fopen(path, "w");
    if (NULL != fp) {
        fputs(body, fp);
        fclose(fp);
    }
}

static void test_judge(void)
{
    char all[512], some[512], none[512], shifted[512];
    const char *tmp = getenv("TMPDIR");
    uid_t me = geteuid();
    pmix_ptl_judgement_t j;

    snprintf(mapdir, sizeof(mapdir), "%s/judge.XXXXXX", (NULL == tmp) ? "/tmp" : tmp);
    if (NULL == mkdtemp(mapdir)) {
        report("judge: scratch directory", 0, strerror(errno));
        return;
    }
    /* every id mapped as itself, as with no user namespace */
    write_map(all, sizeof(all), "all", "0 0 4294967295\n");
    /* only our own uid and gid, as a rootless container maps them */
    snprintf(some, sizeof(some), "%lu %lu 1\n", (unsigned long) me, (unsigned long) me);
    write_map(some, sizeof(some), "some", some);
    /* inside ids 0-999 are outside ids 100000-100999 */
    write_map(shifted, sizeof(shifted), "shifted", "0 100000 1000\n");
    snprintf(none, sizeof(none), "%s/nonexistent", mapdir);

    j = pmix_ptl_base_judge_server(all, all, true, me, true, getegid(), false, 0, 0, false);
    report("judge: a server of our own user, the kernel naming it", PMIX_PTL_SERVER_OK == j,
           "not taken");
    j = pmix_ptl_base_judge_server(all, all, true, 4242, true, 4343, false, 0, 0, true);
    report("judge: another user we know is refused, vouched or not",
           PMIX_PTL_SERVER_WRONG_USER == j, "not refused");
    j = pmix_ptl_base_judge_server(some, some, true, 65534, true, 65534, false, 0, 0, false);
    report("judge: an id from outside our user namespace, nothing vouching, is refused",
           PMIX_PTL_SERVER_UNNAMED == j, "not refused as unnamed");
    j = pmix_ptl_base_judge_server(some, some, true, 65534, true, 65534, false, 0, 0, true);
    report("judge: ...and taken where its source vouches for it",
           PMIX_PTL_SERVER_OK_VOUCHED == j, "not taken");
    j = pmix_ptl_base_judge_server(some, some, true, 65534, true, 65534, true, 0, 0, true);
    report("judge: ...even with an identity given that it cannot be checked against",
           PMIX_PTL_SERVER_OK_VOUCHED == j, "not taken");
    j = pmix_ptl_base_judge_server(all, some, true, me, true, 65534, false, 0, 0, false);
    report("judge: a mapped uid with a gid from outside is not named",
           PMIX_PTL_SERVER_UNNAMED == j, "named");
    j = pmix_ptl_base_judge_server(none, none, true, 4242, true, 4343, false, 0, 0, false);
    report("judge: where there is no map, every id is named", PMIX_PTL_SERVER_WRONG_USER == j,
           "not judged by its id");
    j = pmix_ptl_base_judge_server(all, all, false, 0, false, 0, false, 0, 0, false);
    report("judge: no answer at all, nothing vouching, is refused",
           PMIX_PTL_SERVER_UNNAMED == j, "not refused");
    j = pmix_ptl_base_judge_server(all, all, false, 0, false, 0, false, 0, 0, true);
    report("judge: ...and taken where its source vouches", PMIX_PTL_SERVER_OK_VOUCHED == j,
           "not taken");
    j = pmix_ptl_base_judge_server(all, all, true, 4242, true, 4343, true, 4242, 4343, false);
    report("judge: the identity given is taken", PMIX_PTL_SERVER_OK == j, "not taken");
    j = pmix_ptl_base_judge_server(all, all, true, 4242, true, 4343, true, 4242, 1, false);
    report("judge: ...but not with another group", PMIX_PTL_SERVER_WRONG_USER == j,
           "taken");
    j = pmix_ptl_base_judge_server(shifted, shifted, true, 5, true, 6, true, 100005, 100006,
                                   false);
    report("judge: an identity given from outside matches the ids the maps translate to",
           PMIX_PTL_SERVER_OK == j, "not taken");
    j = pmix_ptl_base_judge_server(all, all, true, 4242, false, 0, true, 4242, 4343, false);
    report("judge: over TCP, with no gid, the uid given is enough", PMIX_PTL_SERVER_OK == j,
           "not taken");
    unlink(all);
    unlink(some);
    unlink(shifted);
    rmdir(mapdir);
}

int main(void)
{
    struct sockaddr_in addr;
    struct pollfd pfd;
    char detail[128];
    int ls, sd, as, i, refused = 0, broken = 0;
    pthread_t tid;
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

    test_judge();
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

    if (0 != pthread_create(&tid, NULL, acceptor, &ls)) {
        report("a server of our own is ours the moment we are connected", 0,
               "no thread to accept on");
        close(ls);
        PMIx_server_finalize();
        return 1;
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
    }
    accepting = false;
    pthread_join(tid, NULL);
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
