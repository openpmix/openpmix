/*
 * Copyright (c) 2004-2010 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2011 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2013-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "src/include/pmix_config.h"
#include "pmix_common.h"

#include <string.h>

#include "src/mca/ptl/base/base.h"
#include "src/mca/ptl/tcp/ptl_tcp.h"

static int component_query(pmix_mca_base_module_t **module, int *priority);

/* TCP is always available. Its parameters are registered by the base,
 * under the ptl_base_* names they have always had - the ptl_tcp_* names
 * are their deprecated synonyms */
PMIX_EXPORT pmix_ptl_tcp_component_t pmix_mca_ptl_tcp_component = {
    .super = {
        .base = {
            PMIX_MCA_BASE_VERSION(ptl),

            /* Component name and version */
            .pmix_mca_component_name = "tcp",
            PMIX_MCA_BASE_MAKE_VERSION(component, PMIX_MAJOR_VERSION, PMIX_MINOR_VERSION,
                                       PMIX_RELEASE_VERSION),

            .pmix_mca_query_component = component_query
        },
        .priority = 30
    }
};
PMIX_MCA_BASE_COMPONENT_INIT(pmix, ptl, tcp)

static int component_query(pmix_mca_base_module_t **module, int *priority)
{
    *priority = pmix_mca_ptl_tcp_component.super.priority;
    *module = (pmix_mca_base_module_t *) &pmix_ptl_tcp_module;
    return PMIX_SUCCESS;
}
