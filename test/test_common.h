/*
 * Copyright (c) 2013-2015 Intel, Inc.  All rights reserved.
 * Copyright (c) 2015      Artem Y. Polyakov <artpol84@gmail.com>.
 *                         All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 */

#ifndef TEST_COMMON_H
#define TEST_COMMON_H

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>

#define TEST_NAMESPACE "smoky_nspace"
#define TEST_CREDENTIAL "dummy"

/* WARNING: pmix_test_output_prepare is currently not threadsafe!
 * fix it once needed!
 */
char *pmix_test_output_prepare(const char *fmt,... );
extern int pmix_test_verbose;
extern FILE *file;

#define STRIPPED_FILE_NAME (strrchr(__FILE__, '/') ? strrchr(__FILE__, '/') + 1 : __FILE__)

#define TEST_OUTPUT(x) { \
    fprintf(file,"%s:%s: %s\n",STRIPPED_FILE_NAME, __FUNCTION__, \
            pmix_test_output_prepare x ); \
    fflush(file); \
}

// Write output wightout adding anything to it.
// Need for automate tests to receive "OK" string
#define TEST_OUTPUT_CLEAR(x) { \
    fprintf(file, "%s", pmix_test_output_prepare x ); \
    fflush(file); \
}

// Always write errors to the stderr
#define TEST_ERROR(x) { \
    fprintf(stderr,"ERROR [%s:%d:%s]: %s\n", STRIPPED_FILE_NAME, __LINE__, __FUNCTION__, \
            pmix_test_output_prepare x ); \
    fflush(stderr); \
}

#define TEST_VERBOSE_ON() (pmix_test_verbose = 1)
#define TEST_VERBOSE_GET() (pmix_test_verbose)

#define TEST_VERBOSE(x) { \
    if( pmix_test_verbose ){ \
        TEST_OUTPUT(x); \
    } \
}

#define TEST_DEFAULT_TIMEOUT 10
#define MAX_DIGIT_LEN 10

#define TEST_SET_FILE(prefix, rank) { \
    char *fname = malloc( strlen(prefix) + MAX_DIGIT_LEN + 2 ); \
    sprintf(fname, "%s.%d", prefix, rank); \
    file = fopen(fname, "w"); \
    free(fname); \
    if( NULL == file ){ \
        fprintf(stderr, "Cannot open file %s for writing!", fname); \
        exit(1); \
    } \
}

#define TEST_CLOSE_FILE() { \
    if ( stderr != file ) { \
        fclose(file); \
    } \
}

typedef struct {
    char *binary;
    char *np;
    char *prefix;
    char *nspace;
    uint32_t nprocs;
    int timeout;
    int barrier;
    int collect;
    int nonblocking;
    int verbose;
} test_params;

#define INIT_TEST_PARAMS(params) do { \
    params.nprocs = 1;                \
    params.barrier = 0;               \
    params.collect = 0;               \
    params.nonblocking = 0;           \
    params.verbose = 0;               \
    params.binary = NULL;             \
    params.np = NULL;                 \
    params.timeout = TEST_DEFAULT_TIMEOUT; \
    params.prefix = NULL;             \
    params.nspace = NULL;             \
} while (0)

#define FREE_TEST_PARAMS(params) do { \
    if (NULL != params.binary) {      \
        free(params.binary);          \
    }                                 \
    if (NULL != params.np) {          \
        free(params.np);              \
    }                                 \
    if (NULL != params.prefix) {      \
        free(params.prefix);          \
    }                                 \
    if (NULL != params.nspace) {      \
        free(params.nspace);          \
    }                                 \
} while (0)

void parse_cmd(int argc, char **argv, test_params *params);

#endif // TEST_COMMON_H
