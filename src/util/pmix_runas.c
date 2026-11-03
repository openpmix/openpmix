/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "src/include/pmix_config.h"

#include <errno.h>
#include <grp.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#ifdef HAVE_SYS_WAIT_H
#    include <sys/wait.h>
#endif
#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif

#include "src/util/pmix_runas.h"
#include "src/util/pmix_fd.h"

volatile bool pmix_util_runas_child = false;

#if defined(HAVE_FORK) && defined(HAVE_WAITPID)

/* hand fd to the other end of sd */
static int send_fd(int sd, int fd)
{
    struct msghdr msg;
    struct iovec iov;
    char byte = 'f';
    union {
        struct cmsghdr hdr;
        char buf[CMSG_SPACE(sizeof(int))];
    } ctl;
    struct cmsghdr *cm;
    ssize_t rc;

    memset(&msg, 0, sizeof(msg));
    memset(&ctl, 0, sizeof(ctl));
    iov.iov_base = &byte;
    iov.iov_len = 1;
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = ctl.buf;
    msg.msg_controllen = sizeof(ctl.buf);
    cm = CMSG_FIRSTHDR(&msg);
    cm->cmsg_level = SOL_SOCKET;
    cm->cmsg_type = SCM_RIGHTS;
    cm->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cm), &fd, sizeof(int));
    do {
        rc = sendmsg(sd, &msg, 0);
    } while (0 > rc && EINTR == errno);
    return (1 == rc) ? 0 : -1;
}

#ifdef MSG_CMSG_CLOEXEC
#    define PMIX_MSG_CMSG_CLOEXEC MSG_CMSG_CLOEXEC
#else
#    define PMIX_MSG_CMSG_CLOEXEC 0
#endif

/* the descriptor the other end of sd sent, or -1 */
static int recv_fd(int sd)
{
    struct msghdr msg;
    struct iovec iov;
    char byte;
    union {
        struct cmsghdr hdr;
        char buf[CMSG_SPACE(sizeof(int))];
    } ctl;
    struct cmsghdr *cm;
    ssize_t rc;
    int fd = -1;

    memset(&msg, 0, sizeof(msg));
    memset(&ctl, 0, sizeof(ctl));
    iov.iov_base = &byte;
    iov.iov_len = 1;
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = ctl.buf;
    msg.msg_controllen = sizeof(ctl.buf);
    do {
        rc = recvmsg(sd, &msg, PMIX_MSG_CMSG_CLOEXEC);
    } while (0 > rc && EINTR == errno);
    if (1 != rc) {
        return -1;
    }
    for (cm = CMSG_FIRSTHDR(&msg); NULL != cm; cm = CMSG_NXTHDR(&msg, cm)) {
        if (SOL_SOCKET == cm->cmsg_level && SCM_RIGHTS == cm->cmsg_type &&
            CMSG_LEN(sizeof(int)) == cm->cmsg_len) {
            memcpy(&fd, CMSG_DATA(cm), sizeof(int));
        }
    }
    /* close-on-exec on arrival where the platform can, else at once */
    if (0 <= fd && 0 == PMIX_MSG_CMSG_CLOEXEC) {
        (void) pmix_fd_set_cloexec(fd);
    }
    return fd;
}

#endif

int pmix_util_run_as(uid_t uid, gid_t gid, const gid_t *gids, size_t ngids, bool want_fd,
                     int (*fn)(void *arg), void *arg)
{
#if defined(HAVE_FORK) && defined(HAVE_WAITPID)
    int sv[2] = {-1, -1}, fd, status = 0;
    pid_t pid, r;
#endif
    int rc;

    if (NULL == fn || (uid_t) -1 == uid || (gid_t) -1 == gid) {
        return -1;
    }
    if (geteuid() == uid && getegid() == gid) {
        /* we are that user already - the kernel is applying exactly the
         * check we want */
        rc = fn(arg);
        if (want_fd) {
            return rc;
        }
        if (0 <= rc) {
            return 0;
        }
        return -1;
    }

#if defined(HAVE_FORK) && defined(HAVE_WAITPID)
    if (want_fd && 0 != socketpair(AF_UNIX, SOCK_STREAM | PMIX_SOCK_CLOEXEC, 0, sv)) {
        return -1;
    }
    if (want_fd && 0 == PMIX_SOCK_CLOEXEC) {
        (void) pmix_fd_set_cloexec(sv[0]);
        (void) pmix_fd_set_cloexec(sv[1]);
    }
    pid = fork();
    if (0 > pid) {
        if (want_fd) {
            close(sv[0]);
            close(sv[1]);
        }
        return -1;
    }
    if (0 == pid) {
        /* Child. Nothing here may return: every path out is _exit(), not
         * exit(), because this is a forked child of a threaded process
         * and running the parent's atexit handlers would tear down a
         * library that is still very much alive in the parent. */
        pmix_util_runas_child = true;
        if (want_fd) {
            close(sv[0]);
        }
#    ifdef HAVE_SETGROUPS
        /* setuid() does not touch the supplementary groups, so without
         * this the child keeps *our* memberships and reaches what the user
         * cannot. The user's own set was looked up by the caller - NSS is
         * not safe to call here */
        if (0 != setgroups(ngids, (0 < ngids) ? gids : NULL)) {
            _exit(1);
        }
#    else
        (void) gids;
        (void) ngids;
#    endif
        /* group first: once the uid is gone so is the privilege needed
         * to change the gid */
        if (0 != setgid(gid) || 0 != setuid(uid)) {
            _exit(1);
        }
        /* a drop that silently did not take would have us doing the work
         * with our original identity, which is the whole thing avoided */
        if (geteuid() != uid || getuid() != uid || getegid() != gid || getgid() != gid) {
            _exit(1);
        }
        fd = fn(arg);
        if (0 > fd) {
            _exit(1);
        }
        if (want_fd && 0 != send_fd(sv[1], fd)) {
            _exit(1);
        }
        _exit(0);
    }

    /* Parent */
    fd = -1;
    if (want_fd) {
        close(sv[1]);
        /* the descriptor, or EOF if the child gave up */
        fd = recv_fd(sv[0]);
        close(sv[0]);
    }
    /* Wait for our own child by pid, and only our own. The host process
     * we are embedded in may sweep with waitpid(-1) or set SIGCHLD to
     * SIG_IGN, in which case our child is reaped out from under us and
     * ECHILD means "already gone" - its answer, if any, is the descriptor
     * it sent */
    do {
        r = waitpid(pid, &status, 0);
    } while (0 > r && EINTR == errno);
    if (pid == r && !(WIFEXITED(status) && 0 == WEXITSTATUS(status))) {
        if (0 <= fd) {
            close(fd);
        }
        return -1;
    }
    if (want_fd) {
        return fd;
    }
    return (pid == r || ECHILD == errno) ? 0 : -1;
#else
    (void) gids;
    (void) ngids;
    (void) want_fd;
    (void) arg;
    /* no way to become the user, and doing the work as ourselves is what
     * this exists to stop */
    return -1;
#endif
}
