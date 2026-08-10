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

/* Fabric support: registration and update of a fabric plane on behalf of
 * a requestor, and the device-distance computation that reports how far
 * each fabric device sits from a process. */

#include "src/include/pmix_config.h"

#include "src/include/pmix_stdint.h"

#include "include/pmix_server.h"
#include "src/include/pmix_globals.h"

#ifdef HAVE_STRING_H
#    include <string.h>
#endif
#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif
#ifdef HAVE_TIME_H
#    include <time.h>
#endif

#include "src/hwloc/pmix_hwloc.h"
#include "src/mca/bfrops/base/base.h"
#include "src/mca/pnet/pnet.h"
#include "src/mca/ptl/base/base.h"
#include "src/util/pmix_error.h"
#include "src/util/pmix_output.h"

#include "pmix_server_ops.h"

/* Carrier for a locally computed distance array. The callback that
 * receives one only thread-shifts; the reply is packed from it later, on
 * the progress thread, so the array has to outlive the function that
 * built it. That is what the (release_fn, release_cbdata) pair in the
 * callback signature is for - the host path uses it to hold its own data
 * alive, and the local path needs it for exactly the same reason. */
typedef struct {
    pmix_device_distance_t *dist;
    size_t ndist;
} pmix_dist_relcaddy_t;

static void dist_relfn(void *cbdata)
{
    pmix_dist_relcaddy_t *rel = (pmix_dist_relcaddy_t *) cbdata;

    PMIX_DEVICE_DIST_FREE(rel->dist, rel->ndist);
    free(rel);
}

static void _fabric_response(int sd, short args, void *cbdata)
{
    pmix_query_caddy_t *qcd = (pmix_query_caddy_t *) cbdata;
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    PMIX_ACQUIRE_OBJECT(qcd);

    /* qcd->cbfunc is the switchyard's fabric_cbfunc, which expects the
     * query caddy - not the server caddy hanging off qcd->cbdata. Handing
     * it qcd->cbdata made it read a pmix_server_caddy_t as a
     * pmix_query_caddy_t and dereference the resulting garbage pointer.
     * The callback chain owns qcd (and its info array) from here on, so
     * we must not release it a second time. */
    qcd->cbfunc(PMIX_SUCCESS, qcd->info, qcd->ninfo, qcd, NULL, NULL);
}

static void frcbfunc(pmix_status_t status, void *cbdata)
{
    pmix_query_caddy_t *qcd = (pmix_query_caddy_t *) cbdata;

    PMIX_ACQUIRE_OBJECT(qcd);
    qcd->status = status;
    PMIX_POST_OBJECT(qcd);

    PMIX_WAKEUP_THREAD(&qcd->lock);
}
/* we are being called from the PMIx server's switchyard function,
 * which means we are in an event and can access global data */
pmix_status_t pmix_server_fabric_register(pmix_server_caddy_t *cd, pmix_buffer_t *buf,
                                          pmix_info_cbfunc_t cbfunc)
{
    int32_t cnt;
    pmix_status_t rc;
    pmix_query_caddy_t *qcd = NULL;
    pmix_proc_t proc;
    pmix_fabric_t fabric;

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        return PMIX_ERR_NOT_AVAILABLE;
    }

    pmix_output_verbose(2, pmix_server_globals.base_output,
                        "recvd register_fabric request from client");

    qcd = PMIX_NEW(pmix_query_caddy_t);
    if (NULL == qcd) {
        return PMIX_ERR_NOMEM;
    }
    PMIX_RETAIN(cd);
    qcd->cbfunc = cbfunc;
    qcd->cbdata = cd;

    /* unpack the number of directives */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, cd->peer, buf, &qcd->ninfo, &cnt, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto exit;
    }
    /* unpack the directives */
    if (0 < qcd->ninfo) {
        PMIX_INFO_CREATE(qcd->info, qcd->ninfo);
        cnt = qcd->ninfo;
        PMIX_BFROPS_UNPACK(rc, cd->peer, buf, qcd->info, &cnt, PMIX_INFO);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            goto exit;
        }
    }

    /* see if we support this request ourselves */
    PMIX_FABRIC_CONSTRUCT(&fabric);
    rc = pmix_pnet.register_fabric(&fabric, qcd->info, qcd->ninfo, frcbfunc, qcd);
    if (PMIX_OPERATION_SUCCEEDED == rc) {
        /* we need to respond, but we want to ensure
         * that occurs _after_ the client returns from its API */
        if (NULL != qcd->info) {
            PMIX_INFO_FREE(qcd->info, qcd->ninfo);
        }
        qcd->info = fabric.info;
        qcd->ninfo = fabric.ninfo;
        PMIX_THREADSHIFT(qcd, _fabric_response);
        return PMIX_SUCCESS;
    } else if (PMIX_SUCCESS == rc) {
        PMIX_WAIT_THREAD(&qcd->lock);
        /* we need to respond, but we want to ensure
         * that occurs _after_ the client returns from its API */
        if (NULL != qcd->info) {
            PMIX_INFO_FREE(qcd->info, qcd->ninfo);
        }
        qcd->info = fabric.info;
        qcd->ninfo = fabric.ninfo;
        PMIX_THREADSHIFT(qcd, _fabric_response);
        return PMIX_SUCCESS;
    }

    /* if we don't internally support it, see if
     * our host does */
    if (NULL == pmix_host_server.fabric) {
        rc = PMIX_ERR_NOT_SUPPORTED;
        goto exit;
    }

    /* setup the requesting peer name */
    PMIX_LOAD_PROCID(&proc, cd->peer->info->pname.nspace, cd->peer->info->pname.rank);

    /* ask the host to execute the request */
    if (PMIX_SUCCESS
        != (rc = pmix_host_server.fabric(&proc, PMIX_FABRIC_REQUEST_INFO, qcd->info, qcd->ninfo,
                                         cbfunc, qcd))) {
        goto exit;
    }
    return PMIX_SUCCESS;

exit:
    /* every path reaching here failed before handing qcd to an async
     * callback, so release it - and the reference it took on the server
     * caddy, which the query caddy's destructor knows nothing about. The
     * switchyard releases the caddy's own reference on our error return. */
    if (NULL != qcd) {
        PMIX_RELEASE(qcd);
    }
    PMIX_RELEASE(cd);
    return rc;
}

pmix_status_t pmix_server_fabric_update(pmix_server_caddy_t *cd, pmix_buffer_t *buf,
                                        pmix_info_cbfunc_t cbfunc)
{
    int32_t cnt;
    size_t index;
    pmix_status_t rc;
    pmix_query_caddy_t *qcd;
    pmix_proc_t proc;
    pmix_fabric_t fabric;

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        return PMIX_ERR_NOT_AVAILABLE;
    }

    pmix_output_verbose(2, pmix_server_globals.base_output,
                        "recvd update_fabric request from client");

    qcd = PMIX_NEW(pmix_query_caddy_t);
    if (NULL == qcd) {
        return PMIX_ERR_NOMEM;
    }
    PMIX_RETAIN(cd);
    qcd->cbfunc = cbfunc;
    qcd->cbdata = cd;

    /* unpack the fabric index */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, cd->peer, buf, &index, &cnt, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto exit;
    }

    /* see if we support this request ourselves */
    PMIX_FABRIC_CONSTRUCT(&fabric);
    fabric.index = index;
    rc = pmix_pnet.update_fabric(&fabric);
    if (PMIX_SUCCESS == rc) {
        /* we need to respond, but we want to ensure
         * that occurs _after_ the client returns from its API */
        if (NULL != qcd->info) {
            PMIX_INFO_FREE(qcd->info, qcd->ninfo);
        }
        qcd->info = fabric.info;
        qcd->ninfo = fabric.ninfo;
        PMIX_THREADSHIFT(qcd, _fabric_response);
        return rc;
    }

    /* if we don't internally support it, see if
     * our host does */
    if (NULL == pmix_host_server.fabric) {
        rc = PMIX_ERR_NOT_SUPPORTED;
        goto exit;
    }

    /* setup the requesting peer name */
    PMIX_LOAD_PROCID(&proc, cd->peer->info->pname.nspace, cd->peer->info->pname.rank);
    /* add the index. Nothing screens this allocation the way an unpack
     * would - the load below writes straight through it */
    qcd->ninfo = 1;
    PMIX_INFO_CREATE(qcd->info, qcd->ninfo);
    if (NULL == qcd->info) {
        qcd->ninfo = 0;
        rc = PMIX_ERR_NOMEM;
        goto exit;
    }
    PMIX_INFO_LOAD(&qcd->info[0], PMIX_FABRIC_INDEX, &index, PMIX_SIZE);

    /* ask the host to execute the request */
    if (PMIX_SUCCESS
        != (rc = pmix_host_server.fabric(&proc, PMIX_FABRIC_UPDATE_INFO, qcd->info, qcd->ninfo,
                                         cbfunc, qcd))) {
        goto exit;
    }
    return PMIX_SUCCESS;

exit:
    /* every path reaching here failed before handing qcd to an async
     * callback, so release it - and the reference it took on the server
     * caddy, which the query caddy's destructor knows nothing about.
     * The sibling pmix_server_fabric_register does the same. */
    PMIX_RELEASE(qcd);
    PMIX_RELEASE(cd);
    return rc;
}

pmix_status_t pmix_server_device_dists(pmix_server_caddy_t *cd,
                                       pmix_buffer_t *buf,
                                       pmix_device_dist_cbfunc_t cbfunc)
{
    pmix_topology_t topo = {NULL, NULL};
    pmix_cpuset_t cpuset = {NULL, NULL};
    pmix_status_t rc;
    pmix_device_distance_t *distances;
    pmix_dist_relcaddy_t *rel;
    size_t ndist;
    int32_t cnt;
    pmix_cb_t cb;
    pmix_kval_t *kv;
    pmix_proc_t proc;

    /* unpack the topology they want us to use */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, cd->peer, buf, &topo, &cnt, PMIX_TOPO);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }

    /* unpack the cpuset */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, cd->peer, buf, &cpuset, &cnt, PMIX_PROC_CPUSET);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto cleanup;
    }

    /* unpack any directives */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, cd->peer, buf, &cd->ninfo, &cnt, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto cleanup;
    }
    if (0 < cd->ninfo) {
        PMIX_INFO_CREATE(cd->info, cd->ninfo);
        cnt = cd->ninfo;
        PMIX_BFROPS_UNPACK(rc, cd->peer, buf, cd->info, &cnt, PMIX_INFO);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            goto cleanup;
        }
    }

    /* if the provided topo is NULL, use my own */
    if (NULL == topo.topology) {
        if (NULL == pmix_globals.topology.topology) {
            /* try to get it */
            rc = pmix_hwloc_load_topology(&pmix_globals.topology);
            if (PMIX_SUCCESS != rc) {
                /* nothing we can do */
                goto cleanup;
            }
        }
        topo.topology = pmix_globals.topology.topology;
    }

    /* if the cpuset is NULL, see if we know the binding of the requesting process */
    if (NULL == cpuset.bitmap) {
        /* the unpack hands back a "hwloc" source string even when it
         * carried no bitmap, and pmix_hwloc_parse_cpuset_string below
         * overwrites that member with a fresh one - so give the first
         * back now, while we still have the pointer to it */
        if (NULL != cpuset.source) {
            free(cpuset.source);
            cpuset.source = NULL;
        }
        PMIX_CONSTRUCT(&cb, pmix_cb_t);
        /* the cb destructor does not own cb.key, so point it at the
         * string constant rather than a strdup that nothing would free */
        cb.key = PMIX_CPUSET;
        PMIX_LOAD_PROCID(&proc, cd->peer->info->pname.nspace, cd->peer->info->pname.rank);
        cb.proc = &proc;
        cb.scope = PMIX_LOCAL;
        cb.copy = true;
        PMIX_GDS_FETCH_KV(rc, cd->peer, &cb);
        if (PMIX_SUCCESS != rc) {
            PMIX_DESTRUCT(&cb);
            goto cleanup;
        }
        if (0 == pmix_list_get_size(&cb.kvs)) {
            /* a "successful" fetch that returned nothing - pmix_list_get_first
             * would hand back the list sentinel, not NULL, so guard on the
             * size before dereferencing it */
            rc = PMIX_ERR_NOT_FOUND;
            PMIX_DESTRUCT(&cb);
            goto cleanup;
        }
        kv = (pmix_kval_t*)pmix_list_get_first(&cb.kvs);
        if (NULL == kv->value || PMIX_STRING != kv->value->type) {
            rc = PMIX_ERR_INVALID_VAL;
            PMIX_DESTRUCT(&cb);
            goto cleanup;
        }
        rc = pmix_hwloc_parse_cpuset_string(kv->value->data.string, &cpuset);
        if (PMIX_SUCCESS != rc) {
            PMIX_DESTRUCT(&cb);
            goto cleanup;
        }
        PMIX_DESTRUCT(&cb);
    }
    /* compute the distances */
    rc = pmix_hwloc_compute_distances(&topo, &cpuset, cd->info, cd->ninfo, &distances, &ndist);
    if (PMIX_SUCCESS == rc) {
        /* Send the reply. The callback only thread-shifts, and the reply
         * is packed from the array on the progress thread after we have
         * returned - so freeing it here handed the packer freed memory.
         * Give the callback a release function instead, and let the
         * chain free the array once it is done with it. */
        rel = (pmix_dist_relcaddy_t *) malloc(sizeof(pmix_dist_relcaddy_t));
        if (NULL == rel) {
            PMIX_DEVICE_DIST_FREE(distances, ndist);
            rc = PMIX_ERR_NOMEM;
            goto cleanup;
        }
        rel->dist = distances;
        rel->ndist = ndist;
        cbfunc(rc, distances, ndist, cd, dist_relfn, rel);
    }

cleanup:
    if (NULL != topo.topology &&
        topo.topology != pmix_globals.topology.topology) {
        pmix_hwloc_destruct_topology(&topo);
    }
    /* the unpack strdup's a source string even for the NULL topology that
     * says "use your own" - and on that path we borrow the global
     * topology, so the destruct above is skipped and nothing else would
     * ever free it. It NULLs the member when it does run */
    if (NULL != topo.source) {
        free(topo.source);
    }
    if (NULL != cpuset.bitmap) {
        pmix_hwloc_destruct_cpuset(&cpuset);
    }
    if (NULL != cpuset.source) {
        free(cpuset.source);
    }
    return rc;
}
