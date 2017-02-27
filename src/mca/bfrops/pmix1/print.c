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
 * Copyright (c) 2014-2017 Intel, Inc.  All rights reserved.
 * Copyright (c) 2016      Mellanox Technologies, Inc.
 *                         All rights reserved.
 *
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include <src/include/pmix_config.h>

#include <src/include/pmix_stdint.h>

#include <stdio.h>
#ifdef HAVE_TIME_H
#include <time.h>
#endif

#include "src/util/error.h"

#include "src/include/pmix_globals.h"
#include "src/mca/bfrops/base/base.h"
#include "bfrop_pmix1.h"

pmix_status_t pmix1_print(char **output, char *prefix,
                           void *src, pmix_data_type_t type)
{

    /* there are a few types we have to handle ourselves */
    switch(type) {
        case PMIX_INFO:
            return pmix1_print_info(output, prefix, src, type);
            break;

        case PMIX_PDATA:
            return pmix1_print_pdata(output, prefix, src, type);
            break;

        case PMIX_APP:
            return pmix1_print_app(output, prefix, src, type);
            break;

        case PMIX_KVAL:
            return pmix1_print_kval(output, prefix, src, type);
            break;

        case PMIX_INFO_ARRAY:
            return pmix1_print_array(output, prefix, src, type);
            break;

        default:
            /* let the standard methods do it */
            return pmix_bfrops_base_print(&mca_bfrops_pmix1_component.types,
                                          output, prefix, src, type);
    }
}


pmix_status_t pmix1_print_info(char **output, char *prefix,
                                pmix_info_t *src, pmix_data_type_t type)
{
    char *tmp=NULL;

    /* must handle arrays ourselves */
    if (PMIX_INFO_ARRAY == src->value.type) {
        pmix1_print_array(&tmp, NULL, src->value.data.array, type);
    } else {
        pmix_bfrops_base_print_value(&tmp, NULL, &src->value, PMIX_VALUE);
    }
    asprintf(output, "%sKEY: %s\n%s\t%s\n", prefix, src->key, prefix, tmp);
    free(tmp);
    return PMIX_SUCCESS;
}

pmix_status_t pmix1_print_pdata(char **output, char *prefix,
                                 pmix_pdata_t *src, pmix_data_type_t type)
{
    char *tmp1, *tmp2;

    pmix_bfrops_base_print_proc(&tmp1, NULL, &src->proc, PMIX_PROC);
    /* must handle arrays ourselves */
    if (PMIX_INFO_ARRAY == src->value.type) {
        pmix1_print_array(&tmp2, NULL, src->value.data.array, type);
    } else {
        pmix_bfrops_base_print_value(&tmp2, NULL, &src->value, PMIX_VALUE);
    }
    asprintf(output, "%s  %s  KEY: %s %s", prefix, tmp1, src->key,
             (NULL == tmp2) ? "NULL" : tmp2);
    if (NULL != tmp1) {
        free(tmp1);
    }
    if (NULL != tmp2) {
        free(tmp2);
    }
    return PMIX_SUCCESS;
}

pmix_status_t pmix1_print_app(char **output, char *prefix,
                               pmix_app_t *src, pmix_data_type_t type)
{
    return PMIX_SUCCESS;
}

pmix_status_t pmix1_print_kval(char **output, char *prefix,
                                pmix_kval_t *src, pmix_data_type_t type)
{
    return PMIX_SUCCESS;
}

pmix_status_t pmix1_print_array(char **output, char *prefix,
                                 pmix_info_array_t *src, pmix_data_type_t type)
{
    size_t j;
    char *tmp, *tmp2, *tmp3, *pfx;
    pmix_info_t *s1;

    asprintf(&tmp, "%sARRAY SIZE: %ld", prefix, (long)src->size);
    asprintf(&pfx, "\n%s\t",  (NULL == prefix) ? "" : prefix);
    s1 = (pmix_info_t*)src->array;

    for (j=0; j < src->size; j++) {
        pmix1_print_info(&tmp2, pfx, &s1[j], PMIX_INFO);
        asprintf(&tmp3, "%s%s", tmp, tmp2);
        free(tmp);
        free(tmp2);
        tmp = tmp3;
    }
    *output = tmp;
    return PMIX_SUCCESS;
}
