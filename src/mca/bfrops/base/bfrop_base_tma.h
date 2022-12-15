/*
 * Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2006 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2015-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2019      Mellanox Technologies, Inc.
 *                         All rights reserved.
 * Copyright (c) 2021-2022 Nanook Consulting  All rights reserved.
 * Copyright (c) 2022      IBM Corporation.  All rights reserved.
 * Copyright (c) 2022      Triad National Security, LLC. All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/**
 * Houses infrastructure that supports custom memory allocators for bfrops.
 * If tma is NULL, then the default heap manager is used.
 */

#ifndef PMIX_BFROPS_BASE_TMA_H
#define PMIX_BFROPS_BASE_TMA_H

#include "src/include/pmix_config.h"
#include "src/include/pmix_globals.h"

#include "src/hwloc/pmix_hwloc.h"

#include "src/mca/bfrops/base/base.h"

static inline void
pmix_bfrops_base_load_key_tma(
    pmix_key_t key,
    const char *src,
    pmix_tma_t *tma
) {
    PMIX_HIDE_UNUSED_PARAMS(tma);

    memset(key, 0, PMIX_MAX_KEYLEN+1);
    if (NULL != src) {
        pmix_strncpy((char*)key, src, PMIX_MAX_KEYLEN);
    }
}

static inline bool
pmix_bfrops_base_check_key_tma(
    const char *key,
    const char *str,
    pmix_tma_t *tma
) {
    PMIX_HIDE_UNUSED_PARAMS(tma);

    return (0 == strncmp(key, str, PMIX_MAX_KEYLEN));
}

static inline bool
pmix_bfrops_base_nspace_invalid_tma(
    const char *nspace,
    pmix_tma_t *tma
) {
    PMIX_HIDE_UNUSED_PARAMS(tma);

    if (NULL == nspace) {
        return true;
    }
    if (0 == pmix_nslen(nspace)) {
        return true;
    }
    return false;
}

static inline bool
pmix_bfrops_base_check_nspace_tma(
    const char *nspace1,
    const char *nspace2,
    pmix_tma_t *tma
) {
    if (pmix_bfrops_base_nspace_invalid_tma(nspace1, tma)) {
        return true;
    }
    if (pmix_bfrops_base_nspace_invalid_tma(nspace2, tma)) {
        return true;
    }
    return (0 == strncmp(nspace1, nspace2, PMIX_MAX_NSLEN));
}

static inline bool
pmix_bfrops_base_check_reserved_key_tma(
    const char *key,
    pmix_tma_t *tma
) {
    PMIX_HIDE_UNUSED_PARAMS(tma);

    return (0 == strncmp(key, "pmix", 4));
}

static inline void
pmix_bfrops_base_xfer_procid_tma(
    pmix_proc_t *dst,
    const pmix_proc_t *src,
    pmix_tma_t *tma
) {
    PMIX_HIDE_UNUSED_PARAMS(tma);

    memcpy(dst, src, sizeof(pmix_proc_t));
}

static inline bool
pmix_bfrops_base_check_rank_tma(
    pmix_rank_t a,
    pmix_rank_t b,
    pmix_tma_t *tma
) {
    PMIX_HIDE_UNUSED_PARAMS(tma);

    if (a == b) {
        return true;
    }
    if (PMIX_RANK_WILDCARD == a ||
        PMIX_RANK_WILDCARD == b) {
        return true;
    }
    return false;
}

static inline bool
pmix_bfrops_base_check_procid_tma(
    const pmix_proc_t *a,
    const pmix_proc_t *b,
    pmix_tma_t *tma
) {
    if (!pmix_bfrops_base_check_nspace_tma(a->nspace, b->nspace, tma)) {
        return false;
    }
    return pmix_bfrops_base_check_rank_tma(a->rank, b->rank, tma);
}

static inline bool
pmix_bfrops_base_procid_invalid_tma(
    const pmix_proc_t *p,
    pmix_tma_t *tma
) {
    if (pmix_bfrops_base_nspace_invalid_tma(p->nspace, tma)) {
        return true;
    }
    if (PMIX_RANK_INVALID == p->rank) {
        return true;
    }
    return false;
}

static inline void
pmix_bfrops_base_load_nspace_tma(
    pmix_nspace_t nspace,
    const char *str,
    pmix_tma_t *tma
) {
    PMIX_HIDE_UNUSED_PARAMS(tma);

    memset(nspace, 0, PMIX_MAX_NSLEN+1);
    if (NULL != str) {
        pmix_strncpy((char*)nspace, str, PMIX_MAX_NSLEN);
    }
}

static inline void
pmix_bfrops_base_load_procid_tma(
    pmix_proc_t *p,
    const char *ns,
    pmix_rank_t rk,
    pmix_tma_t *tma
) {
    pmix_bfrops_base_load_nspace_tma(p->nspace, ns, tma);
    p->rank = rk;
}

static inline void
pmix_bfrops_base_proc_construct_tma(
    pmix_proc_t *p,
    pmix_tma_t *tma
) {
    PMIX_HIDE_UNUSED_PARAMS(tma);

    memset(p, 0, sizeof(pmix_proc_t));
    p->rank = PMIX_RANK_UNDEF;
}

static inline void
pmix_bfrops_base_proc_destruct_tma(
    pmix_proc_t *p,
    pmix_tma_t *tma
) {
    // TODO(skg) Is this correct?
    pmix_bfrops_base_proc_construct_tma(p, tma);
}

static inline void
pmix_bfrops_base_proc_load_tma(
    pmix_proc_t *p,
    char *nspace,
    pmix_rank_t rank,
    pmix_tma_t *tma
) {
    pmix_bfrops_base_proc_construct_tma(p, tma);
    pmix_bfrops_base_load_procid_tma(p, nspace, rank, tma);
}

static inline pmix_proc_t *
pmix_bfrops_base_proc_create_tma(
    size_t n,
    pmix_tma_t *tma
) {
    if (0 == n) {
        return NULL;
    }
    pmix_proc_t *p = (pmix_proc_t*)pmix_tma_malloc(tma, n * sizeof(pmix_proc_t));
    if (PMIX_LIKELY(NULL != p)) {
        for (size_t m = 0; m < n; m++) {
            pmix_bfrops_base_proc_construct_tma(&p[m], tma);
        }
    }
    return p;
}

static inline void
pmix_bfrops_base_proc_free_tma(
    pmix_proc_t *p,
    size_t n,
    pmix_tma_t *tma
) {
    if (NULL == p) {
        return;
    }
    for (size_t m = 0; m < n; m++) {
        // TODO(skg) Is this correct?
        pmix_bfrops_base_proc_destruct_tma(&p[m], tma);
    }
}

static inline void
pmix_bfrops_base_proc_info_construct_tma(
    pmix_proc_info_t *p,
    pmix_tma_t *tma
) {
    PMIX_HIDE_UNUSED_PARAMS(tma);

    memset(p, 0, sizeof(pmix_proc_info_t));
    p->state = PMIX_PROC_STATE_UNDEF;
}

static inline pmix_proc_info_t *
pmix_bfrops_base_proc_info_create_tma(
    size_t n,
    pmix_tma_t *tma
) {
    if (0 == n) {
        return NULL;
    }
    pmix_proc_info_t *p = (pmix_proc_info_t*)pmix_tma_malloc(tma, n * sizeof(pmix_proc_info_t));
    if (PMIX_LIKELY(NULL != p)) {
        for (size_t m = 0; m < n; m++) {
            pmix_bfrops_base_proc_info_construct_tma(&p[m], tma);
        }
    }
    return p;
}

static inline pmix_status_t
pmix_bfrops_base_copy_pinfo_tma(
    pmix_proc_info_t **dest,
    pmix_proc_info_t *src,
    pmix_data_type_t type,
    pmix_tma_t *tma
) {
    PMIX_HIDE_UNUSED_PARAMS(type);

    pmix_proc_info_t *p = pmix_bfrops_base_proc_info_create_tma(1, tma);
    if (PMIX_UNLIKELY(NULL == p)) {
        return PMIX_ERR_NOMEM;
    }
    memcpy(&p->proc, &src->proc, sizeof(pmix_proc_t));
    if (NULL != src->hostname) {
        p->hostname = pmix_tma_strdup(tma, src->hostname);
    }
    if (NULL != src->executable_name) {
        p->executable_name = pmix_tma_strdup(tma, src->executable_name);
    }
    memcpy(&p->pid, &src->pid, sizeof(pid_t));
    memcpy(&p->exit_code, &src->exit_code, sizeof(int));
    memcpy(&p->state, &src->state, sizeof(pmix_proc_state_t));
    *dest = p;
    return PMIX_SUCCESS;
}

static inline  pmix_status_t
pmix_bfrops_base_fill_coord_tma(
    pmix_coord_t *dst,
    pmix_coord_t *src,
    pmix_tma_t *tma
) {
    dst->view = src->view;
    dst->dims = src->dims;
    if (0 < dst->dims) {
        dst->coord = (uint32_t *)pmix_tma_malloc(tma, dst->dims * sizeof(uint32_t));
        if (PMIX_UNLIKELY(NULL == dst->coord)) {
            return PMIX_ERR_NOMEM;
        }
        memcpy(dst->coord, src->coord, dst->dims * sizeof(uint32_t));
    }
    return PMIX_SUCCESS;
}

static inline void
pmix_bfrops_base_coord_destruct_tma(
    pmix_coord_t *m,
    pmix_tma_t *tma
) {
    if (NULL == m) {
        return;
    }
    m->view = PMIX_COORD_VIEW_UNDEF;
    if (NULL != m->coord) {
        pmix_tma_free(tma, m->coord);
        m->coord = NULL;
        m->dims = 0;
    }
}

static inline void
pmix_bfrops_base_coord_construct_tma(
    pmix_coord_t *m,
    pmix_tma_t *tma
) {
    PMIX_HIDE_UNUSED_PARAMS(tma);

    if (NULL == m) {
        return;
    }
    m->view = PMIX_COORD_VIEW_UNDEF;
    m->coord = NULL;
    m->dims = 0;
}

static inline pmix_status_t
pmix_bfrops_base_copy_coord_tma(
    pmix_coord_t **dest,
    pmix_coord_t *src,
    pmix_data_type_t type,
    pmix_tma_t *tma
) {
    PMIX_HIDE_UNUSED_PARAMS(type);

    pmix_coord_t *d = (pmix_coord_t *)pmix_tma_malloc(tma, sizeof(pmix_coord_t));
    if (PMIX_UNLIKELY(NULL == d)) {
        return PMIX_ERR_NOMEM;
    }
    pmix_bfrops_base_coord_construct_tma(d, tma);
    pmix_status_t rc = pmix_bfrops_base_fill_coord_tma(d, src, tma);
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        pmix_bfrops_base_coord_destruct_tma(d, tma);
        pmix_tma_free(tma, d);
    }
    else {
        *dest = d;
    }
    return rc;
}

static inline void
pmix_bfrops_base_topology_construct_tma(
    pmix_topology_t *t,
    pmix_tma_t *tma
) {
    PMIX_HIDE_UNUSED_PARAMS(tma);

    memset(t, 0, sizeof(pmix_topology_t));
}

static inline pmix_topology_t *
pmix_bfrops_base_topology_create_tma(
    size_t n,
    pmix_tma_t *tma
) {
    if (0 == n) {
        return NULL;
    }
    pmix_topology_t *t = (pmix_topology_t *)pmix_tma_malloc(tma, n * sizeof(pmix_topology_t));
    if (PMIX_LIKELY(NULL != t)) {
        for (size_t m = 0; m < n; m++) {
            pmix_bfrops_base_topology_construct_tma(&t[m], tma);
        }
    }
    return t;
}

static inline pmix_status_t
pmix_bfrops_base_copy_topology_tma(
    pmix_topology_t **dest,
    pmix_topology_t *src,
    pmix_data_type_t type,
    pmix_tma_t *tma
) {
    PMIX_HIDE_UNUSED_PARAMS(type);

    pmix_topology_t *dst = pmix_bfrops_base_topology_create_tma(1, tma);
    if (PMIX_UNLIKELY(NULL == dst)) {
        return PMIX_ERR_NOMEM;
    }
    // TODO(skg) Add all the free and destruct, too.
    // TODO(skg)
    pmix_status_t rc = pmix_hwloc_copy_topology(dst, src);
    if (PMIX_SUCCESS == rc) {
        *dest = dst;
    }
    else {
        pmix_tma_free(tma, dst);
    }
    return rc;
}

static inline void
pmix_bfrops_base_cpuset_construct_tma(
    pmix_cpuset_t *c,
    pmix_tma_t *tma
) {
    PMIX_HIDE_UNUSED_PARAMS(tma);

    memset(c, 0, sizeof(pmix_cpuset_t));
}

static inline pmix_cpuset_t *
pmix_bfrops_base_cpuset_create_tma(
    size_t n,
    pmix_tma_t *tma
) {
    if (0 == n) {
        return NULL;
    }

    pmix_cpuset_t *c = (pmix_cpuset_t *)pmix_tma_malloc(tma, n * sizeof(pmix_cpuset_t));
    if (PMIX_UNLIKELY(NULL == c)) {
        return NULL;
    }
    for (size_t m = 0; m < n; m++) {
        pmix_bfrops_base_cpuset_construct_tma(&c[m], tma);
    }
    return c;
}

static inline pmix_status_t
pmix_bfrops_base_copy_cpuset_tma(
    pmix_cpuset_t **dest,
    pmix_cpuset_t *src,
    pmix_data_type_t type,
    pmix_tma_t *tma
) {
    PMIX_HIDE_UNUSED_PARAMS(type);

    pmix_cpuset_t *dst = pmix_bfrops_base_cpuset_create_tma(1, tma);
    if (PMIX_UNLIKELY(NULL == dst)) {
        return PMIX_ERR_NOMEM;
    }
    // TODO(skg) pmix_hwloc_copy_cpuset
    pmix_status_t rc = pmix_hwloc_copy_cpuset(dst, src);
    if (PMIX_SUCCESS == rc) {
        *dest = dst;
    }
    else {
        pmix_tma_free(tma, dst);
    }
    return rc;
}

static inline void
pmix_bfrops_base_geometry_construct_tma(
    pmix_geometry_t *g,
    pmix_tma_t *tma
) {
    PMIX_HIDE_UNUSED_PARAMS(tma);

    memset(g, 0, sizeof(pmix_geometry_t));
}

static inline pmix_geometry_t *
pmix_bfrops_base_geometry_create_tma(
    size_t n,
    pmix_tma_t *tma
) {
    if (0 == n) {
        return NULL;
    }
    pmix_geometry_t *g = (pmix_geometry_t *)pmix_tma_malloc(tma, n * sizeof(pmix_geometry_t));
    if (PMIX_LIKELY(NULL != g)) {
        for (size_t m = 0; m < n; m++) {
            pmix_bfrops_base_geometry_construct_tma(&g[m], tma);
        }
    }
    return g;
}

static inline void
pmix_bfrops_base_coord_free_tma(
    pmix_coord_t *m,
    size_t number,
    pmix_tma_t *tma
) {
    if (NULL == m) {
        return;
    }
    for (size_t n = 0; n < number; n++) {
        // TODO(skg) Done differently in pmix_bfrops_base_copy_coord_tma().
        pmix_bfrops_base_coord_destruct_tma(&m[n], tma);
    }
}

static inline void
pmix_bfrops_base_geometry_destruct_tma(
    pmix_geometry_t *g,
    pmix_tma_t *tma
) {
    if (NULL != g->uuid) {
        pmix_tma_free(tma, g->uuid);
        g->uuid = NULL;
    }
    if (NULL != g->osname) {
        pmix_tma_free(tma, g->osname);
        g->osname = NULL;
    }
    if (NULL != g->coordinates) {
        pmix_bfrops_base_coord_free_tma(g->coordinates, g->ncoords, tma);
    }
}

static inline void
pmix_bfrops_base_geometry_free_tma(
    pmix_geometry_t *g,
    size_t n,
    pmix_tma_t *tma
) {
    if (NULL == g) {
        return;
    }
    for (size_t m = 0; m < n; m++) {
        pmix_bfrops_base_geometry_destruct_tma(&g[m], tma);
    }
}

static inline pmix_status_t
pmix_bfrops_base_copy_geometry_tma(
    pmix_geometry_t **dest,
    pmix_geometry_t *src,
    pmix_data_type_t type,
    pmix_tma_t *tma
) {
    PMIX_HIDE_UNUSED_PARAMS(type);

    pmix_geometry_t *dst = pmix_bfrops_base_geometry_create_tma(1, tma);
    if (PMIX_UNLIKELY(NULL == dst)) {
        return PMIX_ERR_NOMEM;
    }

    dst->fabric = src->fabric;
    if (NULL != src->uuid) {
        dst->uuid = pmix_tma_strdup(tma, src->uuid);
    }
    if (NULL != src->osname) {
        dst->osname = pmix_tma_strdup(tma, src->osname);
    }
    if (NULL != src->coordinates) {
        dst->ncoords = src->ncoords;
        dst->coordinates = (pmix_coord_t *)pmix_tma_calloc(tma, dst->ncoords, sizeof(pmix_coord_t));
        for (size_t n = 0; n < dst->ncoords; n++) {
            pmix_status_t rc = pmix_bfrops_base_fill_coord_tma(&dst->coordinates[n], &src->coordinates[n], tma);
            if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
                PMIX_GEOMETRY_FREE(dst, 1);
                return rc;
            }
        }
    }

    *dest = dst;
    return PMIX_SUCCESS;
}

static inline pmix_status_t
pmix_bfrops_base_copy_nspace_tma(
    pmix_nspace_t **dest,
    pmix_nspace_t *src,
    pmix_data_type_t type,
    pmix_tma_t *tma
) {
    PMIX_HIDE_UNUSED_PARAMS(type);

    pmix_nspace_t *dst = (pmix_nspace_t *)pmix_tma_malloc(tma, sizeof(pmix_nspace_t));
    if (PMIX_UNLIKELY(NULL == dst)) {
        return PMIX_ERR_NOMEM;
    }
    pmix_bfrops_base_load_nspace_tma(*dst, (char *)src, tma);
    *dest = dst;
    return PMIX_SUCCESS;
}

static inline void
pmix_bfrops_base_device_distance_construct_tma(
    pmix_device_distance_t *d,
    pmix_tma_t *tma
) {
    PMIX_HIDE_UNUSED_PARAMS(tma);

    memset(d, 0, sizeof(pmix_device_distance_t));
    d->type = PMIX_DEVTYPE_UNKNOWN;
    d->mindist = UINT16_MAX;
    d->maxdist = UINT16_MAX;
}

static inline pmix_device_distance_t *
pmix_bfrops_base_device_distance_create_tma(
    size_t n,
    pmix_tma_t *tma
) {
    if (0 == n) {
        return NULL;
    }
    pmix_device_distance_t *d = (pmix_device_distance_t *)pmix_tma_malloc(tma, n * sizeof(pmix_device_distance_t));
    if (PMIX_LIKELY(NULL != d)) {
        for (size_t m = 0; m < n; m++) {
            pmix_bfrops_base_device_distance_construct_tma(&d[m], tma);
        }
    }
    return d;
}

static inline pmix_status_t
pmix_bfrops_base_copy_devdist_tma(
    pmix_device_distance_t **dest,
    pmix_device_distance_t *src,
    pmix_data_type_t type,
    pmix_tma_t *tma
) {
    PMIX_HIDE_UNUSED_PARAMS(type);

    pmix_device_distance_t *dst = pmix_bfrops_base_device_distance_create_tma(1, tma);
    if (PMIX_UNLIKELY(NULL == dst)) {
        return PMIX_ERR_NOMEM;
    }

    if (NULL != src->uuid) {
        dst->uuid = pmix_tma_strdup(tma, src->uuid);
    }
    if (NULL != src->osname) {
        dst->osname = pmix_tma_strdup(tma, src->osname);
    }
    dst->type = src->type;
    dst->mindist = src->mindist;
    dst->maxdist = src->maxdist;

    *dest = dst;
    return PMIX_SUCCESS;
}

static inline void
pmix_bfrops_base_endpoint_construct_tma(
    pmix_endpoint_t *e,
    pmix_tma_t *tma
) {
    PMIX_HIDE_UNUSED_PARAMS(tma);

    memset(e, 0, sizeof(pmix_endpoint_t));
}

static inline pmix_endpoint_t *
pmix_bfrops_base_endpoint_create_tma(
    size_t n,
    pmix_tma_t *tma
) {
    if (0 == n) {
        return NULL;
    }

    pmix_endpoint_t *e = (pmix_endpoint_t *)pmix_tma_malloc(tma, n * sizeof(pmix_endpoint_t));
    if (PMIX_LIKELY(NULL != e)) {
        for (size_t m = 0; m < n; m++) {
            pmix_bfrops_base_endpoint_construct_tma(&e[m], tma);
        }
    }
    return e;
}

static inline pmix_status_t
pmix_bfrops_base_copy_endpoint_tma(
    pmix_endpoint_t **dest,
    pmix_endpoint_t *src,
    pmix_data_type_t type,
    pmix_tma_t *tma
) {
    PMIX_HIDE_UNUSED_PARAMS(type);

    pmix_endpoint_t *dst = pmix_bfrops_base_endpoint_create_tma(1, tma);
    if (PMIX_UNLIKELY(NULL == dst)) {
        return PMIX_ERR_NOMEM;
    }

    if (NULL != src->uuid) {
        dst->uuid = pmix_tma_strdup(tma, src->uuid);
    }
    if (NULL != src->osname) {
        dst->osname = pmix_tma_strdup(tma, src->osname);
    }
    if (NULL != src->endpt.bytes) {
        dst->endpt.bytes = (char *)pmix_tma_malloc(tma, src->endpt.size);
        memcpy(dst->endpt.bytes, src->endpt.bytes, src->endpt.size);
        dst->endpt.size = src->endpt.size;
    }

    *dest = dst;
    return PMIX_SUCCESS;
}

static inline void
pmix_bfrops_base_regattr_construct_tma(
    pmix_regattr_t *p,
    pmix_tma_t *tma
) {
    p->name = NULL;
    pmix_bfrops_base_load_key_tma(p->string, NULL, tma);
    p->type = PMIX_UNDEF;
    p->description = NULL;
}

static inline pmix_regattr_t *
pmix_bfrops_base_regattr_create_tma(
    size_t n,
    pmix_tma_t *tma
) {
    if (0 == n) {
        return NULL;
    }

    pmix_regattr_t *p = (pmix_regattr_t*)pmix_tma_malloc(tma, n * sizeof(pmix_regattr_t));
    if (PMIX_LIKELY(NULL != p)) {
        for (size_t m = 0; m < n; m++) {
            pmix_bfrops_base_regattr_construct_tma(&p[m], tma);
        }
    }
    return p;
}

static inline int
pmix_bfrops_base_argv_count_tma(
    char **argv,
    pmix_tma_t *tma
) {
    PMIX_HIDE_UNUSED_PARAMS(tma);

    char **p;
    int i;

    if (NULL == argv)
        return 0;

    for (i = 0, p = argv; *p; i++, p++)
        continue;

    return i;
}

static inline pmix_status_t
pmix_bfrops_base_argv_append_nosize_tma(
    char ***argv,
    const char *arg,
    pmix_tma_t *tma
) {
    int argc;

    /* Create new argv. */

    if (NULL == *argv) {
        *argv = (char **)pmix_tma_malloc(tma, 2 * sizeof(char *));
        if (NULL == *argv) {
            return PMIX_ERR_OUT_OF_RESOURCE;
        }
        argc = 0;
        (*argv)[0] = NULL;
        (*argv)[1] = NULL;
    }

    /* Extend existing argv. */
    else {
        /* count how many entries currently exist */
        argc = PMIx_Argv_count(*argv);

        *argv = (char **)pmix_tma_realloc(tma, *argv, (argc + 2) * sizeof(char *));
        if (NULL == *argv) {
            return PMIX_ERR_OUT_OF_RESOURCE;
        }
    }

    /* Set the newest element to point to a copy of the arg string */

    (*argv)[argc] = pmix_tma_strdup(tma, arg);
    if (NULL == (*argv)[argc]) {
        return PMIX_ERR_OUT_OF_RESOURCE;
    }

    argc = argc + 1;
    (*argv)[argc] = NULL;

    return PMIX_SUCCESS;
}

static inline char **
pmix_bfrops_base_argv_copy_tma(
    char **argv,
    pmix_tma_t *tma
) {
    if (NULL == argv) {
        return NULL;
    }
    /* create an "empty" list, so that we return something valid if we
     were passed a valid list with no contained elements */
    char **dupv = (char **)pmix_tma_malloc(tma, sizeof(char *));
    dupv[0] = NULL;

    while (NULL != *argv) {
        if (PMIX_SUCCESS != pmix_bfrops_base_argv_append_nosize_tma(&dupv, *argv, tma)) {
            PMIX_ARGV_FREE(dupv);
            return NULL;
        }
        ++argv;
    }
    /* All done */
    return dupv;
}

static inline pmix_status_t
pmix_bfrops_base_copy_regattr_tma(
    pmix_regattr_t **dest,
    pmix_regattr_t *src,
    pmix_data_type_t type,
    pmix_tma_t *tma
) {
    PMIX_HIDE_UNUSED_PARAMS(type);

    *dest = pmix_bfrops_base_regattr_create_tma(1, tma);
    if (PMIX_UNLIKELY(NULL == (*dest))) {
        return PMIX_ERR_NOMEM;
    }
    if (NULL != src->name) {
        (*dest)->name = pmix_tma_strdup(tma, src->name);
    }
    pmix_bfrops_base_load_key_tma((*dest)->string, src->string, tma);
    (*dest)->type = src->type;
    (*dest)->description = pmix_bfrops_base_argv_copy_tma(src->description, tma);
    return PMIX_SUCCESS;
}

static inline void
pmix_bfrops_base_data_buffer_destruct_tma(
    pmix_data_buffer_t *b,
    pmix_tma_t *tma
) {
    if (NULL != b->base_ptr) {
        pmix_tma_free(tma, b->base_ptr);
        b->base_ptr = NULL;
    }
    b->pack_ptr = NULL;
    b->unpack_ptr = NULL;
    b->bytes_allocated = 0;
    b->bytes_used = 0;
}

static inline void
pmix_bfrops_base_data_buffer_construct_tma(
    pmix_data_buffer_t *b,
    pmix_tma_t *tma
) {
    PMIX_HIDE_UNUSED_PARAMS(tma);

    memset(b, 0, sizeof(pmix_data_buffer_t));
}

static inline pmix_data_buffer_t *
pmix_bfrops_base_data_buffer_create_tma(
    pmix_tma_t *tma
) {
    pmix_data_buffer_t *b = (pmix_data_buffer_t *)pmix_tma_malloc(tma, sizeof(pmix_data_buffer_t));
    if (PMIX_LIKELY(NULL != b)) {
        pmix_bfrops_base_data_buffer_construct_tma(b, tma);
    }
    return b;
}

static inline void
pmix_bfrops_base_data_buffer_release_tma(
    pmix_data_buffer_t *b,
    pmix_tma_t *tma
) {
    if (NULL == b) {
        return;
    }
    pmix_bfrops_base_data_buffer_destruct_tma(b, tma);
}

static inline pmix_status_t
pmix_bfrops_base_copy_dbuf_tma(
    pmix_data_buffer_t **dest,
    pmix_data_buffer_t *src,
    pmix_data_type_t type,
    pmix_tma_t *tma
) {
    PMIX_HIDE_UNUSED_PARAMS(type);

    /* create the new object */
    pmix_data_buffer_t *p = pmix_bfrops_base_data_buffer_create_tma(tma);
    if (PMIX_UNLIKELY(NULL == p)) {
        return PMIX_ERR_NOMEM;
    }
    *dest = p;

    // TODO(skg)
    return PMIx_Data_copy_payload(p, src);
}

static inline void
pmix_bfrops_base_proc_stats_construct_tma(
    pmix_proc_stats_t *p,
    pmix_tma_t *tma
) {
    PMIX_HIDE_UNUSED_PARAMS(tma);

    memset(p, 0, sizeof(pmix_proc_stats_t));
}

static inline pmix_proc_stats_t *
pmix_bfrops_base_proc_stats_create_tma(
    size_t n,
    pmix_tma_t *tma
) {
    if (0 == n) {
        return NULL;
    }

    pmix_proc_stats_t *p = (pmix_proc_stats_t*)pmix_tma_malloc(tma, n * sizeof(pmix_proc_stats_t));
    if (PMIX_LIKELY(NULL != p)) {
        for (size_t m = 0; m < n; m++) {
            pmix_bfrops_base_proc_stats_construct_tma(&p[m], tma);
        }
    }
    return p;
}

static inline void
pmix_bfrops_base_populate_pstats_tma(
    pmix_proc_stats_t *p,
    pmix_proc_stats_t *src,
    pmix_tma_t *tma
) {
    /* copy the individual fields */
    if (NULL != src->node) {
        p->node = pmix_tma_strdup(tma, src->node);
    }
    memcpy(&p->proc, &src->proc, sizeof(pmix_proc_t));
    p->pid = src->pid;
    if (NULL != src->cmd) {
        p->cmd = pmix_tma_strdup(tma, src->cmd);
    }
    p->state = src->state;
    p->time = src->time;
    p->priority = src->priority;
    p->num_threads = src->num_threads;
    p->pss = src->pss;
    p->vsize = src->vsize;
    p->rss = src->rss;
    p->peak_vsize = src->peak_vsize;
    p->processor = src->processor;
    p->sample_time.tv_sec = src->sample_time.tv_sec;
    p->sample_time.tv_usec = src->sample_time.tv_usec;
}

static inline pmix_status_t
pmix_bfrops_base_copy_pstats_tma(
    pmix_proc_stats_t **dest,
    pmix_proc_stats_t *src,
    pmix_data_type_t type,
    pmix_tma_t *tma
) {
    PMIX_HIDE_UNUSED_PARAMS(type);
    /* create the new object */
    pmix_proc_stats_t *p = pmix_bfrops_base_proc_stats_create_tma(1, tma);
    if (PMIX_UNLIKELY(NULL == p)) {
        return PMIX_ERR_NOMEM;
    }
    *dest = p;
    pmix_bfrops_base_populate_pstats_tma(p, src, tma);
    return PMIX_SUCCESS;
}

static inline void
pmix_bfrops_base_disk_stats_construct_tma(
    pmix_disk_stats_t *p,
    pmix_tma_t *tma
) {
    PMIX_HIDE_UNUSED_PARAMS(tma);

    memset(p, 0, sizeof(pmix_disk_stats_t));
}

static inline pmix_disk_stats_t *
pmix_bfrops_base_disk_stats_create_tma(
    size_t n,
    pmix_tma_t *tma
) {
    if (0 == n) {
        return NULL;
    }

    pmix_disk_stats_t *p = (pmix_disk_stats_t *)pmix_tma_malloc(tma, n * sizeof(pmix_disk_stats_t));
    if (NULL != p) {
        for (size_t m = 0; m < n; m++) {
            pmix_bfrops_base_disk_stats_construct_tma(&p[m], tma);
        }
    }
    return p;
}

static inline void
pmix_bfrops_base_populate_dkstats_tma(
    pmix_disk_stats_t *p,
    pmix_disk_stats_t *src,
    pmix_tma_t *tma
) {
    if (NULL != src->disk) {
        p->disk = pmix_tma_strdup(tma, src->disk);
    }
    p->num_reads_completed = src->num_reads_completed;
    p->num_reads_merged = src->num_reads_merged;
    p->num_sectors_read = src->num_sectors_read;
    p->milliseconds_reading = src->milliseconds_reading;
    p->num_writes_completed = src->num_writes_completed;
    p->num_writes_merged = src->num_writes_merged;
    p->num_sectors_written = src->num_sectors_written;
    p->milliseconds_writing = src->milliseconds_writing;
    p->num_ios_in_progress = src->num_ios_in_progress;
    p->milliseconds_io = src->milliseconds_io;
    p->weighted_milliseconds_io = src->weighted_milliseconds_io;
}

static inline pmix_status_t
pmix_bfrops_base_copy_dkstats_tma(
    pmix_disk_stats_t **dest,
    pmix_disk_stats_t *src,
    pmix_data_type_t type,
    pmix_tma_t *tma
) {
    PMIX_HIDE_UNUSED_PARAMS(type);
    /* create the new object */
    pmix_disk_stats_t *p = pmix_bfrops_base_disk_stats_create_tma(1, tma);
    if (PMIX_UNLIKELY(NULL == p)) {
        return PMIX_ERR_NOMEM;
    }
    *dest = p;
    pmix_bfrops_base_populate_dkstats_tma(p, src, tma);
    return PMIX_SUCCESS;
}

static inline void
pmix_bfrops_base_net_stats_construct_tma(
    pmix_net_stats_t *p,
    pmix_tma_t *tma
) {
    PMIX_HIDE_UNUSED_PARAMS(tma);

    memset(p, 0, sizeof(pmix_net_stats_t));
}

static inline pmix_net_stats_t *
pmix_bfrops_base_net_stats_create_tma(
    size_t n,
    pmix_tma_t *tma
) {
    if (0 == n) {
        return NULL;
    }

    pmix_net_stats_t *p = (pmix_net_stats_t*)pmix_tma_malloc(tma, n * sizeof(pmix_net_stats_t));
    if (PMIX_LIKELY(NULL != p)) {
        for (size_t m = 0; m < n; m++) {
            pmix_bfrops_base_net_stats_construct_tma(&p[m], tma);
        }
    }
    return p;
}

static inline void
pmix_bfrops_base_populate_netstats_tma(
    pmix_net_stats_t *p,
    pmix_net_stats_t *src,
    pmix_tma_t *tma
) {
    if (NULL != src->net_interface) {
        p->net_interface = pmix_tma_strdup(tma, src->net_interface);
    }
    p->num_bytes_recvd = src->num_bytes_recvd;
    p->num_packets_recvd = src->num_packets_recvd;
    p->num_recv_errs = src->num_recv_errs;
    p->num_bytes_sent = src->num_bytes_sent;
    p->num_packets_sent = src->num_packets_sent;
    p->num_send_errs = src->num_send_errs;
}

static inline pmix_status_t
pmix_bfrops_base_copy_netstats_tma(
    pmix_net_stats_t **dest,
    pmix_net_stats_t *src,
    pmix_data_type_t type,
    pmix_tma_t *tma
) {
    PMIX_HIDE_UNUSED_PARAMS(type);
    /* create the new object */
    pmix_net_stats_t *p = pmix_bfrops_base_net_stats_create_tma(1, tma);
    if (PMIX_UNLIKELY(NULL == p)) {
        return PMIX_ERR_NOMEM;
    }
    *dest = p;
    pmix_bfrops_base_populate_netstats_tma(p, src, tma);
    return PMIX_SUCCESS;
}

static inline void
pmix_bfrops_base_node_stats_construct_tma(
    pmix_node_stats_t *p,
    pmix_tma_t *tma
) {
    PMIX_HIDE_UNUSED_PARAMS(tma);

    memset(p, 0, sizeof(pmix_node_stats_t));
}

static inline pmix_node_stats_t *
pmix_bfrops_base_node_stats_create_tma(
    size_t n,
    pmix_tma_t *tma
) {
    if (0 == n) {
        return NULL;
    }

    pmix_node_stats_t *p = (pmix_node_stats_t *)pmix_tma_malloc(tma, n * sizeof(pmix_node_stats_t));
    if (PMIX_LIKELY(NULL != p)) {
        for (size_t m = 0; m < n; m++) {
            pmix_bfrops_base_node_stats_construct_tma(&p[m], tma);
        }
    }
    return p;
}

static inline void
pmix_bfrops_base_populate_ndstats_tma(
    pmix_node_stats_t *p,
    pmix_node_stats_t *src,
    pmix_tma_t *tma
) {
    /* copy the individual fields */
    if (NULL != src->node) {
        p->node = pmix_tma_strdup(tma, src->node);
    }
    p->la = src->la;
    p->la5 = src->la5;
    p->la15 = src->la15;
    p->total_mem = src->total_mem;
    p->free_mem = src->free_mem;
    p->buffers = src->buffers;
    p->cached = src->cached;
    p->swap_cached = src->swap_cached;
    p->swap_total = src->swap_total;
    p->swap_free = src->swap_free;
    p->mapped = src->mapped;
    p->sample_time.tv_sec = src->sample_time.tv_sec;
    p->sample_time.tv_usec = src->sample_time.tv_usec;
    p->ndiskstats = src->ndiskstats;
    if (0 < p->ndiskstats) {
        p->diskstats = pmix_bfrops_base_disk_stats_create_tma(p->ndiskstats, tma);
        for (size_t n = 0; n < p->ndiskstats; n++) {
            pmix_bfrops_base_populate_dkstats_tma(
                &p->diskstats[n], &src->diskstats[n], tma
            );
        }
    }
    p->nnetstats = src->nnetstats;
    if (0 < p->nnetstats) {
        p->netstats = pmix_bfrops_base_net_stats_create_tma(p->nnetstats, tma);
        for (size_t n = 0; n < p->nnetstats; n++) {
            pmix_bfrops_base_populate_netstats_tma(
                &p->netstats[n], &src->netstats[n], tma
            );
        }
    }
}

static inline pmix_status_t
pmix_bfrops_base_copy_ndstats_tma(
    pmix_node_stats_t **dest,
    pmix_node_stats_t *src,
    pmix_data_type_t type,
    pmix_tma_t *tma
) {
    PMIX_HIDE_UNUSED_PARAMS(type);
    /* create the new object */
    pmix_node_stats_t *p = pmix_bfrops_base_node_stats_create_tma(1, tma);
    if (PMIX_UNLIKELY(NULL == p)) {
        return PMIX_ERR_NOMEM;
    }
    *dest = p;
    pmix_bfrops_base_populate_ndstats_tma(p, src, tma);
    return PMIX_SUCCESS;
}

static inline pmix_status_t
pmix_bfrops_base_value_xfer_tma(
    pmix_value_t *p,
    const pmix_value_t *src,
    pmix_tma_t *tma
) {
    pmix_status_t rc;

    /* copy the right field */
    p->type = src->type;
    switch (src->type) {
    case PMIX_UNDEF:
        break;
    case PMIX_BOOL:
        p->data.flag = src->data.flag;
        break;
    case PMIX_BYTE:
        p->data.byte = src->data.byte;
        break;
    case PMIX_STRING:
        if (NULL != src->data.string) {
            p->data.string = pmix_tma_strdup(tma, src->data.string);
        } else {
            p->data.string = NULL;
        }
        break;
    case PMIX_SIZE:
        p->data.size = src->data.size;
        break;
    case PMIX_PID:
        p->data.pid = src->data.pid;
        break;
    case PMIX_INT:
        /* to avoid alignment issues */
        memcpy(&p->data.integer, &src->data.integer, sizeof(int));
        break;
    case PMIX_INT8:
        p->data.int8 = src->data.int8;
        break;
    case PMIX_INT16:
        /* to avoid alignment issues */
        memcpy(&p->data.int16, &src->data.int16, 2);
        break;
    case PMIX_INT32:
        /* to avoid alignment issues */
        memcpy(&p->data.int32, &src->data.int32, 4);
        break;
    case PMIX_INT64:
        /* to avoid alignment issues */
        memcpy(&p->data.int64, &src->data.int64, 8);
        break;
    case PMIX_UINT:
        /* to avoid alignment issues */
        memcpy(&p->data.uint, &src->data.uint, sizeof(unsigned int));
        break;
    case PMIX_UINT8:
        p->data.uint8 = src->data.uint8;
        break;
    case PMIX_UINT16:
    case PMIX_STOR_ACCESS_TYPE:
        /* to avoid alignment issues */
        memcpy(&p->data.uint16, &src->data.uint16, 2);
        break;
    case PMIX_UINT32:
        /* to avoid alignment issues */
        memcpy(&p->data.uint32, &src->data.uint32, 4);
        break;
    case PMIX_UINT64:
    case PMIX_STOR_MEDIUM:
    case PMIX_STOR_ACCESS:
    case PMIX_STOR_PERSIST:
       /* to avoid alignment issues */
        memcpy(&p->data.uint64, &src->data.uint64, 8);
        break;
    case PMIX_FLOAT:
        p->data.fval = src->data.fval;
        break;
    case PMIX_DOUBLE:
        p->data.dval = src->data.dval;
        break;
    case PMIX_TIMEVAL:
        memcpy(&p->data.tv, &src->data.tv, sizeof(struct timeval));
        break;
    case PMIX_TIME:
        memcpy(&p->data.time, &src->data.time, sizeof(time_t));
        break;
    case PMIX_STATUS:
        memcpy(&p->data.status, &src->data.status, sizeof(pmix_status_t));
        break;
    case PMIX_PROC_RANK:
        memcpy(&p->data.rank, &src->data.rank, sizeof(pmix_rank_t));
        break;
    case PMIX_PROC_NSPACE:
        return pmix_bfrops_base_copy_nspace_tma(&p->data.nspace, src->data.nspace, PMIX_PROC_NSPACE, tma);
    case PMIX_PROC:
        p->data.proc = pmix_bfrops_base_proc_create_tma(1, tma);
        if (PMIX_UNLIKELY(NULL == p->data.proc)) {
            return PMIX_ERR_NOMEM;
        }
        memcpy(p->data.proc, src->data.proc, sizeof(pmix_proc_t));
        break;
    case PMIX_BYTE_OBJECT:
    case PMIX_COMPRESSED_STRING:
    case PMIX_REGEX:
    case PMIX_COMPRESSED_BYTE_OBJECT:
        memset(&p->data.bo, 0, sizeof(pmix_byte_object_t));
        if (NULL != src->data.bo.bytes && 0 < src->data.bo.size) {
            p->data.bo.bytes = pmix_tma_malloc(tma, src->data.bo.size);
            memcpy(p->data.bo.bytes, src->data.bo.bytes, src->data.bo.size);
            p->data.bo.size = src->data.bo.size;
        } else {
            p->data.bo.bytes = NULL;
            p->data.bo.size = 0;
        }
        break;
    case PMIX_PERSIST:
        memcpy(&p->data.persist, &src->data.persist, sizeof(pmix_persistence_t));
        break;
    case PMIX_SCOPE:
        memcpy(&p->data.scope, &src->data.scope, sizeof(pmix_scope_t));
        break;
    case PMIX_DATA_RANGE:
        memcpy(&p->data.range, &src->data.range, sizeof(pmix_data_range_t));
        break;
    case PMIX_PROC_STATE:
        memcpy(&p->data.state, &src->data.state, sizeof(pmix_proc_state_t));
        break;
    case PMIX_PROC_INFO:
        return pmix_bfrops_base_copy_pinfo_tma(&p->data.pinfo, src->data.pinfo, PMIX_PROC_INFO, tma);
    case PMIX_DATA_ARRAY:
        // TODO(skg)
        return pmix_bfrops_base_copy_darray(&p->data.darray, src->data.darray, PMIX_DATA_ARRAY);
    case PMIX_POINTER:
        p->data.ptr = src->data.ptr;
        break;
    case PMIX_ALLOC_DIRECTIVE:
        memcpy(&p->data.adir, &src->data.adir, sizeof(pmix_alloc_directive_t));
        break;
    case PMIX_ENVAR:
        // No TMA required because PMIx_Envar_construct() does not allocate memory.
        PMIx_Envar_construct(&p->data.envar);

        if (NULL != src->data.envar.envar) {
            p->data.envar.envar = pmix_tma_strdup(tma, src->data.envar.envar);
        }
        if (NULL != src->data.envar.value) {
            p->data.envar.value = pmix_tma_strdup(tma, src->data.envar.value);
        }
        p->data.envar.separator = src->data.envar.separator;
        break;
    case PMIX_COORD:
        return pmix_bfrops_base_copy_coord_tma(&p->data.coord, src->data.coord, PMIX_COORD, tma);
    case PMIX_LINK_STATE:
        memcpy(&p->data.linkstate, &src->data.linkstate, sizeof(pmix_link_state_t));
        break;
    case PMIX_JOB_STATE:
        memcpy(&p->data.jstate, &src->data.jstate, sizeof(pmix_job_state_t));
        break;
    case PMIX_TOPO:
        rc = pmix_bfrops_base_copy_topology_tma(&p->data.topo, src->data.topo, PMIX_TOPO, tma);
        if (PMIX_ERR_INIT == rc || PMIX_ERR_NOT_SUPPORTED == rc) {
            /* we are being asked to do this before init, so
             * just copy the pointer across */
            p->data.topo = src->data.topo;
        }
        break;
    case PMIX_PROC_CPUSET:
        rc = pmix_bfrops_base_copy_cpuset_tma(&p->data.cpuset, src->data.cpuset, PMIX_PROC_CPUSET, tma);
        if (PMIX_ERR_INIT == rc || PMIX_ERR_NOT_SUPPORTED == rc) {
            /* we are being asked to do this before init, so
             * just copy the pointer across */
            p->data.cpuset = src->data.cpuset;
        }
        break;
    case PMIX_LOCTYPE:
        memcpy(&p->data.locality, &src->data.locality, sizeof(pmix_locality_t));
        break;
    case PMIX_GEOMETRY:
        return pmix_bfrops_base_copy_geometry_tma(&p->data.geometry, src->data.geometry, PMIX_GEOMETRY, tma);
    case PMIX_DEVTYPE:
        memcpy(&p->data.devtype, &src->data.devtype, sizeof(pmix_device_type_t));
        break;
    case PMIX_DEVICE_DIST:
        return pmix_bfrops_base_copy_devdist_tma(&p->data.devdist, src->data.devdist, PMIX_DEVICE_DIST, tma);
    case PMIX_ENDPOINT:
        return pmix_bfrops_base_copy_endpoint_tma(&p->data.endpoint, src->data.endpoint, PMIX_ENDPOINT, tma);
    case PMIX_REGATTR:
        return pmix_bfrops_base_copy_regattr_tma((pmix_regattr_t **) &p->data.ptr, src->data.ptr,
                                                 PMIX_REGATTR, tma);
    case PMIX_DATA_BUFFER:
        return pmix_bfrops_base_copy_dbuf_tma(&p->data.dbuf, src->data.dbuf, PMIX_DATA_BUFFER, tma);
    case PMIX_PROC_STATS:
        return pmix_bfrops_base_copy_pstats_tma(&p->data.pstats, src->data.pstats, PMIX_PROC_STATS, tma);
    case PMIX_DISK_STATS:
        return pmix_bfrops_base_copy_dkstats_tma(&p->data.dkstats, src->data.dkstats, PMIX_DISK_STATS, tma);
    case PMIX_NET_STATS:
        return pmix_bfrops_base_copy_netstats_tma(&p->data.netstats, src->data.netstats,
                                                  PMIX_NET_STATS, tma);
    case PMIX_NODE_STATS:
        return pmix_bfrops_base_copy_ndstats_tma(&p->data.ndstats, src->data.ndstats, PMIX_NODE_STATS, tma);
    default:
        pmix_output(0, "PMIX-XFER-VALUE: UNSUPPORTED TYPE %d", (int) src->type);
        return PMIX_ERROR;
    }
    return PMIX_SUCCESS;
}

#endif
