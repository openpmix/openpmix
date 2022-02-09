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
 * Copyright (c) 2007-2013 Los Alamos National Security, LLC.  All rights
 *                         reserved.
 * Copyright (c) 2015-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2015      Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2016      IBM Corporation.  All rights reserved.
 * Copyright (c) 2021      Nanook Consulting.  All rights reserved.
 * Copyright (c) 2022      Amazon.com, Inc. or its affiliates.
 *                         All Rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/**
 * @file
 *
 * Generic helper routines for environment manipulation.
 */

#ifndef PMIX_ENVIRON_H
#define PMIX_ENVIRON_H


#include <unistd.h>
#ifdef HAVE_CRT_EXTERNS_H
#    include <crt_externs.h>
#endif

#include "include/pmix_common.h"
#include "src/class/pmix_list.h"

BEGIN_C_DECLS

/**
 * Merge two environ-like arrays into a single, new array, ensuring
 * that there are no duplicate entries.
 *
 * @param minor Set 1 of the environ's to merge
 * @param major Set 2 of the environ's to merge
 * @retval New array of environ
 *
 * Merge two environ-like arrays into a single, new array,
 * ensuring that there are no duplicate entires.  If there are
 * duplicates, entries in the \em major array are favored over
 * those in the \em minor array.
 *
 * Both \em major and \em minor are expected to be argv-style
 * arrays (i.e., terminated with a NULL pointer).
 *
 * The array that is returned is an unencumbered array that should
 * later be freed with a call to pmix_argv_free().
 *
 * Either (or both) of \em major and \em minor can be NULL.  If
 * one of the two is NULL, the other list is simply copied to the
 * output.  If both are NULL, NULL is returned.
 */
PMIX_EXPORT char **pmix_environ_merge(char **minor,
                                      char **major) __pmix_attribute_warn_unused_result__;

/**
 * Merge contents of an environ-like array into a second environ-like
 * array
 *
 * @param orig The environment to update
 * @param additions The environment to merge into orig
 *
 * Merge the contents of \em additions into \em orig. If a key from
 * \em additions is found in \em orig, then the value in orig is not
 * updated (ie, it is an additions-only merge).  The original
 * environment cannot be environ, because pmix_argv_append is used to
 * extend the environment, and pmix_argv_append_nosize() may not be
 * safe to call on environ (for the same reason that realloc() may
 * note be safe to call on environ).
 *
 * New strings are allocated when copied, so both \em orig and \em
 * additions individually maintain their ability to be freed with
 * pmix_argv_free().
 *
 * Note that on error, the \em orig array may be partially updated
 * with values from additions, but the array will still be a valid
 * argv-style array.
 */
PMIX_EXPORT pmix_status_t pmix_environ_merge_inplace(char ***orig,
						     char **additions)
    __pmix_attribute_warn_unused_result__ __pmix_attribute_nonnull__(1);

/**
 * Portable version of setenv(3), allowing editing of any
 * environ-like array.
 *
 * @param name String name of the environment variable to look for
 * @param value String value to set (may be NULL)
 * @param overwrite Whether to overwrite any existing value with
 * the same name
 * @param env The environment to use
 *
 * @retval PMIX_ERR_OUT_OF_RESOURCE If internal malloc() fails.
 * @retval PMIX_EXISTS If the name already exists in \em env and
 * \em overwrite is false (and therefore the \em value was not
 * saved in \em env)
 * @retval PMIX_SUCESS If the value replaced another value or is
 * appended to \em env.
 *
 * \em env is expected to be a NULL-terminated array of pointers
 * (argv-style).  Note that unlike some implementations of
 * putenv(3), if \em value is inserted in \em env, it is copied.
 * So the caller can modify/free both \em name and \em value after
 * pmix_setenv() returns.
 *
 * The \em env array will be grown if necessary.
 *
 * It is permissable to invoke this function with the
 * system-defined \em environ variable.  For example:
 *
 * \code
 *   #include "pmix/util/pmix_environ.h"
 *   pmix_setenv("foo", "bar", true, &environ);
 * \endcode
 *
 * NOTE: If you use the real environ, pmix_setenv() will turn
 * around and perform putenv() to put the value in the
 * environment.  This may very well lead to a memory leak, so its
 * use is strongly discouraged.
 *
 * It is also permissable to call this function with an empty \em
 * env, as long as it is pre-initialized with NULL:
 *
 * \code
 *   char **my_env = NULL;
 *   pmix_setenv("foo", "bar", true, &my_env);
 * \endcode
 */
PMIX_EXPORT pmix_status_t pmix_setenv(const char *name, const char *value, bool overwrite,
                                      char ***env) __pmix_attribute_nonnull__(1);

/**
 * Portable version of getenv(3), allowing searches of any key=value array
 *
 * @param name String name of the environment variable to look for
 * @param env The environment to use
 *
 * The return value will be a pointer to the start of the value
 * string.  The string returned should not be free()ed or modified by
 * the caller, similar to getenv().
 *
 * Unlike getenv(), pmix_getenv() will accept a \em name in key=value
 * format.  In that case, only the key portion of \em name is used for
 * the search, and the return value of pmix_getenv() is the value of
 * the same key in \em env.
 */
PMIX_EXPORT char * pmix_getenv(const char *name, char **env) __pmix_attribute_nonnull__(1);

/**
 * Portable version of unsetenv(3), allowing editing of any
 * environ-like array.
 *
 * @param name String name of the environment variable to look for
 * @param env The environment to use
 *
 * @retval PMIX_ERR_OUT_OF_RESOURCE If an internal malloc fails.
 * @retval PMIX_ERR_NOT_FOUND If \em name is not found in \em env.
 * @retval PMIX_SUCCESS If \em name is found and successfully deleted.
 *
 * If \em name is found in \em env, the string corresponding to
 * that entry is freed and its entry is eliminated from the array.
 */
PMIX_EXPORT pmix_status_t pmix_unsetenv(const char *name, char ***env)
    __pmix_attribute_nonnull__(1);

/* A consistent way to retrieve the home and tmp directory on all supported
 * platforms.
 */
PMIX_EXPORT const char *pmix_home_directory(uid_t uid);
PMIX_EXPORT const char *pmix_tmp_directory(void);

/* Provide a utility for harvesting envars */
PMIX_EXPORT pmix_status_t pmix_util_harvest_envars(char **incvars, char **excvars,
                                                   pmix_list_t *ilist);

/* Some care is needed with environ on OS X when dealing with shared
   libraries.  Handle that care here... */
#ifdef HAVE__NSGETENVIRON
#    define environ (*_NSGetEnviron())
#else
extern char **environ;
#endif

END_C_DECLS

#endif /* PMIX_ENVIRON */
