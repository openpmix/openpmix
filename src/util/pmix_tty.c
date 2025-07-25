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
 * Copyright (c) 2018      Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2019-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2021-2025 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "src/include/pmix_config.h"

#ifdef HAVE_SYS_CDEFS_H
#    include <sys/cdefs.h>
#endif
#ifdef HAVE_SYS_TYPES_H
#    include <sys/types.h>
#endif
#ifdef HAVE_SYS_IOCTL_H
#    include <sys/ioctl.h>
#endif
#ifdef HAVE_TERMIOS_H
#    include <termios.h>
#else
#    ifdef HAVE_TERMIO_H
#        include <termio.h>
#    endif
#endif
#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif
#ifdef HAVE_UTIL_H
#    include <util.h>
#endif

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "src/util/pmix_output.h"
#include "src/util/pmix_tty.h"

pmix_status_t pmix_gettermios(int fd, struct termios *terms)
{
    int rc;

    rc = tcgetattr(fd, terms);
    if (0 == rc) {
        return PMIX_SUCCESS;
    }
    return PMIX_ERROR;
}

pmix_status_t pmix_getwinsz(int fd, struct winsize *ws)
{
    int rc;

    rc = ioctl(fd, TIOCGWINSZ, (char*) ws);
    if (0 == rc) {
        return PMIX_SUCCESS;
    }
    return PMIX_ERROR;
}

pmix_status_t pmix_settermios(int fd, struct termios *terms)
{
    struct termios old, check;
    int rc, err;

    rc = pmix_gettermios(fd, &old);

    if (0 != rc) {
        return PMIX_ERROR;
    }

    rc = tcsetattr(fd, TCSAFLUSH, terms);
    if (0 != rc) {
        // attempt to reset it
        err = errno;  // preserve original error
        tcsetattr(fd, TCSAFLUSH, &old);
        errno = err;

    } else {

        /* check that it was fully successful - tcsetattr returns
         * success if ANY change succeeded */
        rc = pmix_gettermios(fd, &check);
        if (0 != rc) {
            return PMIX_ERROR;
        }
        rc = memcmp(terms, &check, sizeof(struct termios));
        if (0 != rc) {
            errno = EINVAL;
            return PMIX_ERROR;
        }
    }

    return PMIX_SUCCESS;
}

pmix_status_t pmix_setwinsz(int fd, struct winsize *ws)
{
    int rc;

    rc = ioctl(fd, TIOCSWINSZ, (char*) ws);
    if (0 == rc) {
        return PMIX_SUCCESS;
    }
    return PMIX_ERROR;
}

pmix_status_t pmix_setraw(int fd, struct termios *prior)
{
    struct termios terms;
    int rc;

    rc = pmix_gettermios(fd, &terms);

    if (0 != rc) {
        return PMIX_ERROR;
    }

    if (NULL != prior) {
        memcpy(prior, &terms, sizeof(struct termios));
    }

    terms.c_lflag &= ~(ICANON | ISIG | IEXTEN | ECHO);
                        /* Noncanonical mode, disable signals, extended
                           input processing, and echoing */

    terms.c_iflag &= ~(BRKINT | ICRNL | IGNBRK | IGNCR | INLCR |
                       INPCK | ISTRIP | IXON | PARMRK);
                        /* Disable special handling of CR, NL, and BREAK.
                           No 8th-bit stripping or parity error handling.
                           Disable START/STOP output flow control. */

    terms.c_oflag &= ~OPOST;                /* Disable all output processing */

    terms.c_cc[VMIN] = 1;                   /* Character-at-a-time input */
    terms.c_cc[VTIME] = 0;                  /* with blocking */

    rc = pmix_settermios(fd, &terms);
    return rc;
}
