/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Unit tests for the pmix_tty helpers.
 *
 * These are thin wrappers over tcgetattr/tcsetattr and the winsize
 * ioctls, so the round-trip cases below are cheap.  The case worth the
 * setup is the one that does not appear in a round trip at all: a
 * caller that is told a terminal is in raw mode when it is not will
 * read line-buffered, echoed input forever, and nothing about the
 * descriptor it holds says so.  pmix_settermios() therefore has to
 * report a set it could not make.
 *
 * Making tcsetattr() fail on demand is the hard part.  POSIX gives one
 * portable lever: tcsetattr() on a process's *controlling* terminal
 * from a process group that is both in the background and orphaned
 * fails with EIO, and no SIGTTOU is delivered.  Building that position
 * takes three processes - a session leader that acquires the pty as its
 * controlling terminal, a child that moves itself into its own process
 * group, and a grandchild that is left orphaned when that child exits.
 * The session leader has to stay alive throughout: when the controlling
 * process dies the terminal is disassociated from the session and, on
 * BSD-derived systems, the slave is revoked out from under the probe.
 *
 * The probe validates its own lever - it calls tcsetattr() directly
 * first and skips if that succeeded - so a platform that does not
 * implement the rule reports a skip rather than a spurious failure.
 *
 * Exit 0 if all tests pass, 1 otherwise.
 */

#include "src/include/pmix_config.h"
#include "src/include/pmix_globals.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include "src/util/pmix_pty.h"
#include "src/util/pmix_tty.h"

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

#if PMIX_ENABLE_PTY_SUPPORT

/* Verdicts the orphaned-process-group probe hands back.  They are exit
 * codes, so they have to be small and must not collide with the shell's
 * signal encoding. */
#define PROBE_REPORTED  0  /* pmix_settermios reported the failure */
#define PROBE_SILENT    1  /* it claimed success - the defect */
#define PROBE_SKIP      2  /* tcsetattr succeeded; lever unavailable */
#define PROBE_SETUP     3  /* could not build the position at all */

static int open_pty(int *master, int *slave)
{
    return pmix_openpty(master, slave, NULL, NULL, NULL);
}

/* ------------------------------------------------------------------ */

static void test_gettermios(void)
{
    struct termios t;
    int master, slave, devnull;

    if (0 != open_pty(&master, &slave)) {
        report("gettermios: pty setup (skipped)", 1);
        return;
    }
    report("gettermios: reads a terminal's settings",
           PMIX_SUCCESS == pmix_gettermios(slave, &t));
    close(slave);
    close(master);

    devnull = open("/dev/null", O_RDWR);
    if (0 <= devnull) {
        report("gettermios: refuses a descriptor that is not a terminal",
               PMIX_SUCCESS != pmix_gettermios(devnull, &t));
        report("settermios: refuses a descriptor that is not a terminal",
               PMIX_SUCCESS != pmix_settermios(devnull, &t));
        close(devnull);
    }
}

static void test_winsize(void)
{
    struct winsize ws, back;
    int master, slave;

    if (0 != open_pty(&master, &slave)) {
        report("winsize: pty setup (skipped)", 1);
        return;
    }

    memset(&ws, 0, sizeof(ws));
    ws.ws_row = 41;
    ws.ws_col = 137;
    report("setwinsz: sets a window size",
           PMIX_SUCCESS == pmix_setwinsz(slave, &ws));

    memset(&back, 0, sizeof(back));
    report("getwinsz: reads a window size",
           PMIX_SUCCESS == pmix_getwinsz(slave, &back));
    report("getwinsz: reads back what setwinsz wrote",
           41 == back.ws_row && 137 == back.ws_col);

    close(slave);
    close(master);
}

static void test_setraw(void)
{
    struct termios before, prior, now;
    int master, slave;

    if (0 != open_pty(&master, &slave)) {
        report("setraw: pty setup (skipped)", 1);
        return;
    }
    if (PMIX_SUCCESS != pmix_gettermios(slave, &before)) {
        report("setraw: pty setup (skipped)", 1);
        close(slave);
        close(master);
        return;
    }

    memset(&prior, 0, sizeof(prior));
    report("setraw: succeeds on a terminal",
           PMIX_SUCCESS == pmix_setraw(slave, &prior));
    report("setraw: hands back the settings it replaced",
           prior.c_iflag == before.c_iflag && prior.c_oflag == before.c_oflag
               && prior.c_cflag == before.c_cflag
               && prior.c_lflag == before.c_lflag);

    if (PMIX_SUCCESS == pmix_gettermios(slave, &now)) {
        report("setraw: turns off canonical mode, echo, signals and extensions",
               0 == (now.c_lflag & (ICANON | ISIG | IEXTEN | ECHO)));
        report("setraw: turns off input and output post-processing",
               0 == (now.c_iflag & (BRKINT | ICRNL | IGNBRK | IGNCR | INLCR
                                    | INPCK | ISTRIP | IXON | PARMRK))
                   && 0 == (now.c_oflag & OPOST));
        report("setraw: asks for character-at-a-time blocking input",
               1 == now.c_cc[VMIN] && 0 == now.c_cc[VTIME]);
    } else {
        report("setraw: read back the raw settings", 0);
    }

    /* a NULL 'prior' is documented as "do not tell me what I replaced" */
    report("setraw: accepts a NULL prior", PMIX_SUCCESS == pmix_setraw(slave, NULL));

    /* and the settings setraw handed back must put the terminal back */
    report("settermios: restores the settings setraw saved",
           PMIX_SUCCESS == pmix_settermios(slave, &prior));
    if (PMIX_SUCCESS == pmix_gettermios(slave, &now)) {
        /* compare the settings, not the whole field: re-enabling
         * canonical mode makes the driver raise its own status bits in
         * c_lflag (PENDIN on macOS), which nobody asked for */
        report("settermios: the restore actually took",
               (now.c_lflag & (ICANON | ISIG | IEXTEN | ECHO))
                       == (before.c_lflag & (ICANON | ISIG | IEXTEN | ECHO))
                   && now.c_iflag == before.c_iflag
                   && now.c_oflag == before.c_oflag);
    } else {
        report("settermios: read back the restored settings", 0);
    }

    close(slave);
    close(master);
}

/* ------------------------------------------------------------------ */

/* Runs in the grandchild: a background, orphaned process group whose
 * controlling terminal is 'slave'.  Writes one verdict byte to 'wfd'. */
static void orphan_probe(int slave, int wfd)
{
    struct termios t, want;
    pid_t was = getppid();
    char verdict;
    int i;

    /* wait to be reparented - the process group is not orphaned until
     * the intermediate child is gone */
    for (i = 0; i < 5000 && getppid() == was; i++) {
        usleep(1000);
    }

    if (0 != tcgetattr(slave, &t)) {
        verdict = PROBE_SETUP;
        goto done;
    }
    want = t;
    want.c_lflag &= ~ECHO;

    /* validate the lever before trusting the result */
    if (0 == tcsetattr(slave, TCSANOW, &want)) {
        verdict = PROBE_SKIP;
        goto done;
    }

    if (PMIX_SUCCESS == pmix_settermios(slave, &want)) {
        verdict = PROBE_SILENT;
    } else {
        verdict = PROBE_REPORTED;
    }

done:
    if (1 != write(wfd, &verdict, 1)) {
        _exit(PROBE_SETUP);
    }
    _exit(0);
}

static int run_orphan_probe(void)
{
    int pfd[2], master, slave;
    pid_t leader;
    int status = 0;

    if (0 != pipe(pfd)) {
        return PROBE_SETUP;
    }

    leader = fork();
    if (0 > leader) {
        close(pfd[0]);
        close(pfd[1]);
        return PROBE_SETUP;
    }

    if (0 == leader) {
        pid_t mid;
        char verdict = PROBE_SETUP;

        /* nothing here should block, but a wait that did would wedge
         * make check rather than fail it */
        alarm(30);
        if (0 > setsid()) {
            _exit(PROBE_SETUP);
        }
        if (0 != pmix_openpty(&master, &slave, NULL, NULL, NULL)) {
            _exit(PROBE_SETUP);
        }
        /* pmix_openpty deliberately opens with O_NOCTTY, so ask for the
         * controlling terminal explicitly - the background-process-group
         * rules only apply to it */
        if (0 != ioctl(slave, TIOCSCTTY, 0)) {
            _exit(PROBE_SETUP);
        }
        mid = fork();
        if (0 > mid) {
            _exit(PROBE_SETUP);
        }
        if (0 == mid) {
            if (0 > setpgid(0, 0)) {
                _exit(PROBE_SETUP);
            }
            if (0 == fork()) {
                orphan_probe(slave, pfd[1]);
            }
            /* exiting here is what orphans the probe's process group */
            _exit(0);
        }
        waitpid(mid, NULL, 0);
        close(pfd[1]);
        if (1 != read(pfd[0], &verdict, 1)) {
            _exit(PROBE_SETUP);
        }
        /* stay alive until the verdict is in: killing the controlling
         * process disassociates the terminal and, on BSD, revokes the
         * slave the probe is holding */
        _exit((int) verdict);
    }

    close(pfd[0]);
    close(pfd[1]);
    if (0 > waitpid(leader, &status, 0)) {
        return PROBE_SETUP;
    }
    if (!WIFEXITED(status)) {
        return PROBE_SETUP;
    }
    return WEXITSTATUS(status);
}

static void test_reports_a_set_it_could_not_make(void)
{
    int rc = run_orphan_probe();

    if (PROBE_SETUP == rc || PROBE_SKIP == rc) {
        report("settermios: reports a set the terminal refused (skipped)", 1);
        return;
    }
    report("settermios: reports a set the terminal refused",
           PROBE_REPORTED == rc);
}

#endif /* PMIX_ENABLE_PTY_SUPPORT */

/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    PMIX_HIDE_UNUSED_PARAMS(argc, argv);

    fprintf(stdout, "\n=== pmix_tty unit tests ===\n\n");

#if PMIX_ENABLE_PTY_SUPPORT
    test_gettermios();
    test_winsize();
    test_setraw();
    test_reports_a_set_it_could_not_make();
#else
    fprintf(stdout, "  (PMIX_ENABLE_PTY_SUPPORT is 0: no pty to test against)\n");
#endif

    fprintf(stdout, "\nResults: %d passed, %d failed\n\n", npass, nfail);
    return (nfail > 0) ? 1 : 0;
}
