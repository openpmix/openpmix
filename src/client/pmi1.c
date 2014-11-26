/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2014      Intel, Inc.  All rights reserved.
 * Copyright (c) 2014      Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "pmix_config.h"
#include "constants.h"
#include "types.h"

#ifdef HAVE_STRING_H
#include <string.h>
#endif
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#include "src/api/pmix.h"
#include "src/api/pmi.h"

#include "src/buffer_ops/buffer_ops.h"
#include "event.h"

#include "src/util/error.h"
#include "src/util/output.h"
#include "src/util/progress_threads.h"

/* local variables */
static char *namespace;
static int rank;

int PMI_Init( int *spawned )
{
    return PMIx_Init(&namespace, &rank);
}

int PMI_Initialized(PMI_BOOL *initialized)
{
    *initialized = (PMI_BOOL)PMIx_Initialized();
    return PMI_SUCCESS;
}

int PMI_Finalize(void)
{
    return PMIx_Finalize();
}

int PMI_Abort(int flag, const char msg[])
{
    return PMIx_Abort(flag, msg);
}

/* KVS_Put - we default to PMIX_GLOBAL scope and ignore the
 * provided kvsname as we only put into our own namespace */
int PMI_KVS_Put(const char kvsname[], const char key[], const char value[])
{
    int rc;
    pmix_value_t val;

    OBJ_CONSTRUCT(&val, pmix_value_t);
    val.key = strdup(key);
    val.type = PMIX_STRING;
    val.data.string = strdup(value);

    rc = PMIx_Put(PMIX_GLOBAL, &val);
    OBJ_DESTRUCT(&val);
    return rc;
}

/* KVS_Commit - PMIx has no equivalent operation as any Put
 * values are locally cached and transmitted upon Fence */
int PMI_KVS_Commit(const char kvsname[])
{
    return PMI_SUCCESS;
}

/* Barrier only applies to our own namespace */
int PMI_Barrier(void)
{
    return PMIx_Fence(NULL);
}

int PMI_Get_size(int *size)
{
    return PMI_SUCCESS;
}

int PMI_Get_rank(int *rank)
{
    return PMI_SUCCESS;
}

int PMI_Get_universe_size( int *size )
{
    return PMI_SUCCESS;
}

int PMI_Get_appnum(int *appnum)
{
    return PMI_SUCCESS;
}

int PMI_Publish_name(const char service_name[], const char port[])
{
    int rc;

    /* create an info object to pass the port */
    
    /* pass the service name, the port, and our namespace */
    rc = PMIx_Publish(service_name, NULL, namespace);
    return rc;
}

int PMI_Unpublish_name(const char service_name[])
{
    return PMI_SUCCESS;
}

int PMI_Lookup_name(const char service_name[], char port[])
{
    return PMI_SUCCESS;
}

int PMI_Get_id(char id_str[], int length)
{
    return PMI_SUCCESS;
}

int PMI_Get_kvs_domain_id(char id_str[], int length)
{
    return PMI_SUCCESS;
}

int PMI_Get_id_length_max(int *length)
{
    return PMI_SUCCESS;
}

int PMI_Get_clique_size(int *size)
{
    return PMI_SUCCESS;
}

int PMI_Get_clique_ranks(int ranks[], int length)
{
    return PMI_SUCCESS;
}

int PMI_KVS_Get_my_name(char kvsname[], int length)
{
    return PMI_SUCCESS;
}

int PMI_KVS_Get_name_length_max(int *length)
{
    return PMI_SUCCESS;
}

int PMI_KVS_Get_key_length_max(int *length)
{
    return PMI_SUCCESS;
}

int PMI_KVS_Get_value_length_max(int *length)
{
    return PMI_SUCCESS;
}

int PMI_KVS_Create(char kvsname[], int length)
{
    return PMI_SUCCESS;
}

int PMI_KVS_Destroy(const char kvsname[])
{
    return PMI_SUCCESS;
}

int PMI_KVS_Iter_first(const char kvsname[], char key[], int key_len, char val[], int val_len)
{
    return PMI_SUCCESS;
}

int PMI_KVS_Iter_next(const char kvsname[], char key[], int key_len, char val[], int val_len)
{
    return PMI_SUCCESS;
}

int PMI_Spawn_multiple(int count,
                       const char * cmds[],
                       const char ** argvs[],
                       const int maxprocs[],
                       const int info_keyval_sizesp[],
                       const PMI_keyval_t * info_keyval_vectors[],
                       int preput_keyval_size,
                       const PMI_keyval_t preput_keyval_vector[],
                       int errors[])
{
    return PMI_SUCCESS;
}

int PMI_Parse_option(int num_args, char *args[], int *num_parsed, PMI_keyval_t **keyvalp, int *size)
{
    return PMI_SUCCESS;
}

int PMI_Args_to_keyval(int *argcp, char *((*argvp)[]), PMI_keyval_t **keyvalp, int *size)
{
    return PMI_SUCCESS;
}

int PMI_Free_keyvals(PMI_keyval_t keyvalp[], int size)
{
    return PMI_SUCCESS;
}

int PMI_Get_options(char *str, int *length)
{
    return PMI_SUCCESS;
}
