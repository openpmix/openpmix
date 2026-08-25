/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Unit tests for pmix_few(): fork, exec, wait for a child.
 *
 * Includes the regression for the child's exit path.  A forked child
 * shares the parent's stdio buffers, so if it leaves through exit()
 * rather than _exit() it flushes whatever the parent had written and
 * not yet flushed - and that output appears twice.  That case is run
 * inside a child of our own, with its stdout on a pipe so it is fully
 * buffered, because it cannot be observed on a terminal.
 *
 * No PMIx_Init: pmix_few is a leaf utility, and its own header warns
 * that it must not be used once a SIGCHLD handler is in place.
 *
 * Exit 0 if all tests pass, 1 otherwise.
 */

#include "src/include/pmix_config.h"
#include "src/include/pmix_globals.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "src/util/pmix_few.h"

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

static bool have_sh(void)
{
    struct stat sb;
    return (0 == stat("/bin/sh", &sb));
}

/* ------------------------------------------------------------------ */

static void test_child_exit_code(void)
{
    char *argv[] = {"/bin/sh", "-c", "exit 7", NULL};
    /* poison it, so a status the callee never writes is visible as such */
    int status = 0x5a5a5a5a;
    pmix_status_t rc;

    rc = pmix_few(argv, &status);
    report("few: a child that ran returns SUCCESS", PMIX_SUCCESS == rc);
    report("few: status decodes as a normal exit", WIFEXITED(status));
    report("few: the child's exit code comes through",
           WIFEXITED(status) && 7 == WEXITSTATUS(status));
}

static void test_child_signal(void)
{
    char *argv[] = {"/bin/sh", "-c", "kill -TERM $$", NULL};
    int status = 0x5a5a5a5a;
    pmix_status_t rc;

    /* a child that dies on a signal is still a child that ran */
    rc = pmix_few(argv, &status);
    report("few: a signalled child still returns SUCCESS", PMIX_SUCCESS == rc);
    report("few: status decodes as a signal death",
           WIFSIGNALED(status) && SIGTERM == WTERMSIG(status));
}

static void test_path_search(void)
{
    char *argv[] = {"sh", "-c", "exit 3", NULL};
    int status = 0x5a5a5a5a;
    pmix_status_t rc;

    /* argv[0] is handed to execvp, so a bare name is searched for on
     * PATH - that is the documented contract */
    rc = pmix_few(argv, &status);
    report("few: a bare name is resolved against PATH",
           PMIX_SUCCESS == rc && WIFEXITED(status) && 3 == WEXITSTATUS(status));
}

static void test_exec_failure_reports_errno(void)
{
    char *argv[] = {"/nonexistent/pmix-util-few-probe", NULL};
    int status = 0x5a5a5a5a;
    pmix_status_t rc;

    /* the fork and the wait both worked, so this is a success as far as
     * pmix_few is concerned; the failure shows up as the child's exit
     * status, which carries errno */
    rc = pmix_few(argv, &status);
    report("few: a failed exec is still SUCCESS (the child ran and exited)",
           PMIX_SUCCESS == rc);
    report("few: the child carries errno out as its exit status",
           WIFEXITED(status) && ENOENT == WEXITSTATUS(status));
}

/* ------------------------------------------------------------------ */
/* the stdio regression                                                */
/* ------------------------------------------------------------------ */

#define MARKER "PMIX-FEW-BUFFERED-MARKER"

static void run_flush_child(int write_fd)
{
    char *argv[] = {"/nonexistent/pmix-util-few-probe", NULL};
    int status = 0;

    if (write_fd != STDOUT_FILENO) {
        if (0 > dup2(write_fd, STDOUT_FILENO)) {
            _exit(1);
        }
        close(write_fd);
    }
    /* stdout is a pipe now, so this sits in the buffer unflushed */
    printf("%s\n", MARKER);

    /* the exec inside here fails, and the grandchild leaves.  If it
     * leaves through exit() it flushes OUR buffer on the way out. */
    (void) pmix_few(argv, &status);

    /* exactly one flush is owed, and it is this one */
    fflush(stdout);
    _exit(0);
}

static int count_markers(const char *buf)
{
    const char *p = buf;
    int n = 0;

    while (NULL != (p = strstr(p, MARKER))) {
        n++;
        p += strlen(MARKER);
    }
    return n;
}

static void test_child_does_not_flush_our_stdio(void)
{
    int p[2];
    pid_t child;
    char buf[4096];
    ssize_t got, total = 0;
    int wstatus = 0;

    if (0 != pipe(p)) {
        report("few_no_double_flush: fixture", 0);
        return;
    }
    child = fork();
    if (0 > child) {
        report("few_no_double_flush: fixture (fork)", 0);
        close(p[0]);
        close(p[1]);
        return;
    }
    if (0 == child) {
        close(p[0]);
        run_flush_child(p[1]);
        /* not reached */
    }
    close(p[1]);
    memset(buf, 0, sizeof(buf));
    while (0 < (got = read(p[0], buf + total, sizeof(buf) - 1 - (size_t) total))) {
        total += got;
        if ((size_t) total >= sizeof(buf) - 1) {
            break;
        }
    }
    close(p[0]);
    waitpid(child, &wstatus, 0);

    /* two of these means the grandchild flushed a buffer it did not
     * own - which is what exit() does and _exit() does not */
    report("few: the exec'd child does not flush the parent's stdio",
           1 == count_markers(buf));
}

/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    PMIX_HIDE_UNUSED_PARAMS(argc, argv);

    fprintf(stdout, "\n=== pmix_few unit tests ===\n\n");

    if (!have_sh()) {
        fprintf(stdout, "  /bin/sh not present - skipping the shell cases\n");
    } else {
        test_child_exit_code();
        test_child_signal();
        test_path_search();
    }
    test_exec_failure_reports_errno();
    test_child_does_not_flush_our_stdio();

    fprintf(stdout, "\nResults: %d passed, %d failed\n\n", npass, nfail);

    return (nfail > 0) ? 1 : 0;
}
