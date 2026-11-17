/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/*
 * PMIX_LOG_TARGET: output meant for some of the tools, not all of them.
 *
 * Every tool that connects to a server is registered for the server's own
 * output, so whatever the server writes as itself - a show_help among it -
 * reaches all of them. Output about one tool's request is that tool's
 * business alone. PMIX_LOG_TARGET names the procs a message is for, and
 * this checks what pmix_server_iof_deliver_local() and the log path do
 * with it:
 *
 *   - without it, every registered tool still gets the server's output;
 *   - with it, only the named tools do, PMIX_REQUESTOR travelling with
 *     the output and PMIX_LOG_TARGET not;
 *   - a target with no registration - a tool that has gone - sends the
 *     output to the server's own stderr, never to the other tools, and
 *     it is not cached for whoever registers next;
 *   - a target that is not an array of procs names nobody, and is
 *     treated the same way;
 *   - pmix_show_help_norender_dirs() carries the target through the log
 *     path to the same delivery.
 *
 * The tools are stand-in peers on socketpairs, so what is checked is the
 * message actually packed and sent, read back off the far end; the
 * server's stderr is a pipe put in place of fd 2 before the server starts.
 */

#include "src/include/pmix_config.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "include/pmix.h"
#include "include/pmix_server.h"

#include "src/common/pmix_iof.h"
#include "src/include/pmix_globals.h"
#include "src/mca/bfrops/bfrops.h"
#include "src/mca/ptl/base/base.h"
#include "src/server/pmix_server_ops.h"
#include "src/threads/pmix_threads.h"
#include "src/util/pmix_show_help.h"

static FILE *out = NULL; /* the real stdout's copy - fd 2 is the pipe */
static int efd = -1;     /* read end of the pipe standing in for stderr */
static int failures = 0;

static void report(const char *what, bool pass)
{
    fprintf(out, "%-66s : %s\n", what, pass ? "PASS" : "FAIL");
    fflush(out);
    if (!pass) {
        ++failures;
    }
}

/* ------------------------------------------------------------------ */
/* running things on the progress thread                               */
/* ------------------------------------------------------------------ */

typedef struct {
    pmix_object_t super;
    pmix_event_t ev;
    pmix_lock_t lock;
    void (*fn)(void *arg);
    void *arg;
} shift_t;
static PMIX_CLASS_INSTANCE(shift_t, pmix_object_t, NULL, NULL);

static void shift_hdlr(int sd, short args, void *cbdata)
{
    shift_t *s = (shift_t *) cbdata;
    PMIX_HIDE_UNUSED_PARAMS(sd, args);
    s->fn(s->arg);
    PMIX_WAKEUP_THREAD(&s->lock);
}

/* the request array, the peer's events and the namespace list all belong
 * to the progress thread */
static void on_progress_thread(void (*fn)(void *arg), void *arg)
{
    shift_t *s = PMIX_NEW(shift_t);

    PMIX_CONSTRUCT_LOCK(&s->lock);
    s->fn = fn;
    s->arg = arg;
    PMIX_THREADSHIFT(s, shift_hdlr);
    PMIX_WAIT_THREAD(&s->lock);
    PMIX_DESTRUCT_LOCK(&s->lock);
    PMIX_RELEASE(s);
}

/* ------------------------------------------------------------------ */
/* the stand-in tools                                                  */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *nspace;
    pmix_peer_t *peer;
    int far; /* the end we read what the server sent from */
} watcher_t;

static void make_watcher(void *arg)
{
    watcher_t *w = (watcher_t *) arg;
    pmix_peer_t *peer;
    int sv[2];

    if (0 != socketpair(AF_UNIX, SOCK_STREAM, 0, sv)) {
        perror("socketpair");
        exit(1);
    }
    peer = PMIX_NEW(pmix_peer_t);
    peer->nptr = PMIX_NEW(pmix_namespace_t);
    peer->nptr->nspace = strdup(w->nspace);
    peer->nptr->compat.type = pmix_globals.mypeer->nptr->compat.type;
    peer->nptr->compat.bfrops = pmix_globals.mypeer->nptr->compat.bfrops;
    peer->info = PMIX_NEW(pmix_rank_info_t);
    peer->info->pname.nspace = strdup(w->nspace);
    peer->info->pname.rank = 0;
    peer->info->uid = geteuid();
    peer->info->gid = getegid();
    peer->proc_type.type |= PMIX_PROC_TOOL;
    peer->sd = sv[0];
    pmix_ptl_base_set_nonblocking(peer->sd);
    pmix_event_assign(&peer->send_event, pmix_globals.evbase, peer->sd, EV_WRITE | EV_PERSIST,
                      pmix_ptl_base_send_handler, peer);
    w->peer = peer;
    w->far = sv[1];
}

static void release_watcher(void *arg)
{
    watcher_t *w = (watcher_t *) arg;
    PMIX_RELEASE(w->peer);
}

/* the registration a connecting tool is given for the server's own
 * output - see ptl_base_connection_hdlr.c */
static void add_watch(void *arg)
{
    watcher_t *w = (watcher_t *) arg;
    pmix_iof_req_t *req;

    req = PMIX_NEW(pmix_iof_req_t);
    PMIX_RETAIN(w->peer);
    req->requestor = w->peer;
    req->nprocs = 1;
    PMIX_PROC_CREATE(req->procs, 1);
    PMIX_LOAD_PROCID(&req->procs[0], pmix_globals.myid.nspace, PMIX_RANK_WILDCARD);
    req->channels = PMIX_FWD_STDOUT_CHANNEL | PMIX_FWD_STDERR_CHANNEL |
                    PMIX_FWD_STDDIAG_CHANNEL;
    req->remote_id = 7;
    req->local_id = pmix_pointer_array_add(&pmix_globals.iof_requests, req);
}

static void drop_all_watches(void *arg)
{
    pmix_iof_req_t *req;
    int i;
    PMIX_HIDE_UNUSED_PARAMS(arg);

    for (i = 0; i < pmix_globals.iof_requests.size; i++) {
        req = (pmix_iof_req_t *) pmix_pointer_array_get_item(&pmix_globals.iof_requests, i);
        if (NULL != req) {
            pmix_pointer_array_set_item(&pmix_globals.iof_requests, i, NULL);
            PMIX_RELEASE(req);
        }
    }
}

/* ------------------------------------------------------------------ */
/* what the server sends                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    bool ok;
    pmix_proc_t source;
    pmix_info_t *info;
    size_t ninfo;
} fwd_msg_t;

static bool read_all(int fd, char *ptr, size_t len)
{
    size_t got = 0;
    ssize_t rc;

    while (got < len) {
        rc = read(fd, ptr + got, len - got);
        if (0 >= rc) {
            return false;
        }
        got += (size_t) rc;
    }
    return true;
}

/* Read one forwarded-output message off the far end and unpack it the way
 * the tool's receive handler does. */
static void read_fwd(watcher_t *w, fwd_msg_t *m)
{
    pmix_ptl_hdr_t hdr;
    pmix_buffer_t buf;
    pmix_iof_channel_t channel;
    pmix_byte_object_t bo;
    size_t refid, nbytes;
    int32_t cnt;
    pmix_status_t rc;
    char *payload;

    memset(m, 0, sizeof(*m));
    if (!read_all(w->far, (char *) &hdr, sizeof(hdr)) ||
        PMIX_PTL_TAG_IOF != ntohl(hdr.tag)) {
        return;
    }
    nbytes = ntohl(hdr.nbytes);
    payload = (char *) malloc(nbytes);
    if (NULL == payload || !read_all(w->far, payload, nbytes)) {
        free(payload);
        return;
    }
    PMIX_CONSTRUCT(&buf, pmix_buffer_t);
    PMIX_LOAD_BUFFER(w->peer, &buf, payload, nbytes);

    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, w->peer, &buf, &m->source, &cnt, PMIX_PROC);
    if (PMIX_SUCCESS == rc) {
        cnt = 1;
        PMIX_BFROPS_UNPACK(rc, w->peer, &buf, &channel, &cnt, PMIX_IOF_CHANNEL);
    }
    if (PMIX_SUCCESS == rc) {
        cnt = 1;
        PMIX_BFROPS_UNPACK(rc, w->peer, &buf, &refid, &cnt, PMIX_SIZE);
    }
    if (PMIX_SUCCESS == rc) {
        cnt = 1;
        PMIX_BFROPS_UNPACK(rc, w->peer, &buf, &m->ninfo, &cnt, PMIX_SIZE);
    }
    if (PMIX_SUCCESS == rc && 0 < m->ninfo) {
        m->info = PMIx_Info_create(m->ninfo);
        cnt = (int32_t) m->ninfo;
        PMIX_BFROPS_UNPACK(rc, w->peer, &buf, m->info, &cnt, PMIX_INFO);
    }
    if (PMIX_SUCCESS == rc) {
        PMIX_BYTE_OBJECT_CONSTRUCT(&bo);
        cnt = 1;
        PMIX_BFROPS_UNPACK(rc, w->peer, &buf, &bo, &cnt, PMIX_BYTE_OBJECT);
        PMIX_BYTE_OBJECT_DESTRUCT(&bo);
    }
    m->ok = (PMIX_SUCCESS == rc);
    PMIX_DESTRUCT(&buf);
}

static void free_fwd(fwd_msg_t *m)
{
    if (NULL != m->info) {
        PMIx_Info_free(m->info, m->ninfo);
    }
    memset(m, 0, sizeof(*m));
}

/* is anything waiting on this fd within the given time? */
static bool readable(int fd, int msec)
{
    struct pollfd p = {fd, POLLIN, 0};
    return (0 < poll(&p, 1, msec) && 0 != (p.revents & POLLIN));
}

static bool has_key(fwd_msg_t *m, const char *key)
{
    size_t n;

    for (n = 0; n < m->ninfo; n++) {
        if (PMIx_Check_key(m->info[n].key, key)) {
            return true;
        }
    }
    return false;
}

/* read what the pipe standing in for stderr holds, waiting up to msec for
 * the first of it */
static size_t read_stderr(char *buf, size_t len, int msec)
{
    size_t got = 0;
    ssize_t rc;

    while (got < len - 1 && readable(efd, (0 == got) ? msec : 200)) {
        rc = read(efd, buf + got, len - 1 - got);
        if (0 >= rc) {
            break;
        }
        got += (size_t) rc;
    }
    buf[got] = '\0';
    return got;
}

static void drain_stderr(void)
{
    char buf[4096];
    while (0 < read_stderr(buf, sizeof(buf), 100)) {
    }
}

typedef struct {
    const char *msg;
    pmix_info_t *info;
    size_t ninfo;
    pmix_status_t rc;
} deliver_t;

static void do_deliver(void *arg)
{
    deliver_t *d = (deliver_t *) arg;
    pmix_byte_object_t bo;

    bo.bytes = (char *) d->msg;
    bo.size = strlen(d->msg);
    d->rc = pmix_server_iof_deliver_local(&pmix_globals.myid, PMIX_FWD_STDERR_CHANNEL, &bo,
                                          d->info, d->ninfo);
}

static void deliver(const char *msg, pmix_info_t *info, size_t ninfo)
{
    deliver_t d = {msg, info, ninfo, PMIX_ERROR};
    on_progress_thread(do_deliver, &d);
    if (PMIX_SUCCESS != d.rc) {
        fprintf(out, "    delivery returned %s\n", PMIx_Error_string(d.rc));
    }
}

typedef struct {
    size_t n;
} count_t;

static void count_cached(void *arg)
{
    count_t *c = (count_t *) arg;
    c->n = pmix_list_get_size(&pmix_server_globals.iof);
}

static size_t cached(void)
{
    count_t c = {0};
    on_progress_thread(count_cached, &c);
    return c.n;
}

/* a PMIX_LOG_TARGET naming the given procs */
static void load_target(pmix_info_t *info, const pmix_proc_t *procs, size_t nprocs)
{
    pmix_data_array_t darray;

    darray.type = PMIX_PROC;
    darray.size = nprocs;
    darray.array = (void *) procs;
    PMIX_INFO_LOAD(info, PMIX_LOG_TARGET, &darray, PMIX_DATA_ARRAY);
}

/* did this tool get exactly one message from the server, and nothing
 * else? The message is returned for a closer look */
static bool got_one(watcher_t *w, fwd_msg_t *m)
{
    if (!readable(w->far, 5000)) {
        memset(m, 0, sizeof(*m));
        return false;
    }
    read_fwd(w, m);
    return m->ok && PMIX_CHECK_NAMES(&m->source, &pmix_globals.myid);
}

/* ------------------------------------------------------------------ */
/* the cases                                                           */
/* ------------------------------------------------------------------ */

static void test_untargeted(watcher_t *a, watcher_t *b)
{
    fwd_msg_t m;
    bool ok;

    deliver("to everybody\n", NULL, 0);
    ok = got_one(a, &m);
    free_fwd(&m);
    report("untargeted: the first tool gets the server's output", ok);
    ok = got_one(b, &m);
    free_fwd(&m);
    report("untargeted: so does the second", ok);
    drain_stderr();
}

static void test_targeted(watcher_t *a, watcher_t *b)
{
    pmix_info_t info[2];
    pmix_proc_t target, requestor;
    fwd_msg_t m;
    char buf[256];
    bool ok;
    size_t before = cached();

    PMIX_LOAD_PROCID(&target, a->nspace, 0);
    PMIX_LOAD_PROCID(&requestor, "some-tool", 3);
    load_target(&info[0], &target, 1);
    PMIX_INFO_LOAD(&info[1], PMIX_REQUESTOR, &requestor, PMIX_PROC);
    deliver("for a only\n", info, 2);

    ok = got_one(a, &m);
    report("targeted: the named tool gets it", ok);
    report("targeted: PMIX_REQUESTOR goes with it", ok && has_key(&m, PMIX_REQUESTOR));
    report("targeted: PMIX_LOG_TARGET does not", ok && !has_key(&m, PMIX_LOG_TARGET));
    free_fwd(&m);
    report("targeted: the other tool gets nothing", !readable(b->far, 1000));
    report("targeted: the server's stderr gets nothing",
           0 == read_stderr(buf, sizeof(buf), 500));
    report("targeted: nothing is cached", before == cached());
    PMIX_INFO_DESTRUCT(&info[0]);
    PMIX_INFO_DESTRUCT(&info[1]);
}

static void test_wildcard_target(watcher_t *a, watcher_t *b)
{
    pmix_info_t info;
    pmix_proc_t target;
    fwd_msg_t m;
    bool ok;

    PMIX_LOAD_PROCID(&target, b->nspace, PMIX_RANK_WILDCARD);
    load_target(&info, &target, 1);
    deliver("for b's job\n", &info, 1);
    ok = got_one(b, &m);
    free_fwd(&m);
    report("wildcard target: a tool in the named job gets it", ok);
    report("wildcard target: a tool in another job does not", !readable(a->far, 1000));
    PMIX_INFO_DESTRUCT(&info);
    drain_stderr();
}

static void test_gone(watcher_t *a, watcher_t *b)
{
    pmix_info_t info;
    pmix_proc_t target;
    char buf[256];
    size_t before = cached();

    PMIX_LOAD_PROCID(&target, "gone-tool", 0);
    load_target(&info, &target, 1);
    deliver("for a tool that left\n", &info, 1);
    read_stderr(buf, sizeof(buf), 5000);
    report("target gone: the server's stderr gets it",
           NULL != strstr(buf, "for a tool that left"));
    report("target gone: the first tool gets nothing", !readable(a->far, 500));
    report("target gone: nor does the second", !readable(b->far, 500));
    report("target gone: nothing is cached", before == cached());
    PMIX_INFO_DESTRUCT(&info);
}

static void test_not_procs(watcher_t *a, watcher_t *b)
{
    pmix_info_t info;
    char buf[256];

    PMIX_INFO_LOAD(&info, PMIX_LOG_TARGET, "jd-a", PMIX_STRING);
    deliver("for a target that is no array\n", &info, 1);
    read_stderr(buf, sizeof(buf), 5000);
    report("target not procs: the server's stderr gets it",
           NULL != strstr(buf, "for a target that is no array"));
    report("target not procs: no tool does",
           !readable(a->far, 500) && !readable(b->far, 500));
    PMIX_INFO_DESTRUCT(&info);
}

static void test_show_help(watcher_t *a, watcher_t *b)
{
    pmix_info_t info;
    pmix_proc_t target;
    fwd_msg_t m;
    bool ok;

    PMIX_LOAD_PROCID(&target, b->nspace, 0);
    load_target(&info, &target, 1);
    pmix_show_help_norender_dirs(NULL, "help-test.txt", "target", "a show_help for b\n", &info,
                                 1);
    ok = got_one(b, &m);
    free_fwd(&m);
    report("show_help: the named tool gets it", ok);
    report("show_help: the other tool does not", !readable(a->far, 1000));
    PMIX_INFO_DESTRUCT(&info);
}

static pmix_server_module_t mymodule = {0};

int main(int argc, char **argv)
{
    pmix_status_t rc;
    watcher_t a = {"jd-a", NULL, -1}, b = {"jd-b", NULL, -1};
    int pfd[2];
    PMIX_HIDE_UNUSED_PARAMS(argc, argv);

    /* the server writes its own output to fd 2, so put a pipe there
     * before it opens its sinks, and report on a copy of stdout */
    out = fdopen(dup(1), "w");
    if (NULL == out || 0 != pipe(pfd) || 0 > dup2(pfd[1], 2)) {
        perror("setup");
        return 1;
    }
    close(pfd[1]);
    efd = pfd[0];

    rc = PMIx_server_init(&mymodule, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(out, "PMIx_server_init failed: %s\n", PMIx_Error_string(rc));
        return 1;
    }
    /* the show_help path delivers only once enabled */
    pmix_show_help_enabled = 1;

    on_progress_thread(make_watcher, &a);
    on_progress_thread(make_watcher, &b);
    on_progress_thread(add_watch, &a);
    on_progress_thread(add_watch, &b);

    test_untargeted(&a, &b);
    test_targeted(&a, &b);
    test_wildcard_target(&a, &b);
    test_gone(&a, &b);
    test_not_procs(&a, &b);
    test_show_help(&a, &b);

    on_progress_thread(drop_all_watches, NULL);
    on_progress_thread(release_watcher, &a);
    on_progress_thread(release_watcher, &b);
    close(a.far);
    close(b.far);
    PMIx_server_finalize();

    fprintf(out, "\n%s: %d failure(s)\n", 0 == failures ? "SUCCESS" : "FAILURE", failures);
    return (0 == failures) ? 0 : 1;
}
