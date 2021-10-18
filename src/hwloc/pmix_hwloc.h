/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2007-2008 Cisco Systems, Inc.  All rights reserved.
 *
 * Copyright (c) 2015-2018 Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2018-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2021      Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/**
 * @file
 *
 * This interface is for use by PMIx  to obtain topology-related info
 *
 */

#ifndef PMIX_HWLOC_H
#define PMIX_HWLOC_H

#include "src/include/pmix_config.h"

#include PMIX_HWLOC_HEADER

#include "include/pmix_common.h"

#include "src/class/pmix_list.h"
#include "src/include/pmix_globals.h"
#include "src/mca/base/pmix_mca_base_framework.h"
#include "src/mca/base/pmix_mca_base_var.h"
#include "src/mca/mca.h"
#include "src/server/pmix_server_ops.h"

BEGIN_C_DECLS

#if HWLOC_API_VERSION < 0x20000

#    ifndef HAVE_HWLOC_TOPOLOGY_DUP
#        define HAVE_HWLOC_TOPOLOGY_DUP 0
#    endif

#    define HWLOC_OBJ_L3CACHE HWLOC_OBJ_CACHE
#    define HWLOC_OBJ_L2CACHE HWLOC_OBJ_CACHE
#    define HWLOC_OBJ_L1CACHE HWLOC_OBJ_CACHE

#    if HWLOC_API_VERSION < 0x10a00
#        define HWLOC_OBJ_NUMANODE HWLOC_OBJ_NODE
#        define HWLOC_OBJ_PACKAGE  HWLOC_OBJ_SOCKET
#    endif

#    define HAVE_DECL_HWLOC_OBJ_OSDEV_COPROC 0

#else

#    define HAVE_DECL_HWLOC_OBJ_OSDEV_COPROC 1

#endif

typedef enum {
    VM_HOLE_NONE = -1,
    VM_HOLE_BEGIN = 0,        /* use hole at the very beginning */
    VM_HOLE_AFTER_HEAP = 1,   /* use hole right after heap */
    VM_HOLE_BEFORE_STACK = 2, /* use hole right before stack */
    VM_HOLE_BIGGEST = 3,      /* use biggest hole */
    VM_HOLE_IN_LIBS = 4,      /* use biggest hole between heap and stack */
    VM_HOLE_CUSTOM = 5,       /* use given address if available */
} pmix_hwloc_vm_hole_kind_t;

typedef enum {
    VM_MAP_FILE = 0,
    VM_MAP_ANONYMOUS = 1,
    VM_MAP_HEAP = 2,
    VM_MAP_STACK = 3,
    VM_MAP_OTHER = 4 /* vsyscall/vdso/vvar shouldn't occur since we stop after stack */
} pmix_hwloc_vm_map_kind_t;


/**
 * Register params
 */
PMIX_EXPORT pmix_status_t pmix_hwloc_register(void);

/**
 * Finalize. Tear down any allocated storage
 */
PMIX_EXPORT void pmix_hwloc_finalize(void);

/* Setup the topology for delivery to clients */
PMIX_EXPORT pmix_status_t pmix_hwloc_setup_topology(pmix_info_t *info, size_t ninfo);

/* Load the topology */
PMIX_EXPORT pmix_status_t pmix_hwloc_load_topology(pmix_topology_t *topo);

/* Generate the string representation of a cpuset */
PMIX_EXPORT pmix_status_t pmix_hwloc_generate_cpuset_string(const pmix_cpuset_t *cpuset,
                                                            char **cpuset_string);

/* get cpuset from its string representation */
PMIX_EXPORT pmix_status_t pmix_hwloc_parse_cpuset_string(const char *cpuset_string, pmix_cpuset_t *cpuset);

/* Get locality string */
PMIX_EXPORT pmix_status_t pmix_hwloc_generate_locality_string(const pmix_cpuset_t *cpuset, char **loc);

/* Get relative locality */
PMIX_EXPORT pmix_status_t pmix_hwloc_get_relative_locality(const char *locality1,
                                                           const char *locality2,
                                                           pmix_locality_t *loc);

/* Get current bound location */
PMIX_EXPORT pmix_status_t pmix_hwloc_get_cpuset(pmix_cpuset_t *cpuset, pmix_bind_envelope_t ref);

/* Get distance array */
PMIX_EXPORT pmix_status_t pmix_hwloc_compute_distances(pmix_topology_t *topo, pmix_cpuset_t *cpuset,
                                                       pmix_info_t info[], size_t ninfo,
                                                       pmix_device_distance_t **dist, size_t *ndist);

PMIX_EXPORT pmix_status_t pmix_hwloc_check_vendor(pmix_topology_t *topo,
                                                  unsigned short vendorID,
                                                  uint16_t class);

/* cpuset pack/unpack/copy/print functions */
PMIX_EXPORT pmix_status_t pmix_hwloc_pack_cpuset(pmix_buffer_t *buf, pmix_cpuset_t *src,
                                                 pmix_pointer_array_t *regtypes);

PMIX_EXPORT pmix_status_t pmix_hwloc_unpack_cpuset(pmix_buffer_t *buf, pmix_cpuset_t *dest,
                                                   pmix_pointer_array_t *regtypes);

PMIX_EXPORT pmix_status_t pmix_hwloc_copy_cpuset(pmix_cpuset_t *dest, pmix_cpuset_t *src);

PMIX_EXPORT char *pmix_hwloc_print_cpuset(pmix_cpuset_t *src);

PMIX_EXPORT void pmix_hwloc_destruct_cpuset(pmix_cpuset_t *cpuset);

PMIX_EXPORT void pmix_hwloc_release_cpuset(pmix_cpuset_t *ptr, size_t sz);

/* topology pack/unpack/copy/print functions */
PMIX_EXPORT pmix_status_t pmix_hwloc_pack_topology(pmix_buffer_t *buf, pmix_topology_t *src,
                                                   pmix_pointer_array_t *regtypes);

PMIX_EXPORT pmix_status_t pmix_hwloc_unpack_topology(pmix_buffer_t *buf, pmix_topology_t *dest,
                                                     pmix_pointer_array_t *regtypes);

PMIX_EXPORT pmix_status_t pmix_hwloc_copy_topology(pmix_topology_t *dest, pmix_topology_t *src);

PMIX_EXPORT char *pmix_hwloc_print_topology(pmix_topology_t *src);

PMIX_EXPORT void pmix_hwloc_destruct_topology(pmix_topology_t *ptr);

PMIX_EXPORT void pmix_hwloc_release_topology(pmix_topology_t *ptr, size_t sz);

/****  PRESERVE ABI  ****/
PMIX_EXPORT void pmix_ploc_base_destruct_cpuset(pmix_cpuset_t *cpuset);
PMIX_EXPORT void pmix_ploc_base_release_cpuset(pmix_cpuset_t *ptr, size_t sz);
PMIX_EXPORT void pmix_ploc_base_destruct_topology(pmix_topology_t *ptr);
PMIX_EXPORT void pmix_ploc_base_release_topology(pmix_topology_t *ptr, size_t sz);

END_C_DECLS

#endif
