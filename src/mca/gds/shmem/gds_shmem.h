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

#include "src/mca/gds/base/base.h"
#include "src/mca/gds/gds.h"

#include "include/pmix_common.h"

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
 * Environment variables used to find shared-memory segment info.
 */
#define PMIX_GDS_SHMEM_ENVVAR_SEG_PATH "PMIX_GDS_SHMEM_SEG_PATH"
#define PMIX_GDS_SHMEM_ENVVAR_SEG_SIZE "PMIX_GDS_SHMEM_SEG_SIZE"
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

extern pmix_gds_base_module_t pmix_shmem_module;

typedef struct {
    pmix_gds_base_component_t super;
    /** List of jobs that I'm supporting. */
    pmix_list_t jobs;
    /** List of sessions that I'm supporting */
    pmix_list_t sessions;
} pmix_gds_shmem_component_t;
// The component must be visible data for the linker to find it.
PMIX_EXPORT extern pmix_gds_shmem_component_t pmix_mca_gds_shmem_component;

typedef struct {
    pmix_list_item_t super;
    uint32_t nodeid;
    char *hostname;
    char **aliases;
    pmix_list_t info;
} pmix_gds_shmem_nodeinfo_t;
PMIX_CLASS_DECLARATION(pmix_gds_shmem_nodeinfo_t);

typedef struct {
    pmix_list_item_t super;
    /** Session ID. */
    uint32_t id;
    pmix_list_t sessioninfo;
    pmix_list_t nodeinfo;
} pmix_gds_shmem_session_t;
PMIX_EXPORT PMIX_CLASS_DECLARATION(pmix_gds_shmem_session_t);

typedef struct {
    pmix_list_item_t super;
    /** Namespace identifier. */
    char *nspace_id;
    /** Pointer to the namespace. */
    pmix_namespace_t *nspace;
    // TODO(skg) Is the session nodeinfo the same info?
    /** Node information. */
    pmix_list_t nodeinfo;
    /** Session information. */
    pmix_gds_shmem_session_t *session;
    /** List of applications in this job. */
    pmix_list_t apps;
    /** Stores internal data. */
    pmix_hash_table_t hashtab_internal;
    /** Stores local data. */
    pmix_hash_table_t hashtab_local;
    /** Stores remote data. */
    pmix_hash_table_t hashtab_remote;
    /** Shared-memory object. */
    pmix_shmem_t *shmem;
    /** Shared-memory allocator. */
    pmix_tma_t tma;
} pmix_gds_shmem_job_t;
PMIX_EXPORT PMIX_CLASS_DECLARATION(pmix_gds_shmem_job_t);

typedef struct {
    pmix_list_item_t super;
    uint32_t appnum;
    pmix_list_t appinfo;
    pmix_list_t nodeinfo;
    pmix_gds_shmem_job_t *job;
} pmix_gds_shmem_app_t;
PMIX_CLASS_DECLARATION(pmix_gds_shmem_app_t);

END_C_DECLS

#endif

/*
 * vim: ft=cpp ts=4 sts=4 sw=4 expandtab
 */
