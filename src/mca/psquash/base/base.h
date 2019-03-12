/* -*- C -*-
 *
 * Copyright (c) 2019      IBM Corporation.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 */
#ifndef PMIX_PSQUASH_BASE_H_
#define PMIX_PSQUASH_BASE_H_

#include <src/include/pmix_config.h>

#ifdef HAVE_STRING_H
#include <string.h>
#endif

#include "src/class/pmix_pointer_array.h"
#include "src/mca/mca.h"
#include "src/mca/base/pmix_mca_base_framework.h"

#include "src/mca/psquash/psquash.h"


BEGIN_C_DECLS

PMIX_EXPORT extern pmix_mca_base_framework_t pmix_psquash_base_framework;

PMIX_EXPORT pmix_status_t pmix_psquash_base_select(void);

END_C_DECLS

#endif
