/*
 * Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2005 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2008-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2014-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2019      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "pmix_config.h"

#include "pmix_common.h"
#include "src/include/pmix_globals.h"
#include "src/mca/base/pmix_base.h"
#include "src/mca/mca.h"
#include "src/mca/pstat/base/base.h"
#include "src/mca/pstat/pstat.h"
#include "src/runtime/pmix_progress_threads.h"
#include "src/util/pmix_output.h"

/*
 * The following file was created by configure.  It contains extern
 * statements and the definition of an array of pointers to each
 * component's public pmix_mca_base_component_t struct.
 */
#include "src/mca/pstat/base/static-components.h"

/* unsupported functions */
static pmix_status_t pmix_pstat_base_unsupported_init(void);
static pmix_status_t pmix_pstat_base_unsupported_query(pmix_proc_t *requestor,
                                                       const pmix_info_t *monitor, pmix_status_t error,
                                                       const pmix_info_t directives[], size_t ndirs,
                                                       pmix_info_t **results, size_t *nresults);
static pmix_status_t pmix_pstat_base_unsupported_finalize(void);

/*
 * Globals
 */
pmix_pstat_base_component_t *pmix_pstat_base_component = NULL;
pmix_pstat_base_module_t pmix_pstat = {
    pmix_pstat_base_unsupported_init,
    pmix_pstat_base_unsupported_query,
    pmix_pstat_base_unsupported_finalize
};
pmix_pstat_base_t pmix_pstat_base = {
    .evbase = NULL,
    .ops = PMIX_LIST_STATIC_INIT
};

static bool use_separate_thread = false;

static int pmix_pstat_register(pmix_mca_base_register_flag_t flags)
{
    (void) flags;
    (void) pmix_mca_base_var_register("pmix", "pstat", "base", "use_separate_thread",
                                      "Use a separate thread for monitoring local procs resource usage",
                                      PMIX_MCA_BASE_VAR_TYPE_BOOL,
                                      &use_separate_thread);
    return PMIX_SUCCESS;
}

/* Use default register/open/close functions */
static int pmix_pstat_base_close(void)
{
    /* Quiesce the sampling thread before touching the ops, but keep its
     * event base alive across the teardown. Both halves matter:
     *
     * - every live op carries a timer armed on this base, and
     *   pmix_event_del does not wait for a callback that is already
     *   running - so destructing the ops while the thread still runs can
     *   free an op out from under the update() executing on it. Pausing
     *   joins the thread, so nothing is in flight once it returns.
     * - the base cannot be freed yet either, because opdes deletes each
     *   op's timer and pmix_event_del reads the base out of the event.
     *
     * That is why this is a pause here and a stop further down, rather
     * than one call. In the default configuration there is no separate
     * thread to pause: evbase is the library's shared base, which
     * PMIx_server_finalize has already stopped before it closes any
     * framework. */
    if (use_separate_thread && NULL != pmix_pstat_base.evbase) {
        (void) pmix_progress_thread_pause("PSTAT");
    }

    PMIX_LIST_DESTRUCT(&pmix_pstat_base.ops);
    /* let the selected module finalize */
    if (NULL != pmix_pstat.finalize) {
        pmix_pstat.finalize();
    }

    /* Now the base can go. Do NOT free it here as well: this drops the
     * last reference to the "PSTAT" tracker, and the tracker's destructor
     * is what calls pmix_event_base_free. Freeing it again here was a
     * double free - compare pmix_psensor_base_close, which stops its own
     * thread the same way and correctly leaves the base alone. */
    if (use_separate_thread && NULL != pmix_pstat_base.evbase) {
        (void) pmix_progress_thread_stop("PSTAT");
    }
    pmix_pstat_base.evbase = NULL;

    /* Put the unsupported stubs back. The module we are holding belongs to
     * a component that components_close is about to let go of - and under
     * --enable-mca-dso that means its plugin is unloaded. A server that
     * finalizes and initializes again re-runs select, but select only
     * overwrites pmix_pstat when it finds a component; if the second cycle
     * finds none, every one of these pointers would still name the last
     * cycle's unloaded plugin instead of falling back to unsupported. */
    pmix_pstat.init = pmix_pstat_base_unsupported_init;
    pmix_pstat.query = pmix_pstat_base_unsupported_query;
    pmix_pstat.finalize = pmix_pstat_base_unsupported_finalize;
    pmix_pstat_base_component = NULL;

    return pmix_mca_base_framework_components_close(&pmix_pstat_base_framework, NULL);
}

static int pmix_pstat_base_open(pmix_mca_base_open_flag_t flags)
{
    int rc;

    if (use_separate_thread) {
        /* create an event base and progress thread for us */
        pmix_pstat_base.evbase = pmix_progress_thread_init("PSTAT");
        if (NULL == pmix_pstat_base.evbase) {
            return PMIX_ERROR;
        }
        /* and start it. init() builds the base and the thread object but
         * leaves the engine parked - pmix_init.c starts the library's own
         * thread in a separate step for the same reason. Without this the
         * base exists and ops arm their timers on it quite happily, and
         * nothing ever runs them: the whole point of the parameter, which
         * is to sample off the main thread, silently does not happen. */
        rc = pmix_progress_thread_start("PSTAT");
        if (PMIX_SUCCESS != rc) {
            (void) pmix_progress_thread_stop("PSTAT");
            pmix_pstat_base.evbase = NULL;
            return rc;
        }

    } else {
        pmix_pstat_base.evbase = pmix_globals.evbase;
    }
    PMIX_CONSTRUCT(&pmix_pstat_base.ops, pmix_list_t);

    /* Open up all available components */
    rc = pmix_mca_base_framework_components_open(&pmix_pstat_base_framework, flags);
    if (PMIX_SUCCESS != rc) {
        /* Undo our own setup. When a framework's open fails, the base
         * never sets the OPEN flag, and so never calls the framework's
         * close function - leaving the progress thread, its event base
         * and the ops list to leak. No component has run yet, so there
         * are no ops to race with here. */
        PMIX_LIST_DESTRUCT(&pmix_pstat_base.ops);
        if (use_separate_thread) {
            (void) pmix_progress_thread_stop("PSTAT");
        }
        pmix_pstat_base.evbase = NULL;
    }
    return rc;
}

PMIX_MCA_BASE_VERSIONED_FRAMEWORK_DECLARE(pmix, pstat, "process statistics",
                                pmix_pstat_register, pmix_pstat_base_open,
                                pmix_pstat_base_close, pmix_mca_pstat_base_static_components, 0);

static pmix_status_t pmix_pstat_base_unsupported_init(void)
{
    return PMIX_ERR_NOT_SUPPORTED;
}

static pmix_status_t pmix_pstat_base_unsupported_query(pmix_proc_t *requestor,
                                                       const pmix_info_t *monitor, pmix_status_t error,
                                                       const pmix_info_t directives[], size_t ndirs,
                                                       pmix_info_t **results, size_t *nresults)
{
    PMIX_HIDE_UNUSED_PARAMS(requestor, monitor, error, directives, ndirs, results, nresults);

    return PMIX_ERR_NOT_SUPPORTED;
}

static pmix_status_t pmix_pstat_base_unsupported_finalize(void)
{
    return PMIX_ERR_NOT_SUPPORTED;
}

static void opcon(pmix_pstat_op_t *p)
{
    /* PMIX_NEW does not zero the object, so every member a consumer might
     * read has to be set here. requestor and eventcode are the two that
     * matter: a periodic op notifies with PMIx_Notify_event(eventcode)
     * scoped to requestor, so leaving them holding heap garbage means
     * raising an arbitrary status at an arbitrary process. Both real
     * components assign them immediately after PMIX_NEW, which is what
     * keeps this latent rather than live - but nothing enforces that.
     * ev and tv are deliberately not initialized: tv is set by
     * PMIX_PSTAT_OP_START before the timer is armed, and ev is only ever
     * touched under the active flag, which starts false. */
    PMIX_PROC_CONSTRUCT(&p->requestor);
    p->eventcode = PMIX_SUCCESS;
    p->id = NULL;
    p->active = false;
    p->rate = 0;
    PMIX_CONSTRUCT(&p->peers, pmix_list_t);
    p->disks = NULL;
    p->nets = NULL;
    PMIX_PROCSTATS_INIT(&p->pstats);
    PMIX_NDSTATS_INIT(&p->ndstats);
    PMIX_NETSTATS_INIT(&p->netstats);
    PMIX_DKSTATS_INIT(&p->dkstats);
    p->cb = NULL;
}
static void opdes(pmix_pstat_op_t *p)
{
    pmix_peerlist_t *pl, *plnext;

    if (p->active) {
        pmix_event_del(&p->ev);
    }
    if (NULL != p->id) {
        free(p->id);
    }
    /* the list holds a reference on every peer it records - see
     * PMIX_PSTAT_APPEND_PEER_UNIQUE in base.h, which takes it */
    PMIX_LIST_FOREACH_SAFE(pl, plnext, &p->peers, pmix_peerlist_t) {
        pmix_list_remove_item(&p->peers, &pl->super);
        PMIX_RELEASE(pl->peer);
        PMIX_RELEASE(pl);
    }
    PMIX_LIST_DESTRUCT(&p->peers);
    if (NULL != p->disks) {
        PMIx_Argv_free(p->disks);
    }
    if (NULL != p->nets) {
        PMIx_Argv_free(p->nets);
    }
}
PMIX_CLASS_INSTANCE(pmix_pstat_op_t,
                    pmix_list_item_t,
                    opcon, opdes);
