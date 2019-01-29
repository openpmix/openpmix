/*
 * Copyright (c) 2004-2010 The Trustees of Indiana University.
 *                         All rights reserved.
 * Copyright (c) 2019      Intel, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "pmix_config.h"

#include <string.h>

#include "pmix_common.h"
#include "src/mca/mca.h"
#include "src/mca/base/base.h"
#include "src/mca/compress/compress.h"
#include "src/mca/compress/base/base.h"

int pmix_compress_base_close(void)
{
    /* Call the component's finalize routine */
    if( NULL != pmix_compress.finalize ) {
        pmix_compress.finalize();
    }

    /* Close all available modules that are open */
    return pmix_mca_base_framework_components_close (&pmix_compress_base_framework, NULL);
}
