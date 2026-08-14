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
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
 * Copyright (c) 2022-2024 Triad National Security, LLC. All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 */

// TODO(skg) Assert that the tma from the table and keyindex match in all
// relevant places.

#include "src/include/pmix_config.h"

#include "src/include/pmix_stdint.h"

#include <string.h>

#include "src/class/pmix_hash_table.h"
#include "src/class/pmix_pointer_array.h"
#include "src/include/pmix_dictionary.h"
#include "src/include/pmix_globals.h"
#include "src/mca/bfrops/bfrops.h"
#include "src/mca/bfrops/base/bfrop_base_tma.h"
#include "src/mca/gds/base/base.h"
#include "src/util/pmix_error.h"
#include "src/util/pmix_output.h"

#include "src/util/pmix_hash.h"

/**
 * Utility function that takes a pointer to a pmix_keyindex_t and returns the
 * global keyindex if NULL, returns the input otherwise.
 */
static inline pmix_keyindex_t *get_keyindex_ptr(pmix_keyindex_t *input)
{
    return input ? input : &pmix_globals.keyindex;
}

/**
 * Data for a particular pmix process
 * The name association is maintained in the
 * proc_data hash table.
 */
typedef struct {
    pmix_object_t super;
    /**
     * Array of pmix_dstor_t structures containing all data received from this
     * process. Note that these are pointers because the shared-memory TMA
     * requires that these structures and their data reside on the heap.
     */
    pmix_pointer_array_t *data;
    pmix_pointer_array_t *quals;
} pmix_proc_data_t;
/* How many key slots to give a proc up front. Every proc in every
 * namespace gets one of these arrays in each of the internal, local
 * and remote tables, so the default is a memory/realloc trade made
 * across the whole job rather than a per-proc one: too low and a proc
 * that publishes a lot pays repeated reallocs, too high and a big job
 * carries slots nobody fills - and for gds/shmem3 that also inflates
 * the shared segment estimate. Registered in src/runtime/pmix_params.c
 * as pmix_hash_proc_alloc. */
int pmix_hash_proc_alloc = 128;

/* How many qualifier slots to give a proc up front. Most procs publish no
 * qualified values at all, so this stays far below pmix_hash_proc_alloc -
 * but it must not be 1. pmix_pointer_array's grow_table() rounds the new
 * size up to a multiple of the block size, so a block size of 1 grows the
 * array by exactly one slot per addition: a proc publishing q qualified
 * values did q reallocs. That is merely wasteful on the heap, and worse
 * under the gds/shmem3 TMA, where free is a no-op - each of those reallocs
 * copies the array and strands the old one in the shared segment, which is
 * part of what the segment sizing fluff is paying for. */
#define PMIX_HASH_QUAL_ALLOC 8

static void pdcon(pmix_proc_data_t *p)
{
    pmix_tma_t *const tma = pmix_obj_get_tma(&p->super);
    /* the array divides by its block size when it grows, so a value
     * the user set to zero or below has to be refused here rather than
     * become a SIGFPE later */
    const int nalloc = (0 < pmix_hash_proc_alloc) ? pmix_hash_proc_alloc : 128;

    p->data = PMIX_NEW(pmix_pointer_array_t, tma);
    pmix_pointer_array_init(p->data, nalloc, INT_MAX, nalloc);
    p->quals = PMIX_NEW(pmix_pointer_array_t, tma);
    pmix_pointer_array_init(p->quals, PMIX_HASH_QUAL_ALLOC, INT_MAX,
                            PMIX_HASH_QUAL_ALLOC);
}
size_t pmix_hash_sizeof_proc_storage(void)
{
    /* Mirror pdcon(): the object itself, plus the two pointer arrays it
     * constructs and the storage each of those allocates (the slot array
     * and its free-bit map). A caller pre-sizing a datastore before
     * filling it - gds/shmem3 has to size a shared-memory segment up
     * front - cannot see pmix_proc_data_t, so it has to ask. Keep this in
     * step with pdcon(); it is the only reason the two can drift. */
    const size_t nalloc = (0 < pmix_hash_proc_alloc)
                          ? (size_t)pmix_hash_proc_alloc : 128;
    const size_t bits_per_word = 8 * sizeof(uint64_t);

    return sizeof(pmix_proc_data_t)
           + 2 * sizeof(pmix_pointer_array_t)
           + nalloc * sizeof(void *)
           + ((nalloc + bits_per_word - 1) / bits_per_word) * sizeof(uint64_t)
           + PMIX_HASH_QUAL_ALLOC * sizeof(void *)
           + (((size_t)PMIX_HASH_QUAL_ALLOC + bits_per_word - 1) / bits_per_word)
                 * sizeof(uint64_t);
}

static void pddes(pmix_proc_data_t *p)
{
    int n;
    size_t nq;
    pmix_dstor_t *d;
    pmix_qual_t *q;
    pmix_data_array_t *darray;
    pmix_tma_t *const tma = pmix_obj_get_tma(&p->super);

    for (n=0; n < p->data->size; n++) {
        d = (pmix_dstor_t*)pmix_pointer_array_get_item(p->data, n);
        if (NULL != d) {
            pmix_dstor_release_tma(d, tma);
            pmix_pointer_array_set_item(p->data, n, NULL);
        }
    }
    PMIX_RELEASE(p->data);
    for (n=0; n < p->quals->size; n++) {
        darray = (pmix_data_array_t*)pmix_pointer_array_get_item(p->quals, n);
        if (NULL != darray) {
            q = (pmix_qual_t*)darray->array;
            for (nq=0; nq < darray->size; nq++) {
                if (NULL != q[nq].value) {
                    pmix_bfrops_base_tma_value_release(&q[nq].value, tma);
                }
            }
            pmix_tma_free(tma, darray->array);
            pmix_tma_free(tma, darray);
        }
        pmix_pointer_array_set_item(p->quals, n, NULL);
    }
    PMIX_RELEASE(p->quals);
}
static PMIX_CLASS_INSTANCE(pmix_proc_data_t,
                           pmix_object_t,
                           pdcon, pddes);

static pmix_dstor_t *lookup_keyval(pmix_proc_data_t *proc, uint32_t kid,
                                   pmix_info_t *qualifiers, size_t nquals,
                                   pmix_keyindex_t *kidx);
static pmix_proc_data_t *lookup_proc(pmix_hash_table_t *jtable, uint32_t id, bool create);
static void erase_qualifiers(pmix_proc_data_t *proc,
                             uint32_t index);


pmix_status_t pmix_hash_store(pmix_hash_table_t *table,
                              pmix_rank_t rank, pmix_kval_t *kin,
                              pmix_info_t *qualifiers, size_t nquals,
                              pmix_keyindex_t *kidx)
{
    pmix_proc_data_t *proc_data;
    uint32_t kid;
    pmix_dstor_t *hv;
    pmix_regattr_input_t *p;
    pmix_status_t rc;
    pmix_data_array_t *darray;
    pmix_qual_t *qarray;
    size_t n, m = 0;
    pmix_tma_t *const tma = pmix_obj_get_tma(&table->super);
    pmix_keyindex_t *const keyindex = get_keyindex_ptr(kidx);

    pmix_output_verbose(10, pmix_gds_base_framework.framework_output,
                        "%s HASH:STORE:QUAL table %s rank %s key %s",
                        PMIX_NAME_PRINT(&pmix_globals.myid),
                        (NULL == table->ht_label) ? "UNKNOWN" : table->ht_label,
                        PMIX_RANK_PRINT(rank), (NULL == kin) ? "NULL KVAL" : kin->key);

    if (PMIX_UNLIKELY(NULL == kin)) {
        return PMIX_ERR_BAD_PARAM;
    }

    /* lookup the key's corresponding index - this should be
     * moved to the periphery of the PMIx library so we can
     * refer to the key numerically throughout the internals
     */
    p = pmix_hash_lookup_key(UINT32_MAX, kin->key, keyindex);
    if (PMIX_UNLIKELY(NULL == p)) {
        /* we don't know this key */
        pmix_output_verbose(10, pmix_gds_base_framework.framework_output,
                            "%s UNKNOWN KEY: %s",
                            PMIX_NAME_PRINT(&pmix_globals.myid),
                            kin->key);
        return PMIX_ERR_BAD_PARAM;
    }
    kid = p->index;

    /* lookup the proc data object for this proc - create
     * it if we don't already have it */
    if (PMIX_UNLIKELY(NULL == (proc_data = lookup_proc(table, rank, true)))) {
        return PMIX_ERR_NOMEM;
    }

    /* see if we already have this key-value */
    hv = lookup_keyval(proc_data, kid, qualifiers, nquals, keyindex);
    if (NULL != hv) {
        if (PMIX_UNLIKELY(9 < pmix_output_get_verbosity(pmix_gds_base_framework.framework_output))) {
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
                pmix_output_verbose(10, pmix_gds_base_framework.framework_output,
                                    "EQUAL VALUE - IGNORING");
                return PMIX_SUCCESS;
            }
            if (PMIX_UNLIKELY(9 < pmix_output_get_verbosity(pmix_gds_base_framework.framework_output))) {
                // Note that this doesn't have to use a TMA because it is just a
                // temporary value.
                char *tmp;
                tmp = PMIx_Value_string(kin->value);
                pmix_output(0, "%s KEY %s VALUE UPDATING TO: %s",
                            PMIX_NAME_PRINT(&pmix_globals.myid), kin->key, tmp);
                free(tmp);
            }
            pmix_bfrops_base_tma_value_release(&hv->value, tma);
        }
        /* TODO(skg) eventually, we want to eliminate this copy */
        rc = pmix_bfrops_base_tma_copy_value(&hv->value, kin->value, PMIX_VALUE, tma);
        if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
            PMIX_ERROR_LOG(rc);
            return rc;
        }
        return PMIX_SUCCESS;
    }

    /* we don't already have it, so create it */
    hv = pmix_dstor_new_tma(kid, tma);
    if (PMIX_UNLIKELY(NULL == hv)) {
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
            /* zero-initialize so a partially-filled array can be safely
             * released by erase_qualifiers() on an error path below */
            darray->array = (pmix_qual_t*)pmix_tma_calloc(tma, m, sizeof(pmix_qual_t));
            darray->size = m;
            hv->qualindex = pmix_pointer_array_add(proc_data->quals, darray);
            qarray = (pmix_qual_t*)darray->array;
            for (n=0, m=0; n < nquals; n++) {
                if (PMIX_INFO_IS_QUALIFIER(&qualifiers[n])) {
                    p = pmix_hash_lookup_key(UINT32_MAX, qualifiers[n].key, keyindex);
                    if (PMIX_UNLIKELY(NULL == p)) {
                        /* we don't know this key */
                        pmix_output_verbose(10, pmix_gds_base_framework.framework_output,
                                            "%s UNKNOWN KEY: %s",
                                            PMIX_NAME_PRINT(&pmix_globals.myid),
                                            kin->key);
                        erase_qualifiers(proc_data, hv->qualindex);
                        pmix_dstor_release_tma(hv, tma);
                        return PMIX_ERR_BAD_PARAM;
                    }
                    qarray[m].index = p->index;
                    rc = pmix_bfrops_base_tma_copy_value(&qarray[m].value, &qualifiers[n].value, PMIX_VALUE, tma);
                    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
                        PMIX_ERROR_LOG(rc);
                        erase_qualifiers(proc_data, hv->qualindex);
                        pmix_dstor_release_tma(hv, tma);
                        return rc;
                    }
                    ++m;
                }
            }
        }
    }

    /* TODO(skg) eventually, we want to eliminate this copy */
    rc = pmix_bfrops_base_tma_copy_value(&hv->value, kin->value, PMIX_VALUE, tma);
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        PMIX_ERROR_LOG(rc);
        if (UINT32_MAX != hv->qualindex) {
            /* release the associated qualifiers */
            erase_qualifiers(proc_data, hv->qualindex);
        }
        pmix_dstor_release_tma(hv, tma);
        return rc;
    }
    if (PMIX_UNLIKELY(9 < pmix_output_get_verbosity(pmix_gds_base_framework.framework_output))) {
        // Note that this doesn't have to use a TMA because it is just a
        // temporary value.
        char *v = PMIx_Value_string(kin->value);
        pmix_output(0, "%s ADDING KEY %s VALUE %s FOR RANK %s WITH %u QUALS TO TABLE %s",
                    PMIX_NAME_PRINT(&pmix_globals.myid),
                    kin->key, v,
                    PMIX_RANK_PRINT(rank), (unsigned)m,
                    (NULL == table->ht_label) ? "UNKNOWN" : table->ht_label);
        free(v);
    }
    pmix_pointer_array_add(proc_data->data, hv);
    return PMIX_SUCCESS;
}

static pmix_status_t make_copy(pmix_regattr_input_t *p,
                               pmix_dstor_t *hv,
                               pmix_list_t *kvals,
                               pmix_proc_data_t *proc_data,
                               pmix_keyindex_t *const keyindex)
{
    pmix_kval_t *kv;
    pmix_data_array_t *darray;
    pmix_qual_t *quals;
    pmix_info_t *iptr;
    size_t nq, m;

    if (UINT32_MAX != hv->qualindex) {
        /* this is a qualified value - need to return it as such */
        PMIX_KVAL_NEW(kv, PMIX_QUALIFIED_VALUE);
        darray = (pmix_data_array_t*)pmix_pointer_array_get_item(proc_data->quals, hv->qualindex);
        if (NULL == darray) {
            PMIX_ERROR_LOG(PMIX_ERR_NOT_FOUND);
            return PMIX_ERR_NOT_FOUND;
        }
        quals = (pmix_qual_t*)darray->array;
        nq = darray->size;
        PMIX_DATA_ARRAY_CREATE(darray, nq+1, PMIX_INFO);
        iptr = (pmix_info_t*)darray->array;
        /* the first location is the actual value */
        PMIX_LOAD_KEY(iptr[0].key, p->string);
        PMIx_Value_xfer(&iptr[0].value, hv->value);
        /* now add the qualifiers */
        for (m=0; m < nq; m++) {
            p = pmix_hash_lookup_key(quals[m].index, NULL, keyindex);
            if (NULL == p) {
                /* should never happen */
                PMIX_RELEASE(kv);
                PMIX_DATA_ARRAY_FREE(darray);
                return PMIX_ERR_BAD_PARAM;
            }
            PMIX_LOAD_KEY(iptr[m+1].key, p->string);
            PMIx_Value_xfer(&iptr[m+1].value, quals[m].value);
            PMIX_INFO_SET_QUALIFIER(&iptr[m+1]);
        }
        kv->value->type = PMIX_DATA_ARRAY;
        kv->value->data.darray = darray;
        pmix_list_append(kvals, &kv->super);
    } else {
        PMIX_KVAL_NEW(kv, p->string);
        PMIx_Value_xfer(kv->value, hv->value);
        pmix_list_append(kvals, &kv->super);
    }
    return PMIX_SUCCESS;
}

// TODO(skg) We may have to provide a different fetch entry point with different
// semantics for gds shmem to further reduce memory usage. For now this seems to
// work for our current use case.
pmix_status_t pmix_hash_fetch(pmix_hash_table_t *table,
                              pmix_rank_t rank,
                              const char *key,
                              pmix_info_t *qualifiers, size_t nquals,
                              pmix_list_t *kvals,
                              pmix_keyindex_t *kidx)
{
    pmix_status_t rc;
    pmix_proc_data_t *proc_data;
    pmix_dstor_t *hv;
    uint32_t id, kid=UINT32_MAX;
    char *node;
    /* Only ever set on the "NULL != key" path below, and only ever read
     * on that same path - but the two are separated by the search loop,
     * so the compiler cannot see the correlation and reports it as
     * possibly-uninitialized. It is a false positive, and it is also
     * fatal: PMIx builds -Werror under --enable-devel-check, and GCC
     * raises it at -O1, which is what a sanitizer build uses. So the
     * tree could not be built with -fsanitize=address at all.
     *
     * Initializing it is the honest fix rather than a pragma. It costs
     * a store the optimizer removes, and if the correlation is ever
     * broken by a later edit the result is a deterministic NULL
     * dereference in make_copy() rather than a wild pointer. */
    pmix_regattr_input_t *p = NULL;
    int n;
    bool fullsearch = false;
    pmix_keyindex_t *const keyindex = get_keyindex_ptr(kidx);

    pmix_output_verbose(10, pmix_gds_base_framework.framework_output,
                        "%s HASH:FETCH table %s id %s key %s",
                        PMIX_NAME_PRINT(&pmix_globals.myid),
                        (NULL == table->ht_label) ? "UNKNOWN" : table->ht_label,
                        PMIX_RANK_PRINT(rank), (NULL == key) ? "NULL" : key);

    /* - PMIX_RANK_UNDEF should return following statuses
     *     PMIX_ERR_NOT_FOUND | PMIX_SUCCESS
     * - specified rank can return following statuses
     *     PMIX_ERR_NOT_FOUND | PMIX_ERR_NOT_FOUND | PMIX_SUCCESS
     * special logic is basing on these statuses on a client and a server */
    if (PMIX_RANK_UNDEF == rank) {
        rc = pmix_hash_table_get_first_key_uint32(table, &id, (void **) &proc_data,
                                                  (void **) &node);
        if (PMIX_SUCCESS != rc) {
            pmix_output_verbose(10, pmix_gds_base_framework.framework_output,
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
         * refer to the key numerically throughout the internals.
         *
         * Note that we deliberately do NOT register the key if it is
         * missing. A key nobody ever stored cannot be in this table, so
         * "not found" is the right answer - and registering it here
         * would grow the keyindex on every failed fetch. It would also
         * be fatal for a keyindex that lives in a shared-memory segment
         * the caller has mapped read-only. */
        p = pmix_hash_find_key(UINT32_MAX, key, keyindex);
        if (NULL == p) {
            /* this key has never been stored anywhere */
            return PMIX_ERR_NOT_FOUND;
        }
        kid = p->index;
    }

    rc = PMIX_SUCCESS;
    while (PMIX_SUCCESS == rc) {
        proc_data = lookup_proc(table, id, false);
        if (NULL == proc_data) {
            pmix_output_verbose(10, pmix_gds_base_framework.framework_output,
                        "HASH:FETCH[%s:%d] proc data for rank %s not found - key %s",
                        __func__, __LINE__,
                        PMIX_RANK_PRINT(rank), key);
            return PMIX_ERR_NOT_FOUND;
        }

        /* if the key is NULL, then the user wants -all- data
         * put by the specified rank */
        if (NULL == key) {
            /* copy the data */
            for (n=0; n < proc_data->data->size; n++) {
                hv = (pmix_dstor_t*)pmix_pointer_array_get_item(proc_data->data, n);
                if (NULL != hv) {
                    p = pmix_hash_lookup_key(hv->index, NULL, keyindex);
                    if (NULL == p) {
                        return PMIX_ERR_NOT_FOUND;
                    }
                    pmix_output_verbose(10, pmix_gds_base_framework.framework_output,
                                        "%s FETCH NULL LOOKING AT %s",
                                        PMIX_NAME_PRINT(&pmix_globals.myid), p->name);
                    /* if the rank is UNDEF, we ignore reserved keys */
                    if (PMIX_RANK_UNDEF == rank &&
                        PMIX_CHECK_RESERVED_KEY(p->string)) {
                        continue;
                    }
                    if (9 < pmix_output_get_verbosity(pmix_gds_base_framework.framework_output)) {
                        char *_tmp = PMIx_Value_string(hv->value);
                        pmix_output(0, "%s INCLUDE %s VALUE %s FROM TABLE %s FOR RANK %s",
                                        PMIX_NAME_PRINT(&pmix_globals.myid), p->name,
                                        _tmp, (NULL == table->ht_label) ? "UNKNOWN" : table->ht_label,
                                        PMIX_RANK_PRINT(rank));
                        free(_tmp);
                    }
                    rc = make_copy(p, hv, kvals, proc_data, keyindex);
                    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
                        return rc;
                    }
                }
            }
            return PMIX_SUCCESS;
        } else {
            /* find the value from within this data object */
            hv = lookup_keyval(proc_data, kid, qualifiers, nquals, keyindex);
            if (NULL != hv) {
                rc = make_copy(p, hv, kvals, proc_data, keyindex);
                break;
            } else if (!fullsearch) {
                pmix_output_verbose(10, pmix_gds_base_framework.framework_output,
                                    "HASH:FETCH data for key %s not found", key);
                return PMIX_ERR_NOT_FOUND;
            }
        }

        rc = pmix_hash_table_get_next_key_uint32(table, &id, (void **) &proc_data, node,
                                                 (void **) &node);
        if (PMIX_SUCCESS != rc) {
            pmix_output_verbose(10, pmix_gds_base_framework.framework_output,
                                "%s:%d HASH:FETCH data for key %s not found",
                                __func__, __LINE__, key);
            return PMIX_ERR_NOT_FOUND;
        }
    }

    return rc;
}

pmix_status_t pmix_hash_fetch_lowest_rank(pmix_hash_table_t *table,
                                          pmix_rank_t maxrank,
                                          const char *key,
                                          pmix_info_t *qualifiers, size_t nquals,
                                          pmix_list_t *kvals,
                                          pmix_keyindex_t *kidx)
{
    pmix_proc_data_t *proc_data;
    pmix_regattr_input_t *p;
    uint32_t id, kid, best = 0;
    bool found = false;
    char *node;
    pmix_status_t rc;
    pmix_keyindex_t *const keyindex = get_keyindex_ptr(kidx);

    /* "all data for a rank" has no lowest-rank answer to give, and the
     * caller wants every rank's contribution rather than one of them */
    if (PMIX_UNLIKELY(NULL == key)) {
        return PMIX_ERR_BAD_PARAM;
    }

    /* a key nobody ever stored cannot be in this table. Look without
     * registering, for the reasons given in pmix_hash_fetch. */
    p = pmix_hash_find_key(UINT32_MAX, key, keyindex);
    if (NULL == p) {
        return PMIX_ERR_NOT_FOUND;
    }
    kid = p->index;

    /* Walk the entries that are actually present rather than probing
     * every rank the job could have. The two differ by a lot: a client
     * that has not fenced has an empty "local" and "remote" table, and
     * asking each of them for nprocs ranks it does not hold is where a
     * PMIx_Get(NULL, key) at scale spends its time.
     *
     * Entries are visited in bucket order, so the whole table has to be
     * seen before the lowest-ranked match is known. That is the ordering
     * the ascending per-rank loop this replaces produced, and callers
     * depend on it - two ranks can hold the same key. */
    rc = pmix_hash_table_get_first_key_uint32(table, &id, (void **) &proc_data,
                                              (void **) &node);
    while (PMIX_SUCCESS == rc) {
        /* Bound the search to the job's real ranks. This is what keeps
         * the PMIX_RANK_WILDCARD and PMIX_RANK_UNDEF pseudo-rank entries
         * - and the job-level data they carry - out of a per-rank
         * search. A job whose size is not known yet has nothing to
         * match, which is also what the loop this replaces did. */
        if (id < maxrank && (!found || id < best) && NULL != proc_data) {
            if (NULL != lookup_keyval(proc_data, kid, qualifiers, nquals, keyindex)) {
                best = id;
                found = true;
            }
        }
        rc = pmix_hash_table_get_next_key_uint32(table, &id, (void **) &proc_data, node,
                                                 (void **) &node);
    }

    if (!found) {
        return PMIX_ERR_NOT_FOUND;
    }

    /* Build the answer through the ordinary path so the copy and
     * qualified-value handling live in exactly one place. */
    return pmix_hash_fetch(table, (pmix_rank_t) best, key, qualifiers, nquals, kvals, kidx);
}

pmix_status_t pmix_hash_remove_data(pmix_hash_table_t *table,
                                    pmix_rank_t rank, const char *key,
                                    pmix_keyindex_t *kidx)
{
    pmix_status_t rc = PMIX_SUCCESS;
    pmix_proc_data_t *proc_data;
    pmix_dstor_t *d;
    uint32_t id, kid=UINT32_MAX;
    int n;
    char *node;
    pmix_regattr_input_t *p;
    pmix_tma_t *const tma = pmix_obj_get_tma(&table->super);
    pmix_keyindex_t *const keyindex = get_keyindex_ptr(kidx);

    if (NULL != key) {
        /* removing a key we never registered is a no-op, so look
         * without registering - see the note in pmix_hash_fetch */
        p = pmix_hash_find_key(UINT32_MAX, key, keyindex);
        if (PMIX_UNLIKELY(NULL == p)) {
            /* this key has never been stored anywhere */
            return PMIX_ERR_NOT_FOUND;
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
                    for (n=0; n < proc_data->data->size; n++) {
                        d = (pmix_dstor_t*)pmix_pointer_array_get_item(proc_data->data, n);
                        if (NULL != d && kid == d->index) {
                            if (NULL != d->value) {
                                pmix_bfrops_base_tma_value_release(&d->value, tma);
                            }
                            if (UINT32_MAX != d->qualindex) {
                                erase_qualifiers(proc_data, d->qualindex);
                            }
                            pmix_tma_free(tma, d);
                            pmix_pointer_array_set_item(proc_data->data, n, NULL);
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
        for (n=0; n < proc_data->data->size; n++) {
            d = (pmix_dstor_t*)pmix_pointer_array_get_item(proc_data->data, n);
            if (NULL != d) {
                if (NULL != d->value) {
                    pmix_bfrops_base_tma_value_release(&d->value, tma);
                }
                if (UINT32_MAX != d->qualindex) {
                    erase_qualifiers(proc_data, d->qualindex);
                }
                pmix_tma_free(tma, d);
                pmix_pointer_array_set_item(proc_data->data, n, NULL);
            }
        }
        /* remove the proc_data object itself from the jtable */
        pmix_hash_table_remove_value_uint32(table, rank);
        /* cleanup */
        PMIX_RELEASE(proc_data);
        return PMIX_SUCCESS;
    }

    /* remove this item */
    for (n=0; n < proc_data->data->size; n++) {
        d = (pmix_dstor_t*)pmix_pointer_array_get_item(proc_data->data, n);
        if (NULL != d && kid == d->index) {
            if (NULL != d->value) {
                pmix_bfrops_base_tma_value_release(&d->value, tma);
            }
            if (UINT32_MAX != d->qualindex) {
                erase_qualifiers(proc_data, d->qualindex);
            }
            pmix_tma_free(tma, d);
            pmix_pointer_array_set_item(proc_data->data, n, NULL);
            break;
        }
    }

    return PMIX_SUCCESS;
}

/**
 * Find data for a given key in a given pmix_list_t.
 */
static pmix_dstor_t *lookup_keyval(pmix_proc_data_t *proc_data, uint32_t kid,
                                   pmix_info_t *qualifiers, size_t nquals,
                                   pmix_keyindex_t *kidx)
{
    pmix_dstor_t *d;
    pmix_data_array_t *darray;
    pmix_qual_t *qarray;
    pmix_regattr_input_t *p;
    size_t m, numquals = 0, nq, nfound;
    int n, nseen = 0, occupancy;
    pmix_keyindex_t *const keyindex = get_keyindex_ptr(kidx);

    if (NULL != qualifiers) {
        /* count the qualifiers */
        for (m=0; m < nquals; m++) {
            /* if this isn't marked as a qualifier, skip it */
            if (PMIX_INFO_IS_QUALIFIER(&qualifiers[m])) {
                ++numquals;
            }
        }
    }

    /* Stop once every stored entry has been seen rather than at the end
     * of the allocation. The two are far apart: a proc_data's array is
     * created with 128 slots (see pdcon) and a proc typically publishes
     * a handful of keys, so a miss used to scan 128 slots to look at
     * three - and a fetch for an unqualified rank does this once per
     * rank. Holes are possible once anything has been removed, so the
     * bound counts entries seen rather than indices visited. */
    occupancy = pmix_pointer_array_get_occupancy(proc_data->data);
    for (n=0; n < proc_data->data->size && nseen < occupancy; n++) {
        d = (pmix_dstor_t*)pmix_pointer_array_get_item(proc_data->data, n);
        if (NULL == d) {
            continue;
        }
        ++nseen;
        if (kid == d->index) {
            if (0 < numquals) {
                if (UINT32_MAX == d->qualindex) {
                    continue;
                }
                darray = (pmix_data_array_t*)pmix_pointer_array_get_item(proc_data->quals, d->qualindex);
                if (NULL == darray) {
                    continue;
                }
                qarray = (pmix_qual_t*)darray->array;
                nfound = 0;
                /* check the qualifiers */
                for (m=0; m < nquals; m++) {
                    /* if this isn't marked as a qualifier, skip it */
                    if (!PMIX_INFO_IS_QUALIFIER(&qualifiers[m])) {
                        continue;
                    }
                    /* a qualifier key we have never registered cannot
                     * match anything already stored, so look without
                     * registering - see the note in pmix_hash_fetch */
                    p = pmix_hash_find_key(UINT32_MAX, qualifiers[m].key, keyindex);
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
static pmix_proc_data_t *lookup_proc(pmix_hash_table_t *jtable, uint32_t id, bool create)
{
    pmix_proc_data_t *proc_data = NULL;
    pmix_tma_t *const tma = pmix_obj_get_tma(&jtable->super);

    pmix_hash_table_get_value_uint32(jtable, id, (void **) &proc_data);
    if (NULL == proc_data && create) {
        /* The proc clearly exists, so create a data structure for it */
        proc_data = PMIX_NEW(pmix_proc_data_t, tma);
        if (PMIX_UNLIKELY(NULL == proc_data)) {
            return NULL;
        }
        pmix_hash_table_set_value_uint32(jtable, id, proc_data);
    }

    return proc_data;
}

/* Add an entry to the string -> entry side of the index. The entry is
 * borrowed; keyindex->table owns it. */
static void add_to_lookup(pmix_keyindex_t *keyindex,
                          pmix_regattr_input_t *ptr)
{
    if (NULL == keyindex->lookup || NULL == ptr->string) {
        return;
    }
    pmix_hash_table_set_value_ptr(keyindex->lookup, ptr->string,
                                  strlen(ptr->string), ptr);
}

void pmix_hash_keyindex_rebuild(pmix_keyindex_t *kidx)
{
    pmix_keyindex_t *const keyindex = get_keyindex_ptr(kidx);
    pmix_regattr_input_t *ptr;
    int id;

    if (NULL == keyindex->lookup) {
        return;
    }
    pmix_hash_table_remove_all(keyindex->lookup);
    for (id = 0; id < keyindex->table->size; id++) {
        ptr = pmix_pointer_array_get_item(keyindex->table, id);
        if (NULL != ptr) {
            add_to_lookup(keyindex, ptr);
        }
    }
}

void pmix_hash_register_key(uint32_t inid,
                            pmix_regattr_input_t *ptr,
                            pmix_keyindex_t *kidx)
{
    pmix_regattr_input_t *p = NULL;
    pmix_keyindex_t *const keyindex = get_keyindex_ptr(kidx);

    if (UINT32_MAX == inid) {
        /* store the pointer in the array */
        pmix_pointer_array_set_item(keyindex->table, (int)keyindex->next_id, ptr);
        ptr->index = keyindex->next_id;
        keyindex->next_id += 1;
        add_to_lookup(keyindex, ptr);
        return;
    }

    /* check to see if this key was already registered */
    p = pmix_pointer_array_get_item(keyindex->table, inid);
    if (NULL != p) {
        /* already have this one */
        return;
    }
    /* store the pointer in the table */
    pmix_pointer_array_set_item(keyindex->table, inid, ptr);
    add_to_lookup(keyindex, ptr);
}

// skg: Note that one may have to add a TMA wrapper for this call if changes are
// made to how pmix_hash operates. Something for developers to keep in mind.
static pmix_regattr_input_t* lookup_key(uint32_t inid,
                                        const char *key,
                                        pmix_keyindex_t *kidx,
                                        bool register_if_missing)
{
    int id;
    pmix_regattr_input_t *ptr = NULL;
    pmix_keyindex_t *const keyindex = get_keyindex_ptr(kidx);

    if (UINT32_MAX == inid) {
        if (NULL == key) {
            /* they have to give us something! */
            return NULL;
        }
        if (NULL != keyindex->lookup && 0 < strlen(key)) {
            void *found = NULL;
            if (PMIX_SUCCESS == pmix_hash_table_get_value_ptr(keyindex->lookup, key,
                                                              strlen(key), &found)) {
                return (pmix_regattr_input_t*)found;
            }
        } else {
            /* no lookup side available - fall back on scanning the
             * table we do have */
            for (id = 0; id < keyindex->table->size; id++) {
                ptr = pmix_pointer_array_get_item(keyindex->table, id);
                if (NULL != ptr && NULL != ptr->string) {
                    if (0 == strcmp(key, ptr->string)) {
                        return ptr;
                    }
                }
            }
        }

        if (!register_if_missing) {
            /* the caller only wanted to know whether this key is known,
             * so tell them it isn't rather than minting an index for it */
            return NULL;
        }

        /* We didn't find it - register it.
         *
         * Allocate through the keyindex's own allocator, not the heap.
         * A keyindex can live in a shared-memory segment (gds/shmem3
         * keeps one beside the modex data it describes), and every
         * pointer reachable from it is read by other processes that
         * mapped that segment. A heap pointer stored here is valid only
         * in the process that minted it, so a reader dereferences an
         * address that means nothing in its own space. The TMA helpers
         * fall back to plain malloc/strdup when there is no allocator,
         * which is the ordinary process-global case - and this is what
         * keyindex_destruct() has always assumed, since it frees these
         * with pmix_tma_free(). */
        pmix_tma_t *const tma = pmix_obj_get_tma(&keyindex->super);

        ptr = (pmix_regattr_input_t*)pmix_tma_malloc(tma, sizeof(pmix_regattr_input_t));
        if (PMIX_UNLIKELY(NULL == ptr)) {
            return NULL;
        }
        ptr->name = pmix_tma_strdup(tma, key);
        ptr->string = pmix_tma_strdup(tma, key);
        ptr->type = PMIX_UNDEF; // we don't know what type the user will set
        ptr->description = (char**)pmix_tma_malloc(tma, 2 * sizeof(char*));
        if (PMIX_UNLIKELY(NULL == ptr->description)) {
            /* the entry was never handed to the keyindex, so nothing
             * else will ever free it - release what we took here */
            pmix_tma_free(tma, ptr->name);
            pmix_tma_free(tma, ptr->string);
            pmix_tma_free(tma, ptr);
            return NULL;
        }
        ptr->description[0] = pmix_tma_strdup(tma, "USER DEFINED");
        ptr->description[1] = NULL;
        pmix_hash_register_key(UINT32_MAX, ptr, keyindex);
        return ptr;
    }

    /* get the pointer from the table - if it is a reserved key, then
     * it had to be registered at the beginning of time. If it is a
     * non-reserved key, then it had to be registered or else the caller
     * would not have an index to pass us. Thus, the pointer is either
     * found or not - we don't register it if not found. */
    ptr = pmix_pointer_array_get_item(keyindex->table, inid);
    return ptr;
}

pmix_regattr_input_t* pmix_hash_lookup_key(uint32_t inid,
                                           const char *key,
                                           pmix_keyindex_t *kidx)
{
    return lookup_key(inid, key, kidx, true);
}

size_t pmix_hash_sizeof_key_entry(size_t keylen)
{
    /* Mirror the registration branch of lookup_key() above: the attribute
     * record, its two copies of the key string, the two-element
     * description vector and the string it holds, the slot the pointer
     * array takes, and the third copy of the key that the lookup table
     * makes for itself.
     *
     * Three copies of the key string is the part worth knowing, because a
     * caller reserving space for a key index will otherwise reach for
     * PMIX_MAX_KEYLEN and be wrong by more than an order of magnitude:
     * real keys run tens of bytes against a 511-byte maximum. */
    static const size_t description_len = sizeof("USER DEFINED");

    return sizeof(pmix_regattr_input_t)
           + 2 * (keylen + 1)
           + 2 * sizeof(char *)
           + description_len
           + sizeof(void *)
           + keylen;
}

pmix_regattr_input_t* pmix_hash_find_key(uint32_t inid,
                                         const char *key,
                                         pmix_keyindex_t *kidx)
{
    return lookup_key(inid, key, kidx, false);
}

static void erase_qualifiers(pmix_proc_data_t *proc,
                             uint32_t index)
{
    pmix_data_array_t *darray;
    pmix_qual_t *qarray;
    size_t n;
    pmix_tma_t *const tma = pmix_obj_get_tma(&proc->super);

    darray = (pmix_data_array_t*)pmix_pointer_array_get_item(proc->quals, index);
    if (NULL == darray || NULL == darray->array) {
        return;
    }
    qarray = (pmix_qual_t*)darray->array;
    for (n=0; n < darray->size; n++) {
        if (NULL != qarray[n].value) {
            pmix_bfrops_base_tma_value_release(&qarray[n].value, tma);
        }
    }
    pmix_tma_free(tma, qarray);
    pmix_tma_free(tma, darray);
    pmix_pointer_array_set_item(proc->quals, index, NULL);
}
