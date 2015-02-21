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
#include <unistd.h>

#include "src/api/pmix.h"
#include "src/class/pmix_object.h"
#include "src/buffer_ops/types.h"
#include "test_common.h"

int main(int argc, char **argv)
{
    char nspace[PMIX_MAX_VALLEN];
    int rank;
    int rc, i, j;
    pmix_value_t value;
    char key[50], sval[50];
    int nprocs = 1;
    bool barrier = false;
    bool collect = false;
    bool nonblocking = false;
    pmix_value_t *val = &value;

    fprintf(stderr, "rank %d: Start\n", rank);

    /* check options */
    for (i=1; i < argc; i++) {
        if (0 == strcmp(argv[i], "--n") || 0 == strcmp(argv[i], "-n")) {
            i++;
            nprocs = strtol(argv[i], NULL, 10);
        } else if (0 == strcmp(argv[i], "barrier")) {
            barrier = true;
        } else if (0 == strcmp(argv[i], "collect")) {
            collect = true;
        } else if (0 == strcmp(argv[i], "nb")) {
            nonblocking = true;
        } else {
            fprintf(stderr, "unrecognized option: %s\n", argv[i]);
            exit(1);
        }
    }

    fprintf(stderr, "rank %d: parsed command line\n", rank);

    /* init us */
    if (PMIX_SUCCESS != (rc = PMIx_Init(nspace, &rank))) {
        fprintf(stderr, "rank %d: PMIx_Init failed: %d\n", rank, rc);
        goto error_out;
    }

    fprintf(stderr, "rank %d: PMIx_Init success\n", rank);

    if( 0 != strcmp(nspace, TEST_NAMESPACE) ) {
        printf("rank %d: Bad nspace!\n", rank);
    }

    for (i=0; i < 3; i++) {
        (void)snprintf(key, 50, "local-key-%d", i);
        PMIX_VAL_SET(&value, int, 12340 + i);
        if (PMIX_SUCCESS != (rc = PMIx_Put(PMIX_LOCAL, key, &value))) {
            fprintf(stderr, "PMIx cli: PMIx_Put failed: %d\n", rc);
            goto error_out;
        }

        (void)snprintf(key, 50, "remote-key-%d", i);
        (void)snprintf(sval, 50, "Test string #%d", i);
        PMIX_VAL_SET(&value, string, sval);
        if (PMIX_SUCCESS != (rc = PMIx_Put(PMIX_REMOTE, key, &value))) {
            fprintf(stderr, "PMIx cli: PMIx_Put failed: %d\n", rc);
            goto error_out;
        }
        PMIX_VALUE_DESTRUCT(&value);
        
        (void)snprintf(key, 50, "global-key-%d", i);
        PMIX_VAL_SET(&value, float, 12.15 + i);
        if (PMIX_SUCCESS != (rc = PMIx_Put(PMIX_GLOBAL, key, &value))) {
            fprintf(stderr, "PMIx cli: PMIx_Put failed: %d\n", rc);
            goto error_out;
        }
    }
    
    /* Submit the data */
    if (PMIX_SUCCESS != (rc = PMIx_Fence(NULL, 0, 1))) {
        fprintf(stderr, "rank %d [ERROR]: PMIx_Fence failed (%d)\n",
                rank, rc);
        goto error_out;
    }
    fprintf(stderr, "rank %d: Fence successfully completed\n", rank);
    
    /* Check the predefined output */
    for (i=0; i < nprocs; i++) {

        for (j=0; j < 3; j++) {
            sprintf(key,"local-key-%d",j);
            if (PMIX_SUCCESS != (rc = PMIx_Get(nspace, i, key, &val))) {
                fprintf(stderr, "rank %d [ERROR]: PMIx_Get failed (%d)\n",
                        rank, rc);
                goto error_out;
            }
            if (NULL == val) {
                fprintf(stderr, "rank %d [ERROR]: PMIx_Get returned NULL value\n", rank);
                goto error_out;
            }
            if (val->type != PMIX_INT || val->data.integer != (12340+j)) {
                fprintf(stderr, "rank %d [ERROR]: Key %s value or type mismatch,"
                        " want %d(%d) get %d(%d)\n",
                        rank, key, (12340+j), PMIX_INT,
                        val->data.integer, val->type);
                goto error_out;
            }
            fprintf(stderr, "rank %d: GET OF %s SUCCEEDED\n", rank, key);
            PMIX_VALUE_RELEASE(val);

            sprintf(key,"remote-key-%d",j);
            sprintf(sval,"Test string #%d",j);
            if (PMIX_SUCCESS != (rc = PMIx_Get(nspace, i, key, &val))) {
                fprintf(stderr, "rank %d [ERROR]: PMIx_Get failed (%d)\n", rank, rc);
                goto error_out;
            }
            if (val->type != PMIX_STRING || strcmp(val->data.string, sval)) {
                fprintf(stderr, "PMIx cli: Key %s value or type mismatch, wait %s(%d) get %s(%d)\n",
                        key, sval, PMIX_STRING, val->data.string, val->type);
                goto error_out;
            }
            fprintf(stderr, "rank %d: GET OF %s SUCCEEDED\n", rank, key);
            PMIX_VALUE_RELEASE(val);

            sprintf(key, "global-key-%d", j);
            if (PMIX_SUCCESS != (rc = PMIx_Get(nspace, i, key, &val))) {
                fprintf(stderr, "PMIx cli: PMIx_Get failed (%d)\n", rc);
                goto error_out;
            }
            if (val->type != PMIX_FLOAT || val->data.fval != (float)12.15 + j) {
                fprintf(stderr, "rank %d [ERROR]: Key %s value or type mismatch,"
                        " wait %f(%d) get %f(%d)\n",
                        rank, key, ((float)10.15 + i), PMIX_FLOAT,
                        val->data.fval, val->type);
                goto error_out;
            }
            PMIX_VALUE_RELEASE(val);
            fprintf(stderr, "rank %d: GET OF %s SUCCEEDED\n", rank, key);
            fprintf(stderr,"rank %d: rank %d is OK\n", rank, i);
        }
        /* ask for a non-existent key */
        if (PMIX_SUCCESS == (rc = PMIx_Get(nspace, i, "foobar", &val))) {
            fprintf(stderr, "rank %d [ERROR]: PMIx_Get returned success instead of failure\n",
                    rank);
            goto error_out;
        }
        if (PMIX_ERR_NOT_FOUND != rc) {
            fprintf(stderr, "rank %d [ERROR]: PMIx_Get returned %d instead of not_found\n",
                    rank, rc);
        }
        if (NULL != val) {
            fprintf(stderr, "rank %d [ERROR]: PMIx_Get did not return NULL value\n", rank);
            goto error_out;
        }
        fprintf(stderr, "rank %d: GET OF NON-EXISTENT KEY CORRECTLY HANDLED\n", rank);
    }

 error_out:
    /* finalize us */
    fprintf(stderr, "rank %d: Finalizing\n", rank);
    fflush(stderr);
    if (PMIX_SUCCESS != (rc = PMIx_Finalize())) {
        fprintf(stderr, "rank %d:PMIx_Finalize failed: %d\n", rank, rc);
    } else {
        fprintf(stderr, "rank %d:PMIx_Finalize successfully completed\n", rank);
    }
    
    exit(0);
}
