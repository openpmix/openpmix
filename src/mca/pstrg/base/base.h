/*
 * Copyright (c) 2009      Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2013      Los Alamos National Security, LLC.  All rights reserved.
 * Copyright (c) 2017-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2020      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2021      Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */
/** @file:
 */

#ifndef PMIX_PSTRG_BASE_H_
#define PMIX_PSTRG_BASE_H_

#include "src/include/pmix_config.h"

#include "src/class/pmix_list.h"
#include "src/mca/base/pmix_mca_base_framework.h"
#include "src/mca/mca.h"

#include "src/mca/pstrg/pstrg.h"

BEGIN_C_DECLS

/*
 * MCA Framework
 */
PMIX_EXPORT extern pmix_mca_base_framework_t pmix_pstrg_base_framework;

PMIX_EXPORT int pmix_pstrg_base_select(void);

/* define a struct to hold framework-global values */
typedef struct {
    pmix_list_t actives;
    pmix_event_base_t *evbase;
    bool selected;
    bool init;
} pmix_pstrg_base_t;

typedef struct {
    pmix_list_item_t super;
    pmix_pstrg_base_component_t *component;
    pmix_pstrg_base_module_t *module;
    int priority;
} pmix_pstrg_active_module_t;
PMIX_CLASS_DECLARATION(pmix_pstrg_active_module_t);

PMIX_EXPORT extern pmix_pstrg_base_t pmix_pstrg_base;

typedef struct {
    pmix_list_item_t super;
    pmix_key_t key;
    pmix_list_t results;
} pmix_pstrg_query_results_t;
PMIX_CLASS_DECLARATION(pmix_pstrg_query_results_t);

PMIX_EXPORT pmix_status_t pmix_pstrg_base_query(pmix_query_t queries[], size_t nqueries,
                                                pmix_list_t *results,
                                                pmix_pstrg_query_cbfunc_t cbfunc, void *cbdata);

PMIX_EXPORT extern pmix_hash_table_t fs_mount_to_id_hash;
PMIX_EXPORT extern pmix_hash_table_t fs_id_to_mount_hash;

typedef struct {
    char *mnt_fsname;
    char *mnt_dir;
    char *mnt_type;
} pmix_pstrg_mntent_t;

//XXX individual modules allocate these, register them, and if successful-> add to per-mod list
typedef struct {
    char *id;
    char *mount_dir;
} pmix_pstrg_fs_info_t;

PMIX_EXPORT pmix_status_t pmix_pstrg_get_fs_mounts(pmix_value_array_t **mounts);
PMIX_EXPORT pmix_status_t pmix_pstrg_free_fs_mounts(pmix_value_array_t **mounts);
PMIX_EXPORT pmix_status_t pmix_pstrg_register_fs(pmix_pstrg_fs_info_t fs_info);
PMIX_EXPORT pmix_status_t pmix_pstrg_deregister_fs(pmix_pstrg_fs_info_t fs_info);
PMIX_EXPORT char *pmix_pstrg_get_registered_fs_id_by_mount(char *mount_dir);
PMIX_EXPORT char *pmix_pstrg_get_registered_fs_mount_by_id(char *id);

END_C_DECLS
#endif
