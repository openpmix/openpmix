/*
 * Copyright (c) 2015      Intel, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 */

#include "server_callbacks.h"
#include "src/util/argv.h"

pmix_server_module_t mymodule = {
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

PMIX_CLASS_INSTANCE(pmix_test_data_t,
                          pmix_list_item_t,
                          pcon, pdes);

typedef struct {
    pmix_list_item_t super;
    pmix_info_t data;
    char *namespace_published;
} pmix_test_info_t;

static void tcon(pmix_test_info_t *p)
{
    PMIX_INFO_CONSTRUCT(&p->data);
}

static void tdes(pmix_test_info_t *p)
{
    PMIX_INFO_DESTRUCT(&p->data);
}

PMIX_CLASS_INSTANCE(pmix_test_info_t,
                          pmix_list_item_t,
                          tcon, tdes);

pmix_list_t *pmix_test_published_list = NULL;

static int finalized_count = 0;

int finalized(const char nspace[], int rank, void *server_object,
                     pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    if( CLI_TERM <= cli_info[rank].state ){
        TEST_ERROR(("double termination of rank %d", rank));
        return PMIX_SUCCESS;
    }
    TEST_VERBOSE(("Rank %d terminated", rank));
    cli_finalize(&cli_info[rank]);
    finalized_count++;
    if (finalized_count == cli_info_cnt) {
        if (NULL != pmix_test_published_list) {
            PMIX_LIST_RELEASE(pmix_test_published_list);
        }
    }
    if (NULL != cbfunc) {
        cbfunc(PMIX_SUCCESS, cbdata);
    }
    return PMIX_SUCCESS;
}

int abort_fn(const char nspace[], int rank, void *server_object,
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

    TEST_VERBOSE(("gather data: from %d list has %d items",
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

int fencenb_fn(const pmix_range_t ranges[], size_t nranges,
                      int collect_data,
                      pmix_modex_cbfunc_t cbfunc, void *cbdata)
{
    pmix_list_t data;
    size_t i, j;
    pmix_modex_data_t *mdxarray = NULL;
    size_t size=0, n;
    
    /* We need to wait until all the
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

int store_modex_fn(pmix_modex_data_t *data,
                   void *server_object,
                   pmix_scope_t scope)
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

int get_modexnb_fn(const char nspace[], int rank,
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

int publish_fn(pmix_scope_t scope, pmix_persistence_t persist,
                      const pmix_info_t info[], size_t ninfo,
                      pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    size_t i;
    int found;
    pmix_test_info_t *new_info, *old_info;
    if (NULL == pmix_test_published_list) {
        pmix_test_published_list = PMIX_NEW(pmix_list_t);
    }
    for (i = 0; i < ninfo; i++) {
        found = 0;
        PMIX_LIST_FOREACH(old_info, pmix_test_published_list, pmix_test_info_t) {
            if (!strcmp(old_info->data.key, info[i].key)) {
                found = 1;
                break;
            }
        }
        if (!found) {
            new_info = PMIX_NEW(pmix_test_info_t);
            strncpy(new_info->data.key, info[i].key, strlen(info[i].key)+1);
            pmix_value_xfer(&new_info->data.value, &info[i].value);
//            new_info->namespace_published
            pmix_list_append(pmix_test_published_list, &new_info->super);
        }
    }
    if (NULL != cbfunc) {
        cbfunc(PMIX_SUCCESS, cbdata);
    }
    return PMIX_SUCCESS;
}

int lookup_fn(pmix_scope_t scope, int wait, char **keys,
                     pmix_lookup_cbfunc_t cbfunc, void *cbdata)
{
    size_t i, ninfo, ret;
    pmix_info_t *info;
    pmix_test_info_t *tinfo;
    if (NULL == pmix_test_published_list) {
        return PMIX_ERR_NOT_FOUND;
    }
    ninfo = pmix_argv_count(keys);
    PMIX_INFO_CREATE(info, ninfo);
    ret = 0;
    for (i = 0; i < ninfo; i++) {
        PMIX_INFO_CONSTRUCT(&info[i]);
        PMIX_LIST_FOREACH(tinfo, pmix_test_published_list, pmix_test_info_t) {
            if (!strcmp(tinfo->data.key, keys[i])) {
                strncpy(info[i].key, keys[i], strlen(keys[i])+1);
                pmix_value_xfer(&info[i].value, &tinfo->data.value);
                ret++;
                break;
            }
        }
    }
    if (NULL != cbfunc) {
        cbfunc((ret == ninfo) ? PMIX_SUCCESS : PMIX_ERR_NOT_FOUND, info, ninfo, "lalala", cbdata);
    }
    PMIX_INFO_FREE(info, ninfo);
    return PMIX_SUCCESS;
}

int unpublish_fn(pmix_scope_t scope, char **keys,
                        pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    size_t i, ninfo;
    pmix_test_info_t *info, *next;
    if (NULL == pmix_test_published_list) {
        return PMIX_ERR_NOT_FOUND;
    }
    PMIX_LIST_FOREACH_SAFE(info, next, pmix_test_published_list, pmix_test_info_t) {
        if (1) {// if data posted by this process
            if (NULL == keys) {
                pmix_list_remove_item(pmix_test_published_list, &info->super);
                PMIX_RELEASE(info);
            } else {
                ninfo = pmix_argv_count(keys);
                for (i = 0; i < ninfo; i++) {
                    if (!strcmp(info->data.key, keys[i])) {
                        pmix_list_remove_item(pmix_test_published_list, &info->super);
                        PMIX_RELEASE(info);
                        break;
                    }
                }
            }
        }
    }
    if (NULL != cbfunc) {
        cbfunc(PMIX_SUCCESS, cbdata);
    }
    return PMIX_SUCCESS;
}

int spawn_fn(const pmix_app_t apps[], size_t napps,
                    pmix_spawn_cbfunc_t cbfunc, void *cbdata)
{
   if (NULL != cbfunc) {
        cbfunc(PMIX_SUCCESS, "foobar", cbdata);
    }
    return PMIX_SUCCESS;
}

int connect_fn(const pmix_range_t ranges[], size_t nranges,
                      pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    if (NULL != cbfunc) {
        /* return PMIX_EXISTS here just to ensure we get the correct status on the client */
        cbfunc(PMIX_EXISTS, cbdata);
    }
   return PMIX_SUCCESS;
}

int disconnect_fn(const pmix_range_t ranges[], size_t nranges,
                         pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    if (NULL != cbfunc) {
        cbfunc(PMIX_SUCCESS, cbdata);
    }
    return PMIX_SUCCESS;
}

