/*
 * Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2005 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2014-2015 Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2021-2026 Nanook Consulting.  All rights reserved.
 * Copyright (c) 2022      Triad National Security, LLC. All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "src/include/pmix_config.h"

#include <stdlib.h>
#include <string.h>

#include "src/class/pmix_hash_table.h"
#include "src/class/pmix_list.h"
#include "src/util/pmix_output.h"

#include "pmix_common.h"

/*
 * pmix_hash_table_t
 */

#define HASH_MULTIPLIER 31

/*
 * Define the structs that are opaque in the .h
 */

struct pmix_hash_element_t {
    int valid; /* whether this element is valid */
    union {    /* the key, in its various forms */
        uint32_t u32;
        uint64_t u64;
        struct {
            const void *key;
            size_t key_size;
        } ptr;
    } key;
    pmix_tma_t *tma; /* The heap memory manager used for this element. */
    void *value; /* the value */
};
typedef struct pmix_hash_element_t pmix_hash_element_t;

struct pmix_hash_type_methods_t {
    /* Frees any storage associated with the element
     * The value is not owned by the hash table
     * The key,key_size of pointer keys is
     */
    void (*elt_destructor)(pmix_hash_element_t *elt);
    /* Hash the key of the element -- for growing and adjusting-after-removal */
    uint64_t (*hash_elt)(pmix_hash_element_t *elt);
};

/* interact with the class-like mechanism */

static void pmix_hash_table_construct(pmix_hash_table_t *ht);
static void pmix_hash_table_destruct(pmix_hash_table_t *ht);

PMIX_CLASS_INSTANCE(pmix_hash_table_t, pmix_object_t, pmix_hash_table_construct,
                    pmix_hash_table_destruct);

static void pmix_hash_table_construct(pmix_hash_table_t *ht)
{
    /* ht_label is a real, read field, not decoration: src/util/pmix_hash.c
     * prints it as "(NULL == table->ht_label) ? "UNKNOWN" : table->ht_label"
     * at several verbosity-gated sites. Leaving it out of the constructor
     * meant every table that does not set one carried whatever malloc()
     * returned -- back when PMIX_NEW did not zero its allocation -- so
     * that NULL test passed on garbage and the %s dereferenced a wild
     * pointer. gds/shmem3 and the mca base tables are all built this way.
     * Zeroing would answer this particular case now, since NULL is what
     * the test looks for; the assignment stays because that is a
     * coincidence and not a design. */
    ht->ht_label = NULL;
    ht->ht_table = NULL;
    ht->ht_capacity = ht->ht_size = ht->ht_growth_trigger = 0;
    ht->ht_density_numer = ht->ht_density_denom = 0;
    ht->ht_growth_numer = ht->ht_growth_denom = 0;
    ht->ht_type_methods = NULL;
}

static void pmix_hash_table_destruct(pmix_hash_table_t *ht)
{
    pmix_tma_t *const tma = pmix_obj_get_tma(&ht->super);
    pmix_hash_table_remove_all(ht);
    pmix_tma_free(tma, ht->ht_table);
    /* Back to the constructed state. Leaving ht_table dangling with a
     * non-zero ht_capacity meant a second PMIX_DESTRUCT freed it again,
     * and any stray lookup indexed freed memory instead of failing the
     * capacity check. */
    pmix_hash_table_construct(ht);
}

/*
 * Init, etc
 */

/* The density pmix_hash_table_init() applies to a requested element count.
 * Shared with pmix_hash_table_sizeof_storage(), which has to reproduce the
 * same arithmetic for a caller that must reserve the storage up front. */
#define PMIX_HASH_TABLE_DENSITY_NUMER 1
#define PMIX_HASH_TABLE_DENSITY_DENOM 2

static size_t pmix_hash_round_capacity_up(size_t capacity)
{
    /* round up to (1 mod 30) */
    return ((capacity + 29) / 30 * 30 + 1);
}

/* this could be the new init if people wanted a more general API */
/* (that's why it isn't static) */
int /* PMIX_ return code */
pmix_hash_table_init2(pmix_hash_table_t *ht, size_t estimated_max_size, int density_numer,
                      int density_denom, int growth_numer, int growth_denom)
{
    pmix_tma_t *const tma = pmix_obj_get_tma(&ht->super);
    size_t est_capacity, capacity;

    /* Validate the two ratios before dividing by either denominator. This
     * is a PMIX_EXPORT entry point ("this could be the new init if people
     * wanted a more general API"), so it has to survive its arguments:
     *
     *  - a zero density_numer divides by zero right below;
     *  - a zero growth_denom divides by zero in pmix_hash_grow();
     *  - a growth factor that does not actually grow (numer <= denom) leaves
     *    pmix_hash_grow() returning a table no larger than the one it
     *    replaced, so the table eventually fills completely and the
     *    unbounded linear probe in every get/set/remove spins forever.
     *  - a density of 1/1 or worse lets the table fill before the growth
     *    trigger ever fires, with the same result.
     */
    if (0 >= density_numer || 0 >= density_denom || density_numer >= density_denom) {
        return PMIX_ERR_BAD_PARAM;
    }
    if (0 >= growth_numer || 0 >= growth_denom || growth_numer <= growth_denom) {
        return PMIX_ERR_BAD_PARAM;
    }

    est_capacity = estimated_max_size * density_denom / density_numer;
    capacity = pmix_hash_round_capacity_up(est_capacity);
    ht->ht_table = (pmix_hash_element_t *)pmix_tma_calloc(tma, capacity, sizeof(pmix_hash_element_t));
    if (PMIX_UNLIKELY(NULL == ht->ht_table)) {
        return PMIX_ERR_OUT_OF_RESOURCE;
    }
    ht->ht_capacity = capacity;
    ht->ht_density_numer = density_numer;
    ht->ht_density_denom = density_denom;
    ht->ht_growth_numer = growth_numer;
    ht->ht_growth_denom = growth_denom;
    ht->ht_growth_trigger = capacity * density_numer / density_denom;
    ht->ht_type_methods = NULL;
    return PMIX_SUCCESS;
}

int /* PMIX_ return code */
pmix_hash_table_init(pmix_hash_table_t *ht, size_t table_size)
{
    /* default to density of 1/2 and growth of 2/1. The density is named
     * because pmix_hash_table_sizeof_storage() has to apply the same ratio
     * to answer how much this call will allocate. */
    return pmix_hash_table_init2(ht, table_size,
                                 PMIX_HASH_TABLE_DENSITY_NUMER,
                                 PMIX_HASH_TABLE_DENSITY_DENOM, 2, 1);
}

int /* PMIX_ return code */
pmix_hash_table_remove_all(pmix_hash_table_t *ht)
{
    size_t ii;
    for (ii = 0; ii < ht->ht_capacity; ii += 1) {
        pmix_hash_element_t *elt = &ht->ht_table[ii];
        if (elt->valid && ht->ht_type_methods && ht->ht_type_methods->elt_destructor) {
            ht->ht_type_methods->elt_destructor(elt);
        }
        elt->valid = 0;
        elt->value = NULL;
    }
    ht->ht_size = 0;
    /* the tests reuse the hash table for different types after removing all */
    /* so we should allow that by forgetting what type it used to be */
    ht->ht_type_methods = NULL;
    return PMIX_SUCCESS;
}

static int /* PMIX_ return code */
pmix_hash_grow(pmix_hash_table_t *ht)
{
    pmix_tma_t *const tma = pmix_obj_get_tma(&ht->super);
    size_t jj, ii;
    pmix_hash_element_t *old_table;
    pmix_hash_element_t *new_table;
    size_t old_capacity;
    size_t new_capacity;

    old_table = ht->ht_table;
    old_capacity = ht->ht_capacity;

    new_capacity = old_capacity * ht->ht_growth_numer / ht->ht_growth_denom;
    new_capacity = pmix_hash_round_capacity_up(new_capacity);

    new_table = (pmix_hash_element_t *)pmix_tma_calloc(tma, new_capacity, sizeof(new_table[0]));
    if (PMIX_UNLIKELY(NULL == new_table)) {
        return PMIX_ERR_OUT_OF_RESOURCE;
    }

    /* for each element of the old table (indexed by jj), insert it
       into the new table (indexed by ii), using the hash_elt method
       to generically hash an element, then modulo the new capacity,
       and using struct-assignment to copy an old element into its
       place int he new table.  The hash table never owns the value,
       and in the case of ptr keys the old dlements will be blindly
       deleted, so we still own the ptr key storage, just in the new
       table now */
    for (jj = 0; jj < old_capacity; jj += 1) {
        pmix_hash_element_t *old_elt;
        pmix_hash_element_t *new_elt;
        old_elt = &old_table[jj];
        if (old_elt->valid) {
            for (ii = (ht->ht_type_methods->hash_elt(old_elt) % new_capacity);; ii += 1) {
                if (ii == new_capacity) {
                    ii = 0;
                }
                new_elt = &new_table[ii];
                if (!new_elt->valid) {
                    *new_elt = *old_elt;
                    break;
                }
            }
        }
    }
    /* update with the new, free the old, return */
    ht->ht_table = new_table;
    ht->ht_capacity = new_capacity;
    ht->ht_growth_trigger = new_capacity * ht->ht_density_numer / ht->ht_density_denom;
    pmix_tma_free(tma, old_table);
    return PMIX_SUCCESS;
}

/* one of the removal functions has determined which element should be
   removed.  With the help of the type methods this can be generic.
   The important thing is to rehash any valid elements immediately
   following the element-being-removed */
static int /* PMIX_ return code */
pmix_hash_table_remove_elt_at(pmix_hash_table_t *ht, size_t ii)
{
    size_t jj, capacity = ht->ht_capacity;
    pmix_hash_element_t *elts = ht->ht_table;
    pmix_hash_element_t *elt;

    elt = &elts[ii];

    if (!elt->valid) {
        /* huh?  removing a not-valid element? */
        return PMIX_ERROR;
    }

    elt->valid = 0;
    if (ht->ht_type_methods->elt_destructor) {
        ht->ht_type_methods->elt_destructor(elt);
    }

    /* need to possibly re-insert followers because of the now-gap */
    /* E.g., XYyAaCbz.  (where upper is ideal, lower is not)
     * remove A
     * leaving XYy.aCbz. and we need to reconsider aCbz
     * first a gets reinserted where it wants to be: XYya.Cbz.
     * next  C doesn't move:                         XYya.Cbz.
     * then  b gets put where it wants to be:        XYyabC.z.
     * then  z moves down a little:                  XYyabCz..
     * then  . means we're done
     */
    for (ii = ii + 1;; ii += 1) { /* scan immediately following elements */
        if (ii == capacity) {
            ii = 0;
        }
        elt = &elts[ii];
        if (!elt->valid) {
            break; /* done */
        }
        /* rehash it and move it if necessary */
        for (jj = ht->ht_type_methods->hash_elt(elt) % capacity;; jj += 1) {
            if (jj == capacity) {
                jj = 0;
            }
            if (jj == ii) {
                /* already in place, either ideal or best-for-now */
                break;
            } else if (!elts[jj].valid) {
                /* move it down, and invaildate where it came from */
                elts[jj] = elts[ii];
                elts[ii].valid = 0;
                break;
            } else {
                /* still need to find its place */
            }
        }
    }
    ht->ht_size -= 1;
    return PMIX_SUCCESS;
}

/***************************************************************************/

static uint64_t pmix_hash_hash_elt_uint32(pmix_hash_element_t *elt)
{
    return elt->key.u32;
}

static const struct pmix_hash_type_methods_t pmix_hash_type_methods_uint32
    = {NULL, pmix_hash_hash_elt_uint32};

/* Record the key type a table is being used with.
 *
 * Comparing before storing is not micro-optimization: an unconditional
 * store turns every operation into a write to the table's head cache
 * line, including the ones that are otherwise pure reads. The methods
 * are already right in the overwhelmingly common case, so this is a
 * load and a perfectly predicted branch.
 */
static inline void hash_table_set_type(pmix_hash_table_t *ht,
                                       const struct pmix_hash_type_methods_t *methods)
{
    if (methods != ht->ht_type_methods) {
        ht->ht_type_methods = methods;
    }
}

/* The same, for a lookup - which must not write to a table it does not
 * own.
 *
 * &pmix_hash_type_methods_* is the address of a file-scope static in
 * whichever process is executing. For a table in a TMA segment that
 * address means nothing to the peers sharing it, and under ASLR it
 * differs in every one of them. gds/shmem3 has one writer (the server)
 * and many readers, all mapping the same tables, so a reader that
 * stamped its own address into the segment would leave the *writer*
 * dispatching through a foreign pointer the next time
 * pmix_hash_grow() or pmix_hash_table_remove_elt_at() called
 * ht_type_methods->hash_elt().
 *
 * So a lookup on a TMA table leaves the field alone. The owner sets it
 * when it stores, which is the only process that has any business
 * writing there. This is the same exemption the type check makes, for
 * the same reason - it just has to cover the assignment as well as the
 * check.
 */
static inline void hash_table_note_type_for_lookup(pmix_hash_table_t *ht,
                                                   const struct pmix_hash_type_methods_t *methods)
{
    if (NULL != pmix_obj_get_tma(&ht->super)) {
        return;
    }
    hash_table_set_type(ht, methods);
}

int /* PMIX_ return code */
pmix_hash_table_get_value_uint32(pmix_hash_table_t *ht, uint32_t key, void **value)
{
    size_t ii, capacity = ht->ht_capacity;
    pmix_hash_element_t *elt;

    /* An un-init'd table has no slots, and every lookup below starts
     * with "key % capacity" -- a division by zero. This check used to
     * live inside the PMIX_ENABLE_DEBUG block below, so an optimized
     * build took the SIGFPE instead of the error return. */
    if (PMIX_UNLIKELY(0 == capacity)) {
#if PMIX_ENABLE_DEBUG
        pmix_output(0, "pmix_hash_table_get_value_uint32:"
                       "pmix_hash_table_init() has not been called");
#endif
        return PMIX_ERROR;
    }
    /* A table holds exactly one key type at a time, and this check is the
     * only thing that enforces it. It used to sit inside
     * "#if PMIX_ENABLE_DEBUG", so in every build that ships, mixing key
     * types was silent: the assignment below simply retargets
     * ht_type_methods, and from that point on a ptr-keyed table has no
     * elt_destructor (every copied key leaks) while pmix_hash_grow() and
     * pmix_hash_table_remove_elt_at() rehash its elements through
     * key.u32 -- the low bytes of a key pointer -- scattering live
     * entries to slots no later lookup will probe. One pointer compare,
     * on a path that already does a division, does not buy that. Only
     * the diagnostic belongs behind the debug switch.
     *
     * The TMA exemption is real and stays: a table living in a shared
     * segment may carry method pointers another process installed, which
     * mean nothing in this address space. */
    if (PMIX_UNLIKELY(NULL == pmix_obj_get_tma(&ht->super) && NULL != ht->ht_type_methods
                      && &pmix_hash_type_methods_uint32 != ht->ht_type_methods)) {
#if PMIX_ENABLE_DEBUG
        pmix_output(0, "pmix_hash_table_get_value_uint32:"
                       "hash table is for a different key type");
#endif
        return PMIX_ERROR;
    }

    hash_table_note_type_for_lookup(ht, &pmix_hash_type_methods_uint32);
    for (ii = key % capacity;; ii += 1) {
        if (ii == capacity) {
            ii = 0;
        }
        elt = &ht->ht_table[ii];
        if (!elt->valid) {
            return PMIX_ERR_NOT_FOUND;
        } else if (elt->key.u32 == key) {
            *value = elt->value;
            return PMIX_SUCCESS;
        } else {
            /* keep looking */
        }
    }
}

int /* PMIX_ return code */
pmix_hash_table_set_value_uint32(pmix_hash_table_t *ht, uint32_t key, void *value)
{
    int rc;
    size_t ii, capacity = ht->ht_capacity;
    pmix_hash_element_t *elt;
    pmix_tma_t *const tma = pmix_obj_get_tma(&ht->super);

    /* An un-init'd table has no slots, and every lookup below starts
     * with "key % capacity" -- a division by zero. This check used to
     * live inside the PMIX_ENABLE_DEBUG block below, so an optimized
     * build took the SIGFPE instead of the error return. */
    if (PMIX_UNLIKELY(0 == capacity)) {
#if PMIX_ENABLE_DEBUG
        pmix_output(0, "pmix_hash_table_set_value_uint32:"
                       "pmix_hash_table_init() has not been called");
#endif
        return PMIX_ERR_BAD_PARAM;
    }
    /* One key type per table; see pmix_hash_table_get_value_uint32() for
     * why this check is not inside "#if PMIX_ENABLE_DEBUG" and why the
     * TMA exemption is. */
    if (PMIX_UNLIKELY(NULL == pmix_obj_get_tma(&ht->super) && NULL != ht->ht_type_methods
                      && &pmix_hash_type_methods_uint32 != ht->ht_type_methods)) {
#if PMIX_ENABLE_DEBUG
        pmix_output(0, "pmix_hash_table_set_value_uint32:"
                       "hash table is for a different key type");
#endif
        return PMIX_ERROR;
    }

    hash_table_set_type(ht, &pmix_hash_type_methods_uint32);
    for (ii = key % capacity;; ii += 1) {
        if (ii == capacity) {
            ii = 0;
        }
        elt = &ht->ht_table[ii];
        if (!elt->valid) {
            /* new entry */
            elt->key.u32 = key;
            elt->value = value;
            elt->valid = 1;
            elt->tma = tma;
            ht->ht_size += 1;
            if (ht->ht_size >= ht->ht_growth_trigger) {
                if (PMIX_SUCCESS != (rc = pmix_hash_grow(ht))) {
                    return rc;
                }
            }
            return PMIX_SUCCESS;
        } else if (elt->key.u32 == key) {
            /* replace existing element */
            elt->value = value;
            return PMIX_SUCCESS;
        } else {
            /* keep looking */
        }
    }
}

int pmix_hash_table_remove_value_uint32(pmix_hash_table_t *ht, uint32_t key)
{
    size_t ii, capacity = ht->ht_capacity;

    /* An un-init'd table has no slots, and every lookup below starts
     * with "key % capacity" -- a division by zero. This check used to
     * live inside the PMIX_ENABLE_DEBUG block below, so an optimized
     * build took the SIGFPE instead of the error return. */
    if (PMIX_UNLIKELY(0 == capacity)) {
#if PMIX_ENABLE_DEBUG
        pmix_output(0, "pmix_hash_table_get_value_uint32:"
                       "pmix_hash_table_init() has not been called");
#endif
        return PMIX_ERROR;
    }
    /* One key type per table; see pmix_hash_table_get_value_uint32() for
     * why this check is not inside "#if PMIX_ENABLE_DEBUG" and why the
     * TMA exemption is. */
    if (PMIX_UNLIKELY(NULL == pmix_obj_get_tma(&ht->super) && NULL != ht->ht_type_methods
                      && &pmix_hash_type_methods_uint32 != ht->ht_type_methods)) {
#if PMIX_ENABLE_DEBUG
        pmix_output(0, "pmix_hash_table_remove_value_uint32:"
                       "hash table is for a different key type");
#endif
        return PMIX_ERROR;
    }

    hash_table_set_type(ht, &pmix_hash_type_methods_uint32);
    for (ii = key % capacity;; ii += 1) {
        pmix_hash_element_t *elt;
        if (ii == capacity) {
            ii = 0;
        }
        elt = &ht->ht_table[ii];
        if (!elt->valid) {
            return PMIX_ERR_NOT_FOUND;
        } else if (elt->key.u32 == key) {
            return pmix_hash_table_remove_elt_at(ht, ii);
        } else {
            /* keep looking */
        }
    }
}

/***************************************************************************/

static uint64_t pmix_hash_hash_elt_uint64(pmix_hash_element_t *elt)
{
    return elt->key.u64;
}

static const struct pmix_hash_type_methods_t pmix_hash_type_methods_uint64
    = {NULL, pmix_hash_hash_elt_uint64};

int /* PMIX_ return code */
pmix_hash_table_get_value_uint64(pmix_hash_table_t *ht, uint64_t key, void **value)
{
    size_t ii;
    size_t capacity = ht->ht_capacity;
    pmix_hash_element_t *elt;

    /* An un-init'd table has no slots, and every lookup below starts
     * with "key % capacity" -- a division by zero. This check used to
     * live inside the PMIX_ENABLE_DEBUG block below, so an optimized
     * build took the SIGFPE instead of the error return. */
    if (PMIX_UNLIKELY(0 == capacity)) {
#if PMIX_ENABLE_DEBUG
        pmix_output(0, "pmix_hash_table_get_value_uint64:"
                       "pmix_hash_table_init() has not been called");
#endif
        return PMIX_ERROR;
    }
    /* One key type per table; see pmix_hash_table_get_value_uint32() for
     * why this check is not inside "#if PMIX_ENABLE_DEBUG" and why the
     * TMA exemption is. */
    if (PMIX_UNLIKELY(NULL == pmix_obj_get_tma(&ht->super) && NULL != ht->ht_type_methods
                      && &pmix_hash_type_methods_uint64 != ht->ht_type_methods)) {
#if PMIX_ENABLE_DEBUG
        pmix_output(0, "pmix_hash_table_get_value_uint64:"
                       "hash table is for a different key type");
#endif
        return PMIX_ERROR;
    }

    hash_table_note_type_for_lookup(ht, &pmix_hash_type_methods_uint64);
    for (ii = key % capacity;; ii += 1) {
        if (ii == capacity) {
            ii = 0;
        }
        elt = &ht->ht_table[ii];
        if (!elt->valid) {
            return PMIX_ERR_NOT_FOUND;
        } else if (elt->key.u64 == key) {
            *value = elt->value;
            return PMIX_SUCCESS;
        } else {
            /* keep looking */
        }
    }
}

int /* PMIX_ return code */
pmix_hash_table_set_value_uint64(pmix_hash_table_t *ht, uint64_t key, void *value)
{
    int rc;
    size_t ii, capacity = ht->ht_capacity;
    pmix_hash_element_t *elt;
    pmix_tma_t *const tma = pmix_obj_get_tma(&ht->super);

    /* An un-init'd table has no slots, and every lookup below starts
     * with "key % capacity" -- a division by zero. This check used to
     * live inside the PMIX_ENABLE_DEBUG block below, so an optimized
     * build took the SIGFPE instead of the error return. */
    if (PMIX_UNLIKELY(0 == capacity)) {
#if PMIX_ENABLE_DEBUG
        pmix_output(0, "pmix_hash_table_set_value_uint64:"
                       "pmix_hash_table_init() has not been called");
#endif
        return PMIX_ERR_BAD_PARAM;
    }
    /* One key type per table; see pmix_hash_table_get_value_uint32() for
     * why this check is not inside "#if PMIX_ENABLE_DEBUG" and why the
     * TMA exemption is. */
    if (PMIX_UNLIKELY(NULL == pmix_obj_get_tma(&ht->super) && NULL != ht->ht_type_methods
                      && &pmix_hash_type_methods_uint64 != ht->ht_type_methods)) {
#if PMIX_ENABLE_DEBUG
        pmix_output(0, "pmix_hash_table_set_value_uint64:"
                       "hash table is for a different key type");
#endif
        return PMIX_ERROR;
    }

    hash_table_set_type(ht, &pmix_hash_type_methods_uint64);
    for (ii = key % capacity;; ii += 1) {
        if (ii == capacity) {
            ii = 0;
        }
        elt = &ht->ht_table[ii];
        if (!elt->valid) {
            /* new entry */
            elt->key.u64 = key;
            elt->value = value;
            elt->valid = 1;
            elt->tma = tma;
            ht->ht_size += 1;
            if (ht->ht_size >= ht->ht_growth_trigger) {
                if (PMIX_SUCCESS != (rc = pmix_hash_grow(ht))) {
                    return rc;
                }
            }
            return PMIX_SUCCESS;
        } else if (elt->key.u64 == key) {
            elt->value = value;
            return PMIX_SUCCESS;
        } else {
            /* keep looking */
        }
    }
}

int /* PMIX_ return code */
pmix_hash_table_remove_value_uint64(pmix_hash_table_t *ht, uint64_t key)
{
    size_t ii, capacity = ht->ht_capacity;

    /* An un-init'd table has no slots, and every lookup below starts
     * with "key % capacity" -- a division by zero. This check used to
     * live inside the PMIX_ENABLE_DEBUG block below, so an optimized
     * build took the SIGFPE instead of the error return. */
    if (PMIX_UNLIKELY(0 == capacity)) {
#if PMIX_ENABLE_DEBUG
        pmix_output(0, "pmix_hash_table_get_value_uint64:"
                       "pmix_hash_table_init() has not been called");
#endif
        return PMIX_ERROR;
    }
    /* One key type per table; see pmix_hash_table_get_value_uint32() for
     * why this check is not inside "#if PMIX_ENABLE_DEBUG" and why the
     * TMA exemption is. */
    if (PMIX_UNLIKELY(NULL == pmix_obj_get_tma(&ht->super) && NULL != ht->ht_type_methods
                      && &pmix_hash_type_methods_uint64 != ht->ht_type_methods)) {
#if PMIX_ENABLE_DEBUG
        pmix_output(0, "pmix_hash_table_remove_value_uint64:"
                       "hash table is for a different key type");
#endif
        return PMIX_ERROR;
    }

    hash_table_set_type(ht, &pmix_hash_type_methods_uint64);
    for (ii = key % capacity;; ii += 1) {
        pmix_hash_element_t *elt;
        if (ii == capacity) {
            ii = 0;
        }
        elt = &ht->ht_table[ii];
        if (!elt->valid) {
            return PMIX_ERR_NOT_FOUND;
        } else if (elt->key.u64 == key) {
            return pmix_hash_table_remove_elt_at(ht, ii);
        } else {
            /* keep looking */
        }
    }
}

/***************************************************************************/

/* helper function used in several places */
static uint64_t pmix_hash_hash_key_ptr(const void *key, size_t key_size)
{
    uint64_t hash;
    const unsigned char *scanner;
    size_t ii;

    hash = 0;
    scanner = (const unsigned char *) key;
    for (ii = 0; ii < key_size; ii += 1) {
        hash = HASH_MULTIPLIER * hash + *scanner++;
    }
    return hash;
}

/* ptr methods */

static void pmix_hash_destruct_elt_ptr(pmix_hash_element_t *elt)
{
    elt->key.ptr.key_size = 0;
    void *key = (void *) elt->key.ptr.key; /* cast away const so we can free it */
    if (NULL != key) {
        elt->key.ptr.key = NULL;
        pmix_tma_free(elt->tma, key);
    }
}

static uint64_t pmix_hash_hash_elt_ptr(pmix_hash_element_t *elt)
{
    return pmix_hash_hash_key_ptr(elt->key.ptr.key, elt->key.ptr.key_size);
}

static const struct pmix_hash_type_methods_t pmix_hash_type_methods_ptr
    = {pmix_hash_destruct_elt_ptr, pmix_hash_hash_elt_ptr};

int /* PMIX_ return code */
pmix_hash_table_get_value_ptr(pmix_hash_table_t *ht, const void *key, size_t key_size, void **value)
{
    size_t ii, capacity = ht->ht_capacity;
    pmix_hash_element_t *elt;

    /* An un-init'd table has no slots, and every lookup below starts
     * with "key % capacity" -- a division by zero. This check used to
     * live inside the PMIX_ENABLE_DEBUG block below, so an optimized
     * build took the SIGFPE instead of the error return. */
    if (PMIX_UNLIKELY(0 == capacity)) {
#if PMIX_ENABLE_DEBUG
        pmix_output(0, "pmix_hash_table_get_value_ptr:"
                       "pmix_hash_table_init() has not been called");
#endif
        return PMIX_ERROR;
    }
    /* The ptr family hashes and memcmp's the caller's key bytes, and the
     * setter memcpy's them into storage it owns. A NULL key walks into
     * pmix_hash_hash_key_ptr() and dereferences it; a zero key_size makes
     * every key compare equal to every other (memcmp of 0 bytes is 0) and
     * asks the allocator for a 0-byte block, whose answer is
     * implementation-defined -- NULL on some platforms, which this code
     * would report as an out-of-memory that never happened. Neither is a
     * usable key, so say so. */
    if (PMIX_UNLIKELY(NULL == key || 0 == key_size)) {
        return PMIX_ERR_BAD_PARAM;
    }
    /* One key type per table; see pmix_hash_table_get_value_uint32() for
     * why this check is not inside "#if PMIX_ENABLE_DEBUG" and why the
     * TMA exemption is. */
    if (PMIX_UNLIKELY(NULL == pmix_obj_get_tma(&ht->super) && NULL != ht->ht_type_methods
                      && &pmix_hash_type_methods_ptr != ht->ht_type_methods)) {
#if PMIX_ENABLE_DEBUG
        pmix_output(0, "pmix_hash_table_get_value_ptr:"
                       "hash table is for a different key type");
#endif
        return PMIX_ERROR;
    }

    hash_table_note_type_for_lookup(ht, &pmix_hash_type_methods_ptr);
    for (ii = pmix_hash_hash_key_ptr(key, key_size) % capacity;; ii += 1) {
        if (ii == capacity) {
            ii = 0;
        }
        elt = &ht->ht_table[ii];
        if (!elt->valid) {
            return PMIX_ERR_NOT_FOUND;
        } else if (elt->key.ptr.key_size == key_size
                   && 0 == memcmp(elt->key.ptr.key, key, key_size)) {
            *value = elt->value;
            return PMIX_SUCCESS;
        } else {
            /* keep going */
        }
    }
}

int /* PMIX_ return code */
pmix_hash_table_set_value_ptr(pmix_hash_table_t *ht, const void *key, size_t key_size, void *value)
{
    int rc;
    size_t ii, capacity = ht->ht_capacity;
    pmix_hash_element_t *elt;
    pmix_tma_t *const tma = pmix_obj_get_tma(&ht->super);

    /* An un-init'd table has no slots, and every lookup below starts
     * with "key % capacity" -- a division by zero. This check used to
     * live inside the PMIX_ENABLE_DEBUG block below, so an optimized
     * build took the SIGFPE instead of the error return. */
    if (PMIX_UNLIKELY(0 == capacity)) {
#if PMIX_ENABLE_DEBUG
        pmix_output(0, "pmix_hash_table_set_value_ptr:"
                       "pmix_hash_table_init() has not been called");
#endif
        return PMIX_ERR_BAD_PARAM;
    }
    if (PMIX_UNLIKELY(NULL == key || 0 == key_size)) {
        return PMIX_ERR_BAD_PARAM;
    }
    /* One key type per table; see pmix_hash_table_get_value_uint32() for
     * why this check is not inside "#if PMIX_ENABLE_DEBUG" and why the
     * TMA exemption is. */
    if (PMIX_UNLIKELY(NULL == pmix_obj_get_tma(&ht->super) && NULL != ht->ht_type_methods
                      && &pmix_hash_type_methods_ptr != ht->ht_type_methods)) {
#if PMIX_ENABLE_DEBUG
        pmix_output(0, "pmix_hash_table_set_value_ptr:"
                       "hash table is for a different key type");
#endif
        return PMIX_ERROR;
    }

    hash_table_set_type(ht, &pmix_hash_type_methods_ptr);
    for (ii = pmix_hash_hash_key_ptr(key, key_size) % capacity;; ii += 1) {
        if (ii == capacity) {
            ii = 0;
        }
        elt = &ht->ht_table[ii];
        if (!elt->valid) {
            /* new entry */
            void *key_local = pmix_tma_malloc(tma, key_size);
            if (NULL == key_local) {
                return PMIX_ERR_OUT_OF_RESOURCE;
            }
            memcpy(key_local, key, key_size);
            elt->key.ptr.key = key_local;
            elt->key.ptr.key_size = key_size;
            elt->value = value;
            elt->valid = 1;
            elt->tma = tma;
            ht->ht_size += 1;
            if (ht->ht_size >= ht->ht_growth_trigger) {
                if (PMIX_SUCCESS != (rc = pmix_hash_grow(ht))) {
                    return rc;
                }
            }
            return PMIX_SUCCESS;
        } else if (elt->key.ptr.key_size == key_size
                   && 0 == memcmp(elt->key.ptr.key, key, key_size)) {
            /* replace existing value */
            elt->value = value;
            return PMIX_SUCCESS;
        } else {
            /* keep looking */
        }
    }
}

int /* PMIX_ return code */
pmix_hash_table_remove_value_ptr(pmix_hash_table_t *ht, const void *key, size_t key_size)
{
    size_t ii, capacity = ht->ht_capacity;

    /* An un-init'd table has no slots, and every lookup below starts
     * with "key % capacity" -- a division by zero. This check used to
     * live inside the PMIX_ENABLE_DEBUG block below, so an optimized
     * build took the SIGFPE instead of the error return. */
    if (PMIX_UNLIKELY(0 == capacity)) {
#if PMIX_ENABLE_DEBUG
        pmix_output(0, "pmix_hash_table_get_value_ptr:"
                       "pmix_hash_table_init() has not been called");
#endif
        return PMIX_ERROR;
    }
    if (PMIX_UNLIKELY(NULL == key || 0 == key_size)) {
        return PMIX_ERR_BAD_PARAM;
    }
    /* One key type per table; see pmix_hash_table_get_value_uint32() for
     * why this check is not inside "#if PMIX_ENABLE_DEBUG" and why the
     * TMA exemption is. */
    if (PMIX_UNLIKELY(NULL == pmix_obj_get_tma(&ht->super) && NULL != ht->ht_type_methods
                      && &pmix_hash_type_methods_ptr != ht->ht_type_methods)) {
#if PMIX_ENABLE_DEBUG
        pmix_output(0, "pmix_hash_table_remove_value_ptr:"
                       "hash table is for a different key type");
#endif
        return PMIX_ERROR;
    }

    hash_table_set_type(ht, &pmix_hash_type_methods_ptr);
    for (ii = pmix_hash_hash_key_ptr(key, key_size) % capacity;; ii += 1) {
        pmix_hash_element_t *elt;
        if (ii == capacity) {
            ii = 0;
        }
        elt = &ht->ht_table[ii];
        if (!elt->valid) {
            return PMIX_ERR_NOT_FOUND;
        } else if (elt->key.ptr.key_size == key_size
                   && 0 == memcmp(elt->key.ptr.key, key, key_size)) {
            return pmix_hash_table_remove_elt_at(ht, ii);
        } else {
            /* keep looking */
        }
    }
}

/***************************************************************************/
/* Traversals */

static int /* PMIX_ return code */
pmix_hash_table_get_next_elt(pmix_hash_table_t *ht,
                             pmix_hash_element_t *prev_elt, /* NULL means find first */
                             pmix_hash_element_t **next_elt)
{
    pmix_hash_element_t *elts = ht->ht_table;
    size_t ii, capacity = ht->ht_capacity;

    for (ii = (NULL == prev_elt ? 0 : (prev_elt - elts) + 1); ii < capacity; ii += 1) {
        pmix_hash_element_t *elt = &elts[ii];
        if (elt->valid) {
            *next_elt = elt;
            return PMIX_SUCCESS;
        }
    }
    return PMIX_ERROR;
}

int /* PMIX_ return code */
pmix_hash_table_get_first_key_uint32(pmix_hash_table_t *ht, uint32_t *key, void **value,
                                     void **node)
{
    return pmix_hash_table_get_next_key_uint32(ht, key, value, NULL, node);
}

int /* PMIX_ return code */
pmix_hash_table_get_next_key_uint32(pmix_hash_table_t *ht, uint32_t *key, void **value,
                                    void *in_node, void **out_node)
{
    pmix_hash_element_t *elt;
    if (PMIX_SUCCESS == pmix_hash_table_get_next_elt(ht, (pmix_hash_element_t *) in_node, &elt)) {
        *key = elt->key.u32;
        *value = elt->value;
        *out_node = elt;
        return PMIX_SUCCESS;
    }
    return PMIX_ERROR;
}

int /* PMIX_ return code */
pmix_hash_table_get_first_key_ptr(pmix_hash_table_t *ht, void **key, size_t *key_size, void **value,
                                  void **node)
{
    return pmix_hash_table_get_next_key_ptr(ht, key, key_size, value, NULL, node);
}

int /* PMIX_ return code */
pmix_hash_table_get_next_key_ptr(pmix_hash_table_t *ht, void **key, size_t *key_size, void **value,
                                 void *in_node, void **out_node)
{
    pmix_hash_element_t *elt;
    if (PMIX_SUCCESS == pmix_hash_table_get_next_elt(ht, (pmix_hash_element_t *) in_node, &elt)) {
        *key = (void *) elt->key.ptr.key;
        *key_size = elt->key.ptr.key_size;
        *value = elt->value;
        *out_node = elt;
        return PMIX_SUCCESS;
    }
    return PMIX_ERROR;
}

int /* PMIX_ return code */
pmix_hash_table_get_first_key_uint64(pmix_hash_table_t *ht, uint64_t *key, void **value,
                                     void **node)
{
    return pmix_hash_table_get_next_key_uint64(ht, key, value, NULL, node);
}

int /* PMIX_ return code */
pmix_hash_table_get_next_key_uint64(pmix_hash_table_t *ht, uint64_t *key, void **value,
                                    void *in_node, void **out_node)
{
    pmix_hash_element_t *elt;
    if (PMIX_SUCCESS == pmix_hash_table_get_next_elt(ht, (pmix_hash_element_t *) in_node, &elt)) {
        *key = elt->key.u64;
        *value = elt->value;
        *out_node = elt;
        return PMIX_SUCCESS;
    }
    return PMIX_ERROR;
}

size_t
pmix_hash_table_sizeof_hash_element(void)
{
    return sizeof(pmix_hash_element_t);
}

size_t
pmix_hash_table_sizeof_storage(size_t nelements)
{
    /* Mirror pmix_hash_table_init()/init2(): the request is scaled by the
     * default density ratio and then rounded up. Keep the two in step -
     * a caller reserving storage from this and then calling init() has no
     * other way to learn the answer, and being short is not a slow path
     * for the bump allocator in gds/shmem3, it is an abort. */
    const size_t est_capacity = nelements * PMIX_HASH_TABLE_DENSITY_DENOM
                                / PMIX_HASH_TABLE_DENSITY_NUMER;

    return pmix_hash_round_capacity_up(est_capacity) * sizeof(pmix_hash_element_t);
}

/* there was/is no traversal for the ptr case; it would go here */
/* interact with the class-like mechanism */
