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

#include "src/util/argv.h"
#include "src/util/error.h"
#include "src/include/pmix_globals.h"

#include "src/mca/bfrops/base/base.h"

pmix_status_t pmix_bfrops_base_define_rendezvous(pmix_list_t *listeners,
                                                 char *tmpdir)
{
    pmix_bfrops_base_active_module_t *active;
    pmix_status_t rc;

    if (!pmix_bfrops_globals.initialized) {
        return PMIX_ERR_INIT;
    }

    PMIX_LIST_FOREACH(active, &pmix_bfrops_globals.actives, pmix_bfrops_base_active_module_t) {
        if (NULL != active->component->rendezvous) {
            rc = active->component->rendezvous(listeners, tmpdir);
            if (PMIX_SUCCESS != rc) {
                return rc;
            }
        }
    }
    return PMIX_SUCCESS;
}

char* pmix_bfrops_base_get_available_modules(void)
{
    pmix_bfrops_base_active_module_t *active;
    char **tmp=NULL, *reply=NULL;

    if (!pmix_bfrops_globals.initialized) {
        return NULL;
    }

    PMIX_LIST_FOREACH(active, &pmix_bfrops_globals.actives, pmix_bfrops_base_active_module_t) {
        pmix_argv_append_nosize(&tmp, active->component->base.pmix_mca_component_name);
    }
    if (NULL != tmp) {
        reply = pmix_argv_join(tmp, ',');
        pmix_argv_free(tmp);
    }
    return reply;
}

pmix_bfrops_module_t* pmix_bfrops_base_assign_module(const char *version)
{
    pmix_bfrops_base_active_module_t *active;
    pmix_bfrops_module_t *mod;

    if (!pmix_bfrops_globals.initialized) {
        return NULL;
    }

    PMIX_LIST_FOREACH(active, &pmix_bfrops_globals.actives, pmix_bfrops_base_active_module_t) {
        if (NULL != (mod = active->component->assign_module(version))) {
            return mod;
        }
    }

    /* we only get here if nothing was found */
    return NULL;
}

pmix_buffer_t* pmix_bfrops_base_create_buffer(struct pmix_peer_t *pr)
{
    pmix_buffer_t *bf;
    pmix_peer_t *peer = (pmix_peer_t*)pr;

    bf = PMIX_NEW(pmix_buffer_t);
    if (NULL != bf) {
        bf->type = peer->comm.type;
    }
    return bf;
}

void pmix_bfrops_base_construct_buffer(struct pmix_peer_t *pr,
                                       pmix_buffer_t *buf)
{
    pmix_peer_t *peer = (pmix_peer_t*)pr;

    PMIX_CONSTRUCT(buf, pmix_buffer_t);
    buf->type = peer->comm.type;
}

/* define two public functions */
PMIX_EXPORT void pmix_value_load(pmix_value_t *v, void *data,
                                 pmix_data_type_t type)
{
    pmix_bfrops_base_value_load(v, data, type);
}

PMIX_EXPORT pmix_status_t pmix_value_xfer(pmix_value_t *dest,
                                          pmix_value_t *src)
{
    return pmix_bfrops_base_value_xfer(dest, src);
}

void pmix_bfrops_base_value_load(pmix_value_t *v, void *data,
                                 pmix_data_type_t type)
{
    v->type = type;
    if (NULL == data) {
        /* just set the fields to zero */
        memset(&v->data, 0, sizeof(v->data));
    } else {
        switch(type) {
        case PMIX_UNDEF:
            break;
        case PMIX_BOOL:
            memcpy(&(v->data.flag), data, 1);
            break;
        case PMIX_BYTE:
            memcpy(&(v->data.byte), data, 1);
            break;
        case PMIX_STRING:
            v->data.string = strdup(data);
            break;
        case PMIX_SIZE:
            memcpy(&(v->data.size), data, sizeof(size_t));
            break;
        case PMIX_PID:
            memcpy(&(v->data.pid), data, sizeof(pid_t));
            break;
        case PMIX_INT:
            memcpy(&(v->data.integer), data, sizeof(int));
            break;
        case PMIX_INT8:
            memcpy(&(v->data.int8), data, 1);
            break;
        case PMIX_INT16:
            memcpy(&(v->data.int16), data, 2);
            break;
        case PMIX_INT32:
            memcpy(&(v->data.int32), data, 4);
            break;
        case PMIX_INT64:
            memcpy(&(v->data.int64), data, 8);
            break;
        case PMIX_UINT:
            memcpy(&(v->data.uint), data, sizeof(int));
            break;
        case PMIX_UINT8:
            memcpy(&(v->data.uint8), data, 1);
            break;
        case PMIX_UINT16:
            memcpy(&(v->data.uint16), data, 2);
            break;
        case PMIX_UINT32:
            memcpy(&(v->data.uint32), data, 4);
            break;
        case PMIX_UINT64:
            memcpy(&(v->data.uint64), data, 8);
            break;
        case PMIX_FLOAT:
            memcpy(&(v->data.fval), data, sizeof(float));
            break;
        case PMIX_DOUBLE:
            memcpy(&(v->data.dval), data, sizeof(double));
            break;
        case PMIX_TIMEVAL:
            memcpy(&(v->data.tv), data, sizeof(struct timeval));
            break;
        case PMIX_BYTE_OBJECT:
            v->data.bo.bytes = data;
            memcpy(&(v->data.bo.size), data, sizeof(size_t));
            break;
        case PMIX_TIME:
            memcpy(&(v->data.time), data, sizeof(time_t));
            break;
        default:
            /* silence warnings */
            break;
        }
    }
    return;
}

pmix_status_t pmix_bfrops_base_value_unload(pmix_value_t *kv, void **data,
                                            size_t *sz, pmix_data_type_t type)
{
    pmix_status_t rc;

    rc = PMIX_SUCCESS;
    if (type != kv->type) {
        rc = PMIX_ERR_TYPE_MISMATCH;
    } else if (NULL == data ||
               (NULL == *data && PMIX_STRING != type && PMIX_BYTE_OBJECT != type)) {
        rc = PMIX_ERR_BAD_PARAM;
    } else {
        switch(type) {
        case PMIX_UNDEF:
            rc = PMIX_ERR_UNKNOWN_DATA_TYPE;
            break;
        case PMIX_BOOL:
            memcpy(*data, &(kv->data.flag), 1);
            *sz = 1;
            break;
        case PMIX_BYTE:
            memcpy(*data, &(kv->data.byte), 1);
            *sz = 1;
            break;
        case PMIX_STRING:
            if (NULL != kv->data.string) {
                *data = strdup(kv->data.string);
                *sz = strlen(kv->data.string);
            }
            break;
        case PMIX_SIZE:
            memcpy(*data, &(kv->data.size), sizeof(size_t));
            *sz = sizeof(size_t);
            break;
        case PMIX_PID:
            memcpy(*data, &(kv->data.pid), sizeof(pid_t));
            *sz = sizeof(pid_t);
            break;
        case PMIX_INT:
            memcpy(*data, &(kv->data.integer), sizeof(int));
            *sz = sizeof(int);
            break;
        case PMIX_INT8:
            memcpy(*data, &(kv->data.int8), 1);
            *sz = 1;
            break;
        case PMIX_INT16:
            memcpy(*data, &(kv->data.int16), 2);
            *sz = 2;
            break;
        case PMIX_INT32:
            memcpy(*data, &(kv->data.int32), 4);
            *sz = 4;
            break;
        case PMIX_INT64:
            memcpy(*data, &(kv->data.int64), 8);
            *sz = 8;
            break;
        case PMIX_UINT:
            memcpy(*data, &(kv->data.uint), sizeof(int));
            *sz = sizeof(int);
            break;
        case PMIX_UINT8:
            memcpy(*data, &(kv->data.uint8), 1);
            *sz = 1;
            break;
        case PMIX_UINT16:
            memcpy(*data, &(kv->data.uint16), 2);
            *sz = 2;
            break;
        case PMIX_UINT32:
            memcpy(*data, &(kv->data.uint32), 4);
            *sz = 4;
            break;
        case PMIX_UINT64:
            memcpy(*data, &(kv->data.uint64), 8);
            *sz = 8;
            break;
        case PMIX_FLOAT:
            memcpy(*data, &(kv->data.fval), sizeof(float));
            *sz = sizeof(float);
            break;
        case PMIX_DOUBLE:
            memcpy(*data, &(kv->data.dval), sizeof(double));
            *sz = sizeof(double);
            break;
        case PMIX_TIMEVAL:
            memcpy(*data, &(kv->data.tv), sizeof(struct timeval));
            *sz = sizeof(struct timeval);
            break;
        case PMIX_TIME:
            memcpy(*data, &(kv->data.time), sizeof(time_t));
            *sz = sizeof(time_t);
            break;
        case PMIX_BYTE_OBJECT:
            if (NULL != kv->data.bo.bytes && 0 < kv->data.bo.size) {
                *data = kv->data.bo.bytes;
                *sz = kv->data.bo.size;
            } else {
                *data = NULL;
                *sz = 0;
            }
            break;
        default:
            /* silence warnings */
            rc = PMIX_ERROR;
            break;
        }
    }
    return rc;
}

/* compare function for pmix_value_t */
pmix_value_cmp_t pmix_bfrops_base_value_cmp(pmix_value_t *p,
                                            pmix_value_t *p1)
{
    pmix_value_cmp_t rc = PMIX_VALUE1_GREATER;

    if (p->type != p1->type) {
        return rc;
    }

    switch (p->type) {
        case PMIX_UNDEF:
            rc = PMIX_EQUAL;
            break;
        case PMIX_BOOL:
            if (p->data.flag == p1->data.flag) {
                rc = PMIX_EQUAL;
            }
            break;
        case PMIX_BYTE:
            if (p->data.byte == p1->data.byte) {
                rc = PMIX_EQUAL;
            }
            break;
        case PMIX_SIZE:
            if (p->data.size == p1->data.size) {
                rc = PMIX_EQUAL;
            }
            break;
        case PMIX_INT:
            if (p->data.integer == p1->data.integer) {
                rc = PMIX_EQUAL;
            }
            break;
        case PMIX_INT8:
            if (p->data.int8 == p1->data.int8) {
                rc = PMIX_EQUAL;
            }
            break;
        case PMIX_INT16:
            if (p->data.int16 == p1->data.int16) {
                rc = PMIX_EQUAL;
            }
            break;
        case PMIX_INT32:
            if (p->data.int32 == p1->data.int32) {
                rc = PMIX_EQUAL;
            }
            break;
        case PMIX_INT64:
            if (p->data.int64 == p1->data.int64) {
                rc = PMIX_EQUAL;
            }
            break;
        case PMIX_UINT:
            if (p->data.uint == p1->data.uint) {
                rc = PMIX_EQUAL;
            }
            break;
        case PMIX_UINT8:
            if (p->data.uint8 == p1->data.int8) {
                rc = PMIX_EQUAL;
            }
            break;
        case PMIX_UINT16:
            if (p->data.uint16 == p1->data.uint16) {
                rc = PMIX_EQUAL;
            }
            break;
        case PMIX_UINT32:
            if (p->data.uint32 == p1->data.uint32) {
                rc = PMIX_EQUAL;
            }
            break;
        case PMIX_UINT64:
            if (p->data.uint64 == p1->data.uint64) {
                rc = PMIX_EQUAL;
            }
            break;
        case PMIX_STRING:
            if (0 == strcmp(p->data.string, p1->data.string)) {
                rc = PMIX_EQUAL;
            }
            break;
        case PMIX_STATUS:
            if (p->data.status == p1->data.status) {
                rc = PMIX_EQUAL;
            }
            break;
        default:
            pmix_output(0, "COMPARE-PMIX-VALUE: UNSUPPORTED TYPE %d", (int)p->type);
    }
    return rc;
}

/* Xfer FUNCTIONS FOR GENERIC PMIX TYPES */
pmix_status_t pmix_bfrops_base_value_xfer(pmix_value_t *dest,
                                          pmix_value_t *src)
{
    pmix_info_t *p1, *s1;

    /* copy the right field */
    dest->type = src->type;
    switch (src->type) {
    case PMIX_UNDEF:
        break;
    case PMIX_BOOL:
        dest->data.flag = src->data.flag;
        break;
    case PMIX_BYTE:
        dest->data.byte = src->data.byte;
        break;
    case PMIX_STRING:
        if (NULL != src->data.string) {
            dest->data.string = strdup(src->data.string);
        } else {
            dest->data.string = NULL;
        }
        break;
    case PMIX_SIZE:
        dest->data.size = src->data.size;
        break;
    case PMIX_PID:
        dest->data.pid = src->data.pid;
        break;
    case PMIX_INT:
        /* to avoid alignment issues */
        memcpy(&dest->data.integer, &src->data.integer, sizeof(int));
        break;
    case PMIX_INT8:
        dest->data.int8 = src->data.int8;
        break;
    case PMIX_INT16:
        /* to avoid alignment issues */
        memcpy(&dest->data.int16, &src->data.int16, 2);
        break;
    case PMIX_INT32:
        /* to avoid alignment issues */
        memcpy(&dest->data.int32, &src->data.int32, 4);
        break;
    case PMIX_INT64:
        /* to avoid alignment issues */
        memcpy(&dest->data.int64, &src->data.int64, 8);
        break;
    case PMIX_UINT:
        /* to avoid alignment issues */
        memcpy(&dest->data.uint, &src->data.uint, sizeof(unsigned int));
        break;
    case PMIX_UINT8:
        dest->data.uint8 = src->data.uint8;
        break;
    case PMIX_UINT16:
        /* to avoid alignment issues */
        memcpy(&dest->data.uint16, &src->data.uint16, 2);
        break;
    case PMIX_UINT32:
        /* to avoid alignment issues */
        memcpy(&dest->data.uint32, &src->data.uint32, 4);
        break;
    case PMIX_UINT64:
        /* to avoid alignment issues */
        memcpy(&dest->data.uint64, &src->data.uint64, 8);
        break;
    case PMIX_FLOAT:
        dest->data.fval = src->data.fval;
        break;
    case PMIX_DOUBLE:
        dest->data.dval = src->data.dval;
        break;
    case PMIX_TIMEVAL:
        dest->data.tv.tv_sec = src->data.tv.tv_sec;
        dest->data.tv.tv_usec = src->data.tv.tv_usec;
        break;
    case PMIX_TIME:
        dest->data.time = src->data.time;
        dest->data.time = src->data.time;
        break;
    case PMIX_STATUS:
        memcpy(&dest->data.status, &src->data.status, sizeof(pmix_status_t));
        break;
    case PMIX_INFO_ARRAY:
        dest->data.array.size = src->data.array.size;
        if (0 < src->data.array.size) {
            dest->data.array.array = (struct pmix_info_t*)malloc(src->data.array.size * sizeof(pmix_info_t));
            p1 = (pmix_info_t*)dest->data.array.array;
            s1 = (pmix_info_t*)src->data.array.array;
            memcpy(p1, s1, src->data.array.size * sizeof(pmix_info_t));
        }
        break;
    case PMIX_BYTE_OBJECT:
        if (NULL != src->data.bo.bytes && 0 < src->data.bo.size) {
            dest->data.bo.bytes = malloc(src->data.bo.size);
            memcpy(dest->data.bo.bytes, src->data.bo.bytes, src->data.bo.size);
            dest->data.bo.size = src->data.bo.size;
        } else {
            dest->data.bo.bytes = NULL;
            dest->data.bo.size = 0;
        }
        break;
    case PMIX_POINTER:
        memcpy(&dest->data.ptr, &src->data.ptr, sizeof(void*));
        break;
    default:
        pmix_output(0, "COPY-PMIX-VALUE: UNSUPPORTED TYPE %d", (int)src->type);
        return PMIX_ERR_NOT_SUPPORTED;
    }
    return PMIX_SUCCESS;
}


/**
 * Internal function that resizes (expands) an inuse buffer if
 * necessary.
 */
char* pmix_bfrop_buffer_extend(pmix_buffer_t *buffer, size_t bytes_to_add)
{
    size_t required, to_alloc;
    size_t pack_offset, unpack_offset;

    /* Check to see if we have enough space already */

    if ((buffer->bytes_allocated - buffer->bytes_used) >= bytes_to_add) {
        return buffer->pack_ptr;
    }

    required = buffer->bytes_used + bytes_to_add;
    if (required >= pmix_bfrops_globals.threshold_size) {
        to_alloc = ((required + pmix_bfrops_globals.threshold_size - 1)
                    / pmix_bfrops_globals.threshold_size) * pmix_bfrops_globals.threshold_size;
    } else {
        to_alloc = buffer->bytes_allocated;
        if (0 == to_alloc) {
            to_alloc = pmix_bfrops_globals.initial_size;
        }
        while (to_alloc < required) {
            to_alloc <<= 1;
        }
    }

    if (NULL != buffer->base_ptr) {
        pack_offset = ((char*) buffer->pack_ptr) - ((char*) buffer->base_ptr);
        unpack_offset = ((char*) buffer->unpack_ptr) -
            ((char*) buffer->base_ptr);
        buffer->base_ptr = (char*)realloc(buffer->base_ptr, to_alloc);
        memset(buffer->base_ptr + pack_offset, 0, to_alloc - buffer->bytes_allocated);
    } else {
        pack_offset = 0;
        unpack_offset = 0;
        buffer->bytes_used = 0;
        buffer->base_ptr = (char*)malloc(to_alloc);
        memset(buffer->base_ptr, 0, to_alloc);
    }

    if (NULL == buffer->base_ptr) {
        return NULL;
    }
    buffer->pack_ptr = ((char*) buffer->base_ptr) + pack_offset;
    buffer->unpack_ptr = ((char*) buffer->base_ptr) + unpack_offset;
    buffer->bytes_allocated = to_alloc;

    /* All done */

    return buffer->pack_ptr;
}

/*
 * Internal function that checks to see if the specified number of bytes
 * remain in the buffer for unpacking
 */
bool pmix_bfrop_too_small(pmix_buffer_t *buffer, size_t bytes_reqd)
{
    size_t bytes_remaining_packed;

    if (buffer->pack_ptr < buffer->unpack_ptr) {
        return true;
    }

    bytes_remaining_packed = buffer->pack_ptr - buffer->unpack_ptr;

    if (bytes_remaining_packed < bytes_reqd) {
        /* don't error log this - it could be that someone is trying to
         * simply read until the buffer is empty
         */
        return true;
    }

    return false;
}

pmix_status_t pmix_bfrop_store_data_type(pmix_buffer_t *buffer, pmix_data_type_t type)
{
    uint16_t tmp;
    char *dst;

    /* check to see if buffer needs extending */
     if (NULL == (dst = pmix_bfrop_buffer_extend(buffer, sizeof(tmp)))) {
        return PMIX_ERR_OUT_OF_RESOURCE;
    }

    tmp = pmix_htons(type);
    memcpy(dst, &tmp, sizeof(tmp));
    buffer->pack_ptr += sizeof(tmp);
    buffer->bytes_used += sizeof(tmp);

    return PMIX_SUCCESS;
}

pmix_status_t pmix_bfrop_get_data_type(pmix_buffer_t *buffer, pmix_data_type_t *type)
{
    uint16_t tmp;

    /* check to see if there's enough data in buffer */
    if (pmix_bfrop_too_small(buffer, sizeof(tmp))) {
        return PMIX_ERR_UNPACK_READ_PAST_END_OF_BUFFER;
    }

    /* unpack the data */
    memcpy(&tmp, buffer->unpack_ptr, sizeof(tmp));
    tmp = pmix_ntohs(tmp);
    memcpy(type, &tmp, sizeof(tmp));
    buffer->unpack_ptr += sizeof(tmp);

    return PMIX_SUCCESS;
}
