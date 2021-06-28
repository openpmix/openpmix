/*
 * Copyright (c) 2021      Triad National Security, LLC.
 *                         All rights reserved.
 *
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/* Functionality test of basic fence with no data exchange (barrier) */

#include "pmix.h"
#include "test_common.h"

#define LOOPMAX 100

int main(int argc, char *argv[]) {

    pmix_value_t *val;
    int rc, i;
    size_t ninfo = 0, nprocs = 0;
    test_params params;
    validation_params v_params;
    pmix_proc_t job_proc, this_proc;
    pmix_proc_t *procs;
    struct timeval start, end;

    pmixt_pre_init(argc, argv, &params, &v_params, NULL);
    /* initialization */
    PMIXT_CHECK(PMIx_Init(&this_proc, NULL, ninfo), params, v_params);

    /* Handles everything that needs to happen after PMIx_Init() */
    pmixt_post_init(&this_proc, &params, &v_params);

    PMIX_PROC_CONSTRUCT(&job_proc);
    strncpy(job_proc.nspace, this_proc.nspace, PMIX_MAX_NSLEN);
    job_proc.rank = PMIX_RANK_WILDCARD;


    for (i = 0; i < LOOPMAX; i++) {
        // Fence with NULL procs and NULL info
        rc = PMIx_Fence(NULL, 0, NULL, 0);
        if (0 != rc) {
            TEST_ERROR_EXIT(("%s: Rank %d PMIx_Fence Problem: ret val = %d",
                this_proc.nspace, this_proc.rank, rc));
        }
        // Fence with WILDCARD
        rc = PMIx_Fence(&job_proc, 1, NULL, 0);
        if (0 != rc) {
            TEST_ERROR_EXIT(("%s: Rank %d PMIx_Fence Problem: ret val = %d",
                this_proc.nspace, this_proc.rank, rc));
        }
    }

    PMIX_PROC_DESTRUCT(&job_proc);

    /* finalize */
    PMIXT_CHECK(PMIx_Finalize(NULL, 0), params, v_params);

    /* Handles cleanup */
    pmixt_post_finalize(&this_proc, &params, &v_params);
}
