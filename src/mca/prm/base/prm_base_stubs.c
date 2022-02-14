/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2015-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2016      Mellanox Technologies, Inc.
 *                         All rights reserved.
 * Copyright (c) 2018      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 *
 * Copyright (c) 2021-2022 Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "src/include/pmix_config.h"

#include <string.h>
#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif
#ifdef HAVE_SYS_TYPES_H
#    include <sys/types.h>
#endif
#ifdef HAVE_SYS_STAT_H
#    include <sys/stat.h>
#endif
#ifdef HAVE_FCNTL_H
#    include <fcntl.h>
#endif
#ifdef HAVE_SYS_UTSNAME_H
#    include <sys/utsname.h>
#endif
#include <time.h>

#include "pmix_common.h"
#include "src/include/pmix_globals.h"

#include "src/class/pmix_list.h"
#include "src/mca/preg/preg.h"
#include "src/server/pmix_server_ops.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_error.h"
#include "src/util/pmix_environ.h"

#include "src/mca/prm/base/base.h"

static void cicbfunc(pmix_status_t status, void *cbdata)
{
    pmix_prm_rollup_t *rollup = (pmix_prm_rollup_t *) cbdata;

    PMIX_ACQUIRE_THREAD(&rollup->lock);
    /* check if they had an error */
    if (PMIX_SUCCESS != status && PMIX_SUCCESS == rollup->status) {
        rollup->status = status;
    }
    /* record that we got a reply */
    rollup->replies++;
    /* see if all have replied */
    if (rollup->replies < rollup->requests) {
        /* nope - need to wait */
        PMIX_RELEASE_THREAD(&rollup->lock);
        return;
    }

    /* if we get here, then operation is complete */
    PMIX_RELEASE_THREAD(&rollup->lock);
    if (NULL != rollup->cbfunc) {
        rollup->cbfunc(rollup->status, rollup->cbdata);
    }
    PMIX_RELEASE(rollup);
    return;
}

pmix_status_t pmix_prm_base_notify(pmix_status_t status, const pmix_proc_t *source,
                                   pmix_data_range_t range, const pmix_info_t info[], size_t ninfo,
                                   pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    pmix_prm_base_active_module_t *active;
    pmix_prm_rollup_t *myrollup;
    pmix_status_t rc;

    /* we cannot block here as each plugin could take some time to
     * complete the request. So instead, we call each active plugin
     * and get their immediate response - if "in progress", then
     * we record that we have to wait for their answer before providing
     * the caller with a response. If "error", then we know we
     * won't be getting a response from them */

    if (!pmix_prm_globals.initialized) {
        return PMIX_ERR_INIT;
    }

    /* create the rollup object */
    myrollup = PMIX_NEW(pmix_prm_rollup_t);
    if (NULL == myrollup) {
        return PMIX_ERR_NOMEM;
    }
    myrollup->cbfunc = cbfunc;
    myrollup->cbdata = cbdata;

    /* hold the lock until all active modules have been called
     * to avoid race condition where replies come in before
     * the requests counter has been fully updated */
    PMIX_ACQUIRE_THREAD(&myrollup->lock);

    PMIX_LIST_FOREACH (active, &pmix_prm_globals.actives, pmix_prm_base_active_module_t) {
        if (NULL != active->module->notify) {
            pmix_output_verbose(5, pmix_prm_base_framework.framework_output, "NOTIFYING %s",
                                active->module->name);
            rc = active->module->notify(status, source, range, info, ninfo, cicbfunc,
                                        (void *) myrollup);
            /* if they return succeeded, then nothing
             * to wait for here */
            if (PMIX_OPERATION_IN_PROGRESS == rc) {
                myrollup->requests++;
            } else if (PMIX_OPERATION_SUCCEEDED == rc || PMIX_ERR_TAKE_NEXT_OPTION == rc
                       || PMIX_ERR_NOT_SUPPORTED == rc) {
                continue;
            } else {
                /* a true error - we need to wait for
                 * all pending requests to complete
                 * and then notify the caller of the error */
                if (PMIX_SUCCESS == myrollup->status) {
                    myrollup->status = rc;
                }
            }
        }
    }
    if (0 == myrollup->requests) {
        /* report back */
        PMIX_RELEASE_THREAD(&myrollup->lock);
        rc = myrollup->status;
        PMIX_RELEASE(myrollup);
        return PMIX_OPERATION_SUCCEEDED;
    }

    PMIX_RELEASE_THREAD(&myrollup->lock);
    /* return success to indicate we are processing it */
    return PMIX_SUCCESS;
}

int pmix_prm_base_get_remaining_time(uint32_t *timeleft)
{
    pmix_prm_base_active_module_t *active;
    pmix_status_t rc;

    PMIX_LIST_FOREACH(active, &pmix_prm_globals.actives, pmix_prm_base_active_module_t) {
        if (NULL != active->module->get_remaining_time) {
            rc = active->module->get_remaining_time(timeleft);
            if (PMIX_SUCCESS == rc) {
                return rc;
            } else if (PMIX_ERR_TAKE_NEXT_OPTION != rc) {
                return rc;
            }
        }
    }

    return PMIX_ERR_NOT_SUPPORTED;
}
