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
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
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

#ifdef HAVE_SYS_TYPES_H
#    include <sys/types.h>
#endif
#ifdef HAVE_PTY_H
#    include <pty.h>
#endif

#include "src/include/pmix_globals.h"
#include "src/util/pmix_pty.h"

/* Both functions are thin wrappers over the system's openpty(3) and
 * forkpty(3).  Without them there is no pty to be had, and the answer
 * is -1, on which every caller falls back to a pipe. */

#if PMIX_ENABLE_PTY_SUPPORT == 0

int pmix_openpty(int *amaster, int *aslave, char *name,
                 void *termp, void *winpp)
{
    PMIX_HIDE_UNUSED_PARAMS(amaster, aslave, name, termp, winpp);
    return -1;
}

pid_t pmix_forkpty(int *master, char *slave,
                   const void *sterm, const void *sws)
{
    PMIX_HIDE_UNUSED_PARAMS(master, slave, sterm, sws);
    return -1;
}

#else

int pmix_openpty(int *amaster, int *aslave, char *name,
                 struct termios *termp, struct winsize *winp)
{
#    if defined(HAVE_OPENPTY)
    return openpty(amaster, aslave, name, termp, winp);
#    else
    PMIX_HIDE_UNUSED_PARAMS(amaster, aslave, name, termp, winp);
    return -1;
#    endif
}

pid_t pmix_forkpty(int *master, char *slave,
                   const struct termios *sterm,
                   const struct winsize *sws)
{
#    if defined(HAVE_FORKPTY)
    // some OS don't have the "const" in the above declaration
    return forkpty(master, slave, (struct termios *) sterm, (struct winsize *) sws);
#    else
    PMIX_HIDE_UNUSED_PARAMS(master, slave, sterm, sws);
    return -1;
#    endif
}

#endif
