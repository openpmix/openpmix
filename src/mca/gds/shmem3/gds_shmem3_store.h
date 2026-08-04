/*
 * Copyright (c) 2015-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2016-2018 IBM Corporation.  All rights reserved.
 * Copyright (c) 2018      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2018-2020 Mellanox Technologies, Inc.
 *                         All rights reserved.
 * Copyright (c) 2021-2024 Nanook Consulting  All rights reserved.
 * Copyright (c) 2022-2023 Triad National Security, LLC. All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#ifndef PMIX_GDS_SHMEM3_STORE_H
#define PMIX_GDS_SHMEM3_STORE_H

#include "gds_shmem3.h"

BEGIN_C_DECLS

/* Store a qualified value. kidx names the keyindex that translates the
 * key and its qualifiers; pass NULL for the process-global one, or the
 * segment's own keyindex when storing into a shared-memory table whose
 * indices other processes will read. */
PMIX_EXPORT pmix_status_t
pmix_gds_shmem3_store_qualified(
    pmix_hash_table_t *ht,
    pmix_rank_t rank,
    pmix_value_t *value,
    pmix_keyindex_t *kidx
);

PMIX_EXPORT pmix_status_t
pmix_gds_shmem3_store_local_job_data_in_shmem3(
    pmix_gds_shmem3_job_t *job,
    pmix_list_t *job_data
);

END_C_DECLS

#endif

/*
 * vim: ft=cpp ts=4 sts=4 sw=4 expandtab
 */
