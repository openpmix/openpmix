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
 */

#include "src/include/pmix_config.h"
#include "include/pmix_common.h"

#include "ploc_hwloc.h"
#include "src/mca/ploc/ploc.h"

static pmix_status_t component_query(pmix_mca_base_module_t **module, int *priority);
static int component_register(void);

/*
 * Instantiate the public struct with all of our public information
 * and pointers to our public functions in it
 */
pmix_ploc_hwloc_component_t mca_ploc_hwloc_component = {
    .super = {
        .base = {
            PMIX_PLOC_BASE_VERSION_1_0_0,

            /* Component name and version */
            .pmix_mca_component_name = "hwloc",
            PMIX_MCA_BASE_MAKE_VERSION(component,
                                       PMIX_MAJOR_VERSION,
                                       PMIX_MINOR_VERSION,
                                       PMIX_RELEASE_VERSION),

            /* Component open and close functions */
            .pmix_mca_query_component = component_query,
            .pmix_mca_register_component_params = component_register,
        },
        .data = {
            /* The component is checkpoint ready */
            PMIX_MCA_BASE_METADATA_PARAM_CHECKPOINT
        }
    },
    .hole_kind = VM_HOLE_BIGGEST,
    .topo_file = NULL,
    .testcpuset = NULL
};

static char *vmhole = "biggest";

static int component_register(void)
{
    pmix_mca_base_component_t *component = &mca_ploc_hwloc_component.super.base;

    (void) pmix_mca_base_component_var_register(
        component, "hole_kind",
        "Kind of VM hole to identify - none, begin, biggest, libs, heap, stack (default=biggest)",
        PMIX_MCA_BASE_VAR_TYPE_STRING, NULL, 0, PMIX_MCA_BASE_VAR_FLAG_NONE, PMIX_INFO_LVL_9,
        PMIX_MCA_BASE_VAR_SCOPE_READONLY, &vmhole);
    if (0 == strcasecmp(vmhole, "none")) {
        mca_ploc_hwloc_component.hole_kind = VM_HOLE_NONE;
    } else if (0 == strcasecmp(vmhole, "begin")) {
        mca_ploc_hwloc_component.hole_kind = VM_HOLE_BEGIN;
    } else if (0 == strcasecmp(vmhole, "biggest")) {
        mca_ploc_hwloc_component.hole_kind = VM_HOLE_BIGGEST;
    } else if (0 == strcasecmp(vmhole, "libs")) {
        mca_ploc_hwloc_component.hole_kind = VM_HOLE_IN_LIBS;
    } else if (0 == strcasecmp(vmhole, "heap")) {
        mca_ploc_hwloc_component.hole_kind = VM_HOLE_AFTER_HEAP;
    } else if (0 == strcasecmp(vmhole, "stack")) {
        mca_ploc_hwloc_component.hole_kind = VM_HOLE_BEFORE_STACK;
    } else {
        pmix_output(0, "INVALID VM HOLE TYPE");
        return PMIX_ERROR;
    }

    (void) pmix_mca_base_component_var_register(
        component, "topo_file",
        "Topology file to use instead of discovering it (mostly for testing purposes)",
        PMIX_MCA_BASE_VAR_TYPE_STRING, NULL, 0, PMIX_MCA_BASE_VAR_FLAG_NONE, PMIX_INFO_LVL_9,
        PMIX_MCA_BASE_VAR_SCOPE_READONLY, &mca_ploc_hwloc_component.topo_file);

    (void) pmix_mca_base_component_var_register(component, "test_cpuset",
                                                "Cpuset for testing purposes",
                                                PMIX_MCA_BASE_VAR_TYPE_STRING, NULL, 0,
                                                PMIX_MCA_BASE_VAR_FLAG_NONE, PMIX_INFO_LVL_9,
                                                PMIX_MCA_BASE_VAR_SCOPE_READONLY,
                                                &mca_ploc_hwloc_component.testcpuset);

    return PMIX_SUCCESS;
}

static pmix_status_t component_query(pmix_mca_base_module_t **module, int *priority)
{
    *priority = 100;
    *module = (pmix_mca_base_module_t *) &pmix_ploc_hwloc_module;
    return PMIX_SUCCESS;
}
