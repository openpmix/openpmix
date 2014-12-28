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
 * Copyright (c) 2014      Intel, Inc. All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "pmix_config.h"
#include "pmix_stdint.h"

#include <stdio.h>
#ifdef HAVE_TIME_H
#include <time.h>
#endif

#include "src/buffer_ops/internal.h"

int pmix_bfrop_print(char **output, char *prefix, void *src, pmix_data_type_t type)
{
    pmix_bfrop_type_info_t *info;

    /* check for error */
    if (NULL == output) {
        return PMIX_ERR_BAD_PARAM;
    }

    /* Lookup the print function for this type and call it */

    if(NULL == (info = (pmix_bfrop_type_info_t*)pmix_pointer_array_get_item(&pmix_bfrop_types, type))) {
        return PMIX_ERR_UNKNOWN_DATA_TYPE;
    }

    return info->odti_print_fn(output, prefix, src, type);
}

/*
 * STANDARD PRINT FUNCTIONS FOR SYSTEM TYPES
 */
int pmix_bfrop_print_byte(char **output, char *prefix, uint8_t *src, pmix_data_type_t type)
{
    char *prefx;

    /* deal with NULL prefix */
    if (NULL == prefix) asprintf(&prefx, " ");
    else prefx = prefix;

    /* if src is NULL, just print data type and return */
    if (NULL == src) {
        asprintf(output, "%sData type: PMIX_BYTE\tValue: NULL pointer", prefx);
        if (prefx != prefix) {
            free(prefx);
        }
        return PMIX_SUCCESS;
    }

    asprintf(output, "%sData type: PMIX_BYTE\tValue: %x", prefix, *src);
    if (prefx != prefix) {
        free(prefx);
    }

    return PMIX_SUCCESS;
}

int pmix_bfrop_print_string(char **output, char *prefix, char *src, pmix_data_type_t type)
{
    char *prefx;

    /* deal with NULL prefix */
    if (NULL == prefix) asprintf(&prefx, " ");
    else prefx = prefix;

    /* if src is NULL, just print data type and return */
    if (NULL == src) {
        asprintf(output, "%sData type: PMIX_STRING\tValue: NULL pointer", prefx);
        if (prefx != prefix) {
            free(prefx);
        }
        return PMIX_SUCCESS;
    }

    asprintf(output, "%sData type: PMIX_STRING\tValue: %s", prefx, src);
    if (prefx != prefix) {
        free(prefx);
    }

    return PMIX_SUCCESS;
}

int pmix_bfrop_print_size(char **output, char *prefix, size_t *src, pmix_data_type_t type)
{
    char *prefx;

    /* deal with NULL prefix */
    if (NULL == prefix) asprintf(&prefx, " ");
    else prefx = prefix;

    /* if src is NULL, just print data type and return */
    if (NULL == src) {
        asprintf(output, "%sData type: PMIX_SIZE\tValue: NULL pointer", prefx);
        if (prefx != prefix) {
            free(prefx);
        }
        return PMIX_SUCCESS;
    }

    asprintf(output, "%sData type: PMIX_SIZE\tValue: %lu", prefx, (unsigned long) *src);
    if (prefx != prefix) {
        free(prefx);
    }

    return PMIX_SUCCESS;
}

int pmix_bfrop_print_pid(char **output, char *prefix, pid_t *src, pmix_data_type_t type)
{
    char *prefx;

    /* deal with NULL prefix */
    if (NULL == prefix) asprintf(&prefx, " ");
    else prefx = prefix;

    /* if src is NULL, just print data type and return */
    if (NULL == src) {
        asprintf(output, "%sData type: PMIX_PID\tValue: NULL pointer", prefx);
        if (prefx != prefix) {
            free(prefx);
        }
        return PMIX_SUCCESS;
    }

    asprintf(output, "%sData type: PMIX_PID\tValue: %lu", prefx, (unsigned long) *src);
    if (prefx != prefix) {
        free(prefx);
    }
    return PMIX_SUCCESS;
}

int pmix_bfrop_print_bool(char **output, char *prefix, bool *src, pmix_data_type_t type)
{
    char *prefx;

    /* deal with NULL prefix */
    if (NULL == prefix) asprintf(&prefx, " ");
    else prefx = prefix;

    /* if src is NULL, just print data type and return */
    if (NULL == src) {
        asprintf(output, "%sData type: PMIX_BOOL\tValue: NULL pointer", prefx);
        if (prefx != prefix) {
            free(prefx);
        }
        return PMIX_SUCCESS;
    }

    asprintf(output, "%sData type: PMIX_BOOL\tValue: %s", prefx, *src ? "TRUE" : "FALSE");
    if (prefx != prefix) {
        free(prefx);
    }

    return PMIX_SUCCESS;
}

int pmix_bfrop_print_int(char **output, char *prefix, int *src, pmix_data_type_t type)
{
    char *prefx;

    /* deal with NULL prefix */
    if (NULL == prefix) asprintf(&prefx, " ");
    else prefx = prefix;

    /* if src is NULL, just print data type and return */
    if (NULL == src) {
        asprintf(output, "%sData type: PMIX_INT\tValue: NULL pointer", prefx);
        if (prefx != prefix) {
            free(prefx);
        }
        return PMIX_SUCCESS;
    }

    asprintf(output, "%sData type: PMIX_INT\tValue: %ld", prefx, (long) *src);
    if (prefx != prefix) {
        free(prefx);
    }

    return PMIX_SUCCESS;
}

int pmix_bfrop_print_uint(char **output, char *prefix, int *src, pmix_data_type_t type)
{
    char *prefx;

    /* deal with NULL prefix */
    if (NULL == prefix) asprintf(&prefx, " ");
    else prefx = prefix;

    /* if src is NULL, just print data type and return */
    if (NULL == src) {
        asprintf(output, "%sData type: PMIX_UINT\tValue: NULL pointer", prefx);
        if (prefx != prefix) {
            free(prefx);
        }
        return PMIX_SUCCESS;
    }

    asprintf(output, "%sData type: PMIX_UINT\tValue: %lu", prefx, (unsigned long) *src);
    if (prefx != prefix) {
        free(prefx);
    }

    return PMIX_SUCCESS;
}

int pmix_bfrop_print_uint8(char **output, char *prefix, uint8_t *src, pmix_data_type_t type)
{
    char *prefx;

    /* deal with NULL prefix */
    if (NULL == prefix) asprintf(&prefx, " ");
    else prefx = prefix;

    /* if src is NULL, just print data type and return */
    if (NULL == src) {
        asprintf(output, "%sData type: PMIX_UINT8\tValue: NULL pointer", prefx);
        if (prefx != prefix) {
            free(prefx);
        }
        return PMIX_SUCCESS;
    }

    asprintf(output, "%sData type: PMIX_UINT8\tValue: %u", prefx, (unsigned int) *src);
    if (prefx != prefix) {
        free(prefx);
    }

    return PMIX_SUCCESS;
}

int pmix_bfrop_print_uint16(char **output, char *prefix, uint16_t *src, pmix_data_type_t type)
{
    char *prefx;

    /* deal with NULL prefix */
    if (NULL == prefix) asprintf(&prefx, " ");
    else prefx = prefix;

    /* if src is NULL, just print data type and return */
    if (NULL == src) {
        asprintf(output, "%sData type: PMIX_UINT16\tValue: NULL pointer", prefx);
        if (prefx != prefix) {
            free(prefx);
        }
        return PMIX_SUCCESS;
    }

    asprintf(output, "%sData type: PMIX_UINT16\tValue: %u", prefx, (unsigned int) *src);
    if (prefx != prefix) {
        free(prefx);
    }

    return PMIX_SUCCESS;
}

int pmix_bfrop_print_uint32(char **output, char *prefix,
                            uint32_t *src, pmix_data_type_t type)
{
    char *prefx;

    /* deal with NULL prefix */
    if (NULL == prefix) asprintf(&prefx, " ");
    else prefx = prefix;

    /* if src is NULL, just print data type and return */
    if (NULL == src) {
        asprintf(output, "%sData type: PMIX_UINT32\tValue: NULL pointer", prefx);
        if (prefx != prefix) {
            free(prefx);
        }
        return PMIX_SUCCESS;
    }

    asprintf(output, "%sData type: PMIX_UINT32\tValue: %u", prefx, (unsigned int) *src);
    if (prefx != prefix) {
        free(prefx);
    }

    return PMIX_SUCCESS;
}

int pmix_bfrop_print_int8(char **output, char *prefix,
                          int8_t *src, pmix_data_type_t type)
{
    char *prefx;

    /* deal with NULL prefix */
    if (NULL == prefix) asprintf(&prefx, " ");
    else prefx = prefix;

    /* if src is NULL, just print data type and return */
    if (NULL == src) {
        asprintf(output, "%sData type: PMIX_INT8\tValue: NULL pointer", prefx);
        if (prefx != prefix) {
            free(prefx);
        }
        return PMIX_SUCCESS;
    }

    asprintf(output, "%sData type: PMIX_INT8\tValue: %d", prefx, (int) *src);
    if (prefx != prefix) {
        free(prefx);
    }

    return PMIX_SUCCESS;
}

int pmix_bfrop_print_int16(char **output, char *prefix,
                           int16_t *src, pmix_data_type_t type)
{
    char *prefx;

    /* deal with NULL prefix */
    if (NULL == prefix) asprintf(&prefx, " ");
    else prefx = prefix;

    /* if src is NULL, just print data type and return */
    if (NULL == src) {
        asprintf(output, "%sData type: PMIX_INT16\tValue: NULL pointer", prefx);
        if (prefx != prefix) {
            free(prefx);
        }
        return PMIX_SUCCESS;
    }

    asprintf(output, "%sData type: PMIX_INT16\tValue: %d", prefx, (int) *src);
    if (prefx != prefix) {
        free(prefx);
    }

    return PMIX_SUCCESS;
}

int pmix_bfrop_print_int32(char **output, char *prefix, int32_t *src, pmix_data_type_t type)
{
    char *prefx;

    /* deal with NULL prefix */
    if (NULL == prefix) asprintf(&prefx, " ");
    else prefx = prefix;

    /* if src is NULL, just print data type and return */
    if (NULL == src) {
        asprintf(output, "%sData type: PMIX_INT32\tValue: NULL pointer", prefx);
        if (prefx != prefix) {
            free(prefx);
        }
        return PMIX_SUCCESS;
    }

    asprintf(output, "%sData type: PMIX_INT32\tValue: %d", prefx, (int) *src);
    if (prefx != prefix) {
        free(prefx);
    }

    return PMIX_SUCCESS;
}
int pmix_bfrop_print_uint64(char **output, char *prefix,
                            uint64_t *src,
                            pmix_data_type_t type)
{
    char *prefx;

    /* deal with NULL prefix */
    if (NULL == prefix) asprintf(&prefx, " ");
    else prefx = prefix;

    /* if src is NULL, just print data type and return */
    if (NULL == src) {
        asprintf(output, "%sData type: PMIX_UINT64\tValue: NULL pointer", prefx);
        if (prefx != prefix) {
            free(prefx);
        }
        return PMIX_SUCCESS;
    }

    asprintf(output, "%sData type: PMIX_UINT64\tValue: %lu", prefx, (unsigned long) *src);
    if (prefx != prefix) {
        free(prefx);
    }

    return PMIX_SUCCESS;
}

int pmix_bfrop_print_int64(char **output, char *prefix,
                           int64_t *src,
                           pmix_data_type_t type)
{
    char *prefx;

    /* deal with NULL prefix */
    if (NULL == prefix) asprintf(&prefx, " ");
    else prefx = prefix;

    /* if src is NULL, just print data type and return */
    if (NULL == src) {
        asprintf(output, "%sData type: PMIX_INT64\tValue: NULL pointer", prefx);
        if (prefx != prefix) {
            free(prefx);
        }
        return PMIX_SUCCESS;
    }

    asprintf(output, "%sData type: PMIX_INT64\tValue: %ld", prefx, (long) *src);
    if (prefx != prefix) {
        free(prefx);
    }

    return PMIX_SUCCESS;
}

int pmix_bfrop_print_float(char **output, char *prefix,
                           float *src, pmix_data_type_t type)
{
    char *prefx;

    /* deal with NULL prefix */
    if (NULL == prefix) asprintf(&prefx, " ");
    else prefx = prefix;

    /* if src is NULL, just print data type and return */
    if (NULL == src) {
        asprintf(output, "%sData type: PMIX_FLOAT\tValue: NULL pointer", prefx);
        if (prefx != prefix) {
            free(prefx);
        }
        return PMIX_SUCCESS;
    }

    asprintf(output, "%sData type: PMIX_FLOAT\tValue: %f", prefx, *src);
    if (prefx != prefix) {
        free(prefx);
    }

    return PMIX_SUCCESS;
}

int pmix_bfrop_print_double(char **output, char *prefix,
                            double *src, pmix_data_type_t type)
{
    char *prefx;

    /* deal with NULL prefix */
    if (NULL == prefix) asprintf(&prefx, " ");
    else prefx = prefix;

    /* if src is NULL, just print data type and return */
    if (NULL == src) {
        asprintf(output, "%sData type: PMIX_DOUBLE\tValue: NULL pointer", prefx);
        if (prefx != prefix) {
            free(prefx);
        }
        return PMIX_SUCCESS;
    }

    asprintf(output, "%sData type: PMIX_DOUBLE\tValue: %f", prefx, *src);
    if (prefx != prefix) {
        free(prefx);
    }

    return PMIX_SUCCESS;
}

int pmix_bfrop_print_time(char **output, char *prefix,
                          time_t *src, pmix_data_type_t type)
{
    char *prefx;
    char *t;

    /* deal with NULL prefix */
    if (NULL == prefix) asprintf(&prefx, " ");
    else prefx = prefix;

    /* if src is NULL, just print data type and return */
    if (NULL == src) {
        asprintf(output, "%sData type: PMIX_TIME\tValue: NULL pointer", prefx);
        if (prefx != prefix) {
            free(prefx);
        }
        return PMIX_SUCCESS;
    }

    t = ctime(src);
    t[strlen(t)-1] = '\0';  // remove trailing newline

    asprintf(output, "%sData type: PMIX_TIME\tValue: %s", prefx, t);
    if (prefx != prefix) {
        free(prefx);
    }

    return PMIX_SUCCESS;
}

int pmix_bfrop_print_timeval(char **output, char *prefix,
                             struct timeval *src, pmix_data_type_t type)
{
    char *prefx;

    /* deal with NULL prefix */
    if (NULL == prefix) asprintf(&prefx, " ");
    else prefx = prefix;

    /* if src is NULL, just print data type and return */
    if (NULL == src) {
        asprintf(output, "%sData type: PMIX_TIMEVAL\tValue: NULL pointer", prefx);
        if (prefx != prefix) {
            free(prefx);
        }
        return PMIX_SUCCESS;
    }

    asprintf(output, "%sData type: PMIX_TIMEVAL\tValue: %ld.%06ld", prefx,
             (long)src->tv_sec, (long)src->tv_usec);
    if (prefx != prefix) {
        free(prefx);
    }

    return PMIX_SUCCESS;
}

/* PRINT FUNCTIONS FOR GENERIC PMIX TYPES */

/*
 * PMIX_VALUE
 */
int pmix_bfrop_print_value(char **output, char *prefix,
                           pmix_value_t *src, pmix_data_type_t type)
{
    char *prefx, *t2;
    int i;

    /* deal with NULL prefix */
    if (NULL == prefix) asprintf(&prefx, " ");
    else prefx = prefix;
    
    /* if src is NULL, just print data type and return */
    if (NULL == src) {
        asprintf(output, "%sData type: PMIX_VALUE\tValue: NULL pointer", prefx);
        if (prefx != prefix) {
            free(prefx);
        }
        return PMIX_SUCCESS;
    }
    
    switch (src->type) {
    case PMIX_BOOL:
        asprintf(output, "%sPMIX_VALUE: Data type: PMIX_BOOL\tValue: %s",
                 prefx, src->data.flag ? "true" : "false");
        break;
    case PMIX_BYTE:
        asprintf(output, "%sPMIX_VALUE: Data type: PMIX_BYTE\tValue: %x",
                 prefx, src->data.byte);
        break;
    case PMIX_STRING:
        asprintf(output, "%sPMIX_VALUE: Data type: PMIX_STRING\tValue: %s",
                 prefx, src->data.string);
        break;
    case PMIX_SIZE:
        asprintf(output, "%sPMIX_VALUE: Data type: PMIX_SIZE\tValue: %lu",
                 prefx, (unsigned long)src->data.size);
        break;
    case PMIX_PID:
        asprintf(output, "%sPMIX_VALUE: Data type: PMIX_PID\tValue: %lu",
                 prefx, (unsigned long)src->data.pid);
        break;
    case PMIX_INT:
        asprintf(output, "%sPMIX_VALUE: Data type: PMIX_INT\tValue: %d",
                 prefx, src->data.integer);
        break;
    case PMIX_INT8:
        asprintf(output, "%sPMIX_VALUE: Data type: PMIX_INT8\tValue: %d",
                 prefx, (int)src->data.int8);
        break;
    case PMIX_INT16:
        asprintf(output, "%sPMIX_VALUE: Data type: PMIX_INT16\tValue: %d",
                 prefx, (int)src->data.int16);
        break;
    case PMIX_INT32:
        asprintf(output, "%sPMIX_VALUE: Data type: PMIX_INT32\tValue: %d",
                 prefx, src->data.int32);
        break;
    case PMIX_INT64:
        asprintf(output, "%sPMIX_VALUE: Data type: PMIX_INT64\tValue: %ld",
                 prefx, (long)src->data.int64);
        break;
    case PMIX_UINT:
        asprintf(output, "%sPMIX_VALUE: Data type: PMIX_UINT\tValue: %u",
                 prefx, src->data.uint);
        break;
    case PMIX_UINT8:
        asprintf(output, "%sPMIX_VALUE: Data type: PMIX_UINT8\tValue: %u",
                 prefx, (unsigned int)src->data.uint8);
        break;
    case PMIX_UINT16:
        asprintf(output, "%sPMIX_VALUE: Data type: PMIX_UINT16\tValue: %u",
                 prefx, (unsigned int)src->data.uint16);
        break;
    case PMIX_UINT32:
        asprintf(output, "%sPMIX_VALUE: Data type: PMIX_UINT32\tValue: %u",
                 prefx, src->data.uint32);
        break;
    case PMIX_UINT64:
        asprintf(output, "%sPMIX_VALUE: Data type: PMIX_UINT64\tValue: %lu",
                 prefx, (unsigned long)src->data.uint64);
        break;
    case PMIX_FLOAT:
        asprintf(output, "%sPMIX_VALUE: Data type: PMIX_FLOAT\tValue: %f",
                 prefx, src->data.fval);
        break;
    case PMIX_DOUBLE:
        asprintf(output, "%sPMIX_VALUE: Data type: PMIX_DOUBLE\tValue: %f",
                 prefx, src->data.dval);
        break;
    case PMIX_TIMEVAL:
        asprintf(output, "%sPMIX_VALUE: Data type: PMIX_TIMEVAL\tValue: %ld.%06ld", prefx,
                 (long)src->data.tv.tv_sec, (long)src->data.tv.tv_usec);
        break;
    default:
        asprintf(output, "%sPMIX_VALUE: Data type: UNKNOWN\tValue: UNPRINTABLE", prefx);
        break;
    }
    if (prefx != prefix) {
        free(prefx);
    }
    return PMIX_SUCCESS;
}

int pmix_bfrop_print_info(char **output, char *prefix,
                          pmix_info_t *src, pmix_data_type_t type)
{
    return PMIX_SUCCESS;
}

int pmix_bfrop_print_buf(char **output, char *prefix,
                         pmix_buffer_t *src, pmix_data_type_t type)
{
    return PMIX_SUCCESS;
}

int pmix_bfrop_print_app(char **output, char *prefix,
                         pmix_app_t *src, pmix_data_type_t type)
{
    return PMIX_SUCCESS;
}


int pmix_bfrop_print_range(char **output, char *prefix,
                           pmix_range_t *src, pmix_data_type_t type)
{
    return PMIX_SUCCESS;
}

int pmix_bfrop_print_kval(char **output, char *prefix,
                          pmix_kval_t *src, pmix_data_type_t type)
{
    return PMIX_SUCCESS;
}

int pmix_bfrop_print_array(char **output, char *prefix,
                           pmix_array_t *src, pmix_data_type_t type)
{
    return PMIX_SUCCESS;
}
