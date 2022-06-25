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
 * Copyright (c) 2022      Triad National Security, LLC. All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 */

////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
// Please note that structures here potentially use a custom memory allocator.
// This means that memory management calls should go through calls like
// pmix_tma_calloc(), not calloc().
// See src/class/pmix_object.h for more information about TMA.
////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////

#include "src/include/pmix_config.h"

#include "pmix_hash2.h"

#include "src/include/pmix_hash_string.h"
#include "src/include/pmix_stdint.h"

#if 0 // TODO(skg)
#include "src/class/pmix_hash_table.h"
#else
#include "pmix_hash_table2.h"
#endif
#if 0 // TODO(skg)
#include "src/class/pmix_pointer_array.h"
#else
#include "pmix_pointer_array2.h"
#endif
#include "src/include/pmix_dictionary.h"
#include "src/include/pmix_globals.h"
#include "src/include/pmix_hash_string.h"
#include "src/mca/bfrops/bfrops.h"
#include "src/util/pmix_error.h"
#include "src/util/pmix_output.h"

// TODO(skg) This shouldn't be here. For XFER now.
#include "gds_shmem_utils.h"

#ifdef HAVE_STRING_H
#include <string.h>
#endif

// TODO(skg) This doesn't belong here.
#define PMIX_DSTOR_NEW_TMA(d, k, tma)                                  \
do {                                                                   \
    (d) = (pmix_dstor_t*)pmix_tma_malloc((tma), sizeof(pmix_dstor_t)); \
    if (NULL != (d)) {                                                 \
        (d)->index = k;                                                \
        (d)->qualindex = UINT32_MAX;                                   \
        (d)->value = NULL;                                             \
    }                                                                  \
} while(0)

/**
 * Data for a particular pmix process The name association is maintained in the
 * proc_data hash table.
 */
typedef struct {
    pmix_object_t super;
    /*
     * Array of pmix_dstor_t structures containing all data
     * received from this process.
     */
#if 0 
    pmix_pointer_array2_t data;
    pmix_pointer_array2_t quals;
#else
    pmix_pointer_array2_t *data;
    pmix_pointer_array2_t *quals;
#endif
} pmix_proc_data2_t;

static void pdcon(pmix_proc_data2_t *p)
{
    pmix_tma_t *tma = pmix_obj_get_tma(&p->super);
    // TODO(skg) Note that we moved to PMIX_NEW. Cannot use PMIX_CONSTRUCT with
    // the TMA we care about.
#if 0
    PMIX_CONSTRUCT(&p->data, pmix_pointer_array2_t, tma);
    pmix_pointer_array2_init(&p->data, 128, INT_MAX, 128);
    PMIX_CONSTRUCT(&p->quals, pmix_pointer_array2_t, tma);
    pmix_pointer_array2_init(&p->quals, 1, INT_MAX, 1);
#else
    p->data = PMIX_NEW(pmix_pointer_array2_t, tma);
    pmix_pointer_array2_init(p->data, 128, INT_MAX, 128);
    p->quals = PMIX_NEW(pmix_pointer_array2_t, tma);
    pmix_pointer_array2_init(p->quals, 1, INT_MAX, 1);
#endif
}

static void pddes(pmix_proc_data2_t *p)
{
    int n;
    size_t nq;
    pmix_dstor_t *d;
    pmix_qual_t *q;
    pmix_data_array_t *darray;
    pmix_tma_t *tma = pmix_obj_get_tma(&p->super);

    for (n=0; n < p->data->size; n++) {
        d = (pmix_dstor_t*)pmix_pointer_array2_get_item(p->data, n);
        if (NULL != d) {
            PMIX_DSTOR_RELEASE(d);
            pmix_pointer_array2_set_item(p->data, n, NULL);
        }
    }
    PMIX_DESTRUCT(&p->data);
    for (n=0; n < p->quals->size; n++) {
        darray = (pmix_data_array_t*)pmix_pointer_array2_get_item(p->quals, n);
        if (NULL != darray) {
            q = (pmix_qual_t*)darray->array;
            for (nq=0; nq < darray->size; nq++) {
                if (NULL != q[nq].value) {
                    PMIX_VALUE_RELEASE(q[nq].value);
                }
            }
            pmix_tma_free(tma, darray->array);
            pmix_tma_free(tma, darray);
        }
        pmix_pointer_array2_set_item(p->quals, n, NULL);
    }
    PMIX_DESTRUCT(&p->quals);
}
static PMIX_CLASS_INSTANCE(pmix_proc_data2_t, pmix_object_t, pdcon, pddes);

static pmix_dstor_t *lookup_keyval(pmix_proc_data2_t *proc, uint32_t kid,
                                   pmix_info_t *qualifiers, size_t nquals);
static pmix_proc_data2_t *lookup_proc(pmix_hash_table2_t *jtable, uint32_t id, bool create);
static void erase_qualifiers(pmix_proc_data2_t *proc,
                             uint32_t index);


pmix_status_t pmix_hash2_store(pmix_hash_table2_t *table,
                              pmix_rank_t rank, pmix_kval_t *kin,
                              pmix_info_t *qualifiers, size_t nquals)
{
    pmix_proc_data2_t *proc_data;
    uint32_t kid;
    pmix_dstor_t *hv;
    pmix_regattr_input_t *p;
    pmix_status_t rc;
    pmix_data_array_t *darray;
    pmix_qual_t *qarray;
    size_t n, m = 0;
    pmix_tma_t *tma = pmix_obj_get_tma(&table->super);

    pmix_output_verbose(10, pmix_globals.debug_output,
                        "HASH:STORE:QUAL rank %s key %s",
                        PMIX_RANK_PRINT(rank),
                        (NULL == kin) ? "NULL KVAL" : kin->key);

    if (NULL == kin) {
        return PMIX_ERR_BAD_PARAM;
    }

    /* lookup the key's corresponding index - this should be
     * moved to the periphery of the PMIx library so we can
     * refer to the key numerically throughout the internals
     */
    p = pmix_hash2_lookup_key(UINT32_MAX, kin->key);
    if (NULL == p) {
        /* we don't know this key */
        pmix_output_verbose(10, pmix_globals.debug_output,
                            "%s UNKNOWN KEY: %s",
                            PMIX_NAME_PRINT(&pmix_globals.myid),
                            kin->key);
        return PMIX_ERR_BAD_PARAM;
    }
    kid = p->index;

    /* lookup the proc data object for this proc - create
     * it if we don't already have it */
    if (NULL == (proc_data = lookup_proc(table, rank, true))) {
        return PMIX_ERR_NOMEM;
    }

    /* see if we already have this key-value */
    hv = lookup_keyval(proc_data, kid, qualifiers, nquals);
    if (NULL != hv) {
        if (9 < pmix_output_get_verbosity(pmix_globals.debug_output)) {
            // Note that this doesn't have to use a TMA because it is just a
            // temporary value.
            char *tmp;
            tmp = PMIx_Value_string(hv->value);
            pmix_output(0, "%s PREEXISTING ENTRY FOR PROC %s KEY %s: %s",
                        PMIX_NAME_PRINT(&pmix_globals.myid),
                        PMIX_RANK_PRINT(rank), kin->key, tmp);
            free(tmp);
        }
        /* yes we do - so just replace the current value if it changed */
        if (NULL != hv->value) {
            if (PMIX_EQUAL == PMIx_Value_compare(hv->value, kin->value)) {
                pmix_output_verbose(10, pmix_globals.debug_output,
                                    "EQUAL VALUE - IGNORING");
                return PMIX_SUCCESS;
            }
            if (9 < pmix_output_get_verbosity(pmix_globals.debug_output)) {
                // Note that this doesn't have to use a TMA because it is just a
                // temporary value.
                char *tmp;
                tmp = PMIx_Value_string(kin->value);
                pmix_output(0, "%s VALUE UPDATING TO: %s",
                            PMIX_NAME_PRINT(&pmix_globals.myid), tmp);
                free(tmp);
            }
            PMIX_VALUE_RELEASE(hv->value);
        }
        /* eventually, we want to eliminate this copy */
        // TODO(skg) We need to modify this.
        PMIX_BFROPS_COPY(rc, pmix_globals.mypeer, (void **)&hv->value, kin->value, PMIX_VALUE);
        assert(false);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            return rc;
        }
        return PMIX_SUCCESS;
    }

    /* we don't already have it, so create it */
    PMIX_DSTOR_NEW_TMA(hv, kid, tma);
    if (NULL == hv) {
        return PMIX_ERR_NOMEM;
    }
    if (NULL != qualifiers) {
        /* count the number of actual qualifiers */
        for (n=0, m=0; n < nquals; n++) {
            if (PMIX_INFO_IS_QUALIFIER(&qualifiers[n])) {
                ++m;
            }
        }
        if (0 < m) {
            darray = (pmix_data_array_t*)pmix_tma_malloc(tma, sizeof(pmix_data_array_t));
            darray->array = (pmix_qual_t*)pmix_tma_malloc(tma, m * sizeof(pmix_qual_t));
            darray->size = m;
            hv->qualindex = pmix_pointer_array2_add(proc_data->quals, darray);
            qarray = (pmix_qual_t*)darray->array;
            for (n=0, m=0; n < nquals; n++) {
                if (PMIX_INFO_IS_QUALIFIER(&qualifiers[n])) {
                    p = pmix_hash2_lookup_key(UINT32_MAX, qualifiers[n].key);
                    if (NULL == p) {
                        /* we don't know this key */
                        pmix_output_verbose(10, pmix_globals.debug_output,
                                            "%s UNKNOWN KEY: %s",
                                            PMIX_NAME_PRINT(&pmix_globals.myid),
                                            kin->key);
                        erase_qualifiers(proc_data, hv->qualindex);
                        PMIX_DSTOR_RELEASE(hv);
                        return PMIX_ERR_BAD_PARAM;
                    }
                    qarray[n].index = p->index;
                    // TODO(skg) We need to modify this.
                    assert(false);
                    PMIX_BFROPS_COPY(rc, pmix_globals.mypeer, (void **)&qarray[m].value, &qualifiers[n].value, PMIX_VALUE);
                    if (PMIX_SUCCESS != rc) {
                        PMIX_ERROR_LOG(rc);
                        erase_qualifiers(proc_data, hv->qualindex);
                        PMIX_DSTOR_RELEASE(hv);
                        return rc;
                    }
                    ++m;
                }
            }
        }
    }

    /* XXX(skg) eventually, we want to eliminate this copy */
    // TODO(skg) We need to modify this.
#if 0
    PMIX_BFROPS_COPY(rc, pmix_globals.mypeer, (void **)&hv->value, kin->value, PMIX_VALUE);
#else
    PMIX_GDS_SHMEM_BFROPS_COPY_TMA(rc, &hv->value, kin->value, PMIX_VALUE, tma);
#endif
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        if (UINT32_MAX != hv->qualindex) {
            /* release the associated qualifiers */
            erase_qualifiers(proc_data, hv->qualindex);
        }
        PMIX_DSTOR_RELEASE(hv);
        return rc;
    }
    pmix_output_verbose(10, pmix_globals.debug_output,
                        "%s ADDING KEY %s VALUE %s FOR RANK %s WITH %u QUALS TO TABLE %s",
                        PMIX_NAME_PRINT(&pmix_globals.myid),
                        kin->key, PMIx_Value_string(kin->value),
                        PMIX_RANK_PRINT(rank), (unsigned)m,
                        table->ht_label);
    pmix_pointer_array2_add(proc_data->data, hv);
    return PMIX_SUCCESS;
}

pmix_status_t pmix_hash2_fetch(pmix_hash_table2_t *table,
                              pmix_rank_t rank,
                              const char *key,
                              pmix_info_t *qualifiers, size_t nquals,
                              pmix_list_t *kvals)
{
    pmix_status_t rc = PMIX_SUCCESS;
    pmix_proc_data2_t *proc_data;
    pmix_dstor_t *hv;
    uint32_t id, kid=UINT32_MAX;
    char *node;
    pmix_regattr_input_t *p;
    pmix_info_t *iptr;
    size_t nq, m;
    int n;
    pmix_kval_t *kv;
    bool fullsearch = false;
    pmix_data_array_t *darray;
    pmix_qual_t *quals;
#if 0 // TODO(skg)
    pmix_tma_t *table_tma = pmix_obj_get_tma(&table->super);
    pmix_tma_t *kvals_tma = pmix_obj_get_tma(&kvals->super);
    pmix_tma_t *tma = NULL;

    if (table_tma && kvals_tma) {
        // Just pick one. Hopefully they are compatible.
        tma = table_tma;
    }
#endif

    pmix_output_verbose(10, pmix_globals.debug_output,
                        "%s HASH:FETCH id %s key %s",
                        PMIX_NAME_PRINT(&pmix_globals.myid),
                        PMIX_RANK_PRINT(rank),
                        (NULL == key) ? "NULL" : key);

    /* - PMIX_RANK_UNDEF should return following statuses
     *     PMIX_ERR_NOT_FOUND | PMIX_SUCCESS
     * - specified rank can return following statuses
     *     PMIX_ERR_NOT_FOUND | PMIX_ERR_NOT_FOUND | PMIX_SUCCESS
     * special logic is basing on these statuses on a client and a server */
    if (PMIX_RANK_UNDEF == rank) {
        rc = pmix_hash_table2_get_first_key_uint32(table, &id, (void **) &proc_data,
                                                  (void **) &node);
        if (PMIX_SUCCESS != rc) {
            pmix_output_verbose(10, pmix_globals.debug_output,
                                "HASH:FETCH[%s:%d] proc data for rank %s not found",
                                __func__, __LINE__, PMIX_RANK_PRINT(rank));
            return PMIX_ERR_NOT_FOUND;
        }
        fullsearch = true;
    } else {
        id = rank;
    }

    if (NULL != key) {
        /* lookup the key's corresponding index - this should be
         * moved to the periphery of the PMIx library so we can
         * refer to the key numerically throughout the internals
         */
        p = pmix_hash2_lookup_key(UINT32_MAX, key);
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
            /* copy the data */
            for (n=0; n < proc_data->data->size; n++) {
                hv = (pmix_dstor_t*)pmix_pointer_array2_get_item(proc_data->data, n);
                if (NULL != hv) {
                    p = pmix_hash2_lookup_key(hv->index, NULL);
                    if (NULL == p) {
                        return PMIX_ERR_NOT_FOUND;
                    }
                    pmix_output_verbose(10, pmix_globals.debug_output,
                                        "%s FETCH NULL LOOKING AT %s",
                                        PMIX_NAME_PRINT(&pmix_globals.myid), p->name);
                    /* if the rank is UNDEF, we ignore reserved keys */
                    if (PMIX_RANK_UNDEF == rank &&
                        PMIX_CHECK_RESERVED_KEY(p->string)) {
                        continue;
                    }
                    if (UINT32_MAX != hv->qualindex) {
                        pmix_output_verbose(10, pmix_globals.debug_output,
                                            "%s INCLUDE %s VALUE %u FROM TABLE %s FOR RANK %s",
                                            PMIX_NAME_PRINT(&pmix_globals.myid), p->name,
                                            (unsigned)hv->value->data.size, table->ht_label, PMIX_RANK_PRINT(rank));
                        /* this is a qualified value - need to return it as such */
                        // TODO(skg) We need to modify this.
                        assert(false);
                        PMIX_KVAL_NEW(kv, PMIX_QUALIFIED_VALUE);
                        darray = (pmix_data_array_t*)pmix_pointer_array2_get_item(proc_data->quals, hv->qualindex);
                        quals = (pmix_qual_t*)darray->array;
                        nq = darray->size;
                        // TODO(skg) We need to modify this.
                        assert(false);
                        PMIX_DATA_ARRAY_CREATE(darray, nq+1, PMIX_INFO);
                        iptr = (pmix_info_t*)darray->array;
                        /* the first location is the actual value */
                        // TODO(skg) We need to modify this.
                        assert(false);
                        PMIX_LOAD_KEY(&iptr[0].key, p->string);
                        PMIx_Value_xfer(&iptr[0].value, hv->value);
                        /* now add the qualifiers */
                        for (m=0; m < nq; m++) {
                            p = pmix_hash2_lookup_key(quals[m].index, NULL);
                            if (NULL == p) {
                                /* should never happen */
                                PMIX_RELEASE(kv);
                                PMIX_DATA_ARRAY_FREE(darray);
                                return PMIX_ERR_BAD_PARAM;
                            }
                            PMIX_LOAD_KEY(&iptr[m+1].key, p->string);
                            PMIx_Value_xfer(&iptr[m+1].value, quals[m].value);
                            PMIX_INFO_SET_QUALIFIER(&iptr[m+1]);
                        }
                        kv->value->type = PMIX_DATA_ARRAY;
                        kv->value->data.darray = darray;
                        pmix_list_append(kvals, &kv->super);
                    } else {
                        // TODO(skg) We need to modify this.
                        // Ultimately we want to get rid of this copy.
#if 1
                        pmix_output_verbose(10, pmix_globals.debug_output,
                                            "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxKEY=%s", p->string);
                        kv = PMIX_NEW(pmix_kval_t);
                        kv->key = pmix_tma_strdup(NULL, p->string);
                        PMIX_VALUE_XFER(rc, kv->value, hv->value);
                        pmix_list_append(kvals, &kv->super);
#else
                        kv = PMIX_NEW(pmix_kval_t, tma);
                        kv->key = pmix_tma_strdup(tma, p->string);
                        PMIX_GDS_SHMEM_VALUE_XFER(rc, kv->value, hv->value, tma);
                        pmix_list_append(kvals, &kv->super);
#endif
                    }
                }
            }
            return PMIX_SUCCESS;
        } else {
            /* find the value from within this data object */
            hv = lookup_keyval(proc_data, kid, qualifiers, nquals);
            if (NULL != hv) {
                /* create the copy */
                // TODO(skg) We need to modify this.
                PMIX_KVAL_NEW(kv, key);
                // TODO(skg) We need to modify this.
                PMIX_BFROPS_COPY(rc, pmix_globals.mypeer, (void **)&kv->value, hv->value, PMIX_VALUE);
                if (PMIX_SUCCESS != rc) {
                    PMIX_ERROR_LOG(rc);
                    PMIX_RELEASE(kv);
                    return rc;
                }
                pmix_list_append(kvals, &kv->super);
                break;
            } else if (!fullsearch) {
                pmix_output_verbose(10, pmix_globals.debug_output,
                                    "HASH:FETCH data for key %s not found", key);
                return PMIX_ERR_NOT_FOUND;
            }
        }

        rc = pmix_hash_table2_get_next_key_uint32(table, &id, (void **) &proc_data, node,
                                                 (void **) &node);
        if (PMIX_SUCCESS != rc) {
            pmix_output_verbose(10, pmix_globals.debug_output,
                                "%s:%d HASH:FETCH data for key %s not found",
                                __func__, __LINE__, key);
            return PMIX_ERR_NOT_FOUND;
        }
    }

    return rc;
}

pmix_status_t pmix_hash2_remove_data(pmix_hash_table2_t *table,
                                    pmix_rank_t rank, const char *key)
{
    pmix_status_t rc = PMIX_SUCCESS;
    pmix_proc_data2_t *proc_data;
    pmix_dstor_t *d;
    uint32_t id, kid=UINT32_MAX;
    int n;
    char *node;
    pmix_regattr_input_t *p;
    pmix_tma_t *tma = pmix_obj_get_tma(&table->super);

    if (NULL != key) {
        p = pmix_hash2_lookup_key(UINT32_MAX, key);
        if (NULL == p) {
            /* we don't know this key */
            return PMIX_ERR_BAD_PARAM;
        }
        kid = p->index;
    }

    /* if the rank is wildcard, we want to apply this to
     * all rank entries */
    if (PMIX_RANK_WILDCARD == rank) {
        rc = pmix_hash_table2_get_first_key_uint32(table, &id, (void **) &proc_data,
                                                  (void **) &node);
        while (PMIX_SUCCESS == rc) {
            if (NULL != proc_data) {
                if (NULL == key) {
                    PMIX_RELEASE(proc_data);
                } else {
                    for (n=0; n < proc_data->data->size; n++) {
                        d = (pmix_dstor_t*)pmix_pointer_array2_get_item(proc_data->data, n);
                        if (NULL != d && kid == d->index) {
                            if (NULL != d->value) {
                                PMIX_VALUE_RELEASE(d->value);
                            }
                            if (UINT32_MAX != d->qualindex) {
                                erase_qualifiers(proc_data, d->qualindex);
                            }
                            pmix_tma_free(tma, d);
                            pmix_pointer_array2_set_item(proc_data->data, n, NULL);
                            break;
                        }
                    }
                }
            }
            rc = pmix_hash_table2_get_next_key_uint32(table, &id, (void **) &proc_data, node,
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
        for (n=0; n < proc_data->data->size; n++) {
            d = (pmix_dstor_t*)pmix_pointer_array2_get_item(proc_data->data, n);
            if (NULL != d) {
                if (NULL != d->value) {
                    PMIX_VALUE_RELEASE(d->value);
                }
                if (UINT32_MAX != d->qualindex) {
                    erase_qualifiers(proc_data, d->qualindex);
                }
                pmix_tma_free(tma, d);
                pmix_pointer_array2_set_item(proc_data->data, n, NULL);
            }
        }
        /* remove the proc_data object itself from the jtable */
        pmix_hash_table2_remove_value_uint32(table, rank);
        /* cleanup */
        PMIX_RELEASE(proc_data);
        return PMIX_SUCCESS;
    }

    /* remove this item */
    for (n=0; n < proc_data->data->size; n++) {
        d = (pmix_dstor_t*)pmix_pointer_array2_get_item(proc_data->data, n);
        if (NULL != d && kid == d->index) {
            if (NULL != d->value) {
                PMIX_VALUE_RELEASE(d->value);
            }
            if (UINT32_MAX != d->qualindex) {
                erase_qualifiers(proc_data, d->qualindex);
            }
            pmix_tma_free(tma, d);
            pmix_pointer_array2_set_item(proc_data->data, n, NULL);
            break;
        }
    }

    return PMIX_SUCCESS;
}

/**
 * Find data for a given key in a given pmix_list_t.
 */
static pmix_dstor_t *lookup_keyval(pmix_proc_data2_t *proc_data, uint32_t kid,
                                   pmix_info_t *qualifiers, size_t nquals)
{
    pmix_dstor_t *d;
    pmix_data_array_t *darray;
    pmix_qual_t *qarray;
    pmix_regattr_input_t *p;
    size_t m, numquals = 0, nq, nfound;
    int n;

    p = pmix_hash2_lookup_key(kid, NULL);

    if (NULL != qualifiers) {
        /* count the qualifiers */
        for (m=0; m < nquals; m++) {
            /* if this isn't marked as a qualifier, skip it */
            if (PMIX_INFO_IS_QUALIFIER(&qualifiers[m])) {
                ++numquals;
            }
        }
    }

    for (n=0; n < proc_data->data->size; n++) {
        d = (pmix_dstor_t*)pmix_pointer_array2_get_item(proc_data->data, n);
        if (NULL == d) {
            continue;
        }
        if (kid == d->index) {
            if (0 < numquals) {
                if (UINT32_MAX == d->qualindex) {
                    continue;
                }
                darray = (pmix_data_array_t*)pmix_pointer_array2_get_item(proc_data->quals, d->qualindex);
                qarray = (pmix_qual_t*)darray->array;
                nfound = 0;
                /* check the qualifiers */
                for (m=0; m < nquals; m++) {
                    /* if this isn't marked as a qualifier, skip it */
                    if (!PMIX_INFO_IS_QUALIFIER(&qualifiers[m])) {
                        continue;
                    }
                    p = pmix_hash2_lookup_key(UINT32_MAX, qualifiers[m].key);
                    if (NULL == p) {
                        /* we don't know this key */
                        return NULL;
                    }
                    for (nq=0; nq < darray->size; nq++) {
                        /* see if the keys match */
                        if (qarray[nq].index == p->index) {
                            /* if the values don't match, then we reject
                             * this entry */
                            if (PMIX_EQUAL == PMIx_Value_compare(&qualifiers[m].value, qarray[nq].value)) {
                                /* match! */
                                ++nfound;
                                break;
                            }
                        }
                    }
                }
                /* did we get a complete match? */
                if (nfound == numquals) {
                    return d;
                }
            } else {
                /* if the stored key is also "unqualified",
                 * then return it */
                if (UINT32_MAX == d->qualindex) {
                    return d;
                }
            }
        }
    }

    return NULL;
}

/**
 * Find proc_data_t container associated with given
 * pmix_identifier_t.
 */
static pmix_proc_data2_t *lookup_proc(pmix_hash_table2_t *jtable, uint32_t id, bool create)
{
    pmix_proc_data2_t *proc_data = NULL;
    pmix_tma_t *tma = pmix_obj_get_tma(&jtable->super);

    pmix_hash_table2_get_value_uint32(jtable, id, (void **) &proc_data);
    if (NULL == proc_data && create) {
        /* The proc clearly exists, so create a data structure for it */
        proc_data = PMIX_NEW(pmix_proc_data2_t, tma);
        if (NULL == proc_data) {
            return NULL;
        }
        pmix_hash_table2_set_value_uint32(jtable, id, proc_data);
    }

    return proc_data;
}

void pmix_hash2_register_key(uint32_t inid,
                            pmix_regattr_input_t *ptr)
{
    pmix_regattr_input_t *p = NULL;

    if (UINT32_MAX == inid) {
        /* store the pointer in the array */
#if 0 // TODO(skg) Remove
        pmix_pointer_array2_set_item(&pmix_globals.keyindex, pmix_globals.next_keyid, ptr);
#else
        pmix_pointer_array2_set_item((pmix_pointer_array2_t *)&pmix_globals.keyindex, pmix_globals.next_keyid, ptr);
#endif
        ptr->index = pmix_globals.next_keyid;
        pmix_globals.next_keyid += 1;
        return;
    }

    /* check to see if this key was already registered */
#if 0 // TODO(skg) Remove
    p = pmix_pointer_array2_get_item(&pmix_globals.keyindex, inid);
#else
    p = pmix_pointer_array2_get_item((pmix_pointer_array2_t *)&pmix_globals.keyindex, inid);
#endif
    if (NULL != p) {
        /* already have this one */
        return;
    }
    /* store the pointer in the table */
#if 0 // TODO(skg) Remove
    pmix_pointer_array2_set_item(&pmix_globals.keyindex, inid, ptr);
#else
    pmix_pointer_array2_set_item((pmix_pointer_array2_t *)&pmix_globals.keyindex, inid, ptr);
#endif
}

// TODO(skg) We may have to modify the signature of this function. How will we
// get a handle to the TMA?
pmix_regattr_input_t* pmix_hash2_lookup_key(uint32_t inid,
                                           const char *key)
{
    int id;
    pmix_regattr_input_t *ptr = NULL;

    if (UINT32_MAX == inid) {
        if (NULL == key) {
            /* they have to give us something! */
            return NULL;
        }
        if (PMIX_CHECK_RESERVED_KEY(key)) {
            /* reserved keys are in the front of the table */
            for (id = 0; id < PMIX_INDEX_BOUNDARY; id++) {
#if 0 // TODO(skg) Remove
                ptr = pmix_pointer_array2_get_item(&pmix_globals.keyindex, id);
#else
                ptr = pmix_pointer_array2_get_item((pmix_pointer_array2_t *)&pmix_globals.keyindex, id);
#endif
                if (NULL != ptr) {
                    if (0 == strcmp(key, ptr->string)) {
                        return ptr;
                    }
                }
            }
            /* reserved keys must already have been registered */
            return NULL;
        }
        /* unreserved keys are at the back of the table */
        for (id = PMIX_INDEX_BOUNDARY; id < pmix_globals.keyindex.size; id++) {
#if 0 // TODO(skg) Remove
            ptr = pmix_pointer_array2_get_item(&pmix_globals.keyindex, id);
#else
            ptr = pmix_pointer_array2_get_item((pmix_pointer_array2_t *)&pmix_globals.keyindex, id);
#endif
            if (NULL != ptr) {
                if (0 == strcmp(key, ptr->string)) {
                    return ptr;
                }
            }
        }
        /* we didn't find it - register it */
        ptr = (pmix_regattr_input_t*)pmix_malloc(sizeof(pmix_regattr_input_t));
        ptr->name = strdup(key);
        ptr->string = strdup(key);
        ptr->type = PMIX_UNDEF; // we don't know what type the user will set
        ptr->description = (char**)pmix_malloc(2 * sizeof(char*));
        ptr->description[0] = strdup("USER DEFINED");
        ptr->description[1] = NULL;
        pmix_hash2_register_key(UINT32_MAX, ptr);
        return ptr;
    }

    /* get the pointer from the table - if it is a reserved key, then
     * it had to be registered at the beginning of time. If it is a
     * non-reserved key, then it had to be registered or else the caller
     * would not have an index to pass us. Thus, the pointer is either
     * found or not - we don't register it if not found. */
#if 0 // TODO(skg) Remove
    ptr = pmix_pointer_array2_get_item(&pmix_globals.keyindex, inid);
#else
    ptr = pmix_pointer_array2_get_item((pmix_pointer_array2_t *)&pmix_globals.keyindex, inid);
#endif
    return ptr;
}

static void erase_qualifiers(pmix_proc_data2_t *proc,
                             uint32_t index)
{
    pmix_data_array_t *darray;
    pmix_qual_t *qarray;
    size_t n;
    pmix_tma_t *tma = pmix_obj_get_tma(&proc->super);

    darray = (pmix_data_array_t*)pmix_pointer_array2_get_item(proc->quals, index);
    if (NULL == darray || NULL == darray->array) {
        return;
    }
    qarray = (pmix_qual_t*)darray->array;
    for (n=0; n < darray->size; n++) {
        if (NULL != qarray[n].value) {
            PMIX_VALUE_RELEASE(qarray[n].value);
        }
    }
    pmix_tma_free(tma, qarray);
    pmix_tma_free(tma, darray);
    pmix_pointer_array2_set_item(proc->quals, index, NULL);
}
