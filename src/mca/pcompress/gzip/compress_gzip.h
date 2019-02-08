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
 * GZIP COMPRESS component
 *
 * Uses the gzip library
 */

#ifndef MCA_COMPRESS_GZIP_EXPORT_H
#define MCA_COMPRESS_GZIP_EXPORT_H

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
    struct pmix_compress_gzip_component_t {
        pmix_compress_base_component_t super;  /** Base COMPRESS component */

    };
    typedef struct pmix_compress_gzip_component_t pmix_compress_gzip_component_t;
    extern pmix_compress_gzip_component_t mca_compress_gzip_component;

    int pmix_compress_gzip_component_query(pmix_mca_base_module_t **module, int *priority);

    /*
     * Module functions
     */
    int pmix_compress_gzip_module_init(void);
    int pmix_compress_gzip_module_finalize(void);

    /*
     * Actual funcationality
     */
    int pmix_compress_gzip_compress(char *fname, char **cname, char **postfix);
    int pmix_compress_gzip_compress_nb(char *fname, char **cname, char **postfix, pid_t *child_pid);
    int pmix_compress_gzip_decompress(char *cname, char **fname);
    int pmix_compress_gzip_decompress_nb(char *cname, char **fname, pid_t *child_pid);

#if defined(c_plusplus) || defined(__cplusplus)
}
#endif

#endif /* MCA_COMPRESS_GZIP_EXPORT_H */
