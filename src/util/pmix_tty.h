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

#ifndef PMIX_UTIL_TTY_H
#define PMIX_UTIL_TTY_H

#include "src/include/pmix_config.h"
#include "pmix_common.h"

#ifdef HAVE_TERMIOS_H
#    include <termios.h>
#else
#    ifdef HAVE_TERMIO_H
#        include <termio.h>
#    endif
#endif
#ifdef HAVE_SYS_IOCTL_H
#    include <sys/ioctl.h>
#endif

BEGIN_C_DECLS

PMIX_EXPORT pmix_status_t pmix_gettermios(int fd, struct termios *terms);

PMIX_EXPORT pmix_status_t pmix_getwinsz(int fd, struct winsize *ws);

PMIX_EXPORT pmix_status_t pmix_settermios(int fd, struct termios *terms);

PMIX_EXPORT pmix_status_t pmix_setwinsz(int fd, struct winsize *ws);

PMIX_EXPORT pmix_status_t pmix_setraw(int fd, struct termios *prior);

END_C_DECLS

#endif /* PMIX_UTIL_TTY_H */
