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
 * Copyright (c) 2008      Sun Microsystems, Inc.  All rights reserved.
 * Copyright (c) 2016      Intel, Inc.  All rights reserved.
 * Copyright (c) 2022      Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/** @file:
 *
 */

#ifndef _PMIX_PARSE_OPTIONS_H_
#define _PMIX_PARSE_OPTIONS_H_

#include "src/include/pmix_config.h"

BEGIN_C_DECLS

/**
 * Expand a comma-delimited list of numbers and ranges into an argv.
 *
 * "1,3-5,9" appends "1", "3", "4", "5", "9" to *output.  The input is
 * not modified.  Elements are *appended*, so an *output that already
 * holds values keeps them - the caller owns the array and frees it with
 * PMIx_Argv_free().
 *
 * Three pieces of syntax are not obvious:
 *
 * - "-1" anywhere in the list is a wildcard meaning "everything".  It
 *   *discards* whatever *output already held and replaces it with the
 *   single element "-1", and the rest of the input is not read.
 * - a trailing "!" is a flag rather than a value: it is stripped from
 *   the input, and the element "BANG" is appended after everything
 *   else.  It is the caller's to interpret.
 * - a range whose end is below its start ("5-3") expands to nothing,
 *   silently.
 *
 * An element that is not a whole number an int can hold - "abc", "3x",
 * "4294967295" - is reported on the default output stream and skipped;
 * the rest of the list is still expanded.  Note that this reporting is
 * all the caller gets: the function returns void, so it also cannot
 * tell the caller that an allocation failed.  A NULL input is a no-op.
 */
PMIX_EXPORT void pmix_util_parse_range_options(char *input, char ***output);

/**
 * Split a comma-delimited list of ranges into parallel start/end arrays.
 *
 * "3-5,7" appends "3" and "7" to *startpts and "5" and "7" to *endpts -
 * a bare value is its own start and end.  Unlike
 * pmix_util_parse_range_options() the values are not expanded and not
 * parsed as numbers; they are handed back as the strings they were.
 *
 * The two arrays are parallel and the caller is entitled to that: entry
 * n of one describes the same range as entry n of the other.  Both are
 * appended to, both belong to the caller, and both are freed with
 * PMIx_Argv_free().  An element that is neither a value nor a pair is
 * reported on the default output stream and contributes to neither
 * array.  A NULL input is a no-op.
 */
PMIX_EXPORT void pmix_util_get_ranges(char *inp, char ***startpts, char ***endpts);

END_C_DECLS
#endif
