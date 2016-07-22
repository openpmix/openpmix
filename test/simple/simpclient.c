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
 * Copyright (c) 2013-2016 Intel, Inc.  All rights reserved.
 * Copyright (c) 2015      Mellanox Technologies, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 */

#include <src/include/pmix_config.h>
#include <pmix.h>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>

#include "src/class/pmix_object.h"
#include "src/util/output.h"
#include "src/util/printf.h"

#define MAXCNT 2

static volatile bool completed = false;
static pmix_proc_t myproc;

static void notification_fn(size_t evhdlr_registration_id,
                            pmix_status_t status,
                            const pmix_proc_t *source,
                            pmix_info_t info[], size_t ninfo,
                            pmix_info_t results[], size_t nresults,
                            pmix_event_notification_cbfunc_fn_t cbfunc,
                            void *cbdata)
{
    pmix_output(0, "Client %s:%d NOTIFIED with status %s", myproc.nspace, myproc.rank, PMIx_Error_string(status));
    if (NULL != cbfunc) {
        cbfunc(PMIX_SUCCESS, NULL, 0, NULL, NULL, cbdata);
    }
    completed = true;
}

static void errhandler_reg_callbk(pmix_status_t status,
                                  size_t errhandler_ref,
                                  void *cbdata)
{
    volatile bool *active = (volatile bool*)cbdata;

    pmix_output(0, "Client: ERRHANDLER REGISTRATION CALLBACK CALLED WITH STATUS %d, ref=%lu",
                status, (unsigned long)errhandler_ref);
    *active = false;
}

int main(int argc, char **argv)
{
    int rc;
    pmix_value_t value;
    pmix_value_t *val = &value;
    char *tmp;
    pmix_proc_t proc;
    uint32_t nprocs, n;
    int cnt, j;
    bool doabort = false;
    volatile bool active;

    if (1 < argc) {
        if (0 == strcmp("-abort", argv[1])) {
            doabort = true;
        }
    }

    /* init us */
    if (PMIX_SUCCESS != (rc = PMIx_Init(&myproc, NULL, 0))) {
        pmix_output(0, "Client ns %s rank %d: PMIx_Init failed: %d", myproc.nspace, myproc.rank, rc);
        exit(rc);
    }
    pmix_output(0, "Client ns %s rank %d: Running", myproc.nspace, myproc.rank);

    /* register our errhandler */
    active = true;
    PMIx_Register_event_handler(NULL, 0, NULL, 0,
                                notification_fn, errhandler_reg_callbk, (void*)&active);
    while (active) {
        usleep(10);
    }

    /* get our universe size */
    if (PMIX_SUCCESS != (rc = PMIx_Get(&myproc, PMIX_UNIV_SIZE, NULL, 0, &val))) {
        pmix_output(0, "Client ns %s rank %d: PMIx_Get universe size failed: %d", myproc.nspace, myproc.rank, rc);
        goto done;
    }
    nprocs = val->data.uint32;
    PMIX_VALUE_RELEASE(val);
    pmix_output(0, "Client %s:%d universe size %d", myproc.nspace, myproc.rank, nprocs);

    /* put a few values */
    (void)asprintf(&tmp, "%s-%d-internal", myproc.nspace, myproc.rank);
    value.type = PMIX_UINT32;
    value.data.uint32 = 1234;
    if (PMIX_SUCCESS != (rc = PMIx_Store_internal(&myproc, tmp, &value))) {
        pmix_output(0, "Client ns %s rank %d: PMIx_Store_internal failed: %d", myproc.nspace, myproc.rank, rc);
        goto done;
    }

    for (cnt=0; cnt < MAXCNT; cnt++) {
        (void)asprintf(&tmp, "%s-%d-local-%d", myproc.nspace, myproc.rank, cnt);
        value.type = PMIX_UINT64;
        value.data.uint64 = 1234;
        if (PMIX_SUCCESS != (rc = PMIx_Put(PMIX_LOCAL, tmp, &value))) {
            pmix_output(0, "Client ns %s rank %d: PMIx_Put internal failed: %d", myproc.nspace, myproc.rank, rc);
            goto done;
        }

        (void)asprintf(&tmp, "%s-%d-remote-%d", myproc.nspace, myproc.rank, cnt);
        value.type = PMIX_STRING;
        value.data.string = "1234";
        if (PMIX_SUCCESS != (rc = PMIx_Put(PMIX_REMOTE, tmp, &value))) {
            pmix_output(0, "Client ns %s rank %d: PMIx_Put internal failed: %d", myproc.nspace, myproc.rank, rc);
            goto done;
        }

        if (PMIX_SUCCESS != (rc = PMIx_Commit())) {
            pmix_output(0, "Client ns %s rank %d cnt %d: PMIx_Commit failed: %d", myproc.nspace, myproc.rank, cnt, rc);
            goto done;
        }

        /* call fence to ensure the data is received */
        PMIX_PROC_CONSTRUCT(&proc);
        (void)strncpy(proc.nspace, myproc.nspace, PMIX_MAX_NSLEN);
        proc.rank = PMIX_RANK_WILDCARD;
        if (PMIX_SUCCESS != (rc = PMIx_Fence(&proc, 1, NULL, 0))) {
            pmix_output(0, "Client ns %s rank %d cnt %d: PMIx_Fence failed: %d", myproc.nspace, myproc.rank, cnt, rc);
            goto done;
        }

        /* check the returned data */
        (void)strncpy(proc.nspace, myproc.nspace, PMIX_MAX_NSLEN);
        for (j=0; j <= cnt; j++) {
            for (n=0; n < nprocs; n++) {
                proc.rank = n;
                (void)asprintf(&tmp, "%s-%d-local-%d", myproc.nspace, n, j);
                if (PMIX_SUCCESS != (rc = PMIx_Get(&proc, tmp, NULL, 0, &val))) {
                    pmix_output(0, "Client ns %s rank %d cnt %d: PMIx_Get %s failed: %s[%d]", myproc.nspace, myproc.rank, j, tmp, PMIx_Error_string(rc), rc);
                    exit(1);
                }
                if (PMIX_UINT64 != val->type) {
                    pmix_output(0, "Client ns %s rank %d cnt %d: PMIx_Get %s returned wrong type: %d", myproc.nspace, myproc.rank, j, tmp, val->type);
                    PMIX_VALUE_RELEASE(val);
                    free(tmp);
                    continue;
                }
                if (1234 != val->data.uint64) {
                    pmix_output(0, "Client ns %s rank %d cnt %d: PMIx_Get %s returned wrong value: %d", myproc.nspace, myproc.rank, j, tmp, (int)val->data.uint64);
                    PMIX_VALUE_RELEASE(val);
                    free(tmp);
                    continue;
                }
                pmix_output(0, "Client ns %s rank %d cnt %d: PMIx_Get %s returned correct", myproc.nspace, myproc.rank, j, tmp);
                PMIX_VALUE_RELEASE(val);
                free(tmp);

                (void)asprintf(&tmp, "%s-%d-remote-%d", proc.nspace, n, j);
                if (PMIX_SUCCESS != (rc = PMIx_Get(&proc, tmp, NULL, 0, &val))) {
                    pmix_output(0, "Client ns %s rank %d cnt %d: PMIx_Get %s failed: %d", myproc.nspace, myproc.rank, j, tmp, rc);
                    continue;
                }
                if (PMIX_STRING != val->type) {
                    pmix_output(0, "Client ns %s rank %d cnt %d: PMIx_Get %s returned wrong type: %d", myproc.nspace, myproc.rank, j, tmp, val->type);
                    PMIX_VALUE_RELEASE(val);
                    free(tmp);
                    continue;
                }
                if (0 != strcmp(val->data.string, "1234")) {
                    pmix_output(0, "Client ns %s rank %d cnt %d: PMIx_Get %s returned wrong value: %s", myproc.nspace, myproc.rank, j, tmp, val->data.string);
                    PMIX_VALUE_RELEASE(val);
                    free(tmp);
                    continue;
                }
                pmix_output(0, "Client ns %s rank %d cnt %d: PMIx_Get %s returned correct", myproc.nspace, myproc.rank, j, tmp);
                PMIX_VALUE_RELEASE(val);
                free(tmp);
            }
        }
    }

    /* if requested and our rank is 0, call abort */
    if (doabort) {
        if (0 == myproc.rank) {
            PMIx_Abort(PMIX_ERR_PROC_REQUESTED_ABORT, "CALLING ABORT", NULL, 0);
        } else {
            while(!completed) {
                usleep(10);
            }
        }
    }

 done:
    /* finalize us */
    pmix_output(0, "Client ns %s rank %d: Finalizing", myproc.nspace, myproc.rank);
    if (PMIX_SUCCESS != (rc = PMIx_Finalize(NULL, 0))) {
        fprintf(stderr, "Client ns %s rank %d:PMIx_Finalize failed: %d\n", myproc.nspace, myproc.rank, rc);
    } else {
        fprintf(stderr, "Client ns %s rank %d:PMIx_Finalize successfully completed\n", myproc.nspace, myproc.rank);
    }
    fflush(stderr);
    return(rc);
}
