/*
 * Copyright (c) 2004-2007 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2006 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2014-2016 Intel, Inc. All rights reserved.
 * Copyright (c) 2015      Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include <src/include/pmix_config.h>


#include "src/util/argv.h"
#include "src/util/error.h"
#include "src/util/output.h"
#include "src/include/pmix_globals.h"

#include "src/mca/bfrops/base/base.h"

pmix_status_t pmix_bfrops_base_copy(pmix_pointer_array_t *regtypes,
                                    void **dest, void *src,
                                    pmix_data_type_t type)
{
    pmix_bfrop_type_info_t *info;

    /* check for error */
    if (NULL == dest || NULL == src) {
        PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
        return PMIX_ERR_BAD_PARAM;
    }

    /* Lookup the copy function for this type and call it */
    if (NULL == (info = (pmix_bfrop_type_info_t*)pmix_pointer_array_get_item(regtypes, type))) {
        PMIX_ERROR_LOG(PMIX_ERR_UNKNOWN_DATA_TYPE);
        return PMIX_ERR_UNKNOWN_DATA_TYPE;
    }

    return info->odti_copy_fn(dest, src, type);
}

pmix_status_t pmix_bfrops_base_copy_payload(pmix_buffer_t *dest,
                                            pmix_buffer_t *src)
{
    size_t to_copy = 0;
    char *ptr;

    /* deal with buffer type */
    if (NULL == dest->base_ptr){
        /* destination buffer is empty - derive src buffer type */
        dest->type = src->type;
    } else if (dest->type != src->type) {
        /* buffer types mismatch */
        PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
        return PMIX_ERR_BAD_PARAM;
    }

    to_copy = src->pack_ptr - src->unpack_ptr;
    if (NULL == (ptr = pmix_bfrop_buffer_extend(dest, to_copy))) {
        PMIX_ERROR_LOG(PMIX_ERR_OUT_OF_RESOURCE);
        return PMIX_ERR_OUT_OF_RESOURCE;
    }
    memcpy(ptr,src->unpack_ptr, to_copy);
    dest->bytes_used += to_copy;
    dest->pack_ptr += to_copy;
    return PMIX_SUCCESS;
}


/*
 * STANDARD COPY FUNCTION - WORKS FOR EVERYTHING NON-STRUCTURED
 */
pmix_status_t pmix_bfrops_base_std_copy(void **dest, void *src,
                                        pmix_data_type_t type)
{
    size_t datasize;
    uint8_t *val = NULL;

    switch(type) {
    case PMIX_BOOL:
        datasize = sizeof(bool);
        break;

    case PMIX_INT:
    case PMIX_UINT:
        datasize = sizeof(int);
        break;

    case PMIX_SIZE:
        datasize = sizeof(size_t);
        break;

    case PMIX_PID:
        datasize = sizeof(pid_t);
        break;

    case PMIX_BYTE:
    case PMIX_INT8:
    case PMIX_UINT8:
        datasize = 1;
        break;

    case PMIX_INT16:
    case PMIX_UINT16:
        datasize = 2;
        break;

    case PMIX_INT32:
    case PMIX_UINT32:
    case PMIX_INFO_DIRECTIVES:
        datasize = 4;
        break;

    case PMIX_INT64:
    case PMIX_UINT64:
        datasize = 8;
        break;

    case PMIX_FLOAT:
        datasize = sizeof(float);
        break;

    case PMIX_TIMEVAL:
        datasize = sizeof(struct timeval);
        break;

    case PMIX_TIME:
        datasize = sizeof(time_t);
        break;

    case PMIX_STATUS:
        datasize = sizeof(pmix_status_t);
        break;

    case PMIX_PERSIST:
    case PMIX_POINTER:
    case PMIX_SCOPE:
    case PMIX_DATA_RANGE:
    case PMIX_COMMAND:
        datasize = sizeof(char*);
        break;


    default:
        return PMIX_ERR_UNKNOWN_DATA_TYPE;
    }

    val = (uint8_t*)malloc(datasize);
    if (NULL == val) {
        return PMIX_ERR_OUT_OF_RESOURCE;
    }

    memcpy(val, src, datasize);
    *dest = val;

    return PMIX_SUCCESS;
}

/* COPY FUNCTIONS FOR NON-STANDARD SYSTEM TYPES */

/*
 * STRING
 */
 pmix_status_t pmix_bfrops_base_copy_string(char **dest, char *src,
                                            pmix_data_type_t type)
{
    if (NULL == src) {  /* got zero-length string/NULL pointer - store NULL */
        *dest = NULL;
    } else {
        *dest = strdup(src);
    }

    return PMIX_SUCCESS;
}

/* PMIX_VALUE */
pmix_status_t pmix_bfrops_base_copy_value(pmix_value_t **dest,
                                          pmix_value_t *src,
                                          pmix_data_type_t type)
{
    pmix_value_t *p;

    /* create the new object */
    *dest = (pmix_value_t*)malloc(sizeof(pmix_value_t));
    if (NULL == *dest) {
        return PMIX_ERR_OUT_OF_RESOURCE;
    }
    p = *dest;

    /* copy the type */
    p->type = src->type;
    /* copy the data */
    return pmix_bfrops_base_value_xfer(p, src);
}

pmix_status_t pmix_bfrops_base_copy_info(pmix_info_t **dest,
                                         pmix_info_t *src,
                                         pmix_data_type_t type)
{
    *dest = (pmix_info_t*)malloc(sizeof(pmix_info_t));
    (void)strncpy((*dest)->key, src->key, PMIX_MAX_KEYLEN);
    (*dest)->flags = src->flags;
    return pmix_bfrops_base_value_xfer(&(*dest)->value, &src->value);
}

pmix_status_t pmix_bfrops_base_copy_buf(pmix_buffer_t **dest,
                                        pmix_buffer_t *src,
                                        pmix_data_type_t type)
{
    *dest = PMIX_NEW(pmix_buffer_t);
    pmix_bfrops_base_copy_payload(*dest, src);
    return PMIX_SUCCESS;
}

pmix_status_t pmix_bfrops_base_copy_app(pmix_app_t **dest,
                                        pmix_app_t *src,
                                        pmix_data_type_t type)
{
    size_t j;

    *dest = (pmix_app_t*)malloc(sizeof(pmix_app_t));
    (*dest)->cmd = strdup(src->cmd);
    (*dest)->argc = src->argc;
    (*dest)->argv = pmix_argv_copy(src->argv);
    (*dest)->env = pmix_argv_copy(src->env);
    (*dest)->maxprocs = src->maxprocs;
    (*dest)->ninfo = src->ninfo;
    (*dest)->info = (pmix_info_t*)malloc(src->ninfo * sizeof(pmix_info_t));
    for (j=0; j < src->ninfo; j++) {
        (void)strncpy((*dest)->info[j].key, src->info[j].key, PMIX_MAX_KEYLEN);
        pmix_bfrops_base_value_xfer(&(*dest)->info[j].value, &src->info[j].value);
    }
    return PMIX_SUCCESS;
}

pmix_status_t pmix_bfrops_base_copy_kval(pmix_kval_t **dest,
                                         pmix_kval_t *src,
                                         pmix_data_type_t type)
{
    pmix_kval_t *p;

    /* create the new object */
    *dest = PMIX_NEW(pmix_kval_t);
    if (NULL == *dest) {
        return PMIX_ERR_OUT_OF_RESOURCE;
    }
    p = *dest;

    /* copy the type */
    p->value->type = src->value->type;
    /* copy the data */
    return pmix_bfrops_base_value_xfer(p->value, src->value);
}

pmix_status_t pmix_bfrops_base_copy_array(pmix_info_array_t **dest,
                                          pmix_info_array_t *src,
                                          pmix_data_type_t type)
{
    pmix_info_t *d1, *s1;

    *dest = (pmix_info_array_t*)malloc(sizeof(pmix_info_array_t));
    (*dest)->size = src->size;
    (*dest)->array = (struct pmix_info_t*)malloc(src->size * sizeof(pmix_info_t));
    d1 = (pmix_info_t*)(*dest)->array;
    s1 = (pmix_info_t*)src->array;
    memcpy(d1, s1, src->size * sizeof(pmix_info_t));
    return PMIX_SUCCESS;
}

pmix_status_t pmix_bfrops_base_copy_proc(pmix_proc_t **dest,
                                         pmix_proc_t *src,
                                         pmix_data_type_t type)
{
    *dest = (pmix_proc_t*)malloc(sizeof(pmix_proc_t));
    if (NULL == *dest) {
        return PMIX_ERR_OUT_OF_RESOURCE;
    }
    (void)strncpy((*dest)->nspace, src->nspace, PMIX_MAX_NSLEN);
    (*dest)->rank = src->rank;
    return PMIX_SUCCESS;
}

#if PMIX_HAVE_HWLOC
pmix_status_t pmix_bfrops_base_copy_topo(hwloc_topology_t *dest,
                                         hwloc_topology_t src,
                                         pmix_data_type_t type)
{
    /* use the hwloc dup function */
    return hwloc_topology_dup(dest, src);
}
#endif

pmix_status_t pmix_bfrops_base_copy_modex(pmix_modex_data_t **dest,
                                          pmix_modex_data_t *src,
                                          pmix_data_type_t type)
{
    *dest = (pmix_modex_data_t*)malloc(sizeof(pmix_modex_data_t));
    if (NULL == *dest) {
        return PMIX_ERR_OUT_OF_RESOURCE;
    }
    (*dest)->blob = NULL;
    (*dest)->size = 0;
    if (NULL != src->blob) {
        (*dest)->blob = (uint8_t*)malloc(src->size * sizeof(uint8_t));
        if (NULL == (*dest)->blob) {
            return PMIX_ERR_OUT_OF_RESOURCE;
        }
        memcpy((*dest)->blob, src->blob, src->size * sizeof(uint8_t));
        (*dest)->size = src->size;
    }
    return PMIX_SUCCESS;
}

pmix_status_t pmix_bfrops_base_copy_bo(pmix_byte_object_t **dest,
                                       pmix_byte_object_t *src,
                                       pmix_data_type_t type)
{
    *dest = (pmix_byte_object_t*)malloc(sizeof(pmix_byte_object_t));
    if (NULL == *dest) {
        return PMIX_ERR_OUT_OF_RESOURCE;
    }
    (*dest)->bytes = (char*)malloc(src->size);
    memcpy((*dest)->bytes, src->bytes, src->size);
    (*dest)->size = src->size;
    return PMIX_SUCCESS;
}

pmix_status_t pmix_bfrops_base_copy_pdata(pmix_pdata_t **dest,
                                          pmix_pdata_t *src,
                                          pmix_data_type_t type)
{
    *dest = (pmix_pdata_t*)malloc(sizeof(pmix_pdata_t));
    (void)strncpy((*dest)->proc.nspace, src->proc.nspace, PMIX_MAX_NSLEN);
    (*dest)->proc.rank = src->proc.rank;
    (void)strncpy((*dest)->key, src->key, PMIX_MAX_KEYLEN);
    return pmix_bfrops_base_value_xfer(&(*dest)->value, &src->value);
}

