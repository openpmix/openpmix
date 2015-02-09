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
#include <sys/types.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <event.h>
#include <errno.h>

#include "src/include/pmix_globals.h"
#include "pmix_server.h"
#include "src/class/pmix_list.h"
#include "src/util/argv.h"
#include "src/util/pmix_environ.h"
#include "src/util/output.h"
#include "src/usock/usock.h"

#include "test_common.h"

/* setup the PMIx server module */
static int terminated(const char nspace[], int rank);
static int abort_fn(int status, const char msg[],
                    pmix_op_cbfunc_t cbfunc, void *cbdata);
static int fencenb_fn(const pmix_range_t ranges[], size_t nranges,
                      int barrier, int collect_data,
                      pmix_modex_cbfunc_t cbfunc, void *cbdata);
static int store_modex_fn(pmix_scope_t scope, pmix_modex_data_t *data);
static int get_modexnb_fn(const char nspace[], int rank,
                          pmix_modex_cbfunc_t cbfunc, void *cbdata);
static int get_job_info_fn(const char nspace[], int rank,
                           pmix_info_t *info[], size_t *ninfo);
static int publish_fn(pmix_scope_t scope, const pmix_info_t info[], size_t ninfo,
                      pmix_op_cbfunc_t cbfunc, void *cbdata);
static int lookup_fn(pmix_scope_t scope, char **keys,
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
    terminated,
    abort_fn,
    fencenb_fn,
    store_modex_fn,
    get_modexnb_fn,
    get_job_info_fn,
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
static OBJ_CLASS_INSTANCE(pmix_test_data_t,
                          pmix_list_item_t,
                          pcon, pdes);

static bool test_complete = false;
static pmix_list_t modex;
static bool barrier = false;
static bool collect = false;
static bool nonblocking = false;
static uint32_t nprocs = 1;
struct event_base *server_base = NULL;
pmix_event_t *listen_ev = NULL;
pmix_event_t *cli_ev = NULL;
int listen_fd = -1;
int client_fd = -1;
pid_t client_pid = -1;

static void connection_handler(int incoming_sd, short flags, void* cbdata);
static void message_handler(int incoming_sd, short flags, void* cbdata);
static int start_listening(struct sockaddr_un *address);

static void errhandler(pmix_status_t status,
                       pmix_range_t ranges[], size_t nranges,
                       pmix_info_t info[], size_t ninfo)
{
    --nprocs;
    if (nprocs <= 0) {
        test_complete = true;
    }
}

int main(int argc, char **argv)
{
    char **client_env=NULL;
    char **client_argv=NULL;
    int rc, i;
    uint32_t n;
    pid_t pid;
    char *binary = "pmix_client2";
    char *tmp;
    char *np = NULL;
    uid_t myuid;
    gid_t mygid;
    struct sockaddr_un address;

    /* smoke test */
    if (PMIX_SUCCESS != 0) {
        fprintf(stderr, "ERROR IN COMPUTING CONSTANTS: PMIX_SUCCESS = %d\n", PMIX_SUCCESS);
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
        } else {
            fprintf(stderr, "unrecognized option: %s\n", argv[i]);
            exit(1);
        }
    }

    OBJ_CONSTRUCT(&modex, pmix_list_t);
    
    fprintf(stderr,"Start PMIx_lite smoke test\n");

    /* setup the server library */
    if (PMIX_SUCCESS != (rc = PMIx_server_init(&mymodule, false))) {
        fprintf(stderr, "Init failed with error %d\n", rc);
        return rc;
    }
    /* register the errhandler */
    PMIx_Register_errhandler(errhandler);
        

    /* initialize the event library - we will be providing messaging support
     * for the server */
    if (NULL == (server_base = event_base_new())) {
        printf("Cannot create libevent event\n");
        goto cleanup;
    }
    
    /* retrieve the rendezvous address */
    if (PMIX_SUCCESS != PMIx_get_rendezvous_address(&address)) {
        fprintf(stderr, "%s: failed to get rendezvous address\n", argv[0]);
        rc = -1;
        goto cleanup;
    }

    fprintf(stderr,"PMIx srv: Initialization finished\n");

    /* start listening */
    listen_fd = start_listening(&address);

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
    
    tmp = pmix_argv_join(client_argv, ' ');
    fprintf(stderr, "Executing test: %s\n", tmp);
    free(tmp);

    myuid = getuid();
    mygid = getgid();
    for (n=0; n < nprocs; n++) {
        if (PMIX_SUCCESS != (rc = PMIx_server_setup_fork(TEST_NAMESPACE, n, myuid, mygid, &client_env))) {
            fprintf(stderr, "Server fork setup failed with error %d\n", rc);
            PMIx_server_finalize();
            return rc;
        }
        for (i=0; NULL != client_env[i]; i++) {
            pmix_output(0, "env[%d]: %s", i, client_env[i]);
        }
    
        pid = fork();    
        if (pid < 0) {
            fprintf(stderr, "Fork failed\n");
            PMIx_server_finalize();
            return -1;
        }
        
        if (pid == 0) {
            execve(binary, client_argv, client_env);
            /* Does not return */
        }
    }

    /* hang around until the client(s) finalize */
    while (!test_complete) {
        event_base_loop(server_base, EVLOOP_ONCE);
    }
    
    pmix_argv_free(client_argv);
    pmix_argv_free(client_env);
    
    /* deregister the errhandler */
    PMIx_Deregister_errhandler();

    PMIX_LIST_DESTRUCT(&modex);
    cleanup:
    /* finalize the server library */
    if (PMIX_SUCCESS != (rc = PMIx_server_finalize())) {
        fprintf(stderr, "Finalize failed with error %d\n", rc);
    }
    
    return rc;
}

static int terminated(const char nspace[], int rank)
{
    --nprocs;
    if (nprocs <= 0) {
        test_complete = true;
    }
    return PMIX_SUCCESS;
}

static int abort_fn(int status, const char msg[],
                    pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    if (NULL != cbfunc) {
        cbfunc(PMIX_SUCCESS, cbdata);
    }
    return PMIX_SUCCESS;
}

static void gather_data(const char nspace[], int rank,
                        pmix_list_t *mdxlist)
{
    pmix_test_data_t *tdat, *mdx;

    pmix_output(0, "gather_data: list has %d items", pmix_list_get_size(&modex));
    
    PMIX_LIST_FOREACH(mdx, &modex, pmix_test_data_t) {
        pmix_output(0, "gather_data: checking %s vs %s", nspace, mdx->data.nspace);
        if (0 != strcmp(nspace, mdx->data.nspace)) {
            continue;
        }
        pmix_output(0, "gather_data: checking %d vs %d", rank, mdx->data.rank);
        if (rank != mdx->data.rank && PMIX_RANK_WILDCARD != rank) {
            continue;
        }
        pmix_output_verbose(5, pmix_globals.debug_output,
                            "test:gather_data adding blob for %s:%d of size %d",
                            mdx->data.nspace, mdx->data.rank, (int)mdx->data.size);
        tdat = OBJ_NEW(pmix_test_data_t);
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
        OBJ_CONSTRUCT(&data, pmix_list_t);
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

static int store_modex_fn(pmix_scope_t scope, pmix_modex_data_t *data)
{
    pmix_test_data_t *mdx;

    pmix_output_verbose(5, pmix_globals.debug_output,
                        "test: storing modex data for %s:%d of size %d",
                        data->nspace, data->rank, (int)data->size);
    
    mdx = OBJ_NEW(pmix_test_data_t);
    (void)strncpy(mdx->data.nspace, data->nspace, PMIX_MAX_NSLEN);
    mdx->data.rank = data->rank;
    mdx->data.size = data->size;
    if (0 < mdx->data.size) {
        mdx->data.blob = (uint8_t*)malloc(mdx->data.size);
        memcpy(mdx->data.blob, data->blob, mdx->data.size);
    }
    pmix_list_append(&modex, &mdx->super);
    return PMIX_SUCCESS;
}

static int get_modexnb_fn(const char nspace[], int rank,
                          pmix_modex_cbfunc_t cbfunc, void *cbdata)
{
    pmix_list_t data;
    pmix_modex_data_t *mdxarray;
    size_t n, size;
    int rc=PMIX_SUCCESS;
    
    pmix_output(0, "Getting data for %s:%d", nspace, rank);

    OBJ_CONSTRUCT(&data, pmix_list_t);
    gather_data(nspace, rank, &data);
    /* convert the data to an array */
    xfer_to_array(&data, &mdxarray, &size);
    pmix_output_verbose(5, pmix_globals.debug_output,
                        "test:get_modexnb returning %d array blocks",
                        (int)size);
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

static int get_job_info_fn(const char nspace[], int rank,
                           pmix_info_t *info[], size_t *ninfo)
{
    pmix_info_t *resp;

    resp = (pmix_info_t*)malloc(sizeof(pmix_info_t));
    (void)strncpy(resp[0].key, PMIX_UNIV_SIZE, PMIX_MAX_KEYLEN);
    resp[0].value.type = PMIX_UINT32;
    resp[0].value.data.uint32 = nprocs;

    *info = resp;
    *ninfo = 1;
    return PMIX_SUCCESS;
}

static int publish_fn(pmix_scope_t scope, const pmix_info_t info[], size_t ninfo,
                      pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    if (NULL != cbfunc) {
        cbfunc(PMIX_SUCCESS, cbdata);
    }
    return PMIX_SUCCESS;
}

static int lookup_fn(pmix_scope_t scope, char **keys,
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
        printf("%s:%d socket() failed", __FILE__, __LINE__);
        return -1;
    }

    addrlen = sizeof(struct sockaddr_un);
    if (bind(listen_fd, (struct sockaddr*)address, addrlen) < 0) {
        printf("%s:%d bind() failed", __FILE__, __LINE__);
        return -1;
    }
        
    /* setup listen backlog to maximum allowed by kernel */
    if (listen(listen_fd, SOMAXCONN) < 0) {
        printf("%s:%d listen() failed", __FILE__, __LINE__);
        return -1;
    }
        
    /* set socket up to be non-blocking, otherwise accept could block */
    if ((flags = fcntl(listen_fd, F_GETFL, 0)) < 0) {
        printf("%s:%d fcntl(F_GETFL) failed", __FILE__, __LINE__);
        return -1;
    }
    flags |= O_NONBLOCK;
    if (fcntl(listen_fd, F_SETFL, flags) < 0) {
        printf("%s:%d fcntl(F_SETFL) failed", __FILE__, __LINE__);
        return -1;
    }

    /* setup to listen via the event lib */
    listen_ev = event_new(server_base, listen_fd,
                          EV_READ|EV_PERSIST, connection_handler, 0);
    event_add(listen_ev, 0);
    fprintf(stderr, "PMIx srv: Server is listening for incoming connections\n");
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

    fprintf(stderr, "PMIx srv: Incoming connection from the client\n");

    if( 0 > (sd = accept(incomind_sd,NULL,0)) ){
        fprintf(stderr, "PMIx srv: %s:%d accept() failed %d:%s",
                __FILE__, __LINE__, errno, strerror(errno));
        exit(0);
    }

    /* authenticate the connection */
    if (PMIX_SUCCESS != (rc = PMIx_server_authenticate_client(sd, &rank, snd_ack))) {
        printf("PMIx srv: Bad authentification!\n");
        exit(0);
    }

    cli_ev = event_new(server_base, sd,
                      EV_READ|EV_PERSIST, message_handler, NULL);
    event_add(cli_ev,NULL);
    pmix_usock_set_nonblocking(sd);
    printf("PMIx srv: New client connection accepted\n");
}


static void message_handler(int sd, short flags, void* cbdata)
{
    char *hdr, *payload = NULL;
    size_t hdrsize, paysize;

    hdrsize = PMIx_message_hdr_size();
    hdr = (char*)malloc(hdrsize);
    
    if (PMIX_SUCCESS != pmix_usock_recv_blocking(sd, hdr, hdrsize)) {
        /* Communication error */
        fprintf(stderr,"PMIx srv: Client closed connection\n");
        event_del(cli_ev);
        event_free(cli_ev);
        close(sd);
        return;
    }

    /* if this is a zero-byte message, then we are done */
    paysize = PMIx_message_payload_size(hdr);
    if (0 < paysize) {
        payload = (char*)malloc(paysize);
        if (PMIX_SUCCESS != pmix_usock_recv_blocking(sd, payload, paysize)) {
            /* Communication error */
            fprintf(stderr,"PMIx srv: Client closed connection\n");
            event_del(cli_ev);
            event_free(cli_ev);
            close(sd);
            return;
        }
    }

    /* process the message */
    if (PMIX_SUCCESS != PMIx_server_process_msg(sd, hdr, payload, snd_ack)) {
        /* Communication error */
        fprintf(stderr,"PMIx srv: Client closed connection\n");
        event_del(cli_ev);
        event_free(cli_ev);
        close(sd);
    }
}
