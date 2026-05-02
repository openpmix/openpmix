/*
 * Copyright (c) 2004-2010 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2011 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2006-2013 Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2009-2012 Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2011      Oak Ridge National Labs.  All rights reserved.
 * Copyright (c) 2013-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2015      Mellanox Technologies, Inc.  All rights reserved.
 * Copyright (c) 2019      IBM Corporation.  All rights reserved.
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
 * Copyright (c) 2022      ParTec AG.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 */

#define _GNU_SOURCE
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include "examples.h"
#include <pmix.h>

int main(int argc, char **argv)
{
    pmix_status_t rc;
    pid_t pid;
    pmix_proc_t myproc;

    EXAMPLES_HIDE_UNUSED_PARAMS(argc, argv);

    pid = getpid();
    fprintf(stderr, "Client %lu: Running\n", (unsigned long) pid);

    /* init us - note that the call to "init" includes the return of
     * any job-related info provided by the RM. This includes any
     * debugger flag instructing us to stop-in-init. If such a directive
     * is included, then the process will be stopped in this call until
     * the "debugger release" notification arrives */
    if (PMIX_SUCCESS != (rc = PMIx_Init(&myproc, NULL, 0))) {
        if (PMIX_ERR_UNREACH == rc) {
            fprintf(stderr, "Client ns %s rank %d: Cannot operate as singleton\n",
                    myproc.nspace, myproc.rank);
        } else {
            fprintf(stderr, "Client ns %s rank %d: PMIx_Init failed: %s\n",
                    myproc.nspace, myproc.rank,
                    PMIx_Error_string(rc));
        }
        exit(1);
    }
    fprintf(stderr, "Client ns %s rank %d pid %lu: Running\n", myproc.nspace, myproc.rank,
            (unsigned long) pid);

    // rank=0 waits for a few seconds
    if (0 == myproc.rank) {
        sleep(2);
        // now call abort
        PMIx_Abort(PMIX_ERROR, "die", NULL, 0);
    } else {
        sleep(10);
        /* finalize us */
        fprintf(stderr, "Client ns %s rank %d: Finalizing\n", myproc.nspace, myproc.rank);
        if (PMIX_SUCCESS != (rc = PMIx_Finalize(NULL, 0))) {
            fprintf(stderr, "Client ns %s rank %d:PMIx_Finalize failed: %s\n", myproc.nspace,
                    myproc.rank, PMIx_Error_string(rc));
        } else {
            fprintf(stderr, "Client ns %s rank %d:PMIx_Finalize successfully completed\n",
                    myproc.nspace, myproc.rank);
        }
        fflush(stderr);
    }
    return (0);
}
