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

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <event.h>
extern int errno;
#include <errno.h>
#include <time.h>

#include "src/include/pmix_globals.h"
#include "pmix_server.h"
#include "src/class/pmix_list.h"
#include "src/util/argv.h"
#include "src/util/pmix_environ.h"
#include "src/util/output.h"
#include "src/usock/usock.h"

#include "test_common.h"

/* setup the PMIx server module */
static int finalized(const char nspace[], int rank, void *server_object,
                     pmix_op_cbfunc_t cbfunc, void *cbdata);
static int abort_fn(const char nspace[], int rank, void *server_object,
                    int status, const char msg[],
                    pmix_op_cbfunc_t cbfunc, void *cbdata);
static int fencenb_fn(const pmix_range_t ranges[], size_t nranges,
                      int barrier, int collect_data,
                      pmix_modex_cbfunc_t cbfunc, void *cbdata);
static int store_modex_fn(const char nspace[], int rank, void *server_object,
                          pmix_scope_t scope, pmix_modex_data_t *data);
static int get_modexnb_fn(const char nspace[], int rank,
                          pmix_modex_cbfunc_t cbfunc, void *cbdata);
static int publish_fn(pmix_scope_t scope, pmix_persistence_t persist,
                      const pmix_info_t info[], size_t ninfo,
                      pmix_op_cbfunc_t cbfunc, void *cbdata);
static int lookup_fn(pmix_scope_t scope, int wait, char **keys,
                     pmix_lookup_cbfunc_t cbfunc, void *cbdata);
static int unpublish_fn(pmix_scope_t scope, char **keys,
                        pmix_op_cbfunc_t cbfunc, void *cbdata);
static int spawn_fn(const pmix_app_t apps[], size_t napps,
                    pmix_spawn_cbfunc_t cbfunc, void *cbdata);
static int connect_fn(const pmix_range_t ranges[], size_t nranges,
                      pmix_op_cbfunc_t cbfunc, void *cbdata);
static int disconnect_fn(const pmix_range_t ranges[], size_t nranges,
                         pmix_op_cbfunc_t cbfunc, void *cbdata);

static pmix_server_module_t mymodule = {
    finalized,
    abort_fn,
    fencenb_fn,
    store_modex_fn,
    get_modexnb_fn,
    publish_fn,
    lookup_fn,
    unpublish_fn,
    spawn_fn,
    connect_fn,
    disconnect_fn
};

typedef struct {
    pmix_list_item_t super;
    pmix_modex_data_t data;
} pmix_test_data_t;
static void pcon(pmix_test_data_t *p)
{
    p->data.blob = NULL;
    p->data.size = 0;
}
static void pdes(pmix_test_data_t *p)
{
    if (NULL != p->data.blob) {
        free(p->data.blob);
    }
}
static PMIX_CLASS_INSTANCE(pmix_test_data_t,
                          pmix_list_item_t,
                          pcon, pdes);

// In correct scenario each client has to sequentially pass all of this stages
typedef enum {
    CLI_UNINIT, CLI_FORKED, CLI_CONNECTED, CLI_FIN, CLI_DISCONN, CLI_TERM
} cli_state_t;

typedef struct {
    pmix_list_t modex;
    pid_t pid;
    int sd;
    pmix_event_t *ev;
    cli_state_t state;
} cli_info_t;

static cli_info_t *cli_info = NULL;
int cli_info_cnt = 0;

static bool test_abort = false;

static bool barrier = false;
static bool collect = false;
static bool nonblocking = false;
static uint32_t nprocs = 1;
struct event_base *server_base = NULL;
pmix_event_t *listen_ev = NULL;

int listen_fd = -1;
static void connection_handler(int incoming_sd, short flags, void* cbdata);
static void message_handler(int incoming_sd, short flags, void* cbdata);
static int start_listening(struct sockaddr_un *address);

bool verbose = false;

static void errhandler(pmix_status_t status,
                       pmix_range_t ranges[], size_t nranges,
                       pmix_info_t info[], size_t ninfo)
{
    TEST_ERROR(("Error handler with status = %d", status))
    test_abort = true;
}

static int cli_rank(cli_info_t *cli)
{
    int i;
    for(i=0; i < cli_info_cnt; i++){
        if( cli == &cli_info[i] ){
            return i;
        }
    }
    return -1;
}

static void cli_init(int nprocs)
{
    int n;
    cli_info = malloc( sizeof(*cli_info) * nprocs);
    cli_info_cnt = nprocs;

    for (n=0; n < nprocs; n++) {
        cli_info[n].sd = -1;
        cli_info[n].ev = NULL;
        cli_info[n].pid = -1;
        cli_info[n].state = CLI_UNINIT;
        PMIX_CONSTRUCT(&(cli_info[n].modex), pmix_list_t);
    }
}

static void cli_connect(cli_info_t *cli, int sd)
{
    if( CLI_FORKED != cli->state ){
        TEST_ERROR(("Rank %d has bad state: expect %d have %d!",
                     cli_rank(cli), CLI_FORKED, cli->state));
        test_abort = true;
        return;
    }

    cli->sd = sd;
    cli->ev = event_new(server_base, sd,
                      EV_READ|EV_PERSIST, message_handler, cli);
    event_add(cli->ev,NULL);
    pmix_usock_set_nonblocking(sd);
    TEST_OUTPUT(("Connection accepted from rank %d", cli_rank(cli) ));
    cli->state = CLI_CONNECTED;
}

static void cli_finalize(cli_info_t *cli)
{
    if( CLI_CONNECTED != cli->state ){
        TEST_ERROR(("rank %d: bad client state: expect %d have %d!",
                     cli_rank(cli), CLI_CONNECTED, cli->state));
        test_abort = true;
        return;
    }

    cli->state = CLI_FIN;
}

static void cli_disconnect(cli_info_t *cli)
{

    if( CLI_FIN != cli->state ){
        TEST_ERROR(("rank %d: bad client state: expect %d have %d!",
                     cli_rank(cli), CLI_FIN, cli->state));
    }

    if( 0 > cli->sd ){
        TEST_ERROR(("Bad sd = %d of rank = %d ", cli->sd, cli_rank(cli)));
    } else {
        TEST_VERBOSE(("close sd = %d for rank = %d", cli->sd, cli_rank(cli)));
        close(cli->sd);
        cli->sd = -1;
    }

    if( NULL == cli->ev ){
        TEST_ERROR(("Bad ev = NULL of rank = %d ", cli->sd, cli_rank(cli)));
    } else {
        TEST_VERBOSE(("remove event of rank %d from event queue", cli_rank(cli)));
        event_del(cli->ev);
        event_free(cli->ev);
        cli->ev = NULL;
    }

    TEST_VERBOSE(("Destruct modex list for the rank %d", cli_rank(cli)));
    PMIX_LIST_DESTRUCT(&(cli->modex));

    cli->state = CLI_DISCONN;
}

static void cli_terminate(cli_info_t *cli)
{
    if( CLI_DISCONN != cli->state ){
        TEST_ERROR(("rank %d: bad client state: expect %d have %d!",
                     cli_rank(cli), CLI_FIN, cli->state));
    }
    cli->pid = -1;
    TEST_VERBOSE(("Client rank = %d terminated", cli_rank(cli)));
    cli->state = CLI_TERM;
}

static void cli_cleanup(cli_info_t *cli)
{
    switch( cli->state ){
    case CLI_CONNECTED:
        cli_finalize(cli);
    case CLI_FIN:
        cli_disconnect(cli);
    case CLI_DISCONN:
        cli_terminate(cli);
    case CLI_TERM:
    case CLI_FORKED:
        cli->state = CLI_TERM;
    case CLI_UNINIT:
        break;
    default:
        TEST_ERROR(("Bad rank %d state %d", cli_rank(cli), cli->state));
    }
}


static bool test_completed(void)
{
    bool ret = true;
    int i;

    // All of the client should deisconnect
    for(i=0; i < cli_info_cnt; i++){
        ret = ret && (CLI_DISCONN <= cli_info[i].state);
    }
    return (ret || test_abort);
}

static bool test_terminated(void)
{
    bool ret = true;
    int i;

    // All of the client should deisconnect
    for(i=0; i < cli_info_cnt; i++){
        ret = ret && (CLI_TERM <= cli_info[i].state);
    }
    return ret;
}

static void cli_wait_all(double timeout)
{
    struct timeval tv;
    double start_time, cur_time;

    gettimeofday(&tv, NULL);
    start_time = tv.tv_sec + 1E-6*tv.tv_usec;
    cur_time = start_time;

    TEST_VERBOSE(("Wait for all children to terminate"))

    // Wait for all childrens to cleanup after the test.
    while( !test_terminated() && ( timeout >= (cur_time - start_time) ) ){
        struct timespec ts;
        int status, i;
        pid_t pid;
        while( 0 < (pid = waitpid(-1, &status, WNOHANG) ) ){
            TEST_VERBOSE(("waitpid = %d", pid));
            if( pid < 0 ){
                TEST_ERROR(("waitpid(): %d : %s", errno, strerror(errno)));
                if( errno == ECHILD ){
                    TEST_ERROR(("No more childs to wait (shouldn't happen)"));
                    break;
                } else {
                    exit(0);
                }
            } else if( 0 < pid ){
                for(i=0; i < cli_info_cnt; i++){
                    if( cli_info[i].pid == pid ){
                        TEST_VERBOSE(("the child with pid = %d has rank = %d\n"
                                      "\t\texited = %d, signalled = %d", pid, i,
                                      WIFEXITED(status), WIFSIGNALED(status) ));
                        if( WIFEXITED(status) || WIFSIGNALED(status) ){
                            cli_cleanup(&cli_info[i]);
                        }
                    }
                }
            }
        }
        ts.tv_sec = 0;
        ts.tv_nsec = 100000;
        nanosleep(&ts, NULL);
        // calculate current timestamp
        gettimeofday(&tv, NULL);
        cur_time = tv.tv_sec + 1E-6*tv.tv_usec;
    }
}

static void cli_kill_all(void)
{
    int i;
    for(i = 0; i < cli_info_cnt; i++){
        if( CLI_UNINIT == cli_info[i].state ){
            TEST_ERROR(("Skip rank %d as it wasn't ever initialized (shouldn't happe)",
                          i));
            continue;
        } else if( CLI_TERM <= cli_info[i].state ){
            TEST_VERBOSE(("Skip rank %d as it was already terminated.", i));
            continue;

        }
        TEST_VERBOSE(("Kill rank %d (pid = %d).", i, cli_info[i].pid));
        kill(cli_info[i].pid, SIGKILL);
    }
}

static void set_job_info(int nprocs)
{
    pmix_info_t *info;

    info = (pmix_info_t*)malloc(sizeof(pmix_info_t));
    (void)strncpy(info[0].key, PMIX_UNIV_SIZE, PMIX_MAX_KEYLEN);
    info[0].value.type = PMIX_UINT32;
    info[0].value.data.uint32 = nprocs;
    PMIx_server_setup_job(TEST_NAMESPACE,info,1);
    free(info);
}

int main(int argc, char **argv)
{
    char **client_env=NULL;
    char **client_argv=NULL;
    int rc, i;
    uint32_t n;
    char *binary = "pmix_client";
    char *tmp;
    char *np = NULL;
    uid_t myuid;
    gid_t mygid;
    struct sockaddr_un address;
    struct stat stat_buf;
    // In what time test should complete
#define TEST_DEFAULT_TIMEOUT 10
    int test_timeout = TEST_DEFAULT_TIMEOUT;
    struct timeval tv;
    double test_start;
    bool verbose = false;

    gettimeofday(&tv, NULL);
    test_start = tv.tv_sec + 1E-6*tv.tv_usec;

    /* smoke test */
    if (PMIX_SUCCESS != 0) {
        TEST_ERROR(("ERROR IN COMPUTING CONSTANTS: PMIX_SUCCESS = %d\n", PMIX_SUCCESS));
        exit(1);
    }
    
    /* parse user options */
    for (i=1; i < argc; i++) {
        if (0 == strcmp(argv[i], "--n") || 0 == strcmp(argv[i], "-n")) {
            i++;
            np = argv[i];
            nprocs = strtol(argv[i], NULL, 10);
        } else if (0 == strcmp(argv[i], "--h") || 0 == strcmp(argv[i], "-h")) {
            /* print help */
            fprintf(stderr, "usage: pmix_test [-h] [-e foo] [-b] [-c] [-nb]\n");
            fprintf(stderr, "\t-e foo   use foo as test client\n");
            fprintf(stderr, "\t-b       execute fence_nb callback when all procs reach that point\n");
            fprintf(stderr, "\t-c       fence[_nb] callback shall include all collected data\n");
            fprintf(stderr, "\t-nb      use non-blocking fence\n");
            fprintf(stderr, "\t-v       verbose output\n");
            fprintf(stderr, "\t-t       --timeout\n");
            exit(0);
        } else if (0 == strcmp(argv[i], "--exec") || 0 == strcmp(argv[i], "-e")) {
            i++;
            binary = argv[i];
        } else if (0 == strcmp(argv[i], "--barrier") || 0 == strcmp(argv[i], "-b")) {
            barrier = true;
        } else if (0 == strcmp(argv[i], "--collect") || 0 == strcmp(argv[i], "-c")) {
            collect = true;
        } else if (0 == strcmp(argv[i], "--non-blocking") || 0 == strcmp(argv[i], "-nb")) {
            nonblocking = true;
        } else if( 0 == strcmp(argv[i], "--verbose") || 0 == strcmp(argv[i],"-v") ){
            TEST_VERBOSE_ON();
            verbose = true;
        } else if (0 == strcmp(argv[i], "--timeout") || 0 == strcmp(argv[i], "-t")) {
            i++;
            test_timeout = atoi(argv[i]);
            if( test_timeout == 0 ){
                test_timeout = TEST_DEFAULT_TIMEOUT;
            }
        }
        else {
            fprintf(stderr, "unrecognized option: %s\n", argv[i]);
            exit(1);
        }
    }

    TEST_OUTPUT(("Start PMIx_lite smoke test (timeout is %d)", test_timeout));

    /* verify executable */
    if( 0 > ( rc = stat(binary, &stat_buf) ) ){
        TEST_ERROR(("Cannot stat() executable \"%s\": %d: %s", binary, errno, strerror(errno)));
        return 0;
    } else if( !S_ISREG(stat_buf.st_mode) ){
        TEST_ERROR(("Client executable \"%s\": is not a regular file", binary));
        return 0;
    }else if( !(stat_buf.st_mode & S_IXUSR) ){
        TEST_ERROR(("Client executable \"%s\": has no executable flag", binary));
        return 0;
    }

    /* setup the server library */
    if (PMIX_SUCCESS != (rc = PMIx_server_init(&mymodule, false))) {
        TEST_ERROR(("Init failed with error %d\n", rc));
        return rc;
    }
    /* register the errhandler */
    PMIx_Register_errhandler(errhandler);
    set_job_info(nprocs);
        

    /* initialize the event library - we will be providing messaging support
     * for the server */
    if (NULL == (server_base = event_base_new())) {
        TEST_ERROR(("Cannot create libevent event\n"));
        goto cleanup;
    }
    
    /* retrieve the rendezvous address */
    if (PMIX_SUCCESS != PMIx_get_rendezvous_address(&address)) {
        TEST_ERROR(("failed to get rendezvous address"));
        rc = -1;
        goto cleanup;
    }

    TEST_OUTPUT(("Initialization finished"));

    /* start listening */
    if( 0 > (listen_fd = start_listening(&address) ) ){
        TEST_ERROR(("start_listening failed"));
        exit(0);
    }

    client_env = pmix_argv_copy(environ);
    
    /* fork/exec the test */
    pmix_argv_append_nosize(&client_argv, binary);
    if (nonblocking) {
        pmix_argv_append_nosize(&client_argv, "nb");
        if (barrier) {
            pmix_argv_append_nosize(&client_argv, "barrier");
        }
    }
    if (collect) {
        pmix_argv_append_nosize(&client_argv, "collect");
    }
    pmix_argv_append_nosize(&client_argv, "-n");
    if (NULL == np) {
        pmix_argv_append_nosize(&client_argv, "1");
    } else {
        pmix_argv_append_nosize(&client_argv, np);
    }

    if( verbose ){
        pmix_argv_append_nosize(&client_argv, "-v");
    }

    tmp = pmix_argv_join(client_argv, ' ');
    TEST_OUTPUT(("Executing test: %s", tmp));
    free(tmp);

    myuid = getuid();
    mygid = getgid();
    cli_init(nprocs);

    for (n=0; n < nprocs; n++) {
        if (PMIX_SUCCESS != (rc = PMIx_server_setup_fork(TEST_NAMESPACE, n, &client_env))) {
            TEST_ERROR(("Server fork setup failed with error %d", rc));
            PMIx_server_finalize();
            cli_kill_all();
            return rc;
        }
        if (PMIX_SUCCESS != (rc = PMIx_server_register_client(TEST_NAMESPACE, n, myuid, mygid, NULL))) {
            TEST_ERROR(("Server fork setup failed with error %d", rc));
            PMIx_server_finalize();
            cli_kill_all();
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
            return -1;
        }
        
        if (cli_info[n].pid == 0) {
            execve(binary, client_argv, client_env);
            /* Does not return */
	    exit(0);
        }
        cli_info[n].state = CLI_FORKED;
    }

    /* hang around until the client(s) finalize */
    while (!test_completed()) {
        // To avoid test hang we want to interrupt the loop each 0.1s
        struct timeval loop_tv = {0, 100000};
        double test_current;

        // run the processing loop
        event_base_loopexit(server_base, &loop_tv);
        event_base_loop(server_base, EVLOOP_ONCE);

        // check if we exceed the max time
        gettimeofday(&tv, NULL);
        test_current = tv.tv_sec + 1E-6*tv.tv_usec;
        if( (test_current - test_start) > test_timeout ){
            break;
        }
	cli_wait_all(0);
    }

    if( !test_completed() ){
        TEST_ERROR(("Test exited by a timeout!"));
        cli_kill_all();
    }

    if( test_abort ){
        TEST_ERROR(("Test was aborted!"));
        cli_kill_all();
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

    if( !test_terminated() ){
        int i;
        // All of the client should deisconnect
        TEST_ERROR(("Error while cleaning up test. Expect state = %d:", CLI_TERM));
        for(i=0; i < cli_info_cnt; i++){
            TEST_ERROR(("\trank %d, state = %d\n", i, cli_info[i].state));
        }

    } else {
        TEST_OUTPUT(("Test finished OK!"));
    }

    close(listen_fd);
    unlink(address.sun_path);

    return rc;
}

static int finalized(const char nspace[], int rank, void *server_object,
                     pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    if( CLI_TERM <= cli_info[rank].state ){
        TEST_ERROR(("double termination of rank %d", rank));
        return PMIX_SUCCESS;
    }
    TEST_VERBOSE(("Rank %d terminated", rank));
    cli_finalize(&cli_info[rank]);
    if (NULL != cbfunc) {
        cbfunc(PMIX_SUCCESS, cbdata);
    }
    return PMIX_SUCCESS;
}

static int abort_fn(const char nspace[], int rank, void *server_object,
                    int status, const char msg[],
                    pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    if (NULL != cbfunc) {
        cbfunc(PMIX_SUCCESS, cbdata);
    }
    TEST_VERBOSE(("Abort is called with status = %d, msg = %s",
                 status, msg));
    test_abort = true;
    return PMIX_SUCCESS;
}

static void gather_data_rank(const char nspace[], int rank,
                        pmix_list_t *mdxlist)
{
    pmix_test_data_t *tdat, *mdx;

    TEST_VERBOSE(("from %d list has %d items",
                rank, pmix_list_get_size(&cli_info[rank].modex)) );

    PMIX_LIST_FOREACH(mdx, &cli_info[rank].modex, pmix_test_data_t) {
        TEST_VERBOSE(("gather_data: checking %s vs %s",
                     nspace, mdx->data.nspace));
        if (0 != strcmp(nspace, mdx->data.nspace)) {
            continue;
        }
        TEST_VERBOSE(("gather_data: checking %d vs %d",
                     rank, mdx->data.rank));
        if (rank != mdx->data.rank && PMIX_RANK_WILDCARD != rank) {
            continue;
        }
        TEST_VERBOSE(("test:gather_data adding blob for %s:%d of size %d",
                            mdx->data.nspace, mdx->data.rank, (int)mdx->data.size));
        tdat = PMIX_NEW(pmix_test_data_t);
        (void)strncpy(tdat->data.nspace, mdx->data.nspace, PMIX_MAX_NSLEN);
        tdat->data.rank = mdx->data.rank;
        tdat->data.size = mdx->data.size;
        if (0 < mdx->data.size) {
            tdat->data.blob = (uint8_t*)malloc(mdx->data.size);
            memcpy(tdat->data.blob, mdx->data.blob, mdx->data.size);
        }
        pmix_list_append(mdxlist, &tdat->super);
    }
}

static void gather_data(const char nspace[], int rank,
                        pmix_list_t *mdxlist)
{
    if( PMIX_RANK_WILDCARD == rank ){
        int i;
        for(i = 0; i < cli_info_cnt; i++){
            gather_data_rank(nspace, i, mdxlist);
        }
    } else {
        gather_data_rank(nspace, rank, mdxlist);
    }
}

static void xfer_to_array(pmix_list_t *mdxlist,
                          pmix_modex_data_t **mdxarray, size_t *size)
{
    pmix_modex_data_t *mdxa;
    pmix_test_data_t *dat;
    size_t n;

    *size = 0;
    *mdxarray = NULL;
    n = pmix_list_get_size(mdxlist);
    if (0 == n) {
        return;
    }
    /* allocate the array */
    mdxa = (pmix_modex_data_t*)malloc(n * sizeof(pmix_modex_data_t));
    *mdxarray = mdxa;
    *size = n;
    n = 0;
    PMIX_LIST_FOREACH(dat, mdxlist, pmix_test_data_t) {
        (void)strncpy(mdxa[n].nspace, dat->data.nspace, PMIX_MAX_NSLEN);
        mdxa[n].rank = dat->data.rank;
        mdxa[n].size = dat->data.size;
        if (0 < dat->data.size) {
            mdxa[n].blob = (uint8_t*)malloc(dat->data.size);
            memcpy(mdxa[n].blob, dat->data.blob, dat->data.size);
        }
        n++;
    }
}

static int fencenb_fn(const pmix_range_t ranges[], size_t nranges,
                      int barrier, int collect_data,
                      pmix_modex_cbfunc_t cbfunc, void *cbdata)
{
    pmix_list_t data;
    size_t i, j;
    pmix_modex_data_t *mdxarray = NULL;
    size_t size=0, n;
    
    /* if barrier is given, then we need to wait until all the
     * procs have reported prior to responding */

    /* if they want all the data returned, do so */
    if (0 != collect_data) {
        PMIX_CONSTRUCT(&data, pmix_list_t);
        for (i=0; i < nranges; i++) {
            if (NULL == ranges[i].ranks) {
                gather_data(ranges[i].nspace, PMIX_RANK_WILDCARD, &data);
            } else {
                for (j=0; j < ranges[i].nranks; j++) {
                    gather_data(ranges[i].nspace, ranges[i].ranks[j], &data);
                }
            }
        }
        /* xfer the data to the mdx array */
        xfer_to_array(&data, &mdxarray, &size);
        PMIX_LIST_DESTRUCT(&data);
    }
    if (NULL != cbfunc) {
        cbfunc(PMIX_SUCCESS, mdxarray, size, cbdata);
    }
    /* free the array */
    for (n=0; n < size; n++) {
        if (NULL != mdxarray[n].blob) {
            free(mdxarray[n].blob);
        }
    }
    if (NULL != mdxarray) {
        free(mdxarray);
    }
    return PMIX_SUCCESS;
}

static int store_modex_fn(const char nspace[], int rank, void *server_object,
                          pmix_scope_t scope, pmix_modex_data_t *data)
{
    pmix_test_data_t *mdx;

    TEST_VERBOSE(("storing modex data for %s:%d of size %d",
                        data->nspace, data->rank, (int)data->size));
    mdx = PMIX_NEW(pmix_test_data_t);
    (void)strncpy(mdx->data.nspace, data->nspace, PMIX_MAX_NSLEN);
    mdx->data.rank = data->rank;
    mdx->data.size = data->size;
    if (0 < mdx->data.size) {
        mdx->data.blob = (uint8_t*)malloc(mdx->data.size);
        memcpy(mdx->data.blob, data->blob, mdx->data.size);
    }
    pmix_list_append(&cli_info[data->rank].modex, &mdx->super);
    return PMIX_SUCCESS;
}

static int get_modexnb_fn(const char nspace[], int rank,
                          pmix_modex_cbfunc_t cbfunc, void *cbdata)
{
    pmix_list_t data;
    pmix_modex_data_t *mdxarray;
    size_t n, size;
    int rc=PMIX_SUCCESS;

    TEST_VERBOSE(("Getting data for %s:%d", nspace, rank));

    PMIX_CONSTRUCT(&data, pmix_list_t);
    gather_data(nspace, rank, &data);
    /* convert the data to an array */
    xfer_to_array(&data, &mdxarray, &size);
    TEST_VERBOSE(("test:get_modexnb returning %d array blocks", (int)size));

    PMIX_LIST_DESTRUCT(&data);
    if (0 == size) {
        rc = PMIX_ERR_NOT_FOUND;
    }
    if (NULL != cbfunc) {
        cbfunc(rc, mdxarray, size, cbdata);
    }
    /* free the array */
    for (n=0; n < size; n++) {
        if (NULL != mdxarray[n].blob) {
            free(mdxarray[n].blob);
        }
    }
    if (NULL != mdxarray) {
        free(mdxarray);
    }
    return PMIX_SUCCESS;
}

static int publish_fn(pmix_scope_t scope, pmix_persistence_t persist,
                      const pmix_info_t info[], size_t ninfo,
                      pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    if (NULL != cbfunc) {
        cbfunc(PMIX_SUCCESS, cbdata);
    }
    return PMIX_SUCCESS;
}

static int lookup_fn(pmix_scope_t scope, int wait, char **keys,
                     pmix_lookup_cbfunc_t cbfunc, void *cbdata)
{
    if (NULL != cbfunc) {
        cbfunc(PMIX_SUCCESS, NULL, 0, NULL, cbdata);
    }
    return PMIX_SUCCESS;
}

static int unpublish_fn(pmix_scope_t scope, char **keys,
                        pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    if (NULL != cbfunc) {
        cbfunc(PMIX_SUCCESS, cbdata);
    }
    return PMIX_SUCCESS;
}

static int spawn_fn(const pmix_app_t apps[], size_t napps,
                    pmix_spawn_cbfunc_t cbfunc, void *cbdata)
{
   if (NULL != cbfunc) {
        cbfunc(PMIX_SUCCESS, "foobar", cbdata);
    }
    return PMIX_SUCCESS;
}

static int connect_fn(const pmix_range_t ranges[], size_t nranges,
                      pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    if (NULL != cbfunc) {
        cbfunc(PMIX_SUCCESS, cbdata);
    }
   return PMIX_SUCCESS;
}

static int disconnect_fn(const pmix_range_t ranges[], size_t nranges,
                         pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    if (NULL != cbfunc) {
        cbfunc(PMIX_SUCCESS, cbdata);
    }
    return PMIX_SUCCESS;
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
        TEST_OUTPUT(("socket() failed"));
        return -1;
    }

    addrlen = sizeof(struct sockaddr_un);
    if (bind(listen_fd, (struct sockaddr*)address, addrlen) < 0) {
        TEST_OUTPUT(("bind() failed"));
        return -1;
    }

    /* setup listen backlog to maximum allowed by kernel */
    if (listen(listen_fd, SOMAXCONN) < 0) {
        TEST_OUTPUT(("listen() failed"));
        return -1;
    }
        
    /* set socket up to be non-blocking, otherwise accept could block */
    if ((flags = fcntl(listen_fd, F_GETFL, 0)) < 0) {
        TEST_OUTPUT(("fcntl(F_GETFL) failed"));
        return -1;
    }
    flags |= O_NONBLOCK;
    if (fcntl(listen_fd, F_SETFL, flags) < 0) {
        TEST_OUTPUT(("fcntl(F_SETFL) failed"));
        return -1;
    }

    /* setup to listen via the event lib */
    listen_ev = event_new(server_base, listen_fd,
                          EV_READ|EV_PERSIST, connection_handler, 0);
    event_add(listen_ev, 0);
    TEST_OUTPUT(("Server is listening for incoming connections"));
    return 0;
}

static void snd_ack(int sd, char *payload, size_t size)
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
    if (PMIX_SUCCESS != (rc = PMIx_server_authenticate_client(sd, snd_ack))) {
        TEST_ERROR(("PMIx srv: Bad authentification!"));
        test_abort = true;
        return;
    }

    // cli_connect(&cli_info[rank], sd);
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
