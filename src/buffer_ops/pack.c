/*
 * Copyright (c) 2004-2007 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2007 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2011-2013 Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2014      Intel, Inc. All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "pmix_config.h"

#ifdef HAVE_ARPA_INET_H
#include <arpa/inet.h>
#endif

#include "src/include/types.h"
#include "src/util/argv.h"
#include "src/util/error.h"
#include "src/util/output.h"
#include "src/buffer_ops/internal.h"

int pmix_bfrop_pack(pmix_buffer_t *buffer,
                    const void *src, int32_t num_vals,
                    pmix_data_type_t type)
{
    int rc;

    /* check for error */
    if (NULL == buffer) {
        return PMIX_ERR_BAD_PARAM;
    }

    /* Pack the number of values */
    if (PMIX_BFROP_BUFFER_FULLY_DESC == buffer->type) {
        if (PMIX_SUCCESS != (rc = pmix_bfrop_store_data_type(buffer, PMIX_INT32))) {
            return rc;
        }
    }
    if (PMIX_SUCCESS != (rc = pmix_bfrop_pack_int32(buffer, &num_vals, 1, PMIX_INT32))) {
        return rc;
    }

    /* Pack the value(s) */
    return pmix_bfrop_pack_buffer(buffer, src, num_vals, type);
}

int pmix_bfrop_pack_buffer(pmix_buffer_t *buffer,
                           const void *src, int32_t num_vals,
                           pmix_data_type_t type)
{
    int rc;
    pmix_bfrop_type_info_t *info;

    PMIX_OUTPUT( ( pmix_bfrop_verbose, "pmix_bfrop_pack_buffer( %p, %p, %lu, %d )\n",
                   (void*)buffer, src, (long unsigned int)num_vals, (int)type ) );

    /* Pack the declared data type */
    if (PMIX_BFROP_BUFFER_FULLY_DESC == buffer->type) {
        if (PMIX_SUCCESS != (rc = pmix_bfrop_store_data_type(buffer, type))) {
            return rc;
        }
    }

    /* Lookup the pack function for this type and call it */

    if (NULL == (info = (pmix_bfrop_type_info_t*)pmix_pointer_array_get_item(&pmix_bfrop_types, type))) {
        return PMIX_ERR_PACK_FAILURE;
    }

    return info->odti_pack_fn(buffer, src, num_vals, type);
}


/* PACK FUNCTIONS FOR GENERIC SYSTEM TYPES */

/*
 * BOOL
 */
int pmix_bfrop_pack_bool(pmix_buffer_t *buffer, const void *src,
                         int32_t num_vals, pmix_data_type_t type)
{
    int ret;

    /* System types need to always be described so we can properly
     * unpack them */
    if (PMIX_SUCCESS != (ret = pmix_bfrop_store_data_type(buffer, BFROP_TYPE_BOOL))) {
        return ret;
    }

    /* Turn around and pack the real type */
    return pmix_bfrop_pack_buffer(buffer, src, num_vals, BFROP_TYPE_BOOL);
}

/*
 * INT
 */
int pmix_bfrop_pack_int(pmix_buffer_t *buffer, const void *src,
                        int32_t num_vals, pmix_data_type_t type)
{
    int ret;

    /* System types need to always be described so we can properly
       unpack them */
    if (PMIX_SUCCESS != (ret = pmix_bfrop_store_data_type(buffer, BFROP_TYPE_INT))) {
        return ret;
    }

    /* Turn around and pack the real type */
    return pmix_bfrop_pack_buffer(buffer, src, num_vals, BFROP_TYPE_INT);
}

/*
 * SIZE_T
 */
int pmix_bfrop_pack_sizet(pmix_buffer_t *buffer, const void *src,
                          int32_t num_vals, pmix_data_type_t type)
{
    int ret;

    /* System types need to always be described so we can properly
       unpack them. */
    if (PMIX_SUCCESS != (ret = pmix_bfrop_store_data_type(buffer, BFROP_TYPE_SIZE_T))) {
        return ret;
    }

    return pmix_bfrop_pack_buffer(buffer, src, num_vals, BFROP_TYPE_SIZE_T);
}

/*
 * PID_T
 */
int pmix_bfrop_pack_pid(pmix_buffer_t *buffer, const void *src,
                        int32_t num_vals, pmix_data_type_t type)
{
    int ret;

    /* System types need to always be described so we can properly
       unpack them. */
    if (PMIX_SUCCESS != (ret = pmix_bfrop_store_data_type(buffer, BFROP_TYPE_PID_T))) {
        return ret;
    }

    /* Turn around and pack the real type */
    return pmix_bfrop_pack_buffer(buffer, src, num_vals, BFROP_TYPE_PID_T);
}


/* PACK FUNCTIONS FOR NON-GENERIC SYSTEM TYPES */

/*
 * BYTE, CHAR, INT8
 */
int pmix_bfrop_pack_byte(pmix_buffer_t *buffer, const void *src,
                         int32_t num_vals, pmix_data_type_t type)
{
    char *dst;

    PMIX_OUTPUT( ( pmix_bfrop_verbose, "pmix_bfrop_pack_byte * %d\n", num_vals ) );
    /* check to see if buffer needs extending */
    if (NULL == (dst = pmix_bfrop_buffer_extend(buffer, num_vals))) {
        return PMIX_ERR_OUT_OF_RESOURCE;
    }

    /* store the data */
    memcpy(dst, src, num_vals);

    /* update buffer pointers */
    buffer->pack_ptr += num_vals;
    buffer->bytes_used += num_vals;

    return PMIX_SUCCESS;
}

/*
 * INT16
 */
int pmix_bfrop_pack_int16(pmix_buffer_t *buffer, const void *src,
                          int32_t num_vals, pmix_data_type_t type)
{
    int32_t i;
    uint16_t tmp, *srctmp = (uint16_t*) src;
    char *dst;

    PMIX_OUTPUT( ( pmix_bfrop_verbose, "pmix_bfrop_pack_int16 * %d\n", num_vals ) );
    /* check to see if buffer needs extending */
    if (NULL == (dst = pmix_bfrop_buffer_extend(buffer, num_vals*sizeof(tmp)))) {
        return PMIX_ERR_OUT_OF_RESOURCE;
    }

    for (i = 0; i < num_vals; ++i) {
        tmp = htons(srctmp[i]);
        memcpy(dst, &tmp, sizeof(tmp));
        dst += sizeof(tmp);
    }
    buffer->pack_ptr += num_vals * sizeof(tmp);
    buffer->bytes_used += num_vals * sizeof(tmp);

    return PMIX_SUCCESS;
}

/*
 * INT32
 */
int pmix_bfrop_pack_int32(pmix_buffer_t *buffer, const void *src,
                          int32_t num_vals, pmix_data_type_t type)
{
    int32_t i;
    uint32_t tmp, *srctmp = (uint32_t*) src;
    char *dst;

    PMIX_OUTPUT( ( pmix_bfrop_verbose, "pmix_bfrop_pack_int32 * %d\n", num_vals ) );
    /* check to see if buffer needs extending */
    if (NULL == (dst = pmix_bfrop_buffer_extend(buffer, num_vals*sizeof(tmp)))) {
        return PMIX_ERR_OUT_OF_RESOURCE;
    }

    for (i = 0; i < num_vals; ++i) {
        tmp = htonl(srctmp[i]);
        memcpy(dst, &tmp, sizeof(tmp));
        dst += sizeof(tmp);
    }
    buffer->pack_ptr += num_vals * sizeof(tmp);
    buffer->bytes_used += num_vals * sizeof(tmp);

    return PMIX_SUCCESS;
}

/*
 * INT64
 */
int pmix_bfrop_pack_int64(pmix_buffer_t *buffer, const void *src,
                          int32_t num_vals, pmix_data_type_t type)
{
    int32_t i;
    uint64_t tmp, *srctmp = (uint64_t*) src;
    char *dst;
    size_t bytes_packed = num_vals * sizeof(tmp);

    PMIX_OUTPUT( ( pmix_bfrop_verbose, "pmix_bfrop_pack_int64 * %d\n", num_vals ) );
    /* check to see if buffer needs extending */
    if (NULL == (dst = pmix_bfrop_buffer_extend(buffer, bytes_packed))) {
        return PMIX_ERR_OUT_OF_RESOURCE;
    }

    for (i = 0; i < num_vals; ++i) {
        tmp = hton64(srctmp[i]);
        memcpy(dst, &tmp, sizeof(tmp));
        dst += sizeof(tmp);
    }
    buffer->pack_ptr += bytes_packed;
    buffer->bytes_used += bytes_packed;

    return PMIX_SUCCESS;
}

/*
 * STRING
 */
int pmix_bfrop_pack_string(pmix_buffer_t *buffer, const void *src,
                           int32_t num_vals, pmix_data_type_t type)
{
    int ret = PMIX_SUCCESS;
    int32_t i, len;
    char **ssrc = (char**) src;

    for (i = 0; i < num_vals; ++i) {
        if (NULL == ssrc[i]) {  /* got zero-length string/NULL pointer - store NULL */
            len = 0;
            if (PMIX_SUCCESS != (ret = pmix_bfrop_pack_int32(buffer, &len, 1, PMIX_INT32))) {
                return ret;
            }
        } else {
            len = (int32_t)strlen(ssrc[i]) + 1;
            if (PMIX_SUCCESS != (ret = pmix_bfrop_pack_int32(buffer, &len, 1, PMIX_INT32))) {
                return ret;
            }
            if (PMIX_SUCCESS != (ret =
                                 pmix_bfrop_pack_byte(buffer, ssrc[i], len, PMIX_BYTE))) {
                return ret;
            }
        }
    }

    return PMIX_SUCCESS;
}

/* FLOAT */
int pmix_bfrop_pack_float(pmix_buffer_t *buffer, const void *src,
                          int32_t num_vals, pmix_data_type_t type)
{
    int ret = PMIX_SUCCESS;
    int32_t i;
    float *ssrc = (float*)src;
    char *convert;

    for (i = 0; i < num_vals; ++i) {
        asprintf(&convert, "%f", ssrc[i]);
        if (PMIX_SUCCESS != (ret = pmix_bfrop_pack_string(buffer, &convert, 1, PMIX_STRING))) {
            free(convert);
            return ret;
        }
        free(convert);
    }

    return PMIX_SUCCESS;
}

/* DOUBLE */
int pmix_bfrop_pack_double(pmix_buffer_t *buffer, const void *src,
                           int32_t num_vals, pmix_data_type_t type)
{
    int ret = PMIX_SUCCESS;
    int32_t i;
    double *ssrc = (double*)src;
    char *convert;

    for (i = 0; i < num_vals; ++i) {
        asprintf(&convert, "%f", ssrc[i]);
        if (PMIX_SUCCESS != (ret = pmix_bfrop_pack_string(buffer, &convert, 1, PMIX_STRING))) {
            free(convert);
            return ret;
        }
        free(convert);
    }

    return PMIX_SUCCESS;
}

/* TIMEVAL */
int pmix_bfrop_pack_timeval(pmix_buffer_t *buffer, const void *src,
                            int32_t num_vals, pmix_data_type_t type)
{
    int64_t tmp[2];
    int ret = PMIX_SUCCESS;
    int32_t i;
    struct timeval *ssrc = (struct timeval *)src;

    for (i = 0; i < num_vals; ++i) {
        tmp[0] = (int64_t)ssrc[i].tv_sec;
        tmp[1] = (int64_t)ssrc[i].tv_usec;
        if (PMIX_SUCCESS != (ret = pmix_bfrop_pack_int64(buffer, tmp, 2, PMIX_INT64))) {
            return ret;
        }
    }

    return PMIX_SUCCESS;
}

/* TIME */
int pmix_bfrop_pack_time(pmix_buffer_t *buffer, const void *src,
                         int32_t num_vals, pmix_data_type_t type)
{
    int ret = PMIX_SUCCESS;
    int32_t i;
    time_t *ssrc = (time_t *)src;
    uint64_t ui64;

    /* time_t is a system-dependent size, so cast it
     * to uint64_t as a generic safe size
     */
    for (i = 0; i < num_vals; ++i) {
        ui64 = (uint64_t)ssrc[i];
        if (PMIX_SUCCESS != (ret = pmix_bfrop_pack_int64(buffer, &ui64, 1, PMIX_UINT64))) {
            return ret;
        }
    }

    return PMIX_SUCCESS;
}


/* PACK FUNCTIONS FOR GENERIC PMIX TYPES */
static int pack_val(pmix_buffer_t *buffer,
                    pmix_value_t *p,
                    pmix_data_type_t type)
{
    int ret;
    
    switch (type) {
    case PMIX_BOOL:
        if (PMIX_SUCCESS != (ret = pmix_bfrop_pack_buffer(buffer, &p->data.flag, 1, PMIX_BOOL))) {
            return ret;
        }
        break;
    case PMIX_BYTE:
        if (PMIX_SUCCESS != (ret = pmix_bfrop_pack_buffer(buffer, &p->data.byte, 1, PMIX_BYTE))) {
            return ret;
        }
        break;
    case PMIX_STRING:
        if (PMIX_SUCCESS != (ret = pmix_bfrop_pack_buffer(buffer, &p->data.string, 1, PMIX_STRING))) {
            return ret;
        }
        break;
    case PMIX_SIZE:
        if (PMIX_SUCCESS != (ret = pmix_bfrop_pack_buffer(buffer, &p->data.size, 1, PMIX_SIZE))) {
            return ret;
        }
        break;
    case PMIX_PID:
        if (PMIX_SUCCESS != (ret = pmix_bfrop_pack_buffer(buffer, &p->data.pid, 1, PMIX_PID))) {
            return ret;
        }
        break;
    case PMIX_INT:
        if (PMIX_SUCCESS != (ret = pmix_bfrop_pack_buffer(buffer, &p->data.integer, 1, PMIX_INT))) {
            return ret;
        }
        break;
    case PMIX_INT8:
        if (PMIX_SUCCESS != (ret = pmix_bfrop_pack_buffer(buffer, &p->data.int8, 1, PMIX_INT8))) {
            return ret;
        }
        break;
    case PMIX_INT16:
        if (PMIX_SUCCESS != (ret = pmix_bfrop_pack_buffer(buffer, &p->data.int16, 1, PMIX_INT16))) {
            return ret;
        }
        break;
    case PMIX_INT32:
        if (PMIX_SUCCESS != (ret = pmix_bfrop_pack_buffer(buffer, &p->data.int32, 1, PMIX_INT32))) {
            return ret;
        }
        break;
    case PMIX_INT64:
        if (PMIX_SUCCESS != (ret = pmix_bfrop_pack_buffer(buffer, &p->data.int64, 1, PMIX_INT64))) {
            return ret;
        }
        break;
    case PMIX_UINT:
        if (PMIX_SUCCESS != (ret = pmix_bfrop_pack_buffer(buffer, &p->data.uint, 1, PMIX_UINT))) {
            return ret;
        }
        break;
    case PMIX_UINT8:
        if (PMIX_SUCCESS != (ret = pmix_bfrop_pack_buffer(buffer, &p->data.uint8, 1, PMIX_UINT8))) {
            return ret;
        }
        break;
    case PMIX_UINT16:
        if (PMIX_SUCCESS != (ret = pmix_bfrop_pack_buffer(buffer, &p->data.uint16, 1, PMIX_UINT16))) {
            return ret;
        }
        break;
    case PMIX_UINT32:
        if (PMIX_SUCCESS != (ret = pmix_bfrop_pack_buffer(buffer, &p->data.uint32, 1, PMIX_UINT32))) {
            return ret;
        }
        break;
    case PMIX_UINT64:
        if (PMIX_SUCCESS != (ret = pmix_bfrop_pack_buffer(buffer, &p->data.uint64, 1, PMIX_UINT64))) {
            return ret;
        }
        break;
    case PMIX_FLOAT:
        if (PMIX_SUCCESS != (ret = pmix_bfrop_pack_buffer(buffer, &p->data.fval, 1, PMIX_FLOAT))) {
            return ret;
        }
        break;
    case PMIX_DOUBLE:
        if (PMIX_SUCCESS != (ret = pmix_bfrop_pack_buffer(buffer, &p->data.dval, 1, PMIX_DOUBLE))) {
            return ret;
        }
        break;
    case PMIX_TIMEVAL:
        if (PMIX_SUCCESS != (ret = pmix_bfrop_pack_buffer(buffer, &p->data.tv, 1, PMIX_TIMEVAL))) {
            return ret;
        }
        break;
    default:
        pmix_output(0, "PACK-PMIX-VALUE: UNSUPPORTED TYPE %d", (int)type);
        return PMIX_ERROR;
    }
    return PMIX_SUCCESS;
}

/*
 * PMIX_VALUE
 */
int pmix_bfrop_pack_value(pmix_buffer_t *buffer, const void *src,
                          int32_t num_vals, pmix_data_type_t type)
{
    pmix_value_t **ptr;
    int32_t i;
    int ret;

    ptr = (pmix_value_t **) src;
    
    for (i = 0; i < num_vals; ++i) {
        /* pack the type */
        if (PMIX_SUCCESS != (ret = pmix_bfrop_pack_int(buffer, &ptr[i]->type, 1, PMIX_INT))) {
            return ret;
        }
        /* now pack the right field */
        if (PMIX_SUCCESS != (ret = pack_val(buffer, ptr[i], ptr[i]->type))) {
            return ret;
        }
    }

    return PMIX_SUCCESS;
}


int pmix_bfrop_pack_info(pmix_buffer_t *buffer, const void *src,
                         int32_t num_vals, pmix_data_type_t type)
{
    pmix_info_t **ptr, *info;
    int32_t i;
    int ret;

    ptr = (pmix_info_t **) src;
    
    for (i = 0; i < num_vals; ++i) {
        info = ptr[i];
        /* pack key */
        if (PMIX_SUCCESS != (ret = pmix_bfrop_pack_string(buffer, &info->key, 1, PMIX_STRING))) {
            return ret;
        }
        /* pack value */
        if (PMIX_SUCCESS != (ret = pmix_bfrop_pack_string(buffer, &info->value, 1, PMIX_STRING))) {
            return ret;
        }
    }
    return PMIX_SUCCESS;
}

int pmix_bfrop_pack_buf(pmix_buffer_t *buffer, const void *src,
                        int32_t num_vals, pmix_data_type_t type)
{
    pmix_buffer_t **ptr;
    int32_t i;
    int ret;

    ptr = (pmix_buffer_t **) src;
    
    for (i = 0; i < num_vals; ++i) {
        /* pack the number of bytes */
        if (PMIX_SUCCESS != (ret = pmix_bfrop_pack_sizet(buffer, &ptr[i]->bytes_used, 1, PMIX_SIZE))) {
            return ret;
        }
        /* pack the bytes */
        if (0 < ptr[i]->bytes_used) {
            if (PMIX_SUCCESS != (ret = pmix_bfrop_pack_byte(buffer, ptr[i]->base_ptr, ptr[i]->bytes_used, PMIX_BYTE))) {
                return ret;
            }
        }
    }
    return PMIX_SUCCESS;
}

int pmix_bfrop_pack_app(pmix_buffer_t *buffer, const void *src,
                        int32_t num_vals, pmix_data_type_t type)
{
    pmix_app_t **ptr, *app;
    int32_t i, j, nvals;
    int ret;
    
    ptr = (pmix_app_t **) src;
    
    for (i = 0; i < num_vals; ++i) {
        app = ptr[i];
        if (PMIX_SUCCESS != (ret = pmix_bfrop_pack_string(buffer, &app->cmd, 1, PMIX_STRING))) {
            return ret;
        }
        /* argv */
        if (PMIX_SUCCESS != (ret = pmix_bfrop_pack_int(buffer, &app->argc, 1, PMIX_INT))) {
            return ret;
        }
        for (j=0; j < app->argc; j++) {
            if (PMIX_SUCCESS != (ret = pmix_bfrop_pack_string(buffer, &app->argv[j], 1, PMIX_STRING))) {
                return ret;
            }
        }
        /* env */
        nvals = pmix_argv_count(app->env);
        if (PMIX_SUCCESS != (ret = pmix_bfrop_pack_int32(buffer, &nvals, 1, PMIX_INT32))) {
            return ret;
        }
        for (j=0; j < nvals; j++) {
            if (PMIX_SUCCESS != (ret = pmix_bfrop_pack_string(buffer, &app->env[j], 1, PMIX_STRING))) {
                return ret;
            }
        }
        /* maxprocs */
        if (PMIX_SUCCESS != (ret = pmix_bfrop_pack_int(buffer, &app->maxprocs, 1, PMIX_INT))) {
            return ret;
        }
        /* info array */
    }
    return PMIX_SUCCESS;
}


int pmix_bfrop_pack_kval(pmix_buffer_t *buffer, const void *src,
                         int32_t num_vals, pmix_data_type_t type)
{
    pmix_kval_t **ptr;
    int32_t i;
    int ret;

    ptr = (pmix_kval_t **) src;
    
    for (i = 0; i < num_vals; ++i) {
        /* pack the key */
        if (PMIX_SUCCESS != (ret = pmix_bfrop_pack_int(buffer, &ptr[i]->key, 1, PMIX_STRING))) {
            return ret;
        }
        /* pack the type */
        if (PMIX_SUCCESS != (ret = pmix_bfrop_pack_int(buffer, &ptr[i]->value->type, 1, PMIX_INT))) {
            return ret;
        }
        /* now pack the right field */
        if (PMIX_SUCCESS != (ret = pack_val(buffer, ptr[i]->value, ptr[i]->value->type))) {
            return ret;
        }
    }

    return PMIX_SUCCESS;
}
