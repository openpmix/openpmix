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
 * Only a privileged process can become another user, so every case
 * that runs a helper needs root, and those are reported as SKIP without
 * it. Each runs in a forked child under alarm(): against an unbounded
 * library a stalled helper is a hang, and it is reported as one instead
 * of taking make check down with it.
 *
 * Test cases, without root:
 *
 *   the helper's groups                -> the gid first, then the
 *                                         others in order, the gid not
 *                                         twice
 *   ... more than fit                  -> as many as fit, the gid first
 *
 * and as root:
 *
 *   a user with more groups than
 *   setgroups() takes                  -> the helper runs, with the gid
 *                                         and as many groups as fit. It
 *                                         used to fail the drop
 *   on Linux, a server whose
 *   capabilities survive setuid()      -> the helper has none, and is
 *                                         refused a file only root may
 *                                         read. It used to keep
 *                                         CAP_DAC_OVERRIDE and read it
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
#include <grp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__linux__) && defined(HAVE_LINUX_CAPABILITY_H)
#    include <linux/securebits.h>
#    include <sys/prctl.h>
#    define TEST_CAPS 1
#else
#    define TEST_CAPS 0
#endif

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

enum { H_HANG, H_STOP, H_FAIL, H_OK, H_OPEN, H_GROUPS, H_ROOTFILE };

/* for H_ROOTFILE: a file only root may read */
static char rootfile[64];

/* for H_GROUPS: how many groups the helper should find, and room to
 * read them */
static gid_t *groups_buf = NULL;
static size_t groups_want = 0;

/* does the helper have exactly groups_want groups, OTHER_GID among
 * them? */
static int has_groups(void)
{
    int n, i;

    n = getgroups((int) groups_want, groups_buf);
    if (0 > n || (size_t) n != groups_want) {
        return -1;
    }
    for (i = 0; i < n; i++) {
        if (OTHER_GID == groups_buf[i]) {
            return 0;
        }
    }
    return -1;
}

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
    case H_GROUPS:
        return has_groups();
    case H_ROOTFILE: {
        /* success is being refused it */
        int fd = open(rootfile, O_RDONLY);

        if (0 <= fd) {
            close(fd);
            return -1;
        }
        return (EACCES == errno) ? 0 : -1;
    }
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

/* a user the host gave more groups than setgroups() takes */
static int many_groups(void)
{
    helper_t h;
    gid_t *gids;
    long max = sysconf(_SC_NGROUPS_MAX);
    size_t n, ngids;
    int rc;

    if (0 >= max) {
        fprintf(stdout, "        (no group limit to exceed)\n");
        return 0;
    }
    ngids = (size_t) max + 5;
    gids = (gid_t *) malloc(ngids * sizeof(gid_t));
    groups_buf = (gid_t *) malloc(((size_t) max + 1) * sizeof(gid_t));
    if (NULL == gids || NULL == groups_buf) {
        return 1;
    }
    for (n = 0; n < ngids; n++) {
        gids[n] = (gid_t) (20000 + n);
    }
    groups_want = (size_t) max;
    h.pidfd = -1;
    h.what = H_GROUPS;
    pmix_util_runas_timeout = 4 * TIMEOUT;
    rc = pmix_util_run_as(OTHER_UID, OTHER_GID, gids, ngids, 0, helper, &h);
    free(gids);
    if (0 != rc) {
        fprintf(stdout, "        (returned %d, errno %d, %lu groups given, %ld taken)\n", rc,
                errno, (unsigned long) ngids, max);
        return 1;
    }
    return 0;
}

#if TEST_CAPS
/* A process whose capabilities survive setuid() - one given CAP_SETUID
 * by file capabilities is in the same state - must not pass them to the
 * helper */
static int caps_kept(void)
{
    helper_t h;
    int fd, rc;

    if (0 != prctl(PR_SET_SECUREBITS, SECBIT_NO_SETUID_FIXUP, 0, 0, 0)) {
        fprintf(stdout, "        (cannot keep capabilities across setuid here: %s)\n",
                strerror(errno));
        return 0;
    }
    snprintf(rootfile, sizeof(rootfile), "/tmp/runas_caps_XXXXXX");
    fd = mkstemp(rootfile);
    if (0 > fd) {
        return 1;
    }
    close(fd);
    (void) chmod(rootfile, S_IRUSR | S_IWUSR);
    h.pidfd = -1;
    h.what = H_ROOTFILE;
    pmix_util_runas_timeout = 4 * TIMEOUT;
    rc = pmix_util_run_as(OTHER_UID, OTHER_GID, NULL, 0, 0, helper, &h);
    (void) unlink(rootfile);
    if (0 != rc) {
        fprintf(stdout, "        (the helper read a file only root may, or did not run)\n");
        return 1;
    }
    return 0;
}
#endif

/* the list the helper's groups are made from */
static void test_group_list(void)
{
    const gid_t gids[] = {7, OTHER_GID, 8, 9};
    gid_t out[8];
    size_t n;

    n = pmix_util_runas_groups(OTHER_GID, gids, 4, out, 8);
    report("the helper's groups are the gid, then the others in order, the gid once",
           4 == n && OTHER_GID == out[0] && 7 == out[1] && 8 == out[2] && 9 == out[3]);

    n = pmix_util_runas_groups(OTHER_GID, gids, 4, out, 2);
    report("...as many as fit, the gid first",
           2 == n && OTHER_GID == out[0] && 7 == out[1]);

    n = pmix_util_runas_groups(OTHER_GID, NULL, 0, out, 8);
    report("...and the gid alone when the user has no others",
           1 == n && OTHER_GID == out[0]);
}

int main(int argc, char **argv)
{
    (void) argc;
    (void) argv;

    setvbuf(stdout, NULL, _IONBF, 0);
    fprintf(stdout, "runas_bound: work done as another user is bounded by a timeout\n");
    test_group_list();
    if (0 != geteuid()) {
        fprintf(stdout, "  SKIP: not running as root, so this process cannot become another user\n");
        fprintf(stdout, "runas_bound: %d passed, %d failed\n", npass, nfail);
        return (0 == nfail) ? 0 : 1;
    }

    run_case("a helper that never finishes is abandoned at the timeout", hang_nofd);
    run_case("...and when it was to return a descriptor", hang_fd);
    run_case("a helper that stops itself is abandoned at the timeout", stop_nofd);
    run_case("...and when it was to return a descriptor", stop_fd);
    run_case("a helper that fails is a failure when SIGCHLD is ignored", fail_ignored);
    run_case("a helper that succeeds is a success when SIGCHLD is ignored", ok_ignored);
    run_case("a helper's descriptor comes back", opens);
    run_case("a user with more groups than setgroups() takes is still served", many_groups);
#if TEST_CAPS
    run_case("a helper keeps no capability its server held across setuid()", caps_kept);
#endif

    fprintf(stdout, "runas_bound: %d passed, %d failed\n", npass, nfail);
    return (0 == nfail) ? 0 : 1;
}
