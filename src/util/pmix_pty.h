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
/* struct winsize appears in the prototypes below, and <termios.h> is not
 * where it comes from - glibc keeps it in <sys/ioctl.h> and only leaks it
 * out of <termios.h> under some feature-test settings.  macOS does leak
 * it, which is why a consumer that forgot this include built there and
 * failed on Linux with "declared inside parameter list".  This header is
 * installed, so it has to bring its own types. */
#ifdef HAVE_SYS_IOCTL_H
#    include <sys/ioctl.h>
#endif

BEGIN_C_DECLS

/**
 * @file
 *
 * Pseudo-terminal helpers.
 *
 * Thin wrappers over openpty(3) and forkpty(3).  On a platform without
 * them each fails, so a caller must always be prepared to fall back to
 * a plain pipe.  Whether to ask for a pty at all is the caller's
 * decision: `--disable-pty-support` sets `PMIX_ENABLE_PTY_SUPPORT` to 0,
 * and pmix_pfexec then does not call pmix_openpty().
 *
 * **None of these gives the caller a controlling terminal, and none of
 * them may.** They run in the process that is *setting up* a pty for
 * somebody else - a PMIx server arranging a child's output - and
 * opening a terminal device from a session leader that has no
 * controlling terminal makes it one.  A daemonized server is exactly
 * that, so a helper that took one would tie the server's fate to a pty
 * it merely handed to a child: closing the last master descriptor
 * hangs up the terminal's foreground process group.  openpty(3) opens
 * with `O_NOCTTY`; do not replace it with anything that does not.  A
 * caller that genuinely wants a controlling terminal asks for one
 * itself, in the child, after `setsid()` - which is what `forkpty()`
 * does.
 */

/**
 * Open a pty and hand back both ends.
 *
 * @param amaster  filled in with the master descriptor
 * @param aslave   filled in with the slave descriptor
 * @param name     if not NULL, receives the slave's device name, with
 *                 openpty(3)'s lack of a length: size it generously
 * @param termp    if not NULL, terminal settings to apply to the slave
 * @param winp     if not NULL, window size to apply to the slave
 * @return         0 on success, -1 on failure
 *
 * On failure neither descriptor is left open and the caller has nothing
 * to clean up.  On success the caller owns both.  This is openpty(3);
 * on a platform without it, it always fails.
 */
PMIX_EXPORT int pmix_openpty(int *amaster, int *aslave, char *name,
                             struct termios *termp, struct winsize *winp);

/**
 * fork(2) with the child's standard descriptors on a new pty.
 *
 * @param master  filled in, in the parent, with the master descriptor
 * @param slave   if not NULL, receives the slave's device name
 * @param sterm   if not NULL, terminal settings to apply to the slave
 * @param sws     if not NULL, window size to apply to the slave
 * @return        0 in the child, the child's pid in the parent, -1 on
 *                failure
 *
 * Unlike pmix_openpty(), the child of this call *does* get the pty as
 * its controlling terminal - that is what forkpty(3) is for.  On a
 * platform without forkpty(3), it always fails.
 */
PMIX_EXPORT pid_t pmix_forkpty(int *master, char *slave,
                               const struct termios *sterm,
                               const struct winsize *sws);

END_C_DECLS

#endif /* PMIX_UTIL_PTY_H */
