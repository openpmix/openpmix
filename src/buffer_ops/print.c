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

int pmix_bfrop_print_uint32(char **output, char *prefix, uint32_t *src, pmix_data_type_t type)
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

int pmix_bfrop_print_int8(char **output, char *prefix, int8_t *src, pmix_data_type_t type)
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

int pmix_bfrop_print_int16(char **output, char *prefix, int16_t *src, pmix_data_type_t type)
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
#ifdef HAVE_INT64_T
                          uint64_t *src,
#else
                          void *src,
#endif  /* HAVE_INT64_T */
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

#ifdef HAVE_INT64_T
    asprintf(output, "%sData type: PMIX_UINT64\tValue: %lu", prefx, (unsigned long) *src);
#else
    asprintf(output, "%sData type: PMIX_UINT64\tValue: unsupported", prefx);
#endif  /* HAVE_INT64_T */
    if (prefx != prefix) {
        free(prefx);
    }

    return PMIX_SUCCESS;
}

int pmix_bfrop_print_int64(char **output, char *prefix,
#ifdef HAVE_INT64_T
                         int64_t *src,
#else
                         void *src,
#endif  /* HAVE_INT64_T */
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

#ifdef HAVE_INT64_T
    asprintf(output, "%sData type: PMIX_INT64\tValue: %ld", prefx, (long) *src);
#else
    asprintf(output, "%sData type: PMIX_INT64\tValue: unsupported", prefx);
#endif  /* HAVE_INT64_T */
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

int pmix_bfrop_print_null(char **output, char *prefix, void *src, pmix_data_type_t type)
{
    char *prefx;

    /* deal with NULL prefix */
    if (NULL == prefix) asprintf(&prefx, " ");
    else prefx = prefix;

    /* if src is NULL, just print data type and return */
    if (NULL == src) {
        asprintf(output, "%sData type: PMIX_NULL\tValue: NULL pointer", prefx);
        if (prefx != prefix) {
            free(prefx);
        }
        return PMIX_SUCCESS;
    }

    asprintf(output, "%sData type: PMIX_NULL", prefx);
    if (prefx != prefix) {
        free(prefx);
    }

    return PMIX_SUCCESS;
}


/* PRINT FUNCTIONS FOR GENERIC PMIX TYPES */

/*
 * PMIX_DATA_TYPE
 */
int pmix_bfrop_print_data_type(char **output, char *prefix, pmix_data_type_t *src, pmix_data_type_t type)
{
    char *prefx;

    /* deal with NULL prefix */
    if (NULL == prefix) asprintf(&prefx, " ");
    else prefx = prefix;

    /* if src is NULL, just print data type and return */
    if (NULL == src) {
        asprintf(output, "%sData type: PMIX_DATA_TYPE\tValue: NULL pointer", prefx);
        if (prefx != prefix) {
            free(prefx);
        }
        return PMIX_SUCCESS;
    }

    asprintf(output, "%sData type: PMIX_DATA_TYPE\tValue: %lu", prefx, (unsigned long) *src);
    if (prefx != prefix) {
        free(prefx);
    }
    return PMIX_SUCCESS;
}

/*
 * PMIX_BYTE_OBJECT
 */
int pmix_bfrop_print_byte_object(char **output, char *prefix, pmix_byte_object_t *src, pmix_data_type_t type)
{
    char *prefx;

    /* deal with NULL prefix */
    if (NULL == prefix) asprintf(&prefx, " ");
    else prefx = prefix;

    /* if src is NULL, just print data type and return */
    if (NULL == src) {
        asprintf(output, "%sData type: PMIX_BYTE_OBJECT\tValue: NULL pointer", prefx);
        if (prefx != prefix) {
            free(prefx);
        }
        return PMIX_SUCCESS;
    }

    asprintf(output, "%sData type: PMIX_BYTE_OBJECT\tSize: %lu", prefx, (unsigned long) src->size);
    if (prefx != prefix) {
        free(prefx);
    }

    return PMIX_SUCCESS;
}

/*
 * PMIX_VALUE
 */
int pmix_bfrop_print_value(char **output, char *prefix, pmix_value_t *src, pmix_data_type_t type)
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
        asprintf(output, "%sPMIX_VALUE: Data type: PMIX_BOOL\tKey: %s\tValue: %s",
                 prefx, src->key, src->data.flag ? "true" : "false");
        break;
    case PMIX_BYTE:
        asprintf(output, "%sPMIX_VALUE: Data type: PMIX_BYTE\tKey: %s\tValue: %x",
                 prefx, src->key, src->data.byte);
        break;
    case PMIX_STRING:
        asprintf(output, "%sPMIX_VALUE: Data type: PMIX_STRING\tKey: %s\tValue: %s",
                 prefx, src->key, src->data.string);
        break;
    case PMIX_SIZE:
        asprintf(output, "%sPMIX_VALUE: Data type: PMIX_SIZE\tKey: %s\tValue: %lu",
                 prefx, src->key, (unsigned long)src->data.size);
        break;
    case PMIX_PID:
        asprintf(output, "%sPMIX_VALUE: Data type: PMIX_PID\tKey: %s\tValue: %lu",
                 prefx, src->key, (unsigned long)src->data.pid);
        break;
    case PMIX_INT:
        asprintf(output, "%sPMIX_VALUE: Data type: PMIX_INT\tKey: %s\tValue: %d",
                 prefx, src->key, src->data.integer);
        break;
    case PMIX_INT8:
        asprintf(output, "%sPMIX_VALUE: Data type: PMIX_INT8\tKey: %s\tValue: %d",
                 prefx, src->key, (int)src->data.int8);
        break;
    case PMIX_INT16:
        asprintf(output, "%sPMIX_VALUE: Data type: PMIX_INT16\tKey: %s\tValue: %d",
                 prefx, src->key, (int)src->data.int16);
        break;
    case PMIX_INT32:
        asprintf(output, "%sPMIX_VALUE: Data type: PMIX_INT32\tKey: %s\tValue: %d",
                 prefx, src->key, src->data.int32);
        break;
    case PMIX_INT64:
        asprintf(output, "%sPMIX_VALUE: Data type: PMIX_INT64\tKey: %s\tValue: %ld",
                 prefx, src->key, (long)src->data.int64);
        break;
    case PMIX_UINT:
        asprintf(output, "%sPMIX_VALUE: Data type: PMIX_UINT\tKey: %s\tValue: %u",
                 prefx, src->key, src->data.uint);
        break;
    case PMIX_UINT8:
        asprintf(output, "%sPMIX_VALUE: Data type: PMIX_UINT8\tKey: %s\tValue: %u",
                 prefx, src->key, (unsigned int)src->data.uint8);
        break;
    case PMIX_UINT16:
        asprintf(output, "%sPMIX_VALUE: Data type: PMIX_UINT16\tKey: %s\tValue: %u",
                 prefx, src->key, (unsigned int)src->data.uint16);
        break;
    case PMIX_UINT32:
        asprintf(output, "%sPMIX_VALUE: Data type: PMIX_UINT32\tKey: %s\tValue: %u",
                 prefx, src->key, src->data.uint32);
        break;
    case PMIX_UINT64:
        asprintf(output, "%sPMIX_VALUE: Data type: PMIX_UINT64\tKey: %s\tValue: %lu",
                 prefx, src->key, (unsigned long)src->data.uint64);
        break;
    case PMIX_FLOAT:
        asprintf(output, "%sPMIX_VALUE: Data type: PMIX_FLOAT\tKey: %s\tValue: %f",
                 prefx, src->key, src->data.fval);
        break;
    case PMIX_DOUBLE:
        asprintf(output, "%sPMIX_VALUE: Data type: PMIX_DOUBLE\tKey: %s\tValue: %f",
                 prefx, src->key, src->data.dval);
        break;
    case PMIX_BYTE_OBJECT:
        asprintf(output, "%sPMIX_VALUE: Data type: PMIX_BYTE_OBJECT\tKey: %s\tData: %s\tSize: %lu",
                 prefx, src->key, (NULL == src->data.bo.bytes) ? "NULL" : "NON-NULL", (unsigned long)src->data.bo.size);
        break;
    case PMIX_TIMEVAL:
        asprintf(output, "%sPMIX_VALUE: Data type: PMIX_TIMEVAL\tKey: %s\tValue: %ld.%06ld", prefx,
                 src->key, (long)src->data.tv.tv_sec, (long)src->data.tv.tv_usec);
        break;
    case PMIX_PTR:
        asprintf(output, "%sPMIX_VALUE: Data type: PMIX_PTR\tKey: %s", prefx, src->key);
        break;
    case PMIX_FLOAT_ARRAY:
        asprintf(output, "%sPMIX_VALUE: Data type: PMIX_FLOAT_ARRAY\tKey: %s \tSIZE: %d \tDATA: ",
                 prefx, src->key, src->data.fval_array.size);
        for (i = 0; i < src->data.fval_array.size; i++) {
            asprintf(&t2, "%s\n%s\t%f", *output, prefx, src->data.fval_array.data[i]);
            free(*output);
            *output = t2;
        }
        break;
    case PMIX_DOUBLE_ARRAY:
        asprintf(output, "%sPMIX_VALUE: Data type: PMIX_DOUBLE_ARRAY\tKey: %s \tSIZE: %d \tDATA: ",
                 prefx, src->key, src->data.dval_array.size);
        for (i = 0; i < src->data.dval_array.size; i++) {
            asprintf(&t2, "%s\n%s\t%f", *output, prefx, src->data.dval_array.data[i]);
            free(*output);
            *output = t2;
        }
        break;
    case PMIX_STRING_ARRAY:
        asprintf(output, "%sPMIX_VALUE: Data type: PMIX_STRING_ARRAY\tKey: %s \tSIZE: %d \tDATA: ",
                 prefx, src->key, src->data.string_array.size);
        for (i = 0; i < src->data.string_array.size; i++) {
            asprintf(&t2, "%s\n%s\t%s", *output, prefx, src->data.string_array.data[i]);
            free(*output);
            *output = t2;
        }
        break;
    case PMIX_BOOL_ARRAY:
        asprintf(output, "%sPMIX_VALUE: Data type: PMIX_BOOL_ARRAY\tKey: %s \tSIZE: %d \tDATA: ",
                 prefx, src->key, src->data.flag_array.size);
        for (i = 0; i < src->data.flag_array.size; i++) {
            asprintf(&t2, "%s\n%s\t%s", *output, prefx, src->data.flag_array.data[i] ? "true" : "false");
            free(*output);
            *output = t2;
        }
        break;
    case PMIX_SIZE_ARRAY:
        asprintf(output, "%sPMIX_VALUE: Data type: PMIX_SIZE_ARRAY\tKey: %s \tSIZE: %d \tDATA: ",
                 prefx, src->key, src->data.size_array.size);
        for (i = 0; i < src->data.size_array.size; i++) {
            asprintf(&t2, "%s\n%s\t%lu", *output, prefx, (unsigned long)src->data.size_array.data[i]);
            free(*output);
            *output = t2;
        }
        break;
    case PMIX_BYTE_ARRAY:
        asprintf(output, "%sPMIX_VALUE: Data type: PMIX_BYTE_ARRAY\tKey: %s \tSIZE: %d \tDATA: ",
                 prefx, src->key, src->data.byte_array.size);
        for (i = 0; i < src->data.byte_array.size; i++) {
            asprintf(&t2, "%s\n%s\t%x", *output, prefx, (uint8_t)src->data.byte_array.data[i]);
            free(*output);
            *output = t2;
        }
        break;
    case PMIX_INT_ARRAY:
        asprintf(output, "%sPMIX_VALUE: Data type: PMIX_INT_ARRAY\tKey: %s \tSIZE: %d \tDATA: ",
                 prefx, src->key, src->data.integer_array.size);
        for (i = 0; i < src->data.integer_array.size; i++) {
            asprintf(&t2, "%s\n%s\t%d", *output, prefx, src->data.integer_array.data[i]);
            free(*output);
            *output = t2;
        }
        break;
    case PMIX_INT8_ARRAY:
        asprintf(output, "%sPMIX_VALUE: Data type: PMIX_INT8_ARRAY\tKey: %s \tSIZE: %d \tDATA: ",
                 prefx, src->key, src->data.int8_array.size);
        for (i = 0; i < src->data.int8_array.size; i++) {
            asprintf(&t2, "%s\n%s\t%d", *output, prefx, (int)src->data.int8_array.data[i]);
            free(*output);
            *output = t2;
        }
        break;
    case PMIX_INT16_ARRAY:
        asprintf(output, "%sPMIX_VALUE: Data type: PMIX_INT16_ARRAY\tKey: %s \tSIZE: %d \tDATA: ",
                 prefx, src->key, src->data.int16_array.size);
        for (i = 0; i < src->data.int16_array.size; i++) {
            asprintf(&t2, "%s\n%s\t%d", *output, prefx, (int)src->data.int16_array.data[i]);
            free(*output);
            *output = t2;
        }
        break;
    case PMIX_INT32_ARRAY:
        asprintf(output, "%sPMIX_VALUE: Data type: PMIX_INT32_ARRAY\tKey: %s \tSIZE: %d \tDATA: ",
                 prefx, src->key, src->data.int32_array.size);
        for (i = 0; i < src->data.int32_array.size; i++) {
            asprintf(&t2, "%s\n%s\t%d", *output, prefx, src->data.int32_array.data[i]);
            free(*output);
            *output = t2;
        }
        break;
    case PMIX_INT64_ARRAY:
        asprintf(output, "%sPMIX_VALUE: Data type: PMIX_INT64_ARRAY\tKey: %s \tSIZE: %d \tDATA: ",
                 prefx, src->key, src->data.int64_array.size);
        for (i = 0; i < src->data.int64_array.size; i++) {
            asprintf(&t2, "%s\n%s\t%ld", *output, prefx, (long)src->data.int64_array.data[i]);
            free(*output);
            *output = t2;
        }
        break;
    case PMIX_UINT_ARRAY:
        asprintf(output, "%sPMIX_VALUE: Data type: PMIX_UINT_ARRAY\tKey: %s \tSIZE: %d \tDATA: ",
                 prefx, src->key, src->data.uint_array.size);
        for (i = 0; i < src->data.uint_array.size; i++) {
            asprintf(&t2, "%s\n%s\t%u", *output, prefx, src->data.uint_array.data[i]);
            free(*output);
            *output = t2;
        }
        break;
    case PMIX_UINT8_ARRAY:
        asprintf(output, "%sPMIX_VALUE: Data type: PMIX_UINT8_ARRAY\tKey: %s \tSIZE: %d \tDATA: ",
                 prefx, src->key, src->data.uint8_array.size);
        for (i = 0; i < src->data.uint8_array.size; i++) {
            asprintf(&t2, "%s\n%s\t%u", *output, prefx, (unsigned int)src->data.uint8_array.data[i]);
            free(*output);
            *output = t2;
        }
        break;
    case PMIX_UINT16_ARRAY:
        asprintf(output, "%sPMIX_VALUE: Data type: PMIX_UINT16_ARRAY\tKey: %s \tSIZE: %d \tDATA: ",
                 prefx, src->key, src->data.uint16_array.size);
        for (i = 0; i < src->data.uint16_array.size; i++) {
            asprintf(&t2, "%s\n%s\t%u", *output, prefx, (unsigned int)src->data.uint16_array.data[i]);
            free(*output);
            *output = t2;
        }
        break;
    case PMIX_UINT32_ARRAY:
        asprintf(output, "%sPMIX_VALUE: Data type: PMIX_UINT32_ARRAY\tKey: %s \tSIZE: %d \tDATA: ",
                 prefx, src->key, src->data.uint32_array.size);
        for (i = 0; i < src->data.uint32_array.size; i++) {
            asprintf(&t2, "%s\n%s\t%u", *output, prefx, (unsigned int)src->data.uint32_array.data[i]);
            free(*output);
            *output = t2;
        }
        break;
    case PMIX_UINT64_ARRAY:
        asprintf(output, "%sPMIX_VALUE: Data type: PMIX_UINT64_ARRAY\tKey: %s \tSIZE: %d \tDATA: ",
                 prefx, src->key, src->data.uint64_array.size);
        for (i = 0; i < src->data.uint64_array.size; i++) {
            asprintf(&t2, "%s\n%s\t%lu", *output, prefx, (unsigned long)src->data.uint64_array.data[i]);
            free(*output);
            *output = t2;
        }
        break;
    case PMIX_BYTE_OBJECT_ARRAY:
        asprintf(output, "%sPMIX_VALUE: Data type: PMIX_BYTE_OBJECT_ARRAY\tKey: %s \tSIZE: %d \tDATA: ",
                 prefx, src->key, src->data.bo_array.size);
        for (i = 0; i < src->data.bo_array.size; i++) {
            asprintf(&t2, "%s\n%s\tData: %s\tSize: %lu ", *output, prefx,
                     (NULL == src->data.bo_array.data[i].bytes) ? "NULL" : "NON-NULL",
                     (unsigned long)src->data.bo_array.data[i].size);
            free(*output);
            *output = t2;
        }
        break;
    case PMIX_PID_ARRAY:
        asprintf(output, "%sPMIX_VALUE: Data type: PMIX_PID_ARRAY\tKey: %s \tSIZE: %d \tDATA: ",
                 prefx, src->key, src->data.pid_array.size);
        for (i = 0; i < src->data.pid_array.size; i++) {
            asprintf(&t2, "%s\n%s\t%u", *output, prefx, (unsigned int)src->data.pid_array.data[i]);
            free(*output);
            *output = t2;
        }
        break;
    case PMIX_TIMEVAL_ARRAY:
        asprintf(output, "%sPMIX_VALUE: Data type: PMIX_TIMEVAL_ARRAY\tKey: %s \tSIZE: %d \tDATA: ",
                 prefx, src->key, src->data.tv_array.size);
        for (i = 0; i < src->data.tv_array.size; i++) {
            asprintf(&t2, "%s\n%s\t%ld.%06ld", *output, prefx,
                     (long)src->data.tv_array.data[i].tv_sec,
                     (long)src->data.tv_array.data[i].tv_usec);
            free(*output);
            *output = t2;
        }
        break;
    default:
        asprintf(output, "%sPMIX_VALUE: Data type: UNKNOWN\tKey: %s\tValue: UNPRINTABLE",
                 prefx, src->key);
        break;
    }
    if (prefx != prefix) {
        free(prefx);
    }
    return PMIX_SUCCESS;
}

int pmix_bfrop_print_buffer_contents(char **output, char *prefix,
                                   pmix_buffer_t *src, pmix_data_type_t type)
{
    return PMIX_SUCCESS;
}

int pmix_bfrop_print_info(char **output, char *prefix,
                          pmix_info_t *src, pmix_data_type_t type)
{
    return PMIX_SUCCESS;
}

int pmix_bfrop_print_app(char **output, char *prefix,
                          pmix_app_t *src, pmix_data_type_t type)
{
    return PMIX_SUCCESS;
}

