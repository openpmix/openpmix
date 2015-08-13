/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2014-2015 Intel, Inc.  All rights reserved.
 * Copyright (c) 2014      Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2014-2015 Artem Y. Polyakov <artpol84@gmail.com>.
 *                         All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include <private/autogen/config.h>
#include <pmix/rename.h>
#include <private/types.h>
#include <private/pmix_stdint.h>
#include <private/pmix_socket_errno.h>

#include <pmix_server.h>
#include "src/include/pmix_globals.h"

#ifdef HAVE_STRING_H
#include <string.h>
#endif
#include <fcntl.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#ifdef HAVE_SYS_SOCKET_H
#include <sys/socket.h>
#endif
#ifdef HAVE_SYS_UN_H
#include <sys/un.h>
#endif
#ifdef HAVE_SYS_UIO_H
#include <sys/uio.h>
#endif
#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif
#include PMIX_EVENT_HEADER

#include "src/class/pmix_list.h"
#include "src/buffer_ops/buffer_ops.h"
#include "src/util/argv.h"
#include "src/util/error.h"
#include "src/util/output.h"
#include "src/util/pmix_environ.h"
#include "src/util/progress_threads.h"
#include "src/usock/usock.h"
#include "src/sec/pmix_sec.h"

#include "pmix_server_ops.h"

pmix_server_module_t pmix_host_server;

static void dmdx_cbfunc(pmix_status_t status, const char *data,
                        size_t ndata, void *cbdata);

typedef struct {
    pmix_object_t super;
    pmix_event_t ev;
    pmix_status_t status;
    const char *data;
    size_t ndata;
    pmix_dmdx_local_t *lcd;
} pmix_dmdx_reply_caddy_t;
PMIX_CLASS_INSTANCE(pmix_dmdx_reply_caddy_t,
                   pmix_object_t, NULL, NULL);


pmix_status_t _satisfy_local_req(pmix_nspace_t *nptr, pmix_rank_info_t *info,
                                  pmix_modex_cbfunc_t cbfunc, void *cbdata)
{
    int rc;
    pmix_buffer_t pbkt, xfer;
    pmix_value_t *val;
    char *data;
    size_t sz;


    /* check for the local/global data - data committed to remote
     * scope does not get returned to a local proc
     * get any local/global contribution - note that there
     * may not be a contribution */
    PMIX_CONSTRUCT(&pbkt, pmix_buffer_t);
    rc = pmix_hash_fetch(&info->nptr->server->mylocal, info->rank, "modex", &val);
    if ( PMIX_SUCCESS == rc && NULL != val) {
        PMIX_CONSTRUCT(&xfer, pmix_buffer_t);
        pmix_buffer_t *pxfer = &xfer;
        PMIX_LOAD_BUFFER(&xfer, val->data.bo.bytes, val->data.bo.size);
        pmix_bfrop.pack(&pbkt, &pxfer, 1, PMIX_BUFFER);
        xfer.base_ptr = NULL;
        xfer.bytes_used = 0;
        PMIX_DESTRUCT(&xfer);
        PMIX_VALUE_RELEASE(val);
        PMIX_UNLOAD_BUFFER(&pbkt, data, sz);
        PMIX_DESTRUCT(&pbkt);
        /* pass it back */
        cbfunc(rc, data, sz, cbdata);
        return rc;
    }

    return PMIX_ERR_NOT_FOUND;

    // WARNING!!!!!!!!!!!!!!!!!!!!!!!!!!!!
    // We won't ever execute this!!!!!!!!!
    // Is this correct ???????????????????

    // TODO: Do we need this code????
    // Local proc shouldn't receive remote portion!
    // In what legal situation can we get here?

    if (PMIX_SUCCESS == (rc = pmix_hash_fetch(&nptr->server->remote, info->rank, "modex", &val)) &&
        NULL != val) {
        /* yes, we have it - pass it down */
        PMIX_CONSTRUCT(&pbkt, pmix_buffer_t);
        PMIX_CONSTRUCT(&xfer, pmix_buffer_t);
        pmix_buffer_t *pxfer = &xfer;
        PMIX_LOAD_BUFFER(&xfer, val->data.bo.bytes, val->data.bo.size);
        pmix_bfrop.pack(&pbkt, &pxfer, 1, PMIX_BUFFER);
        xfer.base_ptr = NULL;
        xfer.bytes_used = 0;
        PMIX_DESTRUCT(&xfer);
        PMIX_VALUE_RELEASE(val);
        PMIX_UNLOAD_BUFFER(&pbkt, data, sz);
        PMIX_DESTRUCT(&pbkt);
        /* pass it back */
        cbfunc(rc, data, sz, cbdata);
        return rc;
    }
}

pmix_status_t _satisfy_remote_req(pmix_nspace_t *nptr, int rank,
                                    pmix_modex_cbfunc_t cbfunc, void *cbdata)
{
    int rc;
    pmix_buffer_t pbkt, xfer;
    pmix_value_t *val;
    char *data;
    size_t sz;

    /* check to see if this data already has been
     * obtained as a result of a prior direct modex request from
     * another local peer */
    rc = pmix_hash_fetch(&nptr->server->remote, rank, "modex", &val);
    if (PMIX_SUCCESS == rc && NULL != val) {
        PMIX_CONSTRUCT(&pbkt, pmix_buffer_t);
        PMIX_CONSTRUCT(&xfer, pmix_buffer_t);
        pmix_buffer_t *pxfer = &xfer;
        PMIX_LOAD_BUFFER(&xfer, val->data.bo.bytes, val->data.bo.size);
        pmix_bfrop.pack(&pbkt, &pxfer, 1, PMIX_BUFFER);
        xfer.base_ptr = NULL;
        xfer.bytes_used = 0;
        PMIX_DESTRUCT(&xfer);
        PMIX_VALUE_RELEASE(val);
        PMIX_UNLOAD_BUFFER(&pbkt, data, sz);
        PMIX_DESTRUCT(&pbkt);
        /* pass it back */
        cbfunc(rc, data, sz, cbdata);
        return rc;
    }
    return PMIX_ERR_NOT_FOUND;
}

pmix_status_t pmix_pending_request(pmix_nspace_t *nptr, int rank,
                                   pmix_modex_cbfunc_t cbfunc, void *cbdata)
{
    pmix_dmdx_local_t *lcd = NULL, *cd;
    pmix_rank_info_t *iptr, *info;
    pmix_buffer_t pbkt, xfer;
    pmix_value_t *val;
    char *data;
    size_t sz;
    int rc;

    /* 1. Try to satisfy the request right now */
    info = NULL;
    PMIX_LIST_FOREACH(iptr, &nptr->server->ranks, pmix_rank_info_t) {
        if (iptr->rank == rank) {
            info = iptr;
            break;
        }
    }

    if (NULL != info) {
        /* this is a request for info about another local proc */
        rc = _satisfy_local_req(nptr, info, cbfunc, cbdata);
    } else {
        /* this is a request for info about a remote proc, or
         * a LOCAL proc that we don't know about yet. Because
         * we register client's in events, it is always possible
         * that a client gets launched and issues a "get" before
         * we finish registering all our local clients. So we
         * have to consider the case where a request is made for
         * data about a process that will be local, but hasn't
         * been registered yet */
        if (!nptr->server->all_registered) {
            /* we cannot know if this will be a local or
             * remote proc, so we have to defer processing
             * this request until all local clients for this nspace
             * have been registered */
        }
        rc = _satisfy_remote_req(nptr, rank, cbfunc, cbdata);
    }

    if( PMIX_SUCCESS == rc ){
        /* request was successfully satisfied */
        return rc;
    }

    /* 2. We were unable to satisfy request right now.
     * Look for existing requests to this namespace/rank */
    PMIX_LIST_FOREACH(cd, &pmix_server_globals.local_reqs, pmix_dmdx_local_t) {
        if (0 != strncmp(nptr->nspace, cd->nspace, PMIX_MAX_NSLEN) ||
                rank != cd->rank ) {
            continue;
        }
        lcd = cd;
        break;
    }

    /* 3. If no requests exists then:
     * - if all local clients are registered then we were called because
     * the remote data was requested. Create request and call direct modex
     * to retrieve the data
     * - if not all local ranks were registered, we need to wait untill
     * pmix_pending_localy_fin would be called to resolve this. Just add the
     * request for now.
     */
    if( NULL == lcd ){
        lcd = PMIX_NEW(pmix_dmdx_local_t);
        if( NULL == lcd ){
            return PMIX_ERR_NOMEM;
        }
        strncpy(lcd->nspace, nptr->nspace, PMIX_MAX_NSLEN);
        lcd->rank = rank;
        PMIX_CONSTRUCT(&lcd->loc_reqs, pmix_list_t);
        pmix_list_append(&pmix_server_globals.local_reqs, &lcd->super);

        /* check & send request if need/possible */
        if( nptr->server->all_registered && NULL == info ){
            if( NULL != pmix_host_server.direct_modex ){
                pmix_host_server.direct_modex(lcd->nspace, lcd->rank, dmdx_cbfunc, lcd);
            } else {
                /* if we don't have direct modex feature, just respond with "not found" */
                cbfunc(PMIX_ERR_NOT_FOUND, NULL, 0, cbdata);
                return PMIX_SUCCESS;
            }
        }
    }
    pmix_dmdx_request_t *req = PMIX_NEW(pmix_dmdx_request_t);
    req->cbfunc = cbfunc;
    req->cbdata = cbdata;
    pmix_list_append(&lcd->loc_reqs, &req->super);
    return PMIX_SUCCESS;
}

void pmix_pending_nspace_fix(pmix_nspace_t *nptr)
{
    pmix_dmdx_local_t *cd, *cd_next;

    /* Now when we know all local ranks, go along request list and ask for remote data
     * for the non-local ranks, and resolve all pending requests for local procs
     * that were waiting for registration to complete
     */
    PMIX_LIST_FOREACH_SAFE(cd, cd_next, &pmix_server_globals.local_reqs, pmix_dmdx_local_t) {
        pmix_rank_info_t *info;
        bool found = false;

        if (0 != strncmp(nptr->nspace, cd->nspace, PMIX_MAX_NSLEN) ) {
            continue;
        }

        PMIX_LIST_FOREACH(info, &nptr->server->ranks, pmix_rank_info_t) {
            if (info->rank == cd->rank) {
                found = true;
                break;
            }
        }

        /* if not found - this is remote process and we need to send
         * corresponding direct modex request */
        if( !found ){
            if( NULL != pmix_host_server.direct_modex ){
                pmix_host_server.direct_modex(cd->nspace, cd->rank, dmdx_cbfunc, cd);
            } else {
                pmix_dmdx_request_t *req, *req_next;
                PMIX_LIST_FOREACH_SAFE(req, req_next, &cd->loc_reqs, pmix_dmdx_request_t) {
                    req->cbfunc(PMIX_ERR_NOT_FOUND, NULL, 0, req->cbdata);
                    pmix_list_remove_item(&cd->loc_reqs, (pmix_list_item_t*)req);
                    PMIX_RELEASE(req);
                }
                pmix_list_remove_item(&pmix_server_globals.local_reqs, (pmix_list_item_t*)cd);
            }
        }
    }
}

pmix_status_t pmix_pending_resolve(pmix_nspace_t *nptr, int rank, pmix_dmdx_local_t *lcd)
{
    pmix_dmdx_local_t *cd;

    /* find corresponding request (if exists) */
    if( NULL == lcd ){
        PMIX_LIST_FOREACH(cd, &pmix_server_globals.local_reqs, pmix_dmdx_local_t) {
            if (0 != strncmp(nptr->nspace, cd->nspace, PMIX_MAX_NSLEN) ||
                    rank != cd->rank) {
                continue;
            }
            lcd = cd;
            break;
        }
    }

    if( NULL != lcd ){
        /* check if this rank is local */
        pmix_rank_info_t *iptr, *info = NULL;
        PMIX_LIST_FOREACH(iptr, &nptr->server->ranks, pmix_rank_info_t) {
            if (iptr->rank == rank) {
                info = iptr;
                break;
            }
        }

        pmix_dmdx_request_t *req;
        PMIX_LIST_FOREACH(req, &lcd->loc_reqs, pmix_dmdx_request_t) {
            int rc;
            if( NULL != info ){
                rc = _satisfy_local_req(nptr, info, req->cbfunc, req->cbdata);
            } else {
                rc = _satisfy_remote_req(nptr, rank, req->cbfunc, req->cbdata);
            }
            if( PMIX_SUCCESS != rc ){
                return rc;
            }
        }
        pmix_list_remove_item(&pmix_server_globals.local_reqs, (pmix_list_item_t*)lcd);
    }
    return PMIX_SUCCESS;
}

pmix_status_t pmix_server_abort(pmix_peer_t *peer, pmix_buffer_t *buf,
                                pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    int32_t cnt;
    pmix_status_t rc;
    int status;
    char *msg;
    size_t nprocs;
    pmix_proc_t *procs = NULL;

    pmix_output_verbose(2, pmix_globals.debug_output, "recvd ABORT");

    /* unpack the status */
    cnt = 1;
    if (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(buf, &status, &cnt, PMIX_INT))) {
        return rc;
    }
    /* unpack the message */
    cnt = 1;
    if (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(buf, &msg, &cnt, PMIX_STRING))) {
        return rc;
    }
    /* unpack the number of procs */
    cnt = 1;
    if (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(buf, &nprocs, &cnt, PMIX_SIZE))) {
        return rc;
    }

    /* unpack any provided procs - these are the procs the caller
     * wants aborted */
    if (0 < nprocs) {
        PMIX_PROC_CREATE(procs, nprocs);
        cnt = nprocs;
        if (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(buf, procs, &cnt, PMIX_PROC))) {
            return rc;
        }
    }

    /* let the local host's server execute it */
    if (NULL != pmix_host_server.abort) {
        rc = pmix_host_server.abort(peer->info->nptr->nspace, peer->info->rank,
                                    peer->info->server_object, status, msg,
                                    procs, nprocs, cbfunc, cbdata);
    } else {
        rc = PMIX_ERR_NOT_SUPPORTED;
        /* release the caller */
        if (NULL != cbfunc) {
            cbfunc(rc, cbdata);
        }
    }
    PMIX_PROC_FREE(procs, nprocs);

    /* the client passed this msg to us so we could give
     * it to the host server - we are done with it now */
    if (NULL != msg) {
        free(msg);
    }

    return rc;
}

pmix_status_t pmix_server_commit(pmix_peer_t *peer, pmix_buffer_t *buf)
{
    int32_t cnt;
    pmix_status_t rc;
    pmix_buffer_t *b2;
    pmix_kval_t *kp;
    pmix_scope_t scope;
    pmix_hash_table_t *ht;
    pmix_nspace_t *nptr;
    pmix_rank_info_t *info;
    pmix_dmdx_remote_t *dcd, *dcdnext;
    pmix_buffer_t pbkt, xfer;
    pmix_value_t *val;
    char *data;
    size_t sz;

    /* shorthand */
    info = peer->info;
    nptr = info->nptr;

    /* this buffer will contain one or more buffers, each
     * representing a different scope. These need to be locally
     * stored separately so we can provide required data based
     * on the requestor's location */
    cnt = 1;
    while (PMIX_SUCCESS == (rc = pmix_bfrop.unpack(buf, &scope, &cnt, PMIX_SCOPE))) {
        if (PMIX_LOCAL == scope) {
           ht = &nptr->server->mylocal;
        } else if (PMIX_REMOTE == scope) {
            ht = &nptr->server->myremote;
        } else {
            PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
            rc = PMIX_ERR_BAD_PARAM;
            return rc;
        }
        /* unpack and store the blob */
        cnt = 1;
        if (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(buf, &b2, &cnt, PMIX_BUFFER))) {
            PMIX_ERROR_LOG(rc);
            return rc;
        }
        kp = PMIX_NEW(pmix_kval_t);
        kp->key = strdup("modex");
        PMIX_VALUE_CREATE(kp->value, 1);
        kp->value->type = PMIX_BYTE_OBJECT;
        PMIX_UNLOAD_BUFFER(b2, kp->value->data.bo.bytes, kp->value->data.bo.size);
        PMIX_RELEASE(b2);
        /* store it in the appropriate hash */
        if (PMIX_SUCCESS != (rc = pmix_hash_store(ht, info->rank, kp))) {
            PMIX_ERROR_LOG(rc);
        }
        PMIX_RELEASE(kp);  // maintain acctg
        cnt = 1;
    }
    if (PMIX_ERR_UNPACK_READ_PAST_END_OF_BUFFER != rc) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    rc = PMIX_SUCCESS;
    /* mark us as having successfully received a blob from this proc */
    info->modex_recvd = true;

    /* see if anyone remote is waiting on this data - could be more than one */
    PMIX_LIST_FOREACH_SAFE(dcd, dcdnext, &pmix_server_globals.remote_pnd, pmix_dmdx_remote_t) {
        if (0 != strncmp(dcd->cd->nspace, nptr->nspace, PMIX_MAX_NSLEN)) {
            continue;
        }
        if (dcd->cd->rank == info->rank) {
           /* we can now fulfill this request - collect the
             * remote/global data from this proc */
            PMIX_CONSTRUCT(&pbkt, pmix_buffer_t);
            /* get any remote contribution - note that there
             * may not be a contribution */
            if (PMIX_SUCCESS == pmix_hash_fetch(&nptr->server->myremote, info->rank, "modex", &val) &&
                NULL != val) {
                PMIX_CONSTRUCT(&xfer, pmix_buffer_t);
                PMIX_LOAD_BUFFER(&xfer, val->data.bo.bytes, val->data.bo.size);
                pmix_buffer_t *pxfer = &xfer;
                pmix_bfrop.pack(&pbkt, &pxfer, 1, PMIX_BUFFER);
                xfer.base_ptr = NULL;
                xfer.bytes_used = 0;
                PMIX_DESTRUCT(&xfer);
                PMIX_VALUE_RELEASE(val);
            }
            PMIX_UNLOAD_BUFFER(&pbkt, data, sz);
            PMIX_DESTRUCT(&pbkt);
            /* execute the callback */
            dcd->cd->cbfunc(PMIX_SUCCESS, data, sz, dcd->cd->cbdata);
            if (NULL != data) {
                free(data);
            }
            /* we have finished this request */
            pmix_list_remove_item(&pmix_server_globals.remote_pnd, &dcd->super);
            PMIX_RELEASE(dcd);
        }
    }
    /* see if anyone local is waiting on this data- could be more than one */
    return pmix_pending_resolve(nptr, info->rank, NULL);
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
static pmix_server_trkr_t* get_tracker(pmix_proc_t *procs,
                                       size_t nprocs, pmix_cmd_t type)
{
    pmix_server_trkr_t *trk;
    pmix_rank_info_t *iptr, *info;
    size_t i;
    bool match, all_def;
    pmix_nspace_t *nptr, *ns;

    pmix_output_verbose(5, pmix_globals.debug_output,
                        "get_tracker called with %d procs", (int)nprocs);

    /* bozo check - should never happen outside of programmer error */
    if (NULL == procs) {
        PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
        return NULL;
    }

    /* there is no shortcut way to search the trackers - all
     * we can do is perform a brute-force search. Fortunately,
     * it is highly unlikely that there will be more than one
     * or two active at a time, and they are most likely to
     * involve only a single proc with WILDCARD rank - so this
     * shouldn't take long */
    PMIX_LIST_FOREACH(trk, &pmix_server_globals.collectives, pmix_server_trkr_t) {
        /* Collective operation if unique identified by
         * the set of participating processes and the type of collective
         */
        if (nprocs != trk->npcs) {
            continue;
        }
        if( type != trk->type ){
            continue;
        }
        match = true;
        for (i=0; i < nprocs; i++) {
            if (0 != strcmp(procs[i].nspace, trk->pcs[i].nspace) ||
                procs[i].rank != trk->pcs[i].rank) {
                match = false;
                break;
            }
        }
        if (match) {
            return trk;
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
static pmix_server_trkr_t* new_tracker(pmix_proc_t *procs,
                                       size_t nprocs, pmix_cmd_t type)
{
    pmix_server_trkr_t *trk;
    pmix_rank_info_t *iptr, *info;
    size_t i;
    bool match, all_def;
    pmix_nspace_t *nptr, *ns;

    pmix_output_verbose(5, pmix_globals.debug_output,
                        "get_tracker called with %d procs", (int)nprocs);

    /* bozo check - should never happen outside of programmer error */
    if (NULL == procs) {
        PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
        return NULL;
    }

    assert( NULL == get_tracker(procs, nprocs, type) );

    pmix_output_verbose(5, pmix_globals.debug_output,
                        "adding new tracker with %d procs", (int)nprocs);

    /* get here if this tracker is new - create it */
    trk = PMIX_NEW(pmix_server_trkr_t);

    /* copy the procs */
    PMIX_PROC_CREATE(trk->pcs, nprocs);
    trk->npcs = nprocs;
    trk->type = type;

    all_def = true;
    for (i=0; i < nprocs; i++) {
        (void)strncpy(trk->pcs[i].nspace, procs[i].nspace, PMIX_MAX_NSLEN);
        trk->pcs[i].rank = procs[i].rank;
        /* is this nspace known to us? */
        nptr = NULL;
        PMIX_LIST_FOREACH(ns, &pmix_server_globals.nspaces, pmix_nspace_t) {
            if (0 == strcmp(procs[i].nspace, ns->nspace)) {
                nptr = ns;
                break;
            }
        }
        if (NULL == nptr) {
            /* cannot be a local proc */
            pmix_output_verbose(8, pmix_globals.debug_output,
                                "get_tracker: unknown nspace %s",
                                procs[i].nspace);
            continue;
        }
        /* have all the clients for this nspace been defined? */
        if (!nptr->server->all_registered) {
            /* nope, so no point in going further on this one - we'll
             * process it once all the procs are known */
            all_def = false;
            pmix_output_verbose(8, pmix_globals.debug_output,
                                "get_tracker: all clients not registered nspace %s",
                                procs[i].nspace);
            continue;
        }
        /* is this one of my local ranks? */
        PMIX_LIST_FOREACH(info, &nptr->server->ranks, pmix_rank_info_t) {
            if (procs[i].rank == info->rank ||
                PMIX_RANK_WILDCARD == procs[i].rank) {
                pmix_output_verbose(5, pmix_globals.debug_output,
                                    "adding local proc %s.%d to tracker", procs[i].nspace, procs[i].rank);
                /* add a tracker for this proc - don't need more than
                 * the nspace pointer and rank */
                iptr = PMIX_NEW(pmix_rank_info_t);
                PMIX_RETAIN(info->nptr);
                iptr->nptr = info->nptr;
                iptr->rank = info->rank;
                pmix_list_append(&trk->ranks, &iptr->super);
                /* track the count */
                ++trk->nlocal;
                if (PMIX_RANK_WILDCARD != procs[i].rank) {
                    break;
                }
            }
        }
    }
    if (all_def) {
        trk->def_complete = true;
    }
    pmix_list_append(&pmix_server_globals.collectives, &trk->super);
    return trk;
}

pmix_status_t pmix_server_fence(pmix_server_caddy_t *cd,
                                pmix_buffer_t *buf,
                                pmix_modex_cbfunc_t modexcbfunc,
                                pmix_op_cbfunc_t opcbfunc)
{
    int32_t cnt;
    pmix_status_t rc;
    size_t nprocs;
    pmix_proc_t *procs=NULL;
    int collect_data;
    pmix_server_trkr_t *trk;
    char *data = NULL;
    size_t sz = 0;
    pmix_buffer_t bucket, xfer;
    pmix_rank_info_t *info;
    pmix_value_t *val;

    pmix_output_verbose(2, pmix_globals.debug_output,
                        "recvd FENCE");

    if (NULL == pmix_host_server.fence_nb) {
        return PMIX_ERR_NOT_SUPPORTED;
    }

    /* unpack the number of procs */
    cnt = 1;
    if (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(buf, &nprocs, &cnt, PMIX_SIZE))) {
        return rc;
    }
    pmix_output_verbose(2, pmix_globals.debug_output,
                        "recvd fence with %d procs", (int)nprocs);
    /* there must be at least one as the client has to at least provide
     * their own namespace */
    if (nprocs < 1) {
        return PMIX_ERR_BAD_PARAM;
    }

    /* create space for the procs */
    PMIX_PROC_CREATE(procs, nprocs);
    /* unpack the procs */
    cnt = nprocs;
    if (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(buf, procs, &cnt, PMIX_PROC))) {
        goto cleanup;
    }

    /* unpack the data flag - indicates if the caller wants
     * all modex data returned at end of procedure */
    cnt = 1;
    if (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(buf, &collect_data, &cnt, PMIX_INT))) {
        goto cleanup;
    }
    /* find/create the local tracker for this operation */
    if (NULL == (trk = get_tracker(procs, nprocs, PMIX_FENCENB_CMD))) {
        /* If no tracker was found - create and initialize it once */
        if( NULL == (trk = new_tracker(procs, nprocs, PMIX_FENCENB_CMD))) {
            /* only if a bozo error occurs */
            PMIX_ERROR_LOG(PMIX_ERROR);
            /* DO NOT HANG */
            if (NULL != opcbfunc) {
                opcbfunc(PMIX_ERROR, cd);
            }
            rc = PMIX_ERROR;
            goto cleanup;
        }
        trk->type = PMIX_FENCENB_CMD;
        trk->modexcbfunc = modexcbfunc;
        /* mark if they want the data back */
        if (0 == collect_data) {
            trk->collect_type = PMIX_COLLECT_NO;
        } else {
            trk->collect_type = PMIX_COLLECT_YES;
        }
    } else {
        switch ( trk->collect_type ) {
        case PMIX_COLLECT_NO:
            if( collect_data ){
                trk->collect_type = PMIX_COLLECT_INVALID;
            }
            break;
        case PMIX_COLLECT_YES:
            if( !collect_data ){
                trk->collect_type = PMIX_COLLECT_INVALID;
            }
            break;
        default:
            break;
        }
    }

    /* add this contributor to the tracker so they get
     * notified when we are done */
    PMIX_RETAIN(cd);
    pmix_list_append(&trk->local_cbs, &cd->super);
    /* if all local contributions have been received,
     * let the local host's server know that we are at the
     * "fence" point - they will callback once the barrier
     * across all participants has been completed */
    if (trk->def_complete &&
        pmix_list_get_size(&trk->local_cbs) == trk->nlocal) {
        pmix_output_verbose(2, pmix_globals.debug_output,
                            "fence complete");
        /* if the user asked us to collect data, then we have
         * to provide any locally collected data to the host
         * server so they can circulate it - only take data
         * from the specified procs as not everyone is necessarily
         * participating! And only take data intended for remote
         * distribution */

        PMIX_CONSTRUCT(&bucket, pmix_buffer_t);

        assert( PMIX_COLLECT_MAX < UCHAR_MAX );
        unsigned char tmp = (unsigned char)trk->collect_type;
        pmix_bfrop.pack(&bucket, &tmp, 1, PMIX_BYTE);

        if( PMIX_COLLECT_YES == trk->collect_type) {
            pmix_buffer_t databuf;
            PMIX_CONSTRUCT(&databuf, pmix_buffer_t);
            pmix_output_verbose(2, pmix_globals.debug_output,
                                "fence - assembling data");
            PMIX_LIST_FOREACH(info, &trk->ranks, pmix_rank_info_t) {
                pmix_buffer_t rankbuf;
                PMIX_CONSTRUCT(&rankbuf, pmix_buffer_t);
                /* get any remote contribution - note that there
                 * may not be a contribution */
                if (PMIX_SUCCESS == pmix_hash_fetch(&info->nptr->server->myremote, info->rank, "modex", &val) &&
                    NULL != val) {
                    /* pack the proc so we know the source */
                    char *foobar = info->nptr->nspace;
                    pmix_bfrop.pack(&rankbuf, &foobar, 1, PMIX_STRING);
                    pmix_bfrop.pack(&rankbuf, &info->rank, 1, PMIX_INT);
                    PMIX_CONSTRUCT(&xfer, pmix_buffer_t);
                    PMIX_LOAD_BUFFER(&xfer, val->data.bo.bytes, val->data.bo.size);
                    pmix_buffer_t *pxfer = &xfer;
                    pmix_bfrop.pack(&rankbuf, &pxfer, 1, PMIX_BUFFER);
                    xfer.base_ptr = NULL;
                    xfer.bytes_used = 0;
                    PMIX_DESTRUCT(&xfer);
                    PMIX_VALUE_RELEASE(val);
                    /* now pack this proc's contribution into the bucket */
                    pmix_buffer_t *pdatabuf = &rankbuf;
                    pmix_bfrop.pack(&databuf, &pdatabuf, 1, PMIX_BUFFER);
                }
                PMIX_DESTRUCT(&rankbuf);
            }
            // TODO: we have multiple data movings while only one is actually need
            pmix_buffer_t *pbkt = &databuf;
            pmix_bfrop.pack(&bucket, &pbkt, 1, PMIX_BUFFER);
            PMIX_DESTRUCT(&databuf);
        }

        PMIX_UNLOAD_BUFFER(&bucket, data, sz);
        PMIX_DESTRUCT(&bucket);
        pmix_host_server.fence_nb(trk->pcs, trk->npcs,
                                  data, sz, trk->modexcbfunc, trk);
    }

 cleanup:
    PMIX_PROC_FREE(procs, nprocs);

    return rc;
}

static void _process_dmdx_reply(int fd, short args, void *cbdata)
{
    pmix_dmdx_reply_caddy_t *caddy = (pmix_dmdx_reply_caddy_t *)cbdata;
    pmix_kval_t *kp;
    pmix_nspace_t *ns, *nptr;
    pmix_rank_info_t *iptr, *info;
    pmix_status_t rc;

    pmix_output_verbose(2, pmix_globals.debug_output,
                    "[%s:%d] queue dmdx reply from %s:%d",
                    __FILE__, __LINE__, caddy->lcd->nspace, caddy->lcd->rank);

    /* find the nspace object for this client */
    nptr = NULL;
    PMIX_LIST_FOREACH(ns, &pmix_server_globals.nspaces, pmix_nspace_t) {
        if (0 == strcmp(caddy->lcd->nspace, ns->nspace)) {
            nptr = ns;
            break;
        }
    }

    if (NULL == nptr) {
        /* should be impossible */
        PMIX_ERROR_LOG(PMIX_ERR_NOT_FOUND);
        caddy->status = PMIX_ERR_NOT_FOUND;
        PMIX_RELEASE(caddy);
        return;
    }
    /* and the rank entry for it */
    info = NULL;
    PMIX_LIST_FOREACH(iptr, &nptr->server->ranks, pmix_rank_info_t) {
        if (iptr->rank == caddy->lcd->rank) {
            info = iptr;
            break;
        }
    }
    if (NULL == info) {
        /* create the entry */
        info = PMIX_NEW(pmix_rank_info_t);
        info->rank = caddy->lcd->rank;
        PMIX_RETAIN(nptr);
        info->nptr = nptr;
        pmix_list_append(&nptr->server->ranks, &info->super);
    }

    kp = PMIX_NEW(pmix_kval_t);
    kp->key = strdup("modex");
    PMIX_VALUE_CREATE(kp->value, 1);
    kp->value->type = PMIX_BYTE_OBJECT;
    kp->value->data.bo.bytes = (char*)caddy->data;
    kp->value->data.bo.size = caddy->ndata;
    /* store it in the appropriate hash */
    if (PMIX_SUCCESS != (rc = pmix_hash_store(&info->nptr->server->remote, info->rank, kp))) {
        PMIX_ERROR_LOG(rc);
    }
    kp->value->data.bo.bytes = NULL;  // protect the data
    PMIX_RELEASE(kp);  // maintain acctg

  cleanup:
    /* always execute the callback to avoid having the client hang */
    pmix_pending_resolve(nptr, caddy->lcd->rank, caddy->lcd);
    PMIX_RELEASE(caddy);
}

static void dmdx_cbfunc(pmix_status_t status,
                        const char *data, size_t ndata,
                        void *cbdata)
{
    pmix_dmdx_reply_caddy_t *caddy;
    char *data_copy = NULL;
    caddy = PMIX_NEW(pmix_dmdx_reply_caddy_t);
    caddy->status = status;
    /* we cannot just point to the caller's data as
     * there is no way to guarantee it remains present
     * until we service our event thread. So we have
     * no choice but to copy it.
     *
     * artpol NOTE: we can also provide the feature to let RM know
     * that we are done with the data and let it cleanup buffer itself.
     * This can be done through one more cbfunc+cbdata that can be passed
     * here from RM. */

    data_copy = malloc(sizeof(char) * ndata);
    memcpy(data_copy, data, ndata);
    caddy->data   = data_copy;
    caddy->ndata  = ndata;
    caddy->lcd    = (pmix_dmdx_local_t *)cbdata;
    pmix_output_verbose(2, pmix_globals.debug_output, "[%s:%d] queue dmdx reply %s:%d",
                        __FILE__, __LINE__, caddy->lcd->nspace, caddy->lcd->rank);
    event_assign(&caddy->ev, pmix_globals.evbase, -1, EV_WRITE,
                 _process_dmdx_reply, caddy);
    event_priority_set(&caddy->ev, 0);
    event_active(&caddy->ev, EV_WRITE, 1);
}

pmix_status_t pmix_server_get(pmix_buffer_t *buf,
                              pmix_modex_cbfunc_t cbfunc,
                              void *cbdata)
{
    int32_t cnt;
    pmix_status_t rc;
    int rank;
    char *cptr;
    char nspace[PMIX_MAX_NSLEN+1];
    pmix_nspace_t *ns, *nptr;

    pmix_output_verbose(2, pmix_globals.debug_output,
                        "recvd GET");

    /* setup */
    memset(nspace, 0, sizeof(nspace));

    /* retrieve the nspace and rank of the requested proc */
    cnt = 1;
    if (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(buf, &cptr, &cnt, PMIX_STRING))) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    (void)strncpy(nspace, cptr, PMIX_MAX_NSLEN);
    free(cptr);
    cnt = 1;
    if (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(buf, &rank, &cnt, PMIX_INT))) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    /* find the nspace object for this client */
    nptr = NULL;
    PMIX_LIST_FOREACH(ns, &pmix_server_globals.nspaces, pmix_nspace_t) {
        if (0 == strcmp(nspace, ns->nspace)) {
            nptr = ns;
            break;
        }
    }

    pmix_output_verbose(2, pmix_globals.debug_output,
                        "%s:%d EXECUTE GET FOR %s:%d",
                        pmix_globals.nspace,
                        pmix_globals.rank, nspace, rank);
    if (NULL == nptr) {
        /* this is for an nspace we don't know about yet, so
         * give the host server a chance to tell us about it */
        nptr = PMIX_NEW(pmix_nspace_t);
        (void)strncpy(nptr->nspace, nspace, PMIX_MAX_NSLEN);
        pmix_list_append(&pmix_server_globals.nspaces, &nptr->super);
    }
    /* if we don't have any ranks for this job, protect ourselves here */
    if (NULL == nptr->server) {
        nptr->server = PMIX_NEW(pmix_server_nspace_t);
    }

    return pmix_pending_request(nptr, rank, cbfunc, cbdata);
}

pmix_status_t pmix_server_publish(pmix_peer_t *peer,
                                  pmix_buffer_t *buf,
                                  pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    pmix_status_t rc;
    int32_t cnt;
    pmix_data_range_t scope;
    pmix_persistence_t persist;
    size_t i, ninfo;
    pmix_info_t *info = NULL;

    pmix_output_verbose(2, pmix_globals.debug_output,
                        "recvd PUBLISH");

    if (NULL == pmix_host_server.publish) {
        return PMIX_ERR_NOT_SUPPORTED;
    }

    /* unpack the scope */
    cnt=1;
    if (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(buf, &scope, &cnt, PMIX_DATA_RANGE))) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    /* unpack the persistence */
    cnt=1;
    if (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(buf, &persist, &cnt, PMIX_PERSIST))) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    /* unpack the number of info objects */
    cnt=1;
    if (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(buf, &ninfo, &cnt, PMIX_SIZE))) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    /* unpack the array of info objects */
    if (0 < ninfo) {
        info = (pmix_info_t*)malloc(ninfo * sizeof(pmix_info_t));
        cnt=ninfo;
        if (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(buf, info, &cnt, PMIX_INFO))) {
            PMIX_ERROR_LOG(rc);
            goto cleanup;
        }
    }

    /* call the local server */
    rc = pmix_host_server.publish(peer->info->nptr->nspace, peer->info->rank,
                                  scope, persist, info, ninfo, cbfunc, cbdata);

 cleanup:
    if (NULL != info) {
        for (i=0; i < ninfo; i++) {
            PMIX_INFO_DESTRUCT(&info[i]);
        }
        free(info);
    }
    return rc;
}

pmix_status_t pmix_server_lookup(pmix_peer_t *peer,
                                 pmix_buffer_t *buf,
                                 pmix_lookup_cbfunc_t cbfunc, void *cbdata)
{
    int32_t cnt;
    pmix_status_t rc;
    int wait;
    pmix_data_range_t scope;
    size_t nkeys, i;
    char **keys=NULL, *sptr;

    pmix_output_verbose(2, pmix_globals.debug_output,
                        "recvd LOOKUP");

    if (NULL == pmix_host_server.lookup) {
        return PMIX_ERR_NOT_SUPPORTED;
    }

    /* unpack the scope */
    cnt=1;
    if  (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(buf, &scope, &cnt, PMIX_DATA_RANGE))) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    /* unpack the wait flag */
    cnt=1;
    if  (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(buf, &wait, &cnt, PMIX_INT))) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    /* unpack the number of keys */
    cnt=1;
    if  (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(buf, &nkeys, &cnt, PMIX_SIZE))) {
         PMIX_ERROR_LOG(rc);
        return rc;
    }
    /* unpack the array of keys */
    for (i=0; i < nkeys; i++) {
        cnt=1;
        if  (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(buf, &sptr, &cnt, PMIX_STRING))) {
            PMIX_ERROR_LOG(rc);
            goto cleanup;
        }
        pmix_argv_append_nosize(&keys, sptr);
        free(sptr);
    }
    /* call the local server */
    rc = pmix_host_server.lookup(peer->info->nptr->nspace, peer->info->rank, scope, wait, keys, cbfunc, cbdata);

 cleanup:
    pmix_argv_free(keys);
    return rc;
}

pmix_status_t pmix_server_unpublish(pmix_peer_t *peer,
                                    pmix_buffer_t *buf,
                                    pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    int32_t cnt;
    pmix_status_t rc;
    pmix_data_range_t scope;
    size_t i, nkeys;
    char **keys=NULL, *sptr;

    pmix_output_verbose(2, pmix_globals.debug_output,
                        "recvd UNPUBLISH");

    if (NULL == pmix_host_server.unpublish) {
        return PMIX_ERR_NOT_SUPPORTED;
    }

    /* unpack the scope */
    cnt=1;
    if  (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(buf, &scope, &cnt, PMIX_DATA_RANGE))) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    /* unpack the number of keys */
    cnt=1;
    if  (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(buf, &nkeys, &cnt, PMIX_SIZE))) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    /* unpack the keys */
    for (i=0; i < nkeys; i++) {
        cnt=1;
        if  (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(buf, &sptr, &cnt, PMIX_STRING))) {
            PMIX_ERROR_LOG(rc);
            goto cleanup;
        }
        pmix_argv_append_nosize(&keys, sptr);
        free(sptr);
    }
    /* call the local server */
    rc = pmix_host_server.unpublish(peer->info->nptr->nspace, peer->info->rank, scope, keys, cbfunc, cbdata);

 cleanup:
    pmix_argv_free(keys);
    return rc;
}

pmix_status_t pmix_server_spawn(pmix_peer_t *peer,
                                pmix_buffer_t *buf,
                                pmix_spawn_cbfunc_t cbfunc,
                                void *cbdata)
{
    int32_t cnt;
    size_t napps;
    pmix_app_t *apps=NULL;
    pmix_status_t rc;

    pmix_output_verbose(2, pmix_globals.debug_output,
                        "recvd SPAWN");

    if (NULL == pmix_host_server.spawn) {
        return PMIX_ERR_NOT_SUPPORTED;
    }

    /* unpack the number of apps */
    cnt=1;
    if  (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(buf, &napps, &cnt, PMIX_SIZE))) {
        return rc;
    }
    /* unpack the array of apps */
    if (0 < napps) {
        PMIX_APP_CREATE(apps, napps);
        cnt=napps;
        if (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(buf, apps, &cnt, PMIX_APP))) {
            PMIX_ERROR_LOG(rc);
            goto cleanup;
        }
    }
    /* call the local server */
    rc = pmix_host_server.spawn(peer->info->nptr->nspace, peer->info->rank,
                                apps, napps, cbfunc, cbdata);

 cleanup:
    if (NULL != apps) {
        PMIX_APP_FREE(apps, napps);
    }
    return rc;
}

pmix_status_t pmix_server_connect(pmix_server_caddy_t *cd,
                                  pmix_buffer_t *buf, bool disconnect,
                                  pmix_op_cbfunc_t cbfunc)
{
    int32_t cnt;
    pmix_status_t rc;
    pmix_proc_t *procs;
    size_t nprocs;
    pmix_server_trkr_t *trk;

    pmix_output_verbose(2, pmix_globals.debug_output,
                        "recvd CONNECT");

    if ((disconnect && NULL == pmix_host_server.disconnect) ||
        (!disconnect && NULL == pmix_host_server.connect)) {
        return PMIX_ERR_NOT_SUPPORTED;
    }

    /* unpack the number of procs */
    cnt = 1;
    if (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(buf, &nprocs, &cnt, PMIX_SIZE))) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    /* there must be at least one proc - we do not allow the client
     * to send us NULL proc as the server has no idea what to do
     * with that situation. Instead, the client should at least send
     * us their own namespace for the use-case where the connection
     * spans all procs in that namespace */
    if (nprocs < 1) {
        PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
        return PMIX_ERR_BAD_PARAM;
    }

    /* unpack the procs */
    PMIX_PROC_CREATE(procs, nprocs);
    cnt = nprocs;
    if (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(buf, procs, &cnt, PMIX_PROC))) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }

    /* find/create the local tracker for this operation */
    pmix_cmd_t type = PMIX_CONNECTNB_CMD;
    if (disconnect) {
        type = PMIX_DISCONNECTNB_CMD;
    }
    if (NULL == (trk = get_tracker(procs, nprocs, type))) {
        if (NULL == (trk = new_tracker(procs, nprocs, type))) {
            /* only if a bozo error occurs */
            PMIX_ERROR_LOG(PMIX_ERROR);
            /* DO NOT HANG */
            if (NULL != cbfunc) {
                cbfunc(PMIX_ERROR, cd);
            }
            rc = PMIX_ERROR;
            goto cleanup;
        }
        trk->op_cbfunc = cbfunc;
    }

    /* add this contributor to the tracker so they get
     * notified when we are done */
    PMIX_RETAIN(cd);
    pmix_list_append(&trk->local_cbs, &cd->super);
    /* if all local contributions have been received,
     * let the local host's server know that we are at the
     * "fence" point - they will callback once the [dis]connect
     * across all participants has been completed */
    if (trk->def_complete &&
        pmix_list_get_size(&trk->local_cbs) == trk->nlocal) {
        if (disconnect) {
            rc = pmix_host_server.disconnect(procs, nprocs, cbfunc, trk);
        } else {
            rc = pmix_host_server.connect(procs, nprocs, cbfunc, trk);
        }
    } else {
        rc = PMIX_SUCCESS;
    }

 cleanup:
    PMIX_PROC_FREE(procs, nprocs);

    return rc;
}

// instance server library classes
static void tcon(pmix_server_trkr_t *t)
{
    t->pcs = NULL;
    t->npcs = 0;
    t->active = true;
    t->def_complete = false;
    PMIX_CONSTRUCT(&t->ranks, pmix_list_t);
    PMIX_CONSTRUCT(&t->local_cbs, pmix_list_t);
    t->nlocal = 0;
    t->local_cnt = 0;
    /* this needs to be set explicitly */
    t->collect_type = PMIX_COLLECT_INVALID;
    t->modexcbfunc = NULL;
    t->op_cbfunc = NULL;
}
static void tdes(pmix_server_trkr_t *t)
{
    if (NULL != t->pcs) {
        free(t->pcs);
    }
    PMIX_LIST_DESTRUCT(&t->ranks);
    PMIX_LIST_DESTRUCT(&t->local_cbs);
}
PMIX_CLASS_INSTANCE(pmix_server_trkr_t,
                   pmix_list_item_t,
                   tcon, tdes);

static void cdcon(pmix_server_caddy_t *cd)
{
    cd->peer = NULL;
    PMIX_CONSTRUCT(&cd->snd, pmix_snd_caddy_t);
}
static void cddes(pmix_server_caddy_t *cd)
{
    if (NULL != cd->peer) {
        PMIX_RELEASE(cd->peer);
    }
    PMIX_DESTRUCT(&cd->snd);
}
PMIX_CLASS_INSTANCE(pmix_server_caddy_t,
                   pmix_list_item_t,
                   cdcon, cddes);


PMIX_CLASS_INSTANCE(pmix_snd_caddy_t,
                   pmix_object_t,
                   NULL, NULL);

static void scadcon(pmix_setup_caddy_t *p)
{
    memset(p->nspace, 0, sizeof(p->nspace));
    p->active = true;
    p->server_object = NULL;
    p->nlocalprocs = 0;
    p->info = NULL;
    p->ninfo = 0;
    p->cbfunc = NULL;
    p->cbdata = NULL;
}
static void scaddes(pmix_setup_caddy_t *p)
{
    if (NULL != p->info) {
        PMIX_INFO_FREE(p->info, p->ninfo);
    }
}
PMIX_CLASS_INSTANCE(pmix_setup_caddy_t,
                    pmix_object_t,
                    scadcon, scaddes);

static void ncon(pmix_notify_caddy_t *p)
{
    p->active = true;
    p->procs = NULL;
    p->nprocs = 0;
    p->error_procs = NULL;
    p->error_nprocs = 0;
    p->info = NULL;
    p->ninfo = 0;
    p->buf = PMIX_NEW(pmix_buffer_t);
}
static void ndes(pmix_notify_caddy_t *p)
{
    if (NULL != p->procs) {
        PMIX_PROC_FREE(p->procs, p->nprocs);
    }
    if (NULL != p->error_procs) {
        PMIX_PROC_FREE(p->error_procs, p->error_nprocs);
    }
    if (NULL != p->info) {
        PMIX_INFO_FREE(p->info, p->ninfo);
    }
    if (NULL != p->buf) {
        PMIX_RELEASE(p->buf);
    }
}
PMIX_CLASS_INSTANCE(pmix_notify_caddy_t,
                    pmix_object_t,
                    ncon, ndes);

PMIX_CLASS_INSTANCE(pmix_trkr_caddy_t,
                    pmix_object_t,
                    NULL, NULL);

static void dmcon(pmix_dmdx_remote_t *p)
{
    p->cd = NULL;
}
static void dmdes(pmix_dmdx_remote_t *p)
{
    if (NULL != p->cd) {
        PMIX_RELEASE(p->cd);
    }
}
PMIX_CLASS_INSTANCE(pmix_dmdx_remote_t,
                    pmix_list_item_t,
                    dmcon, dmdes);

PMIX_CLASS_INSTANCE(pmix_dmdx_request_t,
                    pmix_list_item_t,
                    NULL, NULL);

static void lmcon(pmix_dmdx_local_t *p)
{
    memset(p->nspace, 0, PMIX_MAX_NSLEN+1);
}
PMIX_CLASS_INSTANCE(pmix_dmdx_local_t,
                    pmix_list_item_t,
                    lmcon, NULL);

PMIX_CLASS_INSTANCE(pmix_pending_connection_t,
                    pmix_object_t,
                    NULL, NULL);
