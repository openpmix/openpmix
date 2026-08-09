/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "pmix_config.h"

#include "compress_zstd.h"
#include "pmix_common.h"
#include "src/mca/pcompress/base/base.h"

/*
 * Public string for version number
 */
const char *pmix_compress_zstd_component_version_string
    = "PMIX COMPRESS zstd MCA component version " PMIX_VERSION;

/* Level 3 is zstd's own default and is the right one here.  Measured through
 * PMIx on a 25.6 MB aggregated modex, single-threaded: level 3 reaches a ratio
 * of 0.627 at 434 MB/s, against zlib's 0.649 at 103 MB/s (level 1) and 0.638 at
 * 54 MB/s (level 9).  So it is both smaller and several times faster than
 * anything the zlib components offer, which is why this one can be ranked above
 * them without asking anyone to retune.  Like theirs, it is a parameter rather
 * than a constant - see the framework AGENTS.md. */
int pmix_pcompress_zstd_level = 3;

/*
 * Local functionality
 */
static int compress_zstd_register(void);
static int compress_zstd_query(pmix_mca_base_module_t **module, int *priority);

/*
 * Instantiate the public struct with all of our public information
 * and pointer to our public functions in it
 */
PMIX_EXPORT pmix_mca_base_component_t pmix_mca_pcompress_zstd_component = {
    /* Handle the general mca_component_t struct containing
     *  meta information about the component zstd
     */
    PMIX_COMPRESS_BASE_VERSION_3_0_0,

    /* Component name and version */
    .pmix_mca_component_name = "zstd",
    PMIX_MCA_BASE_MAKE_VERSION(component, PMIX_MAJOR_VERSION, PMIX_MINOR_VERSION,
                               PMIX_RELEASE_VERSION),

    /* Component open and close functions */
    .pmix_mca_register_component_params = compress_zstd_register,
    .pmix_mca_query_component = compress_zstd_query
};
PMIX_MCA_BASE_COMPONENT_INIT(pmix, pcompress, zstd)

static int compress_zstd_register(void)
{
    pmix_mca_base_component_var_register(&pmix_mca_pcompress_zstd_component, "level",
                                         "Compression level passed to zstd (1 is fastest, "
                                         "19 is smallest; the default of 3 suits the large "
                                         "payloads this framework compresses)",
                                         PMIX_MCA_BASE_VAR_TYPE_INT,
                                         &pmix_pcompress_zstd_level);
    return PMIX_SUCCESS;
}

static int compress_zstd_query(pmix_mca_base_module_t **module, int *priority)
{
    *module = (pmix_mca_base_module_t *) &pmix_pcompress_zstd_module;
    /* Above zlibng (75) and zlib (50).  Like theirs, this query gates on
     * nothing: a component is compiled only if its configure.m4 found the
     * library, so an unavailable one is simply not in the build. */
    *priority = 90;

    return PMIX_SUCCESS;
}
