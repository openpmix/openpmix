/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2018-2020 Intel, Inc.  All rights reserved.
 *
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "src/include/pmix_config.h"

#include "pmix_common.h"
#include "src/include/pmix_globals.h"

#include "src/class/pmix_list.h"
#include "src/server/pmix_server_ops.h"
#include "src/util/pmix_error.h"
#include "src/util/pmix_show_help.h"

#include "src/mca/plog/base/base.h"

/* this takes place in a progress event */
pmix_status_t pmix_plog_base_log(const pmix_proc_t *source,
                                 const pmix_info_t data[], size_t ndata,
                                 const pmix_info_t directives[], size_t ndirs)
{
    pmix_plog_base_active_module_t *active;
    pmix_status_t rc = PMIX_ERR_NOT_AVAILABLE;
    size_t n, k;
    int m;
    bool logonce = false;
    pmix_list_t channels;
    char *key = NULL, *val = NULL;
    bool agg = true;  // default to aggregating show_help messages
    pmix_info_t *dt = (pmix_info_t*)data;

    if (!pmix_plog_globals.initialized) {
        return PMIX_ERR_INIT;
    }

    /* if there is no data to output, then nothing to do */
    if (NULL == data) {
        return PMIX_OPERATION_SUCCEEDED;
    }

    pmix_output_verbose(2, pmix_plog_base_framework.framework_output,
                        "plog:log called");

    /* initialize the list of channels */
    PMIX_CONSTRUCT(&channels, pmix_list_t);

    if (NULL != directives) {
        /* scan the directives for the PMIX_LOG_ONCE attribute
         * which indicates we should stop with the first log
         * channel that can successfully handle this request,
         * and any channel directives */
        for (n = 0; n < ndirs; n++) {
            if (PMIX_CHECK_KEY(&directives[n], PMIX_LOG_ONCE)) {
                logonce = PMIX_INFO_TRUE(&directives[n]);
            }
            else if (PMIX_CHECK_KEY(&directives[n], PMIX_LOG_AGG)) {
                    agg = PMIX_INFO_TRUE(&directives[n]);
            }
            else if (PMIX_CHECK_KEY(&directives[n], PMIX_LOG_KEY)) {
                key = directives[n].value.data.string;
            }
            else if (PMIX_CHECK_KEY(&directives[n], PMIX_LOG_VAL)) {
                val = directives[n].value.data.string;
            }
        }
        if (agg && NULL != key && NULL != val) {
            if (PMIX_SUCCESS == pmix_help_check_dups(key, val)) {
                for (k = 0; k < ndata; k++) {
                    // This is a dup and has been tracked as such,
                    // mark this as complete so we don't log it again.
                    PMIX_INFO_OP_COMPLETED(&dt[k]);
                }
            }
        }
    }

    /* scan the incoming logging requests and assemble the modules in
     * the corresponding order - this will ensure that the one they
     * requested first gets first shot at "log once" */
    for (n = 0; n < ndata; n++) {
        if (PMIX_INFO_OP_IS_COMPLETE(&data[n])) {
            continue;
        }
        for (m = 0; m < pmix_plog_globals.actives.size; m++) {
            active = (pmix_plog_base_active_module_t *)
                            pmix_pointer_array_get_item(&pmix_plog_globals.actives, m);
            if (NULL == active) {
                continue;
            }
            /* if this channel is included in the ones serviced by this
             * module, then include the module */
            if (NULL == active->module->channels) {
                if (!active->added) {
                    /* add this channel to the list */
                    pmix_list_append(&channels, &active->super);
                    /* mark it as added */
                    active->added = true;
                }
            } else {
                for (k = 0; NULL != active->module->channels[k]; k++) {
                    if (NULL != strstr(data[n].key, active->module->channels[k])) {
                        if (!active->added) {
                            /* add this channel to the list */
                            pmix_list_append(&channels, &active->super);
                            /* mark it as added */
                            active->added = true;
                            break;
                        }
                    }
                }
            }
        }
    }

    // do we have anything we need to do?
    if (0 == pmix_list_get_size(&channels)) {
        /* nothing we need do - clear the channels list but don't
         * release the members */
        while (NULL != pmix_list_remove_first(&channels));
        PMIX_DESTRUCT(&channels);

        // Don't return PMIX_SUCCESS here, or else the called
        // will expect the cbfunc to be executed (which it won't be.).
        return PMIX_OPERATION_SUCCEEDED;
    }

    /* reset the added marker for the next time we are called */
    PMIX_LIST_FOREACH (active, &channels, pmix_plog_base_active_module_t) {
        active->added = false;
    }

    // cycle through the channels and let them log
    PMIX_LIST_FOREACH (active, &channels, pmix_plog_base_active_module_t) {
        if (NULL != active->module->log) {

            rc = active->module->log(source, data, ndata, directives, ndirs);

            if (PMIX_SUCCESS == rc || PMIX_OPERATION_SUCCEEDED == rc) {
                // if we are only logging once, then we are done
                if (logonce) {
                    break;
                }
            } else if (PMIX_ERR_NOT_AVAILABLE == rc ||
                       PMIX_ERR_TAKE_NEXT_OPTION == rc) {
                // ignore this module
                continue;
            } else {
                // hit a real error - abort
                break;
            }
        }
    }

    /* cannot release the modules - just remove everything from the list */
    while (NULL != pmix_list_remove_first(&channels));
    PMIX_DESTRUCT(&channels);

    return rc;
}
