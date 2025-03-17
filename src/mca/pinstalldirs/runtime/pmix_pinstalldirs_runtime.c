/*
 * Copyright (c) 2025      NVIDIA Corporation.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "src/include/pmix_config.h"

#include <stdlib.h>
#include <string.h>

#include "pmix_common.h"
#include "src/mca/pinstalldirs/pinstalldirs.h"
#include <dlfcn.h>
#include "src/util/pmix_basename.h"


static void pinstalldirs_runtime_init(pmix_info_t info[], size_t ninfo);

pmix_pinstalldirs_base_component_t pmix_mca_pinstalldirs_runtime_component = {
    /* First, the mca_component_t struct containing meta information
       about the component itself */
    .component = {
        PMIX_PINSTALLDIRS_BASE_VERSION_1_0_0,

         /* Component name and version */
         "runtime", PMIX_MAJOR_VERSION, PMIX_MINOR_VERSION, PMIX_RELEASE_VERSION,
    },

    .install_dirs_data = {
        .prefix = NULL,
        .exec_prefix = NULL,
        .bindir = NULL,
        .sbindir = NULL,
        .libexecdir = NULL,
        .datarootdir = NULL,
        .datadir = NULL,
        .sysconfdir = NULL,
        .sharedstatedir = NULL,
        .localstatedir = NULL,
        .libdir = NULL,
        .includedir = NULL,
        .infodir = NULL,
        .mandir = NULL,
        .pmixdatadir = NULL,
        .pmixlibdir = NULL,
        .pmixincludedir = NULL
    },
    .init = pinstalldirs_runtime_init
};

static void pinstalldirs_runtime_init(pmix_info_t unused[], size_t ninfo)
{
    Dl_info info;
    void* pmix_fct;

    /* Casting from void* to fct pointer according to POSIX.1-2001 and POSIX.1-2008 */
    *(void **)&pmix_fct = dlsym(RTLD_DEFAULT, "pmix_init_util");

    if( 0 != dladdr(pmix_fct, &info) ) {
        char* dname = pmix_dirname(info.dli_fname);
        char* prefix = pmix_dirname(dname);
        free(dname);

        pmix_mca_pinstalldirs_runtime_component.install_dirs_data.prefix = prefix;
    }
}
