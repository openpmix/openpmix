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

#ifndef PMIX_BFROP_BASE_TMA_H
#define PMIX_BFROP_BASE_TMA_H

#include "src/include/pmix_config.h"
#include "src/include/pmix_globals.h"

#include "src/hwloc/pmix_hwloc.h"

#include "src/mca/preg/preg.h"
#include "src/mca/bfrops/base/base.h"

static inline void
pmix_bfrops_base_value_destruct_tma(
    pmix_value_t *v,
    pmix_tma_t *tma
);

static inline pmix_boolean_t
pmix_bfrops_base_value_true_tma(
    const pmix_value_t *value,
    pmix_tma_t *tma
);

static inline pmix_status_t
pmix_bfrops_base_value_xfer_tma(
    pmix_value_t *p,
    const pmix_value_t *src,
    pmix_tma_t *tma
);

static inline pmix_status_t
pmix_bfrops_base_value_load_tma(
    pmix_value_t *v,
    const void *data,
    pmix_data_type_t type,
    pmix_tma_t *tma
);

static inline void
pmix_bfrops_base_data_array_construct_tma(
    pmix_data_array_t *p,
    size_t num,
    pmix_data_type_t type,
    pmix_tma_t *tma
);

static inline void
pmix_bfrops_base_data_buffer_release_tma(
    pmix_data_buffer_t *b,
    pmix_tma_t *tma
);

static inline void
pmix_bfrops_base_proc_stats_free_tma(
    pmix_proc_stats_t *p,
    size_t n,
    pmix_tma_t *tma
);

static inline void
pmix_bfrops_base_disk_stats_free_tma(
    pmix_disk_stats_t *p,
    size_t n,
    pmix_tma_t *tma
);

static inline void
pmix_bfrops_base_node_stats_free_tma(
    pmix_node_stats_t *p,
    size_t n,
    pmix_tma_t *tma
);

static inline void
pmix_bfrops_base_net_stats_free_tma(
    pmix_net_stats_t *p,
    size_t n,
    pmix_tma_t *tma
);

static inline bool
pmix_bfrops_base_info_is_persistent_tma(
    const pmix_info_t *p,
    pmix_tma_t *tma
);

static inline void
pmix_bfrops_base_load_key_tma(
    pmix_key_t key,
    const char *src,
    pmix_tma_t *tma
) {
    PMIX_HIDE_UNUSED_PARAMS(tma);

    memset(key, 0, PMIX_MAX_KEYLEN + 1);
    if (NULL != src) {
        pmix_strncpy((char *)key, src, PMIX_MAX_KEYLEN);
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
    return (0 == pmix_nslen(nspace));
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

    memset(nspace, 0, PMIX_MAX_NSLEN + 1);
    if (NULL != str) {
        pmix_strncpy((char *)nspace, str, PMIX_MAX_NSLEN);
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
pmix_bfrops_base_data_buffer_load_tma(
    pmix_data_buffer_t *b,
    char *bytes,
    size_t sz,
    pmix_tma_t *tma
) {
    PMIX_HIDE_UNUSED_PARAMS(tma);

    pmix_byte_object_t bo;

    bo.bytes = bytes;
    bo.size = sz;
    // TODO(skg)
    PMIx_Data_load(b, &bo);
}

static inline void
pmix_bfrops_base_data_buffer_unload_tma(
    pmix_data_buffer_t *b,
    char **bytes,
    size_t *sz,
    pmix_tma_t *tma
) {
    PMIX_HIDE_UNUSED_PARAMS(tma);

    pmix_byte_object_t bo;
    pmix_status_t r;

    // TODO(skg)
    r = PMIx_Data_unload(b, &bo);
    if (PMIX_SUCCESS == r) {
        *bytes = bo.bytes;
        *sz = bo.size;
    } else {
        *bytes = NULL;
        *sz = 0;
    }
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
pmix_bfrops_base_multicluster_nspace_construct_tma(
    pmix_nspace_t target,
    pmix_nspace_t cluster,
    pmix_nspace_t nspace,
    pmix_tma_t *tma
) {
    pmix_bfrops_base_load_nspace_tma(target, NULL, tma);
    size_t len = pmix_nslen(cluster);
    if ((len + pmix_nslen(nspace)) < PMIX_MAX_NSLEN) {
        pmix_strncpy((char*)target, cluster, PMIX_MAX_NSLEN);
        target[len] = ':';
        pmix_strncpy((char*)&target[len+1], nspace, PMIX_MAX_NSLEN - len);
    }
}

static inline void
pmix_bfrops_base_multicluster_nspace_parse_tma(
    pmix_nspace_t target,
    pmix_nspace_t cluster,
    pmix_nspace_t nspace,
    pmix_tma_t *tma
) {
    size_t n, j;

    pmix_bfrops_base_load_nspace_tma(cluster, NULL, tma);
    for (n=0; '\0' != target[n] && ':' != target[n] && n < PMIX_MAX_NSLEN; n++) {
        cluster[n] = target[n];
    }
    n++;
    for (j=0; n < PMIX_MAX_NSLEN && '\0' != target[n]; n++, j++) {
        nspace[j] = target[n];
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

static inline void
pmix_bfrops_base_proc_info_destruct_tma(
    pmix_proc_info_t *p,
    pmix_tma_t *tma
) {
    if (NULL != p->hostname) {
        pmix_tma_free(tma, p->hostname);
    }
    if (NULL != p->executable_name) {
        pmix_tma_free(tma, p->executable_name);
    }
    pmix_bfrops_base_proc_info_construct_tma(p, tma);
}

static inline void
pmix_bfrops_base_proc_info_free_tma(
    pmix_proc_info_t *p,
    size_t n,
    pmix_tma_t *tma
) {
    if (NULL == p) {
        return;
    }
    for (size_t m = 0; m < n; m++) {
        pmix_bfrops_base_proc_info_destruct_tma(&p[m], tma);
    }
}

static inline pmix_proc_info_t *
pmix_bfrops_base_proc_info_create_tma(
    size_t n,
    pmix_tma_t *tma
) {
    if (0 == n) {
        return NULL;
    }
    pmix_proc_info_t *p = (pmix_proc_info_t *)pmix_tma_malloc(tma, n * sizeof(pmix_proc_info_t));
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

static inline void
pmix_bfrops_base_value_construct_tma(
    pmix_value_t *val,
    pmix_tma_t *tma
) {
    PMIX_HIDE_UNUSED_PARAMS(tma);

    memset(val, 0, sizeof(pmix_value_t));
    val->type = PMIX_UNDEF;
}

static inline pmix_value_t *
pmix_bfrops_base_value_create_tma(
    size_t n,
    pmix_tma_t *tma
) {
    if (0 == n) {
        return NULL;
    }
    pmix_value_t *v = (pmix_value_t *)pmix_tma_malloc(tma, n * sizeof(pmix_value_t));
    if (PMIX_UNLIKELY(NULL == v)) {
        return NULL;
    }
    for (size_t m = 0; m < n; m++) {
        pmix_bfrops_base_value_construct_tma(&v[m], tma);
    }
    return v;
}

static inline void
pmix_bfrops_base_info_destruct_tma(
    pmix_info_t *p,
    pmix_tma_t *tma
) {
    if (!pmix_bfrops_base_info_is_persistent_tma(p, tma)) {
        pmix_bfrops_base_value_destruct_tma(&p->value, tma);
    }
}

static inline void
pmix_bfrops_base_info_free_tma(
    pmix_info_t *p,
    size_t n,
    pmix_tma_t *tma
) {
    if (NULL == p) {
        return;
    }
    for (size_t m = 0; m < n; m++) {
        pmix_bfrops_base_info_destruct_tma(&p[m], tma);
    }
}

static inline void
pmix_bfrops_base_info_construct_tma(
    pmix_info_t *p,
    pmix_tma_t *tma
) {
    pmix_bfrops_base_load_key_tma(p->key, NULL, tma);
    p->flags = ~PMIX_INFO_PERSISTENT;  // default to non-persistent for historical reasons
    pmix_bfrops_base_value_construct_tma(&p->value, tma);
}

static inline pmix_info_t *
pmix_bfrops_base_info_create_tma(
    size_t n,
    pmix_tma_t *tma
) {
    if (0 == n) {
        return NULL;
    }
    pmix_info_t *i = (pmix_info_t *)pmix_tma_malloc(tma, n * sizeof(pmix_info_t));
    if (PMIX_UNLIKELY(NULL == i)) {
        return NULL;
    }
    for (size_t m = 0; m < n; m++) {
        pmix_bfrops_base_info_construct_tma(&i[m], tma);
    }
    return i;
}

static inline pmix_status_t
pmix_bfrops_base_info_load_tma(
    pmix_info_t *info,
    const char *key,
    const void *data,
    pmix_data_type_t type,
    pmix_tma_t *tma
) {
    if (NULL == key) {
        return PMIX_ERR_BAD_PARAM;
    }
    pmix_bfrops_base_load_key_tma(info->key, key, tma);
    info->flags = 0;
    return pmix_bfrops_base_value_load_tma(&info->value, data, type, tma);
}

static inline pmix_boolean_t
pmix_bfrops_base_info_true_tma(
    const pmix_info_t *p,
    pmix_tma_t *tma
) {
    return pmix_bfrops_base_value_true_tma(&p->value, tma);
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
pmix_bfrops_base_info_required_tma(
    pmix_info_t *p,
    pmix_tma_t *tma
) {
    PMIX_HIDE_UNUSED_PARAMS(tma);

    p->flags |= PMIX_INFO_REQD;
}

static inline bool
pmix_bfrops_base_info_is_required(
    const pmix_info_t *p,
    pmix_tma_t *tma
) {
    PMIX_HIDE_UNUSED_PARAMS(tma);

    return (p->flags & PMIX_INFO_REQD);
}

static inline void
pmix_bfrops_base_info_optional_tma(
    pmix_info_t *p,
    pmix_tma_t *tma
) {
    PMIX_HIDE_UNUSED_PARAMS(tma);

    p->flags &= ~PMIX_INFO_REQD;
}

static inline bool
pmix_bfrops_base_info_is_optional_tma(
    const pmix_info_t *p,
    pmix_tma_t *tma
) {
    PMIX_HIDE_UNUSED_PARAMS(tma);

    return (!(p->flags & PMIX_INFO_REQD));
}

static inline bool
pmix_bfrops_base_info_was_processed_tma(
    const pmix_info_t *p,
    pmix_tma_t *tma
) {
    PMIX_HIDE_UNUSED_PARAMS(tma);

    return (p->flags & PMIX_INFO_REQD_PROCESSED);
}

static inline void
pmix_bfrops_base_info_set_end_tma(
    pmix_info_t *p,
    pmix_tma_t *tma
) {
    PMIX_HIDE_UNUSED_PARAMS(tma);

    p->flags |= PMIX_INFO_ARRAY_END;
}

static inline bool
pmix_bfrops_base_info_is_end_tma(
    const pmix_info_t *p,
    pmix_tma_t *tma
) {
    PMIX_HIDE_UNUSED_PARAMS(tma);

    return (p->flags & PMIX_INFO_ARRAY_END);
}

static inline void
pmix_bfrops_base_info_qualifier_tma(
    pmix_info_t *p,
    pmix_tma_t *tma
) {
    PMIX_HIDE_UNUSED_PARAMS(tma);

    p->flags |= PMIX_INFO_QUALIFIER;
}

static inline bool
pmix_bfrops_base_info_is_qualifier_tma(
    const pmix_info_t *p,
    pmix_tma_t *tma
) {
    PMIX_HIDE_UNUSED_PARAMS(tma);

    return (p->flags & PMIX_INFO_QUALIFIER);
}

static inline void
pmix_bfrops_base_info_persistent_tma(
    pmix_info_t *p,
    pmix_tma_t *tma
) {
    PMIX_HIDE_UNUSED_PARAMS(tma);

    p->flags |= PMIX_INFO_PERSISTENT;
}

static inline bool
pmix_bfrops_base_info_is_persistent_tma(
    const pmix_info_t *p,
    pmix_tma_t *tma
) {
    PMIX_HIDE_UNUSED_PARAMS(tma);

    return (p->flags & PMIX_INFO_PERSISTENT);
}

static inline pmix_status_t
pmix_bfrops_base_info_xfer_tma(
    pmix_info_t *dest,
    const pmix_info_t *src,
    pmix_tma_t *tma
) {
    pmix_status_t rc;

    if (NULL == dest || NULL == src) {
        return PMIX_ERR_BAD_PARAM;
    }
    pmix_bfrops_base_load_key_tma(dest->key, src->key, tma);
    dest->flags = src->flags;
    if (pmix_bfrops_base_info_is_persistent_tma(src, tma)) {
        memcpy(&dest->value, &src->value, sizeof(pmix_value_t));
        rc = PMIX_SUCCESS;
    } else {
        rc = pmix_bfrops_base_value_xfer_tma(&dest->value, &src->value, tma);
    }
    return rc;
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

static inline void
pmix_bfrops_base_topology_destruct_tma(
    pmix_topology_t *t,
    pmix_tma_t *tma
) {
    PMIX_HIDE_UNUSED_PARAMS(tma);
    // TODO(skg)
    pmix_hwloc_destruct_topology(t);
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
pmix_bfrops_base_topology_free_tma(
    pmix_topology_t *t,
    size_t n,
    pmix_tma_t *tma
) {
    if (NULL == t) {
        return;
    }
    for (size_t m = 0; m < n; m++) {
        pmix_bfrops_base_topology_destruct_tma(&t[m], tma);
    }
}

static inline void
pmix_bfrops_base_cpuset_construct_tma(
    pmix_cpuset_t *c,
    pmix_tma_t *tma
) {
    PMIX_HIDE_UNUSED_PARAMS(tma);

    memset(c, 0, sizeof(pmix_cpuset_t));
}

static inline void
pmix_bfrops_base_cpuset_destruct_tma(
    pmix_cpuset_t *c,
    pmix_tma_t *tma
) {
    PMIX_HIDE_UNUSED_PARAMS(tma);
    // TODO(skg)
    pmix_hwloc_destruct_cpuset(c);
}

static inline void
pmix_bfrops_base_cpuset_free_tma(
    pmix_cpuset_t *c,
    size_t n,
    pmix_tma_t *tma
) {
    if (NULL == c) {
        return;
    }
    for (size_t m = 0; m < n; m++) {
        pmix_bfrops_base_cpuset_destruct_tma(&c[m], tma);
    }
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
    // TODO(skg)
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

static inline pmix_coord_t *
pmix_bfrops_base_coord_create_tma(
    size_t dims,
    size_t number,
    pmix_tma_t *tma
) {
    if (0 == number) {
        return NULL;
    }
    pmix_coord_t *m = (pmix_coord_t *)pmix_tma_malloc(tma, number * sizeof(pmix_coord_t));
    if (PMIX_UNLIKELY(NULL == m)) {
        return NULL;
    }
    m->view = PMIX_COORD_VIEW_UNDEF;
    m->dims = dims;
    if (0 == dims) {
        m->coord = NULL;
    } else {
        m->coord = (uint32_t *)pmix_tma_malloc(tma, dims * sizeof(uint32_t));
        if (NULL != m->coord) {
            memset(m->coord, 0, dims * sizeof(uint32_t));
        }
    }
    return m;
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
pmix_bfrops_base_device_distance_destruct_tma(
    pmix_device_distance_t *d,
    pmix_tma_t *tma
) {
    if (NULL != (d->uuid)) {
        pmix_tma_free(tma, d->uuid);
    }
    if (NULL != (d->osname)) {
        pmix_tma_free(tma, d->osname);
    }
}

static inline void
pmix_bfrops_base_device_distance_free_tma(
    pmix_device_distance_t *d,
    size_t n,
    pmix_tma_t *tma
) {
    if (NULL == d) {
        return;
    }
    for (size_t m = 0; m < n; m++) {
        pmix_bfrops_base_device_distance_destruct_tma(&d[m], tma);
    }
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
pmix_bfrops_base_byte_object_construct_tma(
    pmix_byte_object_t *b,
    pmix_tma_t *tma
) {
    PMIX_HIDE_UNUSED_PARAMS(tma);

    b->bytes = NULL;
    b->size = 0;
}

static inline void
pmix_bfrops_base_byte_object_destruct_tma(
    pmix_byte_object_t *b,
    pmix_tma_t *tma
) {
    if (NULL != b->bytes) {
        pmix_tma_free(tma, b->bytes);
    }
    b->bytes = NULL;
    b->size = 0;
}

static inline pmix_byte_object_t *
pmix_bfrops_base_byte_object_create_tma(
    size_t n,
    pmix_tma_t *tma
) {
    if (0 == n) {
        return NULL;
    }
    pmix_byte_object_t *b = (pmix_byte_object_t *)pmix_tma_malloc(tma, n * sizeof(pmix_byte_object_t));
    if (PMIX_LIKELY(NULL != b)) {
        for (size_t m = 0; m < n; m++) {
            pmix_bfrops_base_byte_object_construct_tma(&b[m], tma);
        }
    }
    return b;
}

static inline void
pmix_bfrops_base_byte_object_free_tma(
    pmix_byte_object_t *b,
    size_t n,
    pmix_tma_t *tma
) {
    if (NULL == b) {
        return;
    }
    for (size_t m = 0; m < n; m++) {
        pmix_bfrops_base_byte_object_destruct_tma(&b[m], tma);
    }
}

static inline void
pmix_bfrops_base_endpoint_construct_tma(
    pmix_endpoint_t *e,
    pmix_tma_t *tma
) {
    PMIX_HIDE_UNUSED_PARAMS(tma);

    memset(e, 0, sizeof(pmix_endpoint_t));
}

static inline void
pmix_bfrops_base_endpoint_destruct_tma(
    pmix_endpoint_t *e,
    pmix_tma_t *tma
) {
    if (NULL != e->uuid) {
        pmix_tma_free(tma, e->uuid);
    }
    if (NULL != e->osname) {
        pmix_tma_free(tma, e->osname);
    }
    if (NULL != e->endpt.bytes) {
        pmix_tma_free(tma, e->endpt.bytes);
    }
}

static inline void
pmix_bfrops_base_endpoint_free_tma(
    pmix_endpoint_t *e,
    size_t n,
    pmix_tma_t *tma
) {
    if (NULL == e) {
        return;
    }
    for (size_t m = 0; m < n; m++) {
        pmix_bfrops_base_endpoint_destruct_tma(&e[m], tma);
    }
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
        argc = pmix_bfrops_base_argv_count_tma(*argv, tma);

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

static inline pmix_status_t
pmix_bfrops_base_argv_prepend_nosize_tma(
    char ***argv,
    const char *arg,
    pmix_tma_t *tma
) {
    int argc;
    int i;

    /* Create new argv. */

    if (NULL == *argv) {
        *argv = (char **)pmix_tma_malloc(tma, 2 * sizeof(char *));
        if (NULL == *argv) {
            return PMIX_ERR_OUT_OF_RESOURCE;
        }
        (*argv)[0] = pmix_tma_strdup(tma, arg);
        (*argv)[1] = NULL;
    } else {
        /* count how many entries currently exist */
        argc = pmix_bfrops_base_argv_count_tma(*argv, tma);

        *argv = (char **)pmix_tma_realloc(tma, *argv, (argc + 2) * sizeof(char *));
        if (NULL == *argv) {
            return PMIX_ERR_OUT_OF_RESOURCE;
        }
        (*argv)[argc + 1] = NULL;

        /* shift all existing elements down 1 */
        for (i = argc; 0 < i; i--) {
            (*argv)[i] = (*argv)[i - 1];
        }
        (*argv)[0] = pmix_tma_strdup(tma, arg);
    }

    return PMIX_SUCCESS;
}

static inline pmix_status_t
pmix_bfrops_base_argv_append_unique_nosize_tma(
    char ***argv,
    const char *arg,
    pmix_tma_t *tma
) {
    int i;

    /* if the provided array is NULL, then the arg cannot be present,
     * so just go ahead and append
     */
    if (NULL == *argv) {
        return pmix_bfrops_base_argv_append_nosize_tma(argv, arg, tma);
    }

    /* see if this arg is already present in the array */
    for (i = 0; NULL != (*argv)[i]; i++) {
        if (0 == strcmp(arg, (*argv)[i])) {
            /* already exists */
            return PMIX_SUCCESS;
        }
    }

    /* we get here if the arg is not in the array - so add it */
    return pmix_bfrops_base_argv_append_nosize_tma(argv, arg, tma);
}

static inline void
pmix_bfrops_base_argv_free_tma(
    char **argv,
    pmix_tma_t *tma
) {
    if (NULL == argv) {
        return;
    }
    for (char **p = argv; NULL != *p; ++p) {
        pmix_tma_free(tma, *p);
    }
    pmix_tma_free(tma, argv);
}

static inline char **
pmix_bfrops_base_argv_split_inter_tma(
    const char *src_string,
    int delimiter,
    bool include_empty,
    pmix_tma_t *tma
) {
    char arg[512];
    char **argv = NULL;
    const char *p;
    char *argtemp;
    size_t arglen;

    while (src_string && *src_string) {
        p = src_string;
        arglen = 0;

        while (('\0' != *p) && (*p != delimiter)) {
            ++p;
            ++arglen;
        }

        /* zero length argument, skip */

        if (src_string == p) {
            if (include_empty) {
                arg[0] = '\0';
                if (PMIX_SUCCESS != pmix_bfrops_base_argv_append_nosize_tma(&argv, arg, NULL)) {
                    return NULL;
                }
            }
            src_string = p + 1;
            continue;
        }

        /* tail argument, add straight from the original string */

        else if ('\0' == *p) {
            if (PMIX_SUCCESS != pmix_bfrops_base_argv_append_nosize_tma(&argv, src_string, tma)) {
                return NULL;
            }
            src_string = p;
            continue;
        }

        /* long argument, malloc buffer, copy and add */

        else if (arglen > 511) {
            argtemp = (char *)pmix_tma_malloc(tma, arglen + 1);
            if (NULL == argtemp)
                return NULL;

            pmix_strncpy(argtemp, src_string, arglen);
            argtemp[arglen] = '\0';

            if (PMIX_SUCCESS != pmix_bfrops_base_argv_append_nosize_tma(&argv, argtemp, tma)) {
                free(argtemp);
                return NULL;
            }

            pmix_tma_free(tma, argtemp);
        }

        /* short argument, copy to buffer and add */

        else {
            pmix_strncpy(arg, src_string, arglen);
            arg[arglen] = '\0';

            if (PMIX_SUCCESS != pmix_bfrops_base_argv_append_nosize_tma(&argv, arg, tma)) {
                return NULL;
            }
        }

        src_string = p + 1;
    }

    /* All done */

    return argv;
}

static inline char **
pmix_bfrops_base_argv_split_with_empty_tma(
    const char *src_string,
    int delimiter,
    pmix_tma_t *tma
) {
    return pmix_bfrops_base_argv_split_inter_tma(src_string, delimiter, true, tma);
}

static inline char **
pmix_bfrops_base_argv_split_tma(
    const char *src_string,
    int delimiter,
    pmix_tma_t *tma
) {
    return pmix_bfrops_base_argv_split_inter_tma(src_string, delimiter, false, tma);
}

static inline char *
pmix_bfrops_base_argv_join_tma(
    char **argv,
    int delimiter,
    pmix_tma_t *tma
) {
    char **p;
    char *pp;
    char *str;
    size_t str_len = 0;
    size_t i;

    /* Bozo case */

    if (NULL == argv || NULL == argv[0]) {
        return pmix_tma_strdup(tma, "");
    }

    /* Find the total string length in argv including delimiters.  The
     last delimiter is replaced by the NULL character. */

    for (p = argv; *p; ++p) {
        str_len += strlen(*p) + 1;
    }

    /* Allocate the string. */

    if (NULL == (str = (char *)pmix_tma_malloc(tma, str_len)))
        return NULL;

    /* Loop filling in the string. */

    str[--str_len] = '\0';
    p = argv;
    pp = *p;

    for (i = 0; i < str_len; ++i) {
        if ('\0' == *pp) {

            /* End of a string, fill in a delimiter and go to the next
             string. */

            str[i] = (char) delimiter;
            ++p;
            pp = *p;
        } else {
            str[i] = *pp++;
        }
    }

    /* All done */

    return str;
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
            pmix_bfrops_base_argv_free_tma(dupv, tma);
            return NULL;
        }
        ++argv;
    }
    /* All done */
    return dupv;
}

static inline void
pmix_bfrops_base_query_destruct_tma(
    pmix_query_t *p,
    pmix_tma_t *tma
) {
    if (NULL != p->keys) {
        pmix_bfrops_base_argv_free_tma(p->keys, tma);
    }
    if (NULL != p->qualifiers) {
        pmix_bfrops_base_info_free_tma(p->qualifiers, p->nqual, tma);
        pmix_tma_free(tma, p->qualifiers);
        p->qualifiers = NULL;
        p->nqual = 0;
    }
}

static inline void
pmix_bfrops_base_query_free_tma(
    pmix_query_t *p,
    size_t n,
    pmix_tma_t *tma
) {
    if (NULL != p) {
        for (size_t m = 0; m < n; m++) {
            pmix_bfrops_base_query_destruct_tma(&p[m], tma);
        }
    }
}

static inline void
pmix_bfrops_base_query_release_tma(
    pmix_query_t *p,
    pmix_tma_t *tma
) {
    pmix_bfrops_base_query_free_tma(p, 1, tma);
}

static inline void
pmix_bfrops_base_pdata_construct_tma(
    pmix_pdata_t *p,
    pmix_tma_t *tma
) {
    PMIX_HIDE_UNUSED_PARAMS(tma);

    memset(p, 0, sizeof(pmix_pdata_t));
    p->value.type = PMIX_UNDEF;
}

static inline void
pmix_bfrops_base_pdata_destruct_tma(
    pmix_pdata_t *p,
    pmix_tma_t *tma
) {
    pmix_bfrops_base_value_destruct_tma(&p->value, tma);
}

static inline pmix_pdata_t *
pmix_bfrops_base_pdata_create_tma(
    size_t n,
    pmix_tma_t *tma
) {
    if (0 == n) {
        return NULL;
    }
    pmix_pdata_t *p = (pmix_pdata_t *)pmix_tma_malloc(tma, n * sizeof(pmix_pdata_t));
    if (PMIX_LIKELY(NULL != p)) {
        for (size_t m = 0; m < n; m++) {
            pmix_bfrops_base_pdata_construct_tma(&p[m], tma);
        }
    }
    return p;
}

static inline void
pmix_bfrops_base_pdata_free_tma(
    pmix_pdata_t *p,
    size_t n,
    pmix_tma_t *tma
) {
    if (NULL != p) {
        for (size_t m = 0; m < n; m++) {
            pmix_bfrops_base_pdata_destruct_tma(&p[m], tma);
        }
    }
}

static inline void
pmix_bfrops_base_app_destruct_tma(
    pmix_app_t *p,
    pmix_tma_t *tma
) {
    if (NULL != p->cmd) {
        pmix_tma_free(tma, p->cmd);
        p->cmd = NULL;
    }
    if (NULL != p->argv) {
        pmix_bfrops_base_argv_free_tma(p->argv, tma);
        p->argv = NULL;
    }
    if (NULL != p->env) {
        pmix_bfrops_base_argv_free_tma(p->env, tma);
        p->env = NULL;
    }
    if (NULL != p->cwd) {
        pmix_tma_free(tma, p->cwd);
        p->cwd = NULL;
    }
    if (NULL != p->info) {
        pmix_bfrops_base_info_free_tma(p->info, p->ninfo, tma);
        p->info = NULL;
        p->ninfo = 0;
    }
}

static inline void
pmix_bfrops_base_app_construct_tma(
    pmix_app_t *p,
    pmix_tma_t *tma
) {
    PMIX_HIDE_UNUSED_PARAMS(tma);

    memset(p, 0, sizeof(pmix_app_t));
}

static inline pmix_app_t *
pmix_bfrops_base_app_create_tma(
    size_t n,
    pmix_tma_t *tma
) {
    if (0 == n) {
        return NULL;
    }
    pmix_app_t *p = (pmix_app_t *)pmix_tma_malloc(tma, n * sizeof(pmix_app_t));
    if (PMIX_LIKELY(NULL != p)) {
        for (size_t m = 0; m < n; m++) {
            pmix_bfrops_base_app_construct_tma(&p[m], tma);
        }
    }
    return p;
}

static inline void
pmix_bfrops_base_app_info_create_tma(
    pmix_app_t *p,
    size_t n,
    pmix_tma_t *tma
) {
    p->ninfo = n;
    p->info = pmix_bfrops_base_info_create_tma(n, tma);
}

static inline void
pmix_bfrops_base_app_free_tma(
    pmix_app_t *p,
    size_t n,
    pmix_tma_t *tma
) {
    if (NULL != p) {
        for (size_t m = 0; m < n; m++) {
            pmix_bfrops_base_app_destruct_tma(&p[m], tma);
        }
    }
}

static inline void
pmix_bfrops_base_app_release_tma(
    pmix_app_t *p,
    pmix_tma_t *tma
) {
    pmix_bfrops_base_app_free_tma(p, 1, tma);
}

static inline void
pmix_bfrops_base_query_construct_tma(
    pmix_query_t *p,
    pmix_tma_t *tma
) {
    PMIX_HIDE_UNUSED_PARAMS(tma);

    memset(p, 0, sizeof(pmix_query_t));
}

static inline pmix_query_t *
pmix_bfrops_base_query_create_tma(
    size_t n,
    pmix_tma_t *tma
) {
    if (0 == n) {
        return NULL;
    }
    pmix_query_t *p = (pmix_query_t *)pmix_tma_malloc(tma, n * sizeof(pmix_query_t));
    if (PMIX_LIKELY(NULL != p)) {
        for (size_t m = 0; m < n; m++) {
            pmix_bfrops_base_query_construct_tma(&p[m], tma);
        }
    }
    return p;
}

static inline void
pmix_bfrops_base_query_qualifiers_create_tma(
    pmix_query_t *p,
    size_t n,
    pmix_tma_t *tma
) {
    p->nqual = n;
    p->qualifiers = pmix_bfrops_base_info_create_tma(n, tma);
}

static inline void
pmix_bfrops_base_regattr_destruct_tma(
    pmix_regattr_t *p,
    pmix_tma_t *tma
) {
    if (NULL != p) {
        if (NULL != p->name) {
            pmix_tma_free(tma, p->name);
        }
        if (NULL != p->description) {
            pmix_bfrops_base_argv_free_tma(p->description, tma);
        }
    }
}

static inline void
pmix_bfrops_base_regattr_free_tma(
    pmix_regattr_t *p,
    size_t n,
    pmix_tma_t *tma
) {
    if (NULL != p) {
        for (size_t m = 0; m < n; m++) {
            pmix_bfrops_base_regattr_destruct_tma(&p[m], tma);
        }
    }
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

static inline void
pmix_bfrops_base_regattr_load_tma(
    pmix_regattr_t *p,
    const char *name,
    const char *key,
    pmix_data_type_t type,
    const char *description,
    pmix_tma_t *tma
) {
    if (NULL != name) {
        p->name = pmix_tma_strdup(tma, name);
    }
    if (NULL != key) {
        pmix_bfrops_base_load_key_tma(p->string, key, tma);
    }
    p->type = type;
    if (NULL != description) {
        pmix_bfrops_base_argv_append_nosize_tma(&p->description, description, tma);
    }
}

static inline void
pmix_bfrops_base_regattr_xfer_tma(
    pmix_regattr_t *dest,
    const pmix_regattr_t *src,
    pmix_tma_t *tma
) {
    pmix_bfrops_base_regattr_construct_tma(dest, tma);
    if (NULL != (src->name)) {
        dest->name = pmix_tma_strdup(tma, src->name);
    }
    pmix_bfrops_base_load_key_tma(dest->string, src->string, tma);
    dest->type = src->type;
    if (NULL != src->description) {
        dest->description = pmix_bfrops_base_argv_copy_tma(src->description, tma);
    }
}

static inline pmix_status_t
pmix_bfrops_base_setenv_tma(
    const char *name,
    const char *value,
    bool overwrite,
    char ***env,
    pmix_tma_t *tma
) {
    int i;
    char newvalue[100000], compare[100000];
    size_t len;
    bool valid;

    /* Check the bozo case */
    if (NULL == env) {
        return PMIX_ERR_BAD_PARAM;
    }

    if (NULL != value) {
        /* check the string for unacceptable length - i.e., ensure
         * it is NULL-terminated */
        valid = false;
        for (i = 0; i < 100000; i++) {
            if ('\0' == value[i]) {
                valid = true;
                break;
            }
        }
        if (!valid) {
            return PMIX_ERR_BAD_PARAM;
        }
    }

    /* If this is the "environ" array, use setenv */
    if (*env == environ) {
        if (NULL == value) {
            /* this is actually an unsetenv request */
            unsetenv(name);
        } else {
            setenv(name, value, overwrite);
        }
        return PMIX_SUCCESS;
    }

    /* Make the new value */
    if (NULL == value) {
        snprintf(newvalue, 100000, "%s=", name);
    } else {
        snprintf(newvalue, 100000, "%s=%s", name, value);
    }

    if (NULL == *env) {
        pmix_bfrops_base_argv_append_nosize_tma(env, newvalue, tma);
        return PMIX_SUCCESS;
    }

    /* Make something easy to compare to */

    snprintf(compare, 100000, "%s=", name);
    len = strlen(compare);

    /* Look for a duplicate that's already set in the env */

    for (i = 0; (*env)[i] != NULL; ++i) {
        if (0 == strncmp((*env)[i], compare, len)) {
            if (overwrite) {
                pmix_tma_free(tma, (*env)[i]);
                (*env)[i] = pmix_tma_strdup(tma, newvalue);
                return PMIX_SUCCESS;
            } else {
                return PMIX_ERR_EXISTS;
            }
        }
    }

    /* If we found no match, append this value */

    pmix_bfrops_base_argv_append_nosize_tma(env, newvalue, tma);

    /* All done */
    return PMIX_SUCCESS;
}

static inline void
pmix_bfrops_base_data_array_init_tma(
    pmix_data_array_t *p,
    pmix_data_type_t type,
    pmix_tma_t *tma
) {
    PMIX_HIDE_UNUSED_PARAMS(tma);

    p->array = NULL;
    p->type = type;
    p->size = 0;
}

static inline void
pmix_bfrops_base_data_array_destruct_tma(
    pmix_data_array_t *d,
    pmix_tma_t *tma
) {
    PMIX_HIDE_UNUSED_PARAMS(tma);
    // TODO(skg)
    pmix_bfrops_base_darray_destruct(d);
}

static inline void
pmix_bfrops_base_data_array_free_tma(
    pmix_data_array_t *p,
    pmix_tma_t *tma
) {
    if (NULL != p) {
        pmix_bfrops_base_data_array_destruct_tma(p, tma);
    }
}

static inline pmix_data_array_t *
pmix_bfrops_base_data_array_create_tma(
    size_t n,
    pmix_data_type_t type,
    pmix_tma_t *tma
) {
    if (0 == n) {
        return NULL;
    }
    pmix_data_array_t *p = (pmix_data_array_t *)pmix_tma_malloc(tma, sizeof(pmix_data_array_t));
    if (PMIX_LIKELY(NULL != p)) {
        pmix_bfrops_base_data_array_construct_tma(p, n, type, tma);
    }
    return p;
}

static inline void
pmix_bfrops_base_value_destruct_tma(
    pmix_value_t *v,
    pmix_tma_t *tma
) {
    switch (v->type) {
        case PMIX_STRING:
            if (NULL != v->data.string) {
                pmix_tma_free(tma, v->data.string);
            }
            break;
        case PMIX_PROC:
            if (NULL != v->data.proc) {
                pmix_bfrops_base_proc_free_tma(v->data.proc, 1, tma);
            }
            break;
        case PMIX_BYTE_OBJECT:
        case PMIX_COMPRESSED_STRING:
        case PMIX_COMPRESSED_BYTE_OBJECT:
            if (NULL != v->data.bo.bytes) {
                pmix_tma_free(tma, v->data.bo.bytes);
            }
            break;
        case PMIX_PROC_INFO:
            if (NULL != v->data.pinfo) {
                pmix_bfrops_base_proc_info_free_tma(v->data.pinfo, 1, tma);
            }
            break;
        case PMIX_DATA_ARRAY:
            if (NULL != v->data.darray) {
                pmix_bfrops_base_data_array_free_tma(v->data.darray, tma);
            }
            break;
        case PMIX_ENVAR:
            if (NULL != v->data.envar.envar) {
                pmix_tma_free(tma, v->data.envar.envar);
            }
            if (NULL != v->data.envar.value) {
                pmix_tma_free(tma, v->data.envar.value);
            }
            break;
        case PMIX_COORD:
            if (NULL != v->data.coord) {
                pmix_bfrops_base_coord_free_tma(v->data.coord, 1, tma);
            }
            break;
        case PMIX_TOPO:
            if (NULL != v->data.topo) {
                // TODO(skg)
                pmix_hwloc_release_topology(v->data.topo, 1);
            }
            break;
        case PMIX_PROC_CPUSET:
            if (NULL != v->data.cpuset) {
                // TODO(skg)
                pmix_hwloc_release_cpuset(v->data.cpuset, 1);
            }
            break;
        case PMIX_GEOMETRY:
            if (NULL != v->data.geometry) {
                pmix_bfrops_base_geometry_free_tma(v->data.geometry, 1, tma);
            }
            break;
        case PMIX_DEVICE_DIST:
            if (NULL != v->data.devdist) {
                pmix_bfrops_base_device_distance_free_tma(v->data.devdist, 1, tma);
            }
            break;
        case PMIX_ENDPOINT:
            if (NULL != v->data.endpoint) {
                pmix_bfrops_base_endpoint_free_tma(v->data.endpoint, 1, tma);
            }
            break;
        case PMIX_REGATTR:
            if (NULL != v->data.ptr) {
                pmix_bfrops_base_regattr_free_tma(v->data.ptr, 1, tma);
            }
            break;
        case PMIX_REGEX:
            if (NULL != v->data.bo.bytes) {
                // TODO(skg)
                pmix_preg.release(v->data.bo.bytes);
            }
            break;
        case PMIX_DATA_BUFFER:
            if (NULL != v->data.dbuf) {
                pmix_bfrops_base_data_buffer_release_tma(v->data.dbuf, tma);
            }
            break;
        case PMIX_PROC_STATS:
            if (NULL != v->data.pstats) {
                pmix_bfrops_base_proc_stats_free_tma(v->data.pstats, 1, tma);
                pmix_tma_free(tma, v->data.pstats);
                v->data.pstats = NULL;
            }
            break;
        case PMIX_DISK_STATS:
            if (NULL != v->data.dkstats) {
                pmix_bfrops_base_disk_stats_free_tma(v->data.dkstats, 1, tma);
                pmix_tma_free(tma, v->data.dkstats);
                v->data.dkstats = NULL;
            }
            break;
        case PMIX_NET_STATS:
            if (NULL != v->data.netstats) {
                pmix_bfrops_base_net_stats_free_tma(v->data.netstats, 1, tma);
                pmix_tma_free(tma, v->data.netstats);
                v->data.netstats = NULL;
            }
            break;
        case PMIX_NODE_STATS:
            if (NULL != v->data.ndstats) {
                pmix_bfrops_base_node_stats_free_tma(v->data.ndstats, 1, tma);
                pmix_tma_free(tma, v->data.ndstats);
                v->data.ndstats = NULL;
            }
            break;

        default:
            /* silence warnings */
            break;
    }
    // mark the value as no longer defined
    memset(v, 0, sizeof(pmix_value_t));
    v->type = PMIX_UNDEF;
    return;
}

static inline void
pmix_bfrops_base_value_free_tma(
    pmix_value_t *v,
    size_t n,
    pmix_tma_t *tma
) {
    if (NULL == v) {
        return;
    }
    for (size_t m = 0; m < n; m++) {
        pmix_bfrops_base_value_destruct_tma(&v[m], tma);
    }
}

static inline pmix_status_t
pmix_bfrops_base_value_load_tma(
    pmix_value_t *v,
    const void *data,
    pmix_data_type_t type,
    pmix_tma_t *tma
) {
    PMIX_HIDE_UNUSED_PARAMS(tma);
    // TODO(skg)
    pmix_bfrops_base_value_load(v, data, type);
    return PMIX_SUCCESS;
}

static inline pmix_status_t
pmix_bfrops_base_value_unload_tma(
    pmix_value_t *kv,
    void **data,
    size_t *sz,
    pmix_tma_t *tma
) {
    PMIX_HIDE_UNUSED_PARAMS(tma);
    // TODO(skg)
    return pmix_bfrops_base_value_unload(kv, data, sz);
}

static inline pmix_boolean_t
pmix_bfrops_base_value_true_tma(
    const pmix_value_t *value,
    pmix_tma_t *tma
) {
    PMIX_HIDE_UNUSED_PARAMS(tma);

    char *ptr;

    if (PMIX_UNDEF == value->type) {
        return PMIX_BOOL_TRUE; // default to true
    }
    if (PMIX_BOOL == value->type) {
        if (value->data.flag) {
            return PMIX_BOOL_TRUE;
        } else {
            return PMIX_BOOL_FALSE;
        }
    }
    if (PMIX_STRING == value->type) {
        if (NULL == value->data.string) {
            return PMIX_BOOL_TRUE;
        }
        ptr = value->data.string;
        /* Trim leading whitespace */
        while (isspace(*ptr)) {
            ++ptr;
        }
        if ('\0' == *ptr) {
            return PMIX_BOOL_TRUE;
        }
        if (isdigit(*ptr)) {
            if (0 == atoi(ptr)) {
                return PMIX_BOOL_FALSE;
            } else {
                return PMIX_BOOL_TRUE;
            }
        } else if (0 == strncasecmp(ptr, "yes", 3) ||
                   0 == strncasecmp(ptr, "true", 4)) {
            return PMIX_BOOL_TRUE;
        } else if (0 == strncasecmp(ptr, "no", 2) ||
                   0 == strncasecmp(ptr, "false", 5)) {
            return PMIX_BOOL_FALSE;
        }
    }

    return PMIX_NON_BOOL;
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
pmix_bfrops_base_envar_construct_tma(
    pmix_envar_t *e,
    pmix_tma_t *tma
) {
    PMIX_HIDE_UNUSED_PARAMS(tma);

    e->envar = NULL;
    e->value = NULL;
    e->separator = '\0';
}

static inline void
pmix_bfrops_base_envar_destruct_tma(
    pmix_envar_t *e,
    pmix_tma_t *tma
) {
    if (NULL != e->envar) {
        pmix_tma_free(tma, e->envar);
        e->envar = NULL;
    }
    if (NULL != e->value) {
        pmix_tma_free(tma, e->value);
        e->value = NULL;
    }
}

static inline pmix_envar_t *
pmix_bfrops_base_envar_create_tma(
    size_t n,
    pmix_tma_t *tma
) {
    if (0 == n) {
        return NULL;
    }
    pmix_envar_t *e = (pmix_envar_t *)pmix_tma_malloc(tma, n * sizeof(pmix_envar_t));
    if (PMIX_LIKELY(NULL != e)) {
        for (size_t m = 0; m < n; m++) {
            pmix_bfrops_base_envar_construct_tma(&e[m], tma);
        }
    }
    return e;
}

static inline void
pmix_bfrops_base_envar_free_tma(
    pmix_envar_t *e,
    size_t n,
    pmix_tma_t *tma
) {
    if (NULL == e) {
        return;
    }
    for (size_t m = 0; m < n; m++) {
        pmix_bfrops_base_envar_destruct_tma(&e[m], tma);
    }
}

static inline void
pmix_bfrops_base_envar_load_tma(
    pmix_envar_t *e,
    char *var,
    char *value,
    char separator,
    pmix_tma_t *tma
) {
    if (NULL != var) {
        e->envar = pmix_tma_strdup(tma, var);
    }
    if (NULL != value) {
        e->value = pmix_tma_strdup(tma, value);
    }
    e->separator = separator;
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

static inline void
pmix_bfrops_base_proc_stats_destruct_tma(
    pmix_proc_stats_t *p,
    pmix_tma_t *tma
) {
    if (NULL != p->node) {
        pmix_tma_free(tma, p->node);
        p->node = NULL;
    }
    if (NULL != p->cmd) {
        pmix_tma_free(tma, p->cmd);
        p->cmd = NULL;
    }
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
pmix_bfrops_base_proc_stats_free_tma(
    pmix_proc_stats_t *p,
    size_t n,
    pmix_tma_t *tma
) {
    if (NULL != p) {
        for (size_t m = 0; m < n; m++) {
            pmix_bfrops_base_proc_stats_destruct_tma(&p[m], tma);
        }
    }
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

static inline void
pmix_bfrops_base_disk_stats_destruct_tma(
    pmix_disk_stats_t *p,
    pmix_tma_t *tma
) {
    if (NULL != p->disk) {
        pmix_tma_free(tma, p->disk);
        p->disk = NULL;
    }
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
pmix_bfrops_base_disk_stats_free_tma(
    pmix_disk_stats_t *p,
    size_t n,
    pmix_tma_t *tma
) {
    if (NULL != p) {
        for (size_t m = 0; m < n; m++) {
            pmix_bfrops_base_disk_stats_destruct_tma(&p[m], tma);
        }
    }
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

static inline void
pmix_bfrops_base_net_stats_destruct_tma(
    pmix_net_stats_t *p,
    pmix_tma_t *tma
) {
    if (NULL != p->net_interface) {
        pmix_tma_free(tma, p->net_interface);
        p->net_interface = NULL;
    }
}

static inline void
pmix_bfrops_base_net_stats_free_tma(
    pmix_net_stats_t *p,
    size_t n,
    pmix_tma_t *tma
) {
    if (NULL != p) {
        for (size_t m = 0; m < n; m++) {
            pmix_bfrops_base_net_stats_destruct_tma(&p[m], tma);
        }
    }
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

static inline void
pmix_bfrops_base_node_stats_destruct_tma(
    pmix_node_stats_t *p,
    pmix_tma_t *tma
) {
    if (NULL != p->node) {
        pmix_tma_free(tma, p->node);
        p->node = NULL;
    }
    if (NULL != p->diskstats) {
        pmix_bfrops_base_disk_stats_free_tma(p->diskstats, p->ndiskstats, tma);
    }
    if (NULL != p->netstats) {
        pmix_bfrops_base_net_stats_free_tma(p->netstats, p->nnetstats, tma);
    }
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
pmix_bfrops_base_node_stats_free_tma(
    pmix_node_stats_t *p,
    size_t n,
    pmix_tma_t *tma
) {
    if (NULL != p) {
        for (size_t m = 0; m < n; m++) {
            pmix_bfrops_base_node_stats_destruct_tma(&p[m], tma);
        }
    }
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

static inline pmix_value_cmp_t
pmix_bfrops_base_value_compare_tma(
    pmix_value_t *v1,
    pmix_value_t *v2,
    pmix_tma_t *tma
) {
    PMIX_HIDE_UNUSED_PARAMS(tma);
    // TODO(skg)
    return pmix_bfrops_base_value_cmp(v1, v2);
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
        pmix_bfrops_base_envar_construct_tma(&p->data.envar, tma);

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

static inline void
pmix_bfrops_base_data_array_construct_tma(
    pmix_data_array_t *p,
    size_t num,
    pmix_data_type_t type,
    pmix_tma_t *tma
) {
    p->type = type;
    p->size = num;
    if (0 < num) {
        if (PMIX_INFO == type) {
            p->array = pmix_bfrops_base_info_create_tma(num, tma);

        } else if (PMIX_PROC == type) {
            p->array = pmix_bfrops_base_proc_create_tma(num, tma);

        } else if (PMIX_PROC_INFO == type) {
            p->array = pmix_bfrops_base_proc_info_create_tma(num, tma);

        } else if (PMIX_ENVAR == type) {
            p->array = pmix_bfrops_base_envar_create_tma(num, tma);

        } else if (PMIX_VALUE == type) {
            p->array = pmix_bfrops_base_value_create_tma(num, tma);

        } else if (PMIX_PDATA == type) {
            p->array = pmix_bfrops_base_pdata_create_tma(num, tma);

        } else if (PMIX_QUERY == type) {
            p->array = pmix_bfrops_base_query_create_tma(num, tma);

        } else if (PMIX_APP == type) {
            p->array = pmix_bfrops_base_app_create_tma(num, tma);

        } else if (PMIX_BYTE_OBJECT == type ||
                   PMIX_COMPRESSED_STRING == type) {
            p->array = pmix_bfrops_base_byte_object_create_tma(num, tma);

        } else if (PMIX_ALLOC_DIRECTIVE == type ||
                   PMIX_PROC_STATE == type ||
                   PMIX_PERSIST == type ||
                   PMIX_SCOPE == type ||
                   PMIX_DATA_RANGE == type ||
                   PMIX_BYTE == type ||
                   PMIX_INT8 == type ||
                   PMIX_UINT8 == type ||
                   PMIX_POINTER == type) {
            p->array = pmix_tma_malloc(tma, num * sizeof(int8_t));
            memset(p->array, 0, num * sizeof(int8_t));

        } else if (PMIX_STRING == type) {
            p->array = pmix_tma_malloc(tma, num * sizeof(char*));
            memset(p->array, 0, num * sizeof(char*));

        } else if (PMIX_SIZE == type) {
            p->array = pmix_tma_malloc(tma, num * sizeof(size_t));
            memset(p->array, 0, num * sizeof(size_t));

        } else if (PMIX_PID == type) {
            p->array = pmix_tma_malloc(tma, num * sizeof(pid_t));
            memset(p->array, 0, num * sizeof(pid_t));

        } else if (PMIX_INT == type ||
                   PMIX_UINT == type ||
                   PMIX_STATUS == type) {
            p->array = pmix_tma_malloc(tma, num * sizeof(int));
            memset(p->array, 0, num * sizeof(int));

        } else if (PMIX_IOF_CHANNEL == type ||
                   PMIX_DATA_TYPE == type ||
                   PMIX_INT16 == type ||
                   PMIX_UINT16 == type) {
            p->array = pmix_tma_malloc(tma, num * sizeof(int16_t));
            memset(p->array, 0, num * sizeof(int16_t));

        } else if (PMIX_PROC_RANK == type ||
                   PMIX_INFO_DIRECTIVES == type ||
                   PMIX_INT32 == type ||
                   PMIX_UINT32 == type) {
            p->array = pmix_tma_malloc(tma, num * sizeof(int32_t));
            memset(p->array, 0, num * sizeof(int32_t));

        } else if (PMIX_INT64 == type ||
                   PMIX_UINT64 == type) {
            p->array = pmix_tma_malloc(tma, num * sizeof(int64_t));
            memset(p->array, 0, num * sizeof(int64_t));

        } else if (PMIX_FLOAT == type) {
            p->array = pmix_tma_malloc(tma, num * sizeof(float));
            memset(p->array, 0, num * sizeof(float));

        } else if (PMIX_DOUBLE == type) {
            p->array = pmix_tma_malloc(tma, num * sizeof(double));
            memset(p->array, 0, num * sizeof(double));

        } else if (PMIX_TIMEVAL == type) {
            p->array = pmix_tma_malloc(tma, num * sizeof(struct timeval));
            memset(p->array, 0, num * sizeof(struct timeval));

        } else if (PMIX_TIME == type) {
            p->array = pmix_tma_malloc(tma, num * sizeof(time_t));
            memset(p->array, 0, num * sizeof(time_t));

        } else if (PMIX_REGATTR == type) {
            p->array = pmix_bfrops_base_regattr_create_tma(num, tma);

        } else if (PMIX_BOOL == type) {
            p->array = pmix_tma_malloc(tma, num * sizeof(bool));
            memset(p->array, 0, num * sizeof(bool));

        } else if (PMIX_COORD == type) {
            /* cannot use PMIx_Coord_create as we do not
             * know the number of dimensions */
            p->array = pmix_tma_malloc(tma, num * sizeof(pmix_coord_t));
            memset(p->array, 0, num * sizeof(pmix_coord_t));

        } else if (PMIX_LINK_STATE == type) {
            p->array = pmix_tma_malloc(tma, num * sizeof(pmix_link_state_t));
            memset(p->array, 0, num * sizeof(pmix_link_state_t));

        } else if (PMIX_ENDPOINT == type) {
            p->array = pmix_bfrops_base_endpoint_create_tma(num, tma);

        } else if (PMIX_PROC_NSPACE == type) {
            p->array = pmix_tma_malloc(tma, num * sizeof(pmix_nspace_t));
            memset(p->array, 0, num * sizeof(pmix_nspace_t));

        } else if (PMIX_PROC_STATS == type) {
            p->array = pmix_bfrops_base_proc_stats_create_tma(num, tma);

        } else if (PMIX_DISK_STATS == type) {
            p->array = pmix_bfrops_base_disk_stats_create_tma(num, tma);

        } else if (PMIX_NET_STATS == type) {
            p->array = pmix_bfrops_base_net_stats_create_tma(num, tma);

        } else if (PMIX_NODE_STATS == type) {
            p->array = pmix_bfrops_base_node_stats_create_tma(num, tma);

        } else if (PMIX_DEVICE_DIST == type) {
            p->array = pmix_bfrops_base_device_distance_create_tma(num, tma);

        } else if (PMIX_GEOMETRY == type) {
            p->array = pmix_bfrops_base_geometry_create_tma(num, tma);

        } else if (PMIX_PROC_CPUSET == type) {
            p->array = pmix_bfrops_base_cpuset_create_tma(num, tma);

        } else {
            p->array = NULL;
            p->size = 0;
        }
    } else {
        p->array = NULL;
    }
}

#endif
