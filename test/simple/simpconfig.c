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
 * Copyright (c) 2019      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2021      Nanook Consulting.  All rights reserved.
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
#include <time.h>
#include <unistd.h>

#include "src/class/pmix_object.h"
#include "src/include/pmix_globals.h"
#include "src/util/output.h"
#include "src/util/printf.h"

#define MAXCNT 1

static pmix_proc_t myproc;

int main(int argc, char **argv)
{
    int rc;
    pmix_value_t value;
    pmix_value_t *val = &value;
    pmix_info_t info, *iptr;
    size_t nsize, n;

    /* init us without connecting */
    PMIX_INFO_LOAD(&info, PMIX_TOOL_DO_NOT_CONNECT, NULL, PMIX_BOOL);
    if (PMIX_SUCCESS != (rc = PMIx_tool_init(&myproc, &info, 1))) {
        pmix_output(0, "Client ns %s rank %d: PMIx_Init failed: %s", myproc.nspace, myproc.rank,
                    PMIx_Error_string(rc));
        exit(rc);
    }
    pmix_output(0, "Client ns %s rank %d: Running on node %s",
                myproc.nspace, myproc.rank, pmix_globals.hostname);

    /* get the config */
    if (PMIX_SUCCESS != (rc = PMIx_Get(&myproc, PMIX_CONFIGURATION, NULL, 0, &val))) {
        pmix_output(0, "Client ns %s rank %d: PMIx_Get configuration failed: %s", myproc.nspace,
                    myproc.rank, PMIx_Error_string(rc));
        exit(rc);
    }
    if (NULL == val || PMIX_DATA_ARRAY != val->type) {
        pmix_output(0, "Bad configuration returned");
        exit(-1);
    }

    // print it out
    iptr = (pmix_info_t*)val->data.darray->array;
    nsize = val->data.darray->size;

    for (n=0; n < nsize; n++) {
        pmix_output(0, "%s: %s", iptr[n].key, iptr[n].value.data.string);
    }

    /* finalize us */
    pmix_output(0, "Client ns %s rank %d: Finalizing", myproc.nspace, myproc.rank);
    if (PMIX_SUCCESS != (rc = PMIx_tool_finalize())) {
        fprintf(stderr, "Client ns %s rank %d:PMIx_Finalize failed: %s\n", myproc.nspace,
                myproc.rank, PMIx_Error_string(rc));
    } else {
        fprintf(stderr, "Client ns %s rank %d:PMIx_Finalize successfully completed\n",
                myproc.nspace, myproc.rank);
    }
    fflush(stderr);
    return (rc);
}
