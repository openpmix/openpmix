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

#include "src/mca/ptl/base/base.h"
#include "ptl_server.h"

static pmix_status_t setup_listener(pmix_info_t info[], size_t ninfo);

pmix_ptl_module_t pmix_ptl_server_module = {
    .name = "server",
    .connect_to_peer = pmix_ptl_base_connect_to_peer,
    .setup_fork = pmix_ptl_base_setup_fork,
    .setup_listener = setup_listener
};

static pmix_status_t setup_listener(pmix_info_t info[], size_t ninfo)
{
    pmix_status_t rc;
    size_t n;

    for (n=0; n < ninfo; n++) {
        if (0 == strcmp(info[n].key, PMIX_SERVER_SESSION_SUPPORT)) {
            pmix_ptl_base.session_tool = PMIX_INFO_TRUE(&info[n]);
        } else if (PMIX_CHECK_KEY(&info[n], PMIX_SERVER_SYSTEM_SUPPORT)) {
            pmix_ptl_base.system_tool = PMIX_INFO_TRUE(&info[n]);
        } else if (0 == strcmp(info[n].key, PMIX_SERVER_TOOL_SUPPORT)) {
            pmix_ptl_base.tool_support = PMIX_INFO_TRUE(&info[n]);
        } else if (PMIX_CHECK_KEY(&info[n], PMIX_SERVER_REMOTE_CONNECTIONS)) {
            pmix_ptl_base.remote_connections = PMIX_INFO_TRUE(&info[n]);
        }
    }

    rc = pmix_ptl_base_setup_listener();
    return rc;
}


