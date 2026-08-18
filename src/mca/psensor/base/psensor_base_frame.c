/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2010      Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2012-2013 Los Alamos National Security, Inc. All rights reserved.
 * Copyright (c) 2017-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2020      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "src/include/pmix_config.h"

#include "pmix_common.h"

#include <pthread.h>
#include <event.h>

#include "src/class/pmix_list.h"
#include "src/include/pmix_types.h"
#include "src/mca/base/pmix_base.h"
#include "src/mca/mca.h"
#include "src/runtime/pmix_progress_threads.h"

#include "src/mca/psensor/base/base.h"

/*
 * The following file was created by configure.  It contains extern
 * statements and the definition of an array of pointers to each
 * component's public mca_base_component_t struct.
 */

#include "src/mca/psensor/base/static-components.h"

/*
 * Global variables
 */
pmix_psensor_base_module_t pmix_psensor = {
    .start = pmix_psensor_base_start,
    .stop = pmix_psensor_base_stop
};

pmix_psensor_base_t pmix_psensor_base = {
    .actives = PMIX_LIST_STATIC_INIT,
    .evbase = NULL,
    .selected = false
};

static bool use_separate_thread = false;

static int pmix_psensor_register(pmix_mca_base_register_flag_t flags)
{
    (void) flags;
    (void) pmix_mca_base_var_register("pmix", "psensor", "base", "use_separate_thread",
                                      "Use a separate thread for monitoring local procs",
                                      PMIX_MCA_BASE_VAR_TYPE_BOOL,
                                      &use_separate_thread);
    return PMIX_SUCCESS;
}

static int pmix_psensor_base_close(void)
{
    int rc;

    pmix_psensor_base.selected = false;

    /* Quiesce the monitor thread before anything is torn down, but keep
     * its event base alive until the components have let go of it. Both
     * halves matter, and they are why this is a pause here and a stop
     * further down rather than one call:
     *
     * - every live tracker carries a timer armed on this base, and the
     *   trackers are destructed by the component close below - which runs
     *   on the finalizing thread, not on this base's. pmix_event_del does
     *   not wait for a callback it did not race, so tearing the trackers
     *   down while the thread still runs can free one out from under the
     *   sampler executing on it. Pausing joins the thread, so nothing is
     *   in flight once it returns.
     * - the base cannot be freed yet either, because the tracker
     *   destructors delete their timers and pmix_event_del reads the base
     *   out of the event.
     *
     * In the default configuration there is no separate thread to pause:
     * evbase is the library's shared base, which PMIx_server_finalize has
     * already stopped before it closes any framework. */
    if (use_separate_thread && NULL != pmix_psensor_base.evbase) {
        (void) pmix_progress_thread_pause("PSENSOR");
    }

    PMIX_LIST_DESTRUCT(&pmix_psensor_base.actives);

    /* Close all remaining available components. Each component's close
     * destructs its tracker list, and a tracker destructor deletes the
     * timer it armed on evbase - so this has to happen while the base is
     * still there. */
    rc = pmix_mca_base_framework_components_close(&pmix_psensor_base_framework, NULL);

    /* Now the base can go. Do NOT free it here as well: this drops the
     * last reference to the "PSENSOR" tracker, and the tracker's
     * destructor is what calls pmix_event_base_free. */
    if (use_separate_thread && NULL != pmix_psensor_base.evbase) {
        (void) pmix_progress_thread_stop("PSENSOR");
    }
    /* clear the pointer either way: pmix_psensor.stop remains callable
     * after the framework closes (the server calls it from its client
     * teardown paths), and it must not post a caddy to a base that has
     * been freed, or to the library's base after finalize */
    pmix_psensor_base.evbase = NULL;

    return rc;
}

/**
 * Function for finding and opening either all MCA components, or the one
 * that was specifically requested via a MCA parameter.
 */
static int pmix_psensor_base_open(pmix_mca_base_open_flag_t flags)
{
    int rc;

    /* construct the list of modules */
    PMIX_CONSTRUCT(&pmix_psensor_base.actives, pmix_list_t);

    if (use_separate_thread) {
        /* create an event base and progress thread for us */
        pmix_psensor_base.evbase = pmix_progress_thread_init("PSENSOR");
        if (NULL == pmix_psensor_base.evbase) {
            PMIX_LIST_DESTRUCT(&pmix_psensor_base.actives);
            return PMIX_ERROR;
        }
        /* and start it. init() builds the base and the thread object but
         * leaves the engine parked - pmix_init.c starts the library's own
         * thread in a separate step for the same reason. Without this the
         * base exists and trackers arm their timers on it quite happily,
         * and nothing ever runs them: no heartbeat window is ever checked
         * and no file is ever sampled, so the monitors silently never
         * fire. The tracker a start posted would not even reach its
         * component's list, since add_tracker runs on this base too. */
        rc = pmix_progress_thread_start("PSENSOR");
        if (PMIX_SUCCESS != rc) {
            (void) pmix_progress_thread_stop("PSENSOR");
            pmix_psensor_base.evbase = NULL;
            PMIX_LIST_DESTRUCT(&pmix_psensor_base.actives);
            return rc;
        }

    } else {
        pmix_psensor_base.evbase = pmix_globals.evbase;
    }

    /* Open up all available components */
    rc = pmix_mca_base_framework_components_open(&pmix_psensor_base_framework, flags);
    if (PMIX_SUCCESS != rc) {
        /* Undo our own setup. When a framework's open fails, the base
         * never sets the OPEN flag, and so never calls the framework's
         * close function - leaving the progress thread, its event base
         * and the actives list to leak. No component has run yet, so
         * there are no trackers to race with here. */
        PMIX_LIST_DESTRUCT(&pmix_psensor_base.actives);
        if (use_separate_thread) {
            (void) pmix_progress_thread_stop("PSENSOR");
        }
        pmix_psensor_base.evbase = NULL;
    }
    return rc;
}

PMIX_MCA_BASE_VERSIONED_FRAMEWORK_DECLARE(pmix, psensor, "PMIx Monitoring Sensors", pmix_psensor_register,
                                pmix_psensor_base_open, pmix_psensor_base_close,
                                pmix_mca_psensor_base_static_components,
                                PMIX_MCA_BASE_FRAMEWORK_FLAG_DEFAULT);

PMIX_CLASS_INSTANCE(pmix_psensor_active_module_t, pmix_list_item_t, NULL, NULL);
