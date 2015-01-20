/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2010      Cisco Systems, Inc.  All rights reserved. 
 * Copyright (c) 2004-2011 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2011-2014 Los Alamos National Security, LLC.  All rights
 *                         reserved.
 * Copyright (c) 2014      Intel, Inc. All rights reserved.
 * $COPYRIGHT$
 * 
 * Additional copyrights may follow
 * 
 * $HEADER$
 *
 */

#include "pmix_config.h"
#include "pmix_client_hash.h"

#include "src/include/pmix_stdint.h"
#include "src/include/hash_string.h"
#include "src/include/pmix_globals.h"

#include <string.h>

#include "src/class/pmix_hash_table.h"
#include "src/class/pmix_pointer_array.h"
#include "src/buffer_ops/buffer_ops.h"
#include "src/util/error.h"
#include "src/util/output.h"

static pmix_hash_table_t hash_data;

/**
 * Data for a particular pmix process
 * The name association is maintained in the
 * proc_data hash table.
 */
typedef struct {
    /** Structure can be put on lists (including in hash tables) */
    pmix_list_item_t super;
    bool loaded;
    /* List of pmix_kval_t structures containing all data
       received from this process, sorted by key. */
    pmix_list_t data;
} pmix_proc_data_t;
static void pdcon(pmix_proc_data_t *p)
{
    p->loaded = false;
    OBJ_CONSTRUCT(&p->data, pmix_list_t);
}
static void pddes(pmix_proc_data_t *p)
{
    PMIX_LIST_DESTRUCT(&p->data);
}
static OBJ_CLASS_INSTANCE(pmix_proc_data_t,
                          pmix_list_item_t,
                          pdcon, pddes);
static pmix_kval_t* lookup_keyval(pmix_proc_data_t *proc_data,
                                        const char *key);
static pmix_proc_data_t* lookup_proc(pmix_hash_table_t *jtable,
                                     uint64_t id);

/* Initialize our hash table */
int pmix_client_hash_init(void)
{
    OBJ_CONSTRUCT(&hash_data, pmix_hash_table_t);
    pmix_hash_table_init(&hash_data, 256);
    return PMIX_SUCCESS;
}

void pmix_client_hash_finalize(void)
{
    pmix_proc_data_t *proc_data;
    uint64_t key;
    char *node;

    /* to assist in getting a clean valgrind, cycle thru the hash table
     * and release all data stored in it
     */
    if (PMIX_SUCCESS == pmix_hash_table_get_first_key_uint64(&hash_data, &key,
                                                             (void**)&proc_data,
                                                             (void**)&node)) {
        if (NULL != proc_data) {
            OBJ_RELEASE(proc_data);
        }
        while (PMIX_SUCCESS == pmix_hash_table_get_next_key_uint64(&hash_data, &key,
                                                                   (void**)&proc_data,
                                                                   node, (void**)&node)) {
            if (NULL != proc_data) {
                OBJ_RELEASE(proc_data);
            }
        }
    }
    OBJ_DESTRUCT(&hash_data);
}



int pmix_client_hash_store(const char *nspace, int rank,
                           pmix_kval_t *kin)
{
    pmix_proc_data_t *proc_data;
    pmix_kval_t *kv;
    uint32_t jobid;
    uint64_t id;

    pmix_output_verbose(10, pmix_globals.debug_output,
                        "HASH:STORE %s:%d key %s",
                        nspace, rank, kin->key);
    
    /* create a hash of the nspace */
    PMIX_HASH_STR(nspace, jobid);
    /* mix in the rank to get the id */
    id = ((uint64_t)jobid << 32) | (int32_t)rank;
    
    /* lookup the proc data object for this proc */
    if (NULL == (proc_data = lookup_proc(&hash_data, id))) {
        return PMIX_ERR_OUT_OF_RESOURCE;
    }

    /* see if we already have this key in the data - means we are updating
     * a pre-existing value
     */
    kv = lookup_keyval(proc_data, kin->key);
    if (NULL != kv) {
        pmix_list_remove_item(&proc_data->data, &kv->super);
        OBJ_RELEASE(kv);
    }
    /* store the new value */
    OBJ_RETAIN(kin);
    pmix_list_append(&proc_data->data, &kin->super);

    return PMIX_SUCCESS;
}

int pmix_client_hash_fetch(const char *nspace, int rank,
                           const char *key, pmix_value_t **kvs)
{
    pmix_proc_data_t *proc_data;
    pmix_kval_t *hv;
    uint32_t jobid;
    uint64_t id;
    int rc;

    pmix_output_verbose(10, pmix_globals.debug_output,
                        "HASH:FETCH %s:%d key %s",
                        nspace, rank, (NULL == key) ? "NULL" : key);
    
    /* NULL keys are not supported */
    if (NULL == key) {
        return PMIX_ERR_BAD_PARAM;
    }
    
    /* create a hash of the nspace */
    PMIX_HASH_STR(nspace, jobid);
    /* mix in the rank to get the id */
    id = ((uint64_t)jobid << 32) | (int32_t)rank;

    /* lookup the proc data object for this proc */
    if (NULL == (proc_data = lookup_proc(&hash_data, id))) {
        return PMIX_ERR_NOT_FOUND;
    }

    /* find the value */
    if (NULL == (hv = lookup_keyval(proc_data, key))) {
        return PMIX_ERR_NOT_FOUND;
    }

    /* create the copy */
    if (PMIX_SUCCESS != (rc = pmix_bfrop.copy((void**)kvs, hv->value, PMIX_VALUE))) {
        return rc;
    }

    return PMIX_SUCCESS;
}

int pmix_client_hash_remove_data(const char *nspace,
                                 int rank, const char *key)
{
    pmix_proc_data_t *proc_data;
    pmix_kval_t *kv;
    uint32_t jobid;
    uint64_t id;

    /* create a hash of the nspace */
    PMIX_HASH_STR(nspace, jobid);
    /* mix in the rank to get the id */
    id = ((uint64_t)jobid << 32) | (int32_t)rank;

    /* lookup the specified proc */
    if (NULL == (proc_data = lookup_proc(&hash_data, id))) {
        /* no data for this proc */
        return PMIX_SUCCESS;
    }

    /* if key is NULL, remove all data for this proc */
    if (NULL == key) {
        while (NULL != (kv = (pmix_kval_t*)pmix_list_remove_first(&proc_data->data))) {
            OBJ_RELEASE(kv);
        }
        /* remove the proc_data object itself from the jtable */
        pmix_hash_table_remove_value_uint64(&hash_data, id);
        /* cleanup */
        OBJ_RELEASE(proc_data);
        return PMIX_SUCCESS;
    }

    /* remove this item */
    PMIX_LIST_FOREACH(kv, &proc_data->data, pmix_kval_t) {
        if (0 == strcmp(key, kv->key)) {
            pmix_list_remove_item(&proc_data->data, &kv->super);
            OBJ_RELEASE(kv);
            break;
        }
    }

    return PMIX_SUCCESS;
}

/**
 * Find data for a given key in a given proc_data_t
 * container.
 */
static pmix_kval_t* lookup_keyval(pmix_proc_data_t *proc_data,
                                        const char *key)
{
    pmix_kval_t *kv;

    PMIX_LIST_FOREACH(kv, &proc_data->data, pmix_kval_t) {
        if (0 == strcmp(key, kv->key)) {
            return kv;
        }
    }
    return NULL;
}


/**
 * Find proc_data_t container associated with given
 * pmix_identifier_t.
 */
static pmix_proc_data_t* lookup_proc(pmix_hash_table_t *jtable,
                                     uint64_t id)
{
    pmix_proc_data_t *proc_data = NULL;

    pmix_hash_table_get_value_uint64(jtable, id, (void**)&proc_data);
    if (NULL == proc_data) {
        /* The proc clearly exists, so create a data structure for it */
        proc_data = OBJ_NEW(pmix_proc_data_t);
        if (NULL == proc_data) {
            pmix_output(0, "pmix:client:hash:lookup_pmix_proc: unable to allocate proc_data_t\n");
            return NULL;
        }
        pmix_hash_table_set_value_uint64(jtable, id, proc_data);
    }
    
    return proc_data;
}
