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
 * Copyright (c) 2013-2016 Intel, Inc.  All rights reserved.
 * Copyright (c) 2015      Mellanox Technologies, Inc.  All rights reserved.
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
#include <time.h>
#include <getopt.h>

#include "pmi.h"

#include <time.h>
#define GET_TS ({ \
    struct timespec ts;                     \
    double ret;                             \
    clock_gettime(CLOCK_MONOTONIC, &ts);    \
    ret = ts.tv_sec + 1E-9 * ts.tv_nsec;    \
    ret;                                    \
})


int key_size = 100, key_count = 10, rank_shift;
int direct_modex = 0;

static void usage(const char *argv0)
{
    printf("Usage:\n");
    printf("  %s [options]              start the benchmark\n", argv0);
    printf("\n");
    printf("  -s, --key-size=<size>     size of the key's submitted\n");
    printf("  -c, --key-count=<size>    number of keys submitted to local and remote parts\n");
    printf("  -d, --direct-modex        use direct modex if available\n");
}


void parse_options(int argc, char **argv)
{
    extern char *optarg;
    extern int optind;
    struct option long_options[] = {
        { "help",           0, NULL, 'h' },
        /* IB options */
        { "key-size",       1, NULL, 's' },
        { "key-count",      1, NULL, 'c' },
        { "direct-modex",   1, NULL, 'd' },
        { 0 }
    };

    while (1) {
        int c;
        c = getopt_long(argc, argv, "hs:c:d", long_options, NULL);

        if (c == -1)
            break;
        switch (c) {
        case 's':
            key_size = atoi(optarg);
            break;
        case 'c':
            key_count = atoi(optarg);
            break;
        case 'd':
            direct_modex = 1;
            break;
        case 'h':
        default:
            usage(argv[0]);
            exit(0);
        }
    }

    rank_shift = 10;
    while( rank_shift <= key_count ){
        rank_shift *= 10;
    }
}

void fill_remote_ranks(int *local_ranks, int local_cnt, int *remote_ranks, int size)
{
    int i, k;
    for(i = 0, k = 0; i < size && k < (size - local_cnt); i++ ){
        int j, flag = 1;
        for(j=0; j < local_cnt; j++){
            if( i == local_ranks[j] ){
                flag = 0;
                break;
            }
        }
        if( flag ){
            remote_ranks[k] = i;
            k++;
        }
    }
}

int main(int argc, char **argv)
{
    int rc;
    char *key_name;
    int *key_val;
    int rank, nproc;
    int cnt;
    int *local_ranks, local_cnt;
    int *remote_ranks, remote_cnt;
    double start, get_loc_time = 0, get_rem_time = 0, put_loc_time = 0, put_rem_time = 0, commit_time = 0, fence_time = 0;
    int get_loc_cnt = 0, get_rem_cnt = 0, put_loc_cnt = 0, put_rem_cnt = 0;

    parse_options(argc, argv);

    pmi_init(&rank, &nproc);
    
    pmi_get_local_ranks(&local_ranks, &local_cnt);
    remote_cnt = nproc - local_cnt;
    if( remote_cnt ){
        remote_ranks = calloc(remote_cnt, sizeof(int));
        fill_remote_ranks(local_ranks, local_cnt, remote_ranks, nproc);
    }

    if( 0 == rank ){
        int i;
        fprintf(stderr,"%d: local ranks: ", rank);
        for(i = 0; i < local_cnt; i++){
            fprintf(stderr,"%d ", local_ranks[i]);
        }
        fprintf(stderr,"\n");
        fflush(stderr);

        fprintf(stderr,"%d: put\n", rank);
        fflush(stderr);
    }

    key_val = calloc(key_size, sizeof(int));
    for (cnt=0; cnt < key_count; cnt++) {
        int i;
        if( local_cnt > 0 ){
            (void)asprintf(&key_name, "KEY-%d-local-%d", rank, cnt);
            for(i=0; i < key_size; i++){
                key_val[i] = rank * rank_shift + cnt;
            }
            put_loc_cnt++;
            start = GET_TS;
            pmi_put_key_loc(key_name, key_val, key_size);
            put_loc_time += GET_TS - start;
            free(key_name);
        }
        if( remote_cnt > 0 ){
            (void)asprintf(&key_name, "KEY-%d-remote-%d", rank, cnt);
            for(i=0; i < key_size; i++){
                key_val[i] = rank * rank_shift + cnt;
            }
            put_rem_cnt++;
            start = GET_TS;
            pmi_put_key_rem(key_name, key_val, key_size);
            put_rem_time += GET_TS - start;
            free(key_name);
        }
    }
    free(key_val);

    if( 0 == rank ){
        fprintf(stderr,"%d: fence\n", rank);
        fflush(stderr);
    }

    start = GET_TS;
    pmi_commit();
    commit_time += GET_TS - start;

    start = GET_TS;
    pmi_fence( !direct_modex );
    fence_time += GET_TS - start;

    for (cnt=0; cnt < key_count; cnt++) {
        int i;

        for(i = 0; i < remote_cnt; i++){
            int rank = remote_ranks[i], j;
            int *key_val, key_size_new;
            double start;
            (void)asprintf(&key_name, "KEY-%d-remote-%d", rank, cnt);

            start = GET_TS;
            pmi_get_key_rem(rank, key_name, &key_val, &key_size_new);
            get_rem_time += GET_TS - start;
            get_rem_cnt++;

            if( key_size != key_size_new ){
                fprintf(stderr,"%d: error in key %s sizes: %d vs %d\n",
                        rank, key_name, key_size, key_size_new);
                abort();
            }

            for(j=0; j < key_size; j++){
                if( key_val[j] != rank * rank_shift + cnt ){
                    fprintf(stderr, "%d: error in key %s value (byte %d)\n",
                            rank, key_name, j);
                    abort();
                }
            }
            free(key_name);
            free(key_val);
        }

         // check the returned data
        for(i = 0; i < local_cnt; i++){
            int rank = local_ranks[i], j;
            int *key_val, key_size_new;
            double start;
            (void)asprintf(&key_name, "KEY-%d-local-%d", rank, cnt);

            start = GET_TS;
            pmi_get_key_loc(rank, key_name, &key_val, &key_size_new);
            get_loc_time += GET_TS - start;
            get_loc_cnt++;

            if( key_size != key_size_new ){
                fprintf(stderr,"%d: error in key %s sizes: %d vs %d\n",
                        rank, key_name, key_size, key_size_new);
                abort();
            }


            for(j=0; j < key_size; j++){
                if( key_val[j] != rank * rank_shift + cnt ){
                    fprintf(stderr, "%d: error in key %s value (byte %d)",
                            rank, key_name, j);
                    abort();
                }
            }
            free(key_name);
            free(key_val);
        }


    }

    if( 0 == rank ){
        fprintf(stderr,"%d: fini\n", rank);
        fflush(stderr);
    }
    pmi_fini();

    if( 0 == rank ) {
        fprintf(stderr,"%d: exit\n", rank);
        fflush(stderr);
    }
    
    fprintf(stderr,"%d: get: total %lf avg loc %lf rem %lf all %lf ; put: %lf %lf commit: %lf fence %lf\n",
            rank, (get_loc_time + get_rem_time), 
            get_loc_time/get_loc_cnt, get_rem_time/get_rem_cnt,
            (get_loc_time + get_rem_time)/(get_loc_cnt + get_rem_cnt),
            put_loc_time/put_loc_cnt, put_rem_time/put_rem_cnt,
            commit_time, fence_time);

    return 0;
}
