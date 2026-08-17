/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2004-2010 The Trustees of Indiana University.
 *                         All rights reserved.
 * Copyright (c) 2011-2013 Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2015      Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2019-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "pmix_config.h"

#include <string.h>

#include "src/include/pmix_globals.h"
#include "src/mca/base/pmix_base.h"
#include "src/mca/pcompress/base/base.h"
#include "src/mca/pcompress/base/static-components.h"
#include "src/mca/ptl/ptl_types.h"
#include "src/util/pmix_show_help.h"

/*
 * Globals
 */
static bool compress_block(const uint8_t *inblock, size_t size, uint8_t **outbytes, size_t *nbytes)
{
    (void) inblock;
    (void) size;
    (void) outbytes;
    (void) nbytes;
    if (!pmix_compress_base.silent && !PMIX_PEER_IS_CLIENT(pmix_globals.mypeer)) {
        pmix_show_help("help-pcompress.txt", "unavailable", true);
        pmix_compress_base.silent = true;
    }
    return false;
}

static bool decompress_block(uint8_t **outbytes, size_t *outlen, const uint8_t *inbytes, size_t len)
{
    (void) outbytes;
    (void) outlen;
    (void) inbytes;
    (void) len;
    return false;
}

static bool compress_string(char *instring, uint8_t **outbytes, size_t *nbytes)
{
    (void) instring;
    (void) outbytes;
    (void) nbytes;
    if (!pmix_compress_base.silent && !PMIX_PEER_IS_CLIENT(pmix_globals.mypeer)) {
        pmix_show_help("help-pcompress.txt", "unavailable", true);
        pmix_compress_base.silent = true;
    }
    return false;
}

static bool decompress_string(char **outstring, uint8_t *inbytes, size_t len)
{
    (void) outstring;
    (void) inbytes;
    (void) len;
    return false;
}

/* These read the uncompressed length from the leading 4 bytes of a
 * compressed blob (see the shared blob format); they need no compression
 * library, so the base default implements them too. This keeps the
 * PMIX_COMPRESSED_* size paths in bfrops safe on a host that built no
 * compression component but still receives a compressed blob from a
 * peer that did. */
static size_t get_decompressed_size(const pmix_byte_object_t *bo)
{
    uint32_t len = 0;

    if (NULL == bo || NULL == bo->bytes || bo->size < sizeof(uint32_t)) {
        return 0;
    }
    /* the first 4 bytes contain the uncompressed size */
    memcpy(&len, bo->bytes, sizeof(uint32_t));
    return (size_t) len;
}

static size_t get_decompressed_strlen(const pmix_byte_object_t *bo)
{
    size_t len;

    /* add one for the NUL terminator, matching PMIX_STRING accounting */
    len = get_decompressed_size(bo);
    if (0 == len) {
        return 0;
    }
    return len + 1;
}

pmix_compress_base_module_t pmix_compress = {
    .compress = compress_block,
    .decompress = decompress_block,
    .get_decompressed_size = get_decompressed_size,
    .compress_string = compress_string,
    .decompress_string = decompress_string,
    .get_decompressed_strlen = get_decompressed_strlen
};

pmix_compress_base_t pmix_compress_base = {
    .compress_limit = 0,
    .selected = false,
    .silent = false
};

static int pmix_compress_base_register(pmix_mca_base_register_flag_t flags)
{
    (void) flags;
    /* Inputs below this size are never offered to the compressor. The
     * floor exists to keep us from spending CPU where there is nothing
     * to gain - it is not a correctness guard, because every component
     * declines any result that is not strictly smaller than its input.
     * A fence bucket from a node running four processes is around a
     * kilobyte and still deflates to well under two-thirds of that for
     * about 13us, so 4096 - the value this carried until August 2026 -
     * left every small-node job shipping its modex raw. See the
     * "Choosing compress_limit" note in AGENTS.md before changing it;
     * this same threshold decides when PMIx_Put converts a large string
     * value into a PMIX_COMPRESSED_STRING. */
    pmix_compress_base.compress_limit = 1024;
    (void) pmix_mca_base_var_register("pmix", "pcompress", "base", "limit",
                                      "Size in bytes below which data is left uncompressed. "
                                      "Also the length above which PMIx_Put converts a string "
                                      "value into a PMIX_COMPRESSED_STRING.",
                                      PMIX_MCA_BASE_VAR_TYPE_SIZE_T,
                                      &pmix_compress_base.compress_limit);

    pmix_compress_base.silent = false;
    (void) pmix_mca_base_var_register("pmix", "pcompress", "base", "silence_warning",
                                      "Do not warn if compression unavailable",
                                      PMIX_MCA_BASE_VAR_TYPE_BOOL,
                                      &pmix_compress_base.silent);
    return PMIX_SUCCESS;
}

/**
 * Function for finding and opening either all MCA components,
 * or the one that was specifically requested via a MCA parameter.
 */
static int pmix_compress_base_open(pmix_mca_base_open_flag_t flags)
{
    /* Open up all available components */
    return pmix_mca_base_framework_components_open(&pmix_pcompress_base_framework, flags);
}

static int pmix_compress_base_close(void)
{
    pmix_compress_base.selected = false;
    /* Call the component's finalize routine */
    if (NULL != pmix_compress.finalize) {
        pmix_compress.finalize();
    }

    /* Close all available modules that are open */
    return pmix_mca_base_framework_components_close(&pmix_pcompress_base_framework, NULL);
}

PMIX_MCA_BASE_VERSIONED_FRAMEWORK_DECLARE(pmix, pcompress, "PCOMPRESS MCA", pmix_compress_base_register,
                                pmix_compress_base_open, pmix_compress_base_close,
                                pmix_mca_pcompress_base_static_components,
                                PMIX_MCA_BASE_FRAMEWORK_FLAG_DEFAULT);
