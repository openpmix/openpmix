/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Unit tests for pmix_util_getid() and pmix_util_getid_tcp().
 *
 * pmix_util_getid() has no caller in PMIx and none in PRRTE, but
 * pmix_getid.h is installed and the symbol is exported, so it is API and
 * has to keep working. Nothing else in the tree exercises it, which is
 * what this test is for: it pins the two things a consumer is entitled
 * to - that a connected AF_UNIX socket yields this process's own
 * credentials, and that a descriptor which is not a socket is refused
 * rather than answered with whatever the stack held.
 *
 * pmix_util_getid_tcp() is what psec/native authenticates a connection
 * with. Its cases cover what it answers and what it declines: a loopback
 * connection in every address shape the listener can hand us is owned by
 * us, and a connection whose peer has already closed answers
 * PMIX_ERR_NOT_FOUND.
 *
 * Exit 0 if all tests pass, 1 otherwise, 77 to skip.
 */

#include "src/include/pmix_config.h"
#include "src/include/pmix_globals.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#ifdef HAVE_SYS_TYPES_H
#    include <sys/types.h>
#endif
#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif
#include <sys/wait.h>

#include "src/util/pmix_getid.h"

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

/* A socketpair is the only portable way to get a connected AF_UNIX
 * socket without a filesystem rendezvous, and both ends belong to this
 * process - so the credentials the kernel reports are ours, whichever
 * of the two routes pmix_util_getid() was compiled with. */
static void test_socketpair_reports_us(void)
{
    int sd[2];
    uid_t uid = (uid_t) -1;
    gid_t gid = (gid_t) -1;
    pmix_status_t rc;

    if (0 != socketpair(AF_UNIX, SOCK_STREAM, 0, sd)) {
        report("getid on a socketpair (socketpair failed)", 0);
        return;
    }

    rc = pmix_util_getid(sd[0], &uid, &gid);
    report("getid on a socketpair == PMIX_SUCCESS", PMIX_SUCCESS == rc);
    if (PMIX_SUCCESS == rc) {
        report("getid reports our own uid", uid == geteuid());
        /* the two routes disagree about which group id they report -
         * SO_PEERCRED gives the real gid, getpeereid the effective one -
         * so accept either rather than encode a platform difference */
        report("getid reports one of our own gids", gid == getegid() || gid == getgid());
    } else {
        report("getid reports our own uid (skipped)", 0);
        report("getid reports one of our own gids (skipped)", 0);
    }

    close(sd[0]);
    close(sd[1]);
}

/* Both ends are symmetric; ask the other one too, since a route that
 * consulted the listening side rather than the descriptor it was given
 * would still pass the test above. */
static void test_both_ends_agree(void)
{
    int sd[2];
    uid_t uid0 = (uid_t) -1, uid1 = (uid_t) -2;
    gid_t gid0 = (gid_t) -1, gid1 = (gid_t) -2;
    pmix_status_t rc0, rc1;

    if (0 != socketpair(AF_UNIX, SOCK_STREAM, 0, sd)) {
        report("getid agrees on both ends (socketpair failed)", 0);
        return;
    }

    rc0 = pmix_util_getid(sd[0], &uid0, &gid0);
    rc1 = pmix_util_getid(sd[1], &uid1, &gid1);
    report("getid agrees on both ends of a socketpair",
           rc0 == rc1 && (PMIX_SUCCESS != rc0 || (uid0 == uid1 && gid0 == gid1)));

    close(sd[0]);
    close(sd[1]);
}

/* A pipe is a descriptor, not a socket. Both routes ask the kernel about
 * a socket and both are told ENOTSOCK, so this must come back as a
 * refusal - never as PMIX_SUCCESS over an untouched uid. */
static void test_not_a_socket(void)
{
    int fds[2];
    uid_t uid = (uid_t) 4242;
    gid_t gid = (gid_t) 4242;
    pmix_status_t rc;

    if (0 != pipe(fds)) {
        report("getid on a pipe (pipe failed)", 0);
        return;
    }

    rc = pmix_util_getid(fds[0], &uid, &gid);
    report("getid on a pipe != PMIX_SUCCESS", PMIX_SUCCESS != rc);
    report("getid on a pipe leaves the out-params alone",
           (uid_t) 4242 == uid && (gid_t) 4242 == gid);

    close(fds[0]);
    close(fds[1]);
}

static void test_closed_fd(void)
{
    uid_t uid = (uid_t) 4242;
    gid_t gid = (gid_t) 4242;
    pmix_status_t rc;
    int sd[2];

    if (0 != socketpair(AF_UNIX, SOCK_STREAM, 0, sd)) {
        report("getid on a closed fd (socketpair failed)", 0);
        return;
    }
    close(sd[0]);
    close(sd[1]);

    rc = pmix_util_getid(sd[0], &uid, &gid);
    report("getid on a closed fd != PMIX_SUCCESS", PMIX_SUCCESS != rc);
}

/* ---------------------------------------------------------------- */

/* A connected pair over a real TCP listener. `family` is the listener's;
 * `connect_v4` connects over IPv4 regardless, which against an AF_INET6
 * listener that also takes IPv4 is how a dual-stack server comes to hold
 * an IPv4 peer as a v4-mapped IPv6 address. Answers false - and the
 * caller skips - where the host cannot build that shape (no IPv6, or no
 * dual stack). */
static bool tcp_pair(int family, bool connect_v4, int *client, int *server)
{
    struct sockaddr_storage addr;
    struct sockaddr_in *a4 = (struct sockaddr_in *) &addr;
    struct sockaddr_in6 *a6 = (struct sockaddr_in6 *) &addr;
    struct sockaddr_in to4;
    socklen_t alen;
    int lsd, off = 0;
    uint16_t port;

    *client = *server = -1;
    lsd = socket(family, SOCK_STREAM, 0);
    if (0 > lsd) {
        return false;
    }
    memset(&addr, 0, sizeof(addr));
    if (AF_INET == family) {
        a4->sin_family = AF_INET;
        a4->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        alen = sizeof(*a4);
    } else {
        a6->sin6_family = AF_INET6;
        if (connect_v4) {
            /* take IPv4 on the same socket - bound to the mapped
             * loopback so the listener is not reachable from off-host */
            (void) setsockopt(lsd, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof(off));
            (void) inet_pton(AF_INET6, "::ffff:127.0.0.1", &a6->sin6_addr);
        } else {
            a6->sin6_addr = in6addr_loopback;
        }
        alen = sizeof(*a6);
    }
    if (0 != bind(lsd, (struct sockaddr *) &addr, alen) || 0 != listen(lsd, 1) ||
        0 != getsockname(lsd, (struct sockaddr *) &addr, &alen)) {
        close(lsd);
        return false;
    }
    port = (AF_INET == family) ? a4->sin_port : a6->sin6_port;

    if (connect_v4) {
        memset(&to4, 0, sizeof(to4));
        to4.sin_family = AF_INET;
        to4.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        to4.sin_port = port;
        *client = socket(AF_INET, SOCK_STREAM, 0);
        if (0 > *client || 0 != connect(*client, (struct sockaddr *) &to4, sizeof(to4))) {
            goto fail;
        }
    } else {
        *client = socket(family, SOCK_STREAM, 0);
        if (0 > *client || 0 != connect(*client, (struct sockaddr *) &addr, alen)) {
            goto fail;
        }
    }
    *server = accept(lsd, NULL, NULL);
    if (0 > *server) {
        goto fail;
    }
    close(lsd);
    return true;

fail:
    if (0 <= *client) {
        close(*client);
    }
    *client = -1;
    close(lsd);
    return false;
}

static void test_tcp_shape(const char *shape, int family, bool connect_v4)
{
    char label[256];
    int client, server;
    uid_t uid = (uid_t) 4242;
    pmix_status_t rc;

    if (!tcp_pair(family, connect_v4, &client, &server)) {
        fprintf(stdout, "  SKIP: %s loopback (not available here)\n", shape);
        return;
    }

    rc = pmix_util_getid_tcp(server, &uid);
    snprintf(label, sizeof(label), "getid_tcp on the accepting end of a %s connection", shape);
    report(label, PMIX_SUCCESS == rc && uid == geteuid());

    /* and from the other side: the peer there is the accepted socket,
     * which this process created too */
    uid = (uid_t) 4242;
    rc = pmix_util_getid_tcp(client, &uid);
    snprintf(label, sizeof(label), "getid_tcp on the connecting end of a %s connection", shape);
    report(label, PMIX_SUCCESS == rc && uid == geteuid());

    close(client);
    close(server);
}

/* A peer that has closed leaves its end in FIN_WAIT/TIME_WAIT, which
 * carries no owner. Only an established connection is answered. */
static void test_tcp_peer_gone(void)
{
    int client, server, n;
    uid_t uid = (uid_t) 4242;
    pmix_status_t rc = PMIX_SUCCESS;
    char c;

    if (!tcp_pair(AF_INET, false, &client, &server)) {
        fprintf(stdout, "  SKIP: peer-gone case (no IPv4 loopback)\n");
        return;
    }
    close(client);
    /* wait for the FIN to land, so the peer's end has left ESTABLISHED */
    for (n = 0; n < 100; n++) {
        if (0 == recv(server, &c, 1, MSG_DONTWAIT)) {
            break;
        }
        usleep(10000);
    }
    rc = pmix_util_getid_tcp(server, &uid);
    report("getid_tcp refuses a connection whose peer has closed",
           PMIX_ERR_NOT_FOUND == rc);
    report("getid_tcp leaves the out-param alone when it refuses", (uid_t) 4242 == uid);
    close(server);
}

/* Every case above connects this process to itself, so the peer's uid
 * and ours are the same number - and a lookup that found the wrong end of
 * the connection, our own socket instead of the peer's, would pass them
 * all. Running as root, we can make the peer somebody else: a child drops
 * to another uid before it creates its socket, and the answer has to be
 * that uid, not ours. */
static void test_tcp_other_user(void)
{
    struct sockaddr_in addr;
    socklen_t alen = sizeof(addr);
    int lsd, sd, status, sync[2];
    uid_t other = (uid_t) 4243, uid = (uid_t) 4242;
    pid_t pid;
    char c = 0;
    ssize_t n;
    pmix_status_t rc;

    if (0 != geteuid()) {
        fprintf(stdout, "  SKIP: cross-user case (needs root)\n");
        return;
    }
    lsd = socket(AF_INET, SOCK_STREAM, 0);
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (0 > lsd || 0 != bind(lsd, (struct sockaddr *) &addr, sizeof(addr)) ||
        0 != listen(lsd, 1) || 0 != getsockname(lsd, (struct sockaddr *) &addr, &alen) ||
        0 != pipe(sync)) {
        report("cross-user case setup", 0);
        return;
    }
    pid = fork();
    if (0 == pid) {
        /* drop privilege BEFORE the socket exists - the kernel records
         * the uid that created it */
        close(sync[1]);
        if (0 != setgid((gid_t) other) || 0 != setuid(other)) {
            _exit(2);
        }
        sd = socket(AF_INET, SOCK_STREAM, 0);
        if (0 > sd || 0 != connect(sd, (struct sockaddr *) &addr, sizeof(addr))) {
            _exit(3);
        }
        /* hold the connection open until the parent has asked */
        n = read(sync[0], &c, 1);
        _exit((1 == n) ? 0 : 4);
    }
    close(sync[0]);
    sd = accept(lsd, NULL, NULL);
    rc = pmix_util_getid_tcp(sd, &uid);
    report("getid_tcp names the peer's uid, not ours", PMIX_SUCCESS == rc && other == uid);
    n = write(sync[1], &c, 1);
    PMIX_HIDE_UNUSED_PARAMS(n);
    close(sync[1]);
    (void) waitpid(pid, &status, 0);
    close(sd);
    close(lsd);
}

static void test_tcp_not_tcp(void)
{
    int sd[2], lsd;
    uid_t uid = (uid_t) 4242;
    struct sockaddr_in a4;

    report("getid_tcp refuses a negative descriptor",
           PMIX_ERR_BAD_PARAM == pmix_util_getid_tcp(-1, &uid));

    if (0 == socketpair(AF_UNIX, SOCK_STREAM, 0, sd)) {
        report("getid_tcp refuses an AF_UNIX socket",
               PMIX_ERR_BAD_PARAM == pmix_util_getid_tcp(sd[0], &uid));
        close(sd[0]);
        close(sd[1]);
    }

    /* a listener has no peer at all */
    lsd = socket(AF_INET, SOCK_STREAM, 0);
    if (0 <= lsd) {
        memset(&a4, 0, sizeof(a4));
        a4.sin_family = AF_INET;
        a4.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (0 == bind(lsd, (struct sockaddr *) &a4, sizeof(a4)) && 0 == listen(lsd, 1)) {
            report("getid_tcp refuses a listening socket",
                   PMIX_ERR_BAD_PARAM == pmix_util_getid_tcp(lsd, &uid));
        }
        close(lsd);
    }
    report("getid_tcp never wrote the out-param", (uid_t) 4242 == uid);
}

int main(int argc, char **argv)
{
    int sd[2], c, s;
    uid_t uid;
    gid_t gid;
    bool unix_supported = true;

    PMIX_HIDE_UNUSED_PARAMS(argc, argv);

    /* A platform with neither SO_PEERCRED nor getpeereid() answers
     * PMIX_ERR_NOT_SUPPORTED to everything; there is nothing to test
     * there for the AF_UNIX half. */
    if (0 == socketpair(AF_UNIX, SOCK_STREAM, 0, sd)) {
        pmix_status_t rc = pmix_util_getid(sd[0], &uid, &gid);
        close(sd[0]);
        close(sd[1]);
        unix_supported = (PMIX_ERR_NOT_SUPPORTED != rc);
    }

    if (unix_supported) {
        fprintf(stdout, "\n=== pmix_util_getid ===\n");
        test_socketpair_reports_us();
        test_both_ends_agree();
        test_not_a_socket();
        test_closed_fd();
    } else {
        fprintf(stdout, "pmix_util_getid is not supported here - skipping its cases\n");
    }

    fprintf(stdout, "\n=== pmix_util_getid_tcp ===\n");
    test_tcp_not_tcp();
    if (tcp_pair(AF_INET, false, &c, &s)) {
        pmix_status_t rc = pmix_util_getid_tcp(s, &uid);
        close(c);
        close(s);
        if (PMIX_ERR_NOT_SUPPORTED == rc) {
            fprintf(stdout, "pmix_util_getid_tcp is not supported here - skipping its cases\n");
            goto done;
        }
    }
    test_tcp_shape("IPv4", AF_INET, false);
    test_tcp_shape("IPv6", AF_INET6, false);
    test_tcp_shape("v4-mapped IPv6", AF_INET6, true);
    test_tcp_peer_gone();
    test_tcp_other_user();

done:
    fprintf(stdout, "\nResults: %d passed, %d failed\n\n", npass, nfail);
    if (0 == npass && 0 == nfail) {
        return 77;
    }
    return (nfail > 0) ? 1 : 0;
}
