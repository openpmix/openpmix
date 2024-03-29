/*
 * Copyright (c) 2015-2020 Intel, Inc.  All rights reserved.
 *
 * Copyright (c) 2021-2022 Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#ifndef PMIX_PSTRG_LUSTRE_H
#define PMIX_PSTRG_LUSTRE_H

#include "src/include/pmix_config.h"

#include "src/mca/pstrg/pstrg.h"

BEGIN_C_DECLS

typedef struct {
    pmix_pstrg_base_component_t super;
} pmix_pstrg_lustre_component_t;

/* the component must be visible data for the linker to find it */
PMIX_EXPORT extern pmix_pstrg_lustre_component_t pmix_mca_pstrg_lustre_component;
extern pmix_pstrg_base_module_t pmix_pstrg_lustre_module;

END_C_DECLS

#endif
