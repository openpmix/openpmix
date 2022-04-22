/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2010      Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2004-2011 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2011-2014 Los Alamos National Security, LLC.  All rights
 *                         reserved.
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2015-2018 Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2016      Mellanox Technologies, Inc.
 *                         All rights reserved.
 * Copyright (c) 2016      IBM Corporation.  All rights reserved.
 * Copyright (c) 2021-2022 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 */

#include "src/include/pmix_config.h"

#include "src/include/pmix_hash_string.h"
#include "src/include/pmix_stdint.h"

#include <string.h>
#include "pmix.h"

#include "src/class/pmix_hash_table.h"
#include "src/class/pmix_pointer_array.h"
#include "src/include/dictionary.h"
#include "src/include/pmix_globals.h"
#include "src/mca/bfrops/base/base.h"
#include "src/util/pmix_error.h"
#include "src/util/pmix_name_fns.h"
#include "src/util/pmix_output.h"

#include "src/util/hash.h"
extern pmix_regattr_input_t *pmix_internal_attrs;

/**
 * Data for a particular pmix process
 * The name association is maintained in the
 * proc_data hash table.
 */
typedef struct {
    pmix_object_t super;
    /* Array of pmix_dstor_t structures containing all data
       received from this process */
    pmix_pointer_array_t data;
} pmix_proc_data_t;
static void pdcon(pmix_proc_data_t *p)
{
    PMIX_CONSTRUCT(&p->data, pmix_pointer_array_t);
    pmix_pointer_array_init(&p->data, 1, INT_MAX, 1);
}
static void pddes(pmix_proc_data_t *p)
{
    int n;
    pmix_dstor_t *d;

    for (n=0; n < p->data.size; n++) {
        d = (pmix_dstor_t*)pmix_pointer_array_get_item(&p->data, n);
        if (NULL != d) {
            if (NULL != d->value) {
                PMIX_VALUE_RELEASE(d->value);
            }
            free(d);
            pmix_pointer_array_set_item(&p->data, n, NULL);
        }
    }
    PMIX_DESTRUCT(&p->data);
}
static PMIX_CLASS_INSTANCE(pmix_proc_data_t, pmix_object_t, pdcon, pddes);

static pmix_dstor_t *lookup_keyval(pmix_proc_data_t *proc, uint32_t kid);
static pmix_proc_data_t *lookup_proc(pmix_hash_table_t *jtable, uint32_t id, bool create);

pmix_status_t pmix_hash_store(pmix_hash_table_t *table,
                              pmix_rank_t rank, pmix_kval_t *kin)
{
    pmix_proc_data_t *proc_data;
    uint32_t kid;
    pmix_dstor_t *hv;
    pmix_regattr_input_t *p;
    pmix_status_t rc;

    pmix_output_verbose(10, pmix_globals.debug_output,
                        "HASH:STORE rank %s key %s",
                        PMIX_RANK_PRINT(rank),
                        (NULL == kin) ? "NULL KVAL" : kin->key);

    if (NULL == kin) {
        return PMIX_ERR_BAD_PARAM;
    }

    /* lookup the key's corresponding index - this should be
     * moved to the periphery of the PMIx library so we can
     * refer to the key numerically throughout the internals
     */
    p = pmix_hash_lookup_key(UINT32_MAX, kin->key);
    if (NULL == p) {
        /* we don't know this key */
        pmix_output_verbose(10, pmix_globals.debug_output,
                            "UNKNOWN KEY: %s", kin->key);
        return PMIX_ERR_BAD_PARAM;
    }
    kid = p->index;

    /* lookup the proc data object for this proc - create
     * it if we don't already have it */
    if (NULL == (proc_data = lookup_proc(table, rank, true))) {
        return PMIX_ERR_NOMEM;
    }

    /* see if we already have this key-value */
    hv = lookup_keyval(proc_data, kid);
    if (NULL != hv) {
        if (9 < pmix_output_get_verbosity(pmix_globals.debug_output)) {
            char *tmp;
            tmp = PMIx_Value_string(hv->value);
            pmix_output(0, "PREEXISTING ENTRY FOR PROC %s KEY %s: %s",
                        PMIX_RANK_PRINT(rank), kin->key, tmp);
            free(tmp);
        }
        /* yes we do - so just replace the current value if it changed */
        if (NULL != hv->value) {
            if (PMIX_EQUAL == pmix_bfrops_base_value_cmp(hv->value, kin->value)) {
                pmix_output_verbose(10, pmix_globals.debug_output,
                                    "EQUAL VALUE - IGNORING");
                return PMIX_SUCCESS;
            }
            if (9 < pmix_output_get_verbosity(pmix_globals.debug_output)) {
                char *tmp;
                tmp = PMIx_Value_string(kin->value);
                pmix_output(0, "VALUE UPDATING TO: %s", tmp);
                free(tmp);
            }
            PMIX_VALUE_RELEASE(hv->value);
        }
        /* eventually, we want to eliminate this copy */
        PMIX_BFROPS_COPY(rc, pmix_globals.mypeer, (void **)&hv->value, kin->value, PMIX_VALUE);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            return rc;
        }
        return PMIX_SUCCESS;
    }

    /* we don't already have it, so create it */
    PMIX_DSTOR_NEW(hv, kid);
    if (NULL == hv) {
        return PMIX_ERR_NOMEM;
    }
    /* eventually, we want to eliminate this copy */
    PMIX_BFROPS_COPY(rc, pmix_globals.mypeer, (void **)&hv->value, kin->value, PMIX_VALUE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        free(hv);
        return rc;
    }
    pmix_pointer_array_add(&proc_data->data, hv);

    return PMIX_SUCCESS;
}

pmix_status_t pmix_hash_fetch(pmix_hash_table_t *table,
                              pmix_rank_t rank, const char *key,
                              pmix_value_t **kvs)
{
    pmix_status_t rc = PMIX_SUCCESS;
    pmix_proc_data_t *proc_data;
    pmix_dstor_t *hv;
    uint32_t id, kid;
    char *node;
    pmix_regattr_input_t *p;
    pmix_info_t *info;
    size_t ninfo;
    int n;
    pmix_value_t *val;

    pmix_output_verbose(10, pmix_globals.debug_output,
                        "HASH:FETCH rank %s key %s",
                        PMIX_RANK_PRINT(rank),
                        (NULL == key) ? "NULL" : key);

    id = rank;

    /* - PMIX_RANK_UNDEF should return following statuses
     *     PMIX_ERR_NOT_FOUND | PMIX_SUCCESS
     * - specified rank can return following statuses
     *     PMIX_ERR_NOT_FOUND | PMIX_ERR_NOT_FOUND | PMIX_SUCCESS
     * special logic is basing on these statuses on a client and a server */
    if (PMIX_RANK_UNDEF == rank) {
        rc = pmix_hash_table_get_first_key_uint32(table, &id, (void **) &proc_data,
                                                  (void **) &node);
        if (PMIX_SUCCESS != rc) {
            pmix_output_verbose(10, pmix_globals.debug_output,
                                "HASH:FETCH[%s:%d] proc data for rank %s not found",
                                __func__, __LINE__,
                                PMIX_RANK_PRINT(rank));
            return PMIX_ERR_NOT_FOUND;
        }
    }

    if (NULL != key) {
        /* lookup the key's corresponding index - this should be
         * moved to the periphery of the PMIx library so we can
         * refer to the key numerically throughout the internals
         */
        p = pmix_hash_lookup_key(UINT32_MAX, key);
        if (NULL == p) {
            /* we don't know this key */
            return PMIX_ERR_BAD_PARAM;
        }
        kid = p->index;
    }

    while (PMIX_SUCCESS == rc) {
        proc_data = lookup_proc(table, id, false);
        if (NULL == proc_data) {
            pmix_output_verbose(10, pmix_globals.debug_output,
                                "HASH:FETCH[%s:%d] proc data for rank %s not found",
                                __func__, __LINE__,
                                PMIX_RANK_PRINT(rank));
            return PMIX_ERR_NOT_FOUND;
        }

        /* if the key is NULL, then the user wants -all- data
         * put by the specified rank */
        if (NULL == key) {
            /* we will return the data as an array of pmix_info_t
             * in the kvs pmix_value_t */
            val = (pmix_value_t *) malloc(sizeof(pmix_value_t));
            if (NULL == val) {
                return PMIX_ERR_NOMEM;
            }
            val->type = PMIX_DATA_ARRAY;
            val->data.darray = (pmix_data_array_t *) malloc(sizeof(pmix_data_array_t));
            if (NULL == val->data.darray) {
                PMIX_VALUE_RELEASE(val);
                return PMIX_ERR_NOMEM;
            }
            val->data.darray->type = PMIX_INFO;
            val->data.darray->size = 0;
            val->data.darray->array = NULL;
            ninfo = 0;
            for (n=0; n < proc_data->data.size; n++) {
                hv = (pmix_dstor_t*)pmix_pointer_array_get_item(&proc_data->data, n);
                if (NULL != hv) {
                    ++ninfo;
                }
            }
            PMIX_INFO_CREATE(info, ninfo);
            if (NULL == info) {
                PMIX_VALUE_RELEASE(val);
                return PMIX_ERR_NOMEM;
            }
            val->data.darray->array = info;
            val->data.darray->size = ninfo;
            /* copy the data */
            ninfo = 0;
            for (n=0; n < proc_data->data.size; n++) {
                hv = (pmix_dstor_t*)pmix_pointer_array_get_item(&proc_data->data, n);
                if (NULL != hv) {
                    p = pmix_hash_lookup_key(hv->index, NULL);
                    if (NULL == p) {
                        /* should never happen */
                        PMIX_VALUE_RELEASE(val);
                        return PMIX_ERR_NOT_FOUND;
                    }
                    PMIX_LOAD_KEY(info[ninfo].key, p->string);
                    PMIx_Value_xfer(&info[ninfo].value, hv->value);
                    ++ninfo;
                }
            }
            *kvs = val;
            return PMIX_SUCCESS;
        } else {
            /* find the value from within this proc_data object */
            hv = lookup_keyval(proc_data, kid);
            if (NULL != hv) {
                /* create the copy */
                PMIX_BFROPS_COPY(rc, pmix_globals.mypeer, (void **) kvs, hv->value, PMIX_VALUE);
                if (PMIX_SUCCESS != rc) {
                    PMIX_ERROR_LOG(rc);
                    return rc;
                }
                break;
            } else if (PMIX_RANK_UNDEF != rank) {
                pmix_output_verbose(10, pmix_globals.debug_output,
                                    "HASH:FETCH data for key %s not found", key);
                return PMIX_ERR_NOT_FOUND;
            }
        }

        rc = pmix_hash_table_get_next_key_uint32(table, &id, (void **) &proc_data, node,
                                                 (void **) &node);
        if (PMIX_SUCCESS != rc) {
            pmix_output_verbose(10, pmix_globals.debug_output,
                                "%s:%d HASH:FETCH data for key %s not found", __func__, __LINE__,
                                key);
            return PMIX_ERR_NOT_FOUND;
        }
    }

    return rc;
}

pmix_status_t pmix_hash_remove_data(pmix_hash_table_t *table,
                                    pmix_rank_t rank, const char *key)
{
    pmix_status_t rc = PMIX_SUCCESS;
    pmix_proc_data_t *proc_data;
    pmix_dstor_t *d;
    uint32_t id, kid;
    int n;
    char *node;
    pmix_regattr_input_t *p;

    if (NULL != key) {
        p = pmix_hash_lookup_key(UINT32_MAX, key);
        if (NULL == p) {
            /* we don't know this key */
            return PMIX_ERR_BAD_PARAM;
        }
        kid = p->index;
    }

    /* if the rank is wildcard, we want to apply this to
     * all rank entries */
    if (PMIX_RANK_WILDCARD == rank) {
        rc = pmix_hash_table_get_first_key_uint32(table, &id, (void **) &proc_data,
                                                  (void **) &node);
        while (PMIX_SUCCESS == rc) {
            if (NULL != proc_data) {
                if (NULL == key) {
                    PMIX_RELEASE(proc_data);
                } else {
                    for (n=0; n < proc_data->data.size; n++) {
                        d = (pmix_dstor_t*)pmix_pointer_array_get_item(&proc_data->data, n);
                        if (NULL != d && kid == d->index) {
                            if (NULL != d->value) {
                                PMIX_VALUE_RELEASE(d->value);
                            }
                            free(d);
                            pmix_pointer_array_set_item(&proc_data->data, n, NULL);
                            break;
                        }
                    }
                }
            }
            rc = pmix_hash_table_get_next_key_uint32(table, &id, (void **) &proc_data, node,
                                                     (void **) &node);
        }
        return PMIX_SUCCESS;
    }

    /* lookup the specified proc */
    if (NULL == (proc_data = lookup_proc(table, rank, false))) {
        /* no data for this proc */
        return PMIX_SUCCESS;
    }

    /* if key is NULL, remove all data for this proc */
    if (NULL == key) {
        for (n=0; n < proc_data->data.size; n++) {
            d = (pmix_dstor_t*)pmix_pointer_array_get_item(&proc_data->data, n);
            if (NULL != d) {
                if (NULL != d->value) {
                    PMIX_VALUE_RELEASE(d->value);
                }
                free(d);
                pmix_pointer_array_set_item(&proc_data->data, n, NULL);
            }
        }
        /* remove the proc_data object itself from the jtable */
        pmix_hash_table_remove_value_uint32(table, rank);
        /* cleanup */
        PMIX_RELEASE(proc_data);
        return PMIX_SUCCESS;
    }

    /* remove this item */
    for (n=0; n < proc_data->data.size; n++) {
        d = (pmix_dstor_t*)pmix_pointer_array_get_item(&proc_data->data, n);
        if (NULL != d && kid == d->index) {
            if (NULL != d->value) {
                PMIX_VALUE_RELEASE(d->value);
            }
            free(d);
            pmix_pointer_array_set_item(&proc_data->data, n, NULL);
            break;
        }
    }

    return PMIX_SUCCESS;
}

/**
 * Find data for a given key in a given pmix_list_t.
 */
static pmix_dstor_t *lookup_keyval(pmix_proc_data_t *proc_data, uint32_t kid)
{
    pmix_dstor_t *d;
    int n;

    for (n=0; n < proc_data->data.size; n++) {
        d = (pmix_dstor_t*)pmix_pointer_array_get_item(&proc_data->data, n);
        if (NULL != d && kid == d->index) {
            return d;
        }
    }

    return NULL;
}

/**
 * Find proc_data_t container associated with given
 * pmix_identifier_t.
 */
static pmix_proc_data_t *lookup_proc(pmix_hash_table_t *jtable, uint32_t id, bool create)
{
    pmix_proc_data_t *proc_data = NULL;

    pmix_hash_table_get_value_uint32(jtable, id, (void **) &proc_data);
    if (NULL == proc_data && create) {
        /* The proc clearly exists, so create a data structure for it */
        proc_data = PMIX_NEW(pmix_proc_data_t);
        if (NULL == proc_data) {
            pmix_output(0, "pmix:client:hash:lookup_pmix_proc: unable to allocate proc_data_t\n");
            return NULL;
        }
        pmix_hash_table_set_value_uint32(jtable, id, proc_data);
    }

    return proc_data;
}

void pmix_hash_register_key(uint32_t inid,
                            pmix_regattr_input_t *ptr)
{
    uint32_t id;
    pmix_regattr_input_t *p = NULL;

    if (UINT32_MAX == inid) {
        /* compute a hash of the string representation */
        PMIX_HASH_STR(ptr->string, id);
    } else {
        id = inid;
    }
    /* check to see if this key was already registered */
    pmix_hash_table_get_value_uint32(&pmix_globals.keyindex,
                                     id, (void**)&p);
    if (NULL != p) {
        /* already have this one */
        return;
    }
    /* store the pointer in the hash */
    pmix_hash_table_set_value_uint32(&pmix_globals.keyindex,
                                     id, ptr);
}

pmix_regattr_input_t* pmix_hash_lookup_key(uint32_t inid,
                                           const char *key)
{
    uint32_t id;
    pmix_regattr_input_t *ptr = NULL;
    char *node;
    pmix_status_t rc;

    if (UINT32_MAX == inid) {
        if (NULL == key) {
            /* they have to give us something! */
            return NULL;
        }
        /* if it is a PMIx standard key, then we look up the
         * index - this needs to be optimized and provided
         * as a separate function as we will need it at
         * all APIs to convert incoming keys! */
        if (PMIX_CHECK_RESERVED_KEY(key)) {
            id = UINT32_MAX;
            rc = pmix_hash_table_get_first_key_uint32(&pmix_globals.keyindex,
                                                      &id, (void **) &ptr,
                                                      (void **) &node);
            while (PMIX_SUCCESS == rc) {
                if (0 == strcmp(key, ptr->string)) {
                    break;
                }
                rc = pmix_hash_table_get_next_key_uint32(&pmix_globals.keyindex, &id,
                                                         (void **) &ptr, node,
                                                         (void **) &node);
            }
            if (UINT32_MAX == id) {
                return NULL;
            }
        } else {
            /* compute a hash of the string representation */
            PMIX_HASH_STR(key, id);
        }
    } else {
        id = inid;
    }
    /* get the pointer from the hash */
    pmix_hash_table_get_value_uint32(&pmix_globals.keyindex,
                                     id, (void**)&ptr);

    if (NULL == ptr && !PMIX_CHECK_RESERVED_KEY(key)) {
        /* register this one */
        ptr = (pmix_regattr_input_t*)pmix_malloc(sizeof(pmix_regattr_input_t));
        ptr->index = id;
        ptr->name = strdup(key);
        ptr->string = strdup(key);
        ptr->type = PMIX_UNDEF;
        ptr->description = (char**)pmix_malloc(2 * sizeof(char*));
        ptr->description[0] = strdup("USER DEFINED");
        ptr->description[1] = NULL;
        pmix_hash_register_key(ptr->index, ptr);
    }
    return ptr;  // will be NULL if nothing found
}
