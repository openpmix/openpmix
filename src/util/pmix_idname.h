/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#ifndef PMIX_IDNAME_H
#define PMIX_IDNAME_H

#include "src/include/pmix_config.h"
#include "pmix_common.h"

BEGIN_C_DECLS

/* A user or group ID given as a number or as a name. A string that is
 * entirely decimal digits is taken as the number; any other string is
 * looked up as a user or group name. Returns PMIX_ERR_BAD_PARAM for a
 * NULL or empty string or an out-of-range number, and PMIX_ERR_NOT_FOUND
 * for a name that does not resolve. */
PMIX_EXPORT pmix_status_t pmix_util_uid_from_string(const char *str, uint32_t *uid);
PMIX_EXPORT pmix_status_t pmix_util_gid_from_string(const char *str, uint32_t *gid);

/* The same for a value, as PMIX_USERID and PMIX_GRPID carry them: a
 * number of any integer type, or a string holding a number or a name.
 * Returns PMIX_ERR_BAD_PARAM for a value of any other type. */
PMIX_EXPORT pmix_status_t pmix_util_uid_from_value(const pmix_value_t *val, uint32_t *uid);
PMIX_EXPORT pmix_status_t pmix_util_gid_from_value(const pmix_value_t *val, uint32_t *gid);

END_C_DECLS

#endif
