/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2010      Cisco Systems, Inc.  All rights reserved. 
 * Copyright (c) 2004-2011 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2011-2014 Los Alamos National Security, LLC.  All rights
 *                         reserved. 
 * $COPYRIGHT$
 * 
 * Additional copyrights may follow
 * 
 * $HEADER$
 *
 */

#include "pmix_config.h"
#include "constants.h"
#include "pmix_stdint.h"

#include <string.h>

#include "src/class/pmix_hash_table.h"
#include "src/class/pmix_pointer_array.h"
#include "src/buffer_ops/buffer_ops.h"
#include "src/util/error.h"
#include "src/util/output.h"

#include "pmix_client.h"
#include "pmix_client_hash.h"

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
    /* List of pmix_value_t structures containing all data
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
static pmix_value_t* lookup_keyval(pmix_proc_data_t *proc_data,
                                   const char *key);
static pmix_proc_data_t* lookup_proc(pmix_hash_table_t *jtable,
                                     pmix_identifier_t id);

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



int pmix_client_hash_store(const pmix_identifier_t *uid,
                           pmix_value_t *val)
{
    pmix_proc_data_t *proc_data;
    pmix_value_t *kv;
    pmix_identifier_t id;
    int rc;

    /* to protect alignment, copy the identifier across */
    memcpy(&id, uid, sizeof(pmix_identifier_t));

    pmix_output_verbose(1, pmix_client_debug_output,
                        "%"PRIu64" pmix:client:hash:store storing data for proc %"PRIu64"",
                        pmix_client_myid, id);

    /* lookup the proc data object for this proc */
    if (NULL == (proc_data = lookup_proc(&hash_data, id))) {
        /* unrecoverable error */
        PMIX_OUTPUT_VERBOSE((5, pmix_client_debug_output,
                             "%"PRIu64" pmix:client:hash:store: storing data for proc %"PRIu64" unrecoverably failed",
                             pmix_client_myid, id));
        return PMIX_ERR_OUT_OF_RESOURCE;
    }

    /* see if we already have this key in the data - means we are updating
     * a pre-existing value
     */
    kv = lookup_keyval(proc_data, val->key);
#if PMIX_ENABLE_DEBUG
    char *_data_type = pmix_bfrop.lookup_data_type(val->type);
    PMIX_OUTPUT_VERBOSE((5, pmix_client_debug_output,
                         "%"PRIu64" pmix:client:hash:store: %s key %s[%s] for proc %"PRIu64"",
                         pmix_client_myid, (NULL == kv ? "storing" : "updating"),
                         val->key, _data_type, id));
    free (_data_type);
#endif

    if (NULL != kv) {
        pmix_list_remove_item(&proc_data->data, &kv->super);
        OBJ_RELEASE(kv);
    }
    /* create the copy */
    if (PMIX_SUCCESS != (rc = pmix_bfrop.copy((void**)&kv, val, PMIX_VALUE))) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    pmix_list_append(&proc_data->data, &kv->super);

    return PMIX_SUCCESS;
}

int pmix_client_hash_fetch(const pmix_identifier_t *uid,
                           const char *key, pmix_list_t *kvs)
{
    pmix_proc_data_t *proc_data;
    pmix_value_t *kv, *knew;
    pmix_identifier_t id;
    int rc;

    /* to protect alignment, copy the identifier across */
    memcpy(&id, uid, sizeof(pmix_identifier_t));

    PMIX_OUTPUT_VERBOSE((5, pmix_client_debug_output,
                         "%"PRIu64" pmix:client:hash:fetch: searching for key %s on proc %"PRIu64"",
                         pmix_client_myid, (NULL == key) ? "NULL" : key, id));

    /* lookup the proc data object for this proc */
    if (NULL == (proc_data = lookup_proc(&hash_data, id))) {
        PMIX_OUTPUT_VERBOSE((5, pmix_client_debug_output,
                             "%"PRIu64" pmix:client:hash:fetch data for proc %"PRIu64" not found",
                             pmix_client_myid, id));
        return PMIX_ERR_NOT_FOUND;
    }

    /* if the key is NULL, that we want everything */
    if (NULL == key) {
        PMIX_LIST_FOREACH(kv, &proc_data->data, pmix_value_t) {
            /* copy the value */
            if (PMIX_SUCCESS != (rc = pmix_bfrop.copy((void**)&knew, kv, PMIX_VALUE))) {
                PMIX_ERROR_LOG(rc);
                return rc;
            }
            PMIX_OUTPUT_VERBOSE((5, pmix_client_debug_output,
                                 "%"PRIu64" pmix:client:hash:fetch: adding data for key %s on proc %"PRIu64"",
                                 pmix_client_myid, (NULL == kv->key) ? "NULL" : kv->key, id));

            /* add it to the output list */
            pmix_list_append(kvs, &knew->super);
        }
        return PMIX_SUCCESS;
    }

    /* find the value */
    if (NULL == (kv = lookup_keyval(proc_data, key))) {
        PMIX_OUTPUT_VERBOSE((5, pmix_client_debug_output,
                             "%"PRIu64" pmix:client:hash:fetch key %s for proc %"PRIu64" not found",
                             pmix_client_myid, (NULL == key) ? "NULL" : key, id));
        return PMIX_ERR_NOT_FOUND;
    }

    /* create the copy */
    if (PMIX_SUCCESS != (rc = pmix_bfrop.copy((void**)&knew, kv, PMIX_VALUE))) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    /* add it to the output list */
    pmix_list_append(kvs, &knew->super);

    return PMIX_SUCCESS;
}

int pmix_client_hash_remove_data(const pmix_identifier_t *uid, const char *key)
{
    pmix_proc_data_t *proc_data;
    pmix_value_t *kv;
    pmix_identifier_t id;

    /* to protect alignment, copy the identifier across */
    memcpy(&id, uid, sizeof(pmix_identifier_t));

    /* lookup the specified proc */
    if (NULL == (proc_data = lookup_proc(&hash_data, id))) {
        /* no data for this proc */
        return PMIX_SUCCESS;
    }

    /* if key is NULL, remove all data for this proc */
    if (NULL == key) {
        while (NULL != (kv = (pmix_value_t *) pmix_list_remove_first(&proc_data->data))) {
            OBJ_RELEASE(kv);
        }
        /* remove the proc_data object itself from the jtable */
        pmix_hash_table_remove_value_uint64(&hash_data, id);
        /* cleanup */
        OBJ_RELEASE(proc_data);
        return PMIX_SUCCESS;
    }

    /* remove this item */
    PMIX_LIST_FOREACH(kv, &proc_data->data, pmix_value_t) {
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
static pmix_value_t* lookup_keyval(pmix_proc_data_t *proc_data,
                                   const char *key)
{
    pmix_value_t *kv;

    PMIX_LIST_FOREACH(kv, &proc_data->data, pmix_value_t) {
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
                                     pmix_identifier_t id)
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

