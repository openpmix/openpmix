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
 * Copyright (c) 2014-2016 Intel, Inc. All rights reserved.
 * Copyright (c) 2016      Mellanox Technologies, Inc.
 *                         All rights reserved.
 *
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include <src/include/pmix_config.h>

#include <src/include/pmix_stdint.h>

#include <stdio.h>
#ifdef HAVE_TIME_H
#include <time.h>
#endif

#include "src/util/error.h"

#include "src/include/pmix_globals.h"
#include "src/mca/bfrops/base/base.h"

pmix_status_t pmix_bfrops_base_print(pmix_pointer_array_t *regtypes,
                                     char **output, char *prefix,
                                     void *src, pmix_data_type_t type)
{
    pmix_bfrop_type_info_t *info;

    /* check for error */
    if (NULL == output || NULL == src) {
        return PMIX_ERR_BAD_PARAM;
    }

    /* Lookup the print function for this type and call it */

    if(NULL == (info = (pmix_bfrop_type_info_t*)pmix_pointer_array_get_item(regtypes, type))) {
        return PMIX_ERR_UNKNOWN_DATA_TYPE;
    }

    return info->odti_print_fn(output, prefix, src, type);
}

/*
 * STANDARD PRINT FUNCTIONS FOR SYSTEM TYPES
 */
int pmix_bfrops_base_print_bool(char **output, char *prefix,
                                bool *src, pmix_data_type_t type)
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

    asprintf(output, "%sData type: PMIX_BOOL\tValue: %s", prefix,
             (*src) ? "TRUE" : "FALSE");
    if (prefx != prefix) {
        free(prefx);
    }

    return PMIX_SUCCESS;
}

int pmix_bfrops_base_print_byte(char **output, char *prefix,
                                uint8_t *src, pmix_data_type_t type)
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

int pmix_bfrops_base_print_string(char **output, char *prefix,
                                  char *src, pmix_data_type_t type)
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

int pmix_bfrops_base_print_size(char **output, char *prefix,
                                size_t *src, pmix_data_type_t type)
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

int pmix_bfrops_base_print_pid(char **output, char *prefix,
                               pid_t *src, pmix_data_type_t type)
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

int pmix_bfrops_base_print_int(char **output, char *prefix,
                               int *src, pmix_data_type_t type)
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

int pmix_bfrops_base_print_uint(char **output, char *prefix,
                                uint *src, pmix_data_type_t type)
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

int pmix_bfrops_base_print_uint8(char **output, char *prefix,
                                 uint8_t *src, pmix_data_type_t type)
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

int pmix_bfrops_base_print_uint16(char **output, char *prefix,
                                  uint16_t *src, pmix_data_type_t type)
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

int pmix_bfrops_base_print_uint32(char **output, char *prefix,
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

int pmix_bfrops_base_print_int8(char **output, char *prefix,
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

int pmix_bfrops_base_print_int16(char **output, char *prefix,
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

int pmix_bfrops_base_print_int32(char **output, char *prefix,
                                 int32_t *src, pmix_data_type_t type)
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
int pmix_bfrops_base_print_uint64(char **output, char *prefix,
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

int pmix_bfrops_base_print_int64(char **output, char *prefix,
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

int pmix_bfrops_base_print_float(char **output, char *prefix,
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

int pmix_bfrops_base_print_double(char **output, char *prefix,
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

int pmix_bfrops_base_print_time(char **output, char *prefix,
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

int pmix_bfrops_base_print_timeval(char **output, char *prefix,
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

int pmix_bfrops_base_print_status(char **output, char *prefix,
                                  pmix_status_t *src, pmix_data_type_t type)
{
    char *prefx;

    /* deal with NULL prefix */
    if (NULL == prefix) asprintf(&prefx, " ");
    else prefx = prefix;

    /* if src is NULL, just print data type and return */
    if (NULL == src) {
        asprintf(output, "%sData type: PMIX_STATUS\tValue: NULL pointer", prefx);
        if (prefx != prefix) {
            free(prefx);
        }
        return PMIX_SUCCESS;
    }

    asprintf(output, "%sData type: PMIX_STATUS\tValue: %s", prefx, PMIx_Error_string(*src));
    if (prefx != prefix) {
        free(prefx);
    }

    return PMIX_SUCCESS;
}


/* PRINT FUNCTIONS FOR GENERIC PMIX TYPES */

/*
 * PMIX_VALUE
 */
 int pmix_bfrops_base_print_value(char **output, char *prefix,
                                  pmix_value_t *src, pmix_data_type_t type)
 {
    char *prefx;

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
        case PMIX_TIME:
        asprintf(output, "%sPMIX_VALUE: Data type: PMIX_TIME\tValue: %ld", prefx,
                (long)src->data.time);
        break;
        case PMIX_STATUS:
        asprintf(output, "%sPMIX_VALUE: Data type: PMIX_STATUS\tValue: %s", prefx,
                 PMIx_Error_string(src->data.status));
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

int pmix_bfrops_base_print_info(char **output, char *prefix,
                                pmix_info_t *src, pmix_data_type_t type)
{
    char *tmp=NULL, *tmp2=NULL;

    pmix_bfrops_base_print_value(&tmp, NULL, &src->value, PMIX_VALUE);
    pmix_bfrops_base_print_info_directives(&tmp2, NULL, &src->flags, PMIX_INFO_DIRECTIVES);
    asprintf(output, "%sKEY: %s\n%s\t%s\n%s\t%s", prefix, src->key,
             prefix, tmp2, prefix, tmp);
    free(tmp);
    free(tmp2);
    return PMIX_SUCCESS;
}

int pmix_bfrops_base_print_pdata(char **output, char *prefix,
                                 pmix_pdata_t *src, pmix_data_type_t type)
{
    char *tmp1, *tmp2;

    pmix_bfrops_base_print_proc(&tmp1, NULL, &src->proc, PMIX_PROC);
    pmix_bfrops_base_print_value(&tmp2, NULL, &src->value, PMIX_VALUE);
    asprintf(output, "%s  %s  KEY: %s %s", prefix, tmp1, src->key,
             (NULL == tmp2) ? "NULL" : tmp2);
    if (NULL != tmp1) {
        free(tmp1);
    }
    if (NULL != tmp2) {
        free(tmp2);
    }
    return PMIX_SUCCESS;
}

int pmix_bfrops_base_print_buf(char **output, char *prefix,
                               pmix_buffer_t *src, pmix_data_type_t type)
{
    return PMIX_SUCCESS;
}

int pmix_bfrops_base_print_app(char **output, char *prefix,
                               pmix_app_t *src, pmix_data_type_t type)
{
    return PMIX_SUCCESS;
}

int pmix_bfrops_base_print_proc(char **output, char *prefix,
                                pmix_proc_t *src, pmix_data_type_t type)
{
    char *prefx;

    /* deal with NULL prefix */
    if (NULL == prefix) asprintf(&prefx, " ");
    else prefx = prefix;

    asprintf(output, "%sPROC: %s:%d", prefx, src->nspace, src->rank);
    return PMIX_SUCCESS;
}

int pmix_bfrops_base_print_kval(char **output, char *prefix,
                                pmix_kval_t *src, pmix_data_type_t type)
{
    return PMIX_SUCCESS;
}

int pmix_bfrops_base_print_array(char **output, char *prefix,
                                 pmix_info_array_t *src, pmix_data_type_t type)
{
    size_t j;
    char *tmp, *tmp2, *tmp3, *pfx;
    pmix_info_t *s1;

    asprintf(&tmp, "%sARRAY SIZE: %ld", prefix, (long)src->size);
    asprintf(&pfx, "\n%s\t",  (NULL == prefix) ? "" : prefix);
    s1 = (pmix_info_t*)src->array;

    for (j=0; j < src->size; j++) {
        pmix_bfrops_base_print_info(&tmp2, pfx, &s1[j], PMIX_INFO);
        asprintf(&tmp3, "%s%s", tmp, tmp2);
        free(tmp);
        free(tmp2);
        tmp = tmp3;
    }
    *output = tmp;
    return PMIX_SUCCESS;
}

#if PMIX_HAVE_HWLOC
#define PMIX_HWLOC_MAX_STRING   2048

static void print_hwloc_obj(char **output, char *prefix,
                            hwloc_topology_t topo, hwloc_obj_t obj)
{
    hwloc_obj_t obj2;
    char string[1024], *tmp, *tmp2, *pfx;
    unsigned i;
    struct hwloc_topology_support *support;

    /* print the object type */
    hwloc_obj_type_snprintf(string, 1024, obj, 1);
    asprintf(&pfx, "\n%s\t", (NULL == prefix) ? "" : prefix);
    asprintf(&tmp, "%sType: %s Number of child objects: %u%sName=%s",
             (NULL == prefix) ? "" : prefix, string, obj->arity,
             pfx, (NULL == obj->name) ? "NULL" : obj->name);
    if (0 < hwloc_obj_attr_snprintf(string, 1024, obj, pfx, 1)) {
        /* print the attributes */
        asprintf(&tmp2, "%s%s%s", tmp, pfx, string);
        free(tmp);
        tmp = tmp2;
    }
    /* print the cpusets - apparently, some new HWLOC types don't
     * have cpusets, so protect ourselves here
     */
     if (NULL != obj->cpuset) {
        hwloc_bitmap_snprintf(string, PMIX_HWLOC_MAX_STRING, obj->cpuset);
        asprintf(&tmp2, "%s%sCpuset:  %s", tmp, pfx, string);
        free(tmp);
        tmp = tmp2;
    }
    if (NULL != obj->online_cpuset) {
        hwloc_bitmap_snprintf(string, PMIX_HWLOC_MAX_STRING, obj->online_cpuset);
        asprintf(&tmp2, "%s%sOnline:  %s", tmp, pfx, string);
        free(tmp);
        tmp = tmp2;
    }
    if (NULL != obj->allowed_cpuset) {
        hwloc_bitmap_snprintf(string, PMIX_HWLOC_MAX_STRING, obj->allowed_cpuset);
        asprintf(&tmp2, "%s%sAllowed: %s", tmp, pfx, string);
        free(tmp);
        tmp = tmp2;
    }
    if (HWLOC_OBJ_MACHINE == obj->type) {
        /* root level object - add support values */
        support = (struct hwloc_topology_support*)hwloc_topology_get_support(topo);
        asprintf(&tmp2, "%s%sBind CPU proc:   %s%sBind CPU thread: %s", tmp, pfx,
                 (support->cpubind->set_thisproc_cpubind) ? "TRUE" : "FALSE", pfx,
                 (support->cpubind->set_thisthread_cpubind) ? "TRUE" : "FALSE");
        free(tmp);
        tmp = tmp2;
        asprintf(&tmp2, "%s%sBind MEM proc:   %s%sBind MEM thread: %s", tmp, pfx,
                 (support->membind->set_thisproc_membind) ? "TRUE" : "FALSE", pfx,
                 (support->membind->set_thisthread_membind) ? "TRUE" : "FALSE");
        free(tmp);
        tmp = tmp2;
    }
    asprintf(&tmp2, "%s%s\n", (NULL == *output) ? "" : *output, tmp);
    free(tmp);
    free(pfx);
    asprintf(&pfx, "%s\t", (NULL == prefix) ? "" : prefix);
    for (i=0; i < obj->arity; i++) {
        obj2 = obj->children[i];
        /* print the object */
        print_hwloc_obj(&tmp2, pfx, topo, obj2);
    }
    free(pfx);
    if (NULL != *output) {
        free(*output);
    }
    *output = tmp2;
}

int pmix_bfrops_base_print_topo(char **output, char *prefix,
                                hwloc_topology_t src, pmix_data_type_t type)
{
    hwloc_obj_t obj;
    char *tmp=NULL;

    /* get root object */
    obj = hwloc_get_root_obj(src);
    /* print it */
    print_hwloc_obj(&tmp, prefix, src, obj);
    *output = tmp;
    return PMIX_SUCCESS;
}

#endif

int pmix_bfrops_base_print_modex(char **output, char *prefix,
                                 pmix_modex_data_t *src, pmix_data_type_t type)
{
    return PMIX_SUCCESS;
}

int pmix_bfrops_base_print_persist(char **output, char *prefix,
                                   pmix_persistence_t *src, pmix_data_type_t type)
{
    char *prefx;

    /* deal with NULL prefix */
    if (NULL == prefix) asprintf(&prefx, " ");
    else prefx = prefix;

    /* if src is NULL, just print data type and return */
    if (NULL == src) {
        asprintf(output, "%sData type: PMIX_PERSIST\tValue: NULL pointer", prefx);
        if (prefx != prefix) {
            free(prefx);
        }
        return PMIX_SUCCESS;
    }

    asprintf(output, "%sData type: PMIX_PERSIST\tValue: %ld", prefx, (long) *src);
    if (prefx != prefix) {
        free(prefx);
    }

    return PMIX_SUCCESS;
}

int pmix_bfrops_base_print_bo(char **output, char *prefix,
                              pmix_byte_object_t *src, pmix_data_type_t type)
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

    asprintf(output, "%sData type: PMIX_BYTE_OBJECT\tSize: %ld", prefx, (long)src->size);
    if (prefx != prefix) {
        free(prefx);
    }

    return PMIX_SUCCESS;
}

int pmix_bfrops_base_print_ptr(char **output, char *prefix,
                               void *src, pmix_data_type_t type)
{
    char *prefx;

    /* deal with NULL prefix */
    if (NULL == prefix) asprintf(&prefx, " ");
    else prefx = prefix;

    asprintf(output, "%sData type: PMIX_POINTER\tAddress: %p", prefx, src);
    if (prefx != prefix) {
        free(prefx);
    }

    return PMIX_SUCCESS;
}

int pmix_bfrops_base_print_scope(char **output, char *prefix,
                                 void *src, pmix_data_type_t type)
{
    char *prefx;
    pmix_scope_t *val = (pmix_scope_t*)src;
    char *str;

    /* deal with NULL prefix */
    if (NULL == prefix) asprintf(&prefx, " ");
    else prefx = prefix;

    switch(*val) {
        case PMIX_SCOPE_UNDEF:
            str = "UNDEF";
            break;
        case PMIX_LOCAL:
            str = "LOCAL";
            break;
        case PMIX_REMOTE:
            str = "REMOTE";
            break;
        case PMIX_GLOBAL:
            str = "GLOBAL";
            break;
        default:
            str = "UNKNOWN";
    }

    asprintf(output, "%sData type: PMIX_SCOPE\tValue: %s", prefx, str);
    if (prefx != prefix) {
        free(prefx);
    }

    return PMIX_SUCCESS;
}

int pmix_bfrops_base_print_range(char **output, char *prefix,
                                 void *src, pmix_data_type_t type)
{
    char *prefx;
    pmix_data_range_t *val = (pmix_data_range_t*)src;
    char *str;

    /* deal with NULL prefix */
    if (NULL == prefix) asprintf(&prefx, " ");
    else prefx = prefix;

    switch(*val) {
        case PMIX_RANGE_UNDEF:
            str = "UNDEF";
            break;
        case PMIX_RANGE_RM:
            str = "Resource Manager";
            break;
        case PMIX_RANGE_LOCAL:
            str = "LOCAL";
            break;
        case PMIX_RANGE_NAMESPACE:
            str = "NAMESPACE";
            break;
        case PMIX_RANGE_SESSION:
            str = "SESSION";
            break;
        case PMIX_RANGE_GLOBAL:
            str = "GLOBAL";
            break;
        case PMIX_RANGE_CUSTOM:
            str = "CUSTOM";
            break;
        default:
            str = "UNKNOWN";
    }

    asprintf(output, "%sData type: PMIX_DATA_RANGE\tValue: %s", prefx, str);
    if (prefx != prefix) {
        free(prefx);
    }

    return PMIX_SUCCESS;
}

int pmix_bfrops_base_print_cmd(char **output, char *prefix,
                               void *src, pmix_data_type_t type)
{
    char *prefx;
    pmix_cmd_t *val = (pmix_cmd_t*)src;
    char *str;

    /* deal with NULL prefix */
    if (NULL == prefix) asprintf(&prefx, " ");
    else prefx = prefix;

    switch(*val) {
        case PMIX_REQ_CMD:
            str = "REQUEST";
            break;
        case PMIX_ABORT_CMD:
            str = "ABORT";
            break;
        case PMIX_COMMIT_CMD:
            str = "COMMIT";
            break;
        case PMIX_FENCENB_CMD:
            str = "FENCENB";
            break;
        case PMIX_GETNB_CMD:
            str = "GETNB";
            break;
        case PMIX_FINALIZE_CMD:
            str = "FINALIZE";
            break;
        case PMIX_PUBLISHNB_CMD:
            str = "PUBLISHNB";
            break;
        case PMIX_SPAWNNB_CMD:
            str = "SPAWNNB";
            break;
        case PMIX_CONNECTNB_CMD:
            str = "CONNECTNB";
            break;
        case PMIX_DISCONNECTNB_CMD:
            str = "DISCONNECTNB";
            break;
        case PMIX_NOTIFY_CMD:
            str = "NOTIFY";
            break;
        case PMIX_REGEVENTS_CMD:
            str = "REGISTER-EVENTS";
            break;
        case PMIX_DEREGEVENTS_CMD:
            str = "DEREGISTER-EVENTS";
            break;
        default:
            str = "UNKNOWN";
    }

    asprintf(output, "%sData type: PMIX_CMD\tValue: %s", prefx, str);
    if (prefx != prefix) {
        free(prefx);
    }

    return PMIX_SUCCESS;
}

int pmix_bfrops_base_print_info_directives(char **output, char *prefix,
                                           void *src, pmix_data_type_t type)
{
    char *prefx;
    pmix_info_directives_t *val = (pmix_info_directives_t*)src;
    char *str;

    /* deal with NULL prefix */
    if (NULL == prefix) asprintf(&prefx, " ");
    else prefx = prefix;

    if (PMIX_INFO_REQD & (*val)) {
        str = "REQUIRED";
    } else {
        str = "OPTIONAL";
    }

    asprintf(output, "%sData type: PMIX_INFO_DIRECTIVES\tValue: %s", prefx, str);
    if (prefx != prefix) {
        free(prefx);
    }

    return PMIX_SUCCESS;
}

