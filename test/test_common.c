#include "test_common.h"
#include <stdarg.h>
#include "src/api/pmix_common.h"

int pmix_test_verbose = 0;

FILE *file;

#define OUTPUT_MAX 1024
char *pmix_test_output_prepare(const char *fmt, ... )
{
    static char output[OUTPUT_MAX];
    va_list args;
    va_start( args, fmt );
    memset(output, 0, sizeof(output));
    vsnprintf(output, OUTPUT_MAX - 1, fmt, args);
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
            fprintf(stderr, "usage: pmix_test [-h] [-e foo] [-b] [-c] [-nb]\n");
            fprintf(stderr, "\t-n       provides information about the job size (for checking purposes)\n");
            fprintf(stderr, "\t-e foo   use foo as test client\n");
            fprintf(stderr, "\t-v       verbose output\n");
            fprintf(stderr, "\t-t <>    set timeout\n");
            fprintf(stderr, "\t-o out   redirect clients logs to file out.<rank>\n");
            fprintf(stderr, "\t--early-fail    force client process with rank 0 to fail before PMIX_Init.\n");
            fprintf(stderr, "\t--ns-dist n1:n2:n3   register n namespaces (3 in this example) each with ni ranks (n1, n2 or n3).\n");
            fprintf(stderr, "\t--fence \"[<data_exchange><blocking> | ns0:ranks;ns1:ranks...][...]\"  specify fences in different configurations.\n");
            fprintf(stderr, "\t--use-same-keys relaative to the --fence option: put the same keys in the interim between multiple fences.\n");
            fprintf(stderr, "\t--job-fence  test fence inside its own namespace.\n");
            fprintf(stderr, "\t-c       relative to the --job-fence option: fence[_nb] callback shall include all collected data\n");
            fprintf(stderr, "\t-nb      relative to the --job-fence option: use non-blocking fence\n");
            fprintf(stderr, "\t--noise \"[ns0:ranks;ns1:ranks...]\"  add system noise to specified processes.\n");
            fprintf(stderr, "\t--test-publish     test publish/lookup/unpublish api.\n");
            fprintf(stderr, "\t--test-spawn       test spawn api.\n");
            fprintf(stderr, "\t--test-connect     test connect/disconnect api.\n");
            fprintf(stderr, "\t--test-resolve-peers    test resolve_peers api.\n");
            exit(0);
        } else if (0 == strcmp(argv[i], "--exec") || 0 == strcmp(argv[i], "-e")) {
            i++;
            if (NULL != argv[i]) {
                params->binary = strdup(argv[i]);
            }
        } else if( 0 == strcmp(argv[i], "--verbose") || 0 == strcmp(argv[i],"-v") ){
            TEST_VERBOSE_ON();
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
        } else if( 0 == strcmp(argv[i], "--early-fail") ){
            params->early_fail = 1;
        } else if (0 == strcmp(argv[i], "--fence")) {
            i++;
            if (NULL != argv[i]) {
                params->fences = strdup(argv[i]);
                if (0 != parse_fence(params->fences, 0)) {
                    fprintf(stderr, "Incorrect --fence option format: %s\n", params->fences);
                    exit(1);
                }
            }
        } else if (0 == strcmp(argv[i], "--use-same-keys")) {
            params->use_same_keys = 1;
        } else if (0 == strcmp(argv[i], "--job-fence")) {
            params->test_job_fence = 1;
        } else if (0 == strcmp(argv[i], "--collect") || 0 == strcmp(argv[i], "-c")) {
            params->collect = 1;
        } else if (0 == strcmp(argv[i], "--non-blocking") || 0 == strcmp(argv[i], "-nb")) {
            params->nonblocking = 1;
        } else if (0 == strcmp(argv[i], "--noise")) {
            i++;
            if (NULL != argv[i]) {
                params->noise = strdup(argv[i]);
                if (0 != parse_noise(params->noise, 0)) {
                    fprintf(stderr, "Incorrect --noise option format: %s\n", params->noise);
                    exit(1);
                }
            }
        } else if (0 == strcmp(argv[i], "--ns-dist")) {
            i++;
            if (NULL != argv[i]) {
                params->ns_dist = strdup(argv[i]);
            }
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
        } else if( 0 == strcmp(argv[i], "--test-publish") ){
            params->test_publish = 1;
        } else if( 0 == strcmp(argv[i], "--test-spawn") ){
            params->test_spawn = 1;
        } else if( 0 == strcmp(argv[i], "--test-connect") ){
            params->test_connect = 1;
        } else if( 0 == strcmp(argv[i], "--test-resolve-peers") ){
            params->test_resolve_peers = 1;
        }

        else {
            fprintf(stderr, "unrecognized option: %s\n", argv[i]);
            exit(1);
        }
    }
    if (NULL == params->binary) {
        params->binary = strdup("pmix_client");
    }
}

pmix_list_t test_fences;
range_desc_t *noise_range = NULL;

static void ncon(nspace_desc_t *p)
{
    p->id = -1;
    PMIX_CONSTRUCT(&(p->ranks), pmix_list_t);
}

static void ndes(nspace_desc_t *p)
{
    PMIX_LIST_DESTRUCT(&(p->ranks));
}

PMIX_CLASS_INSTANCE(nspace_desc_t,
                          pmix_list_item_t,
                          ncon, ndes);

static void fcon(fence_desc_t *p)
{
    p->blocking = 0;
    p->data_exchange = 0;
    p->range = PMIX_NEW(range_desc_t);
}

static void fdes(fence_desc_t *p)
{
    PMIX_RELEASE(p->range);
}

PMIX_CLASS_INSTANCE(fence_desc_t,
                          pmix_list_item_t,
                          fcon, fdes);

PMIX_CLASS_INSTANCE(rank_desc_t,
                          pmix_list_item_t,
                          NULL, NULL);

static void rcon(range_desc_t *p)
{
    PMIX_CONSTRUCT(&(p->nspaces), pmix_list_t);
}

static void rdes(range_desc_t *p)
{
    PMIX_LIST_DESTRUCT(&(p->nspaces));
}

PMIX_CLASS_INSTANCE(range_desc_t,
                          pmix_list_item_t,
                          rcon, rdes);

static int ns_id = -1;
static fence_desc_t *fdesc = NULL;
static range_desc_t *range = NULL;

#define CHECK_STRTOL_VAL(val, str, store) do {                  \
    if (0 == val) {                                             \
        if (0 != strncmp(str, "0", 1)) {                         \
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
    nspace_desc_t *ndesc;
    rank_desc_t *rdesc;
    int rank;
    switch (step) {
        case 0:
            if (store) {
                fdesc = PMIX_NEW(fence_desc_t);
                range = fdesc->range;
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
            if (store && NULL == range) {
                range = PMIX_NEW(range_desc_t);
                noise_range = range;
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
            if (store && NULL != range) {
                ndesc = PMIX_NEW(nspace_desc_t);
                ndesc->id = ns_id;
            }
            while (' ' == *str) {
                str++;
            }
            if ('\0' == *str) {
                /* all ranks from namespace participate */
                if (store && NULL != range) {
                    rdesc = PMIX_NEW(rank_desc_t);
                    rdesc->rank = -1;
                    pmix_list_append(&(ndesc->ranks), &rdesc->super);
                }
            }
            while ('\0' != *str) {
                if (',' == *str && 0 != count) {
                    *str = '\0';
                    if (-1 != remember) {
                        rank = (int)(strtol(str-count, NULL, 10));
                        CHECK_STRTOL_VAL(rank, str-count, store);
                        for (i = remember; i < rank; i++) {
                            if (store && NULL != range) {
                                rdesc = PMIX_NEW(rank_desc_t);
                                rdesc->rank = i;
                                pmix_list_append(&(ndesc->ranks), &rdesc->super);
                            }
                        }
                        remember = -1;
                    }
                    rank = (int)(strtol(str-count, NULL, 10));
                    CHECK_STRTOL_VAL(rank, str-count, store);
                    if (store && NULL != range) {
                        rdesc = PMIX_NEW(rank_desc_t);
                        rdesc->rank = rank;
                        pmix_list_append(&(ndesc->ranks), &rdesc->super);
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
                        if (store && NULL != range) {
                            rdesc = PMIX_NEW(rank_desc_t);
                            rdesc->rank = i;
                            pmix_list_append(&(ndesc->ranks), &rdesc->super);
                        }
                    }
                    remember = -1;
                }
                rank = (int)(strtol(str-count, NULL, 10));
                CHECK_STRTOL_VAL(rank, str-count, store);
                if (store && NULL != range) {
                    rdesc = PMIX_NEW(rank_desc_t);
                    rdesc->rank = rank;
                    pmix_list_append(&(ndesc->ranks), &rdesc->super);
                }
            }
            if (store && NULL != range) {
                pmix_list_append(&(range->nspaces), &ndesc->super);
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
    range = NULL;
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

int parse_noise(char *noise_param, int store)
{
    int ret = 0;
    char *tmp = strdup(noise_param);
    char * pch, *ech;
    range = NULL;
    pch = strchr(tmp, '[');
    if (NULL != pch) {
        pch++;
        ech = strchr(pch, ']');
        if (NULL != ech) {
            *ech = '\0';
            ech++;
            if ('\0' != *ech) {
                ret = 1;
            } else {
                ret = parse_token(pch, 1, store);
            }
        } else {
            ret = 1;
        }
    }
    free(tmp);
    return ret;
}

int get_all_ranks_from_namespace(test_params params, char *nspace, int **ranks, size_t *nranks)
{
    int base_rank = 0;
    size_t num_ranks = 0;
    int num = -1;
    size_t j;
    if (NULL == params.ns_dist) {
        *nranks = params.ns_size;
        *ranks = (int*)malloc(params.ns_size * sizeof(int));
        for (j = 0; j < (size_t)params.ns_size; j++) {
            (*ranks)[j] = j;
        }
    } else {
        char *tmp = strdup(params.ns_dist);
        char *pch = tmp;
        int ns_id = (int)strtol(nspace + strlen(TEST_NAMESPACE) + 1, NULL, 10);
        while (NULL != pch && num != ns_id) {
            base_rank += num_ranks;
            pch = strtok((-1 == num ) ? tmp : NULL, ":");
            num++;
            num_ranks = (size_t)strtol(pch, NULL, 10);
        }
        if (num == ns_id && 0 != num_ranks) {
            *nranks = num_ranks;
            *ranks = (int*)malloc(num_ranks * sizeof(int));
            for (j = 0; j < num_ranks; j++) {
                (*ranks)[j] = base_rank+j;
            }
        } else {
            free(tmp);
            return PMIX_ERROR;
        }
        free(tmp);
    }
    return PMIX_SUCCESS;
}

