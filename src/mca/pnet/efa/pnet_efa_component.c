/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * These symbols are in a file by themselves to provide nice linker
 * semantics.  Since linkers generally pull in symbols by object
 * files, keeping these symbols as the only symbols in this file
 * prevents utility programs such as "pmix_info" from having to import
 * entire components just to query their version and parameters.
 */

#include "src/include/pmix_config.h"
#include "pmix_common.h"

#include "src/mca/pnet/base/base.h"

#include "pnet_efa.h"

static pmix_status_t component_open(void);
static pmix_status_t component_query(pmix_mca_base_module_t **module, int *priority);

/*
 * Instantiate the public struct with all of our public information
 * and pointers to our public functions in it
 */
pmix_pnet_efa_component_t pmix_mca_pnet_efa_component = {
    .super = {
        PMIX_MCA_BASE_VERSION(pnet),

        /* Component name and version */
        .pmix_mca_component_name = "efa",
        PMIX_MCA_BASE_MAKE_VERSION(component,
                                   PMIX_MAJOR_VERSION,
                                   PMIX_MINOR_VERSION,
                                   PMIX_RELEASE_VERSION),

        /* Component open and close functions */
        .pmix_mca_open_component = component_open,
        .pmix_mca_query_component = component_query,
    }
};
PMIX_MCA_BASE_COMPONENT_INIT(pmix, pnet, efa)

/* Open only on a node carrying an EFA device, asked through the same
 * matching setup_fork filters an assignment with. */
static pmix_status_t component_open(void)
{
    return pmix_pnet_base_check_devices(pmix_pnet_efa_match, pmix_pnet_efa_nmatch);
}

static pmix_status_t component_query(pmix_mca_base_module_t **module, int *priority)
{
    *priority = 10;
    *module = (pmix_mca_base_module_t *) &pmix_pnet_efa_module;
    return PMIX_SUCCESS;
}
