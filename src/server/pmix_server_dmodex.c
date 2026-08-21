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

/* Data calls made by the host down into us: a request for the modex
 * data of one of our local clients, and the direct store of a key. */

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
#include "src/runtime/pmix_progress_threads.h"
#include "src/util/pmix_error.h"
#include "src/util/pmix_name_fns.h"
#include "src/util/pmix_output.h"

#include "pmix_server_ops.h"

/***************************************************************************************************
 *  Support calls from the host server down to us requesting direct modex data provided by one     *
 *  of our local clients                                                                           *
 ***************************************************************************************************/

static void _dmodex_req(int sd, short args, void *cbdata)
{
    pmix_setup_caddy_t *cd = (pmix_setup_caddy_t *) cbdata;
    pmix_rank_info_t *info, *iptr;
    pmix_namespace_t *nptr, *ns;
    char *data = NULL;
    size_t sz = 0;
    pmix_dmdx_remote_t *dcd;
    pmix_status_t rc;
    pmix_buffer_t pbkt;
    pmix_kval_t *kv;
    pmix_cb_t cb;

    PMIX_ACQUIRE_OBJECT(cd);
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    pmix_output_verbose(2, pmix_server_globals.base_output, "DMODX LOOKING FOR %s",
                        PMIX_NAME_PRINT(&cd->proc));

    /* this should be one of my clients, but a race condition
     * could cause this request to arrive prior to us having
     * been informed of it - so first check to see if we know
     * about this nspace yet */
    nptr = NULL;
    PMIX_LIST_FOREACH (ns, &pmix_globals.nspaces, pmix_namespace_t) {
        if (0 == strcmp(ns->nspace, cd->proc.nspace)) {
            nptr = ns;
            break;
        }
    }
    if (NULL == nptr) {
        /* we don't know this namespace yet, and so we obviously
         * haven't received the data from this proc yet - defer
         * the request until we do */
        dcd = PMIX_NEW(pmix_dmdx_remote_t);
        if (NULL == dcd) {
            rc = PMIX_ERR_NOMEM;
            goto cleanup;
        }
        dcd->cd = cd;
        pmix_list_append(&pmix_server_globals.remote_pnd, &dcd->super);
        return;
    }

    /* They are asking for job level data for this process */
    if (PMIX_RANK_WILDCARD == cd->proc.rank) {
        /* fetch the job-level info for this nspace */
        /* this is going to a remote peer, so inform the gds
         * that we need an actual copy of the data */
        PMIX_CONSTRUCT(&cb, pmix_cb_t);
        cb.proc = &cd->proc;
        cb.scope = PMIX_REMOTE;
        cb.copy = true;
        PMIX_CONSTRUCT(&pbkt, pmix_buffer_t);
        PMIX_GDS_FETCH_KV(rc, pmix_globals.mypeer, &cb);
        if (PMIX_SUCCESS == rc) {
            /* assemble the provided data into a byte object */
            PMIX_LIST_FOREACH (kv, &cb.kvs, pmix_kval_t) {
                PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, &pbkt, kv, 1, PMIX_KVAL);
                if (PMIX_SUCCESS != rc) {
                    PMIX_DESTRUCT(&pbkt);
                    PMIX_DESTRUCT(&cb);
                    goto cleanup;
                }
            }
        }
        PMIX_DESTRUCT(&cb);
        PMIX_UNLOAD_BUFFER(&pbkt, data, sz);
        PMIX_DESTRUCT(&pbkt);
        goto cleanup;
    }

    /* see if we have this peer in our list */
    info = NULL;
    PMIX_LIST_FOREACH (iptr, &nptr->ranks, pmix_rank_info_t) {
        if (iptr->pname.rank == cd->proc.rank) {
            info = iptr;
            break;
        }
    }
    if (NULL == info) {
        /* Not a rank we currently host. There are two ways to get here
         * and they need opposite answers: it may not have been
         * registered YET, in which case waiting is right and the data
         * will arrive; or it may have run and been reaped, in which case
         * nothing is ever coming and waiting means the process that
         * asked sits in PMIx_Get until the job is killed.
         *
         * nptr->departed is what tells them apart - see the note on it
         * in pmix_globals.h. Answer a departed rank the same way a
         * request already parked when the proc finalized is answered:
         * PMIX_ERR_NOT_FOUND, promptly. Reading a peer that has exited
         * is an application error, and the developer needs to be told
         * that rather than left with a hang to diagnose. */
        pmix_proclist_t *dp;
        PMIX_LIST_FOREACH (dp, &nptr->departed, pmix_proclist_t) {
            if (PMIX_CHECK_PROCID(&dp->proc, &cd->proc)) {
                pmix_output_verbose(2, pmix_server_globals.get_output,
                                    "%s:%d DMODEX FOR TERMINATED PROC %s",
                                    pmix_globals.myid.nspace,
                                    pmix_globals.myid.rank,
                                    PMIX_NAME_PRINT(&cd->proc));
                rc = PMIX_ERR_NOT_FOUND;
                goto cleanup;
            }
        }
        /* rank isn't known yet - defer
         * the request until we do */
        dcd = PMIX_NEW(pmix_dmdx_remote_t);
        if (NULL == dcd) {
            rc = PMIX_ERR_NOMEM;
            goto cleanup;
        }
        dcd->cd = cd;
        pmix_list_append(&pmix_server_globals.remote_pnd, &dcd->super);
        return;
    }

    /* have we received the modex from this proc yet - if
     * not, then defer */
    if (!info->modex_recvd) {
        /* track the request so we can fulfill it once
         * data is recvd */
        dcd = PMIX_NEW(pmix_dmdx_remote_t);
        if (NULL == dcd) {
            rc = PMIX_ERR_NOMEM;
            goto cleanup;
        }
        dcd->cd = cd;
        pmix_list_append(&pmix_server_globals.remote_pnd, &dcd->super);
        return;
    }

    /* collect the remote/global data from this proc */
    PMIX_CONSTRUCT(&cb, pmix_cb_t);
    cb.proc = &cd->proc;
    cb.scope = PMIX_REMOTE;
    cb.copy = true;
    PMIX_GDS_FETCH_KV(rc, pmix_globals.mypeer, &cb);
    if (PMIX_SUCCESS == rc) {
        /* assemble the provided data into a byte object */
        PMIX_CONSTRUCT(&pbkt, pmix_buffer_t);
        PMIX_LIST_FOREACH (kv, &cb.kvs, pmix_kval_t) {
            PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, &pbkt, kv, 1, PMIX_KVAL);
            if (PMIX_SUCCESS != rc) {
                PMIX_DESTRUCT(&pbkt);
                PMIX_DESTRUCT(&cb);
                goto cleanup;
            }
        }
        PMIX_UNLOAD_BUFFER(&pbkt, data, sz);
        PMIX_DESTRUCT(&pbkt);
    }
    PMIX_DESTRUCT(&cb);

cleanup:
    /* execute the callback */
    cd->cbfunc(rc, data, sz, cd->cbdata);
    if (NULL != data) {
        free(data);
    }
    PMIX_RELEASE(cd);
}

void pmix_server_fail_remote_pnd(pmix_peer_t *peer, pmix_proc_t *proc,
                                 pmix_status_t status)
{
    pmix_dmdx_remote_t *dcd, *dnxt;

    PMIX_LIST_FOREACH_SAFE (dcd, dnxt, &pmix_server_globals.remote_pnd, pmix_dmdx_remote_t) {
        if ((NULL != peer && NULL != peer->info
             && PMIX_CHECK_NAMES(&peer->info->pname, &dcd->cd->proc))
            || (NULL != proc && PMIX_CHECK_PROCID(proc, &dcd->cd->proc))) {
            pmix_list_remove_item(&pmix_server_globals.remote_pnd, &dcd->super);
            /* the only thing that ever takes a request off this list is
             * the target committing its data, and the target has just
             * gone away - so answer the host rather than dropping its
             * request. A dropped one leaves the remote server that asked
             * for the data, and the client behind it, waiting on a reply
             * nobody is going to send */
            if (NULL != dcd->cd->cbfunc) {
                dcd->cd->cbfunc(status, NULL, 0, dcd->cd->cbdata);
            }
            PMIX_RELEASE(dcd);
        }
    }
}

PMIX_EXPORT pmix_status_t PMIx_server_dmodex_request(const pmix_proc_t *proc,
                                                     pmix_dmodex_response_fn_t cbfunc, void *cbdata)
{
    pmix_setup_caddy_t *cd;

    if (!pmix_atomic_check_bool(&pmix_globals.initialized)) {
        return PMIX_ERR_INIT;
    }

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        return PMIX_ERR_NOT_AVAILABLE;
    }

    /* protect against bozo */
    if (NULL == cbfunc || NULL == proc) {
        return PMIX_ERR_BAD_PARAM;
    }

    pmix_output_verbose(2, pmix_server_globals.base_output,
                        "%s pmix:server dmodex request for proc %s",
                        PMIX_NAME_PRINT(&pmix_globals.myid), PMIX_NAME_PRINT(proc));

    cd = PMIX_NEW(pmix_setup_caddy_t);
    if (NULL == cd) {
        return PMIX_ERR_NOMEM;
    }
    pmix_strncpy(cd->proc.nspace, proc->nspace, PMIX_MAX_NSLEN);
    cd->proc.rank = proc->rank;
    cd->cbfunc = cbfunc;
    cd->cbdata = cbdata;

    /* we have to push this into our event library to avoid
     * potential threading issues */
    PMIX_THREADSHIFT(cd, _dmodex_req);
    return PMIX_SUCCESS;
}

static void _store_internal(int sd, short args, void *cbdata)
{
    pmix_shift_caddy_t *cd = (pmix_shift_caddy_t *) cbdata;
    pmix_proc_t proc;

    PMIX_ACQUIRE_OBJECT(cd);
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    pmix_strncpy(proc.nspace, cd->pname.nspace, PMIX_MAX_NSLEN);
    proc.rank = cd->pname.rank;
    PMIX_GDS_STORE_KV(cd->status, pmix_globals.mypeer, &proc, PMIX_INTERNAL, cd->kv);
    PMIX_WAKEUP_THREAD(&cd->lock);
}

PMIX_EXPORT pmix_status_t PMIx_Store_internal(const pmix_proc_t *proc, const char key[],
                                              pmix_value_t *val)
{
    pmix_shift_caddy_t *cd;
    pmix_status_t rc;

    if (!pmix_atomic_check_bool(&pmix_globals.initialized)) {
        return PMIX_ERR_INIT;
    }

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        return PMIX_ERR_NOT_AVAILABLE;
    }

    /* the value is dereferenced by the transfer below, so screen it here
     * alongside the key rather than faulting inside bfrops */
    if (NULL == key || PMIX_MAX_KEYLEN < pmix_keylen(key) || NULL == val) {
        return PMIX_ERR_BAD_PARAM;
    }

    if (pmix_progress_thread_check_blocking("PMIx_Store_internal")) {
        /* this call always waits for the progress thread to do the store,
         * so it cannot be made from that thread */
        return PMIX_ERR_WOULD_BLOCK;
    }

    /* setup to thread shift this request */
    cd = PMIX_NEW(pmix_shift_caddy_t);
    if (NULL == cd) {
        return PMIX_ERR_NOMEM;
    }
    if (NULL == proc) {
        cd->pname.nspace = strdup(pmix_globals.myid.nspace);
        cd->pname.rank = pmix_globals.myid.rank;
    } else {
        cd->pname.nspace = strdup(proc->nspace);
        cd->pname.rank = proc->rank;
    }

    cd->kv = PMIX_NEW(pmix_kval_t);
    if (NULL == cd->kv) {
        PMIX_RELEASE(cd);
        return PMIX_ERR_NOMEM;
    }
    cd->kv->key = strdup(key);
    /* construct the destination rather than handing the transfer raw
     * heap: the transfer assigns the type before it copies the payload,
     * so a copy that fails part way leaves a pointer-backed type sitting
     * over an uninitialized union - and the kval destructor below then
     * frees whatever was in it. A constructed value fails safe */
    PMIX_VALUE_CREATE(cd->kv->value, 1);
    if (NULL == cd->kv->value) {
        PMIX_RELEASE(cd);
        return PMIX_ERR_NOMEM;
    }
    PMIX_BFROPS_VALUE_XFER(rc, pmix_globals.mypeer, cd->kv->value, val);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(cd);
        return rc;
    }

    PMIX_THREADSHIFT(cd, _store_internal);
    PMIX_WAIT_THREAD(&cd->lock);
    rc = cd->status;
    PMIX_RELEASE(cd);

    return rc;
}
