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
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2016      Mellanox Technologies, Inc.
 *                         All rights reserved.
 * Copyright (c) 2022      Triad National Security, LLC. All rights reserved.
 * Copyright (c) 2021-2022 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "src/include/pmix_config.h"

#include "src/include/pmix_stdint.h"

#include <stdio.h>
#ifdef HAVE_TIME_H
#    include <time.h>
#endif

#include "src/include/pmix_globals.h"
#include "src/hwloc/pmix_hwloc.h"
#include "src/mca/bfrops/base/base.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_error.h"
#include "src/util/pmix_name_fns.h"
#include "src/util/pmix_printf.h"

char* PMIx_Info_string(const pmix_info_t *info)
{
    pmix_status_t rc;
    char *output = NULL;

    PMIX_BFROPS_PRINT(rc, pmix_globals.mypeer,
                      &output, NULL,
                      (void*)info, PMIX_INFO);
    if (PMIX_SUCCESS != rc) {
        return NULL;
    }
    return output;
}

char* PMIx_Value_string(const pmix_value_t *value)
{
    pmix_status_t rc;
    char *output = NULL;

    PMIX_BFROPS_PRINT(rc, pmix_globals.mypeer,
                      &output, NULL,
                      (void*)value, PMIX_VALUE);
    if (PMIX_SUCCESS != rc) {
        return NULL;
    }
    return output;
}

pmix_status_t pmix_bfrops_base_print(pmix_pointer_array_t *regtypes, char **output, char *prefix,
                                     void *src, pmix_data_type_t type)
{
    pmix_bfrop_type_info_t *info;

    /* check for error */
    if (NULL == output || NULL == src) {
        return PMIX_ERR_BAD_PARAM;
    }

    /* Lookup the print function for this type and call it */

    if (NULL == (info = (pmix_bfrop_type_info_t *) pmix_pointer_array_get_item(regtypes, type))) {
        return PMIX_ERR_UNKNOWN_DATA_TYPE;
    }

    return info->odti_print_fn(output, prefix, src, type);
}

/*
 * STANDARD PRINT FUNCTIONS FOR SYSTEM TYPES
 */
int pmix_bfrops_base_print_bool(char **output, char *prefix, bool *src, pmix_data_type_t type)
{
    char *prefx;
    int ret;

    if (PMIX_BOOL != type) {
        return PMIX_ERR_BAD_PARAM;
    }
    /* deal with NULL prefix */
    if (NULL == prefix) {
        if (0 > asprintf(&prefx, " ")) {
            return PMIX_ERR_NOMEM;
        }
    } else {
        prefx = prefix;
    }

    /* if src is NULL, just print data type and return */
    if (NULL == src) {
        ret = asprintf(output, "%sData type: PMIX_BOOL\tValue: NULL pointer", prefx);
        if (prefx != prefix) {
            free(prefx);
        }
        if (0 > ret) {
            return PMIX_ERR_OUT_OF_RESOURCE;
        } else {
            return PMIX_SUCCESS;
        }
    }

    ret = asprintf(output, "%sData type: PMIX_BOOL\tValue: %s", prefix, (*src) ? "TRUE" : "FALSE");
    if (prefx != prefix) {
        free(prefx);
    }

    if (0 > ret) {
        return PMIX_ERR_OUT_OF_RESOURCE;
    } else {
        return PMIX_SUCCESS;
    }
}

int pmix_bfrops_base_print_byte(char **output, char *prefix, uint8_t *src, pmix_data_type_t type)
{
    char *prefx;
    int ret;

    PMIX_HIDE_UNUSED_PARAMS(type);

    /* deal with NULL prefix */
    if (NULL == prefix) {
        if (0 > asprintf(&prefx, " ")) {
            return PMIX_ERR_NOMEM;
        }
    } else {
        prefx = prefix;
    }

    /* if src is NULL, just print data type and return */
    if (NULL == src) {
        ret = asprintf(output, "%sData type: PMIX_BYTE\tValue: NULL pointer", prefx);
        if (prefx != prefix) {
            free(prefx);
        }
        if (0 > ret) {
            return PMIX_ERR_OUT_OF_RESOURCE;
        } else {
            return PMIX_SUCCESS;
        }
    }

    ret = asprintf(output, "%sData type: PMIX_BYTE\tValue: %x", prefix, *src);
    if (prefx != prefix) {
        free(prefx);
    }

    if (0 > ret) {
        return PMIX_ERR_OUT_OF_RESOURCE;
    } else {
        return PMIX_SUCCESS;
    }
}

int pmix_bfrops_base_print_string(char **output, char *prefix, char *src, pmix_data_type_t type)
{
    char *prefx;
    int ret;

    PMIX_HIDE_UNUSED_PARAMS(type);

    /* deal with NULL prefix */
    if (NULL == prefix) {
        if (0 > asprintf(&prefx, " ")) {
            return PMIX_ERR_NOMEM;
        }
    } else {
        prefx = prefix;
    }

    /* if src is NULL, just print data type and return */
    if (NULL == src) {
        ret = asprintf(output, "%sData type: PMIX_STRING\tValue: NULL pointer", prefx);
        if (prefx != prefix) {
            free(prefx);
        }
        if (0 > ret) {
            return PMIX_ERR_OUT_OF_RESOURCE;
        } else {
            return PMIX_SUCCESS;
        }
    }

    ret = asprintf(output, "%sData type: PMIX_STRING\tValue: %s", prefx, src);
    if (prefx != prefix) {
        free(prefx);
    }

    if (0 > ret) {
        return PMIX_ERR_OUT_OF_RESOURCE;
    } else {
        return PMIX_SUCCESS;
    }
}

int pmix_bfrops_base_print_size(char **output, char *prefix, size_t *src, pmix_data_type_t type)
{
    char *prefx;
    int ret;

    PMIX_HIDE_UNUSED_PARAMS(type);

    /* deal with NULL prefix */
    if (NULL == prefix) {
        if (0 > asprintf(&prefx, " ")) {
            return PMIX_ERR_NOMEM;
        }
    } else {
        prefx = prefix;
    }

    /* if src is NULL, just print data type and return */
    if (NULL == src) {
        ret = asprintf(output, "%sData type: PMIX_SIZE\tValue: NULL pointer", prefx);
        if (prefx != prefix) {
            free(prefx);
        }
        if (0 > ret) {
            return PMIX_ERR_OUT_OF_RESOURCE;
        } else {
            return PMIX_SUCCESS;
        }
    }

    ret = asprintf(output, "%sData type: PMIX_SIZE\tValue: %lu", prefx, (unsigned long) *src);
    if (prefx != prefix) {
        free(prefx);
    }

    if (0 > ret) {
        return PMIX_ERR_OUT_OF_RESOURCE;
    } else {
        return PMIX_SUCCESS;
    }
}

int pmix_bfrops_base_print_pid(char **output, char *prefix, pid_t *src, pmix_data_type_t type)
{
    char *prefx;
    int ret;

    PMIX_HIDE_UNUSED_PARAMS(type);

    /* deal with NULL prefix */
    if (NULL == prefix) {
        if (0 > asprintf(&prefx, " ")) {
            return PMIX_ERR_NOMEM;
        }
    } else {
        prefx = prefix;
    }

    /* if src is NULL, just print data type and return */
    if (NULL == src) {
        ret = asprintf(output, "%sData type: PMIX_PID\tValue: NULL pointer", prefx);
        if (prefx != prefix) {
            free(prefx);
        }
        if (0 > ret) {
            return PMIX_ERR_OUT_OF_RESOURCE;
        } else {
            return PMIX_SUCCESS;
        }
    }

    ret = asprintf(output, "%sData type: PMIX_PID\tValue: %lu", prefx, (unsigned long) *src);
    if (prefx != prefix) {
        free(prefx);
    }
    if (0 > ret) {
        return PMIX_ERR_OUT_OF_RESOURCE;
    } else {
        return PMIX_SUCCESS;
    }
}

int pmix_bfrops_base_print_int(char **output, char *prefix, int *src, pmix_data_type_t type)
{
    char *prefx;
    int ret;

    PMIX_HIDE_UNUSED_PARAMS(type);

    /* deal with NULL prefix */
    if (NULL == prefix) {
        if (0 > asprintf(&prefx, " ")) {
            return PMIX_ERR_NOMEM;
        }
    } else {
        prefx = prefix;
    }

    /* if src is NULL, just print data type and return */
    if (NULL == src) {
        ret = asprintf(output, "%sData type: PMIX_INT\tValue: NULL pointer", prefx);
        if (prefx != prefix) {
            free(prefx);
        }
        if (0 > ret) {
            return PMIX_ERR_OUT_OF_RESOURCE;
        } else {
            return PMIX_SUCCESS;
        }
    }

    ret = asprintf(output, "%sData type: PMIX_INT\tValue: %ld", prefx, (long) *src);
    if (prefx != prefix) {
        free(prefx);
    }

    if (0 > ret) {
        return PMIX_ERR_OUT_OF_RESOURCE;
    } else {
        return PMIX_SUCCESS;
    }
}

int pmix_bfrops_base_print_uint(char **output, char *prefix, uint *src, pmix_data_type_t type)
{
    char *prefx;
    int ret;

    PMIX_HIDE_UNUSED_PARAMS(type);

    /* deal with NULL prefix */
    if (NULL == prefix) {
        if (0 > asprintf(&prefx, " ")) {
            return PMIX_ERR_NOMEM;
        }
    } else {
        prefx = prefix;
    }

    /* if src is NULL, just print data type and return */
    if (NULL == src) {
        ret = asprintf(output, "%sData type: PMIX_UINT\tValue: NULL pointer", prefx);
        if (prefx != prefix) {
            free(prefx);
        }
        if (0 > ret) {
            return PMIX_ERR_OUT_OF_RESOURCE;
        } else {
            return PMIX_SUCCESS;
        }
    }

    ret = asprintf(output, "%sData type: PMIX_UINT\tValue: %lu", prefx, (unsigned long) *src);
    if (prefx != prefix) {
        free(prefx);
    }

    if (0 > ret) {
        return PMIX_ERR_OUT_OF_RESOURCE;
    } else {
        return PMIX_SUCCESS;
    }
}

int pmix_bfrops_base_print_uint8(char **output, char *prefix, uint8_t *src, pmix_data_type_t type)
{
    char *prefx;
    int ret;

    PMIX_HIDE_UNUSED_PARAMS(type);

    /* deal with NULL prefix */
    if (NULL == prefix) {
        if (0 > asprintf(&prefx, " ")) {
            return PMIX_ERR_NOMEM;
        }
    } else {
        prefx = prefix;
    }

    /* if src is NULL, just print data type and return */
    if (NULL == src) {
        ret = asprintf(output, "%sData type: PMIX_UINT8\tValue: NULL pointer", prefx);
        if (prefx != prefix) {
            free(prefx);
        }
        if (0 > ret) {
            return PMIX_ERR_OUT_OF_RESOURCE;
        } else {
            return PMIX_SUCCESS;
        }
    }

    ret = asprintf(output, "%sData type: PMIX_UINT8\tValue: %u", prefx, (unsigned int) *src);
    if (prefx != prefix) {
        free(prefx);
    }

    if (0 > ret) {
        return PMIX_ERR_OUT_OF_RESOURCE;
    } else {
        return PMIX_SUCCESS;
    }
}

int pmix_bfrops_base_print_uint16(char **output, char *prefix, uint16_t *src, pmix_data_type_t type)
{
    char *prefx;
    int ret;

    PMIX_HIDE_UNUSED_PARAMS(type);

    /* deal with NULL prefix */
    if (NULL == prefix) {
        if (0 > asprintf(&prefx, " ")) {
            return PMIX_ERR_NOMEM;
        }
    } else {
        prefx = prefix;
    }

    /* if src is NULL, just print data type and return */
    if (NULL == src) {
        ret = asprintf(output, "%sData type: PMIX_UINT16\tValue: NULL pointer", prefx);
        if (prefx != prefix) {
            free(prefx);
        }
        if (0 > ret) {
            return PMIX_ERR_OUT_OF_RESOURCE;
        } else {
            return PMIX_SUCCESS;
        }
    }

    ret = asprintf(output, "%sData type: PMIX_UINT16\tValue: %u", prefx, (unsigned int) *src);
    if (prefx != prefix) {
        free(prefx);
    }

    if (0 > ret) {
        return PMIX_ERR_OUT_OF_RESOURCE;
    } else {
        return PMIX_SUCCESS;
    }
}

int pmix_bfrops_base_print_uint32(char **output, char *prefix, uint32_t *src, pmix_data_type_t type)
{
    char *prefx;
    int ret;

    PMIX_HIDE_UNUSED_PARAMS(type);

    /* deal with NULL prefix */
    if (NULL == prefix) {
        if (0 > asprintf(&prefx, " ")) {
            return PMIX_ERR_NOMEM;
        }
    } else {
        prefx = prefix;
    }

    /* if src is NULL, just print data type and return */
    if (NULL == src) {
        ret = asprintf(output, "%sData type: PMIX_UINT32\tValue: NULL pointer", prefx);
        if (prefx != prefix) {
            free(prefx);
        }
        if (0 > ret) {
            return PMIX_ERR_OUT_OF_RESOURCE;
        } else {
            return PMIX_SUCCESS;
        }
    }

    ret = asprintf(output, "%sData type: PMIX_UINT32\tValue: %u", prefx, (unsigned int) *src);
    if (prefx != prefix) {
        free(prefx);
    }

    if (0 > ret) {
        return PMIX_ERR_OUT_OF_RESOURCE;
    } else {
        return PMIX_SUCCESS;
    }
}

int pmix_bfrops_base_print_int8(char **output, char *prefix, int8_t *src, pmix_data_type_t type)
{
    char *prefx;
    int ret;

    PMIX_HIDE_UNUSED_PARAMS(type);

    /* deal with NULL prefix */
    if (NULL == prefix) {
        if (0 > asprintf(&prefx, " ")) {
            return PMIX_ERR_NOMEM;
        }
    } else {
        prefx = prefix;
    }

    /* if src is NULL, just print data type and return */
    if (NULL == src) {
        ret = asprintf(output, "%sData type: PMIX_INT8\tValue: NULL pointer", prefx);
        if (prefx != prefix) {
            free(prefx);
        }
        if (0 > ret) {
            return PMIX_ERR_OUT_OF_RESOURCE;
        } else {
            return PMIX_SUCCESS;
        }
    }

    ret = asprintf(output, "%sData type: PMIX_INT8\tValue: %d", prefx, (int) *src);
    if (prefx != prefix) {
        free(prefx);
    }

    if (0 > ret) {
        return PMIX_ERR_OUT_OF_RESOURCE;
    } else {
        return PMIX_SUCCESS;
    }
}

int pmix_bfrops_base_print_int16(char **output, char *prefix, int16_t *src, pmix_data_type_t type)
{
    char *prefx;
    int ret;

    PMIX_HIDE_UNUSED_PARAMS(type);

    /* deal with NULL prefix */
    if (NULL == prefix) {
        if (0 > asprintf(&prefx, " ")) {
            return PMIX_ERR_NOMEM;
        }
    } else {
        prefx = prefix;
    }

    /* if src is NULL, just print data type and return */
    if (NULL == src) {
        ret = asprintf(output, "%sData type: PMIX_INT16\tValue: NULL pointer", prefx);
        if (prefx != prefix) {
            free(prefx);
        }
        if (0 > ret) {
            return PMIX_ERR_OUT_OF_RESOURCE;
        } else {
            return PMIX_SUCCESS;
        }
    }

    ret = asprintf(output, "%sData type: PMIX_INT16\tValue: %d", prefx, (int) *src);
    if (prefx != prefix) {
        free(prefx);
    }

    if (0 > ret) {
        return PMIX_ERR_OUT_OF_RESOURCE;
    } else {
        return PMIX_SUCCESS;
    }
}

int pmix_bfrops_base_print_int32(char **output, char *prefix, int32_t *src, pmix_data_type_t type)
{
    char *prefx;
    int ret;

    PMIX_HIDE_UNUSED_PARAMS(type);

    /* deal with NULL prefix */
    if (NULL == prefix) {
        if (0 > asprintf(&prefx, " ")) {
            return PMIX_ERR_NOMEM;
        }
    } else {
        prefx = prefix;
    }

    /* if src is NULL, just print data type and return */
    if (NULL == src) {
        ret = asprintf(output, "%sData type: PMIX_INT32\tValue: NULL pointer", prefx);
        if (prefx != prefix) {
            free(prefx);
        }
        if (0 > ret) {
            return PMIX_ERR_OUT_OF_RESOURCE;
        } else {
            return PMIX_SUCCESS;
        }
    }

    ret = asprintf(output, "%sData type: PMIX_INT32\tValue: %d", prefx, (int) *src);
    if (prefx != prefix) {
        free(prefx);
    }

    if (0 > ret) {
        return PMIX_ERR_OUT_OF_RESOURCE;
    } else {
        return PMIX_SUCCESS;
    }
}
int pmix_bfrops_base_print_uint64(char **output, char *prefix, uint64_t *src, pmix_data_type_t type)
{
    char *prefx;
    int ret;

    PMIX_HIDE_UNUSED_PARAMS(type);

    /* deal with NULL prefix */
    if (NULL == prefix) {
        if (0 > asprintf(&prefx, " ")) {
            return PMIX_ERR_NOMEM;
        }
    } else {
        prefx = prefix;
    }

    /* if src is NULL, just print data type and return */
    if (NULL == src) {
        ret = asprintf(output, "%sData type: PMIX_UINT64\tValue: NULL pointer", prefx);
        if (prefx != prefix) {
            free(prefx);
        }
        if (0 > ret) {
            return PMIX_ERR_OUT_OF_RESOURCE;
        } else {
            return PMIX_SUCCESS;
        }
    }

    ret = asprintf(output, "%sData type: PMIX_UINT64\tValue: %lu", prefx, (unsigned long) *src);
    if (prefx != prefix) {
        free(prefx);
    }

    if (0 > ret) {
        return PMIX_ERR_OUT_OF_RESOURCE;
    } else {
        return PMIX_SUCCESS;
    }
}

int pmix_bfrops_base_print_int64(char **output, char *prefix, int64_t *src, pmix_data_type_t type)
{
    char *prefx;
    int ret;

    PMIX_HIDE_UNUSED_PARAMS(type);

    /* deal with NULL prefix */
    if (NULL == prefix) {
        if (0 > asprintf(&prefx, " ")) {
            return PMIX_ERR_NOMEM;
        }
    } else {
        prefx = prefix;
    }

    /* if src is NULL, just print data type and return */
    if (NULL == src) {
        ret = asprintf(output, "%sData type: PMIX_INT64\tValue: NULL pointer", prefx);
        if (prefx != prefix) {
            free(prefx);
        }
        if (0 > ret) {
            return PMIX_ERR_OUT_OF_RESOURCE;
        } else {
            return PMIX_SUCCESS;
        }
    }

    ret = asprintf(output, "%sData type: PMIX_INT64\tValue: %ld", prefx, (long) *src);
    if (prefx != prefix) {
        free(prefx);
    }

    if (0 > ret) {
        return PMIX_ERR_OUT_OF_RESOURCE;
    } else {
        return PMIX_SUCCESS;
    }
}

int pmix_bfrops_base_print_float(char **output, char *prefix, float *src, pmix_data_type_t type)
{
    char *prefx;
    int ret;

    PMIX_HIDE_UNUSED_PARAMS(type);

    /* deal with NULL prefix */
    if (NULL == prefix) {
        if (0 > asprintf(&prefx, " ")) {
            return PMIX_ERR_NOMEM;
        }
    } else {
        prefx = prefix;
    }

    /* if src is NULL, just print data type and return */
    if (NULL == src) {
        ret = asprintf(output, "%sData type: PMIX_FLOAT\tValue: NULL pointer", prefx);
        if (prefx != prefix) {
            free(prefx);
        }
        if (0 > ret) {
            return PMIX_ERR_OUT_OF_RESOURCE;
        } else {
            return PMIX_SUCCESS;
        }
    }

    ret = asprintf(output, "%sData type: PMIX_FLOAT\tValue: %f", prefx, *src);
    if (prefx != prefix) {
        free(prefx);
    }

    if (0 > ret) {
        return PMIX_ERR_OUT_OF_RESOURCE;
    } else {
        return PMIX_SUCCESS;
    }
}

int pmix_bfrops_base_print_double(char **output, char *prefix, double *src, pmix_data_type_t type)
{
    char *prefx;
    int ret;

    PMIX_HIDE_UNUSED_PARAMS(type);

    /* deal with NULL prefix */
    if (NULL == prefix) {
        if (0 > asprintf(&prefx, " ")) {
            return PMIX_ERR_NOMEM;
        }
    } else {
        prefx = prefix;
    }

    /* if src is NULL, just print data type and return */
    if (NULL == src) {
        ret = asprintf(output, "%sData type: PMIX_DOUBLE\tValue: NULL pointer", prefx);
        if (prefx != prefix) {
            free(prefx);
        }
        if (0 > ret) {
            return PMIX_ERR_OUT_OF_RESOURCE;
        } else {
            return PMIX_SUCCESS;
        }
    }

    ret = asprintf(output, "%sData type: PMIX_DOUBLE\tValue: %f", prefx, *src);
    if (prefx != prefix) {
        free(prefx);
    }

    if (0 > ret) {
        return PMIX_ERR_OUT_OF_RESOURCE;
    } else {
        return PMIX_SUCCESS;
    }
}

int pmix_bfrops_base_print_time(char **output, char *prefix, time_t *src, pmix_data_type_t type)
{
    char *prefx;
    char *t;
    int ret;

    PMIX_HIDE_UNUSED_PARAMS(type);

    /* deal with NULL prefix */
    if (NULL == prefix) {
        if (0 > asprintf(&prefx, " ")) {
            return PMIX_ERR_NOMEM;
        }
    } else {
        prefx = prefix;
    }

    /* if src is NULL, just print data type and return */
    if (NULL == src) {
        ret = asprintf(output, "%sData type: PMIX_TIME\tValue: NULL pointer", prefx);
        if (prefx != prefix) {
            free(prefx);
        }
        if (0 > ret) {
            return PMIX_ERR_OUT_OF_RESOURCE;
        } else {
            return PMIX_SUCCESS;
        }
    }

    t = ctime(src);
    t[strlen(t) - 1] = '\0'; // remove trailing newline

    ret = asprintf(output, "%sData type: PMIX_TIME\tValue: %s", prefx, t);
    if (prefx != prefix) {
        free(prefx);
    }

    if (0 > ret) {
        return PMIX_ERR_OUT_OF_RESOURCE;
    } else {
        return PMIX_SUCCESS;
    }
}

int pmix_bfrops_base_print_timeval(char **output, char *prefix, struct timeval *src,
                                   pmix_data_type_t type)
{
    char *prefx;
    int ret;

    PMIX_HIDE_UNUSED_PARAMS(type);

    /* deal with NULL prefix */
    if (NULL == prefix) {
        if (0 > asprintf(&prefx, " ")) {
            return PMIX_ERR_NOMEM;
        }
    } else {
        prefx = prefix;
    }

    /* if src is NULL, just print data type and return */
    if (NULL == src) {
        ret = asprintf(output, "%sData type: PMIX_TIMEVAL\tValue: NULL pointer", prefx);
        if (prefx != prefix) {
            free(prefx);
        }
        if (0 > ret) {
            return PMIX_ERR_OUT_OF_RESOURCE;
        } else {
            return PMIX_SUCCESS;
        }
    }

    ret = asprintf(output, "%sData type: PMIX_TIMEVAL\tValue: %ld.%06ld", prefx, (long) src->tv_sec,
                   (long) src->tv_usec);
    if (prefx != prefix) {
        free(prefx);
    }

    if (0 > ret) {
        return PMIX_ERR_OUT_OF_RESOURCE;
    } else {
        return PMIX_SUCCESS;
    }
}

int pmix_bfrops_base_print_status(char **output, char *prefix, pmix_status_t *src,
                                  pmix_data_type_t type)
{
    char *prefx;
    int ret;

    PMIX_HIDE_UNUSED_PARAMS(type);

    /* deal with NULL prefix */
    if (NULL == prefix) {
        if (0 > asprintf(&prefx, " ")) {
            return PMIX_ERR_NOMEM;
        }
    } else {
        prefx = prefix;
    }

    /* if src is NULL, just print data type and return */
    if (NULL == src) {
        ret = asprintf(output, "%sData type: PMIX_STATUS\tValue: NULL pointer", prefx);
        if (prefx != prefix) {
            free(prefx);
        }
        if (0 > ret) {
            return PMIX_ERR_OUT_OF_RESOURCE;
        } else {
            return PMIX_SUCCESS;
        }
    }

    ret = asprintf(output, "%sData type: PMIX_STATUS\tValue: %s", prefx, PMIx_Error_string(*src));
    if (prefx != prefix) {
        free(prefx);
    }

    if (0 > ret) {
        return PMIX_ERR_OUT_OF_RESOURCE;
    } else {
        return PMIX_SUCCESS;
    }
}

/* PRINT FUNCTIONS FOR GENERIC PMIX TYPES */

static int print_val(char **output, pmix_value_t *src)
{
    int rc;
    char *tp;

    switch (src->type) {
        case PMIX_UNDEF:
            tp = strdup(" Data type: PMIX_UNDEF");
            rc = PMIX_SUCCESS;
            break;
        case PMIX_BOOL:
            rc = pmix_bfrops_base_print_bool(&tp, NULL, &src->data.flag, PMIX_BOOL);
            break;
        case PMIX_BYTE:
            rc = pmix_bfrops_base_print_byte(&tp, NULL, &src->data.byte, PMIX_STRING);
            break;
        case PMIX_STRING:
            rc = pmix_bfrops_base_print_string(&tp, NULL, src->data.string, PMIX_STRING);
            break;
        case PMIX_SIZE:
            rc = pmix_bfrops_base_print_size(&tp, NULL, &src->data.size, PMIX_SIZE);
            break;
        case PMIX_PID:
            rc = pmix_bfrops_base_print_pid(&tp, NULL, &src->data.pid, PMIX_PID);
            break;
        case PMIX_INT:
            rc = pmix_bfrops_base_print_int(&tp, NULL, &src->data.integer, PMIX_INT);
            break;
        case PMIX_INT8:
            rc = pmix_bfrops_base_print_int8(&tp, NULL, &src->data.int8, PMIX_INT8);
            break;
        case PMIX_INT16:
            rc = pmix_bfrops_base_print_int16(&tp, NULL, &src->data.int16, PMIX_INT16);
            break;
        case PMIX_INT32:
            rc = pmix_bfrops_base_print_int32(&tp, NULL, &src->data.int32, PMIX_INT32);
            break;
        case PMIX_INT64:
            rc = pmix_bfrops_base_print_int64(&tp, NULL, &src->data.int64, PMIX_INT64);
            break;
        case PMIX_UINT:
            rc = pmix_bfrops_base_print_uint(&tp, NULL, &src->data.uint, PMIX_UINT);
            break;
        case PMIX_UINT8:
            rc = pmix_bfrops_base_print_uint8(&tp, NULL, &src->data.uint8, PMIX_UINT8);
            break;
        case PMIX_UINT16:
            rc = pmix_bfrops_base_print_uint16(&tp, NULL, &src->data.uint16, PMIX_UINT16);
            break;
        case PMIX_UINT32:
            rc = pmix_bfrops_base_print_uint32(&tp, NULL, &src->data.uint32, PMIX_UINT32);
            break;
        case PMIX_UINT64:
            rc = pmix_bfrops_base_print_uint64(&tp, NULL, &src->data.uint64, PMIX_UINT64);
            break;
        case PMIX_FLOAT:
            rc = pmix_bfrops_base_print_float(&tp, NULL, &src->data.fval, PMIX_FLOAT);
            break;
        case PMIX_DOUBLE:
            rc = pmix_bfrops_base_print_double(&tp, NULL, &src->data.dval, PMIX_DOUBLE);
            break;
        case PMIX_TIMEVAL:
            rc = pmix_bfrops_base_print_timeval(&tp, NULL, &src->data.tv, PMIX_TIMEVAL);
            break;
        case PMIX_TIME:
            rc = pmix_bfrops_base_print_time(&tp, NULL, &src->data.time, PMIX_TIME);
            break;
        case PMIX_STATUS:
            rc = pmix_bfrops_base_print_status(&tp, NULL, &src->data.status, PMIX_STATUS);
            break;
        case PMIX_PROC_RANK:
            rc = pmix_bfrops_base_print_rank(&tp, NULL, &src->data.rank, PMIX_PROC_RANK);
            break;
        case PMIX_PROC_NSPACE:
            rc = pmix_bfrops_base_print_nspace(&tp, NULL, src->data.nspace, PMIX_PROC_NSPACE);
            break;
        case PMIX_PROC:
            rc = pmix_bfrops_base_print_proc(&tp, NULL, src->data.proc, PMIX_PROC);
            break;
        case PMIX_BYTE_OBJECT:
            rc = pmix_bfrops_base_print_bo(&tp, NULL, &src->data.bo, PMIX_BYTE_OBJECT);
            break;
        case PMIX_PERSIST:
            rc = pmix_bfrops_base_print_persist(&tp, NULL, &src->data.persist, PMIX_PERSIST);
            break;
        case PMIX_SCOPE:
            rc = pmix_bfrops_base_print_scope(&tp, NULL, &src->data.scope, PMIX_SCOPE);
            break;
        case PMIX_DATA_RANGE:
            rc = pmix_bfrops_base_print_range(&tp, NULL, &src->data.range, PMIX_DATA_RANGE);
            break;
        case PMIX_PROC_STATE:
            rc = pmix_bfrops_base_print_pstate(&tp, NULL, &src->data.state, PMIX_PROC_STATE);
            break;
        case PMIX_PROC_INFO:
            rc = pmix_bfrops_base_print_pinfo(&tp, NULL, src->data.pinfo, PMIX_PROC_INFO);
            break;
        case PMIX_DATA_ARRAY:
            rc = pmix_bfrops_base_print_darray(&tp, NULL, src->data.darray, PMIX_DATA_ARRAY);
            break;
        case PMIX_REGATTR:
            rc = pmix_bfrops_base_print_regattr(&tp, NULL, src->data.ptr, PMIX_REGATTR);
            break;
        case PMIX_ALLOC_DIRECTIVE:
            rc = pmix_bfrops_base_print_alloc_directive(&tp, NULL, &src->data.adir, PMIX_ALLOC_DIRECTIVE);
            break;
        case PMIX_ENVAR:
            rc = pmix_bfrops_base_print_envar(&tp, NULL, &src->data.envar, PMIX_ENVAR);
            break;
        case PMIX_COORD:
            rc = pmix_bfrops_base_print_coord(&tp, NULL, src->data.coord, PMIX_COORD);
            break;
        case PMIX_LINK_STATE:
            rc = pmix_bfrops_base_print_linkstate(&tp, NULL, &src->data.linkstate, PMIX_LINK_STATE);
            break;
        case PMIX_JOB_STATE:
            rc = pmix_bfrops_base_print_jobstate(&tp, NULL, &src->data.jstate, PMIX_JOB_STATE);
            break;
        case PMIX_TOPO:
            rc = pmix_bfrops_base_print_topology(&tp, NULL, src->data.topo, PMIX_TOPO);
            break;
        case PMIX_PROC_CPUSET:
            rc = pmix_bfrops_base_print_cpuset(&tp, NULL, src->data.cpuset, PMIX_PROC_CPUSET);
            break;
        case PMIX_LOCTYPE:
            rc = pmix_bfrops_base_print_locality(&tp, NULL, &src->data.locality, PMIX_LOCTYPE);
            break;
        case PMIX_GEOMETRY:
            rc = pmix_bfrops_base_print_geometry(&tp, NULL, src->data.geometry, PMIX_GEOMETRY);
            break;
        case PMIX_DEVTYPE:
            rc = pmix_bfrops_base_print_devtype(&tp, NULL, &src->data.devtype, PMIX_DEVTYPE);
            break;
        case PMIX_DEVICE_DIST:
            rc = pmix_bfrops_base_print_devdist(&tp, NULL, src->data.devdist, PMIX_DEVICE_DIST);
            break;
        case PMIX_ENDPOINT:
            rc = pmix_bfrops_base_print_endpoint(&tp, NULL, src->data.endpoint, PMIX_ENDPOINT);
            break;
        case PMIX_STOR_MEDIUM:
            rc = pmix_bfrops_base_print_smed(&tp, NULL, &src->data.uint64, PMIX_STOR_MEDIUM);
            break;
        case PMIX_STOR_ACCESS:
            rc = pmix_bfrops_base_print_sacc(&tp, NULL, &src->data.uint64, PMIX_STOR_ACCESS);
            break;
        case PMIX_STOR_PERSIST:
            rc = pmix_bfrops_base_print_spers(&tp, NULL, &src->data.uint64, PMIX_STOR_PERSIST);
            break;
        case PMIX_STOR_ACCESS_TYPE:
            rc = pmix_bfrops_base_print_satyp(&tp, NULL, &src->data.uint16, PMIX_STOR_ACCESS_TYPE);
            break;
        default:
            pmix_asprintf(&tp, " Data type: %s(%d)\tValue: UNPRINTABLE",
                          PMIx_Data_type_string(src->type), (int)(src->type));
            rc = PMIX_SUCCESS;
            break;
    }

    *output = tp;
    return rc;
}
/*
 * PMIX_VALUE
 */
int pmix_bfrops_base_print_value(char **output, char *prefix, pmix_value_t *src,
                                 pmix_data_type_t type)
{
    char *prefx;
    int rc;
    char *tp;

    PMIX_HIDE_UNUSED_PARAMS(type);

    /* deal with NULL prefix */
    if (NULL == prefix) {
        if (0 > asprintf(&prefx, " ")) {
            return PMIX_ERR_NOMEM;
        }
    } else {
        prefx = prefix;
    }

    /* if src is NULL, just print data type and return */
    if (NULL == src) {
        pmix_asprintf(output, "%sData type: PMIX_VALUE\tValue: NULL pointer", prefx);
        if (prefx != prefix) {
            free(prefx);
        }
        return PMIX_SUCCESS;
    }

    rc = print_val(&tp, src);
    if (PMIX_SUCCESS == rc) {
        rc = asprintf(output, "%sPMIX_VALUE: %s", prefx, tp);
        free(tp);
    }
    if (prefx != prefix) {
        free(prefx);
    }
    if (0 > rc) {
        return PMIX_ERR_OUT_OF_RESOURCE;
    } else {
        return PMIX_SUCCESS;
    }
}

int pmix_bfrops_base_print_info(char **output, char *prefix, pmix_info_t *src,
                                pmix_data_type_t type)
{
    char *tmp = NULL, *tmp2 = NULL;
    char *prefx;
    int ret;

    PMIX_HIDE_UNUSED_PARAMS(type);

    if (NULL == prefix) {
        prefx = " ";
    } else {
        prefx = prefix;
    }

    pmix_bfrops_base_print_value(&tmp, prefx, &src->value, PMIX_VALUE);
    pmix_bfrops_base_print_info_directives(&tmp2, prefx, &src->flags, PMIX_INFO_DIRECTIVES);
    ret = asprintf(output, "%sKEY: %s\n%s\t%s\n%s\t%s",
                   (NULL == prefix) ? " " : prefix, src->key,
                   (NULL == prefix) ? " " : prefix, tmp2,
                   (NULL == prefix) ? " " : prefix, tmp);
    free(tmp);
    free(tmp2);
    if (0 > ret) {
        return PMIX_ERR_OUT_OF_RESOURCE;
    } else {
        return PMIX_SUCCESS;
    }
}

int pmix_bfrops_base_print_pdata(char **output, char *prefix, pmix_pdata_t *src,
                                 pmix_data_type_t type)
{
    char *tmp1, *tmp2;
    int ret;

    PMIX_HIDE_UNUSED_PARAMS(type);

    pmix_bfrops_base_print_proc(&tmp1, NULL, &src->proc, PMIX_PROC);
    pmix_bfrops_base_print_value(&tmp2, NULL, &src->value, PMIX_VALUE);
    ret = asprintf(output, "%s  %s  KEY: %s %s",
                   (NULL == prefix) ? " " : prefix, tmp1, src->key,
                   (NULL == tmp2) ? "NULL" : tmp2);
    if (NULL != tmp1) {
        free(tmp1);
    }
    if (NULL != tmp2) {
        free(tmp2);
    }
    if (0 > ret) {
        return PMIX_ERR_OUT_OF_RESOURCE;
    } else {
        return PMIX_SUCCESS;
    }
}

int pmix_bfrops_base_print_buf(char **output, char *prefix, pmix_buffer_t *src,
                               pmix_data_type_t type)
{
    PMIX_HIDE_UNUSED_PARAMS(output, prefix, src, type);

    return PMIX_SUCCESS;
}

int pmix_bfrops_base_print_app(char **output, char *prefix, pmix_app_t *src, pmix_data_type_t type)
{
    PMIX_HIDE_UNUSED_PARAMS(output, prefix, src, type);

    return PMIX_SUCCESS;
}

int pmix_bfrops_base_print_proc(char **output, char *prefix, pmix_proc_t *src,
                                pmix_data_type_t type)
{
    int rc;

    PMIX_HIDE_UNUSED_PARAMS(type);

    if (NULL == src){
        *output = strdup("%sPROC: NULL");
        rc = PMIX_SUCCESS;
    } else {
        switch (src->rank) {
            case PMIX_RANK_UNDEF:
                rc = asprintf(output, "%sPROC: %s:PMIX_RANK_UNDEF",
                              (NULL == prefix) ? " " : prefix, src->nspace);
                break;
            case PMIX_RANK_WILDCARD:
                rc = asprintf(output, "%sPROC: %s:PMIX_RANK_WILDCARD",
                              (NULL == prefix) ? " " : prefix, src->nspace);
                break;
            case PMIX_RANK_LOCAL_NODE:
                rc = asprintf(output, "%sPROC: %s:PMIX_RANK_LOCAL_NODE",
                              (NULL == prefix) ? " " : prefix, src->nspace);
                break;
            default:
                rc = asprintf(output, "%sPROC: %s:%lu",
                              (NULL == prefix) ? " " : prefix
                              , src->nspace, (unsigned long) (src->rank));
        }
    }
    if (0 > rc) {
        return PMIX_ERR_NOMEM;
    }
    return PMIX_SUCCESS;
}

int pmix_bfrops_base_print_kval(char **output, char *prefix, pmix_kval_t *src,
                                pmix_data_type_t type)
{
    PMIX_HIDE_UNUSED_PARAMS(output, prefix, src, type);

    return PMIX_SUCCESS;
}

int pmix_bfrops_base_print_persist(char **output, char *prefix, pmix_persistence_t *src,
                                   pmix_data_type_t type)
{
    PMIX_HIDE_UNUSED_PARAMS(type);

    /* if src is NULL, just print data type and return */
    if (NULL == src) {
        if (0 > asprintf(output, "%sData type: PMIX_PERSIST\tValue: NULL pointer",
                         (NULL == prefix) ? " " : prefix)) {
            return PMIX_ERR_NOMEM;
        }
        return PMIX_SUCCESS;
    }

    if (0 > asprintf(output, "%sData type: PMIX_PERSIST\tValue: %ld",
                     (NULL == prefix) ? " " : prefix, (long) *src)) {
        return PMIX_ERR_NOMEM;
    }

    return PMIX_SUCCESS;
}

pmix_status_t pmix_bfrops_base_print_scope(char **output, char *prefix, pmix_scope_t *src,
                                           pmix_data_type_t type)
{
    PMIX_HIDE_UNUSED_PARAMS(type);

    if (0
        > asprintf(output, "%sData type: PMIX_SCOPE\tValue: %s",
                   (NULL == prefix) ? " " : prefix,
                   PMIx_Scope_string(*src))) {
        return PMIX_ERR_NOMEM;
    }

    return PMIX_SUCCESS;
}

pmix_status_t pmix_bfrops_base_print_range(char **output, char *prefix, pmix_data_range_t *src,
                                           pmix_data_type_t type)
{
    PMIX_HIDE_UNUSED_PARAMS(type);

    if (0 > asprintf(output, "%sData type: PMIX_DATA_RANGE\tValue: %s",
                     (NULL == prefix) ? " " : prefix,
                     PMIx_Data_range_string(*src))) {
        return PMIX_ERR_NOMEM;
    }

    return PMIX_SUCCESS;
}
pmix_status_t pmix_bfrops_base_print_cmd(char **output, char *prefix, pmix_cmd_t *src,
                                         pmix_data_type_t type)
{
    PMIX_HIDE_UNUSED_PARAMS(type);

    if (0 > asprintf(output, "%sData type: PMIX_COMMAND\tValue: %s",
                     (NULL == prefix) ? " " : prefix,
                     pmix_command_string(*src))) {
        return PMIX_ERR_NOMEM;
    }

    return PMIX_SUCCESS;
}

pmix_status_t pmix_bfrops_base_print_info_directives(char **output, char *prefix,
                                                     pmix_info_directives_t *src,
                                                     pmix_data_type_t type)
{
    char *tmp;

    PMIX_HIDE_UNUSED_PARAMS(type);

    tmp = PMIx_Info_directives_string(*src);
    if (0 > asprintf(output, "%sData type: PMIX_INFO_DIRECTIVES\tValue: %s",
                     (NULL == prefix) ? " " : prefix, tmp)) {
        free(tmp);
        return PMIX_ERR_NOMEM;
    }
    free(tmp);

    return PMIX_SUCCESS;
}

pmix_status_t pmix_bfrops_base_print_datatype(char **output, char *prefix,
                                              pmix_data_type_t *src,
                                              pmix_data_type_t type)
{
    int ret;

    PMIX_HIDE_UNUSED_PARAMS(type);

    /* if src is NULL, just print data type and return */
    if (NULL == src) {
        ret = asprintf(output, "%sData type: PMIX_DATA_TYPE\tValue: NULL pointer",
                       (NULL == prefix) ? " " : prefix);
        if (0 > ret) {
            return PMIX_ERR_OUT_OF_RESOURCE;
        } else {
            return PMIX_SUCCESS;
        }
    }

    ret = asprintf(output, "%sData type: PMIX_DATA_TYPE\tValue: %s",
                   (NULL == prefix) ? " " : prefix,
                   PMIx_Data_type_string(*src));

    if (0 > ret) {
        return PMIX_ERR_OUT_OF_RESOURCE;
    } else {
        return PMIX_SUCCESS;
    }
}

int pmix_bfrops_base_print_bo(char **output, char *prefix,
                              pmix_byte_object_t *src,
                              pmix_data_type_t type)
{
    int ret;

    PMIX_HIDE_UNUSED_PARAMS(type);

    /* if src is NULL, just print data type and return */
    if (NULL == src) {
        ret = asprintf(output, "%sData type: %s\tValue: NULL pointer",
                       (NULL == prefix) ? " " : prefix,
                       (PMIX_COMPRESSED_BYTE_OBJECT == type) ? "PMIX_COMPRESSED_BYTE_OBJECT"
                                                             : "PMIX_BYTE_OBJECT");
        if (0 > ret) {
            return PMIX_ERR_OUT_OF_RESOURCE;
        } else {
            return PMIX_SUCCESS;
        }
    }

    ret = asprintf(output, "%sData type: %s\tSize: %ld",
                   (NULL == prefix) ? " " : prefix,
                   (PMIX_COMPRESSED_BYTE_OBJECT == type) ? "PMIX_COMPRESSED_BYTE_OBJECT"
                                                         : "PMIX_BYTE_OBJECT",
                   (long) src->size);

    if (0 > ret) {
        return PMIX_ERR_OUT_OF_RESOURCE;
    } else {
        return PMIX_SUCCESS;
    }
}

int pmix_bfrops_base_print_ptr(char **output, char *prefix, void *src,
                               pmix_data_type_t type)
{
    int ret;

    PMIX_HIDE_UNUSED_PARAMS(type);

    ret = asprintf(output, "%sData type: PMIX_POINTER\tAddress: %p",
                   (NULL == prefix) ? " " : prefix, src);

    if (0 > ret) {
        return PMIX_ERR_OUT_OF_RESOURCE;
    } else {
        return PMIX_SUCCESS;
    }
}

pmix_status_t pmix_bfrops_base_print_pstate(char **output, char *prefix,
                                            pmix_proc_state_t *src,
                                            pmix_data_type_t type)
{
    int ret;

    PMIX_HIDE_UNUSED_PARAMS(type);

    ret = asprintf(output, "%sData type: PMIX_PROC_STATE\tValue: %s",
                   (NULL == prefix) ? " " : prefix,
                   PMIx_Proc_state_string(*src));

    if (0 > ret) {
        return PMIX_ERR_OUT_OF_RESOURCE;
    } else {
        return PMIX_SUCCESS;
    }
}

pmix_status_t pmix_bfrops_base_print_pinfo(char **output, char *prefix,
                                           pmix_proc_info_t *src,
                                           pmix_data_type_t type)
{
    pmix_status_t rc = PMIX_SUCCESS;
    char *p2, *tmp;

    PMIX_HIDE_UNUSED_PARAMS(type);

    if (0 > asprintf(&p2, "%s\t", (NULL == prefix) ? " " : prefix)) {
        rc = PMIX_ERR_NOMEM;
        goto done;
    }

    if (PMIX_SUCCESS != (rc = pmix_bfrops_base_print_proc(&tmp, p2, &src->proc, PMIX_PROC))) {
        free(p2);
        goto done;
    }

    if (0 > asprintf(output,
                     "%sData type: PMIX_PROC_INFO\tValue:\n%s\n%sHostname: %s\tExecutable: "
                     "%s\n%sPid: %lu\tExit code: %d\tState: %s",
                     (NULL == prefix) ? " " : prefix, tmp, p2,
                     src->hostname, src->executable_name, p2,
                     (unsigned long) src->pid, src->exit_code,
                     PMIx_Proc_state_string(src->state))) {
        free(p2);
        rc = PMIX_ERR_NOMEM;
    }

done:

    return rc;
}

pmix_status_t pmix_bfrops_base_print_darray(char **output, char *prefix,
                                            pmix_data_array_t *src,
                                            pmix_data_type_t type)
{
    char *prefx, *tp, *tp2=NULL, *tp3;
    pmix_status_t rc = PMIX_ERR_BAD_PARAM;
    size_t n;
    bool *bptr;
    uint8_t *u8ptr;
    int8_t *i8ptr;
    uint16_t *u16ptr;
    int16_t *i16ptr;
    uint32_t *u32ptr;
    int32_t *i32ptr;
    uint64_t *u64ptr;
    int64_t *i64ptr;
    char **strings;
    size_t *szptr;
    pid_t *pidptr;
    unsigned int *uintptr;
    int *intptr;
    float *fltptr;
    double *dblptr;
    struct timeval *tvlptr;
    time_t *tmptr;
    pmix_status_t *stptr;
    pmix_rank_t *rkptr;
    pmix_nspace_t *nsptr;
    pmix_proc_t *procptr;
    pmix_info_t *iptr;
    pmix_byte_object_t *boptr;
    pmix_persistence_t *prstptr;
    pmix_scope_t *scptr;
    pmix_data_range_t *drptr;
    pmix_proc_state_t *psptr;
    pmix_proc_info_t *piptr;
    pmix_data_array_t *daptr;
    pmix_regattr_t *rgptr;
    pmix_alloc_directive_t *adptr;
    pmix_envar_t *evptr;
    pmix_coord_t *coptr;
    pmix_link_state_t *lkptr;
    pmix_job_state_t *jsptr;
    pmix_topology_t *tptr;
    pmix_cpuset_t *cpsptr;
    pmix_locality_t *lcptr;
    pmix_geometry_t *geoptr;
    pmix_device_type_t *dvptr;
    pmix_device_distance_t *ddptr;
    pmix_endpoint_t *endptr;
    pmix_storage_medium_t *smptr;
    pmix_storage_accessibility_t *saptr;
    pmix_storage_persistence_t *spptr;
    pmix_storage_access_type_t *satptr;

    PMIX_HIDE_UNUSED_PARAMS(type);

    /* deal with NULL prefix */
    if (NULL == prefix) {
        if (0 > asprintf(&prefx, " ")) {
            return PMIX_ERR_NOMEM;
        }
    } else {
        prefx = prefix;
    }

    for (n=0; n < src->size; n++) {
        tp = NULL;
        switch (src->type) {
            case PMIX_BOOL:
                bptr = (bool*)src->array;
                rc = pmix_bfrops_base_print_bool(&tp, prefx, &bptr[n], PMIX_BOOL);
                break;
            case PMIX_BYTE:
                u8ptr = (uint8_t*)src->array;
                rc = pmix_bfrops_base_print_byte(&tp, prefx, &u8ptr[n], PMIX_STRING);
                break;
            case PMIX_STRING:
                strings = (char**)src->array;
                rc = pmix_bfrops_base_print_string(&tp, prefx, strings[n], PMIX_STRING);
                break;
            case PMIX_SIZE:
                szptr = (size_t*)src->array;
                rc = pmix_bfrops_base_print_size(&tp, prefx, &szptr[n], PMIX_SIZE);
                break;
            case PMIX_PID:
                pidptr = (pid_t*)src->array;
                rc = pmix_bfrops_base_print_pid(&tp, prefx, &pidptr[n], PMIX_PID);
                break;
            case PMIX_INT:
                intptr = (int*)src->array;
                rc = pmix_bfrops_base_print_int(&tp, prefx, &intptr[n], PMIX_INT);
                break;
            case PMIX_INT8:
                i8ptr = (int8_t*)src->array;
                rc = pmix_bfrops_base_print_int8(&tp, prefx, &i8ptr[n], PMIX_INT8);
                break;
            case PMIX_INT16:
                i16ptr = (int16_t*)src->array;
                rc = pmix_bfrops_base_print_int16(&tp, prefx, &i16ptr[n], PMIX_INT16);
                break;
            case PMIX_INT32:
                i32ptr = (int32_t*)src->array;
                rc = pmix_bfrops_base_print_int32(&tp, prefx, &i32ptr[n], PMIX_INT32);
                break;
            case PMIX_INT64:
                i64ptr = (int64_t*)src->array;
                rc = pmix_bfrops_base_print_int64(&tp, prefx, &i64ptr[n], PMIX_INT64);
                break;
            case PMIX_UINT:
                uintptr = (unsigned int*)src->array;
                rc = pmix_bfrops_base_print_uint(&tp, prefx, &uintptr[n], PMIX_UINT);
                break;
            case PMIX_UINT8:
                u8ptr = (uint8_t*)src->array;
                rc = pmix_bfrops_base_print_uint8(&tp, prefx, &u8ptr[n], PMIX_UINT8);
                break;
            case PMIX_UINT16:
                u16ptr = (uint16_t*)src->array;
                rc = pmix_bfrops_base_print_uint16(&tp, prefx, &u16ptr[n], PMIX_UINT16);
                break;
            case PMIX_UINT32:
                u32ptr = (uint32_t*)src->array;
                rc = pmix_bfrops_base_print_uint32(&tp, prefx, &u32ptr[n], PMIX_UINT32);
                break;
            case PMIX_UINT64:
                u64ptr = (uint64_t*)src->array;
                rc = pmix_bfrops_base_print_uint64(&tp, prefx, &u64ptr[n], PMIX_UINT64);
                break;
            case PMIX_FLOAT:
                fltptr = (float*)src->array;
                rc = pmix_bfrops_base_print_float(&tp, prefx, &fltptr[n], PMIX_FLOAT);
                break;
            case PMIX_DOUBLE:
                dblptr = (double*)src->array;
                rc = pmix_bfrops_base_print_double(&tp, prefx, &dblptr[n], PMIX_DOUBLE);
                break;
            case PMIX_TIMEVAL:
                tvlptr = (struct timeval*)src->array;
                rc = pmix_bfrops_base_print_timeval(&tp, prefx, &tvlptr[n], PMIX_TIMEVAL);
                break;
            case PMIX_TIME:
                tmptr = (time_t*)src->array;
                rc = pmix_bfrops_base_print_time(&tp, prefx, &tmptr[n], PMIX_TIME);
                break;
            case PMIX_STATUS:
                stptr = (pmix_status_t*)src->array;
                rc = pmix_bfrops_base_print_status(&tp, prefx, &stptr[n], PMIX_STATUS);
                break;
            case PMIX_PROC_RANK:
                rkptr = (pmix_rank_t*)src->array;
                rc = pmix_bfrops_base_print_rank(&tp, prefx, &rkptr[n], PMIX_PROC_RANK);
                break;
            case PMIX_PROC_NSPACE:
                nsptr = (pmix_nspace_t*)src->array;
                rc = pmix_bfrops_base_print_nspace(&tp, prefx, &nsptr[n], PMIX_PROC_NSPACE);
                break;
            case PMIX_PROC:
                procptr = (pmix_proc_t*)src->array;
                rc = pmix_bfrops_base_print_proc(&tp, prefx, &procptr[n], PMIX_PROC);
                break;
            case PMIX_INFO:
                iptr = (pmix_info_t*)src->array;
                rc = pmix_bfrops_base_print_info(&tp, prefx, &iptr[n], PMIX_INFO);
                break;
            case PMIX_BYTE_OBJECT:
                boptr = (pmix_byte_object_t*)src->array;
                rc = pmix_bfrops_base_print_bo(&tp, prefx, &boptr[n], PMIX_BYTE_OBJECT);
                break;
            case PMIX_PERSIST:
                prstptr = (pmix_persistence_t*)src->array;
                rc = pmix_bfrops_base_print_persist(&tp, prefx, &prstptr[n], PMIX_PERSIST);
                break;
            case PMIX_SCOPE:
                scptr = (pmix_scope_t*)src->array;
                rc = pmix_bfrops_base_print_scope(&tp, prefx, &scptr[n], PMIX_SCOPE);
                break;
            case PMIX_DATA_RANGE:
                drptr = (pmix_data_range_t*)src->array;
                rc = pmix_bfrops_base_print_range(&tp, prefx, &drptr[n], PMIX_DATA_RANGE);
                break;
            case PMIX_PROC_STATE:
                psptr = (pmix_proc_state_t*)src->array;
                rc = pmix_bfrops_base_print_pstate(&tp, prefx, &psptr[n], PMIX_PROC_STATE);
                break;
            case PMIX_PROC_INFO:
                piptr = (pmix_proc_info_t*)src->array;
                rc = pmix_bfrops_base_print_pinfo(&tp, prefx, &piptr[n], PMIX_PROC_INFO);
                break;
            case PMIX_DATA_ARRAY:
                daptr = (pmix_data_array_t*)src->array;
                rc = pmix_bfrops_base_print_darray(&tp, prefx, &daptr[n], PMIX_DATA_ARRAY);
                break;
            case PMIX_REGATTR:
                rgptr = (pmix_regattr_t*)src->array;
                rc = pmix_bfrops_base_print_regattr(&tp, prefx, &rgptr[n], PMIX_REGATTR);
                break;
            case PMIX_ALLOC_DIRECTIVE:
                adptr = (pmix_alloc_directive_t*)src->array;
                rc = pmix_bfrops_base_print_alloc_directive(&tp, prefx, &adptr[n], PMIX_ALLOC_DIRECTIVE);
                break;
            case PMIX_ENVAR:
                evptr = (pmix_envar_t*)src->array;
                rc = pmix_bfrops_base_print_envar(&tp, prefx, &evptr[n], PMIX_ENVAR);
                break;
            case PMIX_COORD:
                coptr = (pmix_coord_t*)src->array;
                rc = pmix_bfrops_base_print_coord(&tp, prefx, &coptr[n], PMIX_COORD);
                break;
            case PMIX_LINK_STATE:
                lkptr = (pmix_link_state_t*)src->array;
                rc = pmix_bfrops_base_print_linkstate(&tp, prefx, &lkptr[n], PMIX_LINK_STATE);
                break;
            case PMIX_JOB_STATE:
                jsptr = (pmix_job_state_t*)src->array;
                rc = pmix_bfrops_base_print_jobstate(&tp, prefx, &jsptr[n], PMIX_JOB_STATE);
                break;
            case PMIX_TOPO:
                tptr = (pmix_topology_t*)src->array;
                rc = pmix_bfrops_base_print_topology(&tp, prefx, &tptr[n], PMIX_TOPO);
                break;
            case PMIX_PROC_CPUSET:
                cpsptr = (pmix_cpuset_t*)src->array;
                rc = pmix_bfrops_base_print_cpuset(&tp, prefx, &cpsptr[n], PMIX_PROC_CPUSET);
                break;
            case PMIX_LOCTYPE:
                lcptr = (pmix_locality_t*)src->array;
                rc = pmix_bfrops_base_print_locality(&tp, prefx, &lcptr[n], PMIX_LOCTYPE);
                break;
            case PMIX_GEOMETRY:
                geoptr = (pmix_geometry_t*)src->array;
                rc = pmix_bfrops_base_print_geometry(&tp, prefx, &geoptr[n], PMIX_GEOMETRY);
                break;
            case PMIX_DEVTYPE:
                dvptr = (pmix_device_type_t*)src->array;
                rc = pmix_bfrops_base_print_devtype(&tp, prefx, &dvptr[n], PMIX_DEVTYPE);
                break;
            case PMIX_DEVICE_DIST:
                ddptr = (pmix_device_distance_t*)src->array;
                rc = pmix_bfrops_base_print_devdist(&tp, prefx, &ddptr[n], PMIX_DEVICE_DIST);
                break;
            case PMIX_ENDPOINT:
                endptr = (pmix_endpoint_t*)src->array;
                rc = pmix_bfrops_base_print_endpoint(&tp, prefx, &endptr[n], PMIX_ENDPOINT);
                break;
            case PMIX_STOR_MEDIUM:
                smptr = (pmix_storage_medium_t*)src->array;
                rc = pmix_bfrops_base_print_smed(&tp, prefx, &smptr[n], PMIX_STOR_MEDIUM);
                break;
            case PMIX_STOR_ACCESS:
                saptr = (pmix_storage_accessibility_t*)src->array;
                rc = pmix_bfrops_base_print_sacc(&tp, prefx, &saptr[n], PMIX_STOR_ACCESS);
                break;
            case PMIX_STOR_PERSIST:
                spptr = (pmix_storage_persistence_t*)src->array;
                rc = pmix_bfrops_base_print_spers(&tp, prefx, &spptr[n], PMIX_STOR_PERSIST);
                break;
            case PMIX_STOR_ACCESS_TYPE:
                satptr = (pmix_storage_access_type_t*)src->array;
                rc = pmix_bfrops_base_print_satyp(&tp, prefx, &satptr[n], PMIX_STOR_ACCESS_TYPE);
                break;
            default:
                pmix_asprintf(&tp, " Data type: %s(%d)\tValue: UNPRINTABLE",
                              PMIx_Data_type_string(src->type), (int)(src->type));
                rc = PMIX_SUCCESS;
                break;
        }
        if (NULL != tp) {
            if (NULL == tp2) {
                tp2 = strdup(tp);
            } else {
                pmix_asprintf(&tp3, "%s\n%s%s", tp2, prefx, tp);
                free(tp2);
                tp2 = tp3;
            }
            free(tp);
        }
    }
    pmix_asprintf(output, "%sData type: PMIX_DATA_ARRAY\tType: %s\tSize: %lu\n%s%s", prefx,
                  PMIx_Data_type_string(src->type), (unsigned long) src->size,
                  prefx, (NULL == tp2) ? "NULL" : tp2);
    free(tp2);
    if (prefx != prefix) {
        free(prefx);
    }

    return rc;
}

pmix_status_t pmix_bfrops_base_print_query(char **output, char *prefix, pmix_query_t *src,
                                           pmix_data_type_t type)
{
    char *p2;
    pmix_status_t rc = PMIX_SUCCESS;
    char *tmp, *t2, *t3;
    size_t n;

    PMIX_HIDE_UNUSED_PARAMS(type);

    if (0 > asprintf(&p2, "%s\t", (NULL == prefix) ? " " : prefix)) {
        rc = PMIX_ERR_NOMEM;
        goto done;
    }

    if (0 > asprintf(&tmp, "%sData type: PMIX_QUERY\tValue:",
                     (NULL == prefix) ? " " : prefix)) {
        free(p2);
        rc = PMIX_ERR_NOMEM;
        goto done;
    }

    /* print out the keys */
    if (NULL != src->keys) {
        for (n = 0; NULL != src->keys[n]; n++) {
            if (0 > asprintf(&t2, "%s\n%sKey: %s", tmp, p2, src->keys[n])) {
                free(p2);
                free(tmp);
                rc = PMIX_ERR_NOMEM;
                goto done;
            }
            free(tmp);
            tmp = t2;
        }
    }

    /* now print the qualifiers */
    if (0 < src->nqual) {
        for (n = 0; n < src->nqual; n++) {
            if (PMIX_SUCCESS
                != (rc = pmix_bfrops_base_print_info(&t2, p2, &src->qualifiers[n], PMIX_PROC))) {
                free(p2);
                free(tmp);
                goto done;
            }
            if (0 > asprintf(&t3, "%s\n%s", tmp, t2)) {
                free(p2);
                free(tmp);
                free(t2);
                rc = PMIX_ERR_NOMEM;
                goto done;
            }
            free(tmp);
            free(t2);
            tmp = t3;
        }
    }
    *output = tmp;

done:

    return rc;
}

pmix_status_t pmix_bfrops_base_print_rank(char **output, char *prefix, pmix_rank_t *src,
                                          pmix_data_type_t type)
{
    int rc;

    PMIX_HIDE_UNUSED_PARAMS(type);

    switch (*src) {
    case PMIX_RANK_UNDEF:
        rc = asprintf(output, "%sData type: PMIX_PROC_RANK\tValue: PMIX_RANK_UNDEF",
                      (NULL == prefix) ? " " : prefix);
        break;
    case PMIX_RANK_WILDCARD:
        rc = asprintf(output, "%sData type: PMIX_PROC_RANK\tValue: PMIX_RANK_WILDCARD",
                      (NULL == prefix) ? " " : prefix);
        break;
    case PMIX_RANK_LOCAL_NODE:
        rc = asprintf(output, "%sData type: PMIX_PROC_RANK\tValue: PMIX_RANK_LOCAL_NODE",
                      (NULL == prefix) ? " " : prefix);
        break;
    default:
        rc = asprintf(output, "%sData type: PMIX_PROC_RANK\tValue: %lu",
                      (NULL == prefix) ? " " : prefix,
                      (unsigned long) (*src));
    }
    if (0 > rc) {
        return PMIX_ERR_NOMEM;
    }
    return PMIX_SUCCESS;
}

pmix_status_t pmix_bfrops_base_print_alloc_directive(char **output, char *prefix,
                                                     pmix_alloc_directive_t *src,
                                                     pmix_data_type_t type)
{
    char *prefx;
    int ret;

    PMIX_HIDE_UNUSED_PARAMS(type);

    /* deal with NULL prefix */
    if (NULL == prefix) {
        if (0 > asprintf(&prefx, " ")) {
            return PMIX_ERR_NOMEM;
        }
    } else {
        prefx = prefix;
    }

    ret = asprintf(output, "%sData type: PMIX_ALLOC_DIRECTIVE\tValue: %s", prefx,
                   PMIx_Alloc_directive_string(*src));
    if (prefx != prefix) {
        free(prefx);
    }

    if (0 > ret) {
        return PMIX_ERR_OUT_OF_RESOURCE;
    } else {
        return PMIX_SUCCESS;
    }
}

pmix_status_t pmix_bfrops_base_print_iof_channel(char **output, char *prefix,
                                                 pmix_iof_channel_t *src, pmix_data_type_t type)
{
    char *prefx;
    int ret;

    PMIX_HIDE_UNUSED_PARAMS(type);

    /* deal with NULL prefix */
    if (NULL == prefix) {
        if (0 > asprintf(&prefx, " ")) {
            return PMIX_ERR_NOMEM;
        }
    } else {
        prefx = prefix;
    }

    ret = asprintf(output, "%sData type: PMIX_IOF_CHANNEL\tValue: %s", prefx,
                   PMIx_IOF_channel_string(*src));
    if (prefx != prefix) {
        free(prefx);
    }

    if (0 > ret) {
        return PMIX_ERR_OUT_OF_RESOURCE;
    } else {
        return PMIX_SUCCESS;
    }
}

pmix_status_t pmix_bfrops_base_print_envar(char **output, char *prefix, pmix_envar_t *src,
                                           pmix_data_type_t type)
{
    char *prefx;
    int ret;

    PMIX_HIDE_UNUSED_PARAMS(type);

    /* deal with NULL prefix */
    if (NULL == prefix) {
        if (0 > asprintf(&prefx, " ")) {
            return PMIX_ERR_NOMEM;
        }
    } else {
        prefx = prefix;
    }

    ret = asprintf(output, "%sData type: PMIX_ENVAR\tName: %s\tValue: %s\tSeparator: %c", prefx,
                   (NULL == src->envar) ? "NULL" : src->envar,
                   (NULL == src->value) ? "NULL" : src->value,
                   ('\0' == src->separator) ? ' ' : src->separator);
    if (prefx != prefix) {
        free(prefx);
    }

    if (0 > ret) {
        return PMIX_ERR_OUT_OF_RESOURCE;
    } else {
        return PMIX_SUCCESS;
    }
}

pmix_status_t pmix_bfrops_base_print_coord(char **output, char *prefix, pmix_coord_t *src,
                                           pmix_data_type_t type)
{
    char *prefx;
    int ret;
    char *tp;

    PMIX_HIDE_UNUSED_PARAMS(type);

    /* deal with NULL prefix */
    if (NULL == prefix) {
        if (0 > asprintf(&prefx, " ")) {
            return PMIX_ERR_NOMEM;
        }
    } else {
        prefx = prefix;
    }

    if (PMIX_COORD_VIEW_UNDEF == src->view) {
        tp = "UNDEF";
    } else if (PMIX_COORD_LOGICAL_VIEW == src->view) {
        tp = "LOGICAL";
    } else if (PMIX_COORD_PHYSICAL_VIEW == src->view) {
        tp = "PHYSICAL";
    } else {
        tp = "UNRECOGNIZED";
    }
    ret = asprintf(output, "%sData type: PMIX_COORD\tView: %s\tDims: %lu", prefx, tp,
                   (unsigned long) src->dims);
    if (prefx != prefix) {
        free(prefx);
    }

    if (0 > ret) {
        return PMIX_ERR_OUT_OF_RESOURCE;
    } else {
        return PMIX_SUCCESS;
    }
}

pmix_status_t pmix_bfrops_base_print_regattr(char **output, char *prefix, pmix_regattr_t *src,
                                             pmix_data_type_t type)
{
    char *prefx;
    int ret;

    PMIX_HIDE_UNUSED_PARAMS(type);

    /* deal with NULL prefix */
    if (NULL == prefix) {
        if (0 > asprintf(&prefx, " ")) {
            return PMIX_ERR_NOMEM;
        }
    } else {
        prefx = prefix;
    }

    ret = asprintf(output, "%sData type: PMIX_REGATTR\tName: %s\tString: %s", prefx,
                   (NULL == src->name) ? "NULL" : src->name,
                   (0 == strlen(src->string)) ? "NULL" : src->string);

    if (prefx != prefix) {
        free(prefx);
    }

    if (0 > ret) {
        return PMIX_ERR_OUT_OF_RESOURCE;
    } else {
        return PMIX_SUCCESS;
    }
}

pmix_status_t pmix_bfrops_base_print_regex(char **output, char *prefix, char *src,
                                           pmix_data_type_t type)
{
    char *prefx;
    int ret;

    PMIX_HIDE_UNUSED_PARAMS(type);

    /* deal with NULL prefix */
    if (NULL == prefix) {
        if (0 > asprintf(&prefx, " ")) {
            return PMIX_ERR_NOMEM;
        }
    } else {
        prefx = prefix;
    }

    ret = asprintf(output, "%sData type: PMIX_REGEX\tName: %s", prefx, src);

    if (prefx != prefix) {
        free(prefx);
    }

    if (0 > ret) {
        return PMIX_ERR_OUT_OF_RESOURCE;
    } else {
        return PMIX_SUCCESS;
    }
}

pmix_status_t pmix_bfrops_base_print_jobstate(char **output, char *prefix, pmix_job_state_t *src,
                                              pmix_data_type_t type)
{
    char *prefx;
    int ret;

    PMIX_HIDE_UNUSED_PARAMS(type);

    /* deal with NULL prefix */
    if (NULL == prefix) {
        if (0 > asprintf(&prefx, " ")) {
            return PMIX_ERR_NOMEM;
        }
    } else {
        prefx = prefix;
    }

    ret = asprintf(output, "%sData type: PMIX_JOB_STATE\tValue: %s", prefx,
                   PMIx_Job_state_string(*src));
    if (prefx != prefix) {
        free(prefx);
    }

    if (0 > ret) {
        return PMIX_ERR_OUT_OF_RESOURCE;
    } else {
        return PMIX_SUCCESS;
    }
}

pmix_status_t pmix_bfrops_base_print_linkstate(char **output, char *prefix, pmix_link_state_t *src,
                                               pmix_data_type_t type)
{
    char *prefx;
    int ret;

    PMIX_HIDE_UNUSED_PARAMS(type);

    /* deal with NULL prefix */
    if (NULL == prefix) {
        if (0 > asprintf(&prefx, " ")) {
            return PMIX_ERR_NOMEM;
        }
    } else {
        prefx = prefix;
    }

    ret = asprintf(output, "%sData type: PMIX_LINK_STATE\tValue: %s", prefx,
                   PMIx_Link_state_string(*src));
    if (prefx != prefix) {
        free(prefx);
    }

    if (0 > ret) {
        return PMIX_ERR_OUT_OF_RESOURCE;
    } else {
        return PMIX_SUCCESS;
    }
}

pmix_status_t pmix_bfrops_base_print_cpuset(char **output, char *prefix, pmix_cpuset_t *src,
                                            pmix_data_type_t type)
{
    char *prefx, *string;
    int ret;

    PMIX_HIDE_UNUSED_PARAMS(type);

    string = pmix_hwloc_print_cpuset(src);
    if (NULL == string) {
        return PMIX_ERR_NOT_SUPPORTED;
    }

    /* deal with NULL prefix */
    if (NULL == prefix) {
        if (0 > asprintf(&prefx, " ")) {
            free(string);
            return PMIX_ERR_NOMEM;
        }
    } else {
        prefx = prefix;
    }

    ret = asprintf(output, "%sData type: PMIX_CPUSET\tValue: %s", prefx, string);
    if (prefx != prefix) {
        free(prefx);
    }
    free(string);

    if (0 > ret) {
        return PMIX_ERR_OUT_OF_RESOURCE;
    } else {
        return PMIX_SUCCESS;
    }
}

pmix_status_t pmix_bfrops_base_print_geometry(char **output, char *prefix, pmix_geometry_t *src,
                                              pmix_data_type_t type)
{
    char *prefx, *tmp, *pfx2, **result = NULL;
    int ret;
    size_t n;

    PMIX_HIDE_UNUSED_PARAMS(type);

    /* deal with NULL prefix */
    if (NULL == prefix) {
        if (0 > asprintf(&prefx, " ")) {
            return PMIX_ERR_NOMEM;
        }
    } else {
        prefx = prefix;
    }
    if (0 > asprintf(&pfx2, "%s\t", prefx)) {
        if (prefx != prefix) {
            free(prefx);
        }
        return PMIX_ERR_NOMEM;
    }

    ret = asprintf(&tmp,
                   "%sData type: PMIX_GEOMETRY\tValue: Fabric: %" PRIsize_t " UUID: %s OSName: %s",
                   prefx, src->fabric, (NULL == src->uuid) ? "NULL" : src->uuid,
                   (NULL == src->osname) ? "NULL" : src->osname);
    if (0 > ret) {
        if (prefx != prefix) {
            free(prefx);
        }
        free(pfx2);
        return PMIX_ERR_OUT_OF_RESOURCE;
    }
    pmix_argv_append_nosize(&result, tmp);
    free(tmp);

    for (n = 0; n < src->ncoords; n++) {
        ret = pmix_bfrops_base_print_coord(&tmp, pfx2, &src->coordinates[n], PMIX_COORD);
        if (PMIX_SUCCESS != ret) {
            if (prefx != prefix) {
                free(prefx);
            }
            free(pfx2);
            if (NULL != result) {
                pmix_argv_free(result);
            }
            return ret;
        }
        pmix_argv_append_nosize(&result, tmp);
        free(tmp);
    }

    if (prefx != prefix) {
        free(prefx);
    }

    *output = pmix_argv_join(result, '\n');
    pmix_argv_free(result);

    return PMIX_SUCCESS;
}

pmix_status_t pmix_bfrops_base_print_devdist(char **output, char *prefix,
                                             pmix_device_distance_t *src, pmix_data_type_t type)
{
    char *prefx;
    int ret;

    PMIX_HIDE_UNUSED_PARAMS(type);

    /* deal with NULL prefix */
    if (NULL == prefix) {
        if (0 > asprintf(&prefx, " ")) {
            return PMIX_ERR_NOMEM;
        }
    } else {
        prefx = prefix;
    }

    ret = asprintf(
        output,
        "%sData type: PMIX_DEVICE_DIST\tValue: UUID: %s OSName: %s Type: %s Min: %u Max: %u", prefx,
        (NULL == src->uuid) ? "NULL" : src->uuid, (NULL == src->osname) ? "NULL" : src->osname,
        PMIx_Device_type_string(src->type), (unsigned) src->mindist, (unsigned) src->maxdist);
    if (prefx != prefix) {
        free(prefx);
    }

    if (0 > ret) {
        return PMIX_ERR_OUT_OF_RESOURCE;
    } else {
        return PMIX_SUCCESS;
    }
}

pmix_status_t pmix_bfrops_base_print_endpoint(char **output, char *prefix, pmix_endpoint_t *src,
                                              pmix_data_type_t type)
{
    char *prefx;
    int ret;

    PMIX_HIDE_UNUSED_PARAMS(type);

    /* deal with NULL prefix */
    if (NULL == prefix) {
        if (0 > asprintf(&prefx, " ")) {
            return PMIX_ERR_NOMEM;
        }
    } else {
        prefx = prefix;
    }

    ret = asprintf(output, "%sData type: PMIX_ENDPOINT\tValue: %s(%s) #bytes: %" PRIsize_t, prefx,
                   (NULL == src->uuid) ? "NULL" : src->uuid,
                   (NULL == src->osname) ? "NULL" : src->osname,
                   src->endpt.size);
    if (prefx != prefix) {
        free(prefx);
    }

    if (0 > ret) {
        return PMIX_ERR_OUT_OF_RESOURCE;
    } else {
        return PMIX_SUCCESS;
    }
}

pmix_status_t pmix_bfrops_base_print_topology(char **output, char *prefix, pmix_topology_t *src,
                                              pmix_data_type_t type)
{
    char *prefx, *string;
    int ret;

    PMIX_HIDE_UNUSED_PARAMS(type);

    string = pmix_hwloc_print_topology(src);
    if (NULL == string) {
        return PMIX_ERR_NOT_SUPPORTED;
    }

    /* deal with NULL prefix */
    if (NULL == prefix) {
        if (0 > asprintf(&prefx, " ")) {
            free(string);
            return PMIX_ERR_NOMEM;
        }
    } else {
        prefx = prefix;
    }

    ret = asprintf(output, "%sData type: PMIX_TOPO\tValue: %s", prefx, string);
    if (prefx != prefix) {
        free(prefx);
    }
    free(string);

    if (0 > ret) {
        return PMIX_ERR_OUT_OF_RESOURCE;
    } else {
        return PMIX_SUCCESS;
    }
}

pmix_status_t pmix_bfrops_base_print_devtype(char **output, char *prefix, pmix_device_type_t *src,
                                             pmix_data_type_t type)
{
    char *prefx;
    int ret;

    PMIX_HIDE_UNUSED_PARAMS(type);

    /* deal with NULL prefix */
    if (NULL == prefix) {
        if (0 > asprintf(&prefx, " ")) {
            return PMIX_ERR_NOMEM;
        }
    } else {
        prefx = prefix;
    }

    ret = asprintf(output, "%sData type: PMIX_DEVICE_TYPE\tValue: 0x%" PRIx64, prefx,
                   (uint64_t) src);
    if (prefx != prefix) {
        free(prefx);
    }

    if (0 > ret) {
        return PMIX_ERR_OUT_OF_RESOURCE;
    } else {
        return PMIX_SUCCESS;
    }
}

pmix_status_t pmix_bfrops_base_print_locality(char **output, char *prefix, pmix_locality_t *src,
                                              pmix_data_type_t type)
{
    char *prefx, **tmp = NULL, *str;
    int ret;

    PMIX_HIDE_UNUSED_PARAMS(type);

    /* deal with NULL prefix */
    if (NULL == prefix) {
        if (0 > asprintf(&prefx, " ")) {
            return PMIX_ERR_NOMEM;
        }
    } else {
        prefx = prefix;
    }

    if (PMIX_LOCALITY_UNKNOWN == *src) {
        str = strdup("UNKNOWN");
    } else if (PMIX_LOCALITY_NONLOCAL == *src) {
        str = strdup("NONLOCAL");
    } else {
        if (PMIX_LOCALITY_SHARE_HWTHREAD & *src) {
            pmix_argv_append_nosize(&tmp, "HWTHREAD");
        }
        if (PMIX_LOCALITY_SHARE_CORE & *src) {
            pmix_argv_append_nosize(&tmp, "CORE");
        }
        if (PMIX_LOCALITY_SHARE_L1CACHE & *src) {
            pmix_argv_append_nosize(&tmp, "L1");
        }
        if (PMIX_LOCALITY_SHARE_L2CACHE & *src) {
            pmix_argv_append_nosize(&tmp, "L2");
        }
        if (PMIX_LOCALITY_SHARE_L3CACHE & *src) {
            pmix_argv_append_nosize(&tmp, "L3");
        }
        if (PMIX_LOCALITY_SHARE_PACKAGE & *src) {
            pmix_argv_append_nosize(&tmp, "CORE");
        }
        if (PMIX_LOCALITY_SHARE_NUMA & *src) {
            pmix_argv_append_nosize(&tmp, "NUMA");
        }
        if (PMIX_LOCALITY_SHARE_NODE & *src) {
            pmix_argv_append_nosize(&tmp, "NODE");
        }
        str = pmix_argv_join(tmp, ':');
        pmix_argv_free(tmp);
    }

    ret = asprintf(output, "%sData type: PMIX_LOCALITY\tValue: %s", prefx, str);
    if (prefx != prefix) {
        free(prefx);
    }
    free(str);

    if (0 > ret) {
        return PMIX_ERR_OUT_OF_RESOURCE;
    } else {
        return PMIX_SUCCESS;
    }
}

pmix_status_t pmix_bfrops_base_print_nspace(char **output, char *prefix, pmix_nspace_t *src,
                                            pmix_data_type_t type)
{
    char *prefx;
    int ret;

    PMIX_HIDE_UNUSED_PARAMS(type);

    /* deal with NULL prefix */
    if (NULL == prefix) {
        if (0 > asprintf(&prefx, " ")) {
            return PMIX_ERR_NOMEM;
        }
    } else {
        prefx = prefix;
    }

    ret = asprintf(output, "%sData type: PMIX_PROC_NSPACE\tValue: %s", prefx, *src);
    if (prefx != prefix) {
        free(prefx);
    }

    if (0 > ret) {
        return PMIX_ERR_OUT_OF_RESOURCE;
    } else {
        return PMIX_SUCCESS;
    }
}

pmix_status_t pmix_bfrops_base_print_pstats(char **output, char *prefix, pmix_proc_stats_t *src,
                                            pmix_data_type_t type)
{
    char *prefx;

    PMIX_HIDE_UNUSED_PARAMS(type);

    /* deal with NULL prefix */
    if (NULL == prefix) {
        pmix_asprintf(&prefx, " ");
    } else {
        prefx = prefix;
    }

    /* if src is NULL, just print data type and return */
    if (NULL == src) {
        pmix_asprintf(output, "%sData type: PMIX_PROC_STATS\tValue: NULL pointer", prefx);
        if (prefx != prefix) {
            free(prefx);
        }
        return PMIX_SUCCESS;
    }
    pmix_asprintf(
        output,
        "%sPMIX_PROC_STATS SAMPLED AT: %ld.%06ld\n%snode: %s proc: %s"
        " pid: %d cmd: %s state: %c pri: %d #threads: %d Processor: %d\n"
        "%s\ttime: %ld.%06ld cpu: %5.2f  PSS: %8.2f  VMsize: %8.2f PeakVMSize: %8.2f RSS: %8.2f\n",
        prefx, (long) src->sample_time.tv_sec, (long) src->sample_time.tv_usec, prefx, src->node,
        PMIX_NAME_PRINT(&src->proc), src->pid, src->cmd, src->state, src->priority,
        src->num_threads, src->processor, prefx, (long) src->time.tv_sec, (long) src->time.tv_usec,
        src->percent_cpu, src->pss, src->vsize, src->peak_vsize, src->rss);
    if (prefx != prefix) {
        free(prefx);
    }

    return PMIX_SUCCESS;
}

pmix_status_t pmix_bfrops_base_print_dkstats(char **output, char *prefix, pmix_disk_stats_t *src,
                                             pmix_data_type_t type)
{
    char *prefx;

    PMIX_HIDE_UNUSED_PARAMS(type);

    /* deal with NULL prefix */
    if (NULL == prefix) {
        pmix_asprintf(&prefx, " ");
    } else {
        prefx = prefix;
    }

    /* if src is NULL, just print data type and return */
    if (NULL == src) {
        pmix_asprintf(output, "%sData type: PMIX_DISK_STATS\tValue: NULL pointer", prefx);
        if (prefx != prefix) {
            free(prefx);
        }
        return PMIX_SUCCESS;
    }
    pmix_asprintf(output,
                  "%sPMIX_DISK_STATS Disk: %s\n"
                  "%sNumReadsCompleted: %" PRIx64 " NumReadsMerged: %" PRIx64
                  " NumSectorsRead: %" PRIx64 " MillisecReading: %" PRIx64 "\n"
                  "%sNumWritesCompleted: %" PRIx64 " NumWritesMerged: %" PRIx64
                  " NumSectorsWrote: %" PRIx64 " MillisecWriting: %" PRIx64 "\n"
                  "%sNumIOsInProgress: %" PRIx64 " MillisecondsIO: %" PRIx64
                  " WeightedMillisecsIO: %" PRIx64 "\n",
                  prefx, src->disk, prefx, src->num_reads_completed, src->num_reads_merged,
                  src->num_sectors_read, src->milliseconds_reading, prefx,
                  src->num_writes_completed, src->num_writes_merged, src->num_sectors_written,
                  src->milliseconds_writing, prefx, src->num_ios_in_progress, src->milliseconds_io,
                  src->weighted_milliseconds_io);
    if (prefx != prefix) {
        free(prefx);
    }

    return PMIX_SUCCESS;
}

pmix_status_t pmix_bfrops_base_print_netstats(char **output, char *prefix, pmix_net_stats_t *src,
                                              pmix_data_type_t type)
{
    char *prefx;

    PMIX_HIDE_UNUSED_PARAMS(type);

    /* deal with NULL prefix */
    if (NULL == prefix) {
        pmix_asprintf(&prefx, " ");
    } else {
        prefx = prefix;
    }

    /* if src is NULL, just print data type and return */
    if (NULL == src) {
        pmix_asprintf(output, "%sData type: PMIX_NET_STATS\tValue: NULL pointer", prefx);
        if (prefx != prefix) {
            free(prefx);
        }
        return PMIX_SUCCESS;
    }
    pmix_asprintf(output,
                  "%sPMIX_NET_STATS Interface: %s\n"
                  "%sNumBytesRecvd: %" PRIx64 " NumPacketsRecv: %" PRIx64 " NumRecvErrors: %" PRIx64
                  "\n"
                  "%sNumBytesSent: %" PRIx64 " NumPacketsSent: %" PRIx64 " NumSendErrors: %" PRIx64
                  "\n",
                  prefx, src->net_interface, prefx, src->num_bytes_recvd, src->num_packets_recvd,
                  src->num_recv_errs, prefx, src->num_bytes_sent, src->num_packets_sent,
                  src->num_send_errs);
    if (prefx != prefix) {
        free(prefx);
    }

    return PMIX_SUCCESS;
}

pmix_status_t pmix_bfrops_base_print_ndstats(char **output, char *prefix, pmix_node_stats_t *src,
                                             pmix_data_type_t type)
{
    char *prefx;

    PMIX_HIDE_UNUSED_PARAMS(type);

    /* deal with NULL prefix */
    if (NULL == prefix) {
        pmix_asprintf(&prefx, " ");
    } else {
        prefx = prefix;
    }

    /* if src is NULL, just print data type and return */
    if (NULL == src) {
        pmix_asprintf(output, "%sData type: PMIX_NODE_STATS\tValue: NULL pointer", prefx);
        if (prefx != prefix) {
            free(prefx);
        }
        return PMIX_SUCCESS;
    }
    pmix_asprintf(output,
                  "%sPMIX_NODE_STATS SAMPLED AT: %ld.%06ld\tNode: %s\n%sTotal Mem: %5.2f "
                  "Free Mem: %5.2f Buffers: %5.2f Cached: %5.2f\n"
                  "%sSwapCached: %5.2f SwapTotal: %5.2f SwapFree: %5.2f Mapped: %5.2f\n"
                  "%s\tla: %5.2f\tla5: %5.2f\tla15: %5.2f\n",
                  prefx, (long) src->sample_time.tv_sec, (long) src->sample_time.tv_usec, src->node,
                  prefx, src->total_mem, src->free_mem, src->buffers, src->cached, prefx,
                  src->swap_cached, src->swap_total, src->swap_free, src->mapped, prefx, src->la,
                  src->la5, src->la15);
    if (prefx != prefix) {
        free(prefx);
    }

    return PMIX_SUCCESS;
}
pmix_status_t pmix_bfrops_base_print_dbuf(char **output, char *prefix, pmix_data_buffer_t *src,
                                          pmix_data_type_t type)
{
    char *prefx;

    PMIX_HIDE_UNUSED_PARAMS(type);

    /* deal with NULL prefix */
    if (NULL == prefix) {
        pmix_asprintf(&prefx, " ");
    } else {
        prefx = prefix;
    }

    /* if src is NULL, just print data type and return */
    if (NULL == src) {
        pmix_asprintf(output, "%sData type: PMIX_DATA_BUFFER\tValue: NULL pointer", prefx);
        if (prefx != prefix) {
            free(prefx);
        }
        return PMIX_SUCCESS;
    }
    pmix_asprintf(output, "%sPMIX_DATA_BUFFER NumBytesUsed: %" PRIsize_t "", prefx,
                  src->bytes_used);
    if (prefx != prefix) {
        free(prefx);
    }

    return PMIX_SUCCESS;
}

pmix_status_t pmix_bfrops_base_print_smed(char **output, char *prefix,
                                          pmix_storage_medium_t *src,
                                          pmix_data_type_t type)
{
    char *prefx, **tmp = NULL, *str;
    int ret;

    PMIX_HIDE_UNUSED_PARAMS(type);

    /* deal with NULL prefix */
    if (NULL == prefix) {
        if (0 > asprintf(&prefx, " ")) {
            return PMIX_ERR_NOMEM;
        }
    } else {
        prefx = prefix;
    }

    if (PMIX_STORAGE_MEDIUM_UNKNOWN & *src) {
        str = strdup("UNKNOWN");
    } else {
        if (PMIX_STORAGE_MEDIUM_TAPE & *src) {
            pmix_argv_append_nosize(&tmp, "TAPE");
        }
        if (PMIX_STORAGE_MEDIUM_HDD & *src) {
            pmix_argv_append_nosize(&tmp, "HDD");
        }
        if (PMIX_STORAGE_MEDIUM_SSD & *src) {
            pmix_argv_append_nosize(&tmp, "SSD");
        }
        if (PMIX_STORAGE_MEDIUM_NVME & *src) {
            pmix_argv_append_nosize(&tmp, "NVME");
        }
        if (PMIX_STORAGE_MEDIUM_PMEM & *src) {
            pmix_argv_append_nosize(&tmp, "PMEM");
        }
        if (PMIX_STORAGE_MEDIUM_RAM & *src) {
            pmix_argv_append_nosize(&tmp, "RAM");
        }
        str = pmix_argv_join(tmp, ':');
        pmix_argv_free(tmp);
    }

    ret = asprintf(output, "%sData type: PMIX_STOR_MEDIUM\tValue: %s", prefx, str);
    if (prefx != prefix) {
        free(prefx);
    }
    free(str);

    if (0 > ret) {
        return PMIX_ERR_OUT_OF_RESOURCE;
    } else {
        return PMIX_SUCCESS;
    }
}

pmix_status_t pmix_bfrops_base_print_sacc(char **output, char *prefix,
                                          pmix_storage_accessibility_t *src,
                                          pmix_data_type_t type)
{
    char *prefx, **tmp = NULL, *str;
    int ret;

    PMIX_HIDE_UNUSED_PARAMS(type);

    /* deal with NULL prefix */
    if (NULL == prefix) {
        if (0 > asprintf(&prefx, " ")) {
            return PMIX_ERR_NOMEM;
        }
    } else {
        prefx = prefix;
    }

    if (PMIX_STORAGE_ACCESSIBILITY_NODE & *src) {
        pmix_argv_append_nosize(&tmp, "NODE");
    }
    if (PMIX_STORAGE_ACCESSIBILITY_SESSION & *src) {
        pmix_argv_append_nosize(&tmp, "SESSION");
    }
    if (PMIX_STORAGE_ACCESSIBILITY_JOB & *src) {
        pmix_argv_append_nosize(&tmp, "JOB");
    }
    if (PMIX_STORAGE_ACCESSIBILITY_RACK & *src) {
        pmix_argv_append_nosize(&tmp, "RACK");
    }
    if (PMIX_STORAGE_ACCESSIBILITY_CLUSTER & *src) {
        pmix_argv_append_nosize(&tmp, "CLUSTER");
    }
    if (PMIX_STORAGE_ACCESSIBILITY_REMOTE & *src) {
        pmix_argv_append_nosize(&tmp, "REMOTE");
    }
    str = pmix_argv_join(tmp, ':');
    pmix_argv_free(tmp);

    ret = asprintf(output, "%sData type: PMIX_STOR_ACCESS\tValue: %s", prefx, str);
    if (prefx != prefix) {
        free(prefx);
    }
    free(str);

    if (0 > ret) {
        return PMIX_ERR_OUT_OF_RESOURCE;
    } else {
        return PMIX_SUCCESS;
    }
}

pmix_status_t pmix_bfrops_base_print_spers(char **output, char *prefix,
                                           pmix_storage_persistence_t *src,
                                           pmix_data_type_t type)
{
    char *prefx, **tmp = NULL, *str;
    int ret;

    PMIX_HIDE_UNUSED_PARAMS(type);

    /* deal with NULL prefix */
    if (NULL == prefix) {
        if (0 > asprintf(&prefx, " ")) {
            return PMIX_ERR_NOMEM;
        }
    } else {
        prefx = prefix;
    }

    if (PMIX_STORAGE_PERSISTENCE_TEMPORARY & *src) {
        pmix_argv_append_nosize(&tmp, "TEMPORARY");
    }
    if (PMIX_STORAGE_PERSISTENCE_NODE & *src) {
        pmix_argv_append_nosize(&tmp, "NODE");
    }
    if (PMIX_STORAGE_PERSISTENCE_SESSION & *src) {
        pmix_argv_append_nosize(&tmp, "SESSION");
    }
    if (PMIX_STORAGE_PERSISTENCE_JOB & *src) {
        pmix_argv_append_nosize(&tmp, "JOB");
    }
    if (PMIX_STORAGE_PERSISTENCE_SCRATCH & *src) {
        pmix_argv_append_nosize(&tmp, "SCRATCH");
    }
    if (PMIX_STORAGE_PERSISTENCE_PROJECT & *src) {
        pmix_argv_append_nosize(&tmp, "PROJECT");
    }
    if (PMIX_STORAGE_PERSISTENCE_ARCHIVE & *src) {
        pmix_argv_append_nosize(&tmp, "ARCHIVE");
    }

    str = pmix_argv_join(tmp, ':');
    pmix_argv_free(tmp);

    ret = asprintf(output, "%sData type: PMIX_STOR_PERSIST\tValue: %s", prefx, str);
    if (prefx != prefix) {
        free(prefx);
    }
    free(str);

    if (0 > ret) {
        return PMIX_ERR_OUT_OF_RESOURCE;
    } else {
        return PMIX_SUCCESS;
    }
}

pmix_status_t pmix_bfrops_base_print_satyp(char **output, char *prefix,
                                           pmix_storage_access_type_t *src,
                                           pmix_data_type_t type)
{
    char *prefx, **tmp = NULL, *str;
    int ret;

    PMIX_HIDE_UNUSED_PARAMS(type);

    /* deal with NULL prefix */
    if (NULL == prefix) {
        if (0 > asprintf(&prefx, " ")) {
            return PMIX_ERR_NOMEM;
        }
    } else {
        prefx = prefix;
    }

    if (PMIX_STORAGE_ACCESS_RD & *src) {
        pmix_argv_append_nosize(&tmp, "READ");
    }
    if (PMIX_STORAGE_ACCESS_WR & *src) {
        pmix_argv_append_nosize(&tmp, "WRITE");
    }
    str = pmix_argv_join(tmp, ':');
    pmix_argv_free(tmp);

    ret = asprintf(output, "%sData type: PMIX_STOR_ACCESS_TYPE\tValue: %s", prefx, str);
    if (prefx != prefix) {
        free(prefx);
    }
    free(str);

    if (0 > ret) {
        return PMIX_ERR_OUT_OF_RESOURCE;
    } else {
        return PMIX_SUCCESS;
    }
}
