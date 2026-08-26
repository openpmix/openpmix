/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Unit tests for the pmix_pty helpers.
 *
 * The interesting property here is one that does not show up in the
 * returned descriptors at all: opening a terminal device from a session
 * leader that has no controlling terminal *gives* it one, and these
 * helpers run in the process that is setting a child up, not in the
 * child.  A PMIx server started as a daemon is exactly a session leader
 * with no controlling terminal, so a helper that takes one on its behalf
 * ties the server's fate to a pty it merely handed to a child.
 *
 * Every case runs in a forked child that calls setsid() first, so the
 * starting state is the same whether or not the test harness itself has
 * a terminal.
 *
 * Exit 0 if all tests pass, 1 otherwise.
 */

#include "src/include/pmix_config.h"
#include "src/include/pmix_globals.h"

#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "src/util/pmix_pty.h"

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

/* Exit codes a probe child can hand back. */
#define PROBE_OK        0
#define PROBE_HAS_CTTY  1
#define PROBE_SETUP     2
#define PROBE_FAILED    3
/* Not an exit code: the child died on a signal.  This is a *symptom* of
 * the same defect - once a process has taken the pty as its controlling
 * terminal, closing the last master descriptor hangs it up, so the child
 * is killed by SIGHUP before it can report anything. */
#define PROBE_SIGNALED  4

static int have_ctty(void)
{
    int fd = open("/dev/tty", O_RDWR);
    if (0 <= fd) {
        close(fd);
        return 1;
    }
    return 0;
}

/* Run body() in a child that is a session leader with no controlling
 * terminal, and return the child's exit status. */
static int run_probe(int (*body)(void))
{
    pid_t pid;
    int status = 0;

    pid = fork();
    if (0 > pid) {
        return PROBE_SETUP;
    }
    if (0 == pid) {
        /* nothing here should block, but a pty read that does would
         * otherwise wedge make check rather than fail it */
        alarm(30);
        if (0 > setsid()) {
            _exit(PROBE_SETUP);
        }
        if (have_ctty()) {
            /* setsid() should have detached us from any terminal */
            _exit(PROBE_SETUP);
        }
        _exit(body());
    }
    if (0 > waitpid(pid, &status, 0)) {
        return PROBE_SETUP;
    }
    if (!WIFEXITED(status)) {
        return PROBE_SIGNALED;
    }
    return WEXITSTATUS(status);
}

/* ------------------------------------------------------------------ */

static int probe_openpty(void)
{
    int master = -1;
    int slave = -1;
    char buf[8];
    ssize_t n;

    if (0 != pmix_openpty(&master, &slave, NULL, NULL, NULL)) {
        return PROBE_FAILED;
    }
    if (0 > master || 0 > slave) {
        return PROBE_FAILED;
    }
    /* The pair has to actually carry data.  The slave comes up in
     * canonical mode, so the line has to be terminated or the read
     * below never returns. */
    if (3 != write(master, "hi\n", 3)) {
        return PROBE_FAILED;
    }
    n = read(slave, buf, sizeof(buf));
    if (2 > n || 0 != memcmp(buf, "hi", 2)) {
        return PROBE_FAILED;
    }
    if (have_ctty()) {
        return PROBE_HAS_CTTY;
    }
    close(master);
    close(slave);
    return PROBE_OK;
}

static void test_openpty(void)
{
    int rc = run_probe(probe_openpty);

    if (PROBE_SETUP == rc) {
        report("openpty: probe setup (skipped)", 1);
        return;
    }
    report("openpty: opens a working master/slave pair", PROBE_FAILED != rc);
    report("openpty: leaves the caller without a controlling terminal",
           PROBE_OK == rc);
}

#if PMIX_ENABLE_PTY_SUPPORT

static int probe_ptymopen(void)
{
    char name[64];
    int master;
    int slave;

    master = pmix_ptymopen(name, sizeof(name));
    if (0 > master) {
        return PROBE_FAILED;
    }
    if (have_ctty()) {
        close(master);
        return PROBE_HAS_CTTY;
    }
    slave = pmix_ptysopen(master, name);
    if (0 > slave) {
        close(master);
        return PROBE_FAILED;
    }
    if (have_ctty()) {
        close(master);
        close(slave);
        return PROBE_HAS_CTTY;
    }
    close(master);
    close(slave);
    return PROBE_OK;
}

static void test_ptymopen(void)
{
    int rc = run_probe(probe_ptymopen);

    if (PROBE_SETUP == rc) {
        report("ptymopen: probe setup (skipped)", 1);
        return;
    }
    report("ptymopen/ptysopen: open a master and its slave",
           PROBE_FAILED != rc);
    report("ptymopen/ptysopen: leave the caller without a controlling terminal",
           PROBE_OK == rc);
}

static void test_ptysopen_does_not_close_the_master(void)
{
    char name[64];
    int master;
    int rc;

    master = pmix_ptymopen(name, sizeof(name));
    if (0 > master) {
        report("ptysopen: master open (skipped)", 1);
        return;
    }

    /* a slave name that cannot be opened: the failure must be reported
     * without closing the descriptor the caller handed in, or the
     * caller's own close of it is a double close */
    rc = pmix_ptysopen(master, "/dev/null/no-such-pts");
    report("ptysopen: reports a failure it cannot recover from", 0 > rc);
    report("ptysopen: does not close the master it was handed",
           -1 != fcntl(master, F_GETFD));

    close(master);
}

static void test_ptymopen_short_buffer(void)
{
    char buf[8];
    int rc;

    memset(buf, 'Z', sizeof(buf));
    buf[sizeof(buf) - 1] = '\0';

    /* Too small to hold even the master device name.  strncpy() would
     * fill the buffer without a terminator and the open() that follows
     * would read past the end of it; the size has to be honored. */
    rc = pmix_ptymopen(buf, 4);
    report("ptymopen: refuses a buffer too small for the device name",
           0 > rc);
    report("ptymopen: leaves a too-small buffer untouched",
           0 == strcmp(buf, "ZZZZZZZ"));
    if (0 <= rc) {
        close(rc);
    }
}

#endif /* PMIX_ENABLE_PTY_SUPPORT */

/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    PMIX_HIDE_UNUSED_PARAMS(argc, argv);

    fprintf(stdout, "\n=== pmix_pty unit tests ===\n\n");

    test_openpty();
#if PMIX_ENABLE_PTY_SUPPORT
    test_ptymopen();
    test_ptysopen_does_not_close_the_master();
    test_ptymopen_short_buffer();
#else
    fprintf(stdout, "  (PMIX_ENABLE_PTY_SUPPORT is 0: helper cases skipped)\n");
#endif

    fprintf(stdout, "\nResults: %d passed, %d failed\n\n", npass, nfail);
    return (nfail > 0) ? 1 : 0;
}
