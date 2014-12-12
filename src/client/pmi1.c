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
#ifdef HAVE_STDLIB_H
#include <stdlib.h>
#endif

#include "src/api/pmix.h"
#include "src/api/pmi.h"

#include "src/buffer_ops/buffer_ops.h"
#include "event.h"

#include "src/util/argv.h"
#include "src/util/error.h"
#include "src/util/output.h"
#include "src/util/progress_threads.h"

/* local variables */
static char *namespace;
static int rank;

/* local functions */
static int convert_int(int *value, pmix_value_t *kv);
static int convert_int_array(int *value, pmix_value_t *kv);

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
    pmix_value_t *kv;
    int rc;
    
    if (NULL != size && PMIx_Get_attr(namespace, PMIX_JOB_SIZE, &kv)) {
        rc = convert_int(size, kv);
        OBJ_RELEASE(kv);
        return rc;
}
    
return PMI_FAIL;
}

int PMI_Get_rank(int *rank)
{
    pmix_value_t *kv;
    int rc;
    
    if (NULL != rank && PMIx_Get_attr(namespace, PMIX_RANK, &kv)) {
        rc = convert_int(rank, kv);
        OBJ_RELEASE(kv);
        return rc;
    }
    
    return PMI_FAIL;
}

int PMI_Get_universe_size( int *size )
{
    pmix_value_t *kv;
    int rc;
    
    if (NULL != size && PMIx_Get_attr(namespace, PMIX_UNIV_SIZE, &kv)) {
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
    
    if (NULL != appnum && PMIx_Get_attr(namespace, PMIX_APPNUM, &kv)) {
        rc = convert_int(appnum, kv);
        OBJ_RELEASE(kv);
        return rc;
    }
    
    return PMI_FAIL;
}

int PMI_Publish_name(const char service_name[], const char port[])
{
    int rc;
    pmix_list_t info;
    pmix_info_t *pt;

if (NULL == service_name || NULL == port) {

    /* create an info object to pass the service/port */
    OBJ_CONSTRUCT(&info, pmix_list_t);
    pt = OBJ_NEW(pmix_info_t);
    (void)strncpy(pt->key, PMIX_SERVICE, PMIX_MAX_KEYLEN);
    (void)strncpy(pt->value, service_name, PMIX_MAX_VALLEN);
    pmix_list_append(&info, &pt->super);
    (void)strncpy(pt->key, PMIX_PORT, PMIX_MAX_KEYLEN);
    (void)strncpy(pt->value, port, PMIX_MAX_VALLEN);
    pmix_list_append(&info, &pt->super);
    
    /* publish the info - PMI-1 doesn't support
     * any scope other than inside our own namespace */
    rc = PMIx_Publish(PMIX_NAMESPACE, &info);
    PMIX_LIST_DESTRUCT(&info);
    
    return rc;
}

int PMI_Unpublish_name(const char service_name[])
{
    int rc;
    pmix_list_t info;
    pmix_info_t *pt;
    
    /* create an info object to pass the service */
    OBJ_CONSTRUCT(&info, pmix_list_t);
    pt = OBJ_NEW(pmix_info_t);
    (void)strncpy(pt->key, PMIX_SERVICE, PMIX_MAX_KEYLEN);
    (void)strncpy(pt->value, service_name, PMIX_MAX_VALLEN);
    pmix_list_append(&info, &pt->super);
    
    rc = PMIx_Unpublish(&info);
    PMIX_LIST_DESTRUCT(&info);
    
    return rc;
}

int PMI_Lookup_name(const char service_name[], char port[])
{
    int rc;
    pmix_list_t info;
    pmix_info_t *pt;
    
    /* create an info object to pass the service */
    OBJ_CONSTRUCT(&info, pmix_list_t);
    pt = OBJ_NEW(pmix_info_t);
    (void)strncpy(pt->key, PMIX_SERVICE, PMIX_MAX_KEYLEN);
    (void)strncpy(pt->value, service_name, PMIX_MAX_VALLEN);
    pmix_list_append(&info, &pt->super);

    /* PMI-1 doesn't want the namespace back */
    if (PMIX_SUCCESS != (rc = PMIx_Lookup(&info, NULL))) {
        PMIX_LIST_DESTRUCT(&info);
        return rc;
    }

    /* return the port */
    pt = (pmix_info_t*)pmix_list_get_first(&info);
    (void)strncpy(port, pt->value, PMIX_MAX_VALLEN);
    PMIX_LIST_DESTRUCT(&info);

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
    
    if (PMIx_Get_attr(namespace, PMIX_LOCAL_SIZE, &kv)) {
        rc = convert_int(size, kv);
        OBJ_RELEASE(kv);
        return rc;
    }
    
    return PMI_FAIL;
}

int PMI_Get_clique_ranks(int ranks[], int length)
{
    pmix_value_t *kv;
    char **rks;
    int i;
    
    if (PMIx_Get_attr(namespace, PMIX_LOCAL_PEERS, &kv)) {
        /* kv will contain a string of comma-separated
         * ranks on my node */
        rks = pmix_argv_split(kv->data.string, ',');
        for (i=0; NULL != rks[i] && i < length; i++) {
            ranks[i] = strtol(rks[i], NULL, 10);
        }
        pmix_argv_free(rks);
        OBJ_RELEASE(kv);
        return PMI_SUCCESS;
    }
    return PMI_SUCCESS;
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
    pmix_list_t info;
    pmix_list_t preput;
    pmix_app_t *pt;

    /* convert the incoming PMI_keyval_t structs to pmix_info_t objects */
    OBJ_CONSTRUCT(&info, pmix_list_t);
    for (i=0; i < count; i++) {
        pt = OBJ_NEW(pmix_app_t);
        pt->cmd = strdup(cmds[i]);
        pt->argv = opal_argv_copy(argvs[i]);
    }
    return PMI_SUCCESS;
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

/* internal function */
static int convert_int_array(int value[], int length,
                             pmix_value_t *kv)
{
   int i;
   
   if (kv->type == PMIX_INT_ARRAY) {
       for (i = 0; i < length && i < kv->data.integer_array.size; i++) {
           value[i] = kv->data.integer_array.data[i];
       }
   } else if (kv->type == PMIX_INT8_ARRAY) {
       for (i = 0; i < length && i < kv->data.integer_array.size; i++) {
           value[i] = kv->data.int8_array.data[i];
       }
   } else if (kv->type == PMIX_INT16_ARRAY) {
       for (i = 0; i < length && i < kv->data.integer_array.size; i++) {
           value[i] = kv->data.int16_array.data[i];
       }
   } else if (kv->type == PMIX_INT32_ARRAY) {
       for (i = 0; i < length && i < kv->data.integer_array.size; i++) {
           value[i] = kv->data.int32_array.data[i];
       }
   } else if (kv->type == PMIX_INT64_ARRAY) {
       for (i = 0; i < length && i < kv->data.integer_array.size; i++) {
           value[i] = kv->data.int64_array.data[i];
       }
   } else if (kv->type == PMIX_UINT_ARRAY) {
       for (i = 0; i < length && i < kv->data.integer_array.size; i++) {
           value[i] = kv->data.uint_array.data[i];
       }
   } else if (kv->type == PMIX_UINT8_ARRAY) {
       for (i = 0; i < length && i < kv->data.integer_array.size; i++) {
           value[i] = kv->data.uint8_array.data[i];
       }
   } else if (kv->type == PMIX_UINT16_ARRAY) {
       for (i = 0; i < length && i < kv->data.integer_array.size; i++) {
           value[i] = kv->data.uint16_array.data[i];
       }
   } else if (kv->type == PMIX_UINT32_ARRAY) {
       for (i = 0; i < length && i < kv->data.integer_array.size; i++) {
           value[i] = kv->data.uint32_array.data[i];
       }
   } else if (kv->type == PMIX_UINT64_ARRAY) {
       for (i = 0; i < length && i < kv->data.integer_array.size; i++) {
           value[i] = kv->data.uint64_array.data[i];
       }
   } else if (kv->type == PMIX_BYTE_ARRAY) {
       for (i = 0; i < length && i < kv->data.integer_array.size; i++) {
           value[i] = kv->data.byte_array.data[i];
       }
   } else if (kv->type == PMIX_SIZE_ARRAY) {
       for (i = 0; i < length && i < kv->data.integer_array.size; i++) {
           value[i] = kv->data.size_array.data[i];
       }
   } else {
       /* not an integer array type */
       return PMI_FAIL;
   }
   
   return PMI_SUCCESS;
}
