/*
 * Copyright (c) 2016-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2021-2022 Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#ifndef PMIX_GETID_H
#define PMIX_GETID_H

#include "src/include/pmix_config.h"
#include "pmix_common.h"

#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif
#ifdef HAVE_SYS_TYPES_H
#    include <sys/types.h>
#endif

BEGIN_C_DECLS

/* Ask the kernel for the credentials of the process on the far end of
 * the connected socket `sd`, and write them to *uid and *gid.
 *
 * Returns PMIX_SUCCESS, in which case both out-parameters have been
 * written; PMIX_ERR_INVALID_CRED if the descriptor has no credentials
 * to report, in which case neither has; or PMIX_ERR_NOT_SUPPORTED on a
 * platform that offers no way to ask. Only a connected AF_UNIX socket
 * gives a meaningful answer - see src/util/AGENTS.md. */
PMIX_EXPORT pmix_status_t pmix_util_getid(int sd, uid_t *uid, gid_t *gid);

END_C_DECLS

#endif /* PMIX_PRINTF_H */
