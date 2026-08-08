/*
 * Copyright (c) 2004-2010 The Trustees of Indiana University.
 *                         All rights reserved.
 * Copyright (c) 2019-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2022-2024 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/**
 * @file
 *
 * ZLIBNG COMPRESS component
 *
 * Uses the zlib-ng library
 */

#ifndef MCA_COMPRESS_ZLIBNG_EXPORT_H
#define MCA_COMPRESS_ZLIBNG_EXPORT_H

#include "pmix_config.h"

#include "src/util/pmix_output.h"

#include "src/mca/mca.h"
#include "src/mca/pcompress/pcompress.h"

#if defined(c_plusplus) || defined(__cplusplus)
extern "C" {
#endif

/* Compression level handed to deflateInit().  A parameter rather than a
 * constant: the level is the term that decides whether compressing a large
 * collective payload is worth doing at all, and the right value depends on the
 * link the result will cross.  Every component in this framework exposes it
 * the same way.  See the component's AGENTS.md. */
extern int pmix_pcompress_zlibng_level;

/* the component must be visible data for the linker to find it */
PMIX_EXPORT extern pmix_mca_base_component_t pmix_mca_pcompress_zlibng_component;
extern pmix_compress_base_module_t pmix_pcompress_zlibng_module;

#if defined(c_plusplus) || defined(__cplusplus)
}
#endif

#endif /* MCA_COMPRESS_ZLIBNG_EXPORT_H */
