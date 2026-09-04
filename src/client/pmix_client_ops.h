/*
 * Copyright (c) 2015-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#ifndef PMIX_CLIENT_OPS_H
#define PMIX_CLIENT_OPS_H

#include "src/include/pmix_config.h"

#include "src/class/pmix_list.h"
#include "src/class/pmix_pointer_array.h"
#include "src/common/pmix_iof.h"
#include "src/include/pmix_globals.h"
#include "src/threads/pmix_threads.h"

BEGIN_C_DECLS

typedef struct {
    pmix_peer_t *myserver;        // messaging support to/from my server
    bool singleton;               // no server
    bool local_iof;               // we constructed the server-side IOF lists
                                  // (pmix_server_globals.iof/iof_residuals)
    bool fast_get;                // answer a get on the caller's thread when
                                  //   the datastore says that is safe
                                  // (cleared by PMIX_GET_ON_PROGRESS_THREAD
                                  //  in the environment - a development
                                  //  switch, see pmix_rte_init)
    pmix_list_t pending_requests; // list of pmix_cb_t pending data requests
    pmix_pointer_array_t peers;   // array of pmix_peer_t cached for data ops
    pmix_list_t groups;           // list of groups this client is part of
    /* Guards "groups" - both the list spine and the membership arrays of the
     * pmix_group_t objects on it. Unlike pending_requests, this list is not
     * confined to the progress thread: it is appended and edited there (when
     * a construct completes, and when a PMIX_GROUP_LEFT event trims a
     * departed member out of an existing group), but it is read and removed
     * from on the caller's thread by PMIx_Group_leave_nb,
     * PMIx_Group_destruct_nb, and every collective that expands a group
     * reference through pmix_client_convert_group_procs(). Hold this across
     * any traversal, and across any use of a group's members - the
     * GROUP_LEFT path shifts that array down underneath a reader.
     *
     * Never hold it across a call that waits on the progress thread (a
     * blocking PMIx_Notify_event, say): the handler on the other side may
     * need this same lock. Copy out what you need and release it first. */
    pmix_mutex_t grouplock;
    // verbosity for client get operations
    int get_output;
    int get_verbose;
    // verbosity for client connect operations
    int connect_output;
    int connect_verbose;
    // verbosity for client fence operations
    int fence_output;
    int fence_verbose;
    // verbosity for client pub operations
    int pub_output;
    int pub_verbose;
    // verbosity for client spawn operations
    int spawn_output;
    int spawn_verbose;
    // verbosity for client event operations
    int event_output;
    int event_verbose;
    // verbosity for client iof operations
    int iof_output;
    int iof_verbose;
    // verbosity for basic client functions
    int base_output;
    int base_verbose;
    /* IOF output sinks */
    pmix_iof_sink_t iof_stdout;
    pmix_iof_sink_t iof_stderr;
    // verbosity for client group operations
    int group_output;
    int group_verbose;
    /* Delta-commit bookkeeping. PMIx_Commit used to fetch and ship this
     * process's entire local and remote store on every call, so n
     * put/commit cycles moved O(n^2) bytes. Instead we record which keys
     * have data the server has not been told about, and commit fetches
     * only those.
     *
     * These are the keys dirty at each scope; a PMIX_GLOBAL put lands in
     * both, mirroring the datastore, and a PMIX_INTERNAL put in neither
     * because it never leaves the process. They are recorded whether or
     * not we are currently connected - a singleton that later connects
     * (see the re-entrant branch of PMIx_Init) must still ship what it
     * published beforehand.
     *
     * commit_resync forces the next commit back to the cumulative fetch.
     * It is the escape hatch for everything the per-key record cannot
     * express, and it is set at each such point rather than being
     * inferred here; see pmix_client_commit_resync(). */
    char **dirty_local;
    char **dirty_remote;
    bool commit_resync;
    /* Keys deleted since the last commit, per scope a delete targets.
     *
     * A deletion cannot ride the dirty-key record: that names keys for
     * the commit to fetch back, and a deleted key is precisely the one
     * the fetch will not find. It has to be stated explicitly, so the
     * commit emits it as its own PMIX_DEL_* scope block - before the
     * data blocks, so a key deleted and then published again in the same
     * interval ends up present rather than absent. */
    char **del_local;
    char **del_remote;
} pmix_client_globals_t;

PMIX_EXPORT extern pmix_client_globals_t pmix_client_globals;

PMIX_EXPORT void pmix_parse_localquery(int sd, short args, void *cbdata);

/* Force the next PMIx_Commit to send this process's whole store rather
 * than what has changed since the last one, and drop the per-key record
 * that is about to be meaningless. Call this wherever the server we are
 * committing to may not have seen what we already sent - the tool role
 * repointing pmix_client_globals.myserver is the case that matters. */
PMIX_EXPORT void pmix_client_commit_resync(void);

PMIX_EXPORT pmix_status_t pmix_client_convert_group_procs(const pmix_proc_t *inprocs, size_t insize,
                                                          pmix_proc_t **outprocs, size_t *outsize);

PMIX_EXPORT bool pmix_client_proc_is_included(const pmix_proc_t *procs, size_t nprocs);

/* The three pieces of "this role holds data a server may take back".
 * Both PMIx_Init and PMIx_tool_init use all three, in this order: post
 * the receives during bring-up, mark ourselves initialized as the last
 * act of init, and give back anything still held during finalize.
 * See the comments on each in pmix_client.c. */
PMIX_EXPORT void pmix_client_post_data_recvs(void);
PMIX_EXPORT void pmix_client_mark_initialized(void);
PMIX_EXPORT void pmix_client_release_held_deletes(void);

END_C_DECLS

#endif /* PMIX_CLIENT_OPS_H */
