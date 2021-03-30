/*
 * Copyright (c) 2020      Triad National Security, LLC.
 *                         All rights reserved.
 *
 * Copyright (c) 2021      Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/* Basic test of PMIx_* functionality: Init, Finalize */

#include "pmix.h"
#include "test_common.h"

int main(int argc, char *argv[])
{

    pmix_proc_t this_proc;
    size_t ninfo = 0;
    test_params params;
    validation_params v_params;

    /*
        // for debugging frees down in clients
        setenv("MALLOC_CHECK_", "3", 1);
    */

    /* Handles all setup that's required prior to calling PMIx_Init() */
    pmixt_pre_init(argc, argv, &params, &v_params);

    /* initialization */
    PMIXT_CHECK(PMIx_Init(&this_proc, NULL, ninfo), params, v_params);

    /* Handles everything that needs to happen after PMIx_Init() */
    pmixt_post_init(&this_proc, &params, &v_params);

    /* finalize */
    PMIXT_CHECK(PMIx_Finalize(NULL, 0), params, v_params);

    /* Handles cleanup */
    pmixt_post_finalize(&this_proc, &params, &v_params);
}
