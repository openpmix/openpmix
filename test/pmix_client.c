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

    TEST_OUTPUT(("rank X: Start", rank));

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
        } else if ( (0 == strcmp(argv[i], "-v") ) || (0 == strcmp(argv[i], "--verbose") ) ) {
            TEST_VERBOSE_ON();
        }else {
            fprintf(stderr, "unrecognized option: %s\n", argv[i]);
            exit(1);
        }
    }

    TEST_OUTPUT(("rank X: parsed command line", rank));

    /* init us */
    if (PMIX_SUCCESS != (rc = PMIx_Init(nspace, &rank))) {
        TEST_ERROR(("rank %d: PMIx_Init failed: %d", rank, rc));
        goto error_out;
    }

    TEST_OUTPUT(("rank %d: PMIx_Init success", rank));

    if (PMIX_SUCCESS != (rc = PMIx_Get(nspace, rank,PMIX_UNIV_SIZE,&val))) {
        TEST_ERROR(("rank %d: PMIx_Get universe size failed: %d", rank, rc));
        goto error_out;
    }
    if (NULL == val) {
        TEST_ERROR(("rank %d: PMIx_Get universe size returned NULL value", rank));
        goto error_out;
    }
    if (val->type != PMIX_UINT32 || val->data.uint32 != nprocs ) {
        TEST_ERROR(("rank %d: Universe size value or type mismatch,"
                    " want %d(%d) get %d(%d)",
                    rank, nprocs, PMIX_UINT32,
                    val->data.integer, val->type));
        goto error_out;
    }

    TEST_OUTPUT(("rank %d: Universe size check: PASSED", rank));

    if( 0 != strcmp(nspace, TEST_NAMESPACE) ) {
        TEST_ERROR(("rank %d: Bad nspace!", rank));
        exit(0);
    }

    for (i=0; i < 3; i++) {
        (void)snprintf(key, 50, "local-key-%d", i);
        PMIX_VAL_SET(&value, int, 12340 + i);
        if (PMIX_SUCCESS != (rc = PMIx_Put(PMIX_LOCAL, key, &value))) {
            TEST_ERROR(("rank %d: PMIx_Put failed: %d", rank, rc));
            goto error_out;
        }

        (void)snprintf(key, 50, "remote-key-%d", i);
        (void)snprintf(sval, 50, "Test string #%d", i);
        PMIX_VAL_SET(&value, string, sval);
        if (PMIX_SUCCESS != (rc = PMIx_Put(PMIX_REMOTE, key, &value))) {
            TEST_ERROR(("rank %d: PMIx_Put failed: %d", rank, rc));
            goto error_out;
        }
        PMIX_VALUE_DESTRUCT(&value);
        
        (void)snprintf(key, 50, "global-key-%d", i);
        PMIX_VAL_SET(&value, float, 12.15 + i);
        if (PMIX_SUCCESS != (rc = PMIx_Put(PMIX_GLOBAL, key, &value))) {
            TEST_ERROR(("rank %d: PMIx_Put failed: %d", rank, rc));
            goto error_out;
        }
    }
    
    /* Submit the data */
    if (PMIX_SUCCESS != (rc = PMIx_Fence(NULL, 0, 1))) {
        TEST_ERROR(("rank %d: PMIx_Fence failed: %d", rank, rc));
        goto error_out;
    }
    TEST_OUTPUT(("rank %d: Fence successfully completed", rank));
    
    /* Check the predefined output */
    for (i=0; i < nprocs; i++) {

        for (j=0; j < 3; j++) {
            sprintf(key,"local-key-%d",j);
            if (PMIX_SUCCESS != (rc = PMIx_Get(nspace, i, key, &val))) {
                TEST_ERROR(("rank %d: PMIx_Get failed: %d", rank, rc));
                goto error_out;
            }
            if (NULL == val) {
                TEST_ERROR(("rank %d: PMIx_Get returned NULL value", rank));
                goto error_out;
            }
            if (val->type != PMIX_INT || val->data.integer != (12340+j)) {
                TEST_ERROR(("rank %d: Key %s value or type mismatch,"
                            " want %d(%d) get %d(%d)",
                            rank, key, (12340+j), PMIX_INT,
                            val->data.integer, val->type));
                goto error_out;
            }
            TEST_VERBOSE(("rank %d: GET OF %s SUCCEEDED", rank, key));
            PMIX_VALUE_RELEASE(val);

            sprintf(key,"remote-key-%d",j);
            sprintf(sval,"Test string #%d",j);
            if (PMIX_SUCCESS != (rc = PMIx_Get(nspace, i, key, &val))) {
                TEST_ERROR(("rank %d: PMIx_Get failed (%d)", rank, rc));
                goto error_out;
            }
            if (val->type != PMIX_STRING || strcmp(val->data.string, sval)) {
                TEST_ERROR(("rank %d:  Key %s value or type mismatch, wait %s(%d) get %s(%d)",
                            rank, key, sval, PMIX_STRING, val->data.string, val->type));
                goto error_out;
            }
            TEST_VERBOSE(("rank %d: GET OF %s SUCCEEDED", rank, key));
            PMIX_VALUE_RELEASE(val);

            sprintf(key, "global-key-%d", j);
            if (PMIX_SUCCESS != (rc = PMIx_Get(nspace, i, key, &val))) {
                TEST_ERROR(("rank %d: PMIx_Get failed (%d)", rank, rc))
                goto error_out;
            }
            if (val->type != PMIX_FLOAT || val->data.fval != (float)12.15 + j) {
                TEST_ERROR(("rank %d [ERROR]: Key %s value or type mismatch,"
                            " wait %f(%d) get %f(%d)",
                            rank, key, ((float)10.15 + i), PMIX_FLOAT,
                            val->data.fval, val->type));
                goto error_out;
            }
            PMIX_VALUE_RELEASE(val);
            TEST_VERBOSE(("rank %d: GET OF %s SUCCEEDED", rank, key));
        }

        /* ask for a non-existent key */
        if (PMIX_SUCCESS == (rc = PMIx_Get(nspace, i, "foobar", &val))) {
            TEST_ERROR(("rank %d: PMIx_Get returned success instead of failure",
                        rank));
            goto error_out;
        }
        if (PMIX_ERR_NOT_FOUND != rc) {
            TEST_ERROR(("rank %d [ERROR]: PMIx_Get returned %d instead of not_found",
                        rank, rc));
        }
        if (NULL != val) {
            TEST_ERROR(("rank %d [ERROR]: PMIx_Get did not return NULL value", rank));
            goto error_out;
        }
        TEST_VERBOSE(("rank %d: rank %d is OK", rank, i));
    }

    TEST_OUTPUT(("rank %d: test PASSED", rank));

 error_out:
    /* finalize us */
    TEST_OUTPUT(("rank %d: Finalizing", rank));
    fflush(stderr);
    if (PMIX_SUCCESS != (rc = PMIx_Finalize())) {
        TEST_ERROR(("rank %d:PMIx_Finalize failed: %d", rank, rc));
    } else {
        TEST_OUTPUT(("rank %d:PMIx_Finalize successfully completed", rank));
    }
    
    exit(0);
}
