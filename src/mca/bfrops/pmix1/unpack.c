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
 * Copyright (c) 2012      Los Alamos National Security, Inc.  All rights reserved.
 * Copyright (c) 2014-2017 Intel, Inc.  All rights reserved.
 * Copyright (c) 2015      Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2016      Mellanox Technologies, Inc.
 *                         All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include <src/include/pmix_config.h>

#include <src/include/types.h>

#include "src/util/argv.h"
#include "src/util/error.h"
#include "src/util/output.h"
#include "src/include/pmix_globals.h"
#include "src/mca/bfrops/bfrops_types.h"
#include "src/mca/bfrops/base/base.h"

#include "bfrop_pmix1.h"

pmix_status_t pmix1_unpack(pmix_buffer_t *buffer, void *dest,
                            int32_t *num_vals, pmix_data_type_t type)
{
     /* kick the process off by passing this in to the base */
    return pmix_bfrops_base_unpack(&mca_bfrops_pmix1_component.types,
                                   buffer, dest, num_vals, type);
}

pmix_status_t pmix1_unpack_info(pmix_buffer_t *buffer, void *dest,
                                 int32_t *num_vals, pmix_data_type_t type)
{
    pmix_info_t *ptr;
    int32_t i, n, m;
    pmix_status_t ret;
    char *tmp;

    pmix_output_verbose(20, pmix_globals.debug_output,
                        "pmix_bfrop_unpack: %d info", *num_vals);

    ptr = (pmix_info_t *) dest;
    n = *num_vals;

    for (i = 0; i < n; ++i) {
        memset(ptr[i].key, 0, sizeof(ptr[i].key));
        memset(&ptr[i].value, 0, sizeof(pmix_value_t));
        /* unpack key */
        m=1;
        tmp = NULL;
        if (PMIX_SUCCESS != (ret = pmix_bfrops_base_unpack_string(buffer, &tmp, &m, PMIX_STRING))) {
            PMIX_ERROR_LOG(ret);
            return ret;
        }
        if (NULL == tmp) {
            return PMIX_ERROR;
        }
        (void)strncpy(ptr[i].key, tmp, PMIX_MAX_KEYLEN);
        free(tmp);
        /* we don't have directives */
        /* unpack value - since the value structure is statically-defined
         * instead of a pointer in this struct, we directly unpack it to
         * avoid the malloc */
         if (PMIX_SUCCESS != (ret = pmix_bfrop_get_data_type(buffer, &ptr[i].value.type))) {
            return ret;
        }
        pmix_output_verbose(20, pmix_globals.debug_output,
                            "pmix_bfrop_unpack: info type %d", ptr[i].value.type);
        /* handle info_array's ourselves */
        if (PMIX_INFO_ARRAY == ptr[i].value.type) {
            m=1;
            if (PMIX_SUCCESS != (ret = pmix1_unpack_array(buffer, &ptr[i].value, &m, PMIX_INFO_ARRAY))) {
                return ret;
            }
        } else {
            if (PMIX_SUCCESS != (ret = pmix_bfrops_base_unpack_val(buffer, &ptr[i].value))) {
                return ret;
            }
        }
    }
    return PMIX_SUCCESS;
}

pmix_status_t pmix1_unpack_pdata(pmix_buffer_t *buffer, void *dest,
                                  int32_t *num_vals, pmix_data_type_t type)
{
    pmix_pdata_t *ptr;
    int32_t i, n, m;
    pmix_status_t ret;
    char *tmp;

    pmix_output_verbose(20, pmix_globals.debug_output,
                        "pmix_bfrop_unpack: %d pdata", *num_vals);

    ptr = (pmix_pdata_t *) dest;
    n = *num_vals;

    for (i = 0; i < n; ++i) {
        PMIX_PDATA_CONSTRUCT(&ptr[i]);
        /* unpack the proc */
        m=1;
        if (PMIX_SUCCESS != (ret = pmix_bfrops_base_unpack_proc(buffer, &ptr[i].proc, &m, PMIX_PROC))) {
            return ret;
        }
        /* unpack key */
        m=1;
        tmp = NULL;
        if (PMIX_SUCCESS != (ret = pmix_bfrops_base_unpack_string(buffer, &tmp, &m, PMIX_STRING))) {
            return ret;
        }
        if (NULL == tmp) {
            return PMIX_ERROR;
        }
        (void)strncpy(ptr[i].key, tmp, PMIX_MAX_KEYLEN);
        free(tmp);
        /* unpack value - since the value structure is statically-defined
         * instead of a pointer in this struct, we directly unpack it to
         * avoid the malloc */
         m=1;
         if (PMIX_SUCCESS != (ret = pmix_bfrop_get_data_type(buffer, &ptr[i].value.type))) {
            return ret;
        }
        pmix_output_verbose(20, pmix_globals.debug_output,
                            "pmix_bfrop_unpack: pdata type %d", ptr[i].value.type);
        /* handle info_array's ourselves */
        if (PMIX_INFO_ARRAY == ptr[i].value.type) {
            m=1;
            if (PMIX_SUCCESS != (ret = pmix1_unpack_array(buffer, &ptr[i].value.data.array, &m, PMIX_INFO_ARRAY))) {
                return ret;
            }
        } else {
            if (PMIX_SUCCESS != (ret = pmix_bfrops_base_unpack_val(buffer, &ptr[i].value))) {
                return ret;
            }
        }
    }
    return PMIX_SUCCESS;
}

pmix_status_t pmix1_unpack_app(pmix_buffer_t *buffer, void *dest,
                                int32_t *num_vals, pmix_data_type_t type)
{
    pmix_app_t *ptr;
    int32_t i, k, n, m;
    pmix_status_t ret;
    int32_t nval;
    char *tmp;

    pmix_output_verbose(20, pmix_globals.debug_output,
                        "pmix_bfrop_unpack: %d apps", *num_vals);

    ptr = (pmix_app_t *) dest;
    n = *num_vals;

    for (i = 0; i < n; ++i) {
        /* initialize the fields */
        PMIX_APP_CONSTRUCT(&ptr[i]);
        /* unpack cmd */
        m=1;
        if (PMIX_SUCCESS != (ret = pmix_bfrops_base_unpack_string(buffer, &ptr[i].cmd, &m, PMIX_STRING))) {
            return ret;
        }
        /* unpack argc */
        m=1;
        if (PMIX_SUCCESS != (ret = pmix_bfrops_base_unpack_int(buffer, &ptr[i].argc, &m, PMIX_INT))) {
            return ret;
        }
        /* unpack argv */
        for (k=0; k < ptr[i].argc; k++) {
            m=1;
            tmp = NULL;
            if (PMIX_SUCCESS != (ret = pmix_bfrops_base_unpack_string(buffer, &tmp, &m, PMIX_STRING))) {
                return ret;
            }
            if (NULL == tmp) {
                return PMIX_ERROR;
            }
            pmix_argv_append_nosize(&ptr[i].argv, tmp);
            free(tmp);
        }
        /* unpack env */
        m=1;
        if (PMIX_SUCCESS != (ret = pmix_bfrops_base_unpack_int32(buffer, &nval, &m, PMIX_INT32))) {
            return ret;
        }
        for (k=0; k < nval; k++) {
            m=1;
            tmp = NULL;
            if (PMIX_SUCCESS != (ret = pmix_bfrops_base_unpack_string(buffer, &tmp, &m, PMIX_STRING))) {
                return ret;
            }
            if (NULL == tmp) {
                return PMIX_ERROR;
            }
            pmix_argv_append_nosize(&ptr[i].env, tmp);
            free(tmp);
        }
        /* unpack maxprocs */
        m=1;
        if (PMIX_SUCCESS != (ret = pmix_bfrops_base_unpack_int(buffer, &ptr[i].maxprocs, &m, PMIX_INT))) {
            return ret;
        }
        /* unpack info array */
        m=1;
        if (PMIX_SUCCESS != (ret = pmix_bfrops_base_unpack_sizet(buffer, &ptr[i].ninfo, &m, PMIX_SIZE))) {
            return ret;
        }
        if (0 < ptr[i].ninfo) {
            PMIX_INFO_CREATE(ptr[i].info, ptr[i].ninfo);
            m = ptr[i].ninfo;
            if (PMIX_SUCCESS != (ret = pmix1_unpack_info(buffer, ptr[i].info, &m, PMIX_INFO))) {
                return ret;
            }
        }
    }
    return PMIX_SUCCESS;
}

pmix_status_t pmix1_unpack_kval(pmix_buffer_t *buffer, void *dest,
                                 int32_t *num_vals, pmix_data_type_t type)
{
    pmix_kval_t *ptr;
    int32_t i, n, m;
    pmix_status_t ret;

    pmix_output_verbose(20, pmix_globals.debug_output,
                        "pmix_bfrop_unpack: %d kvals", *num_vals);

    ptr = (pmix_kval_t*) dest;
    n = *num_vals;

    for (i = 0; i < n; ++i) {
        PMIX_CONSTRUCT(&ptr[i], pmix_kval_t);
        /* unpack the key */
        m = 1;
        if (PMIX_SUCCESS != (ret = pmix_bfrops_base_unpack_string(buffer, &ptr[i].key, &m, PMIX_STRING))) {
            PMIX_ERROR_LOG(ret);
            return ret;
        }
        /* allocate the space */
        ptr[i].value = (pmix_value_t*)malloc(sizeof(pmix_value_t));
        /* unpack the type */
        m=1;
        if (PMIX_SUCCESS != (ret = pmix_bfrop_get_data_type(buffer, &ptr[i].value->type))) {
            return ret;
        }
        pmix_output_verbose(20, pmix_globals.debug_output,
                            "pmix_bfrop_unpack: kval type %d", ptr[i].value->type);
        /* handle info_array's ourselves */
        if (PMIX_INFO_ARRAY == ptr[i].value->type) {
            m=1;
            if (PMIX_SUCCESS != (ret = pmix1_unpack_array(buffer, &ptr[i].value->data.array, &m, PMIX_INFO_ARRAY))) {
                return ret;
            }
        } else {
            if (PMIX_SUCCESS != (ret = pmix_bfrops_base_unpack_val(buffer, ptr[i].value))) {
                return ret;
            }
        }
    }
    return PMIX_SUCCESS;
}

pmix_status_t pmix1_unpack_array(pmix_buffer_t *buffer, void *dest,
                                  int32_t *num_vals, pmix_data_type_t type)
{
    pmix_info_array_t *ptr;
    int32_t i, n, m;
    pmix_status_t ret;

    pmix_output_verbose(20, pmix_globals.debug_output,
                        "pmix_bfrop_unpack: %d info arrays", *num_vals);

    ptr = (pmix_info_array_t*) dest;
    n = *num_vals;

    for (i = 0; i < n; ++i) {
        pmix_output_verbose(20, pmix_globals.debug_output,
                            "pmix_bfrop_unpack: init array[%d]", i);
        memset(&ptr[i], 0, sizeof(pmix_info_array_t));
        /* unpack the size of this array */
        m=1;
        if (PMIX_SUCCESS != (ret = pmix_bfrops_base_unpack_sizet(buffer, &ptr[i].size, &m, PMIX_SIZE))) {
            return ret;
        }
        if (0 < ptr[i].size) {
            ptr[i].array = (pmix_info_t*)malloc(ptr[i].size * sizeof(pmix_info_t));
            if (NULL == ptr[i].array) {
                return PMIX_ERR_NOMEM;
            }
            m=ptr[i].size;
            if (PMIX_SUCCESS != (ret = pmix1_unpack_info(buffer, ptr[i].array, &m, PMIX_INFO))) {
                return ret;
            }
        }
    }
    return PMIX_SUCCESS;
}


pmix_status_t pmix1_unpack_persist(pmix_buffer_t *buffer, void *dest,
                                    int32_t *num_vals, pmix_data_type_t type)
{
    return pmix_bfrops_base_unpack(&mca_bfrops_pmix1_component.types,
                                   buffer, dest, num_vals, PMIX_INT);
}

pmix_status_t pmix1_unpack_datatype(pmix_buffer_t *buffer,
                                     void *dest, int32_t *num_vals,
                                     pmix_data_type_t type)
{
    return pmix_bfrops_base_unpack(&mca_bfrops_pmix1_component.types,
                                   buffer, dest, num_vals, PMIX_INT32);
}

pmix_status_t pmix1_unpack_scope(pmix_buffer_t *buffer, void *dest,
                                  int32_t *num_vals, pmix_data_type_t type)
{
    return pmix_bfrops_base_unpack(&mca_bfrops_pmix1_component.types,
                                   buffer, dest, num_vals, PMIX_UINT);
}

pmix_status_t pmix1_unpack_range(pmix_buffer_t *buffer, void *dest,
                                  int32_t *num_vals, pmix_data_type_t type)
{
    return pmix_bfrops_base_unpack(&mca_bfrops_pmix1_component.types,
                                   buffer, dest, num_vals, PMIX_UINT);
}

pmix_status_t pmix1_unpack_cmd(pmix_buffer_t *buffer, void *dest,
                                int32_t *num_vals, pmix_data_type_t type)
{
    return pmix_bfrops_base_unpack(&mca_bfrops_pmix1_component.types,
                                   buffer, dest, num_vals, PMIX_UINT32);
}

pmix_status_t pmix1_unpack_info_directives(pmix_buffer_t *buffer, void *dest,
                                            int32_t *num_vals, pmix_data_type_t type)
{
    return PMIX_SUCCESS;
}
