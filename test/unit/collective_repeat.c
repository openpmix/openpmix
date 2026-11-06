/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * A rank that calls a fence, connect or disconnect more than once is one
 * participant, however many calls it makes.
 *
 * The local phase of these collectives is complete when every expected
 * local participant is accounted for, and the count it compares against
 * nlocal used to be the number of calls received. A repeat call was
 * discounted only for a rank that had lost its connection and come back,
 * so an ordinary second call from a rank already on the tracker stood in
 * for a participant that had not called yet.
 *
 * Two jobs on this node: A with ranks 0 and 1, and B with rank 0. Clients
 * are stand-ins with no socket, at their registered rank entries, so what
 * the server queues for each comes to rest on its send queue and can be
 * counted.
 *
 *   fence over A:*, A:0 calling twice  -> not complete until A:1 calls;
 *                                         then every call is answered
 *   connect {A:0, B:*}, A:0 twice      -> not handed to the host until
 *                                         B:0 calls; then every call is
 *                                         answered
 *   disconnect over A:*, A:0 twice     -> not handed to the host until
 *                                         A:1 calls
 *
 * The fence is local to this node, so it completes here without the host
 * (pmix_server_fence_localonly_opt, the default); the connect and the
 * disconnect go to a host stub that holds them.
 */

#include "src/include/pmix_config.h"

#include "include/pmix.h"
#include "include/pmix_server.h"

#include "src/include/pmix_globals.h"
#include "src/mca/bfrops/bfrops.h"
#include "src/server/pmix_server_ops.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define NS_A "collrep.a"
#define NS_B "collrep.b"

static int npass = 0;
static int nfail = 0;

static void report(const char *name, int passed)
{
    if (passed) {
        fprintf(stdout, "  PASS: %s\n", name);
        ++npass;
    } else {
        fprintf(stdout, "  FAIL: %s\n", name);
        ++nfail;
    }
}

/* --- running a function on the progress thread --- */
typedef struct {
    pmix_event_t ev;
    pmix_lock_t lock;
    void (*fn)(void *arg);
    void *arg;
} shift_t;

static void shifted(int sd, short args, void *cbdata)
{
    shift_t *s = (shift_t *) cbdata;

    (void) sd;
    (void) args;
    s->fn(s->arg);
    PMIX_WAKEUP_THREAD(&s->lock);
}

static void on_progress(void (*fn)(void *arg), void *arg)
{
    shift_t s;

    memset(&s, 0, sizeof(s));
    PMIX_CONSTRUCT_LOCK(&s.lock);
    s.fn = fn;
    s.arg = arg;
    PMIX_THREADSHIFT(&s, shifted);
    PMIX_WAIT_THREAD(&s.lock);
    PMIX_DESTRUCT_LOCK(&s.lock);
}

static void nothing(void *arg)
{
    (void) arg;
}

/* the completions thread-shift; anything queued ahead of this has run
 * when it returns */
static void progress_barrier(void)
{
    on_progress(nothing, NULL);
    on_progress(nothing, NULL);
}

/* --- the host: it takes connect and disconnect, and holds them --- */
static int connect_calls = 0;
static size_t connect_nprocs = 0;
static pmix_op_cbfunc_t connect_cbfunc = NULL;
static void *connect_cbdata = NULL;
static int disconnect_calls = 0;
static pmix_op_cbfunc_t disconnect_cbfunc = NULL;
static void *disconnect_cbdata = NULL;

static pmix_status_t stub_connect(const pmix_proc_t procs[], size_t nprocs,
                                  const pmix_info_t info[], size_t ninfo,
                                  pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    (void) procs;
    (void) info;
    (void) ninfo;
    ++connect_calls;
    connect_nprocs = nprocs;
    connect_cbfunc = cbfunc;
    connect_cbdata = cbdata;
    return PMIX_SUCCESS;
}

static pmix_status_t stub_disconnect(const pmix_proc_t procs[], size_t nprocs,
                                     const pmix_info_t info[], size_t ninfo,
                                     pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    (void) procs;
    (void) nprocs;
    (void) info;
    (void) ninfo;
    ++disconnect_calls;
    disconnect_cbfunc = cbfunc;
    disconnect_cbdata = cbdata;
    return PMIX_SUCCESS;
}

/* --- the jobs and their clients --- */
static pmix_namespace_t *find_ns(const char *ns)
{
    pmix_namespace_t *nptr;

    PMIX_LIST_FOREACH (nptr, &pmix_globals.nspaces, pmix_namespace_t) {
        if (0 == strcmp(nptr->nspace, ns)) {
            return nptr;
        }
    }
    return NULL;
}

static pmix_status_t register_job(const char *ns, const char *ppn, int nprocs)
{
    pmix_nspace_t nspace;
    pmix_proc_t p;
    pmix_info_t info[2];
    pmix_status_t rc;
    char *noderegex = NULL, *ppnregex = NULL;
    int r;

    PMIx_generate_regex(pmix_globals.hostname, &noderegex);
    PMIx_generate_ppn(ppn, &ppnregex);
    PMIX_INFO_LOAD(&info[0], PMIX_NODE_MAP, noderegex, PMIX_REGEX);
    PMIX_INFO_LOAD(&info[1], PMIX_PROC_MAP, ppnregex, PMIX_REGEX);
    free(noderegex);
    free(ppnregex);
    PMIX_LOAD_NSPACE(nspace, ns);
    rc = PMIx_server_register_nspace(nspace, nprocs, info, 2, NULL, NULL);
    PMIX_INFO_DESTRUCT(&info[0]);
    PMIX_INFO_DESTRUCT(&info[1]);
    if (PMIX_SUCCESS != rc && PMIX_OPERATION_SUCCEEDED != rc) {
        return rc;
    }
    for (r = 0; r < nprocs; r++) {
        PMIX_LOAD_PROCID(&p, ns, r);
        rc = PMIx_server_register_client(&p, geteuid(), getegid(), NULL, NULL, NULL);
        if (PMIX_SUCCESS != rc && PMIX_OPERATION_SUCCEEDED != rc) {
            return rc;
        }
    }
    progress_barrier();
    return PMIX_SUCCESS;
}

/* a connected client at its registered rank entry, with no socket */
typedef struct {
    const char *ns;
    pmix_rank_t rank;
    pmix_peer_t *peer;
} client_t;

static void do_client(void *arg)
{
    client_t *c = (client_t *) arg;
    pmix_namespace_t *nptr = find_ns(c->ns);
    pmix_rank_info_t *info, *found = NULL;
    pmix_peer_t *p;

    c->peer = NULL;
    if (NULL == nptr) {
        return;
    }
    PMIX_LIST_FOREACH (info, &nptr->ranks, pmix_rank_info_t) {
        if (c->rank == info->pname.rank) {
            found = info;
            break;
        }
    }
    if (NULL == found) {
        return;
    }
    if (NULL == nptr->compat.bfrops) {
        memcpy(&nptr->compat, &pmix_globals.mypeer->nptr->compat, sizeof(pmix_personality_t));
    }
    p = PMIX_NEW(pmix_peer_t);
    PMIX_RETAIN(nptr);
    p->nptr = nptr;
    PMIX_RETAIN(found);
    p->info = found;
    /* the completion stores and fetches through the peer's datastore */
    p->gds = pmix_globals.mypeer->gds;
    memcpy(&p->proc_type, &pmix_globals.mypeer->proc_type, sizeof(pmix_proc_type_t));
    p->sd = -1;
    p->index = pmix_pointer_array_add(&pmix_server_globals.clients, p);
    found->peerid = p->index;
    found->proc_cnt = 1;
    c->peer = p;
}

static pmix_peer_t *client(const char *ns, pmix_rank_t rank)
{
    client_t c;

    c.ns = ns;
    c.rank = rank;
    on_progress(do_client, &c);
    return c.peer;
}

/* --- what the server has queued for a client --- */
typedef struct {
    pmix_peer_t *peer;
    size_t n;
} queued_t;

static void do_queued(void *arg)
{
    queued_t *q = (queued_t *) arg;
    pmix_ptl_send_t *snd;

    /* count them, and take them off so the next case starts from none */
    q->n = 0;
    if (NULL != q->peer->send_msg) {
        PMIX_RELEASE(q->peer->send_msg);
        q->peer->send_msg = NULL;
        ++q->n;
    }
    while (NULL != (snd = (pmix_ptl_send_t *) pmix_list_remove_first(&q->peer->send_queue))) {
        PMIX_RELEASE(snd);
        ++q->n;
    }
}

static size_t take_queued(pmix_peer_t *peer)
{
    queued_t q;

    q.peer = peer;
    on_progress(do_queued, &q);
    return q.n;
}

static void do_ncollectives(void *arg)
{
    *(size_t *) arg = pmix_list_get_size(&pmix_server_globals.collectives);
}

static size_t ncollectives(void)
{
    size_t n;

    on_progress(do_ncollectives, &n);
    return n;
}

/* --- driving a handler, as the switchyard does --- */
typedef enum { OP_FENCE, OP_CONNECT, OP_DISCONNECT } op_t;

typedef struct {
    op_t op;
    pmix_peer_t *peer;
    const pmix_proc_t *procs;
    size_t nprocs;
    pmix_status_t status;
} drive_t;

static void do_drive(void *arg)
{
    drive_t *d = (drive_t *) arg;
    pmix_buffer_t buf;
    pmix_server_caddy_t *cd;
    pmix_info_t dir;
    pmix_status_t rc;
    size_t ninf = 1;
    bool flag = false;

    PMIX_CONSTRUCT(&buf, pmix_buffer_t);
    PMIX_BFROPS_ASSIGN_TYPE(pmix_globals.mypeer, &buf);
    PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, &buf, &d->nprocs, 1, PMIX_SIZE);
    if (PMIX_SUCCESS == rc) {
        PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, &buf, d->procs, (int32_t) d->nprocs,
                         PMIX_PROC);
    }
    if (PMIX_SUCCESS == rc) {
        PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, &buf, &ninf, 1, PMIX_SIZE);
    }
    if (PMIX_SUCCESS == rc) {
        if (OP_FENCE == d->op) {
            PMIX_INFO_LOAD(&dir, PMIX_COLLECT_DATA, &flag, PMIX_BOOL);
        } else {
            PMIX_INFO_LOAD(&dir, PMIX_EMBED_BARRIER, &flag, PMIX_BOOL);
        }
        PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, &buf, &dir, 1, PMIX_INFO);
        PMIX_INFO_DESTRUCT(&dir);
    }
    if (PMIX_SUCCESS != rc) {
        PMIX_DESTRUCT(&buf);
        d->status = rc;
        return;
    }
    cd = PMIX_NEW(pmix_server_caddy_t);
    PMIX_RETAIN(d->peer);
    cd->peer = d->peer;
    cd->hdr.tag = 1;
    if (OP_FENCE == d->op) {
        rc = pmix_server_fence(cd, &buf, pmix_server_modex_cbfunc);
    } else if (OP_CONNECT == d->op) {
        rc = pmix_server_connect(cd, &buf, pmix_server_cnct_cbfunc);
    } else {
        rc = pmix_server_disconnect(cd, &buf, pmix_server_discnct_cbfunc);
    }
    if (PMIX_SUCCESS != rc) {
        /* the switchyard owns the caddy on a non-success return */
        PMIX_RELEASE(cd);
    }
    PMIX_DESTRUCT(&buf);
    d->status = rc;
}

static pmix_status_t drive(op_t op, pmix_peer_t *peer, const pmix_proc_t *procs, size_t nprocs)
{
    drive_t d;

    d.op = op;
    d.peer = peer;
    d.procs = procs;
    d.nprocs = nprocs;
    d.status = PMIX_ERROR;
    on_progress(do_drive, &d);
    return d.status;
}

int main(int argc, char **argv)
{
    static pmix_server_module_t mymodule = {0};
    pmix_peer_t *a0, *a1, *b0;
    pmix_proc_t procs[2];
    pmix_status_t rc;
    size_t base, n0, n1, nb;

    (void) argc;
    (void) argv;
    /* a crash should not take the lines before it with it */
    setvbuf(stdout, NULL, _IONBF, 0);

    mymodule.connect = stub_connect;
    mymodule.disconnect = stub_disconnect;
    rc = PMIx_server_init(&mymodule, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "PMIx_server_init failed: %s\n", PMIx_Error_string(rc));
        return 1;
    }
    fprintf(stdout, "collective_repeat: a rank's repeat calls count once\n");

    if (PMIX_SUCCESS != register_job(NS_A, "0,1", 2) ||
        PMIX_SUCCESS != register_job(NS_B, "0", 1)) {
        report("the jobs register", 0);
        goto done;
    }
    a0 = client(NS_A, 0);
    a1 = client(NS_A, 1);
    b0 = client(NS_B, 0);
    if (NULL == a0 || NULL == a1 || NULL == b0) {
        report("the clients connect", 0);
        goto done;
    }
    base = ncollectives();

    /* --- a fence over A, A:0 calling twice before A:1 --- */
    PMIX_LOAD_PROCID(&procs[0], NS_A, PMIX_RANK_WILDCARD);
    rc = drive(OP_FENCE, a0, procs, 1);
    report("fence: A:0's call is accepted", PMIX_SUCCESS == rc);
    rc = drive(OP_FENCE, a0, procs, 1);
    report("fence: A:0's second call is accepted", PMIX_SUCCESS == rc);
    progress_barrier();
    n0 = take_queued(a0);
    report("fence: two calls from A:0 do not complete it without A:1",
           0 == n0 && base + 1 == ncollectives());
    if (0 != n0) {
        fprintf(stdout, "        (A:0 was answered %lu times)\n", (unsigned long) n0);
    }
    rc = drive(OP_FENCE, a1, procs, 1);
    progress_barrier();
    n0 = take_queued(a0);
    n1 = take_queued(a1);
    report("fence: A:1's call completes it", PMIX_SUCCESS == rc && base == ncollectives());
    report("fence: ...and every call is answered", 2 == n0 && 1 == n1);

    /* --- a connect of A:0 with B, A:0 calling twice before B:0 --- */
    PMIX_LOAD_PROCID(&procs[0], NS_A, 0);
    PMIX_LOAD_PROCID(&procs[1], NS_B, PMIX_RANK_WILDCARD);
    rc = drive(OP_CONNECT, a0, procs, 2);
    report("connect: A:0's call is accepted", PMIX_SUCCESS == rc);
    rc = drive(OP_CONNECT, a0, procs, 2);
    report("connect: A:0's second call is accepted", PMIX_SUCCESS == rc);
    report("connect: two calls from A:0 do not take it to the host without B:0",
           0 == connect_calls);
    rc = drive(OP_CONNECT, b0, procs, 2);
    report("connect: B:0's call takes it to the host",
           PMIX_SUCCESS == rc && 1 == connect_calls && 2 == connect_nprocs);
    if (1 == connect_calls && NULL != connect_cbfunc) {
        connect_cbfunc(PMIX_SUCCESS, connect_cbdata);
        progress_barrier();
    }
    n0 = take_queued(a0);
    nb = take_queued(b0);
    report("connect: ...and every call is answered",
           2 == n0 && 1 == nb && base == ncollectives());

    /* --- a disconnect over A, A:0 calling twice before A:1 --- */
    PMIX_LOAD_PROCID(&procs[0], NS_A, PMIX_RANK_WILDCARD);
    rc = drive(OP_DISCONNECT, a0, procs, 1);
    report("disconnect: A:0's call is accepted", PMIX_SUCCESS == rc);
    rc = drive(OP_DISCONNECT, a0, procs, 1);
    report("disconnect: A:0's second call is accepted", PMIX_SUCCESS == rc);
    report("disconnect: two calls from A:0 do not take it to the host without A:1",
           0 == disconnect_calls);
    rc = drive(OP_DISCONNECT, a1, procs, 1);
    report("disconnect: A:1's call takes it to the host",
           PMIX_SUCCESS == rc && 1 == disconnect_calls);
    if (1 == disconnect_calls && NULL != disconnect_cbfunc) {
        disconnect_cbfunc(PMIX_SUCCESS, disconnect_cbdata);
        progress_barrier();
    }
    n0 = take_queued(a0);
    n1 = take_queued(a1);
    report("disconnect: ...and every call is answered",
           2 == n0 && 1 == n1 && base == ncollectives());

done:
    PMIx_server_finalize();
    fprintf(stdout, "collective_repeat: %d passed, %d failed\n", npass, nfail);
    return (0 == nfail) ? 0 : 1;
}
