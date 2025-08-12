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
 * Copyright (c) 2024      Triad National Security, LLC. All rights reserved.
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
#include <unistd.h>

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
    int rc, ret;
    pmix_value_t *val = NULL, value;
    pmix_proc_t proc, *procs, *parray;
    uint32_t nprocs;
    mylock_t lock;
    pmix_info_t *results, info[3];
    size_t nresults, cid = 0, n, m, psize;
    char *tmp;
    pmix_query_t query;
    int i;
    bool testquery = false;
    EXAMPLES_HIDE_UNUSED_PARAMS(argc, argv);

    for (i=1; i < argc; i++) {
        if (0 == strcmp(argv[i], "--test-query")) {
            testquery = true;
        } else {
            fprintf(stderr, "Usage: %s [--test-query]\n", basename(argv[0]));
            exit(1);
        }
    }

    /* init us */
    if (PMIX_SUCCESS != (rc = PMIx_Init(&myproc, NULL, 0))) {
        fprintf(stderr, "Client ns %s rank %d: PMIx_Init failed: %s\n", myproc.nspace, myproc.rank,
                PMIx_Error_string(rc));
        exit(1);
    }
    fprintf(stderr, "Client ns %s rank %d: Running\n", myproc.nspace, myproc.rank);

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
    if (nprocs < 4) {
        if (0 == myproc.rank) {
            fprintf(stderr, "This example requires a minimum of 4 processes\n");
        }
        exit(1);
    }
    fprintf(stderr, "Client %s:%d job size %d\n", myproc.nspace, myproc.rank, nprocs);

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

    if (0 > asprintf(&tmp, "%s-%d-local", myproc.nspace, myproc.rank)) {
        exit(1);
    }
    value.type = PMIX_UINT64;
    value.data.uint64 = 1234;
    if (PMIX_SUCCESS != (rc = PMIx_Put(PMIX_LOCAL, tmp, &value))) {
        fprintf(stderr, "Client ns %s rank %d: PMIx_Put internal failed: %s\n", myproc.nspace,
                myproc.rank, PMIx_Error_string(rc));
        goto done;
    }
    free(tmp);

    if (0 > asprintf(&tmp, "%s-%d-remote", myproc.nspace, myproc.rank)) {
        exit(1);
    }
    value.type = PMIX_STRING;
    value.data.string = "1234";
    if (PMIX_SUCCESS != (rc = PMIx_Put(PMIX_REMOTE, tmp, &value))) {
        fprintf(stderr, "Client ns %s rank %d: PMIx_Put internal failed: %s\n", myproc.nspace,
                myproc.rank, PMIx_Error_string(rc));
        goto done;
    }
    free(tmp);

    /* push the data to our PMIx server */
    if (PMIX_SUCCESS != (rc = PMIx_Commit())) {
        fprintf(stderr, "Client ns %s rank %d: PMIx_Commit failed: %s\n", myproc.nspace,
                myproc.rank, PMIx_Error_string(rc));
        goto done;
    }

    /* call fence to sync */
    PMIX_PROC_CONSTRUCT(&proc);
    PMIX_LOAD_PROCID(&proc, myproc.nspace, PMIX_RANK_WILDCARD);
    if (PMIX_SUCCESS != (rc = PMIx_Fence(&proc, 1, NULL, 0))) {
        fprintf(stderr, "Client ns %s rank %d: PMIx_Fence failed: %d\n", myproc.nspace, myproc.rank,
                rc);
        goto done;
    }

    /* rank=0,2,3 construct a new group */
    if (0 == myproc.rank || 2 == myproc.rank || 3 == myproc.rank) {
        fprintf(stderr, "%d executing Group_construct\n", myproc.rank);
        nprocs = 3;
        PMIX_PROC_CREATE(procs, nprocs);
        PMIX_PROC_LOAD(&procs[0], myproc.nspace, 0);
        PMIX_PROC_LOAD(&procs[1], myproc.nspace, 2);
        PMIX_PROC_LOAD(&procs[2], myproc.nspace, 3);
        PMIX_INFO_LOAD(&info[0], PMIX_GROUP_ASSIGN_CONTEXT_ID, NULL, PMIX_BOOL);

        rc = PMIx_Group_construct("ourgroup", procs, nprocs, info, 1, &results, &nresults);
        if (PMIX_SUCCESS != rc) {
            fprintf(stderr, "Client ns %s rank %d: PMIx_Group_construct failed: %s\n",
                    myproc.nspace, myproc.rank, PMIx_Error_string(rc));
            goto done;
        }
        /* we should have a single results object */
        if (NULL != results) {
            cid = 0;
            for (n=0; n < nresults; n++) {
                if (PMIX_CHECK_KEY(&results[n], PMIX_GROUP_CONTEXT_ID)) {
                    rc = PMIx_Value_get_number(&results[n].value, &cid, PMIX_SIZE);
                    fprintf(stderr, "%d Group construct complete with status %s KEY %s CID %lu\n",
                            myproc.rank, PMIx_Error_string(rc), results[n].key, (unsigned long) cid);
                } else if (PMIX_CHECK_KEY(&results[n], PMIX_GROUP_MEMBERSHIP)) {
                    parray = (pmix_proc_t*)results[n].value.data.darray->array;
                    psize = results[n].value.data.darray->size;
                    if (0 == myproc.rank) {
                        fprintf(stderr, "NUM MEMBERS: %u MEMBERSHIP:\n", (unsigned)psize);
                        for (m=0; m < psize; m++) {
                            fprintf(stderr, "\t%s:%u\n", parray[m].nspace, parray[m].rank);
                        }
                    }
                }
            }
            PMIX_INFO_FREE(results, nresults);
        } else {
            fprintf(stderr, "%d Group construct complete, but results returned\n", myproc.rank);
        }
        PMIX_PROC_FREE(procs, nprocs);
        fprintf(stderr, "%d executing Group_destruct\n", myproc.rank);
        rc = PMIx_Group_destruct("ourgroup", NULL, 0);
        if (PMIX_SUCCESS != rc) {
            fprintf(stderr, "Client ns %s rank %d: PMIx_Group_destruct failed: %s\n", myproc.nspace,
                    myproc.rank, PMIx_Error_string(rc));
            goto done;
        }
    }

    if (testquery && 0 == myproc.rank) {
        /* first ask for a list of active namespaces */
        PMIX_QUERY_CONSTRUCT(&query);
        PMIX_ARGV_APPEND(rc, query.keys, PMIX_QUERY_NAMESPACES);

        rc = PMIx_Query_info(&query, 1, &results, &nresults);
        if (PMIX_SUCCESS != rc ) {
            fprintf(stderr, "Error: PMIx_Query_info for namespaces failed: %d (%s)\n", rc, PMIx_Error_string(rc));
            goto done;
        }
        fprintf(stderr, "\n\n--> Query returned (ninfo %d)\n", (int)nresults);
        for (n = 0; n < nresults; ++n) {
            tmp = PMIx_Info_string(&results[n]);
            fprintf(stderr, "%s\n", tmp);
            free(tmp);
        }
        fprintf(stderr, "<--- END\n\n\n");
        PMIX_QUERY_DESTRUCT(&query);
        PMIX_INFO_FREE(results, nresults);
        /* we can then parse the results to find a namespace of interest, and
         * query about that namespace in particular. Or we can simply query
         * for info on ALL namespaces */

        PMIX_QUERY_CONSTRUCT(&query);
        PMIX_ARGV_APPEND(rc, query.keys, PMIX_QUERY_NAMESPACE_INFO);

        rc = PMIx_Query_info(&query, 1, &results, &nresults);
        if (PMIX_SUCCESS != rc ) {
            fprintf(stderr, "Error: PMIx_Query_info for namespace info failed: %d (%s)\n", rc, PMIx_Error_string(rc));
            goto done;
        }
        fprintf(stderr, "--> Query returned (ninfo %d)\n", (int)nresults);
        for (n = 0; n < nresults; ++n) {
            tmp = PMIx_Info_string(&results[n]);
            fprintf(stderr, "%s\n", tmp);
            free(tmp);
        }
        fprintf(stderr, "<--- END\n\n\n");
        PMIX_QUERY_DESTRUCT(&query);
        PMIX_INFO_FREE(results, nresults);
    }

done:
    /* finalize us */
    DEBUG_CONSTRUCT_LOCK(&lock);
    PMIx_Deregister_event_handler(1, op_callbk, &lock);
    DEBUG_WAIT_THREAD(&lock);
    DEBUG_DESTRUCT_LOCK(&lock);

    fprintf(stderr, "Client ns %s rank %d: Finalizing\n", myproc.nspace, myproc.rank);
    if (PMIX_SUCCESS != (ret = PMIx_Finalize(NULL, 0))) {
        fprintf(stderr, "Client ns %s rank %d:PMIx_Finalize failed: %s\n", myproc.nspace,
                myproc.rank, PMIx_Error_string(ret));
        rc = ret;
    } else {
        fprintf(stderr, "Client ns %s rank %d:PMIx_Finalize successfully completed\n",
                myproc.nspace, myproc.rank);
    }
    fprintf(stderr, "%s:%d COMPLETE\n", myproc.nspace, myproc.rank);
    fflush(stderr);
    return (rc);
}
