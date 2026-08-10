/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * White-box unit tests for pmix_server_fence(), driven exactly as the
 * switchyard drives it - a wire buffer packed by hand and a peer - so no
 * DVM, no client, and no second process is involved.
 *
 * What is pinned down here:
 *
 *   the counts that arrive off the wire
 *      A fence message carries a proc count and an info count, both as a
 *      size_t, and both are then consumed through the int32_t that
 *      PMIX_BFROPS_UNPACK takes. Most handlers in src/server survive a
 *      count that does not fit, because their only use of it is to size
 *      an allocation the unpack immediately guards - and the unpack
 *      screens a NULL destination. This handler is not that, twice over:
 *
 *        - the info count is used to *index* the array before anything
 *          has been unpacked into it. Two slots are seeded, at info[ninf]
 *          and info[ninf+1], so a count near SIZE_MAX wraps "ninf + 2"
 *          down to a one-element array and writes the seeds far outside
 *          it. That is an out-of-bounds write driven straight off the
 *          wire by a local client.
 *        - the proc count is multiplied by sizeof(pmix_proc_t) to size
 *          the proc array, and it is that size_t - not the int32_t the
 *          unpack consumed - which the qsort and the free below walk.
 *
 *      Both must now be rejected before they reach an allocator. The
 *      info case *crashes* an unfixed library rather than failing, the
 *      same bargain test/unit/server_control.c and test/unit/iof_output.c
 *      make; the proc case answers PMIX_ERR_NOMEM there instead, so it
 *      fails rather than crashes.
 *
 *   the screen must not cost the handler its job
 *      A well-formed fence still has to build its tracker and reach the
 *      host with the seeded slots intact and in order: the caller's
 *      directives first, then PMIX_SORTED_PROC_ARRAY, then
 *      PMIX_LOCAL_COLLECTIVE_STATUS last. pmix_server_fence reads that
 *      last slot *positionally* - trk->info[trk->ninfo-1] - on two of its
 *      completion arms, so the layout is load-bearing rather than
 *      incidental, and a suite made only of malformed-count cases would
 *      not notice a screen that rejected everything.
 *
 *   the completion answers every participant, with the right status
 *      pmix_server_modex_cbfunc is the other half of this handler: it is
 *      what the host calls when the fence resolves, and it owes a reply
 *      to each caddy the tracker collected. Two things about that loop
 *      are invisible with a single participant, which is all the cases
 *      above build:
 *
 *        - the status packed for each client is the collective's own,
 *          held in a local across the whole loop, and
 *          PMIX_GDS_MARK_MODEX_COMPLETE *assigns* the variable it is
 *          given. Writing it into that local reported a failed fence as
 *          PMIX_SUCCESS to every participant after the first - and the
 *          gds/hash entry point is a no-op returning PMIX_SUCCESS, so it
 *          did so on every ordinary server.
 *        - a failure serving one participant used to leave the loop, and
 *          the tracker release at the foot of the function then frees the
 *          remaining caddies without a reply. Nothing else ever answers
 *          them, so those clients sit in PMIx_Fence forever.
 *
 *      The case below drives the completion directly with a failing
 *      status over a two-participant tracker and reads back what was
 *      actually queued for each. Queueing works here because a peer
 *      whose sd is negative never has its send event armed, so the
 *      messages come to rest on send_msg and send_queue - the same idiom
 *      test/unit/server_control.c uses to inspect a reply.
 */

#include "src/include/pmix_config.h"

#include "include/pmix.h"
#include "include/pmix_server.h"

#include "src/class/pmix_list.h"
#include "src/include/pmix_globals.h"
#include "src/mca/bfrops/bfrops.h"
#include "src/mca/ptl/ptl_types.h"
#include "src/server/pmix_server_ops.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

/* what the host's fence_nb entry point was handed */
static bool fence_fired = false;
static size_t fence_nprocs = 0;
static size_t fence_ninfo = 0;
static bool fence_status_last = false;
static bool fence_sorted_next_to_last = false;

static pmix_status_t stub_fence_nb(const pmix_proc_t procs[], size_t nprocs,
                                   const pmix_info_t info[], size_t ninfo,
                                   char *data, size_t ndata,
                                   pmix_modex_cbfunc_t cbfunc, void *cbdata)
{
    (void) procs;
    (void) data;
    (void) ndata;
    (void) cbfunc;
    (void) cbdata;

    fence_fired = true;
    fence_nprocs = nprocs;
    fence_ninfo = ninfo;
    fence_status_last = false;
    fence_sorted_next_to_last = false;
    if (NULL != info && 2 <= ninfo) {
        fence_status_last = PMIX_CHECK_KEY(&info[ninfo - 1], PMIX_LOCAL_COLLECTIVE_STATUS);
        fence_sorted_next_to_last = PMIX_CHECK_KEY(&info[ninfo - 2], PMIX_SORTED_PROC_ARRAY);
    }
    /* decline, so the handler takes its host-error arm: it detaches our
     * caddy from the tracker and drives the completion, which tears the
     * tracker down. Accepting would instead leave the completion to us,
     * and queueing a reply to a peer with no socket is not something this
     * program can do. */
    return PMIX_ERR_NOT_SUPPORTED;
}

/* carrier for driving pmix_server_fence on the progress thread - it
 * touches pmix_server_globals.collectives, so it must not be called from
 * main(). The pmix_event_t is the thread-shift member required of every
 * caddy. */
typedef struct {
    pmix_event_t ev;
    pmix_lock_t lock;
    size_t nprocs;      /* the count to put on the wire */
    size_t nprocs_real; /* how many procs actually to pack */
    size_t ninf;        /* the count to put on the wire */
    size_t ninf_real;   /* how many info structs actually to pack */
    pmix_status_t status;
} fut_req_t;

static void do_fence(int sd, short args, void *cbdata)
{
    fut_req_t *r = (fut_req_t *) cbdata;
    pmix_buffer_t buf;
    pmix_server_caddy_t *cd;
    pmix_proc_t proc;
    pmix_info_t dir;
    pmix_status_t rc;
    size_t n;
    bool collect = false;

    (void) sd;
    (void) args;

    PMIX_CONSTRUCT(&buf, pmix_buffer_t);
    PMIX_BFROPS_ASSIGN_TYPE(pmix_globals.mypeer, &buf);

    /* the proc count, then the procs. The two may disagree on purpose:
     * that is the whole point of the malformed-count cases. */
    PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, &buf, &r->nprocs, 1, PMIX_SIZE);
    for (n = 0; PMIX_SUCCESS == rc && n < r->nprocs_real; n++) {
        /* a namespace this server has never been told about: new_tracker
         * then marks the tracker non-local and definition-complete, so a
         * well-formed request completes on this one contribution */
        PMIX_LOAD_PROCID(&proc, "fence.ut.ns", (pmix_rank_t) n);
        PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, &buf, &proc, 1, PMIX_PROC);
    }
    /* the info count, then the info structs */
    if (PMIX_SUCCESS == rc) {
        PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, &buf, &r->ninf, 1, PMIX_SIZE);
    }
    for (n = 0; PMIX_SUCCESS == rc && n < r->ninf_real; n++) {
        PMIX_INFO_LOAD(&dir, PMIX_COLLECT_DATA, &collect, PMIX_BOOL);
        PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, &buf, &dir, 1, PMIX_INFO);
        PMIX_INFO_DESTRUCT(&dir);
    }
    if (PMIX_SUCCESS != rc) {
        PMIX_DESTRUCT(&buf);
        r->status = rc;
        PMIX_WAKEUP_THREAD(&r->lock);
        return;
    }

    cd = PMIX_NEW(pmix_server_caddy_t);
    if (NULL == cd) {
        PMIX_DESTRUCT(&buf);
        r->status = PMIX_ERR_NOMEM;
        PMIX_WAKEUP_THREAD(&r->lock);
        return;
    }
    PMIX_RETAIN(pmix_globals.mypeer);
    cd->peer = pmix_globals.mypeer;
    cd->hdr.tag = 0;

    rc = pmix_server_fence(cd, &buf, pmix_server_modex_cbfunc);
    if (PMIX_SUCCESS != rc) {
        /* the switchyard owns the caddy on a non-success return */
        PMIX_RELEASE(cd);
    }
    PMIX_DESTRUCT(&buf);
    r->status = rc;
    PMIX_WAKEUP_THREAD(&r->lock);
}

static pmix_status_t drive_fence(size_t nprocs, size_t nprocs_real,
                                 size_t ninf, size_t ninf_real)
{
    fut_req_t req;
    pmix_status_t rc;

    memset(&req, 0, sizeof(req));
    PMIX_CONSTRUCT_LOCK(&req.lock);
    req.nprocs = nprocs;
    req.nprocs_real = nprocs_real;
    req.ninf = ninf;
    req.ninf_real = ninf_real;
    PMIX_THREADSHIFT(&req, do_fence);
    PMIX_WAIT_THREAD(&req.lock);
    rc = req.status;
    PMIX_DESTRUCT_LOCK(&req.lock);
    return rc;
}

/* --- driving pmix_server_modex_cbfunc over a two-participant tracker ---
 *
 * The tracker and its caddies touch pmix_server_globals.collectives, so
 * they are built on the progress thread like everything else here. The
 * completion itself is called from main(), which is exactly how a host
 * calls it - it does nothing but thread-shift. */
typedef struct {
    pmix_event_t ev;
    pmix_lock_t lock;
    size_t nparticipants;
    pmix_server_trkr_t *trk;
} trkbuild_t;

static void build_tracker(int sd, short args, void *cbdata)
{
    trkbuild_t *b = (trkbuild_t *) cbdata;
    pmix_server_trkr_t *trk;
    pmix_server_caddy_t *cd;
    size_t n;

    (void) sd;
    (void) args;

    trk = PMIX_NEW(pmix_server_trkr_t);
    if (NULL == trk) {
        b->trk = NULL;
        PMIX_WAKEUP_THREAD(&b->lock);
        return;
    }
    trk->type = PMIX_FENCENB_CMD;
    trk->collect_type = PMIX_COLLECT_NO;
    trk->nlocal = (uint32_t) b->nparticipants;
    trk->def_complete = true;
    trk->host_called = true;
    for (n = 0; n < b->nparticipants; n++) {
        cd = PMIX_NEW(pmix_server_caddy_t);
        PMIX_RETAIN(pmix_globals.mypeer);
        cd->peer = pmix_globals.mypeer;
        cd->hdr.tag = (uint32_t) (100 + n);
        pmix_list_append(&trk->local_cbs, &cd->super);
    }
    /* the completion unlinks it from here, so it has to be on the list */
    pmix_list_append(&pmix_server_globals.collectives, &trk->super);
    b->trk = trk;
    PMIX_WAKEUP_THREAD(&b->lock);
}

/* Take the next reply the server queued for our own peer. With no socket
 * the send event is never armed, so the first lands on send_msg and the
 * rest accumulate on send_queue. */
static pmix_buffer_t *take_queued_reply(void)
{
    pmix_peer_t *peer = pmix_globals.mypeer;
    pmix_ptl_send_t *snd = peer->send_msg;
    pmix_buffer_t *buf;

    if (NULL != snd) {
        peer->send_msg = NULL;
    } else {
        snd = (pmix_ptl_send_t *) pmix_list_remove_first(&peer->send_queue);
        if (NULL == snd) {
            return NULL;
        }
    }
    buf = snd->data;
    /* the buffer is ours now - the send's destructor would release it */
    snd->data = NULL;
    PMIX_RELEASE(snd);
    return buf;
}

/* unpack the status a queued reply leads with; PMIX_ERR_NOT_FOUND if
 * there was no reply at all */
static pmix_status_t next_reply_status(void)
{
    pmix_buffer_t *reply;
    pmix_status_t st, rc;
    int32_t cnt = 1;

    reply = take_queued_reply();
    if (NULL == reply) {
        return PMIX_ERR_NOT_FOUND;
    }
    PMIX_BFROPS_UNPACK(rc, pmix_globals.mypeer, reply, &st, &cnt, PMIX_STATUS);
    PMIX_RELEASE(reply);
    if (PMIX_SUCCESS != rc) {
        return rc;
    }
    return st;
}

/* The fence completion thread-shifts, so let anything queued behind us
 * run before we look at the results or finalize. PMIx_Store_internal
 * blocks on the progress thread, so anything queued ahead of it has run
 * by the time it returns. */
static void progress_barrier(void)
{
    pmix_value_t v;

    PMIX_VALUE_LOAD(&v, "barrier", PMIX_STRING);
    PMIx_Store_internal(&pmix_globals.myid, "server-fence-ut.barrier", &v);
    PMIX_VALUE_DESTRUCT(&v);
}

int main(int argc, char **argv)
{
    static pmix_server_module_t mymodule = {0};
    pmix_status_t rc;

    (void) argc;
    (void) argv;

    fprintf(stdout, "server_fence: server-side fence handler unit tests\n");

    mymodule.fence_nb = stub_fence_nb;

    rc = PMIx_server_init(&mymodule, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "PMIx_server_init failed: %s\n", PMIx_Error_string(rc));
        return 1;
    }

    /* --- an info count that cannot survive the round trip --- *
     * SIZE_MAX wraps "ninf + 2" to 1: the array is allocated with room
     * for one element and the seeds are then written at info[0] and
     * info[SIZE_MAX]. Against an unfixed library this is where the
     * program dies. */
    rc = drive_fence(1, 1, SIZE_MAX, 0);
    report("fence rejects an info count of SIZE_MAX", PMIX_ERR_BAD_PARAM == rc);

    /* the same class one step away from the wrap: a count that truncates
     * to a negative int32_t, which is what the sibling handlers screen */
    rc = drive_fence(1, 1, (size_t) 0x80000000ULL, 0);
    report("fence rejects an info count that truncates negative",
           PMIX_ERR_BAD_PARAM == rc);

    /* --- a proc count that cannot survive the round trip --- */
    rc = drive_fence((size_t) 1 << 40, 1, 0, 0);
    report("fence rejects an oversized proc count", PMIX_ERR_BAD_PARAM == rc);

    /* zero procs was always refused - the client has to name at least
     * its own namespace */
    rc = drive_fence(0, 0, 0, 0);
    report("fence rejects a zero proc count", PMIX_ERR_BAD_PARAM == rc);

    progress_barrier();

    /* --- a well-formed fence still reaches the host --- *
     * one proc, one directive. The handler seeds two more slots, and the
     * order of those two is what its own completion arms read back
     * positionally. */
    fence_fired = false;
    rc = drive_fence(1, 1, 1, 1);
    progress_barrier();
    report("well-formed fence reaches the host", fence_fired);
    report("host is handed the caller's procs", 1 == fence_nprocs);
    report("host is handed the directives plus two seeded slots", 3 == fence_ninfo);
    report("PMIX_LOCAL_COLLECTIVE_STATUS is the last slot", fence_status_last);
    report("PMIX_SORTED_PROC_ARRAY sits just ahead of it", fence_sorted_next_to_last);
    /* the stub declined, so the handler must have handed the caddy back
     * to the switchyard rather than keeping it */
    report("a declining host leaves the caddy to the switchyard",
           PMIX_ERR_NOT_SUPPORTED == rc);

    /* nothing may be left parked on the collectives list */
    report("no tracker is left behind",
           0 == pmix_list_get_size(&pmix_server_globals.collectives));

    /* --- the completion answers every participant with the collective's
     *     own status --- *
     * Two participants, and a fence the host failed. Against an unfixed
     * library the first reply carries PMIX_ERR_TIMEOUT and the second
     * carries PMIX_SUCCESS, because PMIX_GDS_MARK_MODEX_COMPLETE
     * overwrote the status local after the first was packed. */
    {
        trkbuild_t b;
        pmix_status_t st1, st2;

        memset(&b, 0, sizeof(b));
        PMIX_CONSTRUCT_LOCK(&b.lock);
        b.nparticipants = 2;
        PMIX_THREADSHIFT(&b, build_tracker);
        PMIX_WAIT_THREAD(&b.lock);
        PMIX_DESTRUCT_LOCK(&b.lock);

        report("a two-participant tracker was built", NULL != b.trk);
        if (NULL != b.trk) {
            /* drain anything an earlier case may have left queued */
            while (NULL != take_queued_reply()) {
                continue;
            }
            pmix_server_modex_cbfunc(PMIX_ERR_TIMEOUT, NULL, 0, b.trk, NULL, NULL);
            progress_barrier();

            st1 = next_reply_status();
            st2 = next_reply_status();
            report("the first participant is told the fence failed",
                   PMIX_ERR_TIMEOUT == st1);
            report("the second participant is told the same thing",
                   PMIX_ERR_TIMEOUT == st2);
            report("the completion leaves no tracker behind",
                   0 == pmix_list_get_size(&pmix_server_globals.collectives));
        }
    }

    fprintf(stdout, "\nResults: %d passed, %d failed\n\n", npass, nfail);

    PMIx_server_finalize();

    return (nfail > 0) ? 1 : 0;
}
