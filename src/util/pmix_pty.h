/*
 * Copyright (c) 2004-2006 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2006 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2019-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2021-2025 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#ifndef PMIX_UTIL_PTY_H
#define PMIX_UTIL_PTY_H

#include "src/include/pmix_config.h"
#include "pmix_common.h"

#ifdef HAVE_UTIL_H
#    include <util.h>
#endif
#ifdef HAVE_LIBUTIL_H
#    include <libutil.h>
#endif
#ifdef HAVE_TERMIOS_H
#    include <termios.h>
#else
#    ifdef HAVE_TERMIO_H
#        include <termio.h>
#    endif
#endif

BEGIN_C_DECLS

#if PMIX_ENABLE_PTY_SUPPORT

PMIX_EXPORT int pmix_openpty(int *amaster, int *aslave, char *name,
                             struct termios *termp, struct winsize *winp);

PMIX_EXPORT int pmix_ptymopen(char *pts_name, size_t maxlen);

PMIX_EXPORT int pmix_ptysopen(int fdm, char *pts_name);

PMIX_EXPORT pid_t pmix_forkpty(int *master, char *slave,
                               const struct termios *sterm,
                               const struct winsize *sws);

#else

PMIX_EXPORT int pmix_openpty(int *amaster, int *aslave, char *name,
                             void *termp, void *winpp);

PMIX_EXPORT int pmix_ptymopen(char *pts_name, size_t maxlen);

PMIX_EXPORT int pmix_ptysopen(int fdm, char *pts_name);

PMIX_EXPORT pid_t pmix_forkpty(int *master, char *slave,
                               const void *sterm, const void *sws);

#endif

END_C_DECLS

#endif /* PMIX_UTIL_PTY_H */
