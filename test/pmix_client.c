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

#include "src/api/pmix.h"
#include "src/class/pmix_object.h"
#include "src/buffer_ops/types.h"
#include "test_common.h"

void notify_parent()
{
    pid_t ppid = getppid();
    kill(ppid, SIGUSR1);
}

int main(int argc, char **argv)
{
    char nspace[PMIX_MAX_VALLEN];
    int rank;
    int rc, i;
    pmix_value_t value;
    char *key = NULL;

    {
        int delay = 0;
        while( delay ){
            sleep(1);
        }
    }

    /* init us */
    if (PMIX_SUCCESS != (rc = PMIx_Init(nspace, &rank))) {
        fprintf(stderr, "PMIx cli: PMIx_Init failed: %d\n", rc);
        notify_parent();
        exit(0);
    }

    if( 0 != strcmp(nspace, TEST_NAMESPACE) ) {
        printf("PMIx cli: Bad namespace!\n");
    }

    key = "local-key-2";
    PMIX_VAL_SET(&value, int, 12342);
    if (PMIX_SUCCESS != (rc = PMIx_Put(PMIX_LOCAL, key, &value))) {
        fprintf(stderr, "PMIx cli: PMIx_Put failed: %d\n", rc);
        notify_parent();
    }

    key = "remote-key-2";
    char *ptr = "Test string #2";
    PMIX_VAL_SET(&value, string, ptr);
    if (PMIX_SUCCESS != (rc = PMIx_Put(PMIX_REMOTE, key, &value))) {
        fprintf(stderr, "PMIx cli: PMIx_Put failed: %d\n", rc);
        notify_parent();
        exit(0);
    }

    key = "global-key-2";
    PMIX_VAL_SET(&value, float, 12.15);
    if (PMIX_SUCCESS != (rc = PMIx_Put(PMIX_GLOBAL, key, &value))) {
        fprintf(stderr, "PMIx cli: PMIx_Put failed: %d\n", rc);
        notify_parent();
        exit(0);
    }

    /* Submit the data */
    pmix_range_t range;
    range.ranks = NULL;
    range.nranks = 0;

    {
        int delay = 0;
        while( delay ){
            sleep(1);
        }
    }


    if (PMIX_SUCCESS != (rc = PMIx_Fence(NULL, 0))) {
        fprintf(stderr, "PMIx cli: PMIx_Fence failed (%d)\n", rc);
        notify_parent();
        exit(0);
    }

    /* Check the predefined output */
    for(i=0;i<3;i++){
        char key[256], sval[256];
        pmix_value_t *val;
        sprintf(key,"local-key-%d",i);
        if( PMIX_SUCCESS != ( rc = PMIx_Get(nspace, i, key, &val) ) ){
            fprintf(stderr, "PMIx cli: PMIx_Fence failed (%d)\n", rc);
            notify_parent();
            exit(0);
        }
        if( val->type != PMIX_INT || val->data.integer != (12340+i) ){
            printf("Key %s value or type mismatch, wait %d(%d) get %d(%d)\n",
                   key, (12340+i), PMIX_INT, val->data.integer, val->type);
            notify_parent();
            exit(0);
        }
        free(val);

        sprintf(key,"remote-key-%d",i);
        sprintf(sval,"Test string #%d",i);
        if( PMIX_SUCCESS != ( rc = PMIx_Get(nspace, i, key, &val) ) ){
            fprintf(stderr, "PMIx cli: PMIx_Fence failed (%d)\n", rc);
            notify_parent();
            exit(0);
        }
        if( val->type != PMIX_STRING || strcmp(val->data.string, sval) ){
            fprintf(stderr, "PMIx cli: Key %s value or type mismatch, wait %s(%d) get %s(%d)\n",
                   key, sval, PMIX_STRING, val->data.string, val->type);
            notify_parent();
            exit(0);
        }
        free(val);

        sprintf(key,"global-key-%d",i);
        if( PMIX_SUCCESS != ( rc = PMIx_Get(nspace, i, key, &val) ) ){
            fprintf(stderr, "PMIx cli: PMIx_Fence failed (%d)\n", rc);
            notify_parent();
            exit(0);
        }
        if( val->type != PMIX_FLOAT || val->data.fval != (float)10.15 + i ){
            printf("PMIx cli: Key %s value or type mismatch, wait %f(%d) get %f(%d)\n",
                   key, ((float)10.15 + i), PMIX_FLOAT, val->data.fval, val->type);
            notify_parent();
            exit(0);
        }
        free(val);
        fprintf(stderr,"PMIx cli: rank %d is OK\n", i);
    }

    /* finalize us */
    if (PMIX_SUCCESS != (rc = PMIx_Finalize())) {
        fprintf(stderr, "PMIx_Finalize failed: %d\n", rc);
        notify_parent();
        exit(0);
    }
    
    return 0;
 kvp_error:
    fprintf(stderr,"Cannot Set Key-Value Pair\n");
    return 0;
}
