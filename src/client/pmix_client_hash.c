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
    /* List of pmix_kval_t structures containing all modex
     * data from this process, sorted by key */
    pmix_list_t modex;
} pmix_proc_data_t;
static void pdcon(pmix_proc_data_t *p)
{
    p->loaded = false;
    PMIX_CONSTRUCT(&p->data, pmix_list_t);
    PMIX_CONSTRUCT(&p->modex, pmix_list_t);
}
static void pddes(pmix_proc_data_t *p)
{
    PMIX_LIST_DESTRUCT(&p->data);
    PMIX_LIST_DESTRUCT(&p->modex);
}
static PMIX_CLASS_INSTANCE(pmix_proc_data_t,
                          pmix_list_item_t,
                          pdcon, pddes);
static pmix_kval_t* lookup_keyval(pmix_list_t *data,
                                  const char *key);
static pmix_proc_data_t* lookup_proc(pmix_hash_table_t *jtable,
                                     uint64_t id, bool create);

/* Initialize our hash table */
int pmix_client_hash_init(void)
{
    PMIX_CONSTRUCT(&hash_data, pmix_hash_table_t);
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
            PMIX_RELEASE(proc_data);
        }
        while (PMIX_SUCCESS == pmix_hash_table_get_next_key_uint64(&hash_data, &key,
                                                                   (void**)&proc_data,
                                                                   node, (void**)&node)) {
            if (NULL != proc_data) {
                PMIX_RELEASE(proc_data);
            }
        }
    }
    PMIX_DESTRUCT(&hash_data);
}


static void hash_store(pmix_list_t *data,
                       pmix_kval_t *kin)
{
     pmix_kval_t *kv;

     /* see if we already have this key in the data - means we are updating
     * a pre-existing value
     */
    kv = lookup_keyval(data, kin->key);
    if (NULL != kv) {
        pmix_list_remove_item(data, &kv->super);
        PMIX_RELEASE(kv);
    }
    /* store the new value */
    PMIX_RETAIN(kin);
    pmix_list_append(data, &kin->super);
}


int pmix_client_hash_store(const char *nspace, int rank,
                           pmix_kval_t *kin)
{
    pmix_proc_data_t *proc_data;
    uint32_t jobid;
    uint64_t id, rk64;

    pmix_output_verbose(10, pmix_globals.debug_output,
                        "HASH:STORE %s:%d key %s",
                        nspace, rank, kin->key);
    
    /* create a hash of the nspace */
    PMIX_HASH_STR(nspace, jobid);
    /* mix in the rank to get the id */
    if (PMIX_RANK_WILDCARD == rank) {
        id = ((uint64_t)jobid << 32) | 0x00000000ffffffff;
    } else {
        rk64 = (uint64_t)rank;
        id = ((uint64_t)jobid << 32) | rk64;
    }

    /* lookup the proc data object for this proc */
    if (NULL == (proc_data = lookup_proc(&hash_data, id, true))) {
        return PMIX_ERR_OUT_OF_RESOURCE;
    }

    hash_store(&proc_data->data, kin);

    return PMIX_SUCCESS;
}

int pmix_client_hash_store_modex(const char *nspace, int rank,
                                 pmix_kval_t *kin)
{
    pmix_proc_data_t *proc_data;
    uint32_t jobid;
    uint64_t id, rk64;

    pmix_output_verbose(10, pmix_globals.debug_output,
                        "HASH:STORE:MODEX %s:%d key %s",
                        nspace, rank, kin->key);
    
    /* create a hash of the nspace */
    PMIX_HASH_STR(nspace, jobid);
    /* mix in the rank to get the id */
    if (PMIX_RANK_WILDCARD == rank) {
        id = ((uint64_t)jobid << 32) | 0x00000000ffffffff;
    } else {
        rk64 = (uint64_t)rank;
        id = ((uint64_t)jobid << 32) | rk64;
    }

    /* lookup the proc data object for this proc */
    if (NULL == (proc_data = lookup_proc(&hash_data, id, true))) {
        return PMIX_ERR_OUT_OF_RESOURCE;
    }

    hash_store(&proc_data->modex, kin);

    return PMIX_SUCCESS;
}

int pmix_client_hash_fetch(const char *nspace, int rank,
                           const char *key, pmix_value_t **kvs)
{
    pmix_proc_data_t *proc_data;
    pmix_kval_t *hv;
    uint32_t jobid;
    uint64_t id, idwild, rk64;
    int rc;
    bool found_wild_data;

    pmix_output_verbose(10, pmix_globals.debug_output,
                        "HASH:FETCH %s:%d key %s",
                        nspace, rank, (NULL == key) ? "NULL" : key);
    
    /* NULL keys are not supported */
    if (NULL == key) {
        return PMIX_ERR_BAD_PARAM;
    }
    
    /* create a hash of the nspace */
    PMIX_HASH_STR(nspace, jobid);
    idwild = ((uint64_t)jobid << 32) | 0x00000000ffffffff;
    /* mix in the rank to get the id */
    if (PMIX_RANK_WILDCARD == rank) {
        id = idwild;
    } else {
        rk64 = (uint64_t)rank;
        id = ((uint64_t)jobid << 32) | rk64;
    }

    found_wild_data = false;
    /* lookup the proc data object for this proc */
    if (NULL == (proc_data = lookup_proc(&hash_data, id, false))) {
        pmix_output_verbose(10, pmix_globals.debug_output,
                            "HASH:FETCH proc data for %s:%d not found",
                            nspace, rank);
        /* if the requestor asked for wildcard data, then no point
         * in looking further - we already checked that option */
        if (id == idwild) {
            pmix_output_verbose(10, pmix_globals.debug_output,
                                "HASH:FETCH id = idwild - returning proc_entry_not_found");
            return PMIX_ERR_PROC_ENTRY_NOT_FOUND;
        }
        /* if the requestor asked for data about a specific
         * rank, but we didn't find that proc, then see if we have
         * the wildcard data object because some data resides
         * in there - i.e., the requestor may be asking for
         * data that relates to the entire job and not just
         * this rank */
        if (NULL == (proc_data = lookup_proc(&hash_data, idwild, false))) {
            pmix_output_verbose(10, pmix_globals.debug_output,
                                "HASH:FETCH WILD proc_data not found - returning proc_entry_not_found");
            return PMIX_ERR_PROC_ENTRY_NOT_FOUND;
        }
        found_wild_data = true;
        pmix_output_verbose(10, pmix_globals.debug_output,
                            "HASH:FETCH WILD proc_data entry found");
    }

    /* find the value from within this proc_data object - first
     * check the job-level data */
    if (NULL == (hv = lookup_keyval(&proc_data->data, key))) {
        pmix_output_verbose(10, pmix_globals.debug_output,
                            "HASH:FETCH data for key %s not found in job-level data", key);
        /* if it isn't present, then we have to treat three cases:
         *
         * (a) the requestor asked for data about a specific rank,
         *     and we found the blob for that rank. In this case,
         *     we have to also check the wildcard rank's object
         *     for the reasons cited above, as well as the modex
         *     data to see if the key is there
         *
         * (b) the requestor asked for data about a specific rank,
         *     but we didn't find a blob for that rank and are
         *     already looking at the wildcard rank's blob. In this
         *     case, we return PROC_ENTRY_NOT_FOUND so the get
         *     function can try to get it from the server
         *
         * (c) the requestor asked for data from the wildcard rank.
         *     In this case, there is nothing more we can do, so
         *     return NOT_FOUND */
        
        if (id == idwild) {  // case (c)
            /* there is no wildcard modex data */
            pmix_output_verbose(10, pmix_globals.debug_output,
                                "HASH:FETCH id = idwild - returning not_found");
            return PMIX_ERR_NOT_FOUND;
        }
        
        if (found_wild_data) {  // case (b)
            /* We don't treat this case as a failure because data blob for target process
             * wasn't found in hash storage, so continue by asking the server for data.
             * This is a typical case when direct modex is used. */
            pmix_output_verbose(10, pmix_globals.debug_output,
                                "HASH:FETCH looked in WILD proc data and key %s not found - returning proc_entry_not_found", key);
            return PMIX_ERR_PROC_ENTRY_NOT_FOUND;
        }
        
        /* it might be a modex key - do we have that data yet? */
        if (0 != pmix_list_get_size(&proc_data->modex)) {
            pmix_output_verbose(10, pmix_globals.debug_output,
                                "HASH:FETCH modex data available for %s:%d", nspace, rank);
            /* we have modex data, so check it */
            if (NULL != (hv = lookup_keyval(&proc_data->modex, key))) {
                pmix_output_verbose(10, pmix_globals.debug_output,
                                    "HASH:FETCH modex data found for key %s", key);
                goto returndata;
            }
        }

        /* at this point, we know we have a proc entry for the requested proc, but
         * either we don't have modex data OR the requested key wasn't in it. We
         * can now check to see if we have job-level data, and if the key
         * is in there */
        if (NULL == (proc_data = lookup_proc(&hash_data, idwild, false))) {  // case (a)
            /* we don't have data for the wildcard rank yet - give us
             * a chance to get it */
            pmix_output_verbose(10, pmix_globals.debug_output,
                                "HASH:FETCH wildcard data not available - returning proc_entry_not_found");
            return PMIX_ERR_PROC_ENTRY_NOT_FOUND;
        }
        if (NULL == (hv = lookup_keyval(&proc_data->data, key))) {
            /* we have the wildcard rank data, but this key isn't in it. So
             * we have both blobs and neither one contains the specified key */
            pmix_output_verbose(10, pmix_globals.debug_output,
                                "HASH:FETCH wildcard available, but key %s not found", key);
            /* if we have modex data, then we simply cannot find this key */
            if (0 != pmix_list_get_size(&proc_data->modex)) {
                return PMIX_ERR_NOT_FOUND;
            } else {
                /* give us a chance to get it via modex */
                return PMIX_ERR_PROC_ENTRY_NOT_FOUND;
            }
        }
    }

 returndata:
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
    if (NULL == (proc_data = lookup_proc(&hash_data, id, false))) {
        /* no data for this proc */
        return PMIX_SUCCESS;
    }

    /* if key is NULL, remove all data for this proc */
    if (NULL == key) {
        while (NULL != (kv = (pmix_kval_t*)pmix_list_remove_first(&proc_data->data))) {
            PMIX_RELEASE(kv);
        }
        /* remove the proc_data object itself from the jtable */
        pmix_hash_table_remove_value_uint64(&hash_data, id);
        /* cleanup */
        PMIX_RELEASE(proc_data);
        return PMIX_SUCCESS;
    }

    /* remove this item */
    PMIX_LIST_FOREACH(kv, &proc_data->data, pmix_kval_t) {
        if (0 == strcmp(key, kv->key)) {
            pmix_list_remove_item(&proc_data->data, &kv->super);
            PMIX_RELEASE(kv);
            break;
        }
    }

    return PMIX_SUCCESS;
}

/**
 * Find data for a given key in a given pmix_list_t.
 */
static pmix_kval_t* lookup_keyval(pmix_list_t *data,
                                  const char *key)
{
    pmix_kval_t *kv;

    PMIX_LIST_FOREACH(kv, data, pmix_kval_t) {
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
                                     uint64_t id, bool create)
{
    pmix_proc_data_t *proc_data = NULL;

    pmix_hash_table_get_value_uint64(jtable, id, (void**)&proc_data);
    if (NULL == proc_data && create) {
        /* The proc clearly exists, so create a data structure for it */
        proc_data = PMIX_NEW(pmix_proc_data_t);
        if (NULL == proc_data) {
            pmix_output(0, "pmix:client:hash:lookup_pmix_proc: unable to allocate proc_data_t\n");
            return NULL;
        }
        pmix_hash_table_set_value_uint64(jtable, id, proc_data);
    }
    
    return proc_data;
}
