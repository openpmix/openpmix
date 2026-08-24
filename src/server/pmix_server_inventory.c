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

/* Collection of local hardware inventory for the host, and delivery of
 * the rolled-up result back down to us. */

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

#include "src/mca/pgpu/base/base.h"
#include "src/mca/pnet/base/base.h"
#include "src/runtime/pmix_progress_threads.h"
#include "src/util/pmix_error.h"
#include "src/util/pmix_output.h"

#include "pmix_server_ops.h"

static void cirelease(void *cbdata)
{
    pmix_shift_caddy_t *cd = (pmix_shift_caddy_t *) cbdata;

    PMIX_ACQUIRE_OBJECT(cd);

    if (NULL != cd->info) {
        PMIX_INFO_FREE(cd->info, cd->ninfo);
    }
    PMIX_RELEASE(cd);
}

static void clct(int sd, short args, void *cbdata)
{
    pmix_shift_caddy_t *cd = (pmix_shift_caddy_t*)cbdata;
    pmix_list_t inventory;
    pmix_data_array_t darray;
    pmix_status_t rc;

    PMIX_ACQUIRE_OBJECT(cd);
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    PMIX_CONSTRUCT(&inventory, pmix_list_t);

    /* collect the pnet inventory */
    rc = pmix_pnet.collect_inventory(cd->directives, cd->ndirs, &inventory);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto report;
    }

    /* collect the pgpu inventory */
    rc = pmix_pgpu.collect_inventory(cd->directives, cd->ndirs, &inventory);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto report;
    }

    /* convert list to an array of info */
    rc = PMIx_Info_list_convert((void*)&inventory, &darray);
    if (PMIX_ERR_EMPTY == rc) {
        rc = PMIX_SUCCESS;
    } else if (PMIX_SUCCESS == rc) {
        cd->info = (pmix_info_t*)darray.array;
        cd->ninfo = darray.size;
    } else {
        PMIX_ERROR_LOG(rc);
    }

report:
    if (NULL != cd->cbfunc.infocbfunc) {
        cd->cbfunc.infocbfunc(rc, cd->info, cd->ninfo, cd->cbdata, cirelease, cd);
    } else {
        /* no callback means nobody will ever call cirelease, so the caddy
         * and the info array converted into it are ours to give back */
        cirelease(cd);
    }
    PMIX_LIST_DESTRUCT(&inventory);
    return;
}

pmix_status_t PMIx_server_collect_inventory(pmix_info_t directives[], size_t ndirs,
                                            pmix_info_cbfunc_t cbfunc, void *cbdata)
{
    pmix_shift_caddy_t *cd;

    /* the *server* library has to be up, not merely some PMIx library:
     * clct fans out to pnet and pgpu, whose active-module lists are only
     * statically initialized until PMIx_server_init opens those
     * frameworks - see pmix_server_globals_t::initialized */
    if (!pmix_atomic_check_bool(&pmix_globals.initialized) ||
        !pmix_atomic_check_bool(&pmix_server_globals.initialized)) {
        return PMIX_ERR_INIT;
    }

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        return PMIX_ERR_NOT_AVAILABLE;
    }

    /* need to threadshift this request */
    cd = PMIX_NEW(pmix_shift_caddy_t);
    if (NULL == cd) {
        PMIX_ERROR_LOG(PMIX_ERR_NOMEM);
        return PMIX_ERR_NOMEM;
    }
    cd->directives = directives;
    cd->ndirs = ndirs;
    cd->cbfunc.infocbfunc = cbfunc;
    cd->cbdata = cbdata;
    PMIX_THREADSHIFT(cd, clct);

    return PMIX_SUCCESS;
}

static void dlinv(int sd, short args, void *cbdata)
{
    pmix_shift_caddy_t *cd = (pmix_shift_caddy_t *) cbdata;
    pmix_status_t rc;

    PMIX_ACQUIRE_OBJECT(cd);
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    rc = pmix_pnet.deliver_inventory(cd->info, cd->ninfo, cd->directives, cd->ndirs);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto report;
    }

    rc = pmix_pgpu.deliver_inventory(cd->info, cd->ninfo, cd->directives, cd->ndirs);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
    }

report:
    if (NULL != cd->cbfunc.opcbfn) {
        cd->cbfunc.opcbfn(rc, cd->cbdata);
    }
    PMIX_RELEASE(cd);
    return;
}

pmix_status_t PMIx_server_deliver_inventory(pmix_info_t info[], size_t ninfo,
                                            pmix_info_t directives[], size_t ndirs,
                                            pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    pmix_shift_caddy_t *cd;
    pmix_lock_t mylock;
    pmix_status_t rc;

    /* same reasoning as PMIx_server_collect_inventory above: dlinv fans
     * out to pnet and pgpu, which only PMIx_server_init stands up */
    if (!pmix_atomic_check_bool(&pmix_globals.initialized) ||
        !pmix_atomic_check_bool(&pmix_server_globals.initialized)) {
        return PMIX_ERR_INIT;
    }

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        return PMIX_ERR_NOT_AVAILABLE;
    }

    /* need to threadshift this request */
    cd = PMIX_NEW(pmix_shift_caddy_t);
    if (NULL == cd) {
        PMIX_ERROR_LOG(PMIX_ERR_NOMEM);
        return PMIX_ERR_NOMEM;
    }
    cd->lock.active = false;
    cd->info = info;
    cd->ninfo = ninfo;
    cd->directives = directives;
    cd->ndirs = ndirs;
    cd->cbfunc.opcbfn = cbfunc;
    cd->cbdata = cbdata;

    /* if the provided callback is NULL, then substitute
     * our own internal cbfunc and block here */
    if (NULL == cbfunc) {
        if (pmix_progress_thread_check_blocking("PMIx_server_deliver_inventory")) {
            /* we are ON the progress thread, so waiting for the event we
             * would post is waiting for ourselves - answer rather than
             * hang. The caller wanted the blocking form; the non-blocking
             * one works fine from here */
            PMIX_RELEASE(cd);
            return PMIX_ERR_WOULD_BLOCK;
        }
        PMIX_CONSTRUCT_LOCK(&mylock);
        cd->cbfunc.opcbfn = pmix_server_lock_opcbfunc;
        cd->cbdata = &mylock;
        PMIX_THREADSHIFT(cd, dlinv);
        PMIX_WAIT_THREAD(&mylock);
        rc = mylock.status;
        PMIX_DESTRUCT_LOCK(&mylock);
        if (PMIX_SUCCESS == rc) {
            rc = PMIX_OPERATION_SUCCEEDED;
        }
        return rc;
    }

    PMIX_THREADSHIFT(cd, dlinv);

    return PMIX_SUCCESS;
}
