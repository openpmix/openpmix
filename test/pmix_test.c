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

#include "src/util/argv.h"
#include "src/util/pmix_environ.h"
#include "src/util/output.h"

#include "server_callbacks.h"
#include "utils.h"

int main(int argc, char **argv)
{
    char **client_env=NULL;
    char **client_argv=NULL;
    int rc;
    uint32_t n;
    char *tmp;
    uid_t myuid;
    gid_t mygid;
    struct stat stat_buf;
    struct timeval tv;
    double test_start;
    char *ranks = NULL;
    int order[CLI_TERM+1];
    char digit[MAX_DIGIT_LEN];
    int cl_arg_len;
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

    TEST_VERBOSE(("Setting job info"));
    fill_seq_ranks_array(params.nprocs, &ranks);
    if (NULL == ranks) {
        PMIx_server_finalize();
        TEST_ERROR(("fill_seq_ranks_array failed"));
        FREE_TEST_PARAMS(params);
        return PMIX_ERROR;
    }
    set_namespace(params.nprocs, ranks, TEST_NAMESPACE);
    if (NULL != ranks) {
        free(ranks);
    }

    /* fork/exec the test */
    client_env = pmix_argv_copy(environ);
    pmix_argv_append_nosize(&client_argv, params.binary);
    pmix_argv_append_nosize(&client_argv, "-s");
    pmix_argv_append_nosize(&client_argv, TEST_NAMESPACE);
    if (params.nonblocking) {
        pmix_argv_append_nosize(&client_argv, "-nb");
        if (params.barrier) {
            pmix_argv_append_nosize(&client_argv, "-b");
        }
    }
    if (params.collect) {
        pmix_argv_append_nosize(&client_argv, "-c");
    }
    pmix_argv_append_nosize(&client_argv, "-n");
    if (NULL == params.np) {
        pmix_argv_append_nosize(&client_argv, "1");
    } else {
        pmix_argv_append_nosize(&client_argv, params.np);
    }
    if( params.verbose ){
        pmix_argv_append_nosize(&client_argv, "-v");
    }
    if (NULL != params.prefix) {
        pmix_argv_append_nosize(&client_argv, "-o");
        pmix_argv_append_nosize(&client_argv, params.prefix);
    }
    if( params.early_fail ){
        pmix_argv_append_nosize(&client_argv, "--early-fail");
    }

    tmp = pmix_argv_join(client_argv, ' ');
    TEST_VERBOSE(("Executing test: %s", tmp));
    free(tmp);

    myuid = getuid();
    mygid = getgid();

    order[CLI_UNINIT] = CLI_FORKED;
    order[CLI_FORKED] = CLI_FIN;
    order[CLI_CONNECTED] = -1;
    order[CLI_FIN] = CLI_TERM;
    order[CLI_DISCONN] = -1;
    order[CLI_TERM] = -1;
    cli_init(params.nprocs, order);

    for (n=0; n < params.nprocs; n++) {
        if (PMIX_SUCCESS != (rc = PMIx_server_setup_fork(TEST_NAMESPACE, n, &client_env))) {
            TEST_ERROR(("Server fork setup failed with error %d", rc));
            PMIx_server_finalize();
            cli_kill_all();
            FREE_TEST_PARAMS(params);
            return rc;
        }
        if (PMIX_SUCCESS != (rc = PMIx_server_register_client(TEST_NAMESPACE, n, myuid, mygid, NULL))) {
            TEST_ERROR(("Server fork setup failed with error %d", rc));
            PMIx_server_finalize();
            cli_kill_all();
            FREE_TEST_PARAMS(params);
            return rc;
        }

        cli_info[n].pid = fork();
        if (cli_info[n].pid < 0) {
            TEST_ERROR(("Fork failed"));
            PMIx_server_finalize();
            cli_kill_all();
            FREE_TEST_PARAMS(params);
            return -1;
        }
        /* add two last arguments: -r <rank> */
        sprintf(digit, "%d", n);
        pmix_argv_append_nosize(&client_argv, "-r");
        pmix_argv_append_nosize(&client_argv, digit);

        if (cli_info[n].pid == 0) {
            if( !TEST_VERBOSE_GET() ){
                // Hide clients stdout
                // TODO: on some systems stdout is a constant, address this
                fclose(stdout);
                stdout = fopen("/dev/null","w");
            }
            execve(params.binary, client_argv, client_env);
            /* Does not return */
            exit(0);
        }
        cli_info[n].state = CLI_FORKED;

        /* delete two last arguments : -r <rank> */
        cl_arg_len = pmix_argv_len(client_argv);
        pmix_argv_delete(&cl_arg_len, &client_argv, cl_arg_len-2, 2);
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

