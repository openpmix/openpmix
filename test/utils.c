#include "utils.h"
#include "test_common.h"
#include "pmix_server.h"
#include "src/util/pmix_environ.h"
#include "cli_stages.h"

static void fill_seq_ranks_array(size_t nprocs, char **ranks)
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

static void set_namespace(int nprocs, char *ranks, char *name)
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

static void set_client_argv(test_params *params, char ***argv)
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

int launch_clients(test_params params, char *** client_env, char ***client_argv)
{
    uint32_t n;
    char *tmp;
    uid_t myuid;
    gid_t mygid;
    char *ranks = NULL;
    char digit[MAX_DIGIT_LEN];
    int cl_arg_len;
    int rc;

    *client_env = pmix_argv_copy(environ);
    set_client_argv(&params, client_argv);

    tmp = pmix_argv_join(*client_argv, ' ');
    TEST_VERBOSE(("Executing test: %s", tmp));
    free(tmp);

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

    myuid = getuid();
    mygid = getgid();

    /* fork/exec the test */
    for (n=0; n < params.nprocs; n++) {
        if (PMIX_SUCCESS != (rc = PMIx_server_setup_fork(TEST_NAMESPACE, n, client_env))) {
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
        pmix_argv_append_nosize(client_argv, "-r");
        pmix_argv_append_nosize(client_argv, digit);

        if (cli_info[n].pid == 0) {
            if( !TEST_VERBOSE_GET() ){
                // Hide clients stdout
                // TODO: on some systems stdout is a constant, address this
                fclose(stdout);
                stdout = fopen("/dev/null","w");
            }
            execve(params.binary, *client_argv, *client_env);
            /* Does not return */
            exit(0);
        }
        cli_info[n].state = CLI_FORKED;

        /* delete two last arguments : -r <rank> */
        cl_arg_len = pmix_argv_len(*client_argv);
        pmix_argv_delete(&cl_arg_len, client_argv, cl_arg_len-2, 2);
    }
    return PMIX_SUCCESS;
}
