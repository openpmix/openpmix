/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "src/include/pmix_config.h"

#include "pmix_common.h"

#include "src/mca/pnet/base/base.h"
#include "src/mca/pnet/pnet.h"

#include "pnet_efa.h"

static pmix_status_t setup_fork(const pmix_proc_t *proc, char ***env);

pmix_pnet_module_t pmix_pnet_efa_module = {
    .name = "efa",
    .setup_fork = setup_fork
};

/* Amazon's Elastic Fabric Adapter.
 *
 * Amazon makes two network devices, and they report the same vendor: EFA,
 * which the kernel registers as a verbs device and hwloc therefore reports
 * as an OpenFabrics device, and ENA, a plain ethernet interface.  The type
 * is what tells them apart, so the entry requires it - and leaves the class
 * open, since the type already says everything the class would. */
const pmix_pnet_pcimatch_t pmix_pnet_efa_match[] = {
    {0x1d0f, 0, PMIX_DEVTYPE_OPENFABRICS}       /* Amazon EFA */
};
const size_t pmix_pnet_efa_nmatch = sizeof(pmix_pnet_efa_match) / sizeof(pmix_pnet_efa_match[0]);

/* Name this process's EFA devices to libfabric.
 *
 * The efa provider restricts itself to the devices listed in
 * FI_EFA_IFACE, a comma-separated list of verbs device names that it
 * compares exactly, entry by entry, against each device's name - so unlike
 * the verbs provider's variable it cannot select more than it names.  The
 * selector is that verbs device name ("rdmap16s27"), which is what hwloc
 * calls the OpenFabrics device.
 *
 * Every libfabric user in the process sees the restriction, since it is
 * applied inside the provider.  That includes MPI libraries that choose
 * their NICs on their own, which is the point: the process uses the
 * devices it was placed against.
 *
 * Overwritten if already set, as the other components' variables are: a
 * process mapped against a device made the more specific request. */
static pmix_status_t setup_fork(const pmix_proc_t *proc, char ***env)
{
    return pmix_pnet_base_set_assigned_devices(proc, pmix_pnet_efa_match, pmix_pnet_efa_nmatch,
                                               "FI_EFA_IFACE", env);
}
