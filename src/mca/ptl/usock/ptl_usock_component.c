/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "src/include/pmix_config.h"
#include "pmix_common.h"

#include "src/mca/ptl/base/base.h"
#include "src/mca/ptl/usock/ptl_usock.h"

static int component_query(pmix_mca_base_module_t **module, int *priority);

/* usock is always available, and is every process's local transport */
PMIX_EXPORT pmix_ptl_usock_component_t pmix_mca_ptl_usock_component = {
    .super = {
        .base = {
            PMIX_MCA_BASE_VERSION(ptl),

            /* Component name and version */
            .pmix_mca_component_name = "usock",
            PMIX_MCA_BASE_MAKE_VERSION(component, PMIX_MAJOR_VERSION, PMIX_MINOR_VERSION,
                                       PMIX_RELEASE_VERSION),

            .pmix_mca_query_component = component_query
        },
        .priority = 50
    },
    .dir = NULL,
    .path = NULL,
    .dfd = -1
};
PMIX_MCA_BASE_COMPONENT_INIT(pmix, ptl, usock)

static int component_query(pmix_mca_base_module_t **module, int *priority)
{
    *priority = pmix_mca_ptl_usock_component.super.priority;
    *module = (pmix_mca_base_module_t *) &pmix_ptl_usock_module;
    return PMIX_SUCCESS;
}
