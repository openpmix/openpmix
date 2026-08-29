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
#include "src/threads/pmix_threads.h"
#include "src/util/pmix_shmem.h"
#include "src/mca/gds/base/base.h"

#ifdef HAVE_STDINT_H
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
/* 2: a segment's key index carries only the non-reserved keys. Reserved
 *    ids come from the dictionary and agree across processes, so they are
 *    resolved against the process-global index instead. No size changed,
 *    but a peer built before this looks for reserved keys in a table that
 *    no longer holds them. */
#define PMIX_GDS_SHMEM3_LAYOUT_VERSION 2u

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
 * Address space reserved for each modex slot in a job's arena, in bytes.
 * Setting it to zero disables the arena entirely, which restores the
 * pre-arena behavior of placing every segment independently.
 */
PMIX_EXPORT extern size_t pmix_gds_shmem3_arena_slot_size;

/**
 * How many modex slots a job's arena holds - that is, how many modex
 * generations can be live at once and still be placed in it. More than
 * one is live whenever a fence contributed only what changed, since the
 * generations before such a one remain the only copy of what it did not
 * repeat. Capped at 32 by the occupancy bitmap.
 */
PMIX_EXPORT extern size_t pmix_gds_shmem3_arena_modex_slots;

/**
 * Whether to keep away from the midpoint of the biggest hole when
 * choosing an address (see VMEM_HOLE_BIGGEST_OFFSET). On by default.
 */
PMIX_EXPORT extern bool pmix_gds_shmem3_offset_placement;

/**
 * Testing-only MCA parameter. When true, a client's attach is forced to
 * fail only for the modex segment, leaving the job and session attaches
 * at PMIx_Init alone. This is the case force_client_attach_failure
 * cannot reach: that one fails every attach, so the client falls back to
 * hash during PMIx_Init and never gets as far as a fence. Never set this
 * in production.
 */
PMIX_EXPORT extern bool pmix_gds_shmem3_force_modex_attach_failure;

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
    /** Guards the spine of the list above.
     *
     * Everything else in this component runs on the progress thread and
     * needs no lock. This one exists because a job tracker owns the
     * mapping of its shared segments - releasing one detaches them - so
     * a reader that is *not* on the progress thread can have the memory
     * it is walking unmapped underneath it by del_nspace().
     *
     * The lock covers finding a tracker and taking a reference to it,
     * not reading its contents. Once a reference is held the object
     * cannot be destructed (the count is a C11 atomic, so this works
     * across threads), which means the segments stay mapped and the
     * read itself proceeds without any lock at all.
     */
    pmix_mutex_t joblock;
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
    /** Session ID, or UINT32_MAX for a job that has not named one.
     *
     * Held here as well as in smdata, and this is the copy every lookup
     * reads. smdata lives inside the segment, so an id taken from there
     * cannot answer the question that has to be settled before the
     * segment exists: which session is this, and does another job
     * already hold it?
     *
     * UINT32_MAX also marks the private object job_construct() gives
     * every job. Those are never shared - two jobs that have not named a
     * session are not thereby in the same one - and never appear on
     * pmix_mca_gds_shmem3_component.sessions.
     */
    uint32_t id;
    /** The session's description, held until there is a segment to put
     *  it in.
     *
     *  A host may register a session before any job is running in it,
     *  and the segment cannot be built that early: it is placed in, and
     *  named after, a job. So the description is kept here - a deep copy,
     *  since the host is free to release its array once the registration
     *  callback returns - and written into the segment when the first job
     *  in the session builds it. NULL for a session nobody has described.
     */
    pmix_info_t *sinfo;
    size_t nsinfo;
    /** Has the session's data been written into the segment yet?
     *
     *  Distinct from PMIX_GDS_SHMEM3_READY_FOR_USE, which is only set
     *  once the whole of a job's store completes. This says "the session
     *  half is done", so that a host registration and a job's own
     *  session array cannot both write - first writer wins, because a
     *  segment a client can see is never written again.
     */
    bool described;
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
    /** List containing this job's node information. */
    pmix_list_t *nodeinfo;
    /** List of applications in this job. */
    pmix_list_t *appinfo;
    /** Stores static local (node) job data. */
    pmix_hash_table_t *local_hashtab;
    /** Translates the NON-RESERVED key indices stored in local_hashtab.
     *
     * Same split as the modex segment's keyindex; see the note there. A
     * reserved attribute's id comes from the dictionary and is the same
     * in every process, so it is not carried here - lookup_key() sends
     * anything below PMIX_INDEX_BOUNDARY to the process-global index
     * instead. What is left is the keys a host supplied that the
     * Standard does not define, which are numbered on first encounter
     * and so genuinely cannot be agreed in advance.
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
    /** Translates the NON-RESERVED key indices stored in hashtab above.
     *
     * An index in that table crosses a process boundary - the server
     * writes it and every local client reads it - so the two ends have
     * to agree on what it means. They agree in two different ways, and
     * only one of them needs anything stored here:
     *
     *  - a RESERVED attribute is numbered by the dictionary, and those
     *    ids are pinned in contrib/dictionary_ids.txt precisely so that
     *    they are the same in every process and every release. Nothing
     *    has to be carried, so nothing is: lookup_key() resolves any id
     *    below PMIX_INDEX_BOUNDARY against the process-global index.
     *  - a NON-RESERVED key is numbered on first encounter, in an order
     *    that depends on what arrived and when, so no two processes can
     *    be assumed to agree. Those go here, beside the data they
     *    describe, where they only have to be consistent within this one
     *    segment - which they are, because exactly one process writes it.
     *
     * The reserved half used to be carried here too, which meant the
     * server allocated several hundred entries out of the segment, for
     * every generation, to say what the dictionary already said.
     *
     * Clients map the segment read-only, so they must reach this only
     * through pmix_hash_find_key() and friends, which never register. */
    pmix_keyindex_t *keyindex;
} pmix_gds_shmem3_shared_modex_data_t;

/* A key this job has been told to stop answering for.
 *
 * Deliberately NOT in shared memory. A segment a client can see is never
 * written again, so a tombstone cannot go into one that already holds
 * the key - it would have to be a new segment, mapped by every local
 * client, for a few bytes per deleted key. Each process keeps its own
 * record instead, built from the notification its server already sends,
 * and a client that attaches later is given the list in the job-info
 * reply.
 *
 * "generation" is the modex generation current when the removal was
 * recorded. Job-segment data is written once and never re-published, so
 * a tombstone against it always applies; modex data can legitimately
 * come back, so a tombstone only shadows generations up to the one it
 * was recorded at. */
typedef struct {
    pmix_list_item_t super;
    pmix_rank_t rank;
    char *key;
    uint32_t generation;
} pmix_gds_shmem3_tombstone_t;
PMIX_EXPORT PMIX_CLASS_DECLARATION(pmix_gds_shmem3_tombstone_t);

/* One segment in a chain of them, newest first.
 *
 * A read enters at the head and follows ->prior until it finds what it
 * wants or runs out, so a newer segment shadows an older one. That is
 * how a modex generation carrying only what changed can be answered
 * alongside the generations before it, which are still the only copy of
 * what it did not repeat.
 *
 * Deliberately NOT a pmix_list_t, and the difference is the whole
 * point. A read may run on the application's thread (see is_tsafe)
 * while the progress thread publishes a new generation, and a
 * pmix_list_t cannot be extended under such a reader: it is doubly
 * linked through a sentinel and carries a length counter, so a prepend
 * is several stores and a walker can catch any of them. This chain can
 * be extended under a reader, because a node is filled in completely
 * BEFORE it is published, is never written again, and is never removed
 * - so publishing is one release-store of the head, and a reader that
 * has acquire-loaded the head walks a chain that cannot change beneath
 * it.
 *
 * None of the list operations are wanted here. It is only ever
 * prepended to and walked in one direction, which is why this carries a
 * bare ->prior rather than deriving from pmix_list_item_t.
 */
typedef struct pmix_gds_shmem3_seg_t {
    /** The segment published before this one; NULL at the end of the
     *  chain. Written once, before this node is published, and never
     *  again - which is what lets it be followed without a lock. */
    struct pmix_gds_shmem3_seg_t *prior;
    /** Which kind of data this segment holds, so a walker knows what
     *  ->smdata points at. */
    pmix_gds_shmem3_job_shmem3_id_t smid;
    pmix_gds_shmem3_status_t status;
    pmix_shmem_t *shmem3;
    /** The shared_*_data_t at the base of the segment, of the type
     *  ->smid names. */
    void *smdata;
    /** which generation this was, so a tombstone recorded later can be
     * told from one recorded before it - see pmix_gds_shmem3_tombstone_t */
    uint32_t generation;
} pmix_gds_shmem3_seg_t;

/** The head of a chain.
 *
 * Atomic so that publishing is a single store and entering the chain a
 * single load. Use pmix_gds_shmem3_chain_publish() and
 * pmix_gds_shmem3_chain_head() rather than touching it directly - they
 * carry the release/acquire pairing the discipline above depends on.
 */
typedef _Atomic(pmix_gds_shmem3_seg_t *) pmix_gds_shmem3_chain_t;
PMIX_EXPORT PMIX_CLASS_DECLARATION(pmix_gds_shmem3_modex_seg_t);

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
     * blob, so nothing on the wire changes.
     *
     * Only the server's counter names anything - a client is told the
     * path - but BOTH ends have to advance it, because it is also what
     * dates a tombstone (see pmix_gds_shmem3_tombstone_t). A client that
     * left it at zero stamped every tombstone with generation zero, and
     * "generation <= t->generation" then held for every generation it
     * ever mapped: a key deleted and re-published in a later fence stayed
     * invisible to that client for the rest of the run. The two counters
     * are never compared with each other, so they do not have to agree -
     * each only has to advance whenever ITS process takes on a new
     * generation.
     */
    uint32_t modex_generation;
    /** Shared-memory object that maintains backing store for smmodex data. */
    pmix_shmem_t *modex_shmem3;
    /** Points to shared job data located in a shared-memory segment. */
    pmix_gds_shmem3_shared_job_data_t *smdata;
    /** Points to shared modex data located in a shared-memory segment. */
    pmix_gds_shmem3_shared_modex_data_t *smmodex;
    /** Does the current modex generation hold only what changed?
     *
     * Set when it was built from a PMIX_MODEX_DELTA contribution, and
     * told to each client in the segment blob so it can make the same
     * keep-or-drop decision this server made. */
    bool modex_is_delta;
    /** Guards the process-local state a read walks: the modex generation
     *  chain below and the tombstone list.
     *
     * This module is is_tsafe, so pmix_gds_shmem3_fetch() runs on the
     * APPLICATION's thread (see try_local_fetch() in
     * src/client/pmix_client_get.c, which is on by default). Two things
     * the progress thread does still need to be kept away from it:
     *
     *   - retire_modex_segment() clears job->smmodex while publishing the
     *     generation it replaces, and a read must not catch that
     *     half-done;
     *   - del_key() appends to the tombstone list, which a read walks.
     *
     * Nothing UNMAPS any more. A modex generation is retired onto the
     * chain and kept, so the chain only ever grows and a walk cannot
     * have a segment taken away underneath it - which is why walking
     * needs no lock, and why the two items above are all that is left.
     *
     * NOT needed for anything in a shared segment: those are written once,
     * before any client can see them, and never again.
     */
    pmix_mutex_t datalock;
    /** Keys this job has been told to stop answering for. Process-local;
     * see pmix_gds_shmem3_tombstone_t. */
    pmix_list_t tombstones;
    /** Modex generations older than the current one, newest first.
     *
     * Non-empty only when a delta contribution has been stored: such a
     * generation holds just what changed, so what came before it is
     * still the only copy of everything it did not repeat. A read walks
     * this after the current generation. */
    /** Retired modex generations, newest first - see
     *  pmix_gds_shmem3_seg_t. Walked by modex_fetch() for the keys a
     *  delta generation did not repeat. */
    pmix_gds_shmem3_chain_t modex_prior;
    /** Base of this job's reserved address-space arena, or 0 if it has
     *  none. See "The address-space arena" in AGENTS.md.
     *
     * Every process that takes part in this job - the server and each of
     * its local clients - holds the SAME range, in its own address space,
     * from the moment it learns of the job. Segments are then mapped over
     * that reservation rather than into whatever happens to be free,
     * which is what makes a fixed-address attach reliable at a point in
     * the run when the process is no longer empty.
     */
    uintptr_t arena_base;
    /** Size of the reservation above. Zero means there is none. */
    size_t arena_size;
    /** Bytes of the arena handed out to segments that live for as long as
     *  the job does (the job segment). Server-side only: a client maps
     *  wherever the server tells it to, so it has no carving to do. */
    size_t arena_static_used;
    /** Size of each modex slot at the top of the arena.
     *
     * A modex generation gets a slot of its own and holds it for as long
     * as it is readable, which is NOT just until the next one arrives: a
     * delta generation carries only what changed, so the generations
     * before it stay mapped and answerable on job->modex_prior. The
     * slots therefore have to be allocated and freed, not alternated
     * between - see arena_alloc_modex(), which reads the occupancy off
     * the live segments rather than keeping a tally that could drift.
     * Server-side only, like arena_static_used. */
    size_t arena_slot_bytes;
    /** How many modex slots the arena was reserved with. A generation
     *  that arrives when they are all taken places itself outside the
     *  arena, the way everything did before there was one. */
    size_t arena_slots;
    /** Packed connection information to this segment. */
    pmix_buffer_t *conni;
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

/**
 * Find the job tracker for an nspace and take a reference to it, safely
 * from a thread other than the progress thread.
 *
 * On success the caller owns a reference and MUST PMIX_RELEASE it when
 * finished. Holding it is what guarantees the tracker's shared-memory
 * segments stay mapped for the duration of the read, even if the
 * progress thread deregisters the nspace meanwhile.
 *
 * Never creates. Returns PMIX_ERR_INVALID_NAMESPACE if there is no such
 * tracker, which for a reader simply means "nothing to read here".
 */
PMIX_EXPORT pmix_status_t
pmix_gds_shmem3_acquire_job_tracker(
    const pmix_nspace_t nspace,
    pmix_gds_shmem3_job_t **job
);

END_C_DECLS

#endif

/*
 * vim: ft=cpp ts=4 sts=4 sw=4 expandtab
 */
