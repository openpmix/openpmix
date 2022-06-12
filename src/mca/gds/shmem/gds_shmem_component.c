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
 * Copyright (c) 2022      Nanook Consulting.  All rights reserved.
 * Copyright (c) 2022      Triad National Security, LLC. All rights reserved.
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

#include "gds_shmem.h"

static int
component_query(
    pmix_mca_base_module_t **module,
    int *priority
) {
    // See if the required system file is present.
    // See pmix_vmem_find_hole() for more information.
    if (access("/proc/self/maps", F_OK) == -1) {
        *priority = 0;
        *module = NULL;
        return PMIX_ERROR;
    }
#if (PMIX_GDS_SHMEM_DISABLE == 1)
    *priority = PMIX_GDS_SHMEM_DEFAULT_PRIORITY;
    *module = NULL;
    return PMIX_ERROR;
#else
    *priority = PMIX_GDS_SHMEM_DEFAULT_PRIORITY;
    *module = (pmix_mca_base_module_t *)&pmix_shmem_module;
    return PMIX_SUCCESS;
#endif
}

/**
 * Instantiate the public struct with all of our public
 * information and pointers to our public functions in it.
 */
pmix_gds_shmem_component_t pmix_mca_gds_shmem_component = {
    .super = {
        PMIX_GDS_BASE_VERSION_1_0_0,
        /** Component name and version. */
        .pmix_mca_component_name = PMIX_GDS_SHMEM_NAME,
        PMIX_MCA_BASE_MAKE_VERSION(
            component,
            PMIX_MAJOR_VERSION,
            PMIX_MINOR_VERSION,
            PMIX_RELEASE_VERSION
        ),
        /** Component query function. */
        .pmix_mca_query_component = component_query,
        .reserved = {0}
    },
    .jobs = PMIX_LIST_STATIC_INIT
};

/*
 * vim: ft=cpp ts=4 sts=4 sw=4 expandtab
 */
