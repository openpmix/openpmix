/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2004-2010 The Trustees of Indiana University.
 *                         All rights reserved.
 * Copyright (c) 2015      Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 *
 * Copyright (c) 2015      Los Alamos National Security, LLC. All rights
 *                         reserved.
 * Copyright (c) 2019-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2021-2026 Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "pmix_config.h"

#ifdef HAVE_UNISTD_H
#    include "unistd.h"
#endif

#include "pmix_common.h"
#include "src/mca/base/pmix_base.h"
#include "src/mca/mca.h"
#include "src/mca/pcompress/base/base.h"
#include "src/util/pmix_output.h"

/* Having no compressor is not an error - this framework's whole stance is
 * that compression is an optimization, so a tree built without any of the
 * libraries, or a run that selects nothing, leaves the base's no-op stubs
 * in place and carries on. `pmix_compress` starts out as those stubs and
 * is only overwritten once a module is ready to take over.
 *
 * That is why this function returns PMIX_SUCCESS even when it selects
 * nothing at all: the one caller, `pmix_rte_init()`, treats any other
 * status as fatal to PMIx_Init. A module that loads and then fails to
 * start is the same situation as one that was never there - the stubs
 * are still what the library will call - so it gets the same answer,
 * and the user still hears about it: the first attempt to compress
 * anything shows the "unavailable" help message. Returning the init
 * error instead would take the whole library down over a compression
 * library that could not start, where an absent one is shrugged off. */
int pmix_compress_base_select(void)
{
    int ret;
    pmix_mca_base_component_t *best_component = NULL;
    pmix_compress_base_module_t *best_module = NULL;

    if (pmix_compress_base.selected) {
        /* ensure we don't do this twice */
        return PMIX_SUCCESS;
    }
    pmix_compress_base.selected = true;
    /*
     * Select the best component
     */
    if (PMIX_SUCCESS
        != pmix_mca_base_select("pcompress", pmix_pcompress_base_framework.framework_output,
                                &pmix_pcompress_base_framework.framework_components,
                                (pmix_mca_base_module_t **) &best_module,
                                (pmix_mca_base_component_t **) &best_component, NULL)) {
        /* This will only happen if no component was selected,
         * in which case we use the default one */
        return PMIX_SUCCESS;
    }

    /* Initialize the winner */
    if (NULL != best_module) {
        if (NULL != best_module->init) {
            ret = best_module->init();
            if (PMIX_SUCCESS != ret) {
                /* leave the base defaults in place - and leave them in
                 * place completely: the module never becomes
                 * `pmix_compress`, so nothing calls its finalize either */
                pmix_output_verbose(2, pmix_pcompress_base_framework.framework_output,
                                    "pcompress: %s failed to initialize (%d) - "
                                    "continuing without compression",
                                    (NULL == best_component) ? "selected component"
                                                             : best_component->pmix_mca_component_name,
                                    ret);
                return PMIX_SUCCESS;
            }
        }
        pmix_compress = *best_module;
    }

    return PMIX_SUCCESS;
}
