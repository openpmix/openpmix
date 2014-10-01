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
    if (NULL == src && (PMIX_NULL != type && PMIX_STRING != type)) {
        return PMIX_ERR_BAD_PARAM;
    }

   /* Lookup the copy function for this type and call it */

    if (NULL == (info = (pmix_bfrop_type_info_t*)pmix_pointer_array_get_item(&pmix_bfrop_types, type))) {
        return PMIX_ERR_UNKNOWN_DATA_TYPE;
    }

    return info->odti_copy_fn(dest, src, type);
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

    case PMIX_DATA_TYPE:
        datasize = sizeof(pmix_data_type_t);
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
 * NULL
 */
int pmix_bfrop_copy_null(char **dest, char *src, pmix_data_type_t type)
{
    char *val;

    *dest = (char*)malloc(sizeof(char*));
    if (NULL == *dest) {
        return PMIX_ERR_OUT_OF_RESOURCE;
    }

    val = *dest;  /* save the address of the value */

    /* set the dest to null */
    *val = 0x00;

    return PMIX_SUCCESS;
}

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

/*
 * PMIX_BYTE_OBJECT
 */
int pmix_bfrop_copy_byte_object(pmix_byte_object_t **dest, pmix_byte_object_t *src,
                              pmix_data_type_t type)
{
    /* allocate space for the new object */
    *dest = (pmix_byte_object_t*)malloc(sizeof(pmix_byte_object_t));
    if (NULL == *dest) {
        return PMIX_ERR_OUT_OF_RESOURCE;
    }

    (*dest)->size = src->size;

    /* allocate the required space for the bytes */
    if (NULL == src->bytes) {
        (*dest)->bytes = NULL;
    } else {
        (*dest)->bytes = (uint8_t*)malloc(src->size);
        if (NULL == (*dest)->bytes) {
            OBJ_RELEASE(*dest);
            return PMIX_ERR_OUT_OF_RESOURCE;
        }
        /* copy the data across */
        memcpy((*dest)->bytes, src->bytes, src->size);
    }

    return PMIX_SUCCESS;
}

/* PMIX_PSTAT */
int pmix_bfrop_copy_pstat(pmix_pstats_t **dest, pmix_pstats_t *src,
                        pmix_data_type_t type)
{
    pmix_pstats_t *p;
    
    /* create the new object */
    *dest = OBJ_NEW(pmix_pstats_t);
    if (NULL == *dest) {
        return PMIX_ERR_OUT_OF_RESOURCE;
    }
    p = *dest;
    
    /* copy the individual fields */
    memcpy(p->node, src->node, sizeof(src->node));
    p->rank = src->rank;
    p->pid = src->pid;
    memcpy(p->cmd, src->cmd, sizeof(src->cmd));
    p->state[0] = src->state[0];
    p->time = src->time;
    p->priority = src->priority;
    p->num_threads = src->num_threads;
    p->vsize = src->vsize;
    p->rss = src->rss;
    p->peak_vsize = src->peak_vsize;
    p->processor = src->processor;
    p->sample_time.tv_sec = src->sample_time.tv_sec;
    p->sample_time.tv_usec = src->sample_time.tv_usec;    
    return PMIX_SUCCESS;
}

/* PMIX_NODE_STAT */
int pmix_bfrop_copy_node_stat(pmix_node_stats_t **dest, pmix_node_stats_t *src,
                            pmix_data_type_t type)
{
    pmix_node_stats_t *p;
    
    /* create the new object */
    *dest = OBJ_NEW(pmix_node_stats_t);
    if (NULL == *dest) {
        return PMIX_ERR_OUT_OF_RESOURCE;
    }
    p = *dest;
    
    /* copy the individual fields */
    p->la = src->la;
    p->la5 = src->la5;
    p->la15 = src->la15;
    p->total_mem = src->total_mem;
    p->free_mem = src->free_mem;
    p->sample_time.tv_sec = src->sample_time.tv_sec;
    p->sample_time.tv_usec = src->sample_time.tv_usec;    
    return PMIX_SUCCESS;
}

/* PMIX_VALUE */
int pmix_bfrop_copy_value(pmix_value_t **dest, pmix_value_t *src,
                        pmix_data_type_t type)
{
    pmix_value_t *p;
    
    /* create the new object */
    *dest = OBJ_NEW(pmix_value_t);
    if (NULL == *dest) {
        return PMIX_ERR_OUT_OF_RESOURCE;
    }
    p = *dest;
    
    /* copy the type and key */
    if (NULL != src->key) {
        p->key = strdup(src->key);
    }
    p->type = src->type;

    /* copy the right field */
    switch (src->type) {
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
    case PMIX_BYTE_OBJECT:
        if (NULL != src->data.bo.bytes && 0 < src->data.bo.size) {
            p->data.bo.bytes = malloc(src->data.bo.size);
            memcpy(p->data.bo.bytes, src->data.bo.bytes, src->data.bo.size);
            p->data.bo.size = src->data.bo.size;
        } else {
            p->data.bo.bytes = NULL;
            p->data.bo.size = 0;
        }
        break;
    default:
        pmix_output(0, "COPY-PMIX-VALUE: UNSUPPORTED TYPE %d", (int)src->type);
        return PMIX_ERROR;
    }

    return PMIX_SUCCESS;
}

int pmix_bfrop_copy_buffer_contents(pmix_buffer_t **dest, pmix_buffer_t *src,
                                  pmix_data_type_t type)
{
    return PMIX_SUCCESS;
}

