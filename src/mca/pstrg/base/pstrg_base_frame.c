/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2010      Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2012-2013 Los Alamos National Security, Inc. All rights reserved.
 * Copyright (c) 2017-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2020      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2021      Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "src/include/pmix_config.h"

#include "include/pmix_common.h"

#include <pthread.h>
#include PMIX_EVENT_HEADER

#include "src/class/pmix_list.h"
#include "src/include/types.h"
#include "src/mca/base/base.h"
#include "src/mca/mca.h"
#include "src/runtime/pmix_progress_threads.h"

#include "src/mca/pstrg/base/base.h"

/*
 * The following file was created by configure.  It contains extern
 * statements and the definition of an array of pointers to each
 * component's public mca_base_component_t struct.
 */

#include "src/mca/pstrg/base/static-components.h"

/*
 * Global variables
 */
pmix_pstrg_API_module_t pmix_pstrg = {pmix_pstrg_base_query};
pmix_pstrg_base_t pmix_pstrg_base = {{{0}}};

static int pmix_pstrg_base_close(void)
{
    pmix_pstrg_active_module_t *active;

    if (!pmix_pstrg_base.init || !pmix_pstrg_base.selected) {
        return PMIX_SUCCESS;
    }
    pmix_pstrg_base.init = false;
    pmix_pstrg_base.selected = false;

    PMIX_LIST_FOREACH (active, &pmix_pstrg_base.actives, pmix_pstrg_active_module_t) {
        if (NULL != active->module->finalize) {
            active->module->finalize();
        }
    }
    PMIX_LIST_DESTRUCT(&pmix_pstrg_base.actives);

    /* destroy file system hash tables */
    PMIX_DESTRUCT(&fs_mount_to_id_hash);
    PMIX_DESTRUCT(&fs_id_to_mount_hash);

    /* Close all remaining available components */
    return pmix_mca_base_framework_components_close(&pmix_pstrg_base_framework, NULL);
}

/**
 * Function for finding and opening either all MCA components, or the one
 * that was specifically requested via a MCA parameter.
 */
static int pmix_pstrg_base_open(pmix_mca_base_open_flag_t flags)
{
    if (pmix_pstrg_base.init) {
        return PMIX_SUCCESS;
    }
    pmix_pstrg_base.init = true;

    /* construct the list of modules */
    PMIX_CONSTRUCT(&pmix_pstrg_base.actives, pmix_list_t);

    /* construct/initialize hash tables of file system mounts<->ids */
    PMIX_CONSTRUCT(&fs_mount_to_id_hash, pmix_hash_table_t);
    pmix_hash_table_init(&fs_mount_to_id_hash, 256);
    PMIX_CONSTRUCT(&fs_id_to_mount_hash, pmix_hash_table_t);
    pmix_hash_table_init(&fs_id_to_mount_hash, 256);

    /* Open up all available components */
    return pmix_mca_base_framework_components_open(&pmix_pstrg_base_framework, flags);
}

PMIX_MCA_BASE_FRAMEWORK_DECLARE(pmix, pstrg, "PMIx Storage Support", NULL, pmix_pstrg_base_open,
                                pmix_pstrg_base_close, mca_pstrg_base_static_components,
                                PMIX_MCA_BASE_FRAMEWORK_FLAG_DEFAULT);

PMIX_CLASS_INSTANCE(pmix_pstrg_active_module_t, pmix_list_item_t, NULL, NULL);

static void qcon(pmix_pstrg_query_results_t *p)
{
    PMIX_CONSTRUCT(&p->results, pmix_list_t);
}
static void qdes(pmix_pstrg_query_results_t *p)
{
    PMIX_LIST_DESTRUCT(&p->results);
}
PMIX_CLASS_INSTANCE(pmix_pstrg_query_results_t, pmix_list_item_t, qcon, qdes);
