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
 * Copyright (c) 2019-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2021-2026 Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "src/include/pmix_config.h"

#include <errno.h>
#include <stdio.h>
#ifdef HAVE_SYS_WAIT_H
#    include <sys/wait.h>
#endif
#include <stdlib.h>
#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif

#include "pmix_common.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_basename.h"
#include "src/util/pmix_few.h"

PMIX_EXPORT pmix_status_t pmix_few(char *argv[], int *status)
{
#if defined(HAVE_FORK) && defined(HAVE_EXECVE) && defined(HAVE_WAITPID)
    pid_t pid, ret;

    if ((pid = fork()) < 0) {
        return PMIX_ERROR;
    }

    /* Child execs.  If it fails to exec, exit. */

    else if (0 == pid) {
        execvp(argv[0], argv);
        /* _exit, not exit: the child shares the parent's stdio buffers,
         * and exit() flushes them - so anything the parent had written
         * but not yet flushed goes out a second time, from here.  It
         * also runs atexit handlers registered by the parent, which in
         * a threaded process can deadlock on a lock some other thread
         * held at fork time. */
        _exit(errno);
    }

    /* Parent loops waiting for the child to die. */

    else {
        do {
            /* If the child exited, return */

            if (pid == (ret = waitpid(pid, status, 0))) {
                break;
            }

            /* If waitpid was interrupted, loop around again */

            else if (ret < 0) {
                if (EINTR == errno) {
                    continue;
                }

                /* Otherwise, some bad juju happened -- need to quit */

                return PMIX_ERROR;
            }
        } while (true);
    }

    /* Return the status to the caller */

    return PMIX_SUCCESS;
#else
    return PMIX_ERR_NOT_SUPPORTED;
#endif
}
