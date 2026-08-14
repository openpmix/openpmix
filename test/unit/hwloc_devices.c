/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/*
 * Unit tests for pmix_hwloc_get_devices().
 *
 * Device enumeration is a pure function of a topology plus a type, so it is
 * fully testable against the XML topologies in PMIX_TEST_TOPO_DIR with no
 * server, no client and no real hardware.
 *
 * What the two shipped topologies are good for:
 *
 *   test-topo2 carries the case the whole design turns on.  One PCI
 *   function (0000:63:00.0) exposes THREE OS devices - a DRM card node, a
 *   DRM render node, and the vendor compute node "rsmi0" - and they are one
 *   GPU, not three.  A second function (0000:02:00.0) is a plain display
 *   controller with only a card node: hwloc reports it with the same osdev
 *   type as the real GPU, and it must not be counted.  So "how many GPUs?"
 *   has one right answer here, 1, and three plausible wrong ones - 2, 3 and
 *   4.
 *
 *   test-topo is the degenerate shape: all six of its PCI devices hang off
 *   one Group of a four-group machine, so "near this device" distinguishes
 *   nothing.  It also separates two properties that are easy to conflate -
 *   four of its OS devices have no PCI ancestor (the "sorts last" rule) and
 *   three resolve no locality narrower than the machine itself, and those
 *   are not the same three.
 */

#include "src/include/pmix_config.h"
#include "include/pmix.h"
#include "src/include/pmix_globals.h"
#include "src/hwloc/pmix_hwloc.h"

#include <hwloc.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;
static int checks = 0;

static void ok(bool cond, const char *what)
{
    ++checks;
    if (!cond) {
        ++failures;
        fprintf(stderr, "FAIL: %s\n", what);
    }
}

static int load_topo_file(const char *path, pmix_topology_t *topo)
{
    hwloc_topology_t t;

    if (0 != hwloc_topology_init(&t)) {
        return -1;
    }
    if (0 != hwloc_topology_set_xml(t, path)) {
        hwloc_topology_destroy(t);
        return -1;
    }
    /* the I/O objects are the entire point here */
    (void) hwloc_topology_set_io_types_filter(t, HWLOC_TYPE_FILTER_KEEP_IMPORTANT);
    (void) hwloc_topology_set_flags(t, HWLOC_TOPOLOGY_FLAG_INCLUDE_DISALLOWED);
    if (0 != hwloc_topology_load(t)) {
        hwloc_topology_destroy(t);
        return -1;
    }
    topo->source = strdup("hwloc");
    topo->topology = (void *) t;
    return 0;
}

static void free_topo(pmix_topology_t *topo)
{
    if (NULL != topo->topology) {
        hwloc_topology_destroy((hwloc_topology_t) topo->topology);
        topo->topology = NULL;
    }
    if (NULL != topo->source) {
        free(topo->source);
        topo->source = NULL;
    }
}

/* is the array ordered by busid, with the PCI-less entries last? */
static bool ordered_by_busid(pmix_hwloc_device_t *devs, size_t n)
{
    size_t i;
    bool seen_null = false;

    for (i = 0; i < n; i++) {
        if (NULL == devs[i].busid) {
            seen_null = true;
            continue;
        }
        if (seen_null) {
            return false; /* a PCI device after a PCI-less one */
        }
        if (0 < i && NULL != devs[i - 1].busid
            && 0 <= strcmp(devs[i - 1].busid, devs[i].busid)) {
            return false;
        }
    }
    return true;
}

static void test_topo2(const char *dir)
{
    pmix_topology_t topo = PMIX_TOPOLOGY_STATIC_INIT;
    pmix_hwloc_device_t *devs = NULL;
    size_t ndevs = 0;
    char path[1024];
    pmix_status_t rc;

    snprintf(path, sizeof(path), "%s/test-topo2.xml", dir);
    if (0 != load_topo_file(path, &topo)) {
        fprintf(stderr, "FAIL: could not load %s\n", path);
        ++failures;
        return;
    }

    /* THE case: one GPU, not two (the display controller) and not three or
     * four (the extra OS devices on the same function) */
    rc = pmix_hwloc_get_devices(&topo, PMIX_DEVTYPE_GPU, NULL, &devs, &ndevs);
    ok(PMIX_SUCCESS == rc, "topo2: gpu enumeration succeeds");
    ok(1 == ndevs, "topo2: exactly one compute GPU");
    if (1 == ndevs) {
        /* the vendor compute node is the most useful name, so it wins over
         * the card and render nodes on the same function */
        ok(NULL != devs[0].dev.osname && 0 == strcmp(devs[0].dev.osname, "rsmi0"),
           "topo2: the GPU is named by its vendor compute node");
        ok(NULL != devs[0].busid && 0 == strcmp(devs[0].busid, "0000:63:00.0"),
           "topo2: the GPU reports its PCI function");
        ok(NULL != devs[0].dev.uuid && 0 == strncmp(devs[0].dev.uuid, "gpu://", 6),
           "topo2: the GPU uuid uses the gpu:// grammar");
        ok(NULL != devs[0].locality && NULL != devs[0].locality->cpuset,
           "topo2: the GPU has a locality with a cpuset");
    }
    pmix_hwloc_release_devices(devs, ndevs);
    devs = NULL;
    ndevs = 0;

    /* lookup by name, and by a name that is not there */
    rc = pmix_hwloc_get_devices(&topo, PMIX_DEVTYPE_GPU, "rsmi0", &devs, &ndevs);
    ok(PMIX_SUCCESS == rc && 1 == ndevs, "topo2: gpu by osname finds it");
    pmix_hwloc_release_devices(devs, ndevs);
    devs = NULL;
    ndevs = 0;

    rc = pmix_hwloc_get_devices(&topo, PMIX_DEVTYPE_GPU, "no-such-device", &devs, &ndevs);
    ok(PMIX_SUCCESS == rc && 0 == ndevs,
       "topo2: an unknown device name is empty, not an error");
    pmix_hwloc_release_devices(devs, ndevs);
    devs = NULL;
    ndevs = 0;

    /* the fabric and network devices sit on different functions */
    rc = pmix_hwloc_get_devices(&topo, PMIX_DEVTYPE_OPENFABRICS, NULL, &devs, &ndevs);
    ok(PMIX_SUCCESS == rc && 1 == ndevs, "topo2: one openfabrics device");
    pmix_hwloc_release_devices(devs, ndevs);
    devs = NULL;
    ndevs = 0;

    rc = pmix_hwloc_get_devices(&topo, PMIX_DEVTYPE_NETWORK, NULL, &devs, &ndevs);
    ok(PMIX_SUCCESS == rc && 2 == ndevs, "topo2: two network devices");
    pmix_hwloc_release_devices(devs, ndevs);
    devs = NULL;
    ndevs = 0;

    /* The dedup rule, stated as a subtraction: one of those two network
     * devices (ib0) shares its PCI function with the openfabrics device
     * (mlx5_0).  Asked for both types at once they are one device, so the
     * union is 2 rather than the 3 an OS-device count would give. */
    rc = pmix_hwloc_get_devices(&topo, PMIX_DEVTYPE_NETWORK | PMIX_DEVTYPE_OPENFABRICS,
                                NULL, &devs, &ndevs);
    ok(PMIX_SUCCESS == rc && 2 == ndevs,
       "topo2: network+openfabrics dedup to one device per PCI function");
    pmix_hwloc_release_devices(devs, ndevs);
    devs = NULL;
    ndevs = 0;

    /* everything, in PCI order */
    rc = pmix_hwloc_get_devices(&topo, PMIX_DEVTYPE_UNKNOWN, NULL, &devs, &ndevs);
    ok(PMIX_SUCCESS == rc, "topo2: enumerating every type succeeds");
    ok(0 < ndevs, "topo2: every-type enumeration is non-empty");
    ok(ordered_by_busid(devs, ndevs), "topo2: devices come back in PCI bus order");
    pmix_hwloc_release_devices(devs, ndevs);

    free_topo(&topo);
}

static void test_topo1(const char *dir)
{
    pmix_topology_t topo = PMIX_TOPOLOGY_STATIC_INIT;
    pmix_hwloc_device_t *devs = NULL;
    size_t ndevs = 0, i, nnopci = 0, nmachine = 0;
    char path[1024];
    pmix_status_t rc;
    hwloc_obj_t shared = NULL, root;
    bool all_same = true;

    snprintf(path, sizeof(path), "%s/test-topo.xml", dir);
    if (0 != load_topo_file(path, &topo)) {
        fprintf(stderr, "FAIL: could not load %s\n", path);
        ++failures;
        return;
    }

    /* this topology has no GPU at all - an empty result, not an error */
    rc = pmix_hwloc_get_devices(&topo, PMIX_DEVTYPE_GPU, NULL, &devs, &ndevs);
    ok(PMIX_SUCCESS == rc && 0 == ndevs, "topo1: no GPUs is empty, not an error");
    ok(NULL == devs, "topo1: an empty result hands back no array");
    pmix_hwloc_release_devices(devs, ndevs);
    devs = NULL;
    ndevs = 0;

    rc = pmix_hwloc_get_devices(&topo, PMIX_DEVTYPE_UNKNOWN, NULL, &devs, &ndevs);
    ok(PMIX_SUCCESS == rc && 0 < ndevs, "topo1: devices are found");
    ok(ordered_by_busid(devs, ndevs), "topo1: devices come back in PCI bus order");

    /* Every PCI device on this machine hangs off one Group of four - the
     * degenerate-locality shape, where "near this device" says nothing
     * because they are all equally near.  The devices with no PCI ancestor
     * resolve all the way up to the machine, which is the other shape a
     * caller has to cope with: a device with no locality narrower than the
     * node. */
    root = hwloc_get_root_obj((hwloc_topology_t) topo.topology);
    for (i = 0; i < ndevs; i++) {
        ok(NULL != devs[i].locality, "topo1: every device resolves a locality");
        if (NULL == devs[i].locality) {
            continue;
        }
        if (devs[i].locality == root) {
            /* nothing narrower than the whole node is local to it */
            ++nmachine;
        }
        if (NULL == devs[i].busid) {
            ++nnopci;
            continue;
        }
        if (NULL == shared) {
            shared = devs[i].locality;
        } else if (shared != devs[i].locality) {
            all_same = false;
        }
    }
    ok(0 < nnopci, "topo1: some devices have no PCI ancestor");
    /* Having no PCI ancestor and having no locality are different things:
     * this topology has a device of each kind, and one that is both. */
    ok(0 < nmachine, "topo1: some devices are local to nothing narrower than the node");
    ok(NULL != shared && shared != root,
       "topo1: the PCI devices have a locality narrower than the machine");
    ok(all_same, "topo1: every PCI device shares one locality (degenerate case)");

    pmix_hwloc_release_devices(devs, ndevs);
    free_topo(&topo);
}

int main(int argc, char **argv)
{
    const char *dir;

    (void) argc;
    (void) argv;

#ifndef PMIX_TEST_TOPO_DIR
    fprintf(stderr, "SKIP: no topology directory configured\n");
    return 77;
#else
    dir = PMIX_TEST_TOPO_DIR;
#endif

    /* the gpu:// uuid embeds the hostname */
    if (NULL == pmix_globals.hostname) {
        pmix_globals.hostname = strdup("testhost");
    }

    test_topo2(dir);
    test_topo1(dir);

    fprintf(stderr, "%s: %d checks, %d failures\n",
            (0 == failures) ? "PASS" : "FAIL", checks, failures);
    return (0 == failures) ? 0 : 1;
}
