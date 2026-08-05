/*
 * Copyright (c) 2010      Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2012      Los Alamos National Security, Inc. All rights reserved.
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2015      Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2021-2026 Nanook Consulting.  All rights reserved.
 * Copyright (c) 2023      Triad National Security, LLC. All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#ifndef PMIX_HASH_H
#define PMIX_HASH_H

#include "src/include/pmix_config.h"

#include "src/class/pmix_hash_table.h"
#include "src/include/pmix_globals.h"
#include "src/mca/bfrops/bfrops_types.h"

BEGIN_C_DECLS

/* store a value in the given hash table for the specified
 * rank index.*/
PMIX_EXPORT pmix_status_t pmix_hash_store(pmix_hash_table_t *table,
                                          pmix_rank_t rank, pmix_kval_t *kin,
                                          pmix_info_t *qualifiers, size_t nquals,
                                          pmix_keyindex_t *kidx);

/* Fetch the value for a specified key and rank from within
 * the given hash_table */
/* Number of key slots initially allocated per process in the
 * datastore, and the increment its array grows by. Registered as the
 * pmix_hash_proc_alloc MCA parameter; see the note at its definition
 * in pmix_hash.c for what the trade actually is. */
PMIX_EXPORT extern int pmix_hash_proc_alloc;

PMIX_EXPORT pmix_status_t pmix_hash_fetch(pmix_hash_table_t *table, pmix_rank_t rank,
                                          const char *key,
                                          pmix_info_t *qualifiers, size_t nquals,
                                          pmix_list_t *kvals,
                                          pmix_keyindex_t *kidx);

/* Fetch "key" from the lowest-numbered rank below "maxrank" that holds
 * it - the answer a caller wants when it knows the key but not which
 * rank published it.
 *
 * This is the searching form of pmix_hash_fetch() and exists so that
 * such a search costs one pass over the entries the table actually
 * holds, rather than one full lookup per rank the job could have.
 *
 * "maxrank" bounds the search to real ranks, which is what keeps the
 * PMIX_RANK_WILDCARD and PMIX_RANK_UNDEF pseudo-rank entries - and the
 * job-level data they carry - from answering a per-rank question. Pass
 * the namespace's proc count; a zero matches nothing.
 *
 * "key" must not be NULL: "all data for a rank" has no lowest-rank
 * answer, and a caller wanting every rank's contribution wants
 * pmix_hash_fetch() per rank instead. NULL returns PMIX_ERR_BAD_PARAM.
 */
PMIX_EXPORT pmix_status_t pmix_hash_fetch_lowest_rank(pmix_hash_table_t *table,
                                                      pmix_rank_t maxrank,
                                                      const char *key,
                                                      pmix_info_t *qualifiers, size_t nquals,
                                                      pmix_list_t *kvals,
                                                      pmix_keyindex_t *kidx);

/* remove the specified key-value from the given hash_table.
 * A NULL key will result in removal of all data for the
 * given rank. A rank of PMIX_RANK_WILDCARD indicates that
 * the specified key  is to be removed from the data for all
 * ranks in the table. Combining key=NULL with rank=PMIX_RANK_WILDCARD
 * will therefore result in removal of all data from the
 * table */
PMIX_EXPORT pmix_status_t pmix_hash_remove_data(pmix_hash_table_t *table,
                                                pmix_rank_t rank,
                                                const char *key,
                                                pmix_keyindex_t *kidx);

PMIX_EXPORT void pmix_hash_register_key(uint32_t inid,
                                        pmix_regattr_input_t *ptr,
                                        pmix_keyindex_t *kidx);

/* Repopulate a keyindex's string -> entry lookup from its pointer
 * array. Call this after modifying the array directly - adding,
 * removing, or renumbering entries without going through
 * pmix_hash_register_key() - or the two sides will disagree. */
PMIX_EXPORT void pmix_hash_keyindex_rebuild(pmix_keyindex_t *kidx);

/* Translate a key to its keyindex entry, registering the key (and thus
 * assigning it an index) if it is not already known. This is the form
 * the store path wants: storing a value is precisely the point at which
 * a new key legitimately acquires an index. */
PMIX_EXPORT pmix_regattr_input_t* pmix_hash_lookup_key(uint32_t inid,
                                                       const char *key,
                                                       pmix_keyindex_t *kidx);

/* Same translation, but return NULL instead of registering an unknown
 * key. This is the form every read-only path wants. Registering on a
 * lookup would grow the keyindex without bound - one entry for every
 * key anyone ever asked for and did not find - and would be fatal for a
 * keyindex living in a shared-memory segment that the caller has mapped
 * read-only. A key that was never stored cannot be found anyway, so the
 * answer is the same either way. */
PMIX_EXPORT pmix_regattr_input_t* pmix_hash_find_key(uint32_t inid,
                                                     const char *key,
                                                     pmix_keyindex_t *kidx);

#define PMIX_HASH_TRACE_KEY_ACTUAL(s, r, k, id, tbl, v)                     \
do {                                                                        \
    const char *_k;                                                         \
    char *_v;                                                               \
    pmix_regattr_input_t *_p;                                               \
    if (NULL == (k) && UINT32_MAX != id) {                                  \
        _p = pmix_hash_lookup_key((id), NULL, NULL);                        \
        if (NULL == _p) {                                                   \
            _k = "KEY NOT FOUND";                                           \
        } else {                                                            \
            _k = _p->string;                                                \
        }                                                                   \
    } else if (NULL != (k)) {                                               \
        _k = (k);                                                           \
    } else {                                                                \
        _k = NULL;                                                          \
    }                                                                       \
    if (NULL != _k) {                                                       \
        if (0 == strcmp((s), _k)) {                                         \
            if (NULL != (v)) {                                              \
                _v = PMIx_Value_string(v);                                  \
            } else {                                                        \
                _v = strdup("\tValue is NULL");                             \
            }                                                               \
            pmix_output(0, "[%s:%s:%d] %s: Rank %s Key %s\n%s\n\n",         \
                        __FILE__, __func__, __LINE__,                       \
                        (tbl)->ht_label, PMIX_RANK_PRINT(r),                \
                        PMIx_Get_attribute_name(_k), _v);                   \
            free(_v);                                                       \
        }                                                                   \
    }                                                                       \
} while(0)

#define PMIX_HASH_TRACE_KEY(c, r, s, k, id, v, tbl)             \
do {                                                            \
    if (0 == strcasecmp(c, "SERVER") &&                         \
        PMIX_PEER_IS_SERVER(pmix_globals.mypeer)) {             \
        PMIX_HASH_TRACE_KEY_ACTUAL(s, r, k, id, tbl, v);           \
    } else if (0 == strcasecmp(c, "CLIENT") &&                  \
               !PMIX_PEER_IS_SERVER(pmix_globals.mypeer)) {     \
        PMIX_HASH_TRACE_KEY_ACTUAL(s, r, k, id, tbl, v);        \
    }                                                           \
} while (0)

END_C_DECLS

#endif /* PMIX_HASH_H */
