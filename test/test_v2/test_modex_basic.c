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

static bool flag = false;
static char test_arg_str[]="--pmix-collect-data";

static int parse_client_args(int *index, int argc, char **argv, test_params *lparams, validation_params *v_params)
{
    PMIX_HIDE_UNUSED_PARAMS(argc, v_params, lparams);

    if (0 == strcmp(argv[*index], test_arg_str)) {
        flag = true;
    }
    return PMIX_SUCCESS;
}

int main(int argc, char *argv[]) {

    int rc, i;
    size_t ninfo = 0;
    test_params l_params;
    validation_params v_params;
    pmix_proc_t this_proc, peer_proc, job_proc;
    char key[PMIX_MAX_KEYLEN];
    pmix_value_t val, *gvalue;
    pmix_info_t *info;
    uint32_t num_procs;

    pmixt_pre_init(argc, argv, &l_params, &v_params, &parse_client_args);
    /* initialization */
    PMIXT_CHECK(PMIx_Init(&this_proc, NULL, ninfo), l_params, v_params);

    /* Handles everything that needs to happen after PMIx_Init() */
    pmixt_post_init(&this_proc, &l_params, &v_params);

    pmix_strncpy(job_proc.nspace, this_proc.nspace, PMIX_MAX_NSLEN);
    job_proc.rank = PMIX_RANK_WILDCARD;

    PMIXT_CHECK(PMIx_Get(&job_proc, PMIX_JOB_SIZE, NULL, 0, &gvalue), l_params, v_params);
    PMIX_VALUE_GET_NUMBER(rc, gvalue, num_procs, uint32_t);
    PMIX_VALUE_RELEASE(gvalue);

    rc = snprintf(key, sizeof(key), "value0-%s-%d", this_proc.nspace, this_proc.rank);
    if (rc >= sizeof(key)) {
        TEST_ERROR_EXIT(("%s: Rank %d key too large",
                          this_proc.nspace, this_proc.rank));
    }

    val.type = PMIX_UINT32;
    val.data.uint32 = 5000 + (uint32_t)this_proc.rank;
    PMIXT_CHECK(PMIx_Put(PMIX_GLOBAL, key, &val), l_params, v_params);

    PMIXT_CHECK(PMIx_Commit(), l_params, v_params);

    PMIX_INFO_CREATE(info, 1);
    ninfo = 1;
    if (flag) {
        TEST_VERBOSE(("Using fence with full data exchange"));
    }

    PMIX_INFO_LOAD(info, PMIX_COLLECT_DATA, &flag, PMIX_BOOL);
    PMIXT_CHECK(PMIx_Fence(NULL, 0, info, 1), l_params, v_params);
    PMIX_INFO_FREE(info, 1);
    TEST_VERBOSE(("Rank %u has exited the fence", this_proc.rank));

    for (i = 0; i < (int)num_procs; i++) {
        peer_proc = this_proc;
        peer_proc.rank = i;
        (void) snprintf(key, sizeof(key), "value0-%s-%d", this_proc.nspace, i);
        PMIXT_CHECK(PMIx_Get(&peer_proc, key, NULL, 0, &gvalue), l_params, v_params);
        if (gvalue->type != PMIX_UINT32) {
            TEST_ERROR_EXIT(("wrong value type gotten for key %s", key));
        }
        if (gvalue->data.uint32 != 5000 + (uint32_t)i) {
            TEST_ERROR_EXIT(("wrong value gotten for key %s - %u", key, gvalue->data.uint32));
        }
        PMIX_VALUE_RELEASE(gvalue);
        TEST_VERBOSE(("rank %u GOT from rank %u", this_proc.rank, (uint32_t)i));
    }

    /* finalize */
    PMIXT_CHECK(PMIx_Finalize(NULL, 0), l_params, v_params);

    /* Handles cleanup */
    pmixt_post_finalize(&this_proc, &l_params, &v_params);
}
