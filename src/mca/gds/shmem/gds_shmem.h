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
#include "src/include/pmix_globals.h"

#include "src/class/pmix_object.h"
#include "src/class/pmix_list.h"

#include "src/util/pmix_shmem.h"
#include "src/util/pmix_vmem.h"

#include "src/mca/gds/gds.h"

/**
 * The name of this module.
 */
#define PMIX_GDS_SHMEM_NAME "shmem"

/**
 * Set to 1 to completely disable this component.
 */
#define PMIX_GDS_SHMEM_DISABLE 1

/**
 * Environment variables used to find shared-memory segment info.
 */
#define PMIX_GDS_SHMEM_ENVVAR_SEG_PATH "PMIX_GDS_SHMEM_SEG_PATH"
#define PMIX_GDS_SHMEM_ENVVAR_SEG_ADDR "PMIX_GDS_SHMEM_SEG_ADDR"

/**
 * Default component/module priority.
 */
#if (PMIX_GDS_SHMEM_DISABLE == 1)
#define PMIX_GDS_SHMEM_DEFAULT_PRIORITY 0
#else
#define PMIX_GDS_SHMEM_DEFAULT_PRIORITY 30
#endif

/**
 * With picky compiler flags PMIX_HIDE_UNUSED_PARAMS doesn't appear to work, so
 * create our own for now. TODO(skg) Report or resolve issue.
 */
#define PMIX_GDS_SHMEM_UNUSED(x)                                               \
do {                                                                           \
    (void)(x);                                                                 \
} while (0)

BEGIN_C_DECLS

typedef struct {
    pmix_gds_base_component_t super;
    /** List of jobs that I'm supporting. */
    pmix_list_t myjobs;
} pmix_gds_shmem_component_t;
/* The component must be visible data for the linker to find it. */
PMIX_EXPORT extern pmix_gds_shmem_component_t pmix_mca_gds_shmem_component;
extern pmix_gds_base_module_t pmix_shmem_module;

// TODO(skg) Make sure all members are necessary.
typedef struct {
    pmix_list_item_t super;
    uint32_t session;
    pmix_list_t sessioninfo;
    pmix_list_t nodeinfo;
} pmix_gds_shmem_session_t;
PMIX_CLASS_DECLARATION(pmix_gds_shmem_session_t);

typedef struct {
    pmix_list_item_t super;
    char *ns;
    pmix_shmem_t *shmem;
    pmix_namespace_t *nptr;
    pmix_gds_shmem_session_t *session;
} pmix_gds_shmem_job_t;
PMIX_CLASS_DECLARATION(pmix_gds_shmem_job_t);

END_C_DECLS

#endif
