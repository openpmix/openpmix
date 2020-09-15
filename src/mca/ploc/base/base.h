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
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 */
#ifndef PMIX_PLOC_BASE_H_
#define PMIX_PLOC_BASE_H_

#include "src/include/pmix_config.h"


#ifdef HAVE_SYS_TIME_H
#include <sys/time.h> /* for struct timeval */
#endif
#ifdef HAVE_STRING_H
#include <string.h>
#endif

#include "src/class/pmix_list.h"
#include "src/class/pmix_pointer_array.h"
#include "src/mca/mca.h"
#include "src/mca/base/pmix_mca_base_framework.h"

#include "src/mca/ploc/ploc.h"


BEGIN_C_DECLS

/*
 * MCA Framework
 */
PMIX_EXPORT extern pmix_mca_base_framework_t pmix_ploc_base_framework;
/**
 * PLOC select function
 *
 * Cycle across available components and construct the list
 * of active modules
 */
PMIX_EXPORT pmix_status_t pmix_ploc_base_select(void);

/**
 * Track an active component / module
 */
struct pmix_ploc_base_active_module_t {
    pmix_list_item_t super;
    int pri;
    pmix_ploc_module_t *module;
    pmix_ploc_base_component_t *component;
};
typedef struct pmix_ploc_base_active_module_t pmix_ploc_base_active_module_t;
PMIX_EXPORT PMIX_CLASS_DECLARATION(pmix_ploc_base_active_module_t);

/* framework globals */
struct pmix_ploc_globals_t {
    pmix_lock_t lock;
    pmix_list_t actives;
    bool initialized;
    bool selected;
};
typedef struct pmix_ploc_globals_t pmix_ploc_globals_t;

PMIX_EXPORT extern pmix_ploc_globals_t pmix_ploc_globals;

PMIX_EXPORT pmix_status_t pmix_ploc_base_setup_topology(pmix_info_t *info, size_t ninfo);
PMIX_EXPORT pmix_status_t pmix_ploc_base_load_topology(pmix_topology_t *topo);
PMIX_EXPORT pmix_status_t pmix_ploc_base_generate_cpuset_string(const pmix_cpuset_t *cpuset,
                                                                char **cpuset_string);
PMIX_EXPORT pmix_status_t pmix_ploc_base_get_cpuset(const char *cpuset_string,
                                                    pmix_cpuset_t *cpuset);
PMIX_EXPORT pmix_status_t pmix_ploc_base_generate_locality_string(const pmix_cpuset_t *cpuset,
                                                                  char **locality);
PMIX_EXPORT pmix_status_t pmix_ploc_base_get_relative_locality(const char *loc1,
                                                               const char *loc2,
                                                               pmix_locality_t *locality);
PMIX_EXPORT pmix_status_t pmix_ploc_base_pack(pmix_buffer_t *buf, pmix_cpuset_t *src);
PMIX_EXPORT pmix_status_t pmix_ploc_base_unpack(pmix_buffer_t *buf, pmix_cpuset_t *dest);
PMIX_EXPORT pmix_status_t pmix_ploc_base_copy(pmix_cpuset_t *dest, pmix_cpuset_t *src);
PMIX_EXPORT char* pmix_ploc_base_print(pmix_cpuset_t *src);
PMIX_EXPORT void pmix_ploc_base_release(pmix_cpuset_t *ptr, size_t sz);
END_C_DECLS

#endif
