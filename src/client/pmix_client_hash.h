/*
 * Copyright (c) 2010      Cisco Systems, Inc.  All rights reserved. 
 * Copyright (c) 2012      Los Alamos National Security, Inc. All rights reserved.
 * Copyright (c) 2014      Intel, Inc. All rights reserved.
 * $COPYRIGHT$
 * 
 * Additional copyrights may follow
 * 
 * $HEADER$
 */

#ifndef PMIX_HASH_H
#define PMIX_HASH_H

#include "src/buffer_ops/buffer_ops.h"
#include "src/class/pmix_hash_table.h"

BEGIN_C_DECLS

int pmix_client_hash_init(void);

void pmix_client_hash_finalize(void);

int pmix_client_hash_store(const char *nspace, int rank,
                           pmix_kval_t *kv);
int pmix_client_hash_store_modex(const char *nspace, int rank,
                                 pmix_kval_t *kin);

int pmix_client_hash_fetch(const char *nspace, int rank,
                           const char *key, pmix_value_t **kvs);

int pmix_client_hash_remove_data(const char *nspace,
                                 int rank, const char *key);

END_C_DECLS

#endif /* PMIX_HASH_H */
