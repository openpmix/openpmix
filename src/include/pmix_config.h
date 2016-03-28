/*
 * Copyright (c) 2016      Mellanox Technologies, Inc.
 *                         All rights reserved.
 *
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#ifndef PMIX_INCLUDE_CONFIG_H
#define PMIX_INCLUDE_CONFIG_H

#include <src/include/private/autogen/config.h>
#include <pmix/rename.h>

#ifndef PMIX_DECLSPEC
# define PMIX_DECLSPEC
#ifdef PMIX_C_HAVE_VISIBILITY
# if PMIX_C_HAVE_VISIBILITY
#  define PMIX_EXPORT __attribute__((__visibility__("default")))
# else
#  define PMIX_EXPORT
# endif
#else
# define PMIX_EXPORT
#endif
#endif

#if defined(c_plusplus) || defined(__cplusplus)
# define BEGIN_C_DECLS extern "C" {
# define END_C_DECLS }
#else
#define BEGIN_C_DECLS          /* empty */
#define END_C_DECLS            /* empty */
#endif

#endif /* PMIX_INCLUDE_CONFIG_H */
