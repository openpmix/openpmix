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
 * Copyright (c) 2021-2025 Nanook Consulting  All rights reserved.
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

#ifndef MAX
#    define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif

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
        /* unpack and store the blob */
        cnt = 1;
        PMIX_CONSTRUCT(&b2, pmix_buffer_t);
        PMIX_BFROPS_ASSIGN_TYPE(peer, &b2);
        PMIX_BFROPS_UNPACK(rc, peer, buf, &b2, &cnt, PMIX_BUFFER);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
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

    /* execute the provided callback function with the error */
    if (NULL != trk->modexcbfunc) {
        trk->modexcbfunc(PMIX_ERR_TIMEOUT, NULL, 0, trk, NULL, NULL);
        return; // the cbfunc will have cleaned up the tracker
    }
    trk->event_active = false;
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
    pmix_status_t ret;
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

        ret = PMIX_ERR_NOT_FOUND;
        // have any local clients registered?
        if (0 < pmix_list_get_size(&nptr->ranks)) {
            // get one of the local clients so we will
            // know its gds module
            rinfo = (pmix_rank_info_t*)pmix_list_get_first(&nptr->ranks);
            if (NULL != rinfo) {
                peer = (pmix_peer_t*)pmix_pointer_array_get_item(&pmix_server_globals.clients, rinfo->peerid);
                if (NULL != peer) {
                    PMIX_GDS_FETCH_KV(ret, peer, &cb);
                }
            }
        }
        // if we didn't find it there, try getting it
        // from our storage
        if (PMIX_SUCCESS != ret) {
            PMIX_GDS_FETCH_KV(ret, pmix_globals.mypeer, &cb);
            if (PMIX_SUCCESS != ret) {
                PMIX_ERROR_LOG(ret);
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
        PMIX_BFROPS_PACK(ret, pmix_globals.mypeer, &cb.data, &pbo, 1, PMIX_BYTE_OBJECT);
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
    PMIX_WAKEUP_THREAD(&cbin->lock);
    PMIX_POST_OBJECT(cb);
}

pmix_status_t PMIx_server_collect_job_info(pmix_proc_t *procs, size_t nprocs,
                                           pmix_data_buffer_t *dbuf)
{
    pmix_cb_t cb;
    pmix_byte_object_t bo;
    pmix_status_t rc;

    // we need to threadshift this request as it accesses
    // global data
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

    PMIX_CONSTRUCT(&bucket, pmix_buffer_t);

    if (PMIX_COLLECT_YES == trk->collect_type) {
       pmix_output_verbose(2, pmix_server_globals.fence_output,
                           "fence - assembling data");

        PMIX_CONSTRUCT(&rank_blobs, pmix_list_t);
        PMIX_LIST_FOREACH (scd, &trk->local_cbs, pmix_server_caddy_t) {
            /* get any remote contribution - note that there
             * may not be a contribution */
            PMIX_LOAD_PROCID(&pcs, scd->peer->info->pname.nspace, scd->peer->info->pname.rank);
            pbkt = PMIX_NEW(pmix_buffer_t);
            /* pack the rank */
            PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, pbkt, &pcs, 1, PMIX_PROC);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_DESTRUCT(&cb);
                PMIX_LIST_DESTRUCT(&rank_blobs);
                PMIX_RELEASE(pbkt);
                goto cleanup;
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
                    if (rc != PMIX_SUCCESS) {
                        PMIX_ERROR_LOG(rc);
                        PMIX_DESTRUCT(&cb);
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
        /* mark the collection type so we can check on the
         * receiving end that all participants did the same. Note
         * that if the receiving end thinks that the collect flag
         * is false, then store_modex will not be called on that
         * node and this information (and the flag) will be ignored,
         * meaning that no error is generated! */
        blob_info_byte= PMIX_COLLECT_YES;
        /* pack the modex blob info byte */
        PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, &bucket, &blob_info_byte, 1, PMIX_BYTE);

        /* pack the collected blobs of processes */
        PMIX_LIST_FOREACH (blob, &rank_blobs, rank_blob_t) {
            /* extract the blob */
            PMIX_UNLOAD_BUFFER(blob->buf, bo.bytes, bo.size);
            blob->buf = NULL;
            /* pack the returned blob */
            PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, &bucket, &bo, 1, PMIX_BYTE_OBJECT);
            PMIX_BYTE_OBJECT_DESTRUCT(&bo); // releases the data
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
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
            PMIX_DESTRUCT(&bkt);
        }
    }

cleanup:
    PMIX_DESTRUCT(&bucket);
    return rc;
}

pmix_status_t pmix_server_fence(pmix_server_caddy_t *cd, pmix_buffer_t *buf,
                                pmix_modex_cbfunc_t modexcbfunc, pmix_op_cbfunc_t opcbfunc)
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
     * their own namespace */
    if (nprocs < 1) {
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
        return rc;
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
                rc = PMIx_Value_get_number(&info[n].value, &tv.tv_sec, PMIX_UINT32);
                if (PMIX_SUCCESS != rc) {
                    PMIX_PROC_FREE(procs, nprocs);
                    PMIX_INFO_FREE(info, ninfo);
                    return rc;
                }
            }
        }
    }

    /* find/create the local tracker for this operation */
    if (NULL == (trk = pmix_server_get_tracker(NULL, procs, nprocs, PMIX_FENCENB_CMD))) {
        /* If no tracker was found - create and initialize it once */
        if (NULL == (trk = pmix_server_new_tracker(NULL, procs, nprocs, PMIX_FENCENB_CMD))) {
            /* only if a bozo error occurs */
            PMIX_ERROR_LOG(PMIX_ERROR);
            /* DO NOT HANG */
            if (NULL != opcbfunc) {
                opcbfunc(PMIX_ERROR, cd);
            }
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
    if (trk->def_complete && pmix_list_get_size(&trk->local_cbs) == trk->nlocal) {
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
