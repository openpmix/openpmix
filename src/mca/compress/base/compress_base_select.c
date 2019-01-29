/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2004-2010 The Trustees of Indiana University.
 *                         All rights reserved.
 * Copyright (c) 2015      Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 *
 * Copyright (c) 2015      Los Alamos National Security, LLC. All rights
 *                         reserved.
 * Copyright (c) 2019      Intel, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "pmix_config.h"

#ifdef HAVE_UNISTD_H
#include "unistd.h"
#endif

#include "pmix_common.h"
#include "src/util/output.h"
#include "src/mca/mca.h"
#include "src/mca/base/base.h"
#include "src/mca/compress/compress.h"
#include "src/mca/compress/base/base.h"

int pmix_compress_base_select(void)
{
    int ret, exit_status = PMIX_SUCCESS;
    pmix_compress_base_component_t *best_component = NULL;
    pmix_compress_base_module_t *best_module = NULL;

    /*
     * Select the best component
     */
    if( PMIX_SUCCESS != pmix_mca_base_select("compress", pmix_compress_base_framework.framework_output,
                                             &pmix_compress_base_framework.framework_components,
                                             (pmix_mca_base_module_t **) &best_module,
                                             (pmix_mca_base_component_t **) &best_component, NULL) ) {
        /* This will only happen if no component was selected */
        exit_status = PMIX_ERROR;
        goto cleanup;
    }

    /* Save the winner */
    pmix_compress_base_selected_component = *best_component;

    /* Initialize the winner */
    if (NULL != best_module) {
        if (PMIX_SUCCESS != (ret = best_module->init()) ) {
            exit_status = ret;
            goto cleanup;
        }
        pmix_compress = *best_module;
    }

 cleanup:
    return exit_status;
}
