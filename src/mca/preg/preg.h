/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2007-2008 Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2015-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2015      Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2021-2025 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/**
 * @file
 *
 * This interface is for regex support. This is a multi-select framework.
 *
 * Available plugins may be defined at runtime via the typical MCA parameter
 * syntax.
 */

#ifndef PMIX_PREG_H
#define PMIX_PREG_H

#include "src/include/pmix_config.h"

#include "src/mca/base/pmix_mca_base_framework.h"
#include "src/mca/base/pmix_mca_base_var.h"
#include "src/mca/bfrops/bfrops_types.h"
#include "src/mca/mca.h"

BEGIN_C_DECLS

/******    MODULE DEFINITION    ******/

/* The encoded form of a node or process map is fundamentally
 * "bytes + length + a type tag", which is what pmix_regex2_t carries. A
 * component therefore implements exactly two operations: encode a
 * delimited list of values into a pmix_regex2_t, and expand one back.
 * The deprecated char* API (PMIx_generate_regex / PMIx_generate_ppn and
 * the parse side that goes with it) is a serialization of that same
 * struct, and is handled entirely in the framework base - see
 * preg_base_legacy.c. Components know nothing about it.
 */

/* Encode the delimited input list into a pmix_regex2_t. The module sets
 * regex->type to its own name, stores the encoded bytes in regex->bytes,
 * and sets regex->len accordingly. The encoding is opaque to the caller
 * and need not be a NULL-terminated string.
 *
 * Note that this operation is indifferent to what the values mean: the
 * same call encodes a comma-delimited node list and a semicolon-delimited
 * process map.
 *
 * Returns PMIX_ERR_TAKE_NEXT_OPTION if the module cannot handle the input.
 */
typedef pmix_status_t (*pmix_preg_base_module_generate_regex_fn_t)(const char *input,
                                                                    pmix_info_t info[],
                                                                    size_t ninfo,
                                                                    pmix_regex2_t *regex);

/* Expand a pmix_regex2_t (as produced by generate_regex) back into the
 * delimited string that was originally encoded. The caller splits it.
 * Returns PMIX_ERR_TAKE_NEXT_OPTION if the module does not recognize
 * the regex->type value.
 */
typedef pmix_status_t (*pmix_preg_base_module_parse_regex_fn_t)(const pmix_regex2_t *regex,
                                                                 pmix_info_t info[], size_t ninfo,
                                                                 char **output);

/**
 * Base structure for a PREG module
 */
typedef struct {
    char *name;
    pmix_preg_base_module_generate_regex_fn_t generate_regex;
    pmix_preg_base_module_parse_regex_fn_t parse_regex;
} pmix_preg_module_t;

/**
 * The framework-level API, instantiated once as the global pmix_preg.
 * Every entry points at a pmix_preg_base_* function; there is no
 * component dispatch table here. The first four entries are the
 * deprecated char* interface, which the base implements on top of the
 * pmix_regex2_t operations above.
 */
typedef struct {
    pmix_status_t (*generate_node_regex)(const char *input, char **regex);
    pmix_status_t (*generate_ppn)(const char *input, char **ppn);
    pmix_status_t (*parse_nodes)(const char *regexp, char ***names);
    pmix_status_t (*parse_procs)(const char *regexp, char ***procs);
    pmix_status_t (*copy)(char **dest, size_t *len, const char *input);
    pmix_status_t (*pack)(pmix_buffer_t *buffer, const char *regex);
    pmix_status_t (*unpack)(pmix_buffer_t *buffer, char **regex);
    pmix_status_t (*release)(char *regexp);
    pmix_status_t (*generate_regex)(const char *input, pmix_info_t info[],
                                    size_t ninfo, pmix_regex2_t *regex);
    pmix_status_t (*parse_regex)(const pmix_regex2_t *regex, pmix_info_t info[],
                                 size_t ninfo, char **output);
} pmix_preg_api_t;

/* we just use the standard component definition */

PMIX_EXPORT extern pmix_preg_api_t pmix_preg;

/* The preg framework interface version. It is stated here and nowhere
 * else: components stamp it into their struct with
 * PMIX_MCA_BASE_VERSION(preg), and the framework's declaration reaches
 * the same three by pasting its name, so the two cannot drift apart.
 * Bump it on any change to the module interface that a component built
 * against the previous one would not survive. */
#define PMIX_MCA_preg_MAJOR_VERSION   2
#define PMIX_MCA_preg_MINOR_VERSION   0
#define PMIX_MCA_preg_RELEASE_VERSION 0

END_C_DECLS

#endif
