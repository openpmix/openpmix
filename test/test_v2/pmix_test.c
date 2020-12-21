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
 * Copyright (c) 2015      Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2015-2018 Mellanox Technologies, Inc.
 *                         All rights reserved.
 * Copyright (c) 2020      Triad National Security, LLC.
 *                         All rights reserved.
 *
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
#include <signal.h>
#include <limits.h>

#include "src/util/pmix_environ.h"
#include "src/util/output.h"

#include "server_callbacks.h"
#include "test_server.h"
#include "test_common.h"

bool spawn_wait = false;

int main(int argc, char **argv)
{
    char **client_env=NULL;
    char **client_argv=NULL;
    int rc, i;
    struct stat stat_buf;
    int test_fail = 0;
    char *tmp;
    //int ns_nprocs;
    sigset_t unblock;

    default_params(&params, &val_params);

    /* smoke test */
    if (PMIX_SUCCESS != 0) {
        TEST_ERROR(("ERROR IN COMPUTING CONSTANTS: PMIX_SUCCESS = %d", PMIX_SUCCESS));
        exit(1);
    }

    TEST_VERBOSE(("Testing version %s", PMIx_Get_version()));

    parse_cmd(argc, argv, &params, &val_params);
    if (val_params.custom_rank_placement){
        TEST_VERBOSE(("val_params.pmix_num_nodes: %d being passed into init_nodes", val_params.pmix_num_nodes));
        init_nodes(val_params.pmix_num_nodes);
        char *local_rank_placement_string = NULL;
        TEST_VERBOSE(("Before parse_rank_placement_string, string: %s", val_params.rank_placement_string));
        local_rank_placement_string = strdup(val_params.rank_placement_string);
        parse_rank_placement_string(local_rank_placement_string, params.nservers);
        free(local_rank_placement_string);
    }
    TEST_VERBOSE(("Start PMIx_lite smoke test (timeout is %d)", params.timeout));

    /* set common argv and env */
    client_env = pmix_argv_copy(environ);
    set_client_argv(&params, &client_argv);

    tmp = pmix_argv_join(client_argv, ' ');
    TEST_VERBOSE(("Executing test: %s", tmp));
    free(tmp);

    /* verify executable */
    if( 0 > ( rc = stat(params.binary, &stat_buf) ) ){
        TEST_ERROR(("Cannot stat() executable \"%s\": %d: %s", params.binary, errno, strerror(errno)));
        free_params(&params, &val_params);
        return 0;
    } else if( !S_ISREG(stat_buf.st_mode) ){
        TEST_ERROR(("Client executable \"%s\": is not a regular file", params.binary));
        free_params(&params, &val_params);
        return 0;
    }else if( !(stat_buf.st_mode & S_IXUSR) ){
        TEST_ERROR(("Client executable \"%s\": has no executable flag", params.binary));
        free_params(&params, &val_params);
        return 0;
    }

    /* ensure that SIGCHLD is unblocked as we need to capture it */
    if (0 != sigemptyset(&unblock)) {
        fprintf(stderr, "SIGEMPTYSET FAILED\n");
        exit(1);
    }
    if (0 != sigaddset(&unblock, SIGCHLD)) {
        fprintf(stderr, "SIGADDSET FAILED\n");
        exit(1);
    }
    if (0 != sigprocmask(SIG_UNBLOCK, &unblock, NULL)) {
        fprintf(stderr, "SIG_UNBLOCK FAILED\n");
        exit(1);
    }
    // fork of other servers happens below in server_init
    if (PMIX_SUCCESS != (rc = server_init(&params, &val_params))) {
        free_params(&params, &val_params);
        return rc;
    }
    // at this point, we can have multiple servers executing this code
    cli_init(val_params.pmix_local_size);

    /* set namespaces and fork clients
     * we always have a single namespace for all clients, unlike original version of this test */
    int launched = 0;
    uint32_t j;
    int base_rank;
    if (val_params.custom_rank_placement) {
        base_rank = INT_MIN;
        TEST_VERBOSE(("Custom rank placement true, my_server_id = %d", my_server_id));
    }
    else {
    /*  Compute my base rank, ie, start counter (only applicable for default rank placements,
        which is why INT_MIN for custom rank placement case (to catch errors)).
        This loop is skipped for my_server_id == 0; so if num_servers == 2, it
        is only entered for my_server_id == 1 */
        base_rank = 0;
        for(j = 0; j < (uint32_t)my_server_id; j++) {
            base_rank += (params.nprocs % params.nservers) > (uint32_t)j ?
                    params.nprocs / params.nservers + 1 :
                    params.nprocs / params.nservers;
            TEST_VERBOSE(("Custom rank placement false, base_rank = %d, j = %d, my_server_id = %d", base_rank, j, my_server_id));
        }
    }
    //ns_nprocs = params.nprocs;
    launched += server_launch_clients(val_params.pmix_local_size, params.nprocs, base_rank,
                               &params, &val_params, &client_env, &client_argv);

    if (val_params.pmix_local_size != (uint32_t)launched) {
        TEST_ERROR(("srv #%d: Total number of processes doesn't correspond to number specified by ns_dist parameter.",
                    my_server_id));
        cli_kill_all();
        test_fail = 1;
        goto done;
    }

    /* hang around until the client(s) finalize */
    while (!test_complete) {
        struct timespec ts;
        ts.tv_sec = 0;
        ts.tv_nsec = 100000;
        nanosleep(&ts, NULL);
    }

    if( test_abort ){
        TEST_ERROR(("srv #%d: Test was aborted!", my_server_id));
        /* do not simply kill the clients as that generates
         * event notifications which these tests then print
         * out, flooding the log */
      //  cli_kill_all();
        test_fail = 1;
    }

    for(i=0; i < cli_info_cnt; i++){
        if (cli_info[i].exit_code != 0) {
            ++test_fail;
        }
       TEST_VERBOSE(("client %d exit code = %d, test_fail = %d", i, cli_info[i].exit_code, test_fail));
    }

    /* deregister the errhandler */
    PMIx_Deregister_event_handler(0, op_callbk, NULL);

  done:
    TEST_VERBOSE(("srv #%d: call server_finalize!", my_server_id));
    test_fail += server_finalize(&params, test_fail);

    TEST_VERBOSE(("srv #%d: exit sequence!", my_server_id));
    free_params(&params, &val_params);
    pmix_argv_free(client_argv);
    pmix_argv_free(client_env);

    if (0 == test_fail) {
        TEST_OUTPUT(("Test SUCCEEDED!"));
    }
    return test_fail;
}
