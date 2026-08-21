/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * A direct modex for a process that has already terminated.
 *
 * This is a deliberately FLAWED application: it reads a peer's value
 * without any guarantee that the peer is still alive. What it is here to
 * check is not that the read succeeds - it must not - but that PMIx says
 * so, promptly, instead of blocking forever. A developer who writes this
 * by accident should get an error they can act on, not a hung job that
 * looks like a PMIx fault.
 *
 * The fence is deliberately NON-collecting, so no server ends up holding
 * a copy of anybody else's data: the only route to rank 0's value is a
 * direct modex to rank 0's own daemon, which is the ordinary path for a
 * job that does not collect. Every rank but the last then leaves at
 * once, and the last one asks for rank 0 well after rank 0 is gone.
 */

#include <pmix.h>

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

#include "examples.h"

#define KEY "dmdx-departed-key"
/* how long the straggler waits, to be sure the peer is not merely slow */
#define LINGER_SEC 5

int main(int argc, char **argv)
{
    pmix_proc_t myproc, wildcard, peer;
    pmix_value_t put, *val = NULL;
    pmix_info_t info[2];
    pmix_status_t rc;
    uint32_t nprocs;
    bool flag;

    EXAMPLES_HIDE_UNUSED_PARAMS(argc, argv);

    if (PMIX_SUCCESS != (rc = PMIx_Init(&myproc, NULL, 0))) {
        fprintf(stderr, "dmdx_departed: PMIx_Init failed: %s\n", PMIx_Error_string(rc));
        exit(1);
    }
    PMIX_LOAD_PROCID(&wildcard, myproc.nspace, PMIX_RANK_WILDCARD);
    if (PMIX_SUCCESS != (rc = PMIx_Get(&wildcard, PMIX_JOB_SIZE, NULL, 0, &val))) {
        fprintf(stderr, "dmdx_departed: job size failed: %s\n", PMIx_Error_string(rc));
        exit(1);
    }
    nprocs = val->data.uint32;
    PMIX_VALUE_RELEASE(val);
    if (2 > nprocs) {
        fprintf(stderr, "dmdx_departed: needs at least 2 procs on 2 nodes\n");
        PMIx_Finalize(NULL, 0);
        exit(1);
    }

    PMIX_VALUE_CONSTRUCT(&put);
    put.type = PMIX_UINT32;
    put.data.uint32 = myproc.rank;
    if (PMIX_SUCCESS != (rc = PMIx_Put(PMIX_GLOBAL, KEY, &put))
        || PMIX_SUCCESS != (rc = PMIx_Commit())) {
        fprintf(stderr, "[rank %u] put/commit failed: %s\n",
                myproc.rank, PMIx_Error_string(rc));
        exit(1);
    }

    /* NON-collecting: everyone has committed, nobody has anyone else's
     * data, so a peer's value can only come from a direct modex */
    flag = false;
    PMIX_INFO_LOAD(&info[0], PMIX_COLLECT_DATA, &flag, PMIX_BOOL);
    rc = PMIx_Fence(&wildcard, 1, &info[0], 1);
    PMIX_INFO_DESTRUCT(&info[0]);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "[rank %u] fence failed: %s\n",
                myproc.rank, PMIx_Error_string(rc));
        exit(1);
    }

    if (myproc.rank != nprocs - 1) {
        /* leave immediately - by the time the straggler asks, we are gone */
        printf("[rank %u] DMDX-DEPARTED done\n", myproc.rank);
        fflush(stdout);
        PMIx_Finalize(NULL, 0);
        return 0;
    }

    sleep(LINGER_SEC);
    PMIX_LOAD_PROCID(&peer, myproc.nspace, 0);
    printf("[rank %u] asking rank 0 for %s, %d s after it terminated\n",
           myproc.rank, KEY, LINGER_SEC);
    fflush(stdout);

    {
        struct timespec t0, t1;
        double el;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        /* deliberately NO PMIX_TIMEOUT: a caller-supplied one would
         * mask the very thing under test by ending the wait itself */
        rc = PMIx_Get(&peer, KEY, NULL, 0, &val);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        el = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
        printf("[rank %u] PMIx_Get(rank 0) returned %s after %.2f s\n",
               myproc.rank, PMIx_Error_string(rc), el);
    }
    fflush(stdout);
    if (NULL != val) {
        PMIX_VALUE_RELEASE(val);
    }

    /* Any prompt answer is a pass. Reading the value of a process that
     * has terminated is not required to work, and it is not required to
     * fail either - what is required is that the caller is TOLD, rather
     * than parked forever on a reply nobody is left to send. */
    printf("[rank %u] DMDX-DEPARTED answered\n", myproc.rank);
    fflush(stdout);

    PMIx_Finalize(NULL, 0);
    return 0;
}
