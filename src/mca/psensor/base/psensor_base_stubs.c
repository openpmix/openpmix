/*
 * Copyright (c) 2010      Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2012      Los Alamos National Security, Inc. All rights reserved.
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
 *
 * Copyright (c) 2021-2026 Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "src/include/pmix_config.h"
#include "pmix_common.h"

#include "src/include/pmix_globals.h"
#include "src/util/pmix_error.h"
#include "src/util/pmix_output.h"

#include "src/mca/psensor/base/base.h"

pmix_status_t pmix_psensor_base_start(pmix_peer_t *requestor, pmix_status_t error,
                                      const pmix_info_t *monitor, const pmix_info_t directives[],
                                      size_t ndirs)
{
    pmix_psensor_active_module_t *mod;
    pmix_status_t rc;
    bool serviced = false;

    pmix_output_verbose(5, pmix_psensor_base_framework.framework_output,
                        "%s:%d sensor:base: starting sensors", pmix_globals.myid.nspace,
                        pmix_globals.myid.rank);

    /* offer the request to the start function of all modules in
     * priority order. A module that recognizes the monitor claims it by
     * returning PMIX_SUCCESS; one that does not declines with
     * PMIX_ERR_TAKE_NEXT_OPTION so the next module gets a chance. */
    PMIX_LIST_FOREACH (mod, &pmix_psensor_base.actives, pmix_psensor_active_module_t) {
        if (NULL == mod->module->start) {
            continue;
        }
        rc = mod->module->start(requestor, error, monitor, directives, ndirs);
        if (PMIX_SUCCESS == rc) {
            /* a module claimed and started the request */
            serviced = true;
        } else if (PMIX_ERR_TAKE_NEXT_OPTION != rc) {
            /* the module claimed the request but hit a hard error */
            return rc;
        }
    }

    /* if no module could service the request, report not supported
     * upwards so the server knows to ask the host to try. This matches
     * the convention pstat uses (its unsupported module returns
     * PMIX_ERR_NOT_SUPPORTED) when nothing can meet the request. */
    if (!serviced) {
        return PMIX_ERR_NOT_SUPPORTED;
    }

    return PMIX_SUCCESS;
}

pmix_status_t pmix_psensor_base_stop(pmix_peer_t *requestor, char *id)
{
    pmix_psensor_active_module_t *mod;
    pmix_status_t rc, ret = PMIX_SUCCESS;

    pmix_output_verbose(5, pmix_psensor_base_framework.framework_output,
                        "%s:%d sensor:base: stopping sensors", pmix_globals.myid.nspace,
                        pmix_globals.myid.rank);

    /* call the stop function of all modules in priority order */
    PMIX_LIST_FOREACH (mod, &pmix_psensor_base.actives, pmix_psensor_active_module_t) {
        if (NULL != mod->module->stop) {
            rc = mod->module->stop(requestor, id);
            if (PMIX_SUCCESS != rc && PMIX_ERR_TAKE_NEXT_OPTION != rc) {
                if (PMIX_SUCCESS == ret) {
                    ret = rc;
                }
                /* need to continue to ensure that all
                 * sensors have been stopped */
            }
        }
    }

    return ret;
}

/* one peer's count of live monitors */
typedef struct {
    pmix_list_item_t super;
    pmix_peer_t *peer; // not retained - each claim's tracker holds it
    int count;
} psensor_claim_t;
static PMIX_CLASS_INSTANCE(psensor_claim_t, pmix_list_item_t, NULL, NULL);

pmix_status_t pmix_psensor_base_claim(pmix_peer_t *peer)
{
    psensor_claim_t *cl, *found = NULL;
    pmix_status_t rc = PMIX_SUCCESS;

    pmix_mutex_lock(&pmix_psensor_base.lock);
    PMIX_LIST_FOREACH (cl, &pmix_psensor_base.claims, psensor_claim_t) {
        if (cl->peer == peer) {
            found = cl;
            break;
        }
    }
    if (NULL == found) {
        found = PMIX_NEW(psensor_claim_t);
        if (NULL == found) {
            rc = PMIX_ERR_NOMEM;
            goto done;
        }
        found->peer = peer;
        found->count = 0;
        pmix_list_append(&pmix_psensor_base.claims, &found->super);
    }
    if (found->count >= pmix_psensor_base.max_per_peer) {
        pmix_output_verbose(2, pmix_psensor_base_framework.framework_output,
                            "psensor: %s already holds %d monitors - refused",
                            PMIX_PEER_PRINT(peer), found->count);
        if (0 == found->count) {
            pmix_list_remove_item(&pmix_psensor_base.claims, &found->super);
            PMIX_RELEASE(found);
        }
        rc = PMIX_ERR_OUT_OF_RESOURCE;
        goto done;
    }
    ++found->count;

done:
    pmix_mutex_unlock(&pmix_psensor_base.lock);
    return rc;
}

void pmix_psensor_base_unclaim(pmix_peer_t *peer)
{
    psensor_claim_t *cl;

    pmix_mutex_lock(&pmix_psensor_base.lock);
    PMIX_LIST_FOREACH (cl, &pmix_psensor_base.claims, psensor_claim_t) {
        if (cl->peer == peer) {
            if (0 == --cl->count) {
                pmix_list_remove_item(&pmix_psensor_base.claims, &cl->super);
                PMIX_RELEASE(cl);
            }
            break;
        }
    }
    pmix_mutex_unlock(&pmix_psensor_base.lock);
}
