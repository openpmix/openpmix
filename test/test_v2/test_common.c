/*
 * Copyright (c) 2013-2017 Intel, Inc. All rights reserved.
 * Copyright (c) 2015      Artem Y. Polyakov <artpol84@gmail.com>.
 *                         All rights reserved.
 * Copyright (c) 2015-2018 Mellanox Technologies, Inc.
 *                         All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 */

/* Note: this file is for functions called by both client and server and their
        callees. */

#include <src/include/pmix_config.h>
#include <pmix_common.h>

#include "test_common.h"
#include <stdarg.h>
#include <stdio.h>
#include <ctype.h>
#include <assert.h>

int pmix_test_verbose = 0;
test_params params;

FILE *file;

#define OUTPUT_MAX 1024
char *pmix_test_output_prepare(const char *fmt, ... )
{
    static char output[OUTPUT_MAX];
    va_list args;
    va_start( args, fmt );
    memset(output, 0, sizeof(output));
    vsnprintf(output, OUTPUT_MAX - 1, fmt, args);
    va_end(args);
    return output;
}

// initialize the global *nodes array
void init_nodes(int num_nodes) {
    int i;
    nodes = malloc(num_nodes * sizeof(node_map));
    for (i = 0; i < num_nodes; i++) {
        nodes[i].pmix_rank = NULL;
        nodes[i].pmix_hostname[0] = '\0';
    }
}

// frees the global *nodes array
void free_nodes(int num_nodes)
{
    int i;
    if (NULL == nodes) {
        TEST_ERROR(("nodes pointer is NULL, cannot free; aborting"));
        exit(1);
    }
    TEST_VERBOSE(("num_nodes = %d, nodes[0].pmix_rank[0] = %d", num_nodes, nodes[0].pmix_rank[0]));
    for (i = 0; i < num_nodes; i++) {
        if (NULL == nodes[i].pmix_rank) {
            TEST_ERROR(("Encountered NULL pointer in free loop of nodes[%d].pmix_rank; aborting", i));
            exit(1);
        }
        free(nodes[i].pmix_rank);
    }
    free(nodes);
}

// populates the global *nodes array
void parse_rank_placement_string(char *placement_str, int num_nodes)
{
    /* example rank placement string:
    // 0:0,2,4;1:1,3,5
    // equates to: (node 0: ranks 0, 2, and 4; node 1: ranks 1, 3, and 5)
    // no error checking in this function, we assume the string has correct syntax
    // and all nodes/procs are properly accounted for */
    char *tempstr, *pch, **buf;
    char *saveptr = NULL;
    size_t i, j, idx = 0;

    buf = malloc(num_nodes * sizeof(char *));
    tempstr = strdup(placement_str);
    // first we separate the nodes from one another
    pch = strtok_r(tempstr,";", &saveptr);
    while (NULL != pch) {
        buf[idx] = malloc(strlen(pch) + 1);
        strcpy(buf[idx], pch);
        pch = strtok_r(NULL, ";", &saveptr);
        idx++;
    }
    // now we go back over populated buf array and separate the numbered nodes from the
    // comma-delimited list of ranks; then we separate out each rank
    for (i = 0; i < idx; i++) {
        pch = strtok_r(buf[i], ":", &saveptr);
        nodes[i].pmix_nodeid = strtol(pch, NULL, 0);
        snprintf(nodes[i].pmix_hostname, PMIX_MAX_KEYLEN, "node%u", nodes[i].pmix_nodeid);
        nodes[i].pmix_rank = malloc(sizeof(pmix_rank_t));
        j = 0;
        pch = strtok_r(NULL, ",", &saveptr);
        while (NULL != pch) {
            nodes[i].pmix_rank = (pmix_rank_t *) realloc(nodes[i].pmix_rank, (j+1) * sizeof(pmix_rank_t));
            nodes[i].pmix_rank[j] = strtol(pch, NULL, 0);
            pch = strtok_r(NULL, ",", &saveptr);
            j++;
        }
        nodes[i].pmix_local_size = j;
        TEST_VERBOSE(("num_nodes: %d idx: %d nodes[%d].pmix_local_size: %d", num_nodes, idx, i, j));
    }

    free(tempstr);
    for (i = 0; i < idx; i++) {
        free(buf[i]);
    }
    free(buf);
}

void parse_cmd(int argc, char **argv, test_params *params, validation_params *v_params)
{
    int i;
    uint32_t job_size;

    /* set output to stdout by default */
    file = stdout;
    if( params->nspace != NULL ) {
        params->nspace = NULL;
    }
    // debug output
    /*
    for (i=0; i< argc; i++){
        printf("argv[%d] = \"%s\"\n", i, argv[i]);
    }
    */
    /* parse user options */
    for (i=1; i < argc; i++) {
        if (0 == strcmp(argv[i], "--n") || 0 == strcmp(argv[i], "-n")) {
            i++;
            if (NULL != argv[i]) {
                params->np = strdup(argv[i]);
                job_size = strtol(argv[i], NULL, 10);
                params->nprocs = job_size;
                PMIXT_VAL_PARAM_SETNUM(v_params, pmix_job_size, job_size);
                if (-1 == params->ns_size) {
                    params->ns_size = job_size;
                }
            }
        } else if (0 == strcmp(argv[i], "--h") || 0 == strcmp(argv[i], "-h")) {
            /* print help */
            fprintf(stderr, "usage: pmix_test [-h] [-e foo] [-b] [-nb]\n");
            fprintf(stderr, "\t-n       the job size (for checking purposes)\n");
            fprintf(stderr, "\t-s       number of servers to emulate\n");
            fprintf(stderr, "\t-e foo   use foo as test client executable\n");
            fprintf(stderr, "\t-v       verbose output\n");
            fprintf(stderr, "\t-t <>    set timeout\n");
            fprintf(stderr, "\t-o out   redirect clients logs to file out.<rank>\n");
            fprintf(stderr, "\t-d str   assign ranks to servers, for example: 0:0,1:1:2,3,4\n");
            fprintf(stderr, "\t-c       fence[_nb] callback shall include all collected data\n");
            fprintf(stderr, "\t-nb      use non-blocking fence\n");
            exit(0);
        } else if (0 == strcmp(argv[i], "--exec") || 0 == strcmp(argv[i], "-e")) {
            i++;
            if (NULL != argv[i]) {
                params->binary = strdup(argv[i]);
            }
        } else if (0 == strcmp(argv[i], "--nservers") || 0 == strcmp(argv[i], "-s")){
            i++;
            if (NULL != argv[i]) {
                params->nservers = atoi(argv[i]);
                PMIXT_VAL_PARAM_SETNUM(v_params, pmix_num_nodes, params->nservers);
            }
        } else if( 0 == strcmp(argv[i], "--verbose") || 0 == strcmp(argv[i],"-v") ){
            PMIXT_VERBOSE_ON();
            params->verbose = 1;
        } else if (0 == strcmp(argv[i], "--timeout") || 0 == strcmp(argv[i], "-t")) {
            i++;
            if (NULL != argv[i]) {
                params->timeout = atoi(argv[i]);
                if( params->timeout == 0 ){
                    params->timeout = TEST_DEFAULT_TIMEOUT;
                }
            }
        } else if( 0 == strcmp(argv[i], "-o")) {
            i++;
            if (NULL != argv[i]) {
                params->prefix = strdup(argv[i]);
            }
        /*
        } else if( 0 == strcmp(argv[i], "--namespace")) {
            i++;
            if (NULL != argv[i]) {
                params->nspace = strdup(argv[i]);
            }
        } else if (0 == strcmp(argv[i], "--rank") || 0 == strcmp(argv[i], "-r")) {
            i++;
            if (NULL != argv[i]) {
                pmix_rank_t rank = strtol(argv[i], NULL, 10);
                PMIXT_VAL_PARAM_SETNUM(v_params, pmix_rank, rank);
            }
        } else if (0 == strcmp(argv[i], "--collect-corrupt")) {
            params->collect_bad = 1;
        } else if (0 == strcmp(argv[i], "--non-blocking") || 0 == strcmp(argv[i], "-nb")) {
            params->nonblocking = 1;
        */
        } else if (0 == strcmp(argv[i], "--collect") || 0 == strcmp(argv[i], "-c")) {
            params->collect = 1;
        } else if (0 == strcmp(argv[i], "--ns-size")) {
            i++;
            if (NULL != argv[i]) {
                params->ns_size = strtol(argv[i], NULL, 10);
            }
        } else if (0 == strcmp(argv[i], "--ns-id")) {
            i++;
            if (NULL != argv[i]) {
                params->ns_id = strtol(argv[i], NULL, 10);
            }
        } else if (0 == strcmp(argv[i], "--base-rank")) {
            i++;
            if (NULL != argv[i]) {
                params->base_rank = strtol(argv[i], NULL, 10);
            }
        } else if (0 == strcmp(argv[i], "--validate-params")) {
            i++;
            v_params->validate_params = 1;
            v_params_ascii_str = strdup(argv[i]);
        // set up custom rank placement
	    } else if (0 == strcmp(argv[i], "--distribute-ranks") || 0 == strcmp(argv[i], "-d") ) {
            i++;
            if ((PMIX_MAX_KEYLEN - 1) < strlen(argv[i])) {
                TEST_ERROR(("Rank distribution string exceeds max length of %d bytes", PMIX_MAX_KEYLEN-1));
                exit(1);
            }
            v_params->custom_rank_placement = 1;
            strcpy(v_params->rank_placement_string, argv[i]);
            TEST_VERBOSE(("rank_placement_string: %s", v_params->rank_placement_string));
        }
        else {
            fprintf(stderr, "unrecognized option: %s\n", argv[i]);
            exit(1);
        }
    }
    if (NULL == params->binary) {
        char *basename = NULL;
        basename = strrchr(argv[0], '/');
        if (basename) {
            *basename = '\0';
            /* pmix_test and pmix_clients are the shell scripts that
             * make sure that actual binary placed in "./.libs" directory
             * is properly linked.
             * after launch pmix_test you'll find the following process:
             *      <pmix-root-dir>/test/.libs/lt-pmix_test
             *
             * To launch
             *      <pmix-root-dir>/test/pmix_client
             * instead of
             *      <pmix-root-dir>/test/.libs/pmix_client
             * we need to do a step back in directory tree.
             */
            if (0 > asprintf(&params->binary, "%s/../pmix_client", argv[0])) {
                exit(1);
            }
            *basename = '/';
        } else {
            if (0 > asprintf(&params->binary, "pmix_client")) {
                exit(1);
            }
        }
    }
/*
    if( params->collect_bad ){
        params->collect = v_params->pmix_rank % 2;
    }
*/
}

void default_params(test_params *params, validation_params *v_params) {
    params->binary = NULL;
    params->np = NULL;
    params->prefix = NULL;
    params->nspace = NULL;
    params->nprocs = 1;
    params->timeout = TEST_DEFAULT_TIMEOUT;
    params->verbose = 0;
    params->collect_bad = 0;
    params->collect = 0;
    params->nonblocking = 0;
    params->ns_size = -1;
    params->ns_id = -1;
    params->base_rank = 0;
    params->nservers = 1;

    v_params->version = PMIXT_VALIDATION_PARAMS_VER;
    v_params->validate_params = false;
    v_params->check_pmix_rank = false;
    v_params->pmix_rank = PMIX_RANK_UNDEF;
    v_params->check_pmix_nspace = false;
    v_params->pmix_nspace[0] = '\0';
    v_params->check_pmix_job_size = false;
    v_params->pmix_job_size = 0;
    v_params->check_pmix_univ_size = false;
    v_params->pmix_univ_size = 0;
    v_params->check_pmix_jobid = false;
    v_params->pmix_jobid[0] = '\0';
    v_params->check_pmix_local_size = false;
    v_params->pmix_local_size = 0;
    v_params->check_pmix_local_rank = false;
    v_params->pmix_local_rank = 0;
    v_params->check_pmix_nodeid = false;
    v_params->pmix_nodeid = 0;
    v_params->check_pmix_local_peers = false;
    v_params->pmix_local_peers[0] = '\0';
    v_params->check_pmix_hostname = false;
    v_params->pmix_hostname[0] = '\0';
    v_params->check_pmix_num_nodes = false;
    v_params->pmix_num_nodes = 1;
    v_params->check_pmix_node_rank = false;
    v_params->pmix_node_rank = 0;
}

// also touches the global array *nodes
void free_params(test_params *params, validation_params *vparams) {
    if (NULL != params->binary) {
        free(params->binary);
    }
    if (NULL != params->np) {
        free(params->np);
    }
    if (NULL != params->prefix) {
        free(params->prefix);
    }
    if (NULL != params->nspace) {
        free(params->nspace);
    }

    if (NULL != v_params_ascii_str) {
        free(v_params_ascii_str);
    }
    if (vparams->custom_rank_placement) {
        free_nodes(vparams->pmix_num_nodes);
    }
    TEST_VERBOSE(("Completed free_params"));
}

static void fcon(fence_desc_t *p)
{
    p->blocking = 0;
    p->data_exchange = 0;
    p->participants = PMIX_NEW(pmix_list_t);
}

static void fdes(fence_desc_t *p)
{
    PMIX_LIST_RELEASE(p->participants);
}

PMIX_CLASS_INSTANCE(fence_desc_t,
                    pmix_list_item_t,
                    fcon, fdes);

PMIX_CLASS_INSTANCE(participant_t,
                    pmix_list_item_t,
                    NULL, NULL);

PMIX_CLASS_INSTANCE(key_replace_t,
                    pmix_list_item_t,
                    NULL, NULL);

static int ns_id = -1;
static fence_desc_t *fdesc = NULL;
pmix_list_t *participants = NULL;
pmix_list_t test_fences;
pmix_list_t *noise_range = NULL;
pmix_list_t key_replace;

#define CHECK_STRTOL_VAL(val, str, store) do {                  \
    if (0 == val) {                                             \
        if (0 != strncmp(str, "0", 1)) {                        \
            if (!store) {                                       \
                return 1;                                       \
            }                                                   \
        }                                                       \
    }                                                           \
} while (0)
