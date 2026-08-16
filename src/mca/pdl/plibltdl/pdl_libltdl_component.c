/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2015       Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2015       Los Alamos National Security, Inc.  All rights
 *                          reserved.
 * Copyright (c) 2017-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "src/include/pmix_config.h"

#include "pmix_common.h"
#include "src/mca/base/pmix_mca_base_var.h"
#include "src/mca/pdl/pdl.h"
#include "src/util/pmix_argv.h"

#include "pdl_libltdl.h"

/*
 * Public string showing the sysinfo ompi_linux component version number
 */
const char *pmix_pdl_plibltdl_component_version_string
    = "PMIX pdl plibltdl MCA component version " PMIX_VERSION;

/*
 * Local functions
 */
static int plibltdl_component_register(void);
static int plibltdl_component_open(void);
static int plibltdl_component_close(void);
static int plibltdl_component_query(pmix_mca_base_module_t **module, int *priority);

/*
 * Instantiate the public struct with all of our public information
 * and pointers to our public functions in it
 */

pmix_pdl_plibltdl_component_t pmix_mca_pdl_plibltdl_component = {

    /* Fill in the pmix_pdl_base_component_t */
    .base = {

        /* First, the pmix_mca_base_component_t struct containing meta
           information about the component itself */
        .base_version = {
            PMIX_MCA_BASE_VERSION(pdl),

            /* Component name and version */
            .pmix_mca_component_name = "plibltdl",
            PMIX_MCA_BASE_MAKE_VERSION(component, PMIX_MAJOR_VERSION, PMIX_MINOR_VERSION,
                                       PMIX_RELEASE_VERSION),

            /* Component functions */
            .pmix_mca_register_component_params = plibltdl_component_register,
            .pmix_mca_open_component = plibltdl_component_open,
            .pmix_mca_close_component = plibltdl_component_close,
            .pmix_mca_query_component = plibltdl_component_query,
        },

        /* The pdl framework members */
        .priority = 50
    },

    /* Now fill in the plibltdl component-specific members */
};
PMIX_MCA_BASE_COMPONENT_INIT(pmix, pdl, plibltdl)

/* Storage for the info parameter below. Registration resolves the value
   through this pointer and keeps it, so it has to outlive the call --
   hence file scope rather than a local. */
static bool plibltdl_have_lt_dladvise = PMIX_INT_TO_BOOL(PMIX_PDL_PLIBLTDL_HAVE_LT_DLADVISE);

static int plibltdl_component_register(void)
{
    int ret;

    /* Register an info param indicating whether we have lt_dladvise
       support or not */
    ret = pmix_mca_base_component_var_register(&pmix_mca_pdl_plibltdl_component.base.base_version,
                                               "have_lt_dladvise",
                                               "Whether the version of libltdl that this component "
                                               "is built against supports lt_dladvise "
                                               "functionality or not",
                                               PMIX_MCA_BASE_VAR_TYPE_BOOL,
                                               &plibltdl_have_lt_dladvise);
    if (0 > ret) {
        return ret;
    }

    return PMIX_SUCCESS;
}

static int plibltdl_component_open(void)
{
    if (lt_dlinit()) {
        return PMIX_ERROR;
    }

#if PMIX_PDL_PLIBLTDL_HAVE_LT_DLADVISE
    pmix_pdl_plibltdl_component_t *c = &pmix_mca_pdl_plibltdl_component;

    if (lt_dladvise_init(&c->advise_private_noext)) {
        return PMIX_ERR_OUT_OF_RESOURCE;
    }

    if (lt_dladvise_init(&c->advise_private_ext) || lt_dladvise_ext(&c->advise_private_ext)) {
        return PMIX_ERR_OUT_OF_RESOURCE;
    }

    if (lt_dladvise_init(&c->advise_public_noext) || lt_dladvise_global(&c->advise_public_noext)) {
        return PMIX_ERR_OUT_OF_RESOURCE;
    }

    if (lt_dladvise_init(&c->advise_public_ext) || lt_dladvise_global(&c->advise_public_ext)
        || lt_dladvise_ext(&c->advise_public_ext)) {
        return PMIX_ERR_OUT_OF_RESOURCE;
    }
#endif

    return PMIX_SUCCESS;
}

static int plibltdl_component_close(void)
{
#if PMIX_PDL_PLIBLTDL_HAVE_LT_DLADVISE
    pmix_pdl_plibltdl_component_t *c = &pmix_mca_pdl_plibltdl_component;

    lt_dladvise_destroy(&c->advise_private_noext);
    lt_dladvise_destroy(&c->advise_private_ext);
    lt_dladvise_destroy(&c->advise_public_noext);
    lt_dladvise_destroy(&c->advise_public_ext);
#endif

    lt_dlexit();

    return PMIX_SUCCESS;
}

static int plibltdl_component_query(pmix_mca_base_module_t **module, int *priority)
{
    /* The priority value is somewhat meaningless here; by
       src/mca/pdl/configure.m4, there's at most one component
       available. */
    *priority = pmix_mca_pdl_plibltdl_component.base.priority;
    *module = &pmix_pdl_plibltdl_module.super;

    return PMIX_SUCCESS;
}
