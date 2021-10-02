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
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2015      Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2021      Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "src/include/pmix_config.h"

#include "src/hwloc/pmix_hwloc.h"
#include "src/include/pmix_globals.h"
#include "src/mca/preg/preg.h"
#include "src/util/argv.h"
#include "src/util/error.h"
#include "src/util/output.h"

#include "src/mca/bfrops/base/base.h"

static void _populate_pstats(pmix_proc_stats_t *p, pmix_proc_stats_t *src);
static void _populate_dkstats(pmix_disk_stats_t *p, pmix_disk_stats_t *src);
static void _populate_netstats(pmix_net_stats_t *p, pmix_net_stats_t *src);
static void _populate_ndstats(pmix_node_stats_t *p, pmix_node_stats_t *src);

pmix_status_t pmix_bfrops_base_copy(pmix_pointer_array_t *regtypes, void **dest, void *src,
                                    pmix_data_type_t type)
{
    pmix_bfrop_type_info_t *info;

    /* check for error */
    if (NULL == dest || NULL == src) {
        PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
        return PMIX_ERR_BAD_PARAM;
    }

    /* Lookup the copy function for this type and call it */
    if (NULL == (info = (pmix_bfrop_type_info_t *) pmix_pointer_array_get_item(regtypes, type))) {
        PMIX_ERROR_LOG(PMIX_ERR_UNKNOWN_DATA_TYPE);
        return PMIX_ERR_UNKNOWN_DATA_TYPE;
    }

    return info->odti_copy_fn(dest, src, type);
}

pmix_status_t pmix_bfrops_base_copy_payload(pmix_buffer_t *dest, pmix_buffer_t *src)
{
    size_t to_copy = 0;
    char *ptr;

    /* deal with buffer type */
    if (NULL == dest->base_ptr) {
        /* destination buffer is empty - derive src buffer type */
        dest->type = src->type;
    } else if (dest->type != src->type) {
        /* buffer types mismatch */
        PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
        return PMIX_ERR_BAD_PARAM;
    }

    /* if the src buffer is empty, then there is
     * nothing to do */
    if (PMIX_BUFFER_IS_EMPTY(src)) {
        return PMIX_SUCCESS;
    }

    /* extend the dest if necessary */
    to_copy = src->pack_ptr - src->unpack_ptr;
    if (NULL == (ptr = pmix_bfrop_buffer_extend(dest, to_copy))) {
        PMIX_ERROR_LOG(PMIX_ERR_OUT_OF_RESOURCE);
        return PMIX_ERR_OUT_OF_RESOURCE;
    }
    memcpy(ptr, src->unpack_ptr, to_copy);
    dest->bytes_used += to_copy;
    dest->pack_ptr += to_copy;
    return PMIX_SUCCESS;
}

/*
 * STANDARD COPY FUNCTION - WORKS FOR EVERYTHING NON-STRUCTURED
 */
pmix_status_t pmix_bfrops_base_std_copy(void **dest, void *src, pmix_data_type_t type)
{
    size_t datasize;
    uint8_t *val = NULL;

    switch (type) {
    case PMIX_BOOL:
        datasize = sizeof(bool);
        break;

    case PMIX_INT:
    case PMIX_UINT:
        datasize = sizeof(int);
        break;

    case PMIX_SIZE:
        datasize = sizeof(size_t);
        break;

    case PMIX_PID:
        datasize = sizeof(pid_t);
        break;

    case PMIX_BYTE:
    case PMIX_INT8:
    case PMIX_UINT8:
    case PMIX_LINK_STATE:
        datasize = 1;
        break;

    case PMIX_INT16:
    case PMIX_UINT16:
    case PMIX_IOF_CHANNEL:
    case PMIX_LOCTYPE:
    case PMIX_STOR_ACCESS_TYPE:
        datasize = 2;
        break;

    case PMIX_INT32:
    case PMIX_UINT32:
        datasize = 4;
        break;

    case PMIX_INT64:
    case PMIX_UINT64:
    case PMIX_DEVTYPE:
    case PMIX_STOR_MEDIUM:
    case PMIX_STOR_ACCESS:
    case PMIX_STOR_PERSIST:
        datasize = 8;
        break;

    case PMIX_FLOAT:
        datasize = sizeof(float);
        break;

    case PMIX_TIMEVAL:
        datasize = sizeof(struct timeval);
        break;

    case PMIX_TIME:
        datasize = sizeof(time_t);
        break;

    case PMIX_STATUS:
        datasize = sizeof(pmix_status_t);
        break;

    case PMIX_PROC_RANK:
        datasize = sizeof(pmix_rank_t);
        break;

    case PMIX_PERSIST:
        datasize = sizeof(pmix_persistence_t);
        break;

    case PMIX_POINTER:
        datasize = sizeof(char *);
        break;

    case PMIX_SCOPE:
        datasize = sizeof(pmix_scope_t);
        break;

    case PMIX_DATA_RANGE:
        datasize = sizeof(pmix_data_range_t);
        break;

    case PMIX_COMMAND:
        datasize = sizeof(pmix_cmd_t);
        break;

    case PMIX_INFO_DIRECTIVES:
        datasize = sizeof(pmix_info_directives_t);
        break;

    case PMIX_PROC_STATE:
        datasize = sizeof(pmix_proc_state_t);
        break;

    case PMIX_ALLOC_DIRECTIVE:
        datasize = sizeof(pmix_alloc_directive_t);
        break;

    case PMIX_JOB_STATE:
        datasize = sizeof(pmix_job_state_t);
        break;

    default:
        return PMIX_ERR_UNKNOWN_DATA_TYPE;
    }

    val = (uint8_t *) malloc(datasize);
    if (NULL == val) {
        return PMIX_ERR_OUT_OF_RESOURCE;
    }

    memcpy(val, src, datasize);
    *dest = val;

    return PMIX_SUCCESS;
}

/* COPY FUNCTIONS FOR NON-STANDARD SYSTEM TYPES */

/*
 * STRING
 */
pmix_status_t pmix_bfrops_base_copy_string(char **dest, char *src, pmix_data_type_t type)
{
    PMIX_HIDE_UNUSED_PARAMS(type);

    if (NULL == src) { /* got zero-length string/NULL pointer - store NULL */
        *dest = NULL;
    } else {
        *dest = strdup(src);
    }

    return PMIX_SUCCESS;
}

/* PMIX_VALUE */
pmix_status_t pmix_bfrops_base_copy_value(pmix_value_t **dest, pmix_value_t *src,
                                          pmix_data_type_t type)
{
    pmix_value_t *p;

    PMIX_HIDE_UNUSED_PARAMS(type);

    /* create the new object */
    *dest = (pmix_value_t *) malloc(sizeof(pmix_value_t));
    if (NULL == *dest) {
        return PMIX_ERR_OUT_OF_RESOURCE;
    }
    p = *dest;

    /* copy the type */
    p->type = src->type;
    /* copy the data */
    return pmix_bfrops_base_value_xfer(p, src);
}

pmix_status_t pmix_bfrops_base_copy_info(pmix_info_t **dest, pmix_info_t *src,
                                         pmix_data_type_t type)
{
    PMIX_HIDE_UNUSED_PARAMS(type);

    *dest = (pmix_info_t *) malloc(sizeof(pmix_info_t));
    pmix_strncpy((*dest)->key, src->key, PMIX_MAX_KEYLEN);
    (*dest)->flags = src->flags;
    return pmix_bfrops_base_value_xfer(&(*dest)->value, &src->value);
}

pmix_status_t pmix_bfrops_base_copy_buf(pmix_buffer_t **dest, pmix_buffer_t *src,
                                        pmix_data_type_t type)
{
    PMIX_HIDE_UNUSED_PARAMS(type);

    *dest = PMIX_NEW(pmix_buffer_t);
    pmix_bfrops_base_copy_payload(*dest, src);
    return PMIX_SUCCESS;
}

pmix_status_t pmix_bfrops_base_copy_app(pmix_app_t **dest, pmix_app_t *src, pmix_data_type_t type)
{
    size_t j;

    PMIX_HIDE_UNUSED_PARAMS(type);

    *dest = (pmix_app_t *) malloc(sizeof(pmix_app_t));
    (*dest)->cmd = strdup(src->cmd);
    (*dest)->argv = pmix_argv_copy(src->argv);
    (*dest)->env = pmix_argv_copy(src->env);
    if (NULL != src->cwd) {
        (*dest)->cwd = strdup(src->cwd);
    }
    (*dest)->maxprocs = src->maxprocs;
    (*dest)->ninfo = src->ninfo;
    (*dest)->info = (pmix_info_t *) malloc(src->ninfo * sizeof(pmix_info_t));
    for (j = 0; j < src->ninfo; j++) {
        pmix_strncpy((*dest)->info[j].key, src->info[j].key, PMIX_MAX_KEYLEN);
        pmix_value_xfer(&(*dest)->info[j].value, &src->info[j].value);
    }
    return PMIX_SUCCESS;
}

pmix_status_t pmix_bfrops_base_copy_kval(pmix_kval_t **dest, pmix_kval_t *src,
                                         pmix_data_type_t type)
{
    pmix_kval_t *p;

    PMIX_HIDE_UNUSED_PARAMS(type);

    /* create the new object */
    *dest = PMIX_NEW(pmix_kval_t);
    if (NULL == *dest) {
        return PMIX_ERR_OUT_OF_RESOURCE;
    }
    p = *dest;

    /* copy the type */
    p->value->type = src->value->type;
    /* copy the data */
    return pmix_bfrops_base_value_xfer(p->value, src->value);
}

pmix_status_t pmix_bfrops_base_copy_proc(pmix_proc_t **dest, pmix_proc_t *src,
                                         pmix_data_type_t type)
{
    PMIX_HIDE_UNUSED_PARAMS(type);

    *dest = (pmix_proc_t *) malloc(sizeof(pmix_proc_t));
    if (NULL == *dest) {
        return PMIX_ERR_OUT_OF_RESOURCE;
    }
    pmix_strncpy((*dest)->nspace, src->nspace, PMIX_MAX_NSLEN);
    (*dest)->rank = src->rank;
    return PMIX_SUCCESS;
}

pmix_status_t pmix_bfrop_base_copy_persist(pmix_persistence_t **dest, pmix_persistence_t *src,
                                           pmix_data_type_t type)
{
    PMIX_HIDE_UNUSED_PARAMS(type);

    *dest = (pmix_persistence_t *) malloc(sizeof(pmix_persistence_t));
    if (NULL == *dest) {
        return PMIX_ERR_OUT_OF_RESOURCE;
    }
    memcpy(*dest, src, sizeof(pmix_persistence_t));
    return PMIX_SUCCESS;
}

pmix_status_t pmix_bfrops_base_copy_bo(pmix_byte_object_t **dest, pmix_byte_object_t *src,
                                       pmix_data_type_t type)
{
    PMIX_HIDE_UNUSED_PARAMS(type);

    *dest = (pmix_byte_object_t *) malloc(sizeof(pmix_byte_object_t));
    if (NULL == *dest) {
        return PMIX_ERR_OUT_OF_RESOURCE;
    }
    (*dest)->bytes = (char *) malloc(src->size);
    memcpy((*dest)->bytes, src->bytes, src->size);
    (*dest)->size = src->size;
    return PMIX_SUCCESS;
}

pmix_status_t pmix_bfrops_base_copy_pdata(pmix_pdata_t **dest, pmix_pdata_t *src,
                                          pmix_data_type_t type)
{
    PMIX_HIDE_UNUSED_PARAMS(type);

    *dest = (pmix_pdata_t *) malloc(sizeof(pmix_pdata_t));
    pmix_strncpy((*dest)->proc.nspace, src->proc.nspace, PMIX_MAX_NSLEN);
    (*dest)->proc.rank = src->proc.rank;
    pmix_strncpy((*dest)->key, src->key, PMIX_MAX_KEYLEN);
    return pmix_bfrops_base_value_xfer(&(*dest)->value, &src->value);
}

pmix_status_t pmix_bfrops_base_copy_pinfo(pmix_proc_info_t **dest, pmix_proc_info_t *src,
                                          pmix_data_type_t type)
{
    pmix_proc_info_t *p;

    PMIX_HIDE_UNUSED_PARAMS(type);

    PMIX_PROC_INFO_CREATE(p, 1);
    if (NULL == p) {
        return PMIX_ERR_NOMEM;
    }
    memcpy(&p->proc, &src->proc, sizeof(pmix_proc_t));
    if (NULL != src->hostname) {
        p->hostname = strdup(src->hostname);
    }
    if (NULL != src->executable_name) {
        p->executable_name = strdup(src->executable_name);
    }
    memcpy(&p->pid, &src->pid, sizeof(pid_t));
    memcpy(&p->exit_code, &src->exit_code, sizeof(int));
    memcpy(&p->state, &src->state, sizeof(pmix_proc_state_t));
    *dest = p;
    return PMIX_SUCCESS;
}

static pmix_status_t fill_coord(pmix_coord_t *dst, pmix_coord_t *src)
{
    dst->view = src->view;
    dst->dims = src->dims;
    if (0 < dst->dims) {
        dst->coord = (uint32_t *) malloc(dst->dims * sizeof(uint32_t));
        if (NULL == dst->coord) {
            return PMIX_ERR_NOMEM;
        }
        memcpy(dst->coord, src->coord, dst->dims * sizeof(uint32_t));
    }
    return PMIX_SUCCESS;
}

/* the pmix_data_array_t is a little different in that it
 * is an array of values, and so we cannot just copy one
 * value at a time. So handle all value types here */
pmix_status_t pmix_bfrops_base_copy_darray(pmix_data_array_t **dest, pmix_data_array_t *src,
                                           pmix_data_type_t type)
{
    pmix_data_array_t *p;
    size_t n, m;
    pmix_status_t rc;
    char **prarray, **strarray;
    pmix_value_t *pv, *sv;
    pmix_app_t *pa, *sa;
    pmix_info_t *p1, *s1;
    pmix_pdata_t *pd, *sd;
    pmix_buffer_t *pb, *sb;
    pmix_byte_object_t *pbo, *sbo;
    pmix_kval_t *pk, *sk;
    pmix_proc_info_t *pi, *si;
    pmix_query_t *pq, *sq;
    pmix_envar_t *pe, *se;
    pmix_regattr_t *pr, *sr;
    pmix_coord_t *pc, *sc;
    pmix_cpuset_t *pcpuset, *scpuset;
    pmix_geometry_t *pgeoset, *sgeoset;
    pmix_device_distance_t *pdevdist, *sdevdist;
    pmix_endpoint_t *pendpt, *sendpt;
    pmix_nspace_t *pns, *sns;
    pmix_proc_stats_t *pstats, *sstats;
    pmix_disk_stats_t *dkdest, *dksrc;
    pmix_net_stats_t *ntdest, *ntsrc;
    pmix_node_stats_t *nddest, *ndsrc;

    PMIX_HIDE_UNUSED_PARAMS(type);

    p = (pmix_data_array_t *) calloc(1, sizeof(pmix_data_array_t));
    if (NULL == p) {
        return PMIX_ERR_NOMEM;
    }
    p->type = src->type;
    p->size = src->size;
    if (0 == p->size || NULL == src->array) {
        *dest = p;
        return PMIX_SUCCESS;
    }

    /* process based on type of array element */
    switch (src->type) {
    case PMIX_UINT8:
    case PMIX_INT8:
    case PMIX_BYTE:
        p->array = (char *) malloc(src->size);
        if (NULL == p->array) {
            free(p);
            return PMIX_ERR_NOMEM;
        }
        memcpy(p->array, src->array, src->size);
        break;
    case PMIX_UINT16:
    case PMIX_INT16:
        p->array = (char *) malloc(src->size * sizeof(uint16_t));
        if (NULL == p->array) {
            free(p);
            return PMIX_ERR_NOMEM;
        }
        memcpy(p->array, src->array, src->size * sizeof(uint16_t));
        break;
    case PMIX_UINT32:
    case PMIX_INT32:
        p->array = (char *) malloc(src->size * sizeof(uint32_t));
        if (NULL == p->array) {
            free(p);
            return PMIX_ERR_NOMEM;
        }
        memcpy(p->array, src->array, src->size * sizeof(uint32_t));
        break;
    case PMIX_UINT64:
    case PMIX_INT64:
        p->array = (char *) malloc(src->size * sizeof(uint64_t));
        if (NULL == p->array) {
            free(p);
            return PMIX_ERR_NOMEM;
        }
        memcpy(p->array, src->array, src->size * sizeof(uint64_t));
        break;
    case PMIX_BOOL:
        p->array = (char *) malloc(src->size * sizeof(bool));
        if (NULL == p->array) {
            free(p);
            return PMIX_ERR_NOMEM;
        }
        memcpy(p->array, src->array, src->size * sizeof(bool));
        break;
    case PMIX_SIZE:
        p->array = (char *) malloc(src->size * sizeof(size_t));
        if (NULL == p->array) {
            free(p);
            return PMIX_ERR_NOMEM;
        }
        memcpy(p->array, src->array, src->size * sizeof(size_t));
        break;
    case PMIX_PID:
        p->array = (char *) malloc(src->size * sizeof(pid_t));
        if (NULL == p->array) {
            free(p);
            return PMIX_ERR_NOMEM;
        }
        memcpy(p->array, src->array, src->size * sizeof(pid_t));
        break;
    case PMIX_STRING:
        p->array = (char **) malloc(src->size * sizeof(char *));
        if (NULL == p->array) {
            free(p);
            return PMIX_ERR_NOMEM;
        }
        prarray = (char **) p->array;
        strarray = (char **) src->array;
        for (n = 0; n < src->size; n++) {
            if (NULL != strarray[n]) {
                prarray[n] = strdup(strarray[n]);
            }
        }
        break;
    case PMIX_INT:
    case PMIX_UINT:
        p->array = (char *) malloc(src->size * sizeof(int));
        if (NULL == p->array) {
            free(p);
            return PMIX_ERR_NOMEM;
        }
        memcpy(p->array, src->array, src->size * sizeof(int));
        break;
    case PMIX_FLOAT:
        p->array = (char *) malloc(src->size * sizeof(float));
        if (NULL == p->array) {
            free(p);
            return PMIX_ERR_NOMEM;
        }
        memcpy(p->array, src->array, src->size * sizeof(float));
        break;
    case PMIX_DOUBLE:
        p->array = (char *) malloc(src->size * sizeof(double));
        if (NULL == p->array) {
            free(p);
            return PMIX_ERR_NOMEM;
        }
        memcpy(p->array, src->array, src->size * sizeof(double));
        break;
    case PMIX_TIMEVAL:
        p->array = (struct timeval *) malloc(src->size * sizeof(struct timeval));
        if (NULL == p->array) {
            free(p);
            return PMIX_ERR_NOMEM;
        }
        memcpy(p->array, src->array, src->size * sizeof(struct timeval));
        break;
    case PMIX_TIME:
        p->array = (time_t *) malloc(src->size * sizeof(time_t));
        if (NULL == p->array) {
            free(p);
            return PMIX_ERR_NOMEM;
        }
        memcpy(p->array, src->array, src->size * sizeof(time_t));
        break;
    case PMIX_STATUS:
        p->array = (pmix_status_t *) malloc(src->size * sizeof(pmix_status_t));
        if (NULL == p->array) {
            free(p);
            return PMIX_ERR_NOMEM;
        }
        memcpy(p->array, src->array, src->size * sizeof(pmix_status_t));
        break;
    case PMIX_VALUE:
        PMIX_VALUE_CREATE(p->array, src->size);
        if (NULL == p->array) {
            free(p);
            return PMIX_ERR_NOMEM;
        }
        pv = (pmix_value_t *) p->array;
        sv = (pmix_value_t *) src->array;
        for (n = 0; n < src->size; n++) {
            if (PMIX_SUCCESS != (rc = pmix_bfrops_base_value_xfer(&pv[n], &sv[n]))) {
                PMIX_VALUE_FREE(pv, src->size);
                free(p);
                return rc;
            }
        }
        break;
    case PMIX_PROC:
        PMIX_PROC_CREATE(p->array, src->size);
        if (NULL == p->array) {
            free(p);
            return PMIX_ERR_NOMEM;
        }
        memcpy(p->array, src->array, src->size * sizeof(pmix_proc_t));
        break;
    case PMIX_PROC_RANK:
        p->array = (char *) malloc(src->size * sizeof(pmix_rank_t));
        if (NULL == p->array) {
            free(p);
            return PMIX_ERR_NOMEM;
        }
        memcpy(p->array, src->array, src->size * sizeof(pmix_proc_t));
        break;
    case PMIX_APP:
        PMIX_APP_CREATE(p->array, src->size);
        if (NULL == p->array) {
            free(p);
            return PMIX_ERR_NOMEM;
        }
        pa = (pmix_app_t *) p->array;
        sa = (pmix_app_t *) src->array;
        for (n = 0; n < src->size; n++) {
            if (NULL != sa[n].cmd) {
                pa[n].cmd = strdup(sa[n].cmd);
            }
            if (NULL != sa[n].argv) {
                pa[n].argv = pmix_argv_copy(sa[n].argv);
            }
            if (NULL != sa[n].env) {
                pa[n].env = pmix_argv_copy(sa[n].env);
            }
            if (NULL != sa[n].cwd) {
                pa[n].cwd = strdup(sa[n].cwd);
            }
            pa[n].maxprocs = sa[n].maxprocs;
            if (0 < sa[n].ninfo && NULL != sa[n].info) {
                PMIX_INFO_CREATE(pa[n].info, sa[n].ninfo);
                if (NULL == pa[n].info) {
                    PMIX_APP_FREE(pa, p->size);
                    free(p);
                    return PMIX_ERR_NOMEM;
                }
                pa[n].ninfo = sa[n].ninfo;
                for (m = 0; m < pa[n].ninfo; m++) {
                    PMIX_INFO_XFER(&pa[n].info[m], &sa[n].info[m]);
                }
            }
        }
        break;
    case PMIX_INFO:
        PMIX_INFO_CREATE(p->array, src->size);
        if (NULL == p->array) {
            free(p);
            return PMIX_ERR_NOMEM;
        }
        p1 = (pmix_info_t *) p->array;
        s1 = (pmix_info_t *) src->array;
        for (n = 0; n < src->size; n++) {
            PMIX_INFO_XFER(&p1[n], &s1[n]);
        }
        break;
    case PMIX_PDATA:
        PMIX_PDATA_CREATE(p->array, src->size);
        if (NULL == p->array) {
            free(p);
            return PMIX_ERR_NOMEM;
        }
        pd = (pmix_pdata_t *) p->array;
        sd = (pmix_pdata_t *) src->array;
        for (n = 0; n < src->size; n++) {
            PMIX_PDATA_XFER(&pd[n], &sd[n]);
        }
        break;
    case PMIX_BUFFER:
        p->array = (pmix_buffer_t *) malloc(src->size * sizeof(pmix_buffer_t));
        if (NULL == p->array) {
            free(p);
            return PMIX_ERR_NOMEM;
        }
        pb = (pmix_buffer_t *) p->array;
        sb = (pmix_buffer_t *) src->array;
        for (n = 0; n < src->size; n++) {
            PMIX_CONSTRUCT(&pb[n], pmix_buffer_t);
            pmix_bfrops_base_copy_payload(&pb[n], &sb[n]);
        }
        break;
    case PMIX_BYTE_OBJECT:
    case PMIX_COMPRESSED_STRING:
        p->array = (pmix_byte_object_t *) malloc(src->size * sizeof(pmix_byte_object_t));
        if (NULL == p->array) {
            free(p);
            return PMIX_ERR_NOMEM;
        }
        pbo = (pmix_byte_object_t *) p->array;
        sbo = (pmix_byte_object_t *) src->array;
        for (n = 0; n < src->size; n++) {
            if (NULL != sbo[n].bytes && 0 < sbo[n].size) {
                pbo[n].size = sbo[n].size;
                pbo[n].bytes = (char *) malloc(pbo[n].size);
                memcpy(pbo[n].bytes, sbo[n].bytes, pbo[n].size);
            } else {
                pbo[n].bytes = NULL;
                pbo[n].size = 0;
            }
        }
        break;
    case PMIX_KVAL:
        p->array = (pmix_kval_t *) calloc(src->size, sizeof(pmix_kval_t));
        if (NULL == p->array) {
            free(p);
            return PMIX_ERR_NOMEM;
        }
        pk = (pmix_kval_t *) p->array;
        sk = (pmix_kval_t *) src->array;
        for (n = 0; n < src->size; n++) {
            if (NULL != sk[n].key) {
                pk[n].key = strdup(sk[n].key);
            }
            if (NULL != sk[n].value) {
                PMIX_VALUE_CREATE(pk[n].value, 1);
                if (NULL == pk[n].value) {
                    PMIX_VALUE_FREE(pk[n].value, 1);
                    free(p);
                    return PMIX_ERR_NOMEM;
                }
                if (PMIX_SUCCESS != (rc = pmix_bfrops_base_value_xfer(pk[n].value, sk[n].value))) {
                    PMIX_VALUE_FREE(pk[n].value, 1);
                    free(p);
                    return rc;
                }
            }
        }
        break;
    case PMIX_PERSIST:
        p->array = (pmix_persistence_t *) malloc(src->size * sizeof(pmix_persistence_t));
        if (NULL == p->array) {
            free(p);
            return PMIX_ERR_NOMEM;
        }
        memcpy(p->array, src->array, src->size * sizeof(pmix_persistence_t));
        break;
    case PMIX_POINTER:
        p->array = (char **) malloc(src->size * sizeof(char *));
        prarray = (char **) p->array;
        strarray = (char **) src->array;
        for (n = 0; n < src->size; n++) {
            prarray[n] = strarray[n];
        }
        break;
    case PMIX_SCOPE:
        p->array = (pmix_scope_t *) malloc(src->size * sizeof(pmix_scope_t));
        if (NULL == p->array) {
            free(p);
            return PMIX_ERR_NOMEM;
        }
        memcpy(p->array, src->array, src->size * sizeof(pmix_scope_t));
        break;
    case PMIX_DATA_RANGE:
        p->array = (pmix_data_range_t *) malloc(src->size * sizeof(pmix_data_range_t));
        if (NULL == p->array) {
            free(p);
            return PMIX_ERR_NOMEM;
        }
        memcpy(p->array, src->array, src->size * sizeof(pmix_data_range_t));
        break;
    case PMIX_COMMAND:
        p->array = (pmix_cmd_t *) malloc(src->size * sizeof(pmix_cmd_t));
        if (NULL == p->array) {
            free(p);
            return PMIX_ERR_NOMEM;
        }
        memcpy(p->array, src->array, src->size * sizeof(pmix_cmd_t));
        break;
    case PMIX_INFO_DIRECTIVES:
        p->array = (pmix_info_directives_t *) malloc(src->size * sizeof(pmix_info_directives_t));
        if (NULL == p->array) {
            free(p);
            return PMIX_ERR_NOMEM;
        }
        memcpy(p->array, src->array, src->size * sizeof(pmix_info_directives_t));
        break;
    case PMIX_PROC_INFO:
        PMIX_PROC_INFO_CREATE(p->array, src->size);
        if (NULL == p->array) {
            free(p);
            return PMIX_ERR_NOMEM;
        }
        pi = (pmix_proc_info_t *) p->array;
        si = (pmix_proc_info_t *) src->array;
        for (n = 0; n < src->size; n++) {
            memcpy(&pi[n].proc, &si[n].proc, sizeof(pmix_proc_t));
            if (NULL != si[n].hostname) {
                pi[n].hostname = strdup(si[n].hostname);
            } else {
                pi[n].hostname = NULL;
            }
            if (NULL != si[n].executable_name) {
                pi[n].executable_name = strdup(si[n].executable_name);
            } else {
                pi[n].executable_name = NULL;
            }
            pi[n].pid = si[n].pid;
            pi[n].exit_code = si[n].exit_code;
            pi[n].state = si[n].state;
        }
        break;
    case PMIX_DATA_ARRAY:
        free(p);
        return PMIX_ERR_NOT_SUPPORTED; // don't support iterative arrays
    case PMIX_QUERY:
        PMIX_QUERY_CREATE(p->array, src->size);
        if (NULL == p->array) {
            free(p);
            return PMIX_ERR_NOMEM;
        }
        pq = (pmix_query_t *) p->array;
        sq = (pmix_query_t *) src->array;
        for (n = 0; n < src->size; n++) {
            if (NULL != sq[n].keys) {
                pq[n].keys = pmix_argv_copy(sq[n].keys);
            }
            if (NULL != sq[n].qualifiers && 0 < sq[n].nqual) {
                PMIX_INFO_CREATE(pq[n].qualifiers, sq[n].nqual);
                if (NULL == pq[n].qualifiers) {
                    PMIX_INFO_FREE(pq[n].qualifiers, sq[n].nqual);
                    free(p);
                    return PMIX_ERR_NOMEM;
                }
                for (m = 0; m < sq[n].nqual; m++) {
                    PMIX_INFO_XFER(&pq[n].qualifiers[m], &sq[n].qualifiers[m]);
                }
                pq[n].nqual = sq[n].nqual;
            } else {
                pq[n].qualifiers = NULL;
                pq[n].nqual = 0;
            }
        }
        break;
    case PMIX_ENVAR:
        PMIX_ENVAR_CREATE(p->array, src->size);
        if (NULL == p->array) {
            free(p);
            return PMIX_ERR_NOMEM;
        }
        pe = (pmix_envar_t *) p->array;
        se = (pmix_envar_t *) src->array;
        for (n = 0; n < src->size; n++) {
            if (NULL != se[n].envar) {
                pe[n].envar = strdup(se[n].envar);
            }
            if (NULL != se[n].value) {
                pe[n].value = strdup(se[n].value);
            }
            pe[n].separator = se[n].separator;
        }
        break;
    case PMIX_COORD:
        p->array = malloc(src->size * sizeof(pmix_coord_t));
        if (NULL == p->array) {
            free(p);
            return PMIX_ERR_NOMEM;
        }
        pc = (pmix_coord_t *) p->array;
        sc = (pmix_coord_t *) src->array;
        for (n = 0; n < src->size; n++) {
            rc = fill_coord(&pc[n], &sc[n]);
            if (PMIX_SUCCESS != rc) {
                PMIX_COORD_FREE(pc, src->size);
                free(p);
                return rc;
            }
        }
        break;
    case PMIX_REGATTR:
        PMIX_REGATTR_CREATE(p->array, src->size);
        if (NULL == p->array) {
            free(p);
            return PMIX_ERR_NOMEM;
        }
        pr = (pmix_regattr_t *) p->array;
        sr = (pmix_regattr_t *) src->array;
        for (n = 0; n < src->size; n++) {
            if (NULL != sr[n].name) {
                pr[n].name = strdup(sr[n].name);
            }
            PMIX_LOAD_KEY(pr[n].string, sr[n].string);
            pr[n].type = sr[n].type;
            pr[n].description = pmix_argv_copy(sr[n].description);
        }
        break;
    case PMIX_PROC_CPUSET:
        PMIX_CPUSET_CREATE(p->array, src->size);
        if (NULL == p->array) {
            free(p);
            return PMIX_ERR_NOMEM;
        }
        pcpuset = (pmix_cpuset_t *) p->array;
        scpuset = (pmix_cpuset_t *) src->array;
        for (n = 0; n < src->size; n++) {
            rc = pmix_hwloc_copy_cpuset(&pcpuset[n], &scpuset[n]);
            if (PMIX_SUCCESS != rc) {
                pmix_hwloc_release_cpuset(pcpuset, src->size);
                free(p->array);
                free(p);
                return rc;
            }
        }
        break;
    case PMIX_GEOMETRY:
        PMIX_GEOMETRY_CREATE(p->array, src->size);
        if (NULL == p->array) {
            free(p);
            return PMIX_ERR_NOMEM;
        }
        pgeoset = (pmix_geometry_t *) p->array;
        sgeoset = (pmix_geometry_t *) src->array;
        for (n = 0; n < src->size; n++) {
            pgeoset[n].fabric = sgeoset[n].fabric;
            if (NULL != sgeoset[n].uuid) {
                pgeoset[n].uuid = strdup(sgeoset[n].uuid);
            }
            if (NULL != sgeoset[n].osname) {
                pgeoset[n].osname = strdup(sgeoset[n].osname);
            }
            if (NULL != sgeoset[n].coordinates) {
                pgeoset[n].ncoords = sgeoset[n].ncoords;
                pgeoset[n].coordinates = (pmix_coord_t *) malloc(pgeoset[n].ncoords
                                                                 * sizeof(pmix_coord_t));
                if (NULL == pgeoset[n].coordinates) {
                    free(p);
                    return PMIX_ERR_NOMEM;
                }
                for (m = 0; m < pgeoset[n].ncoords; m++) {
                    rc = fill_coord(&pgeoset[n].coordinates[m], &sgeoset[n].coordinates[m]);
                    if (PMIX_SUCCESS != rc) {
                        PMIX_GEOMETRY_FREE(pgeoset, src->size);
                        free(p);
                        return rc;
                    }
                }
            }
        }
        break;
    case PMIX_DEVICE_DIST:
        PMIX_DEVICE_DIST_CREATE(p->array, src->size);
        if (NULL == p->array) {
            free(p);
            return PMIX_ERR_NOMEM;
        }
        pdevdist = (pmix_device_distance_t *) p->array;
        sdevdist = (pmix_device_distance_t *) src->array;
        for (n = 0; n < src->size; n++) {
            if (NULL != sdevdist[n].uuid) {
                pdevdist[n].uuid = strdup(sdevdist[n].uuid);
            }
            if (NULL != sdevdist[n].osname) {
                pdevdist[n].osname = strdup(sdevdist[n].osname);
            }
            pdevdist[n].type = sdevdist[n].type;
            pdevdist[n].mindist = sdevdist[n].mindist;
            pdevdist[n].maxdist = sdevdist[n].maxdist;
        }
        break;
    case PMIX_ENDPOINT:
        PMIX_ENDPOINT_CREATE(p->array, src->size);
        if (NULL == p->array) {
            free(p);
            return PMIX_ERR_NOMEM;
        }
        pendpt = (pmix_endpoint_t *) p->array;
        sendpt = (pmix_endpoint_t *) src->array;
        for (n = 0; n < src->size; n++) {
            if (NULL != sendpt[n].uuid) {
                pendpt[n].uuid = strdup(sendpt[n].uuid);
            }
            if (NULL != sendpt[n].osname) {
                pendpt[n].osname = strdup(sendpt[n].osname);
            }
            if (NULL != sendpt[n].endpt.bytes) {
                pendpt[n].endpt.bytes = (char *) malloc(sendpt[n].endpt.size);
                memcpy(pendpt[n].endpt.bytes, sendpt[n].endpt.bytes, sendpt[n].endpt.size);
                pendpt[n].endpt.size = sendpt[n].endpt.size;
            }
        }
        break;
    case PMIX_PROC_NSPACE:
        p->array = malloc(src->size * sizeof(pmix_nspace_t));
        if (NULL == p->array) {
            free(p);
            return PMIX_ERR_NOMEM;
        }
        p->size = src->size;
        pns = (pmix_nspace_t *) p->array;
        sns = (pmix_nspace_t *) src->array;
        for (n = 0; n < src->size; n++) {
            PMIX_LOAD_NSPACE(&pns[n], sns[n]);
        }
        break;
    case PMIX_PROC_STATS:
        PMIX_PROC_STATS_CREATE(p->array, src->size);
        if (NULL == p->array) {
            free(p);
            return PMIX_ERR_NOMEM;
        }
        pstats = (pmix_proc_stats_t *) p->array;
        sstats = (pmix_proc_stats_t *) src->array;
        for (n = 0; n < src->size; n++) {
            _populate_pstats(&pstats[n], &sstats[n]);
        }
        break;
    case PMIX_DISK_STATS:
        PMIX_DISK_STATS_CREATE(p->array, src->size);
        if (NULL == p->array) {
            free(p);
            return PMIX_ERR_NOMEM;
        }
        dkdest = (pmix_disk_stats_t *) p->array;
        dksrc = (pmix_disk_stats_t *) src->array;
        for (n = 0; n < src->size; n++) {
            _populate_dkstats(&dkdest[n], &dksrc[n]);
        }
        break;
    case PMIX_NET_STATS:
        PMIX_NET_STATS_CREATE(p->array, src->size);
        if (NULL == p->array) {
            free(p);
            return PMIX_ERR_NOMEM;
        }
        ntdest = (pmix_net_stats_t *) p->array;
        ntsrc = (pmix_net_stats_t *) src->array;
        for (n = 0; n < src->size; n++) {
            _populate_netstats(&ntdest[n], &ntsrc[n]);
        }
        break;
    case PMIX_NODE_STATS:
        PMIX_NODE_STATS_CREATE(p->array, src->size);
        if (NULL == p->array) {
            free(p);
            return PMIX_ERR_NOMEM;
        }
        nddest = (pmix_node_stats_t *) p->array;
        ndsrc = (pmix_node_stats_t *) src->array;
        for (n = 0; n < src->size; n++) {
            _populate_ndstats(&nddest[n], &ndsrc[n]);
        }
        break;
    default:
        free(p);
        return PMIX_ERR_UNKNOWN_DATA_TYPE;
    }

    (*dest) = p;
    return PMIX_SUCCESS;
}

pmix_status_t pmix_bfrops_base_copy_query(pmix_query_t **dest, pmix_query_t *src,
                                          pmix_data_type_t type)
{
    pmix_status_t rc;

    PMIX_HIDE_UNUSED_PARAMS(type);

    *dest = (pmix_query_t *) malloc(sizeof(pmix_query_t));
    if (NULL != src->keys) {
        (*dest)->keys = pmix_argv_copy(src->keys);
    }
    (*dest)->nqual = src->nqual;
    if (NULL != src->qualifiers) {
        if (PMIX_SUCCESS
            != (rc = pmix_bfrops_base_copy_info(&((*dest)->qualifiers), src->qualifiers,
                                                PMIX_INFO))) {
            free(*dest);
            return rc;
        }
    }
    return PMIX_SUCCESS;
}

pmix_status_t pmix_bfrops_base_copy_envar(pmix_envar_t **dest, pmix_envar_t *src,
                                          pmix_data_type_t type)
{
    PMIX_HIDE_UNUSED_PARAMS(type);

    PMIX_ENVAR_CREATE(*dest, 1);
    if (NULL == (*dest)) {
        return PMIX_ERR_NOMEM;
    }
    if (NULL != src->envar) {
        (*dest)->envar = strdup(src->envar);
    }
    if (NULL != src->value) {
        (*dest)->value = strdup(src->value);
    }
    (*dest)->separator = src->separator;
    return PMIX_SUCCESS;
}

pmix_status_t pmix_bfrops_base_copy_coord(pmix_coord_t **dest, pmix_coord_t *src,
                                          pmix_data_type_t type)
{
    pmix_coord_t *d;
    pmix_status_t rc;

    PMIX_HIDE_UNUSED_PARAMS(type);

    d = (pmix_coord_t *) malloc(sizeof(pmix_coord_t));
    if (NULL == d) {
        return PMIX_ERR_NOMEM;
    }
    PMIX_COORD_CONSTRUCT(d);
    rc = fill_coord(d, src);
    if (PMIX_SUCCESS != rc) {
        PMIX_COORD_DESTRUCT(d);
        free(d);
    } else {
        *dest = d;
    }
    return rc;
}

pmix_status_t pmix_bfrops_base_copy_regattr(pmix_regattr_t **dest, pmix_regattr_t *src,
                                            pmix_data_type_t type)
{
    PMIX_HIDE_UNUSED_PARAMS(type);

    PMIX_REGATTR_CREATE(*dest, 1);
    if (NULL == (*dest)) {
        return PMIX_ERR_NOMEM;
    }
    if (NULL != src->name) {
        (*dest)->name = strdup(src->name);
    }
    PMIX_LOAD_KEY((*dest)->string, src->string);
    (*dest)->type = src->type;
    (*dest)->description = pmix_argv_copy(src->description);
    return PMIX_SUCCESS;
}

pmix_status_t pmix_bfrops_base_copy_regex(char **dest, char *src, pmix_data_type_t type)
{
    size_t len;

    PMIX_HIDE_UNUSED_PARAMS(type);

    return pmix_preg.copy(dest, &len, src);
}

pmix_status_t pmix_bfrops_base_copy_cpuset(pmix_cpuset_t **dest, pmix_cpuset_t *src,
                                           pmix_data_type_t type)
{
    pmix_cpuset_t *dst;
    pmix_status_t rc;

    PMIX_HIDE_UNUSED_PARAMS(type);

    PMIX_CPUSET_CREATE(dst, 1);
    if (NULL == dst) {
        return PMIX_ERR_NOMEM;
    }

    rc = pmix_hwloc_copy_cpuset(dst, src);
    if (PMIX_SUCCESS == rc) {
        *dest = dst;
    } else {
        free(dst);
    }
    return rc;
}

pmix_status_t pmix_bfrops_base_copy_geometry(pmix_geometry_t **dest, pmix_geometry_t *src,
                                             pmix_data_type_t type)
{
    pmix_geometry_t *dst;
    pmix_status_t rc;
    size_t n;

    PMIX_HIDE_UNUSED_PARAMS(type);

    PMIX_GEOMETRY_CREATE(dst, 1);
    if (NULL == dst) {
        return PMIX_ERR_NOMEM;
    }

    dst->fabric = src->fabric;
    if (NULL != src->uuid) {
        dst->uuid = strdup(src->uuid);
    }
    if (NULL != src->osname) {
        dst->osname = strdup(src->osname);
    }
    if (NULL != src->coordinates) {
        dst->ncoords = src->ncoords;
        dst->coordinates = (pmix_coord_t *) calloc(dst->ncoords, sizeof(pmix_coord_t));
        for (n = 0; n < dst->ncoords; n++) {
            rc = fill_coord(&dst->coordinates[n], &src->coordinates[n]);
            if (PMIX_SUCCESS != rc) {
                PMIX_GEOMETRY_FREE(dst, 1);
                return rc;
            }
        }
    }

    *dest = dst;
    return PMIX_SUCCESS;
}

pmix_status_t pmix_bfrops_base_copy_devdist(pmix_device_distance_t **dest,
                                            pmix_device_distance_t *src, pmix_data_type_t type)
{
    pmix_device_distance_t *dst;

    PMIX_HIDE_UNUSED_PARAMS(type);

    PMIX_DEVICE_DIST_CREATE(dst, 1);
    if (NULL == dst) {
        return PMIX_ERR_NOMEM;
    }

    if (NULL != src->uuid) {
        dst->uuid = strdup(src->uuid);
    }
    if (NULL != src->osname) {
        dst->osname = strdup(src->osname);
    }
    dst->type = src->type;
    dst->mindist = src->mindist;
    dst->maxdist = src->maxdist;

    *dest = dst;
    return PMIX_SUCCESS;
}

pmix_status_t pmix_bfrops_base_copy_endpoint(pmix_endpoint_t **dest, pmix_endpoint_t *src,
                                             pmix_data_type_t type)
{
    pmix_endpoint_t *dst;

    PMIX_HIDE_UNUSED_PARAMS(type);

    PMIX_ENDPOINT_CREATE(dst, 1);
    if (NULL == dst) {
        return PMIX_ERR_NOMEM;
    }

    if (NULL != src->uuid) {
        dst->uuid = strdup(src->uuid);
    }
    if (NULL != src->osname) {
        dst->osname = strdup(src->osname);
    }
    if (NULL != src->endpt.bytes) {
        dst->endpt.bytes = (char *) malloc(src->endpt.size);
        memcpy(dst->endpt.bytes, src->endpt.bytes, src->endpt.size);
        dst->endpt.size = src->endpt.size;
    }

    *dest = dst;
    return PMIX_SUCCESS;
}

pmix_status_t pmix_bfrops_base_copy_topology(pmix_topology_t **dest, pmix_topology_t *src,
                                             pmix_data_type_t type)
{
    pmix_topology_t *dst;
    pmix_status_t rc;

    PMIX_HIDE_UNUSED_PARAMS(type);

    PMIX_TOPOLOGY_CREATE(dst, 1);
    if (NULL == dst) {
        return PMIX_ERR_NOMEM;
    }

    rc = pmix_hwloc_copy_topology(dst, src);
    if (PMIX_SUCCESS == rc) {
        *dest = dst;
    } else {
        free(dst);
    }
    return rc;
}

pmix_status_t pmix_bfrops_base_copy_nspace(pmix_nspace_t **dest, pmix_nspace_t *src,
                                           pmix_data_type_t type)
{
    pmix_nspace_t *dst;

    PMIX_HIDE_UNUSED_PARAMS(type);

    dst = (pmix_nspace_t *) malloc(sizeof(pmix_nspace_t));
    if (NULL == dst) {
        return PMIX_ERR_NOMEM;
    }
    PMIX_LOAD_NSPACE(dst, (char *) src);
    *dest = dst;
    return PMIX_SUCCESS;
}

static void _populate_pstats(pmix_proc_stats_t *p, pmix_proc_stats_t *src)
{
    /* copy the individual fields */
    if (NULL != src->node) {
        p->node = strdup(src->node);
    }
    memcpy(&p->proc, &src->proc, sizeof(pmix_proc_t));
    p->pid = src->pid;
    if (NULL != src->cmd) {
        p->cmd = strdup(src->cmd);
    }
    p->state = src->state;
    p->time = src->time;
    p->priority = src->priority;
    p->num_threads = src->num_threads;
    p->pss = src->pss;
    p->vsize = src->vsize;
    p->rss = src->rss;
    p->peak_vsize = src->peak_vsize;
    p->processor = src->processor;
    p->sample_time.tv_sec = src->sample_time.tv_sec;
    p->sample_time.tv_usec = src->sample_time.tv_usec;
}

pmix_status_t pmix_bfrops_base_copy_pstats(pmix_proc_stats_t **dest, pmix_proc_stats_t *src,
                                           pmix_data_type_t type)
{
    pmix_proc_stats_t *p;

    PMIX_HIDE_UNUSED_PARAMS(type);

    /* create the new object */
    PMIX_PROC_STATS_CREATE(p, 1);
    if (NULL == p) {
        return PMIX_ERR_NOMEM;
    }
    *dest = p;
    _populate_pstats(p, src);
    return PMIX_SUCCESS;
}

static void _populate_dkstats(pmix_disk_stats_t *p, pmix_disk_stats_t *src)
{
    if (NULL != src->disk) {
        p->disk = strdup(src->disk);
    }
    p->num_reads_completed = src->num_reads_completed;
    p->num_reads_merged = src->num_reads_merged;
    p->num_sectors_read = src->num_sectors_read;
    p->milliseconds_reading = src->milliseconds_reading;
    p->num_writes_completed = src->num_writes_completed;
    p->num_writes_merged = src->num_writes_merged;
    p->num_sectors_written = src->num_sectors_written;
    p->milliseconds_writing = src->milliseconds_writing;
    p->num_ios_in_progress = src->num_ios_in_progress;
    p->milliseconds_io = src->milliseconds_io;
    p->weighted_milliseconds_io = src->weighted_milliseconds_io;
}

pmix_status_t pmix_bfrops_base_copy_dkstats(pmix_disk_stats_t **dest, pmix_disk_stats_t *src,
                                            pmix_data_type_t type)
{
    pmix_disk_stats_t *p;

    PMIX_HIDE_UNUSED_PARAMS(type);

    /* create the new object */
    PMIX_DISK_STATS_CREATE(p, 1);
    if (NULL == p) {
        return PMIX_ERR_NOMEM;
    }
    *dest = p;
    _populate_dkstats(p, src);
    return PMIX_SUCCESS;
}

static void _populate_netstats(pmix_net_stats_t *p, pmix_net_stats_t *src)
{
    if (NULL != src->net_interface) {
        p->net_interface = strdup(src->net_interface);
    }
    p->num_bytes_recvd = src->num_bytes_recvd;
    p->num_packets_recvd = src->num_packets_recvd;
    p->num_recv_errs = src->num_recv_errs;
    p->num_bytes_sent = src->num_bytes_sent;
    p->num_packets_sent = src->num_packets_sent;
    p->num_send_errs = src->num_send_errs;
}

pmix_status_t pmix_bfrops_base_copy_netstats(pmix_net_stats_t **dest, pmix_net_stats_t *src,
                                             pmix_data_type_t type)
{
    pmix_net_stats_t *p;

    PMIX_HIDE_UNUSED_PARAMS(type);

    /* create the new object */
    PMIX_NET_STATS_CREATE(p, 1);
    if (NULL == p) {
        return PMIX_ERR_NOMEM;
    }
    *dest = p;
    _populate_netstats(p, src);
    return PMIX_SUCCESS;
}

static void _populate_ndstats(pmix_node_stats_t *p, pmix_node_stats_t *src)
{
    size_t n;

    /* copy the individual fields */
    if (NULL != src->node) {
        p->node = strdup(src->node);
    }
    p->la = src->la;
    p->la5 = src->la5;
    p->la15 = src->la15;
    p->total_mem = src->total_mem;
    p->free_mem = src->free_mem;
    p->buffers = src->buffers;
    p->cached = src->cached;
    p->swap_cached = src->swap_cached;
    p->swap_total = src->swap_total;
    p->swap_free = src->swap_free;
    p->mapped = src->mapped;
    p->sample_time.tv_sec = src->sample_time.tv_sec;
    p->sample_time.tv_usec = src->sample_time.tv_usec;
    p->ndiskstats = src->ndiskstats;
    if (0 < p->ndiskstats) {
        PMIX_DISK_STATS_CREATE(p->diskstats, p->ndiskstats);
        for (n = 0; n < p->ndiskstats; n++) {
            _populate_dkstats(&p->diskstats[n], &src->diskstats[n]);
        }
    }
    p->nnetstats = src->nnetstats;
    if (0 < p->nnetstats) {
        PMIX_NET_STATS_CREATE(p->netstats, p->nnetstats);
        for (n = 0; n < p->nnetstats; n++) {
            _populate_netstats(&p->netstats[n], &src->netstats[n]);
        }
    }
}

pmix_status_t pmix_bfrops_base_copy_ndstats(pmix_node_stats_t **dest, pmix_node_stats_t *src,
                                            pmix_data_type_t type)
{
    pmix_node_stats_t *p;

    PMIX_HIDE_UNUSED_PARAMS(type);

    /* create the new object */
    PMIX_NODE_STATS_CREATE(p, 1);
    if (NULL == p) {
        return PMIX_ERR_NOMEM;
    }
    *dest = p;
    _populate_ndstats(p, src);
    return PMIX_SUCCESS;
}

pmix_status_t pmix_bfrops_base_copy_dbuf(pmix_data_buffer_t **dest, pmix_data_buffer_t *src,
                                         pmix_data_type_t type)
{
    pmix_data_buffer_t *p;
    pmix_status_t rc;

    PMIX_HIDE_UNUSED_PARAMS(type);

    /* create the new object */
    PMIX_DATA_BUFFER_CREATE(p);
    if (NULL == p) {
        return PMIX_ERR_NOMEM;
    }
    *dest = p;

    rc = PMIx_Data_copy_payload(p, src);
    return rc;
}
