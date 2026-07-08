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
 * Exercise group construct recovery when a whole DAEMON is lost mid-construct.
 *
 * This is the daemon-death companion to group_construct_abort (which drops a
 * client). Every rank in the job is a group member. All ranks fence, then every
 * rank EXCEPT the last enters PMIx_Group_construct and blocks; the last rank
 * (which the harness places alone on its own node) never contributes - it just
 * stays alive - so the construct is pending across servers. The harness then
 * kills that node's prted. PRRTE's grpcomm fault handler must abort the pending
 * construct on the surviving servers (rather than tear down the whole DVM, its
 * former behavior), so every surviving member's PMIx_Group_construct returns
 * PMIX_GROUP_CONSTRUCT_ABORT. Each survivor prints a distinctive success line.
 *
 * Requires a launcher that keeps the job alive across a daemon loss; under PRRTE
 * that is `prterun --rtos recoverable ...`. The victim must be alone on its node
 * so that killing that node's daemon does not also remove a surviving member.
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

    /* the entire job is the group membership; the last rank is the victim - it
     * stays alive but never contributes, so the construct is pending when the
     * harness kills its daemon */
    victim = nprocs - 1;

    if (myproc.rank == victim) {
        fprintf(stderr, "%d alive but NOT joining the construct - awaiting daemon kill\n",
                myproc.rank);
        sleep(60);
        /* if we are still here, the harness never killed our daemon */
        goto done;
    }

    fprintf(stderr, "%d executing Group_construct\n", myproc.rank);
    PMIX_PROC_CREATE(procs, nprocs);
    for (n = 0; n < nprocs; n++) {
        PMIX_PROC_LOAD(&procs[n], myproc.nspace, n);
    }

    results = NULL;
    nresults = 0;
    rc = PMIx_Group_construct("faultgroup", procs, nprocs, NULL, 0, &results, &nresults);
    PMIX_PROC_FREE(procs, nprocs);
    if (NULL != results) {
        PMIX_INFO_FREE(results, nresults);
    }

    if (PMIX_GROUP_CONSTRUCT_ABORT == rc) {
        fprintf(stderr, "%d construct ABORT after daemon loss: PASS\n", myproc.rank);
    } else {
        fprintf(stderr, "%d construct returned %s, expected PMIX_GROUP_CONSTRUCT_ABORT: FAILED\n",
                myproc.rank, PMIx_Error_string(rc));
    }

done:
    PMIx_Finalize(NULL, 0);
    fprintf(stderr, "Client ns %s rank %d: FINALIZED\n", myproc.nspace, myproc.rank);
    return 0;
}
