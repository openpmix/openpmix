#include "test_common.h"
#include <stdarg.h>

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
            }
        } else if (0 == strcmp(argv[i], "--h") || 0 == strcmp(argv[i], "-h")) {
            /* print help */
            fprintf(stderr, "usage: pmix_test [-h] [-e foo] [-b] [-c] [-nb]\n");
            fprintf(stderr, "\t-n       provides information about the job size (for checking purposes)\n");
            fprintf(stderr, "\t-s nsp   namespace of the job (for checking purposes)\n");
            fprintf(stderr, "\t-e foo   use foo as test client\n");
            fprintf(stderr, "\t-b       execute fence_nb callback when all procs reach that point\n");
            fprintf(stderr, "\t-c       fence[_nb] callback shall include all collected data\n");
            fprintf(stderr, "\t-nb      use non-blocking fence\n");
            fprintf(stderr, "\t-v       verbose output\n");
            fprintf(stderr, "\t-t <>    set timeout\n");
            fprintf(stderr, "\t-o out   redirect clients logs to file out.<rank>\n");
            exit(0);
        } else if (0 == strcmp(argv[i], "--exec") || 0 == strcmp(argv[i], "-e")) {
            i++;
            if (NULL != argv[i]) {
                params->binary = strdup(argv[i]);
            }
        } else if (0 == strcmp(argv[i], "--barrier") || 0 == strcmp(argv[i], "-b")) {
            params->barrier = 1;
        } else if (0 == strcmp(argv[i], "--collect") || 0 == strcmp(argv[i], "-c")) {
            params->collect = 1;
        } else if (0 == strcmp(argv[i], "--non-blocking") || 0 == strcmp(argv[i], "-nb")) {
            params->nonblocking = 1;
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
