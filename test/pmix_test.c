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
 * Copyright (c) 2013-2015 Intel, Inc.  All rights reserved.
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

#include "src/util/pmix_environ.h"
#include "src/util/output.h"

#include "server_callbacks.h"
#include "utils.h"

int main(int argc, char **argv)
{
    char **client_env=NULL;
    char **client_argv=NULL;
    int rc;
    struct stat stat_buf;
    struct timeval tv;
    double test_start;
    int order[CLI_TERM+1];
    test_params params;
    INIT_TEST_PARAMS(params);
    int test_fail = 0;

    gettimeofday(&tv, NULL);
    test_start = tv.tv_sec + 1E-6*tv.tv_usec;

    /* smoke test */
    if (PMIX_SUCCESS != 0) {
        TEST_ERROR(("ERROR IN COMPUTING CONSTANTS: PMIX_SUCCESS = %d", PMIX_SUCCESS));
        exit(1);
    }

    TEST_VERBOSE(("Testing version %s", PMIx_Get_version()));

    parse_cmd(argc, argv, &params);
    TEST_VERBOSE(("Start PMIx_lite smoke test (timeout is %d)", params.timeout));

    /* verify executable */
    if( 0 > ( rc = stat(params.binary, &stat_buf) ) ){
        TEST_ERROR(("Cannot stat() executable \"%s\": %d: %s", params.binary, errno, strerror(errno)));
        FREE_TEST_PARAMS(params);
        return 0;
    } else if( !S_ISREG(stat_buf.st_mode) ){
        TEST_ERROR(("Client executable \"%s\": is not a regular file", params.binary));
        FREE_TEST_PARAMS(params);
        return 0;
    }else if( !(stat_buf.st_mode & S_IXUSR) ){
        TEST_ERROR(("Client executable \"%s\": has no executable flag", params.binary));
        FREE_TEST_PARAMS(params);
        return 0;
    }

    /* setup the server library */
    if (PMIX_SUCCESS != (rc = PMIx_server_init(&mymodule, true))) {
        TEST_ERROR(("Init failed with error %d", rc));
        FREE_TEST_PARAMS(params);
        return rc;
    }
    /* register the errhandler */
    PMIx_Register_errhandler(errhandler);

    order[CLI_UNINIT] = CLI_FORKED;
    order[CLI_FORKED] = CLI_FIN;
    order[CLI_CONNECTED] = -1;
    order[CLI_FIN] = CLI_TERM;
    order[CLI_DISCONN] = -1;
    order[CLI_TERM] = -1;
    cli_init(params.nprocs, order);

    /* set namespaces and fork clients */
    rc = launch_clients(params, &client_env, &client_argv);
    if (PMIX_SUCCESS != rc) {
        return rc;
    }

    /* hang around until the client(s) finalize */
    while (!test_terminated()) {
        // To avoid test hang we want to interrupt the loop each 0.1s
        double test_current;

        // check if we exceed the max time
        gettimeofday(&tv, NULL);
        test_current = tv.tv_sec + 1E-6*tv.tv_usec;
        if( (test_current - test_start) > params.timeout ){
            break;
        }
        cli_wait_all(0);
    }

    if( !test_terminated() ){
        TEST_ERROR(("Test exited by a timeout!"));
        cli_kill_all();
        test_fail = 1;
    }

    if( test_abort ){
        TEST_ERROR(("Test was aborted!"));
        cli_kill_all();
        test_fail = 1;
    }

    pmix_argv_free(client_argv);
    pmix_argv_free(client_env);

    /* deregister the errhandler */
    PMIx_Deregister_errhandler();

    cli_wait_all(1.0);

    /* finalize the server library */
    if (PMIX_SUCCESS != (rc = PMIx_server_finalize())) {
        TEST_ERROR(("Finalize failed with error %d", rc));
    }

    if (0 == test_fail) {
        TEST_OUTPUT(("Test finished OK!"));
    }

    FREE_TEST_PARAMS(params);
    return rc;
}

