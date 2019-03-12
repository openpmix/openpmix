/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2019      IBM Corporation.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/**
 * @file
 *
 * This interface is for the encoding/decoding of basic types and the
 * compression/decompression of larger blobs of data (i.e., modex).
 *
 * Available plugins may be defined at runtime via the typical MCA parameter
 * syntax.
 */

#ifndef PMIX_PSQUASH_H
#define PMIX_PSQUASH_H

#include <src/include/pmix_config.h>

#include "src/mca/mca.h"
#include "src/mca/base/pmix_mca_base_var.h"
#include "src/mca/base/pmix_mca_base_framework.h"

BEGIN_C_DECLS

/******    MODULE DEFINITION    ******/

/**
 * Initialize the module
 */
typedef pmix_status_t (*pmix_psquash_base_module_init_fn_t)(void);

/**
 * Finalize the module
 */
typedef void (*pmix_psquash_base_module_finalize_fn_t)(void);

/**
 * Maximum size of the buffer passed to encode/decode for integers.
 *
 * Define a constant max here to avoid dynamic memory allocation in the call path.
 */
#define PMIX_PSQUASH_INT_MAX_BUF_SIZE (SIZEOF_SIZE_T+1)

/**
 * Encode a basic integer type into a contiguous destination buffer.
 *
 * type     - Type of the 'src' pointer (PMIX_SIZE, PMIX_INT to PMIX_UINT64)
 * src      - pointer to a single basic integer type
 * dest     - pointer to buffer to store data
 * dest_len - length, in bytes, of the dest buffer
 */
typedef pmix_status_t (*pmix_psquash_encode_int_fn_t) (pmix_data_type_t type,
                                                       void *src,
                                                       uint8_t dest[PMIX_PSQUASH_INT_MAX_BUF_SIZE],
                                                       uint8_t *dest_len);

/**
 * Decode a basic a contiguous destination buffer into a basic integer type.
 *
 * type     - Type of the 'dest' pointer (PMIX_SIZE, PMIX_INT to PMIX_UINT64)
 * src      - pointer to buffer where data was stored
 * src_len  - length, in bytes, of the src buffer
 * dest     - pointer to a single basic integer type
 */
typedef pmix_status_t (*pmix_psquash_decode_int_fn_t) (pmix_data_type_t type,
                                                       uint8_t src[PMIX_PSQUASH_INT_MAX_BUF_SIZE],
                                                       uint8_t src_len,
                                                       void *dest);

/**
 * Base structure for a PSQUASH module
 */
typedef struct {
    const char *name;

    /** init/finalize */
    pmix_psquash_base_module_init_fn_t     init;
    pmix_psquash_base_module_finalize_fn_t finalize;

    /** Integer compression */
    pmix_psquash_encode_int_fn_t           encode_int;
    pmix_psquash_decode_int_fn_t           decode_int;
} pmix_psquash_base_module_t;

/**
 * Base structure for a PSQUASH component
 */
struct pmix_psquash_base_component_t {
    pmix_mca_base_component_t                        base;
    pmix_mca_base_component_data_t                   data;
    int                                              priority;
};
typedef struct pmix_psquash_base_component_t pmix_psquash_base_component_t;

PMIX_EXPORT extern pmix_psquash_base_module_t pmix_psquash;

/*
 * Macro for use in components that are of type psquash
 */
#define PMIX_PSQUASH_BASE_VERSION_1_0_0 \
    PMIX_MCA_BASE_VERSION_1_0_0("psquash", 1, 0, 0)

END_C_DECLS

#endif
