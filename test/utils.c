#include "utils.h"
#include "test_common.h"
#include "pmix_server.h"

void fill_seq_ranks_array(size_t nprocs, char **ranks)
{
    uint32_t i;
    int len = 0, max_ranks_len;
    max_ranks_len = nprocs * (MAX_DIGIT_LEN+1);
    *ranks = (char*) malloc(max_ranks_len);
    for (i = 0; i < nprocs; i++) {
        len += snprintf(*ranks + len, max_ranks_len-len-1, "%d", i);
        if (i != nprocs-1) {
            len += snprintf(*ranks + len, max_ranks_len-len-1, "%c", ',');
        }
    }
    if (len >= max_ranks_len-1) {
        free(*ranks);
        *ranks = NULL;
        fprintf(stderr, "Not enough allocated space for global ranks array.");
    }
}

void set_namespace(int nprocs, char *ranks, char *name)
{
    size_t ninfo;
    pmix_info_t *info;
    ninfo = 4;
    PMIX_INFO_CREATE(info, ninfo);
    (void)strncpy(info[0].key, PMIX_UNIV_SIZE, PMIX_MAX_KEYLEN);
    info[0].value.type = PMIX_UINT32;
    info[0].value.data.uint32 = nprocs;
    
    (void)strncpy(info[1].key, PMIX_SPAWNED, PMIX_MAX_KEYLEN);
    info[1].value.type = PMIX_UINT32;
    info[1].value.data.uint32 = 0;

    (void)strncpy(info[2].key, PMIX_LOCAL_SIZE, PMIX_MAX_KEYLEN);
    info[2].value.type = PMIX_UINT32;
    info[2].value.data.uint32 = nprocs;

    (void)strncpy(info[3].key, PMIX_LOCAL_PEERS, PMIX_MAX_KEYLEN);
    info[3].value.type = PMIX_STRING;
    info[3].value.data.string = strdup(ranks);
    
    PMIx_server_register_nspace(name, nprocs, info, ninfo);
    PMIX_INFO_FREE(info, ninfo);
}

void set_client_argv(test_params *params, char ***argv)
{
    pmix_argv_append_nosize(argv, params->binary);
    pmix_argv_append_nosize(argv, "-s");
    pmix_argv_append_nosize(argv, TEST_NAMESPACE);
    if (params->nonblocking) {
        pmix_argv_append_nosize(argv, "-nb");
        if (params->barrier) {
            pmix_argv_append_nosize(argv, "-b");
        }
    }
    if (params->collect) {
        pmix_argv_append_nosize(argv, "-c");
    }
    pmix_argv_append_nosize(argv, "-n");
    if (NULL == params->np) {
        pmix_argv_append_nosize(argv, "1");
    } else {
        pmix_argv_append_nosize(argv, params->np);
    }
    if( params->verbose ){
        pmix_argv_append_nosize(argv, "-v");
    }
    if (NULL != params->prefix) {
        pmix_argv_append_nosize(argv, "-o");
        pmix_argv_append_nosize(argv, params->prefix);
    }
    if( params->early_fail ){
        pmix_argv_append_nosize(argv, "--early-fail");
    }
    if (NULL != params->fences) {
        pmix_argv_append_nosize(argv, "--fence");
        pmix_argv_append_nosize(argv, params->fences);
    }
    if (NULL != params->data) {
        pmix_argv_append_nosize(argv, "--data");
        pmix_argv_append_nosize(argv, params->data);
    }
    if (NULL != params->noise) {
        pmix_argv_append_nosize(argv, "--noise");
        pmix_argv_append_nosize(argv, params->noise);
    }

}
