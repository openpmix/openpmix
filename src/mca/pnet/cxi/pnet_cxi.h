/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#ifndef PMIX_PNET_CXI_H
#define PMIX_PNET_CXI_H

#include "src/include/pmix_config.h"

#include "src/mca/pnet/pnet.h"

BEGIN_C_DECLS

typedef struct {
    pmix_pnet_base_component_t super;
    /* root of the sysfs tree a Slingshot interface's CXI device is found in */
    char *sysfs_root;
} pmix_pnet_cxi_component_t;

/* the component must be visible data for the linker to find it */
PMIX_EXPORT extern pmix_pnet_cxi_component_t pmix_mca_pnet_cxi_component;
extern pmix_pnet_module_t pmix_pnet_cxi_module;

/* the PCI families this component opens for and claims - see pnet_cxi.c */
extern const pmix_pnet_pcimatch_t pmix_pnet_cxi_match[];
extern const size_t pmix_pnet_cxi_nmatch;

END_C_DECLS

#endif
