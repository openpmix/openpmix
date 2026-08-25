/*
 * Copyright (c) 2004-2007 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2005 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2019-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2022-2026 Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#ifndef PMIX_FEW_H
#define PMIX_FEW_H

#include "pmix_config.h"

#include "pmix_common.h"

BEGIN_C_DECLS

/**
 *  Forks, execs, and waits for a subordinate program
 *
 * @param argv Null-terminated argument vector; argv[0] is the program
 * (same as arguments to execvp())
 *
 * @param status Upon success, will be filled with the return status
 * from waitpid(2).  The WIF* macros can be used to examine the value
 * (see waitpid(2)).
 *
 * @retval PMIX_SUCCESS If the child launched and exited.
 * @retval PMIX_ERROR If the child could not be launched or could not be
 * waited for; errno should be examined for the specific error.
 * @retval PMIX_ERR_NOT_SUPPORTED If this platform has no fork/exec.
 *
 * This function forks, execs, and waits for an executable to
 * complete.  The input argv must be a NULL-terminated array (perhaps
 * built with the pmix_arr_*() interface).  Upon success, PMIX_SUCCESS
 * is returned.  This function will wait either until the child
 * process has exited or waitpid() returns an error other than EINTR.
 *
 * Note that a return of PMIX_SUCCESS does \em not imply that the child
 * process exited successfully -- it simply indicates that the child
 * process exited.  The WIF* macros (see waitpid(2)) should be used to
 * examine the status to see how the child exited.
 *
 * \warning \c status is written only when PMIX_SUCCESS is returned.
 *          On any other return there was no child to wait for and the
 *          caller's variable is left exactly as it was, so it must not
 *          be handed to the WIF* macros -- initialize it, and decode it
 *          only after checking the return code.  Note also that
 *          \c status is a wait status and not an errno: the reason a
 *          launch failed is in errno, not in here.
 *
 * If the exec itself fails, the child exits carrying errno as its exit
 * status, so WEXITSTATUS() reports it -- truncated to the low 8 bits,
 * as every exit status is.
 *
 * \warning This function is not safe in a multi-threaded environment,
 *          nor in one in which a handler for \c SIGCHLD has been
 *          registered: it waits on its own child by pid, and a SIGCHLD
 *          handler that reaps indiscriminately will take that child
 *          before waitpid() can see it.
 */
PMIX_EXPORT pmix_status_t pmix_few(char *argv[], int *status);

END_C_DECLS
#endif /* PMIX_FEW_H */
