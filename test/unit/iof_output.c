/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/*
 * IOF output delivery: what happens at the end of a stream.
 *
 * A source that closes its output is announced by a zero-byte delivery.
 * Two things used to go wrong when one arrived, and both of them are
 * silent - the output simply never appears:
 *
 *  - The shared stdout/stderr sinks (fd 1 and fd 2) are fed by every
 *    source a server outputs for, but the write handler treated a
 *    zero-byte marker as "this channel is finished": it returned with
 *    the sink still flagged "pending" and its write event un-armed.
 *    Since output is only ever queued-and-armed when "pending" is clear,
 *    every later chunk from every other source then sat in the queue
 *    unwritten until finalize.
 *
 *  - Output that does not end in a newline is held back as a "residual"
 *    until the rest of the line shows up. The end of the stream is the
 *    last chance to write it, and the zero-byte path did not look: a
 *    server flushed it at finalize (late, and out of order) and a client
 *    or tool never flushed it at all.
 *
 * This drives the real path by standing a pipe up in place of stdout and
 * another in place of stderr, then handing the library output through
 * PMIx_server_IOF_deliver.
 */

#include "src/include/pmix_config.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "include/pmix.h"
#include "include/pmix_server.h"

#include "src/include/pmix_globals.h"
#include "src/mca/bfrops/bfrops.h"
#include "src/server/pmix_server_ops.h"

#define WAIT_USEC    20000
#define WAIT_TRIES   250
#define SETTLE_TRIES 25
#define BUFSIZE      4096

static FILE *out = NULL; /* the real stdout, saved before we take it */
static int rfd = -1;     /* read end of the pipe standing in for stdout */
static int efd = -1;     /* read end of the pipe standing in for stderr */
static int failures = 0;

static void report(const char *what, bool pass)
{
    fprintf(out, "%-62s : %s\n", what, pass ? "PASS" : "FAIL");
    fflush(out);
    if (!pass) {
        ++failures;
    }
}

/* Collect what shows up on a captured stream, stopping once we have
 * "want" bytes or run out of patience. Returns the byte count; the
 * result is NUL-terminated so it can be compared as a string. */
static size_t collect(int fd, char *buf, size_t want)
{
    size_t got = 0;
    int i;
    ssize_t n;

    for (i = 0; i < WAIT_TRIES && got < want; i++) {
        n = read(fd, &buf[got], BUFSIZE - 1 - got);
        if (0 < n) {
            got += (size_t) n;
            continue;
        }
        usleep(WAIT_USEC);
    }
    buf[got] = '\0';
    return got;
}

/* Give the progress thread a good run at whatever we handed it, for the
 * cases where the assertion is that nothing comes out yet. */
static size_t settle(int fd, char *buf)
{
    size_t got = 0;
    int i;
    ssize_t n;

    for (i = 0; i < SETTLE_TRIES; i++) {
        usleep(WAIT_USEC);
        n = read(fd, &buf[got], BUFSIZE - 1 - got);
        if (0 < n) {
            got += (size_t) n;
        }
    }
    buf[got] = '\0';
    return got;
}

static bool deliver(const pmix_proc_t *src, pmix_iof_channel_t channel,
                    const char *data, size_t len)
{
    pmix_byte_object_t bo;
    pmix_status_t rc;

    bo.bytes = (char *) data;
    bo.size = len;
    rc = PMIx_server_IOF_deliver(src, channel, &bo, NULL, 0, NULL, NULL);
    if (PMIX_SUCCESS != rc && PMIX_OPERATION_SUCCEEDED != rc) {
        fprintf(out, "PMIx_server_IOF_deliver failed: %s\n", PMIx_Error_string(rc));
        fflush(out);
        return false;
    }
    return true;
}

/* What the blocking form of the two IOF push APIs returned when called
 * from the progress thread, and a flag saying the attempt has happened. */
static pmix_status_t wb_deliver = PMIX_SUCCESS;
static pmix_status_t wb_flow = PMIX_SUCCESS;
static volatile bool wb_done = false;

/* Runs on the progress thread - a delivery completion is invoked from
 * there, which is exactly the position a host is in when it forwards
 * output from inside one of our callbacks.
 *
 * Both APIs must refuse the blocking form here rather than wait on the
 * loop they are standing in. What that refusal must NOT do is hand the
 * request caddy back with our proc and byte object still attached: the
 * caddy destructor frees both unconditionally, so the refusal used to
 * free two objects living in this frame. Against an unfixed library
 * this aborts in the allocator rather than failing a check. */
static void wouldblock_cbfunc(pmix_status_t status, void *cbdata)
{
    pmix_proc_t src;
    pmix_byte_object_t bo;
    (void) status;
    (void) cbdata;

    PMIX_LOAD_PROCID(&src, "wouldblock", 0);
    bo.bytes = (char *) "never emitted\n";
    bo.size = 14;
    wb_deliver = PMIx_server_IOF_deliver(&src, PMIX_FWD_STDOUT_CHANNEL, &bo,
                                         NULL, 0, NULL, NULL);
    wb_flow = PMIx_server_IOF_flow_control(&src, PMIX_FWD_STDIN_CHANNEL, true,
                                           NULL, 0, NULL, NULL);
    wb_done = true;
}

/* What the host was asked to stop. pmix_server_iof_fn_t is documented as
 * removing this server from the distribution list "for the specified
 * channel/proc combination", and the deregistration wire message carries
 * neither - the request being torn down is the only place they exist. The
 * handler used to hand the host a freshly allocated caddy with all three
 * still empty, so the host kept relaying that job's output to a server
 * with no registration left to match it. The procs have to be copied
 * here: the array belongs to the caddy, which is released on the way out
 * of the handler. */
static pmix_proc_t stop_procs[8];
static size_t stop_nprocs = SIZE_MAX;
static pmix_iof_channel_t stop_channels = PMIX_FWD_NO_CHANNELS;
static bool stop_directive = false;

static pmix_status_t stub_iof_pull(const pmix_proc_t procs[], size_t nprocs,
                                   const pmix_info_t directives[], size_t ndirs,
                                   pmix_iof_channel_t channels,
                                   pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    size_t n;

    (void) cbfunc;
    (void) cbdata;

    stop_nprocs = nprocs;
    stop_channels = channels;
    stop_directive = false;
    for (n = 0; n < nprocs && n < 8 && NULL != procs; n++) {
        memcpy(&stop_procs[n], &procs[n], sizeof(pmix_proc_t));
    }
    for (n = 0; n < ndirs; n++) {
        if (PMIX_CHECK_KEY(&directives[n], PMIX_IOF_STOP)) {
            stop_directive = true;
        }
    }
    /* refusing keeps the handler's cleanup on the path this test drives;
     * everything worth checking has already been recorded */
    return PMIX_ERR_NOT_SUPPORTED;
}

static pmix_server_module_t mymodule = {.push_stdin = NULL,
                                        .iof_pull = stub_iof_pull};

static void iof_op_stub(pmix_status_t status, void *cbdata)
{
    (void) status;
    (void) cbdata;
}

/* Drive one IOF DEREGISTER off a hand-packed wire buffer whose directive
 * count is whatever the caller says. The handler sizes its array as
 * "count + 1" so it can append the stop directive, and seeds that last
 * slot itself - so a count near SIZE_MAX wraps the sum to zero,
 * PMIx_Info_create answers NULL for a zero-element array, and the seed
 * is written at info[SIZE_MAX]. A count that merely fails to survive the
 * round trip through the int32_t the unpack consumes is the same
 * hazard one step smaller. Both must be refused before any allocation. */
static pmix_status_t do_iofdereg_refid(size_t claimed_ninfo, size_t refid)
{
    pmix_buffer_t *buf;
    pmix_status_t rc;

    buf = PMIX_NEW(pmix_buffer_t);
    if (NULL == buf) {
        return PMIX_ERR_NOMEM;
    }
    PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, buf, &claimed_ninfo, 1, PMIX_SIZE);
    if (PMIX_SUCCESS == rc) {
        PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, buf, &refid, 1, PMIX_SIZE);
    }
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(buf);
        return rc;
    }
    rc = pmix_server_iofdereg(pmix_globals.mypeer, buf, iof_op_stub, NULL);
    PMIX_RELEASE(buf);
    return rc;
}

static pmix_status_t do_iofdereg(size_t claimed_ninfo)
{
    return do_iofdereg_refid(claimed_ninfo, 0);
}

/* Replace "fd" with the write end of a fresh pipe, handing back a
 * non-blocking read end.
 *
 * NOTE for anyone chasing a sanitizer failure in this test: stderr is one
 * of the streams this takes, and AddressSanitizer writes its report to
 * stderr - so the report goes into the capture pipe and is never seen.
 * CI shows "FAIL: iof_output" over a .log holding nothing but PASS lines
 * and a silent exit 1. Run it with
 * ASAN_OPTIONS=...:log_path=/tmp/asanrep to get the trace into a file
 * instead. */
static int capture(int fd)
{
    int pipefd[2];

    if (0 != pipe(pipefd)) {
        fprintf(out, "pipe failed: %s\n", strerror(errno));
        return -1;
    }
    if (0 > dup2(pipefd[1], fd)) {
        fprintf(out, "dup2 failed: %s\n", strerror(errno));
        return -1;
    }
    close(pipefd[1]);
    if (0 > fcntl(pipefd[0], F_SETFL, O_NONBLOCK)) {
        fprintf(out, "fcntl failed: %s\n", strerror(errno));
        return -1;
    }
    return pipefd[0];
}

int main(int argc, char **argv)
{
    pmix_status_t rc;
    pmix_proc_t src;
    int savedout;
    char buf[BUFSIZE];
    size_t got;
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

    fprintf(out, "\n=== PMIx IOF output delivery unit test ===\n\n");
    fflush(out);

    /* the sinks are built on fd 1 and fd 2 during server init, so the
     * substitution has to be in place before that runs */
    fflush(stderr);
    rfd = capture(fileno(stdout));
    efd = capture(fileno(stderr));
    if (0 > rfd || 0 > efd) {
        return 1;
    }

    rc = PMIx_server_init(&mymodule, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(out, "PMIx_server_init failed: %s\n", PMIx_Error_string(rc));
        return 1;
    }

    /* ---- baseline: a complete line is written out ---- */
    PMIX_LOAD_PROCID(&src, "outtest", 0);
    if (!deliver(&src, PMIX_FWD_STDOUT_CHANNEL, "alpha\n", 6)) {
        PMIx_server_finalize();
        return 1;
    }
    got = collect(rfd, buf, 6);
    report("a complete line reaches stdout", 6 == got && 0 == strcmp(buf, "alpha\n"));

    /* ---- one source closing must not silence the shared channel ---- */
    if (!deliver(&src, PMIX_FWD_STDOUT_CHANNEL, NULL, 0)) {
        PMIx_server_finalize();
        return 1;
    }
    PMIX_LOAD_PROCID(&src, "outtest", 1);
    if (!deliver(&src, PMIX_FWD_STDOUT_CHANNEL, "beta\n", 5)) {
        PMIx_server_finalize();
        return 1;
    }
    got = collect(rfd, buf, 5);
    report("stdout still flows after another source closed",
           5 == got && 0 == strcmp(buf, "beta\n"));

    /* and the sink has to stay usable, not just take one more write */
    if (!deliver(&src, PMIX_FWD_STDOUT_CHANNEL, "gamma\n", 6)) {
        PMIx_server_finalize();
        return 1;
    }
    got = collect(rfd, buf, 6);
    report("and keeps flowing for every chunk after that",
           6 == got && 0 == strcmp(buf, "gamma\n"));

    /* ---- an unterminated last line is written when the stream ends ---- */
    PMIX_LOAD_PROCID(&src, "outtest", 2);
    if (!deliver(&src, PMIX_FWD_STDOUT_CHANNEL, "no-newline-here", 15)) {
        PMIx_server_finalize();
        return 1;
    }
    got = settle(rfd, buf);
    report("a partial line is held back until the line completes", 0 == got);

    if (!deliver(&src, PMIX_FWD_STDOUT_CHANNEL, NULL, 0)) {
        PMIx_server_finalize();
        return 1;
    }
    got = collect(rfd, buf, 15);
    report("the partial line is written out when the stream closes",
           15 == got && 0 == strcmp(buf, "no-newline-here"));

    /* ---- stderr is a separate shared sink, and behaves the same ---- */
    PMIX_LOAD_PROCID(&src, "outtest", 3);
    if (!deliver(&src, PMIX_FWD_STDERR_CHANNEL, NULL, 0) ||
        !deliver(&src, PMIX_FWD_STDERR_CHANNEL, "delta\n", 6)) {
        PMIx_server_finalize();
        return 1;
    }
    got = collect(efd, buf, 6);
    report("stderr still flows after a source closed",
           6 == got && 0 == strcmp(buf, "delta\n"));

    /* ---- the blocking form, attempted from the progress thread ---- */
    {
        pmix_byte_object_t bo;
        int i;

        PMIX_LOAD_PROCID(&src, "outtest", 4);
        bo.bytes = (char *) "epsilon\n";
        bo.size = 8;
        /* this frame owns src and bo, and must still own them when the
         * completion returns - hence the wait rather than a fire and
         * forget */
        rc = PMIx_server_IOF_deliver(&src, PMIX_FWD_STDOUT_CHANNEL, &bo, NULL, 0,
                                     wouldblock_cbfunc, NULL);
        if (PMIX_SUCCESS != rc) {
            fprintf(out, "PMIx_server_IOF_deliver failed: %s\n", PMIx_Error_string(rc));
            PMIx_server_finalize();
            return 1;
        }
        for (i = 0; i < WAIT_TRIES && !wb_done; i++) {
            usleep(WAIT_USEC);
        }
        report("a delivery completion runs and is answered", wb_done);
        report("blocking IOF deliver from the progress thread is refused",
               PMIX_ERR_WOULD_BLOCK == wb_deliver);
        report("blocking IOF flow control from the progress thread is refused",
               PMIX_ERR_WOULD_BLOCK == wb_flow);
        got = collect(rfd, buf, 8);
        report("and the delivery that carried us there was still written",
               8 == got && 0 == strcmp(buf, "epsilon\n"));
    }

    /* ---- wire counts a client controls, in the deregister handler ----
     * these run last on purpose: a rejection logs to stderr, which is
     * still a pipe nothing reads from here on */
    report("a deregister count that wraps the +1 is refused",
           PMIX_ERR_BAD_PARAM == do_iofdereg(SIZE_MAX));
    report("a deregister count that truncates is refused",
           PMIX_ERR_BAD_PARAM == do_iofdereg((size_t) 0x80000000UL));
    /* and the screen must not cost the handler its ordinary job: a
     * well-formed count carries on to the handler lookup, which is what
     * rejects this one - nothing is registered under that refid */
    report("a well-formed deregister gets past the screen",
           PMIX_ERR_NOT_FOUND == do_iofdereg(0));

    /* ---- what the host is told to stop --------------------------------
     * Register a handler by hand, exactly as pmix_server_iofreg does, and
     * then deregister it. The host must be handed the channel/proc
     * combination the registration named; before this it was handed a
     * caddy nothing had filled in - no procs, no channels - so it had no
     * way to take this server off the distribution list. */
    {
        pmix_iof_req_t *req;
        pmix_status_t drc;
        int refid;

        req = PMIX_NEW(pmix_iof_req_t);
        PMIX_RETAIN(pmix_globals.mypeer);
        req->requestor = pmix_globals.mypeer;
        req->nprocs = 1;
        PMIX_PROC_CREATE(req->procs, 1);
        PMIX_LOAD_PROCID(&req->procs[0], "iof-dereg-ut", PMIX_RANK_WILDCARD);
        req->channels = PMIX_FWD_STDOUT_CHANNEL | PMIX_FWD_STDERR_CHANNEL;
        refid = pmix_pointer_array_add(&pmix_globals.iof_requests, req);
        req->local_id = refid;

        /* keep the id in a local: a successful deregistration RELEASEs
         * the request, so "req" is a dangling pointer from the call below
         * onwards and reading req->local_id afterwards is a use-after-free */
        stop_nprocs = SIZE_MAX;
        stop_channels = PMIX_FWD_NO_CHANNELS;
        drc = do_iofdereg_refid(0, refid);
        report("the deregistration reached the host", PMIX_ERR_NOT_SUPPORTED == drc);
        report("the stop request carries PMIX_IOF_STOP", stop_directive);
        report("the stop request names the registered channels",
               (PMIX_FWD_STDOUT_CHANNEL | PMIX_FWD_STDERR_CHANNEL) == stop_channels);
        report("the stop request names the registered procs",
               1 == stop_nprocs
                   && 0 == strcmp(stop_procs[0].nspace, "iof-dereg-ut")
                   && PMIX_RANK_WILDCARD == stop_procs[0].rank);
        report("and the local registration is gone",
               NULL == pmix_pointer_array_get_item(&pmix_globals.iof_requests, refid));
    }

    PMIx_server_finalize();
    close(rfd);
    close(efd);

    fprintf(out, "\n%s\n", (0 == failures) ? "ALL PASSED" : "FAILURES DETECTED");
    fflush(out);
    return (0 == failures) ? 0 : 1;
}
