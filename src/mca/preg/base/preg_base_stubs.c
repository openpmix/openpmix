/*
 * Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2006 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2015-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2021-2022 Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "src/include/pmix_config.h"

#include <stdio.h>
#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif

#include "src/client/pmix_client_ops.h"
#include "src/include/pmix_globals.h"
#include "src/mca/bfrops/bfrops.h"
#include "src/mca/bfrops/base/base.h"
#include "src/mca/preg/base/base.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_error.h"

/* The deprecated char* entry points below are serializations of the
 * pmix_regex2_t operations that follow them: generate encodes, parse
 * decodes and then splits, and copy/pack/unpack/release only need to
 * know how long the serialized form is. Components implement none of
 * this - see preg_base_legacy.c for the layouts. */

static pmix_status_t generate_legacy(const char *input, char **regex)
{
    pmix_regex2_t r2 = PMIX_REGEX2_STATIC_INIT;
    pmix_status_t rc;
    size_t len;

    rc = pmix_preg_base_generate_regex(input, NULL, 0, &r2);
    if (PMIX_SUCCESS == rc) {
        rc = pmix_preg_base_legacy_encode(&r2, regex, &len);
        PMIx_Regex2_destruct(&r2);
        if (PMIX_SUCCESS == rc) {
            return PMIX_SUCCESS;
        }
    }

    /* no component could encode it, or its encoding has no deprecated
     * form - ship the list uncompressed, as we always have */
    *regex = strdup(input);
    if (NULL == *regex) {
        return PMIX_ERR_NOMEM;
    }
    return PMIX_SUCCESS;
}

static pmix_status_t parse_legacy(const char *regexp, char ***result, int delimiter)
{
    pmix_regex2_t r2 = PMIX_REGEX2_STATIC_INIT;
    pmix_status_t rc;
    char *decoded;
    size_t total;

    rc = pmix_preg_base_legacy_decode(regexp, SIZE_MAX, &r2, &total);
    if (PMIX_SUCCESS == rc) {
        rc = pmix_preg_base_parse_regex(&r2, NULL, 0, &decoded);
        if (PMIX_SUCCESS == rc) {
            *result = PMIx_Argv_split(decoded, delimiter);
            free(decoded);
            return PMIX_SUCCESS;
        }
        /* we recognized the framing but no active component owns the
         * encoding - splitting the payload would yield nonsense */
        return PMIX_ERR_NOT_SUPPORTED;
    }

    /* not an encoded regex at all, so it must be the plain list */
    *result = PMIx_Argv_split(regexp, delimiter);
    return PMIX_SUCCESS;
}

pmix_status_t pmix_preg_base_generate_node_regex(const char *input, char **regex)
{
    return generate_legacy(input, regex);
}

pmix_status_t pmix_preg_base_generate_ppn(const char *input, char **ppn)
{
    return generate_legacy(input, ppn);
}

pmix_status_t pmix_preg_base_parse_nodes(const char *regexp, char ***names)
{
    return parse_legacy(regexp, names, ',');
}

pmix_status_t pmix_preg_base_parse_procs(const char *regexp, char ***procs)
{
    return parse_legacy(regexp, procs, ';');
}

pmix_status_t pmix_preg_base_copy(char **dest, size_t *len, const char *input)
{
    pmix_regex2_t r2 = PMIX_REGEX2_STATIC_INIT;
    size_t total;

    if (PMIX_SUCCESS != pmix_preg_base_legacy_decode(input, SIZE_MAX, &r2, &total)) {
        /* it must just be a string */
        *dest = strdup(input);
        if (NULL == *dest) {
            return PMIX_ERR_NOMEM;
        }
        *len = strlen(input) + 1;
        return PMIX_SUCCESS;
    }

    *dest = calloc(total, sizeof(char));
    if (NULL == *dest) {
        return PMIX_ERR_NOMEM;
    }
    memcpy(*dest, input, total);
    *len = total;
    return PMIX_SUCCESS;
}

pmix_status_t pmix_preg_base_pack(pmix_buffer_t *buffer, const char *input)
{
    pmix_regex2_t r2 = PMIX_REGEX2_STATIC_INIT;
    pmix_status_t rc;
    size_t total;
    char *ptr;

    if (PMIX_SUCCESS != pmix_preg_base_legacy_decode(input, SIZE_MAX, &r2, &total)) {
        /* just pack it as a string */
        PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, buffer, input, 1, PMIX_STRING);
        return rc;
    }

    /* the serialized form carries its own length, so it goes onto the
     * wire verbatim - no bfrops framing */
    ptr = pmix_bfrop_buffer_extend(buffer, total);
    if (NULL == ptr) {
        return PMIX_ERR_NOMEM;
    }
    memcpy(ptr, input, total);
    buffer->bytes_used += total;
    buffer->pack_ptr += total;
    return PMIX_SUCCESS;
}

pmix_status_t pmix_preg_base_unpack(pmix_buffer_t *buffer, char **regex)
{
    pmix_regex2_t r2 = PMIX_REGEX2_STATIC_INIT;
    pmix_status_t rc;
    size_t avail, total;
    int32_t cnt = 1;
    char *output;

    /* the same "bytes remaining to unpack" the bfrops guard uses */
    if (buffer->pack_ptr < buffer->unpack_ptr) {
        return PMIX_ERR_UNPACK_READ_PAST_END_OF_BUFFER;
    }
    avail = (size_t) (buffer->pack_ptr - buffer->unpack_ptr);

    if (0 == avail ||
        PMIX_SUCCESS != pmix_preg_base_legacy_decode(buffer->unpack_ptr, avail, &r2, &total)) {
        /* must just be a string */
        PMIX_BFROPS_UNPACK(rc, pmix_globals.mypeer, buffer, regex, &cnt, PMIX_STRING);
        return rc;
    }

    output = (char *) malloc(total);
    if (NULL == output) {
        *regex = NULL;
        return PMIX_ERR_NOMEM;
    }
    memcpy(output, buffer->unpack_ptr, total);
    buffer->unpack_ptr += total;
    *regex = output;
    return PMIX_SUCCESS;
}

pmix_status_t pmix_preg_base_release(char *regexp)
{
    pmix_regex2_t r2 = PMIX_REGEX2_STATIC_INIT;
    size_t total;

    if (NULL == regexp) {
        return PMIX_SUCCESS;
    }
    if (PMIX_SUCCESS != pmix_preg_base_legacy_decode(regexp, SIZE_MAX, &r2, &total)) {
        return PMIX_ERR_BAD_PARAM;
    }
    free(regexp);
    return PMIX_SUCCESS;
}

pmix_status_t pmix_preg_base_parse_regex(const pmix_regex2_t *regex,
                                          pmix_info_t info[], size_t ninfo,
                                          char **output)
{
    pmix_preg_base_active_module_t *active;

    PMIX_LIST_FOREACH (active, &pmix_preg_globals.actives, pmix_preg_base_active_module_t) {
        if (NULL != active->module->parse_regex) {
            if (PMIX_SUCCESS == active->module->parse_regex(regex, info, ninfo, output)) {
                return PMIX_SUCCESS;
            }
        }
    }
    return PMIX_ERR_NOT_SUPPORTED;
}

pmix_status_t pmix_preg_base_generate_regex(const char *input,
                                             pmix_info_t info[], size_t ninfo,
                                             pmix_regex2_t *regex)
{
    pmix_preg_base_active_module_t *active;
    pmix_regex2_t candidate = PMIX_REGEX2_STATIC_INIT;
    pmix_regex2_t best = PMIX_REGEX2_STATIC_INIT;

    PMIX_LIST_FOREACH (active, &pmix_preg_globals.actives, pmix_preg_base_active_module_t) {
        if (NULL == active->module->generate_regex) {
            continue;
        }
        if (PMIX_SUCCESS != active->module->generate_regex(input, info, ninfo, &candidate)) {
            continue;
        }
        /* keep this result if it is the first or smaller than the current best */
        if (NULL == best.bytes || candidate.len < best.len) {
            /* release the previous best if there was one */
            if (NULL != best.type) {
                free(best.type);
            }
            if (NULL != best.bytes) {
                free(best.bytes);
            }
            best = candidate;
        } else {
            /* discard this candidate */
            if (NULL != candidate.type) {
                free(candidate.type);
            }
            if (NULL != candidate.bytes) {
                free(candidate.bytes);
            }
        }
        candidate = (pmix_regex2_t) PMIX_REGEX2_STATIC_INIT;
    }

    if (NULL == best.bytes) {
        return PMIX_ERR_NOT_SUPPORTED;
    }

    *regex = best;
    return PMIX_SUCCESS;
}
