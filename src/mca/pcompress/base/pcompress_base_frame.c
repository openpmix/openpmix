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

/* The two decompress stubs clear their output parameters rather than leaving
 * them alone. A caller is required to check the bool and every one of them
 * does, so nothing depends on this - but all four real components clear
 * theirs, and a slot whose five implementations disagree about what they
 * leave behind on refusal is a trap for whoever writes the sixth. */
static bool decompress_block(uint8_t **outbytes, size_t *outlen, const uint8_t *inbytes, size_t len)
{
    (void) inbytes;
    (void) len;
    *outbytes = NULL;
    *outlen = 0;
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
    (void) inbytes;
    (void) len;
    *outstring = NULL;
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

/* The base default module, stated once and used twice: `pmix_compress` starts
 * out as it, select() overwrites that with the winning component's module,
 * and close() puts it back (see below). A macro rather than one object copied
 * into the other because a const struct is not a constant expression, so it
 * cannot initialize a global. */
#define PCOMPRESS_BASE_DEFAULT_MODULE                        \
    {                                                        \
        .compress = compress_block,                          \
        .decompress = decompress_block,                      \
        .get_decompressed_size = get_decompressed_size,      \
        .compress_string = compress_string,                  \
        .decompress_string = decompress_string,              \
        .get_decompressed_strlen = get_decompressed_strlen   \
    }

static const pmix_compress_base_module_t base_default_module = PCOMPRESS_BASE_DEFAULT_MODULE;

pmix_compress_base_module_t pmix_compress = PCOMPRESS_BASE_DEFAULT_MODULE;

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
     * Measured on modex-shaped payloads the compressor declines outright
     * below about 256 bytes and starts returning real gains just above
     * it, for under 10us, so this is where the floor stops costing us
     * opportunity and starts saving pointless work. 4096 - the value
     * this carried until August 2026 - left every small-node job
     * shipping its whole modex raw. See "Choosing compress_limit" in
     * AGENTS.md before changing it. */
    pmix_compress_base.compress_limit = 256;
    (void) pmix_mca_base_var_register("pmix", "pcompress", "base", "limit",
                                      "Size in bytes below which data is left uncompressed",
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

    /* Put the no-op defaults back before the components go away. The close
     * below dlcloses the winning component - in a --enable-mca-dso build that
     * is every component - and pmix_compress is still holding six function
     * pointers into it. Clearing `selected` alone only guarantees that a
     * later select() will run; it does not guarantee that select() will find
     * anything, and one that selects nothing leaves the stale pointers in
     * place rather than replacing them. Restoring the defaults here makes
     * "no module" mean the same thing whether it was never selected or has
     * been closed. */
    pmix_compress = base_default_module;

    /* Close all available modules that are open */
    return pmix_mca_base_framework_components_close(&pmix_pcompress_base_framework, NULL);
}

PMIX_MCA_BASE_VERSIONED_FRAMEWORK_DECLARE(pmix, pcompress, "PCOMPRESS MCA", pmix_compress_base_register,
                                pmix_compress_base_open, pmix_compress_base_close,
                                pmix_mca_pcompress_base_static_components,
                                PMIX_MCA_BASE_FRAMEWORK_FLAG_DEFAULT);
