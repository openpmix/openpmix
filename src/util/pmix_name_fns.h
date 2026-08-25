/*
 * Copyright (c) 2004-2008 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2011 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2010      Oracle and/or its affiliates.  All rights reserved.
 * Copyright (c) 2014-2016 Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2018-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2021-2022 Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#ifndef _PMIX_NAME_FNS_H_
#define _PMIX_NAME_FNS_H_

#include "src/include/pmix_config.h"

#ifdef HAVE_STDINT_H
#    include <stdint.h>
#endif

#include "pmix_common.h"

BEGIN_C_DECLS

/* define an internal-only process name that has
 * a dynamically-sized nspace field to save memory */
typedef struct {
    char *nspace;
    pmix_rank_t rank;
} pmix_name_t;

#define PMIX_CHECK_NAMES(a, b) \
    (PMIX_CHECK_NSPACE((a)->nspace, (b)->nspace) && ((a)->rank == (b)->rank || (PMIX_RANK_WILDCARD == (a)->rank || PMIX_RANK_WILDCARD == (b)->rank)))

/* Useful defines to print name args in output messages.
 *
 * All three answer out of a small ring of buffers private to the
 * calling thread, so the result must never be freed and stays valid
 * only until the ring wraps around to it again.  The ring holds 16
 * buffers, but a name print spends *two* of them - one for the rank,
 * one for the assembled "[nspace,rank]" - so about eight name prints
 * may be live at once in a single statement.  Nothing in the tree
 * comes close (two per statement is the most any call site uses), and
 * exceeding it does not crash: the earliest result quietly becomes a
 * later one.
 *
 * A thread that cannot get its buffers gets a shared, static "NULL"
 * back instead, so the answer is never a null pointer and is always
 * safe to hand to a "%s" - but it is not safe to write through.
 */
PMIX_EXPORT char *pmix_util_print_name_args(const pmix_proc_t *name);
#define PMIX_NAME_PRINT(n) pmix_util_print_name_args(n)

PMIX_EXPORT char *pmix_util_print_pname_args(const pmix_name_t *name);
#define PMIX_PNAME_PRINT(n) pmix_util_print_pname_args(n)

/* Renders the five named reserved ranks by name; anything else, valid
 * rank or unnamed member of the reserved band, as an unsigned decimal.
 */
PMIX_EXPORT char *pmix_util_print_rank(const pmix_rank_t vpid);
#define PMIX_RANK_PRINT(n) pmix_util_print_rank(n)

#define PMIX_PEER_PRINT(p) pmix_util_print_pname_args(&(p)->info->pname)

/* qsort/bsearch comparator over pmix_proc_t: namespace first, then
 * rank.  The rank half is an explicit three-way compare rather than a
 * subtraction, because pmix_rank_t is unsigned and the reserved ranks
 * sit at the top of its range - a difference reduced mod 2^32 has the
 * wrong sign whenever two ranks are more than INT_MAX apart, which is
 * exactly what a sort mixing ordinary and reserved ranks does.
 */
PMIX_EXPORT int pmix_util_compare_proc(const void *a, const void *b);

/* reset the one-time initialization of the print-buffer TSD key so that
 * a subsequent PMIx_Init recreates it. The key itself is deleted by
 * pmix_tsd_keys_destruct; this only clears the local latch. */
PMIX_EXPORT void pmix_name_fns_finalize(void);

END_C_DECLS
#endif
