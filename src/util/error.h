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
#include "src/api/pmix_common.h"
#include "src/util/output.h"

BEGIN_C_DECLS

#define PMIX_ERROR_LOG(r) \
    pmix_output(0, "PMIX ERROR: %s in file %s at line %d", \
                pmix_strerror((r)), __FILE__, __LINE__);

#define PMIX_REPORT_ERROR(e) \
    pmix_errhandler_invoke(e, pmix_globals.nspace, pmix_globals.rank)

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
PMIX_DECLSPEC const char *pmix_strerror(pmix_status_t errnum);

PMIX_DECLSPEC void pmix_errhandler_invoke(int error, const char nspace[], int rank);

END_C_DECLS

#endif /* PMIX_UTIL_ERROR_H */
