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
#include "src/mca/pgpu/base/base.h"
#include "src/mca/pnet/base/base.h"
#include "src/mca/psensor/base/base.h"
#include "src/runtime/pmix_progress_threads.h"
#include "src/util/pmix_error.h"
#include "src/util/pmix_output.h"

#include "pmix_server_ops.h"
#include "src/client/pmix_client_ops.h"

/* Collect one job-level entry an update carried, for the single
 * PMIX_GDS_ADD_JOB_DATA fan-out below.
 *
 * Borrowed views of the caller's array - the datastores copy whatever
 * they keep, and this array does not outlive the call. Later entries
 * replace earlier ones with the same key, so a host that names a key
 * twice in one call gets the last word rather than two stores.
 *
 * Returns false only on allocation failure.
 */
static bool add_job_update(pmix_info_t **array, size_t *n,
                           pmix_info_t *entry)
{
    pmix_info_t *tmp;
    size_t i;

    if (0 == strlen(entry->key)) {
        return true;
    }
    for (i = 0; i < *n; i++) {
        if (PMIX_CHECK_KEY(&(*array)[i], entry->key)) {
            memcpy(&(*array)[i], entry, sizeof(pmix_info_t));
            return true;
        }
    }
    tmp = (pmix_info_t *) realloc(*array, (*n + 1) * sizeof(pmix_info_t));
    if (NULL == tmp) {
        return false;
    }
    *array = tmp;
    memcpy(&(*array)[*n], entry, sizeof(pmix_info_t));
    *n += 1;
    return true;
}

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
    pmix_nspace_caddy_t *nm;
    size_t prev_nlocal;
    bool counted;
    bool nodata;
    bool prev_all_reg;
    pmix_rank_t rank;
    size_t idpos;
    pmix_info_t *jupdates = NULL;
    size_t njupdates = 0;

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
        if (NULL == nptr->nspace) {
            /* do not put a nameless namespace on the global list - every
             * lookup in this directory strcmp's that member, so it would
             * turn a transient allocation failure into a permanent
             * segfault of the progress thread */
            PMIX_RELEASE(nptr);
            rc = PMIX_ERR_NOMEM;
            goto release;
        }
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
                    /* the type has to be checked before the darray member
                     * of the union is read: an entry carrying this key with
                     * any other type hands us whatever that member happens
                     * to overlay - a char* for a string - which is then
                     * dereferenced. Both gds/hash readers of this key check
                     * it, and one of them says in so many words that every
                     * path taking the key owes the same check */
                    if (PMIX_DATA_ARRAY != cd->info[i].value.type ||
                        NULL == cd->info[i].value.data.darray ||
                        NULL == cd->info[i].value.data.darray->array ||
                        0 == cd->info[i].value.data.darray->size) {
                        /* nothing to describe a rank with */
                        rc = PMIX_ERR_BAD_PARAM;
                        goto release;
                    }
                    iptr = (pmix_info_t*)cd->info[i].value.data.darray->array;
                    ninfo = cd->info[i].value.data.darray->size;
                    /* the array has to say which proc it describes, and the
                     * host may do that with a PMIX_RANK or a PMIX_PROCID,
                     * in any position - which is what the shared helper
                     * works out and what every other reader of this key in
                     * the tree uses. Reading iptr[0].value.data.rank
                     * outright took the union's rank member of whatever
                     * entry happened to come first: a host that identified
                     * the proc any other way had its data stored against a
                     * garbage rank, and its identifier entry stored as
                     * though it were data. */
                    rc = pmix_gds_base_proc_array_id(iptr, ninfo, &rank, &idpos);
                    if (PMIX_SUCCESS != rc) {
                        goto release;
                    }
                    PMIX_LOAD_PROCID(&proc, cd->proc.nspace, rank);
                    /* every other entry is that rank's data */
                    for (m=0; m < ninfo; m++) {
                        if (m == idpos) {
                            continue;
                        }
                        PMIX_KVAL_NEW(kv, iptr[m].key);
                        if (PMIX_UNLIKELY(NULL == kv)) {
                            rc = PMIX_ERR_NOMEM;
                            goto release;
                        }
                        PMIX_VALUE_XFER(rc, kv->value, &iptr[m].value);
                        if (PMIX_SUCCESS == rc) {
                            /* capture the store's own status - and do not
                             * store at all if the transfer failed, or we
                             * hand the datastore a value that was never
                             * populated and then report the store's result
                             * as though the transfer had succeeded */
                            rc = gds->store(&proc, PMIX_REMOTE, kv);
                        }
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
                        /* We already hold this namespace's job-level
                         * info, so this is a RESTATEMENT of it rather
                         * than the first word - which is one of the two
                         * shapes an update is allowed to take (see
                         * PMIx_server_register_nspace(3)). Treat its
                         * contents as job-level updates: the datastores
                         * drop every entry that matches what they already
                         * answer, so restating an unchanged description
                         * reaches the end of this and does nothing.
                         *
                         * Dropping it here instead is what made this API
                         * unable to update job data at all - the very
                         * thing the negative nlocalprocs form exists
                         * for. */
                        if (PMIX_DATA_ARRAY == cd->info[i].value.type &&
                            NULL != cd->info[i].value.data.darray &&
                            NULL != cd->info[i].value.data.darray->array) {
                            iptr = (pmix_info_t *) cd->info[i].value.data.darray->array;
                            ninfo = cd->info[i].value.data.darray->size;
                            for (m = 0; m < ninfo; m++) {
                                /* the leading entry names the nspace this
                                 * array describes; it is not a value */
                                if (PMIX_CHECK_KEY(&iptr[m], PMIX_NSPACE)) {
                                    continue;
                                }
                                if (!add_job_update(&jupdates, &njupdates,
                                                    &iptr[m])) {
                                    rc = PMIX_ERR_NOMEM;
                                    goto release;
                                }
                            }
                        }
                        continue;
                    }
                    // first entry is the nspace, followed by the job info
                    rc = gds->cache_job_info((struct pmix_namespace_t*)nptr, cd->info, cd->ninfo);
                    if (PMIX_SUCCESS != rc) {
                        goto release;
                    }
                    /* record it, or the guard above never fires on this
                     * path - only a prior full registration set the flag,
                     * so a second update carrying job info cached the whole
                     * array all over again */
                    nptr->job_info_recvd = true;
                } else {
                    /* A plain job-level key. This arm did not exist, so
                     * every such entry was silently dropped - which is
                     * why an update sent this way appeared to do nothing
                     * whatever the host passed. */
                    if (!add_job_update(&jupdates, &njupdates, &cd->info[i])) {
                        rc = PMIX_ERR_NOMEM;
                        goto release;
                    }
                }
            }
        }
        /* Push whatever job-level values this update carried into EVERY
         * module that holds this namespace, and let them tell the
         * clients already reading it.
         *
         * A fan-out rather than a store through one module, for the
         * reason PMIX_GDS_ADD_JOB_DATA exists: a server assigns itself
         * hash, so its own copy of a namespace's job data lives there,
         * while the segments its local clients read are shmem3's. The
         * arms above reach only the first of those, so an update sent
         * this way could never reach a client's own datastore. */
        if (0 < njupdates) {
            pmix_status_t urc;
            PMIX_GDS_ADD_JOB_DATA(urc, cd->proc.nspace, jupdates, njupdates);
            if (PMIX_SUCCESS != urc) {
                PMIX_ERROR_LOG(urc);
                rc = urc;
                goto release;
            }
        }
        rc = PMIX_SUCCESS;
        goto release;
    }
    /* remember whether this namespace's local-process count was already
     * known, because that is exactly the question "has any live tracker
     * already counted it?" - pmix_server_new_tracker counts a namespace
     * only when the count is known, and every tracker in the list was
     * created before this call */
    prev_nlocal = nptr->nlocalprocs;
    prev_all_reg = nptr->all_registered;
    nptr->nlocalprocs = cd->nlocalprocs;

    /* see if we already have everyone */
    if (nptr->nlocalprocs == pmix_list_get_size(&nptr->ranks)) {
        nptr->all_registered = true;
    }

    /* check info directives to see if we want to store this info */
    nodata = false;
    for (i = 0; i < cd->ninfo; i++) {
        if (PMIX_CHECK_KEY(&cd->info[i], PMIX_REGISTER_NODATA)) {
            nodata = true;
            break;
        }
    }

    /* PMIX_REGISTER_NODATA suppresses the *data* half of this call and
     * nothing else. It pre-registers a namespace whose job data will
     * follow later - but it still carries nlocalprocs, and that count is
     * precisely what a pending collective, a parked direct-modex request,
     * or a blocked group definition is waiting on. Returning here skipped
     * the sweep below, so a host that registered its clients first and
     * then pre-registered the namespace set all_registered and told
     * nobody: every tracker naming this namespace stayed incomplete, and
     * the two sweeps live nowhere else (only these two handlers call
     * them), so nothing later would re-drive them. */
    if (!nodata) {
        /* register nspace for each activate components */
        PMIX_GDS_ADD_NSPACE(rc, nptr->nspace, cd->nlocalprocs, cd->info, cd->ninfo);
        if (PMIX_SUCCESS != rc) {
            goto restore;
        }

        /* store this data in our own GDS module - we will retrieve
         * it later so it can be passed down to the launched procs
         * once they connect to us and we know what GDS module they
         * are using */
        PMIX_GDS_CACHE_JOB_INFO(rc, pmix_globals.mypeer, nptr, cd->info, cd->ninfo);
        if (PMIX_SUCCESS != rc) {
            goto restore;
        }
        // record that we recvd the job-level info for this namespace
        nptr->job_info_recvd = true;

        /* give the programming models a chance to add anything they need */
        rc = pmix_pmdl.register_nspace(nptr);
        if (PMIX_SUCCESS != rc) {
            goto restore;
        }
    }

    /* check any pending trackers to see if they are
     * waiting for us. There is a slight race condition whereby
     * the host server could have spawned the local client and
     * it called back into the collective -before- our local event
     * would fire the register_client callback. Deal with that here. */
    PMIX_LIST_FOREACH (trk, &pmix_server_globals.collectives, pmix_server_trkr_t) {
        /* if this tracker is already complete, then we
         * don't need to update it */
        if (trk->def_complete) {
            continue;
        }
        /* "are all this tracker's namespaces registered" is a question
         * about THIS tracker, so reset it here. Hoisted above the loop it
         * carried over: one tracker naming a namespace we have not been
         * told about left the flag false for every tracker after it in
         * the list, so those were marked incomplete - and stayed that way
         * until some later registration happened to re-walk them with a
         * clean flag. Whether a collective completed therefore depended
         * on its position in the collectives list. */
        all_def = true;
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
                /* Accumulate - do NOT assign. A hybrid collective names
                 * several namespaces, and pmix_server_new_tracker already
                 * added the local count of every one that was registered
                 * when the tracker was created. Assigning here discarded
                 * those, so a fence over two wildcard namespaces went up to
                 * the host as soon as the last-registered one's local procs
                 * had contributed - the rest then contributed to a tracker
                 * that no longer existed and hung on a fresh one nobody
                 * would ever complete.
                 *
                 * Guard against counting this namespace twice: only a
                 * namespace whose count was previously unknown can have
                 * been skipped by new_tracker. Record it on the tracker's
                 * nspace list as new_tracker does for the ones it counts. */
                if (SIZE_MAX == prev_nlocal) {
                    counted = false;
                    PMIX_LIST_FOREACH (nm, &trk->nslist, pmix_nspace_caddy_t) {
                        if (0 == strcmp(nm->ns->nspace, nptr->nspace)) {
                            counted = true;
                            break;
                        }
                    }
                    if (!counted) {
                        nm = PMIX_NEW(pmix_nspace_caddy_t);
                        if (NULL == nm) {
                            rc = PMIX_ERR_NOMEM;
                            goto release;
                        }
                        PMIX_RETAIN(nptr);
                        nm->ns = nptr;
                        pmix_list_append(&trk->nslist, &nm->super);
                        trk->nlocal += nptr->nlocalprocs;
                    }
                }
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
    /* and any group block whose definition was blocked on a participant
     * namespace we had not been told about */
    pmix_server_grp_check_pending();
    rc = PMIX_SUCCESS;
    goto release;

restore:
    /* The host is being told this registration failed, so the library
     * must not go on behaving as though it had. nlocalprocs is the one
     * thing this call publishes before any of the work above can fail,
     * and it is exactly what the rest of the directory reads to decide
     * that a namespace is fully known: leaving it set meant the next
     * register_client saw the count satisfied, set all_registered, swept
     * the pending collectives and handed them to the host - for a
     * namespace whose job data was never stored. Put the count, and the
     * flag derived from it above, back where they were. */
    nptr->nlocalprocs = prev_nlocal;
    nptr->all_registered = prev_all_reg;

release:
    /* borrowed views of cd->info, so the array goes and its entries do
     * not. Every exit from the update branch above reaches here. */
    if (NULL != jupdates) {
        free(jupdates);
    }
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

    /* A namespace name is what every lookup in this directory keys on, so
     * screen it here rather than letting an empty one through: the
     * library would build a namespace object named "" that no later
     * registration or query could name, and that a deregistration
     * carrying the same empty name would find ahead of any real one. */
    if (0 == pmix_nslen(nspace)) {
        return PMIX_ERR_BAD_PARAM;
    }

    cd = PMIX_NEW(pmix_setup_caddy_t);
    if (NULL == cd) {
        return PMIX_ERR_NOMEM;
    }
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

void pmix_server_purge_events(pmix_peer_t *peer, pmix_proc_t *proc,
                              pmix_status_t status)
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
        /* A request with no requestor peer is one THIS process registered
         * through PMIx_IOF_pull and forwarded to its own upstream server -
         * the second kind of entry this array holds (see the IOF section of
         * ../common/AGENTS.md). It belongs to us, not to the departing peer,
         * and its refid is a handle we have already handed back to our own
         * caller. Deleting it here retired a live registration of our own
         * every time any local client or tool finalized, silently stopping
         * the output it had asked for. Skip it, exactly as
         * pmix_iof_process_iof does. A requestor whose info is not yet set
         * cannot be name-matched either, so it is skipped for the same
         * reason rather than destroyed. */
        if (NULL == req->requestor || NULL == req->requestor->info) {
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
             * reply nobody would ever send.
             *
             * What they are told is our caller's to decide - see the header.
             * These are OTHER local procs, still running, and the status
             * lands as the return of their PMIx_Get. */
            pmix_server_fail_local_reqs(dlcd, status);
        }
    }

    /* and in any the host asked us for on a remote server's behalf */
    pmix_server_fail_remote_pnd(peer, proc, status);

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
                    if (NULL == tgs) {
                        /* leave the notification's target list alone - it
                         * names one departed proc, which is harmless,
                         * where memcpy'ing into NULL is not */
                        continue;
                    }
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
                pmix_pgpu.child_finalized(&proc);
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
                    pmix_pgpu.child_finalized(&proc);
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
                pmix_pgpu.local_app_finalized(nptr);
            }
            /* Remember that this rank existed and is gone.
             *
             * The entry below is about to be destroyed, and once it is,
             * a direct-modex request naming this rank looks exactly like
             * one that arrived before the rank was ever registered -
             * which _dmodex_req() answers by waiting. Nothing would ever
             * end that wait. Record the departure so it can answer "not
             * found" instead, which is the same thing a request already
             * parked when the proc finalized is told.
             *
             * Only for a named rank: p == NULL is the namespace being
             * torn down, and this list dies with it. */
            if (NULL != p) {
                pmix_proclist_t *dp = PMIX_NEW(pmix_proclist_t);
                if (NULL == dp) {
                    /* say so - without the record, a direct-modex request
                     * naming this rank parks forever instead of being told
                     * "not found", and nothing else would report why */
                    PMIX_ERROR_LOG(PMIX_ERR_NOMEM);
                } else {
                    PMIX_LOAD_PROCID(&dp->proc, info->pname.nspace,
                                     info->pname.rank);
                    pmix_list_append(&nptr->departed, &dp->super);
                }
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

    /* and any job-level GPU resources.  pgpu caches one envar list per
     * namespace, holding a reference on the namespace object while it
     * does, and this is the only thing that releases it - without this
     * call a persistent server accumulates one of each per job it ever
     * ran */
    pmix_pgpu.deregister_nspace(cd->proc.nspace);

    /* release any programming model info */
    pmix_pmdl.deregister_nspace(cd->proc.nspace);

    /* let our local storage clean up */
    PMIX_GDS_DEL_NSPACE(rc, cd->proc.nspace);

    /* remove any event registrations, IOF registrations, and
     * cached notifications targeting procs from this nspace */
    pmix_server_purge_events(NULL, &cd->proc, PMIX_ERR_NOT_FOUND);

    /* find the nspace object. Compare exactly, as every other lookup in
     * this file does: PMIX_CHECK_NSPACE answers true whenever *either*
     * name is NULL or empty, so an empty namespace reaching here matched
     * whichever namespace happened to sit first on the list and tore that
     * one down instead - releasing its clients and closing their sockets.
     * The registration paths have always used strcmp, so a name this
     * would match is not a name registration could have created. */
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

static void _register_session(int sd, short args, void *cbdata)
{
    pmix_setup_caddy_t *cd = (pmix_setup_caddy_t *) cbdata;
    pmix_status_t rc;

    PMIX_ACQUIRE_OBJECT(cd);
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    pmix_output_verbose(2, pmix_server_globals.base_output,
                        "pmix:server _register_session %u",
                        (unsigned) cd->sessionid);

    /* Every active module is told, not the one some peer resolves to:
     * which module will serve the jobs in this session is not known
     * yet, and need not be the same for all of them. Same reasoning as
     * PMIX_GDS_ADD_NSPACE. */
    PMIX_GDS_ADD_SESSION(rc, cd->sessionid, cd->info, cd->ninfo);

    if (NULL != cd->opcbfunc) {
        cd->opcbfunc(rc, cd->cbdata);
    }
    PMIX_RELEASE(cd);
}

PMIX_EXPORT pmix_status_t PMIx_server_register_session(uint32_t sessionID,
                                                       pmix_info_t info[], size_t ninfo,
                                                       pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    pmix_setup_caddy_t *cd;
    pmix_lock_t mylock;
    pmix_status_t rc;

    if (!pmix_atomic_check_bool(&pmix_globals.initialized)) {
        return PMIX_ERR_INIT;
    }

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        return PMIX_ERR_NOT_AVAILABLE;
    }

    /* UINT32_MAX is how the library spells "no session named" internally,
     * so it cannot also be a session. Refuse it here rather than let it
     * establish something no job can ever be matched against. */
    if (UINT32_MAX == sessionID) {
        return PMIX_ERR_BAD_PARAM;
    }

    pmix_output_verbose(2, pmix_server_globals.base_output,
                        "pmix:server register session %u", (unsigned) sessionID);

    cd = PMIX_NEW(pmix_setup_caddy_t);
    if (NULL == cd) {
        return PMIX_ERR_NOMEM;
    }
    cd->sessionid = sessionID;
    /* pointers, not copies: the caller keeps these alive until the
     * callback fires, which is the contract every PMIx API states */
    cd->info = info;
    cd->ninfo = ninfo;
    cd->opcbfunc = cbfunc;
    cd->cbdata = cbdata;

    /* if the provided callback is NULL, then substitute
     * our own internal cbfunc and block here */
    if (NULL == cbfunc) {
        if (pmix_progress_thread_check_blocking("PMIx_server_register_session")) {
            /* We are ON the progress thread - the host called us from
             * inside one of its own upcalls, so the loop that would run
             * the event we are about to post is the one we are standing
             * in. Complete asynchronously and report success: the
             * session will be established exactly as it would have been,
             * and every other PMIx operation is serialized through the
             * same loop, so nothing can observe the difference. */
            cd->opcbfunc = NULL;
            PMIX_THREADSHIFT(cd, _register_session);
            return PMIX_SUCCESS;
        }
        PMIX_CONSTRUCT_LOCK(&mylock);
        cd->opcbfunc = pmix_server_lock_opcbfunc;
        cd->cbdata = &mylock;
        PMIX_THREADSHIFT(cd, _register_session);
        PMIX_WAIT_THREAD(&mylock);
        rc = mylock.status;
        PMIX_DESTRUCT_LOCK(&mylock);
        return rc;
    }

    /* we have to push this into our event library to avoid
     * potential threading issues */
    PMIX_THREADSHIFT(cd, _register_session);
    return PMIX_SUCCESS;
}

static void _deregister_session(int sd, short args, void *cbdata)
{
    pmix_setup_caddy_t *cd = (pmix_setup_caddy_t *) cbdata;
    pmix_status_t rc;

    PMIX_ACQUIRE_OBJECT(cd);
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    pmix_output_verbose(2, pmix_server_globals.base_output,
                        "pmix:server _deregister_session %u",
                        (unsigned) cd->sessionid);

    /* Only the session's own data. The jobs in it are deregistered by
     * their own nspaces - a module that dropped them here would tear
     * down namespaces the host still believes it has registered. */
    PMIX_GDS_DEL_SESSION(rc, cd->sessionid);

    if (NULL != cd->opcbfunc) {
        cd->opcbfunc(rc, cd->cbdata);
    }
    PMIX_RELEASE(cd);
}

PMIX_EXPORT void PMIx_server_deregister_session(uint32_t sessionID,
                                                pmix_op_cbfunc_t cbfunc, void *cbdata)
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
        /* this API reports no status, so the callback is the only way to
         * say so - see the matching comment in PMIx_server_deregister_nspace */
        if (NULL != cbfunc) {
            cbfunc(PMIX_ERR_NOT_AVAILABLE, cbdata);
        }
        return;
    }

    if (UINT32_MAX == sessionID) {
        if (NULL != cbfunc) {
            cbfunc(PMIX_ERR_BAD_PARAM, cbdata);
        }
        return;
    }

    pmix_output_verbose(2, pmix_server_globals.base_output,
                        "pmix:server deregister session %u", (unsigned) sessionID);

    cd = PMIX_NEW(pmix_setup_caddy_t);
    if (NULL == cd) {
        if (NULL != cbfunc) {
            cbfunc(PMIX_ERR_NOMEM, cbdata);
        }
        return;
    }
    cd->sessionid = sessionID;
    cd->opcbfunc = cbfunc;
    cd->cbdata = cbdata;

    if (NULL == cbfunc) {
        if (pmix_progress_thread_check_blocking("PMIx_server_deregister_session")) {
            /* see the matching comment in PMIx_server_deregister_nspace */
            cd->opcbfunc = NULL;
            PMIX_THREADSHIFT(cd, _deregister_session);
            return;
        }
        PMIX_CONSTRUCT_LOCK(&mylock);
        cd->opcbfunc = pmix_server_lock_opcbfunc;
        cd->cbdata = &mylock;
        PMIX_THREADSHIFT(cd, _deregister_session);
        PMIX_WAIT_THREAD(&mylock);
        PMIX_DESTRUCT_LOCK(&mylock);
        return;
    }

    PMIX_THREADSHIFT(cd, _deregister_session);
}

PMIX_EXPORT void PMIx_server_deregister_nspace(const pmix_nspace_t nspace, pmix_op_cbfunc_t cbfunc,
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
        /* the request cannot be serviced, and this API reports no status,
         * so the callback is the only way to say so - a host that was
         * given one and never hears back waits forever. The initialized
         * check above has always done this; this one did not */
        if (NULL != cbfunc) {
            cbfunc(PMIX_ERR_NOT_AVAILABLE, cbdata);
        }
        return;
    }

    /* an empty name is not a namespace this library could have
     * registered - see the matching comment in the handler below */
    if (0 == pmix_nslen(nspace)) {
        if (NULL != cbfunc) {
            cbfunc(PMIX_ERR_BAD_PARAM, cbdata);
        }
        return;
    }

    pmix_output_verbose(2, pmix_server_globals.base_output,
                        "pmix:server deregister nspace %s",
                        nspace);

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

/* Cancel a tracker's local-phase timeout.
 *
 * This must happen before the tracker is handed to the host, and before
 * the tracker is released on any path that tears it down here. The
 * tracker destructor does NOT delete the event, so a release with the
 * timer still armed leaves libevent holding a pointer into freed memory;
 * and once the host has the tracker, our timeout and the host's
 * completion race - a timeout that wins releases a tracker the host is
 * about to hand back. pmix_server_fence and pmix_server_connect both do
 * this at their own up-call sites and say why. */
static void cancel_collective_timer(pmix_server_trkr_t *trk)
{
    if (trk->event_active) {
        pmix_event_del(&trk->ev);
        trk->event_active = false;
    }
}

/* Is this tracker still awaiting execution? The caddy that carried it
 * onto the progress thread holds a reference, so the object outlives
 * being retired - but the reference does not put it back on the
 * collectives list. Anything that completes a collective (a departing
 * participant, a timeout) unlinks it, and a tracker that has already been
 * completed must not be handed to the host: its participants have been
 * answered and its caddies freed. Mirrors tracker_is_pending in
 * pmix_server_get.c, which guards the same window for direct modex. */
static bool collective_is_pending(pmix_server_trkr_t *trk)
{
    pmix_server_trkr_t *t;

    PMIX_LIST_FOREACH (t, &pmix_server_globals.collectives, pmix_server_trkr_t) {
        if (t == trk) {
            return true;
        }
    }
    return false;
}

/* Fail a collective we cannot hand to the host.
 *
 * Every caddy on local_cbs belongs to a client blocked in the collective,
 * and by the time we are called the tracker has been declared locally
 * complete - so no further contribution is coming and nothing else in the
 * tree will ever answer them. Returning without completing the tracker
 * therefore hangs every participant and strands the tracker on the
 * collectives list for the life of the server. Drive the completion
 * function instead: it replies to each participant and then unlinks and
 * releases the tracker. If no completion function was ever attached, tear
 * it down here - unlinking first, since being on the collectives list is
 * not a reference. */
static void fail_collective(pmix_server_trkr_t *trk, pmix_status_t status)
{
    cancel_collective_timer(trk);
    trk->host_called = false; // the host will not be calling us back
    if (PMIX_FENCENB_CMD == trk->type && NULL != trk->modexcbfunc) {
        trk->modexcbfunc(status, NULL, 0, trk, NULL, NULL);
        return;
    }
    if (NULL != trk->op_cbfunc) {
        trk->op_cbfunc(status, trk);
        return;
    }
    pmix_list_remove_item(&pmix_server_globals.collectives, &trk->super);
    PMIX_RELEASE(trk);
}

void pmix_server_execute_collective(int sd, short args, void *cbdata)
{
    pmix_trkr_caddy_t *tcd = (pmix_trkr_caddy_t *) cbdata;
    pmix_server_trkr_t *trk = tcd->trk;
    char *data = NULL;
    size_t sz = 0;
    pmix_buffer_t bucket;
    pmix_status_t rc;

    PMIX_ACQUIRE_OBJECT(tcd);
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    if (!collective_is_pending(trk)) {
        pmix_output_verbose(2, pmix_server_globals.base_output,
                            "pmix:server execute_collective on a retired tracker - discarding");
        PMIX_RELEASE(tcd);
        return;
    }

    /* Screen the host entry point we are about to call. The comment that
     * used to sit here claimed this had already been done when the
     * tracker was created, but pmix_server_new_tracker makes no such
     * check - each handler checks its own entry point only on the arm
     * where the collective completed while it was still holding it, which
     * is by definition not the arm that lands us here. */
    if (PMIX_FENCENB_CMD == trk->type) {
        if (NULL == pmix_host_server.fence_nb) {
            fail_collective(trk, PMIX_ERR_NOT_SUPPORTED);
            PMIX_RELEASE(tcd);
            return;
        }
        /* Assemble the modex bucket with the shared builder, exactly as
         * pmix_server_fence does. This was hand-rolled here, and the copy
         * had drifted from the original in two ways that matter on the
         * wire: it shipped the raw collect-type-plus-rank-blobs bucket
         * with neither the compression flag nor the enclosing byte object
         * that pmix_gds_base_store_modex unpacks, and it packed with the
         * first participant's bfrops module instead of our own. So a fence
         * that completed through this path - one that had to wait on a
         * late register_nspace or register_client - handed the host a blob
         * no receiving server could parse. Nothing here may build that
         * payload a second way. */
        PMIX_CONSTRUCT(&bucket, pmix_buffer_t);
        rc = pmix_server_collect_data(trk, &bucket);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_DESTRUCT(&bucket);
            /* every participant is parked on this tracker and nothing
             * else will ever answer them */
            fail_collective(trk, rc);
            PMIX_RELEASE(tcd);
            return;
        }
        PMIX_UNLOAD_BUFFER(&bucket, data, sz);
        PMIX_DESTRUCT(&bucket);
        /* the blob transfers to the host on the call - the request
         * direction carries no release function, and the reference host
         * reads the pointer later from another thread */
        cancel_collective_timer(trk);
        trk->host_called = true;
        rc = pmix_host_server.fence_nb(trk->pcs, trk->npcs, trk->info, trk->ninfo, data, sz,
                                       trk->modexcbfunc, trk);
        if (PMIX_SUCCESS == rc || PMIX_OPERATION_SUCCEEDED == rc) {
            /* the host has taken the bucket - see pmix_server_fence. Do
             * this before any completion below, which releases trk. */
            pmix_server_modex_contributed(trk);
        }
        if (PMIX_SUCCESS != rc && PMIX_OPERATION_SUCCEEDED != rc) {
            fail_collective(trk, rc);
        } else if (PMIX_OPERATION_SUCCEEDED == rc) {
            /* the host completed atomically and will not call us back, so
             * we owe every participant the reply ourselves */
            trk->host_called = false;
            rc = pmix_server_get_collective_status(trk->info, trk->ninfo);
            trk->modexcbfunc(rc, NULL, 0, trk, NULL, NULL);
        }
    } else if (PMIX_CONNECTNB_CMD == trk->type) {
        if (NULL == pmix_host_server.connect) {
            fail_collective(trk, PMIX_ERR_NOT_SUPPORTED);
            PMIX_RELEASE(tcd);
            return;
        }
        cancel_collective_timer(trk);
        trk->host_called = true;
        rc = pmix_host_server.connect(trk->pcs, trk->npcs, trk->info, trk->ninfo, trk->op_cbfunc,
                                      trk);
        if (PMIX_SUCCESS != rc && PMIX_OPERATION_SUCCEEDED != rc) {
            fail_collective(trk, rc);
        } else if (PMIX_OPERATION_SUCCEEDED == rc) {
            trk->host_called = false;
            rc = pmix_server_get_collective_status(trk->info, trk->ninfo);
            trk->op_cbfunc(rc, trk);
        }
    } else if (PMIX_DISCONNECTNB_CMD == trk->type) {
        if (NULL == pmix_host_server.disconnect) {
            fail_collective(trk, PMIX_ERR_NOT_SUPPORTED);
            PMIX_RELEASE(tcd);
            return;
        }
        cancel_collective_timer(trk);
        trk->host_called = true;
        rc = pmix_host_server.disconnect(trk->pcs, trk->npcs, trk->info, trk->ninfo, trk->op_cbfunc,
                                         trk);
        if (PMIX_SUCCESS != rc && PMIX_OPERATION_SUCCEEDED != rc) {
            fail_collective(trk, rc);
        } else if (PMIX_OPERATION_SUCCEEDED == rc) {
            trk->host_called = false;
            rc = pmix_server_get_collective_status(trk->info, trk->ninfo);
            trk->op_cbfunc(rc, trk);
        }
    } else {
        /* unknown type - nobody is going to complete this one, so tear it
         * down here: cancel the timer first, since the tracker destructor
         * does not, then unlink before releasing */
        PMIX_ERROR_LOG(PMIX_ERR_NOT_FOUND);
        cancel_collective_timer(trk);
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
        if (NULL == nptr->nspace) {
            /* see the matching comment in _register_nspace */
            PMIX_RELEASE(nptr);
            rc = PMIX_ERR_NOMEM;
            goto cleanup;
        }
        pmix_list_append(&pmix_globals.nspaces, &nptr->super);
    }
    /* Setup a peer object for this client - since the host server only
     * deals with the original processes and not any clones, this should
     * be called only once per rank. Say so rather than trusting it: the
     * "have we got everyone" test below is an exact equality against the
     * length of the ranks list, so a second entry for a rank pushes that
     * list permanently past nlocalprocs, all_registered is never set, and
     * every collective involving this namespace hangs with nothing
     * anywhere reporting why. */
    PMIX_LIST_FOREACH (info, &nptr->ranks, pmix_rank_info_t) {
        if (info->pname.rank == cd->proc.rank) {
            PMIX_ERROR_LOG(PMIX_ERR_DUPLICATE_KEY);
            rc = PMIX_ERR_DUPLICATE_KEY;
            goto cleanup;
        }
    }
    info = PMIX_NEW(pmix_rank_info_t);
    if (NULL == info) {
        rc = PMIX_ERR_NOMEM;
        goto cleanup;
    }
    info->pname.nspace = strdup(nptr->nspace);
    if (NULL == info->pname.nspace) {
        PMIX_RELEASE(info);
        rc = PMIX_ERR_NOMEM;
        goto cleanup;
    }
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
        PMIX_LIST_FOREACH (trk, &pmix_server_globals.collectives, pmix_server_trkr_t) {
            /* if this tracker is already complete, then we
             * don't need to update it */
            if (trk->def_complete) {
                continue;
            }
            /* per-tracker - see the matching comment in _register_nspace */
            all_def = true;
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
        /* and any group block whose definition was blocked on a
         * participant namespace we had not been told about */
        pmix_server_grp_check_pending();
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

    /* see the matching comment in PMIx_server_register_nspace - and the
     * pointer itself is dereferenced on the next line */
    if (NULL == proc || 0 == pmix_nslen(proc->nspace)) {
        return PMIX_ERR_BAD_PARAM;
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

    /* see the matching comment in PMIx_server_deregister_nspace - and the
     * pointer itself is dereferenced on the next line */
    if (NULL == proc || 0 == pmix_nslen(proc->nspace)) {
        if (NULL != cbfunc) {
            cbfunc(PMIX_ERR_BAD_PARAM, cbdata);
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
