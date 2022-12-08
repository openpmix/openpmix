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

/**
 * IDs for pmix_shmem_ts in pmix_gds_shmem_job_t.
 */
typedef enum {
    PMIX_GDS_SHMEM_JOB_SHMEM_JOB,
    PMIX_GDS_SHMEM_JOB_SHMEM_MODEX,
    PMIX_GDS_SHMEM_JOB_SHMEM_INVALID
} pmix_gds_shmem_job_shmem_id_t;

typedef struct {
    pmix_gds_base_component_t super;
    /** List of jobs that I'm supporting. */
    pmix_list_t jobs;
    /** List of sessions that I'm supporting. */
    pmix_list_t sessions;
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
    /** Node ID. */
    uint32_t nodeid;
    /** Hostname. */
    char *hostname;
    /** Node name aliases. */
    pmix_list_t *aliases;
    /** Node information. */
    pmix_list_t *info;
} pmix_gds_shmem_nodeinfo_t;
PMIX_CLASS_DECLARATION(pmix_gds_shmem_nodeinfo_t);

typedef struct {
    pmix_list_item_t super;
    /** Session ID. */
    uint32_t session;
    /** Session information. */
    pmix_list_t *sessioninfo;
    /** Node information. */
    pmix_list_t *nodeinfo;
} pmix_gds_shmem_session_t;
PMIX_CLASS_DECLARATION(pmix_gds_shmem_session_t);

/**
 * Shared data structures that reside in shared-memory. The server populates
 * these data and clients are only permitted to read from them.
 *
 * Note that the shared data structures in pmix_gds_shmem_shared_*_data_t are
 * pointers since their respective locations must reside on the shared heap
 * located in shared-memory and managed by a shared-memory TMA.
 */
typedef struct {
    /** Shared-memory allocator for data this structure. */
    pmix_tma_t tma;
    /** Holds the current address of the shared-memory allocator. */
    void *current_addr;
    /** Stores static remote job data. */
    pmix_hash_table2_t *hashtab;
} pmix_gds_shmem_shared_modex_data_t;

typedef struct {
    /** Shared-memory allocator for data this structure. */
    pmix_tma_t tma;
    /** Holds the current address of the shared-memory allocator. */
    void *current_addr;
    /** Points to this job's session information. */
    pmix_gds_shmem_session_t *session;
    /** List containing job information. */
    pmix_list_t *jobinfo;
    /** List containing this job's node information. */
    pmix_list_t *nodeinfo;
    /** List of applications in this job. */
    pmix_list_t *apps;
    /** Stores static local (node) job data. */
    pmix_hash_table2_t *local_hashtab;
} pmix_gds_shmem_shared_job_data_t;

typedef struct {
    pmix_list_item_t super;
    /** Namespace identifier (name). */
    char *nspace_id;
    /** Pointer to the namespace. */
    pmix_namespace_t *nspace;
    /** Flag indicating whether or not to release shmem. */
    bool release_shmem;
    /** Shared-memory object that maintains information for smjob data. */
    pmix_shmem_t *shmem;
    /** Shared-memory object that maintains information for smmodex data. */
    pmix_shmem_t *modex_shmem;
    /** Points to shared data located in shared-memory segment. */
    pmix_gds_shmem_shared_job_data_t *smdata;
    /**
     * Points to a structure that maintains static modex
     * data residing in another shared-memory segment.
     */
    pmix_gds_shmem_shared_modex_data_t *smmodex;
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
