/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/**
 * @file
 *
 * LZ4 COMPRESS component
 *
 * Uses the lz4 library
 */

#ifndef MCA_COMPRESS_LZ4_EXPORT_H
#define MCA_COMPRESS_LZ4_EXPORT_H

#include "pmix_config.h"

#include "src/util/pmix_output.h"

#include "src/mca/mca.h"
#include "src/mca/pcompress/pcompress.h"

#if defined(c_plusplus) || defined(__cplusplus)
extern "C" {
#endif

/* Compression level handed to LZ4F_compressFrame().  Zero selects the plain
 * LZ4 codec, which is the whole point of this component; a positive value
 * switches the library to LZ4HC, which is a different (slower, denser)
 * algorithm reached through the same call.  A parameter rather than a
 * constant for the same reason as the other components - see the framework's
 * AGENTS.md. */
extern int pmix_pcompress_lz4_level;

/* the component must be visible data for the linker to find it */
PMIX_EXPORT extern pmix_mca_base_component_t pmix_mca_pcompress_lz4_component;
extern pmix_compress_base_module_t pmix_pcompress_lz4_module;

#if defined(c_plusplus) || defined(__cplusplus)
}
#endif

#endif /* MCA_COMPRESS_LZ4_EXPORT_H */
