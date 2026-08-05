/*
 * Copyright (c) 2015-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2022      Nanook Consulting.  All rights reserved.
 * Copyright (c) 2022-2023 Triad National Security, LLC. All rights reserved.
 * Copyright (c) 2026      Jeff Squyres  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#ifndef PMIX_GDS_SHMEM3_H
#define PMIX_GDS_SHMEM3_H

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
#define PMIX_GDS_SHMEM3_NAME "shmem3"

/**
 * Identifies the in-memory layout of everything this component puts in a
 * shared segment.
 *
 * The server builds these structures inside the segment and its clients read
 * them *in place*, so both ends must lay them out identically. The name of
 * the component guards that across releases - an older client does not
 * recognize "shmem3" and falls back to hash - but it cannot guard two builds
 * that both call themselves shmem3 and still disagree. They disagree more
 * easily than one might expect:
 *
 *   - --enable-debug adds obj_magic_id to the FRONT of pmix_object_t, plus
 *     two fields at the back. A debug server and a default-build client
 *     differ by 24 bytes in the base class alone - pmix_list_t is 248 bytes
 *     against 192, and pmix_list_length sits at offset 240 against 184.
 *   - Any struct here gaining or losing a field during a development series,
 *     without the component being renamed.
 *
 * Both produce exactly the corruption the rename exists to prevent, so the
 * segment carries this ID and a mismatched peer declines it (see
 * pmix_shmem_segment_attach) and falls back.
 *
 * It is COMPUTED, deliberately. A hand-maintained version number is a number
 * someone has to remember to bump, and the history of this component is that
 * it does not get bumped. Distinct prime multipliers keep a growth in one
 * type from cancelling a shrink in another.
 *
 * Extend the list when a new type starts living in the segment. Types
 * reached only through a pointer still count - they are allocated in the
 * segment too. The trailing version term is for changes that alter meaning
 * without altering any size.
 */
#define PMIX_GDS_SHMEM3_LAYOUT_VERSION 1u

#define PMIX_GDS_SHMEM3_LAYOUT_ID                                            \
    ((uint32_t)(                                                             \
        PMIX_GDS_SHMEM3_LAYOUT_VERSION                                       \
      +   3u * (uint32_t)sizeof(pmix_object_t)                               \
      +   5u * (uint32_t)sizeof(pmix_list_t)                                 \
      +   7u * (uint32_t)sizeof(pmix_list_item_t)                            \
      +  11u * (uint32_t)sizeof(pmix_hash_table_t)                           \
      +  13u * (uint32_t)sizeof(pmix_tma_t)                                  \
      +  17u * (uint32_t)sizeof(pmix_value_t)                                \
      +  19u * (uint32_t)sizeof(pmix_info_t)                                 \
      +  23u * (uint32_t)sizeof(pmix_kval_t)                                 \
      +  29u * (uint32_t)sizeof(pmix_gds_shmem3_shared_session_data_t)       \
      +  31u * (uint32_t)sizeof(pmix_gds_shmem3_shared_job_data_t)           \
      +  37u * (uint32_t)sizeof(pmix_gds_shmem3_shared_modex_data_t)         \
      +  41u * (uint32_t)sizeof(pmix_gds_shmem3_nodeinfo_t)                  \
      +  43u * (uint32_t)sizeof(pmix_gds_shmem3_app_t)                       \
      +  47u * (uint32_t)sizeof(pmix_gds_shmem3_host_alias_t)                \
      +  53u * (uint32_t)sizeof(pmix_keyindex_t)                             \
      +  59u * (uint32_t)sizeof(pmix_regattr_input_t)                        \
      +  61u * (uint32_t)sizeof(pmix_pointer_array_t)                        \
    ))

/**
 * Default component/module priority.
 */
#define PMIX_GDS_SHMEM3_DEFAULT_PRIORITY 20

BEGIN_C_DECLS

extern pmix_gds_base_module_t pmix_shmem3_module;

/**
 * Stores MCA parameter value for segment_size_multiplier.
 */
PMIX_EXPORT extern double pmix_gds_shmem3_segment_size_multiplier;

/**
 * Testing-only MCA parameter. When true, a client's attempt to attach a
 * shared-memory segment at its required fixed address is forced to fail,
 * so the graceful fallback to the next GDS module can be exercised in
 * tests without depending on each process's virtual-memory layout. Never
 * set this in production.
 */
PMIX_EXPORT extern bool pmix_gds_shmem3_force_client_attach_failure;

/**
 * IDs for pmix_shmem_ts in pmix_gds_shmem3_job_t.
 */
typedef enum {
    PMIX_GDS_SHMEM3_JOB_ID = 0,
    PMIX_GDS_SHMEM3_SESSION_ID,
    PMIX_GDS_SHMEM3_MODEX_ID,
    PMIX_GDS_SHMEM3_INVALID_ID
} pmix_gds_shmem3_job_shmem3_id_t;

/**
 * Bitmap container for flags associated with a pmix_shmem_t structure.
 */
typedef uint8_t pmix_gds_shmem3_status_t;

typedef enum {
    /** Indicates that caller is shmem3 creator. */
    PMIX_GDS_SHMEM3_MINE = 0x01,
    /** Indicates that the shared-memory segment is attached to. */
    PMIX_GDS_SHMEM3_ATTACHED = 0x02,
    /** Indicates that the shared-memory segment is ready for use. */
    PMIX_GDS_SHMEM3_READY_FOR_USE = 0x04
} pmix_gds_shmem3_status_flag_t;

typedef struct {
    pmix_gds_base_component_t super;
    /** List of jobs that I'm supporting. */
    pmix_list_t jobs;
    /** List of sessions that I'm supporting. */
    pmix_list_t sessions;
} pmix_gds_shmem3_component_t;
// The component must be visible data for the linker to find it.
PMIX_EXPORT extern
pmix_gds_shmem3_component_t pmix_mca_gds_shmem3_component;

typedef struct {
    pmix_list_item_t super;
    /** Hostname. */
    char *name;
} pmix_gds_shmem3_host_alias_t;
PMIX_CLASS_DECLARATION(pmix_gds_shmem3_host_alias_t);

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
} pmix_gds_shmem3_nodeinfo_t;
PMIX_CLASS_DECLARATION(pmix_gds_shmem3_nodeinfo_t);

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
} pmix_gds_shmem3_shared_session_data_t;

typedef struct {
    pmix_list_item_t super;
    /** Shared-memory object that maintains backing store for session data. */
    pmix_shmem_t *shmem3;
    /** Stores status for shmem3. */
    pmix_gds_shmem3_status_t shmem3_status;
    /** Session data stored in shared-memory. */
    pmix_gds_shmem3_shared_session_data_t *smdata;
} pmix_gds_shmem3_session_t;
PMIX_CLASS_DECLARATION(pmix_gds_shmem3_session_t);

/**
 * Shared data structures that reside in shared-memory. The server populates
 * these data and clients are only permitted to read from them.
 *
 * Note that the shared data structures in pmix_gds_shmem3_shared_*_data_t are
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
    /** Translates the key indices stored in local_hashtab above.
     *
     * Same reasoning as the modex segment's keyindex, and for the same
     * reason it has to live here rather than being the process-global
     * one: the indices in that table cross a process boundary, written
     * by the server and read by every local client, and no two
     * processes can be assumed to number a key alike.
     *
     * This used to be the global keyindex on both sides, kept in step
     * by shipping the server's copy in the job-info reply and merging
     * it into the client's. That worked, but it made every read of
     * shared data depend on a process-global structure the progress
     * thread mutates on every put - growing its pointer array and
     * rehashing its lookup table - which is exactly what a reader
     * cannot tolerate.
     *
     * Clients map the segment read-only, so they must reach this only
     * through pmix_hash_find_key() and friends, which never register. */
    pmix_keyindex_t *keyindex;
} pmix_gds_shmem3_shared_job_data_t;

typedef struct {
    /** Shared-memory allocator for data this structure. */
    pmix_tma_t tma;
    /** Holds the current address of the shared-memory allocator. */
    void *current_addr;
    /** Stores static modex data. */
    pmix_hash_table_t *hashtab;
    /** Translates the key indices stored in hashtab above.
     *
     * This lives in the segment, rather than being the process-global
     * keyindex, because the indices in that table cross a process
     * boundary here: the server writes them and every local client
     * reads them. A non-reserved key's global index is assigned per
     * process in order of first encounter, so no two processes can be
     * assumed to agree on one - and the reserved half is only stable
     * between processes built from the same PMIx release. Keeping the
     * translation beside the data it describes means the indices only
     * have to be consistent within this segment, which they are because
     * exactly one process writes them.
     *
     * Clients map the segment read-only, so they must reach this only
     * through pmix_hash_find_key() and friends, which never register. */
    pmix_keyindex_t *keyindex;
} pmix_gds_shmem3_shared_modex_data_t;

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
    pmix_gds_shmem3_session_t *session;
    /** Stores status for shmem3. */
    pmix_gds_shmem3_status_t shmem3_status;
    /** Shared-memory object that maintains backing store for smdata data. */
    pmix_shmem_t *shmem3;
    /** Stores status for modex_shmem3. */
    pmix_gds_shmem3_status_t modex_shmem3_status;
    /** Which modex this job is on.
     *
     * Each modex gets its own segment rather than being written into the
     * one before it, because clients have that one mapped and a segment
     * a client can see must never be written again. The counter names
     * the backing file, so successive generations do not collide; the
     * client tells them apart by that path, which is already in the seg
     * blob, so nothing on the wire changes. Server-side only.
     */
    uint32_t modex_generation;
    /** Shared-memory object that maintains backing store for smmodex data. */
    pmix_shmem_t *modex_shmem3;
    /** Points to shared job data located in a shared-memory segment. */
    pmix_gds_shmem3_shared_job_data_t *smdata;
    /** Points to shared modex data located in a shared-memory segment. */
    pmix_gds_shmem3_shared_modex_data_t *smmodex;
    /** Packed connection information to this segment. */
    pmix_buffer_t *conni;
    /** Flag indicating whether client-side keyindex updates have been done. */
    bool client_keyindex_fixup_done;
} pmix_gds_shmem3_job_t;
PMIX_EXPORT PMIX_CLASS_DECLARATION(pmix_gds_shmem3_job_t);

typedef struct {
    pmix_list_item_t super;
    /** Application number. */
    uint32_t appnum;
    /** Application info. */
    pmix_list_t *appinfo;
    /** Node information. */
    pmix_list_t *nodeinfo;
    /* Application job info. */
    pmix_gds_shmem3_job_t *job;
} pmix_gds_shmem3_app_t;
PMIX_CLASS_DECLARATION(pmix_gds_shmem3_app_t);

END_C_DECLS

#endif

/*
 * vim: ft=cpp ts=4 sts=4 sw=4 expandtab
 */
