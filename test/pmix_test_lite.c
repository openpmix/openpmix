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
 * Copyright (c) 2015      Artem Y. Polyakov <artpol84@gmail.com>.
 *                         All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 */

#include <stdio.h>
#include "src/util/argv.h"
#include "src/util/pmix_environ.h"
#include "src/util/output.h"

#include "server_callbacks.h"
#include "utils.h"

struct event_base *server_base = NULL;
pmix_event_t *listen_ev = NULL;

int listen_fd = -1;
static void connection_handler(int incoming_sd, short flags, void* cbdata);
static void message_handler(int incoming_sd, short flags, void* cbdata);
static int start_listening(struct sockaddr_un *address);

int main(int argc, char **argv)
{
    char **client_env=NULL;
    char **client_argv=NULL;
    int rc;
    uint32_t n;
    char *tmp;
    uid_t myuid;
    gid_t mygid;
    struct sockaddr_un address;
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
        TEST_ERROR(("ERROR IN COMPUTING CONSTANTS: PMIX_SUCCESS = %d\n", PMIX_SUCCESS));
        exit(1);
    }

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
    if (PMIX_SUCCESS != (rc = PMIx_server_init(&mymodule, false))) {
        TEST_ERROR(("Init failed with error %d\n", rc));
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

    /* initialize the event library - we will be providing messaging support
     * for the server */
    if (NULL == (server_base = event_base_new())) {
        TEST_ERROR(("Cannot create libevent event\n"));
        goto cleanup;
    }

    /* retrieve the rendezvous address */
    if (PMIX_SUCCESS != PMIx_get_rendezvous_address(&address, NULL)) {
        TEST_ERROR(("failed to get rendezvous address"));
        rc = -1;
        goto cleanup;
    }

    TEST_VERBOSE(("Initialization finished"));

    /* start listening */
    if( 0 > (listen_fd = start_listening(&address) ) ){
        TEST_ERROR(("start_listening failed"));
        exit(0);
    }

    client_env = pmix_argv_copy(environ);

    /* fork/exec the test */
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
    order[CLI_FORKED] = CLI_CONNECTED;
    order[CLI_CONNECTED] = CLI_FIN;
    order[CLI_FIN] = CLI_DISCONN;
    order[CLI_DISCONN] = CLI_TERM;
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

//        for (i=0; NULL != client_env[i]; i++) {
//            TEST_VERBOSE(("env[%d]: %s", i, client_env[i]));
//        }

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
        struct timeval loop_tv = {0, 100000};
        double test_current;

        // run the processing loop
        event_base_loopexit(server_base, &loop_tv);
        event_base_loop(server_base, EVLOOP_ONCE);

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


    cleanup:

    cli_wait_all(1.0);

    /* finalize the server library */
    if (PMIX_SUCCESS != (rc = PMIx_server_finalize())) {
        TEST_ERROR(("Finalize failed with error %d", rc));
    }

    if (0 == test_fail) {
        TEST_OUTPUT(("Test finished OK!"));
    }

    close(listen_fd);
    unlink(address.sun_path);

    FREE_TEST_PARAMS(params);
    return rc;
}

/*
 * start listening on our rendezvous file
 */
static int start_listening(struct sockaddr_un *address)
{
    int flags;
    unsigned int addrlen;

    /* create a listen socket for incoming connection attempts */
    listen_fd = socket(PF_UNIX, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        TEST_ERROR(("socket() failed"));
        return -1;
    }

    addrlen = sizeof(struct sockaddr_un);
    if (bind(listen_fd, (struct sockaddr*)address, addrlen) < 0) {
        TEST_ERROR(("bind() failed"));
        return -1;
    }

    /* setup listen backlog to maximum allowed by kernel */
    if (listen(listen_fd, SOMAXCONN) < 0) {
        TEST_ERROR(("listen() failed"));
        return -1;
    }

    /* set socket up to be non-blocking, otherwise accept could block */
    if ((flags = fcntl(listen_fd, F_GETFL, 0)) < 0) {
        TEST_ERROR(("fcntl(F_GETFL) failed"));
        return -1;
    }
    flags |= O_NONBLOCK;
    if (fcntl(listen_fd, F_SETFL, flags) < 0) {
        TEST_ERROR(("fcntl(F_SETFL) failed"));
        return -1;
    }

    /* setup to listen via the event lib */
    listen_ev = event_new(server_base, listen_fd,
                          EV_READ|EV_PERSIST, connection_handler, 0);
    event_add(listen_ev, 0);
    TEST_VERBOSE(("Server is listening for incoming connections"));
    return 0;
}

static void snd_ack(int sd, void *server_obj, char *payload, size_t size)
{
    /* the call to authenticate_client will have included
     * the result of the handshake - so collect add job-related
     * info and send it down */
    pmix_usock_send_blocking(sd, payload, size);
}

/*
 * Handler for accepting connections from the event library
 */
static void connection_handler(int incomind_sd, short flags, void* cbdata)
{
    int rc, sd;
    int rank;

    TEST_VERBOSE(("Incoming connection from the client"));

    if( 0 > (sd = accept(incomind_sd,NULL,0)) ){
        TEST_ERROR(("accept() failed %d:%s!", errno, strerror(errno) ));
        test_abort = true;
        return;
    }

    /* authenticate the connection */
    if (PMIX_SUCCESS != (rc = PMIx_server_authenticate_client(sd, &rank, snd_ack))) {
        TEST_ERROR(("PMIx srv: Bad authentification!"));
        test_abort = true;
        return;
    }

    cli_connect(&cli_info[rank], sd, server_base, message_handler);
}


static void message_handler(int sd, short flags, void* cbdata)
{
    char *hdr, *payload = NULL;
    size_t hdrsize, paysize;
    cli_info_t *item = (cli_info_t *)cbdata;

    TEST_VERBOSE(("Message from sd = %d, rank = %d", item->sd, cli_rank(item)) );

    // sanity check, shouldn't happen
    if( item->sd != sd ){
        TEST_ERROR(("Rank %d file descriptor mismatch: saved = %d, real = %d",
                cli_rank(item), item->sd, sd ));
        test_abort = true;
        return;
    }

    // Restore blocking mode for the time of receive
    pmix_usock_set_blocking(sd);

    hdrsize = PMIx_message_hdr_size();
    hdr = (char*)malloc(hdrsize);
    if (PMIX_SUCCESS != pmix_usock_recv_blocking(sd, hdr, hdrsize)) {
        cli_disconnect(item);
        return;
    }

    /* if this is a zero-byte message, then we are done */
    paysize = PMIx_message_payload_size(hdr);
    TEST_VERBOSE(("rank %d: header received, payload size is %d",
                  cli_rank(item), paysize ));

    if (0 < paysize) {
        payload = (char*)malloc(paysize);
        if (PMIX_SUCCESS != pmix_usock_recv_blocking(sd, payload, paysize)) {
            /* Communication error */
            TEST_ERROR(("Rank %d closed connection in the middle of the transmission",
                        cli_rank(item) ));
            test_abort = true;
            return;
        }
    }

    pmix_usock_set_nonblocking(sd);

    TEST_VERBOSE(("rank %d: payload received", cli_rank(item)));

    /* process the message */
    if (PMIX_SUCCESS != PMIx_server_process_msg(sd, hdr, payload, snd_ack)) {
        /* Communication error */
        TEST_ERROR(("Cannot process message from the rank %d",
                    cli_rank(item) ));
        test_abort = true;
    }
    TEST_VERBOSE(("rank %d: message processed", cli_rank(item)));
}
