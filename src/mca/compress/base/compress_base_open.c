/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2004-2010 The Trustees of Indiana University.
 *                         All rights reserved.
 * Copyright (c) 2011-2013 Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2015      Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2019      Intel, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "pmix_config.h"

#include "src/mca/base/base.h"
#include "src/mca/compress/base/base.h"

#include "src/mca/compress/base/static-components.h"

/*
 * Globals
 */
pmix_compress_base_module_t pmix_compress = {
    NULL, /* init             */
    NULL, /* finalize         */
    NULL, /* compress         */
    NULL, /* compress_nb      */
    NULL, /* decompress       */
    NULL  /* decompress_nb    */
};

pmix_compress_base_component_t pmix_compress_base_selected_component = {{0}};

static int pmix_compress_base_register(pmix_mca_base_register_flag_t flags);

PMIX_MCA_BASE_FRAMEWORK_DECLARE(pmix, compress, "COMPRESS",
                                pmix_compress_base_register, pmix_compress_base_open,
                                pmix_compress_base_close, mca_compress_base_static_components, 0);

static int pmix_compress_base_register(pmix_mca_base_register_flag_t flags)
{
    return PMIX_SUCCESS;
}

/**
 * Function for finding and opening either all MCA components,
 * or the one that was specifically requested via a MCA parameter.
 */
int pmix_compress_base_open(pmix_mca_base_open_flag_t flags)
{
    /* Open up all available components */
    return pmix_mca_base_framework_components_open(&pmix_compress_base_framework, flags);
}
