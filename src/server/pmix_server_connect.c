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
 * Copyright (c) 2021-2024 Nanook Consulting  All rights reserved.
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

/* Timeout for either collective in this file. The local-collection phase
 * is ours to bound: until every local participant has contributed we
 * have not called the host, so nothing the host might do about
 * PMIX_TIMEOUT can help a participant that never arrives. */
static void collective_timeout(int sd, short args, void *cbdata)
{
    pmix_server_trkr_t *trk = (pmix_server_trkr_t *) cbdata;
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    pmix_output_verbose(2, pmix_server_globals.connect_output,
                        "ALERT: %s timeout fired",
                        (PMIX_CONNECTNB_CMD == trk->type) ? "connect" : "disconnect");

    /* execute the provided callback function with the error */
    if (NULL != trk->op_cbfunc) {
        trk->op_cbfunc(PMIX_ERR_TIMEOUT, trk);
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

/* Pull a PMIX_TIMEOUT out of the directives the client sent, if it gave
 * one. Use the type-tolerant accessor rather than a raw union read so any
 * integer type is accepted, and convert through a uint32_t of its own:
 * the accessor writes exactly the width of the requested type, so handing
 * it the address of a wider time_t would fill only half the field - the
 * wrong half on a big-endian host. */
static void check_timeout(pmix_info_t *info, size_t ninfo, struct timeval *tv)
{
    size_t n;
    uint32_t tmo;

    for (n = 0; n < ninfo; n++) {
        if (PMIX_CHECK_KEY(&info[n], PMIX_TIMEOUT)) {
            if (PMIX_SUCCESS == PMIx_Value_get_number(&info[n].value, &tmo, PMIX_UINT32)) {
                tv->tv_sec = tmo;
            }
            return;
        }
    }
}

pmix_status_t pmix_server_disconnect(pmix_server_caddy_t *cd, pmix_buffer_t *buf,
                                     pmix_op_cbfunc_t cbfunc)
{
    int32_t cnt;
    pmix_status_t rc;
    pmix_info_t *info = NULL;
    size_t nprocs, ninfo, ninf;
    pmix_server_trkr_t *trk;
    pmix_proc_t *procs = NULL;
    struct timeval tv = {0, 0};

    /* unpack the number of procs */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, cd->peer, buf, &nprocs, &cnt, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto cleanup;
    }
    /* there must be at least one proc - we do not allow the client
     * to send us NULL proc as the server has no idea what to do
     * with that situation. Instead, the client should at least send
     * us their own namespace for the use-case where the connection
     * spans all procs in that namespace */
    if (nprocs < 1) {
        PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
        rc = PMIX_ERR_BAD_PARAM;
        goto cleanup;
    }

    /* unpack the procs */
    PMIX_PROC_CREATE(procs, nprocs);
    if (NULL == procs) {
        rc = PMIX_ERR_NOMEM;
        goto cleanup;
    }
    cnt = nprocs;
    PMIX_BFROPS_UNPACK(rc, cd->peer, buf, procs, &cnt, PMIX_PROC);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto cleanup;
    }
    /* sort the procs */
    qsort(procs, nprocs, sizeof(pmix_proc_t), pmix_util_compare_proc);

    /* unpack the number of provided info structs */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, cd->peer, buf, &ninf, &cnt, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        goto cleanup;
    }
    ninfo = ninf + 2;
    PMIX_INFO_CREATE(info, ninfo);
    if (NULL == info) {
        rc = PMIX_ERR_NOMEM;
        goto cleanup;
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
            goto cleanup;
        }
        /* check for a timeout */
        check_timeout(info, ninf, &tv);
    }

    /* find/create the local tracker for this operation */
    if (NULL == (trk = pmix_server_get_tracker(NULL, procs, nprocs, PMIX_DISCONNECTNB_CMD))) {
        /* we don't have this tracker yet, so get a new one */
        if (NULL == (trk = pmix_server_new_tracker(NULL, procs, nprocs, PMIX_DISCONNECTNB_CMD))) {
            /* only if a bozo error occurs */
            PMIX_ERROR_LOG(PMIX_ERROR);
            rc = PMIX_ERROR;
            goto cleanup;
        }
        trk->op_cbfunc = cbfunc;
    }

    /* if the info keys have not been provided yet, pass
     * them along here */
    if (NULL == trk->info && NULL != info) {
        trk->info = info;
        trk->ninfo = ninfo;
        info = NULL;
        ninfo = 0;
    }

    /* add this contributor to the tracker so they get
     * notified when we are done */
    pmix_list_append(&trk->local_cbs, &cd->super);

    /* if a timeout was specified, arm it once - guard against re-arming for
     * each contributor. Do not retain the tracker: its collectives-list
     * reference persists until completion, and completion deletes the timer
     * before releasing (mirroring the fence family). */
    if (0 < tv.tv_sec && !trk->event_active) {
        PMIX_THREADSHIFT_DELAY(trk, collective_timeout, tv.tv_sec);
        trk->event_active = true;
    }

    /* if all local contributions have been received,
     * let the local host's server know that we are at the
     * "fence" point - they will callback once the [dis]connect
     * across all participants has been completed */
    if (pmix_server_trk_complete(trk)) {
        /* delete the local-phase timer before handing the tracker to the
         * host, so a late internal timeout cannot race the host completion
         * (which could return the tracker after we released it). Once handed
         * up, the host owns any further timeout. */
        if (trk->event_active) {
            pmix_event_del(&trk->ev);
            trk->event_active = false;
        }
        if (trk->local) {
            /* the operation is being atomically completed and the host will
             * not be calling us back - ensure we notify all participants.
             * the cbfunc thread-shifts the call prior to processing,
             * so it is okay to call it directly from here */
            trk->host_called = false; // the host will not be calling us back
            cbfunc(PMIX_SUCCESS, trk);
            /* ensure that the switchyard doesn't release the caddy */
            rc = PMIX_SUCCESS;
        } else if (NULL == pmix_host_server.disconnect) {
            /* the host cannot execute this operation. Detach this caddy so
             * the switchyard can send the error to this caller, and drive the
             * op completion function to notify all other participants and tear
             * down the tracker (unlink from collectives + release, exactly
             * once). Releasing the still-linked tracker directly here would
             * leave a dangling collectives entry and double-free this caddy. */
            pmix_list_remove_item(&trk->local_cbs, &cd->super);
            cd->trk = NULL;
            trk->host_called = false;
            cbfunc(PMIX_ERR_NOT_SUPPORTED, trk);
            rc = PMIX_ERR_NOT_SUPPORTED;
            goto cleanup;
        } else {
            trk->host_called = true;
            rc = pmix_host_server.disconnect(trk->pcs, trk->npcs, trk->info, trk->ninfo, cbfunc,
                                             trk);
            if (PMIX_SUCCESS != rc && PMIX_OPERATION_SUCCEEDED != rc) {
                /* clear the caddy from this tracker so it can be
                 * released upon return - the switchyard will send an
                 * error to this caller, and so the op completion
                 * function doesn't need to do so */
                pmix_list_remove_item(&trk->local_cbs, &cd->super);
                cd->trk = NULL;
                /* we need to ensure that all other local participants don't
                 * just hang waiting for the error return, so execute
                 * the op completion function - it threadshifts the call
                 * prior to processing, so it is okay to call it directly
                 * from here */
                trk->host_called = false; // the host will not be calling us back
                cbfunc(rc, trk);
            } else if (PMIX_OPERATION_SUCCEEDED == rc) {
                /* the operation was atomically completed and the host will
                 * not be calling us back - ensure we notify all participants.
                 * the cbfunc thread-shifts the call prior to processing,
                 * so it is okay to call it directly from here */
                trk->host_called = false; // the host will not be calling us back
                cbfunc(PMIX_SUCCESS, trk);
                /* ensure that the switchyard doesn't release the caddy */
                rc = PMIX_SUCCESS;
            }
        }
    } else {
        rc = PMIX_SUCCESS;
    }

cleanup:
    if (NULL != procs) {
        PMIX_PROC_FREE(procs, nprocs);
    }
    if (NULL != info) {
        PMIX_INFO_FREE(info, ninfo);
    }
    return rc;
}

pmix_status_t pmix_server_connect(pmix_server_caddy_t *cd,
                                  pmix_buffer_t *buf,
                                  pmix_op_cbfunc_t cbfunc)
{
    int32_t cnt;
    pmix_status_t rc;
    pmix_proc_t *procs = NULL;
    pmix_info_t *info = NULL, *iptr, endpt;
    size_t nprocs, ninfo, n, ninf;
    pmix_server_trkr_t *trk;
    struct timeval tv = {0, 0};

    pmix_output_verbose(2, pmix_server_globals.connect_output,
                        "recvd CONNECT from peer %s:%d",
                        cd->peer->info->pname.nspace, cd->peer->info->pname.rank);

    /* unpack the number of procs */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, cd->peer, buf, &nprocs, &cnt, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto cleanup;
    }
    /* there must be at least one proc - we do not allow the client
     * to send us NULL proc as the server has no idea what to do
     * with that situation. Instead, the client should at least send
     * us their own namespace for the use-case where the connection
     * spans all procs in that namespace */
    if (nprocs < 1) {
        PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
        rc = PMIX_ERR_BAD_PARAM;
        goto cleanup;
    }

    /* unpack the procs */
    PMIX_PROC_CREATE(procs, nprocs);
    if (NULL == procs) {
        rc = PMIX_ERR_NOMEM;
        goto cleanup;
    }
    cnt = nprocs;
    PMIX_BFROPS_UNPACK(rc, cd->peer, buf, procs, &cnt, PMIX_PROC);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto cleanup;
    }
    /* sort the procs */
    qsort(procs, nprocs, sizeof(pmix_proc_t), pmix_util_compare_proc);

    /* unpack the number of provided info structs */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, cd->peer, buf, &ninf, &cnt, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto cleanup;
    }
    ninfo = ninf + 2;
    PMIX_INFO_CREATE(info, ninfo);
    if (NULL == info) {
        rc = PMIX_ERR_NOMEM;
        goto cleanup;
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
            PMIX_ERROR_LOG(rc);
            goto cleanup;
        }
        /* check for a timeout */
        check_timeout(info, ninf, &tv);
    }

    /* find/create the local tracker for this operation */
    if (NULL == (trk = pmix_server_get_tracker(NULL, procs, nprocs, PMIX_CONNECTNB_CMD))) {
        /* we don't have this tracker yet, so get a new one */
        if (NULL == (trk = pmix_server_new_tracker(NULL, procs, nprocs, PMIX_CONNECTNB_CMD))) {
            /* only if a bozo error occurs. Do NOT drive the completion
             * function here: it takes a tracker, not this caddy, and there
             * is no tracker to give it. Returning the error is enough - the
             * switchyard answers this caller and releases the caddy, so the
             * client cannot hang. Handing it the caddy read a
             * pmix_server_caddy_t as a pmix_server_trkr_t and then released
             * the caddy a second time. */
            PMIX_ERROR_LOG(PMIX_ERROR);
            rc = PMIX_ERROR;
            goto cleanup;
        }
        trk->op_cbfunc = cbfunc;
    }

    /* if the info keys have not been provided yet, pass
     * them along here */
    if (NULL == trk->info && NULL != info) {
        trk->info = info;
        trk->ninfo = ninfo;
        info = NULL;
        ninfo = 0;
    }

    // see if they provided endpt info
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, cd->peer, buf, &endpt, &cnt, PMIX_INFO);
    if (PMIX_SUCCESS != rc && PMIX_ERR_UNPACK_READ_PAST_END_OF_BUFFER != rc) {
        PMIX_ERROR_LOG(rc);
        goto cleanup;
    }
    if (PMIX_SUCCESS == rc) {
        // add the endpt to the end
        ninf = trk->ninfo + 1;
        PMIX_INFO_CREATE(iptr, ninf);
        if (NULL == iptr) {
            PMIX_INFO_DESTRUCT(&endpt);
            rc = PMIX_ERR_NOMEM;
            goto cleanup;
        }
        for (n=0; n < trk->ninfo; n++) {
            PMIX_INFO_XFER(&iptr[n], &trk->info[n]);
        }
        PMIX_INFO_XFER(&iptr[trk->ninfo], &endpt);
        PMIX_INFO_FREE(trk->info, trk->ninfo);
        PMIX_INFO_DESTRUCT(&endpt);
        trk->info = iptr;
        trk->ninfo = ninf;
    }

    // see if they provided job-level info
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, cd->peer, buf, &endpt, &cnt, PMIX_INFO);
    if (PMIX_SUCCESS != rc && PMIX_ERR_UNPACK_READ_PAST_END_OF_BUFFER != rc) {
        PMIX_ERROR_LOG(rc);
        goto cleanup;
    }
    if (PMIX_SUCCESS == rc) {
        // add the info to the end
        ninf = trk->ninfo + 1;
        PMIX_INFO_CREATE(iptr, ninf);
        if (NULL == iptr) {
            PMIX_INFO_DESTRUCT(&endpt);
            rc = PMIX_ERR_NOMEM;
            goto cleanup;
        }
        for (n=0; n < trk->ninfo; n++) {
            PMIX_INFO_XFER(&iptr[n], &trk->info[n]);
        }
        PMIX_INFO_XFER(&iptr[trk->ninfo], &endpt);
        PMIX_INFO_FREE(trk->info, trk->ninfo);
        PMIX_INFO_DESTRUCT(&endpt);
        trk->info = iptr;
        trk->ninfo = ninf;
    }

    /* add this contributor to the tracker so they get
     * notified when we are done */
    pmix_list_append(&trk->local_cbs, &cd->super);

    /* if a timeout was specified, arm it once - guard against re-arming for
     * each contributor. Do not retain the tracker: its collectives-list
     * reference persists until completion, and completion deletes the timer
     * before releasing (mirroring the fence family). */
    if (0 < tv.tv_sec && !trk->event_active) {
        PMIX_THREADSHIFT_DELAY(trk, collective_timeout, tv.tv_sec);
        trk->event_active = true;
    }

    /* if all local contributions have been received,
     * let the local host's server know that we are at the
     * "fence" point - they will callback once the [dis]connect
     * across all participants has been completed */
    if (pmix_server_trk_complete(trk)) {
        /* delete the local-phase timer before handing the tracker to the
         * host, so a late internal timeout cannot race the host completion
         * (which could return the tracker after we released it). Once handed
         * up, the host owns any further timeout. */
        if (trk->event_active) {
            pmix_event_del(&trk->ev);
            trk->event_active = false;
        }
        /* if all the participants are local, then we don't need the host */
        if (trk->local) {
            /* the operation is being atomically completed and the host will
             * not be calling us back - ensure we notify all participants.
             * the cbfunc thread-shifts the call prior to processing,
             * so it is okay to call it directly from here */
            trk->host_called = false; // the host will not be calling us back
            cbfunc(PMIX_SUCCESS, trk);
            /* ensure that the switchyard doesn't release the caddy */
            rc = PMIX_SUCCESS;
        } else if (NULL == pmix_host_server.connect) {
            /* the host cannot execute this operation. Detach this caddy so
             * the switchyard can send the error to this caller, and drive the
             * op completion function to notify all other participants and tear
             * down the tracker (unlink from collectives + release, exactly
             * once). Releasing the still-linked tracker directly here would
             * leave a dangling collectives entry and double-free this caddy. */
            pmix_list_remove_item(&trk->local_cbs, &cd->super);
            cd->trk = NULL;
            trk->host_called = false;
            cbfunc(PMIX_ERR_NOT_SUPPORTED, trk);
            rc = PMIX_ERR_NOT_SUPPORTED;
            goto cleanup;
        } else {
            trk->host_called = true;
            rc = pmix_host_server.connect(trk->pcs, trk->npcs, trk->info, trk->ninfo, cbfunc, trk);
            if (PMIX_SUCCESS != rc && PMIX_OPERATION_SUCCEEDED != rc) {
                /* clear the caddy from this tracker so it can be
                 * released upon return - the switchyard will send an
                 * error to this caller, and so the op completion
                 * function doesn't need to do so */
                pmix_list_remove_item(&trk->local_cbs, &cd->super);
                cd->trk = NULL;
                /* we need to ensure that all other local participants don't
                 * just hang waiting for the error return, so execute
                 * the op completion function - it threadshifts the call
                 * prior to processing, so it is okay to call it directly
                 * from here */
                trk->host_called = false; // the host will not be calling us back
                cbfunc(rc, trk);
            } else if (PMIX_OPERATION_SUCCEEDED == rc) {
                /* the operation was atomically completed and the host will
                 * not be calling us back - ensure we notify all participants.
                 * the cbfunc thread-shifts the call prior to processing,
                 * so it is okay to call it directly from here */
                trk->host_called = false; // the host will not be calling us back
                cbfunc(PMIX_SUCCESS, trk);
                /* ensure that the switchyard doesn't release the caddy */
                rc = PMIX_SUCCESS;
            }
        }
    } else {
        rc = PMIX_SUCCESS;
    }

cleanup:
    if (NULL != procs) {
        PMIX_PROC_FREE(procs, nprocs);
    }
    if (NULL != info) {
        PMIX_INFO_FREE(info, ninfo);
    }
    return rc;
}
