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
 * Copyright (c) 2009-2014 Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2014      Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "src/include/pmix_config.h"

#include <stdlib.h>
#ifdef HAVE_STRING_H
#    include <string.h>
#endif /* HAVE_STRING_H */
#ifdef HAVE_LIBGEN_H
#    include <libgen.h>
#endif /* HAVE_LIBGEN_H */

#include "src/util/pmix_basename.h"
#include "src/util/pmix_os_path.h"

/**
 * Return a pointer into the original string where the last PATH delimiter
 * was found. It does not modify the original string. Moreover, it does not
 * scan the full string, but only the part allowed by the specified number
 * of characters.
 * If the last character on the string is a path separator, it will be skipped.
 */
static inline char *pmix_find_last_path_separator(const char *filename, size_t n)
{
    char *p;

    if (0 == n) {
        return NULL;
    }

    /* Start at the last character, not the terminating NUL */
    p = (char *) filename + n - 1;

    /* First skip the latest separators */
    for (; p >= filename; p--) {
        if (*p != PMIX_PATH_SEP[0])
            break;
    }

    for (; p >= filename; p--) {
        if (*p == PMIX_PATH_SEP[0])
            return p;
    }

    return NULL; /* nothing found inside the filename */
}

char *pmix_basename(const char *filename)
{
    size_t i;
    char *tmp, *ret = NULL;
    const char sep = PMIX_PATH_SEP[0];

    /* Check for the bozo case */
    if (NULL == filename) {
        return NULL;
    }
    if (0 == strlen(filename)) {
        return strdup("");
    }
    if (sep == filename[0] && '\0' == filename[1]) {
        return strdup(filename);
    }

    /* Remove trailing sep's (note that we already know that strlen > 0) */
    tmp = strdup(filename);
    if (1 < strlen(tmp)) {
        for (i = strlen(tmp) - 1; i > 0; --i) {
            if (sep == tmp[i]) {
                tmp[i] = '\0';
            } else {
                break;
            }
        }
        if (0 == i && sep == tmp[0]) {
            /* The entire string consisted of separators */
            tmp[0] = sep;
            tmp[1] = '\0';
            return tmp;
        }
    }

    /* Look for the final sep */
    ret = pmix_find_last_path_separator(tmp, strlen(tmp));
    if (NULL == ret) {
        return tmp;
    }
    ret = strdup(ret + 1);
    free(tmp);
    return ret;
}

char *pmix_dirname(const char *filename)
{
    /* Check for the bozo case */
    if (NULL == filename) {
        return NULL;
    }

#if defined(HAVE_DIRNAME) || PMIX_HAVE_DIRNAME
    char *safe_tmp = strdup(filename), *result;
    result = strdup(dirname(safe_tmp));
    free(safe_tmp);
    return result;
#else
    const char *p = pmix_find_last_path_separator(filename, strlen(filename));
    const char *end;
    char *ret;

    /* NOTE: p will be NULL if no path separator was in the filename - i.e.,
     * if filename is just a local file - in which case the dirname is "." */
    if (NULL == p) {
        return strdup(".");
    }

    /* p points at the last non-trailing separator. Back up over any run of
     * consecutive separators that precede the final path component. */
    end = p;
    while (end > filename && (('\\' == *(end - 1)) || ('/' == *(end - 1)))) {
        end--;
    }

    if (end == filename) {
        /* The separators run all the way to the start of the string
         * (e.g., "/yow.c" or "///foo"); the dirname is the root. */
        return strdup(PMIX_PATH_SEP);
    }

    ret = (char *) calloc((end - filename) + 1, sizeof(char));
    if (NULL == ret) {
        return NULL;
    }
    pmix_strncpy(ret, filename, end - filename);
    ret[end - filename] = '\0';
    return pmix_make_filename_os_friendly(ret);
#endif /* defined(HAVE_DIRNAME) || PMIX_HAVE_DIRNAME */
}
