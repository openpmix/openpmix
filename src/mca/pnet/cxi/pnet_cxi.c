/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "src/include/pmix_config.h"

#include <dirent.h>
#include <stdio.h>
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
 * Cassini reports Cray's vendor id, 0x17db (device 0x0501, class 0x200 on
 * an HPE Cray EX235n - test/topologies/slingshot.xml).  The class is left
 * open; the NIC is a network device either way. */
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

/* The CXI device of the Slingshot interface "netdev", from sysfs.
 *
 * hwloc 2.14 and later report it beside the interface, and the enumerator
 * then names the function by it.  An older hwloc reports only the
 * interface, so the name has to be found where hwloc 2.14 itself finds it:
 * the interface's PCI device carries a "cxi" class directory,
 * <root>/class/net/<hsnN>/device/cxi/, whose entry is the CXI device.
 * This runs on the node that will fork the process, so that is the tree
 * describing the NIC the process will see.
 *
 * Exactly one entry of the form cxiN is an answer.  None, more than one,
 * or a directory that cannot be read is not, and the caller declines: the
 * number in "hsn0" says nothing about which CXI device it is, and is never
 * used to make one up.  The root of the tree is a parameter only so this
 * can be run against a captured one.  Returns a new string, or NULL. */
static char *cxi_from_sysfs(const char *netdev)
{
    const char *root = pmix_mca_pnet_cxi_component.sysfs_root;
    char path[PMIX_PATH_MAX];
    struct dirent *ent;
    char *found = NULL;
    DIR *dir;
    int len, n = 0;

    /* the name becomes a path component, so it must be one */
    if (NULL == netdev || '\0' == netdev[0] || NULL != strchr(netdev, '/')
        || 0 == strcmp(netdev, ".") || 0 == strcmp(netdev, "..")) {
        return NULL;
    }
    if (NULL == root || '\0' == root[0]) {
        root = "/sys";
    }
    len = snprintf(path, sizeof(path), "%s/class/net/%s/device/cxi", root, netdev);
    if (0 > len || (size_t) len >= sizeof(path)) {
        return NULL;
    }
    dir = opendir(path);
    if (NULL == dir) {
        pmix_output_verbose(2, pmix_pnet_base_framework.framework_output,
                            "pnet:cxi: no CXI device found for %s - %s cannot be read",
                            netdev, path);
        return NULL;
    }
    while (NULL != (ent = readdir(dir))) {
        if (!is_cxi_name(ent->d_name)) {
            continue;
        }
        ++n;
        if (1 < n) {
            break;
        }
        found = strdup(ent->d_name);
    }
    closedir(dir);
    if (1 != n || NULL == found) {
        pmix_output_verbose(2, pmix_pnet_base_framework.framework_output,
                            "pnet:cxi: no CXI device found for %s - %s holds %s",
                            netdev, path, (0 == n) ? "none" : "more than one");
        free(found);
        return NULL;
    }
    return found;
}

/* Name this process's Slingshot NICs to libfabric's CXI provider.
 *
 * The selector is the name of the function's CXI device when hwloc 2.14 or
 * later reported one, since the enumerator prefers it over the hsnN
 * interface on the same function.  Otherwise - an older hwloc, or a process
 * mapped against a device named by its interface - it is hsnN, and the CXI
 * device is looked up for it in sysfs; see cxi_from_sysfs().  The provider
 * refuses the whole variable if any entry is not a CXI device name, so if
 * any NIC cannot be resolved the variable is left unset rather than
 * written short.
 *
 * Overwritten if already set, as the other components' variables are: a
 * process mapped against a device made the more specific request. */
static pmix_status_t setup_fork(const pmix_proc_t *proc, char ***env)
{
    pmix_status_t rc;
    char *val = NULL, *cxi;
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
        if (is_cxi_name(names[n])) {
            continue;
        }
        cxi = cxi_from_sysfs(names[n]);
        if (NULL == cxi) {
            pmix_output_verbose(2, pmix_pnet_base_framework.framework_output,
                                "pnet:cxi: not setting FI_CXI_DEVICE_NAME for %s - %s is "
                                "not a CXI device and none was found for it",
                                PMIX_NAME_PRINT(proc), names[n]);
            PMIx_Argv_free(names);
            free(val);
            return PMIX_ERR_TAKE_NEXT_OPTION;
        }
        free(names[n]);
        names[n] = cxi;
    }
    free(val);
    val = PMIx_Argv_join(names, ',');
    PMIx_Argv_free(names);
    if (NULL == val) {
        return PMIX_ERR_NOMEM;
    }

    pmix_output_verbose(2, pmix_pnet_base_framework.framework_output,
                        "pnet: FI_CXI_DEVICE_NAME=%s for %s", val, PMIX_NAME_PRINT(proc));
    rc = PMIx_Setenv("FI_CXI_DEVICE_NAME", val, true, env);
    free(val);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
    }
    return rc;
}
