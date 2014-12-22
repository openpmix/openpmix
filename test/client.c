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
 * Copyright (c) 2013-2014 Intel, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 */

#include <stdio.h>
#include <stdlib.h>

#include <api/pmix.h>
#include <class/pmix_object.h>
#include <buffer_ops/types.h>


int main(int argc, char **argv)
{
    char *namespace;
    int rank;
    int rc;
    pmix_value_t kv;

    /* check for the server uri */
    if (NULL == getenv("PMIX_SERVER_URI")) {
        fprintf(stderr, "PMIX_SERVER_URI not found\n");
        exit(1);
    }

    /* check for the PMIX_ID */
    if (NULL == getenv("PMIX_ID")) {
        fprintf(stderr, "PMIX_ID not found\n");
        exit(1);
    }

    /* init us */
    if (PMIX_SUCCESS != (rc = PMIx_Init(&namespace, &rank))) {
        fprintf(stderr, "PMIx_Init failed: %d\n", rc);
        return rc;
    }

    PMIX_KP_SET(&kv, "local-key", int, 12345, rc, kvp_error );
    if (PMIX_SUCCESS != (rc = PMIx_Put(PMIX_LOCAL, &kv))) {
        fprintf(stderr, "PMIx_Put failed: %d\n", rc);
    }
    OBJ_DESTRUCT(&kv);

    char *ptr = "Test string";
    PMIX_KP_SET(&kv, "remote-key", string, ptr, rc, kvp_error );
    if (PMIX_SUCCESS != (rc = PMIx_Put(PMIX_REMOTE, &kv))) {
        fprintf(stderr, "PMIx_Put failed: %d\n", rc);
    }
    OBJ_DESTRUCT(&kv);

    PMIX_KP_SET(&kv, "global-key", float, 10.15, rc, kvp_error );
    if (PMIX_SUCCESS != (rc = PMIx_Put(PMIX_GLOBAL, &kv))) {
        fprintf(stderr, "PMIx_Put failed: %d\n", rc);
    }
    OBJ_DESTRUCT(&kv);


    /* Submit the data */
    if (PMIX_SUCCESS != (rc = PMIx_Fence(NULL))) {
        fprintf(stderr, "PMIx_Fence failed: %d\n", rc);
        return rc;
    }

    /* finalize us */
    if (PMIX_SUCCESS != (rc = PMIx_Finalize())) {
        fprintf(stderr, "PMIx_Finalize failed: %d\n", rc);
        return rc;
    }
    
    return 0;
kvp_error:
    fprintf(stderr,"Cannot Set Key-Value Pair\n");
    return 0;
}
