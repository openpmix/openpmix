/*
 * Copyright (c) 2004-2007 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2007 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2008      Sun Microsystems, Inc.  All rights reserved.
 * Copyright (c) 2010-2022 Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2021-2025 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/** @file **/

#ifndef PMIX_RTE_H
#define PMIX_RTE_H

#include "src/include/pmix_config.h"
#include "pmix_common.h"
#include "src/class/pmix_object.h"

#include <stdio.h>
#include <sys/types.h>
#include <unistd.h>
#include <event.h>

#include "src/include/pmix_globals.h"
#include "src/mca/ptl/ptl_types.h"

BEGIN_C_DECLS

/* pmix_timing_output and pmix_timing_overhead are declared by
 * src/util/pmix_timings.h, which is where the macros that use them live
 * and which, unlike this header, is installed. pmix_timing_sync_file was
 * declared here and defined nowhere, so any use of it was a link error;
 * it is gone. */

PMIX_EXPORT extern char *pmix_net_private_ipv4;
PMIX_EXPORT extern int pmix_event_caching_window;
PMIX_EXPORT extern char *pmix_progress_thread_cpus;
PMIX_EXPORT extern bool pmix_bind_progress_thread_reqd;
PMIX_EXPORT extern int pmix_maxfd;
PMIX_EXPORT extern int pmix_server_client_fintime;
PMIX_EXPORT extern bool pmix_keep_fqdn_hostnames;
PMIX_EXPORT extern bool pmix_log_host_only;

/** version string of pmix */
extern const char pmix_version_string[];

/**
 * Initialize the PMIX layer, including the MCA system.
 *
 * @retval PMIX_SUCCESS Upon success.
 * @retval PMIX_ERROR Upon failure.
 *
 */
PMIX_EXPORT pmix_status_t pmix_rte_init(uint32_t type, pmix_info_t info[], size_t ninfo,
                                        pmix_ptl_cbfunc_t cbfunc);

/**
 * Finalize the PMIX layer, including the MCA system.
 *
 */
PMIX_EXPORT void pmix_rte_finalize(void);

/**
 * Internal function.  Do not call.
 */
PMIX_EXPORT pmix_status_t pmix_register_params(void);
PMIX_EXPORT pmix_status_t pmix_deregister_params(void);

PMIX_EXPORT void pmix_set_aliases(char ***aliases, char *hostname);

END_C_DECLS

#endif /* PMIX_RTE_H */
