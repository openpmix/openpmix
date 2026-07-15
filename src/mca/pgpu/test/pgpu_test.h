/*
 * Copyright (c) 2015-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2021-2022 Nanook Consulting.  All rights reserved.
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#ifndef PMIX_PGPU_TEST_H
#define PMIX_PGPU_TEST_H

#include "src/include/pmix_config.h"

#include "src/mca/pgpu/pgpu.h"

BEGIN_C_DECLS

typedef struct {
    pmix_pgpu_base_component_t super;
    char *incparms;
    char *excparms;
    char **include;
    char **exclude;
} pmix_pgpu_test_component_t;

/* the component must be visible data for the linker to find it */
PMIX_EXPORT extern pmix_pgpu_test_component_t pmix_mca_pgpu_test_component;
extern pmix_pgpu_module_t pmix_pgpu_test_module;

/* define a key for the blob we send in a launch msg */
#define PMIX_PGPU_TEST_BLOB "pmix.pgpu.test.blob"

END_C_DECLS

#endif
