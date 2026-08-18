/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2007-2008 Cisco Systems, Inc.  All rights reserved.
 *
 * Copyright (c) 2015-2018 Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2018-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
 * Copyright (c) 2022      Triad National Security, LLC. All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/**
 * @file
 *
 * This interface is for use by PMIx  to obtain topology-related info
 *
 */

#ifndef PMIX_HWLOC_H
#define PMIX_HWLOC_H

#include "src/include/pmix_config.h"

#include <hwloc.h>

#include "pmix_common.h"

#include "src/class/pmix_list.h"
#include "src/include/pmix_globals.h"
#include "src/mca/base/pmix_mca_base_framework.h"
#include "src/mca/base/pmix_mca_base_var.h"
#include "src/mca/mca.h"
#include "src/server/pmix_server_ops.h"

BEGIN_C_DECLS

/**
 * Register params
 */
PMIX_EXPORT pmix_status_t pmix_hwloc_register(void);

/**
 * Finalize. Tear down any allocated storage
 */
PMIX_EXPORT void pmix_hwloc_finalize(void);

/* Setup the topology for delivery to clients */
PMIX_EXPORT pmix_status_t pmix_hwloc_setup_topology(pmix_info_t *info, size_t ninfo);

/* Load the topology */
PMIX_EXPORT pmix_status_t pmix_hwloc_load_topology(pmix_topology_t *topo);

/* Generate the string representation of a cpuset */
PMIX_EXPORT pmix_status_t pmix_hwloc_generate_cpuset_string(const pmix_cpuset_t *cpuset,
                                                            char **cpuset_string);

/* get cpuset from its string representation */
PMIX_EXPORT pmix_status_t pmix_hwloc_parse_cpuset_string(const char *cpuset_string, pmix_cpuset_t *cpuset);

/* Get locality string */
PMIX_EXPORT pmix_status_t pmix_hwloc_generate_locality_string(const pmix_cpuset_t *cpuset, char **loc);

/* Get relative locality */
PMIX_EXPORT pmix_status_t pmix_hwloc_get_relative_locality(const char *locality1,
                                                           const char *locality2,
                                                           pmix_locality_t *loc);

/* Get current bound location */
PMIX_EXPORT pmix_status_t pmix_hwloc_get_cpuset(pmix_cpuset_t *cpuset, pmix_bind_envelope_t ref);

/* Get distance array */
PMIX_EXPORT pmix_status_t pmix_hwloc_compute_distances(pmix_topology_t *topo, pmix_cpuset_t *cpuset,
                                                       pmix_info_t info[], size_t ninfo,
                                                       pmix_device_distance_t **dist, size_t *ndist);

/* One device found in a topology.
 *
 * "dev" is the identity an application sees - the same uuid and osname
 * reported through PMIX_DEVICE_DISTANCES, so a caller that assigns a device
 * to a process can name it in terms the process will recognize.  The other
 * two fields describe where the device sits, which is what a caller placing
 * processes needs and what a distance array cannot express.
 */
typedef struct {
    pmix_device_t dev;
    /* PCI bus id ("0000:06:00.0"), or NULL for a device with no PCI
     * ancestor.  This is the sort key: see pmix_hwloc_get_devices(). */
    char *busid;
    /* The vendor's own identifier for this device - an NVIDIA
     * "GPU-<uuid>", an AMD uuid, a Level Zero uuid - or NULL when the
     * topology carries none.
     *
     * This is the only handle a vendor runtime accepts for naming a
     * specific device, so it is what makes an assignment actionable rather
     * than merely reportable: dev.uuid identifies the device within PMIx,
     * but no GPU library has ever heard of it.
     *
     * It is NULL far more often than not.  hwloc records it only from its
     * vendor backends (NVML, RSMI, Level Zero), so a topology gathered by
     * an hwloc built without them describes the same hardware with no way
     * to name any of it.  Whether that is fatal is the caller's policy
     * question, not this layer's - but note that whether the field CAN be
     * filled is a property of the topology rather than of the node, since
     * an absent attribute changes the topology's shape.  Its VALUE is
     * per-node and must be read from that node's own topology. */
    char *vendor_id;
    /* Which vendor's grammar vendor_id is written in - "NVIDIA", "AMD",
     * "INTEL" - or NULL whenever vendor_id is.
     *
     * Needed because vendor_id alone does not say who will accept it, and
     * a node may carry cards from more than one vendor.  Each vendor wants
     * its own environment variable and its own value syntax, so whoever
     * acts on an assignment has to be able to pick out the devices that
     * are theirs.  Deriving it from the identity's spelling would be a
     * guess; this is the key hwloc actually recorded it under. */
    char *vendor;
    /* What to write in the device-selection variable of the software that
     * will use this device, to name it to a process - CUDA_VISIBLE_DEVICES,
     * ROCR_VISIBLE_DEVICES, ZE_AFFINITY_MASK for a GPU; UCX_NET_DEVICES,
     * NCCL_IB_HCA, PSM3_NIC for a NIC - or NULL when the topology does not
     * support saying it.  Several devices are named by joining their
     * selectors with ','.
     *
     * For a network or OpenFabrics device it is the OS device name, which
     * is what every one of those variables accepts and what the enumerator
     * already chose to name the PCI function by (see the note on
     * pmix_hwloc_get_devices() about which of a function's OS devices
     * wins).  There is nothing to be missing here and no vendor backend
     * involved, so unlike a GPU's it is never NULL.
     *
     * Separate from vendor_id because the two coincide only for the vendors
     * whose variable accepts an identity.  NVIDIA's and AMD's do, so their
     * selector IS the identity.  Intel's does not: ZE_AFFINITY_MASK takes
     * Level Zero device ordinals, so the selector is the ordinal the driver
     * itself reported for this device - a different string, in a grammar
     * that says nothing about which device it is, only where it sat in one
     * enumeration.
     *
     * That last point is the reason a caller must not treat this as an
     * identity: an ordinal is meaningful only against the enumeration it
     * came from, and for Level Zero that enumeration depends on the device
     * hierarchy model in force (see pmix_hwloc_levelzero_hierarchy()).
     * NULL where an identity exists but cannot be turned into a selector,
     * which is honest rather than a failure: a wrong value in these
     * variables does not error, it silently narrows what the process can
     * see. */
    char *selector;
    /* The PCI vendor and class ids of the function this device hangs off,
     * or zero for a device with no PCI ancestor.  Read straight from the
     * topology, not derived.
     *
     * They are here because for a NIC they are the only discriminator
     * there is.  A GPU carries its vendor in the identity hwloc recorded,
     * so a component acting on a GPU assignment can pick out its own by
     * the "vendor" field above; a NIC carries no such attribute, and its
     * OS device name says nothing about who made it.  The (vendor, class)
     * pair is the same one a component passes to
     * pmix_hwloc_check_vendor() to decide whether to run at all, so a
     * component that opened on 0x15b3/0x207 can select exactly the
     * devices it opened for - which matters on a node carrying more than
     * one fabric, where naming another vendor's NIC in your variable is
     * worse than naming none. */
    uint16_t pci_vendor;
    uint16_t pci_class;
    /* Nearest ancestor carrying a cpuset - the set of PUs local to this
     * device.  Borrowed from the topology, so it is valid only as long as
     * the topology is, and must not be freed. */
    hwloc_obj_t locality;
} pmix_hwloc_device_t;

/* Enumerate the devices of the given type(s) in a topology.
 *
 * The unit is the PCI *function*, not the OS device: a GPU commonly exposes
 * several OS devices (a DRM card node, a render node, and a vendor compute
 * node such as "cuda0" or "rsmi0") and they are one device, not three.  So
 * does an HCA, which shows up as both an OpenFabrics device ("mlx5_0") and
 * a network interface ("ib0").
 *
 * Where a function has several, the one that names it is the one an
 * application is most likely to recognize and be able to act on: a vendor
 * compute node over a render node over a card node for a GPU, and the
 * OpenFabrics device over the network interface for an HCA - the fabric
 * libraries' device-selection variables take the former and none of them
 * takes the latter.  A caller that asked for only one of the two types
 * naturally gets that one, since the other was never a candidate.
 *
 * Devices come back ordered by PCI bus id ascending, with any device having
 * no PCI ancestor last, ordered by name.  That ordering is deterministic,
 * identical on every node carrying the same hardware, and stable across
 * reboots - which matters because a caller assigning devices to processes
 * has to make the same assignment everywhere.
 *
 * "hostname" is the node the topology describes, and is REQUIRED - a NULL
 * is PMIX_ERR_BAD_PARAM rather than a convenience default.  A device's uuid
 * names the node it lives on, so producing one means saying which node that
 * is, and the answer is not "wherever this code happens to be running": a
 * caller reading another node's topology (a mapper on the head node, say)
 * would otherwise stamp its own hostname on every device in the job, and
 * the process that later computes the same uuid locally would not match it.
 * Pass pmix_globals.hostname only when the topology really is this node's.
 *
 * "type" is a bitmask of the desired types; PMIX_DEVTYPE_UNKNOWN means all.
 * "devid" restricts the result to a single device matching it by osname or
 * uuid, and may be NULL.
 *
 * An empty result is not an error: the caller decides what "this node has no
 * such device" means.  Release the array with pmix_hwloc_release_devices().
 */
PMIX_EXPORT pmix_status_t pmix_hwloc_get_devices(pmix_topology_t *topo,
                                                 const char *hostname,
                                                 pmix_device_type_t type,
                                                 const char *devid,
                                                 pmix_hwloc_device_t **devs,
                                                 size_t *ndevs);

PMIX_EXPORT void pmix_hwloc_release_devices(pmix_hwloc_device_t *devs, size_t ndevs);

/* Which Level Zero device hierarchy model this topology's GPUs were
 * enumerated under: "COMPOSITE" if the driver reported each card as one
 * device carrying its tiles as sub-devices, or "FLAT" if it reported the
 * tiles themselves as devices.  The strings are the values
 * ZE_FLAT_DEVICE_HIERARCHY takes, because the only use for the answer is
 * to state the model an ordinal was computed under.
 *
 * Returns PMIX_ERR_TAKE_NEXT_OPTION with *mode NULL when the topology
 * cannot tell them apart, which happens exactly when they do not differ -
 * no Level Zero device in it has more than one tile, so every model
 * enumerates the same devices in the same order.  Callers must therefore
 * treat "cannot tell" as "does not matter" rather than as an error.
 *
 * Note this reports the model in force where and when the topology was
 * gathered.  Since it is read to accompany ordinals drawn from that same
 * topology, that is the model those ordinals mean something in.
 *
 * The caller frees *mode.
 */
PMIX_EXPORT pmix_status_t pmix_hwloc_levelzero_hierarchy(pmix_topology_t *topo,
                                                         char **mode);

/* Does this topology carry a PCI device from this vendor, of exactly this
 * class/subclass?  PMIX_ERR_NOT_AVAILABLE when it does not, and
 * PMIX_ERR_TAKE_NEXT_OPTION when the topology is not hwloc-sourced and so
 * cannot answer.
 *
 * Use this only where the subclass is itself the question - a fabric
 * component distinguishing an InfiniBand controller (0x0207) from a
 * fabric one (0x0208) does. Asking "is this vendor's GPU here?" is not
 * that question: see below. */
PMIX_EXPORT pmix_status_t pmix_hwloc_check_vendor(pmix_topology_t *topo,
                                                  unsigned short vendorID,
                                                  uint16_t class);

/* The same, matching the PCI *base* class and ignoring the subclass.
 *
 * This is the right form for "does this node have this vendor's GPU?",
 * because one vendor's GPUs do not agree on a subclass: within the
 * display base class 0x03, a card may report 0x0300 (VGA compatible),
 * 0x0302 (3D controller) or 0x0380 (other) depending on the part and on
 * whether a display is wired to it. Matching one of those exactly means
 * declining on hardware that is plainly present - and a component that
 * believes the hardware is absent looks exactly like one that is right
 * about it, so the failure is silent. */
PMIX_EXPORT pmix_status_t pmix_hwloc_check_vendor_baseclass(pmix_topology_t *topo,
                                                            unsigned short vendorID,
                                                            uint8_t baseclass);

/* cpuset pack/unpack/copy/print functions */
PMIX_EXPORT pmix_status_t pmix_hwloc_pack_cpuset(pmix_buffer_t *buf, pmix_cpuset_t *src,
                                                 pmix_pointer_array_t *regtypes);

PMIX_EXPORT pmix_status_t pmix_hwloc_unpack_cpuset(pmix_buffer_t *buf, pmix_cpuset_t *dest,
                                                   pmix_pointer_array_t *regtypes);

PMIX_EXPORT pmix_status_t pmix_hwloc_copy_cpuset(pmix_cpuset_t *dest, pmix_cpuset_t *src);

PMIX_EXPORT char *pmix_hwloc_print_cpuset(pmix_cpuset_t *src);

PMIX_EXPORT void pmix_hwloc_destruct_cpuset(pmix_cpuset_t *cpuset);

PMIX_EXPORT void pmix_hwloc_release_cpuset(pmix_cpuset_t *ptr, size_t sz);

PMIX_EXPORT pmix_status_t pmix_hwloc_get_cpuset_size(pmix_cpuset_t *ptr, size_t *sz);

/* topology pack/unpack/copy/print functions */
PMIX_EXPORT pmix_status_t pmix_hwloc_pack_topology(pmix_buffer_t *buf, pmix_topology_t *src,
                                                   pmix_pointer_array_t *regtypes);

PMIX_EXPORT pmix_status_t pmix_hwloc_unpack_topology(pmix_buffer_t *buf, pmix_topology_t *dest,
                                                     pmix_pointer_array_t *regtypes);

PMIX_EXPORT pmix_status_t pmix_hwloc_copy_topology(pmix_topology_t *dest, pmix_topology_t *src);

PMIX_EXPORT char *pmix_hwloc_print_topology(pmix_topology_t *src);

PMIX_EXPORT void pmix_hwloc_destruct_topology(pmix_topology_t *ptr);

PMIX_EXPORT void pmix_hwloc_release_topology(pmix_topology_t *ptr, size_t sz);

PMIX_EXPORT pmix_status_t pmix_hwloc_get_topology_size(pmix_topology_t *ptr, size_t *sz);

/****  PRESERVE ABI  ****/
PMIX_EXPORT void pmix_ploc_base_destruct_cpuset(pmix_cpuset_t *cpuset);
PMIX_EXPORT void pmix_ploc_base_release_cpuset(pmix_cpuset_t *ptr, size_t sz);
PMIX_EXPORT void pmix_ploc_base_destruct_topology(pmix_topology_t *ptr);
PMIX_EXPORT void pmix_ploc_base_release_topology(pmix_topology_t *ptr, size_t sz);

END_C_DECLS

#endif
