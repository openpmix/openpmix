/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * White-box unit tests for pmix_server_connect() and
 * pmix_server_disconnect(), driven exactly as the switchyard drives them
 * - a wire buffer packed by hand and a peer - so no DVM, no client, and
 * no second process is involved.
 *
 * These cover the malformed-count screens, which both handlers share with
 * pmix_server_fence(): see the header of test/unit/server_fence.c for why
 * this family needs them when most of src/server does not. In short, the
 * info count is used to *index* the array before anything has been
 * unpacked into it - two slots are seeded at info[ninf] and info[ninf+1],
 * so a count near SIZE_MAX wraps "ninf + 2" down to a one-element array
 * and writes the seeds far outside it - and the proc count is multiplied
 * by sizeof(pmix_proc_t) to size the proc array, with the size_t rather
 * than the int32_t the unpack consumed driving the qsort and the free
 * afterwards.
 *
 * The info-count cases *crash* an unfixed library rather than failing it,
 * the same bargain test/unit/server_fence.c and test/unit/iof_output.c
 * make.
 *
 * The well-formed case asserts only that the request is not rejected as a
 * bad parameter: a screen that turned every connect into PMIX_ERR_BAD_PARAM
 * would satisfy every case above it, and that is what this guards against.
 * What the handler then does with a well-formed connect depends on the
 * host module and belongs to the multi-node suite.
 */

#include "src/include/pmix_config.h"

#include "include/pmix.h"
#include "include/pmix_server.h"

#include "src/class/pmix_list.h"
#include "src/include/pmix_globals.h"
#include "src/mca/bfrops/bfrops.h"
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

static void op_stub(pmix_status_t status, void *cbdata)
{
    (void) status;
    (void) cbdata;
}

/* A host that ACCEPTS the connect, which is the only way to reach the
 * state where the host owns a tracker that is still on the collectives
 * list: a host that refuses - or an absent entry point - has the handler
 * tear the tracker down before it returns. Records the cbdata, which is
 * the tracker itself, so the test can resolve it afterwards. */
static int connect_calls = 0;
static void *connect_cbdata = NULL;

static pmix_status_t stub_connect(const pmix_proc_t procs[], size_t nprocs,
                                  const pmix_info_t info[], size_t ninfo,
                                  pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    (void) procs;
    (void) nprocs;
    (void) info;
    (void) ninfo;
    (void) cbfunc;

    ++connect_calls;
    connect_cbdata = cbdata;
    return PMIX_SUCCESS;
}

/* carrier for driving the handlers on the progress thread - they touch
 * pmix_server_globals.collectives, so they must not be called from
 * main(). The pmix_event_t is the thread-shift member required of every
 * caddy. */
typedef struct {
    pmix_event_t ev;
    pmix_lock_t lock;
    bool disconnect;
    size_t nprocs;      /* the count to put on the wire */
    size_t nprocs_real; /* how many procs actually to pack */
    size_t ninf;        /* the count to put on the wire */
    size_t ninf_real;   /* how many info structs actually to pack */
    /* append a trailing object that is not a well-formed pmix_info_t, so
     * the connect handler's optional endpoint/job-info unpacks fail with
     * something other than "read past end of buffer" */
    bool bad_trailer;
    /* Pack the proc array in ONE call, the way PMIx_Connect does, rather
     * than one element at a time. The per-element form is what lets a
     * case put a count on the wire that disagrees with the payload, but
     * it is NOT the real wire layout: the unpacker reads the count that
     * introduces an array, so a handler asking for N finds only the first
     * element's run of 1 and leaves the rest in the buffer. Any case that
     * wants a well-formed multi-proc request has to use this. */
    bool array_procs;
    /* drive the handler with the completion the switchyard really passes,
     * rather than the inert stub - the teardown of a failed collective
     * runs through that callback, so a stub cannot observe it */
    bool real_cb;
    pmix_status_t status;
    size_t ncollectives;  /* trackers left on the list afterwards */
} cut_req_t;

static void do_op(int sd, short args, void *cbdata)
{
    cut_req_t *r = (cut_req_t *) cbdata;
    pmix_buffer_t buf;
    pmix_server_caddy_t *cd;
    pmix_proc_t proc;
    pmix_info_t dir;
    pmix_status_t rc;
    size_t n;
    bool flag = true;

    (void) sd;
    (void) args;

    PMIX_CONSTRUCT(&buf, pmix_buffer_t);
    PMIX_BFROPS_ASSIGN_TYPE(pmix_globals.mypeer, &buf);

    /* the proc count, then the procs - the two may disagree on purpose */
    PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, &buf, &r->nprocs, 1, PMIX_SIZE);
    if (PMIX_SUCCESS == rc && r->array_procs && 0 < r->nprocs_real) {
        pmix_proc_t *parray;
        PMIX_PROC_CREATE(parray, r->nprocs_real);
        if (NULL == parray) {
            PMIX_DESTRUCT(&buf);
            r->status = PMIX_ERR_NOMEM;
            PMIX_WAKEUP_THREAD(&r->lock);
            return;
        }
        for (n = 0; n < r->nprocs_real; n++) {
            PMIX_LOAD_PROCID(&parray[n], "connect.ut.ns", (pmix_rank_t) n);
        }
        PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, &buf, parray,
                         (int32_t) r->nprocs_real, PMIX_PROC);
        PMIX_PROC_FREE(parray, r->nprocs_real);
    } else {
        for (n = 0; PMIX_SUCCESS == rc && n < r->nprocs_real; n++) {
            /* a namespace this server has never been told about, so the
             * tracker comes out non-local and definition-complete */
            PMIX_LOAD_PROCID(&proc, "connect.ut.ns", (pmix_rank_t) n);
            PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, &buf, &proc, 1, PMIX_PROC);
        }
    }
    /* the info count, then the info structs */
    if (PMIX_SUCCESS == rc) {
        PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, &buf, &r->ninf, 1, PMIX_SIZE);
    }
    for (n = 0; PMIX_SUCCESS == rc && n < r->ninf_real; n++) {
        PMIX_INFO_LOAD(&dir, PMIX_EMBED_BARRIER, &flag, PMIX_BOOL);
        PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, &buf, &dir, 1, PMIX_INFO);
        PMIX_INFO_DESTRUCT(&dir);
    }
    if (PMIX_SUCCESS == rc && r->bad_trailer) {
        /* A pmix_info_t goes onto the wire as key(string), flags, then a
         * value that leads with its own data type. The trailer has to
         * fail with a REAL error rather than the end-of-buffer the
         * handler reads as "no trailing info was sent", so lay down a
         * NULL key: the packer stores a zero length for it, the unpacker
         * hands it straight back as NULL, and pmix_bfrops_base_unpack_info
         * answers PMIX_ERROR.
         *
         * The earlier version of this wrote a well-formed key and flags
         * followed by an unrecognized type value - which does not work,
         * because in a fully-described buffer that value carries its own
         * type header and unpacks cleanly as a uint16. It only ever
         * "failed" because the procs above were packed one at a time, so
         * the handler left most of the array in the buffer and the drain
         * loop was reading proc bytes as directives. Anything relying on
         * that layout is testing the harness, not the handler. */
        char *k = NULL;

        PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, &buf, &k, 1, PMIX_STRING);
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

    if (r->disconnect) {
        rc = pmix_server_disconnect(cd, &buf,
                                    r->real_cb ? pmix_server_discnct_cbfunc : op_stub);
    } else {
        rc = pmix_server_connect(cd, &buf,
                                 r->real_cb ? pmix_server_cnct_cbfunc : op_stub);
    }
    if (PMIX_SUCCESS != rc) {
        /* the switchyard owns the caddy on a non-success return */
        PMIX_RELEASE(cd);
    }
    PMIX_DESTRUCT(&buf);
    r->status = rc;
    r->ncollectives = pmix_list_get_size(&pmix_server_globals.collectives);
    PMIX_WAKEUP_THREAD(&r->lock);
}

static size_t last_ncollectives = 0;

static pmix_status_t drive_full(bool disconnect, size_t nprocs, size_t nprocs_real,
                                size_t ninf, size_t ninf_real, bool bad_trailer,
                                bool real_cb, bool array_procs)
{
    cut_req_t req;
    pmix_status_t rc;

    memset(&req, 0, sizeof(req));
    PMIX_CONSTRUCT_LOCK(&req.lock);
    req.disconnect = disconnect;
    req.nprocs = nprocs;
    req.nprocs_real = nprocs_real;
    req.ninf = ninf;
    req.ninf_real = ninf_real;
    req.bad_trailer = bad_trailer;
    req.real_cb = real_cb;
    req.array_procs = array_procs;
    PMIX_THREADSHIFT(&req, do_op);
    PMIX_WAIT_THREAD(&req.lock);
    rc = req.status;
    last_ncollectives = req.ncollectives;
    PMIX_DESTRUCT_LOCK(&req.lock);
    return rc;
}

static pmix_status_t drive(bool disconnect, size_t nprocs, size_t nprocs_real,
                           size_t ninf, size_t ninf_real)
{
    return drive_full(disconnect, nprocs, nprocs_real, ninf, ninf_real, false, false, true);
}

/* the completions thread-shift, so let anything queued behind us run
 * before we finalize. PMIx_Store_internal blocks on the progress thread,
 * so anything queued ahead of it has run by the time it returns. */
static void progress_barrier(void)
{
    pmix_value_t v;

    PMIX_VALUE_LOAD(&v, "barrier", PMIX_STRING);
    PMIx_Store_internal(&pmix_globals.myid, "server-connect-ut.barrier", &v);
    PMIX_VALUE_DESTRUCT(&v);
}

int main(int argc, char **argv)
{
    static pmix_server_module_t mymodule = {0};
    pmix_status_t rc;
    int i;

    (void) argc;
    (void) argv;

    fprintf(stdout, "server_connect: connect/disconnect handler unit tests\n");

    mymodule.connect = stub_connect;

    rc = PMIx_server_init(&mymodule, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "PMIx_server_init failed: %s\n", PMIx_Error_string(rc));
        return 1;
    }

    /* both handlers carry the same two screens, so run the same cases
     * against each rather than trusting them to stay in step */
    for (i = 0; i < 2; i++) {
        bool dis = (1 == i);
        const char *who = dis ? "disconnect" : "connect";
        char name[128];

        /* SIZE_MAX wraps "ninf + 2" to 1: the array is allocated with
         * room for one element and the seeds are then written at info[0]
         * and info[SIZE_MAX]. Against an unfixed library this is where
         * the program dies. */
        rc = drive(dis, 1, 1, SIZE_MAX, 0);
        snprintf(name, sizeof(name), "%s rejects an info count of SIZE_MAX", who);
        report(name, PMIX_ERR_BAD_PARAM == rc);

        rc = drive(dis, 1, 1, (size_t) 0x80000000ULL, 0);
        snprintf(name, sizeof(name), "%s rejects an info count that truncates negative", who);
        report(name, PMIX_ERR_BAD_PARAM == rc);

        rc = drive(dis, (size_t) 1 << 40, 1, 0, 0);
        snprintf(name, sizeof(name), "%s rejects an oversized proc count", who);
        report(name, PMIX_ERR_BAD_PARAM == rc);

        rc = drive(dis, 0, 0, 0, 0);
        snprintf(name, sizeof(name), "%s rejects a zero proc count", who);
        report(name, PMIX_ERR_BAD_PARAM == rc);

        /* the screens must not simply reject everything */
        rc = drive(dis, 1, 1, 1, 1);
        snprintf(name, sizeof(name), "%s accepts a well-formed request", who);
        report(name, PMIX_ERR_BAD_PARAM != rc);

        progress_barrier();
    }

    /* A connect whose optional trailing info is malformed fails after the
     * tracker has been created but before this caddy has joined it. The
     * handler owes the collective a completion on that arm: without one
     * the tracker sits on pmix_server_globals.collectives for the life of
     * the server, and any participant that contributed ahead of this one
     * waits in PMIx_Connect forever. Drive it with the completion the
     * switchyard really passes - an inert stub cannot tear a tracker
     * down, so it cannot tell the two behaviors apart - and hold the
     * collectives list against itself across the call. Against an
     * unfixed library the list grows by one and stays that way. */
    {
        pmix_status_t st;
        size_t before, after;

        progress_barrier();
        before = pmix_list_get_size(&pmix_server_globals.collectives);
        st = drive_full(false, 3, 3, 1, 1, true, true, true);
        progress_barrier();
        after = pmix_list_get_size(&pmix_server_globals.collectives);

        report("a malformed trailing info is rejected",
               PMIX_SUCCESS != st);
        report("a connect that fails past tracker creation strands nothing",
               after == before);
    }

    /* --- a tracker the host already owns is not modified, and not
     *     completed underneath it ---
     *
     * pmix_server_get_tracker matches on the participant set, so a second
     * contribution over the same set while the host still owns the
     * tracker - a fork/exec'd clone of a rank that already called in -
     * is handed that in-flight tracker. Everything between the lookup and
     * the local_cbs append then modifies it: add_trk_info frees the very
     * info array this server passed to pmix_host_server.connect, and a
     * failure in there jumps to trkerr, which clears host_called and
     * drives the completion - replying to every participant, unlinking
     * the tracker and releasing it while the host still holds it as
     * cbdata. The host's callback then runs on freed memory.
     *
     * Drive it with a malformed trailing info, which is what makes the
     * second contribution take that failure path, and hold the
     * collectives list against itself: against an unfixed library the
     * in-flight tracker is torn down and the count drops. A distinct proc
     * count keeps this off the trackers the cases above left behind. */
    {
        pmix_status_t st;
        size_t before, after;
        int calls_before;

        progress_barrier();
        connect_calls = 0;
        connect_cbdata = NULL;

        /* first contribution: the namespace is unknown to this server, so
         * the tracker is definition-complete on arrival and goes straight
         * to the host, which accepts and says nothing further */
        st = drive_full(false, 5, 5, 1, 1, false, true, true);
        progress_barrier();
        report("a connect the host accepts reaches it once",
               PMIX_SUCCESS == st && 1 == connect_calls && NULL != connect_cbdata);

        calls_before = connect_calls;
        before = pmix_list_get_size(&pmix_server_globals.collectives);
        st = drive_full(false, 5, 5, 1, 1, true, true, true);
        progress_barrier();
        after = pmix_list_get_size(&pmix_server_globals.collectives);

        report("a late contribution does not tear down the host's tracker",
               after == before);
        report("the late contribution is accepted and parked",
               PMIX_SUCCESS == st);
        report("the host is not called a second time",
               connect_calls == calls_before);

        /* resolve it once, which is all the host was ever asked for */
        if (NULL != connect_cbdata) {
            pmix_server_cnct_cbfunc(PMIX_SUCCESS, connect_cbdata);
            progress_barrier();
        }
    }

    fprintf(stdout, "\nResults: %d passed, %d failed\n\n", npass, nfail);

    PMIx_server_finalize();

    return (nfail > 0) ? 1 : 0;
}
