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
 * Copyright (c) 2015-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2021-2022 Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/**
 * @file
 *
 * Returns an OS-independant basename() of a given filename.
 */

#ifndef PMIX_BASENAME_H
#define PMIX_BASENAME_H

#include "src/include/pmix_config.h"
#include "pmix_common.h"

BEGIN_C_DECLS

/**
 * Return the basename of a filename.
 *
 * @param filename The filename to examine
 *
 * @returns A string containing the basename, or NULL if there is an error
 *
 * The contents of the \em filename parameter are unchanged.  This
 * function returns a new string containing the basename of the
 * filename (which must be eventually freed by the caller), or
 * NULL if there is an error.  Trailing "/" characters in the
 * filename do not count, unless it is in the only part of the
 * filename (e.g., "/" or "C:\").
 *
 * The path separator is "/".  For example:
 *
 * foo.txt returns "foo.txt"
 *
 * /foo/bar/baz returns "baz"
 *
 * /yow.c returns "yow.c"
 *
 * /foo/bar/ returns "bar"
 *
 * / returns "/"
 *
 * The caller is responsible for freeing the returned string.
 */
PMIX_EXPORT char *
pmix_basename(const char *filename) __pmix_attribute_malloc__ __pmix_attribute_warn_unused_result__;

/**
 * Return the dirname of a filename.
 *
 * @param filename The filename to examine
 *
 * @returns A string containing the dirname, or NULL if there is an error
 *
 * The contents of the \em filename parameter are unchanged.  This
 * function returns a new string containing the dirname of the
 * filename (which must be eventually freed by the caller), or
 * NULL if there is an error.  Trailing "/" characters in the
 * filename do not count, unless it is in the only part of the
 * filename (e.g., "/" or "C:\").
 *
 * The path separator is "/".  For example:
 *
 * foo.txt returns "."
 *
 * /foo/bar/baz returns "/foo/bar"
 *
 * /foo/bar/ returns "/foo"
 *
 * /yow.c returns "/"
 *
 * / returns "/"
 *
 * The caller is responsible for freeing the returned string.
 */
PMIX_EXPORT char *
pmix_dirname(const char *filename) __pmix_attribute_malloc__ __pmix_attribute_warn_unused_result__;

END_C_DECLS

#endif /* PMIX_BASENAME_H */
