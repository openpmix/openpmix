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

#ifndef PMIX_GDS_SHMEM_STORE_H
#define PMIX_GDS_SHMEM_STORE_H

#include "gds_shmem.h"

BEGIN_C_DECLS

/**
 * Process a node array: an array of node-level info for a single node. Either
 * the node ID, hostname, or both must be included in the array to identify the
 * node.
 */
PMIX_EXPORT pmix_status_t
pmix_gds_shmem_store_node_array(
    pmix_gds_shmem_job_t *job,
    pmix_value_t *val,
    pmix_list_t *target
);

/**
 * Process an app array: an array of app-level info for a single app.  If the
 * appnum is not included in the array, then it is assumed that only app is in
 * the job.  This assumption is checked and generates an error if violated
 */
PMIX_EXPORT pmix_status_t
pmix_gds_shmem_store_app_array(
    pmix_gds_shmem_job_t *job,
    pmix_value_t *val
);

/**
 *
 */
PMIX_EXPORT pmix_status_t
pmix_gds_shmem_store_proc_data(
    pmix_gds_shmem_job_t *job,
    const pmix_kval_t *kval
);

PMIX_EXPORT pmix_status_t
pmix_gds_shmem_store_job_array(
    pmix_info_t *info,
    pmix_gds_shmem_job_t *job,
    uint32_t *flags,
    char ***procs,
    char ***nodes
);

END_C_DECLS

#endif

/*
 * vim: ft=cpp ts=4 sts=4 sw=4 expandtab
 */
