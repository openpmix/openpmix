/*
 * Copyright (c) 2007-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2019      Intel, Inc.  All rights reserved.
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
 * Per https://svn.open-mpi.org/trac/ompi/ticket/933, use a
 * combination of $PWD and getcwd() to find the current working
 * directory.
 */

#ifndef PMIX_GETCWD_H
#define PMIX_GETCWD_H

#include "src/include/pmix_config.h"

BEGIN_C_DECLS

/**
 * Per https://svn.open-mpi.org/trac/ompi/ticket/933, use a
 * combination of $PWD and getcwd() to find the current working
 * directory.
 *
 * The result of getcwd() is authoritative. $PWD is consulted, but is
 * used only when it is textually identical to getcwd(); any difference
 * (a stale $PWD, or one that preserves the symlink components getcwd()
 * resolves) falls back to the getcwd() value.
 *
 * @param buf Caller-allocated buffer to put the result
 * @param size Length of the buf array, including room for the
 * terminating NUL. A buffer exactly as long as the path is therefore
 * too short, and is reported as a truncation.
 *
 * @retval PMIX_ERR_OUT_OF_RESOURCE If an internal malloc() fails, or if
 * the supplied buf buffer was not long enough to hold the full result
 * (in which case as much of the basename as fits is copied in).
 * @retval PMIX_ERR_BAD_PARAM If buf is NULL or size>INT_MAX
 * @retval PMIX_ERR_IN_ERRNO If an other error occurred
 * @retval PMIX_SUCCESS If all went well and a valid value was placed
 * in the buf buffer.
 */
PMIX_EXPORT int pmix_getcwd(char *buf, size_t size);

END_C_DECLS

#endif /* PMIX_GETCWD_H */
