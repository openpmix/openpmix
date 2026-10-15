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

#ifndef PMIX_NATIVE_H
#define PMIX_NATIVE_H

#include "src/include/pmix_config.h"

#include "src/mca/psec/psec.h"

BEGIN_C_DECLS

/* the component must be visible data for the linker to find it */
PMIX_EXPORT extern pmix_psec_base_component_t pmix_mca_psec_native_component;
extern pmix_psec_module_t pmix_native_module;

typedef struct {
    /* accept a peer on the identity it claims, when it connects over TCP
     * and the kernel cannot confirm who owns the connection */
    bool legacy_auth;
} pmix_psec_native_params_t;
PMIX_EXPORT extern pmix_psec_native_params_t pmix_psec_native_params;

END_C_DECLS

#endif
