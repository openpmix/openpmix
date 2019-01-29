/*
 * Copyright (c) 2004-2010 The Trustees of Indiana University.
 *                         All rights reserved.
 * Copyright (c) 2019      Intel, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/**
 * @file
 *
 * ZLIB COMPRESS component
 *
 * Uses the zlib library
 */

#ifndef MCA_COMPRESS_ZLIB_EXPORT_H
#define MCA_COMPRESS_ZLIB_EXPORT_H

#include "pmix_config.h"

#include "src/util/output.h"

#include "src/mca/mca.h"
#include "src/mca/compress/compress.h"

#if defined(c_plusplus) || defined(__cplusplus)
extern "C" {
#endif

    /*
     * Local Component structures
     */
    struct pmix_compress_zlib_component_t {
        pmix_compress_base_component_t super;  /** Base COMPRESS component */

    };
    typedef struct pmix_compress_zlib_component_t pmix_compress_zlib_component_t;
    extern pmix_compress_zlib_component_t mca_compress_zlib_component;

    int pmix_compress_zlib_component_query(pmix_mca_base_module_t **module, int *priority);

    /*
     * Module functions
     */
    int pmix_compress_zlib_module_init(void);
    int pmix_compress_zlib_module_finalize(void);

    /*
     * Actual funcationality
     */
    bool pmix_compress_zlib_compress_string(char *instring,
                                            uint8_t **outbytes,
                                            size_t *nbytes);
    void pmix_compress_zlib_decompress_string(char **outstring,
                                              uint8_t *inbytes, size_t len);

#if defined(c_plusplus) || defined(__cplusplus)
}
#endif

#endif /* MCA_COMPRESS_ZLIB_EXPORT_H */
