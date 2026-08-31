/*
 * Copyright (c) 2022      Triad National Security, LLC. All rights reserved.
 * Copyright (c) 2024-2025 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#ifndef PMIX_GDS_SHMEM3_FETCH_H
#define PMIX_GDS_SHMEM3_FETCH_H

#include "gds_shmem3.h"

PMIX_EXPORT pmix_status_t
pmix_gds_shmem3_fetch(
    struct pmix_peer_t *peer,
    const pmix_proc_t *proc,
    pmix_scope_t scope,
    bool copy,
    const char *key,
    pmix_info_t qualifiers[],
    size_t nqual,
    pmix_realm_t realm,
    pmix_list_t *kvs
);

#endif
