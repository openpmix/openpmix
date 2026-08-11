/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * White-box unit tests for pmix_server_trk_peer_lost().
 *
 * This is the fence/connect/disconnect family's lost-connection accounting:
 * the server calls it when a client's socket drops, and it decides, for every
 * in-flight tracker the departing rank participates in, whether that rank is
 * still being waited on. The rules it must implement (see
 * docs/how-things-work/collectives):
 *
 *   Case A   the rank had already contributed -> the loss is ignored, the
 *            contribution and its data stand.
 *   Case B   the rank had not contributed and never will -> it is recorded on
 *            'departed' so the collective can complete on the survivors, and
 *            the collective's status is degraded.
 *   Neither  the peer merely called PMIx_Finalize. That is not a loss: the
 *            rank stays registered and counted (nlocalprocs is deliberately
 *            left alone for it), so it remains an expected participant and may
 *            PMIx_Init again and contribute. Recording it as departed counts
 *            the rank twice and lets the collective complete without it -
 *            which is what desynchronized the fence sequence of a client
 *            cycling init/finalize (issue #4113).
 *
 * The last of those is the one with teeth, so it is tested as a pair: the same
 * tracker, one contribution short of complete, and a departing peer that
 * differs only in whether it finalized. Abnormal loss must complete the
 * collective; a finalize must leave it waiting.
 */

#include "src/include/pmix_config.h"

#include "include/pmix.h"
#include "src/include/pmix_globals.h"
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

#define TEST_NSPACE "trkloss.ns"

/* records that the collective's completion function was driven, without
 * touching the tracker - the test owns its lifetime */
static int completions = 0;
static pmix_status_t completion_status = PMIX_SUCCESS;

static void fake_modexcbfunc(pmix_status_t status, const char *data, size_t ndata,
                             void *cbdata, pmix_release_cbfunc_t relfn, void *relcbd)
{
    PMIX_HIDE_UNUSED_PARAMS(data, ndata, cbdata, relfn, relcbd);
    ++completions;
    completion_status = status;
}

/* a peer carrying just the identity and the finalized flag that the loss
 * accounting reads */
static pmix_peer_t *make_peer(pmix_rank_t rank, bool finalized)
{
    pmix_peer_t *peer;

    peer = PMIX_NEW(pmix_peer_t);
    peer->info = PMIX_NEW(pmix_rank_info_t);
    peer->info->pname.nspace = strdup(TEST_NSPACE);
    peer->info->pname.rank = rank;
    peer->finalized = finalized;
    return peer;
}

/* a local fence tracker over ranks 0..nlocal-1 of TEST_NSPACE, carrying the
 * status slot the participant handlers seed */
static pmix_server_trkr_t *make_trk(uint32_t nlocal)
{
    pmix_server_trkr_t *trk;
    pmix_status_t seed = PMIX_SUCCESS;
    uint32_t n;

    trk = PMIX_NEW(pmix_server_trkr_t);
    trk->type = PMIX_FENCENB_CMD;
    trk->def_complete = true;
    trk->local = true;
    trk->nlocal = nlocal;
    trk->modexcbfunc = fake_modexcbfunc;
    PMIX_PROC_CREATE(trk->pcs, nlocal);
    trk->npcs = nlocal;
    for (n = 0; n < nlocal; n++) {
        PMIX_LOAD_PROCID(&trk->pcs[n], TEST_NSPACE, n);
    }
    PMIX_INFO_CREATE(trk->info, 1);
    trk->ninfo = 1;
    PMIX_INFO_LOAD(&trk->info[0], PMIX_LOCAL_COLLECTIVE_STATUS, &seed, PMIX_STATUS);
    pmix_list_append(&pmix_server_globals.collectives, &trk->super);
    return trk;
}

/* record a contribution from the given rank */
static void contribute(pmix_server_trkr_t *trk, pmix_rank_t rank)
{
    pmix_server_caddy_t *cd;

    cd = PMIX_NEW(pmix_server_caddy_t);
    cd->peer = make_peer(rank, false);
    pmix_list_append(&trk->local_cbs, &cd->super);
}

static void release_trk(pmix_server_trkr_t *trk)
{
    pmix_list_remove_item(&pmix_server_globals.collectives, &trk->super);
    PMIX_RELEASE(trk);
}

int main(int argc, char **argv)
{
    pmix_status_t rc;
    static pmix_server_module_t mymodule = {0};
    pmix_server_trkr_t *trk;
    pmix_peer_t *peer;
    PMIX_HIDE_UNUSED_PARAMS(argc, argv);

    rc = PMIx_server_init(&mymodule, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "PMIx_server_init failed: %s\n", PMIx_Error_string(rc));
        return 1;
    }

    fprintf(stdout, "\n=== collective lost-connection accounting unit tests ===\n\n");

    /* Case B: an abnormal loss before contributing is recorded as departed,
     * and degrades the status - two of four ranks are still awaited, so the
     * collective is only partially successful, not over */
    trk = make_trk(4);
    contribute(trk, 0);
    peer = make_peer(1, false);
    completions = 0;
    pmix_server_trk_peer_lost(peer);
    report("abnormal loss is recorded as departed",
           1 == pmix_list_get_size(&trk->departed));
    report("abnormal loss degrades the status",
           PMIX_ERR_PARTIAL_SUCCESS == pmix_server_get_collective_status(trk->info, trk->ninfo));
    report("abnormal loss does not remove the contribution",
           1 == pmix_list_get_size(&trk->local_cbs));
    report("an incomplete collective is not driven to completion", 0 == completions);
    PMIX_RELEASE(peer);
    release_trk(trk);

    /* Case A: a loss after contributing is ignored entirely */
    trk = make_trk(4);
    contribute(trk, 1);
    peer = make_peer(1, false);
    pmix_server_trk_peer_lost(peer);
    report("a loss after contributing records no departure",
           0 == pmix_list_get_size(&trk->departed));
    report("a loss after contributing leaves the status alone",
           PMIX_SUCCESS == pmix_server_get_collective_status(trk->info, trk->ninfo));
    PMIX_RELEASE(peer);
    release_trk(trk);

    /* a peer that is not a participant is not accounted for at all */
    trk = make_trk(2);
    peer = make_peer(7, false);
    pmix_server_trk_peer_lost(peer);
    report("a non-participant records no departure",
           0 == pmix_list_get_size(&trk->departed));
    PMIX_RELEASE(peer);
    release_trk(trk);

    /* The #4113 pair. Two local ranks, one has contributed: the departure of
     * the other is the last thing the collective is waiting on.
     *
     * Abnormal loss -> it will never contribute, so complete on the survivor
     * and tell it the connection was lost. */
    trk = make_trk(2);
    contribute(trk, 0);
    peer = make_peer(1, false);
    completions = 0;
    completion_status = PMIX_SUCCESS;
    pmix_server_trk_peer_lost(peer);
    report("a final abnormal loss completes the collective", 1 == completions);
    report("a final abnormal loss reports lost-connection",
           PMIX_ERR_LOST_CONNECTION == completion_status);
    PMIX_RELEASE(peer);
    release_trk(trk);

    /* Graceful finalize -> the rank is still expected (it may PMIx_Init again
     * and contribute), so nothing is recorded, nothing is degraded, and the
     * collective keeps waiting for it. */
    trk = make_trk(2);
    contribute(trk, 0);
    peer = make_peer(1, true);
    completions = 0;
    pmix_server_trk_peer_lost(peer);
    report("a finalize records no departure",
           0 == pmix_list_get_size(&trk->departed));
    report("a finalize leaves the status alone",
           PMIX_SUCCESS == pmix_server_get_collective_status(trk->info, trk->ninfo));
    report("a finalize does not complete the collective", 0 == completions);
    report("a finalize leaves the tracker in flight",
           1 == pmix_list_get_size(&pmix_server_globals.collectives));
    PMIX_RELEASE(peer);
    release_trk(trk);

    fprintf(stdout, "\nResults: %d passed, %d failed\n\n", npass, nfail);

    PMIx_server_finalize();

    return (0 == nfail) ? 0 : 1;
}
