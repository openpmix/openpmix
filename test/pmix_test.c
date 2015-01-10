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
#include <unistd.h>
#include <sys/types.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <event.h>

#include "pmix_server.h"
#include "src/class/pmix_list.h"
#include "src/util/argv.h"
#include "src/util/pmix_environ.h"
#include "src/util/output.h"

#include "test_common.h"

/* setup the PMIx server module */
static int authenticate(char *credential);
static int terminated(const char namespace[], int rank);
static int abort_fn(int status, const char msg[]);
static int fencenb_fn(const pmix_range_t ranges[], size_t nranges,
                      int barrier, int collect_data,
                      pmix_modex_cbfunc_t cbfunc, void *cbdata);
static int store_modex_fn(pmix_scope_t scope, pmix_modex_data_t *data);
static int get_modexnb_fn(const char namespace[], int rank,
                          pmix_modex_cbfunc_t cbfunc, void *cbdata);
static int get_job_info_fn(const char namespace[], int rank,
                           pmix_info_t *info[], size_t *ninfo);
static int publish_fn(pmix_scope_t scope, const pmix_info_t info[], size_t ninfo);
static int lookup_fn(pmix_scope_t scope,
                     pmix_info_t info[], size_t ninfo,
                     char *namespace[]);
static int unpublish_fn(pmix_scope_t scope, char **keys);
static int spawn_fn(const pmix_app_t apps[],
                    size_t napps,
                    pmix_spawn_cbfunc_t cbfunc, void *cbdata);
static int connect_fn(const pmix_range_t ranges[], size_t nranges,
                      pmix_connect_cbfunc_t cbfunc, void *cbdata);
static int disconnect_fn(const pmix_range_t ranges[], size_t nranges,
                         pmix_connect_cbfunc_t cbfunc, void *cbdata);

static pmix_server_module_t mymodule = {
    authenticate,
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

static void errhandler(int error)
{
    test_complete = true;
}

int main(int argc, char **argv)
{
    char **client_env=NULL;
    char **client_argv=NULL;
    int rc, i;
    pid_t pid;
    int nprocs=1;
    char *binary = "pmix_client2";
    char *tmp;
    char *np = NULL;
    
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
    
    /* setup the server library */
    if (PMIX_SUCCESS != (rc = PMIx_server_init(&mymodule, NULL, NULL, "1234"))) {
        fprintf(stderr, "Init failed with error %d\n", rc);
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
    
    tmp = pmix_argv_join(client_argv, ' ');
    fprintf(stderr, "Executing test: %s\n", tmp);
    free(tmp);

    for (i=0; i < nprocs; i++) {
        if (PMIX_SUCCESS != (rc = PMIx_server_setup_fork(TEST_NAMESPACE, i, &client_env))) {
            fprintf(stderr, "Server fork setup failed with error %d\n", rc);
            PMIx_server_finalize();
            return rc;
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

    /* hang around until the client finalizes */
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
        fprintf(stderr, "Finalize failed with error %d\n", rc);
    }
    
    return rc;
}

static int authenticate(char *credential)
{
    if (0 == strcmp(credential, TEST_CREDENTIAL)) {
        return PMIX_SUCCESS;
    }
    return PMIX_ERROR;
}

static int terminated(const char namespace[], int rank)
{
    test_complete = true;
    return PMIX_SUCCESS;
}

static int abort_fn(int status, const char msg[])
{
    return PMIX_SUCCESS;
}

static void gather_data(const char namespace[], int rank,
                        pmix_list_t *mdxlist)
{
    pmix_test_data_t *tdat, *mdx;

    PMIX_LIST_FOREACH(mdx, &modex, pmix_test_data_t) {
        if (0 != strcmp(namespace, mdx->data.namespace)) {
            continue;
        }
        if (rank != mdx->data.rank || PMIX_RANK_WILDCARD == rank) {
            continue;
        }
        tdat = OBJ_NEW(pmix_test_data_t);
        (void)strncpy(tdat->data.namespace, mdx->data.namespace, PMIX_MAX_NSLEN);
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
        (void)strncpy(mdxa[n].namespace, dat->data.namespace, PMIX_MAX_NSLEN);
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
                gather_data(ranges[i].namespace, UINT_MAX, &data);
            } else {
                for (j=0; j < ranges[i].nranks; j++) {
                    gather_data(ranges[i].namespace, ranges[i].ranks[j], &data);
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

    pmix_output(0, "Storing data for %s:%d", data->namespace, data->rank);
    
    mdx = OBJ_NEW(pmix_test_data_t);
    (void)strncpy(mdx->data.namespace, data->namespace, PMIX_MAX_NSLEN);
    mdx->data.rank = data->rank;
    mdx->data.size = data->size;
    if (0 < mdx->data.size) {
        mdx->data.blob = (uint8_t*)malloc(mdx->data.size);
        memcpy(mdx->data.blob, data->blob, mdx->data.size);
    }
    pmix_list_append(&modex, &mdx->super);
    return PMIX_SUCCESS;
}

static int get_modexnb_fn(const char namespace[], int rank,
                          pmix_modex_cbfunc_t cbfunc, void *cbdata)
{
    pmix_list_t data;
    pmix_modex_data_t *mdxarray;
    size_t n, size;

    pmix_output(0, "Getting data for %s:%d", namespace, rank);

    OBJ_CONSTRUCT(&data, pmix_list_t);
    gather_data(namespace, rank, &data);
    /* convert the data to an array */
    xfer_to_array(&data, &mdxarray, &size);
    PMIX_LIST_DESTRUCT(&data);
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

static int get_job_info_fn(const char namespace[], int rank,
                           pmix_info_t *info[], size_t *ninfo)
{
    *info = NULL;
    *ninfo = 0;
    return PMIX_SUCCESS;
}

static int publish_fn(pmix_scope_t scope, const pmix_info_t info[], size_t ninfo)
{
    return PMIX_SUCCESS;
}

static int lookup_fn(pmix_scope_t scope,
                     pmix_info_t info[], size_t ninfo,
                     char *namespace[])
{
    *namespace = NULL;
    return PMIX_SUCCESS;
}

static int unpublish_fn(pmix_scope_t scope, char **keys)
{
    return PMIX_SUCCESS;
}

static int spawn_fn(const pmix_app_t apps[],
                    size_t napps,
                    pmix_spawn_cbfunc_t cbfunc, void *cbdata)
{
   if (NULL != cbfunc) {
        cbfunc(PMIX_SUCCESS, "foobar", cbdata);
    }
    return PMIX_SUCCESS;
}

static int connect_fn(const pmix_range_t ranges[], size_t nranges,
                      pmix_connect_cbfunc_t cbfunc, void *cbdata)
{
    if (NULL != cbfunc) {
        cbfunc(PMIX_SUCCESS, cbdata);
    }
   return PMIX_SUCCESS;
}

static int disconnect_fn(const pmix_range_t ranges[], size_t nranges,
                         pmix_connect_cbfunc_t cbfunc, void *cbdata)
{
    if (NULL != cbfunc) {
        cbfunc(PMIX_SUCCESS, cbdata);
    }
    return PMIX_SUCCESS;
}

