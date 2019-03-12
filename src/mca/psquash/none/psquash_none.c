/*
 * Copyright (c) 2019      IBM Corporation.  All rights reserved.
 *
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include <src/include/pmix_config.h>

#include <pmix_common.h>

#include "src/include/pmix_socket_errno.h"
#include "src/include/pmix_globals.h"
#include "src/util/argv.h"
#include "src/util/error.h"
#include "src/util/output.h"

#include <unistd.h>
#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif

#include "src/mca/psquash/psquash.h"
#include "psquash_none.h"

#include "src/mca/bfrops/base/base.h"

static pmix_status_t none_init(void);

static void none_finalize(void);

pmix_psquash_base_module_t pmix_none_module = {
    .name = "none",
    .init = none_init,
    .finalize = none_finalize,
    .encode_int = NULL,
    .decode_int = NULL
};


static pmix_status_t none_init(void)
{
    pmix_output_verbose(2, pmix_globals.debug_output,
                        "psquash: none init");
    return PMIX_SUCCESS;
}

static void none_finalize(void)
{
    pmix_output_verbose(2, pmix_globals.debug_output,
                        "psquash: none finalize");
}
