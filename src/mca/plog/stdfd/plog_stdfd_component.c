/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2018-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2021-2025 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/*
 * includes
 */
#include "pmix_config.h"
#include "pmix_common.h"

#include "plog_stdfd.h"

static pmix_status_t component_query(pmix_mca_base_module_t **module, int *priority);

/*
 * Struct of function pointers that need to be initialized
 */
pmix_plog_base_component_t pmix_mca_plog_stdfd_component = {
    PMIX_PLOG_BASE_VERSION_1_0_0,

    .pmix_mca_component_name = "stdfd",
    PMIX_MCA_BASE_MAKE_VERSION(component, PMIX_MAJOR_VERSION, PMIX_MINOR_VERSION,
                          PMIX_RELEASE_VERSION),
    .pmix_mca_query_component = component_query,
};
PMIX_MCA_BASE_COMPONENT_INIT(pmix, plog, stdfd)

static pmix_status_t component_query(pmix_mca_base_module_t **module, int *priority)
{
    *priority = 5;
    *module = (pmix_mca_base_module_t *) &pmix_plog_stdfd_module;
    return PMIX_SUCCESS;
}
