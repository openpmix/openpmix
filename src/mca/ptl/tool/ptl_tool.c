/*
 * Copyright (c) 2004-2010 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2011 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2010-2011 Oak Ridge National Labs.  All rights reserved.
 * Copyright (c) 2011-2014 Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2011-2013 Los Alamos National Security, LLC.  All rights
 *                         reserved.
 * Copyright (c) 2013-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2018      IBM Corporation.  All rights reserved.
 * Copyright (c) 2021-2025 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 */

#include "src/include/pmix_config.h"
#include "src/include/pmix_globals.h"

#include "src/common/pmix_control.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_show_help.h"
#include "ptl_tool.h"
#include "src/mca/ptl/base/base.h"

static pmix_status_t setup_listener(pmix_info_t info[], size_t ninfo);

pmix_ptl_module_t pmix_ptl_tool_module = {
    .name = "tool",
    .connect_to_peer = pmix_ptl_base_connect_to_peer,
    .setup_fork = pmix_ptl_base_setup_fork,
    .setup_listener = setup_listener
};

static pmix_status_t setup_listener(pmix_info_t info[], size_t ninfo)
{
    pmix_status_t rc;
    char **clnup = NULL, *cptr = NULL;
    pmix_info_t dir;

    rc = pmix_ptl_base_setup_listener(info, ninfo);
    if (PMIX_SUCCESS != rc) {
        return rc;
    }

    /* If we are connected, then register any rendezvous files for
     * cleanup.
     *
     * Be aware that this block does not currently run at all. Both
     * PMIx_tool_init and PMIx_server_init start their listener - which
     * is what calls us - before they set pmix_globals.initialized, and
     * a job-control request is refused until that is set, because one
     * sent any earlier arrives in the middle of the init exchange. So
     * the registration is attempted and declined, every time.
     *
     * That has always been so; it was simply invisible while the call
     * went through PMIx_Job_control_nb, which returned PMIX_ERR_INIT
     * and had its answer thrown away. Making it work means moving the
     * registration to somewhere after init completes, which is a change
     * to what the host is asked to clean up rather than a change to
     * this component - so it belongs with whoever owns that decision.
     * Left here, refused and reported, rather than deleted or quietly
     * switched on. */
    if (pmix_atomic_check_bool(&pmix_globals.connected)) {
        if (NULL != pmix_ptl_base.nspace_filename) {
            PMIx_Argv_append_nosize(&clnup, pmix_ptl_base.nspace_filename);
        }
        if (NULL != pmix_ptl_base.session_filename) {
            PMIx_Argv_append_nosize(&clnup, pmix_ptl_base.session_filename);
        }
        if (NULL != clnup) {
            cptr = PMIx_Argv_join(clnup, ',');
            PMIx_Argv_free(clnup);
            PMIX_INFO_LOAD(&dir, PMIX_REGISTER_CLEANUP, cptr, PMIX_STRING);
            free(cptr);
            /* Go straight to the internal relay rather than through
             * PMIx_Job_control_nb: library code must not call a public
             * API that thread-shifts. The relay packs the directive
             * before it returns and only the send is shifted, so the
             * info is ours to destruct immediately afterward. */
            rc = pmix_job_control_relay(&pmix_globals.myid, 1, &dir, 1, NULL, NULL);
            PMIX_INFO_DESTRUCT(&dir);
            if (PMIX_SUCCESS != rc) {
                /* not fatal - we simply leave the files for the session
                 * directory sweep to deal with */
                pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                                    "ptl:tool could not register rendezvous files "
                                    "for cleanup: %s (see the note above - this is "
                                    "expected during init)", PMIx_Error_string(rc));
            }
        }
    }

    return PMIX_SUCCESS;
}
