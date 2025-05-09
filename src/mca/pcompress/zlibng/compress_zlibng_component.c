/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2004-2010 The Trustees of Indiana University.
 *                         All rights reserved.
 * Copyright (c) 2015      Los Alamos National Security, LLC. All rights
 *                         reserved.
 * Copyright (c) 2019-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2021-2025 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "pmix_config.h"

#include "compress_zlibng.h"
#include "pmix_common.h"
#include "src/mca/pcompress/base/base.h"

/*
 * Public string for version number
 */
const char *pmix_compress_zlibng_component_version_string
    = "PMIX COMPRESS zlibng MCA component version " PMIX_VERSION;

/*
 * Local functionality
 */
static int compress_zlibng_query(pmix_mca_base_module_t **module, int *priority);

/*
 * Instantiate the public struct with all of our public information
 * and pointer to our public functions in it
 */
PMIX_EXPORT pmix_mca_base_component_t pmix_mca_pcompress_zlibng_component = {
    /* Handle the general mca_component_t struct containing
     *  meta information about the component zlibng
     */
    PMIX_COMPRESS_BASE_VERSION_2_0_0,

    /* Component name and version */
    .pmix_mca_component_name = "zlibng",
    PMIX_MCA_BASE_MAKE_VERSION(component, PMIX_MAJOR_VERSION, PMIX_MINOR_VERSION,
                               PMIX_RELEASE_VERSION),

    /* Component open and close functions */
    .pmix_mca_query_component = compress_zlibng_query
};
PMIX_MCA_BASE_COMPONENT_INIT(pmix, pcompress, zlibng)

static int compress_zlibng_query(pmix_mca_base_module_t **module, int *priority)
{
    *module = (pmix_mca_base_module_t *) &pmix_pcompress_zlibng_module;
    *priority = 75;

    return PMIX_SUCCESS;
}
