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

/* The switchyard: dispatch of inbound client and tool commands to their
 * handlers, and the family of host-server callbacks that queue the
 * resulting replies. */

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
#include "src/mca/ptl/base/base.h"
#include "src/tool/pmix_tool_ops.h"
#include "src/util/pmix_error.h"
#include "src/util/pmix_name_fns.h"
#include "src/util/pmix_output.h"

#include "pmix_server_ops.h"
#include "src/client/pmix_client_ops.h"

static void _opcbfunc(int sd, short args, void *cbdata)
{
    pmix_shift_caddy_t *scd = (pmix_shift_caddy_t*)cbdata;
    pmix_server_caddy_t *cd = (pmix_server_caddy_t*)scd->cbdata;
    pmix_buffer_t *reply;
    pmix_status_t rc;
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    PMIX_ACQUIRE_OBJECT(scd);

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        PMIX_RELEASE(cd);
        PMIX_RELEASE(scd);
        return;
    }

    /* setup the reply with the returned status */
    if (NULL == (reply = PMIX_NEW(pmix_buffer_t))) {
        PMIX_ERROR_LOG(PMIX_ERR_OUT_OF_RESOURCE);
        goto cleanup;
    }
    PMIX_BFROPS_PACK(rc, cd->peer, reply, &scd->status, 1, PMIX_STATUS);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(reply);
        goto cleanup;
    }

    /* the function that created the server_caddy did a
     * retain on the peer, so we don't have to worry about
     * it still being present - send a copy to the originator */
    PMIX_PTL_SEND_ONEWAY(rc, cd->peer, reply, cd->hdr.tag);
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(reply);
    }

    if (scd->enviro) {
        /* ensure that we know the peer has finalized else we
         * will generate an event when the socket closes - yes,
         * it should have been done, but it is REALLY important
         * that it be set */
        cd->peer->finalized = true;
    }

cleanup:
    /* cleanup */
    PMIX_RELEASE(cd);
    PMIX_RELEASE(scd);
}

static void op_cbfunc(pmix_status_t status, void *cbdata)
{
    pmix_shift_caddy_t *scd;
    pmix_server_caddy_t *cd = (pmix_server_caddy_t*)cbdata;

    pmix_output_verbose(2, pmix_server_globals.base_output,
                        "server:op_cbfunc called with %s status",
                        PMIx_Error_string(status));

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        PMIX_RELEASE(cd);
        return;
    }

    /* need to thread-shift this callback */
    scd = PMIX_NEW(pmix_shift_caddy_t);
    if (NULL == scd) {
        /* nothing we can do */
        PMIX_ERROR_LOG(PMIX_ERR_NOMEM);
        return;
    }
    scd->status = status;
    scd->cbdata = cbdata;
    scd->enviro = false;  // flag that we are not finalizing the peer
    PMIX_THREADSHIFT(scd, _opcbfunc);
}

static void op_cbfunc2(pmix_status_t status, void *cbdata)
{
    pmix_shift_caddy_t *scd;
    pmix_server_caddy_t *cd = (pmix_server_caddy_t*)cbdata;

    pmix_output_verbose(2, pmix_server_globals.base_output,
                        "server:op_cbfunc2 called with %s status",
                        PMIx_Error_string(status));

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        PMIX_RELEASE(cd);
        return;
    }

    /* need to thread-shift this callback */
    scd = PMIX_NEW(pmix_shift_caddy_t);
    if (NULL == scd) {
        /* nothing we can do */
        PMIX_ERROR_LOG(PMIX_ERR_NOMEM);
        return;
    }
    scd->status = status;
    scd->cbdata = cbdata;
    scd->enviro = true;  // flag that we are finalizing this peer
    PMIX_THREADSHIFT(scd, _opcbfunc);
}

static void _resopcbfunc(int sd, short args, void *cbdata)
{
    pmix_shift_caddy_t *scdwrapper = (pmix_shift_caddy_t *) cbdata;
    pmix_setup_caddy_t *scd = (pmix_setup_caddy_t*)scdwrapper->cbdata;
    pmix_server_caddy_t *cd = (pmix_server_caddy_t *) scd->cbdata;
    pmix_buffer_t *reply = NULL;
    pmix_status_t rc;
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    PMIX_ACQUIRE_OBJECT(scdwrapper);

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        goto cleanup;
    }

    /* setup the reply with the returned status */
    if (NULL == (reply = PMIX_NEW(pmix_buffer_t))) {
        PMIX_ERROR_LOG(PMIX_ERR_OUT_OF_RESOURCE);
        goto cleanup;
    }
    PMIX_BFROPS_PACK(rc, cd->peer, reply, &scdwrapper->status, 1, PMIX_STATUS);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto cleanup;
    }

    /* the function that created the server_caddy did a
     * retain on the peer, so we don't have to worry about
     * it still being present - send a copy to the originator */
    PMIX_PTL_SEND_ONEWAY(rc, cd->peer, reply, cd->hdr.tag);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto cleanup;
    }
    /* the PTL owns the reply on a successful send - fall through to
     * release the caddies (previously this path returned early, leaking
     * cd, scd, scd->nspace, and scdwrapper on every successful reply) */
    reply = NULL;

    /* cleanup */
cleanup:
    PMIX_RELEASE(cd);
    if (NULL != scd->nspace) {
        free(scd->nspace);
    }
    PMIX_RELEASE(scd);
    PMIX_RELEASE(scdwrapper);
    if (NULL != reply) {
        PMIX_RELEASE(reply);
    }
}

static void resop_cbfunc(pmix_status_t status, void *cbdata)
{
    pmix_shift_caddy_t *scd;
    pmix_server_caddy_t *cd = (pmix_server_caddy_t *)cbdata;

    pmix_output_verbose(2, pmix_server_globals.base_output,
                        "server:resop_cbfunc called with %s status",
                        PMIx_Error_string(status));

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        PMIX_RELEASE(cd);
        return;
    }

    /* need to thread-shift this callback */
    scd = PMIX_NEW(pmix_shift_caddy_t);
    if (NULL == scd) {
        /* nothing we can do */
        PMIX_ERROR_LOG(PMIX_ERR_NOMEM);
        return;
    }
    scd->status = status;
    scd->cbdata = cbdata;
    PMIX_THREADSHIFT(scd, _resopcbfunc);
}

static void _spcb(int sd, short args, void *cbdata)
{
    pmix_shift_caddy_t *cd = (pmix_shift_caddy_t *) cbdata;
    pmix_buffer_t *reply;
    pmix_status_t rc;
    pmix_proc_t proc;
    pmix_cb_t cb;
    pmix_kval_t *kv;
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    PMIX_ACQUIRE_OBJECT(cd);

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        goto cleanup;
    }

    /* setup the reply with the returned status */
    if (NULL == (reply = PMIX_NEW(pmix_buffer_t))) {
        PMIX_ERROR_LOG(PMIX_ERR_OUT_OF_RESOURCE);
        goto cleanup;
    }
    PMIX_BFROPS_PACK(rc, cd->cd->peer, reply, &cd->status, 1, PMIX_STATUS);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(reply);
        goto cleanup;
    }
    /* pass back the name of the nspace */
    PMIX_BFROPS_PACK(rc, cd->cd->peer, reply, &cd->pname.nspace, 1, PMIX_STRING);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(reply);
        goto cleanup;
    }
    if (PMIX_SUCCESS == cd->status) {
        /* add the job-level info, if we have it */
        PMIX_LOAD_PROCID(&proc, cd->pname.nspace, PMIX_RANK_WILDCARD);
        /* this is going to a local client, so let the gds
         * have the option of returning a copy of the data,
         * or a pointer to local storage */
        PMIX_CONSTRUCT(&cb, pmix_cb_t);
        cb.proc = &proc;
        cb.scope = PMIX_SCOPE_UNDEF;
        cb.copy = false;
        PMIX_GDS_FETCH_KV(rc, pmix_globals.mypeer, &cb);
        if (PMIX_SUCCESS == rc) {
            PMIX_LIST_FOREACH (kv, &cb.kvs, pmix_kval_t) {
                PMIX_BFROPS_PACK(rc, cd->cd->peer, reply, kv, 1, PMIX_KVAL);
                if (PMIX_SUCCESS != rc) {
                    PMIX_ERROR_LOG(rc);
                    PMIX_RELEASE(reply);
                    PMIX_DESTRUCT(&cb);
                    goto cleanup;
                }
            }
            PMIX_DESTRUCT(&cb);
        }
    }

    /* the function that created the server_caddy did a
     * retain on the peer, so we don't have to worry about
     * it still being present - tell the originator the result */
    PMIX_SERVER_QUEUE_REPLY(rc, cd->cd->peer, cd->cd->hdr.tag, reply);
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(reply);
    }

cleanup:
    /* cleanup */
    PMIX_RELEASE(cd->cd);
    PMIX_RELEASE(cd);
}

static void spawn_cbfunc(pmix_status_t status, char *nspace, void *cbdata)
{
    pmix_shift_caddy_t *cd;
    pmix_server_caddy_t *scd = (pmix_server_caddy_t *) cbdata;

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        PMIX_RELEASE(scd);
        return;
    }

    /* need to thread-shift this request */
    cd = PMIX_NEW(pmix_shift_caddy_t);
    cd->status = status;
    if (NULL != nspace) {
        cd->pname.nspace = strdup(nspace);
    }
    cd->cd = (pmix_server_caddy_t *) cbdata;

    PMIX_THREADSHIFT(cd, _spcb);
}

static void _lkupcbfunc(int sd, short args, void *cbdata)
{
    pmix_shift_caddy_t *scd = (pmix_shift_caddy_t *) cbdata;
    pmix_server_caddy_t *cd = (pmix_server_caddy_t *) scd->cbdata;
    pmix_buffer_t *reply;
    pmix_status_t rc;
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    PMIX_ACQUIRE_OBJECT(scd);

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        goto cleanup;
    }
    /* no need to thread-shift as no global data is accessed */
    /* setup the reply with the returned status */
    if (NULL == (reply = PMIX_NEW(pmix_buffer_t))) {
        PMIX_ERROR_LOG(PMIX_ERR_OUT_OF_RESOURCE);
        goto cleanup;
    }
    PMIX_BFROPS_PACK(rc, cd->peer, reply, &scd->status, 1, PMIX_STATUS);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(reply);
        goto cleanup;
    }
    if (NULL != scd->pdata) {
        /* pack the returned data objects */
        PMIX_BFROPS_PACK(rc, cd->peer, reply, &scd->ndata, 1, PMIX_SIZE);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_RELEASE(reply);
            goto cleanup;
        }
        PMIX_BFROPS_PACK(rc, cd->peer, reply, scd->pdata, scd->ndata, PMIX_PDATA);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_RELEASE(reply);
            goto cleanup;
        }
        /* the array was created with ndata, and npdata is never assigned
         * anywhere - it is zero from the constructor, and PMIx_Pdata_free()
         * does nothing at all for a count of zero, so this leaked the whole
         * array and every value in it on every lookup that returned data */
        PMIX_PDATA_FREE(scd->pdata, scd->ndata);
    }

    /* the function that created the server_caddy did a
     * retain on the peer, so we don't have to worry about
     * it still being present - tell the originator the result */
    PMIX_SERVER_QUEUE_REPLY(rc, cd->peer, cd->hdr.tag, reply);
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(reply);
    }

cleanup:
    /* cleanup */
    PMIX_RELEASE(cd);
    PMIX_RELEASE(scd);
}

static void lookup_cbfunc(pmix_status_t status, pmix_pdata_t pdata[], size_t ndata, void *cbdata)
{
    pmix_server_caddy_t *cd = (pmix_server_caddy_t *)cbdata;
    pmix_shift_caddy_t *scd;
    size_t n;
    pmix_status_t rc;

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        PMIX_RELEASE(cd);
        return;
    }

    /* need to thread-shift this request */
    scd = PMIX_NEW(pmix_shift_caddy_t);
    scd->status = status;
    if (NULL != pdata) {
        scd->ndata = ndata;
        PMIX_PDATA_CREATE(scd->pdata, scd->ndata);
        for (n=0; n < scd->ndata; n++) {
            memcpy(&scd->pdata[n].proc, &pdata[n].proc, sizeof(pmix_proc_t));
            memcpy(scd->pdata[n].key, pdata[n].key, sizeof(pmix_key_t));
            PMIX_BFROPS_VALUE_XFER(rc, cd->peer, &scd->pdata[n].value, &pdata[n].value);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
            }
        }
    }
    scd->cbdata = cbdata;

    PMIX_THREADSHIFT(scd, _lkupcbfunc);
}

/* fence modex calls return here when the host RM has completed
 * the operation - any enclosed data is provided to us as a blob
 * which contains byte objects, one for each set of data. Our
 * peer servers will have packed the blobs using our common
 * GDS module, so use the mypeer one to unpack them */
static void _mdxcbfunc(int sd, short args, void *cbdata)
{
    pmix_shift_caddy_t *scd = (pmix_shift_caddy_t *) cbdata;
    pmix_server_trkr_t *tracker = scd->tracker;
    pmix_buffer_t xfer, *reply;
    pmix_server_caddy_t *cd, *nxt;
    pmix_status_t rc = PMIX_SUCCESS, ret;
    pmix_nspace_caddy_t *nptr;
    pmix_list_t nslist;
    bool found;
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    PMIX_ACQUIRE_OBJECT(scd);

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        PMIX_RELEASE(scd);
        return;
    }

    if (NULL == tracker) {
        /* give them a release if they want it - this should
         * never happen, but protect against the possibility.
         * modex_cbfunc parks the host's release data in relcbdata; cbdata
         * is not set on this caddy at all, so handing that to the host's
         * release function passes it something that is not its own. */
        if (NULL != scd->cbfunc.relfn) {
            scd->cbfunc.relfn(scd->relcbdata);
        }
        PMIX_RELEASE(scd);
        return;
    }

    /* if we get here, then there are processes waiting
     * for a response */

    /* if the timer is active, clear it */
    if (tracker->event_active) {
        pmix_event_del(&tracker->ev);
    }

    /* pass the blobs being returned */
    PMIX_CONSTRUCT(&xfer, pmix_buffer_t);
    PMIX_CONSTRUCT(&nslist, pmix_list_t);

    if (PMIX_SUCCESS != scd->status) {
        rc = scd->status;
        goto finish_collective;
    }

    if (PMIX_COLLECT_INVALID == tracker->collect_type) {
        rc = PMIX_ERR_INVALID_ARG;
        goto finish_collective;
    }

    /* Collect the list of unique nspaces so we store the
     * data so each namespace can access it */
    PMIX_LIST_FOREACH (cd, &tracker->local_cbs, pmix_server_caddy_t) {
        // see if we already have this nspace
        found = false;
        PMIX_LIST_FOREACH (nptr, &nslist, pmix_nspace_caddy_t) {
            if (0 == strcmp(nptr->ns->nspace, cd->peer->nptr->nspace)) {
                found = true;
                break;
            }
        }
        if (!found) {
            // add it
            nptr = PMIX_NEW(pmix_nspace_caddy_t);
            PMIX_RETAIN(cd->peer->nptr);
            nptr->ns = cd->peer->nptr;
            pmix_list_append(&nslist, &nptr->super);
        }
    }

    // Skip storing the data if we didn't collect it
    if (NULL == scd->data) {
        rc = PMIX_SUCCESS;
        goto finish_collective;
    }

    /* Store the returned blobs, once per participating nspace.
     *
     * All local clients of one nspace share a gds module, but clients of
     * *different* nspaces need not - so there is no single module that
     * can be handed the whole payload. Walk it once for each nspace,
     * through a peer of that nspace, storing only that nspace's blobs.
     * The walk is cheap relative to the collective, and a real job has
     * only a couple of local nspaces.
     *
     * Note the buffer is reloaded each pass: the walker consumes it, and
     * PMIX_LOAD_BUFFER_NON_DESTRUCT simply re-points at the payload the
     * host handed us without copying or taking ownership of it. */
    PMIX_LIST_FOREACH (nptr, &nslist, pmix_nspace_caddy_t) {
        pmix_peer_t *nspeer = NULL;

        /* find a local peer of this nspace to resolve its module */
        PMIX_LIST_FOREACH (cd, &tracker->local_cbs, pmix_server_caddy_t) {
            if (0 == strcmp(nptr->ns->nspace, cd->peer->nptr->nspace)) {
                nspeer = cd->peer;
                break;
            }
        }
        if (NULL == nspeer) {
            /* cannot happen - the nspace came from this very list */
            continue;
        }
        PMIX_LOAD_BUFFER_NON_DESTRUCT(pmix_globals.mypeer, &xfer, scd->data, scd->ndata);
        PMIX_GDS_STORE_MODEX(rc, nspeer, nptr->ns->nspace, &xfer, tracker);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            break;
        }
    }
    /* do NOT destruct the xfer buffer as that would release the payload! */

finish_collective:
    /* loop across all procs in the tracker, sending them the reply */
    PMIX_LIST_FOREACH_SAFE (cd, nxt, &tracker->local_cbs, pmix_server_caddy_t) {
        reply = PMIX_NEW(pmix_buffer_t);
        if (NULL == reply) {
            break;
        }
        /* setup the reply, starting with the returned status */
        PMIX_BFROPS_PACK(ret, cd->peer, reply, &rc, 1, PMIX_STATUS);
        if (PMIX_SUCCESS != ret) {
            PMIX_ERROR_LOG(ret);
            PMIX_RELEASE(reply);
            goto cleanup;
        }
        /* let the gds have a chance to add any data it needs
         * for providing access to any collected data */
        PMIX_GDS_MARK_MODEX_COMPLETE(rc, cd->peer, &nslist, reply);
        if (PMIX_SUCCESS != rc) {
            PMIX_RELEASE(reply);
            PMIX_ERROR_LOG(rc);
            goto cleanup;
        }
        pmix_output_verbose(2, pmix_server_globals.base_output,
                            "server:modex_cbfunc reply being sent to %s:%u",
                            cd->peer->info->pname.nspace, cd->peer->info->pname.rank);
        PMIX_SERVER_QUEUE_REPLY(ret, cd->peer, cd->hdr.tag, reply);
        if (PMIX_SUCCESS != ret) {
            PMIX_RELEASE(reply);
        }
        /* remove this entry */
        pmix_list_remove_item(&tracker->local_cbs, &cd->super);
        PMIX_RELEASE(cd);
    }

cleanup:
    /* Protect data from being free'd because RM pass
     * the pointer that is set to the middle of some
     * buffer (the case with SLURM).
     * RM is responsible on the release of the buffer
     */
    xfer.base_ptr = NULL;
    xfer.bytes_used = 0;
    PMIX_DESTRUCT(&xfer);

    pmix_list_remove_item(&pmix_server_globals.collectives, &tracker->super);
    PMIX_RELEASE(tracker);
    PMIX_LIST_DESTRUCT(&nslist);

    /* we are done */
    if (NULL != scd->cbfunc.relfn) {
        scd->cbfunc.relfn(scd->relcbdata);
    }
    PMIX_RELEASE(scd);
}

static void modex_cbfunc(pmix_status_t status, const char *data, size_t ndata, void *cbdata,
                         pmix_release_cbfunc_t relfn, void *relcbd)
{
    pmix_server_trkr_t *tracker = (pmix_server_trkr_t *) cbdata;
    pmix_shift_caddy_t *scd;

    pmix_output_verbose(2, pmix_server_globals.base_output,
                        "server:modex_cbfunc called with %d bytes", (int) ndata);

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        return;
    }

    /* need to thread-shift this callback as it accesses global data */
    scd = PMIX_NEW(pmix_shift_caddy_t);
    if (NULL == scd) {
        PMIX_ERROR_LOG(PMIX_ERR_NOMEM);
        /* nothing we can do beyond honoring the release contract - with
         * the host's own release data, not the tracker we were handed */
        if (NULL != relfn) {
            relfn(relcbd);
        }
        return;
    }
    scd->status = status;
    scd->data = data;
    scd->ndata = ndata;
    scd->tracker = tracker;
    scd->cbfunc.relfn = relfn;
    scd->relcbdata = relcbd;
    PMIX_THREADSHIFT(scd, _mdxcbfunc);
}

static void _getcbfunc(int sd, short args, void *cbdata)
{
    pmix_shift_caddy_t *scd = (pmix_shift_caddy_t *) cbdata;
    pmix_server_caddy_t *cd = (pmix_server_caddy_t *) scd->cbdata;
    pmix_buffer_t *reply, buf;
    pmix_status_t rc;
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        PMIX_RELEASE(scd);
        PMIX_RELEASE(cd);
        return;
    }

    if (NULL == cd) {
        /* nothing to do - but be sure to give them
         * a release if they want it */
        goto cleanup;
    }

    /* setup the reply, starting with the returned status */
    reply = PMIX_NEW(pmix_buffer_t);
    if (NULL == reply) {
        rc = PMIX_ERR_NOMEM;
        goto cleanup;
    }
    PMIX_BFROPS_PACK(rc, cd->peer, reply, &scd->status, 1, PMIX_STATUS);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(reply);
        goto cleanup;
    }
    /* if there are data, pack the blob being returned */
    if (NULL != scd->data) {
        PMIX_CONSTRUCT(&buf, pmix_buffer_t);
        PMIX_LOAD_BUFFER(cd->peer, &buf, scd->data, scd->ndata);
        PMIX_BFROPS_COPY_PAYLOAD(rc, cd->peer, reply, &buf);
        if (PMIX_SUCCESS != rc) {
            PMIX_RELEASE(reply);
            PMIX_DESTRUCT(&buf);
            goto cleanup;
        }
        buf.base_ptr = NULL;
        buf.bytes_used = 0;
        PMIX_DESTRUCT(&buf);
    }

    /* send the data to the requestor */
    pmix_output_verbose(2, pmix_server_globals.base_output,
                        "server:get_cbfunc reply being sent to %s:%u with status %s",
                        cd->peer->info->pname.nspace,
                        cd->peer->info->pname.rank,
                        PMIx_Error_string(scd->status));
    pmix_output_hexdump(10, pmix_server_globals.base_output, reply->base_ptr,
                        (reply->bytes_used < 256 ? reply->bytes_used : 256));

    PMIX_SERVER_QUEUE_REPLY(rc, cd->peer, cd->hdr.tag, reply);
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(reply);
    }

cleanup:
    /* if someone wants a release, give it to them */
    if (NULL != scd->cbfunc.relfn) {
        scd->cbfunc.relfn(scd->relcbdata);
    }
    if (NULL != cd) {
        PMIX_RELEASE(cd);
    }
    PMIX_RELEASE(scd);
}

static void get_cbfunc(pmix_status_t status, const char *data, size_t ndata, void *cbdata,
                       pmix_release_cbfunc_t relfn, void *relcbd)
{
    pmix_shift_caddy_t *scd;
    pmix_server_caddy_t *cd = (pmix_server_caddy_t *) cbdata;

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        PMIX_RELEASE(cd);
        return;
    }

    /* need to thread-shift this callback as it accesses global data */
    scd = PMIX_NEW(pmix_shift_caddy_t);
    if (NULL == scd) {
        /* nothing we can do beyond honoring the release contract - with
         * the host's own release data, not the caddy we were handed */
        if (NULL != relfn) {
            relfn(relcbd);
        }
        return;
    }
    scd->status = status;
    scd->data = data;
    scd->ndata = ndata;
    scd->cbdata = cbdata;
    scd->cbfunc.relfn = relfn;
    scd->relcbdata = relcbd;
    PMIX_THREADSHIFT(scd, _getcbfunc);
}

static void _cnct(int sd, short args, void *cbdata)
{
    pmix_shift_caddy_t *scd = (pmix_shift_caddy_t *) cbdata;
    pmix_server_trkr_t *tracker = scd->tracker;
    pmix_buffer_t *reply, pbkt;
    pmix_byte_object_t bo;
    pmix_status_t rc;
    int i;
    pmix_server_caddy_t *cd;
    char **nspaces = NULL;
    bool found;
    pmix_proc_t proc;
    pmix_cb_t cb;
    pmix_kval_t *kptr;
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    PMIX_ACQUIRE_OBJECT(scd);

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        PMIX_RELEASE(scd);
        return;
    }

    if (NULL == tracker) {
        /* nothing to do - but the shifter is still ours to free */
        PMIX_RELEASE(scd);
        return;
    }

    /* if we get here, then there are processes waiting
     * for a response */

    /* if the timer is active, clear it */
    if (tracker->event_active) {
        pmix_event_del(&tracker->ev);
    }

    /* find the unique nspaces that are participating */
    PMIX_LIST_FOREACH (cd, &tracker->local_cbs, pmix_server_caddy_t) {
        if (NULL == nspaces) {
            PMIx_Argv_append_nosize(&nspaces, cd->peer->info->pname.nspace);
        } else {
            found = false;
            for (i = 0; NULL != nspaces[i]; i++) {
                if (0 == strcmp(nspaces[i], cd->peer->info->pname.nspace)) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                PMIx_Argv_append_nosize(&nspaces, cd->peer->info->pname.nspace);
            }
        }
    }

    /* loop across all local procs in the tracker, sending them the reply */
    PMIX_LIST_FOREACH (cd, &tracker->local_cbs, pmix_server_caddy_t) {
        /* setup the reply, starting with the returned status */
        reply = PMIX_NEW(pmix_buffer_t);
        if (NULL == reply) {
            PMIX_ERROR_LOG(PMIX_ERR_NOMEM);
            rc = PMIX_ERR_NOMEM;
            goto cleanup;
        }
        /* start with the status */
        PMIX_BFROPS_PACK(rc, cd->peer, reply, &scd->status, 1, PMIX_STATUS);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_RELEASE(reply);
            goto cleanup;
        }
        if (PMIX_SUCCESS == scd->status) {
            /* loop across all participating nspaces and include their
             * job-related info */
            for (i = 0; NULL != nspaces[i]; i++) {
                /* if this is the local proc's own nspace, then
                 * ignore it - it already has this info */
                if (0 == strncmp(nspaces[i], cd->peer->info->pname.nspace, PMIX_MAX_NSLEN)) {
                    continue;
                }

                /* this is a local request, so give the gds the option
                 * of returning a copy of the data, or a pointer to
                 * local storage */
                /* add the job-level info, if necessary */
                PMIX_LOAD_PROCID(&proc, nspaces[i], PMIX_RANK_WILDCARD);
                PMIX_CONSTRUCT(&cb, pmix_cb_t);
                /* this is for a local client, so give the gds the
                 * option of returning a complete copy of the data,
                 * or returning a pointer to local storage */
                cb.proc = &proc;
                cb.scope = PMIX_SCOPE_UNDEF;
                cb.copy = false;
                PMIX_GDS_FETCH_KV(rc, cd->peer, &cb);
                if (PMIX_SUCCESS != rc) {
                    /* try getting it from our storage */
                    PMIX_GDS_FETCH_KV(rc, pmix_globals.mypeer, &cb);
                    if (PMIX_SUCCESS != rc) {
                        PMIX_ERROR_LOG(rc);
                        PMIX_RELEASE(reply);
                        PMIX_DESTRUCT(&cb);
                        goto error;
                    }
                }
                PMIX_CONSTRUCT(&pbkt, pmix_buffer_t);
                /* pack the nspace name */
                PMIX_BFROPS_PACK(rc, cd->peer, &pbkt, &nspaces[i], 1, PMIX_STRING);
                if (PMIX_SUCCESS != rc) {
                    PMIX_ERROR_LOG(rc);
                    PMIX_RELEASE(reply);
                    PMIX_DESTRUCT(&pbkt);
                    PMIX_DESTRUCT(&cb);
                    goto error;
                }
                PMIX_LIST_FOREACH (kptr, &cb.kvs, pmix_kval_t) {
                    PMIX_BFROPS_PACK(rc, cd->peer, &pbkt, kptr, 1, PMIX_KVAL);
                    if (PMIX_SUCCESS != rc) {
                        PMIX_ERROR_LOG(rc);
                        PMIX_RELEASE(reply);
                        PMIX_DESTRUCT(&pbkt);
                        PMIX_DESTRUCT(&cb);
                        goto error;
                    }
                }
                PMIX_DESTRUCT(&cb);

                if (PMIX_PEER_IS_V1(cd->peer) || PMIX_PEER_IS_V20(cd->peer)) {
                    PMIX_BFROPS_PACK(rc, cd->peer, reply, &pbkt, 1, PMIX_BUFFER);
                    if (PMIX_SUCCESS != rc) {
                        PMIX_ERROR_LOG(rc);
                        PMIX_RELEASE(reply);
                        PMIX_DESTRUCT(&pbkt);
                        goto error;
                    }
                } else {
                    PMIX_UNLOAD_BUFFER(&pbkt, bo.bytes, bo.size);
                    PMIX_BFROPS_PACK(rc, cd->peer, reply, &bo, 1, PMIX_BYTE_OBJECT);
                    PMIX_BYTE_OBJECT_DESTRUCT(&bo); // data has been copied
                    if (PMIX_SUCCESS != rc) {
                        PMIX_ERROR_LOG(rc);
                        PMIX_RELEASE(reply);
                        PMIX_DESTRUCT(&pbkt);
                        goto error;
                    }
                }

                PMIX_DESTRUCT(&pbkt);
            }
        }
        pmix_output_verbose(2, pmix_server_globals.connect_output,
                            "server:cnct_cbfunc reply being sent to %s",
                            PMIX_PEER_PRINT(cd->peer));
        PMIX_SERVER_QUEUE_REPLY(rc, cd->peer, cd->hdr.tag, reply);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_RELEASE(reply);
        }
    }
    goto cleanup;

error:
    reply = PMIX_NEW(pmix_buffer_t);
    if (NULL == reply) {
        PMIX_ERROR_LOG(PMIX_ERR_NOMEM);
        rc = PMIX_ERR_NOMEM;
        goto cleanup;
    }
    /* return an error status so they don't hang */
    scd->status = rc;
    PMIX_BFROPS_PACK(rc, cd->peer, reply, &scd->status, 1, PMIX_STATUS);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(reply);
        goto cleanup;
    }
    PMIX_SERVER_QUEUE_REPLY(rc, cd->peer, cd->hdr.tag, reply);
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(reply);
    }

cleanup:
    if (NULL != nspaces) {
        PMIx_Argv_free(nspaces);
    }
    pmix_list_remove_item(&pmix_server_globals.collectives, &tracker->super);
    PMIX_RELEASE(tracker);

    /* we are done */
    PMIX_RELEASE(scd);
}

static void cnct_cbfunc(pmix_status_t status, void *cbdata)
{
    pmix_server_trkr_t *tracker = (pmix_server_trkr_t *) cbdata;
    pmix_shift_caddy_t *scd;

    pmix_output_verbose(2, pmix_server_globals.connect_output,
                        "server:cnct_cbfunc called");

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        return;
    }

    /* need to thread-shift this callback as it accesses global data */
    scd = PMIX_NEW(pmix_shift_caddy_t);
    if (NULL == scd) {
        /* nothing we can do */
        return;
    }
    scd->status = status;
    scd->tracker = tracker;
    PMIX_THREADSHIFT(scd, _cnct);
}

static void _discnct(int sd, short args, void *cbdata)
{
    pmix_shift_caddy_t *scd = (pmix_shift_caddy_t *) cbdata;
    pmix_server_trkr_t *tracker = scd->tracker;
    pmix_buffer_t *reply;
    pmix_status_t rc;
    pmix_server_caddy_t *cd;
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    PMIX_ACQUIRE_OBJECT(scd);

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        PMIX_RELEASE(scd);
        return;
    }

    if (NULL == tracker) {
        /* nothing to do - but the shifter is still ours to free */
        PMIX_RELEASE(scd);
        return;
    }

    /* if we get here, then there are processes waiting
     * for a response */

    /* if the timer is active, clear it */
    if (tracker->event_active) {
        pmix_event_del(&tracker->ev);
    }

    /* loop across all local procs in the tracker, sending them the reply */
    PMIX_LIST_FOREACH (cd, &tracker->local_cbs, pmix_server_caddy_t) {
        /* setup the reply */
        reply = PMIX_NEW(pmix_buffer_t);
        if (NULL == reply) {
            PMIX_ERROR_LOG(PMIX_ERR_NOMEM);
            rc = PMIX_ERR_NOMEM;
            goto cleanup;
        }
        /* return the status */
        PMIX_BFROPS_PACK(rc, cd->peer, reply, &scd->status, 1, PMIX_STATUS);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_RELEASE(reply);
            goto cleanup;
        }
        pmix_output_verbose(2, pmix_server_globals.connect_output,
                            "server:cnct_cbfunc reply being sent to %s:%u",
                            cd->peer->info->pname.nspace, cd->peer->info->pname.rank);
        PMIX_SERVER_QUEUE_REPLY(rc, cd->peer, cd->hdr.tag, reply);
        if (PMIX_SUCCESS != rc) {
            PMIX_RELEASE(reply);
        }
    }

cleanup:
    /* cleanup the tracker -- the host RM is responsible for
     * telling us when to remove the nspace from our data */
    pmix_list_remove_item(&pmix_server_globals.collectives, &tracker->super);
    PMIX_RELEASE(tracker);

    /* we are done */
    PMIX_RELEASE(scd);
}

static void discnct_cbfunc(pmix_status_t status, void *cbdata)
{
    pmix_server_trkr_t *tracker = (pmix_server_trkr_t *) cbdata;
    pmix_shift_caddy_t *scd;

    pmix_output_verbose(2, pmix_server_globals.base_output,
                        "server:discnct_cbfunc called on nspace %s",
                        (NULL == tracker) ? "NULL" : tracker->pname.nspace);

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        return;
    }

    /* need to thread-shift this callback as it accesses global data */
    scd = PMIX_NEW(pmix_shift_caddy_t);
    if (NULL == scd) {
        /* nothing we can do */
        return;
    }
    scd->status = status;
    scd->tracker = tracker;
    PMIX_THREADSHIFT(scd, _discnct);
}

static void _evcbfunc(int sd, short args, void *cbdata)
{
    pmix_shift_caddy_t *scd = (pmix_shift_caddy_t*)cbdata;
    pmix_server_caddy_t *cd = (pmix_server_caddy_t *)scd->cbdata;
    pmix_buffer_t *reply;
    pmix_status_t rc;
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        PMIX_RELEASE(cd);
        PMIX_RELEASE(scd);
        return;
    }

    pmix_output_verbose(2, pmix_server_globals.base_output,
                        "server:events_cbfunc called status = %s",
                        PMIx_Error_string(scd->status));

    reply = PMIX_NEW(pmix_buffer_t);
    if (NULL == reply) {
        PMIX_ERROR_LOG(PMIX_ERR_NOMEM);
        goto cleanup;
    }
    PMIX_BFROPS_PACK(rc, cd->peer, reply, &scd->status, 1, PMIX_STATUS);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
    }
    // send reply
    PMIX_SERVER_QUEUE_REPLY(rc, cd->peer, cd->hdr.tag, reply);
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(reply);
    }

cleanup:
    PMIX_RELEASE(cd);
    PMIX_RELEASE(scd);
}

static void events_cbfunc(pmix_status_t status, void *cbdata)
{
    pmix_shift_caddy_t *scd;
    pmix_server_caddy_t *cd = (pmix_server_caddy_t *)cbdata;

    pmix_output_verbose(2, pmix_server_globals.base_output,
                        "server:events_cbfunc called");

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        PMIX_RELEASE(cd);
        return;
    }

    /* need to thread-shift this callback as it accesses global data */
    scd = PMIX_NEW(pmix_shift_caddy_t);
    if (NULL == scd) {
        /* nothing we can do */
        return;
    }
    scd->status = status;
    scd->cbdata = cbdata;
    PMIX_THREADSHIFT(scd, _evcbfunc);
}

static void _alloccbfunc(int sd, short args, void *cbdata)
{
    pmix_shift_caddy_t *scd = (pmix_shift_caddy_t*)cbdata;
    pmix_query_caddy_t *qcd = (pmix_query_caddy_t *)scd->cbdata;
    pmix_server_caddy_t *cd = (pmix_server_caddy_t *) qcd->cbdata;
    pmix_buffer_t *reply;
    pmix_status_t rc;
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        if (NULL != qcd->queries) {
            PMIX_QUERY_FREE(qcd->queries, qcd->nqueries);
        }
        if (NULL != qcd->info) {
            PMIX_INFO_FREE(qcd->info, qcd->ninfo);
        }
        PMIX_RELEASE(cd);
        PMIX_RELEASE(qcd);
        PMIX_RELEASE(scd);
        return;
     }

    pmix_output_verbose(2, pmix_server_globals.base_output,
                        "pmix:alloc callback with status %s",
                        PMIx_Error_string(scd->status));

    reply = PMIX_NEW(pmix_buffer_t);
    if (NULL == reply) {
        PMIX_ERROR_LOG(PMIX_ERR_NOMEM);
        goto cleanup;
    }
    PMIX_BFROPS_PACK(rc, cd->peer, reply, &scd->status, 1, PMIX_STATUS);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto complete;
    }
    /* pack the returned data */
    PMIX_BFROPS_PACK(rc, cd->peer, reply, &scd->ninfo, 1, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto complete;
    }
    if (0 < scd->ninfo) {
        PMIX_BFROPS_PACK(rc, cd->peer, reply, scd->info, scd->ninfo, PMIX_INFO);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
        }
    }

complete:
    // send reply
    PMIX_SERVER_QUEUE_REPLY(rc, cd->peer, cd->hdr.tag, reply);
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(reply);
    }


cleanup:
    // cleanup
    if (NULL != qcd->queries) {
        PMIX_QUERY_FREE(qcd->queries, qcd->nqueries);
    }
    if (NULL != qcd->info) {
        PMIX_INFO_FREE(qcd->info, qcd->ninfo);
    }

    if (NULL != scd->cbfunc.relfn) {
        scd->cbfunc.relfn(scd->relcbdata);
    }
    PMIX_RELEASE(cd);
    PMIX_RELEASE(qcd);
    PMIX_RELEASE(scd);
}

static void alloc_cbfunc(pmix_status_t status, pmix_info_t *info, size_t ninfo, void *cbdata,
                         pmix_release_cbfunc_t release_fn, void *release_cbdata)
{
    pmix_shift_caddy_t *scd;
    pmix_query_caddy_t *qcd = (pmix_query_caddy_t *) cbdata;
    pmix_server_caddy_t *cd = (pmix_server_caddy_t *) qcd->cbdata;

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        if (NULL != qcd->queries) {
            PMIX_QUERY_FREE(qcd->queries, qcd->nqueries);
        }
        if (NULL != qcd->info) {
            PMIX_INFO_FREE(qcd->info, qcd->ninfo);
        }
        PMIX_RELEASE(cd);
        PMIX_RELEASE(qcd);
        return;
    }

    pmix_output_verbose(2, pmix_server_globals.base_output,
                        "server:alloc_cbfunc called");

    /* need to thread-shift this callback as it accesses global data */
    scd = PMIX_NEW(pmix_shift_caddy_t);
    if (NULL == scd) {
        /* nothing we can do beyond honoring the release contract -
         * the host handed us its data along with the function that
         * gives it back, and dropping both here strands it for the
         * life of the host process */
        if (NULL != release_fn) {
            release_fn(release_cbdata);
        }
        return;
    }
    scd->status = status;
    scd->info = info;
    scd->ninfo = ninfo;
    scd->cbdata = cbdata;
    scd->cbfunc.relfn = release_fn;
    scd->relcbdata = release_cbdata;
    PMIX_THREADSHIFT(scd, _alloccbfunc);
}

static void _qrycbfunc(int sd, short args, void *cbdata)
{
    pmix_shift_caddy_t *scdwrapper = (pmix_shift_caddy_t*)cbdata;
    pmix_server_caddy_t *scd = (pmix_server_caddy_t*)scdwrapper->cbdata;
    pmix_buffer_t *reply;
    pmix_status_t rc;
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        PMIX_RELEASE(scd);
        PMIX_RELEASE(scdwrapper);
        return;
    }

    pmix_output_verbose(2, pmix_server_globals.base_output,
                        "pmix:query callback with status %s",
                        PMIx_Error_string(scdwrapper->status));

    reply = PMIX_NEW(pmix_buffer_t);
    if (NULL == reply) {
        PMIX_ERROR_LOG(PMIX_ERR_NOMEM);
        goto cleanup;
    }
    PMIX_BFROPS_PACK(rc, scd->peer, reply, &scdwrapper->status, 1, PMIX_STATUS);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto complete;
    }
    /* pack the returned data */
    PMIX_BFROPS_PACK(rc, scd->peer, reply, &scdwrapper->ninfo, 1, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto complete;
    }
    if (0 < scdwrapper->ninfo) {
        PMIX_BFROPS_PACK(rc, scd->peer, reply, scdwrapper->info, scdwrapper->ninfo, PMIX_INFO);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
        }
    }

    /* cache the data for any future requests */

complete:
    // send reply
    PMIX_SERVER_QUEUE_REPLY(rc, scd->peer, scd->hdr.tag, reply);
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(reply);
    }

cleanup:
    // cleanup
    if (NULL != scdwrapper->cbfunc.relfn) {
        scdwrapper->cbfunc.relfn(scdwrapper->relcbdata);
    }
    PMIX_RELEASE(scd);
    PMIX_RELEASE(scdwrapper);
}

static void query_cbfunc(pmix_status_t status, pmix_info_t *info, size_t ninfo, void *cbdata,
                         pmix_release_cbfunc_t release_fn, void *release_cbdata)
{
    pmix_shift_caddy_t *scd;
    pmix_server_caddy_t *cd = (pmix_server_caddy_t*)cbdata;

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        PMIX_RELEASE(cd);
        return;
    }

    pmix_output_verbose(2, pmix_server_globals.base_output,
                        "server:query_cbfunc called");

    /* need to thread-shift this callback as it accesses global data */
    scd = PMIX_NEW(pmix_shift_caddy_t);
    if (NULL == scd) {
        /* nothing we can do beyond honoring the release contract -
         * the host handed us its data along with the function that
         * gives it back, and dropping both here strands it for the
         * life of the host process */
        if (NULL != release_fn) {
            release_fn(release_cbdata);
        }
        return;
    }
    scd->status = status;
    scd->info = info;
    scd->ninfo = ninfo;
    scd->cbdata = cbdata;
    scd->cbfunc.relfn = release_fn;
    scd->relcbdata = release_cbdata;
    PMIX_THREADSHIFT(scd, _qrycbfunc);
}

static void _sctrl_cbfunc(int sd, short args, void *cbdata)
{
    pmix_shift_caddy_t *scdwrapper = (pmix_shift_caddy_t*)cbdata;
    pmix_shift_caddy_t *scd = (pmix_shift_caddy_t *)scdwrapper->cbdata;
    pmix_server_caddy_t *cd = (pmix_server_caddy_t *)scd->cbdata;
    pmix_buffer_t *reply;
    pmix_status_t rc;
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        PMIX_RELEASE(cd);
        PMIX_RELEASE(scd);
        PMIX_RELEASE(scdwrapper);
        return;
    }

    pmix_output_verbose(2, pmix_server_globals.base_output,
                        "pmix:session_ctrl callback with status %s",
                        PMIx_Error_string(scdwrapper->status));

    reply = PMIX_NEW(pmix_buffer_t);
    if (NULL == reply) {
        PMIX_ERROR_LOG(PMIX_ERR_NOMEM);
        goto cleanup;
    }
    PMIX_BFROPS_PACK(rc, cd->peer, reply, &scdwrapper->status, 1, PMIX_STATUS);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto complete;
    }
    /* pack the returned data */
    PMIX_BFROPS_PACK(rc, cd->peer, reply, &scdwrapper->ninfo, 1, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto complete;
    }
    if (0 < scdwrapper->ninfo) {
        PMIX_BFROPS_PACK(rc, cd->peer, reply, scdwrapper->info, scdwrapper->ninfo, PMIX_INFO);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
        }
    }

complete:
    // send reply
    PMIX_SERVER_QUEUE_REPLY(rc, cd->peer, cd->hdr.tag, reply);
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(reply);
    }

cleanup:
    if (NULL != scd->info) {
        PMIX_INFO_FREE(scd->info, scd->ninfo);
    }
    if (NULL != scdwrapper->cbfunc.relfn) {
        scdwrapper->cbfunc.relfn(scdwrapper->relcbdata);
    }
    PMIX_RELEASE(scd);
    PMIX_RELEASE(cd);
    PMIX_RELEASE(scdwrapper);
}

static void sessctrl_cbfunc(pmix_status_t status, pmix_info_t *info, size_t ninfo, void *cbdata,
                            pmix_release_cbfunc_t release_fn, void *release_cbdata)
{
    pmix_shift_caddy_t *scdwrapper;
    pmix_shift_caddy_t *scd = (pmix_shift_caddy_t *)cbdata;
    pmix_server_caddy_t *cd = (pmix_server_caddy_t *)scd->cbdata;

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        PMIX_RELEASE(cd);
        PMIX_RELEASE(scd);
        return;
    }

    pmix_output_verbose(2, pmix_server_globals.base_output,
                        "server:sessctrl_cbfunc called");

    /* need to thread-shift this callback as it accesses global data */
    scdwrapper = PMIX_NEW(pmix_shift_caddy_t);
    if (NULL == scdwrapper) {
        /* nothing we can do beyond honoring the release contract -
         * the host handed us its data along with the function that
         * gives it back, and dropping both here strands it for the
         * life of the host process */
        if (NULL != release_fn) {
            release_fn(release_cbdata);
        }
        return;
    }
    scdwrapper->status = status;
    scdwrapper->info = info;
    scdwrapper->ninfo = ninfo;
    scdwrapper->cbdata = cbdata;
    scdwrapper->cbfunc.relfn = release_fn;
    scdwrapper->relcbdata = release_cbdata;
    PMIX_THREADSHIFT(scdwrapper, _sctrl_cbfunc);
}

static void _jctrl_cbfunc(int sd, short args, void *cbdata)
{
    pmix_shift_caddy_t *scd = (pmix_shift_caddy_t*)cbdata;
    pmix_query_caddy_t *qcd = (pmix_query_caddy_t *) scd->cbdata;
    pmix_server_caddy_t *cd = (pmix_server_caddy_t *) qcd->cbdata;
    pmix_buffer_t *reply;
    pmix_status_t rc;
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        PMIX_RELEASE(cd);
        PMIX_RELEASE(qcd);
        PMIX_RELEASE(scd);
        return;
    }

    pmix_output_verbose(2, pmix_server_globals.base_output,
                        "pmix:_jctrl_cbfunc callback with status %s",
                        PMIx_Error_string(scd->status));

    reply = PMIX_NEW(pmix_buffer_t);
    if (NULL == reply) {
        PMIX_ERROR_LOG(PMIX_ERR_NOMEM);
        goto cleanup;
    }
    PMIX_BFROPS_PACK(rc, cd->peer, reply, &scd->status, 1, PMIX_STATUS);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto complete;
    }
    /* pack the returned data */
    PMIX_BFROPS_PACK(rc, cd->peer, reply, &scd->ninfo, 1, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto complete;
    }
    if (0 < scd->ninfo) {
        PMIX_BFROPS_PACK(rc, cd->peer, reply, scd->info, scd->ninfo, PMIX_INFO);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
        }
    }

complete:
    // send reply
    PMIX_SERVER_QUEUE_REPLY(rc, cd->peer, cd->hdr.tag, reply);
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(reply);
    }

cleanup:
    if (NULL != qcd->queries) {
        PMIX_QUERY_FREE(qcd->queries, qcd->nqueries);
    }
    if (NULL != qcd->info) {
        PMIX_INFO_FREE(qcd->info, qcd->ninfo);
    }
    if (NULL != scd->cbfunc.relfn) {
        scd->cbfunc.relfn(scd->relcbdata);
    }

    PMIX_RELEASE(qcd);
    PMIX_RELEASE(cd);
    PMIX_RELEASE(scd);
}

static void jctrl_cbfunc(pmix_status_t status, pmix_info_t *info, size_t ninfo, void *cbdata,
                         pmix_release_cbfunc_t release_fn, void *release_cbdata)
{
    pmix_shift_caddy_t *scd;
    pmix_query_caddy_t *qcd = (pmix_query_caddy_t *) cbdata;
    pmix_server_caddy_t *cd = (pmix_server_caddy_t *) qcd->cbdata;

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        PMIX_RELEASE(cd);
        PMIX_RELEASE(qcd);
        return;
    }

    pmix_output_verbose(2, pmix_server_globals.base_output,
                        "server:jctrl_cbfunc called");

    /* need to thread-shift this callback as it accesses global data */
    scd = PMIX_NEW(pmix_shift_caddy_t);
    if (NULL == scd) {
        /* nothing we can do beyond honoring the release contract -
         * the host handed us its data along with the function that
         * gives it back, and dropping both here strands it for the
         * life of the host process */
        if (NULL != release_fn) {
            release_fn(release_cbdata);
        }
        return;
    }
    scd->status = status;
    scd->info = info;
    scd->ninfo = ninfo;
    scd->cbdata = cbdata;
    scd->cbfunc.relfn = release_fn;
    scd->relcbdata = release_cbdata;
    PMIX_THREADSHIFT(scd, _jctrl_cbfunc);
}

static void _mon_cbfunc(int sd, short args, void *cbdata)
{
    pmix_shift_caddy_t *scd = (pmix_shift_caddy_t*)cbdata;
    pmix_server_caddy_t *cd = (pmix_server_caddy_t *) scd->cbdata;
    pmix_buffer_t *reply;
    pmix_status_t rc;
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        PMIX_RELEASE(cd);
        PMIX_RELEASE(scd);
        return;
    }

    pmix_output_verbose(2, pmix_server_globals.base_output,
                        "pmix:_mon_cbfunc callback with status %s",
                        PMIx_Error_string(scd->status));

    reply = PMIX_NEW(pmix_buffer_t);
    if (NULL == reply) {
        PMIX_ERROR_LOG(PMIX_ERR_NOMEM);
        goto cleanup;
    }
    PMIX_BFROPS_PACK(rc, cd->peer, reply, &scd->status, 1, PMIX_STATUS);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto complete;
    }
    /* pack the returned data */
    PMIX_BFROPS_PACK(rc, cd->peer, reply, &scd->ninfo, 1, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto complete;
    }
    if (0 < scd->ninfo) {
        PMIX_BFROPS_PACK(rc, cd->peer, reply, scd->info, scd->ninfo, PMIX_INFO);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
        }
    }

complete:
    // send reply
    PMIX_SERVER_QUEUE_REPLY(rc, cd->peer, cd->hdr.tag, reply);
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(reply);
    }

cleanup:
    PMIX_RELEASE(cd);
    if (NULL != scd->cbfunc.relfn) {
        scd->cbfunc.relfn(scd->relcbdata);
    }
    PMIX_RELEASE(scd);
}

static void monitor_cbfunc(pmix_status_t status, pmix_info_t *info, size_t ninfo, void *cbdata,
                           pmix_release_cbfunc_t release_fn, void *release_cbdata)
{
    pmix_shift_caddy_t *scd;
    pmix_server_caddy_t *cd = (pmix_server_caddy_t *) cbdata;

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        PMIX_RELEASE(cd);
        return;
    }

    pmix_output_verbose(2, pmix_server_globals.base_output,
                        "server:monitor_cbfunc called");

    /* need to thread-shift this callback as it accesses global data */
    scd = PMIX_NEW(pmix_shift_caddy_t);
    if (NULL == scd) {
        /* nothing we can do beyond honoring the release contract -
         * the host handed us its data along with the function that
         * gives it back, and dropping both here strands it for the
         * life of the host process */
        if (NULL != release_fn) {
            release_fn(release_cbdata);
        }
        return;
    }
    scd->status = status;
    scd->info = info;
    scd->ninfo = ninfo;
    scd->cbdata = cbdata;
    scd->cbfunc.relfn = release_fn;
    scd->relcbdata = release_cbdata;
    PMIX_THREADSHIFT(scd, _mon_cbfunc);
}

static void _cred_cbfunc(int sd, short args, void *cbdata)
{
    pmix_shift_caddy_t *scd = (pmix_shift_caddy_t*)cbdata;
    pmix_query_caddy_t *qcd = (pmix_query_caddy_t *) scd->cbdata;
    pmix_server_caddy_t *cd = (pmix_server_caddy_t *) qcd->cbdata;
    pmix_buffer_t *reply;
    pmix_status_t rc;
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        PMIX_RELEASE(cd);
        PMIX_RELEASE(qcd);
        PMIX_RELEASE(scd);
        return;
    }

    pmix_output_verbose(2, pmix_globals.debug_output,
                        "pmix:get credential callback with status %s",
                        PMIx_Error_string(scd->status));

    reply = PMIX_NEW(pmix_buffer_t);
    if (NULL == reply) {
        PMIX_ERROR_LOG(PMIX_ERR_NOMEM);
        goto cleanup;
    }

    /* pack the status */
    PMIX_BFROPS_PACK(rc, cd->peer, reply, &scd->status, 1, PMIX_STATUS);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto complete;
    }

    if (PMIX_SUCCESS == scd->status) {
        /* pack the returned credential */
        PMIX_BFROPS_PACK(rc, cd->peer, reply, scd->bo, 1, PMIX_BYTE_OBJECT);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            goto complete;
        }

        /* pack any returned data */
        PMIX_BFROPS_PACK(rc, cd->peer, reply, &scd->ninfo, 1, PMIX_SIZE);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            goto complete;
        }
        if (0 < scd->ninfo) {
            PMIX_BFROPS_PACK(rc, cd->peer, reply, scd->info, scd->ninfo, PMIX_INFO);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
            }
        }
    }

complete:
    // send reply
    PMIX_SERVER_QUEUE_REPLY(rc, cd->peer, cd->hdr.tag, reply);
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(reply);
    }

cleanup:
    if (NULL != qcd->info) {
        PMIX_INFO_FREE(qcd->info, qcd->ninfo);
    }
    if (NULL != scd->bo) {
        PMIX_BYTE_OBJECT_FREE(scd->bo, 1);
    }
    PMIX_RELEASE(qcd);
    PMIX_RELEASE(cd);
    PMIX_RELEASE(scd);
}

static void cred_cbfunc(pmix_status_t status, pmix_byte_object_t *credential,
                        pmix_info_t info[], size_t ninfo, void *cbdata)
{
    pmix_shift_caddy_t *scd;
    pmix_query_caddy_t *qcd = (pmix_query_caddy_t *) cbdata;
    pmix_server_caddy_t *cd = (pmix_server_caddy_t *) qcd->cbdata;
    pmix_status_t rc;

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        PMIX_RELEASE(cd);
        PMIX_RELEASE(qcd);
        return;
    }

    pmix_output_verbose(2, pmix_server_globals.base_output,
                        "server:cred_cbfunc called");

    /* need to thread-shift this callback as it accesses global data */
    scd = PMIX_NEW(pmix_shift_caddy_t);
    if (NULL == scd) {
        /* nothing we can do */
        return;
    }
    scd->status = status;
    scd->info = info;
    scd->ninfo = ninfo;
    // need to copy this as they may not hold it for us
    PMIX_BFROPS_COPY(rc, cd->peer, (void**)&scd->bo, credential, PMIX_BYTE_OBJECT);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(cd);
        PMIX_RELEASE(qcd);
        PMIX_RELEASE(scd);
        return;
    }
    scd->cbdata = cbdata;
    PMIX_THREADSHIFT(scd, _cred_cbfunc);
}

static void _valcbfunc(int sd, short args, void *cbdata)
{
    pmix_shift_caddy_t *scd = (pmix_shift_caddy_t*)cbdata;
    pmix_query_caddy_t *qcd = (pmix_query_caddy_t *) scd->cbdata;
    pmix_server_caddy_t *cd = (pmix_server_caddy_t *) qcd->cbdata;
    pmix_buffer_t *reply;
    pmix_status_t rc;
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        PMIX_RELEASE(cd);
        PMIX_RELEASE(qcd);
        PMIX_RELEASE(scd);
        return;
    }

    pmix_output_verbose(2, pmix_globals.debug_output,
                        "pmix:validate credential callback with status %s",
                        PMIx_Error_string(scd->status));

    reply = PMIX_NEW(pmix_buffer_t);
    if (NULL == reply) {
        PMIX_ERROR_LOG(PMIX_ERR_NOMEM);
        goto cleanup;
    }
    PMIX_BFROPS_PACK(rc, cd->peer, reply, &scd->status, 1, PMIX_STATUS);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto complete;
    }
    /* pack any returned data */
    PMIX_BFROPS_PACK(rc, cd->peer, reply, &scd->ninfo, 1, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto complete;
    }
    if (0 < scd->ninfo) {
        PMIX_BFROPS_PACK(rc, cd->peer, reply, scd->info, scd->ninfo, PMIX_INFO);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
        }
    }

complete:
    // send reply
    PMIX_SERVER_QUEUE_REPLY(rc, cd->peer, cd->hdr.tag, reply);
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(reply);
    }

cleanup:
    if (NULL != qcd->info) {
        PMIX_INFO_FREE(qcd->info, qcd->ninfo);
    }
    if (NULL != scd->info) {
        PMIX_INFO_FREE(scd->info, scd->ninfo);
    }
    PMIX_RELEASE(qcd);
    PMIX_RELEASE(cd);
    PMIX_RELEASE(scd);
}

static void validate_cbfunc(pmix_status_t status, pmix_info_t info[], size_t ninfo, void *cbdata)
{
    pmix_shift_caddy_t *scd;
    pmix_query_caddy_t *qcd = (pmix_query_caddy_t *) cbdata;
    pmix_server_caddy_t *cd = (pmix_server_caddy_t *) qcd->cbdata;
    pmix_status_t rc;
    size_t n;

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        PMIX_RELEASE(cd);
        PMIX_RELEASE(qcd);
        return;
    }

    pmix_output_verbose(2, pmix_server_globals.base_output,
                        "server:validate_cbfunc called");

    /* need to thread-shift this callback as it accesses global data */
    scd = PMIX_NEW(pmix_shift_caddy_t);
    if (NULL == scd) {
        /* nothing we can do */
        return;
    }
    scd->status = status;
    // need to copy the info as they may not hold it for us
    if (NULL != info) {
        scd->ninfo = ninfo;
        PMIX_INFO_CREATE(scd->info, scd->ninfo);
        for (n=0; n < scd->ninfo; n++) {
            rc = PMIx_Info_xfer(&scd->info[n], &info[n]);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_INFO_FREE(scd->info, scd->ninfo);
                PMIX_RELEASE(scd);
                return;
            }
        }
    }
    scd->cbdata = cbdata;
    PMIX_THREADSHIFT(scd, _valcbfunc);
}

static void _iofreg(int sd, short args, void *cbdata)
{
    pmix_setup_caddy_t *cd = (pmix_setup_caddy_t *) cbdata;
    pmix_server_caddy_t *scd = (pmix_server_caddy_t *) cd->cbdata;
    pmix_buffer_t *reply;
    pmix_status_t rc;
    pmix_iof_req_t *req;
    pmix_iof_cache_t *iof, *inxt;
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    PMIX_ACQUIRE_OBJECT(cd);

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        PMIX_RELEASE(scd);
        PMIX_RELEASE(cd);
        return;
    }

    /* setup the reply to the requestor */
    reply = PMIX_NEW(pmix_buffer_t);
    if (NULL == reply) {
        PMIX_ERROR_LOG(PMIX_ERR_NOMEM);
        rc = PMIX_ERR_NOMEM;
        goto cleanup;
    }
    /* start with the status */
    PMIX_BFROPS_PACK(rc, scd->peer, reply, &cd->status, 1, PMIX_STATUS);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(reply);
        goto cleanup;
    }

    /* was the request a success? */
    if (PMIX_SUCCESS != cd->status) {
        /* find and remove the tracker */
        req = (pmix_iof_req_t *) pmix_pointer_array_get_item(&pmix_globals.iof_requests,
                                                             cd->ncodes);
        if (NULL != req) {
            PMIX_RELEASE(req);
        }
        pmix_pointer_array_set_item(&pmix_globals.iof_requests, cd->ncodes, NULL);
    } else {
        /* return our reference ID for this handler */
        PMIX_BFROPS_PACK(rc, scd->peer, reply, &cd->ncodes, 1, PMIX_SIZE);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_RELEASE(reply);
            goto cleanup;
        }
    }

    pmix_output_verbose(2, pmix_server_globals.iof_output,
                        "server:_iofreg reply being sent to %s:%u", scd->peer->info->pname.nspace,
                        scd->peer->info->pname.rank);
    PMIX_SERVER_QUEUE_REPLY(rc, scd->peer, scd->hdr.tag, reply);
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(reply);
    }

    /* if the request succeeded, then process any cached IO - doing it here
     * guarantees that the IO will be received AFTER the client gets the
     * refid response */
    if (PMIX_SUCCESS == cd->status) {
        /* get the request */
        req = (pmix_iof_req_t *) pmix_pointer_array_get_item(&pmix_globals.iof_requests,
                                                             cd->ncodes);
        if (NULL != req) {
            PMIX_LIST_FOREACH_SAFE (iof, inxt, &pmix_server_globals.iof, pmix_iof_cache_t) {
                rc = pmix_iof_process_iof(iof->channel, &iof->source, iof->bo, iof->info,
                                            iof->ninfo, req);
                if (PMIX_OPERATION_SUCCEEDED == rc) {
                    pmix_list_remove_item(&pmix_server_globals.iof, &iof->super);
                    PMIX_RELEASE(iof);
                }
            }
        }
    }

cleanup:
    /* release the cached info */
    if (NULL != cd->procs) {
        PMIX_PROC_FREE(cd->procs, cd->nprocs);
    }
    PMIX_INFO_FREE(cd->info, cd->ninfo);
    /* we are done */
    PMIX_RELEASE(cd);
}

static void iof_cbfunc(pmix_status_t status, void *cbdata)
{
    pmix_setup_caddy_t *cd = (pmix_setup_caddy_t *) cbdata;

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        PMIX_RELEASE(cd);
        return;
    }
    pmix_output_verbose(2, pmix_server_globals.iof_output,
                        "server:iof_cbfunc called with status %d", status);

    if (NULL == cd) {
        /* nothing to do */
        return;
    }
    cd->status = status;

    /* need to thread-shift this callback as it accesses global data */
    PMIX_THREADSHIFT(cd, _iofreg);
}

static void _iofdreg(int sd, short args, void *cbdata)
{
    pmix_shift_caddy_t *scdwrapper = (pmix_shift_caddy_t*)cbdata;
    pmix_setup_caddy_t *cd = (pmix_setup_caddy_t *) scdwrapper->cbdata;
    pmix_server_caddy_t *scd = (pmix_server_caddy_t *) cd->cbdata;
    pmix_buffer_t *reply;
    pmix_status_t rc;
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    PMIX_ACQUIRE_OBJECT(cd);

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        PMIX_RELEASE(scd);
        PMIX_RELEASE(cd);
        PMIX_RELEASE(scdwrapper);
        return;
    }

    /* setup the reply to the requestor */
    reply = PMIX_NEW(pmix_buffer_t);
    if (NULL == reply) {
        PMIX_ERROR_LOG(PMIX_ERR_NOMEM);
        rc = PMIX_ERR_NOMEM;
        goto cleanup;
    }
    /* its just the status */
    PMIX_BFROPS_PACK(rc, scd->peer, reply, &scdwrapper->status, 1, PMIX_STATUS);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(reply);
        goto cleanup;
    }

    pmix_output_verbose(2, pmix_server_globals.iof_output,
                        "server:_iofreg reply being sent to %s:%u",
                        scd->peer->info->pname.nspace,
                        scd->peer->info->pname.rank);
    PMIX_SERVER_QUEUE_REPLY(rc, scd->peer, scd->hdr.tag, reply);
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(reply);
    }

cleanup:
    PMIX_RELEASE(scd);
    PMIX_RELEASE(cd);
    PMIX_RELEASE(scdwrapper);
}

static void iofdereg(pmix_status_t status, void *cbdata)
{
    pmix_shift_caddy_t *scdwrapper;
    pmix_setup_caddy_t *cd = (pmix_setup_caddy_t *) cbdata;
    pmix_server_caddy_t *scd = (pmix_server_caddy_t *) cd->cbdata;

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        PMIX_RELEASE(scd);
        PMIX_RELEASE(cd);
        return;
    }

    pmix_output_verbose(2, pmix_server_globals.base_output,
                        "server:_iofdereg called with %s status",
                        PMIx_Error_string(status));

    /* need to thread-shift this callback */
    scdwrapper = PMIX_NEW(pmix_shift_caddy_t);
    if (NULL == scdwrapper) {
        /* nothing we can do */
        PMIX_ERROR_LOG(PMIX_ERR_NOMEM);
        return;
    }
    scdwrapper->status = status;
    scdwrapper->cbdata = cbdata;
    PMIX_THREADSHIFT(scdwrapper, _iofdreg);
}

static void _fabcbfunc(int sd, short args, void *cbdata)
{
    pmix_shift_caddy_t *scd = (pmix_shift_caddy_t*)cbdata;
    pmix_query_caddy_t *qcd = (pmix_query_caddy_t *) scd->cbdata;
    pmix_server_caddy_t *cd = (pmix_server_caddy_t *) qcd->cbdata;
    pmix_buffer_t *reply;
    pmix_status_t rc;
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        PMIX_RELEASE(scd);
        PMIX_RELEASE(qcd);
        PMIX_RELEASE(cd);
        return;
    }

    pmix_output_verbose(2, pmix_server_globals.base_output,
                        "pmix:fabric callback with status %s",
                        PMIx_Error_string(scd->status));

    reply = PMIX_NEW(pmix_buffer_t);
    if (NULL == reply) {
        PMIX_ERROR_LOG(PMIX_ERR_NOMEM);
        goto cleanup;
    }
    PMIX_BFROPS_PACK(rc, cd->peer, reply, &scd->status, 1, PMIX_STATUS);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto complete;
    }
    /* pack the returned data */
    PMIX_BFROPS_PACK(rc, cd->peer, reply, &scd->ninfo, 1, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto complete;
    }
    if (0 < scd->ninfo) {
        PMIX_BFROPS_PACK(rc, cd->peer, reply, scd->info, scd->ninfo, PMIX_INFO);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
        }
    }

complete:
    // send reply
    PMIX_SERVER_QUEUE_REPLY(rc, cd->peer, cd->hdr.tag, reply);
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(reply);
    }

cleanup:
    if (NULL != qcd->queries) {
        PMIX_QUERY_FREE(qcd->queries, qcd->nqueries);
    }
    if (NULL != qcd->info) {
        PMIX_INFO_FREE(qcd->info, qcd->ninfo);
    }
    if (NULL != scd->cbfunc.relfn) {
        scd->cbfunc.relfn(scd->relcbdata);
    }
    PMIX_RELEASE(cd);
    PMIX_RELEASE(qcd);
    PMIX_RELEASE(scd);
}

static void fabric_cbfunc(pmix_status_t status, pmix_info_t *info, size_t ninfo, void *cbdata,
                          pmix_release_cbfunc_t release_fn, void *release_cbdata)
{
    pmix_shift_caddy_t *scd;
    pmix_query_caddy_t *qcd = (pmix_query_caddy_t *) cbdata;
    pmix_server_caddy_t *cd = (pmix_server_caddy_t *) qcd->cbdata;

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        PMIX_RELEASE(cd);
        PMIX_RELEASE(qcd);
        return;
    }

    pmix_output_verbose(2, pmix_server_globals.base_output,
                        "server:fabric_cbfunc called");

    /* need to thread-shift this callback as it accesses global data */
    scd = PMIX_NEW(pmix_shift_caddy_t);
    if (NULL == scd) {
        PMIX_ERROR_LOG(PMIX_ERR_NOMEM);
        /* nothing we can do */
        if (NULL != release_fn) {
            release_fn(release_cbdata);
        }
        return;
    }
    scd->status = status;
    scd->info = info;
    scd->ninfo = ninfo;
    scd->cbdata = cbdata;
    scd->cbfunc.relfn = release_fn;
    scd->relcbdata = release_cbdata;
    PMIX_THREADSHIFT(scd, _fabcbfunc);
}

static void _distcbfunc(int sd, short args, void *cbdata)
{
    pmix_shift_caddy_t *scd = (pmix_shift_caddy_t*)cbdata;
    pmix_server_caddy_t *cd = (pmix_server_caddy_t *)scd->cbdata;
    pmix_buffer_t *reply;
    pmix_status_t rc;
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        PMIX_RELEASE(scd);
        PMIX_RELEASE(cd);
        return;
    }

    pmix_output_verbose(2, pmix_server_globals.base_output,
                        "pmix:fabric callback with status %s",
                        PMIx_Error_string(scd->status));

    reply = PMIX_NEW(pmix_buffer_t);
    if (NULL == reply) {
        PMIX_ERROR_LOG(PMIX_ERR_NOMEM);
        goto cleanup;
    }
    PMIX_BFROPS_PACK(rc, cd->peer, reply, &scd->status, 1, PMIX_STATUS);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto complete;
    }
    /* pack the returned data */
    PMIX_BFROPS_PACK(rc, cd->peer, reply, &scd->ndist, 1, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto complete;
    }
    if (0 < scd->ndist) {
        PMIX_BFROPS_PACK(rc, cd->peer, reply, scd->dist, scd->ndist, PMIX_DEVICE_DIST);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
        }
    }

complete:
    // send reply
    PMIX_SERVER_QUEUE_REPLY(rc, cd->peer, cd->hdr.tag, reply);
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(reply);
    }

cleanup:
    PMIX_RELEASE(cd);
    if (NULL != scd->cbfunc.relfn) {
        scd->cbfunc.relfn(scd->relcbdata);
    }
    PMIX_RELEASE(scd);
}

static void dist_cbfunc(pmix_status_t status, pmix_device_distance_t *dist, size_t ndist, void *cbdata,
                        pmix_release_cbfunc_t release_fn, void *release_cbdata)
{
    pmix_shift_caddy_t *scd;
    pmix_server_caddy_t *cd = (pmix_server_caddy_t *)cbdata;

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        PMIX_RELEASE(cd);
        return;
    }

    pmix_output_verbose(2, pmix_server_globals.base_output,
                        "server:dist_cbfunc called");

    /* need to thread-shift this callback as it accesses global data */
    scd = PMIX_NEW(pmix_shift_caddy_t);
    if (NULL == scd) {
        PMIX_ERROR_LOG(PMIX_ERR_NOMEM);
        /* nothing we can do */
        if (NULL != release_fn) {
            release_fn(release_cbdata);
        }
        return;
    }
    scd->status = status;
    scd->dist = dist;
    scd->ndist = ndist;
    scd->cbdata = cbdata;
    scd->cbfunc.relfn = release_fn;
    scd->relcbdata = release_cbdata;
    PMIX_THREADSHIFT(scd, _distcbfunc);
}

static void _respeerscbfunc(int sd, short args, void *cbdata)
{
    pmix_shift_caddy_t *scd = (pmix_shift_caddy_t*)cbdata;
    pmix_server_caddy_t *cd = (pmix_server_caddy_t *)scd->cbdata;
    pmix_buffer_t *reply;
    pmix_status_t rc, ret;
    pmix_value_t *val;
    pmix_proc_t *pa = NULL;
    size_t np = 0;
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    PMIX_ACQUIRE_OBJECT(cd);

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        PMIX_RELEASE(cd);
        PMIX_RELEASE(scd);
        return;
    }

    ret = scd->status;
    if (PMIX_SUCCESS == ret) {
        // array return should be in first info
        if (0 == scd->ninfo) {
            // they didn't return anything
            ret = PMIX_ERR_NOT_FOUND;
            goto done;
        }
        val = &scd->info[0].value;
        if (PMIX_DATA_ARRAY != val->type ||
            PMIX_PROC != val->data.darray->type) {
            PMIX_ERROR_LOG(PMIX_ERR_INVALID_VAL);
            ret = PMIX_ERR_INVALID_VAL;
            goto done;
        }
        pa = (pmix_proc_t*)val->data.darray->array;
        np = val->data.darray->size;
    } else {
        /* attempt to locally resolve the request */
        PMIX_THREADSHIFT(cd, pmix_server_locally_resolve_peers);
        // give the host its release
        if (NULL != scd->cbfunc.relfn) {
            scd->cbfunc.relfn(scd->relcbdata);
        }
        PMIX_RELEASE(scd);
        return;
    }

done:
    reply = PMIX_NEW(pmix_buffer_t);
    if (NULL == reply) {
        PMIX_ERROR_LOG(PMIX_ERR_NOMEM);
        goto cleanup;
    }
    PMIX_BFROPS_PACK(rc, cd->peer, reply, &ret, 1, PMIX_STATUS);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto complete;
    }

    if (PMIX_SUCCESS == ret) {
        PMIX_BFROPS_PACK(rc, cd->peer, reply, &np, 1, PMIX_SIZE);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            goto complete;
        }
        if (0 < np) {
            PMIX_BFROPS_PACK(rc, cd->peer, reply, pa, np, PMIX_PROC);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                goto complete;
            }
        }
    }

complete:
    // send reply
    PMIX_SERVER_QUEUE_REPLY(rc, cd->peer, cd->hdr.tag, reply);
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(reply);
    }

cleanup:
    PMIX_RELEASE(cd);
    if (NULL != scd->cbfunc.relfn) {
        scd->cbfunc.relfn(scd->relcbdata);
    }
    PMIX_RELEASE(scd);
}

static void respeers_cbfunc(pmix_status_t status, pmix_info_t info[], size_t ninfo, void *cbdata,
                            pmix_release_cbfunc_t release_fn, void *release_cbdata)
{
    pmix_shift_caddy_t *scd;
    pmix_server_caddy_t *cd = (pmix_server_caddy_t *)cbdata;

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        PMIX_RELEASE(cd);
        return;
    }

    pmix_output_verbose(2, pmix_server_globals.base_output,
                        "server:respeers_cbfunc called");

    /* need to thread-shift this callback as it accesses global data */
    scd = PMIX_NEW(pmix_shift_caddy_t);
    if (NULL == scd) {
        PMIX_ERROR_LOG(PMIX_ERR_NOMEM);
        /* nothing we can do */
        if (NULL != release_fn) {
            release_fn(release_cbdata);
        }
        return;
    }
    scd->status = status;
    scd->info = info;
    scd->ninfo = ninfo;
    scd->cbdata = cbdata;
    scd->cbfunc.relfn = release_fn;
    scd->relcbdata = release_cbdata;
    PMIX_THREADSHIFT(scd, _respeerscbfunc);
}

static void _resnodescbfunc(int sd, short args, void *cbdata)
{
    pmix_shift_caddy_t *scd = (pmix_shift_caddy_t*)cbdata;
    pmix_server_caddy_t *cd = (pmix_server_caddy_t *)scd->cbdata;
    pmix_buffer_t *reply;
    pmix_status_t rc, ret;
    pmix_value_t *val;
    char *nodelist = NULL;
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    PMIX_ACQUIRE_OBJECT(cd);

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        PMIX_RELEASE(scd);
        PMIX_RELEASE(cd);
        return;
    }

    ret = scd->status;
    if (PMIX_SUCCESS == ret) {
        // array return should be in first info
        if (0 == scd->ninfo) {
            // they didn't return anything
            ret = PMIX_ERR_NOT_FOUND;
            goto done;
        }
        val = &scd->info[0].value;
        if (PMIX_STRING != val->type) {
            PMIX_ERROR_LOG(PMIX_ERR_INVALID_VAL);
            ret = PMIX_ERR_INVALID_VAL;
            goto done;
        }
        nodelist = val->data.string;
    } else {
        /* attempt to locally resolve the request */
        PMIX_THREADSHIFT(cd, pmix_server_locally_resolve_node);
        // give the host its release
        if (NULL != scd->cbfunc.relfn) {
            scd->cbfunc.relfn(scd->relcbdata);
        }
        PMIX_RELEASE(scd);
        return;
    }

done:
    reply = PMIX_NEW(pmix_buffer_t);
    if (NULL == reply) {
        PMIX_ERROR_LOG(PMIX_ERR_NOMEM);
        goto cleanup;
    }
    PMIX_BFROPS_PACK(rc, cd->peer, reply, &ret, 1, PMIX_STATUS);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto complete;
    }

    if (PMIX_SUCCESS == ret) {
        PMIX_BFROPS_PACK(rc, cd->peer, reply, &nodelist, 1, PMIX_STRING);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            goto complete;
        }
    }

complete:
    // send reply
    PMIX_SERVER_QUEUE_REPLY(rc, cd->peer, cd->hdr.tag, reply);
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(reply);
    }

cleanup:
    PMIX_RELEASE(cd);

    // give the caller their release
    if (NULL != scd->cbfunc.relfn) {
        scd->cbfunc.relfn(scd->relcbdata);
    }
    PMIX_RELEASE(scd);
}

static void resnodes_cbfunc(pmix_status_t status, pmix_info_t info[], size_t ninfo, void *cbdata,
                            pmix_release_cbfunc_t release_fn, void *release_cbdata)
{
    pmix_shift_caddy_t *scd;
    pmix_server_caddy_t *cd = (pmix_server_caddy_t *)cbdata;

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        PMIX_RELEASE(cd);
        return;
    }

    pmix_output_verbose(2, pmix_server_globals.base_output,
                        "server:resnodes_cbfunc called");

    /* need to thread-shift this callback as it accesses global data */
    scd = PMIX_NEW(pmix_shift_caddy_t);
    if (NULL == scd) {
        PMIX_ERROR_LOG(PMIX_ERR_NOMEM);
        /* nothing we can do */
        if (NULL != release_fn) {
            release_fn(release_cbdata);
        }
        return;
    }
    scd->status = status;
    scd->info = info;
    scd->ninfo = ninfo;
    scd->cbdata = cbdata;
    scd->cbfunc.relfn = release_fn;
    scd->relcbdata = release_cbdata;
    PMIX_THREADSHIFT(scd, _resnodescbfunc);
}

/* the switchyard is the primary message handling function. It's purpose
 * is to take incoming commands (packed into a buffer), unpack them,
 * and then call the corresponding host server's function to execute
 * them. Some commands involve only a single proc (i.e., the one
 * sending the command) and can be executed while we wait. In these cases,
 * the switchyard will construct and pack a reply buffer to be returned
 * to the sender.
 *
 * Other cases (either multi-process collective or cmds that require
 * an async reply) cannot generate an immediate reply. In these cases,
 * the reply buffer will be NULL. An appropriate callback function will
 * be called that will be responsible for eventually replying to the
 * calling processes.
 *
 * Should an error be encountered at any time within the switchyard, an
 * error reply buffer will be returned so that the caller can be notified,
 * thereby preventing the process from hanging. */
static pmix_status_t server_switchyard(pmix_peer_t *peer, uint32_t tag, pmix_buffer_t *buf)
{
    pmix_status_t rc = PMIX_ERR_NOT_SUPPORTED;
    int32_t cnt;
    pmix_cmd_t cmd;
    pmix_server_caddy_t *cd;
    pmix_proc_t proc;
    pmix_buffer_t *reply;

    /* protect against zero-byte buffers - these can come if the
     * connection is dropped due to a process failure */
    if (PMIX_BUFFER_IS_EMPTY(buf)) {
        return PMIX_ERR_LOST_CONNECTION;
    }

    /* retrieve the cmd */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, peer, buf, &cmd, &cnt, PMIX_COMMAND);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    pmix_output_verbose(2, pmix_server_globals.base_output,
                        "%s recvd pmix cmd %s from %s bytes %u",
                        PMIX_NAME_PRINT(&pmix_globals.myid),
                        pmix_command_string(cmd),
                        PMIX_PEER_PRINT(peer),
                        (unsigned int) buf->bytes_used);

    /* If I am a tool, relay this to my primary server when I have one.
     * The rule is relay-if-attached, service-it-myself otherwise - the
     * same one PMIx_Spawn applies to a launcher's own requests. */
    if (PMIX_PEER_IS_TOOL(pmix_globals.mypeer)) {
        rc = pmix_tool_relay_op(cmd, peer, buf, tag);
        if (PMIX_ERR_NOT_SUPPORTED != rc && PMIX_ERR_UNREACH != rc) {
            return rc;
        }
        /* Fall through to the logic tree, either because this is not a
         * command we relay (PMIX_ERR_NOT_SUPPORTED) or because we have no
         * server to relay it to (PMIX_ERR_UNREACH). A launcher with no
         * server attached fork/execs a spawn itself from there; a role
         * that genuinely cannot service the command answers
         * PMIX_ERR_NOT_SUPPORTED from the handler, which is the right
         * answer to give the requester anyway. Note pmix_tool_relay_op
         * returns both of those BEFORE it rewinds the buffer, so our
         * unpack position is still where the cmd read left it. */
    }

    /* if I am a server, then redirect the cmd to the appropriate
     * function for processing */

    if (PMIX_REQ_CMD == cmd) {
        reply = PMIX_NEW(pmix_buffer_t);
        if (NULL == reply) {
            PMIX_ERROR_LOG(PMIX_ERR_NOMEM);
            return PMIX_ERR_NOMEM;
        }
        PMIX_GDS_REGISTER_JOB_INFO(rc, peer, reply);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_RELEASE(reply);
            return rc;
        }
        PMIX_SERVER_QUEUE_REPLY(rc, peer, tag, reply);
        if (PMIX_SUCCESS != rc) {
            PMIX_RELEASE(reply);
        }
        peer->nptr->ndelivered++;
        return PMIX_SUCCESS;
    }

    if (PMIX_GDS_FALLBACK_CMD == cmd) {
        /* the client could not use the GDS module it selected at connect
         * time and has switched to another one. Record the new module on
         * this peer (only this peer - its nspace peers are unaffected) and
         * re-register the job data in that module's format. */
        char *modname = NULL;
        pmix_gds_base_module_t *mod;
        pmix_info_t ginfo;

        cnt = 1;
        PMIX_BFROPS_UNPACK(rc, peer, buf, &modname, &cnt, PMIX_STRING);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            return rc;
        }
        /* guard against a malformed/malicious client: a zero-length string
         * unpacks to NULL, and PMIX_INFO_LOAD would strdup(NULL) and crash */
        if (NULL == modname) {
            return PMIX_ERR_BAD_PARAM;
        }
        PMIX_INFO_LOAD(&ginfo, PMIX_GDS_MODULE, modname, PMIX_STRING);
        mod = pmix_gds_base_assign_module(&ginfo, 1);
        PMIX_INFO_DESTRUCT(&ginfo);
        /* assign_module always returns some module (modules offer themselves
         * by priority and a requested name only bumps that module's
         * priority), so confirm we actually got the module the client named
         * rather than a higher-priority default. If not, we do not have it. */
        if (NULL == mod || 0 != strcmp(mod->name, modname)) {
            free(modname);
            return PMIX_ERR_NOT_SUPPORTED;
        }
        free(modname);
        peer->gds = mod;
        reply = PMIX_NEW(pmix_buffer_t);
        if (NULL == reply) {
            PMIX_ERROR_LOG(PMIX_ERR_NOMEM);
            return PMIX_ERR_NOMEM;
        }
        PMIX_GDS_REGISTER_JOB_INFO(rc, peer, reply);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_RELEASE(reply);
            return rc;
        }
        PMIX_SERVER_QUEUE_REPLY(rc, peer, tag, reply);
        if (PMIX_SUCCESS != rc) {
            /* return the error so the switchyard caller sends a failure
             * reply rather than leaving the client's PMIx_Init blocked
             * waiting for a response that was never queued */
            PMIX_RELEASE(reply);
            return rc;
        }
        /* do NOT increment ndelivered: this client was already counted by
         * its original PMIX_REQ_CMD; this is the same client re-requesting
         * the same job data in a different format */
        return PMIX_SUCCESS;
    }

    if (PMIX_ABORT_CMD == cmd) {
        PMIX_GDS_CADDY(cd, peer, tag);
        if (PMIX_SUCCESS != (rc = pmix_server_abort(peer, buf, op_cbfunc, cd))) {
            PMIX_RELEASE(cd);
        }
        return rc;
    }

    if (PMIX_COMMIT_CMD == cmd) {
        rc = pmix_server_commit(peer, buf);
        if (!PMIX_PEER_IS_V1(peer)) {
            reply = PMIX_NEW(pmix_buffer_t);
            if (NULL == reply) {
                PMIX_ERROR_LOG(PMIX_ERR_NOMEM);
                return PMIX_ERR_NOMEM;
            }
            PMIX_BFROPS_PACK(rc, peer, reply, &rc, 1, PMIX_STATUS);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
            }
            PMIX_SERVER_QUEUE_REPLY(rc, peer, tag, reply);
            if (PMIX_SUCCESS != rc) {
                PMIX_RELEASE(reply);
            }
        }
        return PMIX_SUCCESS; // don't reply twice
    }

    if (PMIX_FENCENB_CMD == cmd) {
        PMIX_GDS_CADDY(cd, peer, tag);
        if (PMIX_SUCCESS != (rc = pmix_server_fence(cd, buf, modex_cbfunc))) {
            PMIX_RELEASE(cd);
        }
        return rc;
    }

    if (PMIX_GETNB_CMD == cmd) {
        PMIX_GDS_CADDY(cd, peer, tag);
        if (PMIX_SUCCESS != (rc = pmix_server_get(buf, get_cbfunc, cd))) {
            PMIX_RELEASE(cd);
        }
        return rc;
    }

    if (PMIX_FINALIZE_CMD == cmd) {
        peer->nptr->nfinalized++;
        /* purge events */
        pmix_server_purge_events(peer, NULL);
        PMIX_GDS_CADDY(cd, peer, tag);
        /* Call the local server, if supported. A tool counts here just as a
         * client does: the host was told when the tool connected, and this
         * is the only notice it will ever get that the tool has gone. The
         * connection drop that follows cannot serve instead - the peer is
         * marked finalized by then, so no lost-connection event is raised
         * for it - so a host that skipped this call would carry the tool's
         * state, and anything the tool was granted, for the rest of its own
         * lifetime. peer->info is set for a tool peer exactly as it is for
         * a client; server_object is simply NULL for one the host never
         * registered, which the host must already tolerate. */
        if (NULL != pmix_host_server.client_finalized &&
            (PMIX_PEER_IS_CLIENT(peer) || PMIX_PEER_IS_TOOL(peer))) {
            pmix_strncpy(proc.nspace, peer->info->pname.nspace, PMIX_MAX_NSLEN);
            proc.rank = peer->info->pname.rank;
            /* now tell the host server */
            rc = pmix_host_server.client_finalized(&proc, peer->info->server_object, op_cbfunc2, cd);
            if (PMIX_SUCCESS == rc) {
                /* don't reply to them ourselves - we will do so when the host
                 * server calls us back */
                return rc;
            } else if (PMIX_OPERATION_SUCCEEDED == rc) {
                /* they did it atomically */
                rc = PMIX_SUCCESS;
            }
            /* if the call doesn't succeed (e.g., they provided the stub
             * but return NOT_SUPPORTED), then the callback function
             * won't be called, but we still need to cleanup
             * any lingering references to this peer and answer
             * the client. Thus, we call the callback function ourselves
             * in this case */
            op_cbfunc2(rc, cd);
            /* return SUCCESS as the cbfunc generated the return msg
             * and released the cd object */
            return PMIX_SUCCESS;
        }
        /* if the host doesn't provide a client_finalized function,
         * we still need to ensure that we cleanup any lingering
         * references to this peer. We use the callback function
         * here as well to ensure the client gets its required
         * response and that we delay before cleaning up the
         * connection*/
        op_cbfunc2(PMIX_SUCCESS, cd);
        /* return SUCCESS as the cbfunc generated the return msg
         * and released the cd object */
        return PMIX_SUCCESS;
    }

    if (PMIX_PUBLISHNB_CMD == cmd) {
        PMIX_GDS_CADDY(cd, peer, tag);
        if (PMIX_SUCCESS != (rc = pmix_server_publish(peer, buf, op_cbfunc, cd))) {
            PMIX_RELEASE(cd);
        }
        return rc;
    }

    if (PMIX_LOOKUPNB_CMD == cmd) {
        PMIX_GDS_CADDY(cd, peer, tag);
        if (PMIX_SUCCESS != (rc = pmix_server_lookup(peer, buf, lookup_cbfunc, cd))) {
            PMIX_RELEASE(cd);
        }
        return rc;
    }

    if (PMIX_UNPUBLISHNB_CMD == cmd) {
        PMIX_GDS_CADDY(cd, peer, tag);
        if (PMIX_SUCCESS != (rc = pmix_server_unpublish(peer, buf, op_cbfunc, cd))) {
            PMIX_RELEASE(cd);
        }
        return rc;
    }

    if (PMIX_SPAWNNB_CMD == cmd) {
        PMIX_GDS_CADDY(cd, peer, tag);
        if (PMIX_SUCCESS != (rc = pmix_server_spawn(peer, buf, spawn_cbfunc, cd))) {
            PMIX_RELEASE(cd);
        }
        return rc;
    }

    if (PMIX_CONNECTNB_CMD == cmd) {
        PMIX_GDS_CADDY(cd, peer, tag);
        rc = pmix_server_connect(cd, buf, cnct_cbfunc);
        if (PMIX_SUCCESS != rc) {
            PMIX_RELEASE(cd);
        }
        return rc;
    }

    if (PMIX_DISCONNECTNB_CMD == cmd) {
        PMIX_GDS_CADDY(cd, peer, tag);
        rc = pmix_server_disconnect(cd, buf, discnct_cbfunc);
        if (PMIX_SUCCESS != rc) {
            PMIX_RELEASE(cd);
        }
        return rc;
    }

    if (PMIX_REGEVENTS_CMD == cmd) {
        PMIX_GDS_CADDY(cd, peer, tag);
        if (PMIX_SUCCESS != (rc = pmix_server_register_events(peer, buf, events_cbfunc, cd))) {
            PMIX_RELEASE(cd);
        }
        return rc;
    }

    if (PMIX_DEREGEVENTS_CMD == cmd) {
        pmix_server_deregister_events(peer, buf);
        return PMIX_SUCCESS;
    }

    if (PMIX_NOTIFY_CMD == cmd) {
        PMIX_GDS_CADDY(cd, peer, tag);
        rc = pmix_server_event_recvd_from_client(peer, buf, events_cbfunc, cd);
        if (PMIX_SUCCESS != rc) {
            PMIX_RELEASE(cd);
        }
        return rc;
    }

    if (PMIX_QUERY_CMD == cmd) {
        PMIX_GDS_CADDY(cd, peer, tag);
        rc = pmix_server_query(peer, buf, query_cbfunc, cd);
        if (PMIX_SUCCESS != rc) {
            PMIX_RELEASE(cd);
        }
        return rc;
    }

    if (PMIX_LOG_CMD == cmd) {
        PMIX_GDS_CADDY(cd, peer, tag);
        if (PMIX_SUCCESS != (rc = pmix_server_log(peer, buf, op_cbfunc, cd))) {
            PMIX_RELEASE(cd);
        }
        return rc;
    }

    if (PMIX_ALLOC_CMD == cmd) {
        PMIX_GDS_CADDY(cd, peer, tag);
        if (PMIX_SUCCESS != (rc = pmix_server_alloc(peer, buf, alloc_cbfunc, cd))) {
            PMIX_RELEASE(cd);
        }
        return rc;
    }

    if (PMIX_JOB_CONTROL_CMD == cmd) {
        PMIX_GDS_CADDY(cd, peer, tag);
        if (PMIX_SUCCESS != (rc = pmix_server_job_ctrl(peer, buf, jctrl_cbfunc, cd))) {
            PMIX_RELEASE(cd);
        }
        return rc;
    }

    if (PMIX_MONITOR_CMD == cmd) {
        PMIX_GDS_CADDY(cd, peer, tag);
        if (PMIX_SUCCESS != (rc = pmix_server_monitor(peer, buf, monitor_cbfunc, cd))) {
            PMIX_RELEASE(cd);
        }
        return rc;
    }

    if (PMIX_GET_CREDENTIAL_CMD == cmd) {
        PMIX_GDS_CADDY(cd, peer, tag);
        if (PMIX_SUCCESS != (rc = pmix_server_get_credential(peer, buf, cred_cbfunc, cd))) {
            PMIX_RELEASE(cd);
        }
        return rc;
    }

    if (PMIX_VALIDATE_CRED_CMD == cmd) {
        PMIX_GDS_CADDY(cd, peer, tag);
        if (PMIX_SUCCESS
            != (rc = pmix_server_validate_credential(peer, buf, validate_cbfunc, cd))) {
            PMIX_RELEASE(cd);
        }
        return rc;
    }

    if (PMIX_IOF_PULL_CMD == cmd) {
        PMIX_GDS_CADDY(cd, peer, tag);
        if (PMIX_SUCCESS != (rc = pmix_server_iofreg(peer, buf, iof_cbfunc, cd))) {
            PMIX_RELEASE(cd);
        }
        return rc;
    }

    if (PMIX_IOF_PUSH_CMD == cmd) {
        PMIX_GDS_CADDY(cd, peer, tag);
        if (PMIX_SUCCESS != (rc = pmix_server_iofstdin(peer, buf, op_cbfunc, cd))) {
            PMIX_RELEASE(cd);
        }
        return rc;
    }

    if (PMIX_IOF_DEREG_CMD == cmd) {
        PMIX_GDS_CADDY(cd, peer, tag);
        if (PMIX_SUCCESS != (rc = pmix_server_iofdereg(peer, buf, iofdereg, cd))) {
            PMIX_RELEASE(cd);
        }
        return rc;
    }

    if (PMIX_GROUP_CONSTRUCT_CMD == cmd) {
        PMIX_GDS_CADDY(cd, peer, tag);
        if (PMIX_SUCCESS != (rc = pmix_server_group(cd, buf, PMIX_GROUP_CONSTRUCT))) {
            PMIX_ERROR_LOG(rc);
            PMIX_RELEASE(cd);
        }
        return rc;
    }

    if (PMIX_GROUP_DESTRUCT_CMD == cmd) {
        PMIX_GDS_CADDY(cd, peer, tag);
        if (PMIX_SUCCESS != (rc = pmix_server_group(cd, buf, PMIX_GROUP_DESTRUCT))) {
            PMIX_RELEASE(cd);
        }
        return rc;
    }

    if (PMIX_FABRIC_REGISTER_CMD == cmd) {
        PMIX_GDS_CADDY(cd, peer, tag);
        if (PMIX_SUCCESS != (rc = pmix_server_fabric_register(cd, buf, fabric_cbfunc))) {
            PMIX_RELEASE(cd);
        }
        return rc;
    }

    if (PMIX_FABRIC_UPDATE_CMD == cmd) {
        PMIX_GDS_CADDY(cd, peer, tag);
        if (PMIX_SUCCESS != (rc = pmix_server_fabric_update(cd, buf, fabric_cbfunc))) {
            PMIX_RELEASE(cd);
        }
        return rc;
    }

    if (PMIX_COMPUTE_DEVICE_DISTANCES_CMD == cmd) {
        PMIX_GDS_CADDY(cd, peer, tag);
        if (PMIX_SUCCESS != (rc = pmix_server_device_dists(cd, buf, dist_cbfunc))) {
            PMIX_RELEASE(cd);
        }
        return rc;
    }

    if (PMIX_REFRESH_CACHE == cmd) {
        PMIX_GDS_CADDY(cd, peer, tag);
        if (PMIX_SUCCESS != (rc = pmix_server_refresh_cache(cd, buf, op_cbfunc))) {
            PMIX_RELEASE(cd);
        }
        return rc;
    }

    if (PMIX_RESBLK_CMD == cmd) {
        PMIX_GDS_CADDY(cd, peer, tag);
        if (PMIX_SUCCESS != (rc = pmix_server_resblk(cd, buf, resop_cbfunc))) {
            PMIX_RELEASE(cd);
        }
        return rc;
    }

    if (PMIX_SESSION_CTRL_CMD == cmd) {
        PMIX_GDS_CADDY(cd, peer, tag);
        if (PMIX_SUCCESS != (rc = pmix_server_session_ctrl(cd, buf, sessctrl_cbfunc))) {
            PMIX_RELEASE(cd);
        }
        return rc;
    }


    if (PMIX_RESOLVE_PEERS_CMD == cmd) {
        PMIX_GDS_CADDY(cd, peer, tag);
        if (PMIX_SUCCESS != (rc = pmix_server_resolve_peers(cd, buf, respeers_cbfunc))) {
            PMIX_RELEASE(cd);
        }
        return rc;
    }

    if (PMIX_RESOLVE_NODE_CMD == cmd) {
        PMIX_GDS_CADDY(cd, peer, tag);
        if (PMIX_SUCCESS != (rc = pmix_server_resolve_node(cd, buf, resnodes_cbfunc))) {
            PMIX_RELEASE(cd);
        }
        return rc;
    }

    return PMIX_ERR_NOT_SUPPORTED;
}

void pmix_server_message_handler(struct pmix_peer_t *pr, pmix_ptl_hdr_t *hdr,
                                 pmix_buffer_t *buf, void *cbdata)
{
    pmix_peer_t *peer = (pmix_peer_t *) pr;
    pmix_buffer_t *reply;
    pmix_status_t rc, ret;

    pmix_output_verbose(2, pmix_server_globals.base_output, "SWITCHYARD for %s:%u:%d",
                        peer->info->pname.nspace, peer->info->pname.rank, peer->sd);
    PMIX_HIDE_UNUSED_PARAMS(cbdata);

    ret = server_switchyard(peer, hdr->tag, buf);
    /* send the return, if there was an error returned */
    if (PMIX_SUCCESS != ret) {
        reply = PMIX_NEW(pmix_buffer_t);
        if (NULL == reply) {
            PMIX_ERROR_LOG(PMIX_ERR_NOMEM);
            return;
        }
        if (PMIX_OPERATION_SUCCEEDED == ret) {
            ret = PMIX_SUCCESS;
        }
        PMIX_BFROPS_PACK(rc, pr, reply, &ret, 1, PMIX_STATUS);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
        }
        PMIX_SERVER_QUEUE_REPLY(rc, peer, hdr->tag, reply);
        if (PMIX_SUCCESS != rc) {
            PMIX_RELEASE(reply);
        }
    }
}
