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

#ifdef HAVE_STRING_H
#include <string.h>
#endif

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
    pmix_bfrop_compare,
    pmix_bfrop_print,
    pmix_bfrop_structured,
    pmix_bfrop_peek,
    pmix_bfrop_unload,
    pmix_bfrop_load,
    pmix_bfrop_copy_payload,
    pmix_bfrop_register,
    pmix_bfrop_lookup_data_type,
    pmix_bfrop_dump_data_types,
    pmix_bfrop_dump
};

/**
 * Object constructors, destructors, and instantiations
 */
/** Value **/
static void pmix_value_construct(pmix_value_t* ptr)
{
    ptr->key = NULL;
    ptr->type = PMIX_UNDEF;
    memset(&ptr->data, 0, sizeof(ptr->data));
}
static void pmix_value_destruct(pmix_value_t* ptr)
{
    if (NULL != ptr->key) {
        free(ptr->key);
    }
    if (PMIX_STRING == ptr->type &&
        NULL != ptr->data.string) {
        free(ptr->data.string);
    }
    if (PMIX_BYTE_OBJECT == ptr->type &&
        NULL != ptr->data.bo.bytes) {
        free(ptr->data.bo.bytes);
    }
}
OBJ_CLASS_INSTANCE(pmix_value_t,
                   pmix_list_item_t,
                   pmix_value_construct,
                   pmix_value_destruct);


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
    obj->odti_compare_fn = NULL;
    obj->odti_print_fn = NULL;
    obj->odti_structured = false;
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


int pmix_bfrop_register_vars (void)
{
    char *enviro_val;

    enviro_val = getenv("PMIX_bfrop_debug");
    if (NULL != enviro_val) {  /* debug requested */
        pmix_bfrop_verbose = 0;
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

    /* setup the initial size of the buffer. */
    pmix_bfrop_initial_size = PMIX_BFROP_DEFAULT_INITIAL_SIZE;

    /* the threshold as to where to stop doubling the size of the buffer 
     * allocated memory and start doing additive increases */
    pmix_bfrop_threshold_size = PMIX_BFROP_DEFAULT_THRESHOLD_SIZE;

    return PMIX_SUCCESS;
}

int pmix_bfrop_open(void)
{
    int rc;
    pmix_data_type_t tmp;

    if (pmix_bfrop_initialized) {
        return PMIX_SUCCESS;
    }

    /* Setup the types array */
    OBJ_CONSTRUCT(&pmix_bfrop_types, pmix_pointer_array_t);
    if (PMIX_SUCCESS != (rc = pmix_pointer_array_init(&pmix_bfrop_types,
                                                      PMIX_BFROP_ID_DYNAMIC,
                                                      PMIX_BFROP_ID_MAX,
                                                      PMIX_BFROP_ID_MAX))) {
        return rc;
    }
    pmix_bfrop_num_reg_types = 0;

    /* Register all the intrinsic types */

    tmp = PMIX_NULL;
    if (PMIX_SUCCESS != (rc = pmix_bfrop.register_type(pmix_bfrop_pack_null,
                                          pmix_bfrop_unpack_null,
                                          (pmix_bfrop_copy_fn_t)pmix_bfrop_copy_null,
                                          (pmix_bfrop_compare_fn_t)pmix_bfrop_compare_null,
                                          (pmix_bfrop_print_fn_t)pmix_bfrop_print_null,
                                          PMIX_BFROP_UNSTRUCTURED,
                                          "PMIX_NULL", &tmp))) {
        return rc;
    }
    tmp = PMIX_BYTE;
    if (PMIX_SUCCESS != (rc = pmix_bfrop.register_type(pmix_bfrop_pack_byte,
                                          pmix_bfrop_unpack_byte,
                                          (pmix_bfrop_copy_fn_t)pmix_bfrop_std_copy,
                                          (pmix_bfrop_compare_fn_t)pmix_bfrop_compare_byte,
                                          (pmix_bfrop_print_fn_t)pmix_bfrop_print_byte,
                                          PMIX_BFROP_UNSTRUCTURED,
                                          "PMIX_BYTE", &tmp))) {
        return rc;
    }
    tmp = PMIX_BOOL;
    if (PMIX_SUCCESS != (rc = pmix_bfrop.register_type(pmix_bfrop_pack_bool,
                                          pmix_bfrop_unpack_bool,
                                          (pmix_bfrop_copy_fn_t)pmix_bfrop_std_copy,
                                          (pmix_bfrop_compare_fn_t)pmix_bfrop_compare_bool,
                                          (pmix_bfrop_print_fn_t)pmix_bfrop_print_bool,
                                          PMIX_BFROP_UNSTRUCTURED,
                                          "PMIX_BOOL", &tmp))) {
        return rc;
    }
    tmp = PMIX_INT;
    if (PMIX_SUCCESS != (rc = pmix_bfrop.register_type(pmix_bfrop_pack_int,
                                          pmix_bfrop_unpack_int,
                                          (pmix_bfrop_copy_fn_t)pmix_bfrop_std_copy,
                                          (pmix_bfrop_compare_fn_t)pmix_bfrop_compare_int,
                                          (pmix_bfrop_print_fn_t)pmix_bfrop_print_int,
                                          PMIX_BFROP_UNSTRUCTURED,
                                          "PMIX_INT", &tmp))) {
        return rc;
    }
    tmp = PMIX_UINT;
    if (PMIX_SUCCESS != (rc = pmix_bfrop.register_type(pmix_bfrop_pack_int,
                                          pmix_bfrop_unpack_int,
                                          (pmix_bfrop_copy_fn_t)pmix_bfrop_std_copy,
                                          (pmix_bfrop_compare_fn_t)pmix_bfrop_compare_uint,
                                          (pmix_bfrop_print_fn_t)pmix_bfrop_print_uint,
                                          PMIX_BFROP_UNSTRUCTURED,
                                          "PMIX_UINT", &tmp))) {
        return rc;
    }
    tmp = PMIX_INT8;
    if (PMIX_SUCCESS != (rc = pmix_bfrop.register_type(pmix_bfrop_pack_byte,
                                          pmix_bfrop_unpack_byte,
                                          (pmix_bfrop_copy_fn_t)pmix_bfrop_std_copy,
                                          (pmix_bfrop_compare_fn_t)pmix_bfrop_compare_int8,
                                          (pmix_bfrop_print_fn_t)pmix_bfrop_print_int8,
                                          PMIX_BFROP_UNSTRUCTURED,
                                          "PMIX_INT8", &tmp))) {
        return rc;
    }
    tmp = PMIX_UINT8;
    if (PMIX_SUCCESS != (rc = pmix_bfrop.register_type(pmix_bfrop_pack_byte,
                                          pmix_bfrop_unpack_byte,
                                          (pmix_bfrop_copy_fn_t)pmix_bfrop_std_copy,
                                          (pmix_bfrop_compare_fn_t)pmix_bfrop_compare_uint8,
                                          (pmix_bfrop_print_fn_t)pmix_bfrop_print_uint8,
                                          PMIX_BFROP_UNSTRUCTURED,
                                          "PMIX_UINT8", &tmp))) {
        return rc;
    }
    tmp = PMIX_INT16;
    if (PMIX_SUCCESS != (rc = pmix_bfrop.register_type(pmix_bfrop_pack_int16,
                                          pmix_bfrop_unpack_int16,
                                          (pmix_bfrop_copy_fn_t)pmix_bfrop_std_copy,
                                          (pmix_bfrop_compare_fn_t)pmix_bfrop_compare_int16,
                                          (pmix_bfrop_print_fn_t)pmix_bfrop_print_int16,
                                          PMIX_BFROP_UNSTRUCTURED,
                                          "PMIX_INT16", &tmp))) {
        return rc;
    }
    tmp = PMIX_UINT16;
    if (PMIX_SUCCESS != (rc = pmix_bfrop.register_type(pmix_bfrop_pack_int16,
                                          pmix_bfrop_unpack_int16,
                                          (pmix_bfrop_copy_fn_t)pmix_bfrop_std_copy,
                                          (pmix_bfrop_compare_fn_t)pmix_bfrop_compare_uint16,
                                          (pmix_bfrop_print_fn_t)pmix_bfrop_print_uint16,
                                          PMIX_BFROP_UNSTRUCTURED,
                                          "PMIX_UINT16", &tmp))) {
        return rc;
    }
    tmp = PMIX_INT32;
    if (PMIX_SUCCESS != (rc = pmix_bfrop.register_type(pmix_bfrop_pack_int32,
                                          pmix_bfrop_unpack_int32,
                                          (pmix_bfrop_copy_fn_t)pmix_bfrop_std_copy,
                                          (pmix_bfrop_compare_fn_t)pmix_bfrop_compare_int32,
                                          (pmix_bfrop_print_fn_t)pmix_bfrop_print_int32,
                                          PMIX_BFROP_UNSTRUCTURED,
                                          "PMIX_INT32", &tmp))) {
        return rc;
    }
    tmp = PMIX_UINT32;
    if (PMIX_SUCCESS != (rc = pmix_bfrop.register_type(pmix_bfrop_pack_int32,
                                          pmix_bfrop_unpack_int32,
                                          (pmix_bfrop_copy_fn_t)pmix_bfrop_std_copy,
                                          (pmix_bfrop_compare_fn_t)pmix_bfrop_compare_uint32,
                                          (pmix_bfrop_print_fn_t)pmix_bfrop_print_uint32,
                                          PMIX_BFROP_UNSTRUCTURED,
                                          "PMIX_UINT32", &tmp))) {
        return rc;
    }
    tmp = PMIX_INT64;
    if (PMIX_SUCCESS != (rc = pmix_bfrop.register_type(pmix_bfrop_pack_int64,
                                          pmix_bfrop_unpack_int64,
                                          (pmix_bfrop_copy_fn_t)pmix_bfrop_std_copy,
                                          (pmix_bfrop_compare_fn_t)pmix_bfrop_compare_int64,
                                          (pmix_bfrop_print_fn_t)pmix_bfrop_print_int64,
                                          PMIX_BFROP_UNSTRUCTURED,
                                          "PMIX_INT64", &tmp))) {
        return rc;
    }
    tmp = PMIX_UINT64;
    if (PMIX_SUCCESS != (rc = pmix_bfrop.register_type(pmix_bfrop_pack_int64,
                                          pmix_bfrop_unpack_int64,
                                          (pmix_bfrop_copy_fn_t)pmix_bfrop_std_copy,
                                          (pmix_bfrop_compare_fn_t)pmix_bfrop_compare_uint64,
                                          (pmix_bfrop_print_fn_t)pmix_bfrop_print_uint64,
                                          PMIX_BFROP_UNSTRUCTURED,
                                          "PMIX_UINT64", &tmp))) {
        return rc;
    }
    tmp = PMIX_SIZE;
    if (PMIX_SUCCESS != (rc = pmix_bfrop.register_type(pmix_bfrop_pack_sizet,
                                          pmix_bfrop_unpack_sizet,
                                          (pmix_bfrop_copy_fn_t)pmix_bfrop_std_copy,
                                          (pmix_bfrop_compare_fn_t)pmix_bfrop_compare_size,
                                          (pmix_bfrop_print_fn_t)pmix_bfrop_print_size,
                                          PMIX_BFROP_UNSTRUCTURED,
                                          "PMIX_SIZE", &tmp))) {
        return rc;
    }
    tmp = PMIX_PID;
    if (PMIX_SUCCESS != (rc = pmix_bfrop.register_type(pmix_bfrop_pack_pid,
                                          pmix_bfrop_unpack_pid,
                                          (pmix_bfrop_copy_fn_t)pmix_bfrop_std_copy,
                                          (pmix_bfrop_compare_fn_t)pmix_bfrop_compare_pid,
                                          (pmix_bfrop_print_fn_t)pmix_bfrop_print_pid,
                                          PMIX_BFROP_UNSTRUCTURED,
                                          "PMIX_PID", &tmp))) {
        return rc;
    }
    tmp = PMIX_STRING;
    if (PMIX_SUCCESS != (rc = pmix_bfrop.register_type(pmix_bfrop_pack_string,
                                          pmix_bfrop_unpack_string,
                                          (pmix_bfrop_copy_fn_t)pmix_bfrop_copy_string,
                                          (pmix_bfrop_compare_fn_t)pmix_bfrop_compare_string,
                                          (pmix_bfrop_print_fn_t)pmix_bfrop_print_string,
                                          PMIX_BFROP_STRUCTURED,
                                          "PMIX_STRING", &tmp))) {
        return rc;
    }
    tmp = PMIX_DATA_TYPE;
    if (PMIX_SUCCESS != (rc = pmix_bfrop.register_type(pmix_bfrop_pack_data_type,
                                          pmix_bfrop_unpack_data_type,
                                          (pmix_bfrop_copy_fn_t)pmix_bfrop_std_copy,
                                          (pmix_bfrop_compare_fn_t)pmix_bfrop_compare_dt,
                                          (pmix_bfrop_print_fn_t)pmix_bfrop_print_data_type,
                                          PMIX_BFROP_UNSTRUCTURED,
                                          "PMIX_DATA_TYPE", &tmp))) {
        return rc;
    }

    tmp = PMIX_BYTE_OBJECT;
    if (PMIX_SUCCESS != (rc = pmix_bfrop.register_type(pmix_bfrop_pack_byte_object,
                                          pmix_bfrop_unpack_byte_object,
                                          (pmix_bfrop_copy_fn_t)pmix_bfrop_copy_byte_object,
                                          (pmix_bfrop_compare_fn_t)pmix_bfrop_compare_byte_object,
                                          (pmix_bfrop_print_fn_t)pmix_bfrop_print_byte_object,
                                          PMIX_BFROP_STRUCTURED,
                                          "PMIX_BYTE_OBJECT", &tmp))) {
        return rc;
    }

    tmp = PMIX_VALUE;
    if (PMIX_SUCCESS != (rc = pmix_bfrop.register_type(pmix_bfrop_pack_value,
                                                     pmix_bfrop_unpack_value,
                                                     (pmix_bfrop_copy_fn_t)pmix_bfrop_copy_value,
                                                     (pmix_bfrop_compare_fn_t)pmix_bfrop_compare_value,
                                                     (pmix_bfrop_print_fn_t)pmix_bfrop_print_value,
                                                     PMIX_BFROP_STRUCTURED,
                                                     "PMIX_VALUE", &tmp))) {
        return rc;
    }
    tmp = PMIX_BUFFER;
    if (PMIX_SUCCESS != (rc = pmix_bfrop.register_type(pmix_bfrop_pack_buffer_contents,
                                                     pmix_bfrop_unpack_buffer_contents,
                                                     (pmix_bfrop_copy_fn_t)pmix_bfrop_copy_buffer_contents,
                                                     (pmix_bfrop_compare_fn_t)pmix_bfrop_compare_buffer_contents,
                                                     (pmix_bfrop_print_fn_t)pmix_bfrop_print_buffer_contents,
                                                     PMIX_BFROP_STRUCTURED,
                                                     "PMIX_BUFFER", &tmp))) {
        return rc;
    }
    tmp = PMIX_FLOAT;
    if (PMIX_SUCCESS != (rc = pmix_bfrop.register_type(pmix_bfrop_pack_float,
                                                     pmix_bfrop_unpack_float,
                                                     (pmix_bfrop_copy_fn_t)pmix_bfrop_std_copy,
                                                     (pmix_bfrop_compare_fn_t)pmix_bfrop_compare_float,
                                                     (pmix_bfrop_print_fn_t)pmix_bfrop_print_float,
                                                     PMIX_BFROP_UNSTRUCTURED,
                                                     "PMIX_FLOAT", &tmp))) {
        return rc;
    }
    tmp = PMIX_DOUBLE;
    if (PMIX_SUCCESS != (rc = pmix_bfrop.register_type(pmix_bfrop_pack_double,
                                                     pmix_bfrop_unpack_double,
                                                     (pmix_bfrop_copy_fn_t)pmix_bfrop_std_copy,
                                                     (pmix_bfrop_compare_fn_t)pmix_bfrop_compare_double,
                                                     (pmix_bfrop_print_fn_t)pmix_bfrop_print_double,
                                                     PMIX_BFROP_UNSTRUCTURED,
                                                     "PMIX_DOUBLE", &tmp))) {
        return rc;
    }
    tmp = PMIX_TIMEVAL;
    if (PMIX_SUCCESS != (rc = pmix_bfrop.register_type(pmix_bfrop_pack_timeval,
                                                     pmix_bfrop_unpack_timeval,
                                                     (pmix_bfrop_copy_fn_t)pmix_bfrop_std_copy,
                                                     (pmix_bfrop_compare_fn_t)pmix_bfrop_compare_timeval,
                                                     (pmix_bfrop_print_fn_t)pmix_bfrop_print_timeval,
                                                     PMIX_BFROP_UNSTRUCTURED,
                                                     "PMIX_TIMEVAL", &tmp))) {
        return rc;
    }
     tmp = PMIX_TIME;
    if (PMIX_SUCCESS != (rc = pmix_bfrop.register_type(pmix_bfrop_pack_time,
                                                     pmix_bfrop_unpack_time,
                                                     (pmix_bfrop_copy_fn_t)pmix_bfrop_std_copy,
                                                     (pmix_bfrop_compare_fn_t)pmix_bfrop_compare_time,
                                                     (pmix_bfrop_print_fn_t)pmix_bfrop_print_time,
                                                     PMIX_BFROP_UNSTRUCTURED,
                                                     "PMIX_TIME", &tmp))) {
        return rc;
    }
     tmp = PMIX_INFO;
    if (PMIX_SUCCESS != (rc = pmix_bfrop.register_type(pmix_bfrop_pack_info,
                                                     pmix_bfrop_unpack_info,
                                                     (pmix_bfrop_copy_fn_t)pmix_bfrop_copy_info,
                                                     (pmix_bfrop_compare_fn_t)pmix_bfrop_compare_info,
                                                     (pmix_bfrop_print_fn_t)pmix_bfrop_print_info,
                                                     PMIX_BFROP_STRUCTURED,
                                                     "PMIX_INFO", &tmp))) {
        return rc;
    }
     tmp = PMIX_APP;
    if (PMIX_SUCCESS != (rc = pmix_bfrop.register_type(pmix_bfrop_pack_app,
                                                     pmix_bfrop_unpack_apps,
                                                     (pmix_bfrop_copy_fn_t)pmix_bfrop_copy_app,
                                                     (pmix_bfrop_compare_fn_t)pmix_bfrop_compare_app,
                                                     (pmix_bfrop_print_fn_t)pmix_bfrop_print_app,
                                                     PMIX_BFROP_STRUCTURED,
                                                     "PMIX_APP", &tmp))) {
        return rc;
    }
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

bool pmix_bfrop_structured(pmix_data_type_t type)
{
    int i;

    /* find the type */
    for (i = 0 ; i < pmix_bfrop_types.size ; ++i) {
        pmix_bfrop_type_info_t *info = (pmix_bfrop_type_info_t*)pmix_pointer_array_get_item(&pmix_bfrop_types, i);
        if (NULL != info && info->odti_type == type) {
            return info->odti_structured;
        }
    }

    /* default to false */
    return false;
}
