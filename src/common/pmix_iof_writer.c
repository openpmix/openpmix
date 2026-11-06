/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * The thread that writes the output files a privileged server opened for
 * another user - see pmix_iof_writer.h for why there is one.
 *
 * The progress thread hands over a file with pmix_iof_writer_relay() and
 * gets back the write end of a pipe; everything after that is this
 * thread's. It polls every pipe it has been given, and copies what
 * arrives to the matching file with ordinary blocking writes. The first
 * write to a file is preceded by the same check output_as_owner() makes
 * of a file it writes itself - a regular file with one name, owned by the
 * job's owner - because that check is an fstat() of a file on the user's
 * filesystem, and can block like a write. A file that fails it, or whose
 * write fails, is closed and the rest of its pipe read and discarded.
 * The end of a pipe - the progress thread closed the sink - closes the
 * file.
 *
 * One thread for every such file: a file that does not answer holds the
 * others too, but never the progress thread, which only ever sees a
 * non-blocking pipe fill. What it then queues is bounded by
 * pmix_iof_writer_backlog in write_output_line().
 *
 * Nothing here touches library state the progress thread owns. The list
 * of relays is shared with pmix_iof_writer_relay() and guarded by a lock
 * that is never held across a read, a write or a poll.
 */

#include "src/include/pmix_config.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "src/common/pmix_iof_writer.h"
#include "src/server/pmix_server_ops.h"
#include "src/util/pmix_fd.h"
#include "src/util/pmix_output.h"
#include "src/util/pmix_runas.h"

size_t pmix_iof_writer_backlog = 16384;
volatile bool pmix_iof_writer_test_stall = false;

typedef struct {
    int rd;        /* our end of the pipe */
    int fd;        /* the file, or -1 once given up on */
    uid_t owner;
    bool checked;  /* the file has passed its check */
    char *what;    /* names it in what we log */
} relay_t;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t exited_cond = PTHREAD_COND_INITIALIZER;
static relay_t *relays = NULL;
static size_t nrelays = 0, relays_size = 0;
static int ctl[2] = {-1, -1};   /* wakes the thread */
static pthread_t thread;
static bool running = false;    /* a thread exists */
static bool quitting = false;   /* finish what there is, then stop */
static bool exited = false;     /* ...and it has */

static void wake(void)
{
    char c = 'w';
    ssize_t n;

    /* a full pipe already wakes it */
    n = write(ctl[1], &c, 1);
    (void) n;
}

static bool file_ok(int fd, uid_t owner)
{
    struct stat buf;

    return (0 == fstat(fd, &buf) && S_ISREG(buf.st_mode) && 1 == buf.st_nlink &&
            owner == buf.st_uid);
}

static bool write_all(int fd, const char *buf, size_t len)
{
    ssize_t n;

    while (0 < len) {
        n = write(fd, buf, len);
        if (0 > n) {
            if (EINTR == errno) {
                continue;
            }
            return false;
        }
        buf += n;
        len -= (size_t) n;
    }
    return true;
}

/* Service relays[idx], whose pipe poll() said is readable. Returns true
 * when the relay is finished and is to be removed */
static bool service(size_t idx, char *buf, size_t bufsize)
{
    relay_t r;
    ssize_t n;

    pthread_mutex_lock(&lock);
    r = relays[idx];
    pthread_mutex_unlock(&lock);

    do {
        n = read(r.rd, buf, bufsize);
    } while (0 > n && EINTR == errno);
    if (0 > n && EAGAIN == errno) {
        return false;
    }
    if (0 >= n) {
        /* the sink is closed - or the pipe failed, which is the same */
        close(r.rd);
        if (0 <= r.fd) {
            close(r.fd);
        }
        free(r.what);
        return true;
    }
    if (0 <= r.fd && !r.checked) {
        if (!file_ok(r.fd, r.owner)) {
            pmix_output_verbose(2, pmix_server_globals.iof_output,
                                "IOF: output file %s is not a regular file of uid %lu with "
                                "one name - not written", r.what, (unsigned long) r.owner);
            close(r.fd);
            r.fd = -1;
        }
        r.checked = true;
    }
    if (0 <= r.fd && !write_all(r.fd, buf, (size_t) n)) {
        pmix_output_verbose(2, pmix_server_globals.iof_output,
                            "IOF: writing output file %s failed: %s - the rest is not "
                            "written", r.what, strerror(errno));
        close(r.fd);
        r.fd = -1;
    }
    pthread_mutex_lock(&lock);
    relays[idx].fd = r.fd;
    relays[idx].checked = r.checked;
    pthread_mutex_unlock(&lock);
    return false;
}

static void *writer(void *arg)
{
    static char buf[65536];
    struct pollfd *pfds = NULL, *tmp;
    size_t npfds, cap = 0, n, m;
    bool *gone = NULL, *gtmp;
    char c;
    int rc;
    (void) arg;

    for (;;) {
        pthread_mutex_lock(&lock);
        if (quitting && 0 == nrelays) {
            exited = true;
            pthread_cond_broadcast(&exited_cond);
            pthread_mutex_unlock(&lock);
            break;
        }
        npfds = nrelays + 1;
        if (cap < npfds) {
            tmp = (struct pollfd *) realloc(pfds, npfds * sizeof(struct pollfd));
            gtmp = (bool *) realloc(gone, npfds * sizeof(bool));
            if (NULL != tmp) {
                pfds = tmp;
            }
            if (NULL != gtmp) {
                gone = gtmp;
            }
            if (NULL == tmp || NULL == gtmp) {
                /* try again shortly; the pipes buffer meanwhile */
                pthread_mutex_unlock(&lock);
                usleep(10000);
                continue;
            }
            cap = npfds;
        }
        pfds[0].fd = ctl[0];
        pfds[0].events = POLLIN;
        pfds[0].revents = 0;
        for (n = 0; n < nrelays; n++) {
            pfds[n + 1].fd = relays[n].rd;
            pfds[n + 1].events = POLLIN;
            pfds[n + 1].revents = 0;
            gone[n] = false;
        }
        pthread_mutex_unlock(&lock);

        rc = poll(pfds, npfds, -1);
        if (0 > rc) {
            continue;
        }
        if (0 != pfds[0].revents) {
            while (0 < read(ctl[0], &c, 1)) {
            }
        }
        while (pmix_iof_writer_test_stall) {
            usleep(10000);
        }
        /* relays are only ever appended by others and removed by us, so
         * the first npfds - 1 are still the ones polled */
        for (n = 1; n < npfds; n++) {
            if (0 != pfds[n].revents) {
                gone[n - 1] = service(n - 1, buf, sizeof(buf));
            }
        }
        pthread_mutex_lock(&lock);
        for (n = npfds - 1; 0 < n; n--) {
            if (gone[n - 1]) {
                for (m = n - 1; m + 1 < nrelays; m++) {
                    relays[m] = relays[m + 1];
                }
                nrelays--;
            }
        }
        pthread_mutex_unlock(&lock);
    }
    free(pfds);
    free(gone);
    return NULL;
}

/* With the lock held: make sure there is a thread to give a relay to */
static bool start_locked(void)
{
    sigset_t all, old;
    int fl;

    if (running && exited) {
        /* left from a finalize it outlasted, and done since */
        pthread_mutex_unlock(&lock);
        (void) pthread_join(thread, NULL);
        pthread_mutex_lock(&lock);
        running = false;
    }
    if (running) {
        /* alive - including one a finalize stopped waiting for */
        quitting = false;
        return true;
    }
    if (0 > ctl[0]) {
        if (0 != pipe(ctl)) {
            return false;
        }
        (void) pmix_fd_set_cloexec(ctl[0]);
        (void) pmix_fd_set_cloexec(ctl[1]);
        fl = fcntl(ctl[0], F_GETFL);
        (void) fcntl(ctl[0], F_SETFL, fl | O_NONBLOCK);
        fl = fcntl(ctl[1], F_GETFL);
        (void) fcntl(ctl[1], F_SETFL, fl | O_NONBLOCK);
    }
    quitting = false;
    exited = false;
    /* the thread takes no signals - they are the host's */
    sigfillset(&all);
    (void) pthread_sigmask(SIG_SETMASK, &all, &old);
    if (0 != pthread_create(&thread, NULL, writer, NULL)) {
        (void) pthread_sigmask(SIG_SETMASK, &old, NULL);
        return false;
    }
    (void) pthread_sigmask(SIG_SETMASK, &old, NULL);
    running = true;
    return true;
}

int pmix_iof_writer_relay(int fd, uid_t owner, const char *what)
{
    int p[2], fl;
    relay_t *tmp;
    char *name;

    if (0 > fd) {
        return -1;
    }
    name = strdup((NULL == what) ? "" : what);
    if (NULL == name || 0 != pipe(p)) {
        free(name);
        close(fd);
        return -1;
    }
    (void) pmix_fd_set_cloexec(p[0]);
    (void) pmix_fd_set_cloexec(p[1]);
    /* our end is the one that must never block */
    fl = fcntl(p[1], F_GETFL);
    (void) fcntl(p[1], F_SETFL, fl | O_NONBLOCK);

    pthread_mutex_lock(&lock);
    if (nrelays == relays_size) {
        tmp = (relay_t *) realloc(relays, (relays_size + 16) * sizeof(relay_t));
        if (NULL == tmp) {
            goto fail;
        }
        relays = tmp;
        relays_size += 16;
    }
    if (!start_locked()) {
        goto fail;
    }
    relays[nrelays].rd = p[0];
    relays[nrelays].fd = fd;
    relays[nrelays].owner = owner;
    relays[nrelays].checked = false;
    relays[nrelays].what = name;
    nrelays++;
    pthread_mutex_unlock(&lock);
    wake();
    return p[1];

fail:
    pthread_mutex_unlock(&lock);
    free(name);
    close(p[0]);
    close(p[1]);
    close(fd);
    return -1;
}

void pmix_iof_writer_finalize(void)
{
    struct timeval now;
    struct timespec deadline;
    int rc = 0;

    pthread_mutex_lock(&lock);
    if (!running) {
        pthread_mutex_unlock(&lock);
        return;
    }
    quitting = true;
    pthread_mutex_unlock(&lock);
    wake();

    pthread_mutex_lock(&lock);
    gettimeofday(&now, NULL);
    deadline.tv_sec = now.tv_sec + ((0 < pmix_util_runas_timeout) ? pmix_util_runas_timeout : 0);
    deadline.tv_nsec = (long) now.tv_usec * 1000;
    while (!exited && ETIMEDOUT != rc) {
        if (0 < pmix_util_runas_timeout) {
            rc = pthread_cond_timedwait(&exited_cond, &lock, &deadline);
        } else {
            rc = pthread_cond_wait(&exited_cond, &lock);
        }
    }
    if (!exited) {
        /* held by a file that does not answer. Leave it - and everything
         * it uses - in place; a later relay reuses it */
        pthread_mutex_unlock(&lock);
        return;
    }
    pthread_mutex_unlock(&lock);
    (void) pthread_join(thread, NULL);
    pthread_mutex_lock(&lock);
    running = false;
    quitting = false;
    exited = false;
    close(ctl[0]);
    close(ctl[1]);
    ctl[0] = -1;
    ctl[1] = -1;
    free(relays);
    relays = NULL;
    nrelays = 0;
    relays_size = 0;
    pthread_mutex_unlock(&lock);
}
