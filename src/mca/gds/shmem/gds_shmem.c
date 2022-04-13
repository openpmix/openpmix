/*
 * Copyright (c) 2015-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2016-2018 IBM Corporation.  All rights reserved.
 * Copyright (c) 2018      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2018-2020 Mellanox Technologies, Inc.
 *                         All rights reserved.
 * Copyright (c) 2022      Nanook Consulting.  All rights reserved.
 * Copyright (c) 2022      Triad National Security, LLC. All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "src/include/pmix_config.h"
#include "include/pmix_common.h"

#include "src/mca/gds/base/base.h"
#include "gds_shmem_utils.h"
#include "gds_shmem.h"

#include "src/include/pmix_globals.h"
#include "src/class/pmix_list.h"
#include "src/client/pmix_client_ops.h"
#include "src/server/pmix_server_ops.h"
#include "src/mca/pcompress/base/base.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_error.h"
#include "src/util/pmix_output.h"
#include "src/util/pmix_name_fns.h"
#include "src/util/pmix_environ.h"
#include "src/util/pmix_shmem.h"

#ifdef HAVE_STRING_H
#include <string.h>
#endif
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif
#ifdef HAVE_SYS_STAT_H
#include <sys/stat.h>
#endif
#ifdef HAVE_FCNTL_H
#include <fcntl.h>
#endif
#ifdef HAVE_TIME_H
#include <time.h>
#endif

#define PMIX_GDS_SHMEM_VOUT_HERE()                                             \
do {                                                                           \
    pmix_output_verbose(2, pmix_gds_base_framework.framework_output,           \
                        "gds:" PMIX_GDS_SHMEM_NAME                             \
                        ":%s at line %d", __func__, __LINE__);                 \
} while (0)

#define PMIX_GDS_SHMEM_VOUT(...)                                               \
do {                                                                           \
    pmix_output_verbose(2, pmix_gds_base_framework.framework_output,           \
                        "gds:" PMIX_GDS_SHMEM_NAME ":" __VA_ARGS__);           \
} while (0)

/**
 * 8-byte address alignment.
 */
#define PMIX_GDS_SHMEM_ADDR_ALIGN_8(p, s)                                      \
    (uint8_t *)(p) + (((s) + 8UL - 1) & ~(8UL - 1))

/*
 * TODO(skg) Look at PMIX_BFROPS_PACK code.
 * TODO(skg) Look at PMIX_DATA_ARRAY_CONSTRUCT
 * TODO(skg) Look at mca/bfrops/base/bfrop_base_fns.c
 * TODO(skg) Look at pmix_gds_hash_fetch()
 */

/*
 * TODO(skg) Think about reallocs. Pointer arrays may change size.
 * TODO(skg) We don't know how big this will thing will be.
 * * Can do a two-step process; in memory; get size; copy into sm.
 * * On the client side need to write locally. Server as well.
 * * Try to avoid thread locks, too expensive at scale.
 * * Work on getting data stored. Cache will go into hash. Use register job
 *   info.
 */

// TODO(skg) This shouldn't live here. Figure out a way to get this in an
// appropriate place. This modifies PMIX_PROC_CREATE() for our needs.
#define PMIX_GDS_SHMEM_PROC_CREATE(m, n, tma)                                  \
do {                                                                           \
    (m) = (pmix_proc_t *)(tma)->calloc((tma), (n), sizeof(pmix_proc_t));       \
} while (0)

// TODO(skg) This shouldn't live here. Figure out a way to get this in an
// appropriate place. This modifies PMIX_VALUE_XFER() for our needs.
#define PMIX_GDS_SHMEM_VALUE_XFER(r, v, s, tma)                                \
do {                                                                           \
    if (NULL == (v)) {                                                         \
        (v) = (pmix_value_t *)(tma)->malloc((tma), sizeof(pmix_value_t));      \
        if (NULL == (v)) {                                                     \
            (r) = PMIX_ERR_NOMEM;                                              \
        } else {                                                               \
            (r) = pmix_gds_shmem_value_xfer((v), (s), (tma));                  \
        }                                                                      \
    } else {                                                                   \
        (r) = pmix_gds_shmem_value_xfer((v), (s), (tma));                      \
    }                                                                          \
} while(0)

// TODO(skg) This shouldn't live here. Figure out a way to get this in an
// appropriate place. This modifies PMIX_INFO_CREATE() for our needs.
#define PMIX_GDS_SHMEM_INFO_CREATE(m, n, tma)                                  \
do {                                                                           \
    pmix_info_t *i;                                                            \
    (m) = (pmix_info_t *)(tma)->calloc((tma), (n), sizeof(pmix_info_t));       \
    if (NULL != (m)) {                                                         \
        i = (pmix_info_t *)(m);                                                \
        i[(n) - 1].flags = PMIX_INFO_ARRAY_END;                                \
    }                                                                          \
} while (0)

// TODO(skg) This shouldn't live here. Figure out a way to get this in an
// appropriate place. This modifies PMIX_INFO_XFER() for our needs.
#define PMIX_GDS_SHMEM_INFO_XFER(d, s, tma)                                    \
    (void)pmix_gds_shmem_info_xfer(d, s, tma)

// TODO(skg) This will eventually go away.
static pmix_status_t
pmix_gds_shmem_copy_darray(
    pmix_data_array_t **dest,
    pmix_data_array_t *src,
    pmix_data_type_t type,
    pmix_tma_t *tma
);

// TODO(skg) This shouldn't live here. Figure out a way to get this in an
// appropriate place. This modifies pmix_bfrops_base_value_xfer() for our needs.
static pmix_status_t
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
                p->data.string = tma->strdup(tma, src->data.string);
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
            PMIX_GDS_SHMEM_PROC_CREATE(p->data.proc, 1, tma);
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
                p->data.bo.bytes = tma->malloc(tma, src->data.bo.size);
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
                p->data.envar.envar = tma->strdup(tma, src->data.envar.envar);
            }
            if (NULL != src->data.envar.value) {
                p->data.envar.value = tma->strdup(tma, src->data.envar.value);
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
static pmix_status_t
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
static pmix_status_t
pmix_gds_shmem_copy_darray(
    pmix_data_array_t **dest,
    pmix_data_array_t *src,
    pmix_data_type_t type,
    pmix_tma_t *tma
) {
    pmix_status_t rc = PMIX_SUCCESS;
    pmix_data_array_t *p = NULL;

    PMIX_GDS_SHMEM_UNUSED(type);

    p = (pmix_data_array_t *)tma->calloc(tma, 1, sizeof(pmix_data_array_t));
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
            p->array = (char *)tma->malloc(tma, src->size);
            if (NULL == p->array) {
                rc = PMIX_ERR_NOMEM;
                goto out;
            }
            memcpy(p->array, src->array, src->size);
            break;
        case PMIX_UINT16:
        case PMIX_INT16:
            p->array = (char *)tma->malloc(tma, src->size * sizeof(uint16_t));
            if (NULL == p->array) {
                rc = PMIX_ERR_NOMEM;
                goto out;
            }
            memcpy(p->array, src->array, src->size * sizeof(uint16_t));
            break;
        case PMIX_UINT32:
        case PMIX_INT32:
            p->array = (char *)tma->malloc(tma, src->size * sizeof(uint32_t));
            if (NULL == p->array) {
                rc = PMIX_ERR_NOMEM;
                goto out;
            }
            memcpy(p->array, src->array, src->size * sizeof(uint32_t));
            break;
        case PMIX_UINT64:
        case PMIX_INT64:
            p->array = (char *)tma->malloc(tma, src->size * sizeof(uint64_t));
            if (NULL == p->array) {
                rc = PMIX_ERR_NOMEM;
                goto out;
            }
            memcpy(p->array, src->array, src->size * sizeof(uint64_t));
            break;
        case PMIX_BOOL:
            p->array = (char *)tma->malloc(tma, src->size * sizeof(bool));
            if (NULL == p->array) {
                rc = PMIX_ERR_NOMEM;
                goto out;
            }
            memcpy(p->array, src->array, src->size * sizeof(bool));
            break;
        case PMIX_SIZE:
            p->array = (char *)tma->malloc(tma, src->size * sizeof(size_t));
            if (NULL == p->array) {
                rc = PMIX_ERR_NOMEM;
                goto out;
            }
            memcpy(p->array, src->array, src->size * sizeof(size_t));
            break;
        case PMIX_PID:
            p->array = (char *)tma->malloc(tma, src->size * sizeof(pid_t));
            if (NULL == p->array) {
                rc = PMIX_ERR_NOMEM;
                goto out;
            }
            memcpy(p->array, src->array, src->size * sizeof(pid_t));
            break;
        case PMIX_STRING: {
            char **prarray, **strarray;
            p->array = (char **)tma->malloc(tma, src->size * sizeof(char *));
            if (NULL == p->array) {
                rc = PMIX_ERR_NOMEM;
                goto out;
            }
            prarray = (char **)p->array;
            strarray = (char **)src->array;
            for (size_t n = 0; n < src->size; n++) {
                if (NULL != strarray[n]) {
                    prarray[n] = tma->strdup(tma, strarray[n]);
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
            p->array = (char *)tma->malloc(tma, src->size * sizeof(int));
            if (NULL == p->array) {
                rc = PMIX_ERR_NOMEM;
                goto out;
            }
            memcpy(p->array, src->array, src->size * sizeof(int));
            break;
        case PMIX_FLOAT:
            p->array = (char *)tma->malloc(tma, src->size * sizeof(float));
            if (NULL == p->array) {
                rc = PMIX_ERR_NOMEM;
                goto out;
            }
            memcpy(p->array, src->array, src->size * sizeof(float));
            break;
        case PMIX_DOUBLE:
            p->array = (char *)tma->malloc(tma, src->size * sizeof(double));
            if (NULL == p->array) {
                rc = PMIX_ERR_NOMEM;
                goto out;
            }
            memcpy(p->array, src->array, src->size * sizeof(double));
            break;
        case PMIX_TIMEVAL:
            p->array = (struct timeval *)tma->malloc(
                tma, src->size * sizeof(struct timeval)
            );
            if (NULL == p->array) {
                rc = PMIX_ERR_NOMEM;
                goto out;
            }
            memcpy(p->array, src->array, src->size * sizeof(struct timeval));
            break;
        case PMIX_TIME:
            p->array = (time_t *)tma->malloc(tma, src->size * sizeof(time_t));
            if (NULL == p->array) {
                rc = PMIX_ERR_NOMEM;
                goto out;
            }
            memcpy(p->array, src->array, src->size * sizeof(time_t));
            break;
        case PMIX_STATUS:
            p->array = (pmix_status_t *)tma->malloc(
                tma, src->size * sizeof(pmix_status_t)
            );
            if (NULL == p->array) {
                rc = PMIX_ERR_NOMEM;
                goto out;
            }
            memcpy(p->array, src->array, src->size * sizeof(pmix_status_t));
            break;
        case PMIX_PROC:
            PMIX_GDS_SHMEM_PROC_CREATE(p->array, src->size, tma);
            if (NULL == p->array) {
                rc = PMIX_ERR_NOMEM;
                goto out;
            }
            memcpy(p->array, src->array, src->size * sizeof(pmix_proc_t));
            break;
        case PMIX_PROC_RANK:
            p->array = (pmix_rank_t *)tma->malloc(
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
            PMIX_GDS_SHMEM_INFO_CREATE(p->array, src->size, tma);
            if (NULL == p->array) {
                rc = PMIX_ERR_NOMEM;
                goto out;
            }
            p1 = (pmix_info_t *)p->array;
            s1 = (pmix_info_t *)src->array;
            for (size_t n = 0; n < src->size; n++) {
                PMIX_GDS_SHMEM_INFO_XFER(&p1[n], &s1[n], tma);
            }
            break;
        }
        case PMIX_BYTE_OBJECT:
        case PMIX_COMPRESSED_STRING: {
            pmix_byte_object_t *pbo, *sbo;
            p->array = (pmix_byte_object_t *)tma->malloc(
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
                    pbo[n].bytes = (char *)tma->malloc(tma, pbo[n].size);
                    memcpy(pbo[n].bytes, sbo[n].bytes, pbo[n].size);
                } else {
                    pbo[n].bytes = NULL;
                    pbo[n].size = 0;
                }
            }
            break;
        }
        case PMIX_PERSIST:
            p->array = (pmix_persistence_t *)tma->malloc(
                tma, src->size * sizeof(pmix_persistence_t)
            );
            if (NULL == p->array) {
                rc = PMIX_ERR_NOMEM;
                goto out;
            }
            memcpy(p->array, src->array, src->size * sizeof(pmix_persistence_t));
            break;
        case PMIX_SCOPE:
            p->array = (pmix_scope_t *)tma->malloc(
                tma, src->size * sizeof(pmix_scope_t)
            );
            if (NULL == p->array) {
                rc = PMIX_ERR_NOMEM;
                goto out;
            }
            memcpy(p->array, src->array, src->size * sizeof(pmix_scope_t));
            break;
        case PMIX_DATA_RANGE:
            p->array = (pmix_data_range_t *)tma->malloc(
                tma, src->size * sizeof(pmix_data_range_t)
            );
            if (NULL == p->array) {
                rc = PMIX_ERR_NOMEM;
                goto out;
            }
            memcpy(p->array, src->array, src->size * sizeof(pmix_data_range_t));
            break;
        case PMIX_COMMAND:
            p->array = (pmix_cmd_t *)tma->malloc(
                tma, src->size * sizeof(pmix_cmd_t)
            );
            if (NULL == p->array) {
                rc = PMIX_ERR_NOMEM;
                goto out;
            }
            memcpy(p->array, src->array, src->size * sizeof(pmix_cmd_t));
            break;
        case PMIX_INFO_DIRECTIVES:
            p->array = (pmix_info_directives_t *)tma->malloc(
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
        // TODO(skg) We could add a tma->free() for this situation. Then it will
        // be up to the tma backend what happens during its free().
        //free(p);
        p = NULL;
    }
    *dest = p;
    return rc;
}

/**
 * Checks if an info's string value exceeds the length limit and therefore
 * requires compression.
 *
 * Note that we cannot use PMIX_STRING_SIZE_CHECK because we need to inspect
 * pmix_info_t structures, not pmix_kval_ts.
 */
static inline bool
string_value_requires_compression(
    const pmix_value_t *value
) {
    return PMIX_STRING == value->type &&
           NULL != value->data.string &&
           strlen(value->data.string) > pmix_compress_base.compress_limit;
}

/**
 * Returns page size.
 */
static inline size_t
get_page_size(void)
{
    const long i = sysconf(_SC_PAGE_SIZE);
    if (-1 == i) {
        PMIX_ERROR_LOG(PMIX_ERROR);
        return 0;
    }
    return i;
}

/**
 * Returns amount needed to pad to page boundary.
 */
static inline size_t
pad_amount_to_page(
    size_t size
) {
    const size_t page_size = get_page_size();
    return ((~size) + page_size + 1) & (page_size - 1);
}

// TODO(skg) Add memory arena boundary checks.
static inline void *
tma_malloc(
    pmix_tma_t *tma,
    size_t size
) {
    void *current = tma->arena;
    memset(current, 0, size);
    tma->arena = PMIX_GDS_SHMEM_ADDR_ALIGN_8(tma->arena, size);
    return current;
}

// TODO(skg) Add memory arena boundary checks.
static inline void *
tma_calloc(
    struct pmix_tma *tma,
    size_t nmemb,
    size_t size
) {
    const size_t real_size = nmemb * size;
    void *current = tma->arena;
    memset(current, 0, real_size);
    tma->arena = PMIX_GDS_SHMEM_ADDR_ALIGN_8(tma->arena, real_size);
    return current;
}

// TODO(skg) Add memory arena boundary checks.
static inline char *
tma_strdup(
    pmix_tma_t *tma,
    const char *s
) {
    void *current = tma->arena;
    const size_t size = strlen(s) + 1;
    tma->arena = PMIX_GDS_SHMEM_ADDR_ALIGN_8(tma->arena, size);
    return (char *)memmove(current, s, size);
}

// TODO(skg) Add memory arena boundary checks.
static inline void *
tma_memmove(
    struct pmix_tma *tma,
    const void *src,
    size_t size
) {
    void *current = tma->arena;
    tma->arena = PMIX_GDS_SHMEM_ADDR_ALIGN_8(tma->arena, size);
    return memmove(current, src, size);
}

static void
tma_construct(
    pmix_tma_t *tma
) {
    tma->malloc = tma_malloc;
    tma->calloc = tma_calloc;
    // We don't currently support realloc.
    tma->realloc = NULL;
    tma->strdup = tma_strdup;
    tma->memmove = tma_memmove;
    tma->arena = NULL;
    tma->dontfree = 1;
}

static void
tma_destruct(
    pmix_tma_t *tma
) {
    tma->malloc = NULL;
    tma->calloc = NULL;
    tma->realloc = NULL;
    tma->strdup = NULL;
    tma->memmove = NULL;
    tma->arena = NULL;
}

static void
session_construct(
    pmix_gds_shmem_session_t *s
) {
    s->id = UINT32_MAX;
    PMIX_CONSTRUCT(&s->sessioninfo, pmix_list_t);
    PMIX_CONSTRUCT(&s->nodeinfo, pmix_list_t);
}

static void
session_destruct(
    pmix_gds_shmem_session_t *s
) {
    PMIX_LIST_DESTRUCT(&s->sessioninfo);
    PMIX_LIST_DESTRUCT(&s->nodeinfo);
}

PMIX_CLASS_INSTANCE(
    pmix_gds_shmem_session_t,
    pmix_list_item_t,
    session_construct,
    session_destruct
);

static void
job_construct(
    pmix_gds_shmem_job_t *j
) {
    j->nspace_id = NULL;
    j->nspace = NULL;
    j->shmem = PMIX_NEW(pmix_shmem_t);
    tma_construct(&j->tma);
    j->session = NULL;
    pmix_hash_table_init(&j->hashtab_internal, 256);
}

static void
job_destruct(
    pmix_gds_shmem_job_t *j
) {
    free(j->nspace_id);
    PMIX_RELEASE(j->nspace);
    PMIX_RELEASE(j->shmem);
    tma_destruct(&j->tma);
    PMIX_RELEASE(j->session);
    pmix_hash_remove_data(
        &j->hashtab_internal, PMIX_RANK_WILDCARD, NULL
    );
    PMIX_DESTRUCT(&j->hashtab_internal);
}

PMIX_CLASS_INSTANCE(
    pmix_gds_shmem_job_t,
    pmix_list_item_t,
    job_construct,
    job_destruct
);

static pmix_status_t
init(
    pmix_info_t info[],
    size_t ninfo
) {

    PMIX_GDS_SHMEM_UNUSED(info);
    PMIX_GDS_SHMEM_UNUSED(ninfo);

    PMIX_GDS_SHMEM_VOUT_HERE();
    return PMIX_SUCCESS;
}

static void
finalize(void)
{
    PMIX_GDS_SHMEM_VOUT_HERE();
}

/**
 * Note: only clients enter here.
 */
static pmix_status_t
client_assign_module(
    pmix_info_t *info,
    size_t ninfo,
    int *priority
) {
    static const int max_priority = 100;
    *priority = PMIX_GDS_SHMEM_DEFAULT_PRIORITY;
    // The incoming info always overrides anything in the
    // environment as it is set by the application itself.
    bool specified = false;
    for (size_t n = 0; n < ninfo; n++) {
        if (PMIX_CHECK_KEY(&info[n], PMIX_GDS_MODULE)) {
            char **options = NULL;
            specified = true; // They specified who they want.
            options = pmix_argv_split(info[n].value.data.string, ',');
            for (size_t m = 0; NULL != options[m]; m++) {
                if (0 == strcmp(options[m], PMIX_GDS_SHMEM_NAME)) {
                    // They specifically asked for us.
                    *priority = max_priority;
                    break;
                }
            }
            pmix_argv_free(options);
            break;
        }
    }
#if (PMIX_GDS_SHMEM_DISABLE == 1)
    if (true) {
        *priority = 0;
        return PMIX_SUCCESS;
    }
#endif
    // If they don't want us, then disqualify ourselves.
    if (specified && *priority != max_priority) {
        *priority = 0;
        return PMIX_SUCCESS;
    }
    // Else we are a candidate for use.
    // Look for the shared-memory segment connection environment variables: If
    // found, then we can connect to the segment. Otherwise, we have to reject.
    const char *path = getenv(PMIX_GDS_SHMEM_ENVVAR_SEG_PATH);
    const char *size = getenv(PMIX_GDS_SHMEM_ENVVAR_SEG_SIZE);
    const char *addr = getenv(PMIX_GDS_SHMEM_ENVVAR_SEG_ADDR);
    if (!path || !size || !addr) {
        *priority = 0;
        return PMIX_SUCCESS;
    }
    PMIX_GDS_SHMEM_VOUT("%s found path=%s", __func__, path);
    PMIX_GDS_SHMEM_VOUT("%s found size=0x%s B", __func__, size);
    PMIX_GDS_SHMEM_VOUT("%s found addr=0x%s", __func__, addr);
    return PMIX_SUCCESS;
}

/**
 * @note: only servers enter here.
 * At this moment we aren't called since hash has a higher priority.
 */
static pmix_status_t
server_cache_job_info(
    struct pmix_namespace_t *ns,
    pmix_info_t info[],
    size_t ninfo
) {
    PMIX_GDS_SHMEM_UNUSED(ns);
    PMIX_GDS_SHMEM_UNUSED(info);
    PMIX_GDS_SHMEM_UNUSED(ninfo);

    PMIX_GDS_SHMEM_VOUT_HERE();
    return PMIX_SUCCESS;
}

static pmix_status_t
server_register_job_info(
    struct pmix_peer_t *pr,
    pmix_buffer_t *reply
) {
    // TODO(skg) Look at pmix_gds_hash_fetch(). It looks like this is where we
    // would interface with the requester.
    // TODO(skg) GDS_GET pass own peer to get info; cache in sm.
    // TODO(skg) Look at pmix_hash_fetch(ht, PMIX_RANK_WILDCARD, NULL, &val);
    PMIX_GDS_SHMEM_UNUSED(pr);
    PMIX_GDS_SHMEM_UNUSED(reply);

    PMIX_GDS_SHMEM_VOUT_HERE();
    /* Since the data is already in the shmem when we
     * cached it, there is nothing to do here. */
    return PMIX_SUCCESS;
}

/**
 * String to size_t.
 */
static pmix_status_t
strtost(
    const char *str,
    int base,
    size_t *maybe_val
) {
    *maybe_val = 0;

    errno = 0;
    char *end = NULL;
    long long val = strtoll(str, &end, base);
    int err = errno;
    // In valid range?
    if (err == ERANGE && val == LLONG_MAX) {
        return PMIX_ERROR;
    }
    if (err == ERANGE && val == LLONG_MIN) {
        return PMIX_ERROR;
    }
    if (*end != '\0') {
        return PMIX_ERROR;
    }
    *maybe_val = (size_t)val;
    return PMIX_SUCCESS;
}

/**
 * Sets shared-memory connection information from environment variables set by
 * server at setup_fork().
 */
static pmix_status_t
set_shmem_connection_info_from_env(
    pmix_shmem_t *shmem
) {
    pmix_status_t rc = PMIX_SUCCESS;
    size_t base_addr = 0, nw = 0;
    // Get the shared-memory segment connection environment variables.
    const char *pathstr = getenv(PMIX_GDS_SHMEM_ENVVAR_SEG_PATH);
    const char *sizestr = getenv(PMIX_GDS_SHMEM_ENVVAR_SEG_SIZE);
    const char *addrstr = getenv(PMIX_GDS_SHMEM_ENVVAR_SEG_ADDR);
    // These should be set for us, but check for sanity.
    if (!pathstr || !sizestr || !addrstr) {
        rc = PMIX_ERROR;
        goto out;
    }
    // Set job segment path.
    nw = snprintf(
        shmem->backing_path, PMIX_PATH_MAX, "%s", pathstr
    );
    if (nw >= PMIX_PATH_MAX) {
        rc = PMIX_ERROR;
        goto out;
    }
    // Convert string base address to something we can use.
    rc = strtost(addrstr, 16, &base_addr);
    if (PMIX_SUCCESS != rc) {
        goto out;
    }
    // Set job segment base address.
    shmem->base_address = (void *)base_addr;
    // Set job shared-memory segment size.
    rc = strtost(sizestr, 16, &shmem->size);
    if (PMIX_SUCCESS != rc) {
        goto out;
    }

    PMIX_GDS_SHMEM_VOUT(
        "%s: using segment backing file path %s (%zd B) at 0x%zx",
        __func__, shmem->backing_path, shmem->size, (size_t)base_addr
    );
out:
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
    }
    return rc;
}

static pmix_status_t
store_job_info(
    const char *nspace,
    pmix_buffer_t *buf
) {
    PMIX_GDS_SHMEM_VOUT_HERE();

    PMIX_GDS_SHMEM_UNUSED(buf);

    pmix_status_t rc = PMIX_SUCCESS;
    uintptr_t mmap_addr = 0;
    // Create a job tracker for the given namespace.
    pmix_gds_shmem_job_t *job = NULL;
    rc = pmix_gds_shmem_get_job_tracker(nspace, true, &job);
    if (PMIX_SUCCESS != rc) {
        goto out;
    }
    // Set shared-memory segment information for this job.
    rc = set_shmem_connection_info_from_env(job->shmem);
    if (PMIX_SUCCESS != rc) {
        goto out;
    }
    // Attach to the given shared-memory segment.
    rc = pmix_shmem_segment_attach(
        job->shmem,
        // The base address was populated above.
        job->shmem->base_address,
        &mmap_addr
    );
    if (PMIX_SUCCESS != rc) {
        goto out;
    }
    // Make sure that we mapped to the requested address.
    if (mmap_addr != (uintptr_t)job->shmem->base_address) {
        rc = PMIX_ERROR;
    }
    // Done. Before this point the server should have populated the
    // shared-memory segment with the relevant data.
out:
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
    }
    return rc;
}

static pmix_status_t
store(
    const pmix_proc_t *proc,
    pmix_scope_t scope,
    pmix_kval_t *kv
) {
    PMIX_GDS_SHMEM_UNUSED(proc);
    PMIX_GDS_SHMEM_UNUSED(scope);
    PMIX_GDS_SHMEM_UNUSED(kv);

    PMIX_GDS_SHMEM_VOUT_HERE();
    return PMIX_SUCCESS;
}

static pmix_status_t
store_modex(
    struct pmix_namespace_t *ns,
    pmix_buffer_t *buff,
    void *cbdata
) {
    PMIX_GDS_SHMEM_UNUSED(ns);
    PMIX_GDS_SHMEM_UNUSED(buff);
    PMIX_GDS_SHMEM_UNUSED(cbdata);

    PMIX_GDS_SHMEM_VOUT_HERE();
    return PMIX_SUCCESS;
}

static pmix_status_t
fetch(
    const pmix_proc_t *proc,
    pmix_scope_t scope,
    bool copy,
    const char *key,
    pmix_info_t qualifiers[],
    size_t nqual,
    pmix_list_t *kvs
) {
    PMIX_GDS_SHMEM_UNUSED(proc);
    PMIX_GDS_SHMEM_UNUSED(scope);
    PMIX_GDS_SHMEM_UNUSED(copy);
    PMIX_GDS_SHMEM_UNUSED(key);
    PMIX_GDS_SHMEM_UNUSED(qualifiers);
    PMIX_GDS_SHMEM_UNUSED(nqual);
    PMIX_GDS_SHMEM_UNUSED(kvs);

    PMIX_GDS_SHMEM_VOUT_HERE();
    return PMIX_SUCCESS;
}

static pmix_status_t
server_setup_fork(
    const pmix_proc_t *peer,
    char ***env
) {
    // Server stashes segment connection
    // info in the environment of its children.
    pmix_status_t rc = PMIX_SUCCESS;
    char addrbuff[64] = {'\0'}, sizebuff[64] = {'\0'};
    size_t nw = 0;
    PMIX_GDS_SHMEM_VOUT(
        "%s: peer (rank=%d) namespace=%s",
        __func__, peer->rank, peer->nspace
    );
    // Get the tracker for this job. We should have already created one, so
    // that's why we pass false in pmix_gds_shmem_get_job_tracker().
    pmix_gds_shmem_job_t *job = NULL;
    rc = pmix_gds_shmem_get_job_tracker(peer->nspace, false, &job);
    if (PMIX_SUCCESS != rc) {
        goto out;
    }
    // Set backing file path.
    rc = pmix_setenv(
        PMIX_GDS_SHMEM_ENVVAR_SEG_PATH,
        job->shmem->backing_path, true, env
    );
    if (PMIX_SUCCESS != rc) {
        goto out;
    }
    // Set attach size to shared-memory segment.
    nw = snprintf(
        sizebuff, sizeof(sizebuff),
        "%zx", job->shmem->size
    );
    if (nw >= sizeof(sizebuff)) {
        rc = PMIX_ERROR;
        goto out;
    }
    rc = pmix_setenv(
        PMIX_GDS_SHMEM_ENVVAR_SEG_SIZE,
        sizebuff, true, env
    );
    if (PMIX_SUCCESS != rc) {
        goto out;
    }
    // Set base address for attaching to shared-memory segment.
    nw = snprintf(
        addrbuff, sizeof(addrbuff),
        "%zx", (size_t)job->shmem->base_address
    );
    if (nw >= sizeof(addrbuff)) {
        rc = PMIX_ERROR;
        goto out;
    }

    rc = pmix_setenv(
        PMIX_GDS_SHMEM_ENVVAR_SEG_ADDR,
        addrbuff, true, env
    );
out:
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
    }
    return rc;
}

/**
 * Error function for catching unhandled data types.
 */
static inline pmix_status_t
unhandled_type(
    pmix_data_type_t val_type
) {
    static const pmix_status_t rc = PMIX_ERR_FATAL;
    PMIX_GDS_SHMEM_VOUT(
        "%s: %s", __func__, PMIx_Data_type_string(val_type)
    );
    PMIX_ERROR_LOG(rc);
    return rc;
}

/**
 * Returns the size required to store the given type.
 */
static inline pmix_status_t
get_value_size(
    const pmix_value_t *value,
    size_t *result
) {
    pmix_status_t rc = PMIX_SUCCESS;
    size_t total = 0;

    const pmix_data_type_t type = value->type;
    // NOTE(skg) This follows the size conventions set in include/pmix_common.h.
    switch (type) {
        case PMIX_ALLOC_DIRECTIVE:
        case PMIX_BYTE:
        case PMIX_DATA_RANGE:
        case PMIX_INT8:
        case PMIX_PERSIST:
        case PMIX_POINTER:
        case PMIX_PROC_STATE:
        case PMIX_SCOPE:
        case PMIX_UINT8:
            total += sizeof(int8_t);
            break;
        case PMIX_BOOL:
            total += sizeof(bool);
            break;
        // TODO(skg) This needs more work. For example, we can potentially miss
        // the storage requirements of scaffolding around things like
        // PMIX_STRINGs, for example. See pmix_gds_shmem_copy_darray().
        case PMIX_DATA_ARRAY: {
            total += sizeof(pmix_data_array_t);
            const size_t n = value->data.darray->size;
            PMIX_GDS_SHMEM_VOUT(
                "%s: %s of type=%s has size=%zd",
                __func__, PMIx_Data_type_string(type),
                PMIx_Data_type_string(value->data.darray->type), n
            );
            // We don't construct or destruct tmp_value because we are only
            // interested in inspecting its values for the purposes of
            // estimating its size using this function.
            pmix_value_t tmp_value = {
                .type = value->data.darray->type
            };
            size_t sizeofn = 0;
            rc = get_value_size(&tmp_value, &sizeofn);
            if (PMIX_SUCCESS != rc) {
                return rc;
            }
            total += n * sizeofn;
            break;
        }
        case PMIX_DOUBLE:
            total += sizeof(double);
            break;
        case PMIX_ENVAR: {
            const pmix_envar_t *val = &value->data.envar;
            total += sizeof(*val);
            total += strlen(val->envar) + 1;
            total += strlen(val->value) + 1;
            break;
        }
        case PMIX_FLOAT:
            total += sizeof(float);
            break;
        case PMIX_INFO:
            total += sizeof(pmix_info_t);
            break;
        case PMIX_INT:
        case PMIX_UINT:
        case PMIX_STATUS:
            total += sizeof(int);
            break;
        case PMIX_INT16:
            total += sizeof(int16_t);
            break;
        case PMIX_INT32:
            total += sizeof(int32_t);
            break;
        case PMIX_INT64:
            total += sizeof(int64_t);
            break;
        case PMIX_PID:
            total += sizeof(pid_t);
            break;
        case PMIX_PROC:
            total += sizeof(pmix_proc_t);
            break;
        case PMIX_PROC_RANK:
            total += sizeof(pmix_rank_t);
            break;
        case PMIX_REGEX:
            total += sizeof(pmix_byte_object_t);
            total += value->data.bo.size;
            break;
        case PMIX_STRING:
            total += strlen(value->data.string) + 1;
            break;
        case PMIX_SIZE:
            total += sizeof(size_t);
            break;
        case PMIX_UINT16:
            total += sizeof(uint16_t);
            break;
        case PMIX_UINT32:
            total += sizeof(uint32_t);
            break;
        case PMIX_UINT64:
            total += sizeof(uint64_t);
            break;
        case PMIX_APP:
        case PMIX_BUFFER:
        case PMIX_BYTE_OBJECT:
        case PMIX_COMMAND:
        case PMIX_COMPRESSED_STRING:
        case PMIX_COORD:
        case PMIX_DATA_BUFFER:
        case PMIX_DATA_TYPE:
        case PMIX_DEVICE_DIST:
        case PMIX_DEVTYPE:
        case PMIX_DISK_STATS:
        case PMIX_ENDPOINT:
        case PMIX_GEOMETRY:
        case PMIX_INFO_DIRECTIVES:
        case PMIX_IOF_CHANNEL:
        case PMIX_JOB_STATE:
        case PMIX_KVAL:
        case PMIX_LINK_STATE:
        case PMIX_NET_STATS:
        case PMIX_NODE_STATS:
        case PMIX_PDATA:
        case PMIX_PROC_CPUSET:
        case PMIX_PROC_INFO:
        case PMIX_PROC_NSPACE:
        case PMIX_PROC_STATS:
        case PMIX_QUERY:
        case PMIX_REGATTR:
        case PMIX_TIME:
        case PMIX_TIMEVAL:
        case PMIX_TOPO:
        case PMIX_VALUE:
        default:
            return unhandled_type(type);
    }
    *result = total;
    return rc;
}

/**
 * Calculates a rough (upper bound) estimate on the amount of storage required
 * to store the values associated with this namespace.
 */
static pmix_status_t
get_estimated_segment_size(
    const char *nspace,
    uint32_t nlocalprocs,
    pmix_info_t info[],
    size_t ninfo,
    size_t *segment_size
) {
    pmix_status_t rc = PMIX_SUCCESS;
    size_t total = 0, size = 0;
    // Each pmix_info_t has all the associated data structures
    // that we must ultimately store, so account for those.
    total += ninfo * sizeof(pmix_kval_t);
    for (size_t i = 0; i < ninfo; ++i) {
        const pmix_value_t *value = &info[i].value;
        rc = get_value_size(value, &size);
        if (PMIX_SUCCESS != rc) {
            return rc;
        }
        total += size;
    }
    // Add a little room in case we aren't exact, pad to page boundary.
    // XXX(skg) We can probably make this tighter.
    total = total * 1.50;
    total += pad_amount_to_page(total);
    PMIX_GDS_SHMEM_VOUT(
        "%s: segment size=%zd B for namespace=%s nlocalprocs=%d",
        __func__, total, nspace, nlocalprocs
    );
    *segment_size = total;
    return PMIX_SUCCESS;
}

/**
 * Returns a valid path or NULL on error.
 */
static const char *
get_shmem_backing_path(
    pmix_info_t info[],
    size_t ninfo
) {
    static char path[PMIX_PATH_MAX] = {0};
    static const char *basedir = NULL;

    for (size_t i = 0; i < ninfo; ++i) {
        if (PMIX_CHECK_KEY(&info[i], PMIX_NSDIR)) {
            basedir = info[i].value.data.string;
            break;
        }
        if (PMIX_CHECK_KEY(&info[i], PMIX_TMPDIR)) {
            basedir = info[i].value.data.string;
            break;
        }
    }
    if (!basedir) {
        if (NULL == (basedir = getenv("TMPDIR"))) {
            basedir = "/tmp";
        }
    }
    // Now that we have the base dir, append file name.
    size_t nw = snprintf(
        path, PMIX_PATH_MAX, "%s/gds-%s.%d",
        basedir, PMIX_GDS_SHMEM_NAME, getpid()
    );
    if (nw >= PMIX_PATH_MAX) {
        return NULL;
    }
    return path;
}

static pmix_status_t
segment_create_and_attach(
    pmix_info_t info[],
    size_t ninfo,
    pmix_gds_shmem_job_t *job,
    size_t size
) {
    pmix_status_t rc = PMIX_SUCCESS;
    const char *segment_path = NULL;
    uintptr_t mmap_addr = 0;
    // Find a hole in virtual memory that meets our size requirements.
    size_t base_addr = 0;
    rc = pmix_vmem_find_hole(
        VMEM_HOLE_BIGGEST, &base_addr, size
    );
    if (PMIX_SUCCESS != rc) {
        goto out;
    }
    PMIX_GDS_SHMEM_VOUT(
        "%s: found vmhole at address=0x%zx",
        __func__, base_addr
    );
    // Find a unique path for the shared-memory backing file.
    segment_path = get_shmem_backing_path(info, ninfo);
    if (!segment_path) {
        rc = PMIX_ERROR;
        goto out;
    }
    PMIX_GDS_SHMEM_VOUT(
        "%s: segment backing file path is %s (%zd B)",
        __func__, segment_path, size
    );
    // Create a shared-memory segment backing store at the given path.
    rc = pmix_shmem_segment_create(
        job->shmem, size, segment_path
    );
    if (PMIX_SUCCESS != rc) {
        goto out;
    }
    // Attach to the shared-memory segment with the given address.
    rc = pmix_shmem_segment_attach(
        job->shmem, (void *)base_addr, &mmap_addr
    );
    if (PMIX_SUCCESS != rc) {
        goto out;
    }
    // Make sure that we mapped to the requested address.
    if (mmap_addr != (uintptr_t)job->shmem->base_address) {
        rc = PMIX_ERROR;
        goto out;
    }
    PMIX_GDS_SHMEM_VOUT(
        "%s: mmapd at address=0x%zx", __func__, (size_t)mmap_addr
    );
    // Let our allocator know where its base address is.
    job->tma.arena = (void *)mmap_addr;
out:
    if (PMIX_SUCCESS != rc) {
        (void)pmix_shmem_segment_detach(job->shmem);
        PMIX_ERROR_LOG(rc);
    }
    return rc;
}

// NOTE(skg) This methodology comes from hash_cache_job_info().
static pmix_status_t
add_info_to_job(
    const pmix_info_t *info,
    pmix_gds_shmem_job_t *job
) {
    PMIX_GDS_SHMEM_VOUT(
        "%s: Storing k=%s, v=%s", __func__,
        info->key, PMIx_Data_type_string(info->value.type)
    );

    pmix_status_t rc = PMIX_SUCCESS;
    // Create and store a pmix_kval_t structure in shared memory.
    pmix_kval_t *kval = PMIX_NEW(pmix_kval_t, &job->tma);
    if (!kval) {
        rc = PMIX_ERR_NOMEM;
        goto out;
    }
    // Similarly, cache the provided key in shared-memory.
    kval->key = job->tma.strdup(&job->tma, info->key);
    if (!kval->key) {
        rc = PMIX_ERR_NOMEM;
        goto out;
    }
    // First check the special case where the string value requires compression.
    // We do this first because we want to avoid having to free the provided
    // value if compression is required, as we don't currently support free().
    //
    // TODO(skg) Best I can tell, this should probably happen automatically in
    // pmix_bfrops_base_value_xfer().
    if (string_value_requires_compression(&info->value)) {
        char *str = info->value.data.string;
        uint8_t *cdata = NULL;
        size_t cdata_size = 0;
        if (pmix_compress.compress_string(str, &cdata, &cdata_size)) {
            char *sm_cdata = NULL;
            if (!cdata) {
                rc = PMIX_ERR_NOMEM;
                goto out;
            }
            kval->value->type = PMIX_COMPRESSED_STRING;
            // We need to copy into our shared-memory segment.
            sm_cdata = job->tma.memmove(&job->tma, cdata, cdata_size);
            kval->value->data.bo.bytes = sm_cdata;
            kval->value->data.bo.size = cdata_size;
            // Now we can free the compressed data we copied.
            free(cdata);
        }
        // The compression didn't happen for some reason, so return an error.
        else {
            rc = PMIX_ERROR;
            goto out;
        }
    }
    else {
        // TODO(skg) We cannot use PMIX_VALUE_XFER directly because there are
        // memory allocation operations that are outside of our allocator's
        // purview.
        PMIX_GDS_SHMEM_VALUE_XFER(rc, kval->value, &info->value, &job->tma);
        if (PMIX_SUCCESS != rc) {
            goto out;
        }
    }
    // Store the kval into our internal hash table.
    rc = pmix_hash_store(&job->hashtab_internal, PMIX_RANK_WILDCARD, kval);
    if (PMIX_SUCCESS != rc) {
        goto out;
    }
    // Notice that we don't do a PMIX_RELEASE(kval) here, because we don't
    // currently support free(). TODO(skg) We should probably add tma->free() so
    // that the usage conventions are preserved.
out:
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        // TODO(skg) kvdes needs to know about the
        // tma or we avoid use of PMIX_RELEASE().
        //PMIX_RELEASE(kval);
    }
    return rc;
}

static pmix_status_t
server_add_nspace(
    const char *nspace,
    uint32_t nlocalprocs,
    pmix_info_t info[],
    size_t ninfo
) {
    PMIX_GDS_SHMEM_VOUT(
        "%s: adding namespace=%s with nlocalprocs=%d",
        __func__, nspace, nlocalprocs
    );

    pmix_status_t rc = PMIX_SUCCESS;
    pmix_gds_shmem_job_t *job = NULL;
    // Get the shared-memory segment size. Mas o menos.
    size_t segment_size = 0;
    rc = get_estimated_segment_size(
        nspace, nlocalprocs,
        info, ninfo, &segment_size
    );
    if (PMIX_SUCCESS != rc) {
        goto out;
    }
    // Create a job tracker.
    rc = pmix_gds_shmem_get_job_tracker(nspace, true, &job);
    if (PMIX_SUCCESS != rc) {
        goto out;
    }
    // Create and attach to the shared-memory segment and update job tracker
    // with the shared-memory backing store information.
    rc = segment_create_and_attach(
        info, ninfo, job, segment_size
    );
    if (PMIX_SUCCESS != rc) {
        goto out;
    }
    // Now add the information to the shared-memory
    // segment associated with this namespace.
    for (size_t i = 0; i < ninfo; ++i) {
        rc = add_info_to_job(&info[i], job);
        if (PMIX_SUCCESS != rc) {
            break;
        }
    }
out:
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
    }
    return rc;
}

static pmix_status_t
del_nspace(
    const char *nspace
) {
    PMIX_GDS_SHMEM_UNUSED(nspace);

    PMIX_GDS_SHMEM_VOUT_HERE();
    return PMIX_SUCCESS;
}

static pmix_status_t
assemb_kvs_req(
    const pmix_proc_t *proc,
    pmix_list_t *kvs,
    pmix_buffer_t *bo,
    void *cbdata
) {
    PMIX_GDS_SHMEM_UNUSED(proc);
    PMIX_GDS_SHMEM_UNUSED(kvs);
    PMIX_GDS_SHMEM_UNUSED(bo);
    PMIX_GDS_SHMEM_UNUSED(cbdata);

    PMIX_GDS_SHMEM_VOUT_HERE();
    return PMIX_SUCCESS;
}

static pmix_status_t
accept_kvs_resp(
    pmix_buffer_t *buf
) {
    PMIX_GDS_SHMEM_UNUSED(buf);

    PMIX_GDS_SHMEM_VOUT_HERE();
    return PMIX_SUCCESS;
}

pmix_gds_base_module_t pmix_shmem_module = {
    .name = PMIX_GDS_SHMEM_NAME,
    .is_tsafe = false,
    .init = init,
    .finalize = finalize,
    .assign_module = client_assign_module,
    .cache_job_info = server_cache_job_info,
    .register_job_info = server_register_job_info,
    .store_job_info = store_job_info,
    .store = store,
    .store_modex = store_modex,
    .fetch = fetch,
    .setup_fork = server_setup_fork,
    .add_nspace = server_add_nspace,
    .del_nspace = del_nspace,
    .assemb_kvs_req = assemb_kvs_req,
    .accept_kvs_resp = accept_kvs_resp
};

/*
 * vim: ft=cpp ts=4 sts=4 sw=4 expandtab
 */
