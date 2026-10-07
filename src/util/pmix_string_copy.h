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
 * Copyright (c) 2019-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2020      Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2021-2026 Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#ifndef PMIX_STRING_COPY_H
#define PMIX_STRING_COPY_H

#include "src/include/pmix_config.h"
#include "pmix_common.h"

#ifdef HAVE_SYS_TYPES_H
#    include <sys/types.h>
#endif
#ifdef HAVE_FCNTL_H
#    include <fcntl.h>
#endif
#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif

BEGIN_C_DECLS

/**
 * Do a "safe" string copy (i.e., guarantee to \0-terminate the
 * destination string), and assert() fail if the copy length is too
 * large (because we assume it is a programmer error).
 *
 * @param dest Destination string buffer.
 * @param src Source string buffer.
 * @param dest_len Length of the destination string buffer.
 *
 * This function is similar to, but different than, strcpy() and
 * strncpy().
 *
 * It is invalid to pass NULL for either dest or src.
 *
 * If dest_len is larger than
 * PMIX_MAX_SIZE_ALLOWED_BY_PMIX_STRING_COPY, we assume that this is
 * a programmer error (because PMIX does not generally need to do
 * large string copies), and will assert() fail / abort.
 *
 * There is no return value.
 *
 * This function will essentially do the same thing as strncpy(),
 * except that a) it will guarantee to to terminate the destination
 * string with a \0, and b) it will not \0-pad to the right.
 * Specifically:
 *
 * - If the length of the source string is less than (len), the entire
 *   source string will be copied to the destination, including the
 *   \0.
 * - If the length of the source string is greater than (len), then
 *   (len-1) characters of the source string will be copied to the
 *   destination, and dest[len-1] will be set to '\0'.
 */
PMIX_EXPORT void pmix_string_copy(char *dest, const char *src, size_t dest_len)
    __pmix_attribute_nonnull__(1) __pmix_attribute_nonnull__(2);

/**
 * Max dest_size allowed by pmix_string_copy().
 *
 * See the description of pmix_string_copy() for an explanation.
 */
#define PMIX_MAX_SIZE_ALLOWED_BY_PMIX_STRING_COPY (128 * 1024)

/**
 * Read one line from an open stream, without its trailing newline.
 *
 * @param fp An open, readable stream. It is invalid to pass NULL.
 * @param failed If not NULL, set to true when the read stopped short of
 *        the end of the stream, and to false otherwise.
 * @retval A fresh allocation the CALLER frees.
 * @retval NULL nothing more could be read.
 *
 * A line of any length is returned whole: it is read a character at a
 * time until its newline arrives. Three things about this are easy to
 * assume and wrong:
 *
 * - **A returned line is not necessarily newline-terminated in the
 *   file.** The newline is stripped when there is one, and there is none
 *   for the last line of a file that does not end in one.
 *
 * - **NULL is not specifically end-of-file.** A read error, a NUL byte
 *   in the line and a failed allocation also return NULL, and set
 *   "failed". A caller that stops at the first NULL without checking it
 *   takes whatever it read before the failure for the whole file.
 *
 * - **A NUL byte is a failure, not the end of the line.** The line is
 *   handed back as a C string, so the rest of it would silently vanish;
 *   a line-oriented text file has no use for one.
 */
PMIX_EXPORT char *pmix_getline(FILE *fp, bool *failed);


END_C_DECLS

#endif /* PMIX_STRING_COPY_H */
