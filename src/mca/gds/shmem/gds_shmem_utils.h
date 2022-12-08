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

#if PMIX_ENABLE_DEBUG
#define PMIX_GDS_SHMEM_VOUT_HERE()                                             \
do {                                                                           \
    pmix_output_verbose(12, pmix_gds_base_framework.framework_output,          \
                        "gds:" PMIX_GDS_SHMEM_NAME                             \
                        ":%s called at line %d", __func__, __LINE__);          \
} while (0)
#else
#define PMIX_GDS_SHMEM_VOUT_HERE()                                             \
do { } while (0)
#endif

#define PMIX_GDS_SHMEM_VOUT(...)                                               \
do {                                                                           \
    pmix_output_verbose(2, pmix_gds_base_framework.framework_output,           \
                        "gds:" PMIX_GDS_SHMEM_NAME ":" __VA_ARGS__);           \
} while (0)

#if PMIX_ENABLE_DEBUG
#define PMIX_GDS_SHMEM_VVOUT(...)                                              \
do {                                                                           \
    pmix_output_verbose(12, pmix_gds_base_framework.framework_output,          \
                        "gds:" PMIX_GDS_SHMEM_NAME ":" __VA_ARGS__);           \
} while (0)
#else
#define PMIX_GDS_SHMEM_VVOUT(...)                                              \
do { } while (0)
#endif

BEGIN_C_DECLS

PMIX_EXPORT pmix_status_t
pmix_gds_shmem_get_job_tracker(
    const pmix_nspace_t nspace,
    bool create,
    pmix_gds_shmem_job_t **job
);

PMIX_EXPORT pmix_gds_shmem_session_t *
pmix_gds_shmem_check_session(
    pmix_gds_shmem_job_t *job,
    uint32_t sid,
    bool create
);

PMIX_EXPORT bool
pmix_gds_shmem_check_hostname(
    const char *h1,
    const char *h2
);

/**
 * Sets shmem to the appropriate pmix_shmem_t *.
 */
PMIX_EXPORT pmix_status_t
pmix_gds_shmem_get_job_shmem_from_id(
    pmix_gds_shmem_job_t *job,
    pmix_gds_shmem_job_shmem_id_t shmem_id,
    pmix_shmem_t **shmem
);

PMIX_EXPORT size_t
pmix_gds_shmem_pad_amount_to_page(
    size_t size
);

PMIX_EXPORT pmix_status_t
pmix_gds_shmem_segment_create_and_attach(
    pmix_gds_shmem_job_t *job,
    pmix_shmem_t *shmem,
    const char *segment_id,
    size_t segment_size
);

PMIX_EXPORT pmix_status_t
pmix_gds_shmem_segment_attach_and_init(
    pmix_gds_shmem_job_t *job,
    pmix_gds_shmem_job_shmem_id_t shmem_id
);

static inline pmix_tma_t *
pmix_gds_shmem_get_job_tma(
    pmix_gds_shmem_job_t *job
) {
    return &job->smdata->tma;
}

static inline void
pmix_gds_shmem_vout_smdata(
    pmix_gds_shmem_job_t *job
) {
    PMIX_GDS_SHMEM_VOUT(
        "shmem@%p, "
        "smdata tma@%p, "
        "smdata tma data_ptr=%p, "
        "jobinfo@%p, "
        "appinfo@%p, "
        "nodeinfo@%p, "
        "local_hashtab@%p",
        (void *)job->shmem->base_address,
        (void *)&job->smdata->tma,
        (void *)job->smdata->tma.data_ptr,
        (void *)job->smdata->jobinfo,
        (void *)job->smdata->appinfo,
        (void *)job->smdata->nodeinfo,
        (void *)job->smdata->local_hashtab
    );
}

END_C_DECLS

#endif

/*
 * vim: ft=cpp ts=4 sts=4 sw=4 expandtab
 */
