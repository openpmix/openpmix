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
 *
 * Copyright (c) 2021      Nanook Consulting.  All rights reserved.
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

#include "src/hwloc/pmix_hwloc.h"
#include "src/include/pmix_globals.h"
#include "src/mca/bfrops/base/base.h"
#include "src/util/error.h"
#include "src/util/name_fns.h"
#include "src/util/printf.h"

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

/*
 * PMIX_VALUE
 */
int pmix_bfrops_base_print_value(char **output, char *prefix, pmix_value_t *src,
                                 pmix_data_type_t type)
{
    char *prefx;
    int rc;
    pmix_regattr_t *r;
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
        rc = asprintf(output, "%sData type: PMIX_VALUE\tValue: NULL pointer", prefx);
        if (prefx != prefix) {
            free(prefx);
        }
        if (0 > rc) {
            return PMIX_ERR_OUT_OF_RESOURCE;
        } else {
            return PMIX_SUCCESS;
        }
    }

    switch (src->type) {
    case PMIX_UNDEF:
        rc = asprintf(output, "%sPMIX_VALUE: Data type: PMIX_UNDEF", prefx);
        break;
    case PMIX_BYTE:
        rc = asprintf(output, "%sPMIX_VALUE: Data type: PMIX_BYTE\tValue: %x", prefx,
                      src->data.byte);
        break;
    case PMIX_STRING:
        rc = asprintf(output, "%sPMIX_VALUE: Data type: PMIX_STRING\tValue: %s", prefx,
                      src->data.string);
        break;
    case PMIX_SIZE:
        rc = asprintf(output, "%sPMIX_VALUE: Data type: PMIX_SIZE\tValue: %lu", prefx,
                      (unsigned long) src->data.size);
        break;
    case PMIX_PID:
        rc = asprintf(output, "%sPMIX_VALUE: Data type: PMIX_PID\tValue: %lu", prefx,
                      (unsigned long) src->data.pid);
        break;
    case PMIX_INT:
        rc = asprintf(output, "%sPMIX_VALUE: Data type: PMIX_INT\tValue: %d", prefx,
                      src->data.integer);
        break;
    case PMIX_INT8:
        rc = asprintf(output, "%sPMIX_VALUE: Data type: PMIX_INT8\tValue: %d", prefx,
                      (int) src->data.int8);
        break;
    case PMIX_INT16:
        rc = asprintf(output, "%sPMIX_VALUE: Data type: PMIX_INT16\tValue: %d", prefx,
                      (int) src->data.int16);
        break;
    case PMIX_INT32:
        rc = asprintf(output, "%sPMIX_VALUE: Data type: PMIX_INT32\tValue: %d", prefx,
                      src->data.int32);
        break;
    case PMIX_INT64:
        rc = asprintf(output, "%sPMIX_VALUE: Data type: PMIX_INT64\tValue: %ld", prefx,
                      (long) src->data.int64);
        break;
    case PMIX_UINT:
        rc = asprintf(output, "%sPMIX_VALUE: Data type: PMIX_UINT\tValue: %u", prefx,
                      src->data.uint);
        break;
    case PMIX_UINT8:
        rc = asprintf(output, "%sPMIX_VALUE: Data type: PMIX_UINT8\tValue: %u", prefx,
                      (unsigned int) src->data.uint8);
        break;
    case PMIX_UINT16:
        rc = asprintf(output, "%sPMIX_VALUE: Data type: PMIX_UINT16\tValue: %u", prefx,
                      (unsigned int) src->data.uint16);
        break;
    case PMIX_UINT32:
        rc = asprintf(output, "%sPMIX_VALUE: Data type: PMIX_UINT32\tValue: %u", prefx,
                      src->data.uint32);
        break;
    case PMIX_UINT64:
        rc = asprintf(output, "%sPMIX_VALUE: Data type: PMIX_UINT64\tValue: %lu", prefx,
                      (unsigned long) src->data.uint64);
        break;
    case PMIX_FLOAT:
        rc = asprintf(output, "%sPMIX_VALUE: Data type: PMIX_FLOAT\tValue: %f", prefx,
                      src->data.fval);
        break;
    case PMIX_DOUBLE:
        rc = asprintf(output, "%sPMIX_VALUE: Data type: PMIX_DOUBLE\tValue: %f", prefx,
                      src->data.dval);
        break;
    case PMIX_TIMEVAL:
        rc = asprintf(output, "%sPMIX_VALUE: Data type: PMIX_TIMEVAL\tValue: %ld.%06ld", prefx,
                      (long) src->data.tv.tv_sec, (long) src->data.tv.tv_usec);
        break;
    case PMIX_TIME:
        rc = asprintf(output, "%sPMIX_VALUE: Data type: PMIX_TIME\tValue: %ld", prefx,
                      (long) src->data.time);
        break;
    case PMIX_STATUS:
        rc = asprintf(output, "%sPMIX_VALUE: Data type: PMIX_STATUS\tValue: %s", prefx,
                      PMIx_Error_string(src->data.status));
        break;
    case PMIX_PROC:
        if (NULL == src->data.proc) {
            rc = asprintf(output, "%sPMIX_VALUE: Data type: PMIX_PROC\tNULL", prefx);
        } else {
            rc = asprintf(output, "%sPMIX_VALUE: Data type: PMIX_PROC\t%s:%lu", prefx,
                          src->data.proc->nspace, (unsigned long) src->data.proc->rank);
        }
        break;
    case PMIX_BYTE_OBJECT:
        rc = asprintf(output, "%sPMIX_VALUE: Data type: BYTE_OBJECT\tSIZE: %ld", prefx,
                      (long) src->data.bo.size);
        break;
    case PMIX_PERSIST:
        rc = asprintf(output, "%sPMIX_VALUE: Data type: PMIX_PERSIST\tValue: %d", prefx,
                      (int) src->data.persist);
        break;
    case PMIX_SCOPE:
        rc = asprintf(output, "%sPMIX_VALUE: Data type: PMIX_SCOPE\tValue: %d", prefx,
                      (int) src->data.scope);
        break;
    case PMIX_DATA_RANGE:
        rc = asprintf(output, "%sPMIX_VALUE: Data type: PMIX_DATA_RANGE\tValue: %d", prefx,
                      (int) src->data.range);
        break;
    case PMIX_PROC_STATE:
        rc = asprintf(output, "%sPMIX_VALUE: Data type: PMIX_STATE\tValue: %d", prefx,
                      (int) src->data.state);
        break;
    case PMIX_PROC_INFO:
        rc = asprintf(output, "%sPMIX_VALUE: Data type: PMIX_PROC_INFO\tValue: %s:%lu", prefx,
                      src->data.proc->nspace, (unsigned long) src->data.proc->rank);
        break;
    case PMIX_DATA_ARRAY:
        rc = asprintf(output, "%sPMIX_VALUE: Data type: DATA_ARRAY\tARRAY SIZE: %ld", prefx,
                      (long) src->data.darray->size);
        break;
    case PMIX_REGATTR:
        r = (pmix_regattr_t *) src->data.ptr;
        rc = asprintf(output, "%sPMIX_VALUE: Data type: PMIX_REGATTR\tName: %s\tString: %s", prefx,
                      (NULL == r->name) ? "NULL" : r->name,
                      (0 == strlen(r->string)) ? "NULL" : r->string);
        break;
    case PMIX_ALLOC_DIRECTIVE:
        rc = pmix_bfrops_base_print_alloc_directive(output, prefx, &src->data.adir,
                                                    PMIX_ALLOC_DIRECTIVE);
        break;
    case PMIX_ENVAR:
        rc = asprintf(output,
                      "%sPMIX_VALUE: Data type: PMIX_ENVAR\tName: %s\tValue: %s\tSeparator: %c",
                      prefx, (NULL == src->data.envar.envar) ? "NULL" : src->data.envar.envar,
                      (NULL == src->data.envar.value) ? "NULL" : src->data.envar.value,
                      src->data.envar.separator);
        break;
    case PMIX_COORD:
        if (PMIX_COORD_VIEW_UNDEF == src->data.coord->view) {
            tp = "UNDEF";
        } else if (PMIX_COORD_LOGICAL_VIEW == src->data.coord->view) {
            tp = "LOGICAL";
        } else if (PMIX_COORD_PHYSICAL_VIEW == src->data.coord->view) {
            tp = "PHYSICAL";
        } else {
            tp = "UNRECOGNIZED";
        }
        rc = asprintf(output, "%sPMIX_VALUE: Data type: PMIX_COORD\tView: %s\tDims: %lu", prefx, tp,
                      (unsigned long) src->data.coord->dims);
        break;
    case PMIX_LINK_STATE:
        rc = pmix_bfrops_base_print_linkstate(output, prefx, &src->data.linkstate, PMIX_LINK_STATE);
        break;
    case PMIX_JOB_STATE:
        rc = pmix_bfrops_base_print_jobstate(output, prefx, &src->data.jstate, PMIX_JOB_STATE);
        break;
    case PMIX_TOPO:
        rc = pmix_bfrops_base_print_topology(output, prefx, src->data.topo, PMIX_TOPO);
        break;
    case PMIX_PROC_CPUSET:
        rc = pmix_bfrops_base_print_cpuset(output, prefx, src->data.cpuset, PMIX_PROC_CPUSET);
        break;
    case PMIX_LOCTYPE:
        rc = pmix_bfrops_base_print_locality(output, prefx, &src->data.locality, PMIX_LOCTYPE);
        break;
    case PMIX_GEOMETRY:
        rc = pmix_bfrops_base_print_geometry(output, prefx, src->data.geometry, PMIX_GEOMETRY);
        break;
    case PMIX_DEVTYPE:
        rc = pmix_bfrops_base_print_devtype(output, prefx, &src->data.devtype, PMIX_DEVTYPE);
        break;
    case PMIX_DEVICE_DIST:
        rc = pmix_bfrops_base_print_devdist(output, prefx, src->data.devdist, PMIX_DEVICE_DIST);
        break;
    case PMIX_ENDPOINT:
        rc = pmix_bfrops_base_print_endpoint(output, prefx, src->data.endpoint, PMIX_ENDPOINT);
        break;
    case PMIX_STOR_MEDIUM:
        rc = pmix_bfrops_base_print_smed(output, prefx, &src->data.uint64, PMIX_STOR_MEDIUM);
        break;
    case PMIX_STOR_ACCESS:
        rc = pmix_bfrops_base_print_sacc(output, prefx, &src->data.uint64, PMIX_STOR_ACCESS);
        break;
    case PMIX_STOR_PERSIST:
        rc = pmix_bfrops_base_print_spers(output, prefx, &src->data.uint64, PMIX_STOR_PERSIST);
        break;
    case PMIX_STOR_ACCESS_TYPE:
        rc = pmix_bfrops_base_print_satyp(output, prefx, &src->data.uint16, PMIX_STOR_ACCESS_TYPE);
        break;
    default:
        rc = asprintf(output, "%sPMIX_VALUE: Data type: UNKNOWN\tValue: UNPRINTABLE", prefx);
        break;
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
    int ret;

    PMIX_HIDE_UNUSED_PARAMS(type);

    pmix_bfrops_base_print_value(&tmp, NULL, &src->value, PMIX_VALUE);
    pmix_bfrops_base_print_info_directives(&tmp2, NULL, &src->flags, PMIX_INFO_DIRECTIVES);
    ret = asprintf(output, "%sKEY: %s\n%s\t%s\n%s\t%s", prefix, src->key, prefix, tmp2, prefix,
                   tmp);
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
    ret = asprintf(output, "%s  %s  KEY: %s %s", prefix, tmp1, src->key,
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
    char *prefx;
    int rc;

    PMIX_HIDE_UNUSED_PARAMS(type);

    /* deal with NULL prefix */
    if (NULL == prefix) {
        if (0 > asprintf(&prefx, " ")) {
            return PMIX_ERR_NOMEM;
        }
    } else {
        prefx = prefix;
    }

    switch (src->rank) {
    case PMIX_RANK_UNDEF:
        rc = asprintf(output, "%sPROC: %s:PMIX_RANK_UNDEF", prefx, src->nspace);
        break;
    case PMIX_RANK_WILDCARD:
        rc = asprintf(output, "%sPROC: %s:PMIX_RANK_WILDCARD", prefx, src->nspace);
        break;
    case PMIX_RANK_LOCAL_NODE:
        rc = asprintf(output, "%sPROC: %s:PMIX_RANK_LOCAL_NODE", prefx, src->nspace);
        break;
    default:
        rc = asprintf(output, "%sPROC: %s:%lu", prefx, src->nspace, (unsigned long) (src->rank));
    }
    if (prefx != prefix) {
        free(prefx);
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
    char *prefx;

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
        if (0 > asprintf(output, "%sData type: PMIX_PERSIST\tValue: NULL pointer", prefx)) {
            return PMIX_ERR_NOMEM;
        }
        if (prefx != prefix) {
            free(prefx);
        }
        return PMIX_SUCCESS;
    }

    if (0 > asprintf(output, "%sData type: PMIX_PERSIST\tValue: %ld", prefx, (long) *src)) {
        return PMIX_ERR_NOMEM;
    }
    if (prefx != prefix) {
        free(prefx);
    }

    return PMIX_SUCCESS;
}

pmix_status_t pmix_bfrops_base_print_scope(char **output, char *prefix, pmix_scope_t *src,
                                           pmix_data_type_t type)
{
    char *prefx;

    PMIX_HIDE_UNUSED_PARAMS(type);

    /* deal with NULL prefix */
    if (NULL == prefix) {
        if (0 > asprintf(&prefx, " ")) {
            return PMIX_ERR_NOMEM;
        }
    } else {
        prefx = prefix;
    }

    if (0
        > asprintf(output, "%sData type: PMIX_SCOPE\tValue: %s", prefx, PMIx_Scope_string(*src))) {
        return PMIX_ERR_NOMEM;
    }
    if (prefx != prefix) {
        free(prefx);
    }

    return PMIX_SUCCESS;
}

pmix_status_t pmix_bfrops_base_print_range(char **output, char *prefix, pmix_data_range_t *src,
                                           pmix_data_type_t type)
{
    char *prefx;

    PMIX_HIDE_UNUSED_PARAMS(type);

    /* deal with NULL prefix */
    if (NULL == prefix) {
        if (0 > asprintf(&prefx, " ")) {
            return PMIX_ERR_NOMEM;
        }
    } else {
        prefx = prefix;
    }

    if (0 > asprintf(output, "%sData type: PMIX_DATA_RANGE\tValue: %s", prefx,
                     PMIx_Data_range_string(*src))) {
        return PMIX_ERR_NOMEM;
    }
    if (prefx != prefix) {
        free(prefx);
    }

    return PMIX_SUCCESS;
}
pmix_status_t pmix_bfrops_base_print_cmd(char **output, char *prefix, pmix_cmd_t *src,
                                         pmix_data_type_t type)
{
    char *prefx;

    PMIX_HIDE_UNUSED_PARAMS(type);

    /* deal with NULL prefix */
    if (NULL == prefix) {
        if (0 > asprintf(&prefx, " ")) {
            return PMIX_ERR_NOMEM;
        }
    } else {
        prefx = prefix;
    }

    if (0 > asprintf(output, "%sData type: PMIX_COMMAND\tValue: %s", prefx,
                     pmix_command_string(*src))) {
        return PMIX_ERR_NOMEM;
    }
    if (prefx != prefix) {
        free(prefx);
    }

    return PMIX_SUCCESS;
}

pmix_status_t pmix_bfrops_base_print_info_directives(char **output, char *prefix,
                                                     pmix_info_directives_t *src,
                                                     pmix_data_type_t type)
{
    char *prefx;

    PMIX_HIDE_UNUSED_PARAMS(type);

    /* deal with NULL prefix */
    if (NULL == prefix) {
        if (0 > asprintf(&prefx, " ")) {
            return PMIX_ERR_NOMEM;
        }
    } else {
        prefx = prefix;
    }

    if (0 > asprintf(output, "%sData type: PMIX_INFO_DIRECTIVES\tValue: %s", prefx,
                     PMIx_Info_directives_string(*src))) {
        return PMIX_ERR_NOMEM;
    }
    if (prefx != prefix) {
        free(prefx);
    }

    return PMIX_SUCCESS;
}

pmix_status_t pmix_bfrops_base_print_datatype(char **output, char *prefix, pmix_data_type_t *src,
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
        ret = asprintf(output, "%sData type: PMIX_DATA_TYPE\tValue: NULL pointer", prefx);
        if (prefx != prefix) {
            free(prefx);
        }
        if (0 > ret) {
            return PMIX_ERR_OUT_OF_RESOURCE;
        } else {
            return PMIX_SUCCESS;
        }
    }

    ret = asprintf(output, "%sData type: PMIX_DATA_TYPE\tValue: %s", prefx,
                   PMIx_Data_type_string(*src));
    if (prefx != prefix) {
        free(prefx);
    }

    if (0 > ret) {
        return PMIX_ERR_OUT_OF_RESOURCE;
    } else {
        return PMIX_SUCCESS;
    }
}

int pmix_bfrops_base_print_bo(char **output, char *prefix, pmix_byte_object_t *src,
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
        ret = asprintf(output, "%sData type: %s\tValue: NULL pointer", prefx,
                       (PMIX_COMPRESSED_BYTE_OBJECT == type) ? "PMIX_COMPRESSED_BYTE_OBJECT"
                                                             : "PMIX_BYTE_OBJECT");
        if (prefx != prefix) {
            free(prefx);
        }
        if (0 > ret) {
            return PMIX_ERR_OUT_OF_RESOURCE;
        } else {
            return PMIX_SUCCESS;
        }
    }

    ret = asprintf(output, "%sData type: %s\tSize: %ld", prefx,
                   (PMIX_COMPRESSED_BYTE_OBJECT == type) ? "PMIX_COMPRESSED_BYTE_OBJECT"
                                                         : "PMIX_BYTE_OBJECT",
                   (long) src->size);
    if (prefx != prefix) {
        free(prefx);
    }

    if (0 > ret) {
        return PMIX_ERR_OUT_OF_RESOURCE;
    } else {
        return PMIX_SUCCESS;
    }
}

int pmix_bfrops_base_print_ptr(char **output, char *prefix, void *src, pmix_data_type_t type)
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

    ret = asprintf(output, "%sData type: PMIX_POINTER\tAddress: %p", prefx, src);
    if (prefx != prefix) {
        free(prefx);
    }

    if (0 > ret) {
        return PMIX_ERR_OUT_OF_RESOURCE;
    } else {
        return PMIX_SUCCESS;
    }
}

pmix_status_t pmix_bfrops_base_print_pstate(char **output, char *prefix, pmix_proc_state_t *src,
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

    ret = asprintf(output, "%sData type: PMIX_PROC_STATE\tValue: %s", prefx,
                   PMIx_Proc_state_string(*src));
    if (prefx != prefix) {
        free(prefx);
    }

    if (0 > ret) {
        return PMIX_ERR_OUT_OF_RESOURCE;
    } else {
        return PMIX_SUCCESS;
    }
}

pmix_status_t pmix_bfrops_base_print_pinfo(char **output, char *prefix, pmix_proc_info_t *src,
                                           pmix_data_type_t type)
{
    char *prefx;
    pmix_status_t rc = PMIX_SUCCESS;
    char *p2, *tmp;

    PMIX_HIDE_UNUSED_PARAMS(type);

    /* deal with NULL prefix */
    if (NULL == prefix) {
        if (0 > asprintf(&prefx, " ")) {
            return PMIX_ERR_NOMEM;
        }
    } else {
        prefx = prefix;
    }

    if (0 > asprintf(&p2, "%s\t", prefx)) {
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
                     prefx, tmp, p2, src->hostname, src->executable_name, p2,
                     (unsigned long) src->pid, src->exit_code,
                     PMIx_Proc_state_string(src->state))) {
        free(p2);
        rc = PMIX_ERR_NOMEM;
    }

done:
    if (prefx != prefix) {
        free(prefx);
    }

    return rc;
}

pmix_status_t pmix_bfrops_base_print_darray(char **output, char *prefix, pmix_data_array_t *src,
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

    ret = asprintf(output, "%sData type: PMIX_DATA_ARRAY\tSize: %lu", prefx,
                   (unsigned long) src->size);
    if (prefx != prefix) {
        free(prefx);
    }

    if (0 > ret) {
        return PMIX_ERR_OUT_OF_RESOURCE;
    } else {
        return PMIX_SUCCESS;
    }
}

pmix_status_t pmix_bfrops_base_print_query(char **output, char *prefix, pmix_query_t *src,
                                           pmix_data_type_t type)
{
    char *prefx, *p2;
    pmix_status_t rc = PMIX_SUCCESS;
    char *tmp, *t2, *t3;
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

    if (0 > asprintf(&p2, "%s\t", prefx)) {
        rc = PMIX_ERR_NOMEM;
        goto done;
    }

    if (0 > asprintf(&tmp, "%sData type: PMIX_QUERY\tValue:", prefx)) {
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
    if (prefx != prefix) {
        free(prefx);
    }

    return rc;
}

pmix_status_t pmix_bfrops_base_print_rank(char **output, char *prefix, pmix_rank_t *src,
                                          pmix_data_type_t type)
{
    char *prefx;
    int rc;

    PMIX_HIDE_UNUSED_PARAMS(type);

    /* deal with NULL prefix */
    if (NULL == prefix) {
        if (0 > asprintf(&prefx, " ")) {
            return PMIX_ERR_NOMEM;
        }
    } else {
        prefx = prefix;
    }

    switch (*src) {
    case PMIX_RANK_UNDEF:
        rc = asprintf(output, "%sData type: PMIX_PROC_RANK\tValue: PMIX_RANK_UNDEF", prefx);
        break;
    case PMIX_RANK_WILDCARD:
        rc = asprintf(output, "%sData type: PMIX_PROC_RANK\tValue: PMIX_RANK_WILDCARD", prefx);
        break;
    case PMIX_RANK_LOCAL_NODE:
        rc = asprintf(output, "%sData type: PMIX_PROC_RANK\tValue: PMIX_RANK_LOCAL_NODE", prefx);
        break;
    default:
        rc = asprintf(output, "%sData type: PMIX_PROC_RANK\tValue: %lu", prefx,
                      (unsigned long) (*src));
    }
    if (prefx != prefix) {
        free(prefx);
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
