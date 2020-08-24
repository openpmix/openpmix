/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2014-2015 Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2014-2015 Artem Y. Polyakov <artpol84@gmail.com>.
 *                         All rights reserved.
 * Copyright (c) 2016      Mellanox Technologies, Inc.
 *                         All rights reserved.
 * Copyright (c) 2016      IBM Corporation.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "src/include/pmix_config.h"
#include "include/pmix.h"

#include "src/include/pmix_globals.h"

#include "src/mca/ploc/ploc.h"

PMIX_EXPORT pmix_status_t PMIx_Load_topology(pmix_topology_t *topo)
{
    pmix_status_t rc;

    PMIX_ACQUIRE_THREAD(&pmix_global_lock);

    if (pmix_globals.init_cntr <= 0) {
        PMIX_RELEASE_THREAD(&pmix_global_lock);
        return PMIX_ERR_INIT;
    }
    PMIX_RELEASE_THREAD(&pmix_global_lock);

    rc = pmix_ploc.load_topology(topo);
    return rc;
}


PMIX_EXPORT pmix_status_t PMIx_Get_cpuset(const char *cpuset_string,
                                          pmix_cpuset_t *cpuset)
{
    pmix_status_t rc;

    PMIX_ACQUIRE_THREAD(&pmix_global_lock);

    if (pmix_globals.init_cntr <= 0) {
        PMIX_RELEASE_THREAD(&pmix_global_lock);
        return PMIX_ERR_INIT;
    }
    PMIX_RELEASE_THREAD(&pmix_global_lock);

    rc = pmix_ploc.get_cpuset(cpuset_string, cpuset);
    return rc;
}

PMIX_EXPORT pmix_status_t PMIx_Get_relative_locality(const char *locality1,
                                                     const char *locality2,
                                                     pmix_locality_t *locality)
{
    pmix_status_t rc;

    PMIX_ACQUIRE_THREAD(&pmix_global_lock);

    if (pmix_globals.init_cntr <= 0) {
        PMIX_RELEASE_THREAD(&pmix_global_lock);
        return PMIX_ERR_INIT;
    }
    PMIX_RELEASE_THREAD(&pmix_global_lock);

    rc = pmix_ploc.get_relative_locality(locality1, locality2, locality);
    return rc;
}
