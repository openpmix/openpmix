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

/* Test of basic fence with no data exchange (barrier) */

#include "pmix.h"
#include "test_common.h"

int main(int argc, char *argv[]) {

    pmix_value_t *val;
    int rc, i;
    size_t ninfo = 0, nprocs = 0;
    test_params params;
    validation_params v_params;
    pmix_proc_t job_proc, this_proc;
    pmix_proc_t *procs;
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

    // Synchronize before timing
    if (0 != PMIx_Fence(NULL, 0, NULL, 0)) {
        TEST_ERROR_EXIT(("PMIx_Fence Problem: ret val = %d", rc));
    }
    if (0 == this_proc.rank){
        sleep_ms(sleep_time_ms);
    }
    gettimeofday(&start, NULL);
    rc = PMIx_Fence(&job_proc, 1, NULL, 0);
    gettimeofday(&end, NULL);
    if (0 != rc) {
        TEST_ERROR_EXIT(("PMIx_Fence Problem: ret val = %d", rc));
    }
    usecs_elapsed =
        ((end.tv_sec * 1000000 + end.tv_usec) - (start.tv_sec * 1000000 + start.tv_usec));
    secs_elapsed = (double) usecs_elapsed / 1E6;
    TEST_VERBOSE(("Rank %d fence time: %lf padded_fence_time: %lf sleep_time: %lf secs_elapsed: %lf",
        this_proc.rank, fence_time, padded_fence_time, sleep_time, secs_elapsed));
    // secs_elapsed for rank 0 must be less than padded_fence_time
    if (0 == this_proc.rank) {
        if (secs_elapsed > padded_fence_time) {
        TEST_ERROR(("%s: PMIx_Fence timeout: Rank 0 elapsed fence time: %lf exceeded cutoff of: %lf",
            this_proc.nspace, secs_elapsed, padded_fence_time));
        pmixt_exit(PMIX_ERR_TIMEOUT);
        }
    }
    else {
        // secs_elapsed for other ranks must be within sleep_time +/- padded_fence_time
        if (secs_elapsed < (sleep_time - padded_fence_time) ) {
            TEST_ERROR(("%s: PMIx_Fence failed: Rank %d did not wait for Rank 0,"
            " elapsed time: %lf",
            this_proc.nspace, this_proc.rank, secs_elapsed));
            pmixt_exit(PMIX_ERR_TIMEOUT);
        }
        else if (secs_elapsed > (sleep_time + padded_fence_time) ) {
            TEST_ERROR(("%s: PMIx_Fence timeout: Rank %d elapsed fence time: %lf exceeded cutoff of: %lf",
                this_proc.nspace, this_proc.rank, secs_elapsed, sleep_time + padded_fence_time));
            pmixt_exit(PMIX_ERR_TIMEOUT);
        }
    }

    PMIX_PROC_DESTRUCT(&job_proc);

    /* finalize */
    PMIXT_CHECK(PMIx_Finalize(NULL, 0), params, v_params);

    /* Handles cleanup */
    pmixt_post_finalize(&this_proc, &params, &v_params);
}
