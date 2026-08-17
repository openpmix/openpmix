/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2014-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2014-2015 Artem Y. Polyakov <artpol84@gmail.com>.
 *                         All rights reserved.
 * Copyright (c) 2016-2019 Mellanox Technologies, Inc.
 *                         All rights reserved.
 * Copyright (c) 2016-2020 IBM Corporation.  All rights reserved.
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
 * Copyright (c) 2022-2023 Triad National Security, LLC. All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "src/include/pmix_config.h"

#include "src/include/pmix_socket_errno.h"
#include "src/include/pmix_stdint.h"

#include "include/pmix_server.h"
#include "src/include/pmix_globals.h"

#ifdef HAVE_STRING_H
#    include <string.h>
#endif
#ifdef HAVE_SYS_STAT_H
#    include <sys/stat.h>
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
#ifdef HAVE_TIME_H
#    include <time.h>
#endif
#include <event.h>

#include "src/class/pmix_hotel.h"
#include "src/class/pmix_list.h"
#include "src/common/pmix_attributes.h"
#include "src/common/pmix_iof.h"
#include "src/hwloc/pmix_hwloc.h"
#include "src/mca/bfrops/base/base.h"
#include "src/mca/gds/base/base.h"
#include "src/mca/pcompress/pcompress.h"
#include "src/mca/plog/plog.h"
#include "src/mca/pnet/pnet.h"
#include "src/mca/psensor/psensor.h"
#include "src/mca/ptl/base/base.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_error.h"
#include "src/util/pmix_name_fns.h"
#include "src/runtime/pmix_progress_threads.h"
#include "src/util/pmix_output.h"
#include "src/util/pmix_environ.h"

#include "src/client/pmix_client_ops.h"
#include "pmix_server_ops.h"

/* The rank_blob_t type to collect processes blobs,
 * this list afterward will form a node modex blob. */
typedef struct {
    pmix_list_item_t super;
    pmix_buffer_t *buf;
} rank_blob_t;

static void bufcon(rank_blob_t *p)
{
    p->buf = NULL;
}
static void bufdes(rank_blob_t *p)
{
    if (NULL != p->buf) {
        PMIX_RELEASE(p->buf);
    }
}
static PMIX_CLASS_INSTANCE(rank_blob_t,
                           pmix_list_item_t,
                           bufcon, bufdes);

pmix_status_t pmix_server_commit(pmix_peer_t *peer, pmix_buffer_t *buf)
{
    int32_t cnt;
    pmix_status_t rc;
    pmix_buffer_t b2, pbkt;
    pmix_kval_t *kp;
    pmix_scope_t scope;
    pmix_namespace_t *nptr;
    pmix_rank_info_t *info;
    pmix_proc_t proc;
    pmix_dmdx_remote_t *dcd, *dcdnext;
    char *data;
    size_t sz;
    pmix_cb_t cb;

    /* shorthand */
    info = peer->info;
    nptr = peer->nptr;
    pmix_strncpy(proc.nspace, nptr->nspace, PMIX_MAX_NSLEN);
    proc.rank = info->pname.rank;

    pmix_output_verbose(2, pmix_server_globals.fence_output,
                        "%s:%d EXECUTE COMMIT FOR %s:%d",
                        pmix_globals.myid.nspace, pmix_globals.myid.rank, nptr->nspace,
                        info->pname.rank);

    /* this buffer will contain one or more buffers, each
     * representing a different scope. These need to be locally
     * stored separately so we can provide required data based
     * on the requestor's location */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, peer, buf, &scope, &cnt, PMIX_SCOPE);
    while (PMIX_SUCCESS == rc) {
        /* Screen the scope before consuming the block it labels. There
         * used to be no else arm below, so a scope this server did not
         * recognize silently dropped every kval in its block and carried
         * on - the wrong answer for a peer telling us something we cannot
         * act on. PMIX_INTERNAL is rejected too: it names data that never
         * leaves the process, so it has no business on the wire. */
        if (PMIX_LOCAL != scope && PMIX_REMOTE != scope && PMIX_GLOBAL != scope
            && PMIX_DEL_LOCAL != scope && PMIX_DEL_REMOTE != scope
            && PMIX_DEL_GLOBAL != scope) {
            PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
            return PMIX_ERR_BAD_PARAM;
        }
        /* unpack and store the blob */
        cnt = 1;
        PMIX_CONSTRUCT(&b2, pmix_buffer_t);
        PMIX_BFROPS_ASSIGN_TYPE(peer, &b2);
        PMIX_BFROPS_UNPACK(rc, peer, buf, &b2, &cnt, PMIX_BUFFER);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_DESTRUCT(&b2);
            return rc;
        }
        /* unpack the buffer and store the values - we store them
         * in this peer's native GDS component so that other local
         * procs from that nspace can access it */
        kp = PMIX_NEW(pmix_kval_t);
        cnt = 1;
        PMIX_BFROPS_UNPACK(rc, peer, &b2, kp, &cnt, PMIX_KVAL);
        while (PMIX_SUCCESS == rc) {
            if (PMIX_LOCAL == scope || PMIX_GLOBAL == scope) {
                PMIX_GDS_STORE_KV(rc, peer, &proc, scope, kp);
                if (PMIX_SUCCESS != rc) {
                    PMIX_ERROR_LOG(rc);
                    PMIX_RELEASE(kp);
                    PMIX_DESTRUCT(&b2);
                    return rc;
                }
            }
            if (PMIX_REMOTE == scope || PMIX_GLOBAL == scope) {
                PMIX_GDS_STORE_KV(rc, pmix_globals.mypeer, &proc, scope, kp);
                if (PMIX_SUCCESS != rc) {
                    PMIX_ERROR_LOG(rc);
                    PMIX_RELEASE(kp);
                    PMIX_DESTRUCT(&b2);
                    return rc;
                }
            }
            /* A delete goes to the same store its counterpart would
             * have, and the module removes the key rather than adding
             * it. */
            if (PMIX_DEL_LOCAL == scope || PMIX_DEL_GLOBAL == scope) {
                PMIX_GDS_STORE_KV(rc, peer, &proc, scope, kp);
                if (PMIX_SUCCESS != rc) {
                    PMIX_ERROR_LOG(rc);
                    PMIX_RELEASE(kp);
                    PMIX_DESTRUCT(&b2);
                    return rc;
                }
            }
            if (PMIX_DEL_REMOTE == scope || PMIX_DEL_GLOBAL == scope) {
                PMIX_GDS_STORE_KV(rc, pmix_globals.mypeer, &proc, scope, kp);
                if (PMIX_SUCCESS != rc) {
                    PMIX_ERROR_LOG(rc);
                    PMIX_RELEASE(kp);
                    PMIX_DESTRUCT(&b2);
                    return rc;
                }
                /* A later fence contribution must not re-publish what was
                 * just removed. The pending list a delta is built from
                 * still holds the old value, so make the next
                 * contribution cumulative - that one is built from the
                 * datastore, which no longer has the key. */
                pmix_server_modex_resync(&proc);
            }
            if (PMIX_DEL_LOCAL == scope || PMIX_DEL_REMOTE == scope
                || PMIX_DEL_GLOBAL == scope) {
                /* The stores above went to "hash" - shmem3 leaves its
                 * store slot NULL and the macro falls back. A namespace
                 * served by shmem3 keeps its copy in a shared segment it
                 * cannot rewrite, so tell that module separately. */
                PMIX_GDS_DEL_KEY(rc, nptr, &proc, kp->key);
                if (PMIX_SUCCESS != rc) {
                    PMIX_ERROR_LOG(rc);
                }
                /* our store is corrected; the local clients that cached
                 * this key still have it. The requester applied it to
                 * its own store before sending, so skip it. */
                pmix_server_notify_deleted(&proc, scope, kp->key, peer);
            }
            if (pmix_server_globals.fence_delta_modex
                && (PMIX_REMOTE == scope || PMIX_GLOBAL == scope)) {
                /* Keep this for the next collecting fence, which may be
                 * able to contribute only what has changed.
                 *
                 * Gated on the parameter because the list is only ever
                 * drained by a collecting fence: with deltas off it would
                 * hold a second copy of everything the process has ever
                 * committed and nothing would ever take it away. Even
                 * with them on, a job that commits but never fences with
                 * data accumulates - there is simply no contribution to
                 * hand it to - but that is now something the parameter
                 * asked for rather than the default.
                 *
                 * Retain
                 * before the release below rather than at the top of the
                 * loop: the two error returns above leave the loop with
                 * the kval only partly stored, and those must not put it
                 * on the list. The list hangs off the rank_info rather
                 * than the peer because a fork/exec'd clone shares it,
                 * which is the identity the collection dedups on. */
                PMIX_RETAIN(kp);
                pmix_list_append(&info->pending_modex, &kp->super);
            }
            PMIX_RELEASE(kp); // maintain accounting
            kp = PMIX_NEW(pmix_kval_t);
            cnt = 1;
            PMIX_BFROPS_UNPACK(rc, peer, &b2, kp, &cnt, PMIX_KVAL);
        }
        PMIX_RELEASE(kp); // maintain accounting
        PMIX_DESTRUCT(&b2);
        if (PMIX_ERR_UNPACK_READ_PAST_END_OF_BUFFER != rc) {
            PMIX_ERROR_LOG(rc);
            return rc;
        }
        cnt = 1;
        PMIX_BFROPS_UNPACK(rc, peer, buf, &scope, &cnt, PMIX_SCOPE);
    }
    if (PMIX_ERR_UNPACK_READ_PAST_END_OF_BUFFER != rc) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    rc = PMIX_SUCCESS;
    /* mark us as having successfully received a blob from this proc */
    info->modex_recvd = true;

    /* update the commit counter */
    peer->commit_cnt++;

    /* see if anyone remote is waiting on this data - could be more than one */
    PMIX_LIST_FOREACH_SAFE (dcd, dcdnext, &pmix_server_globals.remote_pnd, pmix_dmdx_remote_t) {
        if (0 != strncmp(dcd->cd->proc.nspace, nptr->nspace, PMIX_MAX_NSLEN)) {
            continue;
        }
        if (dcd->cd->proc.rank == info->pname.rank) {
            pmix_list_remove_item(&pmix_server_globals.remote_pnd, &dcd->super);
            /* we can now fulfill this request - collect the
             * remote/global data from this proc - note that there
             * may not be a contribution */
            data = NULL;
            sz = 0;
            PMIX_CONSTRUCT(&cb, pmix_cb_t);
            cb.proc = &proc;
            cb.scope = PMIX_REMOTE;
            cb.copy = true;
            PMIX_GDS_FETCH_KV(rc, pmix_globals.mypeer, &cb);
            if (PMIX_SUCCESS == rc) {
                /* package it up */
                PMIX_CONSTRUCT(&pbkt, pmix_buffer_t);
                PMIX_LIST_FOREACH (kp, &cb.kvs, pmix_kval_t) {
                    /* we pack this in our native BFROPS form as it
                     * will be sent to another daemon */
                    PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, &pbkt, kp, 1, PMIX_KVAL);
                }
                PMIX_UNLOAD_BUFFER(&pbkt, data, sz);
            }
            PMIX_DESTRUCT(&cb);
            /* execute the callback */
            dcd->cd->cbfunc(rc, data, sz, dcd->cd->cbdata);
            if (NULL != data) {
                free(data);
            }
            /* we have finished this request */
            PMIX_RELEASE(dcd);
        }
    }
    /* see if anyone local is waiting on this data- could be more than one */
    rc = pmix_pending_resolve(nptr, info->pname.rank, PMIX_SUCCESS, PMIX_LOCAL, NULL);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
    }
    return rc;
}

/* get an existing object for tracking LOCAL participation in a collective
 * operation such as "fence". The only way this function can be
 * called is if at least one local client process is participating
 * in the operation. Thus, we know that at least one process is
 * involved AND has called the collective operation.
 *
 * NOTE: the host server *cannot* call us with a collective operation
 * as there is no mechanism by which it can do so. We call the host
 * server only after all participating local procs have called us.
 * So it is impossible for us to be called with a collective without
 * us already knowing about all local participants.
 *
 * procs - the array of procs participating in the collective,
 *         regardless of location
 * nprocs - the number of procs in the array
 */
pmix_server_trkr_t *pmix_server_get_tracker(char *id, pmix_proc_t *procs,
                                            size_t nprocs, pmix_cmd_t type)
{
    pmix_server_trkr_t *trk;
    size_t i, j, sz;
    size_t matches;
    pmix_proclist_t *p;
    pmix_proc_t *ptr;
    pmix_list_t cache;
    bool found;

    pmix_output_verbose(5, pmix_server_globals.fence_output,
                        "pmix_server_get_tracker called with %d procs",
                        (int) nprocs);

    /* bozo check - should never happen outside of programmer error */
    if (NULL == procs && NULL == id) {
        PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
        return NULL;
    }

    /* there is no shortcut way to search the trackers - all
     * we can do is perform a brute-force search. Fortunately,
     * it is highly unlikely that there will be more than one
     * or two active at a time, and they are most likely to
     * involve only a single proc with WILDCARD rank - so this
     * shouldn't take long */
    PMIX_LIST_FOREACH (trk, &pmix_server_globals.collectives, pmix_server_trkr_t) {
        /* a tracker whose completion has been driven is on its way out -
         * its handler is thread-shifted and will reply to everyone on
         * local_cbs, unlink the tracker and release it. It is still on
         * this list until then, but it must not take a new participant:
         * that caddy would be freed with the tracker without ever being
         * answered, hanging a client whose only mistake was to call the
         * next collective quickly. Skip it and let the caller open a
         * fresh tracker for the new operation. */
        if (trk->completion_fired) {
            continue;
        }
        /* Collective operation if unique identified by
         * the set of participating processes and the type of collective,
         * or by the operation ID
         */
        if (NULL != id) {
            if (NULL != trk->id && 0 == strcmp(id, trk->id)) {
                PMIX_CONSTRUCT(&cache, pmix_list_t);
                /* update tracked procs to include the given ones,
                 * filtered to only keep unique entries */
                if (NULL != procs) {
                    for (i=0; i < nprocs; i++) {
                        found = false;
                        for (j=0; j < trk->npcs; j++) {
                            if (PMIX_CHECK_PROCID(&procs[i], &trk->pcs[j])) {
                                // match - can ignore it
                                found = true;
                                break;
                            }
                        }
                        if (!found) {
                            // new proc, so we need to add it
                            p = PMIX_NEW(pmix_proclist_t);
                            memcpy(&p->proc, &procs[i], sizeof(pmix_proc_t));
                            pmix_list_append(&cache, &p->super);
                        }
                    }
                }
                i = pmix_list_get_size(&cache);
                if (0 < i) {
                    sz = trk->npcs + i;
                    PMIX_PROC_CREATE(ptr, sz);
                    if (NULL == ptr) {
                        /* nothing screens this allocation downstream - the
                         * next statement copies into it. Hand back the
                         * tracker we found without the new participants
                         * rather than dereferencing NULL; returning NULL
                         * would be worse still, as the caller would then
                         * create a second tracker for an id that already
                         * has one */
                        PMIX_ERROR_LOG(PMIX_ERR_NOMEM);
                        PMIX_LIST_DESTRUCT(&cache);
                        return trk;
                    }
                    memcpy(ptr, trk->pcs, trk->npcs * sizeof(pmix_proc_t));
                    j = trk->npcs;
                    PMIX_LIST_FOREACH(p, &cache, pmix_proclist_t) {
                        memcpy(&ptr[j], &p->proc, sizeof(pmix_proc_t));
                        ++j;
                    }
                    PMIX_PROC_FREE(trk->pcs, trk->npcs);
                    trk->pcs = ptr;
                    trk->npcs = sz;
                }
                PMIX_LIST_DESTRUCT(&cache);
                return trk;
            }
        } else {
            // id must have been NULL, so we know procs is not
            if (nprocs != trk->npcs) {
                continue;
            }
            if (type != trk->type) {
                continue;
            }
            matches = 0;
            if (NULL != procs) {
                for (i = 0; i < nprocs; i++) {
                    /* the procs may be in different order, so we have
                     * to do an exhaustive search */
                    for (j = 0; j < trk->npcs; j++) {
                        if (0 == strcmp(procs[i].nspace, trk->pcs[j].nspace)
                            && procs[i].rank == trk->pcs[j].rank) {
                            ++matches;
                            break;
                        }
                    }
                }
            }
            if (trk->npcs == matches) {
                return trk;
            }
        }
    }
    /* No tracker was found */
    return NULL;
}

/* create a new object for tracking LOCAL participation in a collective
 * operation such as "fence". The only way this function can be
 * called is if at least one local client process is participating
 * in the operation. Thus, we know that at least one process is
 * involved AND has called the collective operation.
 *
 * NOTE: the host server *cannot* call us with a collective operation
 * as there is no mechanism by which it can do so. We call the host
 * server only after all participating local procs have called us.
 * So it is impossible for us to be called with a collective without
 * us already knowing about all local participants.
 *
 * procs - the array of procs participating in the collective,
 *         regardless of location
 * nprocs - the number of procs in the array
 */
pmix_server_trkr_t *pmix_server_new_tracker(char *id, pmix_proc_t *procs,
                                            size_t nprocs, pmix_cmd_t type)
{
    pmix_server_trkr_t *trk;
    size_t i;
    bool all_def, found;
    pmix_namespace_t *nptr, *ns;
    pmix_rank_info_t *info;
    pmix_nspace_caddy_t *nm;
    pmix_nspace_t first;

    pmix_output_verbose(5, pmix_server_globals.fence_output,
                        "pmix_server_new_tracker called with %d procs",
                        (int) nprocs);

    /* bozo check - should never happen outside of programmer error */
    if (NULL == procs && NULL == id) {
        PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
        return NULL;
    }

    pmix_output_verbose(5, pmix_server_globals.fence_output,
                        "adding new tracker %s with %d procs",
                        (NULL == id) ? "NO-ID" : id, (int) nprocs);

    /* this tracker is new - create it */
    trk = PMIX_NEW(pmix_server_trkr_t);
    if (NULL == trk) {
        PMIX_ERROR_LOG(PMIX_ERR_NOMEM);
        return NULL;
    }
    trk->type = type;

    if (NULL != id) {
        trk->id = strdup(id);
    }

    if (NULL == procs) {
        // we are done
        trk->def_complete = true;
        pmix_list_append(&pmix_server_globals.collectives, &trk->super);
        return trk;
    }

    /* copy the procs */
    PMIX_PROC_CREATE(trk->pcs, nprocs);
    if (NULL == trk->pcs) {
        PMIX_ERROR_LOG(PMIX_ERR_NOMEM);
        PMIX_RELEASE(trk);
        return NULL;
    }
    memcpy(trk->pcs, procs, nprocs * sizeof(pmix_proc_t));
    trk->npcs = nprocs;
    trk->local = true;
    trk->nlocal = 0;

    all_def = true;
    PMIX_LOAD_NSPACE(first, NULL);
    for (i = 0; i < nprocs; i++) {
        /* is this nspace known to us? */
        nptr = NULL;
        PMIX_LIST_FOREACH (ns, &pmix_globals.nspaces, pmix_namespace_t) {
            if (0 == strcmp(procs[i].nspace, ns->nspace)) {
                nptr = ns;
                break;
            }
        }
        /* check if multiple nspaces are involved in this operation */
        if (0 == strlen(first)) {
            PMIX_LOAD_NSPACE(first, procs[i].nspace);
        } else if (!PMIX_CHECK_NSPACE(first, procs[i].nspace)) {
            trk->hybrid = true;
        }
        if (NULL == nptr) {
            /* we don't know about this nspace. If there is going to
             * be at least one local process participating in a fence,
             * they we require that either at least one process must already
             * have been registered (via "register client") or that the
             * nspace itself have been regisered. So either the nspace
             * wasn't registered because it doesn't include any local
             * procs, or our host has not been told about this nspace
             * because it won't host any local procs. We therefore mark
             * this tracker as including non-local participants.
             *
             * NOTE: It is conceivable that someone might want to review
             * this constraint at a future date. I believe it has to be
             * required (at least for now) as otherwise we wouldn't have
             * a way of knowing when all local procs have participated.
             * It is possible that a new nspace could come along at some
             * later time and add more local participants - but we don't
             * know how long to wait.
             *
             * The only immediately obvious alternative solutions would
             * be to either require that RMs always inform all daemons
             * about the launch of nspaces, regardless of whether or
             * not they will host local procs; or to drop the aggregation
             * of local participants and just pass every fence call
             * directly to the host. Neither of these seems palatable
             * at this time. */
            trk->local = false;
            /* we don't know any more info about this nspace, so
             * there isn't anything more we can do */
            continue;
        }
        /* it is possible we know about this nspace because the host
         * has registered one or more clients via "register_client",
         * but the host has not yet called "register_nspace". There is
         * a very tiny race condition whereby this can happen due
         * to event-driven processing, but account for it here */
        if (SIZE_MAX == nptr->nlocalprocs) {
            /* delay processing until this nspace is registered */
            all_def = false;
            continue;
        }
        if (0 == nptr->nlocalprocs) {
            /* the host has informed us that this nspace has no local procs */
            pmix_output_verbose(5, pmix_server_globals.fence_output,
                                "pmix_server_new_tracker: nspace %s has no local procs", procs[i].nspace);
            trk->local = false;
            continue;
        }

        /* check and add uniq ns into trk nslist */
        found = false;
        PMIX_LIST_FOREACH (nm, &trk->nslist, pmix_nspace_caddy_t) {
            if (0 == strcmp(nptr->nspace, nm->ns->nspace)) {
                found = true;
                break;
            }
        }
        if (!found) {
            nm = PMIX_NEW(pmix_nspace_caddy_t);
            PMIX_RETAIN(nptr);
            nm->ns = nptr;
            pmix_list_append(&trk->nslist, &nm->super);
        }

        /* if they want all the local members of this nspace, then
         * add them in here. They told us how many procs will be
         * local to us from this nspace, but we don't know their
         * ranks. So as long as they want _all_ of them, we can
         * handle that case regardless of whether the individual
         * clients have been "registered" */
        if (PMIX_RANK_WILDCARD == procs[i].rank) {
            trk->nlocal += nptr->nlocalprocs;
            /* the total number of procs in this nspace was provided
             * in the data blob delivered to register_nspace, so check
             * to see if all the procs are local */
            if (nptr->nprocs != nptr->nlocalprocs) {
                trk->local = false;
            }
            continue;
        }

        /* They don't want all the local clients, or they are at
         * least listing them individually. Check if all the clients
         * for this nspace have been registered via "register_client"
         * so we know the specific ranks on this node */
        if (!nptr->all_registered) {
            /* nope, so no point in going further on this one - we'll
             * process it once all the procs are known */
            all_def = false;
            pmix_output_verbose(5, pmix_server_globals.fence_output,
                                "pmix_server_new_tracker: all clients not registered nspace %s",
                                procs[i].nspace);
            continue;
        }
        /* is this one of my local ranks? */
        found = false;
        PMIX_LIST_FOREACH (info, &nptr->ranks, pmix_rank_info_t) {
            if (procs[i].rank == info->pname.rank) {
                pmix_output_verbose(5, pmix_server_globals.fence_output,
                                    "adding local proc %s.%d to tracker", info->pname.nspace,
                                    info->pname.rank);
                found = true;
                /* track the count */
                trk->nlocal++;
                break;
            }
        }
        if (!found) {
            trk->local = false;
        }
    }

    if (all_def) {
        trk->def_complete = true;
    }
    pmix_list_append(&pmix_server_globals.collectives, &trk->super);
    return trk;
}

static void fence_timeout(int sd, short args, void *cbdata)
{
    pmix_server_trkr_t *trk = (pmix_server_trkr_t *) cbdata;
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    pmix_output_verbose(2, pmix_server_globals.fence_output, "ALERT: fence timeout fired");

    /* a completion already driven for this tracker owns it, and its
     * handler will unlink and release it. Every hand-off deletes this
     * timer, but a timer whose event is already queued fires anyway, so
     * this is the guard that makes that harmless rather than a second
     * completion of a tracker that is about to be freed. */
    if (trk->completion_fired) {
        trk->event_active = false;
        return;
    }

    /* execute the provided callback function with the error */
    if (NULL != trk->modexcbfunc) {
        trk->modexcbfunc(PMIX_ERR_TIMEOUT, NULL, 0, trk, NULL, NULL);
        return; // the cbfunc will have cleaned up the tracker
    }
    /* no completion function was ever attached, so tear the tracker down
     * ourselves - which means unlinking it from the collectives list first.
     * Being on that list is not a reference; releasing while still linked
     * leaves a dangling entry that the next sweep walks into. */
    trk->event_active = false;
    pmix_list_remove_item(&pmix_server_globals.collectives, &trk->super);
    PMIX_RELEASE(trk);
}

static void _collect_job_info(int sd, short args, void *cbdata)
{
    pmix_cb_t *cbin = (pmix_cb_t*)cbdata;
    char **nspaces = NULL;
    size_t n, m;
    bool found;
    pmix_proc_t proc;
    pmix_cb_t cb;
    pmix_status_t ret = PMIX_SUCCESS;
    pmix_status_t rc;
    pmix_buffer_t pbkt;
    pmix_kval_t *kptr;
    pmix_byte_object_t pbo;
    pmix_namespace_t *nptr, *ns;
    pmix_rank_info_t *rinfo;
    pmix_peer_t *peer;
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    PMIX_ACQUIRE_OBJECT(cbin);

    /* find the unique nspaces that are participating */
    for (m=0; m < cbin->nprocs; m++) {
        if (NULL == nspaces) {
            PMIx_Argv_append_nosize(&nspaces, cbin->procs[m].nspace);
        } else {
            found = false;
            for (n = 0; NULL != nspaces[n]; n++) {
                if (0 == strcmp(nspaces[n], cbin->procs[m].nspace)) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                PMIx_Argv_append_nosize(&nspaces, cbin->procs[m].nspace);
            }
        }
    }

    if (NULL == nspaces) {
        // no procs were given, so there is nothing to collect - the
        // loop below would dereference the NULL array
        ret = PMIX_ERR_BAD_PARAM;
        goto done;
    }

    // cycle across the nspaces to collect their job info
    for (n = 0; NULL != nspaces[n]; n++) {
        // see if we have this nspace
        nptr = NULL;
        PMIX_LIST_FOREACH (ns, &pmix_globals.nspaces, pmix_namespace_t) {
            if (0 == strcmp(ns->nspace, nspaces[n])) {
                nptr = ns;
                break;
            }
        }
        if (NULL == nptr) {
            // we don't know this one, so nothing we can do
            continue;
        }
        /* this is a local request, so give the gds the option
         * of returning a copy of the data, or a pointer to
         * local storage */
        PMIX_LOAD_PROCID(&proc, nspaces[n], PMIX_RANK_WILDCARD);
        PMIX_CONSTRUCT(&cb, pmix_cb_t);
        cb.proc = &proc;
        cb.scope = PMIX_SCOPE_UNDEF;
        cb.copy = false;

        /* the fetch status is per-nspace and must not become the status of
         * the whole request: a namespace we cannot answer for is skipped
         * exactly like one we have never heard of, a few lines above. Left
         * in 'ret', it made the outcome depend on which nspace happened to
         * be last - and PMIx_server_collect_job_info only hands the buffer
         * back on success, so one unanswerable nspace discarded the job
         * info already collected for all the others */
        rc = PMIX_ERR_NOT_FOUND;
        // have any local clients registered?
        if (0 < pmix_list_get_size(&nptr->ranks)) {
            // get one of the local clients so we will
            // know its gds module
            rinfo = (pmix_rank_info_t*)pmix_list_get_first(&nptr->ranks);
            if (NULL != rinfo) {
                peer = (pmix_peer_t*)pmix_pointer_array_get_item(&pmix_server_globals.clients, rinfo->peerid);
                if (NULL != peer) {
                    PMIX_GDS_FETCH_KV(rc, peer, &cb);
                }
            }
        }
        // if we didn't find it there, try getting it
        // from our storage
        if (PMIX_SUCCESS != rc) {
            PMIX_GDS_FETCH_KV(rc, pmix_globals.mypeer, &cb);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_DESTRUCT(&cb);
                continue;
            }
        }

        PMIX_CONSTRUCT(&pbkt, pmix_buffer_t);
        /* pack the nspace name */
        PMIX_BFROPS_PACK(ret, pmix_globals.mypeer, &pbkt, &nspaces[n], 1, PMIX_STRING);
        if (PMIX_SUCCESS != ret) {
            PMIX_ERROR_LOG(ret);
            PMIX_DESTRUCT(&pbkt);
            PMIX_DESTRUCT(&cb);
            goto done;
        }
        PMIX_LIST_FOREACH (kptr, &cb.kvs, pmix_kval_t) {
            PMIX_BFROPS_PACK(ret, pmix_globals.mypeer, &pbkt, kptr, 1, PMIX_KVAL);
            if (PMIX_SUCCESS != ret) {
                PMIX_ERROR_LOG(ret);
                PMIX_DESTRUCT(&pbkt);
                PMIX_DESTRUCT(&cb);
                goto done;
            }
        }
        PMIX_DESTRUCT(&cb);


        PMIX_UNLOAD_BUFFER(&pbkt, pbo.bytes, pbo.size);
        /* accumulate into the caller's caddy (cbin), not the local cb that
         * was just destructed above and re-created each iteration - the
         * caller reads cbin->data, so packing into cb.data returned an empty
         * job-info buffer to every consumer of PMIx_server_collect_job_info */
        PMIX_BFROPS_PACK(ret, pmix_globals.mypeer, &cbin->data, &pbo, 1, PMIX_BYTE_OBJECT);
        PMIX_BYTE_OBJECT_DESTRUCT(&pbo);
        if (PMIX_SUCCESS != ret) {
            PMIX_ERROR_LOG(ret);
            PMIX_DESTRUCT(&pbkt);
            goto done;
        }

        PMIX_DESTRUCT(&pbkt);
    }

done:
    PMIx_Argv_free(nspaces);
    cbin->status = ret;
    PMIX_POST_OBJECT(cbin);
    PMIX_WAKEUP_THREAD(&cbin->lock);
}

pmix_status_t PMIx_server_collect_job_info(pmix_proc_t *procs, size_t nprocs,
                                           pmix_data_buffer_t *dbuf)
{
    pmix_cb_t cb;
    pmix_byte_object_t bo;
    pmix_status_t rc;

    if (!pmix_atomic_check_bool(&pmix_globals.initialized)) {
        return PMIX_ERR_INIT;
    }

    /* the request is threadshifted below and we then block waiting for
     * it, so there has to be a progress thread to execute it */
    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        return PMIX_ERR_NOT_AVAILABLE;
    }

    if (NULL == procs || 0 == nprocs || NULL == dbuf) {
        return PMIX_ERR_BAD_PARAM;
    }

    // we need to threadshift this request as it accesses
    // global data
    /* what would release us runs on the progress thread, so waiting
     * for it from that thread waits for ourselves */
    if (PMIX_UNLIKELY(pmix_progress_thread_check_blocking("PMIx_server_collect_job_info"))) {
        return PMIX_ERR_WOULD_BLOCK;
    }

    PMIX_CONSTRUCT(&cb, pmix_cb_t);
    cb.procs = procs;
    cb.nprocs = nprocs;
    PMIX_THREADSHIFT(&cb, _collect_job_info);
    PMIX_WAIT_THREAD(&cb.lock);

    if (PMIX_SUCCESS == cb.status) {
        PMIX_UNLOAD_BUFFER(&cb.data, bo.bytes, bo.size);
        PMIx_Data_buffer_load(dbuf, bo.bytes, bo.size);  // removes data from the byte object
    }
    rc = cb.status;
    PMIX_DESTRUCT(&cb);
    return rc;
}

/* Tell our local clients that a key has been deleted, so their own
 * copies of it go too.
 *
 * A client caches what it reads about other processes, and holds the
 * job-level data it was given at init, so removing a key from this
 * server's store is only half of it - every local client that ever
 * looked the key up still has it. This is the other half.
 *
 * Sent to every local client except the one that asked for the deletion,
 * which has already applied it to its own store. It is deliberately not
 * restricted to clients of the affected namespace: a process may have
 * cached data belonging to any namespace it has asked about, and a
 * client that never held the key simply removes nothing.
 *
 * One message per key. Deletions are rare - this is not a path that
 * needs batching, and one message per key keeps the wire format a
 * single-key statement rather than a list whose length has to be
 * screened on receipt.
 *
 * A peer too old to know the tag never posted a receive for it, so it
 * cannot be reached this way; PMIx_Put refuses a delete against such a
 * server up front, which is where the caller learns about it. */
void pmix_server_notify_deleted(const pmix_proc_t *proc,
                                pmix_scope_t scope,
                                const char *key,
                                pmix_peer_t *skip)
{
    pmix_peer_t *peer;
    pmix_buffer_t *msg;
    pmix_status_t rc;
    int n;

    if (NULL == proc || NULL == key) {
        return;
    }
    for (n = 0; n < pmix_server_globals.clients.size; n++) {
        peer = (pmix_peer_t *) pmix_pointer_array_get_item(&pmix_server_globals.clients, n);
        if (NULL == peer || peer == skip || peer->finalized) {
            continue;
        }
        /* a peer that predates this cannot act on it - and has no
         * receive posted for the tag either */
        if (PMIX_PEER_IS_EARLIER(peer, 7, 0, 0)) {
            continue;
        }
        msg = PMIX_NEW(pmix_buffer_t);
        if (PMIX_UNLIKELY(NULL == msg)) {
            return;
        }
        PMIX_BFROPS_PACK(rc, peer, msg, proc, 1, PMIX_PROC);
        if (PMIX_SUCCESS == rc) {
            PMIX_BFROPS_PACK(rc, peer, msg, &scope, 1, PMIX_SCOPE);
        }
        if (PMIX_SUCCESS == rc) {
            PMIX_BFROPS_PACK(rc, peer, msg, &key, 1, PMIX_STRING);
        }
        if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
            PMIX_ERROR_LOG(rc);
            PMIX_RELEASE(msg);
            continue;
        }
        PMIX_PTL_SEND_ONEWAY(rc, peer, msg, PMIX_PTL_TAG_DATA_DELETE);
        if (PMIX_SUCCESS != rc) {
            /* the peer is finalized - it has no copy left to correct */
            PMIX_RELEASE(msg);
        }
    }
}

/* Digest a fence's participant set.
 *
 * A delta contribution is only sound for a fence over the same
 * participants: contributing one to a fence over some *other* set would
 * leave every server holding only that set's procs never learning the
 * keys we left out - two sub-communicators fencing independently is
 * enough to reach that. Comparing the sets directly would mean keeping a
 * copy of one per local rank, which on a large job is the job's whole
 * proc array per rank, so we keep a 64-bit digest and require it to
 * match exactly.
 *
 * Requiring equality rather than containment is deliberate. It is the
 * conservative half of the rule, so it can only cost an unnecessary
 * cumulative contribution, never a short one - and it still covers the
 * case that matters, a job that fences repeatedly over the same set.
 *
 * The digest is taken over the fields rather than the raw bytes because
 * pmix_proc_t may carry padding between its nspace array and its rank,
 * and padding is not guaranteed to hold the same thing twice. Note the
 * array need not be sorted for this to be correct: a different ordering
 * of the same set simply digests differently, and the only consequence
 * is a cumulative contribution we could in principle have made a delta.
 */
static uint64_t participant_signature(const pmix_proc_t *procs, size_t nprocs)
{
    uint64_t h = 14695981039346656037ULL; /* FNV-1a 64-bit offset basis */
    size_t n, i;
    const unsigned char *p;

#define PMIX_SIG_BYTE(b)                            \
    do {                                            \
        h ^= (uint64_t) (unsigned char) (b);        \
        h *= 1099511628211ULL;                      \
    } while (0)

    for (i = 0; i < sizeof(nprocs); i++) {
        PMIX_SIG_BYTE((nprocs >> (8 * i)) & 0xff);
    }
    if (NULL == procs) {
        return h;
    }
    for (n = 0; n < nprocs; n++) {
        for (p = (const unsigned char *) procs[n].nspace; '\0' != *p; p++) {
            PMIX_SIG_BYTE(*p);
        }
        PMIX_SIG_BYTE(0);
        for (i = 0; i < sizeof(pmix_rank_t); i++) {
            PMIX_SIG_BYTE((procs[n].rank >> (8 * i)) & 0xff);
        }
    }
#undef PMIX_SIG_BYTE
    return h;
}

/* Record that every local participant's contribution has reached the
 * host, so the next one can carry only what changes from here.
 *
 * This is deliberately NOT done inside pmix_server_collect_data. That
 * runs before the up-call, and its caller has three arms that discard
 * the bucket - collect_data failing, the host refusing the request, and
 * fail_collective. Draining there would lose those deltas for good,
 * because nothing else remembers them: the datastore still holds the
 * values, but the record of which ones this rank had yet to send does
 * not survive.
 *
 * Draining a rank twice is a no-op and stamping it twice is idempotent,
 * so unlike the collection itself this needs no clone dedup. */
void pmix_server_modex_contributed(pmix_server_trkr_t *trk)
{
    pmix_server_caddy_t *scd;
    pmix_rank_info_t *info;
    uint64_t sig;

    if (PMIX_COLLECT_YES != trk->collect_type) {
        return;
    }
    sig = participant_signature(trk->pcs, trk->npcs);
    PMIX_LIST_FOREACH (scd, &trk->local_cbs, pmix_server_caddy_t) {
        info = scd->peer->info;
        if (NULL == info) {
            continue;
        }
        PMIX_LIST_DESTRUCT(&info->pending_modex);
        PMIX_CONSTRUCT(&info->pending_modex, pmix_list_t);
        info->modex_sig = sig;
        info->modex_contributed = true;
    }
}

/* Force this proc's next fence contribution to be cumulative.
 *
 * pmix_server_commit is not the only way remote-scope data arrives for a
 * local proc - a host can register a group's endpoint data through
 * PMIx_server_register_resources, and the group collective stores
 * members' contributions directly. Neither goes through the commit path,
 * so neither is on the pending list, and a delta built from that list
 * alone would silently omit it. */
void pmix_server_modex_resync(const pmix_proc_t *proc)
{
    pmix_namespace_t *nptr;
    pmix_rank_info_t *info;

    if (NULL == proc) {
        return;
    }
    PMIX_LIST_FOREACH (nptr, &pmix_globals.nspaces, pmix_namespace_t) {
        if (0 != strncmp(nptr->nspace, proc->nspace, PMIX_MAX_NSLEN)) {
            continue;
        }
        PMIX_LIST_FOREACH (info, &nptr->ranks, pmix_rank_info_t) {
            if (PMIX_RANK_WILDCARD == proc->rank || info->pname.rank == proc->rank) {
                info->modex_contributed = false;
            }
        }
        return;
    }
}

pmix_status_t pmix_server_collect_data(pmix_server_trkr_t *trk,
                                       pmix_buffer_t *buf)
{
    pmix_buffer_t bucket, bkt, *pbkt = NULL;
    pmix_cb_t cb;
    pmix_kval_t *kv;
    pmix_byte_object_t bo, outbo;
    pmix_server_caddy_t *scd;
    pmix_proc_t pcs;
    pmix_status_t rc = PMIX_SUCCESS;
    pmix_list_t rank_blobs;
    rank_blob_t *blob;
    uint8_t blob_info_byte;
    bool compressed;
    pmix_list_t pnames;
    pmix_namelist_t *pn;
    bool found;
    bool usedelta;
    uint64_t sig;

    PMIX_CONSTRUCT(&bucket, pmix_buffer_t);

    if (PMIX_COLLECT_YES == trk->collect_type) {
       pmix_output_verbose(2, pmix_server_globals.fence_output,
                           "fence - assembling data");

        /* Decide once, for the whole bucket, whether this contribution
         * can be a delta. The flag byte that says so describes the
         * server's contribution as a whole, so the bucket cannot be part
         * delta and part not - and a delta is only sound if *every*
         * local participant has already contributed to a fence over this
         * same participant set. Anything else falls back to sending
         * each rank's full published set, which is what this always
         * used to do. */
        sig = participant_signature(trk->pcs, trk->npcs);
        usedelta = pmix_server_globals.fence_delta_modex;
        if (usedelta) {
            PMIX_LIST_FOREACH (scd, &trk->local_cbs, pmix_server_caddy_t) {
                if (NULL == scd->peer->info
                    || !scd->peer->info->modex_contributed
                    || scd->peer->info->modex_sig != sig) {
                    usedelta = false;
                    break;
                }
            }
        }

        PMIX_CONSTRUCT(&rank_blobs, pmix_list_t);
        PMIX_CONSTRUCT(&pnames, pmix_list_t);
        PMIX_LIST_FOREACH (scd, &trk->local_cbs, pmix_server_caddy_t) {
            /* Contribute each participating rank once. A fork/exec'd clone
             * shares its parent's pmix_rank_info_t, so both caddies name
             * the same rank and would otherwise have that rank's remote
             * data fetched and packed into the bucket twice. Identity of
             * the rank_info's pname is the test, exactly as the clone
             * accounting in the tracker engine uses it. */
            found = false;
            PMIX_LIST_FOREACH (pn, &pnames, pmix_namelist_t) {
                if (pn->pname == &scd->peer->info->pname) {
                    found = true;
                    break;
                }
            }
            if (found) {
                continue;
            }
            pn = PMIX_NEW(pmix_namelist_t);
            if (NULL == pn) {
                PMIX_ERROR_LOG(PMIX_ERR_NOMEM);
                rc = PMIX_ERR_NOMEM;
                PMIX_LIST_DESTRUCT(&pnames);
                PMIX_LIST_DESTRUCT(&rank_blobs);
                goto cleanup;
            }
            pn->pname = &scd->peer->info->pname;
            pmix_list_append(&pnames, &pn->super);
            /* get any remote contribution - note that there
             * may not be a contribution */
            PMIX_LOAD_PROCID(&pcs, scd->peer->info->pname.nspace, scd->peer->info->pname.rank);
            pbkt = PMIX_NEW(pmix_buffer_t);
            if (NULL == pbkt) {
                /* the pack below screens a NULL buffer, but its error arm
                 * then releases it - so catch this here */
                PMIX_ERROR_LOG(PMIX_ERR_NOMEM);
                rc = PMIX_ERR_NOMEM;
                PMIX_LIST_DESTRUCT(&pnames);
                PMIX_LIST_DESTRUCT(&rank_blobs);
                goto cleanup;
            }
            /* pack the rank */
            PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, pbkt, &pcs, 1, PMIX_PROC);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_LIST_DESTRUCT(&pnames);
                PMIX_LIST_DESTRUCT(&rank_blobs);
                PMIX_RELEASE(pbkt);
                goto cleanup;
            }
            if (usedelta) {
                /* pack what this rank has committed since it last
                 * contributed - an empty list is a legitimate answer,
                 * and the receiver reads a proc with no kvals as "this
                 * rank published nothing new" */
                PMIX_LIST_FOREACH (kv, &scd->peer->info->pending_modex, pmix_kval_t) {
                    PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, pbkt, kv, 1, PMIX_KVAL);
                    if (PMIX_SUCCESS != rc) {
                        PMIX_ERROR_LOG(rc);
                        PMIX_LIST_DESTRUCT(&pnames);
                        PMIX_LIST_DESTRUCT(&rank_blobs);
                        PMIX_RELEASE(pbkt);
                        goto cleanup;
                    }
                }
                blob = PMIX_NEW(rank_blob_t);
                blob->buf = pbkt;
                pmix_list_append(&rank_blobs, &blob->super);
                pbkt = NULL;
                continue;
            }
            PMIX_CONSTRUCT(&cb, pmix_cb_t);
            cb.proc = &pcs;
            cb.scope = PMIX_REMOTE;
            cb.copy = true;
            PMIX_GDS_FETCH_KV(rc, pmix_globals.mypeer, &cb);
            if (PMIX_SUCCESS == rc) {
                /* pack the returned kval's */
                PMIX_LIST_FOREACH (kv, &cb.kvs, pmix_kval_t) {
                    PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, pbkt, kv, 1, PMIX_KVAL);
                    if (PMIX_SUCCESS != rc) {
                        PMIX_ERROR_LOG(rc);
                        PMIX_DESTRUCT(&cb);
                        PMIX_LIST_DESTRUCT(&pnames);
                        PMIX_LIST_DESTRUCT(&rank_blobs);
                        PMIX_RELEASE(pbkt);
                        goto cleanup;
                    }
                }
                /* add the blob to the list */
                blob = PMIX_NEW(rank_blob_t);
                blob->buf = pbkt;
                pmix_list_append(&rank_blobs, &blob->super);
                pbkt = NULL;
            }
            PMIX_DESTRUCT(&cb);
            if (NULL != pbkt) {
                PMIX_RELEASE(pbkt);
            }
        }
        PMIX_LIST_DESTRUCT(&pnames);
        /* mark the collection type so we can check on the
         * receiving end that all participants did the same. Note
         * that if the receiving end thinks that the collect flag
         * is false, then store_modex will not be called on that
         * node and this information (and the flag) will be ignored,
         * meaning that no error is generated! */
        blob_info_byte = usedelta ? PMIX_MODEX_DELTA : PMIX_COLLECT_YES;
        /* pack the modex blob info byte - check it, as the blob packs
         * below would otherwise overwrite the failure and ship a bucket
         * whose first byte is a rank blob rather than the collect type */
        PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, &bucket, &blob_info_byte, 1, PMIX_BYTE);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_LIST_DESTRUCT(&rank_blobs);
            goto cleanup;
        }

        /* pack the collected blobs of processes */
        PMIX_LIST_FOREACH (blob, &rank_blobs, rank_blob_t) {
            /* extract the blob */
            PMIX_UNLOAD_BUFFER(blob->buf, bo.bytes, bo.size);
            /* the payload now belongs to bo, so release the emptied
             * buffer object - the list destructor skips it once cleared */
            PMIX_RELEASE(blob->buf);
            blob->buf = NULL;
            /* pack the returned blob */
            PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, &bucket, &bo, 1, PMIX_BYTE_OBJECT);
            PMIX_BYTE_OBJECT_DESTRUCT(&bo); // releases the data
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_LIST_DESTRUCT(&rank_blobs);
                goto cleanup;
            }
        }
        PMIX_LIST_DESTRUCT(&rank_blobs);

    } else {
        /* mark the collection type so we can check on the
         * receiving end that all participants did the same.
         * Don't do it for non-debug mode so we don't unnecessarily
         * send the collection bucket. The mdxcbfunc in the
         * server only calls store_modex if the local collect
         * flag is set to true. In debug mode, this check will
         * cause the store_modex function to see that this node
         * thought the collect flag was not set, and therefore
         * generate an error */
        blob_info_byte= PMIX_COLLECT_NO;
        /* pack the modex blob info byte */
        PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, &bucket, &blob_info_byte, 1, PMIX_BYTE);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            goto cleanup;
        }
    }

    if (!PMIX_BUFFER_IS_EMPTY(&bucket)) {
        /* because the remote servers have to unpack things
         * in chunks, we have to pack the bucket as a single
         * byte object to allow remote unpack */
        PMIX_UNLOAD_BUFFER(&bucket, bo.bytes, bo.size);
        // compress the data
        if (pmix_compress.compress((uint8_t*)bo.bytes, bo.size, (uint8_t**)&outbo.bytes, &outbo.size)) {
            compressed = true;
            PMIX_BYTE_OBJECT_DESTRUCT(&bo);
            bo.bytes = outbo.bytes;
            bo.size = outbo.size;
        } else {
            compressed = false;
        }
        // need to get the compressed flag inside the byte object we pass along
        PMIX_CONSTRUCT(&bkt, pmix_buffer_t);
        PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, &bkt, &compressed, 1, PMIX_BOOL);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_BYTE_OBJECT_DESTRUCT(&bo);
            PMIX_DESTRUCT(&bkt);
            goto cleanup;
        }
        PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, &bkt, &bo, 1, PMIX_BYTE_OBJECT);
        PMIX_BYTE_OBJECT_DESTRUCT(&bo); // releases the data
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_DESTRUCT(&bkt);
            goto cleanup;
        }
        // now transfer to final buffer
        PMIX_UNLOAD_BUFFER(&bkt, bo.bytes, bo.size);
        PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, buf, &bo, 1, PMIX_BYTE_OBJECT);
        PMIX_BYTE_OBJECT_DESTRUCT(&bo); // releases the data
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
        }
        /* the unload above emptied it, so this only balances the
         * construct - but destruct it on every path, not just the
         * failing one */
        PMIX_DESTRUCT(&bkt);
    }

cleanup:
    PMIX_DESTRUCT(&bucket);
    return rc;
}

pmix_status_t pmix_server_fence(pmix_server_caddy_t *cd, pmix_buffer_t *buf,
                                pmix_modex_cbfunc_t modexcbfunc)
{
    int32_t cnt;
    pmix_status_t rc;
    size_t nprocs;
    pmix_proc_t *procs = NULL;
    bool collect_data = false;
    pmix_server_trkr_t *trk;
    char *data = NULL;
    size_t sz = 0;
    pmix_buffer_t bucket;
    pmix_info_t *info = NULL;
    size_t ninfo = 0, ninf, n;
    uint32_t tmo;
    struct timeval tv = {0, 0};

    pmix_output_verbose(2, pmix_server_globals.fence_output,
                        "recvd FENCE");

    /* unpack the number of procs */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, cd->peer, buf, &nprocs, &cnt, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        return rc;
    }
    pmix_output_verbose(2, pmix_server_globals.fence_output,
                        "recvd fence from %s with %d procs",
                        PMIX_PEER_PRINT(cd->peer), (int) nprocs);
    /* there must be at least one as the client has to at least provide
     * their own namespace. Bound it from above as well, by the same
     * round trip through the int32_t the unpack consumes it as: this
     * count is multiplied by sizeof(pmix_proc_t) to size the allocation
     * below, and a wire value large enough to wrap that product yields a
     * short allocation whose element constructors then run off the end.
     * It also keeps "cnt" from silently naming a different number of
     * procs than the qsort and the free below walk. */
    cnt = nprocs;
    if (nprocs < 1 || 0 > cnt || (size_t) cnt != nprocs) {
        return PMIX_ERR_BAD_PARAM;
    }

    /* create space for the procs */
    PMIX_PROC_CREATE(procs, nprocs);
    if (NULL == procs) {
        return PMIX_ERR_NOMEM;
    }
    /* unpack the procs */
    cnt = nprocs;
    PMIX_BFROPS_UNPACK(rc, cd->peer, buf, procs, &cnt, PMIX_PROC);
    if (PMIX_SUCCESS != rc) {
        goto cleanup;
    }
    /* sort the array */
    qsort(procs, nprocs, sizeof(pmix_proc_t), pmix_util_compare_proc);

    /* unpack the number of provided info structs */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, cd->peer, buf, &ninf, &cnt, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        goto cleanup;
    }
    /* This count comes off the wire, so screen it before it is used to
     * size an allocation and then to index that allocation. Two slots are
     * seeded at info[ninf] and info[ninf+1] *before* anything is unpacked,
     * so a count near SIZE_MAX wraps "ninf + 2" down to a one-element
     * array and writes the seeds far outside it; the same wrap can happen
     * inside the allocator's "ninfo * sizeof(pmix_info_t)". Requiring the
     * count to survive the round trip through the int32_t that the unpack
     * below consumes it as - the screen pmix_server_register_events
     * uses - bounds it well clear of both. */
    cnt = ninf;
    if (0 > cnt || (size_t) cnt != ninf) {
        rc = PMIX_ERR_BAD_PARAM;
        PMIX_ERROR_LOG(rc);
        goto cleanup;
    }
    ninfo = ninf + 2;
    PMIX_INFO_CREATE(info, ninfo);
    if (NULL == info) {
        PMIX_PROC_FREE(procs, nprocs);
        return PMIX_ERR_NOMEM;
    }
    /* store the default response */
    rc = PMIX_SUCCESS;
    PMIX_INFO_LOAD(&info[ninf+1], PMIX_LOCAL_COLLECTIVE_STATUS, &rc, PMIX_STATUS);
    PMIX_INFO_LOAD(&info[ninf], PMIX_SORTED_PROC_ARRAY, NULL, PMIX_BOOL);
    if (0 < ninf) {
        /* unpack the info */
        cnt = ninf;
        PMIX_BFROPS_UNPACK(rc, cd->peer, buf, info, &cnt, PMIX_INFO);
        if (PMIX_SUCCESS != rc) {
            PMIX_INFO_FREE(info, ninfo);
            goto cleanup;
        }
        /* see if we are to collect data or enforce a timeout - we don't internally care
         * about any other directives */
        for (n = 0; n < ninf; n++) {
            if (PMIX_CHECK_KEY(&info[n], PMIX_COLLECT_DATA)) {
                collect_data = PMIX_INFO_TRUE(&info[n]);
            } else if (PMIX_CHECK_KEY(&info[n], PMIX_TIMEOUT)) {
                /* convert through a uint32_t of its own: the accessor
                 * writes exactly the width of the requested type, so
                 * handing it the address of a wider time_t would fill only
                 * half the field - the wrong half on a big-endian host */
                rc = PMIx_Value_get_number(&info[n].value, &tmo, PMIX_UINT32);
                if (PMIX_SUCCESS != rc) {
                    PMIX_PROC_FREE(procs, nprocs);
                    PMIX_INFO_FREE(info, ninfo);
                    return rc;
                }
                tv.tv_sec = tmo;
            }
        }
    }

    /* find/create the local tracker for this operation */
    if (NULL == (trk = pmix_server_get_tracker(NULL, procs, nprocs, PMIX_FENCENB_CMD))) {
        /* If no tracker was found - create and initialize it once */
        if (NULL == (trk = pmix_server_new_tracker(NULL, procs, nprocs, PMIX_FENCENB_CMD))) {
            /* only if a bozo error occurs. Do NOT drive opcbfunc here: it
             * releases the caddy and answers the client, and we then return
             * an error, so the switchyard would release the same caddy and
             * answer the same client a second time. Returning the error on
             * its own is what keeps the client from hanging. */
            PMIX_ERROR_LOG(PMIX_ERROR);
            rc = PMIX_ERROR;
            PMIX_INFO_FREE(info, ninfo);
            goto cleanup;
        }
        trk->type = PMIX_FENCENB_CMD;
        trk->modexcbfunc = modexcbfunc;
        /* mark if they want the data back */
        if (collect_data) {
            trk->collect_type = PMIX_COLLECT_YES;
        } else {
            trk->collect_type = PMIX_COLLECT_NO;
        }
    } else {
        switch (trk->collect_type) {
        case PMIX_COLLECT_NO:
            if (collect_data) {
                trk->collect_type = PMIX_COLLECT_INVALID;
            }
            break;
        case PMIX_COLLECT_YES:
            if (!collect_data) {
                trk->collect_type = PMIX_COLLECT_INVALID;
            }
            break;
        default:
            break;
        }
    }

    /* we only save the info structs from the first caller
     * who provides them - it is a user error to provide
     * different values from different participants */
    if (NULL == trk->info) {
        trk->info = info;
        trk->ninfo = ninfo;
    } else {
        /* cleanup */
        PMIX_INFO_FREE(info, ninfo);
        info = NULL;
        ninfo = 0;
    }

    /* add this contributor to the tracker so they get
     * notified when we are done */
    pmix_list_append(&trk->local_cbs, &cd->super);
    /* if a timeout was specified, set it */
    if (0 < tv.tv_sec && !trk->event_active) {
        PMIX_THREADSHIFT_DELAY(trk, fence_timeout, tv.tv_sec);
        trk->event_active = true;
    }

    /* if all local contributions have been received,
     * let the local host's server know that we are at the
     * "fence" point - they will callback once the barrier
     * across all participants has been completed */
    if (pmix_server_trk_complete(trk)) {
        pmix_output_verbose(2, pmix_server_globals.fence_output,
                            "fence LOCALLY complete");
        /* if a timeout was set, then we delete it here as we can
         * ONLY check for local completion. Otherwise, passing
         * the tracker object up to the host can result in
         * competing timeout events, and the host could return
         * the tracker AFTER we released it due to our internal
         * timeout firing */
        if (trk->event_active) {
            pmix_event_del(&trk->ev);
            trk->event_active = false;
        }
        /* if this is a purely local fence (i.e., all participants are local),
         * then it is done and we notify accordingly */
        if (pmix_server_globals.fence_localonly_opt && trk->local) {
            /* the modexcbfunc thread-shifts the call prior to processing,
             * so it is okay to call it directly from here. The switchyard
             * will acknowledge successful acceptance of the fence request,
             * but the client still requires a return from the callback in
             * that scenario, so we leave this caddy on the list of local cbs */
            rc = trk->info[trk->ninfo-1].value.data.status;
            trk->modexcbfunc(rc, NULL, 0, trk, NULL, NULL);
            rc = PMIX_SUCCESS;  // ensure the switchyard doesn't release the caddy
            goto cleanup;
        }
        /* this fence involves non-local procs - check if the
         * host supports it */
        if (NULL == pmix_host_server.fence_nb) {
            rc = PMIX_ERR_NOT_SUPPORTED;
            /* clear the caddy from this tracker so it can be
             * released upon return - the switchyard will send an
             * error to this caller, and so the fence completion
             * function doesn't need to do so */
            pmix_list_remove_item(&trk->local_cbs, &cd->super);
            cd->trk = NULL;
            /* we need to ensure that all other local participants don't
             * just hang waiting for the error return, so execute
             * the fence completion function - it threadshifts the call
             * prior to processing, so it is okay to call it directly
             * from here */
            trk->host_called = false; // the host will not be calling us back
            trk->modexcbfunc(rc, NULL, 0, trk, NULL, NULL);
            goto cleanup;
        }
        /* if the user asked us to collect data, then we have
         * to provide any locally collected data to the host
         * server so they can circulate it - only take data
         * from the specified procs as not everyone is necessarily
         * participating! And only take data intended for remote
         * or global distribution */

        PMIX_CONSTRUCT(&bucket, pmix_buffer_t);
        if (PMIX_SUCCESS != (rc = pmix_server_collect_data(trk, &bucket))) {
            PMIX_ERROR_LOG(rc);
            PMIX_DESTRUCT(&bucket);
            /* clear the caddy from this tracker so it can be
             * released upon return - the switchyard will send an
             * error to this caller, and so the fence completion
             * function doesn't need to do so */
            pmix_list_remove_item(&trk->local_cbs, &cd->super);
            cd->trk = NULL;
            /* we need to ensure that all other local participants don't
             * just hang waiting for the error return, so execute
             * the fence completion function - it threadshifts the call
             * prior to processing, so it is okay to call it directly
             * from here */
            trk->modexcbfunc(rc, NULL, 0, trk, NULL, NULL);
            goto cleanup;
        }
        /* now unload the blob and pass it upstairs */
        PMIX_UNLOAD_BUFFER(&bucket, data, sz);
        PMIX_DESTRUCT(&bucket);
        trk->host_called = true;
        rc = pmix_host_server.fence_nb(trk->pcs, trk->npcs, trk->info, trk->ninfo, data, sz,
                                       trk->modexcbfunc, trk);
        if (PMIX_SUCCESS == rc || PMIX_OPERATION_SUCCEEDED == rc) {
            /* the host has taken the bucket, so what it carries is no
             * longer outstanding. Do this before the completion below,
             * which can release the tracker. */
            pmix_server_modex_contributed(trk);
        }
        if (PMIX_SUCCESS != rc && PMIX_OPERATION_SUCCEEDED != rc) {
            /* clear the caddy from this tracker so it can be
             * released upon return - the switchyard will send an
             * error to this caller, and so the fence completion
             * function doesn't need to do so */
            pmix_list_remove_item(&trk->local_cbs, &cd->super);
            cd->trk = NULL;
            /* we need to ensure that all other local participants don't
             * just hang waiting for the error return, so execute
             * the fence completion function - it threadshifts the call
             * prior to processing, so it is okay to call it directly
             * from here */
            trk->host_called = false; // the host will not be calling us back
            trk->modexcbfunc(rc, NULL, 0, trk, NULL, NULL);
        } else if (PMIX_OPERATION_SUCCEEDED == rc) {
            /* the operation was atomically completed and the host will
             * not be calling us back - ensure we notify all participants.
             * the modexcbfunc thread-shifts the call prior to processing,
             * so it is okay to call it directly from here */
            trk->host_called = false; // the host will not be calling us back
            rc = trk->info[trk->ninfo-1].value.data.status;
            trk->modexcbfunc(rc, NULL, 0, trk, NULL, NULL);
            /* ensure that the switchyard doesn't release the caddy */
            rc = PMIX_SUCCESS;
        }
    }

cleanup:
    PMIX_PROC_FREE(procs, nprocs);
    return rc;
}

/* The single predicate for deciding whether a collective's local phase
 * is complete. The tracker definition must be complete (all participating
 * nspaces registered, so nlocal is final) AND every expected local
 * participant must be accounted for - either by having contributed (an
 * entry on local_cbs) or by having departed before contributing (an entry
 * on departed). A live participant that has not yet been heard from is on
 * neither list, so the sum stays below nlocal until it either contributes
 * or departs. The '>=' comparison is deliberate: it tolerates any
 * over-count from fork/exec'd clones without falsely reporting the
 * collective as incomplete. See docs/how-things-work/collectives. */
bool pmix_server_trk_complete(pmix_server_trkr_t *trk)
{
    if (!trk->def_complete) {
        return false;
    }
    return (pmix_list_get_size(&trk->local_cbs) +
            pmix_list_get_size(&trk->departed)) >= trk->nlocal;
}

/* Record a collective's completion status in the tracker's info array. The
 * participant handlers seed a PMIX_LOCAL_COLLECTIVE_STATUS slot; locate it by
 * key rather than by position. It is the last element for the fence and
 * disconnect families, but connect appends per-participant endpoint info and
 * (for cross-namespace connects) job-level info AFTER that slot (see
 * pmix_server_connect), so a positional write to info[ninfo-1] would clobber
 * the appended info and leave the real status slot stale. Locating by key is
 * correct for every family and cannot underflow when info is unset. If the
 * slot is absent this is a no-op. Shared with the collective-status unit test
 * (test/unit/collective_status.c). */
void pmix_server_set_collective_status(pmix_info_t *info, size_t ninfo,
                                       pmix_status_t status)
{
    size_t n;

    if (NULL == info) {
        return;
    }
    for (n = 0; n < ninfo; n++) {
        if (PMIX_CHECK_KEY(&info[n], PMIX_LOCAL_COLLECTIVE_STATUS)) {
            info[n].value.data.status = status;
            return;
        }
    }
}

/* The reader half of the above. Locate the slot by key for the same
 * reason the setter does: connect appends per-participant endpoint and
 * job-level info past it, so the positional read the fence handler uses
 * on its own arms is only correct for the families that append nothing.
 * A tracker carrying no status slot has not been degraded by anything,
 * so report success. */
pmix_status_t pmix_server_get_collective_status(pmix_info_t *info, size_t ninfo)
{
    size_t n;

    if (NULL == info) {
        return PMIX_SUCCESS;
    }
    for (n = 0; n < ninfo; n++) {
        if (PMIX_CHECK_KEY(&info[n], PMIX_LOCAL_COLLECTIVE_STATUS)) {
            return info[n].value.data.status;
        }
    }
    return PMIX_SUCCESS;
}

/* Account for a lost peer across every in-flight fence/connect/disconnect
 * collective. This is the fence family's counterpart to the group family's
 * pmix_server_grp_peer_lost(), and both are called from the one place that
 * learns a client's socket has dropped - lost_connection() in the PTL base.
 * It lives here, beside the tracker engine it walks, so the two families'
 * loss accounting sits next to the state it touches and can be exercised
 * directly by a unit test (test/unit/trk_peer_lost.c).
 *
 * "Lost" means abnormal termination. A peer that dropped its connection by
 * calling PMIx_Finalize has NOT left the accounting: nptr->nlocalprocs is
 * deliberately left alone for it and the peer object is tombstoned rather
 * than retired, so the rank remains an expected participant and is free to
 * PMIx_Init again and contribute - which is exactly what a client cycling
 * init/finalize (MPI Sessions) does. Recording such a rank as departed
 * would count it twice and let a collective complete without it. See
 * docs/how-things-work/collectives and issue #4113.
 *
 * The tracker may be released here (a loss can complete the collective), so
 * the walk uses FOREACH_SAFE. */
void pmix_server_trk_peer_lost(pmix_peer_t *peer)
{
    pmix_server_trkr_t *trk, *tnxt;
    pmix_server_caddy_t *rinfo;
    pmix_proclist_t *dp;
    pmix_status_t rc;
    bool flag;
    size_t n;

    if (peer->finalized) {
        /* an orderly departure - not a loss */
        return;
    }

    PMIX_LIST_FOREACH_SAFE (trk, tnxt, &pmix_server_globals.collectives, pmix_server_trkr_t) {
        /* check if this peer should be participating in this collective */
        flag = false;
        for (n = 0; n < trk->npcs; n++) {
            if (PMIX_CHECK_NAMES(&trk->pcs[n], &peer->info->pname)) {
                flag = true;
                break;
            }
        }
        if (!flag) {
            continue;
        }
        /* Determine whether this participant has already contributed.
         * Its rank counts as contributed if any of its local peers -
         * the client or a fork/exec'd clone sharing the same name -
         * has an entry on local_cbs. If so, the contribution and the
         * data it already delivered stand: we ignore the loss for
         * this collective. We do NOT reduce the expected count and do
         * NOT remove the contribution, so the collective neither
         * completes early nor drops the departed peer's data. The
         * dead peer's reply caddy is harmless - its socket has been
         * closed, so the eventual QUEUE_REPLY is dropped rather than
         * sent. */
        flag = false;
        PMIX_LIST_FOREACH (rinfo, &trk->local_cbs, pmix_server_caddy_t) {
            if (PMIX_CHECK_NAMES(&rinfo->peer->info->pname, &peer->info->pname)) {
                flag = true;
                break;
            }
        }
        if (flag) {
            /* already contributed - nothing to account for */
            continue;
        }
        /* This participant had not yet contributed and now never will.
         * Record its rank as departed (once) so the collective can
         * complete on the remaining live participants instead of
         * hanging forever. Participation is tracked per rank, matching
         * the way nlocal is counted, so a clone sharing the name does
         * not add a second departed entry. */
        flag = false;
        PMIX_LIST_FOREACH (dp, &trk->departed, pmix_proclist_t) {
            if (PMIX_CHECK_NAMES(&dp->proc, &peer->info->pname)) {
                flag = true;
                break;
            }
        }
        if (!flag) {
            dp = PMIX_NEW(pmix_proclist_t);
            PMIX_LOAD_PROCID(&dp->proc, peer->info->pname.nspace,
                             peer->info->pname.rank);
            pmix_list_append(&trk->departed, &dp->super);
        }
        /* report the collective's health to the surviving participants:
         * partial success if any expected participant is still awaited,
         * else lost-connection */
        if ((pmix_list_get_size(&trk->local_cbs) +
             pmix_list_get_size(&trk->departed)) < trk->nlocal) {
            rc = PMIX_ERR_PARTIAL_SUCCESS;
        } else {
            rc = PMIX_ERR_LOST_CONNECTION;
        }
        /* record the collective's health in the status slot the
         * participant handlers seeded (located by key, since connect
         * appends info after it - see pmix_server_set_collective_status) */
        pmix_server_set_collective_status(trk->info, trk->ninfo, rc);
        /* if the host has already been called for this tracker,
         * then the local phase is frozen - just wait for the host
         * to return from the operation */
        if (trk->host_called) {
            continue;
        }
        /* likewise if this tracker's completion has already been driven.
         * It is still on the collectives list and still answers
         * pmix_server_trk_complete() - its completion handler is
         * thread-shifted and has not run yet - but it is spoken for, and
         * completing it again would hand two handlers the same tracker to
         * unlink and release. This is the common case, not an exotic one:
         * a strictly local collective never sets host_called, so a
         * participant dropping its connection in the window between a
         * local fence completing and its handler running lands here. */
        if (trk->completion_fired) {
            continue;
        }
        /* are we now locally complete? */
        if (pmix_server_trk_complete(trk)) {
            /* The loss of this participant is about to complete the
             * collective, so an armed PMIX_TIMEOUT timer has to come
             * down first - this is the same rule pmix_server_fence
             * applies at its own completion point, and this sweep is
             * where a timer is most likely to be armed, since a
             * timeout is exactly what a participant dropping its
             * connection would otherwise produce. Every arm below
             * either hands the tracker to the host or drives a
             * completion function, and both end with the tracker
             * unlinked and released: a timer left armed then fires
             * fence_timeout/connect_timeout on freed memory, or
             * races the host's completion for the right to release
             * it a second time. */
            if (trk->event_active) {
                pmix_event_del(&trk->ev);
                trk->event_active = false;
            }
            /* if this is a local-only collective, then resolve it now */
            if (trk->local) {
                /* everyone else has called in - we need to let them know
                 * that this proc has disappeared
                 * as otherwise the collective will never complete */
                if (PMIX_FENCENB_CMD == trk->type) {
                    if (NULL != trk->modexcbfunc) {
                        trk->modexcbfunc(rc, NULL, 0, trk, NULL, NULL);
                    }
                } else if (PMIX_CONNECTNB_CMD == trk->type) {
                    if (NULL != trk->op_cbfunc) {
                        trk->op_cbfunc(rc, trk);
                    }
                } else if (PMIX_DISCONNECTNB_CMD == trk->type) {
                    if (NULL != trk->op_cbfunc) {
                        trk->op_cbfunc(rc, trk);
                    }
                } else if (PMIX_GROUP_CONSTRUCT_CMD == trk->type) {
                    if (NULL != trk->op_cbfunc) {
                        trk->op_cbfunc(rc, trk);
                    }
                }
            } else {
                /* if the host has not been called, then we need to pass the call
                 * up to the host as otherwise the global collective will hang */
                if (PMIX_FENCENB_CMD == trk->type) {
                    trk->host_called = true;
                    rc = pmix_host_server.fence_nb(trk->pcs, trk->npcs, trk->info,
                                                   trk->ninfo, NULL, 0,
                                                   trk->modexcbfunc, trk);
                    if (PMIX_SUCCESS != rc) {
                        pmix_list_remove_item(&pmix_server_globals.collectives, &trk->super);
                        PMIX_RELEASE(trk);
                    }
                } else if (PMIX_CONNECTNB_CMD == trk->type) {
                    trk->host_called = true;
                    rc = pmix_host_server.connect(trk->pcs, trk->npcs, trk->info,
                                                  trk->ninfo, trk->op_cbfunc, trk);
                    if (PMIX_SUCCESS != rc) {
                        pmix_list_remove_item(&pmix_server_globals.collectives, &trk->super);
                        PMIX_RELEASE(trk);
                    }
                } else if (PMIX_DISCONNECTNB_CMD == trk->type) {
                    trk->host_called = true;
                    rc = pmix_host_server.disconnect(trk->pcs, trk->npcs, trk->info,
                                                     trk->ninfo, trk->op_cbfunc, trk);
                    if (PMIX_SUCCESS != rc) {
                        pmix_list_remove_item(&pmix_server_globals.collectives, &trk->super);
                        PMIX_RELEASE(trk);
                    }
                }
            }
        }
    }
}
