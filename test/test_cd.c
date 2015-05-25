/*
 * Copyright (c) 2015      Intel, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 */

#include "test_cd.h"
#include <time.h>

typedef struct {
    int in_progress;
    int status;
} cd_cbdata;

static void cd_cb(pmix_status_t status, void *cbdata)
{
    cd_cbdata *cb = (cd_cbdata*)cbdata;

    cb->in_progress = 0;
    cb->status = status;
}

static int test_cd_common(char *my_nspace, int my_rank, int blocking, int disconnect)
{
    int rc;
    pmix_proc_t proc;

    (void)strncpy(proc.nspace, my_nspace, PMIX_MAX_NSLEN);
    proc.rank = PMIX_RANK_WILDCARD;
    if (blocking) {
        if (!disconnect) {
            rc = PMIx_Connect(&proc, 1);
        } else {
            rc = PMIx_Disconnect(&proc, 1);
        }
    } else {
        int count;
        cd_cbdata cbdata;
        cbdata.in_progress = 1;
        if (!disconnect) {
            rc = PMIx_Connect_nb(&proc, 1, cd_cb, (void*)&cbdata);
        } else {
            rc = PMIx_Disconnect_nb(&proc, 1, cd_cb, (void*)&cbdata);
        }
        if (PMIX_SUCCESS == rc) {
            count = 0;
            while( cbdata.in_progress ){
                struct timespec ts;
                ts.tv_sec = 0;
                ts.tv_nsec = 100;
                nanosleep(&ts,NULL);
                count++;
            }
        }
        rc = cbdata.status;
    }
    /* the host server callback currently returns PMIX_EXISTS status for checking purposes */
    if (PMIX_EXISTS == rc) {
        rc = PMIX_SUCCESS;
    }
    return rc;
}

int test_connect_disconnect(char *my_nspace, int my_rank)
{
    int rc;
    rc = test_cd_common(my_nspace, my_rank, 1, 0);
    if (PMIX_SUCCESS != rc) {
        TEST_ERROR(("%s:%d: Connect blocking test failed.", my_nspace, my_rank));
        return PMIX_ERROR;
    }
    TEST_VERBOSE(("%s:%d: Connect blocking test succeded.", my_nspace, my_rank));
    rc = test_cd_common(my_nspace, my_rank, 1, 1);
    if (PMIX_SUCCESS != rc) {
        TEST_ERROR(("%s:%d: Disconnect blocking test failed.", my_nspace, my_rank));
        return PMIX_ERROR;
    }
    TEST_VERBOSE(("%s:%d: Disconnect blocking test succeded.", my_nspace, my_rank));
    rc = test_cd_common(my_nspace, my_rank, 0, 0);
    if (PMIX_SUCCESS != rc) {
        TEST_ERROR(("%s:%d: Connect non-blocking test failed.", my_nspace, my_rank));
        return PMIX_ERROR;
    }
    TEST_VERBOSE(("%s:%d: Connect non-blocking test succeded.", my_nspace, my_rank));
    rc = test_cd_common(my_nspace, my_rank, 0, 1);
    if (PMIX_SUCCESS != rc) {
        TEST_ERROR(("%s:%d: Disconnect non-blocking test failed.", my_nspace, my_rank));
        return PMIX_ERROR;
    }
    TEST_VERBOSE(("%s:%d: Disconnect non-blocking test succeded.", my_nspace, my_rank));
    return PMIX_SUCCESS;
}

