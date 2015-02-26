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

#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <event.h>
#include <errno.h>

#include "src/include/pmix_globals.h"
#include "pmix_server.h"
#include "src/class/pmix_list.h"
#include "src/util/argv.h"
#include "src/util/pmix_environ.h"
#include "src/util/output.h"

#include "test_common.h"

/* setup the PMIx server module */
static int terminated(const char nspace[], int rank);
static int abort_fn(const char nspace[], int rank,
                    int status, const char msg[],
                    pmix_op_cbfunc_t cbfunc, void *cbdata);
static int fencenb_fn(const pmix_range_t ranges[], size_t nranges,
                      int barrier, int collect_data,
                      pmix_modex_cbfunc_t cbfunc, void *cbdata);
static int store_modex_fn(const char nspace[], int rank,
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
    terminated,
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

static bool test_complete = false;
static pmix_list_t modex;
static bool barrier = false;
static bool collect = false;
static bool nonblocking = false;
static uint32_t nprocs = 1;

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
    char *binary = "pmix_client";
    char *tmp;
    char *np = NULL;
    uid_t myuid;
    gid_t mygid;
    struct stat stat_buf;
    bool verbose = false;

    /* smoke test */
    if (PMIX_SUCCESS != 0) {
        TEST_ERROR(("ERROR IN COMPUTING CONSTANTS: PMIX_SUCCESS = %d", PMIX_SUCCESS));
        exit(1);
    }

    TEST_OUTPUT(("Testing version %s", PMIx_Get_version()));
    
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
        } else if (0 == strcmp(argv[i], "--verbose") || 0 == strcmp(argv[i], "-v")) {
            TEST_VERBOSE_ON();
            verbose = true;
        }else {
            fprintf(stderr, "unrecognized option: %s\n", argv[i]);
            exit(1);
        }
    }

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

    PMIX_CONSTRUCT(&modex, pmix_list_t);
    
    /* setup the server library */
    if (PMIX_SUCCESS != (rc = PMIx_server_init(&mymodule, true))) {
        TEST_ERROR(("Init failed with error %d", rc));
        return rc;
    }
    /* register the errhandler */
    PMIx_Register_errhandler(errhandler);
    
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
    for (n=0; n < nprocs; n++) {
        if (PMIX_SUCCESS != (rc = PMIx_server_setup_fork(TEST_NAMESPACE, n, myuid, mygid, &client_env))) {
            TEST_ERROR(("Server fork setup failed with error %d", rc));
            PMIx_server_finalize();
            return rc;
        }
    
        pid = fork();    
        if (pid < 0) {
            TEST_ERROR(("Fork failed"));
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
        usleep(10000);
    }
    
    pmix_argv_free(client_argv);
    pmix_argv_free(client_env);
    
    /* deregister the errhandler */
    PMIx_Deregister_errhandler();

    PMIX_LIST_DESTRUCT(&modex);
    /* finalize the server library */
    if (PMIX_SUCCESS != (rc = PMIx_server_finalize())) {
        TEST_ERROR(("Finalize failed with error %d", rc));
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

static int abort_fn(const char nspace[], int rank,
                    int status, const char msg[],
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

    TEST_VERBOSE(("gather_data: list has %d items", (int)pmix_list_get_size(&modex)));
    
    PMIX_LIST_FOREACH(mdx, &modex, pmix_test_data_t) {
        TEST_VERBOSE(("gather_data: checking %s vs %s", nspace, mdx->data.nspace));
        if (0 != strcmp(nspace, mdx->data.nspace)) {
            continue;
        }
        TEST_VERBOSE(("gather_data: checking %d vs %d", rank, mdx->data.rank));
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

static int store_modex_fn(const char nspace[], int rank,
                          pmix_scope_t scope, pmix_modex_data_t *data)
{
    pmix_test_data_t *mdx;

    TEST_VERBOSE(("test: storing modex data for %s:%d of size %d",
                  data->nspace, data->rank, (int)data->size));
    
    mdx = PMIX_NEW(pmix_test_data_t);
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
    
    TEST_VERBOSE(("Getting data for %s:%d", nspace, rank));

    PMIX_CONSTRUCT(&data, pmix_list_t);
    gather_data(nspace, rank, &data);
    /* convert the data to an array */
    xfer_to_array(&data, &mdxarray, &size);
    TEST_VERBOSE(("test:get_modexnb returning %d array blocks",(int)size));
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

