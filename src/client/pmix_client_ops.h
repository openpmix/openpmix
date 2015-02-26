/*
 * Copyright (c) 2015      Intel, Inc. All rights reserved.
 * $COPYRIGHT$
 * 
 * Additional copyrights may follow
 * 
 * $HEADER$
 */

#ifndef PMIX_CLIENT_OPS_H
#define PMIX_CLIENT_OPS_H

#include "src/buffer_ops/buffer_ops.h"
#include "src/class/pmix_hash_table.h"

BEGIN_C_DECLS

typedef struct {
    int init_cntr;
    pmix_nspace_t myserver_nspace;
    pmix_peer_t myserver;
    pmix_buffer_t *cache_local;
    pmix_buffer_t *cache_remote;
    pmix_buffer_t *cache_global;
} pmix_client_globals_t;

extern pmix_client_globals_t pmix_client_globals;

END_C_DECLS

#endif /* PMIX_CLIENT_OPS_H */
