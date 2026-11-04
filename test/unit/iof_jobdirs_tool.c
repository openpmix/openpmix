/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/*
 * The tool's half of PMIX_IOF_JOB_DIRECTIVES: formatting the output of a
 * job it did not spawn.
 *
 * A launcher is forwarded the output of jobs its own job spawns, because
 * they inherit its subscription - but it never saw their spawn requests,
 * so it had nothing to format their output with and wrote it with its
 * process-wide defaults. The server forwarding the output now says, with
 * the first of it, what the job was registered with and whose
 * subscription it inherited (test/unit/iof_jobdirs.c). This checks what
 * the tool makes of that. In order, a job's output is formatted with:
 *
 *   1. our process-wide defaults, as before;
 *   2. over them, the formatting we use for the nearest ancestor we know,
 *      found through the PMIX_PARENT_ID the server sent - which is what
 *      PMIx_Spawn promises a job that inherits its parent's forwarding:
 *      "the same formatting";
 *   3. over that, the job's own directives, one key at a time, so a job
 *      can add to what it inherits or turn part of it off.
 *
 * And it is the fallback only. A reply to a spawn of our own is the better
 * answer and wins; output held for one is held still. Only the first
 * message about a job counts, only formatting directives are taken from
 * it, and an ancestry that loops is walked a bounded distance.
 *
 * Like test/unit/iof_pending.c, which this follows, it comes up as a tool
 * and drives pmix_iof_write_output() on the progress thread, reading what
 * reaches a pipe standing in for stdout.
 */

#include "src/include/pmix_config.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
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
    fprintf(out, "%-66s : %s\n", what, pass ? "PASS" : "FAIL");
    fflush(out);
    if (!pass) {
        ++failures;
    }
}

/* see test/unit/iof_pending.c */
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

/* Everything below runs on the progress thread, which owns the namespace
 * list and the pending cache */
typedef enum {
    OP_WRITE,
    OP_RECORD,
    OP_REPLY,
    OP_BEGIN,
    OP_END
} op_t;

typedef struct {
    pmix_event_t ev;
    pmix_lock_t lock;
    op_t op;
    const char *nspace;
    const char *data;
    pmix_info_t *info;
    size_t ninfo;
    pmix_iof_flags_t flags;
    pmix_status_t status;
} req_t;

static void do_op(int sd, short args, void *cbdata)
{
    req_t *r = (req_t *) cbdata;
    pmix_byte_object_t bo;
    pmix_namespace_t *nptr = NULL, *ns;
    pmix_proc_t src;

    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    r->status = PMIX_SUCCESS;
    switch (r->op) {
    case OP_WRITE:
        PMIX_LOAD_PROCID(&src, r->nspace, 0);
        bo.bytes = (char *) r->data;
        bo.size = strlen(r->data);
        r->status = pmix_iof_write_output(&src, PMIX_FWD_STDOUT_CHANNEL, &bo);
        break;
    case OP_RECORD:
        /* what the tool's receive handler does with each chunk */
        PMIX_LOAD_PROCID(&src, r->nspace, 0);
        pmix_iof_record_job_directives(&src, r->info, r->ninfo);
        break;
    case OP_REPLY:
        /* what the reply to a spawn of ours does: the flags it was issued
         * with go onto the namespace, whatever else we were told of it,
         * and anything held for it is released */
        PMIX_LIST_FOREACH (ns, &pmix_globals.nspaces, pmix_namespace_t) {
            if (0 == strcmp(ns->nspace, r->nspace)) {
                nptr = ns;
                break;
            }
        }
        if (NULL == nptr) {
            nptr = PMIX_NEW(pmix_namespace_t);
            nptr->nspace = strdup(r->nspace);
            pmix_list_append(&pmix_globals.nspaces, &nptr->super);
        }
        memcpy(&nptr->iof_flags, &r->flags, sizeof(pmix_iof_flags_t));
        pmix_iof_release_pending(r->nspace);
        break;
    case OP_BEGIN:
        pmix_iof_spawn_begin();
        break;
    case OP_END:
        pmix_iof_spawn_end();
        break;
    }
    PMIX_WAKEUP_THREAD(&r->lock);
}

static void run_op(req_t *r)
{
    PMIX_CONSTRUCT_LOCK(&r->lock);
    PMIX_THREADSHIFT(r, do_op);
    PMIX_WAIT_THREAD(&r->lock);
    PMIX_DESTRUCT_LOCK(&r->lock);
}

static void simple_op(op_t op)
{
    req_t r;

    memset(&r, 0, sizeof(r));
    r.op = op;
    run_op(&r);
}

static pmix_status_t write_out(const char *nspace, const char *data)
{
    req_t r;

    memset(&r, 0, sizeof(r));
    r.op = OP_WRITE;
    r.nspace = nspace;
    r.data = data;
    run_op(&r);
    return r.status;
}

static void reply_arrives(const char *nspace, pmix_iof_flags_t *flags)
{
    req_t r;

    memset(&r, 0, sizeof(r));
    r.op = OP_REPLY;
    r.nspace = nspace;
    memcpy(&r.flags, flags, sizeof(pmix_iof_flags_t));
    run_op(&r);
}

/* What our server would send with the first of "nspace"'s output: the
 * directives given, and the ancestor when "parent" is set */
static void told(const char *nspace, const char *parent, pmix_info_t *dirs, size_t ndirs)
{
    req_t r;
    pmix_info_t msg, *arr;
    pmix_data_array_t darray;
    pmix_proc_t pproc;
    size_t n, m = 0;

    arr = PMIx_Info_create(ndirs + 1);
    for (n = 0; n < ndirs; n++) {
        PMIx_Info_xfer(&arr[m++], &dirs[n]);
    }
    if (NULL != parent) {
        PMIX_LOAD_PROCID(&pproc, parent, 0);
        PMIX_INFO_LOAD(&arr[m++], PMIX_PARENT_ID, &pproc, PMIX_PROC);
    }
    darray.type = PMIX_INFO;
    darray.size = m;
    darray.array = arr;
    PMIX_INFO_LOAD(&msg, PMIX_IOF_JOB_DIRECTIVES, &darray, PMIX_DATA_ARRAY);
    PMIx_Info_free(arr, ndirs + 1);

    memset(&r, 0, sizeof(r));
    r.op = OP_RECORD;
    r.nspace = nspace;
    r.info = &msg;
    r.ninfo = 1;
    run_op(&r);
    PMIX_INFO_DESTRUCT(&msg);
}

static void told_bool(const char *nspace, const char *parent, const char *key, bool val)
{
    pmix_info_t d;

    PMIX_INFO_LOAD(&d, key, &val, PMIX_BOOL);
    told(nspace, parent, &d, 1);
    PMIX_INFO_DESTRUCT(&d);
}

/* the formatting our own spawn of "nspace" would have recorded */
static void spawned_with_tag(const char *nspace)
{
    pmix_iof_flags_t flags;

    pmix_iof_init_flags(&flags);
    flags.set = true;
    flags.tag = true;
    reply_arrives(nspace, &flags);
}

/* write a line from "nspace" and collect what reaches stdout */
static size_t emit(const char *nspace, const char *line, char *buf)
{
    if (PMIX_SUCCESS != write_out(nspace, line)) {
        buf[0] = '\0';
        return 0;
    }
    return collect(rfd, buf, strlen(line));
}

/* "[<nspace>,0]<stdout>: <line>" appears, and is the start of the output */
static bool tagged_at_start(const char *buf, const char *nspace)
{
    char want[256];

    snprintf(want, sizeof(want), "[%s,0]", nspace);
    return 0 == strncmp(buf, want, strlen(want));
}

/* a timestamp - "[<date>]" - directly before the tag */
static bool timestamped(const char *buf, const char *nspace)
{
    char want[256];

    snprintf(want, sizeof(want), "][%s,0]", nspace);
    return '[' == buf[0] && NULL != strstr(buf, want);
}

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
    pmix_info_t tinfo, dirs[2];
    pmix_iof_flags_t flags;
    char buf[BUFSIZE], tmpl[] = "/tmp/iof_jobdirs.XXXXXX", path[512];
    struct stat st;
    int savedout;
    size_t got;
    bool yes = true;
    PMIX_HIDE_UNUSED_PARAMS(argc, argv);

    fflush(stdout);
    savedout = dup(fileno(stdout));
    if (0 > savedout || NULL == (out = fdopen(savedout, "w"))) {
        fprintf(stderr, "could not keep stdout: %s\n", strerror(errno));
        return 1;
    }
    fprintf(out, "\n=== PMIx IOF forwarded job directives (tool) unit test ===\n\n");
    fflush(out);

    /* the shared sinks are built on fd 1 and fd 2 during init */
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

    /* ---- nothing said: the process-wide defaults, as ever ---- */
    got = emit("jt-none", "plain\n", buf);
    report("a job nobody described is written with our defaults",
           6 == got && 0 == strcmp(buf, "plain\n"));

    /* ---- the job's own directives ---- */
    told_bool("jt-own", NULL, PMIX_IOF_TAG_OUTPUT, true);
    got = emit("jt-own", "own\n", buf);
    report("a job we did not spawn is formatted with its own directives",
           tagged_at_start(buf, "jt-own") && NULL != strstr(buf, "own"));

    /* ---- inherited from an ancestor we spawned ---- */
    spawned_with_tag("jt-p0");
    told("jt-c0", "jt-p0", NULL, 0);
    got = emit("jt-c0", "child\n", buf);
    report("a child is formatted the way we format its parent",
           tagged_at_start(buf, "jt-c0") && NULL != strstr(buf, "child"));

    /* ---- its own directives go over what it inherits ---- */
    told_bool("jt-c1", "jt-p0", PMIX_IOF_TIMESTAMP_OUTPUT, true);
    got = emit("jt-c1", "both\n", buf);
    report("a directive of the child's own is added to what it inherits",
           timestamped(buf, "jt-c1") && NULL != strstr(buf, "both"));

    told_bool("jt-c2", "jt-p0", PMIX_IOF_TAG_OUTPUT, false);
    got = emit("jt-c2", "untagged\n", buf);
    report("and one set false turns the inherited one off",
           9 == got && 0 == strcmp(buf, "untagged\n"));

    /* ---- transitive: a grandchild, through a child with no output ---- */
    told("jt-c3", "jt-p0", NULL, 0);
    told_bool("jt-g3", "jt-c3", PMIX_IOF_TIMESTAMP_OUTPUT, true);
    got = emit("jt-g3", "grand\n", buf);
    report("a grandchild inherits through a parent we have not formatted",
           timestamped(buf, "jt-g3") && NULL != strstr(buf, "grand"));

    /* ---- an ancestry that loops is walked a bounded distance ---- */
    told("jt-x", "jt-y", NULL, 0);
    told_bool("jt-y", "jt-x", PMIX_IOF_TAG_OUTPUT, true);
    got = emit("jt-y", "loop\n", buf);
    report("a circular ancestry terminates, with the job's own directives",
           tagged_at_start(buf, "jt-y") && NULL != strstr(buf, "loop"));

    /* ---- a job is not its own ancestor ---- */
    told("jt-self", "jt-self", NULL, 0);
    got = emit("jt-self", "self\n", buf);
    report("a job named as its own parent is formatted with our defaults",
           5 == got && 0 == strcmp(buf, "self\n"));

    /* ---- only the first message about a job counts ---- */
    told_bool("jt-first", NULL, PMIX_IOF_TAG_OUTPUT, true);
    told_bool("jt-first", NULL, PMIX_IOF_TIMESTAMP_OUTPUT, true);
    got = emit("jt-first", "first\n", buf);
    report("a second message about a job is ignored",
           tagged_at_start(buf, "jt-first") && !timestamped(buf, "jt-first"));

    /* ---- a reply to a spawn of our own wins, and is waited for ----
     * Output for a job we are told of while a spawn of ours is unanswered
     * is held, as any output for a job without flags is, and then written
     * the way the reply says - not the way the forwarded directives do */
    simple_op(OP_BEGIN);
    told_bool("jt-mine", NULL, PMIX_IOF_TAG_OUTPUT, true);
    rc = write_out("jt-mine", "mine\n");
    got = settle(rfd, buf);
    report("output for it is still held while our spawn is unanswered",
           PMIX_SUCCESS == rc && 0 == got);
    pmix_iof_init_flags(&flags);
    flags.set = true;
    reply_arrives("jt-mine", &flags);
    got = collect(rfd, buf, 5);
    report("and is written as the reply says, not as forwarded",
           5 == got && 0 == strcmp(buf, "mine\n"));
    simple_op(OP_END);

    /* ---- only formatting is taken ----
     * A forwarded file name would have us create a file we were never
     * asked to write - that is PMIX_IOF_FILE_WRITER's to arrange, with its
     * own checks */
    if (NULL == mkdtemp(tmpl)) {
        report("could not make a scratch directory", false);
    } else {
        snprintf(path, sizeof(path), "%s/out", tmpl);
        PMIX_INFO_LOAD(&dirs[0], PMIX_IOF_TAG_OUTPUT, &yes, PMIX_BOOL);
        PMIX_INFO_LOAD(&dirs[1], PMIX_IOF_OUTPUT_TO_FILE, path, PMIX_STRING);
        told("jt-file", NULL, dirs, 2);
        PMIX_INFO_DESTRUCT(&dirs[0]);
        PMIX_INFO_DESTRUCT(&dirs[1]);
        got = emit("jt-file", "notfile\n", buf);
        report("a forwarded file directive is not taken up",
               tagged_at_start(buf, "jt-file") && 0 != stat(path, &st));
        report("but the formatting beside it is", NULL != strstr(buf, "notfile"));
        rmdir(tmpl);
    }

    PMIx_tool_finalize();

    fprintf(out, "\n%s: %d failure(s)\n", 0 == failures ? "SUCCESS" : "FAILURE", failures);
    fflush(out);
    return (0 == failures) ? 0 : 1;
}
