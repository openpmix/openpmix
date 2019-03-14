/*
 * Copyright (c) 2019      IBM Corporation.  All rights reserved.
 * Copyright (c) 2019      Mellanox Technologies, Inc.
 *                         All rights reserved.
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
#include "psquash_native.h"

#include "src/mca/bfrops/base/base.h"

static pmix_status_t native_init(void);

static void native_finalize(void);

pmix_psquash_base_module_t pmix_psquash_native_module = {
    .name = "native",
    .init = native_init,
    .finalize = native_finalize,
    .encode_int = NULL,
    .decode_int = NULL
};


static pmix_status_t native_init(void)
{
    pmix_output_verbose(2, pmix_globals.debug_output,
                        "psquash: native init");
    return PMIX_SUCCESS;
}

static void native_finalize(void)
{
    pmix_output_verbose(2, pmix_globals.debug_output,
                        "psquash: native finalize");
}
