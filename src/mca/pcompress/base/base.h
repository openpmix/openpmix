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
 * An output buffer for a decompressor, grown as output arrives rather
 * than allocated up front at the size the blob's length prefix claims.
 *
 * The prefix is a claim about bytes that generally came off a peer's
 * wire, so it must not decide how much memory is committed before a
 * single byte has been decoded. A buffer grown this way never holds
 * more than about twice what the payload really produced, and never
 * more than one byte beyond the claim - a decoder that reaches that
 * byte has produced more than was promised, and is refused.
 *
 * Use:
 *   start()  - allocate a first guess, sized from the compressed length
 *   grow()   - when the decoder has filled the buffer; false means the
 *              output passed the claim, or the allocation failed
 *   finish() - hand back the buffer if exactly `claim` bytes were
 *              produced; it is NUL-terminated at [claim], so the string
 *              path needs no further room
 *   release() - on any failure, including a failed finish()
 */
typedef struct {
    uint8_t *bytes;
    size_t size;    // bytes allocated
    size_t ceiling; // the most it may ever grow to: claim + 1
} pmix_compress_base_outbuf_t;

PMIX_EXPORT bool pmix_compress_base_outbuf_start(pmix_compress_base_outbuf_t *buf, size_t inlen,
                                                 uint32_t claim);
PMIX_EXPORT bool pmix_compress_base_outbuf_grow(pmix_compress_base_outbuf_t *buf);
PMIX_EXPORT bool pmix_compress_base_outbuf_finish(pmix_compress_base_outbuf_t *buf,
                                                  size_t produced, uint32_t claim,
                                                  uint8_t **outbytes);
PMIX_EXPORT void pmix_compress_base_outbuf_release(pmix_compress_base_outbuf_t *buf);

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
