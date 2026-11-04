/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * White-box unit tests for what the server does as a local process
 * leaves, by a lost connection or by the host's deregistration.
 *
 * Clients are stand-ins put on the server's client list as connected
 * clients are; one whose connection is to be lost is given one end of a
 * socketpair, and the server reads the close of the other.
 *
 *   live, deregistered  -> the host deregisters a client that is still
 *                          connected: a direct-modex request parked on its
 *                          data is answered PMIX_ERR_LOST_CONNECTION, as it
 *                          would be had its connection dropped
 *   never connected     -> the same for a rank that never connected
 *   lost                -> a request for a lost rank's data, which never
 *                          arrived, is answered PMIX_ERR_NOT_FOUND at once
 *                          rather than parked
 *   returned            -> a lost rank that connects again starts with no
 *                          data published: a request for it waits for the
 *                          new process's
 *   finalize unanswered -> a client whose finalize request arrived, but
 *                          the host had not answered it when its
 *                          connection closed, is not counted lost
 *   loss event          -> the event raised for a lost connection is not
 *                          cached for handlers registered later
 *   reply to the lost   -> a reply for a client whose connection closed is
 *                          refused, not queued on it
 *   job deregistered    -> a collective naming ranks of two jobs, one of
 *                          which the host deregisters before its rank
 *                          contributed, completes on the other
 */

#include "src/include/pmix_config.h"

#include "include/pmix.h"
#include "include/pmix_server.h"

#include "src/event/pmix_event.h"
#include "src/include/pmix_globals.h"
#include "src/mca/ptl/base/base.h"
#include "src/server/pmix_server_ops.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define NS1    "rankdep.one"
#define NS2    "rankdep.two"
#define NS3    "rankdep.three"
#define NPROCS 4

/* the job the helpers below act on */
static const char *cur_ns = NS1;

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
    PMIx_Store_internal(&pmix_globals.myid, "rankdep-ut.barrier", &v);
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

static pmix_namespace_t *find_ns(const char *name)
{
    pmix_namespace_t *ns;

    PMIX_LIST_FOREACH (ns, &pmix_globals.nspaces, pmix_namespace_t) {
        if (0 == strcmp(ns->nspace, name)) {
            return ns;
        }
    }
    return NULL;
}

static pmix_rank_info_t *find_rank(pmix_namespace_t *nptr, pmix_rank_t rank)
{
    pmix_rank_info_t *info;

    if (NULL == nptr) {
        return NULL;
    }
    PMIX_LIST_FOREACH (info, &nptr->ranks, pmix_rank_info_t) {
        if (rank == info->pname.rank) {
            return info;
        }
    }
    return NULL;
}

/* --- a rank's state, read on the progress thread ---------------------- */
typedef struct {
    pmix_rank_t rank;
    bool found;
    bool lost;
    bool modex_recvd;
    size_t nlocalprocs;
} rankstate_t;

static void do_rank_state(void *arg)
{
    rankstate_t *r = (rankstate_t *) arg;
    pmix_namespace_t *nptr = find_ns(cur_ns);
    pmix_rank_info_t *info = find_rank(nptr, r->rank);

    r->found = (NULL != info);
    r->lost = (NULL != info) && info->lost;
    r->modex_recvd = (NULL != info) && info->modex_recvd;
    r->nlocalprocs = (NULL == nptr) ? 0 : nptr->nlocalprocs;
}

static rankstate_t rank_state(pmix_rank_t rank)
{
    rankstate_t r;

    memset(&r, 0, sizeof(r));
    r.rank = rank;
    on_progress(do_rank_state, &r);
    return r;
}

static void do_set_modex(void *arg)
{
    pmix_rank_t *rank = (pmix_rank_t *) arg;
    pmix_rank_info_t *info = find_rank(find_ns(cur_ns), *rank);

    if (NULL != info) {
        info->modex_recvd = true;
    }
}

/* --- a connected client of the job, at its registered rank entry --- */
typedef struct {
    pmix_rank_t rank;
    int sd;
    bool finalize_counted;
    pmix_peer_t *peer;
} connect_t;

static void do_connect(void *arg)
{
    connect_t *c = (connect_t *) arg;
    pmix_namespace_t *nptr = find_ns(cur_ns);
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

    PMIX_LOAD_PROCID(&p, cur_ns, rank);
    PMIx_server_deregister_client(&p, NULL, NULL);
    progress_barrier();
}

/* lost_connection, reached the way the server reaches it for a socket
 * found closed */
static bool reaped = false;

static void reap(void *arg)
{
    reaped = pmix_ptl_base_reap_if_closed((pmix_peer_t *) arg);
}

/* lose a rank the way a dying process is lost: its socket closes, and the
 * server reads the close */
static pmix_peer_t *lose_rank(pmix_rank_t rank, bool finalize_counted, bool modex)
{
    pmix_peer_t *peer;
    int sv[2];

    if (0 != socketpair(AF_UNIX, SOCK_STREAM, 0, sv)) {
        return NULL;
    }
    peer = connect_rank(rank, sv[0], finalize_counted);
    if (modex) {
        on_progress(do_set_modex, &rank);
    }
    close(sv[1]);
    if (NULL != peer) {
        reaped = false;
        on_progress(reap, peer);
    }
    return reaped ? peer : NULL;
}

/* what the connection handler does when the rank connects again */
static void do_return(void *arg)
{
    pmix_peer_t *peer = (pmix_peer_t *) arg;

    pmix_server_rank_returned(peer->nptr, peer->info);
}

/* --- the host's direct-modex requests -------------------------------- */
static bool dm_fired = false;
static pmix_status_t dm_status = PMIX_SUCCESS;

static void dmresponse(pmix_status_t status, char *data, size_t sz, void *cbdata)
{
    (void) data;
    (void) sz;
    (void) cbdata;

    dm_fired = true;
    dm_status = status;
}

static size_t nparked(void)
{
    size_t n;

    progress_barrier();
    n = pmix_list_get_size(&pmix_server_globals.remote_pnd);
    return n;
}

static void dmodex(pmix_rank_t rank)
{
    pmix_proc_t p;

    dm_fired = false;
    dm_status = PMIX_SUCCESS;
    PMIX_LOAD_PROCID(&p, cur_ns, rank);
    (void) PMIx_server_dmodex_request(&p, dmresponse, NULL);
    progress_barrier();
}

/* --- the open loss event --------------------------------------------- */
typedef struct {
    bool found;
    bool nocache;
} chain_t;

static void do_loss_chain(void *arg)
{
    chain_t *c = (chain_t *) arg;
    pmix_event_chain_t *ch;
    size_t n;

    c->found = false;
    c->nocache = false;
    PMIX_LIST_FOREACH (ch, &pmix_globals.cached_events, pmix_event_chain_t) {
        if (PMIX_ERR_LOST_CONNECTION != ch->status) {
            continue;
        }
        c->found = true;
        for (n = 0; n < ch->ninfo; n++) {
            if (PMIX_CHECK_KEY(&ch->info[n], PMIX_EVENT_DO_NOT_CACHE) &&
                PMIX_INFO_TRUE(&ch->info[n])) {
                c->nocache = true;
            }
        }
    }
}

/* --- a reply to a client ---------------------------------------------- */
typedef struct {
    pmix_peer_t *peer;
    pmix_status_t rc;
    bool queued;
} reply_t;

static void do_reply(void *arg)
{
    reply_t *r = (reply_t *) arg;
    pmix_buffer_t *buf = PMIX_NEW(pmix_buffer_t);

    PMIX_SERVER_QUEUE_REPLY(r->rc, r->peer, 0, buf);
    if (PMIX_SUCCESS != r->rc) {
        PMIX_RELEASE(buf);
    }
    r->queued = (NULL != r->peer->send_msg);
}

/* --- a collective naming ranks of two jobs ---------------------------- */
static int completions = 0;

static void fake_modexcbfunc(pmix_status_t status, const char *data, size_t ndata,
                             void *cbdata, pmix_release_cbfunc_t relfn, void *relcbd)
{
    (void) status;
    (void) data;
    (void) ndata;
    (void) cbdata;
    (void) relfn;
    (void) relcbd;
    ++completions;
}

typedef struct {
    pmix_server_trkr_t *trk;
    size_t ndeparted;
} coll_t;

static void do_new_coll(void *arg)
{
    coll_t *c = (coll_t *) arg;
    pmix_proc_t procs[2];
    pmix_server_caddy_t *cd;

    PMIX_LOAD_PROCID(&procs[0], NS2, 2);
    PMIX_LOAD_PROCID(&procs[1], NS3, 0);
    c->trk = pmix_server_new_tracker(NULL, procs, 2, PMIX_FENCENB_CMD);
    if (NULL == c->trk) {
        return;
    }
    c->trk->modexcbfunc = fake_modexcbfunc;
    c->trk->collect_type = PMIX_COLLECT_NO;
    /* rank 2 of the second job has contributed */
    cd = PMIX_NEW(pmix_server_caddy_t);
    cd->peer = PMIX_NEW(pmix_peer_t);
    cd->peer->info = PMIX_NEW(pmix_rank_info_t);
    cd->peer->info->pname.nspace = strdup(NS2);
    cd->peer->info->pname.rank = 2;
    pmix_list_append(&c->trk->local_cbs, &cd->super);
}

static void do_coll_state(void *arg)
{
    coll_t *c = (coll_t *) arg;
    pmix_server_trkr_t *trk;

    /* still listed only while it waits */
    c->ndeparted = 0;
    PMIX_LIST_FOREACH (trk, &pmix_server_globals.collectives, pmix_server_trkr_t) {
        if (trk == c->trk) {
            c->ndeparted = pmix_list_get_size(&trk->departed);
        }
    }
}

static pmix_status_t register_job(const char *name, pmix_rank_t nprocs)
{
    pmix_nspace_t ns;
    pmix_proc_t p;
    pmix_status_t rc;
    pmix_rank_t r;
    pmix_info_t info[2];
    char *noderegex = NULL, *ppnregex = NULL, ppn[64];

    snprintf(ppn, sizeof(ppn), "%s", (2 == nprocs) ? "0,1" : "0,1,2,3");
    PMIx_generate_regex(pmix_globals.hostname, &noderegex);
    PMIx_generate_ppn(ppn, &ppnregex);
    PMIX_INFO_LOAD(&info[0], PMIX_NODE_MAP, noderegex, PMIX_REGEX);
    PMIX_INFO_LOAD(&info[1], PMIX_PROC_MAP, ppnregex, PMIX_REGEX);
    free(noderegex);
    free(ppnregex);
    PMIX_LOAD_NSPACE(ns, name);
    rc = PMIx_server_register_nspace(ns, nprocs, info, 2, NULL, NULL);
    PMIX_INFO_DESTRUCT(&info[0]);
    PMIX_INFO_DESTRUCT(&info[1]);
    if (PMIX_SUCCESS != rc && PMIX_OPERATION_SUCCEEDED != rc) {
        return rc;
    }
    for (r = 0; r < nprocs; r++) {
        PMIX_LOAD_PROCID(&p, name, r);
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
    rankstate_t r;
    chain_t ch;
    reply_t rp;
    coll_t coll;
    pmix_nspace_t ns;
    char c;
    int sv[2];

    (void) argc;
    (void) argv;

    rc = PMIx_server_init(&mymodule, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "PMIx_server_init failed: %s\n", PMIx_Error_string(rc));
        return 1;
    }
    fprintf(stdout, "\n=== a local process leaving: unit tests ===\n\n");

    if (PMIX_SUCCESS != register_job(NS1, NPROCS) || PMIX_SUCCESS != register_job(NS2, NPROCS) ||
        PMIX_SUCCESS != register_job(NS3, 2)) {
        report("the jobs register", 0);
        goto done;
    }

    /* live, deregistered: the host reaps a client whose connection is
     * still open, while a request for its data is parked */
    cur_ns = NS1;
    if (0 != socketpair(AF_UNIX, SOCK_STREAM, 0, sv)) {
        report("socketpair", 0);
        goto done;
    }
    peer = connect_rank(0, sv[0], false);
    report("rank 0 is connected", NULL != peer);
    dmodex(0);
    report("live: a request for its data is parked", 1 == nparked() && !dm_fired);
    deregister(0);
    report("live: deregistering it answers the request", dm_fired && 0 == nparked());
    report("live: as a lost connection would", PMIX_ERR_LOST_CONNECTION == dm_status);
    report("live: and closes its connection", 0 == read(sv[1], &c, 1));
    close(sv[1]);

    /* never connected */
    dmodex(1);
    report("never connected: a request for its data is parked", 1 == nparked() && !dm_fired);
    deregister(1);
    report("never connected: deregistering it answers the request",
           dm_fired && PMIX_ERR_LOST_CONNECTION == dm_status && 0 == nparked());

    /* lost: a request made after the loss */
    peer = lose_rank(2, false, false);
    report("rank 2's connection is lost", NULL != peer && rank_state(2).lost);
    dmodex(2);
    report("lost: a request for its data is answered at once",
           dm_fired && PMIX_ERR_NOT_FOUND == dm_status && 0 == nparked());

    /* the loss event raised for it, while its aggregation window is open */
    on_progress(do_loss_chain, &ch);
    report("loss event: one is raised", ch.found);
    report("loss event: it is not cached for later handlers", ch.found && ch.nocache);

    /* a reply for it after the loss */
    rp.peer = peer;
    rp.rc = PMIX_SUCCESS;
    rp.queued = false;
    on_progress(do_reply, &rp);
    report("reply to the lost: refused", PMIX_ERR_UNREACH == rp.rc);
    report("reply to the lost: nothing queued on it", !rp.queued);

    /* returned: rank 3's data had arrived before it was lost; the process
     * that replaces it has published nothing */
    peer = lose_rank(3, false, true);
    report("rank 3 had published, then was lost", NULL != peer && rank_state(3).lost &&
                                                       rank_state(3).modex_recvd);
    dmodex(3);
    report("lost after publishing: a request for its data is not parked",
           dm_fired && 0 == nparked());
    peer = connect_rank(3, -1, false);
    if (NULL != peer) {
        on_progress(do_return, peer);
    }
    r = rank_state(3);
    report("returned: the rank is counted back in", NULL != peer && !r.lost);
    report("returned: it starts with nothing published", !r.modex_recvd);
    dmodex(3);
    report("returned: a request for its data waits for the new process",
           1 == nparked() && !dm_fired);

    /* finalize unanswered: rank 0 of the second job asked to finalize and
     * gave up waiting for the host's answer */
    cur_ns = NS2;
    r = rank_state(0);
    peer = lose_rank(0, true, false);
    report("finalize unanswered: its connection closes", NULL != peer);
    report("finalize unanswered: it is not counted lost",
           !rank_state(0).lost && r.nlocalprocs == rank_state(0).nlocalprocs);

    /* job deregistered: a collective over rank 2 of the second job, which
     * has contributed, and rank 0 of the third, which has not */
    memset(&coll, 0, sizeof(coll));
    on_progress(do_new_coll, &coll);
    report("a collective over two jobs waits", NULL != coll.trk);
    if (NULL == coll.trk) {
        goto done;
    }
    completions = 0;
    PMIX_LOAD_NSPACE(ns, NS3);
    PMIx_server_deregister_nspace(ns, NULL, NULL);
    progress_barrier();
    on_progress(do_coll_state, &coll);
    report("job deregistered: the collective completes on the other job", 1 == completions);

done:
    fprintf(stdout, "\n%d passed, %d failed\n", npass, nfail);
    PMIx_server_finalize();
    return (0 == nfail) ? 0 : 1;
}
