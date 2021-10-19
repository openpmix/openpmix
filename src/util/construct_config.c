/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2013 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2012-2017 Los Alamos National Security, LLC. All rights
 *                         reserved.
 * Copyright (c) 2012-2015 Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2015-2017 Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2016-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2017      IBM Corporation. All rights reserved.
 * Copyright (c) 2021      Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "pmix_config.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "src/class/pmix_list.h"
#include "src/class/pmix_object.h"
#include "src/include/frameworks.h"
#include "src/include/pmix_portable_platform.h"
#include "src/mca/base/pmix_mca_base_component_repository.h"
#include "src/mca/pinstalldirs/pinstalldirs.h"
#include "src/util/printf.h"

#include "include/pmix_common.h"
#include "src/util/construct_config.h"

/* Return a pmix_data_array_t of key-value pairs corresponding
 * to what pmix_info presents */
pmix_status_t pmix_util_construct_config(pmix_value_t **val,
                                         pmix_get_logic_t *lg)
{
    void *list;
    pmix_status_t rc;
    char *tmp;
    pmix_data_array_t *darray;
    pmix_value_t *ival;

    PMIX_INFO_LIST_START(list);

    PMIX_INFO_LIST_ADD(rc, list, "Package", PMIX_PACKAGE_STRING, PMIX_STRING);

    if (NULL != PMIX_GREEK_VERSION) {
        pmix_asprintf(&tmp, "%d.%d.%d%s",
                      PMIX_MAJOR_VERSION,
                      PMIX_MINOR_VERSION,
                      PMIX_RELEASE_VERSION,
                      PMIX_GREEK_VERSION);
    } else {
        pmix_asprintf(&tmp, "%d.%d.%d",
                      PMIX_MAJOR_VERSION,
                      PMIX_MINOR_VERSION,
                      PMIX_RELEASE_VERSION);
    }
    PMIX_INFO_LIST_ADD(rc, list, "PMIx", tmp, PMIX_STRING);
    free(tmp);

    PMIX_INFO_LIST_ADD(rc, list, "PMIx repo revision", PMIX_REPO_REV, PMIX_STRING);
    PMIX_INFO_LIST_ADD(rc, list, "PMIx release date", PMIX_RELEASE_DATE, PMIX_STRING);
    PMIX_INFO_LIST_ADD(rc, list, "Prefix", pmix_pinstall_dirs.prefix, PMIX_STRING);
    PMIX_INFO_LIST_ADD(rc, list, "Configured architecture", PMIX_ARCH_STRING, PMIX_STRING);
    PMIX_INFO_LIST_ADD(rc, list, "Configure host", PMIX_CONFIGURE_HOST, PMIX_STRING);

    darray = (pmix_data_array_t*)malloc(sizeof(pmix_data_array_t));
    PMIX_INFO_LIST_CONVERT(rc, list, darray);
    PMIX_INFO_LIST_RELEASE(list);

    PMIX_VALUE_CREATE(ival, 1);
    if (NULL == ival) {
        return PMIX_ERR_NOMEM;
    }
    ival->type = PMIX_DATA_ARRAY;
    ival->data.darray = darray;
    *val = ival;
    return PMIX_OPERATION_SUCCEEDED;
}
