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
 * Copyright (c) 2013-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2026      Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 */

#include "src/include/pmix_config.h"
#include "include/pmix.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "src/include/pmix_globals.h"
#include "src/util/pmix_output.h"

/* A PMIx_Group_construct that is not an "add members" or "bootstrap"
 * operation must list every participant in its procs array, and the caller
 * must be one of them. Verify the client rejects a construct in which the
 * caller does not appear: the membership check runs entirely on the client
 * side, before anything is sent to the server, and must return
 * PMIX_ERR_NOT_A_MEMBER. */
int main(int argc, char **argv)
{
    pmix_status_t rc;
    pmix_proc_t myproc;
    pmix_proc_t others[2];
    pmix_info_t *results = NULL;
    size_t nresults = 0;
    PMIX_HIDE_UNUSED_PARAMS(argc, argv);

    if (PMIX_SUCCESS != (rc = PMIx_Init(&myproc, NULL, 0))) {
        fprintf(stderr, "Client PMIx_Init failed: %s\n", PMIx_Error_string(rc));
        exit(1);
    }
    pmix_output(0, "Client %s:%d Init", myproc.nspace, myproc.rank);

    /* build a participant array that deliberately excludes us: a foreign
     * namespace, plus a non-wildcard rank in our own namespace that is not
     * our rank. Neither entry covers the calling process. */
    PMIx_Load_procid(&others[0], "not-a-real-namespace", 0);
    PMIx_Load_procid(&others[1], myproc.nspace, myproc.rank + 100);

    rc = PMIx_Group_construct("simpgrpmember", others, 2, NULL, 0, &results, &nresults);
    if (NULL != results) {
        PMIX_INFO_FREE(results, nresults);
    }
    pmix_output(0, "Client %s:%d Group_construct returned %s", myproc.nspace,
                myproc.rank, PMIx_Error_string(rc));

    if (PMIX_ERR_NOT_A_MEMBER != rc) {
        fprintf(stderr,
                "Client %s:%d FAIL: expected PMIX_ERR_NOT_A_MEMBER, got %s\n",
                myproc.nspace, myproc.rank, PMIx_Error_string(rc));
        PMIx_Finalize(NULL, 0);
        exit(1);
    }

    if (PMIX_SUCCESS != (rc = PMIx_Finalize(NULL, 0))) {
        fprintf(stderr, "Client %s:%d PMIx_Finalize failed: %s\n", myproc.nspace,
                myproc.rank, PMIx_Error_string(rc));
        exit(1);
    }

    fprintf(stderr, "Client %s:%d rejected the not-a-member group construct OK\n",
            myproc.nspace, myproc.rank);
    fflush(stderr);
    return 0;
}
