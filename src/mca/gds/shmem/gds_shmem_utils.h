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
                        ":%s at line %d", __func__, __LINE__);                 \
} while (0)

// TODO(skg) Support different verbosity settings. Like VV or something.
#define PMIX_GDS_SHMEM_VOUT(...)                                               \
do {                                                                           \
    pmix_output_verbose(2, pmix_gds_base_framework.framework_output,           \
                        "gds:" PMIX_GDS_SHMEM_NAME ":" __VA_ARGS__);           \
} while (0)

BEGIN_C_DECLS

// TODO(skg) Consider moving to fetch.
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

END_C_DECLS

#endif

/*
 * vim: ft=cpp ts=4 sts=4 sw=4 expandtab
 */
