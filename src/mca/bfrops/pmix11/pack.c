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
 * Copyright (c) 2015-2016 Intel, Inc. All rights reserved.
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
#include "bfrop_pmix11.h"

pmix_status_t pmix11_pack(pmix_buffer_t *buffer,
                          const void *src, int num_vals,
                          pmix_data_type_t type)
{
    /* kick the process off by passing this in to the base */
    return pmix_bfrops_base_pack(&mca_bfrops_pmix11_component.types,
                                 buffer, src, num_vals, type);
}

pmix_status_t pmix11_pack_info(pmix_buffer_t *buffer, const void *src,
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
        /* handle if ourselves if the type is info_array */
        if (PMIX_INFO_ARRAY == info[i].value.type) {
            if (PMIX_SUCCESS != (ret = pmix11_pack_array(buffer, info[i].value.data.array.array,
                                                         info[i].value.data.array.size, PMIX_INFO_ARRAY))) {
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

pmix_status_t pmix11_pack_pdata(pmix_buffer_t *buffer, const void *src,
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
        /* handle if ourselves if the type is info_array */
        if (PMIX_INFO_ARRAY == pdata[i].value.type) {
            if (PMIX_SUCCESS != (ret = pmix11_pack_array(buffer, pdata[i].value.data.array.array,
                                                         pdata[i].value.data.array.size, PMIX_INFO_ARRAY))) {
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

pmix_status_t pmix11_pack_app(pmix_buffer_t *buffer, const void *src,
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
            if (PMIX_SUCCESS != (ret = pmix11_pack_info(buffer, app[i].info, app[i].ninfo, PMIX_INFO))) {
                return ret;
            }
        }
    }
    return PMIX_SUCCESS;
}


pmix_status_t pmix11_pack_kval(pmix_buffer_t *buffer, const void *src,
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
            if (PMIX_SUCCESS != (ret = pmix11_pack_array(buffer, ptr[i].value->data.array.array,
                                                         ptr[i].value->data.array.size, PMIX_INFO_ARRAY))) {
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

pmix_status_t pmix11_pack_array(pmix_buffer_t *buffer, const void *src,
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
            if (PMIX_SUCCESS != (ret = pmix11_pack_info(buffer, ptr[i].array, ptr[i].size, PMIX_INFO))) {
                return ret;
            }
        }
    }

    return PMIX_SUCCESS;
}

pmix_status_t pmix11_pack_persist(pmix_buffer_t *buffer, const void *src,
                                  int32_t num_vals, pmix_data_type_t type)
{
    return pmix_bfrops_base_pack(&mca_bfrops_pmix11_component.types,
                                 buffer, src, num_vals, PMIX_INT);
}

pmix_status_t pmix11_pack_datatype(pmix_buffer_t *buffer, const void *src,
                                   int32_t num_vals, pmix_data_type_t type)
{
    return pmix_bfrops_base_pack(&mca_bfrops_pmix11_component.types,
                                 buffer, src, num_vals, PMIX_INT32);
}

pmix_status_t pmix11_pack_scope(pmix_buffer_t *buffer, const void *src,
                                int32_t num_vals, pmix_data_type_t type)
{
    return pmix_bfrops_base_pack(&mca_bfrops_pmix11_component.types,
                                 buffer, src, num_vals, PMIX_UINT);
}

pmix_status_t pmix11_pack_range(pmix_buffer_t *buffer, const void *src,
                                int32_t num_vals, pmix_data_type_t type)
{
    return pmix_bfrops_base_pack(&mca_bfrops_pmix11_component.types,
                                 buffer, src, num_vals, PMIX_UINT);
}

pmix_status_t pmix11_pack_cmd(pmix_buffer_t *buffer, const void *src,
                              int32_t num_vals, pmix_data_type_t type)
{
    return pmix_bfrops_base_pack(&mca_bfrops_pmix11_component.types,
                                 buffer, src, num_vals, PMIX_UINT32);
}

pmix_status_t pmix11_pack_info_directives(pmix_buffer_t *buffer, const void *src,
                                          int32_t num_vals, pmix_data_type_t type)
{
    return PMIX_SUCCESS;
}
