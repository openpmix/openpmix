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
    /* the status handed back to the host. It is kept separate from "rc"
     * because "rc" is the transient status of whichever sub-operation ran
     * last: a later success must not erase an earlier failure, and the
     * job-info unpack loop below ends on
     * PMIX_ERR_UNPACK_READ_PAST_END_OF_BUFFER by design - reporting that
     * as the result told the host every registration carrying job info
     * had failed */
    pmix_status_t ret = PMIX_SUCCESS;
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

    PMIX_ACQUIRE_OBJECT(cd);
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    PMIX_CONSTRUCT(&grpinfo, pmix_list_t);
    PMIX_CONSTRUCT(&endpts, pmix_list_t);
    for (n = 0; n < cd->ninfo; n++) {
        if (PMIX_CHECK_KEY(&cd->info[n], PMIX_GROUP_INFO_ARRAY) ||
            PMIX_CHECK_KEY(&cd->info[n], PMIX_GROUP_INFO)) {
            /* both spellings are walked below as an array of pmix_info_t,
             * and the key alone does not make the union an array - see
             * pmix_server_valid_darray */
            if (!pmix_server_valid_darray(&cd->info[n], PMIX_INFO, 1)) {
                PMIX_ERROR_LOG(PMIX_ERR_TYPE_MISMATCH);
                if (PMIX_SUCCESS == ret) {
                    ret = PMIX_ERR_TYPE_MISMATCH;
                }
                continue;
            }
            g = PMIX_NEW(pmix_info_caddy_t);
            if (NULL == g) {
                ret = PMIX_ERR_NOMEM;
                goto release;
            }
            g->info = &cd->info[n];
            pmix_list_append(&grpinfo, &g->super);

        } else if (PMIX_CHECK_KEY(&cd->info[n], PMIX_GROUP_ENDPT_DATA)) {
            /* the procID and the scope occupy the first two positions of
             * the array, so anything shorter than that describes nothing
             * and indexing it would read past the array the host gave us */
            if (!pmix_server_valid_darray(&cd->info[n], PMIX_INFO, 2)) {
                PMIX_ERROR_LOG(PMIX_ERR_TYPE_MISMATCH);
                if (PMIX_SUCCESS == ret) {
                    ret = PMIX_ERR_TYPE_MISMATCH;
                }
                continue;
            }
            ept = PMIX_NEW(pmix_info_caddy_t);
            if (NULL == ept) {
                ret = PMIX_ERR_NOMEM;
                goto release;
            }
            ept->info = &cd->info[n];
            ept->ninfo = 1;
            pmix_list_append(&endpts, &ept->super);

        } else if (PMIX_CHECK_KEY(&cd->info[n], PMIX_GROUP_CONTEXT_ID)) {
            rc = PMIx_Value_get_number(&cd->info[n].value, &ctxid, PMIX_SIZE);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                if (PMIX_SUCCESS == ret) {
                    ret = rc;
                }
            } else {
                gotctxid = true;
            }

        } else if (PMIX_CHECK_KEY(&cd->info[n], PMIX_GROUP_JOB_INFO)) {
            if (PMIX_BYTE_OBJECT != cd->info[n].value.type) {
                PMIX_ERROR_LOG(PMIX_ERR_TYPE_MISMATCH);
                if (PMIX_SUCCESS == ret) {
                    ret = PMIX_ERR_TYPE_MISMATCH;
                }
                continue;
            }
            pbo = &cd->info[n].value.data.bo;

        } else {
            /* add any provided data to our global cache for all nspaces */
            kv = PMIX_NEW(pmix_kval_t);
            if (NULL == kv) {
                ret = PMIX_ERR_NOMEM;
                goto release;
            }
            kv->key = strdup(cd->info[n].key);
            kv->value = (pmix_value_t *) malloc(sizeof(pmix_value_t));
            if (NULL == kv->key || NULL == kv->value) {
                /* the value is raw malloc'd memory until the transfer
                 * below populates it, and the kval destructor would run
                 * value_destruct over whatever the heap happened to hold
                 * there - so it has to go back by hand */
                if (NULL != kv->value) {
                    free(kv->value);
                    kv->value = NULL;
                }
                PMIX_RELEASE(kv);
                ret = PMIX_ERR_NOMEM;
                goto release;
            }
            PMIX_VALUE_XFER(rc, kv->value, &cd->info[n].value);
            if (PMIX_SUCCESS != rc) {
                PMIX_RELEASE(kv);
                ret = rc;
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
            /* the array itself was screened when it was collected above,
             * so it is known to hold at least the two leading elements */
            pinfo = (pmix_info_t*)ept->info->value.data.darray->array;
            npinfo = ept->info->value.data.darray->size;
            /* procID is in the first position and the scope in the second,
             * but the position alone does not make the union a proc
             * pointer - a mistyped first element would be dereferenced by
             * the store below as whatever address the host had there */
            if (PMIX_PROC != pinfo[0].value.type ||
                NULL == pinfo[0].value.data.proc ||
                PMIX_SCOPE != pinfo[1].value.type) {
                PMIX_ERROR_LOG(PMIX_ERR_TYPE_MISMATCH);
                if (PMIX_SUCCESS == ret) {
                    ret = PMIX_ERR_TYPE_MISMATCH;
                }
                continue;
            }
            proc = pinfo[0].value.data.proc;
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
                    if (PMIX_SUCCESS == ret) {
                        ret = rc;
                    }
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
            PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
            ret = PMIX_ERR_BAD_PARAM;
            goto release;
        }
        /* Each list member points to a pmix_info_t that contains
         * a data array of info from a given proc */
        PMIX_LIST_FOREACH(g, &grpinfo, pmix_info_caddy_t) {
            /* screened when it was collected above */
            iptr = (pmix_info_t*)g->info->value.data.darray->array;
            ninfo = g->info->value.data.darray->size;

            if (PMIX_CHECK_KEY(g->info, PMIX_GROUP_INFO)) {
                // this is just a single array of group info
                rc = pmix_server_process_grpinfo(ctxid, iptr, ninfo);
                if (PMIX_SUCCESS != rc) {
                    ret = rc;
                    goto release;
                }
            } else {
                // contains an array of group info arrays
                for (n=0; n < ninfo; n++) {
                    /* each element is itself an array of group info - the
                     * enclosing array's element type says nothing about
                     * what its members carry */
                    if (!pmix_server_valid_darray(&iptr[n], PMIX_INFO, 1)) {
                        PMIX_ERROR_LOG(PMIX_ERR_TYPE_MISMATCH);
                        ret = PMIX_ERR_TYPE_MISMATCH;
                        goto release;
                    }
                    pinfo = (pmix_info_t*)iptr[n].value.data.darray->array;
                    npinfo = iptr[n].value.data.darray->size;
                    rc = pmix_server_process_grpinfo(ctxid, pinfo, npinfo);
                    if (PMIX_SUCCESS != rc) {
                        ret = rc;
                        goto release;
                    }
                }
            }
        }
    }

    // if job info was provided, process it
    if (NULL != pbo) {
        /* Load it for unpacking, but do NOT take it: the byte object sits
         * in the caller's own info array, which the host owns and keeps
         * valid until our callback fires. PMIX_LOAD_BUFFER does not copy -
         * it points the buffer at the payload and NULLs the source - so
         * using it here emptied the host's info element and then leaked
         * the blob, since this buffer is never destructed (destructing it
         * would free memory the host still owns). Borrow instead. */
        PMIX_CONSTRUCT(&jobinfo, pmix_buffer_t);
        PMIX_LOAD_BUFFER_NON_DESTRUCT(pmix_globals.mypeer, &jobinfo,
                                      pbo->bytes, pbo->size);

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
                /* this ends the walk, so the check below the loop is what
                 * logs it and records it - doing either here as well
                 * reports the same failure twice */
                PMIX_DESTRUCT(&bkt);
                break;
            }
            /* extract and process any proc-related info for this nspace */
            PMIX_GDS_STORE_JOB_INFO(rc, pmix_globals.mypeer, nspace, &bkt);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                if (PMIX_SUCCESS == ret) {
                    ret = rc;
                }
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
        /* running the buffer dry is how this loop ends when every blob was
         * consumed, so it is not a failure to report */
        if (PMIX_ERR_UNPACK_READ_PAST_END_OF_BUFFER != rc) {
            PMIX_ERROR_LOG(rc);
            if (PMIX_SUCCESS == ret) {
                ret = rc;
            }
        }
    }

release:
    /* both scratch lists own the caddies appended to them above and were
     * being dropped on the floor - on the normal path as well as this one */
    PMIX_LIST_DESTRUCT(&grpinfo);
    PMIX_LIST_DESTRUCT(&endpts);
    cd->opcbfunc(ret, cd->cbdata);
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

    /* the handler walks the array "ninfo" times */
    if (0 < ninfo && NULL == info) {
        return PMIX_ERR_BAD_PARAM;
    }

    cd = PMIX_NEW(pmix_setup_caddy_t);
    if (NULL == cd) {
        return PMIX_ERR_NOMEM;
    }
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
    pmix_kval_t *kv, *knext;
    size_t n;

    PMIX_ACQUIRE_OBJECT(cd);
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    /* Find any matches in our global cache and remove them - the man page
     * says "each matching entry" is deleted. The cache can legitimately
     * hold more than one entry for a key: _register_resources appends
     * without checking, so a host that re-registers a key to update its
     * value leaves both behind, and the gds walk of this list stores them
     * in order, so the later one is the one in effect. Stopping at the
     * first match therefore removed the entry that was being shadowed and
     * left the one the datastore was actually using: the deregistration
     * silently did nothing. Clear every match, using the SAFE variant since
     * the matching item is released inside the walk.
     *
     * Note this matches on the key alone. The man page also describes
     * narrowing a removal with qualifiers - e.g. a PMIX_NODE_INFO_ARRAY
     * naming one node's fabric device - and that is not implemented here,
     * so such a request removes every entry carrying that key rather than
     * the one the qualifiers describe. */
    for (n = 0; n < cd->ninfo; n++) {
        PMIX_LIST_FOREACH_SAFE (kv, knext, &pmix_server_globals.gdata, pmix_kval_t) {
            if (PMIX_CHECK_KEY(kv, cd->info[n].key)) {
                pmix_list_remove_item(&pmix_server_globals.gdata, &kv->super);
                PMIX_RELEASE(kv);
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

    /* the handler walks the array "ninfo" times */
    if (0 < ninfo && NULL == info) {
        return PMIX_ERR_BAD_PARAM;
    }

    cd = PMIX_NEW(pmix_setup_caddy_t);
    if (NULL == cd) {
        return PMIX_ERR_NOMEM;
    }
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
            rc = PMIx_Value_xfer(&fcd->info[n].value, kv->value);
            if (PMIX_SUCCESS != rc) {
                /* the array would go up to the host with a hole in it and
                 * nothing to say which element was never filled in */
                PMIX_ERROR_LOG(rc);
                _setup_op(rc, fcd);
                fcd = NULL;
                goto depart;
            }
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

/* The preg components take these arguments at face value - the raw one
 * hands "input" straight to strncmp/strdup and writes through "regexp"
 * without looking - so the public entry points screen them here, the same
 * way the regex2 pair below always has. */
PMIX_EXPORT pmix_status_t PMIx_generate_regex(const char *input, char **regexp)
{
    if (!pmix_atomic_check_bool(&pmix_globals.initialized)) {
        return PMIX_ERR_INIT;
    }
    if (NULL == input || NULL == regexp) {
        return PMIX_ERR_BAD_PARAM;
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
    if (NULL == input || NULL == regex) {
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
    if (NULL == input || NULL == regexp) {
        return PMIX_ERR_BAD_PARAM;
    }

    return pmix_preg.generate_ppn(input, regexp);
}

pmix_status_t PMIx_server_generate_locality_string(const pmix_cpuset_t *cpuset, char **locality)
{
    pmix_status_t rc;

    if (!pmix_atomic_check_bool(&pmix_globals.initialized)) {
        return PMIX_ERR_INIT;
    }
    /* hwloc screens the cpuset, but reports a bad one by writing NULL
     * through the output pointer - so that one has to be screened here */
    if (NULL == locality) {
        return PMIX_ERR_BAD_PARAM;
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
    /* same as above: a bad cpuset is reported by writing NULL through the
     * output pointer, so hwloc cannot be the one to screen it */
    if (NULL == cpuset_string) {
        return PMIX_ERR_BAD_PARAM;
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
