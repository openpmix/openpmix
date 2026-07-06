/*
 * Copyright (c) 2004-2010 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2011 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2013-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Exercise PMIx_Connect when a participant is lost before it contributes.
 *
 * Every rank in the job is part of the connect set.  Each rank first posts a
 * bit of remote-scope data and commits it, then all ranks sync with a fence.
 * The remote-scope put matters: the client only sends endpoint info to the
 * server on connect when it has such data posted, and that endpoint info is
 * appended to the connect tracker's info array AFTER the collective-status
 * slot.  This is exactly the layout that used to make the server's
 * lost-connection accounting write the status over the appended endpoint info
 * (see docs/how-things-work/collectives and the ptl fix that locates the
 * status slot by key).
 *
 * After the fence, every rank EXCEPT the last calls PMIx_Connect; the last
 * rank leaves WITHOUT calling it (after a short delay, so the others have
 * already entered the connect and their servers have built the local
 * trackers) and WITHOUT calling PMIx_Finalize, so the server sees a dropped
 * connection.  The server must account for the departed participant and
 * complete the connect on the survivors rather than hang.
 *
 * The whole job is the connect set (rather than a subset) so that, when
 * launched across nodes, the victim's node always also hosts a surviving
 * participant -- the one that creates the local tracker the loss accounting
 * then completes.  Launch with a launcher that does not kill the job when the
 * victim exits; under PRRTE that is `prterun --rtos recoverable ...`.
 */

#define _GNU_SOURCE
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <pmix.h>
#include "examples.h"

static pmix_proc_t myproc;

int main(int argc, char **argv)
{
    pmix_status_t rc;
    pmix_value_t *val = NULL;
    pmix_value_t value;
    pmix_proc_t proc, *procs;
    uint32_t nprocs, n, victim;
    char key[64];
    EXAMPLES_HIDE_UNUSED_PARAMS(argc, argv);

    if (PMIX_SUCCESS != (rc = PMIx_Init(&myproc, NULL, 0))) {
        fprintf(stderr, "Client ns %s rank %d: PMIx_Init failed: %s\n", myproc.nspace, myproc.rank,
                PMIx_Error_string(rc));
        exit(1);
    }

    /* get our job size */
    PMIX_LOAD_PROCID(&proc, myproc.nspace, PMIX_RANK_WILDCARD);
    if (PMIX_SUCCESS != (rc = PMIx_Get(&proc, PMIX_JOB_SIZE, NULL, 0, &val))) {
        fprintf(stderr, "Client ns %s rank %d: PMIx_Get job size failed: %s\n", myproc.nspace,
                myproc.rank, PMIx_Error_string(rc));
        exit(1);
    }
    nprocs = val->data.uint32;
    PMIX_VALUE_RELEASE(val);
    if (nprocs < 4) {
        if (0 == myproc.rank) {
            fprintf(stderr, "This example requires a minimum of 4 processes\n");
        }
        exit(1);
    }
    fprintf(stderr, "Client %s:%d job size %d\n", myproc.nspace, myproc.rank, nprocs);

    /* post some remote-scope data and commit it.  This is what causes the
     * client to send endpoint info to the server on connect, appended to the
     * tracker's info array after the collective-status slot. */
    snprintf(key, sizeof(key), "connect-die-remote-%u", myproc.rank);
    value.type = PMIX_UINT64;
    value.data.uint64 = 1000UL + (unsigned long) myproc.rank;
    if (PMIX_SUCCESS != (rc = PMIx_Put(PMIX_REMOTE, key, &value))) {
        fprintf(stderr, "Client ns %s rank %d: PMIx_Put failed: %s\n", myproc.nspace, myproc.rank,
                PMIx_Error_string(rc));
        exit(1);
    }
    if (PMIX_SUCCESS != (rc = PMIx_Commit())) {
        fprintf(stderr, "Client ns %s rank %d: PMIx_Commit failed: %s\n", myproc.nspace,
                myproc.rank, PMIx_Error_string(rc));
        exit(1);
    }

    /* sync everyone up first - all ranks are alive for this fence */
    PMIX_LOAD_PROCID(&proc, myproc.nspace, PMIX_RANK_WILDCARD);
    if (PMIX_SUCCESS != (rc = PMIx_Fence(&proc, 1, NULL, 0))) {
        fprintf(stderr, "Client ns %s rank %d: PMIx_Fence failed: %s\n", myproc.nspace,
                myproc.rank, PMIx_Error_string(rc));
        exit(1);
    }

    /* the entire job is the connect set; the last rank is the victim */
    victim = nprocs - 1;

    if (myproc.rank == victim) {
        /* the "victim": a participant that leaves before contributing.  Delay
         * briefly so the other participants have entered the connect and their
         * servers have built the local trackers, then leave without calling it
         * - and without calling PMIx_Finalize, so the server sees a dropped
         * connection and must run its lost-connection accounting.  We exit with
         * status 0 so the launcher does not abort the whole job; the dropped
         * socket, not the exit code, is what exercises the server. */
        fprintf(stderr, "%d leaving BEFORE PMIx_Connect (simulated failure)\n", myproc.rank);
        sleep(2);
        _exit(0);
    }

    fprintf(stderr, "%d executing PMIx_Connect\n", myproc.rank);
    PMIX_PROC_CREATE(procs, nprocs);
    for (n = 0; n < nprocs; n++) {
        PMIX_PROC_LOAD(&procs[n], myproc.nspace, n);
    }

    rc = PMIx_Connect(procs, nprocs, NULL, 0);
    PMIX_PROC_FREE(procs, nprocs);

    /* The connect must COMPLETE rather than hang.  It may return success or a
     * partial-success/lost-connection status depending on how the host treats
     * the departed participant; either way, reaching here is the point of the
     * test - the collective did not hang on the lost participant, and the
     * server did not corrupt the tracker's info array while accounting for the
     * loss. */
    fprintf(stderr, "%d Connect complete on survivors: status %s\n",
            myproc.rank, PMIx_Error_string(rc));

    PMIx_Finalize(NULL, 0);
    fprintf(stderr, "Client ns %s rank %d: FINALIZED\n", myproc.nspace, myproc.rank);
    return 0;
}
