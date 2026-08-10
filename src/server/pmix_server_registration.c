/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2014-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2014-2015 Artem Y. Polyakov <artpol84@gmail.com>.
 *                         All rights reserved.
 * Copyright (c) 2016-2020 Mellanox Technologies, Inc.
 *                         All rights reserved.
 * Copyright (c) 2016-2018 IBM Corporation.  All rights reserved.
 * Copyright (c) 2018      Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
 * Copyright (c) 2022-2023 Triad National Security, LLC. All rights reserved.
 * Copyright (c) 2026      Jeff Squyres  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/* Registration of namespaces and clients with the local PMIx server,
 * and the peer-departure bookkeeping that mirrors it. */

#include "src/include/pmix_config.h"

#include "src/include/pmix_stdint.h"

#include "pmix_common.h"
#include "include/pmix_server.h"

#include "src/include/pmix_globals.h"

#ifdef HAVE_STRING_H
#    include <string.h>
#endif
#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif

#include "src/mca/bfrops/base/base.h"
#include "src/mca/gds/base/base.h"
#include "src/mca/pmdl/base/base.h"
#include "src/mca/pnet/base/base.h"
#include "src/mca/psensor/base/base.h"
#include "src/runtime/pmix_progress_threads.h"
#include "src/util/pmix_error.h"
#include "src/util/pmix_output.h"

#include "pmix_server_ops.h"
#include "src/client/pmix_client_ops.h"

static void _register_nspace(int sd, short args, void *cbdata)
{
    pmix_setup_caddy_t *cd = (pmix_setup_caddy_t *) cbdata;
    pmix_namespace_t *nptr, *tmp;
    pmix_status_t rc;
    size_t i, m, ninfo;
    pmix_info_t *iptr;
    bool all_def;
    pmix_server_trkr_t *trk;
    pmix_namespace_t *ns;
    pmix_trkr_caddy_t *tcd;
    pmix_gds_base_module_t *gds;
    pmix_kval_t *kv;
    pmix_proc_t proc;

    PMIX_ACQUIRE_OBJECT(cd);

    pmix_output_verbose(2, pmix_server_globals.base_output,
                        "pmix:server _register_nspace %s",
                        cd->proc.nspace);

    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    /* see if we already have this nspace */
    nptr = NULL;
    PMIX_LIST_FOREACH (tmp, &pmix_globals.nspaces, pmix_namespace_t) {
        if (0 == strcmp(tmp->nspace, cd->proc.nspace)) {
            nptr = tmp;
            break;
        }
    }
    if (NULL == nptr) {
        nptr = PMIX_NEW(pmix_namespace_t);
        if (NULL == nptr) {
            rc = PMIX_ERR_NOMEM;
            goto release;
        }
        nptr->nspace = strdup(cd->proc.nspace);
        pmix_list_append(&pmix_globals.nspaces, &nptr->super);
    }
    if (0 > cd->nlocalprocs) {
        /* this is just an update, so we store it
         * in our hash datastore until someone
         * requests it */
        gds = pmix_globals.mypeer->nptr->compat.gds;
        if (NULL != gds && NULL != gds->store) {
            /* default the target to the job level. A PMIX_PROC_INFO_ARRAY
             * narrows it to the rank that array describes, but the other
             * arms below are job-level updates that carry no rank of their
             * own - and they used to store against whatever this variable
             * happened to hold, which was stack garbage unless a proc-info
             * array had come earlier in the same call */
            PMIX_LOAD_PROCID(&proc, cd->proc.nspace, PMIX_RANK_WILDCARD);
            for (i=0; i < cd->ninfo; i++) {
                if (PMIX_CHECK_KEY(&cd->info[i], PMIX_PROC_INFO_ARRAY)) {
                    if (NULL == cd->info[i].value.data.darray ||
                        NULL == cd->info[i].value.data.darray->array ||
                        0 == cd->info[i].value.data.darray->size) {
                        /* nothing to describe a rank with */
                        rc = PMIX_ERR_BAD_PARAM;
                        goto release;
                    }
                    iptr = (pmix_info_t*)cd->info[i].value.data.darray->array;
                    ninfo = cd->info[i].value.data.darray->size;
                    /* the first position is the rank */
                    PMIX_LOAD_PROCID(&proc, cd->proc.nspace, iptr[0].value.data.rank);
                    /* get the peer object for this rank */
                    for (m=1; m < ninfo; m++) {
                        PMIX_KVAL_NEW(kv, iptr[m].key);
                        if (PMIX_UNLIKELY(NULL == kv)) {
                            rc = PMIX_ERR_NOMEM;
                            goto release;
                        }
                        PMIX_VALUE_XFER(rc, kv->value, &iptr[m].value);
                        rc = gds->store(&proc, PMIX_REMOTE, kv);
                        PMIX_RELEASE(kv); // maintain refcount
                        if (PMIX_SUCCESS != rc) {
                            goto release;
                        }
                    }
                } else if (PMIX_CHECK_KEY(&cd->info[i], PMIX_GROUP_CONTEXT_ID)) {
                    PMIX_KVAL_NEW(kv, cd->info[i].key);
                    if (PMIX_UNLIKELY(NULL == kv)) {
                        rc = PMIX_ERR_NOMEM;
                        goto release;
                    }
                    PMIX_VALUE_XFER(rc, kv->value, &cd->info[i].value);
                    if (PMIX_SUCCESS == rc) {
                        /* capture the store's own status - it was being
                         * discarded, leaving the check below to re-read the
                         * transfer's result and report success for a store
                         * that had failed */
                        rc = gds->store(&proc, PMIX_GLOBAL, kv);
                    }
                    PMIX_RELEASE(kv); // maintain refcount
                    if (PMIX_SUCCESS != rc) {
                        goto release;
                    }
                } else if (PMIX_CHECK_KEY(&cd->info[i], PMIX_JOB_INFO_ARRAY)) {
                    if (nptr->job_info_recvd) {
                        // already have the job-level info for this namespace
                        continue;
                    }
                    // first entry is the nspace, followed by the job info
                    rc = gds->cache_job_info((struct pmix_namespace_t*)nptr, cd->info, cd->ninfo);
                    if (PMIX_SUCCESS != rc) {
                        goto release;
                    }
                }
            }
        }
        rc = PMIX_SUCCESS;
        goto release;
    }
    nptr->nlocalprocs = cd->nlocalprocs;

    /* see if we already have everyone */
    if (nptr->nlocalprocs == pmix_list_get_size(&nptr->ranks)) {
        nptr->all_registered = true;
    }

    /* check info directives to see if we want to store this info */
    for (i = 0; i < cd->ninfo; i++) {
        if (0 == strcmp(cd->info[i].key, PMIX_REGISTER_NODATA)) {
            /* nope - so we are done */
            rc = PMIX_SUCCESS;
            goto release;
        }
    }

    /* register nspace for each activate components */
    PMIX_GDS_ADD_NSPACE(rc, nptr->nspace, cd->nlocalprocs, cd->info, cd->ninfo);
    if (PMIX_SUCCESS != rc) {
        goto release;
    }

    /* store this data in our own GDS module - we will retrieve
     * it later so it can be passed down to the launched procs
     * once they connect to us and we know what GDS module they
     * are using */
    PMIX_GDS_CACHE_JOB_INFO(rc, pmix_globals.mypeer, nptr, cd->info, cd->ninfo);
    if (PMIX_SUCCESS != rc) {
        goto release;
    }
    // record that we recvd the job-level info for this namespace
    nptr->job_info_recvd = true;

    /* give the programming models a chance to add anything they need */
    rc = pmix_pmdl.register_nspace(nptr);
    if (PMIX_SUCCESS != rc) {
        goto release;
    }

    /* check any pending trackers to see if they are
     * waiting for us. There is a slight race condition whereby
     * the host server could have spawned the local client and
     * it called back into the collective -before- our local event
     * would fire the register_client callback. Deal with that here. */
    all_def = true;
    PMIX_LIST_FOREACH (trk, &pmix_server_globals.collectives, pmix_server_trkr_t) {
        /* if this tracker is already complete, then we
         * don't need to update it */
        if (trk->def_complete) {
            continue;
        }
        /* the fact that the tracker is here means that the tracker was
         * created in response to at least one collective call being received
         * from a participant. However, not all local participants may have
         * already called the collective. While the collective created the
         * tracker, it would not have updated the number of local participants
         * from this nspace if they specified PMIX_RANK_WILDCARD in the list of
         * participants since the host hadn't yet called "register_nspace".
         * Take care of that here */
        for (i = 0; i < trk->npcs; i++) {
            /* since we have to do this search, let's see
             * if the nspaces are all completely registered */
            if (all_def) {
                /* so far, they have all been defined - check this one */
                PMIX_LIST_FOREACH (ns, &pmix_globals.nspaces, pmix_namespace_t) {
                    if (0 == strcmp(trk->pcs[i].nspace, ns->nspace)) {
                        if (SIZE_MAX == ns->nlocalprocs || !ns->all_registered) {
                            all_def = false;
                        }
                        break;
                    }
                }
            }
            /* now see if this nspace is the one we just registered */
            if (0 != strncmp(trk->pcs[i].nspace, nptr->nspace, PMIX_MAX_NSLEN)) {
                /* if not, then we really can't say anything more about it as
                 * we have no new information about this nspace */
                continue;
            }
            /* if this request was for all participants from this nspace, then
             * we handle this case here */
            if (PMIX_RANK_WILDCARD == trk->pcs[i].rank) {
                trk->nlocal = nptr->nlocalprocs;
                /* the total number of procs in this nspace was provided
                 * in the data blob delivered to register_nspace, so check
                 * to see if all the procs are local */
                if (nptr->nprocs != nptr->nlocalprocs) {
                    trk->local = false;
                }
                continue;
            }
        }
        /* update this tracker's status */
        trk->def_complete = all_def;
        /* is this now locally completed? */
        if (pmix_server_trk_complete(trk)) {
            /* it did, so now we need to process it
             * we don't want to block someone
             * here, so kick any completed trackers into a
             * new event for processing */
            PMIX_EXECUTE_COLLECTIVE(tcd, trk, pmix_server_execute_collective);
        }
    }
    /* also check any pending local modex requests to see if
     * someone has been waiting for a request on a remote proc
     * in one of our nspaces, but we didn't know all the local procs
     * and so couldn't determine the proc was remote */
    pmix_pending_nspace_requests(nptr);
    rc = PMIX_SUCCESS;

release:
    cd->opcbfunc(rc, cd->cbdata);
    PMIX_RELEASE(cd);
}

/* setup the data for a job */
PMIX_EXPORT pmix_status_t PMIx_server_register_nspace(const pmix_nspace_t nspace, int nlocalprocs,
                                                      pmix_info_t info[], size_t ninfo,
                                                      pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    pmix_setup_caddy_t *cd;
    pmix_status_t rc;
    pmix_lock_t mylock;

    if (!pmix_atomic_check_bool(&pmix_globals.initialized)) {
        return PMIX_ERR_INIT;
    }

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        return PMIX_ERR_NOT_AVAILABLE;
    }

    cd = PMIX_NEW(pmix_setup_caddy_t);
    pmix_strncpy(cd->proc.nspace, nspace, PMIX_MAX_NSLEN);
    cd->nlocalprocs = nlocalprocs;
    cd->opcbfunc = cbfunc;
    cd->cbdata = cbdata;
    /* copy across the info array, if given */
    if (0 < ninfo) {
        cd->ninfo = ninfo;
        cd->info = info;
    }

    /* if the provided callback is NULL, then substitute
     * our own internal cbfunc and block here */
    if (NULL == cbfunc) {
        if (pmix_progress_thread_check_blocking("PMIx_server_register_nspace")) {
            /* we are ON the progress thread, so waiting for the event we
             * would post is waiting for ourselves - answer rather than
             * hang. The caller wanted the blocking form; the non-blocking
             * one works fine from here */
            PMIX_RELEASE(cd);
            return PMIX_ERR_WOULD_BLOCK;
        }
        PMIX_CONSTRUCT_LOCK(&mylock);
        cd->opcbfunc = pmix_server_lock_opcbfunc;
        cd->cbdata = &mylock;
        PMIX_THREADSHIFT(cd, _register_nspace);
        PMIX_WAIT_THREAD(&mylock);
        rc = mylock.status;
        PMIX_DESTRUCT_LOCK(&mylock);
        if (PMIX_SUCCESS == rc) {
            rc = PMIX_OPERATION_SUCCEEDED;
        }
        return rc;
    }

    /* we have to push this into our event library to avoid
     * potential threading issues */
    PMIX_THREADSHIFT(cd, _register_nspace);
    return PMIX_SUCCESS;
}

void pmix_server_purge_events(pmix_peer_t *peer, pmix_proc_t *proc)
{
    pmix_regevents_info_t *reginfo, *regnext;
    pmix_peer_events_info_t *prev, *pnext;
    pmix_iof_req_t *req;
    int i;
    pmix_notify_caddy_t *ncd;
    size_t n, m, p, ntgs;
    pmix_proc_t *tgs, *tgt;
    pmix_dmdx_local_t *dlcd, *dnxt;

    /* since the client is finalizing, remove them from any event
     * registrations they may still have on our list */
    PMIX_LIST_FOREACH_SAFE (reginfo, regnext, &pmix_server_globals.events, pmix_regevents_info_t) {
        PMIX_LIST_FOREACH_SAFE (prev, pnext, &reginfo->peers, pmix_peer_events_info_t) {
            if ((NULL != peer && prev->peer == peer)
                || (NULL != proc && NULL != prev->peer->info
                    && PMIX_CHECK_NAMES(proc, &prev->peer->info->pname))) {
                pmix_list_remove_item(&reginfo->peers, &prev->super);
                PMIX_RELEASE(prev);
                /* if nobody is left registered for this code, then drop
                 * it and tell our host to stop forwarding it */
                if (pmix_server_prune_reginfo(reginfo)) {
                    break;
                }
            }
        }
    }

    /* since the client is finalizing, remove them from any IOF
     * registrations they may still have on our list */
    for (i = 0; i < pmix_globals.iof_requests.size; i++) {
        if (NULL
            == (req = (pmix_iof_req_t *) pmix_pointer_array_get_item(&pmix_globals.iof_requests,
                                                                     i))) {
            continue;
        }
        /* protect against errors */
        if (NULL == req->requestor || NULL == req->requestor->info) {
            pmix_pointer_array_set_item(&pmix_globals.iof_requests, i, NULL);
            PMIX_RELEASE(req);
            continue;
        }
        if (NULL != peer && NULL == peer->info) {
            continue;
        }
        if ((NULL != peer && NULL != peer->info
             && PMIX_CHECK_NAMES(&req->requestor->info->pname, &peer->info->pname))
            || (NULL != proc && PMIX_CHECK_NAMES(&req->requestor->info->pname, proc))) {
            pmix_pointer_array_set_item(&pmix_globals.iof_requests, i, NULL);
            PMIX_RELEASE(req);
        }
    }

    /* see if this proc is involved in any direct modex requests */
    PMIX_LIST_FOREACH_SAFE (dlcd, dnxt, &pmix_server_globals.local_reqs, pmix_dmdx_local_t) {
        if ((NULL != peer && NULL != peer->info
             && PMIX_CHECK_NAMES(&peer->info->pname, &dlcd->proc))
            || (NULL != proc && PMIX_CHECK_PROCID(proc, &dlcd->proc))) {
            /* The proc this request is waiting on has departed, so the
             * data is never coming. Tell everyone parked on it rather than
             * dropping the tracker on the floor: each parked request holds
             * its own reference on the tracker and owns the server caddy of
             * the client behind it, so releasing the tracker alone left it
             * alive but unreachable and left those clients waiting on a
             * reply nobody would ever send. */
            pmix_server_fail_local_reqs(dlcd, PMIX_ERR_LOST_CONNECTION);
        }
    }

    /* and in any the host asked us for on a remote server's behalf */
    pmix_server_fail_remote_pnd(peer, proc, PMIX_ERR_LOST_CONNECTION);

    /* purge this client from any cached notifications */
    for (i = 0; i < pmix_globals.max_events; i++) {
        pmix_hotel_knock(&pmix_globals.notifications, i, (void **) &ncd);
        if (NULL != ncd && NULL != ncd->targets && 0 < ncd->ntargets) {
            tgt = NULL;
            for (n = 0; n < ncd->ntargets; n++) {
                if ((NULL != peer && NULL != peer->info
                     && PMIX_CHECK_NAMES(&peer->info->pname, &ncd->targets[n]))
                    || (NULL != proc && PMIX_CHECK_PROCID(proc, &ncd->targets[n]))) {
                    tgt = &ncd->targets[n];
                    break;
                }
            }
            if (NULL != tgt) {
                /* if this client was the only target, then just
                 * evict the notification */
                if (1 == ncd->ntargets) {
                    pmix_hotel_checkout(&pmix_globals.notifications, i);
                    PMIX_RELEASE(ncd);
                } else if (PMIX_RANK_WILDCARD == tgt->rank && NULL != proc
                           && PMIX_RANK_WILDCARD == proc->rank) {
                    /* we have to remove this target, but leave the rest */
                    ntgs = ncd->ntargets - 1;
                    PMIX_PROC_CREATE(tgs, ntgs);
                    p = 0;
                    for (m = 0; m < ncd->ntargets; m++) {
                        if (tgt != &ncd->targets[m]) {
                            /* copy the target this iteration is looking at -
                             * indexing by n copied the ONE target being
                             * removed into every surviving slot, so the
                             * notification came out addressed to N-1 copies
                             * of the proc that had just gone away */
                            memcpy(&tgs[p], &ncd->targets[m], sizeof(pmix_proc_t));
                            ++p;
                        }
                    }
                    PMIX_PROC_FREE(ncd->targets, ncd->ntargets);
                    ncd->targets = tgs;
                    ncd->ntargets = ntgs;
                }
            }
        }
    }

    if (NULL != peer) {
        /* ensure we honor any peer-level epilog requests */
        pmix_execute_epilog(&peer->epilog);
    }
}

void pmix_server_peer_finalized(pmix_peer_t *peer)
{
    pmix_rank_info_t *info = peer->info;
    pmix_namespace_t *nptr = peer->nptr;
    pmix_peer_t *sib;
    int i;

    /* This is a cleanly-finalized local peer whose socket has now dropped.
     * lost_connection has already stopped the peer's events and closed its
     * socket, and peer->finalized is true, so the object is inert to every
     * finalized-guarded send path.
     *
     * First, reduce the rank's live-process count. */
    if (NULL != info && 0 < info->proc_cnt) {
        --info->proc_cnt;
    }

    if (NULL != info && info->peerid == peer->index) {
        /* This peer is still the rank's referenced peer. How we retire it
         * depends on whether it is an application client or a tool. */
        if (PMIX_PEER_IS_CLIENT(peer)) {
            /* A client can be resolved through info->peerid by a *different*
             * local process's spawn/connect/disconnect collective or
             * direct-modex get that is still walking the clients array and
             * the rank list while this peer departs. Freeing it, nulling the
             * slot, or repointing info->peerid here would mutate that shared
             * state underneath such an in-flight operation - an architecture-
             * and timing-sensitive hang of multi-local-process
             * MPI_Comm_spawn. So we deliberately leave the client in place as
             * a harmless finalized "tombstone" at its existing clients slot,
             * counted as finalized (nfinalized untouched), and reclaim it at
             * a safe point instead: when the rank reconnects on the next
             * PMIx_Init (the connection handler frees the stale tombstone
             * before allocating a fresh peer) or when the namespace is
             * deregistered. See docs/how-things-work/init-finalize.rst. */
            return;
        }
        /* A tool is not an application-job member: it has its own namespace
         * and is never resolved through info->peerid by a concurrent job
         * collective or direct-modex get, so it needs no tombstone. Free it
         * now and repoint the rank's referenced peerid - to a surviving tool
         * sibling sharing this info if one is still live, else to -1. This
         * both restores the post-finalize info->peerid == -1 state that
         * process_tool_request relies on to recognize a reconnecting tool,
         * and keeps tool peers from accumulating in the clients array across
         * tool init/finalize cycles that reuse the same namespace and rank.
         * Fall through to the shared free below. */
        info->peerid = -1;
        if (0 < info->proc_cnt) {
            for (i = 0; i < pmix_server_globals.clients.size; i++) {
                sib = (pmix_peer_t *) pmix_pointer_array_get_item(&pmix_server_globals.clients, i);
                if (NULL != sib && sib != peer && sib->info == info) {
                    info->peerid = i;
                    break;
                }
            }
        }
    }

    /* We reach here either for a just-retired tool referenced peer (handled
     * above) or for a stranded peer that a newer connection already took
     * over - a fork/exec'd clone still running, or a fast re-init that
     * reconnected before this socket-close was processed. In the stranded
     * case info->peerid no longer names this object, so nothing references
     * it as the rank's peer. Either way it is safe to free it now: we null
     * only its OWN clients slot (never the live referenced one), drop the
     * finalized count it was still holding, and release it. */
    if (NULL != nptr && 0 < nptr->nfinalized) {
        --nptr->nfinalized;
    }
    pmix_pointer_array_set_item(&pmix_server_globals.clients, peer->index, NULL);
    PMIX_RELEASE(peer);
}

static void remove_client(pmix_namespace_t *nptr, pmix_proc_t *p)
{
    pmix_rank_info_t *info, *inext;
    pmix_peer_t *peer;
    pmix_proc_t proc;

    PMIX_LIST_FOREACH_SAFE(info, inext, &nptr->ranks, pmix_rank_info_t) {
        if (NULL == p || info->pname.rank == p->rank) {
            if (NULL == p) {
                PMIX_LOAD_PROCID(&proc, info->pname.nspace, info->pname.rank);
            } else {
                memcpy(&proc, p, sizeof(pmix_proc_t));
            }
            /* if this client failed to call finalize, we still need
             * to restore any allocations that were given to it */
            peer = (pmix_peer_t *) pmix_pointer_array_get_item(&pmix_server_globals.clients, info->peerid);
            if (NULL == peer) {
                /* this peer never connected, and hence it won't finalize,
                 * so account for it here */
                nptr->nfinalized++;
                /* even if they never connected, resources were allocated
                 * to them, so we need to ensure they are properly released */
                pmix_pnet.child_finalized(&proc);
            } else {
                if (!peer->finalized) {
                    /* this peer connected to us, but is being deregistered
                     * without having finalized. This usually means an
                     * abnormal termination that was picked up by
                     * our host prior to our seeing the connection drop.
                     * It is also possible that we missed the dropped
                     * connection, so mark the peer as finalized so
                     * we don't duplicate account for it and take care
                     * of it here */
                    peer->finalized = true;
                    nptr->nfinalized++;
                }
                /* resources may have been allocated to them, so
                 * ensure they get cleaned up - this isn't true
                 * for tools, so don't clean them up */
                if (!PMIX_PEER_IS_TOOL(peer)) {
                    pmix_pnet.child_finalized(&proc);
                    pmix_psensor.stop(peer, NULL);
                }
                /* honor any registered epilogs */
                pmix_execute_epilog(&peer->epilog);
                /* ensure we close the socket to this peer so we don't
                 * generate "connection lost" events should it be
                 * subsequently "killed" by the host */
                CLOSE_THE_SOCKET(peer->sd);
                // remove it from our client array
                pmix_pointer_array_set_item(&pmix_server_globals.clients, info->peerid, NULL);
                PMIX_RELEASE(peer);
            }
            /* Fire the "all local processes finalized" callback exactly
             * once. In the tombstone model a finalized peer stays counted in
             * nfinalized until it is reclaimed, so nfinalized is already
             * saturated at nlocalprocs here and this equality holds on every
             * rank we iterate; without the guard the callback would fire once
             * per rank as the nspace is torn down. */
            if (!nptr->local_app_fini_fired &&
                nptr->nlocalprocs == nptr->nfinalized) {
                nptr->local_app_fini_fired = true;
                pmix_pnet.local_app_finalized(nptr);
            }
            pmix_list_remove_item(&nptr->ranks, &info->super);
            PMIX_RELEASE(info);
            if (NULL != p) {
                break;
            }
        }
    }
    return;
}

static void _deregister_nspace(int sd, short args, void *cbdata)
{
    pmix_setup_caddy_t *cd = (pmix_setup_caddy_t *) cbdata;
    pmix_namespace_t *tmp, *nptr;
    pmix_status_t rc;

    PMIX_ACQUIRE_OBJECT(cd);

    pmix_output_verbose(2, pmix_server_globals.base_output,
                        "pmix:server _deregister_nspace %s",
                        cd->proc.nspace);

    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    /* flush anything that is still trying to be written out */
    pmix_iof_static_dump_output(&pmix_client_globals.iof_stdout);
    pmix_iof_static_dump_output(&pmix_client_globals.iof_stderr);

    /* release any job-level network resources */
    pmix_pnet.deregister_nspace(cd->proc.nspace);

    /* release any programming model info */
    pmix_pmdl.deregister_nspace(cd->proc.nspace);

    /* let our local storage clean up */
    PMIX_GDS_DEL_NSPACE(rc, cd->proc.nspace);

    /* remove any event registrations, IOF registrations, and
     * cached notifications targeting procs from this nspace */
    pmix_server_purge_events(NULL, &cd->proc);

    // find the nspace object
    nptr = NULL;
    PMIX_LIST_FOREACH (tmp, &pmix_globals.nspaces, pmix_namespace_t) {
        if (PMIX_CHECK_NSPACE(tmp->nspace, cd->proc.nspace)) {
            nptr = tmp;
            break;
        }
    }
    if (NULL == nptr) {
        /* nothing to do */
        goto cleanup;
    }

    // ensure all local clients have been deregistered
    remove_client(nptr, NULL);

    /* perform any epilog */
    pmix_execute_epilog(&nptr->epilog);

    /* remove and release it */
    pmix_list_remove_item(&pmix_globals.nspaces, &nptr->super);
    PMIX_RELEASE(nptr);

cleanup:
    /* release the caller, if there is one waiting - a request that had to
     * be completed asynchronously because it came from the progress
     * thread carries no callback */
    if (NULL != cd->opcbfunc) {
        cd->opcbfunc(rc, cd->cbdata);
    }
    PMIX_RELEASE(cd);
}

PMIX_EXPORT void PMIx_server_deregister_nspace(const pmix_nspace_t nspace, pmix_op_cbfunc_t cbfunc,
                                               void *cbdata)
{
    pmix_setup_caddy_t *cd;
    pmix_lock_t mylock;

    pmix_output_verbose(2, pmix_server_globals.base_output,
                        "pmix:server deregister nspace %s",
                        nspace);

    if (!pmix_atomic_check_bool(&pmix_globals.initialized)) {
        if (NULL != cbfunc) {
            cbfunc(PMIX_ERR_INIT, cbdata);
        }
        return;
    }

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        /* the request cannot be serviced, and this API reports no status,
         * so the callback is the only way to say so - a host that was
         * given one and never hears back waits forever. The initialized
         * check above has always done this; this one did not */
        if (NULL != cbfunc) {
            cbfunc(PMIX_ERR_NOT_AVAILABLE, cbdata);
        }
        return;
    }

    cd = PMIX_NEW(pmix_setup_caddy_t);
    if (NULL == cd) {
        if (NULL != cbfunc) {
            cbfunc(PMIX_ERR_NOMEM, cbdata);
        }
        return;
    }
    PMIX_LOAD_PROCID(&cd->proc, nspace, PMIX_RANK_WILDCARD);
    cd->opcbfunc = cbfunc;
    cd->cbdata = cbdata;

    /* if the provided callback is NULL, then substitute
     * our own internal cbfunc and block here */
    if (NULL == cbfunc) {
        if (pmix_progress_thread_check_blocking("PMIx_server_deregister_nspace")) {
            /* We are ON the progress thread - the host called us from
             * inside one of its own upcalls. We cannot wait for the event
             * we are about to post, because the loop that would run it is
             * the one we are standing in.
             *
             * Complete asynchronously instead. Nothing observable is lost:
             * this API reports no status, and every other PMIx operation
             * is serialized through the same loop, so it will see the
             * namespace gone exactly as it would have. Running the handler
             * inline is NOT an option - it releases peers and the
             * namespace object itself, which the callback we are nested
             * inside may still be holding.
             */
            cd->opcbfunc = NULL;
            PMIX_THREADSHIFT(cd, _deregister_nspace);
            return;
        }
        PMIX_CONSTRUCT_LOCK(&mylock);
        cd->opcbfunc = pmix_server_lock_opcbfunc;
        cd->cbdata = &mylock;
        PMIX_THREADSHIFT(cd, _deregister_nspace);
        PMIX_WAIT_THREAD(&mylock);
        PMIX_DESTRUCT_LOCK(&mylock);
        return;
    }

    /* we have to push this into our event library to avoid
     * potential threading issues */
    PMIX_THREADSHIFT(cd, _deregister_nspace);
}

void pmix_server_execute_collective(int sd, short args, void *cbdata)
{
    pmix_trkr_caddy_t *tcd = (pmix_trkr_caddy_t *) cbdata;
    pmix_server_trkr_t *trk = tcd->trk;
    pmix_server_caddy_t *cd;
    pmix_peer_t *peer;
    char *data = NULL;
    size_t sz = 0;
    pmix_byte_object_t bo;
    pmix_buffer_t bucket, pbkt;
    pmix_kval_t *kv;
    pmix_proc_t proc;
    bool first;
    pmix_status_t rc;
    pmix_list_t pnames;
    pmix_namelist_t *pn;
    bool found;
    pmix_cb_t cb;

    PMIX_ACQUIRE_OBJECT(tcd);
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    /* we don't need to check for non-NULL APIs here as
     * that was already done when the tracker was created */
    if (PMIX_FENCENB_CMD == trk->type) {
        /* if the user asked us to collect data, then we have
         * to provide any locally collected data to the host
         * server so they can circulate it - only take data
         * from the specified procs as not everyone is necessarily
         * participating! And only take data intended for remote
         * distribution as local data will be added when we send
         * the result to our local clients */
        if (trk->hybrid) {
            /* if this is a hybrid, then we pack everything using
             * the daemon-level bfrops module as each daemon is
             * going to have to unpack it, and then repack it for
             * each participant. */
            peer = pmix_globals.mypeer;
        } else {
            /* in some error situations, the list of local callbacks can
             * be empty - if that happens, we just need to call the fence
             * function to prevent others from hanging */
            if (0 == pmix_list_get_size(&trk->local_cbs)) {
                pmix_host_server.fence_nb(trk->pcs, trk->npcs, trk->info, trk->ninfo, data, sz,
                                          trk->modexcbfunc, trk);
                PMIX_RELEASE(tcd);
                return;
            }
            /* since all procs are the same, just use the first proc's module */
            cd = (pmix_server_caddy_t *) pmix_list_get_first(&trk->local_cbs);
            peer = cd->peer;
        }
        PMIX_CONSTRUCT(&bucket, pmix_buffer_t);

        unsigned char tmp = (unsigned char) trk->collect_type;
        PMIX_BFROPS_PACK(rc, peer, &bucket, &tmp, 1, PMIX_BYTE);

        if (PMIX_COLLECT_YES == trk->collect_type) {
            pmix_output_verbose(2, pmix_server_globals.base_output, "fence - assembling data");
            first = true;
            PMIX_CONSTRUCT(&pnames, pmix_list_t);
            PMIX_LIST_FOREACH (cd, &trk->local_cbs, pmix_server_caddy_t) {
                /* see if we have already gotten the contribution from
                 * this proc */
                found = false;
                PMIX_LIST_FOREACH (pn, &pnames, pmix_namelist_t) {
                    if (pn->pname == &cd->peer->info->pname) {
                        /* got it */
                        found = true;
                        break;
                    }
                }
                if (found) {
                    continue;
                } else {
                    /* record it, or the list stays empty forever: the
                     * duplicate check above then never matches, a clone
                     * sharing a rank with its parent has its remote data
                     * packed into the bucket twice, and every one of these
                     * objects is leaked because the list destructor below
                     * has nothing to free */
                    pn = PMIX_NEW(pmix_namelist_t);
                    pn->pname = &cd->peer->info->pname;
                    pmix_list_append(&pnames, &pn->super);
                }
                if (trk->hybrid || first) {
                    /* setup the nspace */
                    pmix_strncpy(proc.nspace, cd->peer->info->pname.nspace, PMIX_MAX_NSLEN);
                    first = false;
                }
                proc.rank = cd->peer->info->pname.rank;
                /* get any remote contribution - note that there
                 * may not be a contribution */
                PMIX_CONSTRUCT(&cb, pmix_cb_t);
                cb.proc = &proc;
                cb.scope = PMIX_REMOTE;
                cb.copy = true;
                PMIX_GDS_FETCH_KV(rc, peer, &cb);
                if (PMIX_SUCCESS == rc) {
                    /* pack the returned kvals */
                    PMIX_CONSTRUCT(&pbkt, pmix_buffer_t);
                    /* start with the proc id */
                    PMIX_BFROPS_PACK(rc, peer, &pbkt, &proc, 1, PMIX_PROC);
                    if (PMIX_SUCCESS != rc) {
                        PMIX_ERROR_LOG(rc);
                        PMIX_DESTRUCT(&cb);
                        PMIX_DESTRUCT(&pbkt);
                        PMIX_DESTRUCT(&bucket);
                        PMIX_LIST_DESTRUCT(&pnames);
                        PMIX_RELEASE(tcd);
                        return;
                    }
                    PMIX_LIST_FOREACH (kv, &cb.kvs, pmix_kval_t) {
                        PMIX_BFROPS_PACK(rc, peer, &pbkt, kv, 1, PMIX_KVAL);
                        if (PMIX_SUCCESS != rc) {
                            PMIX_ERROR_LOG(rc);
                            PMIX_DESTRUCT(&cb);
                            PMIX_DESTRUCT(&pbkt);
                            PMIX_DESTRUCT(&bucket);
                            PMIX_LIST_DESTRUCT(&pnames);
                            PMIX_RELEASE(tcd);
                            return;
                        }
                    }
                    /* extract the resulting byte object */
                    PMIX_UNLOAD_BUFFER(&pbkt, bo.bytes, bo.size);
                    PMIX_DESTRUCT(&pbkt);
                    /* now pack that into the bucket for return - the pack
                     * copies, so the unloaded bytes are ours to free */
                    PMIX_BFROPS_PACK(rc, peer, &bucket, &bo, 1, PMIX_BYTE_OBJECT);
                    if (PMIX_SUCCESS != rc) {
                        PMIX_ERROR_LOG(rc);
                        PMIX_DESTRUCT(&cb);
                        PMIX_BYTE_OBJECT_DESTRUCT(&bo);
                        PMIX_DESTRUCT(&bucket);
                        PMIX_LIST_DESTRUCT(&pnames);
                        PMIX_RELEASE(tcd);
                        return;
                    }
                    PMIX_BYTE_OBJECT_DESTRUCT(&bo);
                }
                PMIX_DESTRUCT(&cb);
            }
            PMIX_LIST_DESTRUCT(&pnames);
        }
        PMIX_UNLOAD_BUFFER(&bucket, data, sz);
        PMIX_DESTRUCT(&bucket);
        pmix_host_server.fence_nb(trk->pcs, trk->npcs, trk->info, trk->ninfo, data, sz,
                                  trk->modexcbfunc, trk);
    } else if (PMIX_CONNECTNB_CMD == trk->type) {
        pmix_host_server.connect(trk->pcs, trk->npcs, trk->info, trk->ninfo, trk->op_cbfunc, trk);
    } else if (PMIX_DISCONNECTNB_CMD == trk->type) {
        pmix_host_server.disconnect(trk->pcs, trk->npcs, trk->info, trk->ninfo, trk->op_cbfunc,
                                    trk);
    } else {
        /* unknown type */
        PMIX_ERROR_LOG(PMIX_ERR_NOT_FOUND);
        pmix_list_remove_item(&pmix_server_globals.collectives, &trk->super);
        PMIX_RELEASE(trk);
    }
    PMIX_RELEASE(tcd);
}

static void _register_client(int sd, short args, void *cbdata)
{
    pmix_setup_caddy_t *cd = (pmix_setup_caddy_t *) cbdata;
    pmix_rank_info_t *info;
    pmix_namespace_t *nptr, *ns;
    pmix_server_trkr_t *trk;
    pmix_trkr_caddy_t *tcd;
    bool all_def;
    size_t i;
    pmix_status_t rc;

    PMIX_ACQUIRE_OBJECT(cd);
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    pmix_output_verbose(2, pmix_server_globals.base_output,
                        "pmix:server _register_client for nspace %s rank %d %s object",
                        cd->proc.nspace, cd->proc.rank,
                        (NULL == cd->server_object) ? "NULL" : "NON-NULL");

    /* see if we already have this nspace */
    nptr = NULL;
    PMIX_LIST_FOREACH (ns, &pmix_globals.nspaces, pmix_namespace_t) {
        if (0 == strcmp(ns->nspace, cd->proc.nspace)) {
            nptr = ns;
            break;
        }
    }
    if (NULL == nptr) {
        /* there is no requirement in the Standard that hosts register
         * an nspace prior to registering clients for that nspace. So
         * if we didn't find it, just add it to our collection now in
         * anticipation of eventually getting a "register_nspace" call */
        nptr = PMIX_NEW(pmix_namespace_t);
        if (NULL == nptr) {
            rc = PMIX_ERR_NOMEM;
            goto cleanup;
        }
        nptr->nspace = strdup(cd->proc.nspace);
        pmix_list_append(&pmix_globals.nspaces, &nptr->super);
    }
    /* setup a peer object for this client - since the host server
     * only deals with the original processes and not any clones,
     * we know this function will be called only once per rank */
    info = PMIX_NEW(pmix_rank_info_t);
    if (NULL == info) {
        rc = PMIX_ERR_NOMEM;
        goto cleanup;
    }
    info->pname.nspace = strdup(nptr->nspace);
    info->pname.rank = cd->proc.rank;
    info->uid = cd->uid;
    info->gid = cd->gid;
    info->server_object = cd->server_object;
    pmix_list_append(&nptr->ranks, &info->super);
    /* see if we have everyone - note that nlocalprocs is set to
     * a default value to ensure we don't execute this
     * test until the host calls "register_nspace" */
    if (SIZE_MAX != nptr->nlocalprocs && nptr->nlocalprocs == pmix_list_get_size(&nptr->ranks)) {
        nptr->all_registered = true;
        /* check any pending trackers to see if they are
         * waiting for us. There is a slight race condition whereby
         * the host server could have spawned the local client and
         * it called back into the collective -before- our local event
         * would fire the register_client callback. Deal with that here. */
        all_def = true;
        PMIX_LIST_FOREACH (trk, &pmix_server_globals.collectives, pmix_server_trkr_t) {
            /* if this tracker is already complete, then we
             * don't need to update it */
            if (trk->def_complete) {
                continue;
            }
            /* the fact that the tracker is here means that the tracker was
             * created in response to at least one collective call being received
             * from a participant. However, not all local participants may have
             * already called the collective. While the collective created the
             * tracker, it would not have updated the number of local participants
             * from this nspace UNLESS the collective involves all procs in the
             * nspace (i.e., they specified PMIX_RANK_WILDCARD in the list of
             * participants) AND the host already provided the number of local
             * procs for this nspace by calling "register_nspace". So avoid that
             * scenario here to avoid double-counting */
            for (i = 0; i < trk->npcs; i++) {
                /* since we have to do this search, let's see
                 * if the nspaces are all completely registered */
                if (all_def) {
                    /* so far, they have all been defined - check this one */
                    PMIX_LIST_FOREACH (ns, &pmix_globals.nspaces, pmix_namespace_t) {
                        if (0 == strcmp(trk->pcs[i].nspace, ns->nspace)) {
                            if (SIZE_MAX == ns->nlocalprocs || !ns->all_registered) {
                                all_def = false;
                            }
                            break;
                        }
                    }
                }
                /* now see if this nspace is the one to which the client we just
                 * registered belongs */
                if (0 != strncmp(trk->pcs[i].nspace, nptr->nspace, PMIX_MAX_NSLEN)) {
                    /* if not, then we really can't say anything more about it as
                     * we have no new information about this nspace */
                    continue;
                }
                /* if this request was for all participants from this nspace, then
                 * we handle this case elsewhere */
                if (PMIX_RANK_WILDCARD == trk->pcs[i].rank) {
                    continue;
                }
                /* see if the rank we just registered is a participant */
                if (cd->proc.rank == trk->pcs[i].rank) {
                    /* yes, we are included */
                    ++trk->nlocal;
                }
            }
            /* update this tracker's status */
            trk->def_complete = all_def;
            /* is this now locally completed? */
            if (pmix_server_trk_complete(trk)) {
                /* it did, so now we need to process it
                 * we don't want to block someone
                 * here, so kick any completed trackers into a
                 * new event for processing */
                PMIX_EXECUTE_COLLECTIVE(tcd, trk, pmix_server_execute_collective);
            }
        }
        /* also check any pending local modex requests to see if
         * someone has been waiting for a request on a remote proc
         * in one of our nspaces, but we didn't know all the local procs
         * and so couldn't determine the proc was remote */
        pmix_pending_nspace_requests(nptr);
    }
    rc = PMIX_SUCCESS;

cleanup:
    /* let the caller know we are done */
    cd->opcbfunc(rc, cd->cbdata);
    PMIX_RELEASE(cd);
}

PMIX_EXPORT pmix_status_t PMIx_server_register_client(const pmix_proc_t *proc, uid_t uid, gid_t gid,
                                                      void *server_object, pmix_op_cbfunc_t cbfunc,
                                                      void *cbdata)
{
    pmix_setup_caddy_t *cd;
    pmix_status_t rc;
    pmix_lock_t mylock;

    if (!pmix_atomic_check_bool(&pmix_globals.initialized)) {
        return PMIX_ERR_INIT;
    }

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        return PMIX_ERR_NOT_AVAILABLE;
    }

    pmix_output_verbose(2, pmix_server_globals.base_output,
                        "pmix:server register client %s:%d",
                        proc->nspace, proc->rank);

    cd = PMIX_NEW(pmix_setup_caddy_t);
    if (NULL == cd) {
        return PMIX_ERR_NOMEM;
    }
    pmix_strncpy(cd->proc.nspace, proc->nspace, PMIX_MAX_NSLEN);
    cd->proc.rank = proc->rank;
    cd->uid = uid;
    cd->gid = gid;
    cd->server_object = server_object;
    cd->opcbfunc = cbfunc;
    cd->cbdata = cbdata;

    /* if the provided callback is NULL, then substitute
     * our own internal cbfunc and block here */
    if (NULL == cbfunc) {
        if (pmix_progress_thread_check_blocking("PMIx_server_register_client")) {
            /* we are ON the progress thread, so waiting for the event we
             * would post is waiting for ourselves - answer rather than
             * hang. The caller wanted the blocking form; the non-blocking
             * one works fine from here */
            PMIX_RELEASE(cd);
            return PMIX_ERR_WOULD_BLOCK;
        }
        PMIX_CONSTRUCT_LOCK(&mylock);
        cd->opcbfunc = pmix_server_lock_opcbfunc;
        cd->cbdata = &mylock;
        PMIX_THREADSHIFT(cd, _register_client);
        PMIX_WAIT_THREAD(&mylock);
        rc = mylock.status;
        PMIX_DESTRUCT_LOCK(&mylock);
        if (PMIX_SUCCESS == rc) {
            rc = PMIX_OPERATION_SUCCEEDED;
        }
        return rc;
    }

    /* we have to push this into our event library to avoid
     * potential threading issues */
    PMIX_THREADSHIFT(cd, _register_client);
    return PMIX_SUCCESS;
}

static void _deregister_client(int sd, short args, void *cbdata)
{
    pmix_setup_caddy_t *cd = (pmix_setup_caddy_t *) cbdata;
    pmix_namespace_t *nptr, *tmp;

    PMIX_ACQUIRE_OBJECT(cd);
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    pmix_output_verbose(2, pmix_server_globals.base_output,
                        "pmix:server _deregister_client for nspace %s rank %d", cd->proc.nspace,
                        cd->proc.rank);

    /* see if we already have this nspace */
    nptr = NULL;
    PMIX_LIST_FOREACH (tmp, &pmix_globals.nspaces, pmix_namespace_t) {
        if (0 == strcmp(tmp->nspace, cd->proc.nspace)) {
            nptr = tmp;
            break;
        }
    }
    if (NULL == nptr) {
        /* nothing to do */
        goto cleanup;
    }

    /* find and remove this client */
    remove_client(nptr, &cd->proc);

cleanup:
    /* release the caller, if there is one waiting - see the matching
     * comment in _deregister_nspace */
    if (NULL != cd->opcbfunc) {
        cd->opcbfunc(PMIX_SUCCESS, cd->cbdata);
    }
    PMIX_RELEASE(cd);
}

PMIX_EXPORT void PMIx_server_deregister_client(const pmix_proc_t *proc, pmix_op_cbfunc_t cbfunc,
                                               void *cbdata)
{
    pmix_setup_caddy_t *cd;
    pmix_lock_t mylock;

    if (!pmix_atomic_check_bool(&pmix_globals.initialized)) {
        if (NULL != cbfunc) {
            cbfunc(PMIX_ERR_INIT, cbdata);
        }
        return;
    }

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        /* see the matching comment in PMIx_server_deregister_nspace */
        if (NULL != cbfunc) {
            cbfunc(PMIX_ERR_NOT_AVAILABLE, cbdata);
        }
        return;
    }

    pmix_output_verbose(2, pmix_server_globals.base_output,
                        "pmix:server deregister client %s:%d",
                        proc->nspace, proc->rank);

    cd = PMIX_NEW(pmix_setup_caddy_t);
    if (NULL == cd) {
        if (NULL != cbfunc) {
            cbfunc(PMIX_ERR_NOMEM, cbdata);
        }
        return;
    }
    pmix_strncpy(cd->proc.nspace, proc->nspace, PMIX_MAX_NSLEN);
    cd->proc.rank = proc->rank;
    cd->opcbfunc = cbfunc;
    cd->cbdata = cbdata;

    /* if the provided callback is NULL, then substitute
     * our own internal cbfunc and block here */
    if (NULL == cbfunc) {
        if (pmix_progress_thread_check_blocking("PMIx_server_deregister_client")) {
            /* on the progress thread - complete asynchronously rather
             * than wait on the loop we are standing in. See the matching
             * comment in PMIx_server_deregister_nspace */
            cd->opcbfunc = NULL;
            PMIX_THREADSHIFT(cd, _deregister_client);
            return;
        }
        PMIX_CONSTRUCT_LOCK(&mylock);
        cd->opcbfunc = pmix_server_lock_opcbfunc;
        cd->cbdata = &mylock;
        PMIX_THREADSHIFT(cd, _deregister_client);
        PMIX_WAIT_THREAD(&mylock);
        PMIX_DESTRUCT_LOCK(&mylock);
        return;
    }

    /* we have to push this into our event library to avoid
     * potential threading issues */
    PMIX_THREADSHIFT(cd, _deregister_client);
}
