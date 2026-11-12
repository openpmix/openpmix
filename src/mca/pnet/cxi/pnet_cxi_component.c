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

#include "pnet_cxi.h"

static pmix_status_t component_open(void);
static pmix_status_t component_query(pmix_mca_base_module_t **module, int *priority);
static pmix_status_t component_register(void);

/*
 * Instantiate the public struct with all of our public information
 * and pointers to our public functions in it
 */
pmix_pnet_cxi_component_t pmix_mca_pnet_cxi_component = {
    .super = {
        PMIX_MCA_BASE_VERSION(pnet),

        /* Component name and version */
        .pmix_mca_component_name = "cxi",
        PMIX_MCA_BASE_MAKE_VERSION(component,
                                   PMIX_MAJOR_VERSION,
                                   PMIX_MINOR_VERSION,
                                   PMIX_RELEASE_VERSION),

        /* Component open and close functions */
        .pmix_mca_open_component = component_open,
        .pmix_mca_register_component_params = component_register,
        .pmix_mca_query_component = component_query,
    },
    .sysfs_root = NULL
};
PMIX_MCA_BASE_COMPONENT_INIT(pmix, pnet, cxi)

static pmix_status_t component_register(void)
{
    pmix_mca_pnet_cxi_component.sysfs_root = "/sys";
    (void) pmix_mca_base_component_var_register(
        &pmix_mca_pnet_cxi_component.super, "sysfs_root",
        "Root of the sysfs tree in which a Slingshot interface's CXI device is "
        "found when the topology does not report it (default: /sys). Exists so "
        "the lookup can be run against a captured tree",
        PMIX_MCA_BASE_VAR_TYPE_STRING,
        &pmix_mca_pnet_cxi_component.sysfs_root);
    return PMIX_SUCCESS;
}

/* Open only on a node carrying a Slingshot NIC, asked through the same
 * matching setup_fork filters an assignment with. */
static pmix_status_t component_open(void)
{
    return pmix_pnet_base_check_devices(pmix_pnet_cxi_match, pmix_pnet_cxi_nmatch);
}

static pmix_status_t component_query(pmix_mca_base_module_t **module, int *priority)
{
    *priority = 10;
    *module = (pmix_mca_base_module_t *) &pmix_pnet_cxi_module;
    return PMIX_SUCCESS;
}
