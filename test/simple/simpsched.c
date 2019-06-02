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
 * Copyright (c) 2013-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2015-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2016      IBM Corporation.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 */

#include <src/include/pmix_config.h>
#include <pmix_server.h>
#include <pmix_sched.h>
#include <src/include/types.h>
#include <src/include/pmix_globals.h>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>
#include <signal.h>

#if PMIX_HAVE_HWLOC
#include <src/hwloc/hwloc-internal.h>
#endif

#include "src/class/pmix_list.h"
#include "src/util/pmix_environ.h"
#include "src/util/output.h"
#include "src/util/printf.h"
#include "src/util/argv.h"

#include "simptest.h"

static pmix_server_module_t mymodule = {0};

int main(int argc, char **argv)
{
    pmix_info_t *info;
    pmix_status_t rc;
    pmix_fabric_t myfabric;
    uint32_t nverts, n32, m32;
    uint16_t cost;
    pmix_value_t val;
    size_t ninfo;
    int exit_code=0;
    char *nodename;
    size_t n;

    /* smoke test */
    if (PMIX_SUCCESS != 0) {
        fprintf(stderr, "ERROR IN COMPUTING CONSTANTS: PMIX_SUCCESS = %d\n", PMIX_SUCCESS);
        exit(1);
    }

    fprintf(stderr, "Testing version %s\n", PMIx_Get_version());

    ninfo = 1;
    PMIX_INFO_CREATE(info, ninfo);
    PMIX_INFO_LOAD(&info[0], PMIX_SERVER_SCHEDULER, NULL, PMIX_BOOL);
    if (PMIX_SUCCESS != (rc = PMIx_server_init(&mymodule, info, ninfo))) {
        fprintf(stderr, "Init failed with error %d\n", rc);
        return rc;
    }
    PMIX_INFO_FREE(info, ninfo);

    /* register a fabric */
    rc = PMIx_server_register_fabric(&myfabric, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "Fabric registration failed with error: %s\n", PMIx_Error_string(rc));
        goto cleanup;
    }

    /* get the number of vertices in the fabric */
    rc = PMIx_server_get_num_vertices(&myfabric, &nverts);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "Fabric get_num_vertices failed with error: %s\n", PMIx_Error_string(rc));
        goto cleanup;
    }
    fprintf(stderr, "Number of fabric vertices: %u\n", nverts);

    for (n32=0; n32 < nverts; n32++) {
        fprintf(stderr, "%u:", n32);
        for (m32=0; m32 < nverts; m32++) {
            rc = PMIx_server_get_comm_cost(&myfabric, n32, m32, &cost);
            fprintf(stderr, "   %u", cost);
        }
        fprintf(stderr, "\n");
    }

    rc = PMIx_server_get_vertex_info(&myfabric, nverts/2, &val, &nodename);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "Fabric get vertex info failed with error: %s\n", PMIx_Error_string(rc));
        goto cleanup;
    }
    if (PMIX_DATA_ARRAY != val.type) {
        fprintf(stderr, "Fabric get vertex info returned wrong type: %s\n", PMIx_Data_type_string(val.type));
        goto cleanup;
    }
    fprintf(stderr, "Vertex info for index %u on node %s:\n", nverts/2, nodename);
    info = (pmix_info_t*)val.data.darray->array;
    for (n=0; n < val.data.darray->size; n++) {
        fprintf(stderr, "\t%s:\t%s\n", info[n].key, info[n].value.data.string);
    }
    PMIX_VALUE_DESTRUCT(&val);
    free(nodename);

    PMIX_INFO_CREATE(info, 1);
    PMIX_INFO_LOAD(&info[0], PMIX_NETWORK_NIC, "test002:nic002", PMIX_STRING);
    val.type = PMIX_DATA_ARRAY;
    PMIX_DATA_ARRAY_CREATE(val.data.darray, 1, PMIX_INFO);
    val.data.darray->array = info;
    rc = PMIx_server_get_index(&myfabric, &val, &n32, &nodename);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "Fabric get index failed with error: %s\n", PMIx_Error_string(rc));
        goto cleanup;
    }
    fprintf(stderr, "Index %u on host %s\n", n32, nodename);

  cleanup:
    if (PMIX_SUCCESS != rc) {
        exit_code = rc;
    }
    /* finalize the server library */
    if (PMIX_SUCCESS != (rc = PMIx_server_finalize())) {
        fprintf(stderr, "Finalize failed with error %d\n", rc);
        exit_code = rc;
    }

    if (0 == exit_code) {
        fprintf(stderr, "Test finished OK!\n");
    } else {
        fprintf(stderr, "TEST FAILED WITH ERROR %d\n", exit_code);
    }

    return exit_code;
}
