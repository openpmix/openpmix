/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2004-2010 The Trustees of Indiana University.
 *                         All rights reserved.
 * Copyright (c) 2015      Los Alamos National Security, LLC. All rights
 *                         reserved.
 * Copyright (c) 2019      Intel, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "pmix_config.h"

#include "pmix_common.h"
#include "src/mca/compress/compress.h"
#include "src/mca/compress/base/base.h"
#include "compress_gzip.h"

/*
 * Public string for version number
 */
const char *pmix_compress_gzip_component_version_string =
"PMIX COMPRESS gzip MCA component version " PMIX_VERSION;

/*
 * Local functionality
 */
static int compress_gzip_register (void);
static int compress_gzip_open(void);
static int compress_gzip_close(void);

/*
 * Instantiate the public struct with all of our public information
 * and pointer to our public functions in it
 */
pmix_compress_gzip_component_t mca_compress_gzip_component = {
    /* First do the base component stuff */
    {
        /* Handle the general mca_component_t struct containing
         *  meta information about the component itgzip
         */
        .base_version = {
            PMIX_COMPRESS_BASE_VERSION_2_0_0,

            /* Component name and version */
            .pmix_mca_component_name = "gzip",
            PMIX_MCA_BASE_MAKE_VERSION(component, PMIX_MAJOR_VERSION, PMIX_MINOR_VERSION,
                                       PMIX_RELEASE_VERSION),

            /* Component open and close functions */
            .pmix_mca_open_component = compress_gzip_open,
            .pmix_mca_close_component = compress_gzip_close,
            .pmix_mca_query_component = pmix_compress_gzip_component_query,
            .pmix_mca_register_component_params = compress_gzip_register
        },
        .base_data = {
            /* The component is checkpoint ready */
            PMIX_MCA_BASE_METADATA_PARAM_CHECKPOINT
        },

        .verbose = 0,
        .output_handle = -1,
    }
};

static bool nocompress(char *instring, uint8_t **outbytes, size_t *nbytes)
{
    return false;
}

static void nodecompress(char **outstring, uint8_t *inbytes, size_t len)
{
    *outstring = NULL;
}

/*
 * Gzip module
 */
static pmix_compress_base_module_t loc_module = {
    /** Initialization Function */
    .init = pmix_compress_gzip_module_init,
    /** Finalization Function */
    .finalize = pmix_compress_gzip_module_finalize,

    /** Compress Function */
    .compress = pmix_compress_gzip_compress,
    .compress_nb = pmix_compress_gzip_compress_nb,

    /** Decompress Function */
    .decompress = pmix_compress_gzip_decompress,
    .decompress_nb = pmix_compress_gzip_decompress_nb,

    .compress_string = nocompress,
    .decompress_string = nodecompress
};

static int compress_gzip_register (void)
{
    int ret;

    mca_compress_gzip_component.super.priority = 15;
    ret = pmix_mca_base_component_var_register (&mca_compress_gzip_component.super.base_version,
                                           "priority", "Priority of the COMPRESS gzip component "
                                           "(default: 15)", PMIX_MCA_BASE_VAR_TYPE_INT, NULL, 0,
                                           PMIX_MCA_BASE_VAR_FLAG_SETTABLE,
                                           PMIX_INFO_LVL_9, PMIX_MCA_BASE_VAR_SCOPE_ALL_EQ,
                                           &mca_compress_gzip_component.super.priority);
    if (0 > ret) {
        return ret;
    }

    mca_compress_gzip_component.super.verbose = 0;
    ret = pmix_mca_base_component_var_register (&mca_compress_gzip_component.super.base_version,
                                           "verbose",
                                           "Verbose level for the COMPRESS gzip component",
                                           PMIX_MCA_BASE_VAR_TYPE_INT, NULL, 0, PMIX_MCA_BASE_VAR_FLAG_SETTABLE,
                                           PMIX_INFO_LVL_9, PMIX_MCA_BASE_VAR_SCOPE_LOCAL,
                                           &mca_compress_gzip_component.super.verbose);
    return (0 > ret) ? ret : PMIX_SUCCESS;
}

static int compress_gzip_open(void)
{
    /* If there is a custom verbose level for this component than use it
     * otherwise take our parents level and output channel
     */
    if ( 0 != mca_compress_gzip_component.super.verbose) {
        mca_compress_gzip_component.super.output_handle = pmix_output_open(NULL);
        pmix_output_set_verbosity(mca_compress_gzip_component.super.output_handle,
                                  mca_compress_gzip_component.super.verbose);
    } else {
        mca_compress_gzip_component.super.output_handle = pmix_compress_base_framework.framework_output;
    }

    /*
     * Debug output
     */
    pmix_output_verbose(10, mca_compress_gzip_component.super.output_handle,
                        "compress:gzip: open()");
    pmix_output_verbose(20, mca_compress_gzip_component.super.output_handle,
                        "compress:gzip: open: priority = %d",
                        mca_compress_gzip_component.super.priority);
    pmix_output_verbose(20, mca_compress_gzip_component.super.output_handle,
                        "compress:gzip: open: verbosity = %d",
                        mca_compress_gzip_component.super.verbose);
    return PMIX_SUCCESS;
}

static int compress_gzip_close(void)
{
    return PMIX_SUCCESS;
}

int pmix_compress_gzip_component_query(pmix_mca_base_module_t **module, int *priority)
{
    *module   = (pmix_mca_base_module_t *)&loc_module;
    *priority = mca_compress_gzip_component.super.priority;

    return PMIX_SUCCESS;
}

