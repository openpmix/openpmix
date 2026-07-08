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
 * Copyright (c) 2026      Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Exercise the default (non-fault-tolerant) group construct abort path: when a
 * member is lost before contributing and PMIX_GROUP_FT_COLLECTIVE was NOT
 * requested, the construct cannot form as requested and the PMIx server aborts
 * it rather than silently forming a reduced group.
 *
 * The whole job is the group membership. All ranks sync with a fence. The last
 * rank is the victim: it leaves WITHOUT calling PMIx_Group_construct and WITHOUT
 * calling PMIx_Finalize (after a short delay so the survivors have entered the
 * construct and their server has built the local tracker), so the server sees a
 * dropped connection. Every surviving rank calls PMIx_Group_construct with no
 * fault-tolerance flag; the server accounts for the departed member and - the
 * behavior under test - completes the construct with PMIX_GROUP_CONSTRUCT_ABORT.
 * Each survivor verifies its PMIx_Group_construct returned that abort status and
 * prints a distinctive success line. Contrast with simpgrpdie, which sets
 * PMIX_GROUP_FT_COLLECTIVE and completes on the survivors instead.
 *
 * See src/server/pmix_server_group.c (abort_construct / grp_ft_collective) and
 * docs/how-things-work/collectives.
 */

#include "src/include/pmix_config.h"
#include "include/pmix.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "src/include/pmix_globals.h"
#include "src/util/pmix_output.h"

static pmix_proc_t myproc;

int main(int argc, char **argv)
{
    pmix_status_t rc;
    pmix_value_t *val = NULL;
    pmix_proc_t proc, *procs;
    uint32_t nprocs, n, victim;
    pmix_info_t *results = NULL;
    size_t nresults = 0;
    PMIX_HIDE_UNUSED_PARAMS(argc, argv);

    if (PMIX_SUCCESS != (rc = PMIx_Init(&myproc, NULL, 0))) {
        fprintf(stderr, "Client PMIx_Init failed: %s\n", PMIx_Error_string(rc));
        exit(1);
    }

    /* get our job size */
    PMIX_LOAD_PROCID(&proc, myproc.nspace, PMIX_RANK_WILDCARD);
    if (PMIX_SUCCESS != (rc = PMIx_Get(&proc, PMIX_JOB_SIZE, NULL, 0, &val))) {
        fprintf(stderr, "Client %s:%d PMIx_Get job size failed: %s\n", myproc.nspace,
                myproc.rank, PMIx_Error_string(rc));
        exit(1);
    }
    nprocs = val->data.uint32;
    PMIX_VALUE_RELEASE(val);
    if (nprocs < 2) {
        fprintf(stderr, "Client %s:%d FAIL: this test requires at least 2 processes\n",
                myproc.nspace, myproc.rank);
        exit(1);
    }

    /* sync everyone up first - all ranks are alive for this fence */
    PMIX_LOAD_PROCID(&proc, myproc.nspace, PMIX_RANK_WILDCARD);
    if (PMIX_SUCCESS != (rc = PMIx_Fence(&proc, 1, NULL, 0))) {
        fprintf(stderr, "Client %s:%d PMIx_Fence failed: %s\n", myproc.nspace,
                myproc.rank, PMIx_Error_string(rc));
        exit(1);
    }

    /* the entire job is the group membership; the last rank is the victim */
    victim = nprocs - 1;

    if (myproc.rank == victim) {
        /* leave before contributing, without finalizing, so the server sees a
         * dropped connection and must run its lost-connection accounting. Exit
         * 0 so the harness does not treat this as a real failure. */
        fprintf(stderr, "%d leaving BEFORE group construct (simulated failure)\n", myproc.rank);
        sleep(2);
        _exit(0);
    }

    PMIX_PROC_CREATE(procs, nprocs);
    for (n = 0; n < nprocs; n++) {
        PMIX_PROC_LOAD(&procs[n], myproc.nspace, n);
    }

    /* No PMIX_GROUP_FT_COLLECTIVE: the loss of a required member must abort the
     * construct rather than form a reduced group. */
    rc = PMIx_Group_construct("abortgroup", procs, nprocs, NULL, 0, &results, &nresults);
    PMIX_PROC_FREE(procs, nprocs);
    if (NULL != results) {
        PMIX_INFO_FREE(results, nresults);
    }

    if (PMIX_GROUP_CONSTRUCT_ABORT != rc) {
        fprintf(stderr, "Client %s:%d FAIL: construct returned %s, expected "
                "PMIX_GROUP_CONSTRUCT_ABORT\n", myproc.nspace, myproc.rank,
                PMIx_Error_string(rc));
        PMIx_Finalize(NULL, 0);
        exit(1);
    }

    if (PMIX_SUCCESS != (rc = PMIx_Finalize(NULL, 0))) {
        fprintf(stderr, "Client %s:%d PMIx_Finalize failed: %s\n", myproc.nspace,
                myproc.rank, PMIx_Error_string(rc));
        exit(1);
    }

    fprintf(stderr, "Client %s:%d saw construct ABORT on loss of rank %d OK\n",
            myproc.nspace, myproc.rank, victim);
    fflush(stderr);
    return 0;
}
