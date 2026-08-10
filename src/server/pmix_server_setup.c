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

/* Preparation of the information a job needs before its processes are
 * started: resource registration, application and node-local setup, and
 * the helper APIs a host uses to build that information. */

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

#include "src/hwloc/pmix_hwloc.h"
#include "src/mca/bfrops/base/base.h"
#include "src/mca/gds/base/base.h"
#include "src/mca/pgpu/base/base.h"
#include "src/mca/pmdl/base/base.h"
#include "src/mca/pnet/base/base.h"
#include "src/mca/preg/preg.h"
#include "src/runtime/pmix_progress_threads.h"
#include "src/util/pmix_error.h"
#include "src/util/pmix_output.h"

#include "pmix_server_ops.h"

static void _register_resources(int sd, short args, void *cbdata)
{
    pmix_setup_caddy_t *cd = (pmix_setup_caddy_t *) cbdata;
    pmix_kval_t *kv=NULL, kp;
    size_t n, m, ctxid;
    pmix_status_t rc = PMIX_SUCCESS;
    bool gotctxid = false;
    pmix_list_t grpinfo, endpts;
    pmix_info_caddy_t *ept=NULL, *g=NULL;
    pmix_info_t *iptr, *pinfo;
    size_t ninfo, npinfo;
    pmix_byte_object_t *pbo=NULL, bo;
    pmix_buffer_t jobinfo, bkt;
    int32_t cnt;
    char *nspace;
    pmix_proc_t *proc;
    pmix_scope_t scope;
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    PMIX_CONSTRUCT(&grpinfo, pmix_list_t);
    PMIX_CONSTRUCT(&endpts, pmix_list_t);
    for (n = 0; n < cd->ninfo; n++) {
        if (PMIX_CHECK_KEY(&cd->info[n], PMIX_GROUP_INFO_ARRAY) ||
            PMIX_CHECK_KEY(&cd->info[n], PMIX_GROUP_INFO)) {
            g = PMIX_NEW(pmix_info_caddy_t);
            g->info = &cd->info[n];
            pmix_list_append(&grpinfo, &g->super);

        } else if (PMIX_CHECK_KEY(&cd->info[n], PMIX_GROUP_ENDPT_DATA)) {
            ept = PMIX_NEW(pmix_info_caddy_t);
            ept->info = &cd->info[n];
            ept->ninfo = 1;
            pmix_list_append(&endpts, &ept->super);

        } else if (PMIX_CHECK_KEY(&cd->info[n], PMIX_GROUP_CONTEXT_ID)) {
            rc = PMIx_Value_get_number(&cd->info[n].value, &ctxid, PMIX_SIZE);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
            } else {
                gotctxid = true;
            }

        } else if (PMIX_CHECK_KEY(&cd->info[n], PMIX_GROUP_JOB_INFO)) {
            pbo = (pmix_byte_object_t*)&cd->info[n].value.data.bo;

        } else {
            /* add any provided data to our global cache for all nspaces */
            kv = PMIX_NEW(pmix_kval_t);
            kv->key = strdup(cd->info[n].key);
            kv->value = (pmix_value_t *) malloc(sizeof(pmix_value_t));
            PMIX_VALUE_XFER(rc, kv->value, &cd->info[n].value);
            if (PMIX_SUCCESS != rc) {
                PMIX_RELEASE(kv);
                break;
            }
            pmix_list_append(&pmix_server_globals.gdata, &kv->super);
        }
    }

    /* if endpt data was provided, then we need to
     * store it in our hash table */
    if (0 < pmix_list_get_size(&endpts)) {
        /* Each list member points to a pmix_info_t that contains
         * a data array of info about that proc */
        PMIX_LIST_FOREACH(ept, &endpts, pmix_info_caddy_t) {
            // contains an array of proc info
            if (NULL == ept->info->value.data.darray ||
                NULL == ept->info->value.data.darray->array ||
                2 > ept->info->value.data.darray->size) {
                /* the procID and the scope occupy the first two positions,
                 * so anything shorter than that describes nothing and
                 * indexing it would read past the array the host gave us */
                PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
                rc = PMIX_ERR_BAD_PARAM;
                continue;
            }
            pinfo = (pmix_info_t*)ept->info->value.data.darray->array;
            npinfo = ept->info->value.data.darray->size;
            // procID is in the first position
            proc = pinfo[0].value.data.proc;
            // scope is in the second position
            scope = pinfo[1].value.data.scope;
            // rest of the array contains endpts
            for (m=2; m < npinfo; m++) {
                kp.key = pinfo[m].key;
                kp.value = &pinfo[m].value;
                /* go through the macro rather than calling the module's
                 * store slot by hand: a gds component is free to leave
                 * that slot NULL and rely on the macro routing the
                 * operation to the local module, which is exactly what
                 * gds/shmem3 does. We are assigned "hash" today, so this
                 * happened to work - but only by that coincidence. */
                PMIX_GDS_STORE_KV(rc, pmix_globals.mypeer, proc, scope, &kp);
                if (PMIX_SUCCESS != rc) {
                    PMIX_ERROR_LOG(rc);
                    continue;
                }
            }
        }
    }

    // if group info was provided, then we need to store it
    // in our hash table too
    if (0 < pmix_list_get_size(&grpinfo)) {
        // must have been given a context ID
        if (!gotctxid) {
            rc = PMIX_ERR_BAD_PARAM;
            goto release;
        }
        /* Each list member points to a pmix_info_t that contains
         * a data array of info from a given proc */
        PMIX_LIST_FOREACH(g, &grpinfo, pmix_info_caddy_t) {
            iptr = (pmix_info_t*)g->info->value.data.darray->array;
            ninfo = g->info->value.data.darray->size;

            if (PMIX_CHECK_KEY(g->info, PMIX_GROUP_INFO)) {
                // this is just a single array of group info
                rc = pmix_server_process_grpinfo(ctxid, iptr, ninfo);
                if (PMIX_SUCCESS != rc) {
                    goto release;
                }
            } else {
                // contains an array of group info arrays
                for (n=0; n < ninfo; n++) {
                    pinfo = (pmix_info_t*)iptr[n].value.data.darray->array;
                    npinfo = iptr[n].value.data.darray->size;
                    rc = pmix_server_process_grpinfo(ctxid, pinfo, npinfo);
                    if (PMIX_SUCCESS != rc) {
                        goto release;
                    }
                }
            }
        }
    }

    // if job info was provided, process it
    if (NULL != pbo) {
        /* load it for unpacking */
        PMIX_CONSTRUCT(&jobinfo, pmix_buffer_t);
        PMIX_LOAD_BUFFER(pmix_globals.mypeer, &jobinfo, pbo->bytes, pbo->size);

        cnt = 1;
        PMIX_BFROPS_UNPACK(rc, pmix_globals.mypeer, &jobinfo, &bo, &cnt, PMIX_BYTE_OBJECT);
        while (PMIX_SUCCESS == rc) {
            /* load it for unpacking */
            PMIX_CONSTRUCT(&bkt, pmix_buffer_t);
            PMIX_LOAD_BUFFER(pmix_globals.mypeer, &bkt, bo.bytes, bo.size);

            /* unpack the nspace for this blob */
            cnt = 1;
            PMIX_BFROPS_UNPACK(rc, pmix_globals.mypeer, &bkt, &nspace, &cnt, PMIX_STRING);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_DESTRUCT(&bkt);
                continue;
            }
            /* extract and process any proc-related info for this nspace */
            PMIX_GDS_STORE_JOB_INFO(rc, pmix_globals.mypeer, nspace, &bkt);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
            }
            free(nspace);
            PMIX_DESTRUCT(&bkt);
            /* get the next one */
            cnt = 1;
            /* the same peer that opened this buffer has to keep reading it -
             * myserver is repointed while a launcher-spawned server attaches
             * to its parent, so the two are not interchangeable here */
            PMIX_BFROPS_UNPACK(rc, pmix_globals.mypeer, &jobinfo, &bo, &cnt, PMIX_BYTE_OBJECT);
        }
        if (PMIX_ERR_UNPACK_READ_PAST_END_OF_BUFFER != rc) {
            PMIX_ERROR_LOG(rc);
        }
    }

release:
    /* both scratch lists own the caddies appended to them above and were
     * being dropped on the floor - on the normal path as well as this one */
    PMIX_LIST_DESTRUCT(&grpinfo);
    PMIX_LIST_DESTRUCT(&endpts);
    cd->opcbfunc(rc, cd->cbdata);
    PMIX_RELEASE(cd);
}

pmix_status_t PMIx_server_register_resources(pmix_info_t info[], size_t ninfo,
                                             pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    pmix_setup_caddy_t *cd;
    pmix_lock_t mylock;
    pmix_status_t rc;

    pmix_output_verbose(2, pmix_server_globals.base_output,
                        "pmix:server register resources");

    if (!pmix_atomic_check_bool(&pmix_globals.initialized)) {
        return PMIX_ERR_INIT;
    }

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        return PMIX_ERR_NOT_AVAILABLE;
    }

    cd = PMIX_NEW(pmix_setup_caddy_t);
    cd->info = info;
    cd->ninfo = ninfo;
    cd->opcbfunc = cbfunc;
    cd->cbdata = cbdata;

    /* if the provided callback is NULL, then substitute
     * our own internal cbfunc and block here */
    if (NULL == cbfunc) {
        if (pmix_progress_thread_check_blocking("PMIx_server_register_resources")) {
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
        PMIX_THREADSHIFT(cd, _register_resources);
        PMIX_WAIT_THREAD(&mylock);
        rc = mylock.status;
        if (PMIX_SUCCESS == rc) {
            rc = PMIX_OPERATION_SUCCEEDED;
        }
        PMIX_DESTRUCT_LOCK(&mylock);
        return rc;
    }

    /* we have to push this into our event library to avoid
     * potential threading issues */
    PMIX_THREADSHIFT(cd, _register_resources);
    return PMIX_SUCCESS;
}

static void _deregister_resources(int sd, short args, void *cbdata)
{
    pmix_setup_caddy_t *cd = (pmix_setup_caddy_t *) cbdata;
    pmix_kval_t *kv;
    size_t n;

    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    /* find any matches in our global cache and remove them */
    for (n = 0; n < cd->ninfo; n++) {
        PMIX_LIST_FOREACH (kv, &pmix_server_globals.gdata, pmix_kval_t) {
            if (PMIX_CHECK_KEY(kv, cd->info[n].key)) {
                pmix_list_remove_item(&pmix_server_globals.gdata, &kv->super);
                PMIX_RELEASE(kv);
                break;
            }
        }
    }

    cd->opcbfunc(PMIX_SUCCESS, cd->cbdata);
    PMIX_RELEASE(cd);
}

pmix_status_t PMIx_server_deregister_resources(pmix_info_t info[], size_t ninfo,
                                               pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    pmix_setup_caddy_t *cd;
    pmix_lock_t mylock;
    pmix_status_t rc;

    pmix_output_verbose(2, pmix_server_globals.base_output,
                        "pmix:server deregister resources");

    if (!pmix_atomic_check_bool(&pmix_globals.initialized)) {
        return PMIX_ERR_INIT;
    }

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        return PMIX_ERR_NOT_AVAILABLE;
    }

    cd = PMIX_NEW(pmix_setup_caddy_t);
    cd->info = info;
    cd->ninfo = ninfo;
    cd->opcbfunc = cbfunc;
    cd->cbdata = cbdata;

    /* if the provided callback is NULL, then substitute
     * our own internal cbfunc and block here */
    if (NULL == cbfunc) {
        if (pmix_progress_thread_check_blocking("PMIx_server_deregister_resources")) {
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
        PMIX_THREADSHIFT(cd, _deregister_resources);
        PMIX_WAIT_THREAD(&mylock);
        rc = mylock.status;
        if (PMIX_SUCCESS == rc) {
            rc = PMIX_OPERATION_SUCCEEDED;
        }
        PMIX_DESTRUCT_LOCK(&mylock);
        return rc;
    }

    /* we have to push this into our event library to avoid
     * potential threading issues */
    PMIX_THREADSHIFT(cd, _deregister_resources);
    return PMIX_SUCCESS;
}

static void _setup_op(pmix_status_t rc, void *cbdata)
{
    pmix_setup_caddy_t *fcd = (pmix_setup_caddy_t *) cbdata;
    PMIX_HIDE_UNUSED_PARAMS(rc);

    if (NULL != fcd->info) {
        PMIX_INFO_FREE(fcd->info, fcd->ninfo);
    }
    PMIX_RELEASE(fcd);
}

static void _setup_app(int sd, short args, void *cbdata)
{
    pmix_setup_caddy_t *cd = (pmix_setup_caddy_t *) cbdata;
    pmix_setup_caddy_t *fcd = NULL;
    pmix_status_t rc;
    pmix_list_t ilist;
    pmix_kval_t *kv;
    size_t n;

    PMIX_ACQUIRE_OBJECT(cd);
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    PMIX_CONSTRUCT(&ilist, pmix_list_t);

    /* pass to the network libraries */
    if (PMIX_SUCCESS != (rc = pmix_pnet.allocate(cd->nspace, cd->info, cd->ninfo, &ilist))) {
        goto depart;
    }

    /* pass to the GPU libraries */
    if (PMIX_SUCCESS != (rc = pmix_pgpu.allocate(cd->nspace, cd->info, cd->ninfo, &ilist))) {
        goto depart;
    }

    /* pass to the programming model libraries */
    if (PMIX_SUCCESS != (rc = pmix_pmdl.harvest_envars(cd->nspace, cd->info, cd->ninfo, &ilist))) {
        goto depart;
    }

    /* setup the return callback */
    fcd = PMIX_NEW(pmix_setup_caddy_t);
    if (NULL == fcd) {
        rc = PMIX_ERR_NOMEM;
        PMIX_ERROR_LOG(PMIX_ERR_NOMEM);
        goto depart;
    }

    /* if anything came back, construct an info array */
    if (0 < (fcd->ninfo = pmix_list_get_size(&ilist))) {
        PMIX_INFO_CREATE(fcd->info, fcd->ninfo);
        if (NULL == fcd->info) {
            rc = PMIX_ERR_NOMEM;
            PMIX_RELEASE(fcd);
            goto depart;
        }
        n = 0;
        PMIX_LIST_FOREACH (kv, &ilist, pmix_kval_t) {
            pmix_strncpy(fcd->info[n].key, kv->key, PMIX_MAX_KEYLEN);
            PMIx_Value_xfer(&fcd->info[n].value, kv->value);
            ++n;
        }
    }

depart:
    /* always execute the callback to avoid hanging */
    if (NULL != cd->setupcbfunc) {
        if (NULL == fcd) {
            cd->setupcbfunc(rc, NULL, 0, cd->cbdata, NULL, NULL);
        } else {
            cd->setupcbfunc(rc, fcd->info, fcd->ninfo, cd->cbdata, _setup_op, fcd);
        }
    } else if (NULL != fcd) {
        /* nobody to hand the results to, so nobody will call _setup_op to
         * give them back either - the caddy and the info array assembled
         * into it are ours to release */
        _setup_op(rc, fcd);
    }

    /* cleanup memory */
    PMIX_LIST_DESTRUCT(&ilist);
    if (NULL != cd->nspace) {
        free(cd->nspace);
    }
    PMIX_RELEASE(cd);
}

pmix_status_t PMIx_server_setup_application(const pmix_nspace_t nspace, pmix_info_t info[],
                                            size_t ninfo, pmix_setup_application_cbfunc_t cbfunc,
                                            void *cbdata)
{
    pmix_setup_caddy_t *cd;

    if (!pmix_atomic_check_bool(&pmix_globals.initialized)) {
        return PMIX_ERR_INIT;
    }

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        return PMIX_ERR_NOT_AVAILABLE;
    }

    /* need to threadshift this request */
    cd = PMIX_NEW(pmix_setup_caddy_t);
    if (NULL == cd) {
        return PMIX_ERR_NOMEM;
    }
    if (NULL != nspace) {
        cd->nspace = strdup(nspace);
    }
    cd->info = info;
    cd->ninfo = ninfo;
    cd->setupcbfunc = cbfunc;
    cd->cbdata = cbdata;
    PMIX_THREADSHIFT(cd, _setup_app);

    return PMIX_SUCCESS;
}

static void _setup_local_support(int sd, short args, void *cbdata)
{
    pmix_setup_caddy_t *cd = (pmix_setup_caddy_t *) cbdata;
    pmix_status_t rc;

    PMIX_ACQUIRE_OBJECT(cd);
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    /* pass to the network libraries */
    rc = pmix_pnet.setup_local_network(cd->nspace, cd->info, cd->ninfo);

    /* pass to the GPU libraries */
    if (PMIX_SUCCESS == rc) {
        rc = pmix_pgpu.setup_local(cd->nspace, cd->info, cd->ninfo);
    }

    /* pass the info back */
    if (NULL != cd->opcbfunc) {
        cd->opcbfunc(rc, cd->cbdata);
    }
    /* cleanup memory */
    if (NULL != cd->nspace) {
        free(cd->nspace);
    }
    PMIX_RELEASE(cd);
}

pmix_status_t PMIx_server_setup_local_support(const pmix_nspace_t nspace, pmix_info_t info[],
                                              size_t ninfo, pmix_op_cbfunc_t cbfunc, void *cbdata)
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

    /* need to threadshift this request */
    cd = PMIX_NEW(pmix_setup_caddy_t);
    if (NULL == cd) {
        return PMIX_ERR_NOMEM;
    }
    if (NULL != nspace) {
        cd->nspace = strdup(nspace);
    }
    cd->info = info;
    cd->ninfo = ninfo;
    cd->opcbfunc = cbfunc;
    cd->cbdata = cbdata;

    /* if the provided callback is NULL, then substitute
     * our own internal cbfunc and block here */
    if (NULL == cbfunc) {
        if (pmix_progress_thread_check_blocking("PMIx_server_setup_local_support")) {
            /* we are ON the progress thread, so waiting for the event we
             * would post is waiting for ourselves - answer rather than
             * hang. The caller wanted the blocking form; the non-blocking
             * one works fine from here. The nspace copy is ours, and the
             * handler that would have freed it is never going to run */
            if (NULL != cd->nspace) {
                free(cd->nspace);
            }
            PMIX_RELEASE(cd);
            return PMIX_ERR_WOULD_BLOCK;
        }
        PMIX_CONSTRUCT_LOCK(&mylock);
        cd->opcbfunc = pmix_server_lock_opcbfunc;
        cd->cbdata = &mylock;
        PMIX_THREADSHIFT(cd, _setup_local_support);
        PMIX_WAIT_THREAD(&mylock);
        rc = mylock.status;
        PMIX_DESTRUCT_LOCK(&mylock);
        if (PMIX_SUCCESS == rc) {
            rc = PMIX_OPERATION_SUCCEEDED;
        }
        return rc;
    }

    PMIX_THREADSHIFT(cd, _setup_local_support);

    return PMIX_SUCCESS;
}

PMIX_EXPORT pmix_status_t PMIx_generate_regex(const char *input, char **regexp)
{
    if (!pmix_atomic_check_bool(&pmix_globals.initialized)) {
        return PMIX_ERR_INIT;
    }

    return pmix_preg.generate_node_regex(input, regexp);
}

PMIX_EXPORT pmix_status_t PMIx_generate_regex2(const char *input,
                                               pmix_info_t info[], size_t ninfo,
                                               pmix_regex2_t *regex)
{
    if (!pmix_atomic_check_bool(&pmix_globals.initialized)) {
        return PMIX_ERR_INIT;
    }
    if (NULL == regex) {
        return PMIX_ERR_BAD_PARAM;
    }

    return pmix_preg.generate_regex(input, info, ninfo, regex);
}


PMIX_EXPORT pmix_status_t PMIx_parse_regex2(const pmix_regex2_t *regex,
                                            pmix_info_t info[], size_t ninfo,
                                            char **output)
{
    if (!pmix_atomic_check_bool(&pmix_globals.initialized)) {
        return PMIX_ERR_INIT;
    }
    if (NULL == regex || NULL == output) {
        return PMIX_ERR_BAD_PARAM;
    }

    return pmix_preg.parse_regex(regex, info, ninfo, output);
}

PMIX_EXPORT pmix_status_t PMIx_generate_ppn(const char *input, char **regexp)
{
    if (!pmix_atomic_check_bool(&pmix_globals.initialized)) {
        return PMIX_ERR_INIT;
    }

    return pmix_preg.generate_ppn(input, regexp);
}

pmix_status_t PMIx_server_generate_locality_string(const pmix_cpuset_t *cpuset, char **locality)
{
    pmix_status_t rc;

    if (!pmix_atomic_check_bool(&pmix_globals.initialized)) {
        return PMIX_ERR_INIT;
    }

    /* just pass this down */
    rc = pmix_hwloc_generate_locality_string(cpuset, locality);
    return rc;
}

pmix_status_t PMIx_server_generate_cpuset_string(const pmix_cpuset_t *cpuset, char **cpuset_string)
{
    pmix_status_t rc;

    if (!pmix_atomic_check_bool(&pmix_globals.initialized)) {
        return PMIX_ERR_INIT;
    }

    /* just pass this down */
    rc = pmix_hwloc_generate_cpuset_string(cpuset, cpuset_string);
    return rc;
}

pmix_status_t PMIx_server_generate_cpuset(const char *cpuset_string,
                                          pmix_cpuset_t *cpuset)
{
    pmix_status_t rc;

    if (!pmix_atomic_check_bool(&pmix_globals.initialized)) {
        return PMIX_ERR_INIT;
    }

    /* just pass this down */
    rc = pmix_hwloc_parse_cpuset_string(cpuset_string, cpuset);
    return rc;
}
