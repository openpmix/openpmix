/*
 * Copyright (c) 2020      Triad National Security, LLC.
 *                         All rights reserved.
 *
 * Copyright (c) 2021-2022 Nanook Consulting.  All rights reserved.
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
#include <stdarg.h>
#include <sys/time.h>
#include <unistd.h>
*/

pmix_proc_t this_proc;

int main(int argc, char *argv[])
{

    pmix_value_t *val;
    size_t ninfo = 0;
    test_params l_params;
    validation_params v_params;
    pmix_proc_t job_proc, peer_proc;
    uint32_t i;
    pmix_key_t cache;

    pmixt_pre_init(argc, argv, &l_params, &v_params, NULL);
    /* initialization */
    PMIXT_CHECK(PMIx_Init(&this_proc, NULL, ninfo), l_params, v_params);

    /* Handles everything that needs to happen after PMIx_Init() */
    pmixt_post_init(&this_proc, &l_params, &v_params);

    job_proc = this_proc;
    job_proc.rank = PMIX_RANK_WILDCARD;

    PMIX_LOAD_KEY(cache, PMIX_JOB_SIZE);
    PMIXT_CHECK(PMIx_Get(&job_proc, cache, NULL, 0, &val), l_params, v_params);
    pmixt_validate_predefined(&job_proc, cache, val, PMIX_UINT32, &v_params);
    free(val);
    TEST_VERBOSE(("after PMIX_JOB_SIZE check"));

    PMIX_LOAD_KEY(cache, PMIX_LOCAL_PEERS);
    PMIXT_CHECK(PMIx_Get(&job_proc, cache, NULL, 0, &val), l_params, v_params);
    pmixt_validate_predefined(&job_proc, cache, val, PMIX_STRING, &v_params);
    free(val);
    TEST_VERBOSE(("after PMIX_LOCAL_PEERS check"));

    // validation data must be populated for all peers, remote and local, by this point
    peer_proc = this_proc;
    for (i = 0; i < v_params.pmix_job_size; i++) {
        peer_proc.rank = i;
        PMIX_LOAD_KEY(cache, PMIX_LOCAL_RANK);
        PMIXT_CHECK(PMIx_Get(&peer_proc, cache, NULL, 0, &val), l_params, v_params);
        pmixt_validate_predefined(&peer_proc, cache, val, PMIX_UINT16, &v_params);
        free(val);
        TEST_VERBOSE(("after PMIX_LOCAL_RANK check for rank = %d", peer_proc.rank));

        PMIX_LOAD_KEY(cache, PMIX_NODEID);
        PMIXT_CHECK(PMIx_Get(&peer_proc, cache, NULL, 0, &val), l_params, v_params);
        pmixt_validate_predefined(&peer_proc, cache, val, PMIX_UINT32, &v_params);
        free(val);
        TEST_VERBOSE(("after PMIX_NODEID check for rank = %d", peer_proc.rank));

        PMIX_LOAD_KEY(cache, PMIX_NODE_RANK);
        PMIXT_CHECK(PMIx_Get(&peer_proc, cache, NULL, 0, &val), l_params, v_params);
        pmixt_validate_predefined(&peer_proc, cache, val, PMIX_UINT16, &v_params);
        free(val);
        TEST_VERBOSE(("after PMIX_NODE_RANK check for rank = %d", peer_proc.rank));

        PMIX_LOAD_KEY(cache, PMIX_HOSTNAME);
        PMIXT_CHECK(PMIx_Get(&peer_proc, cache, NULL, 0, &val), l_params, v_params);
        pmixt_validate_predefined(&peer_proc, cache, val, PMIX_STRING, &v_params);
        free(val);
        TEST_VERBOSE(("after PMIX_HOSTNAME check for rank = %d", peer_proc.rank));
    }

    /* finalize */
    PMIXT_CHECK(PMIx_Finalize(NULL, 0), l_params, v_params);

    /* Handles cleanup */
    pmixt_post_finalize(&this_proc, &l_params, &v_params);
}
