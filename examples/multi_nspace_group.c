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
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <libgen.h>

#include <pmix.h>
#include "examples.h"

static pmix_proc_t myproc;

static void notification_fn(size_t evhdlr_registration_id, pmix_status_t status,
                            const pmix_proc_t *source, pmix_info_t info[], size_t ninfo,
                            pmix_info_t results[], size_t nresults,
                            pmix_event_notification_cbfunc_fn_t cbfunc, void *cbdata)
{
    EXAMPLES_HIDE_UNUSED_PARAMS(evhdlr_registration_id, source,
                                info, ninfo, results, nresults,
                                cbfunc, cbdata);

    fprintf(stderr, "Client %s:%d NOTIFIED with status %d\n", myproc.nspace, myproc.rank, status);
}

static void op_callbk(pmix_status_t status, void *cbdata)
{
    mylock_t *lock = (mylock_t *) cbdata;

    lock->status = status;
    DEBUG_WAKEUP_THREAD(lock);
}

static void errhandler_reg_callbk(pmix_status_t status, size_t errhandler_ref, void *cbdata)
{
    mylock_t *lock = (mylock_t *) cbdata;
    EXAMPLES_HIDE_UNUSED_PARAMS(errhandler_ref);

    lock->status = status;
    DEBUG_WAKEUP_THREAD(lock);
}

int main(int argc, char **argv)
{
    int rc;
    pmix_value_t *val = NULL;
    uint32_t nprocs;
    pmix_proc_t proc, parray[2], *pptr;
    pmix_value_t value;
    mylock_t lock;
    pmix_info_t *results = NULL, info[3], tinfo;
    size_t nresults, cid, n, m, psize=0, maxprocs = 2;
    char hostname[1024], dir[1024];
    char tmp[1024];
    pmix_app_t app;
    char nsp2[PMIX_MAX_NSLEN + 1];
    pid_t pid;

    EXAMPLES_HIDE_UNUSED_PARAMS(argc, argv);

    gethostname(hostname, 1024);
    if (NULL == getcwd(dir, 1024)) {
        fprintf(stderr, "Getcwd failure\n");
        exit(1);
    }
    pid = getpid();

    if (1 < argc) {
        maxprocs = atoi(argv[1]);
    }

    /* init us */
    if (PMIX_SUCCESS != (rc = PMIx_Init(&myproc, NULL, 0))) {
        fprintf(stderr, "Client ns %s rank %d: PMIx_Init failed: %s\n", myproc.nspace, myproc.rank,
                PMIx_Error_string(rc));
        exit(0);
    }
    fprintf(stderr, "Client ns %s rank %d: Running on host %s (%u)\n",
            myproc.nspace, myproc.rank, hostname, (unsigned)pid);

    PMIX_PROC_CONSTRUCT(&proc);
    PMIX_LOAD_PROCID(&proc, myproc.nspace, PMIX_RANK_WILDCARD);

    /* get our job size */
    if (PMIX_SUCCESS != (rc = PMIx_Get(&proc, PMIX_JOB_SIZE, NULL, 0, &val))) {
        fprintf(stderr, "Client ns %s rank %d: PMIx_Get job size failed: %s\n", myproc.nspace,
                myproc.rank, PMIx_Error_string(rc));
        goto done;
    }
    nprocs = val->data.uint32;
    PMIX_VALUE_RELEASE(val);
    if (nprocs < 2) {
        if (0 == myproc.rank) {
            fprintf(stderr, "This example requires a minimum of 2 processes\n");
        }
        goto done;
    }
    fprintf(stderr, "Client %s:%d job size %u\n", myproc.nspace, myproc.rank, nprocs);

    /* register our default errhandler */
    DEBUG_CONSTRUCT_LOCK(&lock);
    PMIx_Register_event_handler(NULL, 0, NULL, 0, notification_fn, errhandler_reg_callbk,
                                (void *) &lock);
    DEBUG_WAIT_THREAD(&lock);
    rc = lock.status;
    DEBUG_DESTRUCT_LOCK(&lock);
    if (PMIX_SUCCESS != rc) {
        goto done;
    }

    // see if we are the parent job
    if (NULL == getenv("CHILD")) {

        // put some modex-like data
        (void) snprintf(tmp, 1024, "%s-%u-remote", myproc.nspace, myproc.rank);
        value.type = PMIX_UINT64;
        value.data.uint64 = 1234UL + (unsigned long) myproc.rank;
        if (PMIX_SUCCESS != (rc = PMIx_Put(PMIX_GLOBAL, tmp, &value))) {
            fprintf(stderr, "Client ns %s rank %d: PMIx_Put failed: %d\n", myproc.nspace,
                    myproc.rank, rc);
            goto done;
        }

        // commit it
        rc = PMIx_Commit();
        if (PMIX_SUCCESS != rc) {
            fprintf(stderr, "Client ns %s rank %d: PMIx_Commit failed: %s\n",
                    myproc.nspace, myproc.rank, PMIx_Error_string(rc));
            goto done;
        }

        // do our own exchange
        PMIX_PROC_CONSTRUCT(&proc);
        PMIX_LOAD_PROCID(&proc, myproc.nspace, PMIX_RANK_WILDCARD);
        if (PMIX_SUCCESS != (rc = PMIx_Fence(&proc, 1, NULL, 0))) {
            fprintf(stderr, "Client ns %s rank %d: PMIx_Fence failed: %s\n",
                    myproc.nspace, myproc.rank, PMIx_Error_string(rc));
            goto done;
        }

        // parent rank=0 will call spawn
        if (0 == myproc.rank) {
            PMIX_APP_CONSTRUCT(&app);
            app.cmd = strdup(argv[0]);
            app.maxprocs = maxprocs;
            app.argv = (char **) malloc(2 * sizeof(char *));
            app.argv[0] = strdup(basename(argv[0]));
            app.argv[1] = NULL;
            app.env = (char **) malloc(2 * sizeof(char *));
            app.env[0] = strdup("CHILD=3");
            app.env[1] = NULL;

            fprintf(stderr, "Client ns %s rank %d: calling PMIx_Spawn\n", myproc.nspace, myproc.rank);
            PMIX_INFO_LOAD(&info[0], PMIX_MAPBY, "node", PMIX_STRING);
            if (PMIX_SUCCESS != (rc = PMIx_Spawn(info, 1, &app, 1, nsp2))) {
                fprintf(stderr, "Client ns %s rank %d: PMIx_Spawn failed: %d\n", myproc.nspace,
                        myproc.rank, rc);
                goto done;
            }
            PMIX_APP_DESTRUCT(&app);

            // circulate the child nspace with our peers
            value.type = PMIX_STRING;
            value.data.string = nsp2;
            rc = PMIx_Put(PMIX_GLOBAL, "CHILD_NSPACE", &value);
            if (PMIX_SUCCESS != rc) {
                fprintf(stderr, "Put of child job nspace failed\n");
                goto done;
            }

            rc = PMIx_Commit();
            if (PMIX_SUCCESS != rc) {
                fprintf(stderr, "Client ns %s rank %d: PMIx_Commit failed: %s\n",
                        myproc.nspace, myproc.rank, PMIx_Error_string(rc));
                goto done;
            }

            PMIX_PROC_CONSTRUCT(&proc);
            PMIX_LOAD_PROCID(&proc, myproc.nspace, PMIX_RANK_WILDCARD);
            if (PMIX_SUCCESS != (rc = PMIx_Fence(&proc, 1, NULL, 0))) {
                fprintf(stderr, "Client ns %s rank %d: PMIx_Fence failed: %s\n",
                        myproc.nspace, myproc.rank, PMIx_Error_string(rc));
                goto done;
            }

            // now construct the group

            // ask for a new context ID
            PMIX_INFO_LOAD(&info[0], PMIX_GROUP_ASSIGN_CONTEXT_ID, NULL, PMIX_BOOL);

            // add the rest of the parent job
            PMIX_LOAD_PROCID(&parray[0], myproc.nspace, PMIX_RANK_WILDCARD);
            PMIX_LOAD_PROCID(&parray[1], nsp2, PMIX_RANK_WILDCARD);

            // construct the group
            rc = PMIx_Group_construct("ourgroup", parray, 2, info, 1, &results, &nresults);
            if (PMIX_SUCCESS != rc) {
                fprintf(stderr, "Client ns %s rank %d: PMIx_Group_construct failed: %s\n",
                        myproc.nspace, myproc.rank, PMIx_Error_string(rc));
                goto done;
            }

        } else {
            // wait for rank0 to tell us the child nspace
            PMIX_PROC_CONSTRUCT(&proc);
            PMIX_LOAD_PROCID(&proc, myproc.nspace, PMIX_RANK_WILDCARD);
            if (PMIX_SUCCESS != (rc = PMIx_Fence(&proc, 1, NULL, 0))) {
                fprintf(stderr, "Client ns %s rank %d: PMIx_Fence failed: %s\n",
                        myproc.nspace, myproc.rank, PMIx_Error_string(rc));
                goto done;
            }

            // get the nspace
            PMIX_LOAD_PROCID(&proc, myproc.nspace, 0);
            rc = PMIx_Get(&proc, "CHILD_NSPACE", NULL, 0, &val);
            if (PMIX_SUCCESS != rc) {
                fprintf(stderr, "Client ns %s rank %d: PMIx_Get child nspace failed: %s\n",
                        myproc.nspace, myproc.rank, PMIx_Error_string(rc));
                goto done;
            }

            // we will participate in the group assembly with
            // the spawned children
            PMIX_LOAD_PROCID(&parray[0], myproc.nspace, PMIX_RANK_WILDCARD);
            PMIX_LOAD_PROCID(&parray[1], val->data.string, PMIX_RANK_WILDCARD);

            rc = PMIx_Group_construct("ourgroup", parray, 2, NULL, 0, &results, &nresults);
            if (PMIX_SUCCESS != rc) {
                fprintf(stderr, "Client ns %s rank %d: PMIx_Group_construct failed: %s\n",
                        myproc.nspace, myproc.rank, PMIx_Error_string(rc));
                goto done;
            }
            // process results to get final membership

        }
    } else {
        // we are the child job - put some data
        (void) snprintf(tmp, 1024, "%s-%u-remote", myproc.nspace, myproc.rank);
        value.type = PMIX_UINT64;
        value.data.uint64 = 1234UL + (unsigned long) myproc.rank;
        if (PMIX_SUCCESS != (rc = PMIx_Put(PMIX_GLOBAL, tmp, &value))) {
            fprintf(stderr, "Client ns %s rank %d: PMIx_Put failed: %d\n", myproc.nspace,
                    myproc.rank, rc);
            goto done;
        }

        // commit it
        rc = PMIx_Commit();
        if (PMIX_SUCCESS != rc) {
            fprintf(stderr, "Client ns %s rank %d: PMIx_Commit failed: %s\n",
                    myproc.nspace, myproc.rank, PMIx_Error_string(rc));
            goto done;
        }

        // do our own exchange
        PMIX_PROC_CONSTRUCT(&proc);
        PMIX_LOAD_PROCID(&proc, myproc.nspace, PMIX_RANK_WILDCARD);
        if (PMIX_SUCCESS != (rc = PMIx_Fence(&proc, 1, NULL, 0))) {
            fprintf(stderr, "Client ns %s rank %d: PMIx_Fence failed: %s\n",
                    myproc.nspace, myproc.rank, PMIx_Error_string(rc));
            goto done;
        }

        // get the parent jobid
        PMIX_LOAD_PROCID(&proc, myproc.nspace, PMIX_RANK_WILDCARD);
        rc = PMIx_Get(&myproc, PMIX_PARENT_ID, NULL, 0, &val);
        if (PMIX_SUCCESS != rc) {
            fprintf(stderr, "Client ns %s rank %d: PMIx_Get parent ID failed: %s\n",
                    myproc.nspace, myproc.rank, PMIx_Error_string(rc));
            goto done;
        }

        PMIX_LOAD_PROCID(&parray[0], val->data.string, PMIX_RANK_WILDCARD);
        PMIX_LOAD_PROCID(&parray[1], myproc.nspace, PMIX_RANK_WILDCARD);

        // construct the group
        rc = PMIx_Group_construct("ourgroup", parray, 2, NULL, 0, &results, &nresults);
        if (PMIX_SUCCESS != rc) {
            fprintf(stderr, "Client ns %s rank %d: PMIx_Group_construct failed: %s\n",
                    myproc.nspace, myproc.rank, PMIx_Error_string(rc));
            goto done;
        }
    }

    /* we should have a single results object */
    if (NULL != results) {
        cid = 0;
        pptr = NULL;
        for (n=0; n < nresults; n++) {
            if (PMIX_CHECK_KEY(&results[n], PMIX_GROUP_CONTEXT_ID)) {
                rc = PMIx_Value_get_number(&results[n].value, &cid, PMIX_SIZE);
                fprintf(stderr, "%s:%d Group construct complete with status %s KEY %s CID %lu\n",
                        myproc.nspace, myproc.rank, PMIx_Error_string(rc), results[n].key, (unsigned long) cid);
            } else if (PMIX_CHECK_KEY(&results[n], PMIX_GROUP_MEMBERSHIP)) {
                pptr = (pmix_proc_t*)results[n].value.data.darray->array;
                psize = results[n].value.data.darray->size;
                fprintf(stderr, "[%s:%u] NUM MEMBERS: %u MEMBERSHIP:\n", myproc.nspace, myproc.rank, (unsigned)psize);
                for (m=0; m < psize; m++) {
                    fprintf(stderr, "\t%s:%u\n", pptr[m].nspace, pptr[m].rank);
                }
            }
        }
        if (NULL == pptr) {
            fprintf(stderr, "%s:%d Membership not returned\n", myproc.nspace, myproc.rank);
            goto done;
        }
    } else {
        fprintf(stderr, "%s:%d No returned results\n", myproc.nspace, myproc.rank);
        goto done;
    }

    // check for modex data to have been exchanged
    PMIX_INFO_CONSTRUCT(&tinfo);
    PMIX_INFO_LOAD(&tinfo, PMIX_IMMEDIATE, NULL, PMIX_BOOL);
    for (n=0; n < psize; n++) {
        // ignore my own nspace
        if (PMIX_CHECK_NSPACE(pptr[n].nspace, myproc.nspace)) {
            continue;
        }
        if (PMIX_RANK_WILDCARD == parray[n].rank) {
            // get the size of the other nspace
            PMIX_LOAD_PROCID(&proc, pptr[n].nspace, PMIX_RANK_WILDCARD);
            rc = PMIx_Get(&proc, PMIX_JOB_SIZE, &tinfo, 1, &val);
            if (PMIX_SUCCESS != rc) {
                fprintf(stderr, "Client ns %s rank %d: PMIx_Get job size failed for nspace %s: %s\n",
                        myproc.nspace, myproc.rank, pptr[n].nspace, PMIx_Error_string(rc));
                goto done;
            }
            nprocs = val->data.uint32;
            PMIX_VALUE_RELEASE(val);
            // cycle across the procs and get their modex data
            for (m=0; m < nprocs; m++) {
                proc.rank = m;
                (void)snprintf(tmp, 1024, "%s-%u-remote", proc.nspace, proc.rank);
                if (PMIX_SUCCESS != (rc = PMIx_Get(&proc, tmp, &tinfo, 1, &val))) {
                        fprintf(stderr, "Client ns %s rank %d: PMIx_Get %s failed: %s\n",
                                myproc.nspace, (int)n, tmp, PMIx_Error_string(rc));
                } else {
                    fprintf(stderr, "Client ns %s rank %d: Get modex for proc %s:%u returned %lu\n",
                            myproc.nspace, myproc.rank, proc.nspace, proc.rank, (unsigned long)val->data.uint64);
                }
            }
        }
    }
done:
    /* finalize us */
    DEBUG_CONSTRUCT_LOCK(&lock);
    PMIx_Deregister_event_handler(1, op_callbk, &lock);
    DEBUG_WAIT_THREAD(&lock);
    DEBUG_DESTRUCT_LOCK(&lock);

    fprintf(stderr, "Client ns %s rank %d: Finalizing\n", myproc.nspace, myproc.rank);
    if (PMIX_SUCCESS != (rc = PMIx_Finalize(NULL, 0))) {
        fprintf(stderr, "Client ns %s rank %d:PMIx_Finalize failed: %s\n", myproc.nspace,
                myproc.rank, PMIx_Error_string(rc));
    } else {
        fprintf(stderr, "Client ns %s rank %d:PMIx_Finalize successfully completed\n",
                myproc.nspace, myproc.rank);
    }
    fprintf(stderr, "%s:%d COMPLETE\n", myproc.nspace, myproc.rank);
    fflush(stderr);
    return (0);
}
