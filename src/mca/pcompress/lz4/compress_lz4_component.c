/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "pmix_config.h"

#include "compress_lz4.h"
#include "pmix_common.h"
#include "src/mca/pcompress/base/base.h"

/*
 * Public string for version number
 */
const char *pmix_compress_lz4_component_version_string
    = "PMIX COMPRESS lz4 MCA component version " PMIX_VERSION;

/* Zero is LZ4's own fast mode and is the only level this component exists
 * for: it trades ratio for speed, which is the trade the other three
 * components do not offer.  A positive value hands the call to LZ4HC, which
 * is slower than zstd at a similar ratio and therefore not a useful place to
 * be - if you want density, use zstd.  Like the other components' levels it
 * is a parameter rather than a constant; see the framework AGENTS.md. */
int pmix_pcompress_lz4_level = 0;

/*
 * Local functionality
 */
static int compress_lz4_register(void);
static int compress_lz4_query(pmix_mca_base_module_t **module, int *priority);

/*
 * Instantiate the public struct with all of our public information
 * and pointer to our public functions in it
 */
PMIX_EXPORT pmix_mca_base_component_t pmix_mca_pcompress_lz4_component = {
    /* Handle the general mca_component_t struct containing
     *  meta information about the component lz4
     */
    PMIX_MCA_BASE_VERSION(pcompress),

    /* Component name and version */
    .pmix_mca_component_name = "lz4",
    PMIX_MCA_BASE_MAKE_VERSION(component, PMIX_MAJOR_VERSION, PMIX_MINOR_VERSION,
                               PMIX_RELEASE_VERSION),

    /* Component open and close functions */
    .pmix_mca_register_component_params = compress_lz4_register,
    .pmix_mca_query_component = compress_lz4_query
};
PMIX_MCA_BASE_COMPONENT_INIT(pmix, pcompress, lz4)

static int compress_lz4_register(void)
{
    pmix_mca_base_component_var_register(&pmix_mca_pcompress_lz4_component, "level",
                                         "Compression level passed to lz4 (0 selects the "
                                         "fast LZ4 codec this component exists for; a "
                                         "positive value switches to the slower, denser "
                                         "LZ4HC codec)",
                                         PMIX_MCA_BASE_VAR_TYPE_INT,
                                         &pmix_pcompress_lz4_level);
    return PMIX_SUCCESS;
}

static int compress_lz4_query(pmix_mca_base_module_t **module, int *priority)
{
    *module = (pmix_mca_base_module_t *) &pmix_pcompress_lz4_module;
    /* Above zlib (50), below zlibng (75) and zstd (90).
     *
     * Deliberately NOT the default where zstd is present.  This component is
     * the fastest and the least dense of the four, so which of the two should
     * be preferred is a bandwidth-against-CPU judgement that depends on the
     * link a payload will cross - not something to settle by ranking it first
     * and changing every existing build's behaviour.  It sits above zlib
     * because it beats zlib on both terms.  Select it explicitly with
     * "--pmixmca pcompress lz4"; re-rank it here if measurement on a real
     * fabric says it should win by default.
     *
     * Like the others, this query gates on nothing: a component is compiled
     * only if its configure.m4 found the library, so an unavailable one is
     * simply not in the build. */
    *priority = 60;

    return PMIX_SUCCESS;
}
