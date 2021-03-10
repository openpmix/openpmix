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

/* Test of basic Fence with no data exchange (barrier) */

#include "pmix.h"
#include "test_common.h"

// Number of times to iterate Fence loop
#define M 5

int main(int argc, char *argv[]) {

    pmix_value_t *val;
    int rc, i, j;
    size_t ninfo = 0, nprocs = 0;
    test_params params;
    validation_params v_params;
    pmix_proc_t job_proc, this_proc;
    pmix_info_t info;
    pmix_proc_t *procs;
    //fence_desc_t *desc;
    struct timeval start, end;
    unsigned long usecs_elapsed, sleep_time_ms;
    double secs_elapsed, fence_time, sleep_time, padded_fence_time;

    pmixt_pre_init(argc, argv, &params, &v_params);
    /* initialization */
    PMIXT_CHECK(PMIx_Init(&this_proc, NULL, ninfo), params, v_params);

    /* Handles everything that needs to happen after PMIx_Init() */
    pmixt_post_init(&this_proc, &params, &v_params);

    PMIX_PROC_CONSTRUCT(&job_proc);
    strncpy(job_proc.nspace, this_proc.nspace, PMIX_MAX_NSLEN);
    job_proc.rank = PMIX_RANK_WILDCARD;

    // establishes baseline fence time before entering loop
    fence_time = avg_fence_time();
    // padded_fence_time is the time permitted just for the fence call
    padded_fence_time = params.fence_time_multiplier * fence_time;
    sleep_time = params.fence_timeout_ratio * padded_fence_time;
    sleep_time_ms = (long)(sleep_time * 1000.0);

    TEST_VERBOSE(("Rank %d fence timeout ratio: %lf fence time multiplier: %lf",
        this_proc.rank, params.fence_timeout_ratio, params.fence_time_multiplier));

    j = (int)ceil(sleep_time / 2.0);
    TEST_VERBOSE(("sleep_time: %lf Timeout: %d seconds", sleep_time, j));
    PMIX_INFO_LOAD(&info, PMIX_TIMEOUT, &j, PMIX_INT);
    PMIX_INFO_REQUIRED(&info);
    // Synchronize before timing
    if (PMIX_SUCCESS != PMIx_Fence(NULL, 0, NULL, 0)) {
        TEST_ERROR_EXIT(("PMIx_Fence Problem: ret val = %d", rc));
    }
    if (0 == this_proc.rank){
        sleep_ms(sleep_time_ms);
    }
    gettimeofday(&start, NULL);
    rc = PMIx_Fence(&job_proc, 1, &info, 1);
    gettimeofday(&end, NULL);
    if (PMIX_ERR_TIMEOUT == rc) {
        TEST_OUTPUT(("Rank %d PMIx_Fence Timeout (expected behavior)", this_proc.rank));
        fflush(stdout);
        //pmixt_exit(PMIX_ERR_TIMEOUT);
    }
    else if (PMIX_SUCCESS != rc) {
        TEST_ERROR_EXIT(("Rank %d PMIx_Fence Problem: ret val = %d", this_proc.rank, rc));
    }
    else if (PMIX_SUCCESS == rc && 0 != this_proc.rank) {
        TEST_ERROR_EXIT(("Rank %d PMIx_Fence did not timeout as expected", this_proc.rank, rc));
    }

    PMIX_PROC_DESTRUCT(&job_proc);

    /* finalize */
    PMIXT_CHECK(PMIx_Finalize(NULL, 0), params, v_params);

    /* Handles cleanup */
    pmixt_post_finalize(&this_proc, &params, &v_params);
}
