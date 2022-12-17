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

#include "src/mca/bfrops/base/bfrop_base_tma.h"

BEGIN_C_DECLS

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
            p->array = pmix_bfrops_base_tma_proc_create(src->size, tma);
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
            p->array = pmix_bfrops_base_tma_info_create(src->size, tma);
            if (NULL == p->array) {
                rc = PMIX_ERR_NOMEM;
                goto out;
            }
            p1 = (pmix_info_t *)p->array;
            s1 = (pmix_info_t *)src->array;
            for (size_t n = 0; n < src->size; n++) {
                (void)pmix_bfrops_base_tma_info_xfer(&p1[n], &s1[n], tma);
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

END_C_DECLS

#endif

/*
 * vim: ft=cpp ts=4 sts=4 sw=4 expandtab
 */
