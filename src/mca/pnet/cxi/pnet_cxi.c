/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
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

#include "src/mca/pnet/base/base.h"
#include "src/mca/pnet/pnet.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_environ.h"
#include "src/util/pmix_name_fns.h"
#include "src/util/pmix_output.h"

#include "pnet_cxi.h"

static pmix_status_t setup_fork(const pmix_proc_t *proc, char ***env);

pmix_pnet_module_t pmix_pnet_cxi_module = {
    .name = "cxi",
    .setup_fork = setup_fork
};

/* HPE Slingshot (Cassini) NICs.
 *
 * The vendor id is Cray's, 0x17db, which is what the Cassini parts are
 * expected to report.  That has not yet been confirmed against a topology
 * taken from a real Slingshot node, and should be before this is relied
 * on.  The class is left open; the NIC is a network device either way. */
const pmix_pnet_pcimatch_t pmix_pnet_cxi_match[] = {
    {0x17db, 0, PMIX_DEVTYPE_NETWORK}           /* Cray/HPE Cassini */
};
const size_t pmix_pnet_cxi_nmatch = sizeof(pmix_pnet_cxi_match) / sizeof(pmix_pnet_cxi_match[0]);

/* Is this a name FI_CXI_DEVICE_NAME accepts?
 *
 * The CXI provider validates every entry and refuses the variable if any
 * fails (cxip_env_validate_device_name() in prov/cxi/src/cxip_info.c): an
 * entry must be "cxi" followed by a number from 0 to 99, written without a
 * leading zero.  This is the same test, so that we never hand it a value
 * that stops it starting. */
static bool is_cxi_name(const char *name)
{
    size_t len;

    if (NULL == name || 0 != strncmp(name, "cxi", 3)) {
        return false;
    }
    len = strlen(name);
    if (4 == len) {
        return ('0' <= name[3] && '9' >= name[3]);
    }
    if (5 == len) {
        return ('1' <= name[3] && '9' >= name[3] && '0' <= name[4] && '9' >= name[4]);
    }
    return false;
}

/* Name this process's Slingshot NICs to libfabric's CXI provider.
 *
 * The selector is the name of the function's CXI device, which the
 * enumerator prefers over the hsnN interface on the same function - but
 * only hwloc 2.14 and later report a CXI device at all.  Under an older
 * hwloc the selector is the interface name, and so it is when the process
 * was mapped against a device named by its interface.  The provider would
 * refuse such a value outright, and deriving "cxi0" from "hsn0" would be a
 * guess - the two usually correspond, but nothing guarantees it - so the
 * variable is left unset unless every name is one it accepts.
 *
 * Overwritten if already set, as the other components' variables are: a
 * process mapped against a device made the more specific request. */
static pmix_status_t setup_fork(const pmix_proc_t *proc, char ***env)
{
    pmix_status_t rc;
    char *val = NULL;
    char **names;
    int n;

    rc = pmix_pnet_base_get_assigned_devices(proc, pmix_pnet_cxi_match, pmix_pnet_cxi_nmatch,
                                             &val);
    if (PMIX_SUCCESS != rc) {
        return rc;
    }
    names = PMIx_Argv_split(val, ',');
    if (NULL == names) {
        free(val);
        return PMIX_ERR_NOMEM;
    }
    for (n = 0; NULL != names[n]; n++) {
        if (!is_cxi_name(names[n])) {
            pmix_output_verbose(2, pmix_pnet_base_framework.framework_output,
                                "pnet:cxi: not setting FI_CXI_DEVICE_NAME for %s - %s is "
                                "not a CXI device name (an hwloc older than 2.14 reports none)",
                                PMIX_NAME_PRINT(proc), names[n]);
            PMIx_Argv_free(names);
            free(val);
            return PMIX_ERR_TAKE_NEXT_OPTION;
        }
    }
    PMIx_Argv_free(names);

    pmix_output_verbose(2, pmix_pnet_base_framework.framework_output,
                        "pnet: FI_CXI_DEVICE_NAME=%s for %s", val, PMIX_NAME_PRINT(proc));
    rc = PMIx_Setenv("FI_CXI_DEVICE_NAME", val, true, env);
    free(val);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
    }
    return rc;
}
