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
#include "src/mca/prm/prm.h"
#include "src/mca/psensor/psensor.h"
#include "src/mca/ptl/base/base.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_error.h"
#include "src/util/pmix_name_fns.h"
#include "src/util/pmix_output.h"
#include "src/util/pmix_environ.h"

#include "src/client/pmix_client_ops.h"
#include "pmix_server_ops.h"

pmix_status_t pmix_server_disconnect(pmix_server_caddy_t *cd, pmix_buffer_t *buf,
                                     pmix_op_cbfunc_t cbfunc)
{
    int32_t cnt;
    pmix_status_t rc;
    pmix_info_t *info = NULL;
    size_t nprocs, ninfo, ninf;
    pmix_server_trkr_t *trk;
    pmix_proc_t *procs = NULL;

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
        return rc;
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
    /* if all local contributions have been received,
     * let the local host's server know that we are at the
     * "fence" point - they will callback once the [dis]connect
     * across all participants has been completed */
    if (trk->def_complete && pmix_list_get_size(&trk->local_cbs) == trk->nlocal) {
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
            PMIX_RELEASE(trk);
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
    if (NULL != info) {
        PMIX_INFO_FREE(info, ninfo);
    }
    return rc;
}

static void connect_timeout(int sd, short args, void *cbdata)
{
    pmix_server_trkr_t *trk = (pmix_server_trkr_t *) cbdata;
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    pmix_output_verbose(2, pmix_server_globals.connect_output, "ALERT: connect timeout fired");

    /* execute the provided callback function with the error */
    if (NULL != trk->op_cbfunc) {
        trk->op_cbfunc(PMIX_ERR_TIMEOUT, trk);
        return; // the cbfunc will have cleaned up the tracker
    }
    trk->event_active = false;
    PMIX_RELEASE(trk);
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
        for (n = 0; n < ninf; n++) {
            if (0 == strncmp(info[n].key, PMIX_TIMEOUT, PMIX_MAX_KEYLEN)) {
                tv.tv_sec = info[n].value.data.uint32;
                break;
            }
        }
    }

    /* find/create the local tracker for this operation */
    if (NULL == (trk = pmix_server_get_tracker(NULL, procs, nprocs, PMIX_CONNECTNB_CMD))) {
        /* we don't have this tracker yet, so get a new one */
        if (NULL == (trk = pmix_server_new_tracker(NULL, procs, nprocs, PMIX_CONNECTNB_CMD))) {
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

    /* if all local contributions have been received,
     * let the local host's server know that we are at the
     * "fence" point - they will callback once the [dis]connect
     * across all participants has been completed */
    if (trk->def_complete && pmix_list_get_size(&trk->local_cbs) == trk->nlocal) {
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
            PMIX_RELEASE(trk);
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
    /* if a timeout was specified, set it */
    if (PMIX_SUCCESS == rc && 0 < tv.tv_sec) {
        PMIX_RETAIN(trk);
        PMIX_THREADSHIFT_DELAY(trk, connect_timeout, tv.tv_sec);
        trk->event_active = true;
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
