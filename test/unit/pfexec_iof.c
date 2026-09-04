/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/*
 * The output directives a spawn carries must reach a job we fork/exec
 * ourselves.
 *
 * PMIx_Spawn(3) offers the fork/exec fallback so that a tool can "maintain
 * a single code path for both the connected and disconnected cases", which
 * makes the directives' effect on output the thing that has to match. There
 * are two halves to that, and they arrive by different routes:
 *
 *  - The FORMATTING flags (tag, rank, timestamp, xml, output-to-file, ...)
 *    reach the spawned namespace through the job description: pfexec's
 *    register_nspace() copies the spawn's job-level info into it, and
 *    gds/hash runs the same pmix_iof_check_flags() over that description
 *    that the spawn parser would have. Nothing on this path calls the
 *    parser, so that indirection is the whole mechanism - which is why it
 *    is worth a test rather than an assumption.
 *
 *  - The CHANNEL set is decided only by pmix_server_spawn_parser(), and
 *    the fork/exec dispatch used not to call it. PMIX_FWD_STDOUT set false
 *    is documented as a request for silence, and a connected launcher
 *    honors it by registering no subscription for the channel; a
 *    disconnected one printed the output anyway.
 *
 * The test comes up as a disconnected LAUNCHER, which is what selects the
 * fork/exec path, and stands pipes up in place of its own stdout and
 * stderr so it can read back exactly what the library wrote.
 */

#include "src/include/pmix_config.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "include/pmix.h"
#include "include/pmix_tool.h"

#include "src/common/pmix_pfexec.h"
#include "src/include/pmix_globals.h"
#include "src/server/pmix_server_ops.h"

#define WAIT_USEC    20000
#define WAIT_TRIES   500
#define SETTLE_TRIES 50
#define BUFSIZE      4096

/* what the spawned shell says on each of its two streams */
#define OUT_MARK "PFEXEC-STDOUT-MARK"
#define ERR_MARK "PFEXEC-STDERR-MARK"

static FILE *out = NULL; /* the real stdout, saved before we take it */
static int rfd = -1;     /* read end of the pipe standing in for stdout */
static int efd = -1;     /* read end of the pipe standing in for stderr */
static int failures = 0;

static void report(const char *what, bool pass)
{
    fprintf(out, "%-64s : %s\n", what, pass ? "PASS" : "FAIL");
    fflush(out);
    if (!pass) {
        ++failures;
    }
}

/* Put a pipe where "fd" was and hand back the read end, non-blocking so
 * the collectors below can poll it. */
static int capture(int fd)
{
    int p[2];

    if (0 > pipe(p)) {
        fprintf(out, "pipe failed: %s\n", strerror(errno));
        return -1;
    }
    if (0 > dup2(p[1], fd)) {
        fprintf(out, "dup2 failed: %s\n", strerror(errno));
        close(p[0]);
        close(p[1]);
        return -1;
    }
    close(p[1]);
    if (0 > fcntl(p[0], F_SETFL, O_NONBLOCK)) {
        fprintf(out, "fcntl failed: %s\n", strerror(errno));
        close(p[0]);
        return -1;
    }
    return p[0];
}

/* Read from a captured stream until "want" appears in it or we run out of
 * patience. Returns whether it appeared; "buf" holds everything seen. */
static bool collect_until(int fd, char *buf, size_t bufsize, const char *want)
{
    size_t got = 0;
    int i;
    ssize_t n;

    buf[0] = '\0';
    for (i = 0; i < WAIT_TRIES; i++) {
        n = read(fd, &buf[got], bufsize - 1 - got);
        if (0 < n) {
            got += (size_t) n;
            buf[got] = '\0';
            if (NULL != strstr(buf, want)) {
                return true;
            }
            continue;
        }
        usleep(WAIT_USEC);
    }
    return false;
}

/* Give the progress thread a good run at a stream we expect to stay
 * quiet, and report everything that did come out of it. */
static size_t settle(int fd, char *buf, size_t bufsize)
{
    size_t got = 0;
    int i;
    ssize_t n;

    for (i = 0; i < SETTLE_TRIES; i++) {
        usleep(WAIT_USEC);
        n = read(fd, &buf[got], bufsize - 1 - got);
        if (0 < n) {
            got += (size_t) n;
        }
    }
    buf[got] = '\0';
    return got;
}

/* Empty a captured stream so one case cannot read the previous case's
 * bytes. Nothing is asserted about what comes off it. */
static void drain(int fd)
{
    char scratch[BUFSIZE];
    int i;

    for (i = 0; i < SETTLE_TRIES; i++) {
        usleep(WAIT_USEC);
        while (0 < read(fd, scratch, sizeof(scratch))) {
            /* keep going until the pipe is empty */
        }
    }
}

/* Fork/exec a shell that writes one line to each of its streams. */
static bool spawn_talker(pmix_info_t *jinfo, size_t njinfo, pmix_nspace_t nspace)
{
    pmix_app_t app;
    pmix_status_t rc;

    PMIX_APP_CONSTRUCT(&app);
    app.cmd = strdup("/bin/sh");
    PMIx_Argv_append_nosize(&app.argv, "/bin/sh");
    PMIx_Argv_append_nosize(&app.argv, "-c");
    PMIx_Argv_append_nosize(&app.argv, "echo " OUT_MARK "; echo " ERR_MARK " 1>&2");
    app.maxprocs = 1;

    rc = PMIx_Spawn(jinfo, njinfo, &app, 1, nspace);
    PMIX_APP_DESTRUCT(&app);
    if (PMIX_SUCCESS != rc) {
        fprintf(out, "PMIx_Spawn failed: %s\n", PMIx_Error_string(rc));
        fflush(out);
        return false;
    }
    return true;
}

/* Drive pfexec the way pmix_server_spawn()'s fallback does, rather than
 * the way PMIx_Spawn does.
 *
 * That path serves a spawn a CLIENT sent us, and a client is not a tool:
 * the parser's "forward everything to me" default does not apply to it,
 * so the caddy pfexec is handed names no channel and asks to inherit
 * instead. Reading that empty channel set as a request for silence would
 * mute every such spawn. There is no client here to send one, so the
 * caddy is built directly - which is all that fallback does before
 * calling the same entry point.
 */
static pmix_status_t inherit_status = PMIX_ERR_NOT_SUPPORTED;
static volatile bool inherit_done = false;

static void inherit_spcbfunc(pmix_status_t status, char nspace[], void *cbdata)
{
    (void) nspace;
    (void) cbdata;
    inherit_status = status;
    inherit_done = true;
}

static bool spawn_inheriting(void)
{
    pmix_setup_caddy_t *fcd;
    pmix_status_t rc;
    int i;

    fcd = PMIX_NEW(pmix_setup_caddy_t);
    if (NULL == fcd) {
        fprintf(out, "caddy allocation failed\n");
        return false;
    }
    fcd->napps = 1;
    PMIX_APP_CREATE(fcd->apps, 1);
    if (NULL == fcd->apps) {
        PMIX_RELEASE(fcd);
        fprintf(out, "app allocation failed\n");
        return false;
    }
    fcd->copied = true;
    fcd->apps[0].cmd = strdup("/bin/sh");
    PMIx_Argv_append_nosize(&fcd->apps[0].argv, "/bin/sh");
    PMIx_Argv_append_nosize(&fcd->apps[0].argv, "-c");
    PMIx_Argv_append_nosize(&fcd->apps[0].argv,
                            "echo " OUT_MARK "; echo " ERR_MARK " 1>&2");
    fcd->apps[0].maxprocs = 1;
    /* exactly what the parser leaves for a spawn from a non-tool peer
     * that named no output channel */
    fcd->channels = PMIX_FWD_NO_CHANNELS;
    fcd->inherit_iof = true;
    fcd->spcbfunc = inherit_spcbfunc;

    rc = pmix_pfexec_base_spawn_job(fcd);
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(fcd);
        fprintf(out, "pmix_pfexec_base_spawn_job failed: %s\n",
                PMIx_Error_string(rc));
        return false;
    }
    for (i = 0; i < WAIT_TRIES && !inherit_done; i++) {
        usleep(WAIT_USEC);
    }
    if (!inherit_done) {
        fprintf(out, "inheriting spawn never completed\n");
        return false;
    }
    if (PMIX_SUCCESS != inherit_status) {
        fprintf(out, "inheriting spawn failed: %s\n",
                PMIx_Error_string(inherit_status));
        return false;
    }
    return true;
}

int main(int argc, char **argv)
{
    pmix_proc_t myproc;
    pmix_info_t tinfo[2], jinfo[1];
    pmix_nspace_t nspace;
    pmix_status_t rc;
    char buf[BUFSIZE], want[BUFSIZE];
    int savedout;
    bool f = false;
    (void) argc;
    (void) argv;

    /* keep a handle on the real stdout before we take it away, so the
     * results can still be reported */
    fflush(stdout);
    savedout = dup(fileno(stdout));
    if (0 > savedout) {
        fprintf(stderr, "dup of stdout failed: %s\n", strerror(errno));
        return 1;
    }
    out = fdopen(savedout, "w");
    if (NULL == out) {
        fprintf(stderr, "fdopen failed: %s\n", strerror(errno));
        return 1;
    }

    fprintf(out, "\n=== PMIx fork/exec spawn IOF directive unit test ===\n\n");
    fflush(out);

    /* the sinks are built on fd 1 and fd 2 during init, so the
     * substitution has to be in place before that runs */
    fflush(stderr);
    rfd = capture(fileno(stdout));
    efd = capture(fileno(stderr));
    if (0 > rfd || 0 > efd) {
        return 1;
    }

    /* a LAUNCHER is what opens pfexec, and DO_NOT_CONNECT is what makes
     * PMIx_Spawn fork/exec locally instead of asking a server */
    PMIX_INFO_LOAD(&tinfo[0], PMIX_LAUNCHER, NULL, PMIX_BOOL);
    PMIX_INFO_LOAD(&tinfo[1], PMIX_TOOL_DO_NOT_CONNECT, NULL, PMIX_BOOL);
    rc = PMIx_tool_init(&myproc, tinfo, 2);
    PMIX_INFO_DESTRUCT(&tinfo[0]);
    PMIX_INFO_DESTRUCT(&tinfo[1]);
    if (PMIX_SUCCESS != rc) {
        fprintf(out, "PMIx_tool_init failed: %s\n", PMIx_Error_string(rc));
        return 1;
    }
    report("disconnected launcher initialized", true);

    /* ---- a spawn carrying no directives at all still speaks ---- */
    if (!spawn_talker(NULL, 0, nspace)) {
        return 1;
    }
    report("plain spawn: stdout appears",
           collect_until(rfd, buf, sizeof(buf), OUT_MARK));
    report("plain spawn: stderr appears",
           collect_until(efd, buf, sizeof(buf), ERR_MARK));
    drain(rfd);
    drain(efd);

    /* ---- a formatting directive reaches the fork/exec'd namespace ----
     *
     * The tag names the spawned job and the rank inside it, so it is also
     * the check that the flags landed on THAT namespace rather than on
     * the process-wide defaults. */
    PMIX_INFO_LOAD(&jinfo[0], PMIX_IOF_TAG_OUTPUT, NULL, PMIX_BOOL);
    if (!spawn_talker(jinfo, 1, nspace)) {
        return 1;
    }
    PMIX_INFO_DESTRUCT(&jinfo[0]);
    snprintf(want, sizeof(want), "[%s,0]<stdout>: %s", nspace, OUT_MARK);
    report("PMIX_IOF_TAG_OUTPUT: stdout carries this job's tag",
           collect_until(rfd, buf, sizeof(buf), want));
    snprintf(want, sizeof(want), "[%s,0]<stderr>: %s", nspace, ERR_MARK);
    report("PMIX_IOF_TAG_OUTPUT: stderr carries this job's tag",
           collect_until(efd, buf, sizeof(buf), want));
    drain(rfd);
    drain(efd);

    /* ---- a channel turned off is a request for silence ---- */
    PMIX_INFO_LOAD(&jinfo[0], PMIX_FWD_STDOUT, &f, PMIX_BOOL);
    if (!spawn_talker(jinfo, 1, nspace)) {
        return 1;
    }
    PMIX_INFO_DESTRUCT(&jinfo[0]);
    /* the surviving channel is also the sentinel saying the job has run
     * and been drained, so the silence below is a real absence rather
     * than an answer we were too early to see */
    report("PMIX_FWD_STDOUT false: stderr still appears",
           collect_until(efd, buf, sizeof(buf), ERR_MARK));
    settle(rfd, buf, sizeof(buf));
    report("PMIX_FWD_STDOUT false: nothing is written to stdout",
           NULL == strstr(buf, OUT_MARK));
    drain(rfd);
    drain(efd);

    /* ---- and the same for the other channel ---- */
    PMIX_INFO_LOAD(&jinfo[0], PMIX_FWD_STDERR, &f, PMIX_BOOL);
    if (!spawn_talker(jinfo, 1, nspace)) {
        return 1;
    }
    PMIX_INFO_DESTRUCT(&jinfo[0]);
    report("PMIX_FWD_STDERR false: stdout still appears",
           collect_until(rfd, buf, sizeof(buf), OUT_MARK));
    settle(efd, buf, sizeof(buf));
    report("PMIX_FWD_STDERR false: nothing is written to stderr",
           NULL == strstr(buf, ERR_MARK));
    drain(rfd);
    drain(efd);

    /* ---- naming no channel asks to inherit, not to be silent ---- */
    if (!spawn_inheriting()) {
        return 1;
    }
    report("inherited channels: stdout appears",
           collect_until(rfd, buf, sizeof(buf), OUT_MARK));
    report("inherited channels: stderr appears",
           collect_until(efd, buf, sizeof(buf), ERR_MARK));
    drain(rfd);
    drain(efd);

    rc = PMIx_tool_finalize();
    report("launcher finalized", PMIX_SUCCESS == rc);

    fprintf(out, "\n%s: %d failure(s)\n", (0 == failures) ? "SUCCESS" : "FAILURE",
            failures);
    fflush(out);
    return (0 == failures) ? 0 : 1;
}
