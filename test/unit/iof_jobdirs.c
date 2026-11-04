/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/*
 * The server's half of PMIX_IOF_JOB_DIRECTIVES: what a job is to look
 * like, sent with the first of its output to reach each recipient.
 *
 * A spawned job's output reaches the launcher of the job that spawned it
 * because the child inherits the parent's subscription - but the spawn
 * request itself went from the parent to its own server and on into the
 * host, and the launcher never saw it. So the launcher had nothing to
 * format the child's output with, and wrote it with its process-wide
 * defaults: a child of a tagged job came out untagged, whatever either
 * job asked for.
 *
 * The server forwarding the output is the one that can say. The host
 * registered the job with it, formatting directives included, and if the
 * recipient has the output only because the job inherited one of its
 * subscriptions, the server knows whose. This checks what it sends:
 *
 *   - the formatting directives the job was registered with, explicit
 *     "false" included, under their current names even when the host used
 *     a deprecated one - and nothing else, file directives in particular;
 *   - PMIX_PARENT_ID naming the watched ancestor, when the subscription
 *     was inherited, so the recipient can format the job as it formats
 *     that ancestor;
 *   - all of it once per job per recipient, on the first chunk only;
 *   - and no attribute at all when there is nothing to say.
 *
 * The recipient is a stand-in tool peer on a socketpair, so what is
 * checked is the message pmix_iof_process_iof() actually packed and sent,
 * read back off the far end. The tool's half - what it does with this -
 * is test/unit/iof_jobdirs_tool.c.
 */

#include "src/include/pmix_config.h"

#include <arpa/inet.h>
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
#include "src/threads/pmix_threads.h"

static int failures = 0;

static void report(const char *what, bool pass)
{
    printf("%-66s : %s\n", what, pass ? "PASS" : "FAIL");
    fflush(stdout);
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
/* the stand-in tool                                                   */
/* ------------------------------------------------------------------ */

typedef struct {
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
    peer->nptr->nspace = strdup("jd-tool");
    peer->nptr->compat.type = pmix_globals.mypeer->nptr->compat.type;
    peer->nptr->compat.bfrops = pmix_globals.mypeer->nptr->compat.bfrops;
    peer->info = PMIX_NEW(pmix_rank_info_t);
    peer->info->pname.nspace = strdup("jd-tool");
    peer->info->pname.rank = 0;
    /* our own user, which may follow into a job the host registered
     * without naming an owner - see clone_iof_reqs() */
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

typedef struct {
    pmix_peer_t *peer;
    const char *nspace;
    size_t remote_id;
} watch_t;

/* a subscription to a whole job's stdout, as a tool's spawn makes */
static void add_watch(void *arg)
{
    watch_t *w = (watch_t *) arg;
    pmix_iof_req_t *req;

    req = PMIX_NEW(pmix_iof_req_t);
    PMIX_RETAIN(w->peer);
    req->requestor = w->peer;
    req->nprocs = 1;
    PMIX_PROC_CREATE(req->procs, 1);
    PMIX_LOAD_PROCID(&req->procs[0], w->nspace, PMIX_RANK_WILDCARD);
    req->channels = PMIX_FWD_STDOUT_CHANNEL;
    req->remote_id = w->remote_id;
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

static void watch(watcher_t *w, const char *nspace)
{
    watch_t a = {w->peer, nspace, 11};
    on_progress_thread(add_watch, &a);
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

/* the PMIX_IOF_JOB_DIRECTIVES array in a message, or NULL */
static pmix_data_array_t *jobdirs(fwd_msg_t *m)
{
    size_t n;

    for (n = 0; n < m->ninfo; n++) {
        if (PMIx_Check_key(m->info[n].key, PMIX_IOF_JOB_DIRECTIVES)) {
            if (PMIX_DATA_ARRAY == m->info[n].value.type &&
                NULL != m->info[n].value.data.darray &&
                PMIX_INFO == m->info[n].value.data.darray->type) {
                return m->info[n].value.data.darray;
            }
            return NULL;
        }
    }
    return NULL;
}

static pmix_info_t *dir_entry(pmix_data_array_t *d, const char *key)
{
    pmix_info_t *a;
    size_t n;

    if (NULL == d) {
        return NULL;
    }
    a = (pmix_info_t *) d->array;
    for (n = 0; n < d->size; n++) {
        if (PMIx_Check_key(a[n].key, key)) {
            return &a[n];
        }
    }
    return NULL;
}

/* is the entry present, a bool, and equal to "want"? */
static bool dir_is(pmix_data_array_t *d, const char *key, bool want)
{
    pmix_info_t *e = dir_entry(d, key);
    return (NULL != e && PMIX_BOOL == e->value.type && want == e->value.data.flag);
}

/* ------------------------------------------------------------------ */
/* the host's side                                                     */
/* ------------------------------------------------------------------ */

/* Register a job the way a host does, with whatever job-level info the
 * case needs. A parent is given as PMIX_PARENT_ID when "parent_ns" is set */
static pmix_status_t register_job(const char *nspace, const char *parent_ns,
                                  pmix_info_t *extra, size_t nextra)
{
    pmix_info_t *info;
    pmix_proc_t parent;
    pmix_nspace_t ns;
    pmix_status_t rc;
    uint32_t one = 1;
    size_t n, ninfo = 0;

    /* a pmix_nspace_t rather than the caller's string - see the same
     * note in test/unit/iof_inherit.c */
    PMIX_LOAD_NSPACE(ns, nspace);
    info = PMIx_Info_create(nextra + 2);
    PMIX_INFO_LOAD(&info[ninfo++], PMIX_JOB_SIZE, &one, PMIX_UINT32);
    if (NULL != parent_ns) {
        PMIX_LOAD_PROCID(&parent, parent_ns, 0);
        PMIX_INFO_LOAD(&info[ninfo++], PMIX_PARENT_ID, &parent, PMIX_PROC);
    }
    for (n = 0; n < nextra; n++) {
        PMIx_Info_xfer(&info[ninfo++], &extra[n]);
    }
    rc = PMIx_server_register_nspace(ns, 0, info, ninfo, NULL, NULL);
    if (PMIX_OPERATION_SUCCEEDED == rc) {
        rc = PMIX_SUCCESS;
    }
    PMIx_Info_free(info, nextra + 2);
    return rc;
}

/* Hand the server a chunk of a job's stdout, as a host does. Not ours to
 * write locally - only the forwarding is under test. The blocking form
 * reports a completed delivery as PMIX_OPERATION_SUCCEEDED */
static pmix_status_t deliver(const char *nspace)
{
    pmix_proc_t source;
    pmix_byte_object_t bo;
    pmix_info_t local;
    char text[] = "output\n";
    bool flag = false;
    pmix_status_t rc;

    PMIX_LOAD_PROCID(&source, nspace, 0);
    bo.bytes = text;
    bo.size = strlen(text);
    PMIX_INFO_LOAD(&local, PMIX_IOF_LOCAL_OUTPUT, &flag, PMIX_BOOL);
    rc = PMIx_server_IOF_deliver(&source, PMIX_FWD_STDOUT_CHANNEL, &bo, &local, 1, NULL, NULL);
    PMIX_INFO_DESTRUCT(&local);
    return (PMIX_OPERATION_SUCCEEDED == rc) ? PMIX_SUCCESS : rc;
}

/* ------------------------------------------------------------------ */
/* the cases                                                           */
/* ------------------------------------------------------------------ */

static void test_own_directives(watcher_t *w)
{
    pmix_info_t extra[4];
    pmix_data_array_t *d;
    fwd_msg_t m;
    bool yes = true, no = false;
    char dir[] = "/nonexistent/iof_jobdirs";

    /* tag on, timestamp explicitly off, and a file directive that is not
     * formatting and must stay where it is */
    PMIX_INFO_LOAD(&extra[0], PMIX_IOF_TAG_OUTPUT, &yes, PMIX_BOOL);
    PMIX_INFO_LOAD(&extra[1], PMIX_IOF_TIMESTAMP_OUTPUT, &no, PMIX_BOOL);
    PMIX_INFO_LOAD(&extra[2], PMIX_IOF_XML_OUTPUT, &yes, PMIX_BOOL);
    PMIX_INFO_LOAD(&extra[3], PMIX_IOF_OUTPUT_TO_DIRECTORY, dir, PMIX_STRING);
    if (PMIX_SUCCESS != register_job("jd-own", NULL, extra, 4)) {
        report("own: could not register the job", false);
        goto done;
    }
    watch(w, "jd-own");

    if (PMIX_SUCCESS != deliver("jd-own")) {
        report("own: delivery failed", false);
        goto done;
    }
    read_fwd(w, &m);
    report("own: the first chunk is forwarded", m.ok);
    d = jobdirs(&m);
    report("own: it carries PMIX_IOF_JOB_DIRECTIVES", NULL != d);
    report("own: with the job's tag directive", dir_is(d, PMIX_IOF_TAG_OUTPUT, true));
    report("own: an explicit 'false' travels as 'false'",
           dir_is(d, PMIX_IOF_TIMESTAMP_OUTPUT, false));
    report("own: every formatting directive registered is sent",
           dir_is(d, PMIX_IOF_XML_OUTPUT, true));
    report("own: a file directive is not", NULL == dir_entry(d, PMIX_IOF_OUTPUT_TO_DIRECTORY));
    report("own: nor is a parent, for a subscription not inherited",
           NULL == dir_entry(d, PMIX_PARENT_ID));
    report("own: and nothing else is sent in it", NULL != d && 3 == d->size);
    free_fwd(&m);

    /* ---- once per job per recipient ---- */
    deliver("jd-own");
    read_fwd(w, &m);
    report("own: the next chunk is forwarded", m.ok);
    report("own: without the directives again", NULL == jobdirs(&m));
    free_fwd(&m);

done:
    for (size_t n = 0; n < 4; n++) {
        PMIX_INFO_DESTRUCT(&extra[n]);
    }
    on_progress_thread(drop_all_watches, NULL);
}

static void test_nothing_to_say(watcher_t *w)
{
    fwd_msg_t m;

    if (PMIX_SUCCESS != register_job("jd-plain", NULL, NULL, 0)) {
        report("plain: could not register the job", false);
        return;
    }
    watch(w, "jd-plain");
    deliver("jd-plain");
    read_fwd(w, &m);
    report("plain: the chunk is forwarded", m.ok);
    report("plain: a job with no directives sends no attribute at all",
           m.ok && NULL == jobdirs(&m));
    free_fwd(&m);
    on_progress_thread(drop_all_watches, NULL);
}

static void test_deprecated_names(watcher_t *w)
{
    pmix_info_t extra[1];
    pmix_data_array_t *d;
    fwd_msg_t m;
    bool yes = true;

    PMIX_INFO_LOAD(&extra[0], PMIX_MERGE_STDERR_STDOUT, &yes, PMIX_BOOL);
    if (PMIX_SUCCESS != register_job("jd-old", NULL, extra, 1)) {
        report("deprecated: could not register the job", false);
        PMIX_INFO_DESTRUCT(&extra[0]);
        return;
    }
    PMIX_INFO_DESTRUCT(&extra[0]);
    watch(w, "jd-old");
    deliver("jd-old");
    read_fwd(w, &m);
    d = jobdirs(&m);
    report("deprecated: a host's deprecated name is sent under the current one",
           dir_is(d, PMIX_IOF_MERGE_STDERR_STDOUT, true) &&
               NULL == dir_entry(d, PMIX_MERGE_STDERR_STDOUT));
    free_fwd(&m);
    on_progress_thread(drop_all_watches, NULL);
}

static void test_inherited(watcher_t *w)
{
    pmix_info_t extra[1];
    pmix_info_t *e;
    pmix_data_array_t *d;
    fwd_msg_t m;
    bool yes = true;

    /* the tool watches the parent; the host registers the child with its
     * ancestry and one directive of its own, and nothing names the child */
    if (PMIX_SUCCESS != register_job("jd-parent", NULL, NULL, 0)) {
        report("inherited: could not register the parent", false);
        return;
    }
    PMIX_INFO_LOAD(&extra[0], PMIX_IOF_TIMESTAMP_OUTPUT, &yes, PMIX_BOOL);
    if (PMIX_SUCCESS != register_job("jd-child", "jd-parent", extra, 1)) {
        report("inherited: could not register the child", false);
        PMIX_INFO_DESTRUCT(&extra[0]);
        return;
    }
    PMIX_INFO_DESTRUCT(&extra[0]);
    if (PMIX_SUCCESS != register_job("jd-bare", "jd-parent", NULL, 0)) {
        report("inherited: could not register the second child", false);
        return;
    }
    watch(w, "jd-parent");

    deliver("jd-child");
    read_fwd(w, &m);
    report("inherited: a child's output reaches the parent's watcher", m.ok);
    d = jobdirs(&m);
    e = dir_entry(d, PMIX_PARENT_ID);
    report("inherited: it names the watched ancestor",
           NULL != e && PMIX_PROC == e->value.type && NULL != e->value.data.proc &&
               PMIX_CHECK_NSPACE(e->value.data.proc->nspace, "jd-parent"));
    report("inherited: alongside the child's own directives",
           dir_is(d, PMIX_IOF_TIMESTAMP_OUTPUT, true));
    free_fwd(&m);

    deliver("jd-child");
    read_fwd(w, &m);
    report("inherited: and only with the first chunk", m.ok && NULL == jobdirs(&m));
    free_fwd(&m);

    /* a child with nothing of its own still has something to say: whose
     * formatting it takes */
    deliver("jd-bare");
    read_fwd(w, &m);
    d = jobdirs(&m);
    report("inherited: a child with no directives of its own names its ancestor",
           NULL != d && 1 == d->size && NULL != dir_entry(d, PMIX_PARENT_ID));
    free_fwd(&m);

    /* the parent's own output went to the watcher under its own
     * subscription, which nobody inherited - so no ancestor is named */
    deliver("jd-parent");
    read_fwd(w, &m);
    report("inherited: the parent's own output names no ancestor",
           m.ok && NULL == dir_entry(jobdirs(&m), PMIX_PARENT_ID));
    free_fwd(&m);

    on_progress_thread(drop_all_watches, NULL);
}

static pmix_server_module_t mymodule = {0};

int main(int argc, char **argv)
{
    pmix_status_t rc;
    watcher_t w = {NULL, -1};
    struct timeval tv = {5, 0};
    PMIX_HIDE_UNUSED_PARAMS(argc, argv);

    rc = PMIx_server_init(&mymodule, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "PMIx_server_init failed: %s\n", PMIx_Error_string(rc));
        return 1;
    }

    on_progress_thread(make_watcher, &w);
    /* a message that never comes fails its case rather than hanging */
    setsockopt(w.far, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    test_own_directives(&w);
    test_nothing_to_say(&w);
    test_deprecated_names(&w);
    test_inherited(&w);

    on_progress_thread(release_watcher, &w);
    close(w.far);
    PMIx_server_finalize();

    printf("\n%s: %d failure(s)\n", 0 == failures ? "SUCCESS" : "FAILURE", failures);
    return (0 == failures) ? 0 : 1;
}
