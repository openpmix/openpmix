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

int PMI_Init( int *spawned )
{
    return PMIx_Init(spawned);
}

int PMI_Initialized(PMI_BOOL *initialized)
{
    initialized = PMIx_Initialized();
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
    return PMIx_Job_spawn(count, cmds,
                          argcs, argvs,
                          maxprocs,
                          info_keyval_vector,
                          preput_keyval_vector,
                          jobId, jobIdSize,
                          errors);
}

/* KVS_Put - PMIx doesn't use the kvsname, so it is ignored here. The string
 * value is repacked as a pmix_value_t and passed to PMIx_Put. We default
 * to PMIX_GLOBAL scope. */
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
 * values are automatically locally committed and queued
 * for transmission upon Fence */
int PMI_KVS_Commit(const char kvsname[])
{
    return PMI_SUCCESS;
}

int PMI_Barrier(void)
{
    return PMIx_Fence(NULL, 0);
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
    return PMI_SUCCESS;
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

// int PMI_KVS_Get(const char kvsname[], const char key[], char value[], int length)
int PMIx_Get(const pmix_identifier_t *id,
             const char *key,
             pmix_value_t **kv)
{
    pmix_buffer_t *msg, *bptr;
    pmix_cmd_t cmd = PMIX_GET_CMD;
    pmix_cb_t *cb;
    int rc, ret;
    int32_t cnt;
    pmix_list_t vals;
    pmix_value_t *kp;
    bool found;

    pmix_output_verbose(2, pmix_debug_output,
                        "%s pmix:native getting value for proc %s key %s",
                        PMIX_NAME_PRINT(PMIX_PROC_MY_NAME),
                        PMIX_NAME_PRINT(*id), key);

    /* first see if we already have the info in our dstore */
    OBJ_CONSTRUCT(&vals, pmix_list_t);
    if (PMIX_SUCCESS == pmix_dstore.fetch(pmix_dstore_internal, id,
                                          key, &vals)) {
        *kv = (pmix_value_t*)pmix_list_remove_first(&vals);
        PMIX_LIST_DESTRUCT(&vals);
        pmix_output_verbose(2, pmix_debug_output,
                            "%s pmix:native value retrieved from dstore",
                            PMIX_NAME_PRINT(PMIX_PROC_MY_NAME));
        return PMIX_SUCCESS;
    }

    if (NULL == mca_pmix_native_component.uri) {
        /* no server available, so just return */
        return PMIX_ERR_NOT_FOUND;
    }

    /* nope - see if we can get it */
    msg = OBJ_NEW(pmix_buffer_t);
    /* pack the get cmd */
    if (PMIX_SUCCESS != (rc = pmix_dss.pack(msg, &cmd, 1, PMIX_CMD_T))) {
        PMIX_ERROR_LOG(rc);
        OBJ_RELEASE(msg);
        return rc;
    }
    /* pack the request information - we'll get the entire blob
     * for this proc, so we don't need to pass the key */
    if (PMIX_SUCCESS != (rc = pmix_dss.pack(msg, id, 1, PMIX_UINT64))) {
        PMIX_ERROR_LOG(rc);
        OBJ_RELEASE(msg);
        return rc;
    }

    /* create a callback object as we need to pass it to the
     * recv routine so we know which callback to use when
     * the return message is recvd */
    cb = OBJ_NEW(pmix_cb_t);
    cb->active = true;

    /* push the message into our event base to send to the server */
    PMIX_ACTIVATE_SEND_RECV(msg, wait_cbfunc, cb);

    /* wait for the data to return */
    PMIX_WAIT_FOR_COMPLETION(cb->active);

    /* we have received the entire data blob for this process - unpack
     * and cache all values, keeping the one we requested to return
     * to the caller */
    cnt = 1;
    if (PMIX_SUCCESS != (rc = pmix_dss.unpack(&cb->data, &ret, &cnt, PMIX_INT))) {
        PMIX_ERROR_LOG(rc);
        OBJ_RELEASE(cb);
        return rc;
    }
    found = false;
    cnt = 1;
    while (PMIX_SUCCESS == (rc = pmix_dss.unpack(&cb->data, &bptr, &cnt, PMIX_BUFFER))) {
        while (PMIX_SUCCESS == (rc = pmix_dss.unpack(bptr, &kp, &cnt, PMIX_VALUE))) {
            pmix_output_verbose(2, pmix_debug_output,
                                "%s pmix:native retrieved %s (%s) from server for proc %s",
                                PMIX_NAME_PRINT(PMIX_PROC_MY_NAME), kp->key,
                                (PMIX_STRING == kp->type) ? kp->data.string : "NS",
                                PMIX_NAME_PRINT(*id));
            if (PMIX_SUCCESS != (ret = pmix_dstore.store(pmix_dstore_internal, id, kp))) {
                PMIX_ERROR_LOG(ret);
            }
            if (0 == strcmp(key, kp->key)) {
                *kv = kp;
                found = true;
            } else {
                OBJ_RELEASE(kp);
            }
        }
        if (PMIX_ERR_UNPACK_READ_PAST_END_OF_BUFFER != rc) {
            PMIX_ERROR_LOG(rc);
        }
        OBJ_RELEASE(bptr);
        cnt = 1;
    }
    if (PMIX_ERR_UNPACK_READ_PAST_END_OF_BUFFER != rc) {
        PMIX_ERROR_LOG(rc);
    } else {
        rc = PMIX_SUCCESS;
    }
    OBJ_RELEASE(cb);

    pmix_output_verbose(2, pmix_debug_output,
                        "%s pmix:native get completed",
                        PMIX_NAME_PRINT(PMIX_PROC_MY_NAME));
    if (found) {
        return PMIX_SUCCESS;
    }
    /* we didn't find the requested data - pass back a
     * status that indicates the source of the problem,
     * either during the data fetch, message unpacking,
     * or not found */
    *kv = NULL;
    if (PMIX_SUCCESS == rc) {
        if (PMIX_SUCCESS == ret) {
            rc = PMIX_ERR_NOT_FOUND;
        } else {
            rc = ret;
        }
    }
    return rc;
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
