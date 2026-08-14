/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2007-2008 Cisco Systems, Inc.  All rights reserved.
 *
 * Copyright (c) 2015-2018 Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2018-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2021-2024 Nanook Consulting  All rights reserved.
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
    /* Nearest ancestor carrying a cpuset - the set of PUs local to this
     * device.  Borrowed from the topology, so it is valid only as long as
     * the topology is, and must not be freed. */
    hwloc_obj_t locality;
} pmix_hwloc_device_t;

/* Enumerate the devices of the given type(s) in a topology.
 *
 * The unit is the PCI *function*, not the OS device: a GPU commonly exposes
 * several OS devices (a DRM card node, a render node, and a vendor compute
 * node such as "cuda0" or "rsmi0") and they are one device, not three.
 *
 * Devices come back ordered by PCI bus id ascending, with any device having
 * no PCI ancestor last, ordered by name.  That ordering is deterministic,
 * identical on every node carrying the same hardware, and stable across
 * reboots - which matters because a caller assigning devices to processes
 * has to make the same assignment everywhere.
 *
 * "type" is a bitmask of the desired types; PMIX_DEVTYPE_UNKNOWN means all.
 * "devid" restricts the result to a single device matching it by osname or
 * uuid, and may be NULL.
 *
 * An empty result is not an error: the caller decides what "this node has no
 * such device" means.  Release the array with pmix_hwloc_release_devices().
 */
PMIX_EXPORT pmix_status_t pmix_hwloc_get_devices(pmix_topology_t *topo,
                                                 pmix_device_type_t type,
                                                 const char *devid,
                                                 pmix_hwloc_device_t **devs,
                                                 size_t *ndevs);

PMIX_EXPORT void pmix_hwloc_release_devices(pmix_hwloc_device_t *devs, size_t ndevs);

PMIX_EXPORT pmix_status_t pmix_hwloc_check_vendor(pmix_topology_t *topo,
                                                  unsigned short vendorID,
                                                  uint16_t class);

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
