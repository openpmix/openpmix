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
 * Copyright (c) 2021      Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 */

#include "src/include/pmix_config.h"
#include "src/include/pmix_globals.h"

#include "src/util/show_help.h"
#include "ptl_tool.h"
#include "src/mca/ptl/base/base.h"

static pmix_status_t setup_listener(pmix_info_t info[], size_t ninfo);

pmix_ptl_module_t pmix_ptl_tool_module = {.name = "tool",
                                          .connect_to_peer = pmix_ptl_base_connect_to_peer,
                                          .setup_fork = pmix_ptl_base_setup_fork,
                                          .setup_listener = setup_listener};

static pmix_status_t setup_listener(pmix_info_t info[], size_t ninfo)
{
    pmix_status_t rc;
    char **clnup = NULL, *cptr = NULL;
    pmix_info_t dir;
    size_t n;

    for (n = 0; n < ninfo; n++) {
        if (PMIX_CHECK_KEY(&info[n], PMIX_TCP_IF_INCLUDE)) {
            pmix_ptl_base.if_include = strdup(info[n].value.data.string);
        } else if (PMIX_CHECK_KEY(&info[n], PMIX_TCP_IF_EXCLUDE)) {
            pmix_ptl_base.if_exclude = strdup(info[n].value.data.string);
        } else if (PMIX_CHECK_KEY(&info[n], PMIX_TCP_IPV4_PORT)) {
            pmix_ptl_base.ipv4_port = info[n].value.data.integer;
        } else if (PMIX_CHECK_KEY(&info[n], PMIX_TCP_IPV6_PORT)) {
            pmix_ptl_base.ipv6_port = info[n].value.data.integer;
        } else if (PMIX_CHECK_KEY(&info[n], PMIX_TCP_DISABLE_IPV4)) {
            pmix_ptl_base.disable_ipv4_family = PMIX_INFO_TRUE(&info[n]);
        } else if (PMIX_CHECK_KEY(&info[n], PMIX_TCP_DISABLE_IPV6)) {
            pmix_ptl_base.disable_ipv6_family = PMIX_INFO_TRUE(&info[n]);
        }
    }

    if (NULL != pmix_ptl_base.if_include && NULL != pmix_ptl_base.if_exclude) {
        pmix_show_help("help-ptl-base.txt", "include-exclude", true, pmix_ptl_base.if_include,
                       pmix_ptl_base.if_exclude);
        return PMIX_ERR_SILENT;
    }

    rc = pmix_ptl_base_setup_listener();
    if (PMIX_SUCCESS != rc) {
        return rc;
    }

    /* if we are connected, then register any rendezvous files for cleanup */
    if (pmix_globals.connected) {
        if (NULL != pmix_ptl_base.nspace_filename) {
            pmix_argv_append_nosize(&clnup, pmix_ptl_base.nspace_filename);
        }
        if (NULL != pmix_ptl_base.session_filename) {
            pmix_argv_append_nosize(&clnup, pmix_ptl_base.session_filename);
        }
        if (NULL != clnup) {
            cptr = pmix_argv_join(clnup, ',');
            pmix_argv_free(clnup);
            PMIX_INFO_LOAD(&dir, PMIX_REGISTER_CLEANUP, cptr, PMIX_STRING);
            free(cptr);
            PMIx_Job_control_nb(&pmix_globals.myid, 1, &dir, 1, NULL, NULL);
            PMIX_INFO_DESTRUCT(&dir);
        }
    }

    return PMIX_SUCCESS;
}
