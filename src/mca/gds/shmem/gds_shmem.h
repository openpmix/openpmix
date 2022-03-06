/*
 * Copyright (c) 2015-2020 Intel, Inc.  All rights reserved.
 *
 * Copyright (c) 2022      Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#ifndef PMIX_GDS_SHMEM_H
#define PMIX_GDS_SHMEM_H

#include "src/include/pmix_config.h"

#include "src/class/pmix_pointer_array.h"
#include "src/mca/gds/gds.h"

BEGIN_C_DECLS

typedef struct {
    pmix_gds_base_component_t base;
    pmix_pointer_array_t assignments;
} pmix_gds_shmem_component_t;

/* the component must be visible data for the linker to find it */
PMIX_EXPORT extern pmix_gds_shmem_component_t pmix_mca_gds_shmem_component;
extern pmix_gds_base_module_t pmix_shmem_module;

END_C_DECLS

#endif
