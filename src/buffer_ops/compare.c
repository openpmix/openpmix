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
#include "constants.h"

#include <stdlib.h>
#ifdef HAVE_SYS_TIME_H
#include <sys/time.h> /* for struct timeval */
#endif

#include "internal.h"

int pmix_bfrop_compare(const void *value1, const void *value2, pmix_data_type_t type)
{
    pmix_bfrop_type_info_t *info;

    /* check for error */
    if (NULL == value1 || NULL == value2) {
        return PMIX_ERR_BAD_PARAM;
    }

    /* Lookup the compare function for this type and call it */

    if (NULL == (info = (pmix_bfrop_type_info_t*)pmix_pointer_array_get_item(&pmix_bfrop_types, type))) {
        return PMIX_ERR_UNKNOWN_DATA_TYPE;
    }

    return info->odti_compare_fn(value1, value2, type);
}

/*
 * NUMERIC COMPARE FUNCTIONS
 */
int pmix_bfrop_compare_int(int *value1, int *value2, pmix_data_type_t type)
{
    if (*value1 > *value2) return PMIX_VALUE1_GREATER;

    if (*value2 > *value1) return PMIX_VALUE2_GREATER;

    return PMIX_EQUAL;
}

int pmix_bfrop_compare_uint(unsigned int *value1, unsigned int *value2, pmix_data_type_t type)
{
    if (*value1 > *value2) return PMIX_VALUE1_GREATER;

    if (*value2 > *value1) return PMIX_VALUE2_GREATER;

    return PMIX_EQUAL;
}

int pmix_bfrop_compare_size(size_t *value1, size_t *value2, pmix_data_type_t type)
{
    if (*value1 > *value2) return PMIX_VALUE1_GREATER;

    if (*value2 > *value1) return PMIX_VALUE2_GREATER;

    return PMIX_EQUAL;
}

int pmix_bfrop_compare_pid(pid_t *value1, pid_t *value2, pmix_data_type_t type)
{
    if (*value1 > *value2) return PMIX_VALUE1_GREATER;

    if (*value2 > *value1) return PMIX_VALUE2_GREATER;

    return PMIX_EQUAL;
}

int pmix_bfrop_compare_byte(char *value1, char *value2, pmix_data_type_t type)
{
    if (*value1 > *value2) return PMIX_VALUE1_GREATER;

    if (*value2 > *value1) return PMIX_VALUE2_GREATER;

    return PMIX_EQUAL;
}

int pmix_bfrop_compare_char(char *value1, char *value2, pmix_data_type_t type)
{
    if (*value1 > *value2) return PMIX_VALUE1_GREATER;

    if (*value2 > *value1) return PMIX_VALUE2_GREATER;

    return PMIX_EQUAL;
}

int pmix_bfrop_compare_int8(int8_t *value1, int8_t *value2, pmix_data_type_t type)
{
    if (*value1 > *value2) return PMIX_VALUE1_GREATER;

    if (*value2 > *value1) return PMIX_VALUE2_GREATER;

    return PMIX_EQUAL;
}

int pmix_bfrop_compare_uint8(uint8_t *value1, uint8_t *value2, pmix_data_type_t type)
{
    if (*value1 > *value2) return PMIX_VALUE1_GREATER;

    if (*value2 > *value1) return PMIX_VALUE2_GREATER;

    return PMIX_EQUAL;
}

int pmix_bfrop_compare_int16(int16_t *value1, int16_t *value2, pmix_data_type_t type)
{
    if (*value1 > *value2) return PMIX_VALUE1_GREATER;

    if (*value2 > *value1) return PMIX_VALUE2_GREATER;

    return PMIX_EQUAL;
}

int pmix_bfrop_compare_uint16(uint16_t *value1, uint16_t *value2, pmix_data_type_t type)
{
    if (*value1 > *value2) return PMIX_VALUE1_GREATER;

    if (*value2 > *value1) return PMIX_VALUE2_GREATER;

    return PMIX_EQUAL;
}

int pmix_bfrop_compare_int32(int32_t *value1, int32_t *value2, pmix_data_type_t type)
{
    if (*value1 > *value2) return PMIX_VALUE1_GREATER;

    if (*value2 > *value1) return PMIX_VALUE2_GREATER;

    return PMIX_EQUAL;
}

int pmix_bfrop_compare_uint32(uint32_t *value1, uint32_t *value2, pmix_data_type_t type)
{
    if (*value1 > *value2) return PMIX_VALUE1_GREATER;

    if (*value2 > *value1) return PMIX_VALUE2_GREATER;

    return PMIX_EQUAL;
}

int pmix_bfrop_compare_int64(int64_t *value1, int64_t *value2, pmix_data_type_t type)
{
    if (*value1 > *value2) return PMIX_VALUE1_GREATER;

    if (*value2 > *value1) return PMIX_VALUE2_GREATER;

    return PMIX_EQUAL;
}

int pmix_bfrop_compare_uint64(uint64_t *value1, uint64_t *value2, pmix_data_type_t type)
{
    if (*value1 > *value2) return PMIX_VALUE1_GREATER;

    if (*value2 > *value1) return PMIX_VALUE2_GREATER;

    return PMIX_EQUAL;
}

int pmix_bfrop_compare_float(float *value1, float *value2, pmix_data_type_t type)
{
    if (*value1 > *value2) return PMIX_VALUE1_GREATER;

    if (*value2 > *value1) return PMIX_VALUE2_GREATER;

    return PMIX_EQUAL;
}

int pmix_bfrop_compare_double(double *value1, double *value2, pmix_data_type_t type)
{
    if (*value1 > *value2) return PMIX_VALUE1_GREATER;

    if (*value2 > *value1) return PMIX_VALUE2_GREATER;

    return PMIX_EQUAL;
}

/*
 * NON-NUMERIC SYSTEM TYPES
 */

/* NULL */
int pmix_bfrop_compare_null(char *value1, char *value2, pmix_data_type_t type)
{
    return PMIX_EQUAL;
}

/* BOOL */
int pmix_bfrop_compare_bool(bool *value1, bool *value2, pmix_data_type_t type)
{
    if (*value1 && !(*value2)) return PMIX_VALUE1_GREATER;

    if (*value2 && !(*value1)) return PMIX_VALUE2_GREATER;

    return PMIX_EQUAL;

}

/* STRING */
int pmix_bfrop_compare_string(char *value1, char *value2, pmix_data_type_t type)
{
    if (0 < strcmp(value1, value2)) return PMIX_VALUE2_GREATER;

    if (0 > strcmp(value1, value2)) return PMIX_VALUE1_GREATER;

    return PMIX_EQUAL;
}

/* TIMEVAL */
int pmix_bfrop_compare_timeval(struct timeval *value1, struct timeval *value2, pmix_data_type_t type)
{
    if (value1->tv_sec > value2->tv_sec) return PMIX_VALUE1_GREATER;
    if (value2->tv_sec > value1->tv_sec) return PMIX_VALUE2_GREATER;

    /* seconds were equal - check usec's */
    if (value1->tv_usec > value2->tv_usec) return PMIX_VALUE1_GREATER;
    if (value2->tv_usec > value1->tv_usec) return PMIX_VALUE2_GREATER;

    return PMIX_EQUAL;
}

/* TIME */
int pmix_bfrop_compare_time(time_t *value1, time_t *value2, pmix_data_type_t type)
{
    if (value1 > value2) return PMIX_VALUE1_GREATER;
    if (value2 > value1) return PMIX_VALUE2_GREATER;

    return PMIX_EQUAL;
}

/* COMPARE FUNCTIONS FOR GENERIC PMIX TYPES */
/* PMIX_DATA_TYPE */
int pmix_bfrop_compare_dt(pmix_data_type_t *value1, pmix_data_type_t *value2, pmix_data_type_t type)
{
    if (*value1 > *value2) return PMIX_VALUE1_GREATER;

    if (*value2 > *value1) return PMIX_VALUE2_GREATER;

    return PMIX_EQUAL;
}

/* PMIX_BYTE_OBJECT */
int pmix_bfrop_compare_byte_object(pmix_byte_object_t *value1, pmix_byte_object_t *value2, pmix_data_type_t type)
{
    int checksum, diff;
    int32_t i;

    /* compare the sizes first - bigger size object is "greater than" */
    if (value1->size > value2->size) return PMIX_VALUE1_GREATER;

    if (value2->size > value1->size) return PMIX_VALUE2_GREATER;

    /* get here if the two sizes are identical - now do a simple checksum-style
     * calculation to determine "biggest"
     */
    checksum = 0;

    for (i=0; i < value1->size; i++) {
        /* protect against overflows */
        diff = value1->bytes[i] - value2->bytes[i];
        if (INT_MAX-abs(checksum)-abs(diff) < 0) { /* got an overflow condition */
            checksum = 0;
        }
        checksum += diff;
    }

    if (0 > checksum) return PMIX_VALUE2_GREATER;  /* sum of value2 bytes was greater */

    if (0 < checksum) return PMIX_VALUE1_GREATER;  /* of value1 bytes was greater */

    return PMIX_EQUAL;  /* sum of both value's bytes was identical */
}

/* PMIX_VALUE */
int pmix_bfrop_compare_value(pmix_value_t *value1, pmix_value_t *value2, pmix_data_type_t type)
{
    return PMIX_EQUAL;  /* eventually compare field to field */
}

/* PMIX_BUFFER */
int pmix_bfrop_compare_buffer_contents(pmix_buffer_t *value1, pmix_buffer_t *value2, pmix_data_type_t type)
{
    return PMIX_EQUAL;  /* eventually compare bytes in buffers */
}

/* PMIX_INFO */
int pmix_bfrop_compare_info(pmix_info_t *value1, pmix_info_t *value2, pmix_data_type_t type)
{
    int rc;
    
    if (strlen(value1->key) > strlen(value2->key)) {
        return PMIX_VALUE1_GREATER;
    }
    if (strlen(value2->key) > strlen(value1->key)) {
        return PMIX_VALUE2_GREATER;
    }
    if (0 != (rc = strcmp(value1->key, value2->key))) {
        if (0 < rc) {
        return PMIX_VALUE1_GREATER;
        }
        return PMIX_VALUE2_GREATER;
    }

    if (strlen(value1->value) > strlen(value2->value)) {
        return PMIX_VALUE1_GREATER;
    }
    if (strlen(value2->value) > strlen(value1->value)) {
        return PMIX_VALUE2_GREATER;
    }
    if (0 != (rc = strcmp(value1->value, value2->value))) {
        if (0 < rc) {
        return PMIX_VALUE1_GREATER;
        }
        return PMIX_VALUE2_GREATER;
    }

    return PMIX_EQUAL;
}

/* PMIX_APP */
int pmix_bfrop_compare_app(pmix_app_t *value1, pmix_app_t *value2, pmix_data_type_t type)
{
    int rc;
    
    if (strlen(value1->cmd) > strlen(value2->cmd)) {
        return PMIX_VALUE1_GREATER;
    }
    if (strlen(value2->cmd) > strlen(value1->cmd)) {
        return PMIX_VALUE2_GREATER;
    }
    if (0 != (rc = strcmp(value1->cmd, value2->cmd))) {
        if (0 < rc) {
        return PMIX_VALUE1_GREATER;
        }
        return PMIX_VALUE2_GREATER;
    }

    return PMIX_EQUAL;
}
