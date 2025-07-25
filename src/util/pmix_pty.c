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
#    ifdef HAVE_STROPTS_H
#        include <stropts.h>
#    endif
#endif

#ifdef HAVE_UTIL_H
#    include <util.h>
#endif

#include "src/util/pmix_output.h"
#include "src/util/pmix_pty.h"


#if PMIX_ENABLE_PTY_SUPPORT == 0

PMIX_EXPORT int pmix_ptymopen(char *pts_name)
{
    PMIX_HIDE_UNUSED_PARAMS(pts_name);
    return -1
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
    int errsave;

#    ifdef HAVE_PTSNAME
    char *ptr;

#        ifdef _AIX
    strncpy(pts_name, "/dev/ptc", maxlen);
#        else
    strncpy(pts_name, "/dev/ptmx", maxlen);
#        endif
    fdm = open(pts_name, O_RDWR);
    if (fdm < 0) {
        return -1;
    }
    if (grantpt(fdm) < 0) { /* grant access to slave */
        errsave = errno;
        close(fdm);  // might change errno
        errno = errsave;
        return -2;
    }
    if (unlockpt(fdm) < 0) { /* clear slave's lock flag */
        errsave = errno;
        close(fdm);  // might change errno
        errno = errsave;
        return -3;
    }
    ptr = ptsname(fdm);
    if (ptr == NULL) { /* get slave's name */
        errsave = errno;
        close(fdm);  // might change errno
        errno = errsave;
        return -4;
    }
    if (strlen(ptr) < maxlen) {
        strncpy(pts_name, ptr, maxlen); /* return name of slave */
        return fdm;            /* return fd of master */
    } else {
        close(fdm);
        errno = EOVERFLOW;
        return -5;
    }

#    else  // HAVE_PTSNAME

    char *ptr1, *ptr2;

    if (strlen("/dev/ptyXY") < maxlen-1) {
        strcpy(pts_name, "/dev/ptyXY");
    } else {
        return PMIX_ERR_BAD_PARAM;
    }
    /* array index: 012345689 (for references in following code) */
    for (ptr1 = "pqrstuvwxyzPQRST"; *ptr1 != 0; ptr1++) {
        pts_name[8] = *ptr1;
        for (ptr2 = "0123456789abcdef"; *ptr2 != 0; ptr2++) {
            pts_name[9] = *ptr2;
            /* try to open master */
            fdm = open(pts_name, O_RDWR);
            if (fdm < 0) {
                if (errno == ENOENT) { /* different from EIO */
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
    int errsave;

#    ifdef HAVE_PTSNAME
    /* following should allocate controlling terminal */
    fds = open(pts_name, O_RDWR);
    if (fds < 0) {
        pmix_output(0, "FAILED WITH %s", strerror(errno));
        errsave = errno;
        close(fdm);  // might change errno
        errno = errsave;
        return -5;
    }
#        if defined(__SVR4) && defined(__sun)
    if (ioctl(fds, I_PUSH, "ptem") < 0) {
        errsave = errno;
        close(fdm);  // might change errno
        close(fds);  // might change errno
        errno = errsave;
        return -6;
    }
    if (ioctl(fds, I_PUSH, "ldterm") < 0) {
        errsave = errno;
        close(fdm);  // might change errno
        close(fds);  // might change errno
        errno = errsave;
        return -7;
    }
#        endif

#ifdef TIOCSCTTY                        /* Acquire controlling tty on BSD */
        // just make the attempt - don't worry if it fails
        ioctl(fds, TIOCSCTTY, 0);
#endif

    return fds;

#    else

    int gid;
    struct group *grptr;

    grptr = getgrnam("tty");
    if (grptr != NULL) {
        gid = grptr->gr_gid;
    } else {
        gid = -1; /* group tty is not in the group file */
    }
    /* following two functions don't work unless we're root */
    lchown(pts_name, getuid(), gid);  // DO NOT FOLLOW LINKS
    chmod(pts_name, S_IRUSR | S_IWUSR | S_IWGRP);
    fds = open(pts_name, O_RDWR);
    if (fds < 0) {
        errsave = errno;
        close(fdm);  // might change errno
        errno = errsave;
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
    *amaster = pmix_ptymopen(line);
    if (*amaster < 0) {
        return -1;
    }
    *aslave = pmix_ptysopen(*amaster, line);
    if (*aslave < 0) {
        close(*amaster);
        return -1;
    }
    if (name) {
        // We don't know the max length of name, but we do know the
        // max length of the source, so at least use that.
        pmix_string_copy(name, line, sizeof(line));
    }
#    ifndef TCSAFLUSH
#        define TCSAFLUSH TCSETAF
#    endif
    if (termp) {
        (void) tcsetattr(*aslave, TCSAFLUSH, termp);
    }
#    ifdef TIOCSWINSZ
    if (winp) {
        (void) ioctl(*aslave, TIOCSWINSZ, (char *) winp);
    }
#    endif
    return 0;
}

#endif /* #ifdef HAVE_OPENPTY */


#if PMIX_ENABLE_PTY_SUPPORT == 0

pid_t pmix_forkpty(int *master, char *slave, size_t len,
                   const struct termios *sterm,
                   const struct winsize *sws)
{
    PMIX_HIDE_UNUSED_PARAMS(master, slave, len, sterm, sws);
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
