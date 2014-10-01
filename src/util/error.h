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
 * $COPYRIGHT$
 * 
 * Additional copyrights may follow
 * 
 * $HEADER$
 */

#ifndef PMIX_UTIL_ERROR_H
#define PMIX_UTIL_ERROR_H

#include "pmix_config.h"

#include "src/util/output.h"

BEGIN_C_DECLS

#define PMIX_ERROR_LOG(r) \
    pmix_output(0, "PMIX ERROR: %s in file %s at line %d", \
                pmix_strerror((r)), __FILE__, __LINE__);


/**
 * Prints error message for errnum on stderr
 *
 * Print the error message corresponding to the value of \c errnum and
 * writes it, followed by a newline, to the standard error file
 * descriptor.  If the argument \c msg is non-NULL, this string is
 * prepended to the message string and separated from it by a colon
 * and a space.  Otherwise, only the error message string is printed.
 *
 * If errnum is PMIX_ERR_IN_ERRNO, the system perror is called with
 * the argument \c msg.
 */
PMIX_DECLSPEC void pmix_perror(int errnum, const char *msg);

/**
 * Return string for given error message
 *
 * Accepts an error number argument \c errnum and returns a pointer to
 * the corresponding message string.  The result is returned in a
 * static buffer that should not be released with free().
 *
 * If errnum is \c PMIX_ERR_IN_ERRNO, the system strerror is called
 * with an argument of the current value of \c errno and the resulting
 * string is returned.
 *
 * If the errnum is not a known value, the returned value may be
 * overwritten by subsequent calls to pmix_strerror.
 */
PMIX_DECLSPEC const char *pmix_strerror(int errnum);

/**
 * Return string for given error message
 * 
 * Similar to pmix_strerror, but a buffer is passed in which is filled
 * with a string (up to buflen - 1 characters long) containing the
 * error message corresponding to \c errnum.  Unlike pmix_strerror(),
 * if an unknown value for \c errnum is passed, the returned buffer
 * will not be overwritten by subsequent calls to pmix_strerror_r().
 */
PMIX_DECLSPEC int pmix_strerror_r(int errnum, char *strerrbuf, size_t buflen);


typedef int (*pmix_err2str_fn_t)(int errnum, const char **str);

/**
 * \internal
 *
 * Register a handler for converting errnums to error strings
 *
 * Handlers will be invoked by pmix_perror() , pmix_strerror(), and
 * pmix_strerror_r() to return the appropriate values.
 *
 * \note A maximum of 5 converters can be registered.  The 6th
 * converter registration attempt will return PMIX_ERR_OUT_OF_RESOURCE
 */
PMIX_DECLSPEC int pmix_error_register(const char *project,
                                      int err_base, int err_max,
                                      pmix_err2str_fn_t converter);

END_C_DECLS

#endif /* PMIX_UTIL_ERROR_H */
