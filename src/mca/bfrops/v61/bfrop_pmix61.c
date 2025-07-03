/*
 * Copyright (c) 2005-2010 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2005-2011 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2005      High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2005      The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2010-2011 Oak Ridge National Labs.  All rights reserved.
 * Copyright (c) 2011-2015 Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2011-2015 Los Alamos National Security, LLC.  All rights
 *                         reserved.
 * Copyright (c) 2015-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2019      IBM Corporation.  All rights reserved.
 * Copyright (c) 2021-2025 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 */

#include "src/include/pmix_config.h"

#include "bfrop_pmix61.h"
#include "src/mca/bfrops/base/base.h"

#include "src/util/pmix_error.h"

static pmix_status_t init(void);
static void finalize(void);
static pmix_status_t pmix61_pack(pmix_buffer_t *buffer, const void *src, int num_vals,
                                 pmix_data_type_t type);
static pmix_status_t pmix61_unpack(pmix_buffer_t *buffer, void *dest, int32_t *num_vals,
                                   pmix_data_type_t type);
static pmix_status_t pmix61_copy(void **dest, void *src, pmix_data_type_t type);
static pmix_status_t pmix61_print(char **output, char *prefix, void *src, pmix_data_type_t type);
static const char *data_type_string(pmix_data_type_t type);

pmix_bfrops_module_t pmix_bfrops_pmix61_module = {
    .version = "v61",
    .init = init,
    .finalize = finalize,
    .pack = pmix61_pack,
    .unpack = pmix61_unpack,
    .copy = pmix61_copy,
    .print = pmix61_print,
    .copy_payload = pmix_bfrops_base_copy_payload,
    .value_xfer = pmix_bfrops_base_value_xfer,
    .value_load = pmix_bfrops_base_value_load,
    .value_unload = pmix_bfrops_base_value_unload,
    .value_cmp = pmix_bfrops_base_value_cmp,
    .data_type_string = data_type_string
};

static pmix_status_t init(void)
{
    /* some standard types don't require anything special */
    PMIX_REGISTER_TYPE("PMIX_BOOL", PMIX_BOOL, pmix_bfrops_base_pack_bool,
                       pmix_bfrops_base_unpack_bool, pmix_bfrops_base_std_copy,
                       pmix_bfrops_base_print_bool, &pmix_mca_bfrops_v61_component.types);

    PMIX_REGISTER_TYPE("PMIX_BYTE", PMIX_BYTE, pmix_bfrops_base_pack_byte,
                       pmix_bfrops_base_unpack_byte, pmix_bfrops_base_std_copy,
                       pmix_bfrops_base_print_byte, &pmix_mca_bfrops_v61_component.types);

    PMIX_REGISTER_TYPE("PMIX_STRING", PMIX_STRING, pmix_bfrops_base_pack_string,
                       pmix_bfrops_base_unpack_string, pmix_bfrops_base_copy_string,
                       pmix_bfrops_base_print_string, &pmix_mca_bfrops_v61_component.types);

    /* Register the rest of the standard generic types to point to internal functions */
    PMIX_REGISTER_TYPE("PMIX_SIZE", PMIX_SIZE, pmix_bfrops_base_pack_sizet,
                       pmix_bfrops_base_unpack_sizet, pmix_bfrops_base_std_copy,
                       pmix_bfrops_base_print_size, &pmix_mca_bfrops_v61_component.types);

    PMIX_REGISTER_TYPE("PMIX_PID", PMIX_PID, pmix_bfrops_base_pack_pid, pmix_bfrops_base_unpack_pid,
                       pmix_bfrops_base_std_copy, pmix_bfrops_base_print_pid,
                       &pmix_mca_bfrops_v61_component.types);

    PMIX_REGISTER_TYPE("PMIX_INT", PMIX_INT, pmix_bfrops_base_pack_int,
                       pmix_bfrops_base_unpack_int, pmix_bfrops_base_std_copy,
                       pmix_bfrops_base_print_int, &pmix_mca_bfrops_v61_component.types);

    /* Register all the standard fixed types to point to base functions */
    PMIX_REGISTER_TYPE("PMIX_INT8", PMIX_INT8, pmix_bfrops_base_pack_byte,
                       pmix_bfrops_base_unpack_byte, pmix_bfrops_base_std_copy,
                       pmix_bfrops_base_print_int8, &pmix_mca_bfrops_v61_component.types);

    PMIX_REGISTER_TYPE("PMIX_INT16", PMIX_INT16, pmix_bfrops_base_pack_general_int,
                       pmix_bfrops_base_unpack_general_int, pmix_bfrops_base_std_copy,
                       pmix_bfrops_base_print_int16, &pmix_mca_bfrops_v61_component.types);

    PMIX_REGISTER_TYPE("PMIX_INT32", PMIX_INT32, pmix_bfrops_base_pack_general_int,
                       pmix_bfrops_base_unpack_general_int, pmix_bfrops_base_std_copy,
                       pmix_bfrops_base_print_int32, &pmix_mca_bfrops_v61_component.types);

    PMIX_REGISTER_TYPE("PMIX_INT64", PMIX_INT64, pmix_bfrops_base_pack_general_int,
                       pmix_bfrops_base_unpack_general_int, pmix_bfrops_base_std_copy,
                       pmix_bfrops_base_print_int64, &pmix_mca_bfrops_v61_component.types);

    PMIX_REGISTER_TYPE("PMIX_UINT", PMIX_UINT, pmix_bfrops_base_pack_int,
                       pmix_bfrops_base_unpack_int, pmix_bfrops_base_std_copy,
                       pmix_bfrops_base_print_uint, &pmix_mca_bfrops_v61_component.types);

    PMIX_REGISTER_TYPE("PMIX_UINT8", PMIX_UINT8, pmix_bfrops_base_pack_byte,
                       pmix_bfrops_base_unpack_byte, pmix_bfrops_base_std_copy,
                       pmix_bfrops_base_print_uint8, &pmix_mca_bfrops_v61_component.types);

    PMIX_REGISTER_TYPE("PMIX_UINT16", PMIX_UINT16, pmix_bfrops_base_pack_general_int,
                       pmix_bfrops_base_unpack_general_int, pmix_bfrops_base_std_copy,
                       pmix_bfrops_base_print_uint16, &pmix_mca_bfrops_v61_component.types);

    PMIX_REGISTER_TYPE("PMIX_UINT32", PMIX_UINT32, pmix_bfrops_base_pack_general_int,
                       pmix_bfrops_base_unpack_general_int, pmix_bfrops_base_std_copy,
                       pmix_bfrops_base_print_uint32, &pmix_mca_bfrops_v61_component.types);

    PMIX_REGISTER_TYPE("PMIX_UINT64", PMIX_UINT64, pmix_bfrops_base_pack_general_int,
                       pmix_bfrops_base_unpack_general_int, pmix_bfrops_base_std_copy,
                       pmix_bfrops_base_print_uint64, &pmix_mca_bfrops_v61_component.types);

    PMIX_REGISTER_TYPE("PMIX_FLOAT", PMIX_FLOAT, pmix_bfrops_base_pack_float,
                       pmix_bfrops_base_unpack_float, pmix_bfrops_base_std_copy,
                       pmix_bfrops_base_print_float, &pmix_mca_bfrops_v61_component.types);

    PMIX_REGISTER_TYPE("PMIX_DOUBLE", PMIX_DOUBLE, pmix_bfrops_base_pack_double,
                       pmix_bfrops_base_unpack_double, pmix_bfrops_base_std_copy,
                       pmix_bfrops_base_print_double, &pmix_mca_bfrops_v61_component.types);

    PMIX_REGISTER_TYPE("PMIX_TIMEVAL", PMIX_TIMEVAL, pmix_bfrops_base_pack_timeval,
                       pmix_bfrops_base_unpack_timeval, pmix_bfrops_base_std_copy,
                       pmix_bfrops_base_print_timeval, &pmix_mca_bfrops_v61_component.types);

    PMIX_REGISTER_TYPE("PMIX_TIME", PMIX_TIME, pmix_bfrops_base_pack_time,
                       pmix_bfrops_base_unpack_time, pmix_bfrops_base_std_copy,
                       pmix_bfrops_base_print_time, &pmix_mca_bfrops_v61_component.types);

    PMIX_REGISTER_TYPE("PMIX_STATUS", PMIX_STATUS, pmix_bfrops_base_pack_status,
                       pmix_bfrops_base_unpack_status, pmix_bfrops_base_std_copy,
                       pmix_bfrops_base_print_status, &pmix_mca_bfrops_v61_component.types);

    PMIX_REGISTER_TYPE("PMIX_VALUE", PMIX_VALUE, pmix_bfrops_base_pack_value,
                       pmix_bfrops_base_unpack_value, pmix_bfrops_base_copy_value,
                       pmix_bfrops_base_print_value, &pmix_mca_bfrops_v61_component.types);

    PMIX_REGISTER_TYPE("PMIX_PROC", PMIX_PROC, pmix_bfrops_base_pack_proc,
                       pmix_bfrops_base_unpack_proc, pmix_bfrops_base_copy_proc,
                       pmix_bfrops_base_print_proc, &pmix_mca_bfrops_v61_component.types);

    PMIX_REGISTER_TYPE("PMIX_APP", PMIX_APP, pmix_bfrops_base_pack_app, pmix_bfrops_base_unpack_app,
                       pmix_bfrops_base_copy_app, pmix_bfrops_base_print_app,
                       &pmix_mca_bfrops_v61_component.types);

    PMIX_REGISTER_TYPE("PMIX_INFO", PMIX_INFO, pmix_bfrops_base_pack_info,
                       pmix_bfrops_base_unpack_info, pmix_bfrops_base_copy_info,
                       pmix_bfrops_base_print_info, &pmix_mca_bfrops_v61_component.types);

    PMIX_REGISTER_TYPE("PMIX_PDATA", PMIX_PDATA, pmix_bfrops_base_pack_pdata,
                       pmix_bfrops_base_unpack_pdata, pmix_bfrops_base_copy_pdata,
                       pmix_bfrops_base_print_pdata, &pmix_mca_bfrops_v61_component.types);

    PMIX_REGISTER_TYPE("PMIX_BUFFER", PMIX_BUFFER, pmix_bfrops_base_pack_buf,
                       pmix_bfrops_base_unpack_buf, pmix_bfrops_base_copy_buf,
                       pmix_bfrops_base_print_buf, &pmix_mca_bfrops_v61_component.types);

    PMIX_REGISTER_TYPE("PMIX_BYTE_OBJECT", PMIX_BYTE_OBJECT, pmix_bfrops_base_pack_bo,
                       pmix_bfrops_base_unpack_bo, pmix_bfrops_base_copy_bo,
                       pmix_bfrops_base_print_bo, &pmix_mca_bfrops_v61_component.types);

    PMIX_REGISTER_TYPE("PMIX_KVAL", PMIX_KVAL, pmix_bfrops_base_pack_kval,
                       pmix_bfrops_base_unpack_kval, pmix_bfrops_base_copy_kval,
                       pmix_bfrops_base_print_kval, &pmix_mca_bfrops_v61_component.types);

    /* these are fixed-sized values and can be done by base */
    PMIX_REGISTER_TYPE("PMIX_PERSIST", PMIX_PERSIST, pmix_bfrops_base_pack_persist,
                       pmix_bfrops_base_unpack_persist, pmix_bfrops_base_std_copy,
                       pmix_bfrops_base_print_persist, &pmix_mca_bfrops_v61_component.types);

    PMIX_REGISTER_TYPE("PMIX_POINTER", PMIX_POINTER, pmix_bfrops_base_pack_ptr,
                       pmix_bfrops_base_unpack_ptr, pmix_bfrops_base_std_copy,
                       pmix_bfrops_base_print_ptr, &pmix_mca_bfrops_v61_component.types);

    PMIX_REGISTER_TYPE("PMIX_SCOPE", PMIX_SCOPE, pmix_bfrops_base_pack_scope,
                       pmix_bfrops_base_unpack_scope, pmix_bfrops_base_std_copy,
                       pmix_bfrops_base_print_scope, &pmix_mca_bfrops_v61_component.types);

    PMIX_REGISTER_TYPE("PMIX_DATA_RANGE", PMIX_DATA_RANGE, pmix_bfrops_base_pack_range,
                       pmix_bfrops_base_unpack_range, pmix_bfrops_base_std_copy,
                       pmix_bfrops_base_print_ptr, &pmix_mca_bfrops_v61_component.types);

    PMIX_REGISTER_TYPE("PMIX_COMMAND", PMIX_COMMAND, pmix_bfrops_base_pack_cmd,
                       pmix_bfrops_base_unpack_cmd, pmix_bfrops_base_std_copy,
                       pmix_bfrops_base_print_cmd, &pmix_mca_bfrops_v61_component.types);

    PMIX_REGISTER_TYPE("PMIX_INFO_DIRECTIVES", PMIX_INFO_DIRECTIVES,
                       pmix_bfrops_base_pack_info_directives,
                       pmix_bfrops_base_unpack_info_directives, pmix_bfrops_base_std_copy,
                       pmix_bfrops_base_print_info_directives, &pmix_mca_bfrops_v61_component.types);

    PMIX_REGISTER_TYPE("PMIX_DATA_TYPE", PMIX_DATA_TYPE, pmix_bfrops_base_pack_datatype,
                       pmix_bfrops_base_unpack_datatype, pmix_bfrops_base_std_copy,
                       pmix_bfrops_base_print_datatype, &pmix_mca_bfrops_v61_component.types);

    PMIX_REGISTER_TYPE("PMIX_PROC_STATE", PMIX_PROC_STATE, pmix_bfrops_base_pack_pstate,
                       pmix_bfrops_base_unpack_pstate, pmix_bfrops_base_std_copy,
                       pmix_bfrops_base_print_pstate, &pmix_mca_bfrops_v61_component.types);

    PMIX_REGISTER_TYPE("PMIX_PROC_INFO", PMIX_PROC_INFO, pmix_bfrops_base_pack_pinfo,
                       pmix_bfrops_base_unpack_pinfo, pmix_bfrops_base_copy_pinfo,
                       pmix_bfrops_base_print_pinfo, &pmix_mca_bfrops_v61_component.types);

    PMIX_REGISTER_TYPE("PMIX_DATA_ARRAY", PMIX_DATA_ARRAY, pmix_bfrops_base_pack_darray,
                       pmix_bfrops_base_unpack_darray, pmix_bfrops_base_copy_darray,
                       pmix_bfrops_base_print_darray, &pmix_mca_bfrops_v61_component.types);

    PMIX_REGISTER_TYPE("PMIX_PROC_RANK", PMIX_PROC_RANK, pmix_bfrops_base_pack_rank,
                       pmix_bfrops_base_unpack_rank, pmix_bfrops_base_std_copy,
                       pmix_bfrops_base_print_rank, &pmix_mca_bfrops_v61_component.types);

    PMIX_REGISTER_TYPE("PMIX_QUERY", PMIX_QUERY, pmix_bfrops_base_pack_query,
                       pmix_bfrops_base_unpack_query, pmix_bfrops_base_copy_query,
                       pmix_bfrops_base_print_query, &pmix_mca_bfrops_v61_component.types);

    PMIX_REGISTER_TYPE("PMIX_COMPRESSED_STRING", PMIX_COMPRESSED_STRING, pmix_bfrops_base_pack_bo,
                       pmix_bfrops_base_unpack_bo, pmix_bfrops_base_copy_bo,
                       pmix_bfrops_base_print_bo, &pmix_mca_bfrops_v61_component.types);

    PMIX_REGISTER_TYPE("PMIX_ALLOC_DIRECTIVE", PMIX_ALLOC_DIRECTIVE,
                       pmix_bfrops_base_pack_alloc_directive,
                       pmix_bfrops_base_unpack_alloc_directive, pmix_bfrops_base_std_copy,
                       pmix_bfrops_base_print_alloc_directive, &pmix_mca_bfrops_v61_component.types);

    PMIX_REGISTER_TYPE("PMIX_RESBLOCK_DIRECTIVE", PMIX_RESBLOCK_DIRECTIVE,
                       pmix_bfrops_base_pack_resblock_directive,
                       pmix_bfrops_base_unpack_resblock_directive, pmix_bfrops_base_std_copy,
                       pmix_bfrops_base_print_resblock_directive, &pmix_mca_bfrops_v61_component.types);

    PMIX_REGISTER_TYPE("PMIX_IOF_CHANNEL", PMIX_IOF_CHANNEL, pmix_bfrops_base_pack_iof_channel,
                       pmix_bfrops_base_unpack_iof_channel, pmix_bfrops_base_std_copy,
                       pmix_bfrops_base_print_iof_channel, &pmix_mca_bfrops_v61_component.types);

    PMIX_REGISTER_TYPE("PMIX_ENVAR", PMIX_ENVAR, pmix_bfrops_base_pack_envar,
                       pmix_bfrops_base_unpack_envar, pmix_bfrops_base_copy_envar,
                       pmix_bfrops_base_print_envar, &pmix_mca_bfrops_v61_component.types);

    PMIX_REGISTER_TYPE("PMIX_COORD", PMIX_COORD, pmix_bfrops_base_pack_coord,
                       pmix_bfrops_base_unpack_coord, pmix_bfrops_base_copy_coord,
                       pmix_bfrops_base_print_coord, &pmix_mca_bfrops_v61_component.types);

    PMIX_REGISTER_TYPE("PMIX_REGATTR", PMIX_REGATTR, pmix_bfrops_base_pack_regattr,
                       pmix_bfrops_base_unpack_regattr, pmix_bfrops_base_copy_regattr,
                       pmix_bfrops_base_print_regattr, &pmix_mca_bfrops_v61_component.types);

    PMIX_REGISTER_TYPE("PMIX_REGEX", PMIX_REGEX, pmix_bfrops_base_pack_regex,
                       pmix_bfrops_base_unpack_regex, pmix_bfrops_base_copy_regex,
                       pmix_bfrops_base_print_regex, &pmix_mca_bfrops_v61_component.types);

    PMIX_REGISTER_TYPE("PMIX_JOB_STATE", PMIX_JOB_STATE, pmix_bfrops_base_pack_jobstate,
                       pmix_bfrops_base_unpack_jobstate, pmix_bfrops_base_std_copy,
                       pmix_bfrops_base_print_jobstate, &pmix_mca_bfrops_v61_component.types);

    PMIX_REGISTER_TYPE("PMIX_LINK_STATE", PMIX_LINK_STATE, pmix_bfrops_base_pack_linkstate,
                       pmix_bfrops_base_unpack_linkstate, pmix_bfrops_base_std_copy,
                       pmix_bfrops_base_print_linkstate, &pmix_mca_bfrops_v61_component.types);

    PMIX_REGISTER_TYPE("PMIX_PROC_CPUSET", PMIX_PROC_CPUSET, pmix_bfrops_base_pack_cpuset,
                       pmix_bfrops_base_unpack_cpuset, pmix_bfrops_base_copy_cpuset,
                       pmix_bfrops_base_print_cpuset, &pmix_mca_bfrops_v61_component.types);

    PMIX_REGISTER_TYPE("PMIX_GEOMETRY", PMIX_GEOMETRY, pmix_bfrops_base_pack_geometry,
                       pmix_bfrops_base_unpack_geometry, pmix_bfrops_base_copy_geometry,
                       pmix_bfrops_base_print_geometry, &pmix_mca_bfrops_v61_component.types);

    PMIX_REGISTER_TYPE("PMIX_DEVICE", PMIX_DEVICE, pmix_bfrops_base_pack_device,
                       pmix_bfrops_base_unpack_device, pmix_bfrops_base_copy_device,
                       pmix_bfrops_base_print_device, &pmix_mca_bfrops_v61_component.types);

    PMIX_REGISTER_TYPE("PMIX_RESOURCE_UNIT", PMIX_RESOURCE_UNIT, pmix_bfrops_base_pack_resunit,
                       pmix_bfrops_base_unpack_resunit, pmix_bfrops_base_copy_resunit,
                       pmix_bfrops_base_print_resunit, &pmix_mca_bfrops_v61_component.types);

    PMIX_REGISTER_TYPE("PMIX_DEVICE_DIST", PMIX_DEVICE_DIST, pmix_bfrops_base_pack_devdist,
                       pmix_bfrops_base_unpack_devdist, pmix_bfrops_base_copy_devdist,
                       pmix_bfrops_base_print_devdist, &pmix_mca_bfrops_v61_component.types);

    PMIX_REGISTER_TYPE("PMIX_ENDPOINT", PMIX_ENDPOINT, pmix_bfrops_base_pack_endpoint,
                       pmix_bfrops_base_unpack_endpoint, pmix_bfrops_base_copy_endpoint,
                       pmix_bfrops_base_print_endpoint, &pmix_mca_bfrops_v61_component.types);

    PMIX_REGISTER_TYPE("PMIX_TOPO", PMIX_TOPO, pmix_bfrops_base_pack_topology,
                       pmix_bfrops_base_unpack_topology, pmix_bfrops_base_copy_topology,
                       pmix_bfrops_base_print_topology, &pmix_mca_bfrops_v61_component.types);

    PMIX_REGISTER_TYPE("PMIX_DEVTYPE", PMIX_DEVTYPE, pmix_bfrops_base_pack_devtype,
                       pmix_bfrops_base_unpack_devtype, pmix_bfrops_base_std_copy,
                       pmix_bfrops_base_print_devtype, &pmix_mca_bfrops_v61_component.types);

    PMIX_REGISTER_TYPE("PMIX_LOCTYPE", PMIX_LOCTYPE, pmix_bfrops_base_pack_locality,
                       pmix_bfrops_base_unpack_locality, pmix_bfrops_base_std_copy,
                       pmix_bfrops_base_print_locality, &pmix_mca_bfrops_v61_component.types);

    PMIX_REGISTER_TYPE("PMIX_COMPRESSED_BYTE_OBJECT", PMIX_COMPRESSED_BYTE_OBJECT,
                       pmix_bfrops_base_pack_bo, pmix_bfrops_base_unpack_bo,
                       pmix_bfrops_base_copy_bo, pmix_bfrops_base_print_bo,
                       &pmix_mca_bfrops_v61_component.types);

    PMIX_REGISTER_TYPE("PMIX_PROC_NSPACE", PMIX_PROC_NSPACE, pmix_bfrops_base_pack_nspace,
                       pmix_bfrops_base_unpack_nspace, pmix_bfrops_base_copy_nspace,
                       pmix_bfrops_base_print_nspace, &pmix_mca_bfrops_v61_component.types);

    PMIX_REGISTER_TYPE("PMIX_DATA_BUFFER", PMIX_DATA_BUFFER, pmix_bfrops_base_pack_dbuf,
                       pmix_bfrops_base_unpack_dbuf, pmix_bfrops_base_copy_dbuf,
                       pmix_bfrops_base_print_dbuf, &pmix_mca_bfrops_v61_component.types);

    PMIX_REGISTER_TYPE("PMIX_STOR_MEDIUM", PMIX_STOR_MEDIUM, pmix_bfrops_base_pack_smed,
                       pmix_bfrops_base_unpack_smed, pmix_bfrops_base_std_copy,
                       pmix_bfrops_base_print_smed, &pmix_mca_bfrops_v61_component.types);

    PMIX_REGISTER_TYPE("PMIX_STOR_ACCESS", PMIX_STOR_ACCESS, pmix_bfrops_base_pack_sacc,
                       pmix_bfrops_base_unpack_sacc, pmix_bfrops_base_std_copy,
                       pmix_bfrops_base_print_sacc, &pmix_mca_bfrops_v61_component.types);

    PMIX_REGISTER_TYPE("PMIX_STOR_PERSIST", PMIX_STOR_PERSIST, pmix_bfrops_base_pack_spers,
                       pmix_bfrops_base_unpack_spers, pmix_bfrops_base_std_copy,
                       pmix_bfrops_base_print_spers, &pmix_mca_bfrops_v61_component.types);

    PMIX_REGISTER_TYPE("PMIX_STOR_ACCESS_TYPE", PMIX_STOR_ACCESS_TYPE, pmix_bfrops_base_pack_satyp,
                       pmix_bfrops_base_unpack_satyp, pmix_bfrops_base_std_copy,
                       pmix_bfrops_base_print_satyp, &pmix_mca_bfrops_v61_component.types);

    PMIX_REGISTER_TYPE("PMIX_NODE_PID", PMIX_NODE_PID, pmix_bfrops_base_pack_nodepid,
                       pmix_bfrops_base_unpack_nodepid, pmix_bfrops_base_copy_nodepid,
                       pmix_bfrops_base_print_nodepid, &pmix_mca_bfrops_v61_component.types);

    return PMIX_SUCCESS;
}

static void finalize(void)
{
    int n;
    pmix_bfrop_type_info_t *info;

    for (n = 0; n < pmix_mca_bfrops_v61_component.types.size; n++) {
        if (NULL
            != (info = (pmix_bfrop_type_info_t *)
                    pmix_pointer_array_get_item(&pmix_mca_bfrops_v61_component.types, n))) {
            PMIX_RELEASE(info);
            pmix_pointer_array_set_item(&pmix_mca_bfrops_v61_component.types, n, NULL);
        }
    }
}

static pmix_status_t pmix61_pack(pmix_buffer_t *buffer, const void *src, int num_vals,
                                 pmix_data_type_t type)
{
    /* kick the process off by passing this in to the base */
    return pmix_bfrops_base_pack(&pmix_mca_bfrops_v61_component.types, buffer, src, num_vals, type);
}

static pmix_status_t pmix61_unpack(pmix_buffer_t *buffer, void *dest, int32_t *num_vals,
                                   pmix_data_type_t type)
{
    /* kick the process off by passing this in to the base */
    return pmix_bfrops_base_unpack(&pmix_mca_bfrops_v61_component.types, buffer, dest, num_vals, type);
}

static pmix_status_t pmix61_copy(void **dest, void *src, pmix_data_type_t type)
{
    return pmix_bfrops_base_copy(&pmix_mca_bfrops_v61_component.types, dest, src, type);
}

static pmix_status_t pmix61_print(char **output, char *prefix, void *src, pmix_data_type_t type)
{
    return pmix_bfrops_base_print(&pmix_mca_bfrops_v61_component.types, output, prefix, src, type);
}

static const char *data_type_string(pmix_data_type_t type)
{
    return pmix_bfrops_base_data_type_string(&pmix_mca_bfrops_v61_component.types, type);
}
