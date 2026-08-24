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

/* a code with no meaning to the library, so nothing else reacts to it,
 * and deliberately not a system event - those take the arm that calls
 * the host, and this file has no host to call */
#define SEUT_CODE   (PMIX_EXTERNAL_ERR_BASE - 71)
/* the other proc the cached event names, so the event is not spent by a
 * single delivery */
#define SEUT_OTHER  "server-events-ut-other"
#define SEUT_SOURCE "server-events-ut-source"

/* a count whose product with sizeof(pmix_info_t) wraps a 64-bit size_t */
#define SEUT_WRAPPING_COUNT ((size_t) 1 << 61)

static int npass = 0;
static int nfail = 0;

static void report(const char *what, bool ok)
{
    fprintf(stdout, "%-58s : %s\n", what, ok ? "PASS" : "FAIL");
    if (ok) {
        ++npass;
    } else {
        ++nfail;
    }
}

/* A blocking round trip through the progress thread. Both handlers
 * thread-shift the cached-event replay, so this is what makes it
 * observable without a sleep: anything queued ahead of this call has run
 * by the time it returns. */
static void progress_barrier(void)
{
    pmix_value_t v;

    PMIX_VALUE_LOAD(&v, "barrier", PMIX_STRING);
    PMIx_Store_internal(&pmix_globals.myid, "server-events-ut.barrier", &v);
    PMIX_VALUE_DESTRUCT(&v);
}

/* How many messages the server has queued to a peer that has no socket.
 * With sd < 0 the send event is never armed, so they simply come to rest
 * on the peer: the first on send_msg, the rest on send_queue. */
static size_t queued_count(void)
{
    pmix_peer_t *peer = pmix_globals.mypeer;
    size_t n = 0;

    if (NULL != peer->send_msg) {
        ++n;
    }
    n += pmix_list_get_size(&peer->send_queue);
    return n;
}

/* Take the first of those messages and hand back its buffer. */
static pmix_buffer_t *take_queued_reply(void)
{
    pmix_peer_t *peer = pmix_globals.mypeer;
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

static void drain_queued(void)
{
    pmix_peer_t *peer = pmix_globals.mypeer;
    pmix_ptl_send_t *snd;

    if (NULL != peer->send_msg) {
        PMIX_RELEASE(peer->send_msg);
        peer->send_msg = NULL;
    }
    while (NULL != (snd = (pmix_ptl_send_t *) pmix_list_remove_first(&peer->send_queue))) {
        PMIX_RELEASE(snd);
    }
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
    rc = pmix_server_register_events(pmix_globals.mypeer, buf, NULL, NULL);
    PMIX_RELEASE(buf);
    return rc;
}

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
    rc = pmix_server_event_recvd_from_client(pmix_globals.mypeer, buf, NULL, NULL);
    PMIX_RELEASE(buf);
    return rc;
}

/* Park an event in the notification cache, naming our own peer and one
 * absent proc as its targets, so a single delivery does not spend it.
 * Runs on the progress thread - the hotel belongs to it. */
static void seed_cached_event(int sd, short args, void *cbdata)
{
    pmix_lock_t *lock = (pmix_lock_t *) cbdata;
    pmix_notify_caddy_t *cd;

    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    cd = PMIX_NEW(pmix_notify_caddy_t);
    cd->status = SEUT_CODE;
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

int main(int argc, char **argv)
{
    static pmix_server_module_t mymodule = {0};
    pmix_status_t rc, codes[1];
    pmix_info_t dir;
    pmix_lock_t lock;
    pmix_event_t ev;
    pmix_buffer_t *relay;
    pmix_data_range_t range = PMIX_RANGE_UNDEF;
    size_t nqueued;
    bool ok;

    (void) argc;
    (void) argv;

    fprintf(stdout, "server_events: server-side event registration and notification\n");

    rc = PMIx_server_init(&mymodule, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "PMIx_server_init failed: %s\n", PMIx_Error_string(rc));
        return 1;
    }

    codes[0] = SEUT_CODE;
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
    PMIX_CONSTRUCT_LOCK(&lock);
    pmix_event_assign(&ev, pmix_globals.evbase, -1, EV_WRITE, seed_cached_event, &lock);
    pmix_event_active(&ev, EV_WRITE, 1);
    PMIX_WAIT_THREAD(&lock);
    ok = (PMIX_SUCCESS == lock.status);
    PMIX_DESTRUCT_LOCK(&lock);
    report("the test event is parked in the notification cache", ok);

    rc = do_register(1, codes, 0, 0, NULL);
    ok = (PMIX_SUCCESS == rc || PMIX_OPERATION_SUCCEEDED == rc);
    progress_barrier();
    report("first registration is accepted", ok);
    nqueued = queued_count();
    report("first registration replays the cached event", 1 == nqueued);

    /* the same peer registering again must not be given it a second
     * time, and must not spend the target the other proc is owed */
    rc = do_register(1, codes, 0, 0, NULL);
    ok = (PMIX_SUCCESS == rc || PMIX_OPERATION_SUCCEEDED == rc);
    progress_barrier();
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
    progress_barrier();
    report("the notified event is cached", cache_occupied());

    drain_queued();
    PMIX_INFO_DESTRUCT(&dir);
    PMIx_server_finalize();

    fprintf(stdout, "\nserver_events: %d passed, %d failed\n", npass, nfail);
    return (0 == nfail) ? 0 : 1;
}
