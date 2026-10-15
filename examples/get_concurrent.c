/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * A get for a key a peer has committed must not be answered with the
 * outcome of some other get that happens to be outstanding for that peer.
 *
 * The client used to fold every get for a proc onto whatever request for
 * that proc was already in flight to the server, whatever key it named,
 * and answer all of them with the reply to the one that was sent. The
 * server parks a get for a key a local peer has not put and fails it when
 * the wait expires - 2 seconds by default - so a get for a key the peer
 * never published, issued just ahead of a get for one it had committed
 * long before, failed both: the committed key came back PMIX_ERR_TIMEOUT
 * after two seconds. A PMIX_IMMEDIATE get, the documented way to ask
 * "what is here now", inherited the same wait.
 *
 * Ranks 0 and 1 each commit a PMIX_GLOBAL key. Rank 2 then puts a get for
 * a key the target never published in flight against each of them and,
 * behind it, asks for the committed key - plainly of rank 0, with
 * PMIX_IMMEDIATE of rank 1. Both must succeed. Two publishers are needed
 * because a successful reply carries all of a peer's data and is cached
 * here, which would let the second case be answered locally.
 *
 * Everything is on one node and no data is exchanged by a collective: the
 * fence below carries none and is only there to order the commits ahead of
 * the gets. test/unit/run_getconcurrent.pl drives this under
 * test/simple/simptest with three procs.
 */

#define _GNU_SOURCE
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "examples.h"
#include <pmix.h>

#define PUB_KEY    "get.concurrent.pub"
#define ABSENT_KEY "get.concurrent.absent"

static pmix_proc_t myproc;
static volatile int nbdone = 0;

static void nbcb(pmix_status_t status, pmix_value_t *kv, void *cbdata)
{
    EXAMPLES_HIDE_UNUSED_PARAMS(cbdata);

    /* the key was never put, so this is expected to fail - what matters
     * is only that it is answered, and that it did not drag the get
     * behind it down with it */
    fprintf(stderr, "get_concurrent: unpublished key -> %s\n", PMIx_Error_string(status));
    if (NULL != kv) {
        PMIX_VALUE_RELEASE(kv);
    }
    nbdone++;
}

/* put a get for an unpublished key in flight, then ask for the committed
 * one; returns true if the committed one was delivered */
static bool check_peer(pmix_rank_t target, bool immediate)
{
    pmix_proc_t proc;
    pmix_value_t *val = NULL;
    pmix_info_t info;
    pmix_status_t rc;
    bool ok;

    PMIX_LOAD_PROCID(&proc, myproc.nspace, target);
    rc = PMIx_Get_nb(&proc, ABSENT_KEY, NULL, 0, nbcb, NULL);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "get_concurrent: PMIx_Get_nb refused: %s\n", PMIx_Error_string(rc));
        return false;
    }
    if (immediate) {
        PMIX_INFO_LOAD(&info, PMIX_IMMEDIATE, NULL, PMIX_BOOL);
        rc = PMIx_Get(&proc, PUB_KEY, &info, 1, &val);
        PMIX_INFO_DESTRUCT(&info);
    } else {
        rc = PMIx_Get(&proc, PUB_KEY, NULL, 0, &val);
    }
    ok = (PMIX_SUCCESS == rc && NULL != val && PMIX_UINT32 == val->type
          && target == val->data.uint32);
    fprintf(stderr, "get_concurrent: committed key of rank %u (%s) -> %s: %s\n",
            target, immediate ? "immediate" : "plain", PMIx_Error_string(rc),
            ok ? "ok" : "WRONG");
    if (NULL != val) {
        PMIX_VALUE_RELEASE(val);
    }
    return ok;
}

int main(int argc, char **argv)
{
    pmix_status_t rc;
    pmix_value_t value;
    pmix_info_t info;
    bool flag = false, ok = true;
    uint32_t u32;
    int n;

    EXAMPLES_HIDE_UNUSED_PARAMS(argc, argv);

    rc = PMIx_Init(&myproc, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "get_concurrent: PMIx_Init failed: %s\n", PMIx_Error_string(rc));
        exit(1);
    }

    if (2 > myproc.rank) {
        u32 = myproc.rank;
        PMIX_VALUE_LOAD(&value, &u32, PMIX_UINT32);
        rc = PMIx_Put(PMIX_GLOBAL, PUB_KEY, &value);
        PMIX_VALUE_DESTRUCT(&value);
        if (PMIX_SUCCESS == rc) {
            rc = PMIx_Commit();
        }
        if (PMIX_SUCCESS != rc) {
            fprintf(stderr, "get_concurrent: put/commit failed: %s\n", PMIx_Error_string(rc));
            ok = false;
        }
    }

    /* a barrier only - no data rides on it */
    PMIX_INFO_LOAD(&info, PMIX_COLLECT_DATA, &flag, PMIX_BOOL);
    rc = PMIx_Fence(NULL, 0, &info, 1);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "get_concurrent: first fence failed: %s\n", PMIx_Error_string(rc));
        ok = false;
    }

    if (2 == myproc.rank && ok) {
        ok = check_peer(0, false) && ok;
        ok = check_peer(1, true) && ok;
        /* the unpublished gets are answered when the server stops
         * waiting for them; do not leave while they are outstanding */
        for (n = 0; n < 300 && 2 > nbdone; n++) {
            usleep(100000);
        }
        if (2 > nbdone) {
            fprintf(stderr, "get_concurrent: unpublished-key gets never answered\n");
            ok = false;
        }
        fprintf(stderr, "get_concurrent: %s\n", ok ? "PASS" : "FAIL");
    }

    /* keep the publishers alive until rank 2 is finished with them */
    rc = PMIx_Fence(NULL, 0, &info, 1);
    PMIX_INFO_DESTRUCT(&info);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "get_concurrent: second fence failed: %s\n", PMIx_Error_string(rc));
        ok = false;
    }

    rc = PMIx_Finalize(NULL, 0);
    if (PMIX_SUCCESS != rc) {
        ok = false;
    }
    return ok ? 0 : 1;
}
