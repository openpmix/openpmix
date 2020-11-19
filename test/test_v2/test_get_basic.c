/*
 * Copyright (c) 2020      Triad National Security, LLC.
 *                         All rights reserved.
 *
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/* Test of rank position identification inside group */

#include "pmix.h"
#include "test_common.h"
#include <stdio.h>
#include <stdlib.h>
/*
#include <assert.h>
#include <sys/time.h>
#include <stdarg.h>
#include <unistd.h>
*/

extern FILE *file;

pmix_proc_t this_proc;

int main(int argc, char *argv[]) {

    pmix_value_t *val;
    size_t ninfo = 0;
    test_params params;
    validation_params v_params;
    pmix_proc_t job_proc;

    pmixt_pre_init(argc, argv, &params, &v_params);
    /* initialization */
    PMIXT_CHECK(PMIx_Init(&this_proc, NULL, ninfo), params, v_params);

    /* Handles everything that needs to happen after PMIx_Init() */
    pmixt_post_init(&this_proc, &params, &v_params);

    job_proc = this_proc;
    job_proc.rank = PMIX_RANK_WILDCARD; // Note that PMIX_RANK_WILDCARD == -2
    PMIXT_CHECK(PMIx_Get(&job_proc, PMIX_JOB_SIZE, NULL, 0, &val), params, v_params);
    /* After using PMIx_Get to get a value, we need to compare it our validation parameters
       we've passed as an argument; this is the main purpose of pmixt_validate_predefined(). */

    pmixt_validate_predefined(&job_proc, PMIX_JOB_SIZE, val, PMIX_UINT32, &v_params);

    PMIXT_CHECK(PMIx_Get(&job_proc, PMIX_UNIV_SIZE, NULL, 0, &val), params, v_params);
    pmixt_validate_predefined(&job_proc, PMIX_UNIV_SIZE, val, PMIX_UINT32, &v_params);

    PMIXT_CHECK(PMIx_Get(&job_proc, PMIX_LOCAL_SIZE, NULL, 0, &val), params, v_params);
    pmixt_validate_predefined(&job_proc, PMIX_LOCAL_SIZE, val, PMIX_UINT32, &v_params);

    PMIXT_CHECK(PMIx_Get(&this_proc, PMIX_LOCAL_RANK, NULL, 0, &val), params, v_params);
    pmixt_validate_predefined(&this_proc, PMIX_LOCAL_RANK, val, PMIX_UINT16, &v_params);

    PMIXT_CHECK(PMIx_Get(&this_proc, PMIX_NODEID, NULL, 0, &val), params, v_params);
    pmixt_validate_predefined(&this_proc, PMIX_NODEID, val, PMIX_UINT32, &v_params);

    PMIXT_CHECK(PMIx_Get(&job_proc, PMIX_LOCAL_PEERS, NULL, 0, &val), params, v_params);
    pmixt_validate_predefined(&job_proc, PMIX_LOCAL_PEERS, val, PMIX_STRING, &v_params);

    PMIXT_CHECK(PMIx_Get(&this_proc, PMIX_HOSTNAME, NULL, 0, &val), params, v_params);
    pmixt_validate_predefined(&this_proc, PMIX_HOSTNAME, val, PMIX_STRING, &v_params);

    // Code hangs when PMIx_Get of PMIX_RANK is enabled
    /*
    job_proc.rank = PMIX_RANK_UNDEF;
    PMIXT_CHECK(PMIx_Get(&job_proc, PMIX_RANK, NULL, 0, &val), params, v_params);
    pmixt_validate_predefined(&job_proc, PMIX_RANK, val, PMIX_PROC_RANK, &v_params);
    */

    /* finalize */
    PMIXT_CHECK(PMIx_Finalize(NULL, 0), params, v_params);

    /* Handles cleanup */
    pmixt_post_finalize(&this_proc, &params, &v_params);
}
