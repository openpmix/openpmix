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
/**
 * Store a PMIX_SESSION_INFO_ARRAY-shaped value into the session segment
 * the given job is bound to. Answers success without writing for a
 * session that has already been described - see the note at the top of
 * the implementation.
 */
/** Write a session array into the session's build slot, with no
 *  already-described guard - see the implementation. */
PMIX_EXPORT pmix_status_t
pmix_gds_shmem3_store_session_info(
    pmix_gds_shmem3_job_t *job,
    pmix_gds_shmem3_session_t *sesh,
    pmix_value_t *val
);

PMIX_EXPORT pmix_status_t
pmix_gds_shmem3_store_session_array(
    pmix_gds_shmem3_job_t *job,
    pmix_value_t *val
);

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
