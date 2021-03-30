/* -*- Mode: C; c-basic-offset:4 ; -*- */
/*
 * Copyright (c) 2004-2007 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2009 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2012-2013 Los Alamos National Security, Inc.  All rights reserved.
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2015-2020 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2021      Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */
/** @file:
 *
 */
#include "src/include/pmix_config.h"

#include "include/pmix_common.h"

#ifdef HAVE_STRING_H
#    include <string.h>
#endif

#include "src/class/pmix_list.h"
#include "src/mca/base/base.h"
#include "src/mca/ploc/base/base.h"

/*
 * The following file was created by configure.  It contains extern
 * statements and the definition of an array of pointers to each
 * component's public mca_base_component_t struct.
 */

#include "src/mca/ploc/base/static-components.h"

/* Instantiate the global vars */
pmix_ploc_globals_t pmix_ploc_globals = {{0}};
pmix_ploc_API_module_t pmix_ploc
    = {.setup_topology = pmix_ploc_base_setup_topology,
       .load_topology = pmix_ploc_base_load_topology,
       .generate_cpuset_string = pmix_ploc_base_generate_cpuset_string,
       .parse_cpuset_string = pmix_ploc_base_parse_cpuset_string,
       .generate_locality_string = pmix_ploc_base_generate_locality_string,
       .get_relative_locality = pmix_ploc_base_get_relative_locality,
       .get_cpuset = pmix_ploc_base_get_cpuset,
       .compute_distances = pmix_ploc_base_compute_distances,
       .pack_cpuset = pmix_ploc_base_pack_cpuset,
       .unpack_cpuset = pmix_ploc_base_unpack_cpuset,
       .copy_cpuset = pmix_ploc_base_copy_cpuset,
       .print_cpuset = pmix_ploc_base_print_cpuset,
       .destruct_cpuset = pmix_ploc_base_destruct_cpuset,
       .release_cpuset = pmix_ploc_base_release_cpuset,
       .pack_topology = pmix_ploc_base_pack_topology,
       .unpack_topology = pmix_ploc_base_unpack_topology,
       .copy_topology = pmix_ploc_base_copy_topology,
       .print_topology = pmix_ploc_base_print_topology,
       .destruct_topology = pmix_ploc_base_destruct_topology,
       .release_topology = pmix_ploc_base_release_topology};

static pmix_status_t pmix_ploc_close(void)
{
    pmix_ploc_base_active_module_t *active, *prev;

    if (!pmix_ploc_globals.initialized) {
        return PMIX_SUCCESS;
    }
    pmix_ploc_globals.initialized = false;
    pmix_ploc_globals.selected = false;

    PMIX_LIST_FOREACH_SAFE (active, prev, &pmix_ploc_globals.actives,
                            pmix_ploc_base_active_module_t) {
        pmix_list_remove_item(&pmix_ploc_globals.actives, &active->super);
        if (NULL != active->module->finalize) {
            active->module->finalize();
        }
        PMIX_RELEASE(active);
    }
    PMIX_DESTRUCT(&pmix_ploc_globals.actives);

    PMIX_DESTRUCT_LOCK(&pmix_ploc_globals.lock);
    return pmix_mca_base_framework_components_close(&pmix_ploc_base_framework, NULL);
}

static pmix_status_t pmix_ploc_open(pmix_mca_base_open_flag_t flags)
{
    /* initialize globals */
    pmix_ploc_globals.initialized = true;
    PMIX_CONSTRUCT_LOCK(&pmix_ploc_globals.lock);
    pmix_ploc_globals.lock.active = false;
    PMIX_CONSTRUCT(&pmix_ploc_globals.actives, pmix_list_t);

    /* Open up all available components */
    return pmix_mca_base_framework_components_open(&pmix_ploc_base_framework, flags);
}

PMIX_MCA_BASE_FRAMEWORK_DECLARE(pmix, ploc, "PMIx Network Operations", NULL, pmix_ploc_open,
                                pmix_ploc_close, mca_ploc_base_static_components,
                                PMIX_MCA_BASE_FRAMEWORK_FLAG_DEFAULT);

PMIX_CLASS_INSTANCE(pmix_ploc_base_active_module_t, pmix_list_item_t, NULL, NULL);
