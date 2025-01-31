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
 * Copyright (c) 2021-2025 Nanook Consulting  All rights reserved.
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

static pmix_proc_t myproc;

int main(int argc, char **argv)
{
    pmix_status_t rc;
    pmix_proc_t *procs;
    size_t nprocs, n;
    pid_t pid;
    char *nodelist;
    pmix_nspace_t nspace;
    EXAMPLES_HIDE_UNUSED_PARAMS(argc, argv);

    pid = getpid();
    fprintf(stderr, "Client %lu: Running\n", (unsigned long) pid);

    /* init us - note that the call to "init" includes the return of
     * any job-related info provided by the RM. This includes any
     * debugger flag instructing us to stop-in-init. If such a directive
     * is included, then the process will be stopped in this call until
     * the "debugger release" notification arrives */
    if (PMIX_SUCCESS != (rc = PMIx_Init(&myproc, NULL, 0))) {
        fprintf(stderr, "Client ns %s rank %d: PMIx_Init failed: %s\n", myproc.nspace, myproc.rank,
                PMIx_Error_string(rc));
        exit(0);
    }
    fprintf(stderr, "Client ns %s rank %d pid %lu: Running\n", myproc.nspace, myproc.rank,
            (unsigned long) pid);

    rc = PMIx_Resolve_peers(NULL, myproc.nspace, &procs, &nprocs);
    fprintf(stderr, "ResPeers returned: %s\n", PMIx_Error_string(rc));
    if (PMIX_SUCCESS == rc) {
        for (n=0; n < nprocs; n++) {
            fprintf(stderr, "\t%s:%u\n", procs[n].nspace, procs[n].rank);
        }
    }

    rc = PMIx_Resolve_nodes(myproc.nspace, &nodelist);
    fprintf(stderr, "ResNodes returned: %s\n", PMIx_Error_string(rc));
    if (PMIX_SUCCESS == rc) {
        fprintf(stderr, "\t%s\n", nodelist);
    }

    // now do global request
    PMIX_LOAD_NSPACE(nspace, NULL);
    rc = PMIx_Resolve_peers(NULL, nspace, &procs, &nprocs);
    fprintf(stderr, "ResPeers global returned: %s\n", PMIx_Error_string(rc));
    if (PMIX_SUCCESS == rc) {
        for (n=0; n < nprocs; n++) {
            fprintf(stderr, "\t%s:%u\n", procs[n].nspace, procs[n].rank);
        }
    }

    rc = PMIx_Resolve_nodes(nspace, &nodelist);
    fprintf(stderr, "ResNodes global returned: %s\n", PMIx_Error_string(rc));
    if (PMIX_SUCCESS == rc) {
        fprintf(stderr, "\t%s\n", nodelist);
    }

    /* finalize us */
    rc = PMIx_Finalize(NULL, 0);
    fflush(stderr);
    return (rc);
}
