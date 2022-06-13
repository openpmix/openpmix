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

#include "src/mca/gds/base/base.h"

#include "src/class/pmix_object.h"
#include "src/class/pmix_list.h"

#include "src/util/pmix_shmem.h"
#include "src/util/pmix_vmem.h"

#include "src/mca/preg/preg.h"
#include "src/mca/gds/gds.h"

#include "include/pmix_common.h"

// TODO(skg) Remove
#include "pmix_hash_table2.h"

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

/**
 * The name of this module.
 */
#define PMIX_GDS_SHMEM_NAME "shmem"

/**
 * Set to 1 to completely disable this component.
 */
#define PMIX_GDS_SHMEM_DISABLE 1

/**
 * Defines a bitmask to track what information may not
 * have been provided but is computable from other info.
 */
#define PMIX_GDS_SHMEM_PROC_DATA 0x00000001
#define PMIX_GDS_SHMEM_JOB_SIZE  0x00000002
#define PMIX_GDS_SHMEM_MAX_PROCS 0x00000004
#define PMIX_GDS_SHMEM_NUM_NODES 0x00000008
#define PMIX_GDS_SHMEM_PROC_MAP  0x00000010
#define PMIX_GDS_SHMEM_NODE_MAP  0x00000020

/**
 * Default component/module priority.
 */
#if (PMIX_GDS_SHMEM_DISABLE == 1)
#define PMIX_GDS_SHMEM_DEFAULT_PRIORITY 0
#else
// We want to be just above hash's priority.
#define PMIX_GDS_SHMEM_DEFAULT_PRIORITY 20
#endif

BEGIN_C_DECLS

extern pmix_gds_base_module_t pmix_shmem_module;

typedef struct {
    pmix_gds_base_component_t super;
    /** List of jobs that I'm supporting. */
    pmix_list_t jobs;
} pmix_gds_shmem_component_t;
// The component must be visible data for the linker to find it.
PMIX_EXPORT extern
pmix_gds_shmem_component_t pmix_mca_gds_shmem_component;

typedef struct {
    pmix_list_item_t super;
    /** Hostname. */
    char *name;
} pmix_gds_shmem_host_alias_t;
PMIX_CLASS_DECLARATION(pmix_gds_shmem_host_alias_t);

typedef struct {
    pmix_list_item_t super;
    /* Node ID. */
    uint32_t nodeid;
    /** Hostname. */
    char *hostname;
    /** Node name aliases. */
    pmix_list_t *aliases;
    /** Node information. */
    pmix_list_t *info;
} pmix_gds_shmem_nodeinfo_t;
PMIX_CLASS_DECLARATION(pmix_gds_shmem_nodeinfo_t);

// Note that the shared data structures in pmix_gds_shmem_shared_data_t are
// pointers. They need to be because their respective locations must reside on
// the shared heap located in shared-memory and managed by the shared-memory
// TMA.
typedef struct {
    /** Shared-memory allocator. */
    pmix_tma_t tma;
    /** Holds the current address of the shared-memory allocator. */
    void *current_addr;
    /** Node information. */
    pmix_list_t *nodeinfo;
    /** List of applications in this job. */
    pmix_list_t *apps;
    /** Stores local (node) data. */
    pmix_hash_table2_t *local_hashtab;
    /** Job information. */
    pmix_list_t *jobinfo;
} pmix_gds_shmem_shared_data_t;

typedef struct {
    pmix_list_item_t super;
    /** Namespace identifier (name). */
    char *nspace_id;
    /** Pointer to the namespace. */
    pmix_namespace_t *nspace;
    /** Pointer to a full-featured gds module. */
    pmix_gds_base_module_t *ffgds;
    /** Shared-memory object. */
    pmix_shmem_t *shmem;
    /** Points to shared data located in shared-memory segment. */
    pmix_gds_shmem_shared_data_t *smdata;
} pmix_gds_shmem_job_t;
PMIX_EXPORT PMIX_CLASS_DECLARATION(pmix_gds_shmem_job_t);

typedef struct {
    pmix_list_item_t super;
    uint32_t appnum;
    pmix_list_t *appinfo;
    pmix_list_t *nodeinfo;
    pmix_gds_shmem_job_t *job;
} pmix_gds_shmem_app_t;
PMIX_CLASS_DECLARATION(pmix_gds_shmem_app_t);

END_C_DECLS

#endif

/*
 * vim: ft=cpp ts=4 sts=4 sw=4 expandtab
 */
