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

/*
 * Wrappers over tcgetattr/tcsetattr and the winsize ioctls.
 *
 * Every one of these takes a descriptor that must already refer to a
 * terminal, answers PMIX_SUCCESS or PMIX_ERROR, and leaves `errno` set
 * by the call that failed - the status says only that something did.
 *
 * They hold no state of their own and neither allocate nor lock, so
 * they are reentrant and safe to call from any thread or between fork
 * and exec.  What is *not* atomic is a read-modify-write of one
 * terminal: pmix_settermios() reads the current settings so it can put
 * them back, and pmix_setraw() reads them so it can hand them to the
 * caller, so two threads driving the same terminal can each undo the
 * other.  Serialize that in the caller.
 */

/**
 * Read a terminal's current settings.
 *
 * @param fd     descriptor referring to a terminal
 * @param terms  filled in with the current settings
 * @return       PMIX_SUCCESS, or PMIX_ERROR with `errno` set
 */
PMIX_EXPORT pmix_status_t pmix_gettermios(int fd, struct termios *terms);

/**
 * Read a terminal's current window size.
 *
 * @param fd  descriptor referring to a terminal
 * @param ws  filled in with the current window size
 * @return    PMIX_SUCCESS, or PMIX_ERROR with `errno` set
 */
PMIX_EXPORT pmix_status_t pmix_getwinsz(int fd, struct winsize *ws);

/**
 * Apply terminal settings, all of them or none of them.
 *
 * @param fd     descriptor referring to a terminal
 * @param terms  the settings to apply
 * @return       PMIX_SUCCESS, or PMIX_ERROR with `errno` set
 *
 * tcsetattr(3) reports success when *any* part of the request was
 * applied, which is not something a caller can act on, so this reads
 * the settings back and requires them to match.  Anything short of a
 * full match - including a tcsetattr() that failed outright - puts the
 * terminal back the way it was found and reports PMIX_ERROR, so a
 * failure never leaves settings half-applied.  `errno` is EINVAL when
 * the set was accepted but did not take.
 *
 * The status bits the driver maintains for itself in c_lflag - FLUSHO,
 * PENDIN, EXTPROC - are not part of the comparison: they are not
 * settings, and requiring them to match makes an ordinary
 * pmix_setraw()/restore pair report a failure that did not happen.
 */
PMIX_EXPORT pmix_status_t pmix_settermios(int fd, struct termios *terms);

/**
 * Set a terminal's window size.
 *
 * @param fd  descriptor referring to a terminal
 * @param ws  the window size to set
 * @return    PMIX_SUCCESS, or PMIX_ERROR with `errno` set
 */
PMIX_EXPORT pmix_status_t pmix_setwinsz(int fd, struct winsize *ws);

/**
 * Put a terminal into raw mode.
 *
 * @param fd     descriptor referring to a terminal
 * @param prior  if not NULL, receives the settings being replaced, so
 *               the caller can restore them with pmix_settermios()
 * @return       PMIX_SUCCESS, or PMIX_ERROR with `errno` set
 *
 * "Raw" here means the line discipline is out of the way: no canonical
 * mode, no echo, no signal or extended-input processing, no input or
 * output post-processing, and one character at a time with a blocking
 * read.  The control modes (c_cflag) are deliberately left alone - a
 * caller that also wants a particular character size or parity has to
 * ask for it - which is what makes this usable on a pty, where those
 * bits mean nothing.
 *
 * On failure the terminal is unchanged, and `prior` - if it was
 * filled in at all - describes the settings still in effect.
 */
PMIX_EXPORT pmix_status_t pmix_setraw(int fd, struct termios *prior);

END_C_DECLS

#endif /* PMIX_UTIL_TTY_H */
