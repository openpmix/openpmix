/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2014-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2014      Artem Y. Polyakov <artpol84@gmail.com>.
 *                         All rights reserved.
 * Copyright (c) 2016-2017 Mellanox Technologies, Inc.
 *                         All rights reserved.
 * Copyright (c) 2016-2022 IBM Corporation.  All rights reserved.
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
 * Copyright (c) 2023      Triad National Security, LLC. All rights reserved.
 * Copyright (c) 2026      Jeff Squyres  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "src/include/pmix_config.h"

#include "src/include/pmix_prefetch.h"

#include "src/include/pmix_socket_errno.h"
#include "src/include/pmix_stdint.h"

#include "include/pmix.h"

#include "src/include/pmix_globals.h"

#ifdef HAVE_STRING_H
#    include <string.h>
#endif
#include <fcntl.h>
#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif
#ifdef HAVE_SYS_SOCKET_H
#    include <sys/socket.h>
#endif
#ifdef HAVE_SYS_UN_H
#    include <sys/un.h>
#endif
#ifdef HAVE_SYS_UIO_H
#    include <sys/uio.h>
#endif
#ifdef HAVE_SYS_TYPES_H
#    include <sys/types.h>
#endif

#ifdef PMIX_GIT_REPO_BUILD
static const char pmix_version_string[] = "OpenPMIx " PMIX_VERSION ", repo rev: " PMIX_REPO_REV
                                          " (PMIx Standard: " PMIX_STD_VERSION ","
                                          " Stable ABI: " PMIX_STD_ABI_STABLE_VERSION ","
                                          " Provisional ABI: " PMIX_STD_ABI_PROVISIONAL_VERSION ")";
#else
static const char pmix_version_string[] = "OpenPMIx " PMIX_VERSION
                                          " (PMIx Standard: " PMIX_STD_VERSION ","
                                          " Stable ABI: " PMIX_STD_ABI_STABLE_VERSION ","
                                          " Provisional ABI: " PMIX_STD_ABI_PROVISIONAL_VERSION ")";
#endif

#include "src/class/pmix_list.h"
#include "src/common/pmix_attributes.h"
#include "src/common/pmix_iof.h"
#include "src/event/pmix_event.h"
#include "src/hwloc/pmix_hwloc.h"
#include "src/include/pmix_globals.h"
#include "src/mca/bfrops/base/base.h"
#include "src/mca/gds/base/base.h"
#include "src/mca/pcompress/base/base.h"
#include "src/mca/preg/preg.h"
#include "src/mca/ptl/base/base.h"
#include "src/runtime/pmix_progress_threads.h"
#include "src/runtime/pmix_rte.h"
#include "src/threads/pmix_threads.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_error.h"
#include "src/util/pmix_name_fns.h"
#include "src/util/pmix_output.h"
#include "src/util/pmix_printf.h"
#include "src/util/pmix_show_help.h"

#include "pmix_client_ops.h"
#include "src/server/pmix_server_ops.h"

#define PMIX_MAX_RETRIES 10

static void pmix_client_notify_recv(struct pmix_peer_t *peer, pmix_ptl_hdr_t *hdr,
                                    pmix_buffer_t *buf, void *cbdata)
{
    pmix_status_t rc;
    int32_t cnt;
    pmix_cmd_t cmd;
    pmix_event_chain_t *chain;
    size_t ninfo;

    pmix_output_verbose(2, pmix_client_globals.event_output,
                        "%s pmix:client_notify_recv - processing event",
                        PMIX_NAME_PRINT(&pmix_globals.myid));

    PMIX_HIDE_UNUSED_PARAMS(peer, hdr, cbdata);

    /* a zero-byte buffer indicates that this recv is being
     * completed due to a lost connection */
    if (PMIX_BUFFER_IS_EMPTY(buf)) {
        return;
    }

    /* start the local notification chain */
    chain = PMIX_NEW(pmix_event_chain_t);
    if (PMIX_UNLIKELY(NULL == chain)) {
        PMIX_ERROR_LOG(PMIX_ERR_NOMEM);
        return;
    }
    chain->final_cbfunc = pmix_event_notify_complete;
    chain->final_cbdata = chain;

    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, pmix_client_globals.myserver, buf, &cmd, &cnt, PMIX_COMMAND);
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(chain);
        goto error;
    }
    /* unpack the status */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, pmix_client_globals.myserver, buf, &chain->status, &cnt, PMIX_STATUS);
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(chain);
        goto error;
    }

    /* unpack the source of the event */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, pmix_client_globals.myserver, buf, &chain->source, &cnt, PMIX_PROC);
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(chain);
        goto error;
    }

    /* unpack the info that might have been provided */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, pmix_client_globals.myserver, buf, &ninfo, &cnt, PMIX_SIZE);
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(chain);
        goto error;
    }

    /* This count came off the wire, and two things downstream trust it
     * without an unpack in front of them to screen it: the "+ 2" below
     * is multiplied by sizeof(pmix_info_t) to size an allocation whose
     * element constructors then walk it, and pmix_invoke_local_event_hdlr
     * writes the handler name and callback object at info[ninfo] and
     * info[ninfo+1]. A count near SIZE_MAX wraps that sum down to a
     * one- or zero-element array, and those two seeds are then written
     * off the end of it.
     *
     * Require the count to survive the round trip through the int32_t
     * the unpack consumes it as - the same screen the server-side
     * handlers use; see src/server/AGENTS.md. */
    cnt = ninfo;
    if (0 > cnt || (size_t) cnt != ninfo) {
        PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
        rc = PMIX_ERR_BAD_PARAM;
        PMIX_RELEASE(chain);
        goto error;
    }
    /* we always leave space for event hdlr name and a callback object */
    chain->nallocated = ninfo + 2;
    PMIX_INFO_CREATE(chain->info, chain->nallocated);
    if (PMIX_UNLIKELY(NULL == chain->info)) {
        PMIX_ERROR_LOG(PMIX_ERR_NOMEM);
        PMIX_RELEASE(chain);
        return;
    }

    if (0 < ninfo) {
        chain->ninfo = ninfo;
        cnt = ninfo;
        PMIX_BFROPS_UNPACK(rc, pmix_client_globals.myserver, buf, chain->info, &cnt, PMIX_INFO);
        if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
            PMIX_ERROR_LOG(rc);
            PMIX_RELEASE(chain);
            goto error;
        }
    }
    /* prep the chain for processing */
    pmix_prep_event_chain(chain, chain->info, ninfo, false);

    pmix_output_verbose(2, pmix_client_globals.event_output,
                        "%s pmix:client_notify_recv - processing event %s, calling errhandler",
                        PMIX_NAME_PRINT(&pmix_globals.myid), PMIx_Error_string(chain->status));

    pmix_invoke_local_event_hdlr(chain);
    return;

error:
    /* we always need to return */
    pmix_output_verbose(2, pmix_client_globals.event_output,
                        "%s pmix:client_notify_recv - unpack error status =%s, calling def errhandler",
                        PMIX_NAME_PRINT(&pmix_globals.myid), PMIx_Error_string(rc));
    chain = PMIX_NEW(pmix_event_chain_t);
    if (PMIX_UNLIKELY(NULL == chain)) {
        PMIX_ERROR_LOG(PMIX_ERR_NOMEM);
        return;
    }
    chain->status = rc;
    pmix_invoke_local_event_hdlr(chain);
}

/* Is this scope a request to remove the key rather than store it? */
static inline bool is_delete_scope(pmix_scope_t scope)
{
    return (PMIX_DEL_LOCAL == scope || PMIX_DEL_REMOTE == scope
            || PMIX_DEL_GLOBAL == scope || PMIX_DEL_INTERNAL == scope);
}

/* Tell the namespace's own datastore to stop answering for a key.
 *
 * Storing the removal into pmix_globals.mypeer corrects the "hash" store
 * every client keeps, and that is only half of it. A namespace served by
 * another module - gds/shmem3, reading a shared segment it is not
 * allowed to rewrite - holds its own copy and answers out of it, so it
 * has to be told separately or it keeps handing back the deleted key.
 *
 * Both halves are needed on both paths that remove a key: the one where
 * a server tells us about somebody's deletion, and the one where we are
 * the process doing the deleting. The second is easy to overlook,
 * because a rank deleting its OWN key has already applied it to its own
 * store - but its own committed data is also in the modex, which is a
 * different module's segment, and that copy answers first. */
static void del_key_in_nspace_module(const pmix_proc_t *proc, const char *key)
{
    pmix_namespace_t *nptr;
    pmix_status_t rc;

    if (NULL == proc || NULL == key) {
        return;
    }
    /* The module that answers a get for server-provided data is reached
     * through the SERVER peer's namespace object, not through
     * pmix_globals.nspaces. On a client those are different objects -
     * our own carries "hash", which the caller has already corrected,
     * and only the server peer's carries the module holding the modex.
     * Walking pmix_globals.nspaces alone therefore told nobody anything,
     * which is why a rank went on reading a key it had just deleted.
     *
     * The module screens the namespace itself - it has no tracker for
     * one it never served - so this needs no matching test here. */
    if (NULL != pmix_client_globals.myserver
        && NULL != pmix_client_globals.myserver->nptr) {
        PMIX_GDS_DEL_KEY(rc, pmix_client_globals.myserver->nptr, proc, key);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
        }
    }
    /* Any other namespace we hold cached data for. */
    PMIX_LIST_FOREACH (nptr, &pmix_globals.nspaces, pmix_namespace_t) {
        if (0 == strncmp(nptr->nspace, proc->nspace, PMIX_MAX_NSLEN)) {
            PMIX_GDS_DEL_KEY(rc, nptr, proc, key);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
            }
            return;
        }
    }
}

/* Apply one server-announced deletion to our copies. Takes ownership of
 * the key.
 *
 * We may never have had the key - a client caches only what it asked
 * about - and removing something absent is not an error, so nothing here
 * reports one. */
static void apply_delete(const pmix_proc_t *proc, pmix_scope_t scope, char *key)
{
    pmix_kval_t *kv;
    pmix_status_t rc;

    kv = PMIX_NEW(pmix_kval_t);
    if (PMIX_UNLIKELY(NULL == kv)) {
        free(key);
        return;
    }
    kv->key = key; // the kval owns it now
    PMIX_GDS_STORE_KV(rc, pmix_globals.mypeer, proc, scope, kv);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
    }
    /* our own peer is pinned to "hash", which is what the store above
     * corrected - the namespace's own module has to be told separately */
    del_key_in_nspace_module(proc, kv->key);
    PMIX_RELEASE(kv);
}

/* A deletion that arrived before PMIx_Init finished. See
 * client_data_delete_handler(). Only ever touched on the progress
 * thread. */
typedef struct {
    pmix_list_item_t super;
    pmix_proc_t proc;
    pmix_scope_t scope;
    char *key;
} pmix_held_delete_t;
static void hdcon(pmix_held_delete_t *p)
{
    p->key = NULL;
}
static void hddes(pmix_held_delete_t *p)
{
    if (NULL != p->key) {
        free(p->key);
    }
}
static PMIX_CLASS_INSTANCE(pmix_held_delete_t, pmix_list_item_t, hdcon, hddes);

static pmix_list_t pmix_client_held_deletes = PMIX_LIST_STATIC_INIT(pmix_client_held_deletes);

/* A server telling us that a key has been deleted, so our own copy of it
 * goes too. See pmix_server_notify_deleted(). */
static void client_data_delete_handler(struct pmix_peer_t *pr,
                                       pmix_ptl_hdr_t *hdr,
                                       pmix_buffer_t *buf, void *cbdata)
{
    pmix_proc_t proc;
    pmix_scope_t scope;
    char *key = NULL;
    pmix_status_t rc;
    int32_t cnt;
    pmix_held_delete_t *hd;

    PMIX_HIDE_UNUSED_PARAMS(pr, hdr, cbdata);

    /* a zero-byte buffer means the connection was lost */
    if (NULL == buf || PMIX_BUFFER_IS_EMPTY(buf)) {
        return;
    }
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, pmix_client_globals.myserver, buf, &proc, &cnt, PMIX_PROC);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return;
    }
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, pmix_client_globals.myserver, buf, &scope, &cnt, PMIX_SCOPE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return;
    }
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, pmix_client_globals.myserver, buf, &key, &cnt, PMIX_STRING);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return;
    }
    /* the scope arrived off the wire, so it is the sender's word rather
     * than ours - act only on one that really asks for a removal */
    if (NULL == key || !is_delete_scope(scope)) {
        PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
        if (NULL != key) {
            free(key);
        }
        return;
    }
    /* The connection comes up partway through PMIx_Init, and the rest of
     * that function stores into - and reads from - these same
     * per-namespace hash tables on the APPLICATION's thread: the
     * server-ID keys, everything pmix_hwloc_setup_topology() records,
     * and the debugger-directive fetch. Applying a deletion here while
     * that is running has two threads mutating one hash table, which is
     * corruption rather than a stale read.
     *
     * Nothing can read a deleted key before PMIx_Init returns, so hold
     * it instead. _init_complete() drains this list on this thread and
     * only then declares us initialized, so a deletion is either held
     * here or applied inline - there is no gap between the two. */
    if (!pmix_atomic_check_bool(&pmix_globals.initialized)) {
        hd = PMIX_NEW(pmix_held_delete_t);
        if (PMIX_UNLIKELY(NULL == hd)) {
            free(key);
            return;
        }
        PMIX_LOAD_PROCID(&hd->proc, proc.nspace, proc.rank);
        hd->scope = scope;
        hd->key = key; // the item owns it now
        pmix_list_append(&pmix_client_held_deletes, &hd->super);
        return;
    }

    apply_delete(&proc, scope, key);
}

/* Declare ourselves initialized - on the progress thread, so that the
 * deletions held while PMIx_Init worked are applied before anything can
 * read them, and so that no deletion can slip between the drain and the
 * flag. See client_data_delete_handler(). */
static void _init_complete(int sd, short args, void *cbdata)
{
    pmix_cb_t *cb = (pmix_cb_t *) cbdata;
    pmix_held_delete_t *hd;

    PMIX_ACQUIRE_OBJECT(cb);
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    while (NULL != (hd = (pmix_held_delete_t *)
                    pmix_list_remove_first(&pmix_client_held_deletes))) {
        apply_delete(&hd->proc, hd->scope, hd->key);
        hd->key = NULL; // apply_delete took it
        PMIX_RELEASE(hd);
    }
    pmix_atomic_set_bool(&pmix_globals.initialized);

    PMIX_POST_OBJECT(cb);
    PMIX_WAKEUP_THREAD(&cb->lock);
}

pmix_client_globals_t pmix_client_globals = {
    .myserver = NULL,
    .singleton = false,
    .fast_get = true,
    .local_iof = false,
    .pending_requests = PMIX_LIST_STATIC_INIT(pmix_client_globals.pending_requests),
    .peers = PMIX_POINTER_ARRAY_STATIC_INIT,
    .groups = PMIX_LIST_STATIC_INIT(pmix_client_globals.groups),
    .grouplock = PMIX_MUTEX_STATIC_INIT,
    .get_output = -1,
    .get_verbose = 0,
    .connect_output = -1,
    .connect_verbose = 0,
    .fence_output = -1,
    .fence_verbose = 0,
    .pub_output = -1,
    .pub_verbose = 0,
    .spawn_output = -1,
    .spawn_verbose = 0,
    .event_output = -1,
    .event_verbose = 0,
    .iof_output = -1,
    .iof_verbose = 0,
    .base_output = -1,
    .base_verbose = 0,
    .group_output = -1,
    .group_verbose = 0,
    .iof_stdout = PMIX_IOF_SINK_STATIC_INIT(pmix_client_globals.iof_stdout),
    .iof_stderr = PMIX_IOF_SINK_STATIC_INIT(pmix_client_globals.iof_stderr),
    .dirty_local = NULL,
    .dirty_remote = NULL,
    .del_local = NULL,
    .del_remote = NULL,
    /* start cumulative: on a second init in the same process the
     * datastore may still hold what the first one published, and the
     * server we are about to talk to has not seen any of it */
    .commit_resync = true
};

void pmix_client_commit_resync(void)
{
    if (NULL != pmix_client_globals.dirty_local) {
        PMIx_Argv_free(pmix_client_globals.dirty_local);
        pmix_client_globals.dirty_local = NULL;
    }
    if (NULL != pmix_client_globals.dirty_remote) {
        PMIx_Argv_free(pmix_client_globals.dirty_remote);
        pmix_client_globals.dirty_remote = NULL;
    }
    /* The pending deletions go too, and that is correct rather than
     * lossy: the reasons to resync are that the server we are about to
     * talk to has never seen anything we published (a tool repointing at
     * another one, a second PMIx_Init), so there is nothing there to
     * delete. */
    if (NULL != pmix_client_globals.del_local) {
        PMIx_Argv_free(pmix_client_globals.del_local);
        pmix_client_globals.del_local = NULL;
    }
    if (NULL != pmix_client_globals.del_remote) {
        PMIx_Argv_free(pmix_client_globals.del_remote);
        pmix_client_globals.del_remote = NULL;
    }
    pmix_client_globals.commit_resync = true;
}

/* Note that this key has been deleted, so the commit can say so.
 *
 * Also forces the commit to be cumulative. A delete and a re-publish of
 * the same key in one interval would otherwise have to be ordered
 * against each other in the per-key record, and there is nothing in that
 * record to order them by; sending the whole set after the deletions
 * gets the same answer without needing one. */
static void mark_deleted(pmix_scope_t scope, const char *key)
{
    pmix_status_t rc;

    if (PMIX_PEER_IS_SERVER(pmix_globals.mypeer) &&
        !PMIX_PEER_IS_TOOL(pmix_globals.mypeer)) {
        return;
    }
    /* internal data never left the process, so nobody else has a copy */
    if (PMIX_DEL_INTERNAL == scope) {
        return;
    }
    pmix_client_globals.commit_resync = true;
    if (PMIX_DEL_LOCAL == scope || PMIX_DEL_GLOBAL == scope) {
        rc = PMIx_Argv_append_unique_nosize(&pmix_client_globals.del_local, key);
        if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
            PMIX_ERROR_LOG(rc);
        }
    }
    if (PMIX_DEL_REMOTE == scope || PMIX_DEL_GLOBAL == scope) {
        rc = PMIx_Argv_append_unique_nosize(&pmix_client_globals.del_remote, key);
        if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
            PMIX_ERROR_LOG(rc);
        }
    }
}

/* Note that this key now has data the server has not been told about.
 *
 * Runs on the progress thread, from _putfn, and only once the store has
 * succeeded - a key whose store failed has nothing to fetch back. */
static void mark_dirty(pmix_scope_t scope, pmix_kval_t *kv)
{
    pmix_status_t rc;

    /* a server that is not also a tool never commits (see PMIx_Commit),
     * so recording anything for it only grows */
    if (PMIX_PEER_IS_SERVER(pmix_globals.mypeer) &&
        !PMIX_PEER_IS_TOOL(pmix_globals.mypeer)) {
        return;
    }

    /* internal data never leaves the process */
    if (PMIX_INTERNAL == scope) {
        return;
    }

    /* A qualified value cannot be recorded by key. Every one of them is
     * stored under the single PMIX_QUALIFIED_VALUE key, with the real
     * key and its qualifiers inside the array - and lookup_keyval() in
     * src/util/pmix_hash.c deliberately will not match a qualified entry
     * against an unqualified fetch, so there is no key we could ask for
     * it back by. The cumulative fetch returns them correctly, so use
     * that. */
    if (PMIX_CHECK_KEY(kv, PMIX_QUALIFIED_VALUE)) {
        pmix_client_globals.commit_resync = true;
        return;
    }

    /* append_unique is what keeps a key that is published repeatedly
     * between two commits from being sent more than once */
    if (PMIX_LOCAL == scope || PMIX_GLOBAL == scope) {
        rc = PMIx_Argv_append_unique_nosize(&pmix_client_globals.dirty_local, kv->key);
        if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
            pmix_client_globals.commit_resync = true;
        }
    }
    if (PMIX_REMOTE == scope || PMIX_GLOBAL == scope) {
        rc = PMIx_Argv_append_unique_nosize(&pmix_client_globals.dirty_remote, kv->key);
        if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
            pmix_client_globals.commit_resync = true;
        }
    }
}

/* Unpack the status the server acked a simple command with. Returns
 * PMIX_ERR_UNREACH for the NULL/zero-byte buffer that marks a recv being
 * completed synthetically because the connection was lost. */
static pmix_status_t unpack_ack(pmix_buffer_t *buf)
{
    pmix_status_t rc, ret;
    int32_t cnt;

    if (PMIX_UNLIKELY(NULL == buf || PMIX_BUFFER_IS_EMPTY(buf))) {
        return PMIX_ERR_UNREACH;
    }
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, pmix_client_globals.myserver, buf, &ret, &cnt, PMIX_STATUS);
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    return ret;
}

/* Completion of a commit: rather than discarding the reply, this one
 * reads the reply. The server acks PMIX_COMMIT_CMD with the status of the
 * store it just performed, and discarding that reported every commit as a
 * success - including one the server rejected, which is precisely the case
 * the caller needs to hear about, since the data it thinks it published is
 * not there. */
static void commit_cbfunc(struct pmix_peer_t *pr, pmix_ptl_hdr_t *hdr,
                          pmix_buffer_t *buf, void *cbdata)
{
    pmix_cb_t *cb = (pmix_cb_t *) cbdata;
    pmix_status_t ret;
    PMIX_HIDE_UNUSED_PARAMS(pr, hdr);

    PMIX_ACQUIRE_OBJECT(cb);

    ret = unpack_ack(buf);

    pmix_output_verbose(2, pmix_client_globals.base_output,
                        "pmix:client commit completed with status %s",
                        PMIx_Error_string(ret));

    cb->pstatus = ret;
    PMIX_POST_OBJECT(cb);
    PMIX_WAKEUP_THREAD(&cb->lock);
}

/* Completion of an abort. The server replies with the status its host
 * returned for the abort request (see op_cbfunc in src/server), and the
 * reply used to be discarded wholesale - so PMIx_Abort reported success
 * even when the host refused the request or did not support it. */
static void abort_cbfunc(struct pmix_peer_t *pr, pmix_ptl_hdr_t *hdr,
                         pmix_buffer_t *buf, void *cbdata)
{
    pmix_cb_t *cb = (pmix_cb_t *) cbdata;
    PMIX_HIDE_UNUSED_PARAMS(pr, hdr);

    PMIX_ACQUIRE_OBJECT(cb);

    cb->pstatus = unpack_ack(buf);

    pmix_output_verbose(2, pmix_client_globals.base_output,
                        "pmix:client abort completed with status %s",
                        PMIx_Error_string(cb->pstatus));

    PMIX_POST_OBJECT(cb);
    PMIX_WAKEUP_THREAD(&cb->lock);
}

/* callback to receive job info */
static void job_data(struct pmix_peer_t *pr, pmix_ptl_hdr_t *hdr,
                     pmix_buffer_t *buf, void *cbdata)
{
    pmix_status_t rc;
    char *nspace;
    int32_t cnt = 1;
    pmix_cb_t *cb = (pmix_cb_t *) cbdata;
    PMIX_HIDE_UNUSED_PARAMS(pr, hdr);

    PMIX_ACQUIRE_OBJECT(cb);

    /* a zero-byte buffer indicates that this recv is being
     * completed due to a lost connection */
    if (PMIX_BUFFER_IS_EMPTY(buf)) {
        /* Say which of the two it was: the caller can tell "the server
         * went away" from "the server sent something we could not use".
         *
         * Deliberately not PMIX_ERR_UNREACH. PMIx_Init already returns
         * that to mean something quite different and rather cheerful -
         * "no server was found, you are a working singleton" - and the
         * library really is initialized in that case. Reusing it here,
         * where init is about to fail outright, would leave the caller
         * unable to tell a usable library from an unusable one. */
        cb->status = PMIX_ERR_LOST_CONNECTION;
        PMIX_POST_OBJECT(cb);
        PMIX_WAKEUP_THREAD(&cb->lock);
        return;
    }

    /* unpack the nspace - should be same as our own */
    PMIX_BFROPS_UNPACK(rc, pmix_client_globals.myserver, buf, &nspace, &cnt, PMIX_STRING);
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc || !PMIX_CHECK_NSPACE(nspace, pmix_globals.myid.nspace))) {
        if (PMIX_SUCCESS == rc) {
            /* the unpack succeeded, so it allocated the string - it is only
             * the identity that is wrong, and this path owns it */
            free(nspace);
            rc = PMIX_ERR_INVALID_VAL;
        }
        PMIX_ERROR_LOG(rc);
        cb->status = rc;
        PMIX_POST_OBJECT(cb);
        PMIX_WAKEUP_THREAD(&cb->lock);
        return;
    }

    /* decode it - leave cb->status set to the store result so the caller
     * sees a storage failure (e.g. PMIX_ERR_TAKE_NEXT_OPTION when a shmem3
     * segment cannot be attached) rather than having it masked as success */
    PMIX_GDS_STORE_JOB_INFO(cb->status, pmix_client_globals.myserver, nspace, buf);

    free(nspace);
    PMIX_POST_OBJECT(cb);
    PMIX_WAKEUP_THREAD(&cb->lock);
}

/* The GDS module chosen at connect time could not deliver our job data to
 * this client (e.g. shmem3 could not attach its fixed-address segment in
 * our address space). Switch our server connection to the next-priority
 * GDS module and re-request the job data in that module's format, telling
 * the server which module we switched to via PMIX_GDS_FALLBACK_CMD.
 *
 * Returns PMIX_SUCCESS once the fallback data has been stored, or an error
 * otherwise - PMIX_ERR_TAKE_NEXT_OPTION when no other module is available,
 * or the error produced by the re-request. In particular, an older server
 * that does not understand PMIX_GDS_FALLBACK_CMD rejects it, and the
 * reused job_data callback turns that rejection reply into an error, so
 * PMIx_Init fails just as it would have before this fallback existed. */
static pmix_status_t fallback_to_next_gds(void)
{
    pmix_peer_t *myserver = pmix_client_globals.myserver;
    pmix_gds_base_module_t *fb;
    pmix_buffer_t *req;
    pmix_cmd_t cmd = PMIX_GDS_FALLBACK_CMD;
    char *modname;
    pmix_cb_t cb;
    pmix_status_t rc;

    fb = pmix_gds_base_get_fallback_module(PMIX_GDS_PEER_MODULE(myserver));
    if (PMIX_UNLIKELY(NULL == fb)) {
        /* nothing else to fall back to */
        return PMIX_ERR_TAKE_NEXT_OPTION;
    }
    pmix_output_verbose(2, pmix_client_globals.base_output,
                        "pmix:client falling back to GDS module %s", fb->name);
    /* route this client's server connection through the fallback module so
     * the re-requested data is stored - and future ops resolved - with it */
    myserver->gds = fb;
    /* keep the nspace-level default consistent with the per-peer override.
     * On the client this namespace object belongs solely to the server
     * peer (it is not shared with other peers, unlike on the server), so
     * any code that still reads compat.gds directly - e.g. verbose
     * logging - resolves to the fallback module too. */
    myserver->nptr->compat.gds = fb;

    req = PMIX_NEW(pmix_buffer_t);
    if (PMIX_UNLIKELY(NULL == req)) {
        return PMIX_ERR_NOMEM;
    }
    PMIX_BFROPS_PACK(rc, myserver, req, &cmd, 1, PMIX_COMMAND);
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(req);
        return rc;
    }
    /* tell the server which module we switched to */
    modname = (char *) fb->name;
    PMIX_BFROPS_PACK(rc, myserver, req, &modname, 1, PMIX_STRING);
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(req);
        return rc;
    }
    PMIX_CONSTRUCT(&cb, pmix_cb_t);
    PMIX_PTL_SEND_RECV(rc, myserver, req, job_data, (void *) &cb);
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(req);
        PMIX_DESTRUCT(&cb);
        return rc;
    }
    PMIX_WAIT_THREAD(&cb.lock);
    rc = cb.status;
    PMIX_DESTRUCT(&cb);
    return rc;
}

PMIX_EXPORT const char *PMIx_Get_version(void)
{
    return pmix_version_string;
}

/* event handler registration callback */
static void mycbfn(pmix_status_t status, size_t refid, void *cbdata)
{
    pmix_rshift_caddy_t *cd = (pmix_rshift_caddy_t *) cbdata;

    PMIX_ACQUIRE_OBJECT(cd);
    if (PMIX_SUCCESS == status) {
        cd->status = refid;
    } else {
        cd->status = status;
    }
    PMIX_WAKEUP_THREAD(&cd->lock);
}

/* Record an operation's status on the caller's lock and wake it. Used
 * wherever this file hands work to something that answers through a
 * pmix_op_cbfunc_t and then blocks for the answer - the debugger-wait
 * teardown in PMIx_Init, and the host's abort up-call. */
static void opcb_wakeup(pmix_status_t status, void *cbdata)
{
    pmix_lock_t *lock = (pmix_lock_t *) cbdata;

    PMIX_ACQUIRE_OBJECT(lock);
    lock->status = status;
    PMIX_WAKEUP_THREAD(lock);
}

static void notification_fn(size_t evhdlr_registration_id, pmix_status_t status,
                            const pmix_proc_t *source, pmix_info_t info[], size_t ninfo,
                            pmix_info_t results[], size_t nresults,
                            pmix_event_notification_cbfunc_fn_t cbfunc, void *cbdata)
{
    pmix_lock_t *lock = NULL;
    char *name = NULL;
    size_t n;

    pmix_output_verbose(2, pmix_client_globals.base_output,
                        "[%s:%d] DEBUGGER RELEASE RECVD",
                        pmix_globals.myid.nspace, pmix_globals.myid.rank);

    PMIX_HIDE_UNUSED_PARAMS(evhdlr_registration_id, status, source, results, nresults);

    if (NULL != info) {
        for (n = 0; n < ninfo; n++) {
            if (0 == strncmp(info[n].key, PMIX_EVENT_RETURN_OBJECT, PMIX_MAX_KEYLEN)) {
                lock = (pmix_lock_t *) info[n].value.data.ptr;
            } else if (0 == strncmp(info[n].key, PMIX_EVENT_HDLR_NAME, PMIX_MAX_KEYLEN)) {
                name = info[n].value.data.string;
            }
        }
        /* if the object wasn't returned, then that is an error */
        if (PMIX_UNLIKELY(NULL == lock)) {
            pmix_output_verbose(2, pmix_client_globals.base_output,
                                "event handler %s failed to return object",
                                (NULL == name) ? "NULL" : name);
            /* let the event handler progress */
            if (NULL != cbfunc) {
                cbfunc(PMIX_SUCCESS, NULL, 0, NULL, NULL, cbdata);
            }
            return;
        }
    }
    if (NULL != lock) {
        PMIX_WAKEUP_THREAD(lock);
    }

    if (NULL != cbfunc) {
        cbfunc(PMIX_EVENT_ACTION_COMPLETE, NULL, 0, NULL, NULL, cbdata);
    }
}

static void _check_for_notify(pmix_info_t info[], size_t ninfo)
{
    size_t n, m = 0;
    pmix_info_t *model = NULL, *library = NULL, *vers = NULL, *tmod = NULL;
    pmix_shift_caddy_t *scd;

    for (n = 0; n < ninfo; n++) {
        if (0 == strncmp(info[n].key, PMIX_PROGRAMMING_MODEL, PMIX_MAX_KEYLEN)) {
            /* we need to generate an event indicating that
             * a programming model has been declared */
            model = &info[n];
        } else if (0 == strncmp(info[n].key, PMIX_MODEL_LIBRARY_NAME, PMIX_MAX_KEYLEN)) {
            library = &info[n];
        } else if (0 == strncmp(info[n].key, PMIX_MODEL_LIBRARY_VERSION, PMIX_MAX_KEYLEN)) {
            vers = &info[n];
        } else if (0 == strncmp(info[n].key, PMIX_THREADING_MODEL, PMIX_MAX_KEYLEN)) {
            tmod = &info[n];
        }
    }
    /* how many distinct keys we found, which is what we are about to
     * pass on - counting occurrences instead would over-size the array
     * whenever a caller named one of them twice, since each of these
     * holds only the last one seen */
    if (NULL != model) {
        ++m;
    }
    if (NULL != library) {
        ++m;
    }
    if (NULL != vers) {
        ++m;
    }
    if (NULL != tmod) {
        ++m;
    }
    if (0 < m) {
        /* notify anyone listening that a model has been declared */
        scd = PMIX_NEW(pmix_shift_caddy_t);
        if (PMIX_UNLIKELY(NULL == scd)) {
             /* nothing we can do */
            return;
        }
        PMIX_INFO_CREATE(scd->info, m + 1);
        if (PMIX_UNLIKELY(NULL == scd->info)) {
            PMIX_RELEASE(scd);
            return;
        }
        scd->infocopy = true;
        n = 0;
        if (NULL != model) {
            PMIX_INFO_XFER(&scd->info[n], model);
            ++n;
        }
        if (NULL != library) {
            PMIX_INFO_XFER(&scd->info[n], library);
            ++n;
        }
        if (NULL != vers) {
            PMIX_INFO_XFER(&scd->info[n], vers);
            ++n;
        }
        if (NULL != tmod) {
            PMIX_INFO_XFER(&scd->info[n], tmod);
            ++n;
        }
        /* mark that it is not to go to any default handlers */
        PMIX_INFO_LOAD(&scd->info[n], PMIX_EVENT_NON_DEFAULT, NULL, PMIX_BOOL);
        /* n has just counted the same four pointers m did, so this is
         * the whole array and not a prefix of it */
        scd->ninfo = n + 1;
        scd->status = PMIX_MODEL_DECLARED;
        scd->proc = &pmix_globals.myid;
        scd->range = PMIX_RANGE_PROC_LOCAL;
        scd->cbfunc.opcbfn = NULL;
        scd->cbdata = NULL;
        PMIX_THREADSHIFT(scd, pmix_internal_notify_event);
        PMIX_WAIT_THREAD(&scd->lock);
        PMIX_RELEASE(scd);
     }
}

static void client_iof_handler(struct pmix_peer_t *pr, pmix_ptl_hdr_t *hdr, pmix_buffer_t *buf,
                               void *cbdata)
{
    pmix_peer_t *peer = (pmix_peer_t *) pr;
    pmix_proc_t source;
    pmix_iof_channel_t channel;
    pmix_byte_object_t bo;
    int32_t cnt;
    pmix_status_t rc;
    size_t refid, ninfo = 0;
    pmix_iof_req_t *req;
    pmix_info_t *info = NULL;
    PMIX_HIDE_UNUSED_PARAMS(hdr, cbdata);

    PMIX_ACQUIRE_OBJECT(peer);

    pmix_output_verbose(2, pmix_client_globals.iof_output,
                        "recvd IOF with %d bytes",
                        (int) buf->bytes_used);


    /* if the buffer is empty, they are simply closing the socket */
    if (0 == buf->bytes_used) {
        return;
    }
    PMIX_BYTE_OBJECT_CONSTRUCT(&bo);

    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, peer, buf, &source, &cnt, PMIX_PROC);
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        PMIX_ERROR_LOG(rc);
        return;
    }
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, peer, buf, &channel, &cnt, PMIX_IOF_CHANNEL);
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        PMIX_ERROR_LOG(rc);
        return;
    }
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, peer, buf, &refid, &cnt, PMIX_SIZE);
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        PMIX_ERROR_LOG(rc);
        return;
    }
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, peer, buf, &ninfo, &cnt, PMIX_SIZE);
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        PMIX_ERROR_LOG(rc);
        return;
    }
    /* both of these came off the wire. The count is about to size an
     * allocation whose element constructors walk it, so a value large
     * enough to wrap that product runs off the end of a short block
     * before the unpack's NULL screen gets a say; and the request id is
     * consumed below as the int that pmix_pointer_array_get_item takes,
     * so one that does not fit truncates to some other request's slot
     * and hands this package to the wrong callback. Require both to
     * survive the round trip - see src/server/AGENTS.md. */
    cnt = ninfo;
    if (0 > cnt || (size_t) cnt != ninfo || refid > (size_t) INT_MAX) {
        PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
        return;
    }
    if (0 < ninfo) {
        PMIX_INFO_CREATE(info, ninfo);
        cnt = ninfo;
        PMIX_BFROPS_UNPACK(rc, peer, buf, info, &cnt, PMIX_INFO);
        if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
            PMIX_ERROR_LOG(rc);
            goto cleanup;
        }
    }
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, peer, buf, &bo, &cnt, PMIX_BYTE_OBJECT);
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        PMIX_ERROR_LOG(rc);
        goto cleanup;
    }
    /* lookup the handler for this IOF package */
    req = (pmix_iof_req_t *) pmix_pointer_array_get_item(&pmix_globals.iof_requests, (int) refid);
    if (NULL != req && NULL != req->cbfunc) {
        req->cbfunc(refid, channel, &source, &bo, info, ninfo);
    } else {
        /* otherwise, simply write it out to the specified std IO channel */
        if (NULL != bo.bytes && 0 < bo.size) {
            pmix_iof_write_output(&source, channel, &bo);
        }
    }

cleanup:
    /* cleanup the memory */
    if (0 < ninfo) {
        PMIX_INFO_FREE(info, ninfo);
    }
    PMIX_BYTE_OBJECT_DESTRUCT(&bo);
}

/* Remove the debugger-wait handler PMIx_Init registered.
 *
 * This has to be open-coded because PMIx_Deregister_event_handler gates on
 * pmix_globals.initialized, which PMIx_Init does not set until it is nearly
 * done - so the public entry point is a no-op from here. Registration has
 * the same problem and solves it the same way, by threadshifting straight
 * to the internal handler, and this is that call's mirror image.
 *
 * It blocks: the registration holds a pointer into our caller's stack
 * frame, and the point of removing it is that the frame is about to die. */
static void dereg_debugger_wait(size_t evref)
{
    pmix_shift_caddy_t *cd;
    pmix_lock_t lock;

    cd = PMIX_NEW(pmix_shift_caddy_t);
    if (PMIX_UNLIKELY(NULL == cd)) {
        /* nothing else we can do - the handler stays registered */
        return;
    }
    PMIX_CONSTRUCT_LOCK(&lock);
    cd->ref = evref;
    cd->cbfunc.opcbfn = opcb_wakeup;
    cd->cbdata = &lock;
    /* the handler releases the caddy after invoking our callback */
    PMIX_THREADSHIFT(cd, pmix_internal_dereg_event_hdlr);
    PMIX_WAIT_THREAD(&lock);
    PMIX_DESTRUCT_LOCK(&lock);
}

/* Give back everything an init built.
 *
 * Shared by PMIx_Finalize and by PMIx_Init's own unwind, so the two
 * cannot drift apart - an init that fails partway has to return the
 * process to the state it was in beforehand, and "the state it was in
 * beforehand" is defined by exactly this list.
 *
 * Safe to call from any point after the globals block in PMIx_Init.
 * Everything it touches is either statically initialized (the two IOF
 * sinks, the lists, the peer array) or screened for NULL, so a failure
 * before the corresponding setup ran is a no-op rather than a crash. */
static void client_teardown(void)
{
    pmix_peer_t *peer;
    int i;

    /* wait here until all active events have been processed */
    PMIx_Progress_thread_stop(NULL, 0);

    /* flush anything that is still trying to be written out */
    pmix_iof_static_dump_output(&pmix_client_globals.iof_stdout);
    pmix_iof_static_dump_output(&pmix_client_globals.iof_stderr);

    PMIX_DESTRUCT(&pmix_client_globals.iof_stdout);
    PMIX_DESTRUCT(&pmix_client_globals.iof_stderr);

    PMIX_LIST_DESTRUCT(&pmix_client_globals.pending_requests);
    /* A deletion that arrived while we were not marked initialized was
     * held rather than applied, and there is now nothing to apply it to.
     * PMIx_Init constructs this list on every cycle, which is what makes it
     * safe to destruct it on every cycle - the list destructor does re-link
     * the sentinel, but PMIX_DESTRUCT also zeroes the object's magic id, so
     * a second destruct without a construct between them is an abort. */
    PMIX_LIST_DESTRUCT(&pmix_client_held_deletes);
    /* drop the delta-commit record, and leave the flag set so a second
     * PMIx_Init in this process starts cumulative rather than trusting
     * what a previous cycle told a previous server */
    pmix_client_commit_resync();
    for (i = 0; i < pmix_client_globals.peers.size; i++) {
        peer = (pmix_peer_t *) pmix_pointer_array_get_item(&pmix_client_globals.peers, i);
        if (NULL != peer) {
            PMIX_RELEASE(peer);
        }
    }
    PMIX_DESTRUCT(&pmix_client_globals.peers);
    /* tear down the server-side IOF lists only if we actually constructed
     * them during init (no PMIX_NAMESPACE was present). This is tracked by
     * local_iof rather than the singleton flag: the two are independent
     * (see PMIx_Init) and keying off singleton either destructs lists that
     * were never built - a crash - or leaks lists that were. */
    if (pmix_client_globals.local_iof) {
        PMIX_LIST_DESTRUCT(&pmix_server_globals.iof);
        PMIX_LIST_DESTRUCT(&pmix_server_globals.iof_residuals);
        pmix_client_globals.local_iof = false;
    }

    if (NULL != pmix_client_globals.myserver) {
        /* CLOSE_THE_SOCKET screens the descriptor itself, and clears it,
         * so the peer destructor does not close it a second time */
        CLOSE_THE_SOCKET(pmix_client_globals.myserver->sd);
        PMIX_RELEASE(pmix_client_globals.myserver);
    }

    /* and that is the last of ours. pmix_globals.mypeer is NOT released
     * here: on a client it carries exactly one reference, the one
     * pmix_rte_init() created it with, and pmix_rte_finalize() above
     * gives that one back. The server and tool roles do release it a
     * second time, because they really do hold a second reference -
     * they point pmix_client_globals.myserver at that same object. This
     * role does not; myserver is a peer of its own, released above.
     *
     * Releasing it anyway was harmless only by accident. PMIX_RELEASE
     * NULLs its argument when the count reaches zero, so the guard that
     * used to stand here was really asking "did rte_finalize already
     * free it?" - and the answer was always yes. It would stop being
     * yes the moment anything else held a reference at this point, and
     * then this would have freed a live object out from under its
     * owner. */
    pmix_rte_finalize();

    /* finalize the class/object system */
    pmix_class_finalize();
}

static int client_init_cntr = 0;

pmix_status_t PMIx_Init(pmix_proc_t *proc,
                        pmix_info_t info[], size_t ninfo)
{
    char *evar, *suri;
    pmix_status_t rc = PMIX_SUCCESS;
    pmix_cb_t cb;
    pmix_buffer_t *req;
    pmix_cmd_t cmd = PMIX_REQ_CMD;
    pmix_proc_t wildcard, srvr;
    pmix_info_t ginfo, evinfo[4];
    pmix_lock_t releaselock;
    size_t n, evref = 0;
    bool found;
    pmix_ptl_posted_recv_t *rcv;
    pid_t pid;
    pmix_kval_t *kptr, *kv;
    pmix_iof_req_t *iofreq;
    pmix_rshift_caddy_t *cd;
    pmix_shift_caddy_t *scd;
    bool unreach = false;

    // check if an init has been called
    if (pmix_atomic_test_and_set(&pmix_globals.init_called)) {
        // did the prior call get far enough? We might be in a tight
        // race between multiple calls to PMIx_Init - bad programming
        // technique, but all we can do is try to protect against it
        if (PMIX_UNLIKELY(!pmix_atomic_check_bool(&pmix_globals.initialized))) {
            return PMIX_ERR_INIT;
        }
        /* Both of the things this path can go on to do wait on the
         * progress thread: the model-declaration notification threadshifts
         * and blocks, and a connect attempt round-trips. Driving either
         * from that thread waits for ourselves - so screen for it here,
         * as every other blocking entry point in this file does. */
        if (PMIX_UNLIKELY(pmix_progress_thread_check_blocking("PMIx_Init"))) {
            return PMIX_ERR_WOULD_BLOCK;
        }
        /* Track the ref count - and only now, once the call is going to
         * succeed. Counting it up front left a caller that got an error
         * back owing a PMIx_Finalize it has no reason to make, and the
         * real finalize then found a reference still outstanding and
         * returned without tearing anything down. */
        pmix_atomic_fetch_add(&client_init_cntr, 1);
        // return our proc name if they requested it
        if (NULL != proc) {
            PMIX_LOAD_PROCID(proc, pmix_globals.myid.nspace, pmix_globals.myid.rank);
        }
        /* we also need to check the info keys to see if something needs
         * to be done with them - e.g., to notify another library that we
         * also have called init */
        if (NULL != info) {
            _check_for_notify(info, ninfo);
        }
        /* if we were given connection info, then we should try
         * to connect if are currently unconnected */
        if (PMIX_UNLIKELY(!pmix_atomic_check_bool(&pmix_globals.connected))) {
            rc = pmix_ptl.connect_to_peer((struct pmix_peer_t *) pmix_client_globals.myserver, info,
                                          ninfo, &suri);
            if (PMIX_SUCCESS == rc) {
                pmix_client_globals.singleton = false;
                free(suri);
            }
        }
        return PMIX_SUCCESS;

    } else {
        pmix_atomic_fetch_add(&client_init_cntr, 1);
    }

    /* backward compatibility fix - remove any directive to use
     * the old usock component so we avoid a warning message */
    if (NULL != (evar = getenv("PMIX_MCA_ptl"))) {
        if (0 == strcmp(evar, "usock")) {
            /* we cannot support a usock-only environment */
            fprintf(stderr,
                    "-------------------------------------------------------------------\n");
            fprintf(stderr, "PMIx no longer supports the \"usock\" transport for client-server\n");
            fprintf(stderr,
                    "communication. A directive was detected that only allows that mode.\n");
            fprintf(stderr, "We cannot continue - please remove that constraint and try again.\n");
            fprintf(stderr,
                    "-------------------------------------------------------------------\n");
            rc = PMIX_ERR_INIT;
            goto errout_early;
        }
        /* anything else should just be cleared */
        pmix_unsetenv("PMIX_MCA_ptl", &environ);
    }

    /* setup the runtime - this init's the globals,
     * opens and initializes the required frameworks */
    rc = pmix_rte_init(PMIX_PROC_CLIENT, info, ninfo, pmix_client_notify_recv);
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        /* Deliberately not unwound, and deliberately still latched.
         * pmix_rte_init() does not give back what it managed to build
         * before it failed, so there is nothing here that could call the
         * counterpart safely, and letting a caller re-enter a runtime
         * that is open at an unknown depth is worse than telling it the
         * library is unusable. Every failure below this point does
         * unwind, because everything below this point is ours. */
        PMIX_ERROR_LOG(rc);
        return rc;
    }

    /* setup the base verbosity */
    if (0 < pmix_client_globals.base_verbose) {
        /* set default output */
        pmix_client_globals.base_output = pmix_output_open(NULL);
        pmix_output_set_verbosity(pmix_client_globals.base_output,
                                  pmix_client_globals.base_verbose);
    }
    /* setup the IO Forwarding recv */
    rcv = PMIX_NEW(pmix_ptl_posted_recv_t);
    rcv->tag = PMIX_PTL_TAG_IOF;
    rcv->cbfunc = client_iof_handler;
    /* add it to the end of the list of recvs */
    pmix_list_append(&pmix_ptl_base.posted_recvs, &rcv->super);
    /* and the IOF flow control recv */
    rcv = PMIX_NEW(pmix_ptl_posted_recv_t);
    rcv->tag = PMIX_PTL_TAG_IOF_CONTROL;
    rcv->cbfunc = pmix_iof_flow_control_handler;
    pmix_list_append(&pmix_ptl_base.posted_recvs, &rcv->super);
    /* and the "a key you may have cached has been deleted" recv */
    rcv = PMIX_NEW(pmix_ptl_posted_recv_t);
    rcv->tag = PMIX_PTL_TAG_DATA_DELETE;
    rcv->cbfunc = client_data_delete_handler;
    pmix_list_append(&pmix_ptl_base.posted_recvs, &rcv->super);
    /* create the default iof handler */
    iofreq = PMIX_NEW(pmix_iof_req_t);
    iofreq->channels = PMIX_FWD_STDOUT_CHANNEL | PMIX_FWD_STDERR_CHANNEL | PMIX_FWD_STDDIAG_CHANNEL;
    pmix_pointer_array_set_item(&pmix_globals.iof_requests, 0, iofreq);
    /* define the sinks */
    PMIX_IOF_SINK_DEFINE(&pmix_client_globals.iof_stdout, &pmix_globals.myid, 1,
                         PMIX_FWD_STDOUT_CHANNEL, pmix_iof_write_handler);
    PMIX_IOF_SINK_DEFINE(&pmix_client_globals.iof_stderr, &pmix_globals.myid, 2,
                         PMIX_FWD_STDERR_CHANNEL, pmix_iof_write_handler);

    /* setup the globals */
    /* reset these indicators on every fresh init: they are statics that
     * record how *this* cycle came up. "singleton" is set true below only
     * if we fail to find a server; "local_iof" is set true only if we
     * construct the server-side IOF lists. They are independent - a client
     * given a PMIX_NAMESPACE that fails to connect is a singleton without
     * those lists, and a no-namespace process that does find a (system)
     * server constructs them yet is not a singleton - so finalize must key
     * the IOF teardown off local_iof, not off singleton. Leaving either as
     * a stale "true" from a prior cycle would mis-tear-down on finalize. */
    pmix_client_globals.singleton = false;
    pmix_client_globals.local_iof = false;
    PMIX_CONSTRUCT(&pmix_client_globals.pending_requests, pmix_list_t);
    /* Constructed here, and not left to its static initializer, for the two
     * reasons any PMIX_LIST_STATIC_INIT list has to be. Its sentinel is
     * NULL-linked until a construct runs, so it reads as empty but the first
     * pmix_list_append() to it writes through a NULL - and appending is
     * exactly what client_data_delete_handler() does when a deletion arrives
     * before we are marked initialized, which is the case that handler exists
     * for. And PMIX_DESTRUCT zeroes an object's magic id, so the teardown
     * below can only run once per process unless something puts it back:
     * without this, the second PMIx_Init in a process aborted on the
     * assertion in a debug build. */
    PMIX_CONSTRUCT(&pmix_client_held_deletes, pmix_list_t);
    PMIX_CONSTRUCT(&pmix_client_globals.peers, pmix_pointer_array_t);
    pmix_pointer_array_init(&pmix_client_globals.peers, 1, INT_MAX, 1);
    pmix_client_globals.myserver = PMIX_NEW(pmix_peer_t);
    if (PMIX_UNLIKELY(NULL == pmix_client_globals.myserver)) {
        PMIX_ERROR_LOG(PMIX_ERR_NOMEM);
        rc = PMIX_ERR_NOMEM;
        goto errout;
    }
    pmix_client_globals.myserver->nptr = PMIX_NEW(pmix_namespace_t);
    if (PMIX_UNLIKELY(NULL == pmix_client_globals.myserver->nptr)) {
        PMIX_RELEASE(pmix_client_globals.myserver);
        PMIX_ERROR_LOG(PMIX_ERR_NOMEM);
        rc = PMIX_ERR_NOMEM;
        goto errout;
    }
    pmix_client_globals.myserver->info = PMIX_NEW(pmix_rank_info_t);
    if (PMIX_UNLIKELY(NULL == pmix_client_globals.myserver->info)) {
        PMIX_RELEASE(pmix_client_globals.myserver);
        PMIX_ERROR_LOG(PMIX_ERR_NOMEM);
        rc = PMIX_ERR_NOMEM;
        goto errout;
    }

    pmix_output_verbose(2, pmix_client_globals.base_output,
                        "pmix: init called");

    /* see if the required info is present */
    if (NULL == (evar = getenv("PMIX_NAMESPACE"))) {
        /* if we didn't see a PMIx server (e.g., missing envar),
         * then allow us to run as a singleton */
        pid = getpid();
        pmix_snprintf(pmix_globals.myid.nspace, PMIX_MAX_NSLEN, "singleton.%s.%lu",
                 pmix_globals.hostname, (unsigned long) pid);
        pmix_globals.myid.rank = 0;
        if (NULL != proc) {
            PMIX_LOAD_PROCID(proc, pmix_globals.myid.nspace, pmix_globals.myid.rank);
        }
        pmix_globals.mypeer->nptr->nspace = strdup(pmix_globals.myid.nspace);
        /* define us as an IOF endpoint so any output will be printed */
        pmix_globals.iof_flags.local_output = true;
        PMIX_CONSTRUCT(&pmix_server_globals.iof, pmix_list_t);
        PMIX_CONSTRUCT(&pmix_server_globals.iof_residuals, pmix_list_t);
        /* record that we built these so finalize tears them down - note
         * this is independent of whether we ultimately connect to a
         * server below (a system server may accept us as a singleton) */
        pmix_client_globals.local_iof = true;
    } else {
        if (NULL != proc) {
            PMIX_LOAD_NSPACE(proc->nspace, evar);
        }
        PMIX_LOAD_NSPACE(pmix_globals.myid.nspace, evar);
        /* set the global pmix_namespace_t object for our peer */
        pmix_globals.mypeer->nptr->nspace = strdup(evar);

        /* we also require our rank */
        if (PMIX_UNLIKELY(NULL == (evar = getenv("PMIX_RANK")))) {
            /* let the caller know that the server isn't available yet */
            PMIX_ERROR_LOG(PMIX_ERR_DATA_VALUE_NOT_FOUND);
            rc = PMIX_ERR_DATA_VALUE_NOT_FOUND;
            goto errout;
        } else {
            char *endp;
            unsigned long rk;

            /* Say so rather than guessing. strtol returns 0 for a string
             * it cannot read at all, so a PMIX_RANK that is empty, is not
             * a number, or is negative used to make this process rank 0 -
             * colliding with the real rank 0 and producing a job that is
             * quietly wrong instead of one that refuses to start. */
            errno = 0;
            rk = strtoul(evar, &endp, 10);
            if ('\0' == evar[0] || '\0' != *endp || '-' == evar[0]
                || 0 != errno || !PMIX_RANK_IS_VALID(rk)) {
                PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
                rc = PMIX_ERR_BAD_PARAM;
                goto errout;
            }
            pmix_globals.myid.rank = (pmix_rank_t) rk;
        }
        if (NULL != proc) {
            proc->rank = pmix_globals.myid.rank;
        }
    }
    pmix_globals.pindex = -1;
    /* setup a rank_info object for us */
    pmix_globals.mypeer->info = PMIX_NEW(pmix_rank_info_t);
    if (PMIX_UNLIKELY(NULL == pmix_globals.mypeer->info)) {
        PMIX_ERROR_LOG(PMIX_ERR_NOMEM);
        rc = PMIX_ERR_NOMEM;
        goto errout;
    }
    pmix_globals.mypeer->info->pname.nspace = strdup(pmix_globals.myid.nspace);
    pmix_globals.mypeer->info->pname.rank = pmix_globals.myid.rank;
    PMIX_LOAD_PROCID(pmix_globals.myidval.data.proc, pmix_globals.myid.nspace, pmix_globals.myid.rank);
    pmix_globals.myrankval.data.rank = pmix_globals.myid.rank;
    pmix_globals.mypeer->info->realuid = pmix_globals.realuid;
    pmix_globals.mypeer->info->uid = pmix_globals.uid;
    pmix_globals.mypeer->info->realgid = pmix_globals.realgid;
    pmix_globals.mypeer->info->gid = pmix_globals.gid;
    pmix_globals.mypeer->info->pid = pmix_globals.pid;

    /* select our psec compat module - the selection will be based
     * on the corresponding envars that should have been passed
     * to us at launch */
    evar = getenv("PMIX_SECURITY_MODE");
    pmix_globals.mypeer->nptr->compat.psec = pmix_psec_base_assign_module(evar);
    if (PMIX_UNLIKELY(NULL == pmix_globals.mypeer->nptr->compat.psec)) {
        PMIX_ERROR_LOG(PMIX_ERR_INIT);
        rc = PMIX_ERR_INIT;
        goto errout;
    }
    /* the server will be using the same */
    pmix_client_globals.myserver->nptr->compat.psec = pmix_globals.mypeer->nptr->compat.psec;

    /* set the buffer type - the selection will be based
     * on the corresponding envars that should have been passed
     * to us at launch */
    evar = getenv("PMIX_BFROP_BUFFER_TYPE");
    if (NULL == evar) {
        /* just set to our default */
        pmix_globals.mypeer->nptr->compat.type = pmix_bfrops_globals.default_type;
    } else if (0 == strcmp(evar, "PMIX_BFROP_BUFFER_FULLY_DESC")) {
        pmix_globals.mypeer->nptr->compat.type = PMIX_BFROP_BUFFER_FULLY_DESC;
    } else {
        pmix_globals.mypeer->nptr->compat.type = PMIX_BFROP_BUFFER_NON_DESC;
    }
    /* the server will be using the same */
    pmix_client_globals.myserver->nptr->compat.type = pmix_globals.mypeer->nptr->compat.type;

    /* select the gds compat module we will use to interact with
     * our server- the selection will be based
     * on the corresponding envars that should have been passed
     * to us at launch */
    evar = getenv("PMIX_GDS_MODULE");
    if (NULL != evar) {
        PMIX_INFO_LOAD(&ginfo, PMIX_GDS_MODULE, evar, PMIX_STRING);
        pmix_client_globals.myserver->nptr->compat.gds = pmix_gds_base_assign_module(&ginfo, 1);
        PMIX_INFO_DESTRUCT(&ginfo);
    } else {
        pmix_client_globals.myserver->nptr->compat.gds = pmix_gds_base_assign_module(NULL, 0);
    }
    if (PMIX_UNLIKELY(NULL == pmix_client_globals.myserver->nptr->compat.gds)) {
        PMIX_ERROR_LOG(PMIX_ERR_INIT);
        rc = PMIX_ERR_INIT;
        goto errout;
    }
    /* now select a GDS module for our own internal use - the user may
     * have passed down a directive for this purpose. If they did, then
     * use it. Otherwise, we want the "hash" module */
    found = false;
    if (NULL != info) {
        for (n = 0; n < ninfo; n++) {
            if (PMIX_CHECK_KEY(&info[n], PMIX_GDS_MODULE)) {
                PMIX_INFO_LOAD(&ginfo, PMIX_GDS_MODULE, info[n].value.data.string, PMIX_STRING);
                found = true;
                break;
            }
        }
    }
    if (!found) {
        PMIX_INFO_LOAD(&ginfo, PMIX_GDS_MODULE, "hash", PMIX_STRING);
    }
    pmix_globals.mypeer->nptr->compat.gds = pmix_gds_base_assign_module(&ginfo, 1);
    if (PMIX_UNLIKELY(NULL == pmix_globals.mypeer->nptr->compat.gds)) {
        PMIX_INFO_DESTRUCT(&ginfo);
        PMIX_ERROR_LOG(PMIX_ERR_INIT);
        rc = PMIX_ERR_INIT;
        goto errout;
    }
    PMIX_INFO_DESTRUCT(&ginfo);

    /* attempt to connect to a server */
    rc = pmix_ptl.connect_to_peer((struct pmix_peer_t *) pmix_client_globals.myserver,
                                   info, ninfo, &suri);
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        /* mark that we couldn't connect to a server */
        pmix_client_globals.singleton = true;
        /* initialize our data values */
        rc = pmix_tool_init_info();
        if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
            PMIX_ERROR_LOG(rc);
            goto errout;
        }

        /* set our server ID to be ourselves */
        pmix_client_globals.myserver->info->pname.nspace = strdup(pmix_globals.myid.nspace);
        pmix_client_globals.myserver->info->pname.rank = pmix_globals.myid.rank;
        pmix_client_globals.myserver->nptr->nspace = strdup(pmix_globals.myid.nspace);
        pmix_client_globals.myserver->info->uid = pmix_globals.uid;
        pmix_client_globals.myserver->info->gid = pmix_globals.gid;
        // set the compat entries to the same as mine
        memcpy(&pmix_client_globals.myserver->nptr->compat,
               &pmix_globals.mypeer->nptr->compat,
               sizeof(pmix_personality_t));
        /* mark that the server is unreachable */
        unreach = true;

    } else {
        /* send a request for our job info - we do this as a non-blocking
         * transaction because some systems cannot handle very large
         * blocking operations and error out if we try them. */
        req = PMIX_NEW(pmix_buffer_t);
        PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, req, &cmd, 1, PMIX_COMMAND);
        if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
            PMIX_ERROR_LOG(rc);
            PMIX_RELEASE(req);
            free(suri);
            goto errout;
        }
        /* send to the server */
        PMIX_CONSTRUCT(&cb, pmix_cb_t);
        PMIX_PTL_SEND_RECV(rc, pmix_client_globals.myserver, req, job_data, (void *) &cb);
        if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
            PMIX_ERROR_LOG(rc);
            /* the transport refused the message without taking it, and the
             * recv callback will never fire, so unwind both here */
            PMIX_RELEASE(req);
            PMIX_DESTRUCT(&cb);
            free(suri);
            goto errout;
        }
        /* wait for the data to return */
        PMIX_WAIT_THREAD(&cb.lock);
        rc = cb.status;
        PMIX_DESTRUCT(&cb);
        if (PMIX_ERR_TAKE_NEXT_OPTION == rc) {
            /* the connect-time GDS module could not deliver our job data to
             * this client; fall back to the next module and re-request */
            rc = fallback_to_next_gds();
        }
        if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
            /* the URI the connect handed us is ours until it is stored
             * below - every other exit from this branch frees it */
            free(suri);
            goto errout;
        }
    }

    /* store our server's ID */
    if (!pmix_client_globals.singleton &&
        NULL != pmix_client_globals.myserver &&
        NULL != pmix_client_globals.myserver->info) {
        kptr = PMIX_NEW(pmix_kval_t);
        kptr->key = strdup(PMIX_SERVER_NSPACE);
        PMIX_VALUE_CREATE(kptr->value, 1);
        kptr->value->type = PMIX_STRING;
        kptr->value->data.string = strdup(pmix_client_globals.myserver->info->pname.nspace);
        PMIX_GDS_STORE_KV(rc, pmix_globals.mypeer, &pmix_globals.myid, PMIX_INTERNAL, kptr);
        PMIX_RELEASE(kptr); // maintain accounting
        if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
            PMIX_ERROR_LOG(rc);
            free(suri);
            goto errout;
        }
        kptr = PMIX_NEW(pmix_kval_t);
        kptr->key = strdup(PMIX_SERVER_RANK);
        PMIX_VALUE_CREATE(kptr->value, 1);
        kptr->value->type = PMIX_PROC_RANK;
        kptr->value->data.rank = pmix_client_globals.myserver->info->pname.rank;
        PMIX_GDS_STORE_KV(rc, pmix_globals.mypeer, &pmix_globals.myid, PMIX_INTERNAL, kptr);
        PMIX_RELEASE(kptr); // maintain accounting
        if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
            PMIX_ERROR_LOG(rc);
            free(suri);
            goto errout;
        }

        /* store the URI for subsequent lookups */
        PMIX_KVAL_NEW(kptr, PMIX_SERVER_URI);
        kptr->value->type = PMIX_STRING;
        pmix_asprintf(&kptr->value->data.string, "%s.%u;%s",
                      pmix_client_globals.myserver->info->pname.nspace,
                      pmix_client_globals.myserver->info->pname.rank, suri);
        free(suri);
        PMIX_GDS_STORE_KV(rc, pmix_globals.mypeer, &pmix_globals.myid, PMIX_INTERNAL, kptr);
        PMIX_RELEASE(kptr); // maintain accounting
        if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
            PMIX_ERROR_LOG(rc);
            goto errout;
        }
    }

    // enable show_help subsystem
    pmix_atomic_store_int(&pmix_show_help_enabled, 1);

    /* retrieve our topology as a number of APIs utilize it */
    if (!pmix_globals.external_topology &&
        NULL == pmix_globals.topology.topology) {
        rc = pmix_hwloc_setup_topology(NULL, 0);
        if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
            PMIX_ERROR_LOG(rc);
            goto errout;
        }
    }

    /* look for a debugger attach key */
    pmix_strncpy(wildcard.nspace, pmix_globals.myid.nspace, PMIX_MAX_NSLEN);
    wildcard.rank = PMIX_RANK_WILDCARD;
    PMIX_CONSTRUCT(&cb, pmix_cb_t);
    cb.proc = &wildcard;
    cb.key = PMIX_DEBUG_STOP_IN_INIT;
    cb.scope = PMIX_GLOBAL;
    cb.copy = true;
    PMIX_INFO_LOAD(&ginfo, PMIX_OPTIONAL, NULL, PMIX_BOOL);  // no memory allocated
    cb.info = &ginfo;
    cb.ninfo = 1;
    PMIX_GDS_FETCH_KV(rc,  pmix_client_globals.myserver, &cb);
    if (PMIX_SUCCESS == rc) {
        pmix_rank_t rank;

        kv = (pmix_kval_t*)pmix_list_remove_first(&cb.kvs);
        PMIX_DESTRUCT(&cb);
        if (PMIX_UNLIKELY(NULL == kv)) {
            /* a successful fetch that returned nothing - treat it the same
             * as "no debugger waiting" rather than dereferencing the NULL */
            PMIX_ERROR_LOG(PMIX_ERR_NOT_FOUND);
            goto nodebugger;
        }

        if (PMIX_BOOL == kv->value->type) {
            if (kv->value->data.flag) {
                rank = PMIX_RANK_WILDCARD;
            } else {
                rank = PMIX_RANK_UNDEF;
            }
        } else if (PMIX_PROC_RANK == kv->value->type) {
            rank = kv->value->data.rank;
        } else {
            pmix_output(0, "DEBUG STOP_IN_INIT FOUND, BUT VALUE UNRECOGNIZED: %s",
                        PMIx_Data_type_string(kv->value->type));
            PMIX_RELEASE(kv);
            rc = PMIX_ERR_BAD_PARAM;
            goto errout;
        }
        pmix_output_verbose(2, pmix_client_globals.base_output,
                            "[%s:%d] RECEIVED %s FOR RANK %s (%s)",
                            pmix_globals.myid.nspace,
                            pmix_globals.myid.rank,
                            PMIx_Get_attribute_name(kv->key),
                            PMIX_RANK_PRINT(rank),
                            PMIx_Data_type_string(kv->value->type));
        PMIX_RELEASE(kv); // done with this value

        /* if the value was found and we are involved, then we need to wait
         * for debugger attach here */
        if (PMIX_RANK_WILDCARD == rank ||
            pmix_globals.myid.rank == rank) {

            /* register for the debugger release notification */
            PMIX_CONSTRUCT_LOCK(&releaselock);
            PMIX_INFO_LOAD(&evinfo[0], PMIX_EVENT_RETURN_OBJECT, &releaselock, PMIX_POINTER);
            PMIX_INFO_LOAD(&evinfo[1], PMIX_EVENT_HDLR_NAME, "WAIT-FOR-DEBUGGER", PMIX_STRING);
            PMIX_INFO_LOAD(&evinfo[2], PMIX_EVENT_ONESHOT, NULL, PMIX_BOOL);
            pmix_output_verbose(2, pmix_client_globals.event_output,
                                "[%s:%d] REGISTERING WAIT FOR DEBUGGER",
                                pmix_globals.myid.nspace, pmix_globals.myid.rank);
            cd = PMIX_NEW(pmix_rshift_caddy_t);
            if (PMIX_UNLIKELY(NULL == cd)) {
                PMIX_ERROR_LOG(PMIX_ERR_NOMEM);
                PMIX_DESTRUCT_LOCK(&releaselock);
                PMIX_INFO_DESTRUCT(&evinfo[0]);
                PMIX_INFO_DESTRUCT(&evinfo[1]);
                PMIX_INFO_DESTRUCT(&evinfo[2]);
                rc = PMIX_ERR_NOMEM;
                goto errout;
            }
            cd->codes = malloc(sizeof(int));
            if (PMIX_UNLIKELY(NULL == cd->codes)) {
                PMIX_ERROR_LOG(PMIX_ERR_NOMEM);
                PMIX_RELEASE(cd);
                PMIX_DESTRUCT_LOCK(&releaselock);
                PMIX_INFO_DESTRUCT(&evinfo[0]);
                PMIX_INFO_DESTRUCT(&evinfo[1]);
                PMIX_INFO_DESTRUCT(&evinfo[2]);
                rc = PMIX_ERR_NOMEM;
                goto errout;
            }
            cd->codes[0] = PMIX_DEBUGGER_RELEASE;
            cd->ncodes = 1;
            cd->info = evinfo;
            cd->ninfo = 3;
            cd->evhdlr = notification_fn;
            cd->evregcbfn = mycbfn;
            cd->cbdata = cd;
            PMIX_RETAIN(cd); // so pmix_internal_reg_event_hdlr doesn't wind up releasing it
            PMIX_THREADSHIFT(cd, pmix_internal_reg_event_hdlr);
            PMIX_WAIT_THREAD(&cd->lock);
            rc = cd->status;
            PMIX_RELEASE(cd);
            PMIX_INFO_DESTRUCT(&evinfo[0]);
            PMIX_INFO_DESTRUCT(&evinfo[1]);
            PMIX_INFO_DESTRUCT(&evinfo[2]);

            if (PMIX_UNLIKELY(0 > rc)) {
                PMIX_ERROR_LOG(rc);
                PMIX_DESTRUCT_LOCK(&releaselock);
                goto errout;
            }
            /* a successful registration reports the handler's index as its
             * status - hold onto it, as rc is about to be reused, and it is
             * the only way to take the registration back */
            evref = (size_t) rc;

            /* notify the host that we are waiting. The announcement is aimed
             * solely at our local server, which upcalls it to the host - the
             * other local procs have no use for it. PMIX_EVENT_CUSTOM_RANGE
             * takes a pmix_proc_t (or an array of them), and PMIX_INFO_LOAD
             * memcpy's sizeof(pmix_proc_t) out of whatever it is handed, so
             * it has to be given the server's process ID. Handing it the
             * pmix_peer_t itself reinterpreted the object header as an
             * nspace/rank, and the notification went to a target that does
             * not exist - no debugger ever saw it. */
            PMIX_LOAD_PROCID(&srvr, pmix_client_globals.myserver->info->pname.nspace,
                             pmix_client_globals.myserver->info->pname.rank);
            PMIX_INFO_LOAD(&evinfo[0], PMIX_EVENT_NON_DEFAULT, NULL, PMIX_BOOL);
            PMIX_INFO_LOAD(&evinfo[1], PMIX_BREAKPOINT, "pmix-init", PMIX_STRING);
            PMIX_INFO_LOAD(&evinfo[2], PMIX_EVENT_DO_NOT_CACHE, NULL, PMIX_BOOL);
            PMIX_INFO_LOAD(&evinfo[3], PMIX_EVENT_CUSTOM_RANGE, &srvr, PMIX_PROC);
            scd = PMIX_NEW(pmix_shift_caddy_t);
            if (PMIX_UNLIKELY(NULL == scd)) {
                /* same unwind as a failed notification below: nobody will
                 * ever release us, and the handler holds a pointer into
                 * this stack frame */
                PMIX_ERROR_LOG(PMIX_ERR_NOMEM);
                dereg_debugger_wait(evref);
                PMIX_DESTRUCT_LOCK(&releaselock);
                PMIX_INFO_DESTRUCT(&evinfo[0]);
                PMIX_INFO_DESTRUCT(&evinfo[1]);
                PMIX_INFO_DESTRUCT(&evinfo[2]);
                PMIX_INFO_DESTRUCT(&evinfo[3]);
                rc = PMIX_ERR_NOMEM;
                goto errout;
            }
            scd->status = PMIX_READY_FOR_DEBUG;
            scd->proc = &pmix_globals.myid;
            scd->range = PMIX_RANGE_CUSTOM;
            scd->info = evinfo;
            scd->ninfo = 4;
            scd->cbfunc.opcbfn = NULL;
            scd->cbdata = NULL;
            PMIX_THREADSHIFT(scd, pmix_internal_notify_event);
            PMIX_WAIT_THREAD(&scd->lock);
            rc = scd->status;
            PMIX_RELEASE(scd);
            PMIX_INFO_DESTRUCT(&evinfo[0]);
            PMIX_INFO_DESTRUCT(&evinfo[1]);
            PMIX_INFO_DESTRUCT(&evinfo[2]);
            PMIX_INFO_DESTRUCT(&evinfo[3]);
            if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
                // failed to notify ready-for-debug. Nobody knows we are
                // waiting, so nothing will release us - but the handler we
                // just registered holds a pointer to releaselock, which is
                // on this stack frame and is about to go away. Take the
                // registration back before we leave.
                PMIX_ERROR_LOG(rc);
                dereg_debugger_wait(evref);
                PMIX_DESTRUCT_LOCK(&releaselock);
                goto errout;
            }
            /* wait for release to arrive */
            PMIX_WAIT_THREAD(&releaselock);
            PMIX_DESTRUCT_LOCK(&releaselock);
        }
    } else {
        pmix_output_verbose(2, pmix_client_globals.base_output,
                            "[%s:%d] NO DEBUGGER WAITING",
                            pmix_globals.myid.nspace,
                            pmix_globals.myid.rank);
        PMIX_DESTRUCT(&cb);
    }
nodebugger:

    /* check to see if we need to notify anyone */
    if (NULL != info) {
        _check_for_notify(info, ninfo);
    }

    /* register the client supported attrs */
    rc = pmix_register_client_attrs();
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        PMIX_ERROR_LOG(rc);
        goto errout;
    }

    /* mark ourselves as initialized - on the progress thread, which is
     * also where any deletions the server announced while we were
     * working have been waiting */
    PMIX_CONSTRUCT(&cb, pmix_cb_t);
    PMIX_THREADSHIFT(&cb, _init_complete);
    PMIX_WAIT_THREAD(&cb.lock);
    PMIX_DESTRUCT(&cb);

    if (unreach) {
        return PMIX_ERR_UNREACH;
    } else {
        return PMIX_SUCCESS;
    }

errout:
    /* Put the process back the way we found it. An init that fails has
     * to leave nothing behind: the caller is told the library is not
     * available, cannot call PMIx_Finalize to clean up after a call that
     * never succeeded, and may quite reasonably want to try again with
     * different directives - a tool pointed at another server, an
     * application falling back. Leaving the runtime standing half-built
     * and the latch below set made every such attempt answer
     * PMIX_ERR_INIT for the life of the process. */
    client_teardown();

errout_early:
    pmix_atomic_fetch_add(&client_init_cntr, -1);
    /* last, so that a concurrent PMIx_Init sees the latch clear only
     * once there is nothing left of ours for it to trip over */
    pmix_atomic_clear(&pmix_globals.init_called);
    return rc;
}

PMIX_EXPORT int PMIx_Initialized(void)
{
    return pmix_atomic_check_bool(&pmix_globals.initialized);
}

typedef struct {
    pmix_lock_t lock;
    pmix_event_t ev;
    bool active;
} pmix_client_timeout_t;

/* timer callback */
static void fin_timeout(int sd, short args, void *cbdata)
{
    pmix_client_timeout_t *tev;
    tev = (pmix_client_timeout_t *) cbdata;

    pmix_output_verbose(2, pmix_client_globals.base_output, "pmix:client finwait timeout fired");

    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    if (tev->active) {
        tev->active = false;
        PMIX_WAKEUP_THREAD(&tev->lock);
    }
}
/* callback for finalize completion */
static void finwait_cbfunc(struct pmix_peer_t *pr, pmix_ptl_hdr_t *hdr, pmix_buffer_t *buf,
                           void *cbdata)
{
    pmix_client_timeout_t *tev;
    tev = (pmix_client_timeout_t *) cbdata;

    pmix_output_verbose(2, pmix_client_globals.base_output, "pmix:client finwait_cbfunc received");

    PMIX_HIDE_UNUSED_PARAMS(pr, hdr, buf);

    if (tev->active) {
        tev->active = false;
        PMIX_WAKEUP_THREAD(&tev->lock);
    }
}

PMIX_EXPORT pmix_status_t PMIx_Finalize(const pmix_info_t info[], size_t ninfo)
{
    pmix_buffer_t *msg;
    pmix_cmd_t cmd = PMIX_FINALIZE_CMD;
    pmix_status_t rc = PMIX_SUCCESS;
    size_t n;
    pmix_client_timeout_t tev;
    struct timeval tv;
    int i;

    if (PMIX_UNLIKELY(!pmix_atomic_check_bool(&pmix_globals.initialized))) {
        return PMIX_ERR_INIT;
    }
    i = pmix_atomic_fetch_add(&client_init_cntr, -1);
    if (1 < i) {
        return PMIX_SUCCESS;
    }

    /* the teardown below waits on the progress thread more than once, and
     * it stops that thread - so from the thread itself this is not merely
     * a deadlock, it is the thread ending itself mid-callback */
    if (PMIX_UNLIKELY(pmix_progress_thread_check_blocking("PMIx_Finalize"))) {
        /* put the reference back - we did not finalize */
        pmix_atomic_fetch_add(&client_init_cntr, 1);
        return PMIX_ERR_WOULD_BLOCK;
    }

    // mark we are no longer initialized
    pmix_atomic_unset_bool(&pmix_globals.initialized);
    pmix_atomic_clear(&pmix_globals.init_called);

    pmix_output_verbose(2, pmix_client_globals.base_output,
                        "%s:%d pmix:client finalize called",
                        pmix_globals.myid.nspace, pmix_globals.myid.rank);

    /* mark that I called finalize */
    pmix_globals.mypeer->finalized = true;

    if (0 <= pmix_client_globals.myserver->sd) {
        /* check to see if we are supposed to execute a
         * blocking fence prior to actually finalizing */
        if (NULL != info && 0 < ninfo) {
            for (n = 0; n < ninfo; n++) {
                if (0 == strcmp(PMIX_EMBED_BARRIER, info[n].key)) {
                    if (PMIX_INFO_TRUE(&info[n])) {
                        rc = PMIx_Fence(NULL, 0, NULL, 0);
                        if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
                            PMIX_ERROR_LOG(rc);
                        }
                    }
                    break;
                }
            }
        }

        /* setup a cmd message to notify the PMIx
         * server that we are normally terminating */
        msg = PMIX_NEW(pmix_buffer_t);
        /* pack the cmd */
        PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, msg, &cmd, 1, PMIX_COMMAND);
        if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
            PMIX_ERROR_LOG(rc);
            PMIX_RELEASE(msg);
            /* We have already declared ourselves uninitialized, so the
             * caller cannot come back and ask again - which makes the
             * teardown below the only chance this process gets to stop
             * its progress thread and give back what init allocated.
             * Failing to tell the server we are leaving is worth
             * reporting, but it is not a reason to skip all of that. */
            goto teardown;
        }

        pmix_output_verbose(2, pmix_client_globals.base_output,
                            "%s:%d pmix:client sending finalize sync to server",
                            pmix_globals.myid.nspace, pmix_globals.myid.rank);

        /* setup a timer to protect ourselves should the server be unable
         * to answer for some reason */
        PMIX_CONSTRUCT_LOCK(&tev.lock);
        pmix_event_assign(&tev.ev, pmix_globals.evbase, -1, 0, fin_timeout, &tev);
        tev.active = true;
        PMIX_POST_OBJECT(&tev);
        tv.tv_sec = pmix_server_client_fintime;
        tv.tv_usec = 0;
        pmix_event_add(&tev.ev, &tv);
        /* send to the server */
        PMIX_PTL_SEND_RECV(rc, pmix_client_globals.myserver, msg, finwait_cbfunc, (void *) &tev);
        if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
            /* the recv callback will not fire, so cancel the timer and
             * tear down the lock - and reclaim the message the transport
             * declined to take. Then finish finalizing anyway; see the
             * pack failure above. */
            PMIX_RELEASE(msg);
            pmix_event_del(&tev.ev);
            PMIX_DESTRUCT_LOCK(&tev.lock);
            goto teardown;
        }

        /* wait for the ack to return */
        PMIX_WAIT_THREAD(&tev.lock);
        /* cancel the protection timer. If the server answered, the timer
         * is still pending and must be removed from the event base before
         * this stack frame (which holds tev) goes away; if the timer fired
         * instead, deleting an inactive event is a harmless no-op */
        pmix_event_del(&tev.ev);
        PMIX_DESTRUCT_LOCK(&tev.lock);

        pmix_output_verbose(2, pmix_client_globals.base_output,
                            "%s:%d pmix:client finalize sync received", pmix_globals.myid.nspace,
                            pmix_globals.myid.rank);
    }

teardown:
    client_teardown();

    /* report whatever went wrong talking to the server, now that the
     * teardown it must not skip has run */
    return rc;
}

PMIX_EXPORT pmix_status_t PMIx_Abort(int flag, const char msg[],
                                     pmix_proc_t procs[], size_t nprocs)
{
    pmix_buffer_t *bfr;
    pmix_cmd_t cmd = PMIX_ABORT_CMD;
    pmix_status_t rc;
    pmix_cb_t *cb;

    pmix_output_verbose(2, pmix_client_globals.base_output,
                        "pmix:client abort called");

    if (PMIX_UNLIKELY(!pmix_atomic_check_bool(&pmix_globals.initialized))) {
        return PMIX_ERR_INIT;
    }

    if (PMIX_UNLIKELY(pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped))) {
        return PMIX_ERR_NOT_AVAILABLE;
    }

    /* if we are a server (and not a tool), then try to
     * handle this directly */
    if (PMIX_PEER_IS_SERVER(pmix_globals.mypeer) &&
        !PMIX_PEER_IS_TOOL(pmix_globals.mypeer)) {
        pmix_lock_t lock;

        if (NULL == pmix_host_server.abort) {
            return PMIX_ERR_NOT_SUPPORTED;
        }
        /* We wait for the host's answer below, so screen for the caller
         * being the thread that would have to deliver it. */
        if (PMIX_UNLIKELY(pmix_progress_thread_check_blocking("PMIx_Abort"))) {
            return PMIX_ERR_WOULD_BLOCK;
        }
        /* Hand the host a callback and block on it, which is what
         * pmix_server_abort_fn_t asks of us: the up-call is defined as
         * completing through that callback, with the requestor held
         * until it does. Passing NULL got a host that honors the
         * contract a NULL dereference, and a host that guards it - as
         * PRRTE does - answered every abort with "queued", so a request
         * it went on to reject reached the caller as success. */
        PMIX_CONSTRUCT_LOCK(&lock);
        rc = pmix_host_server.abort(&pmix_globals.myid,
                                    pmix_globals.mypeer->info->server_object,
                                    flag, msg, procs, nprocs,
                                    opcb_wakeup, &lock);
        if (PMIX_SUCCESS == rc) {
            /* the host took it and will answer through the callback */
            PMIX_WAIT_THREAD(&lock);
            rc = lock.status;
        } else if (PMIX_OPERATION_SUCCEEDED == rc) {
            /* it did the work itself and will not call back. That is a
             * success to the host and has to read as one to the
             * application, which knows nothing of the up-call. */
            rc = PMIX_SUCCESS;
        }
        PMIX_DESTRUCT_LOCK(&lock);
        return rc;
    }

    /* if we aren't connected, don't attempt to send */
    if (PMIX_UNLIKELY(!pmix_atomic_check_bool(&pmix_globals.connected))) {
        return PMIX_ERR_UNREACH;
    }

    /* what would release us runs on the progress thread, so waiting
     * for it from that thread waits for ourselves */
    if (PMIX_UNLIKELY(pmix_progress_thread_check_blocking("PMIx_Abort"))) {
        return PMIX_ERR_WOULD_BLOCK;
    }

    /* create a buffer to hold the message */
    bfr = PMIX_NEW(pmix_buffer_t);
    /* pack the cmd */
    PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, bfr, &cmd, 1, PMIX_COMMAND);
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(bfr);
        return rc;
    }
    /* pack the status flag */
    PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, bfr, &flag, 1, PMIX_STATUS);
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(bfr);
        return rc;
    }
    /* pack the string message - a NULL is okay */
    PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, bfr, &msg, 1, PMIX_STRING);
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(bfr);
        return rc;
    }
    /* pack the number of procs */
    PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, bfr, &nprocs, 1, PMIX_SIZE);
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(bfr);
        return rc;
    }
    /* pack any provided procs */
    if (0 < nprocs) {
        PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, bfr, procs, nprocs, PMIX_PROC);
        if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
            PMIX_ERROR_LOG(rc);
            PMIX_RELEASE(bfr);
            return rc;
        }
    }

    /* send to the server */
    cb = PMIX_NEW(pmix_cb_t);
    PMIX_PTL_SEND_RECV(rc, pmix_client_globals.myserver, bfr, abort_cbfunc, (void *) cb);
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        PMIX_RELEASE(bfr);
        PMIX_RELEASE(cb);
        return rc;
    }

    /* wait for the release, and report what the server told us - a host
     * that refuses the abort (or does not support one) has to reach the
     * caller, not be swallowed here */
    PMIX_WAIT_THREAD(&cb->lock);
    rc = cb->pstatus;
    PMIX_RELEASE(cb);
    return rc;
}

static void _putfn(int sd, short args, void *cbdata)
{
    pmix_cb_t *cb = (pmix_cb_t *) cbdata;
    pmix_status_t rc;
    pmix_kval_t *kv = NULL;

    /* need to acquire the cb object from its originating thread */
    PMIX_ACQUIRE_OBJECT(cb);

    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    /* A delete carries no value, so it has to be handled before anything
     * below reads one. It is applied to our own store at once, the same
     * way a put is, and recorded so the commit can tell the server. */
    if (is_delete_scope(cb->scope)) {
        kv = PMIX_NEW(pmix_kval_t);
        if (PMIX_UNLIKELY(NULL == kv)) {
            rc = PMIX_ERR_NOMEM;
            goto done;
        }
        kv->key = strdup(cb->key); // the input belongs to the user
        if (PMIX_UNLIKELY(NULL == kv->key)) {
            rc = PMIX_ERR_NOMEM;
            goto done;
        }
        PMIX_GDS_STORE_KV(rc, pmix_globals.mypeer, &pmix_globals.myid, cb->scope, kv);
        if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
            PMIX_ERROR_LOG(rc);
            goto done;
        }
        /* The store above is against our own peer, which is pinned to
         * "hash". Our own committed data is ALSO in the modex, which
         * belongs to whichever module serves this namespace and answers
         * ahead of the hash - so deleting our own key without telling
         * that module leaves us still reading it.
         *
         * Not for PMIX_DEL_INTERNAL, though. That names data this
         * process put internally, which by definition never left it and
         * never reached the modex - so the only thing the namespace
         * module could be holding under that key is job-level data
         * somebody else published, and the module's del_key takes no
         * scope with which to spare it. mark_deleted() declines to tell
         * the server for the same reason. */
        if (PMIX_DEL_INTERNAL != cb->scope) {
            del_key_in_nspace_module(&pmix_globals.myid, kv->key);
        }
        mark_deleted(cb->scope, kv->key);
        goto done;
    }

    if (PMIX_CHECK_KEY(cb, PMIX_QUALIFIED_VALUE)) {
        /* type must be a data array */
        if (PMIX_DATA_ARRAY != cb->value->type) {
            rc = PMIX_ERR_BAD_PARAM;
            goto done;
        }
    }

    /* setup to xfer the data */
    kv = PMIX_NEW(pmix_kval_t);
    if (PMIX_UNLIKELY(NULL == kv)) {
        rc = PMIX_ERR_NOMEM;
        goto done;
    }
    kv->key = strdup(cb->key); // need to copy as the input belongs to the user
    if (PMIX_UNLIKELY(NULL == kv->key)) {
        rc = PMIX_ERR_NOMEM;
        goto done;
    }
    /* every "goto done" below releases the kval, and the kval destructor
     * reads value->type to decide what to free - so the value has to be in a
     * known-empty state from the moment it exists, not only once a transfer
     * has filled it in. A raw malloc() left it holding whatever was on the
     * heap, which the compression-failure path then handed straight to the
     * destructor. PMIX_VALUE_CREATE zeroes it and sets PMIX_UNDEF. */
    PMIX_VALUE_CREATE(kv->value, 1);
    if (PMIX_UNLIKELY(NULL == kv->value)) {
        rc = PMIX_ERR_NOMEM;
        goto done;
    }
    /* Store the value as the caller gave it to us. A large string used to
     * be converted to a PMIX_COMPRESSED_STRING here, which compressed the
     * copy we hand the datastore as well as the one we later put on the
     * wire - so a PMIx_Get of our own put came back as bytes the caller
     * has no way to expand, and the modex carried a pre-compressed blob
     * that the bucket-level compression could no longer find any
     * redundancy in. Compression of the fence payload belongs to the
     * fence, which compresses the whole bucket; see
     * src/mca/pcompress/AGENTS.md for the measurements. */
    PMIX_BFROPS_VALUE_XFER(rc, pmix_globals.mypeer, kv->value, cb->value);
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        PMIX_ERROR_LOG(rc);
        goto done;
    }

    /* store it */
    PMIX_GDS_STORE_KV(rc, pmix_globals.mypeer, &pmix_globals.myid, cb->scope, kv);
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        PMIX_ERROR_LOG(rc);
        goto done;
    }

    /* record that this key now has data the server has not been told
     * about, so the next commit knows to fetch and send it */
    mark_dirty(cb->scope, kv);

done:
    if (NULL != kv) {
        PMIX_RELEASE(kv); // maintain accounting
    }
    cb->pstatus = rc;
    /* post the data so the receiving thread can acquire it */
    PMIX_POST_OBJECT(cb);
    PMIX_WAKEUP_THREAD(&cb->lock);
}

PMIX_EXPORT pmix_status_t PMIx_Put(pmix_scope_t scope,
                                   const char key[],
                                   pmix_value_t *val)
{
    pmix_cb_t *cb;
    pmix_status_t rc;

    if (PMIX_UNLIKELY(!pmix_atomic_check_bool(&pmix_globals.initialized))) {
        return PMIX_ERR_INIT;
    }

    /* Screen both inputs before anything looks at them. The verbose call
     * below prints the key and dereferences the value's type. That macro
     * skips its arguments while the channel is off, so this used to crash
     * only once someone raised the verbosity - ahead of the check that was
     * meant to catch it, and precisely while being debugged. _putfn() then
     * dereferences the value again. */
    if (PMIX_UNLIKELY(NULL == key || PMIX_MAX_KEYLEN < pmix_keylen(key))) {
        return PMIX_ERR_BAD_PARAM;
    }
    /* A delete names a key to remove and carries no value; anything else
     * must have one, since _putfn dereferences it. */
    if (is_delete_scope(scope)) {
        /* A server that predates these scopes drops the block silently -
         * pmix_server_commit matches on the scope it recognizes - so the
         * delete would appear to succeed and do nothing. Refuse it here,
         * where the caller can still be told, rather than at the point
         * where nothing is listening. PMIx_Put(3) documents
         * PMIX_ERR_NOT_SUPPORTED as the answer for a scope an
         * implementation does not support. */
        if (pmix_atomic_check_bool(&pmix_globals.connected)
            && NULL != pmix_client_globals.myserver
            && PMIX_PEER_IS_EARLIER(pmix_client_globals.myserver, 7, 0, 0)) {
            return PMIX_ERR_NOT_SUPPORTED;
        }
    } else if (PMIX_UNLIKELY(NULL == val)) {
        return PMIX_ERR_BAD_PARAM;
    }

    pmix_output_verbose(2, pmix_client_globals.base_output,
                          "pmix: executing put for key %s type %s scope %s",
                          key, (NULL == val) ? "NONE" : PMIx_Data_type_string(val->type),
                          PMIx_Scope_string(scope));

    if (PMIX_UNLIKELY(pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped))) {
        return PMIX_ERR_NOT_AVAILABLE;
    }

    /* what would release us runs on the progress thread, so waiting
     * for it from that thread waits for ourselves */
    if (PMIX_UNLIKELY(pmix_progress_thread_check_blocking("PMIx_Put"))) {
        return PMIX_ERR_WOULD_BLOCK;
    }

    /* create a callback object */
    cb = PMIX_NEW(pmix_cb_t);
    cb->scope = scope;
    cb->key = (char *) key;
    cb->value = val;

    /* pass this into the event library for thread protection */
    PMIX_THREADSHIFT(cb, _putfn);

    /* wait for the result */
    PMIX_WAIT_THREAD(&cb->lock);
    rc = cb->pstatus;
    PMIX_RELEASE(cb);

    return rc;
}

/* Fetch this process's contribution at one scope and add it to the commit
 * message as a PMIX_SCOPE marker followed by a buffer of kvals. Adds
 * nothing, successfully, when there is nothing to send for this scope.
 *
 * There are two ways of deciding what to send, and the cumulative one is
 * not a legacy path waiting to be deleted - it is the fallback the delta
 * rests on. "dirty" names the keys published since the last commit;
 * "resync" asks for everything this process has published at this scope,
 * which is what every commit used to do. */
static pmix_status_t pack_commit_scope(pmix_buffer_t *msgout, pmix_cb_t *cb,
                                       pmix_scope_t scope, bool resync,
                                       char **dirty)
{
    pmix_status_t rc;
    pmix_buffer_t bkt;
    pmix_kval_t *kv;
    size_t nkvs;
    int n;

    /* one caddy serves both scopes, so start from an empty result list */
    PMIX_LIST_DESTRUCT(&cb->kvs);
    PMIX_CONSTRUCT(&cb->kvs, pmix_list_t);
    cb->proc = &pmix_globals.myid;
    cb->scope = scope;
    /* local-scope data only ever reaches another process on this node, so
     * the datastore may hand it back as a connection to its own storage;
     * remote-scope data leaves the node and needs a real copy */
    cb->copy = (PMIX_LOCAL != scope);
    cb->key = NULL;

    if (!resync && NULL != dirty) {
        for (n = 0; NULL != dirty[n]; n++) {
            /* Judge the fetch by what it added, not by what it returned.
             * The datastore reports success whenever the result list is
             * non-empty, and this list is cumulative across the loop - so
             * from the second key onward a miss came back as success, and
             * the check below never fired for any key but the first. */
            nkvs = pmix_list_get_size(&cb->kvs);
            cb->key = dirty[n];
            PMIX_GDS_FETCH_KV(rc, pmix_globals.mypeer, cb);
            if (PMIX_SUCCESS != rc || pmix_list_get_size(&cb->kvs) == nkvs) {
                /* We stored this key ourselves, so a miss means our record
                 * and the datastore disagree. Rather than send a commit
                 * that is quietly short, take everything. */
                resync = true;
                break;
            }
        }
        cb->key = NULL;
    }

    if (resync) {
        PMIX_LIST_DESTRUCT(&cb->kvs);
        PMIX_CONSTRUCT(&cb->kvs, pmix_list_t);
        PMIX_GDS_FETCH_KV(rc, pmix_globals.mypeer, cb);
        if (PMIX_SUCCESS != rc) {
            /* "not found" and its cousins mean this process has published
             * nothing at this scope, which is not an error. Anything else
             * is a datastore that could not answer, and reporting that as
             * a successful commit tells the caller its data is published
             * when it is not. */
            if (PMIX_ERR_NOT_FOUND != rc && PMIX_ERR_EXISTS_OUTSIDE_SCOPE != rc
                && PMIX_ERR_INVALID_NAMESPACE != rc) {
                PMIX_ERROR_LOG(rc);
                return rc;
            }
            return PMIX_SUCCESS;
        }
    }

    if (0 == pmix_list_get_size(&cb->kvs)) {
        return PMIX_SUCCESS;
    }

    PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, msgout, &scope, 1, PMIX_SCOPE);
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    PMIX_CONSTRUCT(&bkt, pmix_buffer_t);
    PMIX_LIST_FOREACH (kv, &cb->kvs, pmix_kval_t) {
        PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, &bkt, kv, 1, PMIX_KVAL);
        if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
            PMIX_ERROR_LOG(rc);
            PMIX_DESTRUCT(&bkt);
            return rc;
        }
    }
    /* now pack the result */
    PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, msgout, &bkt, 1, PMIX_BUFFER);
    PMIX_DESTRUCT(&bkt);
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        PMIX_ERROR_LOG(rc);
    }
    return rc;
}

/* Add a PMIX_DEL_* block naming the keys deleted since the last commit.
 *
 * These cannot be expressed through pack_commit_scope(): that fetches
 * each key back out of the datastore, and a deleted key is exactly the
 * one it will not find. The keys are stated directly instead, each as a
 * kval carrying a PMIX_UNDEF value - the server's delete path ignores
 * the value, and an empty one packs where a NULL would not.
 *
 * Emitted ahead of the data blocks by the caller, so that a key deleted
 * and then published again in the same interval ends up present: the
 * server applies the blocks in the order they appear. */
static pmix_status_t pack_delete_scope(pmix_buffer_t *msgout,
                                       pmix_scope_t scope, char **keys)
{
    pmix_status_t rc;
    pmix_buffer_t bkt;
    pmix_kval_t kv;
    int n;

    if (NULL == keys) {
        return PMIX_SUCCESS;
    }
    PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, msgout, &scope, 1, PMIX_SCOPE);
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    PMIX_CONSTRUCT(&bkt, pmix_buffer_t);
    for (n = 0; NULL != keys[n]; n++) {
        PMIX_CONSTRUCT(&kv, pmix_kval_t);
        kv.key = keys[n]; // borrowed - cleared below so the destructor leaves it
        PMIX_VALUE_CREATE(kv.value, 1);
        if (PMIX_UNLIKELY(NULL == kv.value)) {
            kv.key = NULL;
            PMIX_DESTRUCT(&kv);
            PMIX_DESTRUCT(&bkt);
            return PMIX_ERR_NOMEM;
        }
        PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, &bkt, &kv, 1, PMIX_KVAL);
        kv.key = NULL; // the argv owns it
        PMIX_DESTRUCT(&kv);
        if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
            PMIX_ERROR_LOG(rc);
            PMIX_DESTRUCT(&bkt);
            return rc;
        }
    }
    PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, msgout, &bkt, 1, PMIX_BUFFER);
    PMIX_DESTRUCT(&bkt);
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        PMIX_ERROR_LOG(rc);
    }
    return rc;
}

static void _commitfn(int sd, short args, void *cbdata)
{
    pmix_cb_t *cb = (pmix_cb_t *) cbdata;
    pmix_status_t rc;
    pmix_buffer_t *msgout;
    pmix_cmd_t cmd = PMIX_COMMIT_CMD;
    bool resync;

    /* need to acquire the cb object from its originating thread */
    PMIX_ACQUIRE_OBJECT(cb);

    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    msgout = PMIX_NEW(pmix_buffer_t);
    if (PMIX_UNLIKELY(NULL == msgout)) {
        rc = PMIX_ERR_NOMEM;
        goto error;
    }
    /* pack the cmd */
    PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, msgout, &cmd, 1, PMIX_COMMAND);
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(msgout);
        goto error;
    }

    /* Read the mode once, so both scopes are gathered the same way even
     * if something below sets the flag again. */
    resync = pmix_client_globals.commit_resync;

    /* deletions first - see pack_delete_scope() */
    rc = pack_delete_scope(msgout, PMIX_DEL_LOCAL, pmix_client_globals.del_local);
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        PMIX_RELEASE(msgout);
        goto error;
    }
    rc = pack_delete_scope(msgout, PMIX_DEL_REMOTE, pmix_client_globals.del_remote);
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        PMIX_RELEASE(msgout);
        goto error;
    }

    rc = pack_commit_scope(msgout, cb, PMIX_LOCAL, resync,
                           pmix_client_globals.dirty_local);
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        PMIX_RELEASE(msgout);
        goto error;
    }
    rc = pack_commit_scope(msgout, cb, PMIX_REMOTE, resync,
                           pmix_client_globals.dirty_remote);
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        PMIX_RELEASE(msgout);
        goto error;
    }

    /* always send, even if we have nothing to contribute, so the server knows
     * that we contributed whatever we had */
    PMIX_PTL_SEND_RECV(rc, pmix_client_globals.myserver, msgout, commit_cbfunc, (void *) cb);
    if (PMIX_SUCCESS == rc) {
        /* The transport has taken the message, so what it carries is no
         * longer outstanding. _putfn runs on this same thread, so nothing
         * can have been recorded between the packing above and here, and
         * anything put from now on is new.
         *
         * Note the record is deliberately NOT cleared on the failure paths
         * above: nothing was sent, so it still describes exactly what the
         * next commit owes the server. */
        if (NULL != pmix_client_globals.dirty_local) {
            PMIx_Argv_free(pmix_client_globals.dirty_local);
            pmix_client_globals.dirty_local = NULL;
        }
        if (NULL != pmix_client_globals.dirty_remote) {
            PMIx_Argv_free(pmix_client_globals.dirty_remote);
            pmix_client_globals.dirty_remote = NULL;
        }
        if (NULL != pmix_client_globals.del_local) {
            PMIx_Argv_free(pmix_client_globals.del_local);
            pmix_client_globals.del_local = NULL;
        }
        if (NULL != pmix_client_globals.del_remote) {
            PMIx_Argv_free(pmix_client_globals.del_remote);
            pmix_client_globals.del_remote = NULL;
        }
        pmix_client_globals.commit_resync = false;
        /* wait for the reply - commit_cbfunc sets pstatus from the status
         * the server returns and wakes the caller, so do not pre-declare
         * success here */
        return;
    }
    /* the send was refused without taking the message */
    PMIX_RELEASE(msgout);

error:
    cb->pstatus = rc;
    /* post the data so the receiving thread can acquire it */
    PMIX_POST_OBJECT(cb);
    PMIX_WAKEUP_THREAD(&cb->lock);
}

PMIX_EXPORT pmix_status_t PMIx_Commit(void)
{
    pmix_cb_t *cb;
    pmix_status_t rc;

    if (PMIX_UNLIKELY(!pmix_atomic_check_bool(&pmix_globals.initialized))) {
        return PMIX_ERR_INIT;
    }

    /* if we are a singleton, there is nothing to do */
    if (pmix_client_globals.singleton) {
        return PMIX_SUCCESS;
    }

    if (PMIX_UNLIKELY(pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped))) {
        return PMIX_ERR_NOT_AVAILABLE;
    }

    /* if we are a server (but not a tool), or we aren't connected, don't attempt to send */
    if (PMIX_PEER_IS_SERVER(pmix_globals.mypeer) &&
        !PMIX_PEER_IS_TOOL(pmix_globals.mypeer)) {
        return PMIX_SUCCESS; // not an error
    }
    if (PMIX_UNLIKELY(!pmix_atomic_check_bool(&pmix_globals.connected))) {
        return PMIX_ERR_UNREACH;
    }

    /* what would release us runs on the progress thread, so waiting
     * for it from that thread waits for ourselves */
    if (PMIX_UNLIKELY(pmix_progress_thread_check_blocking("PMIx_Commit"))) {
        return PMIX_ERR_WOULD_BLOCK;
    }

    /* create a callback object */
    cb = PMIX_NEW(pmix_cb_t);
    /* pass this into the event library for thread protection */
    PMIX_THREADSHIFT(cb, _commitfn);

    /* wait for the result */
    PMIX_WAIT_THREAD(&cb->lock);
    rc = cb->pstatus;
    PMIX_RELEASE(cb);

    return rc;
}
