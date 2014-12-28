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
    // No functionality yet.
    abort();
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
                    pmix_value_t *src,
                    pmix_data_type_t type)
{
    /* copy the right field */
    switch (type) {
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
    return copy_val(p, src, type);
}

int pmix_bfrop_copy_info(pmix_info_t **dest, pmix_info_t *src,
                         pmix_data_type_t type)
{
    return PMIX_SUCCESS;
}

int pmix_bfrop_copy_buf(pmix_buffer_t **dest, pmix_buffer_t *src,
                        pmix_data_type_t type)
{
    return PMIX_SUCCESS;
}

int pmix_bfrop_copy_app(pmix_app_t **dest, pmix_app_t *src,
                        pmix_data_type_t type)
{
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
    return copy_val(p->value, src->value, src->value->type);
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
