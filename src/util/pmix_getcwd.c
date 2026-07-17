/*
 * Copyright (c) 2007-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2019      Intel, Inc.  All rights reserved.
 * Copyright (c) 2021-2023 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "pmix_config.h"
#include "pmix_common.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif
#include <string.h>

#include "src/util/pmix_basename.h"
#include "src/util/pmix_getcwd.h"
#include "src/util/pmix_string_copy.h"

/*
 * Return the current working directory. The result of getcwd() is
 * authoritative; $PWD is used only when it is textually identical to
 * getcwd(). Any difference (e.g., $PWD preserving symlink components
 * that getcwd() resolves, or a stale $PWD) results in falling back to
 * the getcwd() value.
 */
int pmix_getcwd(char *buf, size_t size)
{
    char cwd[PMIX_PATH_MAX];
    char *pwd = getenv("PWD");
    char *shortened;

    /* Bozo checks (e.g., if someone accidentally passed -1 to the
       unsigned "size" param) */
    if (NULL == buf || size > INT_MAX) {
        return PMIX_ERR_BAD_PARAM;
    }

    /* Call getcwd() to get a baseline result */
    if (NULL == getcwd(cwd, sizeof(cwd))) {
        return PMIX_ERR_IN_ERRNO;
    }

    if (NULL == pwd) {
        pwd = cwd;
    } else {
        if (0 != strcmp(pwd, cwd)) {
            // fallback to getcwd value
            pwd = cwd;
        }
    }

    /* pwd is pointing to the result that we want to
       give.  Ensure the user's buffer is long enough.  If it is, copy
       in the value and be done. */
    if (strlen(pwd) >= size) {
        /* if it isn't big enough, give them as much
         * of the basename as possible
         */
        shortened = pmix_basename(pwd);
        if (NULL == shortened) {
            /* pmix_basename failed to allocate - report it rather than
             * passing a NULL src to pmix_string_copy */
            return PMIX_ERR_OUT_OF_RESOURCE;
        }
        pmix_string_copy(buf, shortened, size);
        free(shortened);
        /* indicate that it isn't the full path */
        return PMIX_ERR_OUT_OF_RESOURCE;
    }
    pmix_string_copy(buf, pwd, size);
    return PMIX_SUCCESS;
}
