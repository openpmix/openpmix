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
#include "src/include/types.h"

#ifdef HAVE_STRING_H
#include <string.h>
#endif
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#ifdef HAVE_STDLIB_H
#include <stdlib.h>
#endif
#include <event.h>

#include "src/api/pmix.h"
#include "src/api/pmi.h"

#include "src/buffer_ops/buffer_ops.h"

#include "src/util/argv.h"
#include "src/util/error.h"
#include "src/util/output.h"
#include "src/util/progress_threads.h"

/* local variables */
static char namespace[PMIX_MAX_VALLEN];
static int rank;

/* local functions */
static int convert_int(int *value, pmix_value_t *kv);

int PMI_Init( int *spawned )
{
    if (PMIX_SUCCESS == PMIx_Init(namespace, &rank)) {
        return PMI_SUCCESS;
    }
    return PMI_ERR_INIT;
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

    val.type = PMIX_STRING;
    val.data.string = (char*)value;
    rc = PMIx_Put(PMIX_GLOBAL, key, &val);
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
    pmix_range_t range;

    (void)strncpy(range.namespace, namespace, PMIX_MAX_VALLEN);
    range.ranks = NULL;
    range.nranks = 0;
    return PMIx_Fence(&range, 1);
}

int PMI_Get_size(int *size)
{
    pmix_value_t *kv;
    int rc;
    
    if (NULL == size) {
        return PMI_FAIL;
    }
    
    if (PMIX_SUCCESS == PMIx_Get(namespace, rank,
                                 PMIX_JOB_SIZE, &kv)) {
        rc = convert_int(size, kv);
        OBJ_RELEASE(kv);
        return rc;
    }
    
    return PMI_FAIL;
}

int PMI_Get_rank(int *rk)
{
    if (NULL == rk) {
        return PMI_FAIL;
    }
    
    *rk = rank;
    return PMI_SUCCESS;
}

int PMI_Get_universe_size(int *size)
{
    pmix_value_t *kv;
    int rc;
    
    if (NULL == size) {
        return PMI_FAIL;
    }
    
    if (PMIX_SUCCESS == PMIx_Get(namespace, rank,
                                 PMIX_UNIV_SIZE, &kv)) {
        rc = convert_int(size, kv);
        OBJ_RELEASE(kv);
        return rc;
    }
    return PMI_FAIL;
}

int PMI_Get_appnum(int *appnum)
{
    pmix_value_t *kv;
    int rc;
    
    if (NULL != appnum && PMIx_Get(namespace, rank,
                                   PMIX_APPNUM, &kv)) {
        rc = convert_int(appnum, kv);
        OBJ_RELEASE(kv);
        return rc;
    }
    
    return PMI_FAIL;
}

int PMI_Publish_name(const char service_name[], const char port[])
{
    int rc;
    pmix_info_t info;
    
    if (NULL == service_name || NULL == port) {
        return PMIX_ERR_BAD_PARAM;
    }
    /* pass the service/port */
    (void)strncpy(info.key, service_name, PMIX_MAX_KEYLEN);
    info.value = (pmix_value_t*)malloc(sizeof(pmix_value_t));
    info.value->type = PMIX_STRING;
    info.value->data.string = (char*)port;
    
    /* publish the info - PMI-1 doesn't support
     * any scope other than inside our own namespace */
    rc = PMIx_Publish(PMIX_NAMESPACE, &info, 1);
    free(info.value);
    
    return rc;
}

int PMI_Unpublish_name(const char service_name[])
{
    pmix_info_t info;
    
    /* pass the service */
    (void)strncpy(info.key, service_name, PMIX_MAX_KEYLEN);
    
    return PMIx_Unpublish(&info, 1);
}

int PMI_Lookup_name(const char service_name[], char port[])
{
    int rc;
    pmix_info_t info;
    
    /* pass the service */
    (void)strncpy(info.key, service_name, PMIX_MAX_KEYLEN);

    /* PMI-1 doesn't want the namespace back */
    if (PMIX_SUCCESS != (rc = PMIx_Lookup(&info, 1, NULL))) {
        return rc;
    }

    /* should have received a string back */
    if (NULL == info.value || PMIX_STRING != info.value->type ||
        NULL == info.value->data.string) {
        if (NULL != info.value) {
            free(info.value);
        }
        return PMIX_ERR_NOT_FOUND;
    }
    
    /* return the port */
    (void)strncpy(port, info.value->data.string, PMIX_MAX_VALLEN);
    free(info.value->data.string);
    free(info.value);
    
    return PMIX_SUCCESS;
}

int PMI_Get_id(char id_str[], int length)
{
    /* we already obtained our namespace during PMI_Init,
     * so all we have to do here is return it */

    /* bozo check */
    if (NULL == id_str) {
        return PMI_ERR_INVALID_ARGS;
    }
    (void)strncpy(id_str, namespace, length);
    return PMI_SUCCESS;
}

int PMI_Get_kvs_domain_id(char id_str[], int length)
{
    /* same as PMI_Get_id */
    return PMI_Get_id(id_str, length);
}

int PMI_Get_id_length_max(int *length)
{
    if (NULL == length) {
        return PMI_ERR_INVALID_VAL_LENGTH;
    }
    *length = PMIX_MAX_VALLEN;
    return PMI_SUCCESS;
}

int PMI_Get_clique_size(int *size)
{
    pmix_value_t *kv;
    int rc;
    
    if (PMIX_SUCCESS == PMIx_Get(namespace, rank,
                                 PMIX_LOCAL_SIZE, &kv)) {
        rc = convert_int(size, kv);
        free(kv);
        return rc;
    }
    
    return PMI_FAIL;
}

int PMI_Get_clique_ranks(int ranks[], int length)
{
    pmix_value_t *kv;
    char **rks;
    int i;
    
    if (PMIX_SUCCESS == PMIx_Get(namespace, rank, PMIX_LOCAL_PEERS, &kv)) {
        /* kv will contain a string of comma-separated
         * ranks on my node */
        rks = pmix_argv_split(kv->data.string, ',');
        for (i=0; NULL != rks[i] && i < length; i++) {
            ranks[i] = strtol(rks[i], NULL, 10);
        }
        pmix_argv_free(rks);
        free(kv);
        return PMI_SUCCESS;
    }
    return PMI_FAIL;
}

int PMI_KVS_Get_my_name(char kvsname[], int length)
{
    /* same as PMI_Get_id */
    return PMI_Get_id(kvsname, length);
}

int PMI_KVS_Get_name_length_max(int *length)
{
    /* same as PMI_Get_id_length_max */
    return PMI_Get_id_length_max(length);
}

int PMI_KVS_Get_key_length_max(int *length)
{
    if (NULL == length) {
        return PMI_ERR_INVALID_VAL_LENGTH;
    }
    *length = PMIX_MAX_KEYLEN;
    return PMI_SUCCESS;
}

int PMI_KVS_Get_value_length_max(int *length)
{
   if (NULL == length) {
        return PMI_ERR_INVALID_VAL_LENGTH;
    }
    *length = PMIX_MAX_VALLEN;
    return PMI_SUCCESS;
}

/* nobody supports this call, which is why it was
 * dropped for PMI-2 */
int PMI_KVS_Create(char kvsname[], int length)
{
    return PMI_FAIL;
}

/* nobody supports this call, which is why it was
 * dropped for PMI-2 */
int PMI_KVS_Destroy(const char kvsname[])
{
    return PMI_FAIL;
}

/* nobody supports this call, which is why it was
 * dropped for PMI-2 */
int PMI_KVS_Iter_first(const char kvsname[], char key[], int key_len, char val[], int val_len)
{
    return PMI_FAIL;
}

/* nobody supports this call, which is why it was
 * dropped for PMI-2 */
int PMI_KVS_Iter_next(const char kvsname[], char key[], int key_len, char val[], int val_len)
{
    return PMI_FAIL;
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
    pmix_app_t *apps;
    int i, k, rc;
    size_t j;
    char *evar;
    
    /* setup the apps */
    apps = (pmix_app_t*)malloc(count * sizeof(pmix_app_t));
    for (i=0; i < count; i++) {
        apps[i].cmd = strdup(cmds[i]);
        apps[i].maxprocs = maxprocs[i];
        apps[i].argv = pmix_argv_copy((char**)argvs[i]);
        apps[i].argc = pmix_argv_count(apps[i].argv);
        apps[i].ninfo = info_keyval_sizesp[i];
        apps[i].info = (pmix_info_t*)malloc(apps[i].ninfo * sizeof(pmix_info_t));
        /* copy the info objects */
        for (j=0; j < apps[i].ninfo; j++) {
            (void)strncpy(apps[i].info[j].key, info_keyval_vectors[i][j].key, PMIX_MAX_KEYLEN);
            apps[i].info[j].value = (pmix_value_t*)malloc(sizeof(pmix_value_t));
            apps[i].info[j].value->type = PMIX_STRING;
            (void)strncpy(apps[i].info[j].value->data.string, info_keyval_vectors[i][j].key, PMIX_MAX_VALLEN);
        }
        /* push the preput values into the apps environ */
        for (k=0; k < preput_keyval_size; k++) {
            (void)asprintf(&evar, "%s=%s", preput_keyval_vector[j].key, preput_keyval_vector[j].val);
            pmix_argv_append_nosize(&apps[i].env, evar);
            free(evar);
        }
    }

    rc = PMIx_Spawn(apps, count, namespace);
    /* tear down the apps array */
    
    return rc;
}

/* nobody supports this call, which is why it was
 * dropped for PMI-2 */
int PMI_Parse_option(int num_args, char *args[], int *num_parsed, PMI_keyval_t **keyvalp, int *size)
{
    return PMI_FAIL;
}

/* nobody supports this call, which is why it was
 * dropped for PMI-2 */
int PMI_Args_to_keyval(int *argcp, char *((*argvp)[]), PMI_keyval_t **keyvalp, int *size)
{
    return PMI_FAIL;
}

/* nobody supports this call, which is why it was
 * dropped for PMI-2 */
int PMI_Free_keyvals(PMI_keyval_t keyvalp[], int size)
{
    return PMI_FAIL;
}

/* nobody supports this call, which is why it was
 * dropped for PMI-2 */
int PMI_Get_options(char *str, int *length)
{
    return PMI_FAIL;
}


/***   UTILITY FUNCTIONS   ***/
/* internal function */
static int convert_int(int *value, pmix_value_t *kv)
{
   if (kv->type == PMIX_INT) {
       *value = kv->data.integer;
   } else if (kv->type == PMIX_INT8) {
       *value = kv->data.int8;
   } else if (kv->type == PMIX_INT16) {
       *value = kv->data.int16;
   } else if (kv->type == PMIX_INT32) {
       *value = kv->data.int32;
   } else if (kv->type == PMIX_INT64) {
       *value = kv->data.int64;
   } else if (kv->type == PMIX_UINT) {
       *value = kv->data.uint;
   } else if (kv->type == PMIX_UINT8) {
       *value = kv->data.uint8;
   } else if (kv->type == PMIX_UINT16) {
       *value = kv->data.uint16;
   } else if (kv->type == PMIX_UINT32) {
       *value = kv->data.uint32;
   } else if (kv->type == PMIX_UINT64) {
       *value = kv->data.uint64;
   } else if (kv->type == PMIX_BYTE) {
       *value = kv->data.byte;
   } else if (kv->type == PMIX_SIZE) {
       *value = kv->data.size;
   } else {
       /* not an integer type */
       return PMI_FAIL;
   }
   return PMI_SUCCESS;
}
