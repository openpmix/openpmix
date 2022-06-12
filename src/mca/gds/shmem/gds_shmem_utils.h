/*
 * Copyright (c) 2015-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2016-2018 IBM Corporation.  All rights reserved.
 * Copyright (c) 2018      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2018-2020 Mellanox Technologies, Inc.
 *                         All rights reserved.
 * Copyright (c) 2021-2022 Nanook Consulting.  All rights reserved.
 * Copyright (c) 2022      Triad National Security, LLC. All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#ifndef PMIX_GDS_SHMEM_UTILS_H
#define PMIX_GDS_SHMEM_UTILS_H

#include "gds_shmem.h"

#define PMIX_GDS_SHMEM_VOUT_HERE()                                             \
do {                                                                           \
    pmix_output_verbose(2, pmix_gds_base_framework.framework_output,           \
                        "gds:" PMIX_GDS_SHMEM_NAME                             \
                        ":%s called at line %d", __func__, __LINE__);          \
} while (0)

// TODO(skg) Support different verbosity settings. Like VV or something.
#define PMIX_GDS_SHMEM_VOUT(...)                                               \
do {                                                                           \
    pmix_output_verbose(2, pmix_gds_base_framework.framework_output,           \
                        "gds:" PMIX_GDS_SHMEM_NAME ":" __VA_ARGS__);           \
} while (0)

// TODO(skg) This shouldn't live here. Figure out a way to get this in an
// appropriate place. This modifies PMIX_VALUE_XFER() for our needs.
#define PMIX_GDS_SHMEM_VALUE_XFER(r, v, s, tma)                                \
do {                                                                           \
    if (NULL == (v)) {                                                         \
        (v) = (pmix_value_t *)pmix_tma_malloc((tma), sizeof(pmix_value_t));    \
        if (NULL == (v)) {                                                     \
            (r) = PMIX_ERR_NOMEM;                                              \
        } else {                                                               \
            (r) = pmix_gds_shmem_value_xfer((v), (s), (tma));                  \
        }                                                                      \
    } else {                                                                   \
        (r) = pmix_gds_shmem_value_xfer((v), (s), (tma));                      \
    }                                                                          \
} while(0)

// TODO(skg) This shouldn't live here. Figure out a way to get this in an
// appropriate place. This modifies PMIX_BFROPS_COPY() for our needs.
#define PMIX_GDS_SHMEM_BFROPS_COPY_TMA(r, d, s, t, tma)                        \
(r) = pmix_gds_shmem_bfrops_base_copy_value(d, s, t, tma)

BEGIN_C_DECLS

PMIX_EXPORT pmix_status_t
pmix_gds_shmem_get_job_tracker(
    const pmix_nspace_t nspace,
    bool create,
    pmix_gds_shmem_job_t **job
);

PMIX_EXPORT pmix_gds_shmem_nodeinfo_t *
pmix_gds_shmem_get_nodeinfo_by_nodename(
    pmix_list_t *nodes,
    const char *hostname
);

PMIX_EXPORT bool
pmix_gds_shmem_check_hostname(
    const char *h1,
    const char *h2
);

PMIX_EXPORT pmix_status_t
pmix_gds_shmem_get_value_size(
    const pmix_value_t *value,
    size_t *result
);

PMIX_EXPORT size_t
pmix_gds_shmem_pad_amount_to_page(
    size_t size
);

PMIX_EXPORT pmix_status_t
pmix_gds_shmem_segment_create_and_attach(
    pmix_gds_shmem_job_t *job,
    const char *segment_id,
    size_t segment_size
);

PMIX_EXPORT pmix_status_t
pmix_gds_shmem_value_xfer(
    pmix_value_t *p,
    const pmix_value_t *src,
    pmix_tma_t *tma
);

PMIX_EXPORT pmix_status_t
pmix_gds_shmem_bfrops_base_copy_value(
    pmix_value_t **dest,
    pmix_value_t *src,
    pmix_data_type_t type,
    pmix_tma_t *tma
);

PMIX_EXPORT pmix_status_t
pmix_gds_shmem_copy_darray(
    pmix_data_array_t **dest,
    pmix_data_array_t *src,
    pmix_data_type_t type,
    pmix_tma_t *tma
);

static inline pmix_tma_t *
pmix_gds_shmem_get_job_tma(
    pmix_gds_shmem_job_t *job
) {
    return &job->smdata->tma;
}

static inline bool
pmix_gds_shmem_keys_eq(
    const char *k1,
    const char *k2
) {
    return 0 == strcmp(k1, k2);
}

static inline void
pmix_gds_shmem_vout_smdata(
    pmix_gds_shmem_job_t *job
) {
    PMIX_GDS_SHMEM_VOUT(
        "shmem@%p, "
        "jobinfo@%p, "
        "apps@%p, "
        "nodeinfo@%p, "
        "local_hashtab@%p, "
        "tma@%p, "
        "tma.data_ptr=%p",
        (void *)job->shmem->base_address,
        (void *)job->smdata->jobinfo,
        (void *)job->smdata->apps,
        (void *)job->smdata->nodeinfo,
        (void *)job->smdata->local_hashtab,
        (void *)&job->smdata->tma,
        (void *)job->smdata->tma.data_ptr
    );
}

END_C_DECLS

#endif

/*
 * vim: ft=cpp ts=4 sts=4 sw=4 expandtab
 */
