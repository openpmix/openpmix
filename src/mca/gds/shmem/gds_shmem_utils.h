/*
 * Copyright (c) 2015-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2016-2018 IBM Corporation.  All rights reserved.
 * Copyright (c) 2018      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2018-2020 Mellanox Technologies, Inc.
 *                         All rights reserved.
 * Copyright (c) 2021-2022 Nanook Consulting.  All rights reserved.
 * Copyright (c) 2022-2023 Triad National Security, LLC. All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#ifndef PMIX_GDS_SHMEM_UTILS_H
#define PMIX_GDS_SHMEM_UTILS_H

#include "gds_shmem.h"

#define PMIX_GDS_SHMEM_OUT(...)                                                \
do {                                                                           \
    pmix_output(0, "gds:" PMIX_GDS_SHMEM_NAME ":" __VA_ARGS__);                \
} while (0)

#define PMIX_GDS_SHMEM_VOUT(...)                                               \
do {                                                                           \
    pmix_output_verbose(2, pmix_gds_base_framework.framework_output,           \
                        "gds:" PMIX_GDS_SHMEM_NAME ":" __VA_ARGS__);           \
} while (0)

#if PMIX_ENABLE_DEBUG
#define PMIX_GDS_SHMEM_VVOUT(...)                                              \
do {                                                                           \
    pmix_output_verbose(9, pmix_gds_base_framework.framework_output,           \
                        "gds:" PMIX_GDS_SHMEM_NAME ":" __VA_ARGS__);           \
} while (0)
#else
#define PMIX_GDS_SHMEM_VVOUT(...)                                              \
do { } while (0)
#endif

#define PMIX_GDS_SHMEM_VVOUT_HERE()                                            \
PMIX_GDS_SHMEM_VVOUT("HERE AT %s,%d", __func__, __LINE__)

BEGIN_C_DECLS

PMIX_EXPORT pmix_status_t
pmix_gds_shmem_get_job_tracker(
    const pmix_nspace_t nspace,
    bool create,
    pmix_gds_shmem_job_t **job
);

PMIX_EXPORT pmix_gds_shmem_session_t *
pmix_gds_shmem_get_session_tracker(
    pmix_gds_shmem_job_t *job,
    uint32_t sid,
    bool create
);

PMIX_EXPORT bool
pmix_gds_shmem_hostnames_eq(
    const char *h1,
    const char *h2
);

/**
 * Sets shmem to the appropriate pmix_shmem_t *.
 */
PMIX_EXPORT pmix_status_t
pmix_gds_shmem_get_job_shmem_by_id(
    pmix_gds_shmem_job_t *job,
    pmix_gds_shmem_job_shmem_id_t shmem_id,
    pmix_shmem_t **shmem
);

PMIX_EXPORT void
pmix_gds_shmem_set_status(
    pmix_gds_shmem_job_t *job,
    pmix_gds_shmem_job_shmem_id_t shmem_id,
    pmix_gds_shmem_status_flag_t flag
);

PMIX_EXPORT void
pmix_gds_shmem_clear_status(
    pmix_gds_shmem_job_t *job,
    pmix_gds_shmem_job_shmem_id_t shmem_id,
    pmix_gds_shmem_status_flag_t flag
);

PMIX_EXPORT void
pmix_gds_shmem_clearall_status(
    pmix_gds_shmem_job_t *job,
    pmix_gds_shmem_job_shmem_id_t shmem_id
);

PMIX_EXPORT bool
pmix_gds_shmem_has_status(
    pmix_gds_shmem_job_t *job,
    pmix_gds_shmem_job_shmem_id_t shmem_id,
    pmix_gds_shmem_status_flag_t flag
);

static inline pmix_tma_t *
pmix_gds_shmem_get_job_tma(
    pmix_gds_shmem_job_t *job
) {
    return &job->smdata->tma;
}

static inline pmix_tma_t *
pmix_gds_shmem_get_session_tma(
    pmix_gds_shmem_job_t *job
) {
    assert(job->session);
    if (!job->session) {
        return NULL;
    }
    return &job->session->smdata->tma;
}

static inline void
pmix_gds_shmem_vout_smdata(
    pmix_gds_shmem_job_t *job
) {
    PMIX_GDS_SHMEM_VOUT(
        "shmem_hdr@%p, "
        "shmem_data@%p, "
        "smdata tma@%p, "
        "smdata tma data_ptr=%p, "
        "jobinfo@%p, "
        "appinfo@%p, "
        "nodeinfo@%p, "
        "local_hashtab@%p",
        (void *)job->shmem->hdr_address,
        (void *)job->shmem->data_address,
        (void *)&job->smdata->tma,
        (void *)job->smdata->tma.data_ptr,
        (void *)job->smdata->jobinfo,
        (void *)job->smdata->appinfo,
        (void *)job->smdata->nodeinfo,
        (void *)job->smdata->local_hashtab
    );
}

static inline void
pmix_gds_shmem_vout_smmodex(
    pmix_gds_shmem_job_t *job
) {
    PMIX_GDS_SHMEM_VOUT(
        "modex_shmem@%p, "
        "smmodex tma@%p, "
        "smmodex tma data_ptr=%p, "
        "hashtab@%p",
        (void *)job->modex_shmem->data_address,
        (void *)&job->smmodex->tma,
        (void *)job->smmodex->tma.data_ptr,
        (void *)job->smmodex->hashtab
    );
}

static inline void
pmix_gds_shmem_vout_smsession(
    pmix_gds_shmem_session_t *sesh
) {
    PMIX_GDS_SHMEM_VOUT(
        "shmem_hdr@%p, "
        "shmem_data@%p, "
        "smdata tma@%p, "
        "smdata tma data_ptr=%p, "
        "sessioninfo@%p, "
        "nodeinfo@%p",
        (void *)sesh->shmem->hdr_address,
        (void *)sesh->shmem->data_address,
        (void *)&sesh->smdata->tma,
        (void *)sesh->smdata->tma.data_ptr,
        (void *)sesh->smdata->sessioninfo,
        (void *)sesh->smdata->nodeinfo
    );
}

END_C_DECLS

#endif

/*
 * vim: ft=cpp ts=4 sts=4 sw=4 expandtab
 */
