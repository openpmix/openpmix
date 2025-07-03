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
 * Copyright (c) 2021-2025 Nanook Consulting  All rights reserved.
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
static pmix_status_t pmix_pstat_base_unsupported_query(const pmix_info_t *monitor, pmix_status_t error,
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
    PMIX_LIST_DESTRUCT(&pmix_pstat_base.ops);
    /* let the selected module finalize */
    if (NULL != pmix_pstat.finalize) {
        pmix_pstat.finalize();
    }

    if (use_separate_thread && NULL != pmix_pstat_base.evbase) {
        (void) pmix_progress_thread_stop("PSTAT");
        pmix_event_base_free(pmix_pstat_base.evbase);
    }

    return pmix_mca_base_framework_components_close(&pmix_pstat_base_framework, NULL);
}

static int pmix_pstat_base_open(pmix_mca_base_open_flag_t flags)
{
    if (use_separate_thread) {
        /* create an event base and progress thread for us */
        pmix_pstat_base.evbase = pmix_progress_thread_init("PSTAT");
        if (NULL == pmix_pstat_base.evbase) {
            return PMIX_ERROR;
        }

    } else {
        pmix_pstat_base.evbase = pmix_globals.evbase;
    }
    PMIX_CONSTRUCT(&pmix_pstat_base.ops, pmix_list_t);

    /* Open up all available components */
    return pmix_mca_base_framework_components_open(&pmix_pstat_base_framework, flags);
}

PMIX_MCA_BASE_FRAMEWORK_DECLARE(pmix, pstat, "process statistics",
                                pmix_pstat_register, pmix_pstat_base_open,
                                pmix_pstat_base_close, pmix_mca_pstat_base_static_components, 0);

static pmix_status_t pmix_pstat_base_unsupported_init(void)
{
    return PMIX_ERR_NOT_SUPPORTED;
}

static pmix_status_t pmix_pstat_base_unsupported_query(const pmix_info_t *monitor, pmix_status_t error,
                                                       const pmix_info_t directives[], size_t ndirs,
                                                       pmix_info_t **results, size_t *nresults)
{
    PMIX_HIDE_UNUSED_PARAMS(monitor, error, directives, ndirs, results, nresults);

    return PMIX_ERR_NOT_SUPPORTED;
}

static pmix_status_t pmix_pstat_base_unsupported_finalize(void)
{
    return PMIX_ERR_NOT_SUPPORTED;
}

static void opcon(pmix_pstat_op_t *p)
{
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
    if (p->active) {
        pmix_event_del(&p->ev);
    }
    if (NULL != p->id) {
        free(p->id);
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
