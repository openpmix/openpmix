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
    pmix_proc_t job_proc, peer_proc;
    int i;
    bool check_self = true;

    pmixt_pre_init(argc, argv, &params, &v_params);
    /* initialization */
    PMIXT_CHECK(PMIx_Init(&this_proc, NULL, ninfo), params, v_params);

    /* Handles everything that needs to happen after PMIx_Init() */
    pmixt_post_init(&this_proc, &params, &v_params);

    job_proc = this_proc;
    job_proc.rank = PMIX_RANK_WILDCARD; // Note that PMIX_RANK_WILDCARD == -2

    PMIXT_CHECK(PMIx_Get(&job_proc, PMIX_JOB_SIZE, NULL, 0, &val), params, v_params);
    pmixt_validate_predefined(check_self, &job_proc, PMIX_JOB_SIZE, val, PMIX_UINT32, &v_params);
    free(val);
    TEST_VERBOSE(("after PMIX_JOB_SIZE check"));

    PMIXT_CHECK(PMIx_Get(&this_proc, PMIX_LOCAL_RANK, NULL, 0, &val), params, v_params);
    pmixt_validate_predefined(check_self, &this_proc, PMIX_LOCAL_RANK, val, PMIX_UINT16, &v_params);
    free(val);
    TEST_VERBOSE(("after PMIX_LOCAL_RANK check"));

    PMIXT_CHECK(PMIx_Get(&job_proc, PMIX_LOCAL_PEERS, NULL, 0, &val), params, v_params);
    pmixt_validate_predefined(check_self, &job_proc, PMIX_LOCAL_PEERS, val, PMIX_STRING, &v_params);
    free(val);
    TEST_VERBOSE(("after PMIX_LOCAL_PEERS check"));

    PMIXT_CHECK(PMIx_Get(&this_proc, PMIX_NODEID, NULL, 0, &val), params, v_params);
    pmixt_validate_predefined(check_self, &this_proc, PMIX_NODEID, val, PMIX_UINT32, &v_params);
    free(val);
    TEST_VERBOSE(("after PMIX_NODEID check"));

    PMIXT_CHECK(PMIx_Get(&this_proc, PMIX_NODE_RANK, NULL, 0, &val), params, v_params);
    pmixt_validate_predefined(check_self, &this_proc, PMIX_NODE_RANK, val, PMIX_UINT16, &v_params);
    free(val);
    TEST_VERBOSE(("after PMIX_NODE_RANK check"));

    PMIXT_CHECK(PMIx_Get(&this_proc, PMIX_HOSTNAME, NULL, 0, &val), params, v_params);
    pmixt_validate_predefined(check_self, &this_proc, PMIX_HOSTNAME, val, PMIX_STRING, &v_params);
    free(val);
    TEST_VERBOSE(("after PMIX_HOSTNAME check"));

    // validation data must be populated for all peers, remote and local, by this point
    peer_proc = this_proc;
    check_self = false;
    for (i = 0; i < v_params.pmix_job_size; i++ ) {
        peer_proc.rank = i;
        PMIXT_CHECK(PMIx_Get(&peer_proc, PMIX_LOCAL_RANK, NULL, 0, &val), params, v_params);
        pmixt_validate_predefined(check_self, &peer_proc, PMIX_LOCAL_RANK, val, PMIX_UINT16, &v_params);
        free(val);
        TEST_VERBOSE(("after PMIX_LOCAL_RANK check for rank = %d", peer_proc.rank));

        PMIXT_CHECK(PMIx_Get(&peer_proc, PMIX_NODEID, NULL, 0, &val), params, v_params);
        pmixt_validate_predefined(check_self, &peer_proc, PMIX_NODEID, val, PMIX_UINT32, &v_params);
        free(val);
        TEST_VERBOSE(("after PMIX_NODEID check for rank = %d", peer_proc.rank));

        PMIXT_CHECK(PMIx_Get(&peer_proc, PMIX_NODE_RANK, NULL, 0, &val), params, v_params);
        pmixt_validate_predefined(check_self, &peer_proc, PMIX_NODE_RANK, val, PMIX_UINT16, &v_params);
        free(val);
        TEST_VERBOSE(("after PMIX_NODE_RANK check for rank = %d", peer_proc.rank));

        PMIXT_CHECK(PMIx_Get(&peer_proc, PMIX_HOSTNAME, NULL, 0, &val), params, v_params);
        pmixt_validate_predefined(check_self, &peer_proc, PMIX_HOSTNAME, val, PMIX_STRING, &v_params);
        free(val);
        TEST_VERBOSE(("after PMIX_HOSTNAME check for rank = %d", peer_proc.rank));
    }

    /* finalize */
    PMIXT_CHECK(PMIx_Finalize(NULL, 0), params, v_params);

    /* Handles cleanup */
    pmixt_post_finalize(&this_proc, &params, &v_params);
}