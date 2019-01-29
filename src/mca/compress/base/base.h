/*
 * Copyright (c) 2004-2010 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 *
 * Copyright (c) 2019      Intel, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */
#ifndef PMIX_COMPRESS_BASE_H
#define PMIX_COMPRESS_BASE_H

#include "pmix_config.h"
#include "src/mca/compress/compress.h"
#include "src/util/pmix_environ.h"

#include "src/mca/base/base.h"

/*
 * Global functions for MCA overall COMPRESS
 */

#if defined(c_plusplus) || defined(__cplusplus)
extern "C" {
#endif

    /**
     * Initialize the COMPRESS MCA framework
     *
     * @retval PMIX_SUCCESS Upon success
     * @retval PMIX_ERROR   Upon failures
     *
     * This function is invoked during pmix_init();
     */
    PMIX_EXPORT int pmix_compress_base_open(pmix_mca_base_open_flag_t flags);

    /**
     * Select an available component.
     *
     * @retval PMIX_SUCCESS Upon Success
     * @retval PMIX_NOT_FOUND If no component can be selected
     * @retval PMIX_ERROR Upon other failure
     *
     */
    PMIX_EXPORT int pmix_compress_base_select(void);

    /**
     * Finalize the COMPRESS MCA framework
     *
     * @retval PMIX_SUCCESS Upon success
     * @retval PMIX_ERROR   Upon failures
     *
     * This function is invoked during pmix_finalize();
     */
    PMIX_EXPORT int pmix_compress_base_close(void);

    /**
     * Globals
     */
    PMIX_EXPORT extern pmix_mca_base_framework_t pmix_compress_base_framework;
    PMIX_EXPORT extern pmix_compress_base_component_t pmix_compress_base_selected_component;
    PMIX_EXPORT extern pmix_compress_base_module_t pmix_compress;

    /**
     *
     */
    PMIX_EXPORT int pmix_compress_base_tar_create(char ** target);
    PMIX_EXPORT int pmix_compress_base_tar_extract(char ** target);

#if defined(c_plusplus) || defined(__cplusplus)
}
#endif

#endif /* PMIX_COMPRESS_BASE_H */
