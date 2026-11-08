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
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#ifdef HAVE_SYS_WAIT_H
#    include <sys/wait.h>
#endif
#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif

#include "src/util/pmix_runas.h"
#include "src/include/pmix_globals.h"
#include "src/util/pmix_fd.h"
#include "src/util/pmix_output.h"

volatile bool pmix_util_runas_child = false;
int pmix_util_runas_timeout = 10;
volatile bool pmix_util_runas_test_stall = false;

size_t pmix_util_runas_groups(gid_t gid, const gid_t *gids, size_t ngids, gid_t *out,
                              size_t max)
{
    size_t n, nout = 0;

    if (NULL == out || 0 == max) {
        return 0;
    }
    out[nout++] = gid;
    for (n = 0; NULL != gids && n < ngids && nout < max; n++) {
        if (gids[n] != gid) {
            out[nout++] = gids[n];
        }
    }
    return nout;
}

#if defined(HAVE_FORK) && defined(HAVE_WAITPID)
/* the most groups setgroups() takes */
static size_t groups_max(void)
{
    long max = sysconf(_SC_NGROUPS_MAX);

    if (0 < max) {
        return (size_t) max;
    }
#    ifdef NGROUPS_MAX
    return NGROUPS_MAX;
#    else
    /* no limit we can learn - the list goes as it is */
    return SIZE_MAX;
#    endif
}
#endif

#if defined(HAVE_FORK) && defined(HAVE_WAITPID)

/* Children we have stopped waiting for but not yet reaped - one that had
 * not quite exited when its answer arrived, or one we killed. The parent
 * never blocks in waitpid(): the child works on a path the user chose,
 * on a filesystem that may not respond, and may also have been stopped.
 * So a child is asked about with WNOHANG, and one that is not gone yet
 * is kept here and asked again on the next call, and at finalize. Callers are not confined to the
 * progress thread - the epilog runs from destructors - hence the lock */
static pthread_mutex_t unreaped_lock = PTHREAD_MUTEX_INITIALIZER;
static pid_t *unreaped = NULL;
static size_t nunreaped = 0, unreaped_size = 0;

/* reap what has finished; keep what has not */
static void sweep_unreaped(void)
{
    size_t n, keep = 0;
    int status;
    pid_t r;

    pthread_mutex_lock(&unreaped_lock);
    for (n = 0; n < nunreaped; n++) {
        do {
            r = waitpid(unreaped[n], &status, WNOHANG);
        } while (0 > r && EINTR == errno);
        if (0 == r) {
            unreaped[keep++] = unreaped[n];
        }
        /* reaped, or ECHILD - a host sweeping waitpid(-1), or one that
         * set SIGCHLD to SIG_IGN, reaped it for us */
    }
    nunreaped = keep;
    pthread_mutex_unlock(&unreaped_lock);
}

/* reap pid if it is done, else remember it */
static void reap_or_keep(pid_t pid)
{
    pid_t r, *tmp;
    int status;

    do {
        r = waitpid(pid, &status, WNOHANG);
    } while (0 > r && EINTR == errno);
    if (0 != r) {
        return;
    }
    pthread_mutex_lock(&unreaped_lock);
    if (nunreaped == unreaped_size) {
        tmp = (pid_t *) realloc(unreaped, (unreaped_size + 8) * sizeof(pid_t));
        if (NULL == tmp) {
            /* it stays a zombie until we exit - nothing worse */
            pthread_mutex_unlock(&unreaped_lock);
            return;
        }
        unreaped = tmp;
        unreaped_size += 8;
    }
    unreaped[nunreaped++] = pid;
    pthread_mutex_unlock(&unreaped_lock);
}

/* milliseconds left until deadline, or -1 for no deadline */
static int ms_left(const struct timespec *deadline)
{
    struct timespec now;
    long long ms;

    if (0 == deadline->tv_sec && 0 == deadline->tv_nsec) {
        return -1;
    }
    (void) clock_gettime(CLOCK_MONOTONIC, &now);
    ms = (long long) (deadline->tv_sec - now.tv_sec) * 1000 +
         (deadline->tv_nsec - now.tv_nsec) / 1000000;
    if (0 > ms) {
        return 0;
    }
    if ((long long) 1 << 30 < ms) {
        return 1 << 30;
    }
    return (int) ms;
}

/* Wait for sd to become readable - data or EOF - by the deadline.
 * 1 if it did, 0 if the deadline passed, -1 on error */
static int wait_readable(int sd, const struct timespec *deadline)
{
    struct pollfd pfd;
    int rc;

    for (;;) {
        pfd.fd = sd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        rc = poll(&pfd, 1, ms_left(deadline));
        if (0 < rc) {
            return 1;
        }
        if (0 == rc) {
            return 0;
        }
        if (EINTR != errno) {
            return -1;
        }
    }
}

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

void pmix_util_runas_finalize(void)
{
#if defined(HAVE_FORK) && defined(HAVE_WAITPID)
    sweep_unreaped();
    pthread_mutex_lock(&unreaped_lock);
    /* What is left is still exiting, and is still ours to reap - the
     * library may be initialized again, and forgetting a child here
     * would leave it a zombie for the life of the process. Only an
     * empty list is given back */
    if (0 == nunreaped) {
        free(unreaped);
        unreaped = NULL;
        unreaped_size = 0;
    }
    pthread_mutex_unlock(&unreaped_lock);
#endif
}

int pmix_util_run_as(uid_t uid, gid_t gid, const gid_t *gids, size_t ngids, bool want_fd,
                     int (*fn)(void *arg), void *arg)
{
#if defined(HAVE_FORK) && defined(HAVE_WAITPID)
    int sv[2] = {-1, -1}, fd, status = 0, ready;
    pid_t pid, r;
    struct timespec deadline = {0, 0};
    bool answered = false, timedout = false;
    char byte;
    ssize_t nb;
    gid_t *glist;
    size_t gmax, nglist;
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
    sweep_unreaped();
    /* the child's groups, made here: it may not allocate */
    gmax = groups_max();
    if (ngids < gmax) {
        gmax = ngids + 1;
    }
    glist = (gid_t *) malloc(gmax * sizeof(gid_t));
    if (NULL == glist) {
        return -1;
    }
    nglist = pmix_util_runas_groups(gid, gids, ngids, glist, gmax);
    if (nglist == gmax) {
        size_t want = 1;

        for (size_t n = 0; NULL != gids && n < ngids; n++) {
            if (gids[n] != gid) {
                ++want;
            }
        }
        if (want > nglist) {
            pmix_output_verbose(2, pmix_globals.debug_output,
                                "run_as: uid %lu has %lu groups and setgroups() takes %lu - "
                                "the rest are left out", (unsigned long) uid,
                                (unsigned long) want, (unsigned long) nglist);
        }
    }
    /* The child always answers over the socket - the descriptor, or a
     * byte saying fn succeeded - so the answer never depends on reaping
     * it, which a host may do before we can */
    if (0 != socketpair(AF_UNIX, SOCK_STREAM | PMIX_SOCK_CLOEXEC, 0, sv)) {
        free(glist);
        return -1;
    }
    if (0 == PMIX_SOCK_CLOEXEC) {
        (void) pmix_fd_set_cloexec(sv[0]);
        (void) pmix_fd_set_cloexec(sv[1]);
    }
    pid = fork();
    if (0 > pid) {
        close(sv[0]);
        close(sv[1]);
        free(glist);
        return -1;
    }
    if (0 == pid) {
        /* Child. Nothing here may return: every path out is _exit(), not
         * exit(), because this is a forked child of a threaded process
         * and running the parent's atexit handlers would tear down a
         * library that is still very much alive in the parent. */
        pmix_util_runas_child = true;
        close(sv[0]);
#    ifdef HAVE_SETGROUPS
        /* setuid() does not touch the supplementary groups, so without
         * this the child keeps *our* memberships and reaches what the user
         * cannot. The user's own set was looked up by the caller - NSS is
         * not safe to call here */
        if (0 != setgroups(nglist, glist)) {
            _exit(1);
        }
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
        while (pmix_util_runas_test_stall) {
            /* testing only - a helper that never finishes, as one does
             * on a filesystem that never answers */
            pause();
        }
        fd = fn(arg);
        if (0 > fd) {
            _exit(1);
        }
        if (want_fd) {
            if (0 != send_fd(sv[1], fd)) {
                _exit(1);
            }
        } else {
            byte = 'k';
            do {
                nb = write(sv[1], &byte, 1);
            } while (0 > nb && EINTR == errno);
            if (1 != nb) {
                _exit(1);
            }
        }
        _exit(0);
    }

    /* Parent. Nothing here waits without a deadline - see
     * sweep_unreaped(). The caller is usually the progress thread */
    free(glist);
    close(sv[1]);
    if (0 < pmix_util_runas_timeout) {
        (void) clock_gettime(CLOCK_MONOTONIC, &deadline);
        deadline.tv_sec += pmix_util_runas_timeout;
    }
    fd = -1;
    ready = wait_readable(sv[0], &deadline);
    if (0 == ready) {
        timedout = true;
    } else if (0 < ready) {
        if (want_fd) {
            /* the descriptor, or EOF if the child gave up */
            fd = recv_fd(sv[0]);
            answered = (0 <= fd);
        } else {
            do {
                nb = read(sv[0], &byte, 1);
            } while (0 > nb && EINTR == errno);
            answered = (1 == nb && 'k' == byte);
        }
        /* and then for it to go, which closes its end. A child that
         * answered and is then held - stopped between the answer and its
         * exit - has still answered */
        if (answered && 0 == wait_readable(sv[0], &deadline)) {
            timedout = true;
        }
    }
    close(sv[0]);
    /* Ask about our own child by pid, and only our own, and never wait.
     * The host process we are embedded in may sweep with waitpid(-1) or
     * set SIGCHLD to SIG_IGN, and then ECHILD means another reaped it -
     * the answer it sent is still the answer */
    do {
        r = waitpid(pid, &status, WNOHANG);
    } while (0 > r && EINTR == errno);
    if (0 == r) {
        if (timedout || 0 > ready) {
            /* Still ours, so the pid cannot have been given to anyone
             * else - which is why this is asked first: a pid somebody
             * else reaped may already name another process. SIGKILL
             * ends a stopped process as well as a running one */
            (void) kill(pid, SIGKILL);
            /* it is normally gone within a moment of that, so give it
             * one - briefly, as one stuck in the kernel may not be */
            for (int n = 0; n < 50 && 0 == r; n++) {
                struct timespec ms = {0, 1000000};
                (void) nanosleep(&ms, NULL);
                r = waitpid(pid, &status, WNOHANG);
            }
        }
        if (0 == r) {
            reap_or_keep(pid);
        }
    } else if (pid == r && !(WIFEXITED(status) && 0 == WEXITSTATUS(status)) && !timedout) {
        /* it answered and then failed - it never does, but treat
         * that as a failure */
        answered = false;
    }
    if (!answered) {
        if (0 <= fd) {
            close(fd);
        }
        if (timedout) {
            errno = ETIMEDOUT;
        }
        return -1;
    }
    if (want_fd) {
        return fd;
    }
    return 0;
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
