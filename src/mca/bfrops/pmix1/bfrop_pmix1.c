/*
 * Copyright (c) 2004-2010 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-201 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2010-201 Oak Ridge National Labs.  All rights reserved.
 * Copyright (c) 201-2014 Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 201-2013 Los Alamos National Security, LLC.  All rights
 *                         reserved.
 * Copyright (c) 2013-2017 Intel, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 */

#include <src/include/pmix_config.h>

#include "src/mca/bfrops/base/base.h"
#include "bfrop_pmix1.h"

static pmix_status_t init(void);
static void finalize(void);
static pmix_status_t register_type(const char *name,
                                   pmix_data_type_t type,
                                   pmix_bfrop_pack_fn_t pack,
                                   pmix_bfrop_unpack_fn_t unpack,
                                   pmix_bfrop_copy_fn_t copy,
                                   pmix_bfrop_print_fn_t print);
static const char* data_type_string(pmix_data_type_t type);

pmix_bfrops_module_t pmix_bfrops_pmix1_module = {
    .version = "pmix1",
    .init = init,
    .finalize = finalize,
    .pack = pmix1_pack,
    .unpack = pmix1_unpack,
    .copy = pmix1_copy,
    .print = pmix1_print,
    .copy_payload = pmix_bfrops_base_copy_payload,
    .value_xfer = pmix_bfrops_base_value_xfer,
    .value_load = pmix_bfrops_base_value_load,
    .value_unload = pmix_bfrops_base_value_unload,
    .value_cmp = pmix_bfrops_base_value_cmp,
    .register_type = register_type,
    .data_type_string = data_type_string
};

static pmix_status_t init(void)
{
    /* some standard types don't require anything special */
    PMIX_REGISTER_TYPE("PMIX_BOOL", PMIX_BOOL,
                       pmix_bfrops_base_pack_bool,
                       pmix_bfrops_base_unpack_bool,
                       pmix_bfrops_base_std_copy,
                       pmix_bfrops_base_print_bool,
                       &mca_bfrops_pmix1_component.types);

    PMIX_REGISTER_TYPE("PMIX_BYTE", PMIX_BYTE,
                       pmix_bfrops_base_pack_byte,
                       pmix_bfrops_base_unpack_byte,
                       pmix_bfrops_base_std_copy,
                       pmix_bfrops_base_print_byte,
                       &mca_bfrops_pmix1_component.types);

    PMIX_REGISTER_TYPE("PMIX_STRING", PMIX_STRING,
                       pmix_bfrops_base_pack_string,
                       pmix_bfrops_base_unpack_string,
                       pmix_bfrops_base_copy_string,
                       pmix_bfrops_base_print_string,
                       &mca_bfrops_pmix1_component.types);

    /* Register the rest of the standard generic types to point to internal functions */
    PMIX_REGISTER_TYPE("PMIX_SIZE", PMIX_SIZE,
                       pmix_bfrops_base_pack_sizet,
                       pmix_bfrops_base_unpack_sizet,
                       pmix_bfrops_base_std_copy,
                       pmix_bfrops_base_print_size,
                       &mca_bfrops_pmix1_component.types);

    PMIX_REGISTER_TYPE("PMIX_PID", PMIX_PID,
                       pmix_bfrops_base_pack_pid,
                       pmix_bfrops_base_unpack_pid,
                       pmix_bfrops_base_std_copy,
                       pmix_bfrops_base_print_pid,
                       &mca_bfrops_pmix1_component.types);

    PMIX_REGISTER_TYPE("PMIX_INT", PMIX_INT,
                       pmix_bfrops_base_pack_int,
                       pmix_bfrops_base_unpack_int,
                       pmix_bfrops_base_std_copy,
                       pmix_bfrops_base_print_int,
                       &mca_bfrops_pmix1_component.types);

    /* Register all the standard fixed types to point to base functions */
    PMIX_REGISTER_TYPE("PMIX_INT8", PMIX_INT8,
                       pmix_bfrops_base_pack_byte,
                       pmix_bfrops_base_unpack_byte,
                       pmix_bfrops_base_std_copy,
                       pmix_bfrops_base_print_int8,
                       &mca_bfrops_pmix1_component.types);

    PMIX_REGISTER_TYPE("PMIX_INT16", PMIX_INT16,
                       pmix_bfrops_base_pack_int16,
                       pmix_bfrops_base_unpack_int16,
                       pmix_bfrops_base_std_copy,
                       pmix_bfrops_base_print_int16,
                       &mca_bfrops_pmix1_component.types);

    PMIX_REGISTER_TYPE("PMIX_INT32", PMIX_INT32,
                       pmix_bfrops_base_pack_int32,
                       pmix_bfrops_base_unpack_int32,
                       pmix_bfrops_base_std_copy,
                       pmix_bfrops_base_print_int32,
                       &mca_bfrops_pmix1_component.types);

    PMIX_REGISTER_TYPE("PMIX_INT64", PMIX_INT64,
                       pmix_bfrops_base_pack_int64,
                       pmix_bfrops_base_unpack_int64,
                       pmix_bfrops_base_std_copy,
                       pmix_bfrops_base_print_int64,
                       &mca_bfrops_pmix1_component.types);

    PMIX_REGISTER_TYPE("PMIX_UINT", PMIX_UINT,
                       pmix_bfrops_base_pack_int,
                       pmix_bfrops_base_unpack_int,
                       pmix_bfrops_base_std_copy,
                       pmix_bfrops_base_print_uint,
                       &mca_bfrops_pmix1_component.types);

    PMIX_REGISTER_TYPE("PMIX_UINT8", PMIX_UINT8,
                       pmix_bfrops_base_pack_byte,
                       pmix_bfrops_base_unpack_byte,
                       pmix_bfrops_base_std_copy,
                       pmix_bfrops_base_print_uint8,
                       &mca_bfrops_pmix1_component.types);

    PMIX_REGISTER_TYPE("PMIX_UINT16", PMIX_UINT16,
                       pmix_bfrops_base_pack_int16,
                       pmix_bfrops_base_unpack_int16,
                       pmix_bfrops_base_std_copy,
                       pmix_bfrops_base_print_uint16,
                       &mca_bfrops_pmix1_component.types);

    PMIX_REGISTER_TYPE("PMIX_UINT32", PMIX_UINT32,
                       pmix_bfrops_base_pack_int32,
                       pmix_bfrops_base_unpack_int32,
                       pmix_bfrops_base_std_copy,
                       pmix_bfrops_base_print_uint32,
                       &mca_bfrops_pmix1_component.types);

    PMIX_REGISTER_TYPE("PMIX_UINT64", PMIX_UINT64,
                       pmix_bfrops_base_pack_int64,
                       pmix_bfrops_base_unpack_int64,
                       pmix_bfrops_base_std_copy,
                       pmix_bfrops_base_print_uint64,
                       &mca_bfrops_pmix1_component.types);

    PMIX_REGISTER_TYPE("PMIX_FLOAT", PMIX_FLOAT,
                       pmix_bfrops_base_pack_float,
                       pmix_bfrops_base_unpack_float,
                       pmix_bfrops_base_std_copy,
                       pmix_bfrops_base_print_float,
                       &mca_bfrops_pmix1_component.types);

    PMIX_REGISTER_TYPE("PMIX_DOUBLE", PMIX_DOUBLE,
                       pmix_bfrops_base_pack_double,
                       pmix_bfrops_base_unpack_double,
                       pmix_bfrops_base_std_copy,
                       pmix_bfrops_base_print_double,
                       &mca_bfrops_pmix1_component.types);

    PMIX_REGISTER_TYPE("PMIX_TIMEVAL", PMIX_TIMEVAL,
                       pmix_bfrops_base_pack_timeval,
                       pmix_bfrops_base_unpack_timeval,
                       pmix_bfrops_base_std_copy,
                       pmix_bfrops_base_print_timeval,
                       &mca_bfrops_pmix1_component.types);

    PMIX_REGISTER_TYPE("PMIX_TIME", PMIX_TIME,
                       pmix_bfrops_base_pack_time,
                       pmix_bfrops_base_unpack_time,
                       pmix_bfrops_base_std_copy,
                       pmix_bfrops_base_print_time,
                       &mca_bfrops_pmix1_component.types);

    PMIX_REGISTER_TYPE("PMIX_STATUS", PMIX_STATUS,
                       pmix_bfrops_base_pack_status,
                       pmix_bfrops_base_unpack_status,
                       pmix_bfrops_base_std_copy,
                       pmix_bfrops_base_print_status,
                       &mca_bfrops_pmix1_component.types);

    /* structured values need to be done here as they might
     * callback into standard and/or derived values */
    PMIX_REGISTER_TYPE("PMIX_VALUE", PMIX_VALUE,
                       pmix1_pack_value,
                       pmix1_unpack_value,
                       pmix1_copy_value,
                       pmix1_print_value,
                       &mca_bfrops_pmix1_component.types);

    PMIX_REGISTER_TYPE("PMIX_PROC", PMIX_PROC,
                       pmix_bfrops_base_pack_proc,
                       pmix_bfrops_base_unpack_proc,
                       pmix_bfrops_base_copy_proc,
                       pmix_bfrops_base_print_proc,
                       &mca_bfrops_pmix1_component.types);

    PMIX_REGISTER_TYPE("PMIX_APP", PMIX_APP,
                       pmix1_pack_app, pmix1_unpack_app,
                       pmix_bfrops_base_copy_app,
                       pmix1_print_app,
                       &mca_bfrops_pmix1_component.types);

    PMIX_REGISTER_TYPE("PMIX_INFO", PMIX_INFO,
                       pmix1_pack_info, pmix1_unpack_info,
                       pmix_bfrops_base_copy_info,
                       pmix1_print_info,
                       &mca_bfrops_pmix1_component.types);

    PMIX_REGISTER_TYPE("PMIX_PDATA", PMIX_PDATA,
                       pmix1_pack_pdata, pmix1_unpack_pdata,
                       pmix_bfrops_base_copy_pdata,
                       pmix1_print_pdata,
                       &mca_bfrops_pmix1_component.types);

    PMIX_REGISTER_TYPE("PMIX_BUFFER", PMIX_BUFFER,
                       pmix_bfrops_base_pack_buf,
                       pmix_bfrops_base_unpack_buf,
                       pmix_bfrops_base_copy_buf,
                       pmix_bfrops_base_print_buf,
                       &mca_bfrops_pmix1_component.types);

    PMIX_REGISTER_TYPE("PMIX_BYTE_OBJECT", PMIX_BYTE_OBJECT,
                       pmix_bfrops_base_pack_bo,
                       pmix_bfrops_base_unpack_bo,
                       pmix_bfrops_base_copy_bo,
                       pmix_bfrops_base_print_bo,
                       &mca_bfrops_pmix1_component.types);

    PMIX_REGISTER_TYPE("PMIX_KVAL", PMIX_KVAL,
                       pmix1_pack_kval, pmix1_unpack_kval,
                       pmix_bfrops_base_copy_kval,
                       pmix_bfrops_base_print_kval,
                       &mca_bfrops_pmix1_component.types);

    PMIX_REGISTER_TYPE("PMIX_MODEX", PMIX_MODEX,
                       pmix_bfrops_base_pack_modex,
                       pmix_bfrops_base_unpack_modex,
                       pmix_bfrops_base_copy_modex,
                       pmix_bfrops_base_print_modex,
                       &mca_bfrops_pmix1_component.types);

    PMIX_REGISTER_TYPE("PMIX_PERSIST", PMIX_PERSIST,
                       pmix1_pack_persist,
                       pmix1_unpack_persist,
                       pmix1_std_copy,
                       pmix_bfrops_base_print_persist,
                       &mca_bfrops_pmix1_component.types);

    PMIX_REGISTER_TYPE("PMIX_POINTER", PMIX_POINTER,
                       pmix_bfrops_base_pack_ptr,
                       pmix_bfrops_base_unpack_ptr,
                       pmix_bfrops_base_std_copy,
                       pmix_bfrops_base_print_ptr,
                       &mca_bfrops_pmix1_component.types);

    PMIX_REGISTER_TYPE("PMIX_SCOPE", PMIX_SCOPE,
                       pmix1_pack_scope,
                       pmix1_unpack_scope,
                       pmix1_std_copy,
                       pmix_bfrops_base_print_scope,
                       &mca_bfrops_pmix1_component.types);

    PMIX_REGISTER_TYPE("PMIX_DATA_RANGE", PMIX_DATA_RANGE,
                       pmix1_pack_range,
                       pmix1_unpack_range,
                       pmix1_std_copy,
                       pmix_bfrops_base_print_ptr,
                       &mca_bfrops_pmix1_component.types);

    PMIX_REGISTER_TYPE("PMIX_COMMAND", PMIX_COMMAND,
                       pmix1_pack_cmd,
                       pmix1_unpack_cmd,
                       pmix1_std_copy,
                       pmix_bfrops_base_print_cmd,
                       &mca_bfrops_pmix1_component.types);

    PMIX_REGISTER_TYPE("PMIX_INFO_DIRECTIVES", PMIX_INFO_DIRECTIVES,
                       pmix1_pack_info_directives,
                       pmix1_unpack_info_directives,
                       pmix_bfrops_base_std_copy,
                       pmix_bfrops_base_print_info_directives,
                       &mca_bfrops_pmix1_component.types);

    PMIX_REGISTER_TYPE("PMIX_DATA_TYPE", PMIX_DATA_TYPE,
                       pmix1_pack_datatype,
                       pmix1_unpack_datatype,
                       pmix_bfrops_base_std_copy,
                       pmix_bfrops_base_print_datatype,
                       &mca_bfrops_pmix3_component.types);

    PMIX_REGISTER_TYPE("PMIX_PROC_STATE", PMIX_PROC_STATE,
                       pmix1_pack_unsupported,
                       pmix1_unpack_unsupported,
                       pmix1_copy_unsupported,
                       pmix1_print_unsupported,
                       &mca_bfrops_pmix3_component.types);

    PMIX_REGISTER_TYPE("PMIX_PROC_INFO", PMIX_PROC_INFO,
                       pmix1_pack_unsupported,
                       pmix1_unpack_unsupported,
                       pmix1_copy_unsupported,
                       pmix1_print_unsupported,
                       &mca_bfrops_pmix3_component.types);

    PMIX_REGISTER_TYPE("PMIX_DATA_ARRAY", PMIX_DATA_ARRAY,
                       pmix1_pack_darray,
                       pmix1_unpack_darray,
                       pmix1_copy_darray,
                       pmix1_print_darray,
                       &mca_bfrops_pmix3_component.types);

    PMIX_REGISTER_TYPE("PMIX_PROC_RANK", PMIX_PROC_RANK,
                       pmix1_pack_rank,
                       pmix1_unpack_rank,
                       pmix1_std_copy,
                       pmix1_print_rank,
                       &mca_bfrops_pmix3_component.types);

    PMIX_REGISTER_TYPE("PMIX_QUERY", PMIX_QUERY,
                       pmix1_pack_unsupported,
                       pmix1_unpack_unsupported,
                       pmix1_copy_unsupported,
                       pmix1_print_unsupported,
                       &mca_bfrops_pmix3_component.types);

    /**** DEPRECATED ****/
    PMIX_REGISTER_TYPE("PMIX_INFO_ARRAY", PMIX_INFO_ARRAY,
                       pmix1_pack_array, pmix1_unpack_array,
                       pmix_bfrops_base_copy_array,
                       pmix1_print_array,
                       &mca_bfrops_pmix1_component.types);
    /********************/


    return PMIX_SUCCESS;
}

static void finalize(void)
{
    int n;
    pmix_bfrop_type_info_t *info;

    for (n=0; n < mca_bfrops_pmix1_component.types.size; n++) {
        if (NULL != (info = (pmix_bfrop_type_info_t*)pmix_pointer_array_get_item(&mca_bfrops_pmix1_component.types, n))) {
            PMIX_RELEASE(info);
            pmix_pointer_array_set_item(&mca_bfrops_pmix1_component.types, n, NULL);
        }
    }
}

static pmix_status_t register_type(const char *name, pmix_data_type_t type,
                                   pmix_bfrop_pack_fn_t pack,
                                   pmix_bfrop_unpack_fn_t unpack,
                                   pmix_bfrop_copy_fn_t copy,
                                   pmix_bfrop_print_fn_t print)
{
    PMIX_REGISTER_TYPE(name, type,
                       pack, unpack,
                       copy, print,
                       &mca_bfrops_pmix1_component.types);
    return PMIX_SUCCESS;
}

static const char* data_type_string(pmix_data_type_t type)
{
    if (NULL == (info = (pmix_bfrop_type_info_t*)pmix_pointer_array_get_item(&mca_bfrops_pmix1_component.types, type))) {
        return NULL;
    }
    return info->odti_name;
}
