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

#include "compress_zlibng.h"
#include "pmix_common.h"
#include "src/mca/pcompress/base/base.h"

/*
 * Public string for version number
 */
const char *pmix_compress_zlibng_component_version_string
    = "PMIX COMPRESS zlibng MCA component version " PMIX_VERSION;

/* Level handed to zng_deflateInit().  Default 2, not zlib-ng's 1 and not the 9
 * this used to hard-code.
 *
 * Two separate points, and the second is the one that catches people.
 *
 * Level 9 is the wrong end of the curve for what this framework compresses - a
 * large collective payload, on one progress thread, with a whole job waiting on
 * it.  Measured through PMIx on a 25.6 MB aggregated modex, single-threaded:
 * level 9 reaches a ratio of 0.638 at 75 MB/s.  A broadcast pays the deflate
 * once and the wire cost on every link of the tree, so the last couple of
 * percent of ratio only repays itself where the tree is very wide and the link
 * very slow.
 *
 * But 2 rather than the 1 that `zlib` defaults to, because zlib-ng's lowest
 * level is not zlib's.  zlib-ng remaps level 1 onto a quick-deflate strategy
 * that is much faster and appreciably weaker: on the same corpus level 1 gives
 * 0.678 at 242 MB/s while level 2 gives 0.649 at 152 MB/s - and 0.649 is
 * exactly what `zlib` level 1 produces.  So level 2 here is the setting that
 * matches the sibling component's default behaviour rather than merely its
 * number, which is what makes the two interchangeable in practice as well as
 * on the wire.  It is a parameter rather than a constant precisely because that
 * trade depends on the fabric, which no library can see. */
int pmix_pcompress_zlibng_level = 2;

/*
 * Local functionality
 */
static int compress_zlibng_register(void);
static int compress_zlibng_query(pmix_mca_base_module_t **module, int *priority);

/*
 * Instantiate the public struct with all of our public information
 * and pointer to our public functions in it
 */
PMIX_EXPORT pmix_mca_base_component_t pmix_mca_pcompress_zlibng_component = {
    /* Handle the general mca_component_t struct containing
     *  meta information about the component zlibng
     */
    PMIX_COMPRESS_BASE_VERSION_3_0_0,

    /* Component name and version */
    .pmix_mca_component_name = "zlibng",
    PMIX_MCA_BASE_MAKE_VERSION(component, PMIX_MAJOR_VERSION, PMIX_MINOR_VERSION,
                               PMIX_RELEASE_VERSION),

    /* Component open and close functions */
    .pmix_mca_register_component_params = compress_zlibng_register,
    .pmix_mca_query_component = compress_zlibng_query
};
PMIX_MCA_BASE_COMPONENT_INIT(pmix, pcompress, zlibng)

static int compress_zlibng_register(void)
{
    pmix_mca_base_component_var_register(&pmix_mca_pcompress_zlibng_component, "level",
                                         "Compression level passed to zlib-ng (0 is none, "
                                         "1 is fastest, 9 is smallest; the default of 2 suits "
                                         "the large payloads this framework compresses and "
                                         "matches what zlib produces at its level 1)",
                                         PMIX_MCA_BASE_VAR_TYPE_INT,
                                         &pmix_pcompress_zlibng_level);
    return PMIX_SUCCESS;
}

static int compress_zlibng_query(pmix_mca_base_module_t **module, int *priority)
{
    *module = (pmix_mca_base_module_t *) &pmix_pcompress_zlibng_module;
    *priority = 75;

    return PMIX_SUCCESS;
}
