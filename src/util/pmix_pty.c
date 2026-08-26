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
/*-
 * Copyright (c) 1990, 1993
 *      The Regents of the University of California.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 4. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include "src/include/pmix_config.h"

#ifdef HAVE_SYS_CDEFS_H
#    include <sys/cdefs.h>
#endif
#ifdef HAVE_SYS_TYPES_H
#    include <sys/types.h>
#endif
#include <sys/stat.h>
#ifdef HAVE_SYS_IOCTL_H
#    include <sys/ioctl.h>
#endif
#ifdef HAVE_FCNTL_H
#    include <fcntl.h>
#endif
#ifdef HAVE_TERMIOS_H
#    include <termios.h>
#else
#    ifdef HAVE_TERMIO_H
#        include <termio.h>
#    endif
#endif
#include <errno.h>
#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif
#include <stdio.h>
#include <string.h>
#ifdef HAVE_GRP_H
#    include <grp.h>
#endif
#ifdef HAVE_PTY_H
#    include <pty.h>
#endif
#ifdef HAVE_UTMP_H
#    include <utmp.h>
#endif

#ifdef HAVE_PTSNAME
#    include <stdlib.h>
#endif

#ifdef HAVE_UTIL_H
#    include <util.h>
#endif

#include "src/include/pmix_globals.h"
#include "src/util/pmix_output.h"
#include "src/util/pmix_pty.h"
#include "src/util/pmix_string_copy.h"


#if PMIX_ENABLE_PTY_SUPPORT == 0

PMIX_EXPORT int pmix_ptymopen(char *pts_name, size_t maxlen)
{
    PMIX_HIDE_UNUSED_PARAMS(pts_name, maxlen);
    return -1;
}

PMIX_EXPORT int pmix_ptysopen(int fdm, char *pts_name)
{
    PMIX_HIDE_UNUSED_PARAMS(fdm, pts_name);
    return -1;
}

#else

int pmix_ptymopen(char *pts_name, size_t maxlen)
{
    int fdm;

#    ifdef HAVE_PTSNAME
    int errsave;
    char *ptr;

#        ifdef _AIX
    static const char master[] = "/dev/ptc";
#        else
    static const char master[] = "/dev/ptmx";
#        endif

    /* strncpy() would leave pts_name unterminated for a short maxlen,
     and the open() below would then read off the end of it */
    if (maxlen < sizeof(master)) {
        errno = EOVERFLOW;
        return -5;
    }
    pmix_string_copy(pts_name, master, maxlen);
    fdm = open(pts_name, O_RDWR | O_NOCTTY);
    if (0 > fdm) {
        return -1;
    }
    if (0 > grantpt(fdm)) { /* grant access to slave */
        errsave = errno;
        close(fdm);  // might change errno
        errno = errsave;
        return -2;
    }
    if (0 > unlockpt(fdm)) { /* clear slave's lock flag */
        errsave = errno;
        close(fdm);  // might change errno
        errno = errsave;
        return -3;
    }
    ptr = ptsname(fdm);
    if (NULL == ptr) { /* get slave's name */
        errsave = errno;
        close(fdm);  // might change errno
        errno = errsave;
        return -4;
    }
    if (strlen(ptr) < maxlen) {
        pmix_string_copy(pts_name, ptr, maxlen); /* return name of slave */
        return fdm;            /* return fd of master */
    } else {
        close(fdm);
        errno = EOVERFLOW;
        return -5;
    }

#    else  // HAVE_PTSNAME

    static const char ptyname[] = "/dev/ptyXY";
    const char *ptr1, *ptr2;

    /* maxlen is unsigned: "maxlen - 1" wraps for a maxlen of zero, and
     the strcpy below would then run off the end of the buffer */
    if (maxlen < sizeof(ptyname)) {
        errno = EOVERFLOW;
        return -5;
    }
    pmix_string_copy(pts_name, ptyname, maxlen);
    /* array index: 0123456789 (for references in following code) */
    for (ptr1 = "pqrstuvwxyzPQRST"; 0 != *ptr1; ptr1++) {
        pts_name[8] = *ptr1;
        for (ptr2 = "0123456789abcdef"; 0 != *ptr2; ptr2++) {
            pts_name[9] = *ptr2;
            /* try to open master */
            fdm = open(pts_name, O_RDWR | O_NOCTTY);
            if (0 > fdm) {
                if (ENOENT == errno) { /* different from EIO */
                    return -1;         /* out of pty devices */
                } else {
                    continue; /* try next pty device */
                }
            }
            pts_name[5] = 't'; /* change "pty" to "tty" */
            return fdm;        /* got it, return fd of master */
        }
    }
    return -1; /* out of pty devices */

#    endif
}

int pmix_ptysopen(int fdm, char *pts_name)
{
    int fds;

    /* the master descriptor belongs to our caller: we neither use nor
     close it.  It stays in the signature because the signature is
     frozen - the last route that needed it was the STREAMS module push
     on Solaris, which this library no longer supports. */
    PMIX_HIDE_UNUSED_PARAMS(fdm);

#    ifdef HAVE_PTSNAME
    /* O_NOCTTY: opening a terminal device from a session leader that has
     no controlling terminal makes it one, and this runs in the process
     opening the pty rather than in the child that will use it */
    fds = open(pts_name, O_RDWR | O_NOCTTY);
    if (0 > fds) {
        pmix_output_verbose(2, pmix_globals.debug_output,
                            "pmix_ptysopen: could not open %s: %s",
                            pts_name, strerror(errno));
        return -5;
    }
    /* Deliberately NO TIOCSCTTY here.  This runs in the process that is
     opening the pty, not in the child that will use it, and TIOCSCTTY
     succeeds for any session leader that has no controlling terminal -
     which is exactly the shape of a daemonized PMIx server.  It would
     then be the *server* whose controlling terminal is the child's pty,
     so a hangup on that pty reaches the server.  A caller who wants the
     pty to be a controlling terminal has to say so after setsid() in
     the child, the way forkpty()/login_tty() do. */

    return fds;

#    else

    gid_t gid;
    struct group *grptr;

    grptr = getgrnam("tty");
    if (NULL != grptr) {
        gid = grptr->gr_gid;
    } else {
        gid = (gid_t) -1; /* group tty is not in the group file */
    }
    /* following two functions don't work unless we're root */
    lchown(pts_name, getuid(), gid);  // DO NOT FOLLOW LINKS
    chmod(pts_name, S_IRUSR | S_IWUSR | S_IWGRP);
    fds = open(pts_name, O_RDWR | O_NOCTTY);
    if (0 > fds) {
        return -1;
    }
    return fds;
#    endif
}

#endif

#if PMIX_ENABLE_PTY_SUPPORT == 0

int pmix_openpty(int *amaster, int *aslave, char *name,
                 void *termp, void *winpp)
{
    PMIX_HIDE_UNUSED_PARAMS(amaster, aslave, name, termp, winpp);
    return -1;
}


#elif defined(HAVE_OPENPTY)

int pmix_openpty(int *amaster, int *aslave, char *name,
                 struct termios *termp, struct winsize *winp)
{
    return openpty(amaster, aslave, name, termp, winp);
}

#else

/* implement openpty in terms of pmix_ptymopen and pmix_ptysopen */

int pmix_openpty(int *amaster, int *aslave, char *name,
                 struct termios *termp, struct winsize *winp)
{
    char line[20];
    *amaster = pmix_ptymopen(line, sizeof(line));
    if (0 > *amaster) {
        return -1;
    }
    *aslave = pmix_ptysopen(*amaster, line);
    if (0 > *aslave) {
        close(*amaster);
        return -1;
    }
    if (NULL != name) {
        // We don't know the max length of name, but we do know the
        // max length of the source, so at least use that.
        pmix_string_copy(name, line, sizeof(line));
    }
#    ifndef TCSAFLUSH
#        define TCSAFLUSH TCSETAF
#    endif
    if (NULL != termp) {
        (void) tcsetattr(*aslave, TCSAFLUSH, termp);
    }
#    ifdef TIOCSWINSZ
    if (NULL != winp) {
        (void) ioctl(*aslave, TIOCSWINSZ, (char *) winp);
    }
#    endif
    return 0;
}

#endif /* #ifdef HAVE_OPENPTY */


#if PMIX_ENABLE_PTY_SUPPORT == 0

pid_t pmix_forkpty(int *master, char *slave,
                   const void *sterm, const void *sws)
{
    PMIX_HIDE_UNUSED_PARAMS(master, slave, sterm, sws);
    return -1;
}


#elif defined(HAVE_FORKPTY)

pid_t pmix_forkpty(int *master, char *slave,
                   const struct termios *sterm,
                   const struct winsize *sws)
{
    // some OS don't have the "const" in the above declaration
    return forkpty(master, slave, (struct termios *)sterm, (struct winsize *)sws);
}

#else

pid_t pmix_forkpty(int *master, char *slave,
                   const struct termios *sterm,
                   const struct winsize *sws)
{
    PMIX_HIDE_UNUSED_PARAMS(master, slave, sterm, sws);
    return -1;
}

#endif
