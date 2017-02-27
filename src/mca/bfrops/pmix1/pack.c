/*
 * Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2006 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2015-2017 Intel, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include <src/include/pmix_config.h>


#include <stdio.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#include "src/class/pmix_pointer_array.h"
#include "src/util/argv.h"
#include "src/util/error.h"
#include "src/util/output.h"
#include "src/include/pmix_globals.h"

#include "src/mca/bfrops/base/base.h"
#include "bfrop_pmix1.h"

pmix_status_t pmix1_pack(pmix_buffer_t *buffer,
                          const void *src, int num_vals,
                          pmix_data_type_t type)
{
    /* kick the process off by passing this in to the base */
    return pmix_bfrops_base_pack(&mca_bfrops_pmix1_component.types,
                                 buffer, src, num_vals, type);
}

pmix_status_t pmix1_pack_value(pmix_buffer_t *buffer, const void *src,
                               int32_t num_vals, pmix_data_type_t type)
{
    pmix_status_t ret;

    switch (p->type) {
        case PMIX_UNDEF:
            break;
        case PMIX_BOOL:
            if (PMIX_SUCCESS != (ret = pmix_bfrops_base_pack_bool(buffer, &p->data.flag, 1, PMIX_BOOL))) {
                return ret;
            }
            break;
        case PMIX_BYTE:
            if (PMIX_SUCCESS != (ret = pmix_bfrops_base_pack_byte(buffer, &p->data.byte, 1, PMIX_BYTE))) {
                return ret;
            }
            break;
        case PMIX_STRING:
            if (PMIX_SUCCESS != (ret = pmix_bfrops_base_pack_string(buffer, &p->data.string, 1, PMIX_STRING))) {
                return ret;
            }
            break;
        case PMIX_SIZE:
            if (PMIX_SUCCESS != (ret = pmix_bfrops_base_pack_sizet(buffer, &p->data.size, 1, PMIX_SIZE))) {
                return ret;
            }
            break;
        case PMIX_PID:
            if (PMIX_SUCCESS != (ret = pmix_bfrops_base_pack_pid(buffer, &p->data.pid, 1, PMIX_PID))) {
                return ret;
            }
            break;
        case PMIX_INT:
            if (PMIX_SUCCESS != (ret = pmix_bfrops_base_pack_int(buffer, &p->data.integer, 1, PMIX_INT))) {
                return ret;
            }
            break;
        case PMIX_INT8:
            if (PMIX_SUCCESS != (ret = pmix_bfrops_base_pack_byte(buffer, &p->data.int8, 1, PMIX_INT8))) {
                return ret;
            }
            break;
        case PMIX_INT16:
            if (PMIX_SUCCESS != (ret = pmix_bfrops_base_pack_int16(buffer, &p->data.int16, 1, PMIX_INT16))) {
                return ret;
            }
            break;
        case PMIX_INT32:
            if (PMIX_SUCCESS != (ret = pmix_bfrops_base_pack_int32(buffer, &p->data.int32, 1, PMIX_INT32))) {
                return ret;
            }
            break;
        case PMIX_INT64:
            if (PMIX_SUCCESS != (ret = pmix_bfrops_base_pack_int64(buffer, &p->data.int64, 1, PMIX_INT64))) {
                return ret;
            }
            break;
        case PMIX_UINT:
            if (PMIX_SUCCESS != (ret = pmix_bfrops_base_pack_int(buffer, &p->data.uint, 1, PMIX_UINT))) {
                return ret;
            }
            break;
        case PMIX_UINT8:
            if (PMIX_SUCCESS != (ret = pmix_bfrops_base_pack_byte(buffer, &p->data.uint8, 1, PMIX_UINT8))) {
                return ret;
            }
            break;
        case PMIX_UINT16:
            if (PMIX_SUCCESS != (ret = pmix_bfrops_base_pack_int16(buffer, &p->data.uint16, 1, PMIX_UINT16))) {
                return ret;
            }
            break;
        case PMIX_UINT32:
            if (PMIX_SUCCESS != (ret = pmix_bfrops_base_pack_int32(buffer, &p->data.uint32, 1, PMIX_UINT32))) {
                return ret;
            }
            break;
        case PMIX_UINT64:
            if (PMIX_SUCCESS != (ret = pmix_bfrops_base_pack_int64(buffer, &p->data.uint64, 1, PMIX_UINT64))) {
                return ret;
            }
            break;
        case PMIX_FLOAT:
            if (PMIX_SUCCESS != (ret = pmix_bfrops_base_pack_float(buffer, &p->data.fval, 1, PMIX_FLOAT))) {
                return ret;
            }
            break;
        case PMIX_DOUBLE:
            if (PMIX_SUCCESS != (ret = pmix_bfrops_base_pack_double(buffer, &p->data.dval, 1, PMIX_DOUBLE))) {
                return ret;
            }
            break;
        case PMIX_TIMEVAL:
            if (PMIX_SUCCESS != (ret = pmix_bfrops_base_pack_timeval(buffer, &p->data.tv, 1, PMIX_TIMEVAL))) {
                return ret;
            }
            break;
        case PMIX_TIME:
            if (PMIX_SUCCESS != (ret = pmix_bfrops_base_pack_time(buffer, &p->data.time, 1, PMIX_TIME))) {
                return ret;
            }
            break;
        case PMIX_STATUS:
            if (PMIX_SUCCESS != (ret = pmix_bfrops_base_pack_status(buffer, &p->data.status, 1, PMIX_STATUS))) {
                return ret;
            }
            break;
        case PMIX_PROC:
            if (PMIX_SUCCESS != (ret = pmix_bfrops_base_pack_proc(buffer, p->data.proc, 1, PMIX_PROC))) {
                return ret;
            }
            break;
        case PMIX_PROC_RANK:
            if (PMIX_SUCCESS != (ret = pmix_bfrops_base_pack_rank(buffer, &p->data.rank, 1, PMIX_PROC_RANK))) {
                return ret;
            }
            break;
        case PMIX_BYTE_OBJECT:
            if (PMIX_SUCCESS != (ret = pmix_bfrops_base_pack_bo(buffer, &p->data.bo, 1, PMIX_BYTE_OBJECT))) {
                return ret;
            }
            break;
        case PMIX_PERSIST:
            if (PMIX_SUCCESS != (ret = pmix_bfrops_base_pack_persist(buffer, &p->data.persist, 1, PMIX_PERSIST))) {
                return ret;
            }
            break;
        case PMIX_SCOPE:
            if (PMIX_SUCCESS != (ret = pmix_bfrops_base_pack_scope(buffer, &p->data.scope, 1, PMIX_SCOPE))) {
                return ret;
            }
            break;
        case PMIX_DATA_RANGE:
            if (PMIX_SUCCESS != (ret = pmix_bfrops_base_pack_range(buffer, &p->data.range, 1, PMIX_DATA_RANGE))) {
                return ret;
            }
            break;
        case PMIX_PROC_STATE:
            if (PMIX_SUCCESS != (ret = pmix_bfrops_base_pack_pstate(buffer, &p->data.state, 1, PMIX_PROC_STATE))) {
                return ret;
            }
            break;
        case PMIX_PROC_INFO:
            if (PMIX_SUCCESS != (ret = pmix_bfrops_base_pack_pinfo(buffer, p->data.pinfo, 1, PMIX_PROC_INFO))) {
                return ret;
            }
            break;
        case PMIX_DATA_ARRAY:
            if (PMIX_SUCCESS != (ret = pmix_bfrops_base_pack_darray(buffer, p->data.darray, 1, PMIX_DATA_ARRAY))) {
                return ret;
            }
            break;
        case PMIX_QUERY:
            if (PMIX_SUCCESS != (ret = pmix_bfrops_base_pack_query(buffer, p->data.darray, 1, PMIX_QUERY))) {
                return ret;
            }
            break;
        /**** DEPRECATED ****/
        case PMIX_INFO_ARRAY:
            if (PMIX_SUCCESS != (ret = pmix_bfrops_base_pack_array(buffer, p->data.array, 1, PMIX_INFO_ARRAY))) {
                return ret;
            }
            break;
        /********************/
        default:
        pmix_output(0, "PACK-PMIX-VALUE: UNSUPPORTED TYPE %d", (int)p->type);
        return PMIX_ERROR;
    }
    return PMIX_SUCCESS;
}

pmix_status_t pmix1_pack_info(pmix_buffer_t *buffer, const void *src,
                               int32_t num_vals, pmix_data_type_t type)
{
    pmix_info_t *info;
    int32_t i;
    int ret;
    char *foo;

    info = (pmix_info_t *) src;

    for (i = 0; i < num_vals; ++i) {
        /* pack key */
        foo = info[i].key;
        if (PMIX_SUCCESS != (ret = pmix_bfrops_base_pack_string(buffer, &foo, 1, PMIX_STRING))) {
            return ret;
        }
        /* the 1.1 series does not have any directives */
        /* pack the type */
        if (PMIX_SUCCESS != (ret = pmix_bfrop_store_data_type(buffer, info[i].value.type))) {
            return ret;
        }
        /* handle it ourselves if the type is info_array */
        if (PMIX_INFO_ARRAY == info[i].value.type) {
            if (PMIX_SUCCESS != (ret = pmix1_pack_array(buffer, info[i].value.data.array->array,
                                                         info[i].value.data.array->size, PMIX_INFO_ARRAY))) {
                return ret;
            }
        } else {
            if (PMIX_SUCCESS != (ret = pmix_bfrops_base_pack_val(buffer, &info[i].value))) {
                return ret;
            }
        }
    }
    return PMIX_SUCCESS;
}

pmix_status_t pmix1_pack_pdata(pmix_buffer_t *buffer, const void *src,
                                int32_t num_vals, pmix_data_type_t type)
{
    pmix_pdata_t *pdata;
    int32_t i;
    int ret;
    char *foo;

    pdata = (pmix_pdata_t *) src;

    for (i = 0; i < num_vals; ++i) {
        /* pack the proc */
        if (PMIX_SUCCESS != (ret = pmix_bfrops_base_pack_proc(buffer, &pdata[i].proc, 1, PMIX_PROC))) {
            return ret;
        }
        /* pack key */
        foo = pdata[i].key;
        if (PMIX_SUCCESS != (ret = pmix_bfrops_base_pack_string(buffer, &foo, 1, PMIX_STRING))) {
            return ret;
        }
        /* pack the type */
        if (PMIX_SUCCESS != (ret = pmix_bfrop_store_data_type(buffer, pdata[i].value.type))) {
            return ret;
        }
        /* handle it ourselves if the type is info_array */
        if (PMIX_INFO_ARRAY == pdata[i].value.type) {
            if (PMIX_SUCCESS != (ret = pmix1_pack_array(buffer, pdata[i].value.data.array->array,
                                                         pdata[i].value.data.array->size, PMIX_INFO_ARRAY))) {
                return ret;
            }
        } else {
            if (PMIX_SUCCESS != (ret = pmix_bfrops_base_pack_val(buffer, &pdata[i].value))) {
                return ret;
            }
        }
    }
    return PMIX_SUCCESS;
}

pmix_status_t pmix1_pack_app(pmix_buffer_t *buffer, const void *src,
                              int32_t num_vals, pmix_data_type_t type)
{
    pmix_app_t *app;
    int32_t i, j, nvals;
    int ret;

    app = (pmix_app_t *) src;

    for (i = 0; i < num_vals; ++i) {
        if (PMIX_SUCCESS != (ret = pmix_bfrops_base_pack_string(buffer, &app[i].cmd, 1, PMIX_STRING))) {
            return ret;
        }
        /* argv */
        if (PMIX_SUCCESS != (ret = pmix_bfrops_base_pack_int(buffer, &app[i].argc, 1, PMIX_INT))) {
            return ret;
        }
        for (j=0; j < app[i].argc; j++) {
            if (PMIX_SUCCESS != (ret = pmix_bfrops_base_pack_string(buffer, &app[i].argv[j], 1, PMIX_STRING))) {
                return ret;
            }
        }
        /* env */
        nvals = pmix_argv_count(app[i].env);
        if (PMIX_SUCCESS != (ret = pmix_bfrops_base_pack_int32(buffer, &nvals, 1, PMIX_INT32))) {
            return ret;
        }
        for (j=0; j < nvals; j++) {
            if (PMIX_SUCCESS != (ret = pmix_bfrops_base_pack_string(buffer, &app[i].env[j], 1, PMIX_STRING))) {
                return ret;
            }
        }
        /* maxprocs */
        if (PMIX_SUCCESS != (ret = pmix_bfrops_base_pack_int(buffer, &app[i].maxprocs, 1, PMIX_INT))) {
            return ret;
        }
        /* info array */
        if (PMIX_SUCCESS != (ret = pmix_bfrops_base_pack_sizet(buffer, &app[i].ninfo, 1, PMIX_SIZE))) {
            return ret;
        }
        if (0 < app[i].ninfo) {
            if (PMIX_SUCCESS != (ret = pmix1_pack_info(buffer, app[i].info, app[i].ninfo, PMIX_INFO))) {
                return ret;
            }
        }
    }
    return PMIX_SUCCESS;
}


pmix_status_t pmix1_pack_kval(pmix_buffer_t *buffer, const void *src,
                               int32_t num_vals, pmix_data_type_t type)
{
    pmix_kval_t *ptr;
    int32_t i;
    int ret;

    ptr = (pmix_kval_t *) src;

    for (i = 0; i < num_vals; ++i) {
        /* pack the key */
        if (PMIX_SUCCESS != (ret = pmix_bfrops_base_pack_string(buffer, &ptr[i].key, 1, PMIX_STRING))) {
            return ret;
        }
        /* pack the type */
        if (PMIX_SUCCESS != (ret = pmix_bfrop_store_data_type(buffer, ptr[i].value->type))) {
            return ret;
        }
        /* handle it ourselves if the type is info_array */
        if (PMIX_INFO_ARRAY == ptr[i].value->type) {
            if (PMIX_SUCCESS != (ret = pmix1_pack_array(buffer, ptr[i].value->data.array->array,
                                                         ptr[i].value->data.array->size, PMIX_INFO_ARRAY))) {
                return ret;
            }
        } else {
            if (PMIX_SUCCESS != (ret = pmix_bfrops_base_pack_val(buffer, ptr[i].value))) {
                return ret;
            }
        }
    }

    return PMIX_SUCCESS;
}

pmix_status_t pmix1_pack_array(pmix_buffer_t *buffer, const void *src,
                                int32_t num_vals, pmix_data_type_t type)
{
    pmix_info_array_t *ptr;
    int32_t i;
    int ret;

    ptr = (pmix_info_array_t *) src;

    for (i = 0; i < num_vals; ++i) {
        /* pack the size */
        if (PMIX_SUCCESS != (ret = pmix_bfrops_base_pack_sizet(buffer, &ptr[i].size, 1, PMIX_SIZE))) {
            return ret;
        }
        if (0 < ptr[i].size) {
            /* pack the values */
            if (PMIX_SUCCESS != (ret = pmix1_pack_info(buffer, ptr[i].array, ptr[i].size, PMIX_INFO))) {
                return ret;
            }
        }
    }

    return PMIX_SUCCESS;
}

pmix_status_t pmix1_pack_persist(pmix_buffer_t *buffer, const void *src,
                                  int32_t num_vals, pmix_data_type_t type)
{
    return pmix_bfrops_base_pack(&mca_bfrops_pmix1_component.types,
                                 buffer, src, num_vals, PMIX_INT);
}

pmix_status_t pmix1_pack_datatype(pmix_buffer_t *buffer, const void *src,
                                   int32_t num_vals, pmix_data_type_t type)
{
    return pmix_bfrops_base_pack(&mca_bfrops_pmix1_component.types,
                                 buffer, src, num_vals, PMIX_INT32);
}

pmix_status_t pmix1_pack_scope(pmix_buffer_t *buffer, const void *src,
                                int32_t num_vals, pmix_data_type_t type)
{
    return pmix_bfrops_base_pack(&mca_bfrops_pmix1_component.types,
                                 buffer, src, num_vals, PMIX_UINT);
}

pmix_status_t pmix1_pack_range(pmix_buffer_t *buffer, const void *src,
                                int32_t num_vals, pmix_data_type_t type)
{
    return pmix_bfrops_base_pack(&mca_bfrops_pmix1_component.types,
                                 buffer, src, num_vals, PMIX_UINT);
}

pmix_status_t pmix1_pack_cmd(pmix_buffer_t *buffer, const void *src,
                              int32_t num_vals, pmix_data_type_t type)
{
    return pmix_bfrops_base_pack(&mca_bfrops_pmix1_component.types,
                                 buffer, src, num_vals, PMIX_UINT32);
}

pmix_status_t pmix1_pack_info_directives(pmix_buffer_t *buffer, const void *src,
                                          int32_t num_vals, pmix_data_type_t type)
{
    return PMIX_SUCCESS;
}

pmix_status_t pmix1_pack_darray(pmix_buffer_t *buffer, const void *src,
                               int32_t num_vals, pmix_data_type_t type)
{
    pmix_data_array_t *p = (pmix_data_array_t*)src;

    /* the only type we support in v1.1 is pmix_info_t */
    if (PMIX_INFO != p->type) {
        return PMIX_ERR_NOT_SUPPORTED;
    }

}
pmix_status_t pmix1_pack_rank(pmix_buffer_t *buffer, const void *src,
                               int32_t num_vals, pmix_data_type_t type)
{
    /* the rank in v1.1 is just an int32 */
    return pmix_bfrops_base_pack(&mca_bfrops_pmix1_component.types,
                                 buffer, src, num_vals, PMIX_INT32);
}

pmix_status_t pmix1_pack_unsupported(pmix_buffer_t *buffer, const void *src,
                                      int32_t num_vals, pmix_data_type_t type)
{
    return PMIX_ERR_NOT_SUPPORTED;
}
