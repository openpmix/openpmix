/*
 * Copyright (c) 2015-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2022      Nanook Consulting.  All rights reserved.
 * Copyright (c) 2022-2023 Triad National Security, LLC. All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#ifndef PMIX_GDS_SHMEM_H
#define PMIX_GDS_SHMEM_H

#include "pmix_config.h"

#include "include/pmix_common.h"

#include "src/include/pmix_globals.h"
#include "src/util/pmix_shmem.h"
#include "src/mca/gds/base/base.h"

#ifdef HAVE_STDINT_h
#include <stdint.h>
#endif

/**
 * The name of this module.
 */
#define PMIX_GDS_SHMEM_NAME "shmem"

/**
 * Set to 1 to completely disable this component.
 */
#define PMIX_GDS_SHMEM_DISABLE 0

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
 * Stores MCA parameter value for segment_size_multiplier.
 */
PMIX_EXPORT extern double pmix_gds_shmem_segment_size_multiplier;

/**
 * IDs for pmix_shmem_ts in pmix_gds_shmem_job_t.
 */
typedef enum {
    PMIX_GDS_SHMEM_JOB_ID = 0,
    PMIX_GDS_SHMEM_SESSION_ID,
    PMIX_GDS_SHMEM_MODEX_ID,
    PMIX_GDS_SHMEM_INVALID_ID
} pmix_gds_shmem_job_shmem_id_t;

/**
 * Bitmap container for flags associated with a pmix_shmem_t structure.
 */
typedef uint8_t pmix_gds_shmem_status_t;

typedef enum {
    /** Indicates that caller is shmem creator. */
    PMIX_GDS_SHMEM_MINE = 0x01,
    /** Indicates that the shared-memory segment is attached to. */
    PMIX_GDS_SHMEM_ATTACHED = 0x02,
    /** Indicates that the shared-memory segment is ready for use. */
    PMIX_GDS_SHMEM_READY_FOR_USE = 0x04
} pmix_gds_shmem_status_flag_t;

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
    /** Shared-memory allocator for data this structure. */
    pmix_tma_t tma;
    /** Holds the current address of the shared-memory allocator. */
    void *current_addr;
    /** Session ID. */
    uint32_t id;
    /** Session information. */
    pmix_list_t *sessioninfo;
    /** Node information. */
    pmix_list_t *nodeinfo;
} pmix_gds_shmem_shared_session_data_t;

typedef struct {
    pmix_list_item_t super;
    /** Shared-memory object that maintains backing store for session data. */
    pmix_shmem_t *shmem;
    /** Stores status for shmem. */
    pmix_gds_shmem_status_t shmem_status;
    /** Session data stored in shared-memory. */
    pmix_gds_shmem_shared_session_data_t *smdata;
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
    /** List containing job information. */
    pmix_list_t *jobinfo;
    /** List containing this job's node information. */
    pmix_list_t *nodeinfo;
    /** List of applications in this job. */
    pmix_list_t *appinfo;
    /** Stores static local (node) job data. */
    pmix_hash_table_t *local_hashtab;
} pmix_gds_shmem_shared_job_data_t;

typedef struct {
    /** Shared-memory allocator for data this structure. */
    pmix_tma_t tma;
    /** Holds the current address of the shared-memory allocator. */
    void *current_addr;
    /** Stores static modex data. */
    pmix_hash_table_t *hashtab;
} pmix_gds_shmem_shared_modex_data_t;

typedef struct {
    pmix_list_item_t super;
    /** User ID */
    uid_t uid;
    /** Group ID */
    gid_t gid;
    /** Change owner? */
    bool chown;
    /** Change group? */
    bool chgrp;
    /** Namespace identifier (name). */
    char *nspace_id;
    /** Pointer to the namespace. */
    pmix_namespace_t *nspace;
    /** Pointer to this job's session information. */
    pmix_gds_shmem_session_t *session;
    /** Stores status for shmem. */
    pmix_gds_shmem_status_t shmem_status;
    /** Shared-memory object that maintains backing store for smdata data. */
    pmix_shmem_t *shmem;
    /** Stores status for modex_shmem. */
    pmix_gds_shmem_status_t modex_shmem_status;
    /** Shared-memory object that maintains backing store for smmodex data. */
    pmix_shmem_t *modex_shmem;
    /** Points to shared job data located in a shared-memory segment. */
    pmix_gds_shmem_shared_job_data_t *smdata;
    /** Points to shared modex data located in a shared-memory segment. */
    pmix_gds_shmem_shared_modex_data_t *smmodex;
    /** Packed connection information to this segment. */
    pmix_buffer_t *conni;
} pmix_gds_shmem_job_t;
PMIX_EXPORT PMIX_CLASS_DECLARATION(pmix_gds_shmem_job_t);

typedef struct {
    pmix_list_item_t super;
    /** Application number. */
    uint32_t appnum;
    /** Application info. */
    pmix_list_t *appinfo;
    /** Node information. */
    pmix_list_t *nodeinfo;
    /* Application job info. */
    pmix_gds_shmem_job_t *job;
} pmix_gds_shmem_app_t;
PMIX_CLASS_DECLARATION(pmix_gds_shmem_app_t);

END_C_DECLS

#endif

/*
 * vim: ft=cpp ts=4 sts=4 sw=4 expandtab
 */
