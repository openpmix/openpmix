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
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <libgen.h>

#include <pmix.h>

#include "examples.h"

#define NITER 3

int main(int argc, char **argv)
{
    pmix_proc_t myproc;
    int rc, n;
    pmix_value_t *val = NULL;
    pmix_proc_t proc;
    uint32_t nprocs;
    pmix_info_t info[2], lkinfo[2];
    pmix_pdata_t pdata;
    pmix_persistence_t firstread = PMIX_PERSIST_FIRST_READ;
    int timeout=10;
    char *tmp;
    int iters;

    if (1 < argc) {
        iters = strtol(argv[1], NULL, 10);
    } else {
        iters = NITER;
    }

    /* init us */
    if (PMIX_SUCCESS != (rc = PMIx_Init(&myproc, NULL, 0))) {
        fprintf(stderr, "Client ns %s rank %d: PMIx_Init failed: %d\n", myproc.nspace, myproc.rank,
                rc);
        exit(0);
    }

    /* get our job size */
    PMIX_PROC_CONSTRUCT(&proc);
    PMIX_LOAD_PROCID(&proc, myproc.nspace, PMIX_RANK_WILDCARD);
    if (PMIX_SUCCESS != (rc = PMIx_Get(&proc, PMIX_JOB_SIZE, NULL, 0, &val))) {
        fprintf(stderr, "Client ns %s rank %d: PMIx_Get job size failed: %d\n", myproc.nspace,
                myproc.rank, rc);
        goto done;
    }
    nprocs = val->data.uint32;
    PMIX_VALUE_RELEASE(val);
    // require at least 2 procs
    if (nprocs != 2) {
        if (0 == myproc.rank) {
            fprintf(stderr, "%s requires 2 processes\n", basename(argv[0]));
        }
        exit(1);
    }

    PMIX_INFO_LOAD(&info[1], PMIX_PERSISTENCE, &firstread, PMIX_PERSIST);
    PMIX_INFO_LOAD(&lkinfo[0], PMIX_WAIT, NULL, PMIX_BOOL);
    PMIX_INFO_LOAD(&lkinfo[1], PMIX_TIMEOUT, &timeout, PMIX_INT);
    for (n=0; n < iters; n++) {
        if (0 == myproc.rank) {
            /* publish something */
            if (0 > asprintf(&tmp, "FOOBAR:%s.%u:%d", myproc.nspace, myproc.rank, n)) {
                goto done;
            }
            PMIX_INFO_LOAD(&info[0], tmp, &n, PMIX_INT);
            free(tmp);
            if (PMIX_SUCCESS != (rc = PMIx_Publish(info, 2))) {
                fprintf(stderr, "Client ns %s rank %d: PMIx_Publish failed: %d\n", myproc.nspace,
                        myproc.rank, rc);
                goto done;
            }
            PMIX_INFO_DESTRUCT(&info[0]);

            /* lookup other rank's value */
            PMIX_PDATA_CONSTRUCT(&pdata);
            if (0 > asprintf(&tmp, "BAZ:%s.%u:%d", myproc.nspace, 1, n)) {
                goto done;
            }
            PMIX_LOAD_KEY(pdata.key, tmp);
            free(tmp);
            // lookup value
            if (PMIX_SUCCESS != (rc = PMIx_Lookup(&pdata, 1, lkinfo, 2))) {
                fprintf(stderr, "Client ns %s rank %d: PMIx_Lookup failed: %d\n", myproc.nspace,
                        myproc.rank, rc);
                goto done;
            }
            /* check the return for value and source */
            if (0 != strncmp(myproc.nspace, pdata.proc.nspace, PMIX_MAX_NSLEN)) {
                fprintf(stderr, "Client ns %s rank %d: PMIx_Lookup returned wrong nspace: %s\n",
                        myproc.nspace, myproc.rank, pdata.proc.nspace);
                goto done;
            }
            if (1 != pdata.proc.rank) {
                fprintf(stderr, "Client ns %s rank %d: PMIx_Lookup returned wrong rank: %d\n",
                        myproc.nspace, myproc.rank, pdata.proc.rank);
                goto done;
            }
            if (PMIX_INT != pdata.value.type) {
                fprintf(stderr, "Client ns %s rank %d: PMIx_Lookup returned wrong type: %d\n",
                        myproc.nspace, myproc.rank, pdata.value.type);
                goto done;
            }
            if (n != pdata.value.data.integer) {
                fprintf(stderr, "Client ns %s rank %d: PMIx_Lookup returned wrong value: %d\n",
                        myproc.nspace, myproc.rank, (int) pdata.value.data.uint8);
                goto done;
            }
            PMIX_PDATA_DESTRUCT(&pdata);
            fprintf(stderr, "PUBLISH-LOOKUP SUCCEEDED: %d\n", n);
        } else {
            /* lookup other rank's value */
            PMIX_PDATA_CONSTRUCT(&pdata);
            if (0 > asprintf(&tmp, "FOOBAR:%s.%u:%d", myproc.nspace, 0, n)) {
                goto done;
            }
            PMIX_LOAD_KEY(pdata.key, tmp);
            free(tmp);
            // check value
            if (PMIX_SUCCESS != (rc = PMIx_Lookup(&pdata, 1, lkinfo, 2))) {
                fprintf(stderr, "Client ns %s rank %d: PMIx_Lookup failed: %d\n", myproc.nspace,
                        myproc.rank, rc);
                goto done;
            }
            /* check the return for value and source */
            if (0 != strncmp(myproc.nspace, pdata.proc.nspace, PMIX_MAX_NSLEN)) {
                fprintf(stderr, "Client ns %s rank %d: PMIx_Lookup returned wrong nspace: %s\n",
                        myproc.nspace, myproc.rank, pdata.proc.nspace);
                goto done;
            }
            if (0 != pdata.proc.rank) {
                fprintf(stderr, "Client ns %s rank %d: PMIx_Lookup returned wrong rank: %d\n",
                        myproc.nspace, myproc.rank, pdata.proc.rank);
                goto done;
            }
            if (PMIX_INT != pdata.value.type) {
                fprintf(stderr, "Client ns %s rank %d: PMIx_Lookup returned wrong type: %d\n",
                        myproc.nspace, myproc.rank, pdata.value.type);
                goto done;
            }
            if (n != pdata.value.data.integer) {
                fprintf(stderr, "Client ns %s rank %d: PMIx_Lookup returned wrong value: %d\n",
                        myproc.nspace, myproc.rank, (int) pdata.value.data.uint8);
                goto done;
            }
            PMIX_PDATA_DESTRUCT(&pdata);
            /* publish something */
            if (0 > asprintf(&tmp, "BAZ:%s.%u:%d", myproc.nspace, myproc.rank, n)) {
                goto done;
            }
            PMIX_INFO_LOAD(&info[0], tmp, &n, PMIX_INT);
            free(tmp);
            if (PMIX_SUCCESS != (rc = PMIx_Publish(info, 2))) {
                fprintf(stderr, "Client ns %s rank %d: PMIx_Publish failed: %d\n", myproc.nspace,
                        myproc.rank, rc);
                goto done;
            }
            PMIX_INFO_DESTRUCT(&info[0]);
        }
    }

done:
    /* finalize us */
    if (PMIX_SUCCESS != (rc = PMIx_Finalize(NULL, 0))) {
        fprintf(stderr, "Client ns %s rank %d:PMIx_Finalize failed: %d\n", myproc.nspace,
                myproc.rank, rc);
    } else {
        fprintf(stderr, "Client ns %s rank %d:PMIx_Finalize successfully completed\n",
                myproc.nspace, myproc.rank);
    }
    fflush(stderr);
    return (0);
}
