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
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include "examples.h"
#include <pmix.h>

static pmix_proc_t myproc;
static int nupdates = 0;

/* this is the event notification function we pass down below
 * when registering for general events - i.e.,, the default
 * handler. We don't technically need to register one, but it
 * is usually good practice to catch any events that occur */
static void notification_fn(size_t evhdlr_registration_id, pmix_status_t status,
                            const pmix_proc_t *source, pmix_info_t info[], size_t ninfo,
                            pmix_info_t results[], size_t nresults,
                            pmix_event_notification_cbfunc_fn_t cbfunc, void *cbdata)
{
    EXAMPLES_HIDE_UNUSED_PARAMS(evhdlr_registration_id, status, source, info, ninfo, results, nresults);

    if (NULL != cbfunc) {
        cbfunc(PMIX_EVENT_ACTION_COMPLETE, NULL, 0, NULL, NULL, cbdata);
    }
}

static void update(size_t evhdlr_registration_id, pmix_status_t status,
                   const pmix_proc_t *source, pmix_info_t info[], size_t ninfo,
                   pmix_info_t results[], size_t nresults,
                   pmix_event_notification_cbfunc_fn_t cbfunc, void *cbdata)
{
    size_t n;
    EXAMPLES_HIDE_UNUSED_PARAMS(evhdlr_registration_id, status, source, results, nresults);

    fprintf(stderr, "UPDATE:\n");
    for (n=0; n < ninfo; n++) {
        fprintf(stderr, "%s", PMIx_Info_string(&info[n]));
    }
    fprintf(stderr, "\n\n");

    ++nupdates;
    if (NULL != cbfunc) {
        cbfunc(PMIX_EVENT_ACTION_COMPLETE, NULL, 0, NULL, NULL, cbdata);
    }
}

/* event handler registration is done asynchronously because it
 * may involve the PMIx server registering with the host RM for
 * external events. So we provide a callback function that returns
 * the status of the request (success or an error), plus a numerical index
 * to the registered event. The index is used later on to deregister
 * an event handler - if we don't explicitly deregister it, then the
 * PMIx server will do so when it see us exit */
static void evhandler_reg_callbk(pmix_status_t status, size_t evhandler_ref, void *cbdata)
{
    mylock_t *lock = (mylock_t *) cbdata;

    if (PMIX_SUCCESS != status) {
        fprintf(stderr, "Client %s:%d EVENT HANDLER REGISTRATION FAILED WITH STATUS %d, ref=%lu\n",
                myproc.nspace, myproc.rank, status, (unsigned long) evhandler_ref);
    }
    lock->status = status;
    lock->evhandler_ref = evhandler_ref;
    DEBUG_WAKEUP_THREAD(lock);
}

int main(int argc, char **argv)
{
    pmix_status_t rc, code;
    pmix_value_t *val = NULL;
    pmix_proc_t proc;
    uint32_t nprocs, n;
    pmix_info_t monitor, *results=NULL, directives[2];
    size_t nresults;
    bool flag;
    mylock_t mylock;
    EXAMPLES_HIDE_UNUSED_PARAMS(argc, argv);

    /* init us - note that the call to "init" includes the return of
     * any job-related info provided by the RM. */
    if (PMIX_SUCCESS != (rc = PMIx_Init(&myproc, NULL, 0))) {
        fprintf(stderr, "Client ns %s rank %d: PMIx_Init failed: %d\n", myproc.nspace, myproc.rank,
                rc);
        exit(0);
    }
    fprintf(stderr, "Client ns %s rank %d: Running\n", myproc.nspace, myproc.rank);

    /* register our default event handler - again, this isn't strictly
     * required, but is generally good practice */
    DEBUG_CONSTRUCT_LOCK(&mylock);
    PMIx_Register_event_handler(NULL, 0, NULL, 0, notification_fn, evhandler_reg_callbk,
                                (void *) &mylock);
    /* wait for registration to complete */
    DEBUG_WAIT_THREAD(&mylock);
    rc = mylock.status;
    DEBUG_DESTRUCT_LOCK(&mylock);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "[%s:%d] Default handler registration failed\n", myproc.nspace,
                myproc.rank);
        goto done;
    }

    /* register the monitor update event handler */
    DEBUG_CONSTRUCT_LOCK(&mylock);
    code = PMIX_MONITOR_RESUSAGE_UPDATE;
    PMIx_Register_event_handler(&code, 1, NULL, 0, update, evhandler_reg_callbk,
                                (void *) &mylock);
    /* wait for registration to complete */
    DEBUG_WAIT_THREAD(&mylock);
    rc = mylock.status;
    DEBUG_DESTRUCT_LOCK(&mylock);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "[%s:%d] Update handler registration failed\n", myproc.nspace,
                myproc.rank);
        goto done;
    }

    /* job-related info is found in our nspace, assigned to the
     * wildcard rank as it doesn't relate to a specific rank. Setup
     * a name to retrieve such values */
    PMIX_PROC_CONSTRUCT(&proc);
    PMIX_LOAD_PROCID(&proc, myproc.nspace, PMIX_RANK_WILDCARD);

    /* get our universe size */
    if (PMIX_SUCCESS != (rc = PMIx_Get(&proc, PMIX_JOB_SIZE, NULL, 0, &val))) {
        fprintf(stderr, "Client ns %s rank %d: PMIx_Get job size failed: %d\n", myproc.nspace,
                myproc.rank, rc);
        goto done;
    }
    nprocs = val->data.uint32;
    PMIX_VALUE_RELEASE(val);
    fprintf(stderr, "Client %s:%d job size %d\n", myproc.nspace, myproc.rank, nprocs);

    if (0 == myproc.rank) {
        /* ask to monitor process resource usage */
        PMIX_INFO_LOAD(&monitor, PMIX_MONITOR_PROC_RESOURCE_USAGE, NULL, PMIX_DATA_ARRAY);
        PMIX_INFO_LOAD(&directives[0], PMIX_MONITOR_ID, "mymonitor", PMIX_STRING);
        n = 3;
        PMIX_INFO_LOAD(&directives[1], PMIX_MONITOR_RESOURCE_RATE, &n, PMIX_UINT32);
        rc = PMIx_Process_monitor(&monitor, PMIX_MONITOR_RESUSAGE_UPDATE,
                                  directives, 2, &results, &nresults);
        if (PMIX_SUCCESS != rc) {
            fprintf(stderr, "Client ns %s rank %d: PMIx_Process_monitor failed: %s\n", myproc.nspace,
                    myproc.rank, PMIx_Error_string(rc));
            goto done;
        }

        if (0 == nresults) {
            fprintf(stderr, "No procs found\n");
        } else {
            for (n=0; n < nresults; n++) {
                fprintf(stderr, "%s", PMIx_Info_string(&results[n]));
            }
            fprintf(stderr, "\n\n");
        }

        /* ask to monitor node resource usage */
        PMIX_INFO_LOAD(&monitor, PMIX_MONITOR_NODE_RESOURCE_USAGE, NULL, PMIX_DATA_ARRAY);
        PMIX_INFO_LOAD(&directives[0], PMIX_MONITOR_ID, "nodemon", PMIX_STRING);
        rc = PMIx_Process_monitor(&monitor, PMIX_MONITOR_RESUSAGE_UPDATE,
                                  directives, 2, &results, &nresults);
        if (PMIX_SUCCESS != rc) {
            fprintf(stderr, "Client ns %s rank %d: PMIx_Process_monitor failed: %s\n", myproc.nspace,
                    myproc.rank, PMIx_Error_string(rc));
            goto done;
        }

        if (0 == nresults) {
            fprintf(stderr, "No nodes found\n");
        } else {
            for (n=0; n < nresults; n++) {
                fprintf(stderr, "%s", PMIx_Info_string(&results[n]));
            }
            fprintf(stderr, "\n\n");
        }

        /* ask to monitor disk resource usage */
        PMIX_INFO_LOAD(&monitor, PMIX_MONITOR_DISK_RESOURCE_USAGE, NULL, PMIX_DATA_ARRAY);
        PMIX_INFO_LOAD(&directives[0], PMIX_MONITOR_ID, "dkmon", PMIX_STRING);
        rc = PMIx_Process_monitor(&monitor, PMIX_MONITOR_RESUSAGE_UPDATE,
                                  directives, 2, &results, &nresults);
        if (PMIX_SUCCESS != rc) {
            fprintf(stderr, "Client ns %s rank %d: PMIx_Process_monitor failed: %s\n", myproc.nspace,
                    myproc.rank, PMIx_Error_string(rc));
            goto done;
        }

        if (0 == nresults) {
            fprintf(stderr, "No disks found\n");
        } else {
            for (n=0; n < nresults; n++) {
                fprintf(stderr, "%s", PMIx_Info_string(&results[n]));
            }
            fprintf(stderr, "\n\n");
        }

        /* ask to monitor network resource usage */
        PMIX_INFO_LOAD(&monitor, PMIX_MONITOR_NET_RESOURCE_USAGE, NULL, PMIX_DATA_ARRAY);
        PMIX_INFO_LOAD(&directives[0], PMIX_MONITOR_ID, "netmon", PMIX_STRING);
        rc = PMIx_Process_monitor(&monitor, PMIX_MONITOR_RESUSAGE_UPDATE,
                                  directives, 2, &results, &nresults);
        if (PMIX_SUCCESS != rc) {
            fprintf(stderr, "Client ns %s rank %d: PMIx_Process_monitor failed: %s\n", myproc.nspace,
                    myproc.rank, PMIx_Error_string(rc));
            goto done;
        }

        if (0 == nresults) {
            fprintf(stderr, "No networks found\n");
        } else {
            for (n=0; n < nresults; n++) {
                fprintf(stderr, "%s", PMIx_Info_string(&results[n]));
            }
            fprintf(stderr, "\n\n");
        }

        n = 0;
        while (5 > nupdates) {
            ++n;
            if (n > 1000000000) {
                n = 0;
            }
        }
        // cancel the monitors
        PMIX_INFO_LOAD(&monitor, PMIX_MONITOR_CANCEL, "mymonitor", PMIX_STRING);
        rc = PMIx_Process_monitor(&monitor, PMIX_MONITOR_RESUSAGE_UPDATE,
                                  NULL, 0, &results, &nresults);
        if (PMIX_SUCCESS != rc) {
            fprintf(stderr, "Client ns %s rank %d: cancel monitor failed: %s\n", myproc.nspace,
                    myproc.rank, PMIx_Error_string(rc));
            goto done;
        }
        PMIX_INFO_LOAD(&monitor, PMIX_MONITOR_CANCEL, "nodemon", PMIX_STRING);
        rc = PMIx_Process_monitor(&monitor, PMIX_MONITOR_RESUSAGE_UPDATE,
                                  NULL, 0, &results, &nresults);
        if (PMIX_SUCCESS != rc) {
            fprintf(stderr, "Client ns %s rank %d: cancel monitor failed: %s\n", myproc.nspace,
                    myproc.rank, PMIx_Error_string(rc));
            goto done;
        }
        PMIX_INFO_LOAD(&monitor, PMIX_MONITOR_CANCEL, "dkmon", PMIX_STRING);
        rc = PMIx_Process_monitor(&monitor, PMIX_MONITOR_RESUSAGE_UPDATE,
                                  NULL, 0, &results, &nresults);
        if (PMIX_SUCCESS != rc) {
            fprintf(stderr, "Client ns %s rank %d: cancel monitor failed: %s\n", myproc.nspace,
                    myproc.rank, PMIx_Error_string(rc));
            goto done;
        }
        PMIX_INFO_LOAD(&monitor, PMIX_MONITOR_CANCEL, "netmon", PMIX_STRING);
        rc = PMIx_Process_monitor(&monitor, PMIX_MONITOR_RESUSAGE_UPDATE,
                                  NULL, 0, &results, &nresults);
        if (PMIX_SUCCESS != rc) {
            fprintf(stderr, "Client ns %s rank %d: cancel monitor failed: %s\n", myproc.nspace,
                    myproc.rank, PMIx_Error_string(rc));
            goto done;
        }

    }

    /* call fence to synchronize with our peers - no need to
     * collect any info as we didn't "put" anything */
    flag = false;
    PMIX_INFO_LOAD(&monitor, PMIX_COLLECT_DATA, &flag, PMIX_BOOL);
    if (PMIX_SUCCESS != (rc = PMIx_Fence(&proc, 1, &monitor, 1))) {
        fprintf(stderr, "Client ns %s rank %d: PMIx_Fence failed: %d\n", myproc.nspace, myproc.rank,
                rc);
        goto done;
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
