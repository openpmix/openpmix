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
 * Copyright (c) 2007      Voltaire. All rights reserved.
 * Copyright (c) 2012      Los Alamos National Security, LLC. All rights reserved.
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
 *
 * Copyright (c) 2015      Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "src/include/pmix_config.h"

#ifdef HAVE_STDLIB_H
#    include <stdlib.h>
#endif /* HAVE_STDLIB_H */
#ifdef HAVE_STRING_H
#    include <string.h>
#endif /* HAVE_STRING_H */

#include "pmix.h"
#include "src/util/pmix_argv.h"

/*
 * Append a string to the end of a new or existing argv array.
 */
pmix_status_t pmix_argv_append(int *argc, char ***argv, const char *arg)
{
    pmix_status_t rc;

    /* add the new element */
    if (PMIX_SUCCESS != (rc = PMIx_Argv_append_nosize(argv, arg))) {
        return rc;
    }

    *argc = PMIx_Argv_count(*argv);

    return PMIX_SUCCESS;
}

pmix_status_t pmix_argv_append_unique_idx(int *idx, char ***argv, const char *arg)
{
    int i;
    pmix_status_t rc;

    /* screen the inputs the way the public twin
     * (PMIx_Argv_append_unique_nosize) does - the compare loop below
     * would otherwise hand a NULL arg straight to strcmp */
    if (NULL == idx || NULL == argv || NULL == arg) {
        return PMIX_ERR_BAD_PARAM;
    }

    /* if the provided array is NULL, then the arg cannot be present,
     * so just go ahead and append
     */
    if (NULL == *argv) {
        goto add;
    }
    /* see if this arg is already present in the array */
    for (i = 0; NULL != (*argv)[i]; i++) {
        if (0 == strcmp(arg, (*argv)[i])) {
            /* already exists */
            *idx = i;
            return PMIX_SUCCESS;
        }
    }
add:
    if (PMIX_SUCCESS != (rc = PMIx_Argv_append_nosize(argv, arg))) {
        return rc;
    }
    *idx = PMIx_Argv_count(*argv) - 1;

    return PMIX_SUCCESS;
}

/*
 * Join all the elements of an argv array from within a
 * specified range into a single newly-allocated string.
 */
char *pmix_argv_join_range(char **argv, size_t start, size_t end, int delimiter)
{
    char **p;
    char *pp;
    char *str;
    size_t str_len = 0;
    size_t i;

    /* Bozo case */

    if (NULL == argv || NULL == argv[0] || start >= (size_t) PMIx_Argv_count(argv)) {
        return strdup("");
    }

    /* Find the total string length in argv including delimiters.  The
     last delimiter is replaced by the NULL character. */

    for (p = &argv[start], i = start; *p && i < end; ++p, ++i) {
        str_len += strlen(*p) + 1;
    }

    if (0 == str_len) {
        return strdup("");
    }

    /* Allocate the string. */

    if (NULL == (str = (char *) calloc(str_len, sizeof(char)))) {
        return NULL;
    }

    /* Loop filling in the string. */

    str[--str_len] = '\0';
    p = &argv[start];
    pp = *p;

    for (i = 0; i < str_len; ++i) {
        if ('\0' == *pp) {

            /* End of a string, fill in a delimiter and go to the next
             string. */

            str[i] = (char) delimiter;
            ++p;
            pp = *p;
        } else {
            str[i] = *pp++;
        }
    }

    /* All done */

    return str;
}

/*
 * Return the number of bytes consumed by an argv array.
 */
size_t pmix_argv_len(char **argv)
{
    char **p;
    size_t length;

    if (NULL == argv) {
        return (size_t) 0;
    }

    length = sizeof(char *);

    for (p = argv; *p; ++p) {
        length += strlen(*p) + 1 + sizeof(char *);
    }

    return length;
}

/*
 * Copy a NULL-terminated argv array, stripping any leading/trailing
 * quotes from each element
 */
char **pmix_argv_copy_strip(char **argv)
{
    char **dupv = NULL;
    int n;
    char *start;
    char *stripped;
    size_t len;

    if (NULL == argv) {
        return NULL;
    }

    /* create an "empty" list, so that we return something valid if we
     were passed a valid list with no contained elements */
    dupv = (char **) malloc(sizeof(char *));
    if (NULL == dupv) {
        return NULL;
    }
    dupv[0] = NULL;

    for (n=0; NULL != argv[n]; n++) {
        /* Narrow the element to the span that survives the strip and
         * copy that span out. We must not punch a temporary NUL into
         * the caller's string to mark the end: this function copies,
         * so the source array is not ours to write to, and an element
         * that points at read-only storage (a string literal) would
         * fault. */
        start = argv[n];
        len = strlen(start);
        if ('\"' == start[0]) {
            ++start;
            --len;
        }
        if (0 < len && '\"' == start[len-1]) {
            --len;
        }
        stripped = (char *) malloc(len + 1);
        if (NULL == stripped) {
            PMIx_Argv_free(dupv);
            return NULL;
        }
        memcpy(stripped, start, len);
        stripped[len] = '\0';
        if (PMIX_SUCCESS != PMIx_Argv_append_nosize(&dupv, stripped)) {
            free(stripped);
            PMIx_Argv_free(dupv);
            return NULL;
        }
        free(stripped);
    }

    /* All done */

    return dupv;
}

pmix_status_t pmix_argv_delete(int *argc, char ***argv, int start, int num_to_delete)
{
    int i;
    int count;
    int suffix_count;
    char **tmp;

    /* Check for the bozo cases */
    if (NULL == argv || NULL == *argv || 0 == num_to_delete) {
        return PMIX_SUCCESS;
    }
    count = PMIx_Argv_count(*argv);
    if (start > count) {
        return PMIX_SUCCESS;
    } else if (start < 0 || num_to_delete < 0) {
        return PMIX_ERR_BAD_PARAM;
    }

    /* Ok, we have some tokens to delete.  Calculate the new length of
       the argv array. */

    suffix_count = count - (start + num_to_delete);
    if (suffix_count < 0) {
        suffix_count = 0;
    }

    /* Free all items that are being deleted */

    for (i = start; i < count && i < start + num_to_delete; ++i) {
        free((*argv)[i]);
    }

    /* Copy the suffix over the deleted items */

    for (i = start; i < start + suffix_count; ++i) {
        (*argv)[i] = (*argv)[i + num_to_delete];
    }

    /* Add the trailing NULL */

    (*argv)[i] = NULL;

    /* adjust the argv array */
    tmp = (char **) realloc(*argv, sizeof(char *) * (i + 1));
    if (NULL != tmp) {
        *argv = tmp;
    }

    /* adjust the argc: i is start + suffix_count, i.e. the new count */
    (*argc) = i;

    return PMIX_SUCCESS;
}

pmix_status_t pmix_argv_insert(char ***target, int start, char **source)
{
    int i, source_count, target_count;
    int suffix_count;
    char **tmp;
    char **copies;
    pmix_status_t rc;

    /* Check for the bozo cases */

    if (NULL == target || NULL == *target || start < 0) {
        return PMIX_ERR_BAD_PARAM;
    } else if (NULL == source) {
        return PMIX_SUCCESS;
    }

    target_count = PMIx_Argv_count(*target);
    source_count = PMIx_Argv_count(source);
    if (0 == source_count) {
        return PMIX_SUCCESS;
    }

    /* Easy case: appending to the end */

    if (start > target_count) {
        for (i = 0; i < source_count; ++i) {
            rc = pmix_argv_append(&target_count, target, source[i]);
            if (PMIX_SUCCESS != rc) {
                return rc;
            }
        }
    }

    /* Harder: inserting into the middle */

    else {

        /* Copy the source strings before touching the target. A copy
         * that failed after the target had been grown and its suffix
         * shifted would leave a NULL in the middle of the array, which
         * terminates it early - every element beyond the hole is both
         * lost to the caller and leaked - and the old code reported
         * that as success. */

        copies = (char **) calloc(source_count, sizeof(char *));
        if (NULL == copies) {
            return PMIX_ERR_NOMEM;
        }
        for (i = 0; i < source_count; ++i) {
            copies[i] = strdup(source[i]);
            if (NULL == copies[i]) {
                goto nomem;
            }
        }

        /* Allocate new space */

        tmp = (char **) realloc(*target, sizeof(char *) * (target_count + source_count + 1));
        if (NULL == tmp) {
            goto nomem;
        }
        *target = tmp;

        /* Move suffix items down to the end */

        suffix_count = target_count - start;
        for (i = suffix_count - 1; i >= 0; --i) {
            (*target)[start + source_count + i] = (*target)[start + i];
        }
        (*target)[start + suffix_count + source_count] = NULL;

        /* Hand the copies over to the target */

        for (i = 0; i < source_count; ++i) {
            (*target)[start + i] = copies[i];
        }
        free(copies);
    }

    /* All done */

    return PMIX_SUCCESS;

nomem:
    for (i = 0; i < source_count; ++i) {
        free(copies[i]);
    }
    free(copies);
    return PMIX_ERR_NOMEM;
}

pmix_status_t pmix_argv_insert_element(char ***target, int location, char *source)
{
    int i, target_count;
    int suffix_count;
    char **tmp;
    char *copy;

    /* Check for the bozo cases */

    if (NULL == target || NULL == *target || location < 0) {
        return PMIX_ERR_BAD_PARAM;
    } else if (NULL == source) {
        return PMIX_SUCCESS;
    }

    /* Easy case: appending to the end */
    target_count = PMIx_Argv_count(*target);
    if (location > target_count) {
        return pmix_argv_append(&target_count, target, source);
    }

    /* Copy the source before touching the target - see the comment in
     * pmix_argv_insert() for why the copy cannot come last */
    copy = strdup(source);
    if (NULL == copy) {
        return PMIX_ERR_NOMEM;
    }

    /* Allocate new space */
    tmp = (char **) realloc(*target, sizeof(char *) * (target_count + 2));
    if (NULL == tmp) {
        free(copy);
        return PMIX_ERR_NOMEM;
    }
    *target = tmp;

    /* Move suffix items down to the end */
    suffix_count = target_count - location;
    for (i = suffix_count - 1; i >= 0; --i) {
        (*target)[location + 1 + i] = (*target)[location + i];
    }
    (*target)[location + suffix_count + 1] = NULL;

    /* Hand the copy over to the target */
    (*target)[location] = copy;

    /* All done */
    return PMIX_SUCCESS;
}
