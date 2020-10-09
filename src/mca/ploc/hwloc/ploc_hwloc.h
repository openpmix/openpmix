/*
 * Copyright (c) 2015-2020 Intel, Inc.  All rights reserved.
 *
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#ifndef PMIX_PLOC_HWLOC_H
#define PMIX_PLOC_HWLOC_H

#include "src/include/pmix_config.h"

#include PMIX_HWLOC_HEADER

#include "src/mca/ploc/ploc.h"

BEGIN_C_DECLS

#if HWLOC_API_VERSION < 0x20000

#ifndef HAVE_HWLOC_TOPOLOGY_DUP
#define HAVE_HWLOC_TOPOLOGY_DUP 0
#endif

#define HWLOC_OBJ_L3CACHE HWLOC_OBJ_CACHE
#define HWLOC_OBJ_L2CACHE HWLOC_OBJ_CACHE
#define HWLOC_OBJ_L1CACHE HWLOC_OBJ_CACHE

#if HWLOC_API_VERSION < 0x10a00
#define HWLOC_OBJ_NUMANODE HWLOC_OBJ_NODE
#define HWLOC_OBJ_PACKAGE HWLOC_OBJ_SOCKET
#endif

#define HAVE_DECL_HWLOC_OBJ_OSDEV_COPROC 0

#else

#define HAVE_DECL_HWLOC_OBJ_OSDEV_COPROC 1

#endif

typedef struct {
    pmix_ploc_base_component_t super;
    char *topo_file;
    char *testcpuset;
} pmix_ploc_hwloc_component_t;

/* the component must be visible data for the linker to find it */
PMIX_EXPORT extern pmix_ploc_hwloc_component_t mca_ploc_hwloc_component;
extern pmix_ploc_module_t pmix_ploc_hwloc_module;

END_C_DECLS

#endif
