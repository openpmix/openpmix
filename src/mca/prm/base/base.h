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
 * Copyright (c) 2021      Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 */
#ifndef PMIX_PRM_BASE_H_
#define PMIX_PRM_BASE_H_

#include "src/include/pmix_config.h"

#ifdef HAVE_SYS_TIME_H
#    include <sys/time.h> /* for struct timeval */
#endif
#ifdef HAVE_STRING_H
#    include <string.h>
#endif

#include "src/class/pmix_list.h"
#include "src/class/pmix_pointer_array.h"
#include "src/mca/base/pmix_mca_base_framework.h"
#include "src/mca/mca.h"

#include "src/mca/prm/prm.h"

BEGIN_C_DECLS

/*
 * MCA Framework
 */
PMIX_EXPORT extern pmix_mca_base_framework_t pmix_prm_base_framework;
/**
 * PRM select function
 *
 * Cycle across available components and construct the list
 * of active modules
 */
PMIX_EXPORT pmix_status_t pmix_prm_base_select(void);

/**
 * Track an active component / module
 */
struct pmix_prm_base_active_module_t {
    pmix_list_item_t super;
    int pri;
    pmix_prm_module_t *module;
    pmix_prm_base_component_t *component;
};
typedef struct pmix_prm_base_active_module_t pmix_prm_base_active_module_t;
PMIX_EXPORT PMIX_CLASS_DECLARATION(pmix_prm_base_active_module_t);

/* define an object for rolling up operations */
typedef struct {
    pmix_object_t super;
    pmix_lock_t lock;
    pmix_event_t ev;
    pmix_status_t status;
    int requests;
    int replies;
    pmix_op_cbfunc_t cbfunc;
    void *cbdata;
} pmix_prm_rollup_t;
PMIX_EXPORT PMIX_CLASS_DECLARATION(pmix_prm_rollup_t);

/* framework globals */
struct pmix_prm_globals_t {
    pmix_lock_t lock;
    pmix_list_t actives;
    bool initialized;
    bool selected;
};
typedef struct pmix_prm_globals_t pmix_prm_globals_t;

PMIX_EXPORT extern pmix_prm_globals_t pmix_prm_globals;

PMIX_EXPORT pmix_status_t pmix_prm_base_notify(pmix_status_t status,
                                               const pmix_proc_t *source,
                                               pmix_data_range_t range,
                                               const pmix_info_t info[], size_t ninfo,
                                               pmix_op_cbfunc_t cbfunc, void *cbdata);
PMIX_EXPORT int pmix_prm_base_get_remaining_time(uint32_t *timeleft);

END_C_DECLS

#endif
