/* -*- Mode: C; c-basic-offset:4 ; -*- */
/*
 * Copyright (c) 2004-2007 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2009 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2012-2013 Los Alamos National Security, Inc.  All rights reserved. 
 * Copyright (c) 2014      Intel, Inc. All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */
/** @file:
 *
 */
#include "pmix_config.h"
#include "src/api/pmix.h"

#ifdef HAVE_STRING_H
#include <string.h>
#endif

#include "src/util/argv.h"
#include "src/buffer_ops/internal.h"

/**
 * globals
 */
bool pmix_bfrop_initialized = false;
int pmix_bfrop_verbose = -1;  /* by default disabled */
int pmix_bfrop_initial_size;
int pmix_bfrop_threshold_size;
pmix_pointer_array_t pmix_bfrop_types;
pmix_data_type_t pmix_bfrop_num_reg_types;
pmix_bfrop_buffer_type_t default_buf_type = PMIX_BFROP_BUFFER_NON_DESC;

pmix_bfrop_t pmix_bfrop = {
    pmix_bfrop_pack,
    pmix_bfrop_unpack,
    pmix_bfrop_copy,
    pmix_bfrop_print,
    pmix_bfrop_copy_payload,
};

/**
 * Object constructors, destructors, and instantiations
 */
/** Value **/
static void pmix_buffer_construct (pmix_buffer_t* buffer)
{
    /** set the default buffer type */
    buffer->type = default_buf_type;

    /* Make everything NULL to begin with */
    buffer->base_ptr = buffer->pack_ptr = buffer->unpack_ptr = NULL;
    buffer->bytes_allocated = buffer->bytes_used = 0;
}

static void pmix_buffer_destruct (pmix_buffer_t* buffer)
{
    if (NULL != buffer->base_ptr) {
        free (buffer->base_ptr);
    }
}

OBJ_CLASS_INSTANCE(pmix_buffer_t,
                   pmix_object_t,
                   pmix_buffer_construct,
                   pmix_buffer_destruct);


static void pmix_bfrop_type_info_construct(pmix_bfrop_type_info_t *obj)
{
    obj->odti_name = NULL;
    obj->odti_pack_fn = NULL;
    obj->odti_unpack_fn = NULL;
    obj->odti_copy_fn = NULL;
    obj->odti_print_fn = NULL;
}

static void pmix_bfrop_type_info_destruct(pmix_bfrop_type_info_t *obj)
{
    if (NULL != obj->odti_name) {
        free(obj->odti_name);
    }
}

OBJ_CLASS_INSTANCE(pmix_bfrop_type_info_t, pmix_object_t,
                   pmix_bfrop_type_info_construct,
                   pmix_bfrop_type_info_destruct);

static void kvcon(pmix_kval_t *k)
{
    k->key = NULL;
    k->value = NULL;
}
static void kvdes(pmix_kval_t *k)
{
    if (NULL != k->key) {
        free(k->key);
    }
    if (NULL != k->value) {
        OBJ_RELEASE(k->value);
    }
}
OBJ_CLASS_INSTANCE(pmix_kval_t,
                   pmix_list_item_t,
                   kvcon, kvdes);

int pmix_bfrop_open(void)
{
    int rc;

    if (pmix_bfrop_initialized) {
        return PMIX_SUCCESS;
    }

    /** set the default buffer type. If we are in debug mode, then we default
     * to fully described buffers. Otherwise, we default to non-described for brevity
     * and performance
     */
#if PMIX_ENABLE_DEBUG
    default_buf_type = PMIX_BFROP_BUFFER_FULLY_DESC;
#else
    default_buf_type = PMIX_BFROP_BUFFER_NON_DESC;
#endif

    /* Setup the types array */
    OBJ_CONSTRUCT(&pmix_bfrop_types, pmix_pointer_array_t);
    if (PMIX_SUCCESS != (rc = pmix_pointer_array_init(&pmix_bfrop_types, 64, 255, 64))) {
        return rc;
    }
    pmix_bfrop_num_reg_types = 0;

    /* Register all the supported types */
    PMIX_REGISTER_TYPE("PMIX_BYTE", PMIX_BYTE,
                       pmix_bfrop_pack_byte,
                       pmix_bfrop_unpack_byte,
                       pmix_bfrop_std_copy,
                       pmix_bfrop_print_byte);
    
    PMIX_REGISTER_TYPE("PMIX_BOOL", PMIX_BOOL,
                       pmix_bfrop_pack_bool,
                       pmix_bfrop_unpack_bool,
                       pmix_bfrop_std_copy,
                       pmix_bfrop_print_bool);

    PMIX_REGISTER_TYPE("PMIX_STRING", PMIX_STRING,
                       pmix_bfrop_pack_string,
                       pmix_bfrop_unpack_string,
                       pmix_bfrop_copy_string,
                       pmix_bfrop_print_string);

    PMIX_REGISTER_TYPE("PMIX_SIZE", PMIX_SIZE,
                       pmix_bfrop_pack_sizet,
                       pmix_bfrop_unpack_sizet,
                       pmix_bfrop_std_copy,
                       pmix_bfrop_print_size);

    PMIX_REGISTER_TYPE("PMIX_PID", PMIX_PID,
                       pmix_bfrop_pack_pid,
                       pmix_bfrop_unpack_pid,
                       pmix_bfrop_std_copy,
                       pmix_bfrop_print_pid);

    PMIX_REGISTER_TYPE("PMIX_INT", PMIX_INT,
                       pmix_bfrop_pack_int,
                       pmix_bfrop_unpack_int,
                       pmix_bfrop_std_copy,
                       pmix_bfrop_print_int);

    PMIX_REGISTER_TYPE("PMIX_INT8", PMIX_INT8,
                       pmix_bfrop_pack_byte,
                       pmix_bfrop_unpack_byte,
                       pmix_bfrop_std_copy,
                       pmix_bfrop_print_int8);

    PMIX_REGISTER_TYPE("PMIX_INT16", PMIX_INT16,
                       pmix_bfrop_pack_int16,
                       pmix_bfrop_unpack_int16,
                       pmix_bfrop_std_copy,
                       pmix_bfrop_print_int16);

    PMIX_REGISTER_TYPE("PMIX_INT32", PMIX_INT32,
                       pmix_bfrop_pack_int32,
                       pmix_bfrop_unpack_int32,
                       pmix_bfrop_std_copy,
                       pmix_bfrop_print_int32);

    PMIX_REGISTER_TYPE("PMIX_INT64", PMIX_INT64,
                       pmix_bfrop_pack_int64,
                       pmix_bfrop_unpack_int64,
                       pmix_bfrop_std_copy,
                       pmix_bfrop_print_int64);

    PMIX_REGISTER_TYPE("PMIX_UINT", PMIX_UINT,
                       pmix_bfrop_pack_int,
                       pmix_bfrop_unpack_int,
                       pmix_bfrop_std_copy,
                       pmix_bfrop_print_uint);

    PMIX_REGISTER_TYPE("PMIX_UINT8", PMIX_UINT8,
                       pmix_bfrop_pack_byte,
                       pmix_bfrop_unpack_byte,
                       pmix_bfrop_std_copy,
                       pmix_bfrop_print_uint8);

    PMIX_REGISTER_TYPE("PMIX_UINT16", PMIX_UINT16,
                       pmix_bfrop_pack_int16,
                       pmix_bfrop_unpack_int16,
                       pmix_bfrop_std_copy,
                       pmix_bfrop_print_uint16);

    PMIX_REGISTER_TYPE("PMIX_UINT32", PMIX_UINT32,
                       pmix_bfrop_pack_int32,
                       pmix_bfrop_unpack_int32,
                       pmix_bfrop_std_copy,
                       pmix_bfrop_print_uint32);

    PMIX_REGISTER_TYPE("PMIX_UINT64", PMIX_UINT64,
                       pmix_bfrop_pack_int64,
                       pmix_bfrop_unpack_int64,
                       pmix_bfrop_std_copy,
                       pmix_bfrop_print_uint64);

    PMIX_REGISTER_TYPE("PMIX_FLOAT", PMIX_FLOAT,
                       pmix_bfrop_pack_float,
                       pmix_bfrop_unpack_float,
                       pmix_bfrop_std_copy,
                       pmix_bfrop_print_float);

    PMIX_REGISTER_TYPE("PMIX_DOUBLE", PMIX_DOUBLE,
                       pmix_bfrop_pack_double,
                       pmix_bfrop_unpack_double,
                       pmix_bfrop_std_copy,
                       pmix_bfrop_print_double);

    PMIX_REGISTER_TYPE("PMIX_TIMEVAL", PMIX_TIMEVAL,
                       pmix_bfrop_pack_timeval,
                       pmix_bfrop_unpack_timeval,
                       pmix_bfrop_std_copy,
                       pmix_bfrop_print_timeval);

    PMIX_REGISTER_TYPE("PMIX_TIME", PMIX_TIME,
                       pmix_bfrop_pack_time,
                       pmix_bfrop_unpack_time,
                       pmix_bfrop_std_copy,
                       pmix_bfrop_print_time);

#if PMIX_HAVE_HWLOC
    PMIX_REGISTER_TYPE("PMIX_HWLOC_TOPO", PMIX_HWLOC_TOPO,
                       pmix_bfrop_pack_topo,
                       pmix_bfrop_unpack_topo,
                       pmix_bfrop_copy_topo,
                       pmix_bfrop_print_topo);
#endif
    
    PMIX_REGISTER_TYPE("PMIX_VALUE", PMIX_VALUE,
                       pmix_bfrop_pack_value,
                       pmix_bfrop_unpack_value,
                       pmix_bfrop_copy_value,
                       pmix_bfrop_print_value);

    PMIX_REGISTER_TYPE("PMIX_ARRAY", PMIX_ARRAY,
                       pmix_bfrop_pack_array,
                       pmix_bfrop_unpack_array,
                       pmix_bfrop_copy_array,
                       pmix_bfrop_print_array);

    PMIX_REGISTER_TYPE("PMIX_RANGE", PMIX_RANGE,
                       pmix_bfrop_pack_range,
                       pmix_bfrop_unpack_range,
                       pmix_bfrop_copy_range,
                       pmix_bfrop_print_range);

    PMIX_REGISTER_TYPE("PMIX_APP", PMIX_APP,
                       pmix_bfrop_pack_app,
                       pmix_bfrop_unpack_app,
                       pmix_bfrop_copy_app,
                       pmix_bfrop_print_app);

    PMIX_REGISTER_TYPE("PMIX_INFO", PMIX_INFO,
                       pmix_bfrop_pack_info,
                       pmix_bfrop_unpack_info,
                       pmix_bfrop_copy_info,
                       pmix_bfrop_print_info);
    
    PMIX_REGISTER_TYPE("PMIX_BUFFER", PMIX_BUFFER,
                       pmix_bfrop_pack_buf,
                       pmix_bfrop_unpack_buf,
                       pmix_bfrop_copy_buf,
                       pmix_bfrop_print_buf);
    
    PMIX_REGISTER_TYPE("PMIX_KVAL", PMIX_KVAL,
                       pmix_bfrop_pack_kval,
                       pmix_bfrop_unpack_kval,
                       pmix_bfrop_copy_kval,
                       pmix_bfrop_print_kval);

    /* All done */
    pmix_bfrop_initialized = true;
    return PMIX_SUCCESS;
}


int pmix_bfrop_close(void)
{
    int32_t i;

    if (!pmix_bfrop_initialized) {
        return PMIX_SUCCESS;
    }
    pmix_bfrop_initialized = false;

    for (i = 0 ; i < pmix_pointer_array_get_size(&pmix_bfrop_types) ; ++i) {
        pmix_bfrop_type_info_t *info = (pmix_bfrop_type_info_t*)pmix_pointer_array_get_item(&pmix_bfrop_types, i);
        if (NULL != info) {
            pmix_pointer_array_set_item(&pmix_bfrop_types, i, NULL);
            OBJ_RELEASE(info);
        }
    }

    OBJ_DESTRUCT(&pmix_bfrop_types);

    return PMIX_SUCCESS;
}
