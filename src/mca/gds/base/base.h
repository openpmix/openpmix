/* -*- C -*-
 *
 * Copyright (c) 2004-2007 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2006 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2012      Los Alamos National Security, Inc.  All rights reserved.
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2015-2020 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2018      IBM Corporation.  All rights reserved.
 * Copyright (c) 2019      Mellanox Technologies, Inc.
 *                         All rights reserved.
 * Copyright (c) 2021-2024 Nanook Consulting  All rights reserved.
 * Copyright (c) 2026      Jeff Squyres  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 */
#ifndef PMIX_GDS_BASE_H_
#define PMIX_GDS_BASE_H_

#include "src/include/pmix_config.h"

#ifdef HAVE_SYS_TIME_H
#    include <sys/time.h> /* for struct timeval */
#endif
#ifdef HAVE_STRING_H
#    include <string.h>
#endif

#include "src/class/pmix_list.h"
#include "src/mca/base/pmix_mca_base_framework.h"
#include "src/mca/mca.h"

#include "src/mca/gds/gds.h"

BEGIN_C_DECLS

/*
 * MCA Framework
 */
PMIX_EXPORT extern pmix_mca_base_framework_t pmix_gds_base_framework;
/**
 * GDS select function
 *
 * Cycle across available components and construct the list
 * of active modules
 */
PMIX_EXPORT pmix_status_t pmix_gds_base_select(pmix_info_t info[], size_t ninfo);

/**
 * Track an active component / module
 */
struct pmix_gds_base_active_module_t {
    pmix_list_item_t super;
    int pri;
    pmix_gds_base_module_t *module;
    pmix_gds_base_component_t *component;
};
typedef struct pmix_gds_base_active_module_t pmix_gds_base_active_module_t;
PMIX_EXPORT PMIX_CLASS_DECLARATION(pmix_gds_base_active_module_t);

/* framework globals */
struct pmix_gds_globals_t {
    pmix_list_t actives;
    bool initialized;
    bool selected;
    char *all_mods;
};

typedef struct pmix_gds_globals_t pmix_gds_globals_t;

typedef pmix_status_t (*pmix_gds_base_store_modex_cb_fn_t)(pmix_proc_t *proc,
                                                           pmix_buffer_t *pbkt);

PMIX_EXPORT extern pmix_gds_globals_t pmix_gds_globals;

/* get a list of available support - caller must free results
 * when done. The list is returned as a comma-delimited string
 * of available components in priority order */
PMIX_EXPORT char *pmix_gds_base_get_available_modules(void);

/* Select a gds module based on the provided directives */
PMIX_EXPORT pmix_gds_base_module_t *pmix_gds_base_assign_module(pmix_info_t *info, size_t ninfo);

/* Return the highest-priority active gds module other than the given
 * "failing" module, or NULL if no other module is available. Used to
 * fall back to the next module when the current one cannot be used at
 * runtime. No module names are hard-coded: selection is purely by the
 * priority of the remaining active modules. */
PMIX_EXPORT pmix_gds_base_module_t *
pmix_gds_base_get_fallback_module(pmix_gds_base_module_t *failing);

/**
 * Add any envars to a peer's environment that the module needs
 * to communicate. The API stub will rotate across all active modules, giving
 * each a chance to contribute
 *
 * @return PMIX_SUCCESS on success.
 */
PMIX_EXPORT pmix_status_t pmix_gds_base_setup_fork(const pmix_proc_t *proc, char ***env);

/* Walk an aggregated fence result and hand each proc's blob to cb_fn,
 * then call cb_fn once per nspace with a NULL buffer to signal "done".
 *
 * nspace, if not NULL, restricts the walk to blobs belonging to that
 * nspace. Local clients of different nspaces may be assigned different
 * gds modules, and each module has to be given its own nspaces' data,
 * so the same payload is walked once per participating nspace. Pass
 * NULL to store everything in the payload. */
PMIX_EXPORT pmix_status_t pmix_gds_base_store_modex(pmix_buffer_t *buff,
                                                    const char *nspace,
                                                    pmix_gds_base_store_modex_cb_fn_t cb_fn,
                                                    void *cbdata);

/**
 * Find the process identifier carried by a process-realm info array.
 *
 * PMIX_PROC_INFO_ARRAY requires the array to identify the process it
 * describes, and allows the host to do so with either PMIX_RANK (plus
 * PMIX_NSPACE where the job would otherwise be ambiguous) or
 * PMIX_PROCID. It places no requirement on where in the array that
 * identifier appears. Scan for whichever the host used rather than
 * demanding one particular key in the first position.
 *
 * PMIX_RANK is preferred when both are present. The array position
 * that carried the identifier is returned in idpos so the caller can
 * skip it when storing the remaining values - it names the array
 * rather than being data belonging to the process.
 *
 * @param array   the array of info describing the process
 * @param size    number of elements in the array
 * @param rank    OUT - the rank the array describes
 * @param idpos   OUT - index of the element that identified it
 *
 * @return PMIX_SUCCESS, or PMIX_ERR_TYPE_MISMATCH if no usable
 *         identifier is present.
 */
PMIX_EXPORT pmix_status_t pmix_gds_base_proc_array_id(const pmix_info_t *array, size_t size,
                                                      pmix_rank_t *rank, size_t *idpos);

END_C_DECLS

#endif
