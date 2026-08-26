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

/**
 * @file
 *
 * Pseudo-terminal helpers.
 *
 * These are built only when configure found pty support; when it did
 * not, `PMIX_ENABLE_PTY_SUPPORT` is 0 and every function below is a stub
 * that fails, so a caller must always be prepared to fall back to a
 * plain pipe.  Note the declarations differ between the two cases: the
 * stubs take `void *` where the real functions take `struct termios *`
 * and `struct winsize *`, because those types need not exist at all on a
 * platform with no pty support.
 *
 * **None of these gives the caller a controlling terminal, and none of
 * them may.** They run in the process that is *setting up* a pty for
 * somebody else - a PMIx server arranging a child's output - and
 * opening a terminal device from a session leader that has no
 * controlling terminal makes it one.  A daemonized server is exactly
 * that, so a helper that took one would tie the server's fate to a pty
 * it merely handed to a child: closing the last master descriptor
 * hangs up the terminal's foreground process group.  Every open here
 * therefore passes `O_NOCTTY`.  A caller that genuinely wants a
 * controlling terminal asks for one itself, in the child, after
 * `setsid()` - which is what `forkpty()` does.
 */

#if PMIX_ENABLE_PTY_SUPPORT

/**
 * Open a pty and hand back both ends.
 *
 * @param amaster  filled in with the master descriptor
 * @param aslave   filled in with the slave descriptor
 * @param name     if not NULL, receives the slave's device name; it must
 *                 be at least 20 bytes, which is all the fallback
 *                 implementation can promise to respect
 * @param termp    if not NULL, terminal settings to apply to the slave
 * @param winp     if not NULL, window size to apply to the slave
 * @return         0 on success, -1 on failure
 *
 * On failure neither descriptor is left open and the caller has nothing
 * to clean up.  On success the caller owns both.
 */
PMIX_EXPORT int pmix_openpty(int *amaster, int *aslave, char *name,
                             struct termios *termp, struct winsize *winp);

/**
 * Open the master side of a pty and report the slave's device name.
 *
 * @param pts_name  buffer that receives the slave device name
 * @param maxlen    size of that buffer, terminator included
 * @return          the master descriptor, or a negative value on failure
 *
 * `maxlen` is honored: a buffer too small for the device name is a
 * failure (-5, `errno` `EOVERFLOW`) rather than a truncated name.  The
 * negative returns distinguish *where* the sequence failed - the open
 * (-1), `grantpt` (-2), `unlockpt` (-3), `ptsname` (-4), the buffer
 * (-5) - and `errno` is preserved across the cleanup in each case.
 * Nothing is left open on failure.
 *
 * Note that `ptsname(3)` answers out of static storage, so two threads
 * inside this function at once can read each other's answer.
 */
PMIX_EXPORT int pmix_ptymopen(char *pts_name, size_t maxlen);

/**
 * Open the slave side of a pty whose master is already open.
 *
 * @param fdm       the master descriptor, as returned by pmix_ptymopen
 * @param pts_name  the slave device name it reported
 * @return          the slave descriptor, or a negative value on failure
 *
 * **The master descriptor stays the caller's.** This function neither
 * closes it nor keeps it - it is in the signature for the platforms
 * whose STREAMS setup needs it - so a caller unwinding after a failure
 * here must close it itself.  Nothing this function opened survives a
 * failure.
 */
PMIX_EXPORT int pmix_ptysopen(int fdm, char *pts_name);

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
 * Unlike the helpers above, the child of this call *does* get the pty as
 * its controlling terminal - that is what forkpty(3) is for.
 */
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
