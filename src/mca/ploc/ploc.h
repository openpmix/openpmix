/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2007-2008 Cisco Systems, Inc.  All rights reserved.
 *
 * Copyright (c) 2015-2018 Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2018-2020 Intel, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/**
 * @file
 *
 * This interface is for use by PMIx servers to obtain topology-related info
*
 * Available plugins may be defined at runtime via the typical MCA parameter
 * syntax.
 */

#ifndef PMIX_PLOC_H
#define PMIX_PLOC_H

#include "src/include/pmix_config.h"
#include "include/pmix_common.h"

#include "src/class/pmix_list.h"
#include "src/mca/mca.h"
#include "src/mca/base/pmix_mca_base_var.h"
#include "src/mca/base/pmix_mca_base_framework.h"
#include "src/include/pmix_globals.h"
#include "src/server/pmix_server_ops.h"

BEGIN_C_DECLS

/******    MODULE DEFINITION    ******/

/**
 * Initialize the module. Returns an error if the module cannot
 * run, success if it can and wants to be used.
 */
typedef pmix_status_t (*pmix_ploc_base_module_init_fn_t)(void);

/**
 * Finalize the module. Tear down any allocated storage, disconnect
 * from any system support (e.g., LDAP server)
 */
typedef void (*pmix_ploc_base_module_fini_fn_t)(void);

/* Setup the topology for delivery to clients */
typedef pmix_status_t (*pmix_ploc_base_module_setup_topo_fn_t)(pmix_info_t *info, size_t ninfo);

/* Load the topology */
typedef pmix_status_t (*pmix_ploc_base_module_load_topo_fn_t)(pmix_topology_t *topo);

/* Generate the string representation of a cpuset */
typedef pmix_status_t (*pmix_ploc_base_module_generate_cpuset_string_fn_t)(const pmix_cpuset_t *cpuset,
                                                                           char **cpuset_string);

/* get cpuset from its string representation */
typedef pmix_status_t (*pmix_ploc_base_module_get_cpuset_fn_t)(const char *cpuset_string,
                                                               pmix_cpuset_t *cpuset);

/* Get locality string */
typedef pmix_status_t (*pmix_ploc_base_module_generate_loc_str_fn_t)(const pmix_cpuset_t *cpuset,
                                                                     char **locality);


/* Get relative locality */
typedef pmix_status_t (*pmix_ploc_base_module_get_rel_loc_fn_t)(const char *loc1,
                                                                const char *loc2,
                                                                pmix_locality_t *locality);

/**
 * Base structure for a PLOC module. Each component should malloc a
 * copy of the module structure for each fabric plane they support.
 */
typedef struct {
    char *name;
    pmix_ploc_base_module_init_fn_t                     init;
    pmix_ploc_base_module_fini_fn_t                     finalize;
    pmix_ploc_base_module_setup_topo_fn_t               setup_topology;
    pmix_ploc_base_module_load_topo_fn_t                load_topology;
    pmix_ploc_base_module_generate_cpuset_string_fn_t   generate_cpuset_string;
    pmix_ploc_base_module_get_cpuset_fn_t               get_cpuset;
    pmix_ploc_base_module_generate_loc_str_fn_t         generate_locality_string;
    pmix_ploc_base_module_get_rel_loc_fn_t              get_relative_locality;
} pmix_ploc_module_t;

/* define a public API */
typedef struct {
    pmix_ploc_base_module_setup_topo_fn_t               setup_topology;
    pmix_ploc_base_module_load_topo_fn_t                load_topology;
    pmix_ploc_base_module_generate_cpuset_string_fn_t   generate_cpuset_string;
    pmix_ploc_base_module_get_cpuset_fn_t               get_cpuset;
    pmix_ploc_base_module_generate_loc_str_fn_t         generate_locality_string;
    pmix_ploc_base_module_get_rel_loc_fn_t              get_relative_locality;
} pmix_ploc_API_module_t;


/* declare the global APIs */
PMIX_EXPORT extern pmix_ploc_API_module_t pmix_ploc;

/*
 * the standard component data structure
 */
struct pmix_ploc_base_component_t {
    pmix_mca_base_component_t                        base;
    pmix_mca_base_component_data_t                   data;
};
typedef struct pmix_ploc_base_component_t pmix_ploc_base_component_t;

/*
 * Macro for use in components that are of type ploc
 */
#define PMIX_PLOC_BASE_VERSION_1_0_0 \
    PMIX_MCA_BASE_VERSION_1_0_0("ploc", 1, 0, 0)

END_C_DECLS

#endif
