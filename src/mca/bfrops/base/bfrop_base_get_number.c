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
 * Copyright (c) 2015-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2019      Mellanox Technologies, Inc.
 *                         All rights reserved.
 * Copyright (c) 2021-2025 Nanook Consulting  All rights reserved.
 * Copyright (c) 2022      IBM Corporation.  All rights reserved.
 * Copyright (c) 2022      Triad National Security, LLC. All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "src/include/pmix_config.h"

#include <stdio.h>
#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif

#include "pmix.h"

#include "src/include/pmix_globals.h"
#include "src/util/pmix_error.h"

#include "src/mca/bfrops/base/base.h"


/* check differences in type between source and destination
 * to see if we lose precision or might change the sign of
 * the value - note that we already dealt with the case
 * where the source and destination have the same type */
static pmix_status_t check_size(const pmix_value_t *value,
                                void *dest, pmix_data_type_t type);

static pmix_status_t check_int(const pmix_value_t *value,
                               void *dest, pmix_data_type_t type);

static pmix_status_t check_int8(const pmix_value_t *value,
                                void *dest, pmix_data_type_t type);

static pmix_status_t check_int16(const pmix_value_t *value,
                                 void *dest, pmix_data_type_t type);

static pmix_status_t check_int32(const pmix_value_t *value,
                                 void *dest, pmix_data_type_t type);

static pmix_status_t check_int64(const pmix_value_t *value,
                                 void *dest, pmix_data_type_t type);

static pmix_status_t check_uint(const pmix_value_t *value,
                                void *dest, pmix_data_type_t type);

static pmix_status_t check_uint8(const pmix_value_t *value,
                                 void *dest, pmix_data_type_t type);

static pmix_status_t check_uint16(const pmix_value_t *value,
                                  void *dest, pmix_data_type_t type);

static pmix_status_t check_uint32(const pmix_value_t *value,
                                  void *dest, pmix_data_type_t type);

static pmix_status_t check_uint64(const pmix_value_t *value,
                                 void *dest, pmix_data_type_t type);

static pmix_status_t check_float(const pmix_value_t *value,
                                 void *dest, pmix_data_type_t type);

static pmix_status_t check_double(const pmix_value_t *value,
                                  void *dest, pmix_data_type_t type);

static pmix_status_t check_rank(const pmix_value_t *value,
                                void *dest, pmix_data_type_t type);

static pmix_status_t check_status(const pmix_value_t *value,
                                  void *dest, pmix_data_type_t type);

pmix_status_t PMIx_Value_get_number(const pmix_value_t *value,
                                    void *dest, pmix_data_type_t type)
{
    if (PMIX_SIZE == value->type) {
        if (PMIX_SIZE == type) {
            memcpy(dest, &value->data.size, sizeof(size_t));
            return PMIX_SUCCESS;
        } else {
            return check_size(value, dest, type);
        }
    }

    if (PMIX_INT == value->type) {
        if (PMIX_INT == type) {
            memcpy(dest, &value->data.integer, sizeof(int));
            return PMIX_SUCCESS;
        } else {
            return check_int(value, dest, type);
        }
    }

    if (PMIX_INT8 == value->type) {
        if (PMIX_INT8 == type) {
            memcpy(dest, &value->data.int8, sizeof(int8_t));
            return PMIX_SUCCESS;
        } else {
            return check_int8(value, dest, type);
        }
    }

    if (PMIX_INT16 == value->type) {
        if (PMIX_INT16 == type) {
            memcpy(dest, &value->data.int16, sizeof(int16_t));
            return PMIX_SUCCESS;
        } else {
            return check_int16(value, dest, type);
        }
    }

    if (PMIX_INT32 == value->type) {
        if (PMIX_INT32 == type) {
            memcpy(dest, &value->data.int32, sizeof(int32_t));
            return PMIX_SUCCESS;
        } else {
            return check_int32(value, dest, type);
        }
    }

    if (PMIX_INT64 == value->type) {
        if (PMIX_INT64 == type) {
            memcpy(dest, &value->data.int8, sizeof(int64_t));
            return PMIX_SUCCESS;
        } else {
            return check_int64(value, dest, type);
        }
    }

    if (PMIX_UINT == value->type) {
        if (PMIX_UINT == type) {
            memcpy(dest, &value->data.uint, sizeof(unsigned int));
            return PMIX_SUCCESS;
        } else {
            return check_uint(value, dest, type);
        }
    }

    if (PMIX_UINT8 == value->type) {
        if (PMIX_UINT8 == type) {
            memcpy(dest, &value->data.uint8, sizeof(uint8_t));
            return PMIX_SUCCESS;
        } else {
            return check_uint8(value, dest, type);
        }
    }

    if (PMIX_UINT16 == value->type) {
        if (PMIX_UINT16 == type) {
            memcpy(dest, &value->data.uint16, sizeof(uint16_t));
            return PMIX_SUCCESS;
        } else {
            return check_uint16(value, dest, type);
        }
    }

    if (PMIX_UINT32 == value->type) {
        if (PMIX_UINT32 == type) {
            memcpy(dest, &value->data.uint32, sizeof(uint32_t));
            return PMIX_SUCCESS;
        } else {
            return check_uint32(value, dest, type);
        }
    }

    if (PMIX_UINT64 == value->type) {
        if (PMIX_UINT64 == type) {
            memcpy(dest, &value->data.uint64, sizeof(uint64_t));
            return PMIX_SUCCESS;
        } else {
            return check_uint64(value, dest, type);
        }
    }

    if (PMIX_FLOAT == value->type) {
        if (PMIX_FLOAT == type) {
            memcpy(dest, &value->data.fval, sizeof(float));
            return PMIX_SUCCESS;
        } else {
            return check_float(value, dest, type);
        }
    }

    if (PMIX_DOUBLE == value->type) {
        if (PMIX_DOUBLE == type) {
            memcpy(dest, &value->data.dval, sizeof(double));
            return PMIX_SUCCESS;
        } else {
            return check_double(value, dest, type);
        }
    }

    if (PMIX_PID == value->type) {
        if (PMIX_PID == type) {
            memcpy(dest, &value->data.pid, sizeof(pid_t));
            return PMIX_SUCCESS;
        }
    }

    if (PMIX_PROC_RANK == value->type) {
        if (PMIX_PROC_RANK == type) {
            memcpy(dest, &value->data.rank, sizeof(pmix_rank_t));
            return PMIX_SUCCESS;
        } else {
            return check_rank(value, dest, type);
        }
    }

    if (PMIX_STATUS == value->type) {
        if (PMIX_STATUS== type) {
            memcpy(dest, &value->data.status, sizeof(pmix_status_t));
            return PMIX_SUCCESS;
        } else {
            return check_status(value, dest, type);
        }
    }

    /* if we get here, then the value is not a numeric type */
    return PMIX_ERR_BAD_PARAM;
}


static pmix_status_t check_size(const pmix_value_t *value,
                                void *dest, pmix_data_type_t type)
{
    int *i;
    int8_t *i8;
    int16_t *i16;
    int32_t *i32;
    int64_t *i64;
    unsigned int *ui;
    uint8_t *u8;
    uint16_t *u16;
    uint32_t *u32;
    uint64_t *u64;
    float *flt;
    double *dbl;
    pid_t *pid;
    pmix_status_t *ps;
    pmix_rank_t *pr;

    if (PMIX_INT == type) {
        // potentially out-of-range
        if (INT_MAX < value->data.size) {
            return PMIX_ERR_LOST_PRECISION;
        }
        i = (int*)dest;
        *i = (int)value->data.size;
        return PMIX_SUCCESS;
    }
    if (PMIX_INT8 == type) {
        if (INT8_MAX < value->data.size) {
            return PMIX_ERR_LOST_PRECISION;
        }
        i8 = (int8_t*)dest;
        *i8 = (int8_t)value->data.size;
        return PMIX_SUCCESS;
    }
    if (PMIX_INT16 == type) {
        if (INT16_MAX < value->data.size) {
            return PMIX_ERR_LOST_PRECISION;
        }
        i16 = (int16_t*)dest;
        *i16 = (int16_t)value->data.size;
        return PMIX_SUCCESS;
    }
    if (PMIX_INT32 == type) {
        if (INT32_MAX < value->data.size) {
            return PMIX_ERR_LOST_PRECISION;
        }
        i32 = (int32_t*)dest;
        *i32 = (int32_t)value->data.size;
        return PMIX_SUCCESS;
    }
    if (PMIX_INT64 == type) {
        if (INT64_MAX < value->data.size) {
            return PMIX_ERR_LOST_PRECISION;
        }
        i64 = (int64_t*)dest;
        *i64 = (int64_t)value->data.size;
        return PMIX_SUCCESS;
    }

    if (PMIX_UINT == type) {
        if (UINT_MAX < value->data.size) {
            return PMIX_ERR_LOST_PRECISION;
        }
        ui = (unsigned int*)dest;
        *ui = (unsigned int)value->data.size;
        return PMIX_SUCCESS;
    }
    if (PMIX_UINT8 == type) {
        if (UINT8_MAX < value->data.size) {
            return PMIX_ERR_LOST_PRECISION;
        }
        u8 = (uint8_t*)dest;
        *u8 = (uint8_t)value->data.size;
        return PMIX_SUCCESS;
    }
    if (PMIX_UINT16 == type) {
        if (UINT16_MAX < value->data.size) {
            return PMIX_ERR_LOST_PRECISION;
        }
        u16 = (uint16_t*)dest;
        *u16 = (uint16_t)value->data.size;
        return PMIX_SUCCESS;
    }
    if (PMIX_UINT32 == type) {
        if (UINT32_MAX < value->data.size) {
            return PMIX_ERR_LOST_PRECISION;
        }
        u32 = (uint32_t*)dest;
        *u32 = (uint32_t)value->data.size;
        return PMIX_SUCCESS;
    }
    if (PMIX_UINT64 == type) {
        if (UINT64_MAX < value->data.size) {
            return PMIX_ERR_LOST_PRECISION;
        }
        u64 = (uint64_t*)dest;
        *u64 = (uint64_t)value->data.size;
        return PMIX_SUCCESS;
    }

    if (PMIX_FLOAT == type) {
        flt = (float*)dest;
        *flt = (float)value->data.size;
        return PMIX_SUCCESS;
    }
    if (PMIX_DOUBLE == type) {
        dbl = (double*)dest;
        *dbl = (double)value->data.size;
        return PMIX_SUCCESS;
    }

    if (PMIX_PID == type) {
        // pid_t is a signed int
        if (INT_MAX < value->data.size) {
            return PMIX_ERR_LOST_PRECISION;
        }
        pid = (pid_t*)dest;
        *pid = (pid_t)value->data.size;
        return PMIX_SUCCESS;
    }
    if (PMIX_STATUS == type) {
        // status is a signed int
        if (INT_MAX < value->data.size) {
            return PMIX_ERR_LOST_PRECISION;
        }
        ps = (pmix_status_t*)dest;
        *ps = (pmix_status_t)value->data.size;
        return PMIX_SUCCESS;
    }
    if (PMIX_PROC_RANK == type) {
        // rank is a uint32
        if (UINT32_MAX < value->data.size) {
            return PMIX_ERR_LOST_PRECISION;
        }
        pr = (pmix_rank_t*)dest;
        *pr = (pmix_rank_t)value->data.size;
    }

    // get here if the destination is a non-numerical type
    return PMIX_ERR_BAD_PARAM;
}

static pmix_status_t check_int(const pmix_value_t *value,
                               void *dest, pmix_data_type_t type)
{
    int8_t *i8;
    int16_t *i16;
    int32_t *i32;
    int64_t *i64;
    unsigned int *ui;
    uint8_t *u8;
    uint16_t *u16;
    uint32_t *u32;
    uint64_t *u64;
    size_t *sz;
    float *flt;
    double *dbl;
    pid_t *pid;
    pmix_status_t *ps;
    pmix_rank_t *pr;

    // check if this xfer would change sign
    if (0 > value->data.integer) {
        if (PMIX_SIZE == type ||
            PMIX_UINT == type ||
            PMIX_UINT8 == type ||
            PMIX_UINT16 == type ||
            PMIX_UINT32 == type ||
            PMIX_UINT64 == type ||
            PMIX_PROC_RANK == type) {
            return PMIX_ERR_CHANGE_SIGN;
        }
    }

    /* already took care of the negative value case when
     * transferring to unsigned types, and no loss of
     * precision if the dest is equal in size or larger */

    if (PMIX_INT64 == type) {
        i64 = (int64_t*)dest;
        *i64 = (int64_t)value->data.integer;
        return PMIX_SUCCESS;
    }
    if (PMIX_UINT == type) {
        ui = (unsigned int*)dest;
        *ui = (unsigned int)value->data.integer;
        return PMIX_SUCCESS;
    }
    if (PMIX_UINT64 == type) {
        u64 = (uint64_t*)dest;
        *u64 = (uint64_t)value->data.integer;
        return PMIX_SUCCESS;
    }
    if (PMIX_SIZE == type) {
        sz = (size_t*)dest;
        *sz = (size_t)value->data.integer;
        return PMIX_SUCCESS;
    }
    if (PMIX_FLOAT == type) {
        flt = (float*)dest;
        *flt = (float)value->data.integer;
        return PMIX_SUCCESS;
    }
    if (PMIX_DOUBLE == type) {
        dbl = (double*)dest;
        *dbl = (double)value->data.integer;
        return PMIX_SUCCESS;
    }
    if (PMIX_PID == type) {
        // pid_t is a signed int
        pid = (pid_t*)dest;
        *pid = (pid_t)value->data.integer;
        return PMIX_SUCCESS;
    }
    if (PMIX_STATUS == type) {
        // status is a signed int
        ps = (pmix_status_t*)dest;
        *ps = (pmix_status_t)value->data.integer;
        return PMIX_SUCCESS;
    }
    if (PMIX_PROC_RANK == type) {
        // rank is an unsigned int
        pr = (pmix_rank_t*)dest;
        *pr = (pmix_rank_t)value->data.integer;
    }

    // if we get here, then we are dealing with a smaller
    // destination, which means we can lose precision
    if (PMIX_INT8 == type) {
        if (0 < value->data.integer) {
            if (INT8_MAX < value->data.integer) {
                return PMIX_ERR_LOST_PRECISION;
            }
        } else {
            if (INT8_MIN > value->data.integer) {
                return PMIX_ERR_LOST_PRECISION;
            }
        }
        i8 = (int8_t*)dest;
        *i8 = (int8_t)value->data.integer;
        return PMIX_SUCCESS;
    }
    if (PMIX_INT16 == type) {
        if (0 < value->data.integer) {
            if (INT16_MAX < value->data.integer) {
                return PMIX_ERR_LOST_PRECISION;
            }
        } else {
            if (INT16_MIN > value->data.integer) {
                return PMIX_ERR_LOST_PRECISION;
            }
        }
        i16 = (int16_t*)dest;
        *i16 = (int16_t)value->data.integer;
        return PMIX_SUCCESS;
    }
    if (PMIX_INT32 == type) {
        if (0 < value->data.integer) {
            if (INT32_MAX < value->data.integer) {
                return PMIX_ERR_LOST_PRECISION;
            }
        } else {
            if (INT32_MIN > value->data.integer) {
                return PMIX_ERR_LOST_PRECISION;
            }
        }
        i32 = (int32_t*)dest;
        *i32 = (int32_t)value->data.integer;
        return PMIX_SUCCESS;
    }

    if (PMIX_UINT8 == type) {
        if (UINT8_MAX < value->data.integer) {
            return PMIX_ERR_LOST_PRECISION;
        }
        u8 = (uint8_t*)dest;
        *u8 = (uint8_t)value->data.integer;
        return PMIX_SUCCESS;
    }
    if (PMIX_UINT16 == type) {
        if (UINT16_MAX < value->data.integer) {
            return PMIX_ERR_LOST_PRECISION;
        }
        u16 = (uint16_t*)dest;
        *u16 = (uint16_t)value->data.integer;
        return PMIX_SUCCESS;
    }
    if (PMIX_UINT32 == type) {
        u32 = (uint32_t*)dest;
        *u32 = (uint32_t)value->data.integer;
        return PMIX_SUCCESS;
    }

    // get here if the destination is a non-numerical type
    return PMIX_ERR_BAD_PARAM;
}

static pmix_status_t check_int8(const pmix_value_t *value,
                                void *dest, pmix_data_type_t type)
{   int *i;
    int16_t *i16;
    int32_t *i32;
    int64_t *i64;
    unsigned int *ui;
    uint8_t *u8;
    uint16_t *u16;
    uint32_t *u32;
    uint64_t *u64;
    size_t *sz;
    float *flt;
    double *dbl;
    pid_t *pid;
    pmix_status_t *ps;
    pmix_rank_t *pr;

    // check if this xfer would change sign
    if (0 > value->data.int8) {
        if (PMIX_SIZE == type ||
            PMIX_UINT == type ||
            PMIX_UINT8 == type ||
            PMIX_UINT16 == type ||
            PMIX_UINT32 == type ||
            PMIX_UINT64 == type ||
            PMIX_PROC_RANK == type) {
            return PMIX_ERR_CHANGE_SIGN;
        }
    }

    // everything is at least 8-bits
    if (PMIX_INT == type) {
        i = (int*)dest;
        *i = (int)value->data.int8;
        return PMIX_SUCCESS;
    }
    if (PMIX_INT16 == type) {
        i16 = (int16_t*)dest;
        *i16 = (int16_t)value->data.int8;
        return PMIX_SUCCESS;
    }
    if (PMIX_INT32 == type) {
        i32 = (int32_t*)dest;
        *i32 = (int32_t)value->data.int8;
        return PMIX_SUCCESS;
    }
    if (PMIX_INT64 == type) {
        i64 = (int64_t*)dest;
        *i64 = (int64_t)value->data.int8;
        return PMIX_SUCCESS;
    }
    if (PMIX_UINT == type) {
        ui = (unsigned int*)dest;
        *ui = (unsigned int)value->data.int8;
        return PMIX_SUCCESS;
    }
    if (PMIX_UINT8 == type) {
        u8 = (uint8_t*)dest;
        *u8 = (uint8_t)value->data.int8;
        return PMIX_SUCCESS;
    }
    if (PMIX_UINT16 == type) {
        u16 = (uint16_t*)dest;
        *u16 = (uint16_t)value->data.int8;
        return PMIX_SUCCESS;
    }
    if (PMIX_UINT32 == type) {
        u32 = (uint32_t*)dest;
        *u32 = (uint32_t)value->data.int8;
        return PMIX_SUCCESS;
    }
    if (PMIX_UINT64 == type) {
        u64 = (uint64_t*)dest;
        *u64 = (uint64_t)value->data.int8;
        return PMIX_SUCCESS;
    }
    if (PMIX_SIZE == type) {
        sz = (size_t*)dest;
        *sz = (size_t)value->data.int8;
        return PMIX_SUCCESS;
    }
    if (PMIX_FLOAT == type) {
        flt = (float*)dest;
        *flt = (float)value->data.int8;
        return PMIX_SUCCESS;
    }
    if (PMIX_DOUBLE == type) {
        dbl = (double*)dest;
        *dbl = (double)value->data.int8;
        return PMIX_SUCCESS;
    }
    if (PMIX_PID == type) {
        // pid_t is a signed int
        pid = (pid_t*)dest;
        *pid = (pid_t)value->data.int8;
        return PMIX_SUCCESS;
    }
    if (PMIX_STATUS == type) {
        // status is a signed int
        ps = (pmix_status_t*)dest;
        *ps = (pmix_status_t)value->data.int8;
        return PMIX_SUCCESS;
    }
    if (PMIX_PROC_RANK == type) {
        pr = (pmix_rank_t*)dest;
        *pr = (pmix_rank_t)value->data.int8;
    }

    // get here if the destination is a non-numerical type
    return PMIX_ERR_BAD_PARAM;
}

static pmix_status_t check_int16(const pmix_value_t *value,
                                 void *dest, pmix_data_type_t type)
{
    int *i;
    int8_t *i8;
    int32_t *i32;
    int64_t *i64;
    unsigned int *ui;
    uint8_t *u8;
    uint16_t *u16;
    uint32_t *u32;
    uint64_t *u64;
    size_t *sz;
    float *flt;
    double *dbl;
    pid_t *pid;
    pmix_status_t *ps;
    pmix_rank_t *pr;

    // check if this xfer would change sign
    if (0 > value->data.integer) {
        if (PMIX_SIZE == type ||
            PMIX_UINT == type ||
            PMIX_UINT8 == type ||
            PMIX_UINT16 == type ||
            PMIX_UINT32 == type ||
            PMIX_UINT64 == type ||
            PMIX_PROC_RANK == type) {
            return PMIX_ERR_CHANGE_SIGN;
        }
    }

    /* already took care of the negative value case when
     * transferring to unsigned types, and no loss of
     * precision if the dest is equal in size or larger */

    if (PMIX_INT == type) {
        i = (int*)dest;
        *i = (int)value->data.int16;
        return PMIX_SUCCESS;
    }
    if (PMIX_INT32 == type) {
        i32 = (int32_t*)dest;
        *i32 = (int32_t)value->data.int16;
        return PMIX_SUCCESS;
    }
    if (PMIX_INT64 == type) {
        i64 = (int64_t*)dest;
        *i64 = (int64_t)value->data.int16;
        return PMIX_SUCCESS;
    }
     if (PMIX_UINT == type) {
        ui = (unsigned int*)dest;
        *ui = (unsigned int)value->data.int16;
        return PMIX_SUCCESS;
    }
    if (PMIX_UINT16 == type) {
        u16 = (uint16_t*)dest;
        *u16 = (uint16_t)value->data.int16;
        return PMIX_SUCCESS;
    }
   if (PMIX_UINT32 == type) {
        u32 = (uint32_t*)dest;
        *u32 = (uint32_t)value->data.int16;
        return PMIX_SUCCESS;
    }
    if (PMIX_UINT64 == type) {
        u64 = (uint64_t*)dest;
        *u64 = (uint64_t)value->data.int16;
        return PMIX_SUCCESS;
    }
    if (PMIX_SIZE == type) {
        sz = (size_t*)dest;
        *sz = (size_t)value->data.int16;
        return PMIX_SUCCESS;
    }
    if (PMIX_FLOAT == type) {
        flt = (float*)dest;
        *flt = (float)value->data.int16;
        return PMIX_SUCCESS;
    }
    if (PMIX_DOUBLE == type) {
        dbl = (double*)dest;
        *dbl = (double)value->data.int16;
        return PMIX_SUCCESS;
    }
    if (PMIX_PID == type) {
        // pid_t is a signed int
        pid = (pid_t*)dest;
        *pid = (pid_t)value->data.int16;
        return PMIX_SUCCESS;
    }
    if (PMIX_STATUS == type) {
        // status is a signed int
        ps = (pmix_status_t*)dest;
        *ps = (pmix_status_t)value->data.int16;
        return PMIX_SUCCESS;
    }
    if (PMIX_PROC_RANK == type) {
        // rank is an unsigned int
        pr = (pmix_rank_t*)dest;
        *pr = (pmix_rank_t)value->data.int16;
    }

    // if we get here, then we are dealing with a smaller
    // destination, which means we can lose precision
    if (PMIX_INT8 == type) {
        if (0 < value->data.int16) {
            if (INT8_MAX < value->data.int16) {
                return PMIX_ERR_LOST_PRECISION;
            }
        } else {
            if (INT8_MIN > value->data.int16) {
                return PMIX_ERR_LOST_PRECISION;
            }
        }
        i8 = (int8_t*)dest;
        *i8 = (int8_t)value->data.int16;
        return PMIX_SUCCESS;
    }
    if (PMIX_UINT8 == type) {
        if (UINT8_MAX < value->data.int16) {
            return PMIX_ERR_LOST_PRECISION;
        }
        u8 = (uint8_t*)dest;
        *u8 = (uint8_t)value->data.int16;
        return PMIX_SUCCESS;
    }

    // get here if the destination is a non-numerical type
    return PMIX_ERR_BAD_PARAM;
}

static pmix_status_t check_int32(const pmix_value_t *value,
                                 void *dest, pmix_data_type_t type)
{
    int *i;
    int8_t *i8;
    int16_t *i16;
    int64_t *i64;
    unsigned int *ui;
    uint8_t *u8;
    uint16_t *u16;
    uint32_t *u32;
    uint64_t *u64;
    size_t *sz;
    float *flt;
    double *dbl;
    pid_t *pid;
    pmix_status_t *ps;
    pmix_rank_t *pr;

    // check if this xfer would change sign
    if (0 > value->data.integer) {
        if (PMIX_SIZE == type ||
            PMIX_UINT == type ||
            PMIX_UINT8 == type ||
            PMIX_UINT16 == type ||
            PMIX_UINT32 == type ||
            PMIX_UINT64 == type ||
            PMIX_PROC_RANK == type) {
            return PMIX_ERR_CHANGE_SIGN;
        }
    }

    /* already took care of the negative value case when
     * transferring to unsigned types, and no loss of
     * precision if the dest is equal in size or larger */

    if (PMIX_INT == type) {
        i = (int*)dest;
        *i = (int)value->data.int32;
        return PMIX_SUCCESS;
    }
    if (PMIX_INT64 == type) {
        i64 = (int64_t*)dest;
        *i64 = (int64_t)value->data.int32;
        return PMIX_SUCCESS;
    }
     if (PMIX_UINT == type) {
        ui = (unsigned int*)dest;
        *ui = (unsigned int)value->data.int32;
        return PMIX_SUCCESS;
    }
    if (PMIX_UINT32 == type) {
        u32 = (uint32_t*)dest;
        *u32 = (uint32_t)value->data.int32;
        return PMIX_SUCCESS;
    }
    if (PMIX_UINT64 == type) {
        u64 = (uint64_t*)dest;
        *u64 = (uint64_t)value->data.int32;
        return PMIX_SUCCESS;
    }
    if (PMIX_SIZE == type) {
        sz = (size_t*)dest;
        *sz = (size_t)value->data.int32;
        return PMIX_SUCCESS;
    }
    if (PMIX_FLOAT == type) {
        flt = (float*)dest;
        *flt = (float)value->data.int32;
        return PMIX_SUCCESS;
    }
    if (PMIX_DOUBLE == type) {
        dbl = (double*)dest;
        *dbl = (double)value->data.int32;
        return PMIX_SUCCESS;
    }
    if (PMIX_PID == type) {
        // pid_t is a signed int
        pid = (pid_t*)dest;
        *pid = (pid_t)value->data.int32;
        return PMIX_SUCCESS;
    }
    if (PMIX_STATUS == type) {
        // status is a signed int
        ps = (pmix_status_t*)dest;
        *ps = (pmix_status_t)value->data.int32;
        return PMIX_SUCCESS;
    }
    if (PMIX_PROC_RANK == type) {
        // rank is an unsigned int
        pr = (pmix_rank_t*)dest;
        *pr = (pmix_rank_t)value->data.int32;
    }

    // if we get here, then we are dealing with a smaller
    // destination, which means we can lose precision
    if (PMIX_INT8 == type) {
        if (0 < value->data.int16) {
            if (INT8_MAX < value->data.int16) {
                return PMIX_ERR_LOST_PRECISION;
            }
        } else {
            if (INT8_MIN > value->data.int16) {
                return PMIX_ERR_LOST_PRECISION;
            }
        }
        i8 = (int8_t*)dest;
        *i8 = (int8_t)value->data.int16;
        return PMIX_SUCCESS;
    }
    if (PMIX_INT16 == type) {
        if (0 < value->data.int32) {
            if (INT16_MAX < value->data.int32) {
                return PMIX_ERR_LOST_PRECISION;
            }
        } else {
            if (INT16_MIN > value->data.int32) {
                return PMIX_ERR_LOST_PRECISION;
            }
        }
        i16 = (int16_t*)dest;
        *i16 = (int16_t)value->data.int32;
        return PMIX_SUCCESS;
    }
    if (PMIX_UINT8 == type) {
        if (UINT8_MAX < value->data.int16) {
            return PMIX_ERR_LOST_PRECISION;
        }
        u8 = (uint8_t*)dest;
        *u8 = (uint8_t)value->data.int16;
        return PMIX_SUCCESS;
    }
    if (PMIX_UINT16 == type) {
        if (UINT16_MAX < value->data.int32) {
            return PMIX_ERR_LOST_PRECISION;
        }
        u16 = (uint16_t*)dest;
        *u16 = (uint16_t)value->data.int32;
        return PMIX_SUCCESS;
    }

    // get here if the destination is a non-numerical type
    return PMIX_ERR_BAD_PARAM;
}

static pmix_status_t check_int64(const pmix_value_t *value,
                                 void *dest, pmix_data_type_t type)
{
    int *i;
    int8_t *i8;
    int16_t *i16;
    int32_t *i32;
    unsigned int *ui;
    uint8_t *u8;
    uint16_t *u16;
    uint32_t *u32;
    uint64_t *u64;
    size_t *sz;
    float *flt;
    double *dbl;
    pid_t *pid;
    pmix_status_t *ps;
    pmix_rank_t *pr;

    // check if this xfer would change sign
    if (0 > value->data.integer) {
        if (PMIX_SIZE == type ||
            PMIX_UINT == type ||
            PMIX_UINT8 == type ||
            PMIX_UINT16 == type ||
            PMIX_UINT32 == type ||
            PMIX_UINT64 == type ||
            PMIX_PROC_RANK == type) {
            return PMIX_ERR_CHANGE_SIGN;
        }
    }

    /* already took care of the negative value case when
     * transferring to unsigned types, and no loss of
     * precision if the dest is equal in size or larger */
    if (PMIX_UINT64 == type) {
        u64 = (uint64_t*)dest;
        *u64 = (uint64_t)value->data.int64;
        return PMIX_SUCCESS;
    }
    if (PMIX_SIZE == type) {
        sz = (size_t*)dest;
        *sz = (size_t)value->data.int64;
        return PMIX_SUCCESS;
    }
    if (PMIX_FLOAT == type) {
        flt = (float*)dest;
        *flt = (float)value->data.int64;
        return PMIX_SUCCESS;
    }
    if (PMIX_DOUBLE == type) {
        dbl = (double*)dest;
        *dbl = (double)value->data.int64;
        return PMIX_SUCCESS;
    }

    // if we get here, then we are dealing with a smaller
    // destination, which means we can lose precision
    if (PMIX_INT == type) {
        if (0 < value->data.int64) {
            if (INT_MAX < value->data.int64) {
                return PMIX_ERR_LOST_PRECISION;
            }
        } else {
            if (INT_MIN > value->data.int64) {
                return PMIX_ERR_LOST_PRECISION;
            }
        }
        i = (int*)dest;
        *i = (int)value->data.int64;
        return PMIX_SUCCESS;
    }
    if (PMIX_INT8 == type) {
        if (0 < value->data.int64) {
            if (INT8_MAX < value->data.int64) {
                return PMIX_ERR_LOST_PRECISION;
            }
        } else {
            if (INT8_MIN > value->data.int64) {
                return PMIX_ERR_LOST_PRECISION;
            }
        }
        i8 = (int8_t*)dest;
        *i8 = (int8_t)value->data.int64;
        return PMIX_SUCCESS;
    }
    if (PMIX_INT16 == type) {
        if (0 < value->data.int64) {
            if (INT16_MAX < value->data.int64) {
                return PMIX_ERR_LOST_PRECISION;
            }
        } else {
            if (INT16_MIN > value->data.int64) {
                return PMIX_ERR_LOST_PRECISION;
            }
        }
        i16 = (int16_t*)dest;
        *i16 = (int16_t)value->data.int64;
        return PMIX_SUCCESS;
    }
    if (PMIX_INT32 == type) {
        if (0 < value->data.int64) {
            if (INT32_MAX < value->data.int64) {
                return PMIX_ERR_LOST_PRECISION;
            }
        } else {
            if (INT32_MIN > value->data.int64) {
                return PMIX_ERR_LOST_PRECISION;
            }
        }
        i32 = (int32_t*)dest;
        *i32 = (int32_t)value->data.int64;
        return PMIX_SUCCESS;
    }
    if (PMIX_UINT == type) {
        if (0 < value->data.int64) {
            if (UINT_MAX < value->data.int64) {
                return PMIX_ERR_LOST_PRECISION;
            }
        }
        ui = (unsigned int*)dest;
        *ui = (unsigned int)value->data.int64;
        return PMIX_SUCCESS;
    }
    if (PMIX_UINT8 == type) {
        if (UINT8_MAX < value->data.int64) {
            return PMIX_ERR_LOST_PRECISION;
        }
        u8 = (uint8_t*)dest;
        *u8 = (uint8_t)value->data.int64;
        return PMIX_SUCCESS;
    }
    if (PMIX_UINT16 == type) {
        if (UINT16_MAX < value->data.int64) {
            return PMIX_ERR_LOST_PRECISION;
        }
        u16 = (uint16_t*)dest;
        *u16 = (uint16_t)value->data.int64;
        return PMIX_SUCCESS;
    }
    if (PMIX_UINT32 == type) {
        if (0 < value->data.int64) {
            if (UINT32_MAX < value->data.int64) {
                return PMIX_ERR_LOST_PRECISION;
            }
        }
        u32 = (uint32_t*)dest;
        *u32 = (uint32_t)value->data.int64;
        return PMIX_SUCCESS;
    }

    if (PMIX_PID == type) {
        // pid_t is a signed int
        if (0 < value->data.int64) {
            if (UINT32_MAX < value->data.int64) {
                return PMIX_ERR_LOST_PRECISION;
            }
        }
        pid = (pid_t*)dest;
        *pid = (pid_t)value->data.int64;
        return PMIX_SUCCESS;
    }
    if (PMIX_STATUS == type) {
        // status is a signed int
        if (0 < value->data.int64) {
            if (UINT32_MAX < value->data.int64) {
                return PMIX_ERR_LOST_PRECISION;
            }
        }
        ps = (pmix_status_t*)dest;
        *ps = (pmix_status_t)value->data.int64;
        return PMIX_SUCCESS;
    }
    if (PMIX_PROC_RANK == type) {
        // rank is a uint32
        if (UINT32_MAX < value->data.int64) {
            return PMIX_ERR_LOST_PRECISION;
        }
        pr = (pmix_rank_t*)dest;
        *pr = (pmix_rank_t)value->data.int64;
    }

    // get here if the destination is a non-numerical type
    return PMIX_ERR_BAD_PARAM;
}

static pmix_status_t check_uint(const pmix_value_t *value,
                                void *dest, pmix_data_type_t type)
{
    int *i;
    int8_t *i8;
    int16_t *i16;
    int32_t *i32;
    int64_t *i64;
    uint8_t *u8;
    uint16_t *u16;
    uint32_t *u32;
    uint64_t *u64;
    size_t *sz;
    float *flt;
    double *dbl;
    pid_t *pid;
    pmix_status_t *ps;
    pmix_rank_t *pr;

    /* no negative value to be concerned about, and no loss of
     * precision if the dest is equal in size or larger */
    if (PMIX_INT64 == type) {
        i64 = (int64_t*)dest;
        *i64 = (int64_t)value->data.uint;
        return PMIX_SUCCESS;
    }
    if (PMIX_UINT64 == type) {
        u64 = (uint64_t*)dest;
        *u64 = (uint64_t)value->data.uint;
        return PMIX_SUCCESS;
    }
    if (PMIX_SIZE == type) {
        sz = (size_t*)dest;
        *sz = (size_t)value->data.uint;
        return PMIX_SUCCESS;
    }
    if (PMIX_FLOAT == type) {
        flt = (float*)dest;
        *flt = (float)value->data.uint;
        return PMIX_SUCCESS;
    }
    if (PMIX_DOUBLE == type) {
        dbl = (double*)dest;
        *dbl = (double)value->data.uint;
        return PMIX_SUCCESS;
    }
    if (PMIX_PROC_RANK == type) {
        // rank is an unsigned int
        pr = (pmix_rank_t*)dest;
        *pr = (pmix_rank_t)value->data.uint;
    }

    // if we get here, then we are dealing with a smaller
    // destination, which means we can lose precision
    if (PMIX_INT == type) {
        if (INT_MAX < value->data.uint) {
            return PMIX_ERR_LOST_PRECISION;
        }
        i = (int*)dest;
        *i = (int)value->data.uint;
        return PMIX_SUCCESS;
    }
    if (PMIX_INT8 == type) {
        if (INT8_MAX < value->data.uint) {
            return PMIX_ERR_LOST_PRECISION;
        }
        i8 = (int8_t*)dest;
        *i8 = (int8_t)value->data.uint;
        return PMIX_SUCCESS;
    }
    if (PMIX_INT16 == type) {
        if (INT16_MAX < value->data.uint) {
            return PMIX_ERR_LOST_PRECISION;
        }
        i16 = (int16_t*)dest;
        *i16 = (int16_t)value->data.uint;
        return PMIX_SUCCESS;
    }
    if (PMIX_INT32 == type) {
        if (INT32_MAX < value->data.uint) {
            return PMIX_ERR_LOST_PRECISION;
        }
        i32 = (int32_t*)dest;
        *i32 = (int32_t)value->data.uint;
        return PMIX_SUCCESS;
    }

    if (PMIX_UINT8 == type) {
        if (UINT8_MAX < value->data.uint) {
            return PMIX_ERR_LOST_PRECISION;
        }
        u8 = (uint8_t*)dest;
        *u8 = (uint8_t)value->data.uint;
        return PMIX_SUCCESS;
    }
    if (PMIX_UINT16 == type) {
        if (UINT16_MAX < value->data.uint) {
            return PMIX_ERR_LOST_PRECISION;
        }
        u16 = (uint16_t*)dest;
        *u16 = (uint16_t)value->data.uint;
        return PMIX_SUCCESS;
    }
    if (PMIX_UINT32 == type) {
        u32 = (uint32_t*)dest;
        *u32 = (uint32_t)value->data.uint;
        return PMIX_SUCCESS;
    }

    if (PMIX_PID == type) {
        // pid_t is a signed int
        if (INT_MAX < value->data.uint) {
            return PMIX_ERR_LOST_PRECISION;
        }
        pid = (pid_t*)dest;
        *pid = (pid_t)value->data.uint;
        return PMIX_SUCCESS;
    }
    if (PMIX_STATUS == type) {
        // status is a signed int
        if (INT_MAX < value->data.uint) {
            return PMIX_ERR_LOST_PRECISION;
        }
        ps = (pmix_status_t*)dest;
        *ps = (pmix_status_t)value->data.uint;
        return PMIX_SUCCESS;
    }

    // get here if the destination is a non-numerical type
    return PMIX_ERR_BAD_PARAM;
}

static pmix_status_t check_uint8(const pmix_value_t *value,
                                 void *dest, pmix_data_type_t type)
{
    int *i;
    int8_t *i8;
    int16_t *i16;
    int32_t *i32;
    int64_t *i64;
    unsigned int *u;
    uint16_t *u16;
    uint32_t *u32;
    uint64_t *u64;
    size_t *sz;
    float *flt;
    double *dbl;
    pid_t *pid;
    pmix_status_t *ps;
    pmix_rank_t *pr;

    /* no negative value to be concerned about, and no loss of
     * precision since everything is at least 8 bits */
    if (PMIX_INT == type) {
        i = (int*)dest;
        *i = (int)value->data.uint8;
        return PMIX_SUCCESS;
    }
    if (PMIX_INT8 == type) {
        if (INT8_MAX < value->data.uint8) {
            return PMIX_ERR_LOST_PRECISION;
        }
        i8 = (int8_t*)dest;
        *i8 = (int8_t)value->data.uint8;
        return PMIX_SUCCESS;
    }
    if (PMIX_INT16 == type) {
        i16 = (int16_t*)dest;
        *i16 = (int16_t)value->data.uint8;
        return PMIX_SUCCESS;
    }
    if (PMIX_INT32 == type) {
        i32 = (int32_t*)dest;
        *i32 = (int32_t)value->data.uint8;
        return PMIX_SUCCESS;
    }
    if (PMIX_INT64 == type) {
        i64 = (int64_t*)dest;
        *i64 = (int64_t)value->data.uint8;
        return PMIX_SUCCESS;
    }

    if (PMIX_UINT == type) {
        u = (unsigned int *)dest;
        *u = (unsigned int)value->data.uint8;
        return PMIX_SUCCESS;
    }
    if (PMIX_UINT16 == type) {
        u16 = (uint16_t*)dest;
        *u16 = (uint16_t)value->data.uint8;
        return PMIX_SUCCESS;
    }
    if (PMIX_UINT32 == type) {
        u32 = (uint32_t*)dest;
        *u32 = (uint32_t)value->data.uint8;
        return PMIX_SUCCESS;
    }
    if (PMIX_UINT64 == type) {
        u64 = (uint64_t*)dest;
        *u64 = (uint64_t)value->data.uint8;
        return PMIX_SUCCESS;
    }

    if (PMIX_SIZE == type) {
        sz = (size_t*)dest;
        *sz = (size_t)value->data.uint8;
        return PMIX_SUCCESS;
    }
    if (PMIX_FLOAT == type) {
        flt = (float*)dest;
        *flt = (float)value->data.uint8;
        return PMIX_SUCCESS;
    }
    if (PMIX_DOUBLE == type) {
        dbl = (double*)dest;
        *dbl = (double)value->data.uint8;
        return PMIX_SUCCESS;
    }
    if (PMIX_PROC_RANK == type) {
        // rank is an unsigned int
        pr = (pmix_rank_t*)dest;
        *pr = (pmix_rank_t)value->data.uint8;
    }

    if (PMIX_PID == type) {
        // pid_t is a signed int
        pid = (pid_t*)dest;
        *pid = (pid_t)value->data.uint8;
        return PMIX_SUCCESS;
    }
    if (PMIX_STATUS == type) {
        // status is a signed int
        ps = (pmix_status_t*)dest;
        *ps = (pmix_status_t)value->data.uint8;
        return PMIX_SUCCESS;
    }

    // get here if the destination is a non-numerical type
    return PMIX_ERR_BAD_PARAM;
}

static pmix_status_t check_uint16(const pmix_value_t *value,
                                  void *dest, pmix_data_type_t type)
{
    int *i;
    int8_t *i8;
    int16_t *i16;
    int32_t *i32;
    int64_t *i64;
    unsigned int *u;
    uint8_t *u8;
    uint32_t *u32;
    uint64_t *u64;
    size_t *sz;
    float *flt;
    double *dbl;
    pid_t *pid;
    pmix_status_t *ps;
    pmix_rank_t *pr;

    /* no negative value to be concerned about */
    if (PMIX_INT == type) {
        i = (int*)dest;
        *i = (int)value->data.uint16;
        return PMIX_SUCCESS;
    }
    if (PMIX_INT8 == type) {
        if (INT8_MAX < value->data.uint16) {
            return PMIX_ERR_LOST_PRECISION;
        }
        i8 = (int8_t*)dest;
        *i8 = (int8_t)value->data.uint16;
        return PMIX_SUCCESS;
    }
    if (PMIX_INT16 == type) {
        if (INT16_MAX < value->data.uint16) {
            return PMIX_ERR_LOST_PRECISION;
        }
        i16 = (int16_t*)dest;
        *i16 = (int16_t)value->data.uint16;
        return PMIX_SUCCESS;
    }
    if (PMIX_INT32 == type) {
        i32 = (int32_t*)dest;
        *i32 = (int32_t)value->data.uint16;
        return PMIX_SUCCESS;
    }
    if (PMIX_INT64 == type) {
        i64 = (int64_t*)dest;
        *i64 = (int64_t)value->data.uint16;
        return PMIX_SUCCESS;
    }

    if (PMIX_UINT == type) {
        u = (unsigned int *)dest;
        *u = (unsigned int)value->data.uint16;
        return PMIX_SUCCESS;
    }
    if (PMIX_UINT8 == type) {
        if (UINT8_MAX < value->data.uint16) {
            return PMIX_ERR_LOST_PRECISION;
        }
        u8 = (uint8_t*)dest;
        *u8 = (uint8_t)value->data.uint16;
        return PMIX_SUCCESS;
    }
    if (PMIX_UINT32 == type) {
        u32 = (uint32_t*)dest;
        *u32 = (uint32_t)value->data.uint16;
        return PMIX_SUCCESS;
    }
    if (PMIX_UINT64 == type) {
        u64 = (uint64_t*)dest;
        *u64 = (uint64_t)value->data.uint16;
        return PMIX_SUCCESS;
    }

    if (PMIX_SIZE == type) {
        sz = (size_t*)dest;
        *sz = (size_t)value->data.uint16;
        return PMIX_SUCCESS;
    }
    if (PMIX_FLOAT == type) {
        flt = (float*)dest;
        *flt = (float)value->data.uint16;
        return PMIX_SUCCESS;
    }
    if (PMIX_DOUBLE == type) {
        dbl = (double*)dest;
        *dbl = (double)value->data.uint16;
        return PMIX_SUCCESS;
    }
    if (PMIX_PROC_RANK == type) {
        // rank is an unsigned int
        pr = (pmix_rank_t*)dest;
        *pr = (pmix_rank_t)value->data.uint16;
    }

    if (PMIX_PID == type) {
        // pid_t is a signed int
        pid = (pid_t*)dest;
        *pid = (pid_t)value->data.uint16;
        return PMIX_SUCCESS;
    }
    if (PMIX_STATUS == type) {
        // status is a signed int
        ps = (pmix_status_t*)dest;
        *ps = (pmix_status_t)value->data.uint16;
        return PMIX_SUCCESS;
    }

    // get here if the destination is a non-numerical type
    return PMIX_ERR_BAD_PARAM;
}

static pmix_status_t check_uint32(const pmix_value_t *value,
                                  void *dest, pmix_data_type_t type)
{
    int *i;
    int8_t *i8;
    int16_t *i16;
    int32_t *i32;
    int64_t *i64;
    unsigned int *u;
    uint8_t *u8;
    uint16_t *u16;
    uint64_t *u64;
    size_t *sz;
    float *flt;
    double *dbl;
    pid_t *pid;
    pmix_status_t *ps;
    pmix_rank_t *pr;

    /* no negative value to be concerned about */
    if (PMIX_INT == type) {
        if (INT_MAX < value->data.uint32) {
            return PMIX_ERR_LOST_PRECISION;
        }
        i = (int*)dest;
        *i = (int)value->data.uint32;
        return PMIX_SUCCESS;
    }
    if (PMIX_INT8 == type) {
        if (INT8_MAX < value->data.uint32) {
            return PMIX_ERR_LOST_PRECISION;
        }
        i8 = (int8_t*)dest;
        *i8 = (int8_t)value->data.uint32;
        return PMIX_SUCCESS;
    }
    if (PMIX_INT16 == type) {
        if (INT16_MAX < value->data.uint32) {
            return PMIX_ERR_LOST_PRECISION;
        }
        i16 = (int16_t*)dest;
        *i16 = (int16_t)value->data.uint32;
        return PMIX_SUCCESS;
    }
    if (PMIX_INT32 == type) {
        if (INT32_MAX < value->data.uint32) {
            return PMIX_ERR_LOST_PRECISION;
        }
        i32 = (int32_t*)dest;
        *i32 = (int32_t)value->data.uint32;
        return PMIX_SUCCESS;
    }
    if (PMIX_INT64 == type) {
        i64 = (int64_t*)dest;
        *i64 = (int64_t)value->data.uint32;
        return PMIX_SUCCESS;
    }

    if (PMIX_UINT == type) {
        u = (unsigned int *)dest;
        *u = (unsigned int)value->data.uint32;
        return PMIX_SUCCESS;
    }
    if (PMIX_UINT8 == type) {
        if (UINT8_MAX < value->data.uint32) {
            return PMIX_ERR_LOST_PRECISION;
        }
        u8 = (uint8_t*)dest;
        *u8 = (uint8_t)value->data.uint32;
        return PMIX_SUCCESS;
    }
    if (PMIX_UINT16 == type) {
        if (UINT16_MAX < value->data.uint32) {
            return PMIX_ERR_LOST_PRECISION;
        }
        u16 = (uint16_t*)dest;
        *u16 = (uint16_t)value->data.uint32;
        return PMIX_SUCCESS;
    }
    if (PMIX_UINT64 == type) {
        u64 = (uint64_t*)dest;
        *u64 = (uint64_t)value->data.uint32;
        return PMIX_SUCCESS;
    }

    if (PMIX_SIZE == type) {
        sz = (size_t*)dest;
        *sz = (size_t)value->data.uint32;
        return PMIX_SUCCESS;
    }
    if (PMIX_FLOAT == type) {
        flt = (float*)dest;
        *flt = (float)value->data.uint32;
        return PMIX_SUCCESS;
    }
    if (PMIX_DOUBLE == type) {
        dbl = (double*)dest;
        *dbl = (double)value->data.uint32;
        return PMIX_SUCCESS;
    }
    if (PMIX_PROC_RANK == type) {
        // rank is an unsigned int
        pr = (pmix_rank_t*)dest;
        *pr = (pmix_rank_t)value->data.uint32;
    }

    if (PMIX_PID == type) {
        // pid_t is a signed int
        if (INT_MAX < value->data.uint32) {
            return PMIX_ERR_LOST_PRECISION;
        }
        pid = (pid_t*)dest;
        *pid = (pid_t)value->data.uint32;
        return PMIX_SUCCESS;
    }
    if (PMIX_STATUS == type) {
        // status is a signed int
        if (INT_MAX < value->data.uint32) {
            return PMIX_ERR_LOST_PRECISION;
        }
        ps = (pmix_status_t*)dest;
        *ps = (pmix_status_t)value->data.uint32;
        return PMIX_SUCCESS;
    }

    // get here if the destination is a non-numerical type
    return PMIX_ERR_BAD_PARAM;
}

static pmix_status_t check_uint64(const pmix_value_t *value,
                                 void *dest, pmix_data_type_t type)
{
    int *i;
    int8_t *i8;
    int16_t *i16;
    int32_t *i32;
    int64_t *i64;
    unsigned int *u;
    uint8_t *u8;
    uint16_t *u16;
    uint32_t *u32;
    size_t *sz;
    float *flt;
    double *dbl;
    pid_t *pid;
    pmix_status_t *ps;
    pmix_rank_t *pr;

    /* no negative value to be concerned about */
    if (PMIX_INT == type) {
        if (INT_MAX < value->data.uint64) {
            return PMIX_ERR_LOST_PRECISION;
        }
        i = (int*)dest;
        *i = (int)value->data.uint64;
        return PMIX_SUCCESS;
    }
    if (PMIX_INT8 == type) {
        if (INT8_MAX < value->data.uint64) {
            return PMIX_ERR_LOST_PRECISION;
        }
        i8 = (int8_t*)dest;
        *i8 = (int8_t)value->data.uint64;
        return PMIX_SUCCESS;
    }
    if (PMIX_INT16 == type) {
        if (INT16_MAX < value->data.uint64) {
            return PMIX_ERR_LOST_PRECISION;
        }
        i16 = (int16_t*)dest;
        *i16 = (int16_t)value->data.uint64;
        return PMIX_SUCCESS;
    }
    if (PMIX_INT32 == type) {
        if (INT32_MAX < value->data.uint64) {
            return PMIX_ERR_LOST_PRECISION;
        }
        i32 = (int32_t*)dest;
        *i32 = (int32_t)value->data.uint64;
        return PMIX_SUCCESS;
    }
    if (PMIX_INT64 == type) {
        if (INT64_MAX < value->data.uint64) {
            return PMIX_ERR_LOST_PRECISION;
        }
        i64 = (int64_t*)dest;
        *i64 = (int64_t)value->data.uint64;
        return PMIX_SUCCESS;
    }

    if (PMIX_UINT == type) {
        if (UINT_MAX < value->data.uint64) {
            return PMIX_ERR_LOST_PRECISION;
        }
        u = (unsigned int *)dest;
        *u = (unsigned int)value->data.uint64;
        return PMIX_SUCCESS;
    }
    if (PMIX_UINT8 == type) {
        if (UINT8_MAX < value->data.uint64) {
            return PMIX_ERR_LOST_PRECISION;
        }
        u8 = (uint8_t*)dest;
        *u8 = (uint8_t)value->data.uint64;
        return PMIX_SUCCESS;
    }
    if (PMIX_UINT16 == type) {
        if (UINT16_MAX < value->data.uint64) {
            return PMIX_ERR_LOST_PRECISION;
        }
        u16 = (uint16_t*)dest;
        *u16 = (uint16_t)value->data.uint64;
        return PMIX_SUCCESS;
    }
    if (PMIX_UINT32 == type) {
        if (UINT32_MAX < value->data.uint64) {
            return PMIX_ERR_LOST_PRECISION;
        }
        u32 = (uint32_t*)dest;
        *u32 = (uint32_t)value->data.uint64;
        return PMIX_SUCCESS;
    }

    if (PMIX_SIZE == type) {
        sz = (size_t*)dest;
        *sz = (size_t)value->data.uint64;
        return PMIX_SUCCESS;
    }
    if (PMIX_FLOAT == type) {
        flt = (float*)dest;
        *flt = (float)value->data.uint64;
        return PMIX_SUCCESS;
    }
    if (PMIX_DOUBLE == type) {
        dbl = (double*)dest;
        *dbl = (double)value->data.uint64;
        return PMIX_SUCCESS;
    }
    if (PMIX_PROC_RANK == type) {
        // rank is an unsigned int
        if (UINT32_MAX < value->data.uint64) {
            return PMIX_ERR_LOST_PRECISION;
        }
        pr = (pmix_rank_t*)dest;
        *pr = (pmix_rank_t)value->data.uint64;
    }

    if (PMIX_PID == type) {
        // pid_t is a signed int
        if (INT_MAX < value->data.uint64) {
            return PMIX_ERR_LOST_PRECISION;
        }
        pid = (pid_t*)dest;
        *pid = (pid_t)value->data.uint64;
        return PMIX_SUCCESS;
    }
    if (PMIX_STATUS == type) {
        // status is a signed int
        if (INT_MAX < value->data.uint64) {
            return PMIX_ERR_LOST_PRECISION;
        }
        ps = (pmix_status_t*)dest;
        *ps = (pmix_status_t)value->data.uint64;
        return PMIX_SUCCESS;
    }

    // get here if the destination is a non-numerical type
    return PMIX_ERR_BAD_PARAM;
}

static pmix_status_t check_float(const pmix_value_t *value,
                                 void *dest, pmix_data_type_t type)
{
    int *i;
    int8_t *i8;
    int16_t *i16;
    int32_t *i32;
    int64_t *i64;
    unsigned int *u;
    uint8_t *u8;
    uint16_t *u16;
    uint32_t *u32;
    uint64_t *u64;
    size_t *sz;
    double *dbl;
    pid_t *pid;
    pmix_status_t *ps;
    pmix_rank_t *pr;

    // check if this xfer would change sign
    if (0 > value->data.fval) {
        if (PMIX_SIZE == type ||
            PMIX_UINT == type ||
            PMIX_UINT8 == type ||
            PMIX_UINT16 == type ||
            PMIX_UINT32 == type ||
            PMIX_UINT64 == type ||
            PMIX_PROC_RANK == type) {
            return PMIX_ERR_CHANGE_SIGN;
        }
    }

    if (PMIX_INT == type) {
        if (INT_MAX < value->data.fval) {
            return PMIX_ERR_LOST_PRECISION;
        }
        i = (int*)dest;
        *i = (int)value->data.fval;
        return PMIX_SUCCESS;
    }
    if (PMIX_INT8 == type) {
        if (INT8_MAX < value->data.fval) {
            return PMIX_ERR_LOST_PRECISION;
        }
        i8 = (int8_t*)dest;
        *i8 = (int8_t)value->data.fval;
        return PMIX_SUCCESS;
    }
    if (PMIX_INT16 == type) {
        if (INT16_MAX < value->data.fval) {
            return PMIX_ERR_LOST_PRECISION;
        }
        i16 = (int16_t*)dest;
        *i16 = (int16_t)value->data.fval;
        return PMIX_SUCCESS;
    }
    if (PMIX_INT32 == type) {
        if (INT32_MAX < value->data.fval) {
            return PMIX_ERR_LOST_PRECISION;
        }
        i32 = (int32_t*)dest;
        *i32 = (int32_t)value->data.fval;
        return PMIX_SUCCESS;
    }
    if (PMIX_INT64 == type) {
        if (INT64_MAX < value->data.fval) {
            return PMIX_ERR_LOST_PRECISION;
        }
        i64 = (int64_t*)dest;
        *i64 = (int64_t)value->data.fval;
        return PMIX_SUCCESS;
    }

    if (PMIX_UINT == type) {
        if (UINT_MAX < value->data.fval) {
            return PMIX_ERR_LOST_PRECISION;
        }
        u = (unsigned int *)dest;
        *u = (unsigned int)value->data.fval;
        return PMIX_SUCCESS;
    }
    if (PMIX_UINT8 == type) {
        if (UINT8_MAX < value->data.fval) {
            return PMIX_ERR_LOST_PRECISION;
        }
        u8 = (uint8_t*)dest;
        *u8 = (uint8_t)value->data.fval;
        return PMIX_SUCCESS;
    }
    if (PMIX_UINT16 == type) {
        if (UINT16_MAX < value->data.fval) {
            return PMIX_ERR_LOST_PRECISION;
        }
        u16 = (uint16_t*)dest;
        *u16 = (uint16_t)value->data.fval;
        return PMIX_SUCCESS;
    }
    if (PMIX_UINT32 == type) {
        if (UINT32_MAX < value->data.fval) {
            return PMIX_ERR_LOST_PRECISION;
        }
        u32 = (uint32_t*)dest;
        *u32 = (uint32_t)value->data.fval;
        return PMIX_SUCCESS;
    }
    if (PMIX_UINT64 == type) {
        if (UINT64_MAX < value->data.fval) {
            return PMIX_ERR_LOST_PRECISION;
        }
        u64 = (uint64_t*)dest;
        *u64 = (uint64_t)value->data.fval;
        return PMIX_SUCCESS;
    }

    if (PMIX_SIZE == type) {
        if (SIZE_MAX < value->data.fval) {
            return PMIX_ERR_LOST_PRECISION;
        }
        sz = (size_t*)dest;
        *sz = (size_t)value->data.fval;
        return PMIX_SUCCESS;
    }
    if (PMIX_DOUBLE == type) {
        dbl = (double*)dest;
        *dbl = (double)value->data.fval;
        return PMIX_SUCCESS;
    }
    if (PMIX_PROC_RANK == type) {
        // rank is an unsigned int
        if (UINT32_MAX < value->data.fval) {
            return PMIX_ERR_LOST_PRECISION;
        }
        pr = (pmix_rank_t*)dest;
        *pr = (pmix_rank_t)value->data.fval;
    }

    if (PMIX_PID == type) {
        // pid_t is a signed int
        if (INT_MAX < value->data.fval) {
            return PMIX_ERR_LOST_PRECISION;
        }
        pid = (pid_t*)dest;
        *pid = (pid_t)value->data.fval;
        return PMIX_SUCCESS;
    }
    if (PMIX_STATUS == type) {
        // status is a signed int
        if (INT_MAX < value->data.fval) {
            return PMIX_ERR_LOST_PRECISION;
        }
        ps = (pmix_status_t*)dest;
        *ps = (pmix_status_t)value->data.fval;
        return PMIX_SUCCESS;
    }

    // get here if the destination is a non-numerical type
    return PMIX_ERR_BAD_PARAM;
}

static pmix_status_t check_double(const pmix_value_t *value,
                                  void *dest, pmix_data_type_t type)
{
    int *i;
    int8_t *i8;
    int16_t *i16;
    int32_t *i32;
    int64_t *i64;
    unsigned int *u;
    uint8_t *u8;
    uint16_t *u16;
    uint32_t *u32;
    uint64_t *u64;
    size_t *sz;
    pid_t *pid;
    pmix_status_t *ps;
    pmix_rank_t *pr;

    // check if this xfer would change sign
    if (0 > value->data.dval) {
        if (PMIX_SIZE == type ||
            PMIX_UINT == type ||
            PMIX_UINT8 == type ||
            PMIX_UINT16 == type ||
            PMIX_UINT32 == type ||
            PMIX_UINT64 == type ||
            PMIX_PROC_RANK == type) {
            return PMIX_ERR_CHANGE_SIGN;
        }
    }

    if (PMIX_INT == type) {
        if (INT_MAX < value->data.dval) {
            return PMIX_ERR_LOST_PRECISION;
        } else if (INT_MIN > value->data.dval) {
            return PMIX_ERR_LOST_PRECISION;
        }
        i = (int*)dest;
        *i = (int)value->data.dval;
        return PMIX_SUCCESS;
    }
    if (PMIX_INT8 == type) {
        if (INT8_MAX < value->data.dval) {
            return PMIX_ERR_LOST_PRECISION;
        } else if (INT8_MIN > value->data.dval) {
            return PMIX_ERR_LOST_PRECISION;
        }
        i8 = (int8_t*)dest;
        *i8 = (int8_t)value->data.dval;
        return PMIX_SUCCESS;
    }
    if (PMIX_INT16 == type) {
        if (INT16_MAX < value->data.dval) {
            return PMIX_ERR_LOST_PRECISION;
        } else if (INT16_MIN > value->data.dval) {
            return PMIX_ERR_LOST_PRECISION;
        }
        i16 = (int16_t*)dest;
        *i16 = (int16_t)value->data.dval;
        return PMIX_SUCCESS;
    }
    if (PMIX_INT32 == type) {
        if (INT32_MAX < value->data.dval) {
            return PMIX_ERR_LOST_PRECISION;
        } else if (INT32_MIN > value->data.dval) {
            return PMIX_ERR_LOST_PRECISION;
        }
        i32 = (int32_t*)dest;
        *i32 = (int32_t)value->data.dval;
        return PMIX_SUCCESS;
    }
    if (PMIX_INT64 == type) {
        if (INT64_MAX < value->data.dval) {
            return PMIX_ERR_LOST_PRECISION;
        } else if (INT64_MIN > value->data.dval) {
            return PMIX_ERR_LOST_PRECISION;
        }
        i64 = (int64_t*)dest;
        *i64 = (int64_t)value->data.dval;
        return PMIX_SUCCESS;
    }

    if (PMIX_UINT == type) {
        if (UINT_MAX < value->data.dval) {
            return PMIX_ERR_LOST_PRECISION;
        }
        u = (unsigned int *)dest;
        *u = (unsigned int)value->data.dval;
        return PMIX_SUCCESS;
    }
    if (PMIX_UINT8 == type) {
        if (UINT8_MAX < value->data.dval) {
            return PMIX_ERR_LOST_PRECISION;
        }
        u8 = (uint8_t*)dest;
        *u8 = (uint8_t)value->data.dval;
        return PMIX_SUCCESS;
    }
    if (PMIX_UINT16 == type) {
        if (UINT16_MAX < value->data.dval) {
            return PMIX_ERR_LOST_PRECISION;
        }
        u16 = (uint16_t*)dest;
        *u16 = (uint16_t)value->data.dval;
        return PMIX_SUCCESS;
    }
    if (PMIX_UINT32 == type) {
        if (UINT32_MAX < value->data.dval) {
            return PMIX_ERR_LOST_PRECISION;
        }
        u32 = (uint32_t*)dest;
        *u32 = (uint32_t)value->data.dval;
        return PMIX_SUCCESS;
    }
    if (PMIX_UINT64 == type) {
        if (UINT64_MAX < value->data.dval) {
            return PMIX_ERR_LOST_PRECISION;
        }
        u64 = (uint64_t*)dest;
        *u64 = (uint64_t)value->data.dval;
        return PMIX_SUCCESS;
    }

    if (PMIX_SIZE == type) {
        if (SIZE_MAX < value->data.dval) {
            return PMIX_ERR_LOST_PRECISION;
        }
        sz = (size_t*)dest;
        *sz = (size_t)value->data.dval;
        return PMIX_SUCCESS;
    }
    if (PMIX_FLOAT == type) {
        return PMIX_ERR_LOST_PRECISION;
    }
    if (PMIX_PROC_RANK == type) {
        // rank is an unsigned int
        if (UINT32_MAX < value->data.dval) {
            return PMIX_ERR_LOST_PRECISION;
        }
        pr = (pmix_rank_t*)dest;
        *pr = (pmix_rank_t)value->data.dval;
    }

    if (PMIX_PID == type) {
        // pid_t is a signed int
        if (INT_MAX < value->data.dval) {
            return PMIX_ERR_LOST_PRECISION;
        } else if (INT_MIN > value->data.dval) {
            return PMIX_ERR_LOST_PRECISION;
        }
        pid = (pid_t*)dest;
        *pid = (pid_t)value->data.dval;
        return PMIX_SUCCESS;
    }
    if (PMIX_STATUS == type) {
        // status is a signed int
        if (INT_MAX < value->data.dval) {
            return PMIX_ERR_LOST_PRECISION;
        } else if (INT_MIN > value->data.dval) {
            return PMIX_ERR_LOST_PRECISION;
        }
        ps = (pmix_status_t*)dest;
        *ps = (pmix_status_t)value->data.dval;
        return PMIX_SUCCESS;
    }

    // get here if the destination is a non-numerical type
    return PMIX_ERR_BAD_PARAM;
}

static pmix_status_t check_rank(const pmix_value_t *value,
                                void *dest, pmix_data_type_t type)
{
    int *i;
    int8_t *i8;
    int16_t *i16;
    int32_t *i32;
    int64_t *i64;
    unsigned int *u;
    uint8_t *u8;
    uint16_t *u16;
    uint32_t *u32;
    uint64_t *u64;
    size_t *sz;
    float *flt;
    double *dval;

    // rank is an unsigned int

    if (PMIX_INT == type) {
        if (INT_MAX < value->data.rank) {
            return PMIX_ERR_LOST_PRECISION;
        }
        i = (int*)dest;
        *i = (int)value->data.rank;
        return PMIX_SUCCESS;
    }
    if (PMIX_INT8 == type) {
        if (INT8_MAX < value->data.rank) {
            return PMIX_ERR_LOST_PRECISION;
        }
        i8 = (int8_t*)dest;
        *i8 = (int8_t)value->data.rank;
        return PMIX_SUCCESS;
    }
    if (PMIX_INT16 == type) {
        if (INT16_MAX < value->data.rank) {
            return PMIX_ERR_LOST_PRECISION;
        }
        i16 = (int16_t*)dest;
        *i16 = (int16_t)value->data.rank;
        return PMIX_SUCCESS;
    }
    if (PMIX_INT32 == type) {
        if (INT32_MAX < value->data.rank) {
            return PMIX_ERR_LOST_PRECISION;
        }
        i32 = (int32_t*)dest;
        *i32 = (int32_t)value->data.rank;
        return PMIX_SUCCESS;
    }
    if (PMIX_INT64 == type) {
        i64 = (int64_t*)dest;
        *i64 = (int64_t)value->data.rank;
        return PMIX_SUCCESS;
    }

    if (PMIX_UINT == type) {
        u = (unsigned int *)dest;
        *u = (unsigned int)value->data.rank;
        return PMIX_SUCCESS;
    }
    if (PMIX_UINT8 == type) {
        if (UINT8_MAX < value->data.rank) {
            return PMIX_ERR_LOST_PRECISION;
        }
        u8 = (uint8_t*)dest;
        *u8 = (uint8_t)value->data.rank;
        return PMIX_SUCCESS;
    }
    if (PMIX_UINT16 == type) {
        if (UINT16_MAX < value->data.rank) {
            return PMIX_ERR_LOST_PRECISION;
        }
        u16 = (uint16_t*)dest;
        *u16 = (uint16_t)value->data.rank;
        return PMIX_SUCCESS;
    }
    if (PMIX_UINT32 == type) {
        u32 = (uint32_t *)dest;
        *u32 = (uint32_t)value->data.rank;
        return PMIX_SUCCESS;
    }
    if (PMIX_UINT64 == type) {
        u64 = (uint64_t *)dest;
        *u64 = (uint64_t)value->data.rank;
        return PMIX_SUCCESS;
    }

    if (PMIX_SIZE == type) {
        sz = (size_t*)dest;
        *sz = (size_t)value->data.rank;
        return PMIX_SUCCESS;
    }
    if (PMIX_FLOAT == type) {
        flt = (float*)dest;
        *flt = (float)value->data.rank;
        return PMIX_SUCCESS;
    }
    if (PMIX_DOUBLE == type) {
        dval = (double*)dest;
        *dval = (double)value->data.rank;
        return PMIX_SUCCESS;
    }

    // do not allow rank (a PMIx structured value) to be
    // unloaded into other PMIx structured value types

    // get here if the destination is a non-numerical type
    return PMIX_ERR_BAD_PARAM;
}

static pmix_status_t check_status(const pmix_value_t *value,
                                  void *dest, pmix_data_type_t type)
{
    int *i;
    int8_t *i8;
    int16_t *i16;
    int32_t *i32;
    int64_t *i64;
    unsigned int *ui;
    uint8_t *u8;
    uint16_t *u16;
    uint32_t *u32;
    uint64_t *u64;
    size_t *sz;
    float *flt;
    double *dbl;

    // check if this xfer would change sign
    if (0 > value->data.status) {
        if (PMIX_SIZE == type ||
            PMIX_UINT == type ||
            PMIX_UINT8 == type ||
            PMIX_UINT16 == type ||
            PMIX_UINT32 == type ||
            PMIX_UINT64 == type) {
            return PMIX_ERR_CHANGE_SIGN;
        }
    }

    /* already took care of the negative value case when
     * transferring to unsigned types, and no loss of
     * precision if the dest is equal in size or larger */

    if (PMIX_UINT == type) {
        ui = (unsigned int*)dest;
        *ui = (unsigned int)value->data.status;
        return PMIX_SUCCESS;
    }
    if (PMIX_INT == type) {
        // status is a signed int
        i = (int*)dest;
        *i = (int)value->data.status;
        return PMIX_SUCCESS;
    }
    if (PMIX_INT64 == type) {
        i64 = (int64_t*)dest;
        *i64 = (int64_t)value->data.status;
        return PMIX_SUCCESS;
    }
    if (PMIX_UINT64 == type) {
        u64 = (uint64_t*)dest;
        *u64 = (uint64_t)value->data.status;
        return PMIX_SUCCESS;
    }
    if (PMIX_SIZE == type) {
        sz = (size_t*)dest;
        *sz = (size_t)value->data.status;
        return PMIX_SUCCESS;
    }
    if (PMIX_FLOAT == type) {
        flt = (float*)dest;
        *flt = (float)value->data.status;
        return PMIX_SUCCESS;
    }
    if (PMIX_DOUBLE == type) {
        dbl = (double*)dest;
        *dbl = (double)value->data.status;
        return PMIX_SUCCESS;
    }

    // if we get here, then we are dealing with a smaller
    // destination, which means we can lose precision
    if (PMIX_INT8 == type) {
        if (0 < value->data.status) {
            if (INT8_MAX < value->data.status) {
                return PMIX_ERR_LOST_PRECISION;
            }
        } else {
            if (INT8_MIN > value->data.status) {
                return PMIX_ERR_LOST_PRECISION;
            }
        }
        i8 = (int8_t*)dest;
        *i8 = (int8_t)value->data.status;
        return PMIX_SUCCESS;
    }
    if (PMIX_INT16 == type) {
        if (0 < value->data.status) {
            if (INT16_MAX < value->data.status) {
                return PMIX_ERR_LOST_PRECISION;
            }
        } else {
            if (INT16_MIN > value->data.status) {
                return PMIX_ERR_LOST_PRECISION;
            }
        }
        i16 = (int16_t*)dest;
        *i16 = (int16_t)value->data.status;
        return PMIX_SUCCESS;
    }
    if (PMIX_INT32 == type) {
        if (0 < value->data.status) {
            if (INT32_MAX < value->data.status) {
                return PMIX_ERR_LOST_PRECISION;
            }
        } else {
            if (INT32_MIN > value->data.status) {
                return PMIX_ERR_LOST_PRECISION;
            }
        }
        i32 = (int32_t*)dest;
        *i32 = (int32_t)value->data.status;
        return PMIX_SUCCESS;
    }

    if (PMIX_UINT8 == type) {
        if (UINT8_MAX < value->data.status) {
            return PMIX_ERR_LOST_PRECISION;
        }
        u8 = (uint8_t*)dest;
        *u8 = (uint8_t)value->data.status;
        return PMIX_SUCCESS;
    }
    if (PMIX_UINT16 == type) {
        if (UINT16_MAX < value->data.status) {
            return PMIX_ERR_LOST_PRECISION;
        }
        u16 = (uint16_t*)dest;
        *u16 = (uint16_t)value->data.status;
        return PMIX_SUCCESS;
    }
    if (PMIX_UINT32 == type) {
        u32 = (uint32_t*)dest;
        *u32 = (uint32_t)value->data.status;
        return PMIX_SUCCESS;
    }

    /* get here if the destination is a non-numerical type
     * or a mismatched structured type */
    return PMIX_ERR_BAD_PARAM;
}

