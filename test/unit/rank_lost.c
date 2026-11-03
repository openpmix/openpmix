/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * White-box unit tests for pmix_server_rank_lost(): a local rank that
 * departs without finalizing must stop being an expected participant in
 * its job's collectives, however the server learns of it.
 *
 * It can learn two ways, in either order: the rank's socket drops
 * (lost_connection), or the host deregisters it - which a host does as it
 * reaps the process, and which can arrive first. Deregistration used to
 * account for nothing: it marked the peer finalized, which is what makes a
 * later loss look like an orderly exit, and closed the socket so the loss
 * was never seen at all. A ULFM application whose host reaped a dead rank
 * first then hung in MPI_Finalize, whose PMIx_Fence(NULL, 0) waits on
 * nlocalprocs contributions.
 *
 * The job is four local ranks of one namespace. Clients are stand-ins with
 * no socket, put on the server's client list as connected clients are.
 *
 *   host first       -> rank 3 is deregistered while a whole-job fence that
 *                       ranks 0-2 have contributed to is in flight: the
 *                       fence records it as departed and completes, the
 *                       job counts three local procs, and a whole-job
 *                       fence started afterwards waits for three
 *   socket first     -> rank 2's connection closes, then the host
 *                       deregisters it: counted once, and not counted
 *                       finalized as well
 *   never connected  -> rank 1 is deregistered without having connected:
 *                       no collective waits for it either
 *   finalized        -> rank 0 finalized and is then deregistered while a
 *                       whole-job fence waits for it: deregistration is
 *                       the host saying it is gone for good, so the fence
 *                       records it as departed, and it leaves both counts
 *
 * A rank that finalized and whose connection then drops is NOT lost - it
 * may initialize again (issue #4113). That half is trk_peer_lost.c's.
 */

#include "src/include/pmix_config.h"

#include "include/pmix.h"
#include "include/pmix_server.h"

#include "src/include/pmix_globals.h"
#include "src/mca/ptl/base/base.h"
#include "src/server/pmix_server_ops.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define NS     "ranklost.ns"
#define NPROCS 4

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

static void progress_barrier(void)
{
    pmix_value_t v;

    PMIX_VALUE_LOAD(&v, "barrier", PMIX_STRING);
    PMIx_Store_internal(&pmix_globals.myid, "ranklost-ut.barrier", &v);
    PMIX_VALUE_DESTRUCT(&v);
}

/* run fn(arg) on the progress thread, which owns everything touched here */
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

static pmix_namespace_t *find_ns(void)
{
    pmix_namespace_t *ns;

    PMIX_LIST_FOREACH (ns, &pmix_globals.nspaces, pmix_namespace_t) {
        if (0 == strcmp(ns->nspace, NS)) {
            return ns;
        }
    }
    return NULL;
}

static pmix_rank_info_t *find_rank(pmix_namespace_t *nptr, pmix_rank_t rank)
{
    pmix_rank_info_t *info;

    PMIX_LIST_FOREACH (info, &nptr->ranks, pmix_rank_info_t) {
        if (rank == info->pname.rank) {
            return info;
        }
    }
    return NULL;
}

/* a snapshot of the namespace's counts, read on the progress thread */
typedef struct {
    size_t nlocalprocs;
    size_t nfinalized;
} counts_t;

static void read_counts(void *arg)
{
    counts_t *c = (counts_t *) arg;
    pmix_namespace_t *nptr = find_ns();

    c->nlocalprocs = (NULL == nptr) ? 0 : nptr->nlocalprocs;
    c->nfinalized = (NULL == nptr) ? 0 : nptr->nfinalized;
}

static counts_t counts(void)
{
    counts_t c;

    on_progress(read_counts, &c);
    return c;
}

/* --- a connected client of the job, at its registered rank entry --- */
typedef struct {
    pmix_rank_t rank;
    int sd;               // the socket it is given, or -1
    bool finalize_counted;
    pmix_peer_t *peer;
} connect_t;

static void do_connect(void *arg)
{
    connect_t *c = (connect_t *) arg;
    pmix_namespace_t *nptr = find_ns();
    pmix_rank_info_t *info;
    pmix_peer_t *p;

    c->peer = NULL;
    if (NULL == nptr || NULL == (info = find_rank(nptr, c->rank))) {
        return;
    }
    if (NULL == nptr->compat.bfrops) {
        memcpy(&nptr->compat, &pmix_globals.mypeer->nptr->compat, sizeof(pmix_personality_t));
    }
    p = PMIX_NEW(pmix_peer_t);
    PMIX_RETAIN(nptr);
    p->nptr = nptr;
    PMIX_RETAIN(info);
    p->info = info;
    memcpy(&p->proc_type, &pmix_globals.mypeer->proc_type, sizeof(pmix_proc_type_t));
    p->sd = c->sd;
    p->index = pmix_pointer_array_add(&pmix_server_globals.clients, p);
    info->peerid = p->index;
    info->proc_cnt = 1;
    if (c->finalize_counted) {
        /* what the switchyard does when a finalize request arrives */
        p->finalize_counted = true;
        nptr->nfinalized++;
    }
    c->peer = p;
}

static pmix_peer_t *connect_rank(pmix_rank_t rank, int sd, bool finalize_counted)
{
    connect_t c;

    c.rank = rank;
    c.sd = sd;
    c.finalize_counted = finalize_counted;
    on_progress(do_connect, &c);
    return c.peer;
}

static void deregister(pmix_rank_t rank)
{
    pmix_proc_t p;

    PMIX_LOAD_PROCID(&p, NS, rank);
    PMIx_server_deregister_client(&p, NULL, NULL);
    progress_barrier();
}

/* --- a whole-job fence, as PMIx_Fence(NULL, 0) asks for one --- */
static int completions = 0;
static pmix_status_t completion_status = PMIX_SUCCESS;

static void fake_modexcbfunc(pmix_status_t status, const char *data, size_t ndata,
                             void *cbdata, pmix_release_cbfunc_t relfn, void *relcbd)
{
    (void) data;
    (void) ndata;
    (void) cbdata;
    (void) relfn;
    (void) relcbd;
    ++completions;
    completion_status = status;
}

typedef struct {
    pmix_rank_t ncontrib;     // ranks 0..ncontrib-1 contribute
    pmix_server_trkr_t *trk;
    uint32_t nlocal;
    bool local;
} fence_t;

static void new_fence(void *arg)
{
    fence_t *f = (fence_t *) arg;
    pmix_proc_t wild;
    pmix_server_trkr_t *trk;
    pmix_server_caddy_t *cd;
    pmix_status_t seed = PMIX_SUCCESS;
    pmix_rank_t r;

    PMIX_LOAD_PROCID(&wild, NS, PMIX_RANK_WILDCARD);
    trk = pmix_server_new_tracker(NULL, &wild, 1, PMIX_FENCENB_CMD);
    f->trk = trk;
    if (NULL == trk) {
        return;
    }
    trk->modexcbfunc = fake_modexcbfunc;
    trk->collect_type = PMIX_COLLECT_NO;
    PMIX_INFO_CREATE(trk->info, 1);
    trk->ninfo = 1;
    PMIX_INFO_LOAD(&trk->info[0], PMIX_LOCAL_COLLECTIVE_STATUS, &seed, PMIX_STATUS);
    /* contributions from peers of their own - the accounting compares
     * them by name */
    for (r = 0; r < f->ncontrib; r++) {
        cd = PMIX_NEW(pmix_server_caddy_t);
        cd->peer = PMIX_NEW(pmix_peer_t);
        cd->peer->info = PMIX_NEW(pmix_rank_info_t);
        cd->peer->info->pname.nspace = strdup(NS);
        cd->peer->info->pname.rank = r;
        pmix_list_append(&trk->local_cbs, &cd->super);
    }
    f->nlocal = trk->nlocal;
    f->local = trk->local;
}

static void drop_fence(void *arg)
{
    pmix_server_trkr_t *trk = (pmix_server_trkr_t *) arg;

    pmix_list_remove_item(&pmix_server_globals.collectives, &trk->super);
    PMIX_RELEASE(trk);
}

static void departed_count(void *arg)
{
    fence_t *f = (fence_t *) arg;

    f->nlocal = (uint32_t) pmix_list_get_size(&f->trk->departed);
}

/* lost_connection, reached the way the server reaches it for a socket
 * found closed */
static bool reaped = false;

static void reap(void *arg)
{
    reaped = pmix_ptl_base_reap_if_closed((pmix_peer_t *) arg);
}

static pmix_status_t register_job(void)
{
    pmix_nspace_t ns;
    pmix_proc_t p;
    pmix_status_t rc;
    pmix_rank_t r;
    pmix_info_t info[2];
    char *noderegex = NULL, *ppnregex = NULL;

    PMIx_generate_regex(pmix_globals.hostname, &noderegex);
    PMIx_generate_ppn("0,1,2,3", &ppnregex);
    PMIX_INFO_LOAD(&info[0], PMIX_NODE_MAP, noderegex, PMIX_REGEX);
    PMIX_INFO_LOAD(&info[1], PMIX_PROC_MAP, ppnregex, PMIX_REGEX);
    free(noderegex);
    free(ppnregex);
    PMIX_LOAD_NSPACE(ns, NS);
    rc = PMIx_server_register_nspace(ns, NPROCS, info, 2, NULL, NULL);
    PMIX_INFO_DESTRUCT(&info[0]);
    PMIX_INFO_DESTRUCT(&info[1]);
    if (PMIX_SUCCESS != rc && PMIX_OPERATION_SUCCEEDED != rc) {
        return rc;
    }
    for (r = 0; r < NPROCS; r++) {
        PMIX_LOAD_PROCID(&p, NS, r);
        rc = PMIx_server_register_client(&p, geteuid(), getegid(), NULL, NULL, NULL);
        if (PMIX_SUCCESS != rc && PMIX_OPERATION_SUCCEEDED != rc) {
            return rc;
        }
    }
    progress_barrier();
    return PMIX_SUCCESS;
}

int main(int argc, char **argv)
{
    static pmix_server_module_t mymodule = {0};
    pmix_status_t rc;
    pmix_peer_t *peer;
    fence_t f;
    counts_t c;
    int sv[2];

    (void) argc;
    (void) argv;

    rc = PMIx_server_init(&mymodule, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "PMIx_server_init failed: %s\n", PMIx_Error_string(rc));
        return 1;
    }
    fprintf(stdout, "\n=== local rank loss accounting unit tests ===\n\n");

    rc = register_job();
    report("the job registers", PMIX_SUCCESS == rc);
    if (PMIX_SUCCESS != rc) {
        goto done;
    }
    c = counts();
    report("four local procs to start", NPROCS == c.nlocalprocs);

    /* host first: rank 3 dies, and the host reaps it before we see the
     * socket close, while ranks 0-2 wait in a whole-job fence */
    peer = connect_rank(3, -1, false);
    report("rank 3 is connected", NULL != peer);
    memset(&f, 0, sizeof(f));
    f.ncontrib = 3;
    on_progress(new_fence, &f);
    report("a whole-job fence waits for four", NULL != f.trk && 4 == f.nlocal && f.local);
    if (NULL == f.trk) {
        goto done;
    }
    completions = 0;
    deregister(3);
    on_progress(departed_count, &f);
    report("host first: the fence records rank 3 as departed", 1 == f.nlocal);
    report("host first: the fence completes on the survivors", 1 == completions);
    report("host first: and reports the loss",
           PMIX_ERR_LOST_CONNECTION == completion_status);
    on_progress(drop_fence, f.trk);
    c = counts();
    report("host first: the job counts three local procs", 3 == c.nlocalprocs);
    report("host first: the lost rank is not counted finalized", 0 == c.nfinalized);

    /* the fence MPI_Finalize makes afterwards - the reported hang */
    memset(&f, 0, sizeof(f));
    on_progress(new_fence, &f);
    report("a later whole-job fence waits for three, not four",
           NULL != f.trk && 3 == f.nlocal);
    if (NULL != f.trk) {
        on_progress(drop_fence, f.trk);
    }

    /* socket first: rank 2's connection closes, then the host deregisters
     * it - one loss, counted once */
    if (0 != socketpair(AF_UNIX, SOCK_STREAM, 0, sv)) {
        report("socketpair", 0);
        goto done;
    }
    peer = connect_rank(2, sv[0], false);
    report("rank 2 is connected", NULL != peer);
    if (NULL == peer) {
        goto done;
    }
    close(sv[1]);
    on_progress(reap, peer);
    report("socket first: the closed connection is handled as lost", reaped);
    c = counts();
    report("socket first: the job counts two local procs", 2 == c.nlocalprocs);
    deregister(2);
    c = counts();
    report("socket first: deregistering it does not count it again", 2 == c.nlocalprocs);
    report("socket first: nor count it finalized", 0 == c.nfinalized);

    /* never connected: rank 1 died before it reached PMIx_Init */
    deregister(1);
    c = counts();
    report("never connected: the job counts one local proc", 1 == c.nlocalprocs);
    report("never connected: it is not counted finalized", 0 == c.nfinalized);

    /* finalized, then exited: rank 0 finalized and the host deregisters it
     * once it has terminated, while a whole-job fence waits for it. It can
     * never contribute now, so it is not waited for any more than a rank
     * that died - this was the hang of a job whose rank finalized and
     * exited without taking part */
    peer = connect_rank(0, -1, true);
    report("rank 0 is connected and finalizing", NULL != peer);
    memset(&f, 0, sizeof(f));
    on_progress(new_fence, &f);
    report("a whole-job fence waits for the one rank left", NULL != f.trk && 1 == f.nlocal);
    if (NULL == f.trk) {
        goto done;
    }
    completions = 0;
    deregister(0);
    on_progress(departed_count, &f);
    report("finalized: the fence records rank 0 as departed", 1 == f.nlocal);
    report("finalized: the fence completes", 1 == completions);
    on_progress(drop_fence, f.trk);
    c = counts();
    report("finalized: no local procs remain", 0 == c.nlocalprocs);
    report("finalized: and none is counted finalized", 0 == c.nfinalized);

done:
    fprintf(stdout, "\n%d passed, %d failed\n", npass, nfail);
    PMIx_server_finalize();
    return (0 == nfail) ? 0 : 1;
}
