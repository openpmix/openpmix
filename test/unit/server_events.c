/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * White-box unit tests for the event handlers in
 * src/server/pmix_server_events.c. Both entry points are driven exactly
 * as the switchyard drives them - a wire buffer packed by hand and a
 * peer - so no DVM, no client, and no second process is involved.
 *
 * What is pinned down here:
 *
 *   the info counts that arrive off the wire
 *      Both handlers carry the directive count in a size_t and then
 *      consume it through the int32_t that PMIX_BFROPS_UNPACK takes.
 *      Neither is merely wasteful when the count is absurd:
 *      PMIx_Info_create computes "n * sizeof(pmix_info_t)" with no
 *      overflow guard and then *constructs every one of the n elements*,
 *      so a count that wraps that product yields a short allocation
 *      whose constructor loop runs straight off the end. With
 *      sizeof(pmix_info_t) == 552 the count 2^61 wraps it to exactly
 *      zero, for which malloc hands back a live pointer - the crash then
 *      happens inside the allocation, before any unpack has had a chance
 *      to screen anything. The notify handler compounds it by sizing the
 *      array as "ninfo + 1" and seeding the internal-notify marker at
 *      the last slot. So these cases *crash* an unfixed library rather
 *      than failing it - the same bargain test/unit/server_control.c
 *      makes.
 *
 *   a cached event is replayed to a peer once, not once per message
 *      A client sends another PMIX_REGEVENTS_CMD for every handler that
 *      names a code new to its server or that carries a directive, and
 *      each of those messages replays the whole notification cache. With
 *      no record of what a peer has already been sent, the second
 *      message re-delivered the event - the client's handler fired again
 *      - and decremented the event's "nleft" a second time, so an event
 *      targeted at several procs could be evicted from the cache before
 *      the rest of them ever saw it. The cached event below names two
 *      targets, so an unfixed library queues two relays and leaves the
 *      cache empty; a fixed one queues one and leaves the event in place
 *      for the target that has not registered yet.
 *
 *   the replayed event carries its range
 *      The range travels last on the wire. A tool recipient reads it to
 *      decide whether the event has to go on to its own host, and
 *      defaults a missing one to PMIX_RANGE_LOCAL, so leaving it off
 *      silently downgraded the range of every event picked up from the
 *      cache. The live dispatch has always sent it; the two cache
 *      replays did not.
 *
 *   the registration is acknowledged before its replay is delivered
 *      A client's handler must not run before the client has been told
 *      the handler's reference index - that index is what an
 *      application keys per-handler state on, and what it needs to
 *      deregister a one-shot from inside itself. The two arms of
 *      pmix_server_register_events that complete locally return
 *      PMIX_OPERATION_SUCCEEDED and the switchyard's caller queues the
 *      reply before the replay can run. The arm where the host takes
 *      the registration and completes it later does not: there the
 *      reply is the caddy's opcbfunc, which thread-shifts once more
 *      before it queues anything, so sending it from inside the replay
 *      put it on the wire behind every relay. Reaching that arm needs a
 *      system event code the host is not already forwarding and a host
 *      that answers PMIX_SUCCESS rather than completing atomically -
 *      which is what the stub below does.
 *
 *   the host is told who raised an event
 *      A client's event that goes beyond this server reaches the host's
 *      notify_event, and the host decides by the raiser's identity
 *      whether to pass it on - a debugger release that would reach
 *      another user's job, above all. The event arrived as the client
 *      sent it, so a PMIX_USERID it wrote in claimed any user it liked,
 *      and nothing said who it really was. The host is now given the
 *      identity the connection established, once, whatever the event
 *      carried.
 *
 *   a namespace range is the source's job
 *      An event with PMIX_RANGE_NAMESPACE and no targets of its own is
 *      sent live only to peers of the source's job, as the replay of the
 *      same event from the cache is. Registered peers of two jobs are sent the event and only the
 *      source's job's peer may be given it; a custom range naming the
 *      other job's process still reaches exactly that process.
 *
 *   one custom range per event
 *      An event naming more than one PMIX_EVENT_CUSTOM_RANGE is refused;
 *      one naming a single range is taken as before.
 *
 *   a registration the host refuses leaves the store as it was
 *      The request names a code the peer already holds entries for and a
 *      system code new to the store, and the host refuses it - once by
 *      returning an error from register_events, once in the callback it
 *      was handed. Either way the server must remove the entries the
 *      request added under both codes, and the new code's record, while
 *      the peer's earlier entries for the first code stay. A later
 *      registration for the new code must still go to the host and be
 *      recorded once.
 */

#include "src/include/pmix_config.h"

#include "include/pmix.h"
#include "include/pmix_server.h"
#include "src/include/pmix_globals.h"
#include "src/mca/bfrops/base/base.h"
#include "src/mca/ptl/base/base.h"
#include "src/server/pmix_server_ops.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* A stand-in for a connected client that has no socket. The server's
 * handlers are driven with it as the requesting peer, so a reply queued to
 * it comes to rest on its send_msg, where the cases read it back. It
 * cannot be pmix_globals.mypeer itself: a reply the server queues to its
 * own peer is the answer to a request it sent itself, and is delivered
 * straight back through the loopback rather than queued. It shares the
 * server's namespace and rank objects, so it packs and identifies itself
 * exactly as that peer does. */
static pmix_peer_t *standin_peer = NULL;

static pmix_peer_t *standin(void)
{
    if (NULL == standin_peer) {
        standin_peer = PMIX_NEW(pmix_peer_t);
        PMIX_RETAIN(pmix_globals.mypeer->nptr);
        standin_peer->nptr = pmix_globals.mypeer->nptr;
        PMIX_RETAIN(pmix_globals.mypeer->info);
        standin_peer->info = pmix_globals.mypeer->info;
        memcpy(&standin_peer->proc_type, &pmix_globals.mypeer->proc_type,
               sizeof(pmix_proc_type_t));
    }
    return standin_peer;
}

static void release_standin(void)
{
    if (NULL != standin_peer) {
        PMIX_RELEASE(standin_peer);
    }
}

/* a code with no meaning to the library, so nothing else reacts to it,
 * and deliberately not a system event - those take the arm that calls
 * the host, and this file has no host to call */
#define SEUT_CODE   (PMIX_EXTERNAL_ERR_BASE - 71)
/* a system event code - only these take the arm that calls the host */
#define SEUT_SYSCODE PMIX_EVENT_NODE_DOWN
/* the other proc the cached event names, so the event is not spent by a
 * single delivery */
#define SEUT_OTHER  "server-events-ut-other"
#define SEUT_SOURCE "server-events-ut-source"

/* a count whose product with sizeof(pmix_info_t) wraps a 64-bit size_t */
#define SEUT_WRAPPING_COUNT ((size_t) 1 << 61)

static int npass = 0;
static int nfail = 0;

/* The host takes the registration and completes it later, which is the
 * arm the ordering case is about. Park the callback so the test can fire
 * it when it chooses. */
static pmix_op_cbfunc_t host_regcb = NULL;
static void *host_regcbdata = NULL;
/* how many times the info named the requester as PMIX_REQUESTOR, and
 * whether it named the process that registered - the stand-in, which
 * sends as the server's own identity */
static size_t host_nrequestor = 0;
static bool host_requestor_ok = false;
/* when set, the host returns an error from register_events instead of
 * taking the request, and counts the requests it returned one for */
static bool host_refuse = false;
static size_t host_nrefused = 0;

static pmix_status_t stub_register_events(pmix_status_t *codes, size_t ncodes,
                                          const pmix_info_t info[], size_t ninfo,
                                          pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    size_t n;

    (void) codes;
    (void) ncodes;

    if (host_refuse) {
        ++host_nrefused;
        return PMIX_ERR_NOT_SUPPORTED;
    }
    host_nrequestor = 0;
    host_requestor_ok = false;
    for (n = 0; n < ninfo; n++) {
        if (PMIX_CHECK_KEY(&info[n], PMIX_REQUESTOR)) {
            ++host_nrequestor;
            host_requestor_ok = (PMIX_PROC == info[n].value.type &&
                                 NULL != info[n].value.data.proc &&
                                 PMIX_CHECK_PROCID(info[n].value.data.proc, &pmix_globals.myid));
        }
    }
    host_regcb = cbfunc;
    host_regcbdata = cbdata;
    return PMIX_SUCCESS;
}

/* what the host's notify_event was last handed */
static bool host_notified = false;
static size_t host_nuid = 0;
static uint32_t host_uid = 0;
static size_t host_ngid = 0;
static uint32_t host_gid = 0;

static pmix_status_t stub_notify_event(pmix_status_t code, const pmix_proc_t *source,
                                       pmix_data_range_t range, pmix_info_t info[], size_t ninfo,
                                       pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    size_t n;

    (void) code;
    (void) source;
    (void) range;
    (void) cbfunc;
    (void) cbdata;

    host_notified = true;
    host_nuid = 0;
    host_ngid = 0;
    for (n = 0; n < ninfo; n++) {
        if (PMIX_CHECK_KEY(&info[n], PMIX_USERID)) {
            ++host_nuid;
            (void) PMIx_Value_get_number(&info[n].value, &host_uid, PMIX_UINT32);
        } else if (PMIX_CHECK_KEY(&info[n], PMIX_GRPID)) {
            ++host_ngid;
            (void) PMIx_Value_get_number(&info[n].value, &host_gid, PMIX_UINT32);
        }
    }
    /* nothing to forward it to - done here */
    return PMIX_OPERATION_SUCCEEDED;
}

static void report(const char *what, bool ok)
{
    fprintf(stdout, "%-58s : %s\n", what, ok ? "PASS" : "FAIL");
    if (ok) {
        ++npass;
    } else {
        ++nfail;
    }
}

/* A blocking round trip through the progress thread: anything queued
 * ahead of this call has run by the time it returns. */
static void progress_barrier(void)
{
    pmix_value_t v;

    PMIX_VALUE_LOAD(&v, "barrier", PMIX_STRING);
    PMIx_Store_internal(&pmix_globals.myid, "server-events-ut.barrier", &v);
    PMIX_VALUE_DESTRUCT(&v);
}

/* Wait for a registration's cached-event replay to have run.
 *
 * Two barriers, and it takes both. Handling a registration costs two
 * events, not one: _check_cached_events answers the requestor and then
 * posts _replay_cached_events as a separate event, so that the
 * acknowledgement is queued ahead of anything the replay sends. If the
 * first barrier is posted before _check_cached_events runs, the replay
 * is posted behind it and has not run when it returns - a single
 * barrier makes this test pass or fail on timing. The second barrier is
 * posted after the first has returned, so the replay is ahead of it in
 * every interleaving. */
static void settle(void)
{
    progress_barrier();
    progress_barrier();
}

/* How many messages the server has queued to a peer that has no socket.
 * With sd < 0 the send event is never armed, so they simply come to rest
 * on the peer: the first on send_msg, the rest on send_queue. */
static size_t peer_queued_count(pmix_peer_t *peer)
{
    size_t n = 0;

    if (NULL != peer->send_msg) {
        ++n;
    }
    n += pmix_list_get_size(&peer->send_queue);
    return n;
}

static size_t queued_count(void)
{
    return peer_queued_count(standin());
}

/* Take the first of those messages and hand back its buffer. */
static pmix_buffer_t *take_queued_reply(void)
{
    pmix_peer_t *peer = standin();
    pmix_ptl_send_t *snd = peer->send_msg;
    pmix_buffer_t *buf;

    if (NULL == snd) {
        return NULL;
    }
    peer->send_msg = NULL;
    buf = snd->data;
    /* the buffer is ours now - the send's destructor would release it */
    snd->data = NULL;
    PMIX_RELEASE(snd);
    return buf;
}

static void peer_drain(pmix_peer_t *peer)
{
    pmix_ptl_send_t *snd;

    if (NULL != peer->send_msg) {
        PMIX_RELEASE(peer->send_msg);
        peer->send_msg = NULL;
    }
    while (NULL != (snd = (pmix_ptl_send_t *) pmix_list_remove_first(&peer->send_queue))) {
        PMIX_RELEASE(snd);
    }
}

static void drain_queued(void)
{
    peer_drain(standin());
}

/* Drive one PMIX_REGEVENTS_CMD. The wire form is the code count, the
 * codes, the directive count, and the directives - and the declared
 * directive count is a parameter so it can be made to disagree with what
 * is actually packed. */
static pmix_status_t do_register(size_t ncodes, pmix_status_t *codes,
                                 size_t declared_ninfo, size_t ninfo, pmix_info_t *info)
{
    pmix_buffer_t *buf;
    pmix_status_t rc;

    buf = PMIX_NEW(pmix_buffer_t);
    PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, buf, &ncodes, 1, PMIX_SIZE);
    if (PMIX_SUCCESS == rc && 0 < ncodes) {
        PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, buf, codes, ncodes, PMIX_STATUS);
    }
    if (PMIX_SUCCESS == rc) {
        PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, buf, &declared_ninfo, 1, PMIX_SIZE);
    }
    if (PMIX_SUCCESS == rc && 0 < ninfo) {
        PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, buf, info, ninfo, PMIX_INFO);
    }
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(buf);
        return rc;
    }
    rc = pmix_server_register_events(standin(), buf, NULL, NULL);
    PMIX_RELEASE(buf);
    return rc;
}

/* The same request, but handed the reply callback and caddy the
 * switchyard would hand it, so the acknowledgement really travels the
 * path a client's does. */
static pmix_status_t do_register_acked(size_t ncodes, pmix_status_t *codes)
{
    pmix_buffer_t *buf;
    pmix_server_caddy_t *cd;
    pmix_status_t rc;
    size_t ninfo = 0;

    buf = PMIX_NEW(pmix_buffer_t);
    PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, buf, &ncodes, 1, PMIX_SIZE);
    if (PMIX_SUCCESS == rc) {
        PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, buf, codes, ncodes, PMIX_STATUS);
    }
    if (PMIX_SUCCESS == rc) {
        PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, buf, &ninfo, 1, PMIX_SIZE);
    }
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(buf);
        return rc;
    }

    cd = PMIX_NEW(pmix_server_caddy_t);
    cd->peer = standin();
    PMIX_RETAIN(cd->peer);
    cd->hdr.tag = 0;

    rc = pmix_server_register_events(standin(), buf,
                                     pmix_server_events_cbfunc, cd);
    if (PMIX_SUCCESS != rc) {
        /* the switchyard owns the caddy on a non-success return */
        PMIX_RELEASE(cd);
    }
    PMIX_RELEASE(buf);
    return rc;
}

/* Drive one PMIX_NOTIFY_CMD from a given peer */
static pmix_status_t do_notify_as(pmix_peer_t *peer, pmix_status_t status,
                                  pmix_data_range_t range, size_t ninfo, pmix_info_t *info);

/* Drive one PMIX_NOTIFY_CMD: status, range, directive count, directives. */
static pmix_status_t do_notify(pmix_status_t status, pmix_data_range_t range,
                               size_t declared_ninfo, size_t ninfo, pmix_info_t *info)
{
    pmix_buffer_t *buf;
    pmix_status_t rc;

    buf = PMIX_NEW(pmix_buffer_t);
    PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, buf, &status, 1, PMIX_STATUS);
    if (PMIX_SUCCESS == rc) {
        PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, buf, &range, 1, PMIX_DATA_RANGE);
    }
    if (PMIX_SUCCESS == rc) {
        PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, buf, &declared_ninfo, 1, PMIX_SIZE);
    }
    if (PMIX_SUCCESS == rc && 0 < ninfo) {
        PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, buf, info, ninfo, PMIX_INFO);
    }
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(buf);
        return rc;
    }
    rc = pmix_server_event_recvd_from_client(standin(), buf, NULL, NULL);
    PMIX_RELEASE(buf);
    return rc;
}

/* Park an event in the notification cache, naming our own peer and one
 * absent proc as its targets, so a single delivery does not spend it.
 * Runs on the progress thread - the hotel belongs to it. */
static pmix_status_t seed_code = SEUT_CODE;

static void seed_cached_event(int sd, short args, void *cbdata)
{
    pmix_lock_t *lock = (pmix_lock_t *) cbdata;
    pmix_notify_caddy_t *cd;

    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    cd = PMIX_NEW(pmix_notify_caddy_t);
    cd->status = seed_code;
    PMIX_LOAD_PROCID(&cd->source, SEUT_SOURCE, 0);
    cd->range = PMIX_RANGE_SESSION;
    cd->ntargets = 2;
    cd->targets = (pmix_proc_t *) malloc(2 * sizeof(pmix_proc_t));
    PMIX_LOAD_PROCID(&cd->targets[0], pmix_globals.myid.nspace, pmix_globals.myid.rank);
    PMIX_LOAD_PROCID(&cd->targets[1], SEUT_OTHER, 0);
    cd->nleft = 2;
    lock->status = pmix_notify_event_cache(cd);
    if (PMIX_SUCCESS != lock->status) {
        PMIX_RELEASE(cd);
    }
    PMIX_WAKEUP_THREAD(lock);
}

/* Park an event under seed_code and wait for it to land. */
static bool seed_event(pmix_status_t code)
{
    pmix_lock_t lock;
    pmix_event_t ev;
    bool ok;

    seed_code = code;
    PMIX_CONSTRUCT_LOCK(&lock);
    pmix_event_assign(&ev, pmix_globals.evbase, -1, EV_WRITE, seed_cached_event, &lock);
    pmix_event_active(&ev, EV_WRITE, 1);
    PMIX_WAIT_THREAD(&lock);
    ok = (PMIX_SUCCESS == lock.status);
    PMIX_DESTRUCT_LOCK(&lock);
    return ok;
}

/* Is this queued message a relayed notification, or the bare status
 * that acknowledges a registration? The unpacker type-checks, so asking
 * for the command that only a notification carries answers it. */
static bool buffer_is_notify(pmix_buffer_t *buf)
{
    pmix_cmd_t cmd;
    pmix_status_t rc;
    int32_t cnt = 1;

    PMIX_BFROPS_UNPACK(rc, pmix_globals.mypeer, buf, &cmd, &cnt, PMIX_COMMAND);
    return (PMIX_SUCCESS == rc && PMIX_NOTIFY_CMD == cmd);
}

/* is anything still parked in the cache? */
static bool cache_occupied(void)
{
    pmix_notify_caddy_t *cd;
    int i;

    for (i = 0; i < pmix_globals.max_events; i++) {
        pmix_hotel_knock(&pmix_globals.notifications, i, (void **) &cd);
        if (NULL != cd) {
            return true;
        }
    }
    return false;
}

/* Read back a relayed notification and report what it carried. */
static bool relay_is_our_event(pmix_buffer_t *buf, pmix_data_range_t *range)
{
    pmix_cmd_t cmd;
    pmix_status_t rc, status;
    pmix_proc_t source;
    size_t ninfo;
    int32_t cnt;

    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, pmix_globals.mypeer, buf, &cmd, &cnt, PMIX_COMMAND);
    if (PMIX_SUCCESS != rc || PMIX_NOTIFY_CMD != cmd) {
        return false;
    }
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, pmix_globals.mypeer, buf, &status, &cnt, PMIX_STATUS);
    if (PMIX_SUCCESS != rc || SEUT_CODE != status) {
        return false;
    }
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, pmix_globals.mypeer, buf, &source, &cnt, PMIX_PROC);
    if (PMIX_SUCCESS != rc) {
        return false;
    }
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, pmix_globals.mypeer, buf, &ninfo, &cnt, PMIX_SIZE);
    if (PMIX_SUCCESS != rc || 0 != ninfo) {
        return false;
    }
    /* the range travels last, exactly as the live dispatch sends it */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, pmix_globals.mypeer, buf, range, &cnt, PMIX_DATA_RANGE);
    return (PMIX_SUCCESS == rc);
}

static pmix_status_t do_notify_as(pmix_peer_t *peer, pmix_status_t status,
                                  pmix_data_range_t range, size_t ninfo, pmix_info_t *info)
{
    pmix_buffer_t *buf;
    pmix_status_t rc;

    buf = PMIX_NEW(pmix_buffer_t);
    PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, buf, &status, 1, PMIX_STATUS);
    if (PMIX_SUCCESS == rc) {
        PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, buf, &range, 1, PMIX_DATA_RANGE);
    }
    if (PMIX_SUCCESS == rc) {
        PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, buf, &ninfo, 1, PMIX_SIZE);
    }
    if (PMIX_SUCCESS == rc && 0 < ninfo) {
        PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, buf, info, ninfo, PMIX_INFO);
    }
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(buf);
        return rc;
    }
    rc = pmix_server_event_recvd_from_client(peer, buf, NULL, NULL);
    PMIX_RELEASE(buf);
    return rc;
}

/* ------------------------------------------------------------------ */
/* who may release a process held for a debugger                       */
/* ------------------------------------------------------------------ */

/* register a two-rank job owned by uid/gid, both ranks local */
static pmix_status_t reg_job(const char *name, uid_t uid, gid_t gid)
{
    pmix_info_t info[2];
    pmix_nspace_t ns;
    pmix_proc_t proc;
    pmix_status_t rc;
    uint32_t u32;
    pmix_rank_t r;

    u32 = (uint32_t) uid;
    PMIX_INFO_LOAD(&info[0], PMIX_USERID, &u32, PMIX_UINT32);
    u32 = (uint32_t) gid;
    PMIX_INFO_LOAD(&info[1], PMIX_GRPID, &u32, PMIX_UINT32);
    PMIX_LOAD_NSPACE(ns, name);
    rc = PMIx_server_register_nspace(ns, 2, info, 2, NULL, NULL);
    PMIX_INFO_DESTRUCT(&info[0]);
    PMIX_INFO_DESTRUCT(&info[1]);
    if (PMIX_SUCCESS != rc && PMIX_OPERATION_SUCCEEDED != rc) {
        return rc;
    }
    for (r = 0; r < 2; r++) {
        PMIX_LOAD_PROCID(&proc, name, r);
        rc = PMIx_server_register_client(&proc, uid, gid, NULL, NULL, NULL);
        if (PMIX_SUCCESS != rc && PMIX_OPERATION_SUCCEEDED != rc) {
            return rc;
        }
    }
    return PMIX_SUCCESS;
}

/* a connected client of a job, as far as the tables go */
static pmix_peer_t *job_peer(const char *nsname, pmix_rank_t rank, uid_t uid, gid_t gid)
{
    pmix_namespace_t *ns, *nptr = NULL;
    pmix_peer_t *p;

    PMIX_LIST_FOREACH (ns, &pmix_globals.nspaces, pmix_namespace_t) {
        if (NULL != ns->nspace && 0 == strcmp(ns->nspace, nsname)) {
            nptr = ns;
        }
    }
    if (NULL == nptr) {
        return NULL;
    }
    if (NULL == nptr->compat.bfrops) {
        memcpy(&nptr->compat, &pmix_globals.mypeer->nptr->compat, sizeof(pmix_personality_t));
    }
    p = PMIX_NEW(pmix_peer_t);
    PMIX_RETAIN(nptr);
    p->nptr = nptr;
    memcpy(&p->proc_type, &pmix_globals.mypeer->proc_type, sizeof(pmix_proc_type_t));
    p->info = PMIX_NEW(pmix_rank_info_t);
    p->info->pname.nspace = strdup(nsname);
    p->info->pname.rank = rank;
    p->info->uid = uid;
    p->info->gid = gid;
    return p;
}

/* a connected client of another user's job */
static pmix_peer_t *stranger_peer(const char *nsname, uid_t uid, gid_t gid)
{
    return job_peer(nsname, 0, uid, gid);
}

static pmix_status_t release_from(pmix_peer_t *peer, pmix_data_range_t range,
                                  const char *nspace)
{
    pmix_info_t info;
    pmix_proc_t target;
    pmix_status_t rc;

    if (NULL == nspace) {
        return do_notify_as(peer, PMIX_DEBUGGER_RELEASE, range, 0, NULL);
    }
    PMIX_LOAD_PROCID(&target, nspace, 0);
    PMIX_INFO_LOAD(&info, PMIX_EVENT_CUSTOM_RANGE, &target, PMIX_PROC);
    rc = do_notify_as(peer, PMIX_DEBUGGER_RELEASE, range, 1, &info);
    PMIX_INFO_DESTRUCT(&info);
    return rc;
}

/* A release lets a process held for a debugger go on, so it is taken
 * only from a peer whose user may act on that process's job - the job's
 * owner, a user it allowed, root, or the server's own user. A job the host
 * never registered here is the host's to judge: the release passes on to
 * it. A release with no targets reaches every job here, so every one of
 * them has to pass. */
static void test_debugger_release(void)
{
    uid_t other = geteuid() + 1;
    gid_t ogrp = getegid() + 1;
    pmix_peer_t *stranger;

    if (PMIX_SUCCESS != reg_job("seut-mine", geteuid(), getegid()) ||
        PMIX_SUCCESS != reg_job("seut-theirs", other, ogrp) ||
        NULL == (stranger = stranger_peer("seut-theirs", other, ogrp))) {
        report("debugger release: setup", 0);
        return;
    }
    report("another user's peer may not release my job's process",
           PMIX_ERR_NO_PERMISSIONS == release_from(stranger, PMIX_RANGE_CUSTOM, "seut-mine"));
    report("it may release its own job's process",
           PMIX_SUCCESS == release_from(stranger, PMIX_RANGE_CUSTOM, "seut-theirs"));
    report("a job not registered here is left to the host",
           PMIX_SUCCESS == release_from(stranger, PMIX_RANGE_CUSTOM, "seut-elsewhere"));
    report("a release with no targets may not reach my job",
           PMIX_ERR_NO_PERMISSIONS == release_from(stranger, PMIX_RANGE_LOCAL, NULL));
    report("a release to its own namespace is allowed",
           PMIX_SUCCESS == release_from(stranger, PMIX_RANGE_NAMESPACE, NULL));
    report("the server's own user may release any job's process",
           PMIX_SUCCESS == release_from(standin(), PMIX_RANGE_CUSTOM, "seut-theirs"));
    PMIX_RELEASE(stranger);
}

/* A client's event that goes beyond this server reaches the host with
 * the identity its connection established, not one it wrote in */
static void test_event_identity(void)
{
    uid_t other = geteuid() + 2;
    gid_t ogrp = getegid() + 2;
    uint32_t claimed = 0;
    pmix_info_t info[2];
    pmix_peer_t *raiser;
    pmix_status_t rc;

    if (PMIX_SUCCESS != reg_job("seut-raiser", other, ogrp) ||
        NULL == (raiser = stranger_peer("seut-raiser", other, ogrp))) {
        report("event identity: setup", 0);
        return;
    }
    host_notified = false;
    rc = do_notify_as(raiser, SEUT_CODE, PMIX_RANGE_SESSION, 0, NULL);
    settle();
    report("a client's session-wide event is taken",
           PMIX_SUCCESS == rc || PMIX_OPERATION_SUCCEEDED == rc);
    report("the host is told who raised it",
           host_notified && 1 == host_nuid && (uint32_t) other == host_uid &&
               1 == host_ngid && (uint32_t) ogrp == host_gid);

    /* claiming to be root changes nothing the host is told */
    PMIX_INFO_LOAD(&info[0], PMIX_USERID, &claimed, PMIX_UINT32);
    PMIX_INFO_LOAD(&info[1], PMIX_GRPID, &claimed, PMIX_UINT32);
    host_notified = false;
    rc = do_notify_as(raiser, SEUT_CODE, PMIX_RANGE_SESSION, 2, info);
    settle();
    report("an event claiming another identity is taken",
           PMIX_SUCCESS == rc || PMIX_OPERATION_SUCCEEDED == rc);
    report("the host is told the raiser's own uid and gid, once each",
           host_notified && 1 == host_nuid && (uint32_t) other == host_uid &&
               1 == host_ngid && (uint32_t) ogrp == host_gid);
    PMIX_INFO_DESTRUCT(&info[0]);
    PMIX_INFO_DESTRUCT(&info[1]);
    PMIX_RELEASE(raiser);
}

/* ------------------------------------------------------------------ */
/* which registered peers a client's event reaches                      */
/* ------------------------------------------------------------------ */

/* a code no earlier case raised or cached, so a registration made for it
 * replays nothing and what a peer is sent afterwards is the live event */
#define SEUT_RNGCODE (PMIX_EXTERNAL_ERR_BASE - 72)

/* register one peer for one code, as its PMIX_REGEVENTS_CMD would */
static pmix_status_t register_as(pmix_peer_t *peer, pmix_status_t code)
{
    pmix_buffer_t *buf;
    pmix_status_t rc;
    size_t ncodes = 1, ninfo = 0;

    buf = PMIX_NEW(pmix_buffer_t);
    PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, buf, &ncodes, 1, PMIX_SIZE);
    if (PMIX_SUCCESS == rc) {
        PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, buf, &code, 1, PMIX_STATUS);
    }
    if (PMIX_SUCCESS == rc) {
        PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, buf, &ninfo, 1, PMIX_SIZE);
    }
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(buf);
        return rc;
    }
    rc = pmix_server_register_events(peer, buf, NULL, NULL);
    PMIX_RELEASE(buf);
    if (PMIX_OPERATION_SUCCEEDED == rc) {
        rc = PMIX_SUCCESS;
    }
    return rc;
}

/* Was this peer sent a notification of "code"? Everything queued to it
 * is looked at and then dropped. */
static bool peer_was_sent(pmix_peer_t *peer, pmix_status_t code)
{
    pmix_ptl_send_t *snd;
    pmix_cmd_t cmd;
    pmix_status_t rc, status;
    int32_t cnt;
    bool found = false;

    if (NULL != peer->send_msg) {
        pmix_list_prepend(&peer->send_queue, &peer->send_msg->super);
        peer->send_msg = NULL;
    }
    while (NULL != (snd = (pmix_ptl_send_t *) pmix_list_remove_first(&peer->send_queue))) {
        if (NULL != snd->data) {
            cnt = 1;
            PMIX_BFROPS_UNPACK(rc, pmix_globals.mypeer, snd->data, &cmd, &cnt, PMIX_COMMAND);
            if (PMIX_SUCCESS == rc && PMIX_NOTIFY_CMD == cmd) {
                cnt = 1;
                PMIX_BFROPS_UNPACK(rc, pmix_globals.mypeer, snd->data, &status, &cnt,
                                   PMIX_STATUS);
                if (PMIX_SUCCESS == rc && code == status) {
                    found = true;
                }
            }
        }
        PMIX_RELEASE(snd);
    }
    return found;
}

/* An event with PMIX_RANGE_NAMESPACE and no targets of its own reaches
 * the source's job only, as a replay of the cached event does - whoever
 * raised it. A custom range still reaches exactly the processes it
 * names, in whatever job. */
static void test_namespace_range(void)
{
    pmix_peer_t *a0 = NULL, *a1 = NULL, *b0 = NULL;
    pmix_info_t info;
    pmix_proc_t target;
    pmix_status_t rc;
    bool toa1, tob0;

    if (PMIX_SUCCESS != reg_job("seut-rng-a", geteuid(), getegid()) ||
        PMIX_SUCCESS != reg_job("seut-rng-b", geteuid(), getegid()) ||
        NULL == (a0 = job_peer("seut-rng-a", 0, geteuid(), getegid())) ||
        NULL == (a1 = job_peer("seut-rng-a", 1, geteuid(), getegid())) ||
        NULL == (b0 = job_peer("seut-rng-b", 0, geteuid(), getegid())) ||
        PMIX_SUCCESS != register_as(a1, SEUT_RNGCODE) ||
        PMIX_SUCCESS != register_as(b0, SEUT_RNGCODE) ||
        PMIX_SUCCESS != register_as(a1, PMIX_DEBUGGER_RELEASE) ||
        PMIX_SUCCESS != register_as(b0, PMIX_DEBUGGER_RELEASE)) {
        report("namespace range: setup", 0);
        goto cleanup;
    }
    /* whatever the registrations replayed is not what is measured */
    settle();
    peer_drain(a1);
    peer_drain(b0);

    rc = do_notify_as(a0, SEUT_RNGCODE, PMIX_RANGE_NAMESPACE, 0, NULL);
    settle();
    report("a namespace-range event from a client is taken",
           PMIX_SUCCESS == rc || PMIX_OPERATION_SUCCEEDED == rc);
    toa1 = peer_was_sent(a1, SEUT_RNGCODE);
    tob0 = peer_was_sent(b0, SEUT_RNGCODE);
    report("a namespace-range event reaches the source's own job", toa1);
    report("a namespace-range event does not reach another job", !tob0);

    rc = release_from(a0, PMIX_RANGE_NAMESPACE, NULL);
    settle();
    report("a namespace-range debugger release from a client is taken",
           PMIX_SUCCESS == rc || PMIX_OPERATION_SUCCEEDED == rc);
    toa1 = peer_was_sent(a1, PMIX_DEBUGGER_RELEASE);
    tob0 = peer_was_sent(b0, PMIX_DEBUGGER_RELEASE);
    report("a namespace-range release reaches the source's own job", toa1);
    report("a namespace-range release does not reach another job", !tob0);

    /* a custom range is the processes it names, in any job */
    PMIX_LOAD_PROCID(&target, "seut-rng-b", 0);
    PMIX_INFO_LOAD(&info, PMIX_EVENT_CUSTOM_RANGE, &target, PMIX_PROC);
    rc = do_notify_as(a0, SEUT_RNGCODE, PMIX_RANGE_CUSTOM, 1, &info);
    PMIX_INFO_DESTRUCT(&info);
    settle();
    report("a custom-range event naming another job's process is taken",
           PMIX_SUCCESS == rc || PMIX_OPERATION_SUCCEEDED == rc);
    toa1 = peer_was_sent(a1, SEUT_RNGCODE);
    tob0 = peer_was_sent(b0, SEUT_RNGCODE);
    report("a custom-range event reaches the process it names", tob0);
    report("a custom-range event does not reach one it does not name", !toa1);

    /* and the other job's peer does take a release meant for it, so its
     * not being sent the namespace-range one above says something */
    rc = release_from(a0, PMIX_RANGE_CUSTOM, "seut-rng-b");
    settle();
    report("a custom-range release naming another job is taken",
           PMIX_SUCCESS == rc || PMIX_OPERATION_SUCCEEDED == rc);
    toa1 = peer_was_sent(a1, PMIX_DEBUGGER_RELEASE);
    tob0 = peer_was_sent(b0, PMIX_DEBUGGER_RELEASE);
    report("a custom-range release reaches the job it names", tob0);
    report("a custom-range release does not reach the source's job", !toa1);

cleanup:
    if (NULL != a1) {
        peer_drain(a1);
    }
    if (NULL != b0) {
        peer_drain(b0);
    }
    /* the registrations hold their own references */
    if (NULL != a0) {
        PMIX_RELEASE(a0);
    }
    if (NULL != a1) {
        PMIX_RELEASE(a1);
    }
    if (NULL != b0) {
        PMIX_RELEASE(b0);
    }
}

/* An event names at most one custom range */
static void test_custom_range_count(void)
{
    uid_t other = geteuid() + 3;
    gid_t ogrp = getegid() + 3;
    pmix_info_t info[2];
    pmix_proc_t target[2];
    pmix_peer_t *raiser;
    pmix_status_t rc;

    if (PMIX_SUCCESS != reg_job("seut-crng", other, ogrp) ||
        NULL == (raiser = stranger_peer("seut-crng", other, ogrp))) {
        report("custom range count: setup", 0);
        return;
    }
    PMIX_LOAD_PROCID(&target[0], "seut-crng", 0);
    PMIX_LOAD_PROCID(&target[1], "seut-crng", 1);
    PMIX_INFO_LOAD(&info[0], PMIX_EVENT_CUSTOM_RANGE, &target[0], PMIX_PROC);
    PMIX_INFO_LOAD(&info[1], PMIX_EVENT_CUSTOM_RANGE, &target[1], PMIX_PROC);
    rc = do_notify_as(raiser, SEUT_CODE, PMIX_RANGE_CUSTOM, 2, info);
    report("an event with two custom ranges is refused", PMIX_ERR_BAD_PARAM == rc);
    rc = do_notify_as(raiser, SEUT_CODE, PMIX_RANGE_CUSTOM, 1, info);
    report("an event with one custom range is taken",
           PMIX_SUCCESS == rc || PMIX_OPERATION_SUCCEEDED == rc);
    settle();
    PMIX_INFO_DESTRUCT(&info[1]);

    /* a release whose first range is the raiser's own job and whose
     * second is a job of the server's user */
    PMIX_LOAD_PROCID(&target[1], "seut-mine", 0);
    PMIX_INFO_LOAD(&info[1], PMIX_EVENT_CUSTOM_RANGE, &target[1], PMIX_PROC);
    rc = do_notify_as(raiser, PMIX_DEBUGGER_RELEASE, PMIX_RANGE_CUSTOM, 2, info);
    report("a release with two custom ranges is refused", PMIX_ERR_BAD_PARAM == rc);
    rc = do_notify_as(raiser, PMIX_DEBUGGER_RELEASE, PMIX_RANGE_CUSTOM, 1, info);
    report("a release with one custom range, its own job, is taken",
           PMIX_SUCCESS == rc || PMIX_OPERATION_SUCCEEDED == rc);
    settle();
    PMIX_INFO_DESTRUCT(&info[0]);
    PMIX_INFO_DESTRUCT(&info[1]);
    PMIX_RELEASE(raiser);
}

/* ------------------------------------------------------------------ */
/* a registration the host refuses                                      */
/* ------------------------------------------------------------------ */

/* system codes no other case registers, so the store holds nothing for
 * either of them until this case does */
#define SEUT_REFCODE_SYNC  (PMIX_EVENT_SYS_BASE - 61)
#define SEUT_REFCODE_ASYNC (PMIX_EVENT_SYS_BASE - 62)

/* what the registration store holds, read on the progress thread */
typedef struct {
    pmix_lock_t lock;
    pmix_status_t code;
    pmix_peer_t *peer;
    size_t ncodes;      // records in the store, one per code
    size_t nentries;    // peer entries under all of them
    bool have_code;     // the store has a record for "code"
    bool active;        // and the host is forwarding it
    size_t npeer;       // entries "peer" holds under "code"
} store_view_t;

static void read_store(int sd, short args, void *cbdata)
{
    store_view_t *v = (store_view_t *) cbdata;
    pmix_regevents_info_t *reginfo;
    pmix_peer_events_info_t *prev;

    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    PMIX_LIST_FOREACH (reginfo, &pmix_server_globals.events, pmix_regevents_info_t) {
        ++v->ncodes;
        v->nentries += pmix_list_get_size(&reginfo->peers);
        if (v->code != reginfo->code) {
            continue;
        }
        v->have_code = true;
        v->active = reginfo->active;
        PMIX_LIST_FOREACH (prev, &reginfo->peers, pmix_peer_events_info_t) {
            if (v->peer == prev->peer) {
                ++v->npeer;
            }
        }
    }
    PMIX_WAKEUP_THREAD(&v->lock);
}

static void view_store(pmix_status_t code, pmix_peer_t *peer, store_view_t *v)
{
    pmix_event_t ev;

    memset(v, 0, sizeof(*v));
    v->code = code;
    v->peer = peer;
    PMIX_CONSTRUCT_LOCK(&v->lock);
    pmix_event_assign(&ev, pmix_globals.evbase, -1, EV_WRITE, read_store, v);
    pmix_event_active(&ev, EV_WRITE, 1);
    PMIX_WAIT_THREAD(&v->lock);
    PMIX_DESTRUCT_LOCK(&v->lock);
}

/* the store holds what it held in "before" for both codes of a request */
static bool store_unchanged(const store_view_t *before_old, const store_view_t *before_new,
                            pmix_status_t newcode)
{
    store_view_t old_after, new_after;

    view_store(before_old->code, before_old->peer, &old_after);
    view_store(newcode, before_new->peer, &new_after);
    if (old_after.ncodes != before_old->ncodes ||
        old_after.nentries != before_old->nentries) {
        fprintf(stdout, "    store: %lu codes and %lu entries, %lu and %lu before\n",
                (unsigned long) old_after.ncodes, (unsigned long) old_after.nentries,
                (unsigned long) before_old->ncodes, (unsigned long) before_old->nentries);
        return false;
    }
    if (old_after.npeer != before_old->npeer) {
        fprintf(stdout, "    the peer holds %lu entries for the first code, %lu before\n",
                (unsigned long) old_after.npeer, (unsigned long) before_old->npeer);
        return false;
    }
    if (new_after.have_code || 0 != new_after.npeer) {
        fprintf(stdout, "    the store still has a record for the new code (%lu entries)\n",
                (unsigned long) new_after.npeer);
        return false;
    }
    return true;
}

static void test_refused_registration(void)
{
    store_view_t old_before, new_before, view;
    pmix_status_t codes[2], rc;

    /* SEUT_CODE: the stand-in already holds entries for it, from the
     * replay cases above */
    codes[0] = SEUT_CODE;

    /* --- the host returns an error from register_events --- */
    codes[1] = SEUT_REFCODE_SYNC;
    view_store(SEUT_CODE, standin(), &old_before);
    view_store(SEUT_REFCODE_SYNC, standin(), &new_before);
    report("refusal setup: the peer already holds a code's entries",
           0 < old_before.npeer);
    report("refusal setup: no record yet for the new code",
           !new_before.have_code);

    host_refuse = true;
    host_nrefused = 0;
    rc = do_register(2, codes, 0, 0, NULL);
    host_refuse = false;
    report("a registration the host refuses at once is reported failed",
           PMIX_ERR_NOT_SUPPORTED == rc && 1 == host_nrefused);
    report("an immediate refusal leaves the store as it was",
           store_unchanged(&old_before, &new_before, SEUT_REFCODE_SYNC));

    /* --- the host takes the request and refuses it in the callback --- */
    codes[1] = SEUT_REFCODE_ASYNC;
    view_store(SEUT_CODE, standin(), &old_before);
    view_store(SEUT_REFCODE_ASYNC, standin(), &new_before);
    host_regcb = NULL;
    rc = do_register(2, codes, 0, 0, NULL);
    report("the host takes a registration to complete later",
           PMIX_SUCCESS == rc && NULL != host_regcb);
    view_store(SEUT_REFCODE_ASYNC, standin(), &view);
    report("the request is recorded while the host holds it",
           view.have_code && 1 == view.npeer && view.active);
    if (NULL != host_regcb) {
        host_regcb(PMIX_ERR_NOT_SUPPORTED, host_regcbdata);
        host_regcb = NULL;
    }
    settle();
    report("a refusal in the host callback leaves the store as it was",
           store_unchanged(&old_before, &new_before, SEUT_REFCODE_ASYNC));

    /* --- a later registration for the code goes to the host again --- */
    codes[0] = SEUT_REFCODE_SYNC;
    host_regcb = NULL;
    rc = do_register(1, codes, 0, 0, NULL);
    report("a later registration for the code goes to the host",
           PMIX_SUCCESS == rc && NULL != host_regcb);
    if (NULL != host_regcb) {
        host_regcb(PMIX_SUCCESS, host_regcbdata);
        host_regcb = NULL;
    }
    settle();
    view_store(SEUT_REFCODE_SYNC, standin(), &view);
    report("the accepted registration is recorded once",
           view.have_code && 1 == view.npeer && view.active);
    drain_queued();
}

int main(int argc, char **argv)
{
    static pmix_server_module_t mymodule = {0};
    pmix_status_t rc, codes[1], syscodes[1];
    pmix_info_t dir;
    pmix_buffer_t *relay;
    pmix_data_range_t range = PMIX_RANGE_UNDEF;
    size_t nqueued;
    bool ok;

    (void) argc;
    (void) argv;

    fprintf(stdout, "server_events: server-side event registration and notification\n");

    mymodule.register_events = stub_register_events;
    mymodule.notify_event = stub_notify_event;

    rc = PMIx_server_init(&mymodule, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "PMIx_server_init failed: %s\n", PMIx_Error_string(rc));
        return 1;
    }

    codes[0] = SEUT_CODE;
    syscodes[0] = SEUT_SYSCODE;
    PMIX_INFO_LOAD(&dir, "server-events-ut.dir", "value", PMIX_STRING);

    /* --- counts that do not survive the trip through int32_t ---------- */
    rc = do_register(1, codes, SEUT_WRAPPING_COUNT, 1, &dir);
    report("register_events rejects an unusable info count", PMIX_ERR_BAD_PARAM == rc);

    rc = do_notify(SEUT_CODE, PMIX_RANGE_LOCAL, SEUT_WRAPPING_COUNT, 1, &dir);
    report("notify rejects an unusable info count", PMIX_ERR_BAD_PARAM == rc);

    /* --- a cached event is replayed to a peer once, not per message ---
     *
     * Nothing has been notified yet, so the cache holds exactly what is
     * seeded below and the relay counts mean what they say. */
    report("the test event is parked in the notification cache",
           seed_event(SEUT_CODE));

    rc = do_register(1, codes, 0, 0, NULL);
    ok = (PMIX_SUCCESS == rc || PMIX_OPERATION_SUCCEEDED == rc);
    settle();
    report("first registration is accepted", ok);
    nqueued = queued_count();
    report("first registration replays the cached event", 1 == nqueued);

    /* the same peer registering again must not be given it a second
     * time, and must not spend the target the other proc is owed */
    rc = do_register(1, codes, 0, 0, NULL);
    ok = (PMIX_SUCCESS == rc || PMIX_OPERATION_SUCCEEDED == rc);
    settle();
    report("second registration is accepted", ok);
    nqueued = queued_count();
    report("second registration does not replay it again", 1 == nqueued);
    report("the event is still cached for the other target", cache_occupied());

    /* --- and what was replayed carries the range --------------------- */
    relay = take_queued_reply();
    ok = (NULL != relay) && relay_is_our_event(relay, &range);
    report("the replayed event is well formed", ok);
    report("the replayed event carries its range", ok && PMIX_RANGE_SESSION == range);
    if (NULL != relay) {
        PMIX_RELEASE(relay);
    }

    drain_queued();

    /* --- and a well-formed notification is still accepted ------------
     *
     * No relay to count here: the only registered peer is the server's
     * own, and the dispatch deliberately does not notify ourselves. What
     * this pins down is that the screen above did not cost the handler
     * its actual job - the request is taken and the event lands in the
     * cache alongside the seeded one. */
    drain_queued();
    rc = do_notify(SEUT_CODE, PMIX_RANGE_LOCAL, 1, 1, &dir);
    report("notify accepts a well-formed request",
           PMIX_SUCCESS == rc || PMIX_OPERATION_SUCCEEDED == rc);
    settle();
    report("the notified event is cached", cache_occupied());

    /* --- group events only a server raises ---------------------------
     *
     * A client joins, accepts or declines through the group commands and
     * its server raises the event for it; the invitation's outcome is
     * raised by the leader's server. So a client raising any of these
     * itself is refused, and the host never hears of it. The application's
     * own announcement of a new leader is not one of them. */
    {
        pmix_info_t grp;
        pmix_status_t grpcodes[] = {
            PMIX_GROUP_INVITED, PMIX_GROUP_INVITE_ACCEPTED, PMIX_GROUP_INVITE_DECLINED,
            PMIX_GROUP_INVITE_FAILED, PMIX_GROUP_CONSTRUCT_COMPLETE,
            PMIX_GROUP_CONSTRUCT_ABORT, PMIX_GROUP_LEADER_FAILED, PMIX_GROUP_MEMBER_FAILED
        };
        size_t c;
        bool allrefused = true;

        PMIX_INFO_LOAD(&grp, PMIX_GROUP_ID, "seut-grp", PMIX_STRING);
        for (c = 0; c < sizeof(grpcodes) / sizeof(grpcodes[0]); c++) {
            drain_queued();
            rc = do_notify(grpcodes[c], PMIX_RANGE_LOCAL, 1, 1, &grp);
            if (PMIX_ERR_NO_PERMISSIONS != rc) {
                fprintf(stdout, "    %s from a client was not refused: %s\n",
                        PMIx_Error_string(grpcodes[c]), PMIx_Error_string(rc));
                allrefused = false;
            }
        }
        report("a client may not raise a group event only servers raise", allrefused);
        drain_queued();
        rc = do_notify(PMIX_GROUP_LEADER_SELECTED, PMIX_RANGE_LOCAL, 1, 1, &grp);
        report("a client may still announce a new leader",
               PMIX_SUCCESS == rc || PMIX_OPERATION_SUCCEEDED == rc);
        PMIX_INFO_DESTRUCT(&grp);
        settle();
        drain_queued();
    }

    /* --- a member leaves a group only for itself ---------------------
     *
     * The stand-in sends as the server's own identity. A departure that
     * names someone else is refused - the host is not told of it - and
     * one that names the sender is taken as before. */
    {
        pmix_info_t left[2];
        pmix_proc_t other;

        PMIX_LOAD_PROCID(&other, "seut-other", 5);
        PMIX_INFO_LOAD(&left[0], PMIX_GROUP_ID, "seut-grp", PMIX_STRING);
        PMIX_INFO_LOAD(&left[1], PMIX_EVENT_AFFECTED_PROC, &other, PMIX_PROC);
        drain_queued();
        rc = do_notify(PMIX_GROUP_LEFT, PMIX_RANGE_LOCAL, 2, 2, left);
        report("a departure naming another proc is refused", PMIX_ERR_NO_PERMISSIONS == rc);
        PMIX_INFO_DESTRUCT(&left[1]);

        PMIX_INFO_LOAD(&left[1], PMIX_EVENT_AFFECTED_PROC, &pmix_globals.myid, PMIX_PROC);
        drain_queued();
        rc = do_notify(PMIX_GROUP_LEFT, PMIX_RANGE_LOCAL, 2, 2, left);
        report("a departure naming its sender is accepted",
               PMIX_SUCCESS == rc || PMIX_OPERATION_SUCCEEDED == rc);
        PMIX_INFO_DESTRUCT(&left[0]);
        PMIX_INFO_DESTRUCT(&left[1]);
        settle();
        drain_queued();
    }

    /* --- a host-completed registration acks before it replays ------- */
    drain_queued();
    report("a system-code event is parked in the cache", seed_event(SEUT_SYSCODE));

    host_regcb = NULL;
    rc = do_register_acked(1, syscodes);
    report("the host is asked to forward the system code",
           PMIX_SUCCESS == rc && NULL != host_regcb);
    report("the host is told which process registered",
           1 == host_nrequestor && host_requestor_ok);
    report("nothing is sent while the host still holds the request",
           0 == queued_count());

    /* the host now completes the registration it took */
    host_regcb(PMIX_SUCCESS, host_regcbdata);
    settle();
    report("the host-completed registration is answered and replayed",
           2 == queued_count());

    relay = take_queued_reply();
    report("the acknowledgement is sent before the replayed event",
           NULL != relay && !buffer_is_notify(relay));
    if (NULL != relay) {
        PMIX_RELEASE(relay);
    }

    drain_queued();
    test_debugger_release();
    drain_queued();
    test_event_identity();
    drain_queued();
    test_namespace_range();
    drain_queued();
    test_custom_range_count();
    drain_queued();
    test_refused_registration();
    drain_queued();
    PMIX_INFO_DESTRUCT(&dir);
    release_standin();
    PMIx_server_finalize();

    fprintf(stdout, "\nserver_events: %d passed, %d failed\n", npass, nfail);
    return (0 == nfail) ? 0 : 1;
}
