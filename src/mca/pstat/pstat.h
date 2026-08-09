/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2004-2007 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2008 The Trustees of Indiana University.
 *                         All rights reserved.
 * Copyright (c) 2015      Los Alamos National Security, LLC. All rights
 *                         reserved.
 *
 * Copyright (c) 2019      Intel, Inc.  All rights reserved.
 * Copyright (c) 2020      Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2021-2025 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/**
 * @file
 *
 * pstat (process statistics) framework component interface.
 *
 * Intent
 *
 * To support the ompi-top utility.
 *
 */

#ifndef PMIX_MCA_PSTAT_H
#define PMIX_MCA_PSTAT_H

#include "pmix_config.h"
#include "pmix_common.h"

#include "src/mca/base/pmix_base.h"
#include "src/mca/mca.h"

BEGIN_C_DECLS

/**
 * Module initialization function.  Should return PMIX_SUCCESS.
 */
typedef pmix_status_t (*pmix_pstat_base_module_init_fn_t)(void);

typedef pmix_status_t (*pmix_pstat_base_module_query_fn_t)(pmix_proc_t *requestor,
                                                           const pmix_info_t *monitor, pmix_status_t error,
                                                           const pmix_info_t directives[], size_t ndirs,
                                                           pmix_info_t **results, size_t *nresults);

typedef pmix_status_t (*pmix_pstat_base_module_fini_fn_t)(void);

/**
 * Structure for pstat components.
 */
typedef pmix_mca_base_component_t pmix_pstat_base_component_t;

/**
 * Structure for pstat modules
 */
struct pmix_pstat_base_module_1_0_0_t {
    pmix_pstat_base_module_init_fn_t init;
    pmix_pstat_base_module_query_fn_t query;
    pmix_pstat_base_module_fini_fn_t finalize;
};

/**
 * Convenience typedef
 */
typedef struct pmix_pstat_base_module_1_0_0_t pmix_pstat_base_module_1_0_0_t;
typedef struct pmix_pstat_base_module_1_0_0_t pmix_pstat_base_module_t;

/**
 * Macro for use in components that are of type pstat
 */
/* The pstat framework interface version. It is stated here and
 * nowhere else: the component macro below stamps these numbers into
 * every pstat component, and the framework's declaration reaches the
 * same three by pasting its name, so the two cannot drift apart.
 * Bump it on any change to the module interface that a component
 * built against the previous one would not survive. */
#define PMIX_MCA_pstat_MAJOR_VERSION   1
#define PMIX_MCA_pstat_MINOR_VERSION   0
#define PMIX_MCA_pstat_RELEASE_VERSION 0

#define PMIX_PSTAT_BASE_VERSION_1_0_0                                     \
    PMIX_MCA_BASE_VERSION_1_0_0("pstat", PMIX_MCA_pstat_MAJOR_VERSION,   \
                                PMIX_MCA_pstat_MINOR_VERSION,           \
                                PMIX_MCA_pstat_RELEASE_VERSION)

/* Global structure for accessing pstat functions */
PMIX_EXPORT extern pmix_pstat_base_module_t pmix_pstat;

END_C_DECLS

#endif /* PMIX_MCA_PSTAT_H */
