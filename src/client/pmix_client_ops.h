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
#include "src/usock/usock.h"

BEGIN_C_DECLS

typedef struct {
    int init_cntr;
    pmix_nspace_t myserver_nspace;
    pmix_peer_t myserver;
    pmix_buffer_t *cache_local;
    pmix_buffer_t *cache_remote;
    pmix_buffer_t *cache_global;
    pmix_list_t nspaces;
} pmix_client_globals_t;

typedef struct {
    pmix_list_item_t super;
    char nspace[PMIX_MAX_NSLEN];
    pmix_list_t nodes;
} pmix_nsrec_t;
PMIX_CLASS_DECLARATION(pmix_nsrec_t);

typedef struct {
    pmix_list_item_t super;
    char *name;
    char *procs;
} pmix_nrec_t;
PMIX_CLASS_DECLARATION(pmix_nrec_t);

extern pmix_client_globals_t pmix_client_globals;

void pmix_client_process_nspace_blob(const char *nspace, pmix_buffer_t *bptr);

END_C_DECLS

#endif /* PMIX_CLIENT_OPS_H */
