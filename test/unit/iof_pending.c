/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/*
 * Output that arrives before the spawn reply that says how to format it.
 *
 * A tool receives forwarded output for the jobs it spawns, and a rank
 * that writes and exits immediately can get its first chunk to us before
 * the spawn reply names the namespace it came from. Nothing in scope at
 * that moment identifies which of our in-flight spawns produced it, so
 * the library used to format it with a single process-wide stand-in -
 * which cannot tell two concurrent spawns apart, and cannot carry an
 * output-to-file directive at all. Such output was therefore formatted
 * with the wrong spawn's directives, or (for a file-only spawn, where
 * the stand-in says "do not write locally" and has no file to write to)
 * dropped outright.
 *
 * It is now held until the reply arrives and then written with that
 * spawn's own flags. This drives that arrangement directly: the entry
 * points are pmix_iof_spawn_begin / pmix_iof_release_pending /
 * pmix_iof_spawn_end, all of which run on the progress thread, so every
 * step here is thread-shifted onto it.
 *
 * The test comes up as a tool because the cache is a tool-role thing -
 * a client is never sent a spawned job's output.
 */

#include "src/include/pmix_config.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "include/pmix.h"
#include "include/pmix_tool.h"

#include "src/common/pmix_iof.h"
#include "src/include/pmix_globals.h"
#include "src/threads/pmix_threads.h"

#define WAIT_USEC    20000
#define WAIT_TRIES   250
#define SETTLE_TRIES 25
#define BUFSIZE      4096

static FILE *out = NULL; /* the real stdout, saved before we take it */
static int rfd = -1;     /* read end of the pipe standing in for stdout */
static int efd = -1;     /* read end of the pipe standing in for stderr */
static int failures = 0;
static pmix_proc_t myproc;

static void report(const char *what, bool pass)
{
    fprintf(out, "%-62s : %s\n", what, pass ? "PASS" : "FAIL");
    fflush(out);
    if (!pass) {
        ++failures;
    }
}

/* Collect what shows up on a captured stream, stopping once we have
 * "want" bytes or run out of patience. NUL-terminated so it can be
 * compared as a string. */
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
 * cases whose assertion is that nothing comes out yet. */
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

/* Everything below runs on the progress thread. pmix_iof_write_output
 * and the cache entry points all touch pmix_globals state that belongs
 * to it, so main() must not call any of them directly. */
typedef enum {
    OP_WRITE,
    OP_BEGIN,
    OP_END,
    OP_RELEASE,
    OP_ADD_NSPACE,
    OP_PENDING_COUNT
} iofut_op_t;

typedef struct {
    pmix_event_t ev;
    pmix_lock_t lock;
    iofut_op_t op;
    const char *nspace;
    pmix_rank_t rank;
    pmix_iof_channel_t stream;
    const char *data;
    size_t len;
    pmix_iof_flags_t flags;
    pmix_status_t status;
    size_t count;
} iofut_req_t;

static void do_op(int sd, short args, void *cbdata)
{
    iofut_req_t *r = (iofut_req_t *) cbdata;
    pmix_byte_object_t bo;
    pmix_namespace_t *nptr;
    pmix_proc_t src;

    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    r->status = PMIX_SUCCESS;
    switch (r->op) {
    case OP_WRITE:
        PMIX_LOAD_PROCID(&src, r->nspace, r->rank);
        bo.bytes = (char *) r->data;
        bo.size = r->len;
        r->status = pmix_iof_write_output(&src, r->stream, &bo);
        break;
    case OP_BEGIN:
        pmix_iof_spawn_begin();
        break;
    case OP_END:
        pmix_iof_spawn_end();
        break;
    case OP_RELEASE:
        pmix_iof_release_pending(r->nspace);
        break;
    case OP_ADD_NSPACE:
        /* what the spawn reply does when it learns the namespace: record
         * the directives the spawn was issued with against it */
        nptr = PMIX_NEW(pmix_namespace_t);
        if (NULL == nptr) {
            r->status = PMIX_ERR_NOMEM;
            break;
        }
        nptr->nspace = strdup(r->nspace);
        memcpy(&nptr->iof_flags, &r->flags, sizeof(pmix_iof_flags_t));
        pmix_list_append(&pmix_globals.nspaces, &nptr->super);
        break;
    case OP_PENDING_COUNT:
        r->count = pmix_list_get_size(&pmix_globals.iof_pending);
        break;
    }
    PMIX_WAKEUP_THREAD(&r->lock);
}

static void run_op(iofut_req_t *r)
{
    PMIX_CONSTRUCT_LOCK(&r->lock);
    PMIX_THREADSHIFT(r, do_op);
    PMIX_WAIT_THREAD(&r->lock);
    PMIX_DESTRUCT_LOCK(&r->lock);
}

static pmix_status_t write_out(const char *nspace, pmix_rank_t rank,
                               const char *data, size_t len)
{
    iofut_req_t r;

    memset(&r, 0, sizeof(r));
    r.op = OP_WRITE;
    r.nspace = nspace;
    r.rank = rank;
    r.stream = PMIX_FWD_STDOUT_CHANNEL;
    r.data = data;
    r.len = len;
    run_op(&r);
    return r.status;
}

static void simple_op(iofut_op_t op, const char *nspace)
{
    iofut_req_t r;

    memset(&r, 0, sizeof(r));
    r.op = op;
    r.nspace = nspace;
    run_op(&r);
}

static size_t pending_count(void)
{
    iofut_req_t r;

    memset(&r, 0, sizeof(r));
    r.op = OP_PENDING_COUNT;
    run_op(&r);
    return r.count;
}

/* Announce a namespace with the given formatting directives, exactly as
 * the spawn reply would, and release whatever we were holding for it. */
static void reply_arrives(const char *nspace, pmix_iof_flags_t *flags)
{
    iofut_req_t r;

    memset(&r, 0, sizeof(r));
    r.op = OP_ADD_NSPACE;
    r.nspace = nspace;
    memcpy(&r.flags, flags, sizeof(pmix_iof_flags_t));
    run_op(&r);
    if (PMIX_SUCCESS != r.status) {
        fprintf(out, "could not record namespace %s\n", nspace);
        fflush(out);
        ++failures;
        return;
    }
    simple_op(OP_RELEASE, nspace);
}

/* replace "fd" with the write end of a fresh pipe, handing back a
 * non-blocking read end */
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
    pmix_info_t tinfo;
    pmix_iof_flags_t flags;
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

    fprintf(out, "\n=== PMIx IOF pending-output unit test ===\n\n");
    fflush(out);

    /* the shared sinks are built on fd 1 and fd 2 during init, so the
     * substitution has to be in place before that runs */
    fflush(stderr);
    rfd = capture(fileno(stdout));
    efd = capture(fileno(stderr));
    if (0 > rfd || 0 > efd) {
        return 1;
    }

    PMIX_INFO_LOAD(&tinfo, PMIX_TOOL_DO_NOT_CONNECT, NULL, PMIX_BOOL);
    rc = PMIx_tool_init(&myproc, &tinfo, 1);
    PMIX_INFO_DESTRUCT(&tinfo);
    if (PMIX_SUCCESS != rc) {
        fprintf(out, "PMIx_tool_init failed: %s\n", PMIx_Error_string(rc));
        return 1;
    }

    /* ---- with no spawn in flight, nothing is held ----
     * The cache must not delay output that has no reply coming: a tool
     * that pulled a job's IOF without spawning it would otherwise see
     * nothing until something unrelated finished. */
    rc = write_out("nospawn", 0, "alpha\n", 6);
    got = collect(rfd, buf, 6);
    report("output for an unknown nspace flows when nothing is in flight",
           PMIX_SUCCESS == rc && 6 == got && 0 == strcmp(buf, "alpha\n"));

    /* ---- with a spawn in flight, it is held ---- */
    simple_op(OP_BEGIN, NULL);
    rc = write_out("jobA", 0, "beta\n", 5);
    got = settle(rfd, buf);
    report("output for an unknown nspace is held while a spawn is in flight",
           PMIX_SUCCESS == rc && 0 == got);
    report("and is on the pending list", 1 == pending_count());

    /* ---- and the reply's own directives are what format it ---- */
    pmix_iof_init_flags(&flags);
    flags.set = true;
    flags.tag = true;
    reply_arrives("jobA", &flags);
    got = collect(rfd, buf, 5);
    report("the reply releases it", 0 < got);
    report("formatted with the directives that spawn was issued with",
           NULL != strstr(buf, "[jobA,0]") && NULL != strstr(buf, "beta"));
    simple_op(OP_END, NULL);

    /* ---- two concurrent spawns, formatted differently ----
     * This is the case a single process-wide stand-in cannot serve: the
     * second spawn's flags overwrite the first's, so whichever job's
     * output arrived first was formatted with the other's directives.
     * Held output is matched to its own reply by namespace instead. */
    simple_op(OP_BEGIN, NULL);
    simple_op(OP_BEGIN, NULL);
    rc = write_out("jobB", 1, "tagme\n", 6);
    if (PMIX_SUCCESS == rc) {
        rc = write_out("jobC", 2, "plain\n", 6);
    }
    got = settle(rfd, buf);
    report("both jobs' output is held while two spawns are in flight",
           PMIX_SUCCESS == rc && 0 == got);
    report("and both are on the pending list", 2 == pending_count());

    pmix_iof_init_flags(&flags);
    flags.set = true;
    flags.tag = true;
    reply_arrives("jobB", &flags);
    got = collect(rfd, buf, 6);
    report("the first reply releases only its own job's output",
           NULL != strstr(buf, "[jobB,1]") && NULL != strstr(buf, "tagme") &&
               NULL == strstr(buf, "plain"));
    simple_op(OP_END, NULL);

    pmix_iof_init_flags(&flags);
    flags.set = true;
    reply_arrives("jobC", &flags);
    got = collect(rfd, buf, 6);
    report("the second gets its own formatting, not the first's",
           6 == got && 0 == strcmp(buf, "plain\n"));
    simple_op(OP_END, NULL);

    /* ---- arrival order survives being held ---- */
    simple_op(OP_BEGIN, NULL);
    rc = write_out("jobD", 0, "one\n", 4);
    if (PMIX_SUCCESS == rc) {
        rc = write_out("jobD", 0, "two\n", 4);
    }
    if (PMIX_SUCCESS == rc) {
        rc = write_out("jobD", 0, "three\n", 6);
    }
    pmix_iof_init_flags(&flags);
    flags.set = true;
    reply_arrives("jobD", &flags);
    got = collect(rfd, buf, 14);
    report("held output is written in arrival order",
           PMIX_SUCCESS == rc && 14 == got && 0 == strcmp(buf, "one\ntwo\nthree\n"));
    simple_op(OP_END, NULL);

    /* ---- a spawn told not to write locally suppresses what was held ----
     * The stand-in gets this half right today by refusing to write; what
     * it cannot do is write the output to the file the spawn asked for,
     * because no sink can be opened for a namespace we have not been
     * told about. Releasing after the reply is what makes that possible.
     * Here we only check the local half: the namespace says no, so the
     * held output must not reach the terminal. */
    simple_op(OP_BEGIN, NULL);
    rc = write_out("jobE", 0, "unwanted\n", 9);
    pmix_iof_init_flags(&flags);
    flags.set = true;
    flags.local_output_given = true;
    flags.local_output = false;
    reply_arrives("jobE", &flags);
    got = settle(rfd, buf);
    report("held output honors a spawn that declined local output",
           PMIX_SUCCESS == rc && 0 == got);
    report("and is no longer pending", 0 == pending_count());
    simple_op(OP_END, NULL);

    /* ---- the last spawn being answered flushes what is left ----
     * Output held for a namespace no reply ever names belongs to no
     * spawn of ours, so it must not sit there once nothing is in
     * flight - it goes out with the process-wide flags, which is what
     * it would have had all along. */
    simple_op(OP_BEGIN, NULL);
    rc = write_out("orphan", 0, "stranded\n", 9);
    got = settle(rfd, buf);
    report("output for a namespace no reply names is held first",
           PMIX_SUCCESS == rc && 0 == got);
    simple_op(OP_END, NULL);
    got = collect(rfd, buf, 9);
    report("and is flushed when the last spawn is answered",
           9 == got && 0 == strcmp(buf, "stranded\n"));
    report("leaving nothing pending", 0 == pending_count());

    /* ---- the stand-down runs on the progress thread wherever it is
     * called from ----
     * Every other step here is thread-shifted, because that is where the
     * library does this work. PMIx_Spawn_nb has one path that is not: if
     * the transport refuses the request, it counts the spawn back out
     * from the application's thread, and the stand-down that follows the
     * last one walks and empties the very list the progress thread
     * appends to. pmix_iof_spawn_end() therefore shifts and waits when it
     * is not already on that thread. This drives that branch - the
     * outcome is the same either way, so what it holds down is that the
     * shifted path completes rather than hanging or returning early. */
    simple_op(OP_BEGIN, NULL);
    rc = write_out("offthread", 0, "unshifted\n", 10);
    got = settle(rfd, buf);
    report("output is held while the off-thread case sets up",
           PMIX_SUCCESS == rc && 0 == got);
    /* deliberately NOT through run_op(): this is the caller PMIx_Spawn_nb
     * makes on its send-failure path */
    pmix_iof_spawn_end();
    got = collect(rfd, buf, 10);
    report("a stand-down called off the progress thread still drains",
           10 == got && 0 == strcmp(buf, "unshifted\n"));
    report("and leaves nothing pending", 0 == pending_count());

    /* ---- the cache is bounded ----
     * Past the limit output stops being held and falls back to the
     * stand-in, so a job that produces more than we are willing to
     * buffer before its reply lands is delayed rather than lost. */
    pmix_globals.iof_pending_limit = 8;
    simple_op(OP_BEGIN, NULL);
    rc = write_out("jobF", 0, "held\n", 5);
    if (PMIX_SUCCESS == rc) {
        rc = write_out("jobF", 0, "overflowed\n", 11);
    }
    got = collect(rfd, buf, 11);
    report("output past the pending limit is written rather than held",
           PMIX_SUCCESS == rc && 11 == got && 0 == strcmp(buf, "overflowed\n"));
    report("while what fit is still pending", 1 == pending_count());
    simple_op(OP_END, NULL);
    got = collect(rfd, buf, 5);
    report("and what fit is written when the spawn is answered",
           5 == got && 0 == strcmp(buf, "held\n"));

    PMIx_tool_finalize();
    close(rfd);
    close(efd);

    fprintf(out, "\n%s\n", (0 == failures) ? "ALL PASSED" : "FAILURES DETECTED");
    fflush(out);
    return (0 == failures) ? 0 : 1;
}
