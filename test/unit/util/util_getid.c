/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Unit tests for pmix_util_getid().
 *
 * pmix_util_getid() has no caller in PMIx and none in PRRTE, but
 * pmix_getid.h is installed and the symbol is exported, so it is API and
 * has to keep working. Nothing else in the tree exercises it, which is
 * what this test is for: it pins the two things a consumer is entitled
 * to - that a connected AF_UNIX socket yields this process's own
 * credentials, and that a descriptor which is not a socket is refused
 * rather than answered with whatever the stack held.
 *
 * Exit 0 if all tests pass, 1 otherwise, 77 to skip.
 */

#include "src/include/pmix_config.h"
#include "src/include/pmix_globals.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#ifdef HAVE_SYS_TYPES_H
#    include <sys/types.h>
#endif
#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif

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

int main(int argc, char **argv)
{
    int sd[2];
    uid_t uid;
    gid_t gid;

    PMIX_HIDE_UNUSED_PARAMS(argc, argv);

    /* A platform with neither SO_PEERCRED nor getpeereid() answers
     * PMIX_ERR_NOT_SUPPORTED to everything; there is nothing here to
     * test on it. */
    if (0 == socketpair(AF_UNIX, SOCK_STREAM, 0, sd)) {
        pmix_status_t rc = pmix_util_getid(sd[0], &uid, &gid);
        close(sd[0]);
        close(sd[1]);
        if (PMIX_ERR_NOT_SUPPORTED == rc) {
            fprintf(stdout, "pmix_util_getid is not supported here - skipping\n");
            return 77;
        }
    }

    fprintf(stdout, "\n=== pmix_util_getid ===\n");
    test_socketpair_reports_us();
    test_both_ends_agree();
    test_not_a_socket();
    test_closed_fd();

    fprintf(stdout, "\nResults: %d passed, %d failed\n\n", npass, nfail);
    return (nfail > 0) ? 1 : 0;
}
