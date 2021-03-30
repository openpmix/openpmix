/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2004-2008 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2005 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2015      Los Alamos National Security, LLC. All rights
 *                         reserved.
 * Copyright (c) 2016-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2021      Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * These symbols are in a file by themselves to provide nice linker
 * semantics.  Since linkers generally pull in symbols by object
 * files, keeping these symbols as the only symbols in this file
 * prevents utility programs such as "ompi_info" from having to import
 * entire components just to query their version and parameters.
 */

#include "src/include/pmix_config.h"
#include "include/pmix_common.h"

#include "pnet_sshot.h"
#include "src/mca/pnet/pnet.h"
#include "src/util/argv.h"
#include "src/util/show_help.h"

static pmix_status_t component_query(pmix_mca_base_module_t **module, int *priority);
static pmix_status_t component_register(void);

/*
 * Instantiate the public struct with all of our public information
 * and pointers to our public functions in it
 */
pmix_pnet_sshot_component_t mca_pnet_sshot_component = {
    .super = {
        .base = {
            PMIX_PNET_BASE_VERSION_1_0_0,

            /* Component name and version */
            .pmix_mca_component_name = "sshot",
            PMIX_MCA_BASE_MAKE_VERSION(component,
                                       PMIX_MAJOR_VERSION,
                                       PMIX_MINOR_VERSION,
                                       PMIX_RELEASE_VERSION),

            /* Component open and close functions */
            .pmix_mca_query_component = component_query,
            .pmix_mca_register_component_params = component_register
        },
        .data = {
            /* The component is checkpoint ready */
            PMIX_MCA_BASE_METADATA_PARAM_CHECKPOINT
        }
    },
    .configfile = NULL,
    .numnodes = 0,
    .numdevs = 8,
    .ppn = 4
};

static pmix_status_t component_register(void)
{
    pmix_mca_base_component_t *component = &mca_pnet_sshot_component.super.base;

    (void) pmix_mca_base_component_var_register(
        component, "config_file", "Path of file containing Slingshot fabric configuration",
        PMIX_MCA_BASE_VAR_TYPE_STRING, NULL, 0, 0, PMIX_INFO_LVL_2,
        PMIX_MCA_BASE_VAR_SCOPE_READONLY, &mca_pnet_sshot_component.configfile);

    (void) pmix_mca_base_component_var_register(component, "num_nodes",
                                                "Number of nodes to simulate (0 = no simulation)",
                                                PMIX_MCA_BASE_VAR_TYPE_INT, NULL, 0, 0,
                                                PMIX_INFO_LVL_2, PMIX_MCA_BASE_VAR_SCOPE_READONLY,
                                                &mca_pnet_sshot_component.numnodes);
    (void) pmix_mca_base_component_var_register(
        component, "devs_per_node", "Number of devices/node to simulate (0 = no simulation)",
        PMIX_MCA_BASE_VAR_TYPE_INT, NULL, 0, 0, PMIX_INFO_LVL_2, PMIX_MCA_BASE_VAR_SCOPE_READONLY,
        &mca_pnet_sshot_component.numdevs);

    (void) pmix_mca_base_component_var_register(component, "ppn", "PPN to simulate",
                                                PMIX_MCA_BASE_VAR_TYPE_INT, NULL, 0, 0,
                                                PMIX_INFO_LVL_2, PMIX_MCA_BASE_VAR_SCOPE_READONLY,
                                                &mca_pnet_sshot_component.ppn);

    return PMIX_SUCCESS;
}

static pmix_status_t component_query(pmix_mca_base_module_t **module, int *priority)
{
    *priority = 0;

    if (!PMIX_PEER_IS_SERVER(pmix_globals.mypeer)) {
        /* only servers are supported */
        *module = NULL;
        return PMIX_ERROR;
    }

    *module = (pmix_mca_base_module_t *) &pmix_sshot_module;
    return PMIX_SUCCESS;
}
