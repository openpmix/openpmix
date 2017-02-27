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
 * Copyright (c) 2014-2017 Intel, Inc.  All rights reserved.
 * Copyright (c) 2015      Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include <src/include/pmix_config.h>


#include "src/util/argv.h"
#include "src/util/error.h"
#include "src/util/output.h"
#include "src/include/pmix_globals.h"

#include "src/mca/bfrops/base/base.h"
#include "bfrop_pmix1.h"

pmix_status_t pmix1_copy(void **dest, void *src,
                          pmix_data_type_t type)
{
    /* kick the process off by passing this in to the base */
    return pmix_bfrops_base_copy(&mca_bfrops_pmix1_component.types,
                                 dest, src, type);
}

pmix_status_t pmix1_std_copy(void **dest, void *src,
                              pmix_data_type_t type)
{
    size_t datasize;
    uint8_t *val = NULL;

    /* there are a few types we have to handle ourselves */
    switch(type) {
        case PMIX_PERSIST:
            datasize = sizeof(int);
            break;

        case PMIX_SCOPE:
        case PMIX_DATA_RANGE:
            datasize = sizeof(unsigned int);
            break;

        case PMIX_COMMAND:
        case PMIX_DATA_TYPE:
            datasize = sizeof(uint32_t);
            break;

        default:
            return PMIX_ERR_BAD_PARAM;
    }

    val = (uint8_t*)malloc(datasize);
    if (NULL == val) {
        return PMIX_ERR_OUT_OF_RESOURCE;
    }

    memcpy(val, src, datasize);
    *dest = val;

    return PMIX_SUCCESS;
}
