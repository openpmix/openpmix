/*
 * Copyright (c) 2015-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2016      IBM Corporation.  All rights reserved.
 *
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "src/include/pmix_config.h"

#include <string.h>
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
#include <time.h>

#include "include/pmix_common.h"

#include "src/mca/base/pmix_mca_base_var.h"
#include "src/class/pmix_list.h"
#include "src/include/pmix_socket_errno.h"
#include "src/include/pmix_globals.h"
#include "src/class/pmix_list.h"
#include "src/util/alfg.h"
#include "src/util/argv.h"
#include "src/util/error.h"
#include "src/util/output.h"
#include "src/util/pmix_environ.h"
#include "src/mca/preg/preg.h"
#include "src/hwloc/hwloc-internal.h"

#include "src/mca/pstrg/pstrg.h"
#include "src/mca/pstrg/base/base.h"
#include "pstrg_lustre.h"

static pmix_status_t lustre_init(void);
static void lustre_finalize(void);
static pmix_status_t query(pmix_query_t queries[], size_t nqueries,
                           pmix_list_t *results,
                           pmix_pstrg_query_cbfunc_t cbfunc, void *cbdata);

pmix_pstrg_base_module_t pmix_pstrg_lustre_module = {
    .name = "lustre",
    .init = lustre_init,
    .finalize = lustre_finalize,
    .query = query
};

typedef struct {
    char *name;
    pmix_dim_value_t cap;
    pmix_dim_value_t free;
    pmix_dim_value_t avail;
    pmix_dim_value_t bw;
    pmix_dim_value_t availbw;
} lustre_storage_t;

static lustre_storage_t availsys[] = {
    {.name = "lustre1", .cap = {1234.56, PMIX_UNIT_PETABYTES}, .free = {56.78, PMIX_UNIT_PETABYTES},
     .avail = {13.1, PMIX_UNIT_GIGABYTES}, .bw = {100.0, PMIX_UNIT_GBSEC}, .availbw = {13.2, PMIX_UNIT_MBSEC}},

    {.name = "lustre2", .cap = {789.12, PMIX_UNIT_MEGABYTES}, .free = {1.78, PMIX_UNIT_MEGABYTES},
     .avail = {100.1, PMIX_UNIT_KILOBYTES}, .bw = {10.0, PMIX_UNIT_KBSEC}, .availbw = {2.2, PMIX_UNIT_BSEC}},

    {.name = "lustre3", .cap = {9.1, PMIX_UNIT_TERABYTES}, .free = {3.5, PMIX_UNIT_GIGABYTES},
     .avail = {81.0, PMIX_UNIT_BYTES}, .bw = {5.0, PMIX_UNIT_EBSEC}, .availbw = {4.2, PMIX_UNIT_PBSEC}},
    {.name = NULL}
};

static pmix_status_t lustre_init(void)
{
    pmix_output_verbose(2, pmix_pstrg_base_framework.framework_output,
                        "pstrg: lustre init");

    /* ADD HERE:
     *
     * Discover/connect to any available Lustre systems. Return an error
     * if none are preset, or you are unable to connect to them
     */
    return PMIX_SUCCESS;
}

static void lustre_finalize(void)
{
    pmix_output_verbose(2, pmix_pstrg_base_framework.framework_output,
                        "pstrg: lustre finalize");

    /* ADD HERE:
     *
     * Disconnect from any Lustre systems to which you have connected
     */
}

static pmix_status_t query(pmix_query_t queries[], size_t nqueries,
                           pmix_list_t *results,
                           pmix_pstrg_query_cbfunc_t cbfunc, void *cbdata)
{
    size_t n, m, k, nvals;
    pmix_kval_t *kv;
    pmix_info_t *iptr;
    pmix_list_t qres;
    pmix_data_array_t *darray;
    char *sid;
    char **ans = NULL;

    pmix_output_verbose(2, pmix_pstrg_base_framework.framework_output,
                        "pstrg: lustre query");

    for (n=0; n < nqueries; n++) {
        for (m=0; NULL != queries[n].keys[m]; m++) {
            if (0 == strcmp(queries[n].keys[m], PMIX_QUERY_STORAGE_LIST)) {
                /* ADD HERE:
                 *
                 * Obtain a list of all available Lustre storage systems. The IDs
                 * we return should be those that we want the user to provide when
                 * asking about a specific Lustre system.
                 */

                /* add the IDs of the available storage systems */
                kv = PMIX_NEW(pmix_kval_t);
                kv->key = strdup(PMIX_QUERY_STORAGE_LIST);
                kv->value = (pmix_value_t*)malloc(sizeof(pmix_value_t));
                kv->value->type = PMIX_STRING;
                for (k=0; NULL != availsys[k].name; k++) {
                    pmix_argv_append_nosize(&ans, availsys[k].name);
                }
                kv->value->data.string = pmix_argv_join(ans, ',');
                pmix_argv_free(ans);
                pmix_list_append(results, &kv->super);
            } else if (0 == strcmp(queries[n].keys[m], PMIX_STORAGE_CAPACITY)) {
                /* ADD HERE:
                 *
                 * Get the capacity of the Lustre storage systems. If they ask for
                 * a specific one, then get just that one.
                 */

                /* did they specify a storage ID? */
                sid = NULL;
                for (k=0; k < queries[n].nqual; k++) {
                    if (0 == strcmp(queries[n].qualifiers[m].key, PMIX_STORAGE_ID)) {
                        sid = queries[n].qualifiers[m].value.data.string;
                        break;
                    }
                }
                PMIX_CONSTRUCT(&qres, pmix_list_t);
                for (k=0; NULL != availsys[k].name; k++) {
                    if (NULL == sid || 0 == strcmp(sid, availsys[k].name)) {
                        kv = PMIX_NEW(pmix_kval_t);
                        kv->key = strdup(availsys[k].name);
                        kv->value = (pmix_value_t*)malloc(sizeof(pmix_value_t));
                        kv->value->type = PMIX_DIM_VALUE;
                        kv->value->data.dimval = (pmix_dim_value_t*)malloc(sizeof(pmix_dim_value_t));
                        PMIX_DIM_VALUE_LOAD(kv->value->data.dimval, availsys[k].cap.dval, availsys[k].cap.units);
                        pmix_list_append(&qres, &kv->super);
                    }
                }
                nvals = pmix_list_get_size(&qres);
                if (0 < nvals) {
                    kv = PMIX_NEW(pmix_kval_t);
                    kv->key = strdup(PMIX_STORAGE_CAPACITY);
                    kv->value = (pmix_value_t*)malloc(sizeof(pmix_value_t));
                    PMIX_DATA_ARRAY_CREATE(darray, nvals, PMIX_INFO);
                    kv->value->type = PMIX_DATA_ARRAY;
                    kv->value->data.darray = darray;
                    pmix_list_append(results, &kv->super);
                    iptr = (pmix_info_t*)darray->array;
                    k = 0;
                    PMIX_LIST_FOREACH(kv, &qres, pmix_kval_t) {
                        PMIX_LOAD_KEY(&iptr[k], kv->key);
                        iptr[k].value.type = PMIX_DIM_VALUE;
                        iptr[k].value.data.dimval = kv->value->data.dimval;
                        kv->value = NULL;
                        ++k;
                    }
                }
                PMIX_LIST_DESTRUCT(&qres);
            } else if (0 == strcmp(queries[n].keys[m], PMIX_STORAGE_FREE)) {
                /* ADD HERE:
                 *
                 * Get the amount of free capacity of the Lustre storage systems. If they ask for
                 * a specific one, then get just that one.
                 */

                /* did they specify a storage ID? */
                sid = NULL;
                for (k=0; k < queries[n].nqual; k++) {
                    if (0 == strcmp(queries[n].qualifiers[m].key, PMIX_STORAGE_ID)) {
                        sid = queries[n].qualifiers[m].value.data.string;
                        break;
                    }
                }
                PMIX_CONSTRUCT(&qres, pmix_list_t);
                for (k=0; NULL != availsys[k].name; k++) {
                    if (NULL == sid || 0 == strcmp(sid, availsys[k].name)) {
                        kv = PMIX_NEW(pmix_kval_t);
                        kv->key = strdup(availsys[k].name);
                        kv->value = (pmix_value_t*)malloc(sizeof(pmix_value_t));
                        kv->value->type = PMIX_DIM_VALUE;
                        kv->value->data.dimval = (pmix_dim_value_t*)malloc(sizeof(pmix_dim_value_t));
                        PMIX_DIM_VALUE_LOAD(kv->value->data.dimval, availsys[k].free.dval, availsys[k].free.units);
                        pmix_list_append(&qres, &kv->super);
                    }
                }
                nvals = pmix_list_get_size(&qres);
                if (0 < nvals) {
                    kv = PMIX_NEW(pmix_kval_t);
                    kv->key = strdup(PMIX_STORAGE_FREE);
                    kv->value = (pmix_value_t*)malloc(sizeof(pmix_value_t));
                    PMIX_DATA_ARRAY_CREATE(darray, nvals, PMIX_INFO);
                    kv->value->type = PMIX_DATA_ARRAY;
                    kv->value->data.darray = darray;
                    pmix_list_append(results, &kv->super);
                    iptr = (pmix_info_t*)darray->array;
                    k = 0;
                    PMIX_LIST_FOREACH(kv, &qres, pmix_kval_t) {
                        PMIX_LOAD_KEY(&iptr[k], kv->key);
                        iptr[k].value.type = PMIX_DIM_VALUE;
                        iptr[k].value.data.dimval = kv->value->data.dimval;
                        kv->value = NULL;
                        ++k;
                    }
                }
                PMIX_LIST_DESTRUCT(&qres);
            } else if (0 == strcmp(queries[n].keys[m], PMIX_STORAGE_AVAIL)) {
                /* ADD HERE:
                 *
                 * Get the capacity of the Lustre storage systems that is available to the
                 * caller's userid/grpid. If you need further info, please let me know - e.g.,
                 * we could provide the app's jobid or argv[0]. If they ask for
                 * a specific storage system, then get just that one.
                 */

                /* did they specify a storage ID? */
                sid = NULL;
                for (k=0; k < queries[n].nqual; k++) {
                    if (0 == strcmp(queries[n].qualifiers[m].key, PMIX_STORAGE_ID)) {
                        sid = queries[n].qualifiers[m].value.data.string;
                        break;
                    }
                }
                PMIX_CONSTRUCT(&qres, pmix_list_t);
                for (k=0; NULL != availsys[k].name; k++) {
                    if (NULL == sid || 0 == strcmp(sid, availsys[k].name)) {
                        kv = PMIX_NEW(pmix_kval_t);
                        kv->key = strdup(availsys[k].name);
                        kv->value = (pmix_value_t*)malloc(sizeof(pmix_value_t));
                        kv->value->type = PMIX_DIM_VALUE;
                        kv->value->data.dimval = (pmix_dim_value_t*)malloc(sizeof(pmix_dim_value_t));
                        PMIX_DIM_VALUE_LOAD(kv->value->data.dimval, availsys[k].avail.dval, availsys[k].avail.units);
                        pmix_list_append(&qres, &kv->super);
                    }
                }
                nvals = pmix_list_get_size(&qres);
                if (0 < nvals) {
                    kv = PMIX_NEW(pmix_kval_t);
                    kv->key = strdup(PMIX_STORAGE_AVAIL);
                    kv->value = (pmix_value_t*)malloc(sizeof(pmix_value_t));
                    PMIX_DATA_ARRAY_CREATE(darray, nvals, PMIX_INFO);
                    kv->value->type = PMIX_DATA_ARRAY;
                    kv->value->data.darray = darray;
                    pmix_list_append(results, &kv->super);
                    iptr = (pmix_info_t*)darray->array;
                    k = 0;
                    PMIX_LIST_FOREACH(kv, &qres, pmix_kval_t) {
                        PMIX_LOAD_KEY(&iptr[k], kv->key);
                        iptr[k].value.type = PMIX_DIM_VALUE;
                        iptr[k].value.data.dimval = kv->value->data.dimval;
                        kv->value = NULL;
                        ++k;
                    }
                }
                PMIX_LIST_DESTRUCT(&qres);
            } else if (0 == strcmp(queries[n].keys[m], PMIX_STORAGE_BW)) {
                /* ADD HERE:
                 *
                 * Get the overall bandwidth of the Lustre storage systems. If they ask for
                 * a specific one, then get just that one.
                 */

                /* did they specify a storage ID? */
                sid = NULL;
                for (k=0; k < queries[n].nqual; k++) {
                    if (0 == strcmp(queries[n].qualifiers[m].key, PMIX_STORAGE_ID)) {
                        sid = queries[n].qualifiers[m].value.data.string;
                        break;
                    }
                }
                PMIX_CONSTRUCT(&qres, pmix_list_t);
                for (k=0; NULL != availsys[k].name; k++) {
                    if (NULL == sid || 0 == strcmp(sid, availsys[k].name)) {
                        kv = PMIX_NEW(pmix_kval_t);
                        kv->key = strdup(availsys[k].name);
                        kv->value = (pmix_value_t*)malloc(sizeof(pmix_value_t));
                        kv->value->type = PMIX_DIM_VALUE;
                        kv->value->data.dimval = (pmix_dim_value_t*)malloc(sizeof(pmix_dim_value_t));
                        PMIX_DIM_VALUE_LOAD(kv->value->data.dimval, availsys[k].bw.dval, availsys[k].bw.units);
                        pmix_list_append(&qres, &kv->super);
                    }
                }
                nvals = pmix_list_get_size(&qres);
                if (0 < nvals) {
                    kv = PMIX_NEW(pmix_kval_t);
                    kv->key = strdup(PMIX_STORAGE_BW);
                    kv->value = (pmix_value_t*)malloc(sizeof(pmix_value_t));
                    PMIX_DATA_ARRAY_CREATE(darray, nvals, PMIX_INFO);
                    kv->value->type = PMIX_DATA_ARRAY;
                    kv->value->data.darray = darray;
                    pmix_list_append(results, &kv->super);
                    iptr = (pmix_info_t*)darray->array;
                    k = 0;
                    PMIX_LIST_FOREACH(kv, &qres, pmix_kval_t) {
                        PMIX_LOAD_KEY(&iptr[k], kv->key);
                        iptr[k].value.type = PMIX_DIM_VALUE;
                        iptr[k].value.data.dimval = kv->value->data.dimval;
                        kv->value = NULL;
                        ++k;
                    }
                }
                PMIX_LIST_DESTRUCT(&qres);
            } else if (0 == strcmp(queries[n].keys[m], PMIX_STORAGE_AVAIL_BW)) {
                /* ADD HERE:
                 *
                 * Get the bandwidth of the Lustre storage systems that is available to the
                 * caller's userid/grpid. If you need further info, please let me know - e.g.,
                 * we could provide the app's jobid or argv[0]. If they ask for
                 * a specific storage system, then get just that one.
                 */

                /* did they specify a storage ID? */
                sid = NULL;
                for (k=0; k < queries[n].nqual; k++) {
                    if (0 == strcmp(queries[n].qualifiers[m].key, PMIX_STORAGE_ID)) {
                        sid = queries[n].qualifiers[m].value.data.string;
                        break;
                    }
                }
                PMIX_CONSTRUCT(&qres, pmix_list_t);
                for (k=0; NULL != availsys[k].name; k++) {
                    if (NULL == sid || 0 == strcmp(sid, availsys[k].name)) {
                        kv = PMIX_NEW(pmix_kval_t);
                        kv->key = strdup(availsys[k].name);
                        kv->value = (pmix_value_t*)malloc(sizeof(pmix_value_t));
                        kv->value->type = PMIX_DIM_VALUE;
                        kv->value->data.dimval = (pmix_dim_value_t*)malloc(sizeof(pmix_dim_value_t));
                        PMIX_DIM_VALUE_LOAD(kv->value->data.dimval, availsys[k].availbw.dval, availsys[k].availbw.units);
                        pmix_list_append(&qres, &kv->super);
                    }
                }
                nvals = pmix_list_get_size(&qres);
                if (0 < nvals) {
                    kv = PMIX_NEW(pmix_kval_t);
                    kv->key = strdup(PMIX_STORAGE_AVAIL_BW);
                    kv->value = (pmix_value_t*)malloc(sizeof(pmix_value_t));
                    PMIX_DATA_ARRAY_CREATE(darray, nvals, PMIX_INFO);
                    kv->value->type = PMIX_DATA_ARRAY;
                    kv->value->data.darray = darray;
                    pmix_list_append(results, &kv->super);
                    iptr = (pmix_info_t*)darray->array;
                    k = 0;
                    PMIX_LIST_FOREACH(kv, &qres, pmix_kval_t) {
                        PMIX_LOAD_KEY(&iptr[k], kv->key);
                        iptr[k].value.type = PMIX_DIM_VALUE;
                        iptr[k].value.data.dimval = kv->value->data.dimval;
                        kv->value = NULL;
                        ++k;
                    }
                }
                PMIX_LIST_DESTRUCT(&qres);
            }
        }
    }
    return PMIX_OPERATION_SUCCEEDED;
}
