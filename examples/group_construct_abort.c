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
 * Exercise the cross-server group construct abort (PMIX_GROUP_CANCEL path).
 *
 * Every rank in the job is a group member. All ranks fence, then every rank
 * EXCEPT the last calls PMIx_Group_construct WITHOUT PMIX_GROUP_FT_COLLECTIVE;
 * the last rank leaves before contributing (and without finalizing), so its
 * server sees a dropped connection. Because fault-tolerant collective tracking
 * was not requested, the construct must ABORT rather than form a reduced group.
 *
 * The point of running this across nodes (the whole job is the membership,
 * launched --map-by node) is that the lost member's server aborts its own local
 * participants directly AND issues PMIX_GROUP_CANCEL to the host, which aborts
 * the cross-server collective the other servers already joined. Every surviving
 * member on every node must therefore see its PMIx_Group_construct return
 * PMIX_GROUP_CONSTRUCT_ABORT - if the cancel were not propagated, the survivors
 * on the other nodes would hang forever. Each survivor prints a distinctive
 * success line.
 *
 * Launch with a launcher that does not kill the job when the victim exits;
 * under PRRTE that is `prterun --rtos recoverable ...`.
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
    pmix_info_t *results;
    size_t nresults;
    EXAMPLES_HIDE_UNUSED_PARAMS(argc, argv);

    if (PMIX_SUCCESS != (rc = PMIx_Init(&myproc, NULL, 0))) {
        fprintf(stderr, "Client ns %s rank %d: PMIx_Init failed: %s\n", myproc.nspace,
                myproc.rank, PMIx_Error_string(rc));
        exit(1);
    }

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
        /* leave before contributing, without finalizing, so the server sees a
         * dropped connection. Exit 0 so the launcher does not abort the job. */
        fprintf(stderr, "%d leaving BEFORE group construct (simulated failure)\n", myproc.rank);
        sleep(2);
        _exit(0);
    }

    fprintf(stderr, "%d executing Group_construct (no FT_COLLECTIVE)\n", myproc.rank);
    PMIX_PROC_CREATE(procs, nprocs);
    for (n = 0; n < nprocs; n++) {
        PMIX_PROC_LOAD(&procs[n], myproc.nspace, n);
    }

    /* No PMIX_GROUP_FT_COLLECTIVE: the loss of a required member must abort the
     * construct on every surviving member, across all servers. */
    results = NULL;
    nresults = 0;
    rc = PMIx_Group_construct("cabortgroup", procs, nprocs, NULL, 0, &results, &nresults);
    PMIX_PROC_FREE(procs, nprocs);
    if (NULL != results) {
        PMIX_INFO_FREE(results, nresults);
    }

    if (PMIX_GROUP_CONSTRUCT_ABORT == rc) {
        fprintf(stderr, "%d construct ABORT on member loss: PASS\n", myproc.rank);
    } else {
        fprintf(stderr, "%d construct returned %s, expected PMIX_GROUP_CONSTRUCT_ABORT: FAILED\n",
                myproc.rank, PMIx_Error_string(rc));
    }

    PMIx_Finalize(NULL, 0);
    fprintf(stderr, "Client ns %s rank %d: FINALIZED\n", myproc.nspace, myproc.rank);
    return 0;
}
