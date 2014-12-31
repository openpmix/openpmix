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
 * Copyright (c) 2014      Intel, Inc. All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "pmix_config.h"

#include "src/util/argv.h"
#include "src/util/output.h"
#include "src/buffer_ops/internal.h"

int pmix_bfrop_copy(void **dest, void *src, pmix_data_type_t type)
{
    pmix_bfrop_type_info_t *info;

    /* check for error */
    if (NULL == dest) {
        return PMIX_ERR_BAD_PARAM;
    }
    if (NULL == src) {
        return PMIX_ERR_BAD_PARAM;
    }

   /* Lookup the copy function for this type and call it */

    if (NULL == (info = (pmix_bfrop_type_info_t*)pmix_pointer_array_get_item(&pmix_bfrop_types, type))) {
        return PMIX_ERR_UNKNOWN_DATA_TYPE;
    }

    return info->odti_copy_fn(dest, src, type);
}

int pmix_bfrop_copy_payload(pmix_buffer_t *dest, pmix_buffer_t *src)
{
    size_t to_copy = 0;
    char *ptr;
    /* deal with buffer type */
    if( NULL == dest->base_ptr ){
        /* destination buffer is empty - derive src buffer type */
        dest->type = src->type;
    } else if( dest->type != src->type ){
        /* buffer types mismatch */
        pmix_output(0, "PMIX bfrop:copy_payload: source/destination buffer types mismatch");
        return PMIX_ERR_BAD_PARAM;
    }

    to_copy = src->pack_ptr - src->unpack_ptr;
    if( NULL == (ptr = pmix_bfrop_buffer_extend(dest, to_copy)) ){
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
int pmix_bfrop_std_copy(void **dest, void *src, pmix_data_type_t type)
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
int pmix_bfrop_copy_string(char **dest, char *src, pmix_data_type_t type)
{
    if (NULL == src) {  /* got zero-length string/NULL pointer - store NULL */
        *dest = NULL;
    } else {
        *dest = strdup(src);
    }

    return PMIX_SUCCESS;
}

/* COPY FUNCTIONS FOR GENERIC PMIX TYPES */
static int copy_val(pmix_value_t *p,
                    pmix_value_t *src)
{
    size_t j;
    char **pstr, **sstr;
    
    /* copy the right field */
    p->type = src->type;
    switch (src->type) {
    case PMIX_BOOL:
        p->data.flag = src->data.flag;
        break;
    case PMIX_BYTE:
        p->data.byte = src->data.byte;
        break;
    case PMIX_STRING:
        if (NULL != src->data.string) {
            p->data.string = strdup(src->data.string);
        } else {
            p->data.string = NULL;
        }
        break;
    case PMIX_SIZE:
        p->data.size = src->data.size;
        break;
    case PMIX_PID:
        p->data.pid = src->data.pid;
        break;
    case PMIX_INT:
        /* to avoid alignment issues */
        memcpy(&p->data.integer, &src->data.integer, sizeof(int));
        break;
    case PMIX_INT8:
        p->data.int8 = src->data.int8;
        break;
    case PMIX_INT16:
        /* to avoid alignment issues */
        memcpy(&p->data.int16, &src->data.int16, 2);
        break;
    case PMIX_INT32:
        /* to avoid alignment issues */
        memcpy(&p->data.int32, &src->data.int32, 4);
        break;
    case PMIX_INT64:
        /* to avoid alignment issues */
        memcpy(&p->data.int64, &src->data.int64, 8);
        break;
    case PMIX_UINT:
        /* to avoid alignment issues */
        memcpy(&p->data.uint, &src->data.uint, sizeof(unsigned int));
        break;
    case PMIX_UINT8:
        p->data.uint8 = src->data.uint8;
        break;
    case PMIX_UINT16:
        /* to avoid alignment issues */
        memcpy(&p->data.uint16, &src->data.uint16, 2);
        break;
    case PMIX_UINT32:
        /* to avoid alignment issues */
        memcpy(&p->data.uint32, &src->data.uint32, 4);
        break;
    case PMIX_UINT64:
        /* to avoid alignment issues */
        memcpy(&p->data.uint64, &src->data.uint64, 8);
        break;
    case PMIX_FLOAT:
        p->data.fval = src->data.fval;
        break;
    case PMIX_DOUBLE:
        p->data.dval = src->data.dval;
        break;
    case PMIX_TIMEVAL:
        p->data.tv.tv_sec = src->data.tv.tv_sec;
        p->data.tv.tv_usec = src->data.tv.tv_usec;
        break;
    case PMIX_ARRAY:
        p->data.array.type = src->data.array.type;
        p->data.array.size = src->data.array.size;
        if (NULL != src->data.array.array) {
            switch (p->data.array.type) {
            case PMIX_BYTE:
            case PMIX_INT8:
            case PMIX_UINT8:
                p->data.array.array = (uint8_t*)malloc(sizeof(uint8_t));
                memcpy(p->data.array.array, src->data.array.array, src->data.array.size*sizeof(uint8_t));
                break;
            case PMIX_INT16:
            case PMIX_UINT16:
                p->data.array.array = (uint16_t*)malloc(sizeof(uint16_t));
                memcpy(p->data.array.array, src->data.array.array, src->data.array.size*sizeof(uint16_t));
                break;
            case PMIX_INT32:
            case PMIX_UINT32:
                p->data.array.array = (uint32_t*)malloc(sizeof(uint32_t));
                memcpy(p->data.array.array, src->data.array.array, src->data.array.size*sizeof(uint32_t));
                break;
            case PMIX_INT64:
            case PMIX_UINT64:
                p->data.array.array = (uint64_t*)malloc(sizeof(uint64_t));
                memcpy(p->data.array.array, src->data.array.array, src->data.array.size*sizeof(uint64_t));
                break;
            case PMIX_INT:
            case PMIX_UINT:
                p->data.array.array = (int*)malloc(sizeof(int));
                memcpy(p->data.array.array, src->data.array.array, src->data.array.size*sizeof(int));
                break;
            case PMIX_BOOL:
                p->data.array.array = (bool*)malloc(sizeof(bool));
                memcpy(p->data.array.array, src->data.array.array, src->data.array.size*sizeof(bool));
                break;
            case PMIX_STRING:
                p->data.array.array = (char*)malloc(sizeof(char*));
                pstr = (char**)p->data.array.array;
                sstr = (char**)src->data.array.array;
                for (j=0; j < src->data.array.size; j++) {
                    pstr[j] = strdup(sstr[j]);
                }
                break;
            case PMIX_PID:
                p->data.array.array = (pid_t*)malloc(sizeof(pid_t));
                memcpy(p->data.array.array, src->data.array.array, src->data.array.size*sizeof(pid_t));
                break;
            case PMIX_SIZE:
                p->data.array.array = (size_t*)malloc(sizeof(size_t));
                memcpy(p->data.array.array, src->data.array.array, src->data.array.size*sizeof(size_t));
                break;
            case PMIX_FLOAT:
                p->data.array.array = (float*)malloc(sizeof(float));
                memcpy(p->data.array.array, src->data.array.array, src->data.array.size*sizeof(float));
                break;
            case PMIX_DOUBLE:
                p->data.array.array = (double*)malloc(sizeof(double));
                memcpy(p->data.array.array, src->data.array.array, src->data.array.size*sizeof(double));
                break;
            case PMIX_TIMEVAL:
                p->data.array.array = (struct timeval*)malloc(sizeof(struct timeval));
                memcpy(p->data.array.array, src->data.array.array, src->data.array.size*sizeof(struct timeval));
                break;
            default:
                pmix_output(0, "COPY-VALUE: ARRAY TYPE NOT SUPPORTED");
                return PMIX_ERROR;
            }
        }
        break;
    default:
        pmix_output(0, "COPY-PMIX-VALUE: UNSUPPORTED TYPE %d", (int)src->type);
        return PMIX_ERROR;
    }
    return PMIX_SUCCESS;
}

/* PMIX_VALUE */
int pmix_bfrop_copy_value(pmix_value_t **dest, pmix_value_t *src,
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
    return copy_val(p, src);
}

int pmix_bfrop_copy_info(pmix_info_t **dest, pmix_info_t *src,
                         pmix_data_type_t type)
{
    *dest = (pmix_info_t*)malloc(sizeof(pmix_info_t));
    (void)strncpy((*dest)->key, src->key, PMIX_MAX_VALLEN);
    return copy_val((*dest)->value, src->value);
}

int pmix_bfrop_copy_buf(pmix_buffer_t **dest, pmix_buffer_t *src,
                        pmix_data_type_t type)
{
    *dest = OBJ_NEW(pmix_buffer_t);
    pmix_bfrop.copy_payload(*dest, src);
    return PMIX_SUCCESS;
}

int pmix_bfrop_copy_app(pmix_app_t **dest, pmix_app_t *src,
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
        (void)strncpy((*dest)->info[j].key, src->info[j].key, PMIX_MAX_VALLEN);
        copy_val((*dest)->info[j].value, src->info[j].value);
    }
    return PMIX_SUCCESS;
}

int pmix_bfrop_copy_kval(pmix_kval_t **dest, pmix_kval_t *src,
                         pmix_data_type_t type)
{
    pmix_kval_t *p;
    
    /* create the new object */
    *dest = OBJ_NEW(pmix_kval_t);
    if (NULL == *dest) {
        return PMIX_ERR_OUT_OF_RESOURCE;
    }
    p = *dest;
    
    /* copy the type */
    p->value->type = src->value->type;
    /* copy the data */
    return copy_val(p->value, src->value);
}

int pmix_bfrop_copy_array(pmix_value_t **dest, pmix_array_t *src,
                          pmix_data_type_t type)
{
    // No functionality yet
    abort();
}

int pmix_bfrop_copy_range(pmix_value_t **dest, pmix_range_t *src,
                          pmix_data_type_t type)
{
    // No functionality yet
    abort();
}

#if PMIX_HAVE_HWLOC
int pmix_bfrop_copy_topo(hwloc_topology_t *dest,
                         hwloc_topology_t src,
                         pmix_data_type_t type)
{
    /* use the hwloc dup function */
    return hwloc_topology_dup(dest, src);
}
#endif
