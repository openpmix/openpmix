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
#include "src/api/pmi2.h"

#include "src/buffer_ops/buffer_ops.h"
#include "event.h"

#include "src/util/argv.h"
#include "src/util/error.h"
#include "src/util/output.h"
#include "src/util/progress_threads.h"

/* local variables */
static char *namespace;

int PMI2_Init(int *spawned, int *size, int *rank, int *appnum)
{
    return PMIx_Init(&namespace, rank);
}

int PMI2_Initialized(void)
{
    int initialized;
    initialized = (int)PMIx_Initialized();
    return initialized;
}

int PMI2_Finalize(void)
{
    return PMIx_Finalize();
}

int PMI2_Abort(int flag, const char msg[])
{
    return PMIx_Abort(flag, msg);
}

/* KVS_Put - we default to PMIX_GLOBAL scope and ignore the
 * provided kvsname as we only put into our own namespace */
int PMI2_KVS_Put(const char key[], const char value[])
{
    int rc;
    pmix_value_t val;

    PMIX_VAL_SET(&val, string, value, rc, exit);
    rc = PMIx_Put(PMIX_GLOBAL, key, &val);
    OBJ_DESTRUCT(&val);
exit:
    return rc;
}

/* KVS_Fence */
int PMI2_KVS_Fence(void)
{
    return PMIx_Fence(NULL);
}

int PMI2_KVS_Get(const char *jobid, int src_pmi_id, const char key[], char value [], int maxvalue, int *vallen)
{
    int rc;
    pmix_value_t *val;
    rc = PMIx_Get(jobid, src_pmi_id, key, &val);
    if (PMIX_SUCCESS == rc && NULL != val && !strncmp(val->key, key, strlen(key) + 1)) {
        if (PMIX_STRING == val->type && NULL != val->data.string && (strlen(value) >= strlen(val->data.string))) {
            strncpy(value, val->data.string, strlen(val->data.string) + 1);
            *vallen = strlen(val->data.string) + 1;
            free(val->data.string);
            OBJ_RELEASE(val);
            return PMI2_SUCCESS;
        } else if (PMIX_BYTE_OBJECT == val->type && (strlen(value) + 1 >= val->data.bo.size)) {
            strncpy(value, val->data.bo.bytes, val->data.bo.size);
            *vallen = val->data.bo.size;
            free(val->data.bo.bytes);
            OBJ_RELEASE(val);
            return PMI2_SUCCESS;
        }
    }
    return PMI2_FAIL;
}

int PMI2_Info_GetNodeAttr(const char name[], char value[], int valuelen, int *found, int waitfor)
{
    return PMI2_FAIL;
}

int PMI2_Info_PutNodeAttr(const char name[], const char value[])
{
    return PMI2_FAIL;
}

int PMI2_Info_GetJobAttr(const char name[], char value[], int valuelen, int *found)
{
    pmix_value_t *kv;
    if (PMIx_Get_attr(namespace, name, &kv)) {
        if (NULL != kv && PMIX_STRING == kv->type && NULL != kv->data.string && valuelen >= strlen(kv->data.string)) {
            *found = 1;
            strncpy(value, kv->data.string, strlen(kv->data.string) + 1);
            free(kv->data.string);
            OBJ_RELEASE(kv);
            return PMI2_SUCCESS;
        } else if (NULL != kv && PMIX_BYTE_OBJECT == kv->type && valuelen + 1 >= kv->data.bo.size) {
            *found = 1;
            strncpy(value, kv->data.bo.bytes, kv->data.bo.size);
            free(kv->data.bo.bytes);
            OBJ_RELEASE(kv);
            return PMI2_SUCCESS;
        }
    }
    return PMI2_FAIL;
}

int PMI2_Info_GetJobAttrIntArray(const char name[], int array[], int arraylen, int *outlen, int *found)
{
    return PMI2_FAIL;
}

int PMI2_Nameserv_publish(const char service_name[], const PMI_keyval_t *info_ptr, const char port[])
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
    return PMI2_FAIL;
}

int PMI2_Nameserv_unpublish(const char service_name[],
                           const PMI_keyval_t *info_ptr)
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

int PMI2_Nameserv_lookup(const char service_name[], const PMI_keyval_t *info_ptr,
                        char port[], int portLen)
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

int PMI2_Job_GetId(char jobid[], int jobid_size)
{
    /* we already obtained our namespace during PMI_Init,
     * so all we have to do here is return it */

    /* bozo check */
    if (NULL == jobid) {
        return PMI2_ERR_INVALID_ARGS;
    }
    (void)strncpy(jobid, namespace, jobid_size);
    return PMI2_SUCCESS;
}

int PMI2_Job_Connect(const char jobid[], PMI2_Connect_comm_t *conn)
{
    return PMI2_SUCCESS;
}

int PMI2_Job_Disconnect(const char jobid[])
{
    return PMI2_SUCCESS;
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
    pmix_list_t info;
    //pmix_list_t preput;
    pmix_app_t *pt;
    int i;

    /* convert the incoming PMI_keyval_t structs to pmix_info_t objects */
    OBJ_CONSTRUCT(&info, pmix_list_t);
    for (i=0; i < count; i++) {
        pt = OBJ_NEW(pmix_app_t);
        pt->cmd = strdup(cmds[i]);
        pt->argv = pmix_argv_copy((char**)argvs[i]);
    }
    return PMI2_SUCCESS;
}

