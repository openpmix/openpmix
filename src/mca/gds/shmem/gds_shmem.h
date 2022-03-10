/*
 * Copyright (c) 2015-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2022      Nanook Consulting.  All rights reserved.
 * Copyright (c) 2022      Triad National Security, LLC. All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#ifndef PMIX_GDS_SHMEM_H
#define PMIX_GDS_SHMEM_H

#include "src/include/pmix_config.h"

#include "src/mca/gds/gds.h"
#include "src/class/pmix_pointer_array.h"

#include "src/util/pmix_shmem.h"
#include "src/util/pmix_vmem.h"

/**
 * The name of this module.
 */
#define PMIX_GDS_SHMEM_NAME "shmem"

/**
 * Default component/module priority.
 */
#define PMIX_GDS_DEFAULT_PRIORITY 0

BEGIN_C_DECLS

typedef struct {
    pmix_gds_base_component_t super;
    pmix_pointer_array_t assignments;
} pmix_gds_shmem_component_t;

/* The component must be visible data for the linker to find it. */
PMIX_EXPORT extern pmix_gds_shmem_component_t pmix_mca_gds_shmem_component;
extern pmix_gds_base_module_t pmix_shmem_module;

typedef struct {
    pmix_object_t super;
    pmix_shmem_t shmem;
    pmix_tma_t tma;
} pmix_shmem_vmem_tracker_t;
PMIX_EXPORT PMIX_CLASS_DECLARATION(pmix_shmem_vmem_tracker_t);

END_C_DECLS

#endif
