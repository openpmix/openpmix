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
 * Copyright (c) 2013-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2026      Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Exercise the server-armed group construct timeout. Unlike simpgrpcabort - in
 * which the missing member DROPS its connection (a lost participant) - here the
 * last rank stays ALIVE but simply never calls PMIx_Group_construct. The
 * survivors call PMIx_Group_construct with a PMIX_TIMEOUT; because the local
 * phase can never complete (a live member never contributes), the PMIx server's
 * local-phase timer must fire and complete the construct with PMIX_ERR_TIMEOUT
 * rather than hanging forever. Each survivor verifies its PMIx_Group_construct
 * returned PMIX_ERR_TIMEOUT and prints a distinctive success line.
 *
 * See src/server/pmix_server_group.c (group_timeout / abort_construct) and
 * docs/how-things-work/collectives.
 */

#include "src/include/pmix_config.h"
#include "include/pmix.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "src/include/pmix_globals.h"
#include "src/util/pmix_output.h"

static pmix_proc_t myproc;

int main(int argc, char **argv)
{
    pmix_status_t rc;
    pmix_value_t *val = NULL;
    pmix_proc_t proc, *procs;
    uint32_t nprocs, n, absentee;
    pmix_info_t tmo;
    pmix_info_t *results = NULL;
    size_t nresults = 0;
    int timeout = 2;
    PMIX_HIDE_UNUSED_PARAMS(argc, argv);

    if (PMIX_SUCCESS != (rc = PMIx_Init(&myproc, NULL, 0))) {
        fprintf(stderr, "Client PMIx_Init failed: %s\n", PMIx_Error_string(rc));
        exit(1);
    }

    /* get our job size */
    PMIX_LOAD_PROCID(&proc, myproc.nspace, PMIX_RANK_WILDCARD);
    if (PMIX_SUCCESS != (rc = PMIx_Get(&proc, PMIX_JOB_SIZE, NULL, 0, &val))) {
        fprintf(stderr, "Client %s:%d PMIx_Get job size failed: %s\n", myproc.nspace,
                myproc.rank, PMIx_Error_string(rc));
        exit(1);
    }
    nprocs = val->data.uint32;
    PMIX_VALUE_RELEASE(val);
    if (nprocs < 2) {
        fprintf(stderr, "Client %s:%d FAIL: this test requires at least 2 processes\n",
                myproc.nspace, myproc.rank);
        exit(1);
    }

    /* sync everyone up first */
    PMIX_LOAD_PROCID(&proc, myproc.nspace, PMIX_RANK_WILDCARD);
    if (PMIX_SUCCESS != (rc = PMIx_Fence(&proc, 1, NULL, 0))) {
        fprintf(stderr, "Client %s:%d PMIx_Fence failed: %s\n", myproc.nspace,
                myproc.rank, PMIx_Error_string(rc));
        exit(1);
    }

    /* the entire job is the group membership; the last rank is the absentee */
    absentee = nprocs - 1;

    if (myproc.rank == absentee) {
        /* stay ALIVE but never contribute to the construct. Sleep well past the
         * survivors' timeout so their construct times out on a live (not lost)
         * member, then finalize cleanly. */
        fprintf(stderr, "%d NOT joining the construct (staying alive)\n", myproc.rank);
        sleep(timeout + 3);
        goto done;
    }

    PMIX_PROC_CREATE(procs, nprocs);
    for (n = 0; n < nprocs; n++) {
        PMIX_PROC_LOAD(&procs[n], myproc.nspace, n);
    }
    /* bound the wait: a live member never contributes, so the server's
     * local-phase timer must complete us with PMIX_ERR_TIMEOUT */
    PMIX_INFO_LOAD(&tmo, PMIX_TIMEOUT, &timeout, PMIX_INT);
    rc = PMIx_Group_construct("timeoutgroup", procs, nprocs, &tmo, 1, &results, &nresults);
    PMIX_PROC_FREE(procs, nprocs);
    PMIX_INFO_DESTRUCT(&tmo);
    if (NULL != results) {
        PMIX_INFO_FREE(results, nresults);
    }

    if (PMIX_ERR_TIMEOUT != rc) {
        fprintf(stderr, "Client %s:%d FAIL: construct returned %s, expected "
                "PMIX_ERR_TIMEOUT\n", myproc.nspace, myproc.rank, PMIx_Error_string(rc));
        PMIx_Finalize(NULL, 0);
        exit(1);
    }
    fprintf(stderr, "Client %s:%d saw construct TIMEOUT (live member %d absent) OK\n",
            myproc.nspace, myproc.rank, absentee);

done:
    if (PMIX_SUCCESS != (rc = PMIx_Finalize(NULL, 0))) {
        fprintf(stderr, "Client %s:%d PMIx_Finalize failed: %s\n", myproc.nspace,
                myproc.rank, PMIx_Error_string(rc));
        exit(1);
    }
    fflush(stderr);
    return 0;
}
