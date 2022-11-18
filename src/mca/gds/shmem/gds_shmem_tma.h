/*
 * Copyright (c) 2022      Triad National Security, LLC. All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#ifndef PMIX_GDS_SHMEM_TMA_H
#define PMIX_GDS_SHMEM_TMA_H

#include "gds_shmem.h"

/**
 * This modifies PMIX_VALUE_XFER() for our needs.
 */
#define PMIX_GDS_SHMEM_TMA_VALUE_XFER(r, v, s, tma)                            \
do {                                                                           \
    if (NULL == (v)) {                                                         \
        (v) = (pmix_value_t *)pmix_tma_malloc((tma), sizeof(pmix_value_t));    \
        if (NULL == (v)) {                                                     \
            (r) = PMIX_ERR_NOMEM;                                              \
        }                                                                      \
        else {                                                                 \
            (r) = pmix_gds_shmem_value_xfer((v), (s), (tma));                  \
        }                                                                      \
    } else {                                                                   \
        (r) = pmix_gds_shmem_value_xfer((v), (s), (tma));                      \
    }                                                                          \
} while(0)

/**
 * This modifies PMIX_BFROPS_COPY() for our needs.
 */
#define PMIX_GDS_SHMEM_TMA_BFROPS_COPY_TMA(r, d, s, t, tma)                    \
(r) = pmix_gds_shmem_bfrops_base_copy_value(d, s, t, tma)

// TODO(skg) This shouldn't live here. Figure out a way to get this in an
// appropriate place. This modifies PMIX_INFO_CREATE() for our needs.
#define PMIX_GDS_SHMEM_TMA_INFO_CREATE(m, n, tma)                              \
do {                                                                           \
    pmix_info_t *i;                                                            \
    (m) = (pmix_info_t *)pmix_tma_calloc((tma), (n), sizeof(pmix_info_t));     \
    if (NULL != (m)) {                                                         \
        i = (pmix_info_t *)(m);                                                \
        i[(n) - 1].flags = PMIX_INFO_ARRAY_END;                                \
    }                                                                          \
} while (0)

// TODO(skg) This shouldn't live here. Figure out a way to get this in an
// appropriate place. This modifies PMIX_INFO_XFER() for our needs.
#define PMIX_GDS_SHMEM_TMA_INFO_XFER(d, s, tma)                                \
    (void)pmix_gds_shmem_info_xfer(d, s, tma)

// TODO(skg) This shouldn't live here. Figure out a way to get this in an
// appropriate place. This modifies PMIX_PROC_CREATE() for our needs.
#define PMIX_GDS_SHMEM_TMA_PROC_CREATE(m, n, tma)                              \
do {                                                                           \
    (m) = (pmix_proc_t *)pmix_tma_calloc((tma), (n), sizeof(pmix_proc_t));     \
} while (0)

BEGIN_C_DECLS

static inline pmix_status_t
pmix_gds_shmem_copy_darray(
    pmix_data_array_t **dest,
    pmix_data_array_t *src,
    pmix_data_type_t type,
    pmix_tma_t *tma
);

// TODO(skg) This shouldn't live here. Figure out a way to get this in an
// appropriate place. This modifies pmix_bfrops_base_value_xfer() for our needs.
static inline pmix_status_t
pmix_gds_shmem_value_xfer(
    pmix_value_t *p,
    const pmix_value_t *src,
    pmix_tma_t *tma
) {
    p->type = src->type;

    switch (src->type) {
        case PMIX_UNDEF:
            break;
        case PMIX_BOOL:
            p->data.flag = src->data.flag;
            break;
        case PMIX_BYTE:
            p->data.byte = src->data.byte;
            break;
        case PMIX_STRING:
            if (NULL != src->data.string) {
                p->data.string = pmix_tma_strdup(tma, src->data.string);
            } else {
                p->data.string = NULL;
            }
            break;
        case PMIX_SIZE:
            p->data.size = src->data.size;
            break;
        case PMIX_PID:
            p->data.pid = src->data.pid;
            break;
        case PMIX_INT:
            /* to avoid alignment issues */
            memcpy(&p->data.integer, &src->data.integer, sizeof(int));
            break;
        case PMIX_INT8:
            p->data.int8 = src->data.int8;
            break;
        case PMIX_INT16:
            /* to avoid alignment issues */
            memcpy(&p->data.int16, &src->data.int16, 2);
            break;
        case PMIX_INT32:
            /* to avoid alignment issues */
            memcpy(&p->data.int32, &src->data.int32, 4);
            break;
        case PMIX_INT64:
            /* to avoid alignment issues */
            memcpy(&p->data.int64, &src->data.int64, 8);
            break;
        case PMIX_UINT:
            /* to avoid alignment issues */
            memcpy(&p->data.uint, &src->data.uint, sizeof(unsigned int));
            break;
        case PMIX_UINT8:
            p->data.uint8 = src->data.uint8;
            break;
        case PMIX_UINT16:
        case PMIX_STOR_ACCESS_TYPE:
            /* to avoid alignment issues */
            memcpy(&p->data.uint16, &src->data.uint16, 2);
            break;
        case PMIX_UINT32:
            /* to avoid alignment issues */
            memcpy(&p->data.uint32, &src->data.uint32, 4);
            break;
        case PMIX_UINT64:
        case PMIX_STOR_MEDIUM:
        case PMIX_STOR_ACCESS:
        case PMIX_STOR_PERSIST:
           /* to avoid alignment issues */
            memcpy(&p->data.uint64, &src->data.uint64, 8);
            break;
        case PMIX_FLOAT:
            p->data.fval = src->data.fval;
            break;
        case PMIX_DOUBLE:
            p->data.dval = src->data.dval;
            break;
        case PMIX_TIMEVAL:
            memcpy(&p->data.tv, &src->data.tv, sizeof(struct timeval));
            break;
        case PMIX_TIME:
            memcpy(&p->data.time, &src->data.time, sizeof(time_t));
            break;
        case PMIX_STATUS:
            memcpy(&p->data.status, &src->data.status, sizeof(pmix_status_t));
            break;
        case PMIX_PROC_RANK:
            memcpy(&p->data.rank, &src->data.rank, sizeof(pmix_rank_t));
            break;
        case PMIX_PROC:
            PMIX_GDS_SHMEM_TMA_PROC_CREATE(p->data.proc, 1, tma);
            if (NULL == p->data.proc) {
                return PMIX_ERR_NOMEM;
            }
            memcpy(p->data.proc, src->data.proc, sizeof(pmix_proc_t));
            break;
        case PMIX_BYTE_OBJECT:
        case PMIX_COMPRESSED_STRING:
        case PMIX_REGEX:
        case PMIX_COMPRESSED_BYTE_OBJECT:
            memset(&p->data.bo, 0, sizeof(pmix_byte_object_t));
            if (NULL != src->data.bo.bytes && 0 < src->data.bo.size) {
                p->data.bo.bytes = pmix_tma_malloc(tma, src->data.bo.size);
                memcpy(p->data.bo.bytes, src->data.bo.bytes, src->data.bo.size);
                p->data.bo.size = src->data.bo.size;
            } else {
                p->data.bo.bytes = NULL;
                p->data.bo.size = 0;
            }
            break;
        case PMIX_PERSIST:
            memcpy(
                &p->data.persist,
                &src->data.persist,
                sizeof(pmix_persistence_t)
            );
            break;
        case PMIX_SCOPE:
            memcpy(&p->data.scope, &src->data.scope, sizeof(pmix_scope_t));
            break;
        case PMIX_DATA_RANGE:
            memcpy(&p->data.range, &src->data.range, sizeof(pmix_data_range_t));
            break;
        case PMIX_PROC_STATE:
            memcpy(&p->data.state, &src->data.state, sizeof(pmix_proc_state_t));
            break;
        case PMIX_DATA_ARRAY:
            return pmix_gds_shmem_copy_darray(
                &p->data.darray,
                src->data.darray,
                PMIX_DATA_ARRAY,
                tma
            );
        case PMIX_POINTER:
            p->data.ptr = src->data.ptr;
            break;
        case PMIX_ALLOC_DIRECTIVE:
            memcpy(
                &p->data.adir,
                &src->data.adir,
                sizeof(pmix_alloc_directive_t)
            );
            break;
        case PMIX_ENVAR:
            // NOTE(skg) This one is okay because all it does is set members.
            PMIX_ENVAR_CONSTRUCT(&p->data.envar);
            if (NULL != src->data.envar.envar) {
                p->data.envar.envar = pmix_tma_strdup(tma, src->data.envar.envar);
            }
            if (NULL != src->data.envar.value) {
                p->data.envar.value = pmix_tma_strdup(tma, src->data.envar.value);
            }
            p->data.envar.separator = src->data.envar.separator;
            break;
        case PMIX_LINK_STATE:
            memcpy(
                &p->data.linkstate,
                &src->data.linkstate,
                sizeof(pmix_link_state_t)
            );
            break;
        case PMIX_JOB_STATE:
            memcpy(
                &p->data.jstate,
                &src->data.jstate,
                sizeof(pmix_job_state_t)
            );
            break;
        case PMIX_LOCTYPE:
            memcpy(
                &p->data.locality,
                &src->data.locality,
                sizeof(pmix_locality_t)
            );
            break;
        case PMIX_DEVTYPE:
            memcpy(
                &p->data.devtype,
                &src->data.devtype,
                sizeof(pmix_device_type_t)
            );
            break;
        // TODO(skg) Implement currently unsupported types.
        case PMIX_PROC_NSPACE:
        case PMIX_PROC_INFO:
        case PMIX_COORD:
        case PMIX_TOPO:
        case PMIX_PROC_CPUSET:
        case PMIX_GEOMETRY:
        case PMIX_DEVICE_DIST:
        case PMIX_ENDPOINT:
        case PMIX_REGATTR:
        case PMIX_DATA_BUFFER:
        case PMIX_PROC_STATS:
        case PMIX_DISK_STATS:
        case PMIX_NET_STATS:
        case PMIX_NODE_STATS:
        default:
            pmix_output(
                0, "%s: UNSUPPORTED TYPE: %s",
                __func__, PMIx_Data_type_string(src->type)
            );
            return PMIX_ERROR;
    }
    return PMIX_SUCCESS;
}

// TODO(skg) This shouldn't live here. Figure out a way to get this in an
// appropriate place. This modifies PMIX_INFO_XFER() for our needs.
static inline pmix_status_t
pmix_gds_shmem_info_xfer(
    pmix_info_t *dest,
    const pmix_info_t *src,
    pmix_tma_t *tma
) {
    if (NULL == dest || NULL == src) {
        return PMIX_ERR_BAD_PARAM;
    }
    PMIX_LOAD_KEY(dest->key, src->key);
    return pmix_gds_shmem_value_xfer(&dest->value, &src->value, tma);
}

// TODO(skg) This shouldn't live here. Figure out a way to get this in an
// appropriate place. This modifies pmix_bfrops_base_copy_darray() for our needs.
static inline pmix_status_t
pmix_gds_shmem_copy_darray(
    pmix_data_array_t **dest,
    pmix_data_array_t *src,
    pmix_data_type_t type,
    pmix_tma_t *tma
) {
    pmix_status_t rc = PMIX_SUCCESS;
    pmix_data_array_t *p = NULL;

    PMIX_HIDE_UNUSED_PARAMS(type);

    p = (pmix_data_array_t *)pmix_tma_calloc(tma, 1, sizeof(pmix_data_array_t));
    if (NULL == p) {
        rc = PMIX_ERR_NOMEM;
        goto out;
    }

    p->type = src->type;
    p->size = src->size;

    if (0 == p->size || NULL == src->array) {
        // Done!
        goto out;
    }

    // Process based on type of array element.
    switch (src->type) {
        case PMIX_UINT8:
        case PMIX_INT8:
        case PMIX_BYTE:
            p->array = (char *)pmix_tma_malloc(tma, src->size);
            if (NULL == p->array) {
                rc = PMIX_ERR_NOMEM;
                goto out;
            }
            memcpy(p->array, src->array, src->size);
            break;
        case PMIX_UINT16:
        case PMIX_INT16:
            p->array = (char *)pmix_tma_malloc(tma, src->size * sizeof(uint16_t));
            if (NULL == p->array) {
                rc = PMIX_ERR_NOMEM;
                goto out;
            }
            memcpy(p->array, src->array, src->size * sizeof(uint16_t));
            break;
        case PMIX_UINT32:
        case PMIX_INT32:
            p->array = (char *)pmix_tma_malloc(tma, src->size * sizeof(uint32_t));
            if (NULL == p->array) {
                rc = PMIX_ERR_NOMEM;
                goto out;
            }
            memcpy(p->array, src->array, src->size * sizeof(uint32_t));
            break;
        case PMIX_UINT64:
        case PMIX_INT64:
            p->array = (char *)pmix_tma_malloc(tma, src->size * sizeof(uint64_t));
            if (NULL == p->array) {
                rc = PMIX_ERR_NOMEM;
                goto out;
            }
            memcpy(p->array, src->array, src->size * sizeof(uint64_t));
            break;
        case PMIX_BOOL:
            p->array = (char *)pmix_tma_malloc(tma, src->size * sizeof(bool));
            if (NULL == p->array) {
                rc = PMIX_ERR_NOMEM;
                goto out;
            }
            memcpy(p->array, src->array, src->size * sizeof(bool));
            break;
        case PMIX_SIZE:
            p->array = (char *)pmix_tma_malloc(tma, src->size * sizeof(size_t));
            if (NULL == p->array) {
                rc = PMIX_ERR_NOMEM;
                goto out;
            }
            memcpy(p->array, src->array, src->size * sizeof(size_t));
            break;
        case PMIX_PID:
            p->array = (char *)pmix_tma_malloc(tma, src->size * sizeof(pid_t));
            if (NULL == p->array) {
                rc = PMIX_ERR_NOMEM;
                goto out;
            }
            memcpy(p->array, src->array, src->size * sizeof(pid_t));
            break;
        case PMIX_STRING: {
            char **prarray, **strarray;
            p->array = (char **)pmix_tma_malloc(tma, src->size * sizeof(char *));
            if (NULL == p->array) {
                rc = PMIX_ERR_NOMEM;
                goto out;
            }
            prarray = (char **)p->array;
            strarray = (char **)src->array;
            for (size_t n = 0; n < src->size; n++) {
                if (NULL != strarray[n]) {
                    prarray[n] = pmix_tma_strdup(tma, strarray[n]);
                    if (NULL == prarray[n]) {
                        rc = PMIX_ERR_NOMEM;
                        goto out;
                    }
                }
            }
            break;
        }
        case PMIX_INT:
        case PMIX_UINT:
            p->array = (char *)pmix_tma_malloc(tma, src->size * sizeof(int));
            if (NULL == p->array) {
                rc = PMIX_ERR_NOMEM;
                goto out;
            }
            memcpy(p->array, src->array, src->size * sizeof(int));
            break;
        case PMIX_FLOAT:
            p->array = (char *)pmix_tma_malloc(tma, src->size * sizeof(float));
            if (NULL == p->array) {
                rc = PMIX_ERR_NOMEM;
                goto out;
            }
            memcpy(p->array, src->array, src->size * sizeof(float));
            break;
        case PMIX_DOUBLE:
            p->array = (char *)pmix_tma_malloc(tma, src->size * sizeof(double));
            if (NULL == p->array) {
                rc = PMIX_ERR_NOMEM;
                goto out;
            }
            memcpy(p->array, src->array, src->size * sizeof(double));
            break;
        case PMIX_TIMEVAL:
            p->array = (struct timeval *)pmix_tma_malloc(
                tma, src->size * sizeof(struct timeval)
            );
            if (NULL == p->array) {
                rc = PMIX_ERR_NOMEM;
                goto out;
            }
            memcpy(p->array, src->array, src->size * sizeof(struct timeval));
            break;
        case PMIX_TIME:
            p->array = (time_t *)pmix_tma_malloc(tma, src->size * sizeof(time_t));
            if (NULL == p->array) {
                rc = PMIX_ERR_NOMEM;
                goto out;
            }
            memcpy(p->array, src->array, src->size * sizeof(time_t));
            break;
        case PMIX_STATUS:
            p->array = (pmix_status_t *)pmix_tma_malloc(
                tma, src->size * sizeof(pmix_status_t)
            );
            if (NULL == p->array) {
                rc = PMIX_ERR_NOMEM;
                goto out;
            }
            memcpy(p->array, src->array, src->size * sizeof(pmix_status_t));
            break;
        case PMIX_PROC:
            PMIX_GDS_SHMEM_TMA_PROC_CREATE(p->array, src->size, tma);
            if (NULL == p->array) {
                rc = PMIX_ERR_NOMEM;
                goto out;
            }
            memcpy(p->array, src->array, src->size * sizeof(pmix_proc_t));
            break;
        case PMIX_PROC_RANK:
            p->array = (pmix_rank_t *)pmix_tma_malloc(
                tma, src->size * sizeof(pmix_rank_t)
            );
            if (NULL == p->array) {
                rc = PMIX_ERR_NOMEM;
                goto out;
            }
            memcpy(p->array, src->array, src->size * sizeof(pmix_rank_t));
            break;
        case PMIX_INFO: {
            pmix_info_t *p1, *s1;
            PMIX_GDS_SHMEM_TMA_INFO_CREATE(p->array, src->size, tma);
            if (NULL == p->array) {
                rc = PMIX_ERR_NOMEM;
                goto out;
            }
            p1 = (pmix_info_t *)p->array;
            s1 = (pmix_info_t *)src->array;
            for (size_t n = 0; n < src->size; n++) {
                PMIX_GDS_SHMEM_TMA_INFO_XFER(&p1[n], &s1[n], tma);
            }
            break;
        }
        case PMIX_BYTE_OBJECT:
        case PMIX_COMPRESSED_STRING: {
            pmix_byte_object_t *pbo, *sbo;
            p->array = (pmix_byte_object_t *)pmix_tma_malloc(
                tma, src->size * sizeof(pmix_byte_object_t)
            );
            if (NULL == p->array) {
                rc = PMIX_ERR_NOMEM;
                goto out;
            }
            pbo = (pmix_byte_object_t *)p->array;
            sbo = (pmix_byte_object_t *)src->array;
            for (size_t n = 0; n < src->size; n++) {
                if (NULL != sbo[n].bytes && 0 < sbo[n].size) {
                    pbo[n].size = sbo[n].size;
                    pbo[n].bytes = (char *)pmix_tma_malloc(tma, pbo[n].size);
                    memcpy(pbo[n].bytes, sbo[n].bytes, pbo[n].size);
                } else {
                    pbo[n].bytes = NULL;
                    pbo[n].size = 0;
                }
            }
            break;
        }
        case PMIX_PERSIST:
            p->array = (pmix_persistence_t *)pmix_tma_malloc(
                tma, src->size * sizeof(pmix_persistence_t)
            );
            if (NULL == p->array) {
                rc = PMIX_ERR_NOMEM;
                goto out;
            }
            memcpy(p->array, src->array, src->size * sizeof(pmix_persistence_t));
            break;
        case PMIX_SCOPE:
            p->array = (pmix_scope_t *)pmix_tma_malloc(
                tma, src->size * sizeof(pmix_scope_t)
            );
            if (NULL == p->array) {
                rc = PMIX_ERR_NOMEM;
                goto out;
            }
            memcpy(p->array, src->array, src->size * sizeof(pmix_scope_t));
            break;
        case PMIX_DATA_RANGE:
            p->array = (pmix_data_range_t *)pmix_tma_malloc(
                tma, src->size * sizeof(pmix_data_range_t)
            );
            if (NULL == p->array) {
                rc = PMIX_ERR_NOMEM;
                goto out;
            }
            memcpy(p->array, src->array, src->size * sizeof(pmix_data_range_t));
            break;
        case PMIX_COMMAND:
            p->array = (pmix_cmd_t *)pmix_tma_malloc(
                tma, src->size * sizeof(pmix_cmd_t)
            );
            if (NULL == p->array) {
                rc = PMIX_ERR_NOMEM;
                goto out;
            }
            memcpy(p->array, src->array, src->size * sizeof(pmix_cmd_t));
            break;
        case PMIX_INFO_DIRECTIVES:
            p->array = (pmix_info_directives_t *)pmix_tma_malloc(
                tma, src->size * sizeof(pmix_info_directives_t)
            );
            if (NULL == p->array) {
                rc = PMIX_ERR_NOMEM;
                goto out;
            }
            memcpy(p->array, src->array, src->size * sizeof(pmix_info_directives_t));
            break;
        case PMIX_DATA_ARRAY:
            // We don't support nested arrays.
            rc = PMIX_ERR_NOT_SUPPORTED;
            break;
        case PMIX_VALUE:
        case PMIX_APP:
        case PMIX_PDATA:
        case PMIX_BUFFER:
        case PMIX_KVAL:
        // TODO(skg) How are we going to handle this case? My guess is that
        // since these are pointers then the assumption is that they are only
        // valid within the process that asked to store them.
        case PMIX_POINTER:
        case PMIX_PROC_INFO:
        case PMIX_QUERY:
        case PMIX_ENVAR:
        case PMIX_COORD:
        case PMIX_REGATTR:
        case PMIX_PROC_CPUSET:
        case PMIX_GEOMETRY:
        case PMIX_DEVICE_DIST:
        case PMIX_ENDPOINT:
        case PMIX_PROC_NSPACE:
        case PMIX_PROC_STATS:
        case PMIX_DISK_STATS:
        case PMIX_NET_STATS:
        case PMIX_NODE_STATS:
        default:
            pmix_output(
                0, "%s: UNSUPPORTED TYPE: %s",
                __func__, PMIx_Data_type_string(src->type)
            );
            rc = PMIX_ERR_UNKNOWN_DATA_TYPE;
    }
out:
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        pmix_tma_free(tma, p);
        p = NULL;
    }
    *dest = p;
    return rc;
}

// TODO(skg) This shouldn't live here. Figure out a way to get this in an
// appropriate place. This modifies pmix_bfrops_base_copy_value() for our needs.
static inline pmix_status_t
pmix_gds_shmem_bfrops_base_copy_value(
    pmix_value_t **dest,
    pmix_value_t *src,
    pmix_data_type_t type,
    pmix_tma_t *tma
) {
    pmix_value_t *p;

    PMIX_HIDE_UNUSED_PARAMS(type);

    /* create the new object */
    *dest = (pmix_value_t *)pmix_tma_malloc(tma, sizeof(pmix_value_t));
    if (NULL == *dest) {
        return PMIX_ERR_OUT_OF_RESOURCE;
    }
    p = *dest;

    /* copy the type */
    p->type = src->type;
    /* copy the data */
    return pmix_gds_shmem_value_xfer(p, src, tma);
}

END_C_DECLS

#endif

/*
 * vim: ft=cpp ts=4 sts=4 sw=4 expandtab
 */
