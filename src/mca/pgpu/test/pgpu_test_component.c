/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2004-2008 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2005 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2015      Los Alamos National Security, LLC. All rights
 *                         reserved.
 * Copyright (c) 2016-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * These symbols are in a file by themselves to provide nice linker
 * semantics.  Since linkers generally pull in symbols by object
 * files, keeping these symbols as the only symbols in this file
 * prevents utility programs such as "pmix_info" from having to import
 * entire components just to query their version and parameters.
 */

#include "src/include/pmix_config.h"
#include "pmix_common.h"

#include "src/include/pmix_globals.h"
#include "src/mca/base/pmix_mca_base_var.h"
#include "src/mca/pgpu/base/base.h"
#include "src/util/pmix_argv.h"

#include "pgpu_test.h"

static pmix_status_t component_open(void);
static pmix_status_t component_close(void);
static pmix_status_t component_query(pmix_mca_base_module_t **module, int *priority);
static pmix_status_t component_register(void);

/*
 * Instantiate the public struct with all of our public information
 * and pointers to our public functions in it
 */
pmix_pgpu_test_component_t pmix_mca_pgpu_test_component = {
    .super = {
        PMIX_PGPU_BASE_VERSION_1_0_0,

        /* Component name and version */
        .pmix_mca_component_name = "test",
        PMIX_MCA_BASE_MAKE_VERSION(component,
                                   PMIX_MAJOR_VERSION,
                                   PMIX_MINOR_VERSION,
                                   PMIX_RELEASE_VERSION),

        /* Component open and close functions */
        .pmix_mca_open_component = component_open,
        .pmix_mca_close_component = component_close,
        .pmix_mca_register_component_params = component_register,
        .pmix_mca_query_component = component_query,
    },
    .incparms = NULL,
    .excparms = NULL,
    .include = NULL,
    .exclude = NULL
};
PMIX_MCA_BASE_COMPONENT_INIT(pmix, pgpu, test)

static pmix_status_t component_register(void)
{
    pmix_mca_base_component_t *component = &pmix_mca_pgpu_test_component.super;

    pmix_mca_pgpu_test_component.incparms = "PMIX_TEST_GPU_*";
    (void) pmix_mca_base_component_var_register(
        component, "include_envars",
        "Comma-delimited list of envars to harvest (\'*\' and \'?\' supported)",
        PMIX_MCA_BASE_VAR_TYPE_STRING, &pmix_mca_pgpu_test_component.incparms);
    if (NULL != pmix_mca_pgpu_test_component.incparms) {
        pmix_mca_pgpu_test_component.include = PMIx_Argv_split(pmix_mca_pgpu_test_component.incparms, ',');
    }

    pmix_mca_pgpu_test_component.excparms = NULL;
    (void) pmix_mca_base_component_var_register(
        component, "exclude_envars",
        "Comma-delimited list of envars to exclude (\'*\' and \'?\' supported)",
        PMIX_MCA_BASE_VAR_TYPE_STRING, &pmix_mca_pgpu_test_component.excparms);
    if (NULL != pmix_mca_pgpu_test_component.excparms) {
        pmix_mca_pgpu_test_component.exclude = PMIx_Argv_split(pmix_mca_pgpu_test_component.excparms, ',');
    }

    return PMIX_SUCCESS;
}

/* This is a test-only component: it never probes for real GPU hardware.
 * To avoid activating during ordinary runs, we select ourselves ONLY
 * when the local peer is a server AND the user explicitly named "test"
 * in the pgpu MCA selection string (e.g. PMIX_MCA_pgpu=test). That makes
 * the GPU setup path deterministically exercisable on any machine. */
static pmix_status_t component_open(void)
{
    int index;
    const pmix_mca_base_var_storage_t *value = NULL;

    if (!PMIX_PROC_IS_SERVER(&pmix_globals.mypeer->proc_type)) {
        return PMIX_ERROR;
    }

    if (0 > (index = pmix_mca_base_var_find("pmix", "pgpu", NULL, NULL))) {
        return PMIX_ERROR;
    }
    pmix_mca_base_var_get_value(index, &value, NULL, NULL);
    if (NULL != value && NULL != value->stringval && '\0' != value->stringval[0]) {
        if (NULL != strcasestr(value->stringval, "test")) {
            return PMIX_SUCCESS;
        }
    }
    return PMIX_ERROR;
}

static pmix_status_t component_query(pmix_mca_base_module_t **module, int *priority)
{
    *priority = 10;
    *module = (pmix_mca_base_module_t *) &pmix_pgpu_test_module;
    return PMIX_SUCCESS;
}

static pmix_status_t component_close(void)
{
    return PMIX_SUCCESS;
}
