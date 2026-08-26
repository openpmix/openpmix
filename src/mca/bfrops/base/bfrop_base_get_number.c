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

static pmix_status_t check_pid(const pmix_value_t *value,
                               void *dest, pmix_data_type_t type);

static pmix_data_type_t plain_integer_type(pmix_data_type_t type);

static bool structured_number(pmix_data_type_t type);

pmix_status_t PMIx_Value_get_number(const pmix_value_t *value,
                                    void *dest, pmix_data_type_t type)
{
    pmix_value_t plain;
    pmix_data_type_t base;

    /* the type tag is the first thing every branch below reads, and the
     * destination is written by every one that matches, so both have to be
     * screened here. Callers reach this with the "value" member of a
     * pmix_kval_t - which is a pointer that is legitimately NULL for a kval
     * that arrived off the wire (see src/mca/bfrops/AGENTS.md) - so the
     * screen belongs here rather than at the call sites, all of which are
     * equally exposed */
    if (NULL == value || NULL == dest) {
        return PMIX_ERR_BAD_PARAM;
    }

    /* Stated once here rather than at the tail of every check_*: a PMIx
     * structured value is not unloaded into a DIFFERENT structured value
     * type, however alike the integers underneath them are.  A scope is not
     * a proc state and an allocation directive is not a status, and a caller
     * that asked for one of those from the other has made a mistake worth
     * hearing about.  Reading either one as a plain integer, or building
     * either one from a plain integer, is exactly what this function is
     * for and stays allowed. */
    if (value->type != type &&
        structured_number(value->type) && structured_number(type)) {
        return PMIX_ERR_BAD_PARAM;
    }

    /* Rewrite a named integer as the plain one underneath it, at both ends.
     * The union members alias, so the copy carries the datum across without
     * naming which member holds it - and `plain` is only ever read as an
     * integer, so it owns nothing and needs no destructor. */
    base = plain_integer_type(value->type);
    if (PMIX_UNDEF != base) {
        plain = *value;
        plain.type = base;
        value = &plain;
    }
    base = plain_integer_type(type);
    if (PMIX_UNDEF != base) {
        /* same width and representation as the type the caller named, so
         * the store below writes exactly the destination they handed us */
        type = base;
    }

    if (PMIX_SIZE == value->type) {
        if (PMIX_SIZE == type) {
            size_t *sz;
            sz = (size_t*)dest;
            *sz = value->data.size;
            return PMIX_SUCCESS;
        } else {
            return check_size(value, dest, type);
        }
    }

    if (PMIX_INT == value->type) {
        if (PMIX_INT == type) {
            int *i;
            i = (int*)dest;
            *i = value->data.integer;
            return PMIX_SUCCESS;
        } else {
            return check_int(value, dest, type);
        }
    }

    if (PMIX_INT8 == value->type) {
        if (PMIX_INT8 == type) {
            int8_t *i8;
            i8 = (int8_t*)dest;
            *i8 = value->data.int8;
            return PMIX_SUCCESS;
        } else {
            return check_int8(value, dest, type);
        }
    }

    if (PMIX_INT16 == value->type) {
        if (PMIX_INT16 == type) {
            int16_t *i16;
            i16 = (int16_t*)dest;
            *i16 = value->data.int16;
            return PMIX_SUCCESS;
        } else {
            return check_int16(value, dest, type);
        }
    }

    if (PMIX_INT32 == value->type) {
        if (PMIX_INT32 == type) {
            int32_t *i32;
            i32 = (int32_t*)dest;
            *i32 = value->data.int32;
            return PMIX_SUCCESS;
        } else {
            return check_int32(value, dest, type);
        }
    }

    if (PMIX_INT64 == value->type) {
        if (PMIX_INT64 == type) {
            int64_t *i64;
            i64 = (int64_t*)dest;
            *i64 = value->data.int64;
            return PMIX_SUCCESS;
        } else {
            return check_int64(value, dest, type);
        }
    }

    if (PMIX_UINT == value->type) {
        if (PMIX_UINT == type) {
            unsigned int *ui;
            ui = (unsigned int*)dest;
            *ui = value->data.uint;
            return PMIX_SUCCESS;
        } else {
            return check_uint(value, dest, type);
        }
    }

    if (PMIX_UINT8 == value->type) {
        if (PMIX_UINT8 == type) {
            uint8_t *u8;
            u8 = (uint8_t*)dest;
            *u8 = value->data.uint8;
            return PMIX_SUCCESS;
        } else {
            return check_uint8(value, dest, type);
        }
    }

    if (PMIX_UINT16 == value->type) {
        if (PMIX_UINT16 == type) {
            uint16_t *u16;
            u16 = (uint16_t*)dest;
            *u16 = value->data.uint16;
            return PMIX_SUCCESS;
        } else {
            return check_uint16(value, dest, type);
        }
    }

    if (PMIX_UINT32 == value->type) {
        if (PMIX_UINT32 == type) {
            uint32_t *u32;
            u32 = (uint32_t*)dest;
            *u32 = value->data.uint32;
            return PMIX_SUCCESS;
        } else {
            return check_uint32(value, dest, type);
        }
    }

    if (PMIX_UINT64 == value->type) {
        if (PMIX_UINT64 == type) {
            uint64_t *u64;
            u64 = (uint64_t*)dest;
            *u64 = value->data.uint64;
            return PMIX_SUCCESS;
        } else {
            return check_uint64(value, dest, type);
        }
    }

    if (PMIX_FLOAT == value->type) {
        if (PMIX_FLOAT == type) {
            float *f;
            f = (float*)dest;
            *f = value->data.fval;
            return PMIX_SUCCESS;
        } else {
            return check_float(value, dest, type);
        }
    }

    if (PMIX_DOUBLE == value->type) {
        if (PMIX_DOUBLE == type) {
            double *d;
            d = (double*)dest;
            *d = value->data.dval;
            return PMIX_SUCCESS;
        } else {
            return check_double(value, dest, type);
        }
    }

    if (PMIX_PID == value->type) {
        if (PMIX_PID == type) {
            pid_t *p;
            p = (pid_t*)dest;
            *p = value->data.pid;
            return PMIX_SUCCESS;
        } else {
            return check_pid(value, dest, type);
        }
    }

    if (PMIX_PROC_RANK == value->type) {
        if (PMIX_PROC_RANK == type) {
            pmix_rank_t *r;
            r = (pmix_rank_t*)dest;
            *r = value->data.rank;
            return PMIX_SUCCESS;
        } else {
            return check_rank(value, dest, type);
        }
    }

    if (PMIX_STATUS == value->type) {
        if (PMIX_STATUS== type) {
            pmix_status_t *s;
            s = (pmix_status_t*)dest;
            *s = value->data.status;
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
        return PMIX_SUCCESS;
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
        return PMIX_SUCCESS;
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
        return PMIX_SUCCESS;
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
    if (0 > value->data.int16) {
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
        return PMIX_SUCCESS;
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
    if (0 > value->data.int32) {
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
        return PMIX_SUCCESS;
    }

    // if we get here, then we are dealing with a smaller
    // destination, which means we can lose precision
    if (PMIX_INT8 == type) {
        if (0 < value->data.int32) {
            if (INT8_MAX < value->data.int32) {
                return PMIX_ERR_LOST_PRECISION;
            }
        } else {
            if (INT8_MIN > value->data.int32) {
                return PMIX_ERR_LOST_PRECISION;
            }
        }
        i8 = (int8_t*)dest;
        *i8 = (int8_t)value->data.int32;
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
        if (UINT8_MAX < value->data.int32) {
            return PMIX_ERR_LOST_PRECISION;
        }
        u8 = (uint8_t*)dest;
        *u8 = (uint8_t)value->data.int32;
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
    if (0 > value->data.int64) {
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
        // pid_t is a SIGNED 32-bit int, so its range is INT32's and not
        // UINT32's - bounding it by UINT32_MAX let everything from
        // INT32_MAX+1 up wrap to a negative pid, and left the whole
        // negative side unchecked
        if (INT32_MAX < value->data.int64 ||
            INT32_MIN > value->data.int64) {
            return PMIX_ERR_LOST_PRECISION;
        }
        pid = (pid_t*)dest;
        *pid = (pid_t)value->data.int64;
        return PMIX_SUCCESS;
    }
    if (PMIX_STATUS == type) {
        // status is likewise a signed 32-bit int
        if (INT32_MAX < value->data.int64 ||
            INT32_MIN > value->data.int64) {
            return PMIX_ERR_LOST_PRECISION;
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
        return PMIX_SUCCESS;
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
        return PMIX_SUCCESS;
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
        return PMIX_SUCCESS;
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
        return PMIX_SUCCESS;
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
        return PMIX_SUCCESS;
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
        return PMIX_SUCCESS;
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
        if (2147483647.0 < value->data.fval) {
            return PMIX_ERR_LOST_PRECISION;
        } else if (-2147483648.0 > value->data.fval) {
            return PMIX_ERR_LOST_PRECISION;
        }
        i = (int*)dest;
        *i = (int)value->data.fval;
        return PMIX_SUCCESS;
    }
    if (PMIX_INT8 == type) {
        if (127.0 < value->data.fval) {
            return PMIX_ERR_LOST_PRECISION;
        } else if (-128.0 > value->data.fval) {
            return PMIX_ERR_LOST_PRECISION;
        }
        i8 = (int8_t*)dest;
        *i8 = (int8_t)value->data.fval;
        return PMIX_SUCCESS;
    }
    if (PMIX_INT16 == type) {
        if (32767.0 < value->data.fval) {
            return PMIX_ERR_LOST_PRECISION;
        } else if (-32768.0 > value->data.fval) {
            return PMIX_ERR_LOST_PRECISION;
        }
        i16 = (int16_t*)dest;
        *i16 = (int16_t)value->data.fval;
        return PMIX_SUCCESS;
    }
    if (PMIX_INT32 == type) {
        if (2147483647.0 < value->data.fval) {
            return PMIX_ERR_LOST_PRECISION;
        } else if (-2147483648.0 > value->data.fval) {
            return PMIX_ERR_LOST_PRECISION;
        }
        i32 = (int32_t*)dest;
        *i32 = (int32_t)value->data.fval;
        return PMIX_SUCCESS;
    }
    if (PMIX_INT64 == type) {
        if (18446744073709551000.0 < value->data.fval) {
            return PMIX_ERR_LOST_PRECISION;
        } else if (-9223372036854775808.0 > value->data.fval) {
            return PMIX_ERR_LOST_PRECISION;
        }
        i64 = (int64_t*)dest;
        *i64 = (int64_t)value->data.fval;
        return PMIX_SUCCESS;
    }

    if (PMIX_UINT == type) {
        /* 4294967295, not 42949670295 - the extra digit let every float
         * from UINT_MAX+1 up to ten billion through to wrap silently */
        if (4294967295.0 < value->data.fval) {
            return PMIX_ERR_LOST_PRECISION;
        }
        u = (unsigned int *)dest;
        *u = (unsigned int)value->data.fval;
        return PMIX_SUCCESS;
    }
    if (PMIX_UINT8 == type) {
        if (255.0 < value->data.fval) {
            return PMIX_ERR_LOST_PRECISION;
        }
        u8 = (uint8_t*)dest;
        *u8 = (uint8_t)value->data.fval;
        return PMIX_SUCCESS;
    }
    if (PMIX_UINT16 == type) {
        if (65535.0 < value->data.fval) {
            return PMIX_ERR_LOST_PRECISION;
        }
        u16 = (uint16_t*)dest;
        *u16 = (uint16_t)value->data.fval;
        return PMIX_SUCCESS;
    }
    if (PMIX_UINT32 == type) {
        if (4294967040.0 < value->data.fval) {
            return PMIX_ERR_LOST_PRECISION;
        }
        u32 = (uint32_t*)dest;
        *u32 = (uint32_t)value->data.fval;
        return PMIX_SUCCESS;
    }
    if (PMIX_UINT64 == type) {
        if (18446744073709551000.0 < value->data.fval) {
            return PMIX_ERR_LOST_PRECISION;
        }
        u64 = (uint64_t*)dest;
        *u64 = (uint64_t)value->data.fval;
        return PMIX_SUCCESS;
    }

    if (PMIX_SIZE == type) {
        if (18446744073709551000.0 < value->data.fval) {
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
        if (4294967040.0 < value->data.fval) {
            return PMIX_ERR_LOST_PRECISION;
        }
        pr = (pmix_rank_t*)dest;
        *pr = (pmix_rank_t)value->data.fval;
        return PMIX_SUCCESS;
    }

    if (PMIX_PID == type) {
        // pid_t is a signed int
        if (2147483647.0 < value->data.fval) {
            return PMIX_ERR_LOST_PRECISION;
        }
        pid = (pid_t*)dest;
        *pid = (pid_t)value->data.fval;
        return PMIX_SUCCESS;
    }
    if (PMIX_STATUS == type) {
        // status is a signed int
        if (2147483647.0 < value->data.fval) {
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
        if (2147483647.0f < value->data.dval) {
            return PMIX_ERR_LOST_PRECISION;
        } else if (-2147483648.0f > value->data.dval) {
            return PMIX_ERR_LOST_PRECISION;
        }
        i = (int*)dest;
        *i = (int)value->data.dval;
        return PMIX_SUCCESS;
    }
    if (PMIX_INT8 == type) {
        if (127.0 < value->data.dval) {
            return PMIX_ERR_LOST_PRECISION;
        } else if (-128.0 > value->data.dval) {
            return PMIX_ERR_LOST_PRECISION;
        }
        i8 = (int8_t*)dest;
        *i8 = (int8_t)value->data.dval;
        return PMIX_SUCCESS;
    }
    if (PMIX_INT16 == type) {
        if (32767.0 < value->data.dval) {
            return PMIX_ERR_LOST_PRECISION;
        } else if (-32768.0 > value->data.dval) {
            return PMIX_ERR_LOST_PRECISION;
        }
        i16 = (int16_t*)dest;
        *i16 = (int16_t)value->data.dval;
        return PMIX_SUCCESS;
    }
    if (PMIX_INT32 == type) {
        if (2147483647.0 < value->data.dval) {
            return PMIX_ERR_LOST_PRECISION;
        } else if (-2147483648.0 > value->data.dval) {
            return PMIX_ERR_LOST_PRECISION;
        }
        i32 = (int32_t*)dest;
        *i32 = (int32_t)value->data.dval;
        return PMIX_SUCCESS;
    }
    if (PMIX_INT64 == type) {
        if (18446744073709551000.0 < value->data.dval) {
            return PMIX_ERR_LOST_PRECISION;
        } else if (-9223372036854775808.0 > value->data.dval) {
            return PMIX_ERR_LOST_PRECISION;
        }
        i64 = (int64_t*)dest;
        *i64 = (int64_t)value->data.dval;
        return PMIX_SUCCESS;
    }

    if (PMIX_UINT == type) {
        if (42949670295.0 < value->data.dval) {
            return PMIX_ERR_LOST_PRECISION;
        }
        u = (unsigned int *)dest;
        *u = (unsigned int)value->data.dval;
        return PMIX_SUCCESS;
    }
    if (PMIX_UINT8 == type) {
        if (255.0 < value->data.dval) {
            return PMIX_ERR_LOST_PRECISION;
        }
        u8 = (uint8_t*)dest;
        *u8 = (uint8_t)value->data.dval;
        return PMIX_SUCCESS;
    }
    if (PMIX_UINT16 == type) {
        if (65535.0 < value->data.dval) {
            return PMIX_ERR_LOST_PRECISION;
        }
        u16 = (uint16_t*)dest;
        *u16 = (uint16_t)value->data.dval;
        return PMIX_SUCCESS;
    }
    if (PMIX_UINT32 == type) {
        if (4294967295.0 < value->data.dval) {
            return PMIX_ERR_LOST_PRECISION;
        }
        u32 = (uint32_t*)dest;
        *u32 = (uint32_t)value->data.dval;
        return PMIX_SUCCESS;
    }
    if (PMIX_UINT64 == type) {
        if (18446744073709551000.0 < value->data.dval) {
            return PMIX_ERR_LOST_PRECISION;
        }
        u64 = (uint64_t*)dest;
        *u64 = (uint64_t)value->data.dval;
        return PMIX_SUCCESS;
    }

    if (PMIX_SIZE == type) {
        if (18446744073709551000.0 < value->data.dval) {
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
        if (4294967295.0 < value->data.dval) {
            return PMIX_ERR_LOST_PRECISION;
        }
        pr = (pmix_rank_t*)dest;
        *pr = (pmix_rank_t)value->data.dval;
        return PMIX_SUCCESS;
    }

    if (PMIX_PID == type) {
        // pid_t is a signed int
        if (2147483647.0 < value->data.dval) {
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
        if (2147483647.0 < value->data.dval) {
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
    if (PMIX_INT32 == type) {
        i32 = (int32_t*)dest;
        *i32 = (int32_t)value->data.status;
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

    // as in check_rank(): a PMIx structured value is not unloaded into
    // another PMIx structured value type

    /* get here if the destination is a non-numerical type
     * or a mismatched structured type */
    return PMIX_ERR_BAD_PARAM;
}

/* pid_t is a signed 32-bit integer, exactly like pmix_status_t, so this
 * mirrors check_status(). It reads value->data.pid rather than leaning
 * on the two sharing a union offset - that assumption is precisely what
 * made several of the sign checks in this file silently ineffective. */
static pmix_status_t check_pid(const pmix_value_t *value,
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
    if (0 > value->data.pid) {
        if (PMIX_SIZE == type ||
            PMIX_UINT == type ||
            PMIX_UINT8 == type ||
            PMIX_UINT16 == type ||
            PMIX_UINT32 == type ||
            PMIX_UINT64 == type) {
            return PMIX_ERR_CHANGE_SIGN;
        }
    }

    /* the negative case is settled, and there is no loss of precision
     * for a destination at least as wide */

    if (PMIX_INT == type) {
        i = (int*)dest;
        *i = (int)value->data.pid;
        return PMIX_SUCCESS;
    }
    if (PMIX_INT32 == type) {
        i32 = (int32_t*)dest;
        *i32 = (int32_t)value->data.pid;
        return PMIX_SUCCESS;
    }
    if (PMIX_INT64 == type) {
        i64 = (int64_t*)dest;
        *i64 = (int64_t)value->data.pid;
        return PMIX_SUCCESS;
    }
    if (PMIX_UINT == type) {
        ui = (unsigned int*)dest;
        *ui = (unsigned int)value->data.pid;
        return PMIX_SUCCESS;
    }
    if (PMIX_UINT32 == type) {
        u32 = (uint32_t*)dest;
        *u32 = (uint32_t)value->data.pid;
        return PMIX_SUCCESS;
    }
    if (PMIX_UINT64 == type) {
        u64 = (uint64_t*)dest;
        *u64 = (uint64_t)value->data.pid;
        return PMIX_SUCCESS;
    }
    if (PMIX_SIZE == type) {
        sz = (size_t*)dest;
        *sz = (size_t)value->data.pid;
        return PMIX_SUCCESS;
    }
    if (PMIX_FLOAT == type) {
        flt = (float*)dest;
        *flt = (float)value->data.pid;
        return PMIX_SUCCESS;
    }
    if (PMIX_DOUBLE == type) {
        dbl = (double*)dest;
        *dbl = (double)value->data.pid;
        return PMIX_SUCCESS;
    }

    // as in check_rank(): a PMIx structured value is not unloaded into
    // another PMIx structured value type

    // narrower destinations, where precision can be lost
    if (PMIX_INT8 == type) {
        if (INT8_MAX < value->data.pid ||
            INT8_MIN > value->data.pid) {
            return PMIX_ERR_LOST_PRECISION;
        }
        i8 = (int8_t*)dest;
        *i8 = (int8_t)value->data.pid;
        return PMIX_SUCCESS;
    }
    if (PMIX_INT16 == type) {
        if (INT16_MAX < value->data.pid ||
            INT16_MIN > value->data.pid) {
            return PMIX_ERR_LOST_PRECISION;
        }
        i16 = (int16_t*)dest;
        *i16 = (int16_t)value->data.pid;
        return PMIX_SUCCESS;
    }
    if (PMIX_UINT8 == type) {
        if (UINT8_MAX < value->data.pid) {
            return PMIX_ERR_LOST_PRECISION;
        }
        u8 = (uint8_t*)dest;
        *u8 = (uint8_t)value->data.pid;
        return PMIX_SUCCESS;
    }
    if (PMIX_UINT16 == type) {
        if (UINT16_MAX < value->data.pid) {
            return PMIX_ERR_LOST_PRECISION;
        }
        u16 = (uint16_t*)dest;
        *u16 = (uint16_t)value->data.pid;
        return PMIX_SUCCESS;
    }

    /* get here if the destination is a non-numerical type
     * or a mismatched structured type */
    return PMIX_ERR_BAD_PARAM;
}

/* The integer underneath a PMIx type that is an integer wearing a name.
 *
 * A pmix_scope_t, a pmix_proc_state_t, a pmix_alloc_inheritance_t and a
 * dozen more are each a fixed-width unsigned integer given its own type tag
 * and its own member of the value union.  The number in one of them is a
 * number, and a caller asking for it is asking a question with an answer -
 * but the answer used to be PMIX_ERR_BAD_PARAM, because this function knew
 * only the plain widths plus PMIX_PID, PMIX_STATUS and PMIX_PROC_RANK.
 *
 * That mattered most where the documentation tells a caller to use the
 * named type: an attribute annotated "(pmix_alloc_inheritance_t)" is loaded
 * with PMIX_ALLOC_INHERIT, and the reader PMIx offers for it then refused
 * the spelling PMIx had asked for.  Every host was left writing the same
 * special case.
 *
 * Rather than give each of these an arm in each of the fifteen check_*
 * functions, name the plain type underneath and let the ordinary
 * conversions do the work - so the range and precision checks apply to
 * them exactly as they do to everything else.  Returns PMIX_UNDEF for a
 * type that is not one of the family.
 *
 * The widths here are the ones pmix_bfrops_base_value_load() stores; keep
 * the two in step.
 */
static pmix_data_type_t plain_integer_type(pmix_data_type_t type)
{
    switch (type) {
    case PMIX_PERSIST:
    case PMIX_SCOPE:
    case PMIX_DATA_RANGE:
    case PMIX_PROC_STATE:
    case PMIX_ALLOC_DIRECTIVE:
    case PMIX_RESBLOCK_DIRECTIVE:
    case PMIX_ALLOC_INHERIT:
    case PMIX_JOB_STATE:
    case PMIX_LINK_STATE:
        return PMIX_UINT8;

    case PMIX_LOCTYPE:
    case PMIX_STOR_ACCESS_TYPE:
        return PMIX_UINT16;

    case PMIX_DEVTYPE:
    case PMIX_STOR_MEDIUM:
    case PMIX_STOR_ACCESS:
    case PMIX_STOR_PERSIST:
        return PMIX_UINT64;

    default:
        return PMIX_UNDEF;
    }
}

/* Is this type a number PMIx has given a meaning to, rather than a bare
 * width?  PMIX_PID, PMIX_STATUS and PMIX_PROC_RANK are the three the
 * check_* functions already recognized; the rest come from the family
 * above. */
static bool structured_number(pmix_data_type_t type)
{
    if (PMIX_PID == type || PMIX_STATUS == type || PMIX_PROC_RANK == type) {
        return true;
    }
    return (PMIX_UNDEF != plain_integer_type(type));
}
