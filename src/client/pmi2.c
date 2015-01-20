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
#include "src/api/pmix.h"
#include "src/api/pmi2.h"
#include "src/include/types.h"
#include "src/include/pmix_globals.h"

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

#include "src/buffer_ops/buffer_ops.h"
#include "src/util/argv.h"
#include "src/util/error.h"
#include "src/util/output.h"

/* local functions */
static pmix_status_t convert_int(int *value, pmix_value_t *kv);
static int convert_err(pmix_status_t rc);

int PMI2_Init(int *spawned, int *size, int *rank, int *appnum)
{
    pmix_value_t *kv;
    pmix_status_t rc;

    if (PMIX_SUCCESS != PMIx_Init(NULL, NULL)) {
        return PMI2_ERR_INIT;
    }

    if (NULL != rank) {
        *rank = pmix_globals.rank;
    }

    if (NULL != size) {
        /* get the universe size - this will likely pull
         * down all attributes assigned to the job, thus
         * making all subsequent "get" operations purely
         * local */
        if (PMIX_SUCCESS == PMIx_Get(NULL, pmix_globals.rank,
                                     PMIX_UNIV_SIZE, &kv)) {
            rc = convert_int(size, kv);
            PMIx_free_value(&kv);
            return convert_err(rc);
        } else {
            /* cannot continue without this info */
            return PMI2_ERR_INIT;
        }
    }

    if (NULL != spawned) {
        /* get the spawned flag */
        if (PMIX_SUCCESS == PMIx_Get(NULL, pmix_globals.rank,
                                     PMIX_SPAWNED, &kv)) {
            rc = convert_int(spawned, kv);
            PMIx_free_value(&kv);
            return convert_err(rc);
        } else {
            /* if not found, default to not spawned */
            *spawned = 0;
        }
    }
    
    if (NULL != appnum) {
        /* get our appnum */
        if (PMIX_SUCCESS == PMIx_Get(NULL, pmix_globals.rank,
                                     PMIX_APPNUM, &kv)) {
            rc = convert_int(appnum, kv);
            PMIx_free_value(&kv);
            return convert_err(rc);
        } else {
            /* if not found, default to 0 */
            *appnum = 0;
        }
    }
    
    return PMI2_SUCCESS;
}

int PMI2_Initialized(void)
{
    int initialized;
    initialized = (int)PMIx_Initialized();
    return initialized;
}

int PMI2_Finalize(void)
{
    pmix_status_t rc;
    
    rc = PMIx_Finalize();
    return convert_err(rc);
}

int PMI2_Abort(int flag, const char msg[])
{
    pmix_status_t rc;
    
    rc = PMIx_Abort(flag, msg);
    return convert_err(rc);
}

/* KVS_Put - we default to PMIX_GLOBAL scope */
int PMI2_KVS_Put(const char key[], const char value[])
{
    pmix_status_t rc;
    pmix_value_t val;

    val.type = PMIX_STRING;
    val.data.string = (char*)value;
    rc = PMIx_Put(PMIX_GLOBAL, key, &val);
    return convert_err(rc);
}

/* KVS_Fence */
int PMI2_KVS_Fence(void)
{
    pmix_status_t rc;

    /* we want all data to be collected upon completion */
    rc = PMIx_Fence(NULL, 0, 1);
    return convert_err(rc);
}

/* the jobid is equated to the nspace in PMIx, and the
 * src_pmi_id equates to the rank. If jobid=NULL, then PMIx
 * will use the local nspace, which matches the PMI2 spec.
 * The only type of value supported by PMI2 is a string, so
 * the return of anything else is an error */
int PMI2_KVS_Get(const char *jobid, int src_pmi_id,
                 const char key[], char value [],
                 int maxvalue, int *vallen)
{
    pmix_status_t rc;
    pmix_value_t *val;
    
    rc = PMIx_Get(jobid, src_pmi_id, key, &val);
    if (PMIX_SUCCESS == rc && NULL != val) {
        if (PMIX_STRING != val->type) {
            /* this is an error */
            PMIx_free_value(&val);
            return PMI2_FAIL;
        }
        if (NULL != val->data.string) {
            (void)strncpy(value, val->data.string, maxvalue);
            *vallen = strlen(val->data.string);
        }
        PMIx_free_value(&val);
    }
    return convert_err(rc);
}

int PMI2_Info_GetNodeAttr(const char name[], char value[], int valuelen, int *found, int waitfor)
{
    /* translate the provided name to the equivalent PMIx
     * attribute name */
    return PMI2_FAIL;
}

/* push info at the PMIX_LOCAL scope */
int PMI2_Info_PutNodeAttr(const char name[], const char value[])
{
    pmix_status_t rc;
    pmix_value_t val;

    val.type = PMIX_STRING;
    val.data.string = (char*)value;
    rc = PMIx_Put(PMIX_LOCAL, name, &val);
    return convert_err(rc);
}

int PMI2_Info_GetJobAttr(const char name[], char value[], int valuelen, int *found)
{
    pmix_status_t rc;
    pmix_value_t *val;

    *found = 0;
    rc = PMIx_Get(NULL, pmix_globals.rank, name, &val);
    if (PMIX_SUCCESS == rc && NULL != val) {
        if (PMIX_STRING != val->type) {
            /* this is an error */
            PMIx_free_value(&val);
            return PMI2_FAIL;
        }
        if (NULL != val->data.string) {
            (void)strncpy(value, val->data.string, valuelen);
            *found = 1;
        }
        PMIx_free_value(&val);
    }
    return convert_err(rc);
}

int PMI2_Info_GetJobAttrIntArray(const char name[], int array[], int arraylen, int *outlen, int *found)
{
    return PMI2_FAIL;
}

int PMI2_Nameserv_publish(const char service_name[], const PMI_keyval_t *info_ptr, const char port[])
{
    pmix_status_t rc, nvals;
    pmix_info_t info[2];
    
    if (NULL == service_name || NULL == port) {
        return PMI2_ERR_INVALID_ARG;
    }
    /* pass the service/port */
    (void)strncpy(info[0].key, service_name, PMIX_MAX_KEYLEN);
    info[0].value.type = PMIX_STRING;
    info[0].value.data.string = (char*)port;
    nvals = 1;
    
    /* if provided, add any other value */
    if (NULL != info_ptr) {
        (void)strncpy(info[1].key, info_ptr->key, PMIX_MAX_KEYLEN);
        info[1].value.type = PMIX_STRING;
        info[1].value.data.string = (char*)info_ptr->val;
        nvals = 2;
    }
    /* publish the info - PMI-2 doesn't support
     * any scope other than inside our own nspace */
    rc = PMIx_Publish(PMIX_NAMESPACE, info, nvals);
    
    return convert_err(rc);
}

int PMI2_Nameserv_unpublish(const char service_name[],
                           const PMI_keyval_t *info_ptr)
{
    int nvals;
    pmix_info_t info[2];
    pmix_status_t rc;
    
    /* pass the service */
    (void)strncpy(info[0].key, service_name, PMIX_MAX_KEYLEN);
    nvals = 1;

    /* if provided, add any other value */
    if (NULL != info_ptr) {
        (void)strncpy(info[1].key, info_ptr->key, PMIX_MAX_KEYLEN);
        nvals = 2;
    }
    
    rc = PMIx_Unpublish(PMIX_NAMESPACE, info, nvals);
    return convert_err(rc);
}

int PMI2_Nameserv_lookup(const char service_name[], const PMI_keyval_t *info_ptr,
                         char port[], int portLen)
{
    pmix_status_t rc;
    int nvals;
    pmix_info_t info[2];
    
    /* pass the service */
    (void)strncpy(info[0].key, service_name, PMIX_MAX_KEYLEN);
    nvals = 1;

    /* if provided, add any other value */
    if (NULL != info_ptr) {
        (void)strncpy(info[1].key, info_ptr->key, PMIX_MAX_KEYLEN);
        info[1].value.type = PMIX_STRING;
        info[1].value.data.string = info_ptr->val;
        nvals = 2;
    }

    /* PMI-2 doesn't want the nspace back */
    if (PMIX_SUCCESS != (rc = PMIx_Lookup(PMIX_NAMESPACE, info, nvals, NULL))) {
        return convert_err(rc);
    }

    /* should have received a string back */
    if (PMIX_STRING != info[0].value.type ||
        NULL == info[0].value.data.string) {
        PMIx_free_value_data(&info[0].value);
        return PMI2_FAIL;
    }
    
    /* return the port */
    (void)strncpy(port, info[0].value.data.string, PMIX_MAX_VALLEN);
    PMIx_free_value_data(&info[0].value);
    
    return PMI2_SUCCESS;
}

int PMI2_Job_GetId(char jobid[], int jobid_size)
{
    /* we already obtained our nspace during PMI_Init,
     * so all we have to do here is return it */

    /* bozo check */
    if (NULL == jobid) {
        return PMI2_ERR_INVALID_ARGS;
    }
    (void)strncpy(jobid, pmix_globals.nspace, jobid_size);
    return PMI2_SUCCESS;
}

int PMI2_Job_Connect(const char jobid[], PMI2_Connect_comm_t *conn)
{
    pmix_status_t rc;
    pmix_range_t range;

    (void)strncpy(range.nspace, jobid, PMIX_MAX_VALLEN);
    range.ranks = NULL;
    range.nranks = 0;
    rc = PMIx_Connect(&range, 1);
    return convert_err(rc);
}

int PMI2_Job_Disconnect(const char jobid[])
{
    pmix_status_t rc;
    pmix_range_t range;

    (void)strncpy(range.nspace, jobid, PMIX_MAX_VALLEN);
    range.ranks = NULL;
    range.nranks = 0;
    rc = PMIx_Disconnect(&range, 1);
    return convert_err(rc);
}

int PMI2_Job_Spawn(int count, const char * cmds[],
                   int argcs[], const char ** argvs[],
                   const int maxprocs[],
                   const int info_keyval_sizes[],
                   const PMI_keyval_t *info_keyval_vectors[],
                   int preput_keyval_size,
                   const PMI_keyval_t *preput_keyval_vector[],
                   char jobId[], int jobIdSize,
                   int errors[])
{
    pmix_app_t *apps;
    int i, k;
    pmix_status_t rc;
    size_t j;
    char *evar;
    
    /* setup the apps */
    apps = (pmix_app_t*)malloc(count * sizeof(pmix_app_t));
    for (i=0; i < count; i++) {
        apps[i].cmd = strdup(cmds[i]);
        apps[i].maxprocs = maxprocs[i];
        apps[i].argv = pmix_argv_copy((char**)argvs[i]);
        apps[i].argc = pmix_argv_count(apps[i].argv);
        apps[i].ninfo = info_keyval_sizes[i];
        apps[i].info = (pmix_info_t*)malloc(apps[i].ninfo * sizeof(pmix_info_t));
        /* copy the info objects */
        for (j=0; j < apps[i].ninfo; j++) {
            (void)strncpy(apps[i].info[j].key, info_keyval_vectors[i][j].key, PMIX_MAX_KEYLEN);
            apps[i].info[j].value.type = PMIX_STRING;
            apps[i].info[j].value.data.string = strdup(info_keyval_vectors[i][j].val);
        }
        /* push the preput values into the apps environ */
        for (k=0; k < preput_keyval_size; k++) {
            (void)asprintf(&evar, "%s=%s", preput_keyval_vector[j]->key, preput_keyval_vector[j]->val);
            pmix_argv_append_nosize(&apps[i].env, evar);
            free(evar);
        }
    }

    rc = PMIx_Spawn(apps, count, NULL);
    /* tear down the apps array */
    for (i=0; i < count; i++) {
        if (NULL != apps[i].cmd) {
            free(apps[i].cmd);
        }
        pmix_argv_free(apps[i].argv);
        pmix_argv_free(apps[i].env);
        if (NULL != apps[i].info) {
            for (j=0; j < apps[i].ninfo; j++) {
                PMIx_free_value_data(&apps[i].info[j].value);
            }
            free(apps[i].info);
        }
    }
    if (NULL != errors) {
        for (i=0; i < count; i++) {
            errors[i] = convert_err(rc);
        }
    }

    return convert_err(rc);
}

static pmix_status_t convert_int(int *value, pmix_value_t *kv)
{
    switch(kv->type) {
    case PMIX_INT:
        *value = kv->data.integer;
        break;
    case PMIX_INT8:
        *value = kv->data.int8;
        break;
    case PMIX_INT16:
        *value = kv->data.int16;
        break;
    case PMIX_INT32:
        *value = kv->data.int32;
        break;
    case PMIX_INT64:
        *value = kv->data.int64;
        break;
    case PMIX_UINT:
        *value = kv->data.uint;
        break;
    case PMIX_UINT8:
        *value = kv->data.uint8;
        break;
    case PMIX_UINT16:
        *value = kv->data.uint16;
        break;
    case PMIX_UINT32:
        *value = kv->data.uint32;
        break;
    case PMIX_UINT64:
        *value = kv->data.uint64;
        break;
    case PMIX_BYTE:
        *value = kv->data.byte;
        break;
    case PMIX_SIZE:
        *value = kv->data.size;
        break;
    default:
        /* not an integer type */
        return PMIX_ERR_BAD_PARAM;
    }
    return PMIX_SUCCESS;
}

static int convert_err(pmix_status_t rc)
{
    switch(rc) {
    case PMIX_ERR_INVALID_SIZE:
        return PMI2_ERR_INVALID_SIZE;
        
    case PMIX_ERR_INVALID_KEYVALP:
        return PMI2_ERR_INVALID_KEYVALP;
        
    case PMIX_ERR_INVALID_NUM_PARSED:
        return PMI2_ERR_INVALID_NUM_PARSED;
        
    case PMIX_ERR_INVALID_ARGS:
        return PMI2_ERR_INVALID_ARGS;
        
    case PMIX_ERR_INVALID_NUM_ARGS:
        return PMI2_ERR_INVALID_NUM_ARGS;
        
    case PMIX_ERR_INVALID_LENGTH:
        return PMI2_ERR_INVALID_LENGTH;
        
    case PMIX_ERR_INVALID_VAL_LENGTH:
        return PMI2_ERR_INVALID_VAL_LENGTH;
        
    case PMIX_ERR_INVALID_VAL:
        return PMI2_ERR_INVALID_VAL;
        
    case PMIX_ERR_INVALID_KEY_LENGTH:
        return PMI2_ERR_INVALID_KEY_LENGTH;
        
    case PMIX_ERR_INVALID_KEY:
        return PMI2_ERR_INVALID_KEY;
        
    case PMIX_ERR_INVALID_ARG:
        return PMI2_ERR_INVALID_ARG;
        
    case PMIX_ERR_NOMEM:
        return PMI2_ERR_NOMEM;
        
    case PMIX_ERR_UNPACK_READ_PAST_END_OF_BUFFER:
    case PMIX_ERR_COMM_FAILURE:
    case PMIX_ERR_NOT_IMPLEMENTED:
    case PMIX_ERR_NOT_SUPPORTED:
    case PMIX_ERR_NOT_FOUND:
    case PMIX_ERR_SERVER_NOT_AVAIL:
    case PMIX_ERR_INVALID_NAMESPACE:
    case PMIX_ERR_DATA_VALUE_NOT_FOUND:
    case PMIX_ERR_OUT_OF_RESOURCE:
    case PMIX_ERR_RESOURCE_BUSY:
    case PMIX_ERR_BAD_PARAM:
    case PMIX_ERR_IN_ERRNO:
    case PMIX_ERR_UNREACH:
    case PMIX_ERR_TIMEOUT:
    case PMIX_ERR_NO_PERMISSIONS:
    case PMIX_ERR_PACK_MISMATCH:
    case PMIX_ERR_PACK_FAILURE:
    case PMIX_ERR_UNPACK_FAILURE:
    case PMIX_ERR_UNPACK_INADEQUATE_SPACE:
    case PMIX_ERR_TYPE_MISMATCH:
    case PMIX_ERR_PROC_ENTRY_NOT_FOUND:
    case PMIX_ERR_UNKNOWN_DATA_TYPE:
    case PMIX_ERR_WOULD_BLOCK:
    case PMIX_EXISTS:
    case PMIX_ERROR:
        return PMI2_FAIL;
        
    case PMIX_ERR_INIT:
        return PMI2_ERR_INIT;

    case PMIX_SUCCESS:
        return PMI2_SUCCESS;
    default:
        return PMI2_FAIL;
    }
}
