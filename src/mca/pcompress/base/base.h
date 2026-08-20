/*
 * Copyright (c) 2004-2010 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 *
 * Copyright (c) 2019      Intel, Inc.  All rights reserved.
 * Copyright (c) 2021-2022 Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */
#ifndef PMIX_COMPRESS_BASE_H
#define PMIX_COMPRESS_BASE_H

#include "pmix_config.h"
#include "src/mca/pcompress/pcompress.h"
#include "src/util/pmix_environ.h"

#include "src/mca/base/pmix_base.h"

/*
 * Global functions for MCA overall COMPRESS
 */

#if defined(c_plusplus) || defined(__cplusplus)
extern "C" {
#endif

/* A value is never left holding a PMIX_COMPRESSED_STRING: there is no
 * public way for an application to expand one, so pmix_bfrops_base_unpack_val()
 * expands every one it receives and hands back a PMIX_STRING. The two
 * macros that used to live here - PMIX_STRING_SIZE_CHECK, which decided
 * whether PMIx_Put should compress a string before storing it, and
 * PMIX_VALUE_COMPRESSED_STRING_UNPACK, which was meant to undo that at
 * the far end and had lost its last call site - are gone with that
 * decision. */

typedef struct {
    size_t compress_limit;
    bool selected;
    bool silent;
} pmix_compress_base_t;

PMIX_EXPORT extern pmix_compress_base_t pmix_compress_base;

/**
 * Select an available component.
 *
 * Selecting nothing is deliberately NOT an error: the caller keeps the
 * base default no-op module and ships its data uncompressed. The only
 * failure this can report is a non-SUCCESS return from the winning
 * module's init(), which no module implements today.
 *
 * @retval PMIX_SUCCESS a component was selected, or none was and the
 *                      base default stubs remain in place
 * @retval other        the selected module's init() failed; note that
 *                      pmix_init.c treats this as fatal to library init
 */
PMIX_EXPORT int pmix_compress_base_select(void);

/**
 * Globals
 */
PMIX_EXPORT extern pmix_mca_base_framework_t pmix_pcompress_base_framework;
PMIX_EXPORT extern pmix_compress_base_module_t pmix_compress;

#if defined(c_plusplus) || defined(__cplusplus)
}
#endif

#endif /* PMIX_COMPRESS_BASE_H */
