/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * pmix_util_run_as() - work done as another user, in a child that has
 * become that user - waits for the child for at most
 * pmix_util_runas_timeout seconds.
 *
 * A root server does two things this way for a job's owner: it opens the
 * job's output files and it runs the job's cleanup, both on the server's
 * progress thread. These cases cover a child that never finishes and one
 * that is stopped, which the parent used to wait for in waitpid() with
 * no bound.
 *
 * Only a privileged process can become another user, so every case here
 * needs root, and the program reports SKIP without it. Each case runs in
 * a forked child under alarm(): against an unbounded library a stalled
 * helper is a hang, and it is reported as one instead of taking
 * make check down with it.
 *
 * Test cases:
 *
 *   a helper that never finishes       -> -1, ETIMEDOUT, at the timeout;
 *                                         with and without a descriptor
 *   a helper that stops itself         -> the same
 *   ... and neither is left behind     -> killed, and reaped
 *   a helper that fails, while the
 *   host has SIGCHLD set to SIG_IGN    -> -1. The child is reaped by the
 *                                         kernel before we can ask, and
 *                                         the answer used to be read off
 *                                         the ECHILD as success
 *   ... one that succeeds              -> 0
 *   a helper that opens a file         -> the descriptor
 */

#include "src/include/pmix_config.h"

#include "src/util/pmix_runas.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

/* ids no real account should have */
#define OTHER_UID 4242
#define OTHER_GID 4343
#define TIMEOUT 1

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

static double now_s(void)
{
    struct timeval tv;

    gettimeofday(&tv, NULL);
    return (double) tv.tv_sec + (double) tv.tv_usec / 1e6;
}

/* what a helper does - each writes its pid first, so the case can find
 * out afterwards whether it is still there */
typedef struct {
    int pidfd;
    int what;
} helper_t;

enum { H_HANG, H_STOP, H_FAIL, H_OK, H_OPEN };

static int helper(void *arg)
{
    helper_t *h = (helper_t *) arg;
    pid_t me = getpid();

    if (0 <= h->pidfd) {
        /* glibc marks write() warn_unused_result, which a cast does not
         * quiet */
        ssize_t n = write(h->pidfd, &me, sizeof(me));
        (void) n;
    }
    switch (h->what) {
    case H_HANG:
        for (;;) {
            pause();
        }
    case H_STOP:
        (void) kill(me, SIGSTOP);
        return 0;
    case H_FAIL:
        return -1;
    case H_OK:
        return 0;
    default:
        return open("/dev/null", O_RDONLY);
    }
}

/* Run one case in a child of our own. The child's exit status is the
 * verdict: 0 passed, 1 failed, and dying to the alarm is a hang */
typedef int (*case_fn_t)(void);

static void run_case(const char *name, case_fn_t fn)
{
    pid_t pid;
    int status;

    pid = fork();
    if (0 > pid) {
        report(name, 0);
        return;
    }
    if (0 == pid) {
        alarm(4 * TIMEOUT + 5);
        _exit(fn());
    }
    while (0 > waitpid(pid, &status, 0) && EINTR == errno) {
    }
    if (WIFSIGNALED(status) && SIGALRM == WTERMSIG(status)) {
        fprintf(stdout, "        (hung: still waiting at %d seconds)\n", 4 * TIMEOUT + 5);
        report(name, 0);
        return;
    }
    report(name, WIFEXITED(status) && 0 == WEXITSTATUS(status));
}

/* the helper's pid, as it wrote it */
static pid_t helper_pid(int rd)
{
    pid_t pid = -1;

    if (sizeof(pid) != read(rd, &pid, sizeof(pid))) {
        return -1;
    }
    return pid;
}

/* gone, and not a zombie either: kill(pid, 0) succeeds on a zombie */
static int gone(pid_t pid)
{
    int n;

    for (n = 0; n < 50; n++) {
        pmix_util_runas_finalize();
        if (0 != kill(pid, 0) && ESRCH == errno) {
            return 1;
        }
        usleep(20000);
    }
    return 0;
}

static int stalled(int what, int want_fd)
{
    helper_t h;
    int p[2], rc, err;
    double t0, took;
    pid_t pid;

    if (0 != pipe(p)) {
        return 1;
    }
    h.pidfd = p[1];
    h.what = what;
    pmix_util_runas_timeout = TIMEOUT;
    t0 = now_s();
    rc = pmix_util_run_as(OTHER_UID, OTHER_GID, NULL, 0, want_fd, helper, &h);
    err = errno;
    took = now_s() - t0;
    close(p[1]);
    pid = helper_pid(p[0]);
    close(p[0]);
    if (0 <= rc || ETIMEDOUT != err) {
        fprintf(stdout, "        (returned %d, errno %d)\n", rc, err);
        return 1;
    }
    if (took < TIMEOUT - 0.1 || took > TIMEOUT + 2.0) {
        fprintf(stdout, "        (took %.2fs)\n", took);
        return 1;
    }
    if (0 >= pid || !gone(pid)) {
        fprintf(stdout, "        (helper %ld left behind)\n", (long) pid);
        return 1;
    }
    return 0;
}

static int hang_nofd(void)
{
    return stalled(H_HANG, 0);
}

static int hang_fd(void)
{
    return stalled(H_HANG, 1);
}

static int stop_nofd(void)
{
    return stalled(H_STOP, 0);
}

static int stop_fd(void)
{
    return stalled(H_STOP, 1);
}

static int answer_ignored(int what)
{
    helper_t h;
    int rc;

    h.pidfd = -1;
    h.what = what;
    pmix_util_runas_timeout = TIMEOUT;
    (void) signal(SIGCHLD, SIG_IGN);
    rc = pmix_util_run_as(OTHER_UID, OTHER_GID, NULL, 0, 0, helper, &h);
    (void) signal(SIGCHLD, SIG_DFL);
    if ((H_FAIL == what && -1 != rc) || (H_OK == what && 0 != rc)) {
        fprintf(stdout, "        (returned %d)\n", rc);
        return 1;
    }
    return 0;
}

static int fail_ignored(void)
{
    return answer_ignored(H_FAIL);
}

static int ok_ignored(void)
{
    return answer_ignored(H_OK);
}

static int opens(void)
{
    helper_t h;
    int fd;

    h.pidfd = -1;
    h.what = H_OPEN;
    pmix_util_runas_timeout = TIMEOUT;
    fd = pmix_util_run_as(OTHER_UID, OTHER_GID, NULL, 0, 1, helper, &h);
    if (0 > fd) {
        return 1;
    }
    close(fd);
    return 0;
}

int main(int argc, char **argv)
{
    (void) argc;
    (void) argv;

    setvbuf(stdout, NULL, _IONBF, 0);
    fprintf(stdout, "runas_bound: work done as another user is bounded by a timeout\n");
    if (0 != geteuid()) {
        fprintf(stdout, "  SKIP: not running as root, so this process cannot become another user\n");
        return 0;
    }

    run_case("a helper that never finishes is abandoned at the timeout", hang_nofd);
    run_case("...and when it was to return a descriptor", hang_fd);
    run_case("a helper that stops itself is abandoned at the timeout", stop_nofd);
    run_case("...and when it was to return a descriptor", stop_fd);
    run_case("a helper that fails is a failure when SIGCHLD is ignored", fail_ignored);
    run_case("a helper that succeeds is a success when SIGCHLD is ignored", ok_ignored);
    run_case("a helper's descriptor comes back", opens);

    fprintf(stdout, "runas_bound: %d passed, %d failed\n", npass, nfail);
    return (0 == nfail) ? 0 : 1;
}
