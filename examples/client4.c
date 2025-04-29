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

int main(int argc, char **argv)
{
    pmix_proc_t myproc;
    pmix_status_t rc;
    pmix_value_t *val = NULL;
    pmix_proc_t proc;

    EXAMPLES_HIDE_UNUSED_PARAMS(argc, argv);

    if (PMIX_SUCCESS != (rc = PMIx_Init(&myproc, NULL, 0))) {
        if (PMIX_ERR_UNREACH == rc) {
            fprintf(stderr, "%s: Cannot operate as singleton\n",
                    PMIx_Proc_string(&myproc));
        } else {
            fprintf(stderr, "%s: PMIx_Init failed: %s\n",
                    PMIx_Proc_string(&myproc),
                    PMIx_Error_string(rc));
        }
        exit(1);
    }
    fprintf(stderr, "%s: Running\n", PMIx_Proc_string(&myproc));

    // get our rank
    PMIX_LOAD_PROCID(&proc, myproc.nspace, PMIX_RANK_INVALID);
    rc = PMIx_Get(&proc, PMIX_RANK, NULL, 0, &val);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "%s: Get rank failed - %s\n", PMIx_Proc_string(&myproc), PMIx_Error_string(rc));
        fflush(stderr);
        goto done;
    }
    if (val->data.rank != myproc.rank) {
        fprintf(stderr, "%s: Get rank returned wrong rank - %u instead of %u\n",
                PMIx_Proc_string(&myproc), val->data.rank, myproc.rank);
        fflush(stderr);
        goto done;
    }
    fprintf(stderr, "%s: Rank return correct\n", PMIx_Proc_string(&myproc));
    PMIX_VALUE_RELEASE(val);

    // get our node ID
    rc = PMIx_Get(&myproc, PMIX_NODEID, NULL, 0, &val);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "%s: Get my nodeID failed - %s\n", PMIx_Proc_string(&myproc), PMIx_Error_string(rc));
        fflush(stderr);
        goto done;
    }
    // rank=0 is on node 0, rank=1 on node 1
    if (val->data.uint32 != myproc.rank) {
        fprintf(stderr, "%s: Get my nodeID returned wrong value - %u instead of %u\n",
                PMIx_Proc_string(&myproc), val->data.uint32, myproc.rank);
        fflush(stderr);
        goto done;
    }
    fprintf(stderr, "%s: NodeID return correct\n", PMIx_Proc_string(&myproc));
    PMIX_VALUE_RELEASE(val);

    // get our node ID with rank=WILDCARD - should fail!
    PMIX_LOAD_PROCID(&proc, myproc.nspace, PMIX_RANK_WILDCARD);
    rc = PMIx_Get(&proc, PMIX_NODEID, NULL, 0, &val);
    if (PMIX_SUCCESS == rc) {
        fprintf(stderr, "%s: Get my nodeID with WILDCARD rank incorrectly succeeded\n",
                PMIx_Proc_string(&myproc));
        fflush(stderr);
        goto done;
    }
    fprintf(stderr, "%s: NodeID with WILDCARD rank correctly failed - %s\n",
            PMIx_Proc_string(&myproc), PMIx_Error_string(rc));
    PMIX_VALUE_RELEASE(val);

    // get our peer's nodeID
    if (0 == myproc.rank) {
        proc.rank = 1;
    } else {
        proc.rank = 0;
    }
    rc = PMIx_Get(&proc, PMIX_NODEID, NULL, 0, &val);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "%s: Get peer's nodeID failed - %s\n", PMIx_Proc_string(&myproc), PMIx_Error_string(rc));
        fflush(stderr);
        goto done;
    }
    // rank=0 is on node 0, rank=1 on node 1
    if (val->data.uint32 != proc.rank) {
        fprintf(stderr, "%s: Get peer's nodeID returned wrong value - %u instead of %u\n",
                PMIx_Proc_string(&myproc), val->data.uint32, proc.rank);
        fflush(stderr);
        goto done;
    }
    fprintf(stderr, "%s: Peer's NodeID return correct\n", PMIx_Proc_string(&myproc));
    PMIX_VALUE_RELEASE(val);

    // get our appnum
    rc = PMIx_Get(&myproc, PMIX_APPNUM, NULL, 0, &val);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "%s: Get my appnum failed - %s\n", PMIx_Proc_string(&myproc), PMIx_Error_string(rc));
        fflush(stderr);
        goto done;
    }
    // rank=0 is on node 0, rank=1 on node 1
    if (val->data.uint32 != myproc.rank) {
        fprintf(stderr, "%s: Get my appnum returned wrong value - %u instead of %u\n",
                PMIx_Proc_string(&myproc), val->data.uint32, myproc.rank);
        fflush(stderr);
        goto done;
    }
    fprintf(stderr, "%s: Appnum return correct\n", PMIx_Proc_string(&myproc));
    PMIX_VALUE_RELEASE(val);

    // get appnum with WILDCARD - should fail!
    PMIX_LOAD_PROCID(&proc, myproc.nspace, PMIX_RANK_WILDCARD);
    rc = PMIx_Get(&proc, PMIX_APPNUM, NULL, 0, &val);
    if (PMIX_SUCCESS == rc) {
        fprintf(stderr, "%s: Get appnum with WILDCARD incorrectly succeeded - returned %u\n",
                PMIx_Proc_string(&myproc), val->data.uint32);
        fflush(stderr);
        goto done;
    }
    fprintf(stderr, "%s: Appnum with WILDCARD rank correctly failed - %s\n",
            PMIx_Proc_string(&myproc), PMIx_Error_string(rc));
    PMIX_VALUE_RELEASE(val);

    // get peer's appnum
    if (0 == myproc.rank) {
        proc.rank = 1;
    } else {
        proc.rank = 0;
    }
    rc = PMIx_Get(&proc, PMIX_APPNUM, NULL, 0, &val);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "%s: Get peer's appnum failed - %s\n", PMIx_Proc_string(&myproc), PMIx_Error_string(rc));
        fflush(stderr);
        goto done;
    }
    // rank=0 is in appnum 0, rank=1 is in appnum 1
    if (val->data.uint32 != proc.rank) {
        fprintf(stderr, "%s: Get peer's appnum returned wrong value - %u instead of %u\n",
                PMIx_Proc_string(&myproc), val->data.uint32, proc.rank);
        fflush(stderr);
        goto done;
    }
    fprintf(stderr, "%s: Peer's Appnum return correct\n", PMIx_Proc_string(&myproc));
    PMIX_VALUE_RELEASE(val);

done:
    /* finalize us */
    rc = PMIx_Finalize(NULL, 0);
    fflush(stderr);
    return (0);
}
