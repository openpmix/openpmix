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

#include <src/include/pmix_config.h>
#include <pmix_common.h>

#include "test_common.h"
#include <stdarg.h>
#include <stdio.h>
#include <ctype.h>

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

void parse_cmd(int argc, char **argv, test_params *params)
{
    int i;

    /* set output to stderr by default */
    file = stdout;
    if( params->nspace != NULL ) {
        params->nspace = NULL;
    }

    /* parse user options */
    for (i=1; i < argc; i++) {
        if (0 == strcmp(argv[i], "--n") || 0 == strcmp(argv[i], "-n")) {
            i++;
            if (NULL != argv[i]) {
                params->np = strdup(argv[i]);
                params->nprocs = strtol(argv[i], NULL, 10);
                if (-1 == params->ns_size) {
                    params->ns_size = params->nprocs;
                }
            }
        } else if (0 == strcmp(argv[i], "--h") || 0 == strcmp(argv[i], "-h")) {
            /* print help */
            fprintf(stderr, "usage: pmix_test [-h] [-e foo] [-b] [-nb]\n");
            fprintf(stderr, "\t-n       the job size (for checking purposes)\n");
            fprintf(stderr, "\t-s       number of servers to emulate\n");
            fprintf(stderr, "\t-e foo   use foo as test client\n");
            fprintf(stderr, "\t-v       verbose output\n");
            fprintf(stderr, "\t-t <>    set timeout\n");
            fprintf(stderr, "\t-o out   redirect clients logs to file out.<rank>\n");
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
            }
            if (2 < params->nservers) {
                fprintf(stderr, "Only support up to 2 servers\n");
                exit(1);
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
        } else if( 0 == strcmp(argv[i], "-s")) {
            i++;
            if (NULL != argv[i]) {
                params->nspace = strdup(argv[i]);
            }
        } else if (0 == strcmp(argv[i], "--rank") || 0 == strcmp(argv[i], "-r")) {
            i++;
            if (NULL != argv[i]) {
                params->rank = strtol(argv[i], NULL, 10);
            }
        } else if (0 == strcmp(argv[i], "--collect-corrupt")) {
            params->collect_bad = 1;
        } else if (0 == strcmp(argv[i], "--collect") || 0 == strcmp(argv[i], "-c")) {
            params->collect = 1;
        } else if (0 == strcmp(argv[i], "--non-blocking") || 0 == strcmp(argv[i], "-nb")) {
            params->nonblocking = 1;
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

    if( params->collect_bad ){
        params->collect = params->rank % 2;
    }
}

void init_test_params(test_params *params) {
    params->nprocs = 1;                
    params->verbose = 0;               
    params->rank = PMIX_RANK_UNDEF;    
    params->base_rank = 0;             
    params->ns_size = -1;              
    params->ns_id = -1;                
    params->timeout = TEST_DEFAULT_TIMEOUT; 
    params->collect = 0;               
    params->collect_bad = 0;           
    params->nonblocking = 0;           
    params->binary = NULL;             
    params->np = NULL;                 
    params->prefix = NULL;             
    params->nspace = NULL;             
    params->nservers = 1;              
    params->lsize = 0;                 
}

void free_test_params(test_params *params) { 
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
} 

void pmixt_pre_init(int argc, char **argv, test_params *params) {

    init_test_params(params);
    parse_cmd(argc, argv, params);
    /* set filename if available in params */
    if ( NULL != params->prefix && -1 != params->ns_id ) {
	char *fname = malloc( strlen(params->prefix) + MAX_DIGIT_LEN + 2 ); 
        sprintf(fname, "%s.%d.%d", params->prefix, params->ns_id, params->rank); 
        file = fopen(fname, "w"); 
        free(fname); 
        if( NULL == file ){ 
            fprintf(stderr, "Cannot open file %s for writing!", fname); 
            exit(1); 
        } 
    }
    else {
        file = stdout;
    }
}

void pmixt_fix_rank_and_ns(pmix_proc_t *this_proc, test_params *params) {
    // Fix rank if running under RM
    if( PMIX_RANK_UNDEF == params->rank ){
        char *ranklist = getenv("SLURM_GTIDS");
        char *rankno = getenv("SLURM_LOCALID");
        // Fix rank if running under SLURM
        if( NULL != ranklist && NULL != rankno ){
            char **argv = pmix_argv_split(ranklist, ',');
            int count = pmix_argv_count(argv);
            int rankidx = strtoul(rankno, NULL, 10);
            if( rankidx >= count ){
                fprintf(stderr, "It feels like we are running under SLURM:\n\t"
                        "SLURM_GTIDS=%s, SLURM_LOCALID=%s\nbut env vars are conflicting\n",
                        ranklist, rankno);
                exit(1);
            }
            params->rank = strtoul(argv[rankidx], NULL, 10);
            pmix_argv_free(argv);
        }
        else if ( NULL == getenv("PMIX_RANK") ) { /* we must not be running under SLURM */
            // **Call separate function for your own resource manager here**
            // It should likely be wrapped in condition that checks for existence of an RM env var
            // Function name should be of form: fix_rank_and_nspace_rm_* 
            // and should check/fix both rank and nspace
            // e.g: fix_rank_and_ns_rm_pbs(), fix_rank_and_ns_rm_torque(), etc.
        }
        else { /* unknown situation - PMIX_RANK is not null but SLURM env vars are */
            fprintf(stderr, "It feels like we are running under SLURM:\n\t"
                    "PMIX_RANK=%s\nbut SLURM env vars are null\n",
                    getenv("PMIX_RANK"));
            exit(1);
        }
    }

    // Fix namespace if running under RM
    if( NULL == params->nspace ){
        char *nspace = getenv("PMIX_NAMESPACE");
        if( NULL != nspace ){
            params->nspace = strdup(nspace);
        }
	else { /* If we aren't running under SLURM, you should have set nspace 
	          in your custom fix_rank_and_ns_rm_* function! */
            fprintf(stderr, "nspace not set. Is the fix_rank_and_ns_rm_*"
	            " function for this resource manager failing to set it?\n");
            exit(1);
        }
    }

    if (this_proc->rank != params->rank) {
        TEST_ERROR(("Client ns %s Rank returned in PMIx_Init %d does not match rank from command line %d.", 
	    this_proc->nspace, this_proc->rank, params->rank));
        free_test_params(params);
        exit(1);
    }
}

void pmixt_post_init(pmix_proc_t *this_proc, test_params *params) {
    pmixt_fix_rank_and_ns(this_proc, params);
    TEST_VERBOSE((" Client ns %s rank %d: PMIx_Init success", this_proc->nspace, this_proc->rank));
 
}

void pmixt_post_finalize(pmix_proc_t *this_proc, test_params *params) {
    TEST_VERBOSE((" Client ns %s rank %d: PMIx_Finalize success", this_proc->nspace, this_proc->rank));
    if( (stdout != file) && (stderr != file) ) {
        fclose(file);
    }
    free_test_params(params);
    exit(EXIT_SUCCESS);
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

static int parse_token(char *str, int step, int store)
{
    char *pch;
    int count = 0;
    int remember = -1;
    int i;
    int rank;
    participant_t *proc;

    switch (step) {
    case 0:
        if (store) {
            fdesc = PMIX_NEW(fence_desc_t);
            participants = fdesc->participants;
        }
        pch = strchr(str, '|');
        if (NULL != pch) {
            while (pch != str) {
                if ('d' == *str) {
                    if (store && NULL != fdesc) {
                        fdesc->data_exchange = 1;
                    }
                } else if ('b' == *str) {
                    if (store && NULL != fdesc) {
                        fdesc->blocking = 1;
                    }
                } else if (' ' != *str) {
                    if (!store) {
                        return 1;
                    }
                }
                str++;
            }
            if (0 < parse_token(pch+1, 1, store)) {
                if (!store) {
                    return 1;
                }
            }
        } else {
            if (0 < parse_token(str, 1, store)) {
                if (!store) {
                    return 1;
                }
            }
        }
        if (store && NULL != fdesc) {
            pmix_list_append(&test_fences, &fdesc->super);
        }
        break;
    case 1:
        if (store && NULL == participants) {
            participants = PMIX_NEW(pmix_list_t);
            noise_range = participants;
        }
        pch = strtok(str, ";");
        while (NULL != pch) {
            if (0 < parse_token(pch, 2, store)) {
                if (!store) {
                    return 1;
                }
            }
            pch = strtok (NULL, ";");
        }
        break;
    case 2:
        pch = strchr(str, ':');
        if (NULL != pch) {
            *pch = '\0';
            pch++;
            while (' ' == *str) {
                str++;
            }
            ns_id = (int)(strtol(str, NULL, 10));
            CHECK_STRTOL_VAL(ns_id, str, store);
            if (0 < parse_token(pch, 3, store)) {
                if (!store) {
                    return 1;
                }
            }
        } else {
            if (!store) {
                return 1;
            }
        }
        break;
    case 3:
        while (' ' == *str) {
            str++;
        }
        if ('\0' == *str) {
            /* all ranks from namespace participate */
            if (store && NULL != participants) {
                proc = PMIX_NEW(participant_t);
                (void)snprintf(proc->proc.nspace, PMIX_MAX_NSLEN, "%s-%d", TEST_NAMESPACE, ns_id);
                proc->proc.rank = PMIX_RANK_WILDCARD;
                pmix_list_append(participants, &proc->super);
            }
        }
        while ('\0' != *str) {
            if (',' == *str && 0 != count) {
                *str = '\0';
                if (-1 != remember) {
                    rank = (int)(strtol(str-count, NULL, 10));
                    CHECK_STRTOL_VAL(rank, str-count, store);
                    for (i = remember; i < rank; i++) {
                        if (store && NULL != participants) {
                            proc = PMIX_NEW(participant_t);
                            (void)snprintf(proc->proc.nspace, PMIX_MAX_NSLEN, "%s-%d", TEST_NAMESPACE, ns_id);
                            proc->proc.rank = i;
                            pmix_list_append(participants, &proc->super);
                        }
                    }
                    remember = -1;
                }
                rank = (int)(strtol(str-count, NULL, 10));
                CHECK_STRTOL_VAL(rank, str-count, store);
                if (store && NULL != participants) {
                    proc = PMIX_NEW(participant_t);
                    (void)snprintf(proc->proc.nspace, PMIX_MAX_NSLEN, "%s-%d", TEST_NAMESPACE, ns_id);
                    proc->proc.rank = rank;
                    pmix_list_append(participants, &proc->super);
                }
                count = -1;
            } else if ('-' == *str && 0 != count) {
                *str = '\0';
                remember = (int)(strtol(str-count, NULL, 10));
                CHECK_STRTOL_VAL(remember, str-count, store);
                count = -1;
            }
            str++;
            count++;
        }
        if (0 != count) {
            if (-1 != remember) {
                rank = (int)(strtol(str-count, NULL, 10));
                CHECK_STRTOL_VAL(rank, str-count, store);
                for (i = remember; i < rank; i++) {
                    if (store && NULL != participants) {
                        proc = PMIX_NEW(participant_t);
                        (void)snprintf(proc->proc.nspace, PMIX_MAX_NSLEN, "%s-%d", TEST_NAMESPACE, ns_id);
                        proc->proc.rank = i;
                        pmix_list_append(participants, &proc->super);
                    }
                }
                remember = -1;
            }
            rank = (int)(strtol(str-count, NULL, 10));
            CHECK_STRTOL_VAL(rank, str-count, store);
            if (store && NULL != participants) {
                proc = PMIX_NEW(participant_t);
                (void)snprintf(proc->proc.nspace, PMIX_MAX_NSLEN, "%s-%d", TEST_NAMESPACE, ns_id);
                proc->proc.rank = rank;
                pmix_list_append(participants, &proc->super);
            }
        }
        break;
    default:
        fprintf(stderr, "Incorrect parsing step.\n");
        return 1;
    }
    return 0;
}

int parse_fence(char *fence_param, int store)
{
    int ret = 0;
    char *tmp = strdup(fence_param);
    char * pch, *ech;

    pch = strchr(tmp, '[');
    while (NULL != pch) {
        pch++;
        ech = strchr(pch, ']');
        if (NULL != ech) {
            *ech = '\0';
            ech++;
            ret += parse_token(pch, 0, store);
            pch = strchr(ech, '[');
        } else {
            ret = 1;
            break;
        }
    }
    free(tmp);
    return ret;
}

static int is_digit(const char *str)
{
    if (NULL == str)
        return 0;

    while (0 != *str) {
        if (!isdigit(*str)) {
        return 0;
        }
        else {
            str++;
        }
    }
    return 1;
}
