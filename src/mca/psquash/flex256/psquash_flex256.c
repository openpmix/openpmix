/*
 * Copyright (c) 2019      IBM Corporation.  All rights reserved.
 *
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include <src/include/pmix_config.h>

#include <pmix_common.h>

#include "src/include/pmix_socket_errno.h"
#include "src/include/pmix_globals.h"
#include "src/util/argv.h"
#include "src/util/error.h"
#include "src/util/output.h"

#include <unistd.h>
#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif

#include "src/mca/psquash/psquash.h"
#include "psquash_flex256.h"

#include "src/mca/bfrops/base/base.h"

// Move to bfrops framework
#define PMIX_BFROPS_TYPE_SIZEOF(r, t, s)                    \
    do {                                                    \
        (r) = PMIX_SUCCESS;                                 \
        switch (t) {                                        \
            case PMIX_INT16:                                \
            case PMIX_UINT16:                               \
                (s) = SIZEOF_SHORT;                         \
                break;                                      \
            case PMIX_INT:                                  \
            case PMIX_INT32:                                \
            case PMIX_UINT:                                 \
            case PMIX_UINT32:                               \
                (s) = SIZEOF_INT;                           \
                break;                                      \
            case PMIX_INT64:                                \
 case PMIX_UINT64:                                          \
 (s) = SIZEOF_LONG;                                         \
 break;                                                     \
        case PMIX_SIZE:                                     \
            (s) = SIZEOF_SIZE_T;                            \
            break;                                          \
        default:                                            \
            (r) = PMIX_ERR_BAD_PARAM;                       \
        }                                                   \
    } while (0)

static pmix_status_t flex256_init(void);

static void flex256_finalize(void);

static pmix_status_t flex256_encode_int(pmix_data_type_t type,
                                        void *src,
                                        uint8_t dest[PMIX_PSQUASH_INT_MAX_BUF_SIZE],
                                        uint8_t *dest_len);

static pmix_status_t flex256_decode_int(pmix_data_type_t type,
                                        uint8_t src[PMIX_PSQUASH_INT_MAX_BUF_SIZE],
                                        uint8_t src_len,
                                        void *dest);

pmix_psquash_base_module_t pmix_flex256_module = {
    .name = "flex256",
    .init = flex256_init,
    .finalize = flex256_finalize,
    .encode_int = flex256_encode_int,
    .decode_int = flex256_decode_int
};


static pmix_status_t flex256_init(void)
{
    pmix_output_verbose(2, pmix_globals.debug_output,
                        "psquash: flex256 init");
    return PMIX_SUCCESS;
}

static void flex256_finalize(void)
{
    pmix_output_verbose(2, pmix_globals.debug_output,
                        "psquash: flex256 finalize");
}

static pmix_status_t flex256_encode_int(pmix_data_type_t type,
                                        void *src,
                                        uint8_t dest[PMIX_PSQUASH_INT_MAX_BUF_SIZE],
                                        uint8_t *dest_len)
{
    pmix_status_t rc;
    
    PMIX_BFROPS_TYPE_SIZEOF(rc, type, *dest_len);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }

    memcpy(&dest, src, *dest_len);

    return PMIX_SUCCESS;
}

static pmix_status_t flex256_decode_int(pmix_data_type_t type,
                                        uint8_t src[PMIX_PSQUASH_INT_MAX_BUF_SIZE],
                                        uint8_t src_len,
                                        void *dest)
{
    pmix_status_t rc;
    size_t exp_len;

    PMIX_BFROPS_TYPE_SIZEOF(rc, type, exp_len);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }

    if( exp_len != src_len ) { // sanity check
        PMIX_ERROR_LOG(rc);
        return rc;
    }

    memcpy(&dest, src, src_len);

    return PMIX_SUCCESS;
}
