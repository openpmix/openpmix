/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2014-2019 Intel, Inc.  All rights reserved.
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

#include <src/include/pmix_config.h>
#include <pmix_sched.h>

#include <src/include/types.h>
#include <src/include/pmix_stdint.h>
#include <src/include/pmix_socket_errno.h>

#include <pmix_server.h>
#include <pmix_rename.h>
#include "src/include/pmix_globals.h"

#ifdef HAVE_STRING_H
#include <string.h>
#endif
#include <fcntl.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#ifdef HAVE_SYS_SOCKET_H
#include <sys/socket.h>
#endif
#ifdef HAVE_SYS_UN_H
#include <sys/un.h>
#endif
#ifdef HAVE_SYS_UIO_H
#include <sys/uio.h>
#endif
#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif
#include PMIX_EVENT_HEADER

#include "src/class/pmix_list.h"
#include "src/mca/pnet/base/base.h"
#include "src/util/argv.h"
#include "src/util/error.h"
#include "src/util/output.h"
#include "src/util/pmix_environ.h"

#include "pmix_server_ops.h"

PMIX_EXPORT pmix_status_t PMIx_server_register_fabric(pmix_fabric_t *fabric,
                                                      const pmix_info_t directives[],
                                                      size_t ndirs)
{
    pmix_pnet_base_active_module_t *active;
    pmix_status_t rc;

    PMIX_ACQUIRE_THREAD(&pmix_pnet_globals.lock);

    /* it is possible that multiple threads could call this, so
     * check to see if it has already been initialized - if so,
     * then just return success */
    if (NULL != fabric->module) {
        PMIX_RELEASE_THREAD(&pmix_pnet_globals.lock);
        return PMIX_SUCCESS;
    }

    /* scan across active modules until one returns success */
    PMIX_LIST_FOREACH(active, &pmix_pnet_globals.actives, pmix_pnet_base_active_module_t) {
        if (NULL != active->module->register_fabric) {
            rc = active->module->register_fabric(fabric, directives, ndirs);
            if (PMIX_SUCCESS == rc || PMIX_ERR_TAKE_NEXT_OPTION != rc) {
                PMIX_RELEASE_THREAD(&pmix_pnet_globals.lock);
                return rc;
            }
        }
    }

    /* unlock prior to return */
    PMIX_RELEASE_THREAD(&pmix_pnet_globals.lock);

    return PMIX_ERR_NOT_SUPPORTED;
}

PMIX_EXPORT pmix_status_t PMIx_server_get_num_vertices(pmix_fabric_t *fabric,
                                                       uint32_t *nverts)
{
    int rc;
    pmix_status_t ret = PMIX_SUCCESS;
    pmix_pnet_fabric_t *ft;

    if (NULL == fabric || NULL == fabric->module) {
        return PMIX_ERR_BAD_PARAM;
    }
    ft = (pmix_pnet_fabric_t*)fabric->module;

    rc = pmix_atomic_trylock(&ft->atomlock);
    if (0 != rc) {
        return PMIX_ERR_RESOURCE_BUSY;
    }
    /* if fabric data has been updated since the last time
     * this was accessed, let them know */
    if (fabric->revision != ft->revision) {
        ret = PMIX_FABRIC_UPDATED;
        /* update the revision */
        fabric->revision = ft->revision;
    }
    if (NULL == ft->module->get_num_vertices) {
        pmix_atomic_unlock(&ft->atomlock);
        return PMIX_ERR_NOT_SUPPORTED;
    }
    ret = ft->module->get_num_vertices(fabric, nverts);

    /* unlock prior to return */
    pmix_atomic_unlock(&ft->atomlock);

    return ret;
}

PMIX_EXPORT pmix_status_t PMIx_server_get_comm_cost(pmix_fabric_t *fabric,
                                                    uint32_t src, uint32_t dest,
                                                    uint16_t *cost)
{
    int rc;
    pmix_status_t ret;
    pmix_pnet_fabric_t *ft;

    if (NULL == fabric || NULL == fabric->module) {
        return PMIX_ERR_BAD_PARAM;
    }
    ft = (pmix_pnet_fabric_t*)fabric->module;

    rc = pmix_atomic_trylock(&ft->atomlock);
    if (0 != rc) {
        return PMIX_ERR_RESOURCE_BUSY;
    }

    /* if fabric data has been updated since the last time
     * this was accessed, let them know */
    if (fabric->revision != ft->revision) {
        /* unlock prior to return */
        pmix_atomic_unlock(&ft->atomlock);
        return PMIX_FABRIC_UPDATED;
    }
    if (NULL == ft->module->get_cost) {
        pmix_atomic_unlock(&ft->atomlock);
        return PMIX_ERR_NOT_SUPPORTED;
    }

    ret = ft->module->get_cost(fabric, src, dest, cost);

    /* unlock prior to return */
    pmix_atomic_unlock(&ft->atomlock);

    return ret;
}

PMIX_EXPORT pmix_status_t PMIx_server_get_vertex_info(pmix_fabric_t *fabric,
                                                      uint32_t i, pmix_value_t *vertex,
                                                      char **nodename)
{
    int rc;
    pmix_status_t ret;
    pmix_pnet_fabric_t *ft;

    if (NULL == fabric || NULL == fabric->module) {
        return PMIX_ERR_BAD_PARAM;
    }
    ft = (pmix_pnet_fabric_t*)fabric->module;

    rc = pmix_atomic_trylock(&ft->atomlock);
    if (0 != rc) {
        return PMIX_ERR_RESOURCE_BUSY;
    }

    /* if fabric data has been updated since the last time
     * this was accessed, let them know */
    if (fabric->revision != ft->revision) {
        /* unlock prior to return */
        pmix_atomic_unlock(&ft->atomlock);
        return PMIX_FABRIC_UPDATED;
    }
    if (NULL == ft->module->get_vertex) {
        pmix_atomic_unlock(&ft->atomlock);
        return PMIX_ERR_NOT_SUPPORTED;
    }

    ret = ft->module->get_vertex(fabric, i, vertex, nodename);

    /* unlock prior to return */
    pmix_atomic_unlock(&ft->atomlock);

    return ret;
}

PMIX_EXPORT pmix_status_t PMIx_server_get_index(pmix_fabric_t *fabric,
                                                pmix_value_t *vertex, uint32_t *i,
                                                char **nodename)
{
    int rc;
    pmix_status_t ret;
    pmix_pnet_fabric_t *ft;

    if (NULL == fabric || NULL == fabric->module) {
        return PMIX_ERR_BAD_PARAM;
    }
    ft = (pmix_pnet_fabric_t*)fabric->module;

    rc = pmix_atomic_trylock(&ft->atomlock);
    if (0 != rc) {
        return PMIX_ERR_RESOURCE_BUSY;
    }

    /* if fabric data has been updated since the last time
     * this was accessed, let them know */
    if (fabric->revision != ft->revision) {
        /* unlock prior to return */
        pmix_atomic_unlock(&ft->atomlock);
        return PMIX_FABRIC_UPDATED;
    }
    if (NULL == ft->module->get_index) {
        pmix_atomic_unlock(&ft->atomlock);
        return PMIX_ERR_NOT_SUPPORTED;
    }

    ret = ft->module->get_index(fabric, vertex, i, nodename);

    /* unlock prior to return */
    pmix_atomic_unlock(&ft->atomlock);

    return ret;
}
