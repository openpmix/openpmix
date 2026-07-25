/*
 * Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2005 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2016-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2021-2022 Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/** @file */

#ifndef PMIX_UTIL_KEYVAL_PARSE_H
#define PMIX_UTIL_KEYVAL_PARSE_H

#include "src/include/pmix_config.h"

BEGIN_C_DECLS

PMIX_EXPORT extern int pmix_util_keyval_parse_lineno;

/**
 * Callback triggered for each key = value pair
 *
 * Callback triggered from pmix_util_keyval_parse for each key = value
 * pair.  Both key and value will be pointers into static buffers.
 * The buffers must not be free()ed and contents may be overwritten
 * immediately after the callback returns.  The \c file and \c lineno
 * parameters identify where the pair came from, and \c cbdata is the
 * opaque pointer the caller handed to the parser - the callback must
 * take its context from these parameters rather than from any state
 * shared with the caller.
 */
typedef void (*pmix_keyval_parse_fn_t)(const char *file, int lineno,
                                       const char *name,
                                       const char *value,
                                       void *cbdata);

/**
 * Parse \c filename, made up of key = value pairs.
 *
 * Parse \c filename, made up of key = value pairs.  For each line
 * that appears to contain a key = value pair, \c callback will be
 * called exactly once, with \c cbdata passed through untouched.  In a
 * multithreaded context, calls to pmix_util_keyval_parse() will
 * serialize multiple calls.
 */
PMIX_EXPORT int pmix_util_keyval_parse(const char *filename, pmix_keyval_parse_fn_t callback,
                                       void *cbdata);

PMIX_EXPORT int pmix_util_keyval_parse_init(void);

PMIX_EXPORT void pmix_util_keyval_parse_finalize(void);

PMIX_EXPORT int pmix_util_keyval_save_internal_envars(pmix_keyval_parse_fn_t callback,
                                                      void *cbdata);

END_C_DECLS

#endif
