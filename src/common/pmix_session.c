/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2016      Mellanox Technologies, Inc.
 *                         All rights reserved.
 * Copyright (c) 2016-2022 IBM Corporation.  All rights reserved.
 * Copyright (c) 2019      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2021-2023 Nanook Consulting.  All rights reserved.
 * Copyright (c) 2022      Triad National Security, LLC. All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */
#include "src/include/pmix_config.h"

#include "src/include/pmix_socket_errno.h"
#include "src/include/pmix_stdint.h"

#include "include/pmix.h"
#include "pmix_common.h"
#include "include/pmix_server.h"

#include "src/common/pmix_attributes.h"
#include "src/mca/bfrops/bfrops.h"
#include "src/mca/pstrg/pstrg.h"
#include "src/mca/ptl/base/base.h"
#include "src/threads/pmix_threads.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_error.h"
#include "src/util/pmix_name_fns.h"
#include "src/util/pmix_output.h"

#include "src/client/pmix_client_ops.h"
#include "src/include/pmix_globals.h"
#include "src/server/pmix_server_ops.h"

static void myinfocbfunc(pmix_status_t status,
                         pmix_info_t *info, size_t ninfo,
                         void *cbdata,
                         pmix_release_cbfunc_t release_fn,
                         void *release_cbdata)
{
    pmix_lock_t *lock = (pmix_lock_t *) cbdata;
    PMIX_HIDE_UNUSED_PARAMS(info, ninfo, release_fn, release_cbdata);

    lock->status = status;
    PMIX_WAKEUP_THREAD(lock);
}

static void _session_control(int sd, short args, void *cbdata)
{
    pmix_shift_caddy_t *cd = (pmix_shift_caddy_t *) cbdata;

    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    cd->cbfunc.infocbfunc(PMIX_ERR_NOT_SUPPORTED, NULL, 0, cd->cbdata, NULL, NULL);
    PMIX_RELEASE(cd);
}

pmix_status_t PMIx_Session_control(uint32_t sessionID,
                                   const pmix_info_t directives[], size_t ndirs,
                                   pmix_info_cbfunc_t cbfunc, void *cbdata)
{
    pmix_shift_caddy_t *cd;
    pmix_lock_t mylock;
    pmix_status_t rc;

    pmix_output_verbose(2, pmix_server_globals.base_output,
                        "pmix:server session control");

    PMIX_ACQUIRE_THREAD(&pmix_global_lock);
    if (pmix_globals.init_cntr <= 0) {
        PMIX_RELEASE_THREAD(&pmix_global_lock);
        return PMIX_ERR_INIT;
    }
    PMIX_RELEASE_THREAD(&pmix_global_lock);

    cd = PMIX_NEW(pmix_shift_caddy_t);
    cd->sessionid = sessionID;
    cd->directives = (pmix_info_t*)directives;
    cd->ndirs = ndirs;
    cd->cbfunc.infocbfunc = cbfunc;
    cd->cbdata = cbdata;

    /* if the provided callback is NULL, then substitute
     * our own internal cbfunc and block here */
    if (NULL == cbfunc) {
        PMIX_CONSTRUCT_LOCK(&mylock);
        cd->cbfunc.infocbfunc = myinfocbfunc;
        cd->cbdata = &mylock;
        PMIX_THREADSHIFT(cd, _session_control);
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
    PMIX_THREADSHIFT(cd, _session_control);
    return PMIX_SUCCESS;
}
