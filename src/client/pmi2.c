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
#include "event.h"

#include "src/api/pmix.h"
#include "src/api/pmi2.h"

#include "src/buffer_ops/buffer_ops.h"

#include "src/util/argv.h"
#include "src/util/error.h"
#include "src/util/output.h"
#include "src/util/progress_threads.h"

/* local variables */
static char namespace[PMIX_MAX_VALLEN];
static int myrank;

int PMI2_Init(int *spawned, int *size, int *rank, int *appnum)
{
    int rc;
    rc = PMIx_Init(namespace, &myrank);
    *rank = myrank;
    return rc;
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

/* KVS_Put - we default to PMIX_GLOBAL scope */
int PMI2_KVS_Put(const char key[], const char value[])
{
   int rc;
    pmix_value_t val;

    val.type = PMIX_STRING;
    val.data.string = (char*)value;
    rc = PMIx_Put(PMIX_GLOBAL, key, &val);
    return rc;
}

/* KVS_Fence */
int PMI2_KVS_Fence(void)
{
    return PMIx_Fence(NULL, 0);
}

/* the jobid is equated to the namespace in PMIx, and the
 * src_pmi_id equates to the rank. If jobid=NULL, then PMIx
 * will use the local namespace, which matches the PMI2 spec.
 * The only type of value supported by PMI2 is a string, so
 * the return of anything else is an error */
int PMI2_KVS_Get(const char *jobid, int src_pmi_id,
                 const char key[], char value [],
                 int maxvalue, int *vallen)
{
    int rc;
    pmix_value_t *val;
    
    rc = PMIx_Get(jobid, src_pmi_id, key, &val);
    if (PMIX_SUCCESS == rc && NULL != val) {
        if (PMIX_STRING != val->type) {
            /* this is an error */
            PMIx_free_value_data(val);
            free(val);
            return PMI2_FAIL;
        }
        if (NULL != val->data.string) {
            (void)strncpy(value, val->data.string, maxvalue);
            *vallen = strlen(val->data.string);
        }
        PMIx_free_value_data(val);
        free(val);
    }
    return rc;
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
    int rc;
    pmix_value_t val;

    val.type = PMIX_STRING;
    val.data.string = (char*)value;
    rc = PMIx_Put(PMIX_LOCAL, name, &val);
    return rc;
}

int PMI2_Info_GetJobAttr(const char name[], char value[], int valuelen, int *found)
{
    int rc;
    pmix_value_t *val;

    *found = 0;
    rc = PMIx_Get(NULL, myrank, name, &val);
    if (PMIX_SUCCESS == rc && NULL != val) {
        if (PMIX_STRING != val->type) {
            /* this is an error */
            PMIx_free_value_data(val);
            free(val);
            return PMI2_FAIL;
        }
        if (NULL != val->data.string) {
            (void)strncpy(value, val->data.string, valuelen);
            *found = 1;
        }
        PMIx_free_value_data(val);
        free(val);
    }
    return rc;
}

int PMI2_Info_GetJobAttrIntArray(const char name[], int array[], int arraylen, int *outlen, int *found)
{
    return PMI2_FAIL;
}

int PMI2_Nameserv_publish(const char service_name[], const PMI_keyval_t *info_ptr, const char port[])
{
    int rc, nvals;
    pmix_info_t info[2];
    
    if (NULL == service_name || NULL == port) {
        return PMI2_ERR_INVALID_ARG;
    }
    /* pass the service/port */
    (void)strncpy(info[0].key, service_name, PMIX_MAX_KEYLEN);
    info[0].value = (pmix_value_t*)malloc(sizeof(pmix_value_t));
    info[0].value->type = PMIX_STRING;
    info[0].value->data.string = (char*)port;
    nvals = 1;
    
    /* if provided, add any other value */
    if (NULL != info_ptr) {
        (void)strncpy(info[1].key, info_ptr->key, PMIX_MAX_KEYLEN);
        info[1].value = (pmix_value_t*)malloc(sizeof(pmix_value_t));
        info[1].value->type = PMIX_STRING;
        info[1].value->data.string = (char*)info_ptr->val;
        nvals = 2;
    }
    /* publish the info - PMI-2 doesn't support
     * any scope other than inside our own namespace */
    rc = PMIx_Publish(PMIX_NAMESPACE, info, nvals);
    free(info[0].value);
    if (2 == nvals) {
        free(info[1].value);
    }
    
    return rc;
}

int PMI2_Nameserv_unpublish(const char service_name[],
                           const PMI_keyval_t *info_ptr)
{
    int nvals;
    pmix_info_t info[2];
    
    /* pass the service */
    (void)strncpy(info[0].key, service_name, PMIX_MAX_KEYLEN);
    nvals = 1;

    /* if provided, add any other value */
    if (NULL != info_ptr) {
        (void)strncpy(info[1].key, info_ptr->key, PMIX_MAX_KEYLEN);
        nvals = 2;
    }
    
    return PMIx_Unpublish(info, nvals);
}

int PMI2_Nameserv_lookup(const char service_name[], const PMI_keyval_t *info_ptr,
                        char port[], int portLen)
{
    int rc, nvals;
    pmix_info_t info[2];
    
    /* pass the service */
    (void)strncpy(info[0].key, service_name, PMIX_MAX_KEYLEN);
    nvals = 1;

    /* if provided, add any other value */
    if (NULL != info_ptr) {
        (void)strncpy(info[1].key, info_ptr->key, PMIX_MAX_KEYLEN);
        nvals = 2;
    }

    /* PMI-2 doesn't want the namespace back */
    if (PMIX_SUCCESS != (rc = PMIx_Lookup(info, nvals, NULL))) {
        return rc;
    }

    /* should have received a string back */
    if (NULL == info[0].value || PMIX_STRING != info[0].value->type ||
        NULL == info[0].value->data.string) {
        if (NULL != info[0].value) {
            free(info[0].value);
        }
        return PMI2_FAIL;
    }
    
    /* return the port */
    (void)strncpy(port, info[0].value->data.string, PMIX_MAX_VALLEN);
    free(info[0].value->data.string);
    free(info[0].value);
    
    return PMI2_SUCCESS;
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
    return PMI2_ERR_OTHER;
}

int PMI2_Job_Disconnect(const char jobid[])
{
    return PMI2_ERR_OTHER;
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
        apps[i].ninfo = info_keyval_sizes[i];
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
            (void)asprintf(&evar, "%s=%s", preput_keyval_vector[j]->key, preput_keyval_vector[j]->val);
            pmix_argv_append_nosize(&apps[i].env, evar);
            free(evar);
        }
    }

    rc = PMIx_Spawn(apps, count, namespace);
    /* tear down the apps array */
    
    return rc;
}

