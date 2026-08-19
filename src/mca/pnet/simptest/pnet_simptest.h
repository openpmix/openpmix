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

#ifndef PMIX_PNET_simptest_H
#define PMIX_PNET_simptest_H

#include "src/include/pmix_config.h"

#include "src/mca/pnet/pnet.h"

BEGIN_C_DECLS

typedef struct {
    pmix_pnet_base_component_t super;
    char *configfile;
} pmix_pnet_simptest_component_t;

/* the component must be visible data for the linker to find it */
PMIX_EXPORT extern pmix_pnet_simptest_component_t pmix_mca_pnet_simptest_component;
extern pmix_pnet_module_t pmix_pnet_simptest_module;

/* The key for the blob we send in a launch msg. Both ends of that
 * exchange live in pnet_simptest.c and both must name it from here:
 * this used to read "pmix.pnet.simptest.blob" while the code
 * hand-wrote "pmix-pnet-simptest-blob" at each site, so the macro was
 * dead and the first person to "just use the macro" would have moved
 * the key out from under the receiver. The value is the one that has
 * always been on the wire. */
#define PMIX_PNET_SIMPTEST_BLOB "pmix-pnet-simptest-blob"

END_C_DECLS

#endif
