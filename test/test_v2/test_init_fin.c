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

/* Basic test of PMIx_* functionality: Init, Finalize */

#include "pmix.h"
#include "test_common.h"
#include <assert.h>
#include <sys/time.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>

extern FILE *file;

int main(int argc, char *argv[]) {

    pmix_proc_t this_proc;
    size_t ninfo = 0;
    test_params params;
    validation_params v_params;
    int i;

    for (i = 0; i < 2; i++) {
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
}
