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

#include "src/class/pmix_hash_table.h"

BEGIN_C_DECLS

int pmix_client_hash_init(void);

void pmix_client_hash_finalize(void);

int pmix_client_hash_store(const pmix_identifier_t *uid,
                           pmix_value_t *val);

int pmix_client_hash_fetch(const pmix_identifier_t *uid,
                           const char *key, pmix_list_t *kvs);

int pmix_client_hash_remove_data(const pmix_identifier_t *uid, const char *key);

END_C_DECLS

#endif /* PMIX_HASH_H */
