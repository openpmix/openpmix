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
 * Copyright (c) 2016      Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2019      IBM Corporation.  All rights reserved.
 * Copyright (c) 2021-2025 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 */

#include <stdbool.h>

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <sys/param.h>
#include <time.h>
#include <unistd.h>
#include <libgen.h>

#include "examples.h"
#include <pmix.h>

static pmix_proc_t myproc;

int main(int argc, char **argv)
{
    int rc;
    pmix_value_t *val = NULL, value;
    pmix_proc_t proc, *parray;
    uint32_t nprocs;
    size_t nall, n, maxprocs = 2;
    char nsp2[PMIX_MAX_NSLEN + 1], *nsp;
    pmix_app_t *app;
    char hostname[1024], dir[1024];
    pmix_info_t *results = NULL, info;
    size_t nresults = 0;

    EXAMPLES_HIDE_UNUSED_PARAMS(argc, argv);

    if (0 > gethostname(hostname, sizeof(hostname))) {
        exit(1);
    }
    if (NULL == getcwd(dir, 1024)) {
        exit(1);
    }

    /* init us */
    if (PMIX_SUCCESS != (rc = PMIx_Init(&myproc, NULL, 0))) {
        fprintf(stderr, "Client ns %s rank %d: PMIx_Init failed: %d\n", myproc.nspace, myproc.rank,
                rc);
        exit(0);
    }
    fprintf(stderr, "Client ns %s rank %d: Running on host %s\n", myproc.nspace, myproc.rank, hostname);

    PMIX_PROC_CONSTRUCT(&proc);
    PMIX_LOAD_PROCID(&proc, myproc.nspace, PMIX_RANK_WILDCARD);

    /* get our job size */
    if (PMIX_SUCCESS != (rc = PMIx_Get(&proc, PMIX_JOB_SIZE, NULL, 0, &val))) {
        fprintf(stderr, "Client ns %s rank %d: PMIx_Get job size failed: %d\n", myproc.nspace,
                myproc.rank, rc);
        goto done;
    }
    nprocs = val->data.uint32;
    PMIX_VALUE_RELEASE(val);
    fprintf(stderr, "Client %s:%d job size %d\n", myproc.nspace, myproc.rank, nprocs);

    /* call fence to sync */
    PMIX_LOAD_PROCID(&proc, myproc.nspace, PMIX_RANK_WILDCARD);
    if (PMIX_SUCCESS != (rc = PMIx_Fence(&proc, 1, NULL, 0))) {
        fprintf(stderr, "Client ns %s rank %d: PMIx_Fence failed: %d\n", myproc.nspace, myproc.rank,
                rc);
        goto done;
    }

    if (NULL == getenv("PMIX_ENV_VALUE")) {
        // we are the parent
        /* rank=0 calls spawn */
        if (0 == myproc.rank) {
            nsp = basename(argv[0]);
            PMIX_APP_CREATE(app, 1);
            app->cmd = strdup(argv[0]);
            app->maxprocs = maxprocs;
            app->argv = (char **) malloc(4 * sizeof(char *));
            app->argv[0] = strdup(nsp);
            app->argv[1] = strdup(myproc.nspace);
            if (0 > asprintf(&app->argv[2], "%d", nprocs)) {
                goto done;
            }
            app->argv[3] = NULL;
            app->env = (char **) malloc(2 * sizeof(char *));
            app->env[0] = strdup("PMIX_ENV_VALUE=3");
            app->env[1] = NULL;

            PMIX_INFO_LOAD(&info, PMIX_MAPBY, "node", PMIX_STRING);
            fprintf(stderr, "Client ns %s rank %d: calling PMIx_Spawn\n", myproc.nspace, myproc.rank);
            if (PMIX_SUCCESS != (rc = PMIx_Spawn(&info, 1, app, 1, nsp2))) {
                fprintf(stderr, "Client ns %s rank %d: PMIx_Spawn failed: %d\n", myproc.nspace,
                        myproc.rank, rc);
                goto done;
            }
            PMIX_APP_FREE(app, 1);

            // share the child namespace
            value.type = PMIX_STRING;
            value.data.string = nsp2;
            if (PMIX_SUCCESS != (rc = PMIx_Put(PMIX_GLOBAL, "child", &value))) {
                fprintf(stderr, "Client ns %s rank %d: PMIx_Put child nspace failed: %s\n", myproc.nspace,
                        myproc.rank, PMIx_Error_string(rc));
                goto done;
            }
            PMIx_Commit();

            // wait to sync with others
            nsp = nsp2;
            PMIX_LOAD_PROCID(&proc, myproc.nspace, PMIX_RANK_WILDCARD);
            rc = PMIx_Fence(&proc, 1, NULL, 0);

            // everybody calls connect
            nall = nprocs + maxprocs;
            PMIX_PROC_CREATE(parray, nall);
            for (n=0; n < nprocs; n++) {
                PMIX_LOAD_PROCID(&parray[n], myproc.nspace, n);
            }
            for (n=0; n < maxprocs; n++) {
                PMIX_LOAD_PROCID(&parray[n+nprocs], nsp, n);
            }

        } else {
            PMIX_LOAD_PROCID(&proc, myproc.nspace, PMIX_RANK_WILDCARD);
            rc = PMIx_Fence(&proc, 1, NULL, 0);
            // retrieve the child nspace
            proc.rank = 0;
            rc = PMIx_Get(&proc, "child", NULL, 0, &val);
            if (PMIX_SUCCESS != rc) {
                fprintf(stderr, "Client ns %s rank %d: PMIx_Get child nspace failed: %s\n", myproc.nspace,
                        myproc.rank, PMIx_Error_string(rc));
                goto done;
            }
            nsp = val->data.string;

            // everybody calls connect
            nall = nprocs + maxprocs;
            PMIX_PROC_CREATE(parray, nall);
            for (n=0; n < nprocs; n++) {
                PMIX_LOAD_PROCID(&parray[n], myproc.nspace, n);
            }
            for (n=0; n < maxprocs; n++) {
                PMIX_LOAD_PROCID(&parray[n+nprocs], nsp, n);
            }
        }
    } else {
        // we are the child job
        nsp = argv[1];
        maxprocs = atoi(argv[2]);
        // everybody calls connect
        nall = nprocs + maxprocs;
        PMIX_PROC_CREATE(parray, nall);
        for (n=0; n < maxprocs; n++) {
            PMIX_LOAD_PROCID(&parray[n], nsp, n);
        }
        for (n=0; n < nprocs; n++) {
            PMIX_LOAD_PROCID(&parray[n+maxprocs], myproc.nspace, n);
        }
    }


    fprintf(stderr, "Client ns %s rank %d: calling PMIx_Connect\n", myproc.nspace, myproc.rank);
    rc = PMIx_Connect(parray, nall, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "Client ns %s rank %d: PMIx_Connect failed: %s\n", myproc.nspace,
                myproc.rank, PMIx_Error_string(rc));
        goto done;
    }
    if (0 == myproc.rank) {
        fprintf(stderr, "Client ns %s rank %d: PMIx_Connect GOOD\n", myproc.nspace, myproc.rank);
    }
    rc = PMIx_Disconnect(parray, nall, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "Client ns %s rank %d: PMIx_Disonnect failed: %s\n", myproc.nspace,
                myproc.rank, PMIx_Error_string(rc));
        goto done;
    }
    if (0 == myproc.rank) {
        fprintf(stderr, "Client ns %s rank %d: PMIx_Disconnect GOOD\n", myproc.nspace, myproc.rank);
    }
    rc = PMIx_Group_construct("mygrp", parray, nall, NULL, 0, &results, &nresults);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "Client ns %s rank %d: PMIx_Group_construct failed: %s\n", myproc.nspace,
                myproc.rank, PMIx_Error_string(rc));
        goto done;
    }
    if (0 == myproc.rank) {
        fprintf(stderr, "Client ns %s rank %d: PMIx_Group_construct GOOD\n", myproc.nspace, myproc.rank);
    }
    rc = PMIx_Group_destruct("mygrp", NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "Client ns %s rank %d: PMIx_Group_destruct failed: %s\n", myproc.nspace,
                myproc.rank, PMIx_Error_string(rc));
        goto done;
    }
    if (0 == myproc.rank) {
        fprintf(stderr, "Client ns %s rank %d: PMIx_Group_destruct GOOD\n", myproc.nspace, myproc.rank);
    }

done:
    /* finalize us */
    fprintf(stderr, "Client ns %s rank %d: Finalizing\n", myproc.nspace, myproc.rank);

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
