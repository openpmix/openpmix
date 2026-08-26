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
#include <string.h>

#include "src/util/pmix_tty.h"

/* Bits that can appear in c_lflag but are *status* rather than
 * settings: the driver turns them on and off for itself.  PENDIN says
 * queued input has yet to be retyped - which is exactly what happens
 * when canonical mode is re-enabled - FLUSHO says output is being
 * discarded, and EXTPROC can be turned on from the master end of a pty
 * without the slave's caller ever asking for it.  A caller handing
 * back settings it read a moment ago will legitimately disagree with
 * the read-back on all three, so they take no part in the check that
 * a requested set actually took. */
#ifdef FLUSHO
#    define PMIX_TTY_FLUSHO FLUSHO
#else
#    define PMIX_TTY_FLUSHO 0
#endif
#ifdef PENDIN
#    define PMIX_TTY_PENDIN PENDIN
#else
#    define PMIX_TTY_PENDIN 0
#endif
#ifdef EXTPROC
#    define PMIX_TTY_EXTPROC EXTPROC
#else
#    define PMIX_TTY_EXTPROC 0
#endif
#define PMIX_TTY_LFLAG_STATUS \
    ((tcflag_t) (PMIX_TTY_FLUSHO | PMIX_TTY_PENDIN | PMIX_TTY_EXTPROC))

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
    pmix_status_t rc;
    int err;

    rc = pmix_gettermios(fd, &old);
    if (PMIX_SUCCESS != rc) {
        return rc;
    }

    if (0 != tcsetattr(fd, TCSANOW, terms)) {
        /* a failure says nothing about how much of the request was
         * applied, so put the terminal back the way we found it and
         * report the original error */
        err = errno;  // preserve original error
        (void) tcsetattr(fd, TCSANOW, &old);
        errno = err;
        return PMIX_ERROR;
    }

    /* check that it was fully successful - tcsetattr returns
     * success if ANY change succeeded. Compare the individual POSIX
     * termios fields rather than memcmp'ing the whole struct: the
     * struct carries padding and (on some platforms) speed fields the
     * kernel canonicalizes, so a byte-exact compare gives spurious
     * failures even when every requested setting was applied.  The
     * driver-maintained bits of c_lflag are excluded for the same
     * reason - see PMIX_TTY_LFLAG_STATUS above. */
    rc = pmix_gettermios(fd, &check);
    if (PMIX_SUCCESS != rc) {
        err = errno;
        (void) tcsetattr(fd, TCSANOW, &old);
        errno = err;
        return rc;
    }
    if (terms->c_iflag != check.c_iflag ||
        terms->c_oflag != check.c_oflag ||
        terms->c_cflag != check.c_cflag ||
        (terms->c_lflag & ~PMIX_TTY_LFLAG_STATUS)
            != (check.c_lflag & ~PMIX_TTY_LFLAG_STATUS) ||
        0 != memcmp(terms->c_cc, check.c_cc, sizeof(terms->c_cc))) {
        /* only part of the request took - leave nothing half-applied */
        (void) tcsetattr(fd, TCSANOW, &old);
        errno = EINVAL;
        return PMIX_ERROR;
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
    pmix_status_t rc;

    rc = pmix_gettermios(fd, &terms);
    if (PMIX_SUCCESS != rc) {
        return rc;
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

    return pmix_settermios(fd, &terms);
}
