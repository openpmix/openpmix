/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2004-2010 The Trustees of Indiana University.
 *                         All rights reserved.
 * Copyright (c) 2015      Los Alamos National Security, LLC. All rights
 *                         reserved.
 * Copyright (c) 2019-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2021-2025 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "pmix_config.h"

#include "compress_zlib.h"
#include "pmix_common.h"
#include "src/mca/pcompress/base/base.h"

/*
 * Public string for version number
 */
const char *pmix_compress_zlib_component_version_string
    = "PMIX COMPRESS zlib MCA component version " PMIX_VERSION;

/* Level handed to deflateInit().  Default 1, not zlib's 9.
 *
 * This framework's job is shrinking large collective payloads on a single
 * progress thread while a whole job waits on the result, and for that workload
 * level 9 is the wrong end of the curve.  Measured on a 25.6 MB aggregated
 * modex, single-threaded: level 1 deflates at 138 MB/s for a ratio of 0.501,
 * level 9 at 47 MB/s for 0.490 - roughly 3x the CPU to shrink the payload a
 * further 2 percent.  A broadcast pays the deflate once and the wire cost on
 * every link of the tree, so that extra 2 percent is worth having only where
 * the tree is very wide and the link very slow; everywhere else level 1 wins
 * outright.  It is a parameter rather than a constant precisely because that
 * trade depends on the fabric, which no library can see. */
int pmix_pcompress_zlib_level = 1;

/*
 * Local functionality
 */
static int compress_zlib_register(void);
static int compress_zlib_query(pmix_mca_base_module_t **module, int *priority);

/*
 * Instantiate the public struct with all of our public information
 * and pointer to our public functions in it
 */
PMIX_EXPORT pmix_mca_base_component_t pmix_mca_pcompress_zlib_component = {
    /* Handle the general mca_component_t struct containing
     *  meta information about the component zlib
     */
    PMIX_COMPRESS_BASE_VERSION_2_0_0,

    /* Component name and version */
    .pmix_mca_component_name = "zlib",
    PMIX_MCA_BASE_MAKE_VERSION(component, PMIX_MAJOR_VERSION, PMIX_MINOR_VERSION,
                               PMIX_RELEASE_VERSION),

    /* Component open and close functions */
    .pmix_mca_register_component_params = compress_zlib_register,
    .pmix_mca_query_component = compress_zlib_query
};
PMIX_MCA_BASE_COMPONENT_INIT(pmix, pcompress, zlib)

static int compress_zlib_register(void)
{
    pmix_mca_base_component_var_register(&pmix_mca_pcompress_zlib_component, "level",
                                         "Compression level passed to zlib (0 is none, "
                                         "1 is fastest, 9 is smallest; the default of 1 suits "
                                         "the large payloads this framework compresses)",
                                         PMIX_MCA_BASE_VAR_TYPE_INT,
                                         &pmix_pcompress_zlib_level);
    return PMIX_SUCCESS;
}

static int compress_zlib_query(pmix_mca_base_module_t **module, int *priority)
{
    *module = (pmix_mca_base_module_t *) &pmix_pcompress_zlib_module;
    *priority = 50;

    return PMIX_SUCCESS;
}
