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
 * Exercise group construction when a member is lost before it contributes.
 *
 * Every rank in the job is a group member.  All ranks first sync with a fence,
 * then every rank EXCEPT the last calls PMIx_Group_construct; the last rank
 * leaves WITHOUT calling it (after a short delay, so the others have already
 * entered the construct and their servers have created the local trackers) and
 * WITHOUT calling PMIx_Finalize, so the server sees a dropped connection. The
 * server must account for the departed member and complete the construct on the
 * survivors rather than hang. Before the server tracked group participation by
 * identity this would hang, because the group's local phase never reached its
 * expected count. See docs/how-things-work/collectives.
 *
 * The whole job is the membership (rather than a subset) so that, when launched
 * across nodes, the victim's node always also hosts a surviving member -- the
 * one that creates the local tracker the loss accounting then completes. Launch
 * with a launcher that does not kill the job when the victim exits; under PRRTE
 * that is `prterun --rtos recoverable ...`.
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
    pmix_proc_t proc, *procs;
    uint32_t nprocs, n, victim;
    pmix_info_t info, *results;
    size_t nresults;
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

    /* sync everyone up first - all ranks are alive for this fence */
    PMIX_LOAD_PROCID(&proc, myproc.nspace, PMIX_RANK_WILDCARD);
    if (PMIX_SUCCESS != (rc = PMIx_Fence(&proc, 1, NULL, 0))) {
        fprintf(stderr, "Client ns %s rank %d: PMIx_Fence failed: %s\n", myproc.nspace,
                myproc.rank, PMIx_Error_string(rc));
        exit(1);
    }

    /* the entire job is the group membership; the last rank is the victim */
    victim = nprocs - 1;

    if (myproc.rank == victim) {
        /* the "victim": a member that leaves before contributing.  Delay
         * briefly so the other members have entered the construct and their
         * servers have built the local trackers, then leave without calling it
         * - and without calling PMIx_Finalize, so the server sees a dropped
         * connection and must run its lost-connection accounting.  We exit with
         * status 0 so the launcher does not abort the whole job; the dropped
         * socket, not the exit code, is what exercises the server. */
        fprintf(stderr, "%d leaving BEFORE group construct (simulated failure)\n", myproc.rank);
        sleep(2);
        _exit(0);
    }

    fprintf(stderr, "%d executing Group_construct\n", myproc.rank);
    PMIX_PROC_CREATE(procs, nprocs);
    for (n = 0; n < nprocs; n++) {
        PMIX_PROC_LOAD(&procs[n], myproc.nspace, n);
    }
    PMIX_INFO_LOAD(&info, PMIX_GROUP_ASSIGN_CONTEXT_ID, NULL, PMIX_BOOL);

    results = NULL;
    nresults = 0;
    rc = PMIx_Group_construct("diegroup", procs, nprocs, &info, 1, &results, &nresults);
    PMIX_PROC_FREE(procs, nprocs);
    if (NULL != results) {
        PMIX_INFO_FREE(results, nresults);
    }

    /* The construct must COMPLETE rather than hang.  It may return success or a
     * partial-success/lost-connection status depending on how the host treats
     * the departed member; either way, reaching here is the point of the
     * test - the collective did not hang on the lost participant. */
    fprintf(stderr, "%d Group construct complete on survivors: status %s\n",
            myproc.rank, PMIx_Error_string(rc));

    PMIx_Finalize(NULL, 0);
    fprintf(stderr, "Client ns %s rank %d: FINALIZED\n", myproc.nspace, myproc.rank);
    return 0;
}
