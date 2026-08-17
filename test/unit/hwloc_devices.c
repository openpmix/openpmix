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

/* The node whose topology we claim to be reading.  Deliberately NOT the
 * hostname this test process is running under: a device uuid names the node
 * the device lives on, and the two are the same only when the topology is
 * the local one.  Every enumeration below therefore says whose topology it
 * has, exactly as a mapper reading another node's topology must. */
#define TESTHOST "test-node-01"

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
    rc = pmix_hwloc_get_devices(&topo, TESTHOST, PMIX_DEVTYPE_GPU, NULL, &devs, &ndevs);
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
    rc = pmix_hwloc_get_devices(&topo, TESTHOST, PMIX_DEVTYPE_GPU, "rsmi0", &devs, &ndevs);
    ok(PMIX_SUCCESS == rc && 1 == ndevs, "topo2: gpu by osname finds it");
    pmix_hwloc_release_devices(devs, ndevs);
    devs = NULL;
    ndevs = 0;

    rc = pmix_hwloc_get_devices(&topo, TESTHOST, PMIX_DEVTYPE_GPU, "no-such-device", &devs, &ndevs);
    ok(PMIX_SUCCESS == rc && 0 == ndevs,
       "topo2: an unknown device name is empty, not an error");
    pmix_hwloc_release_devices(devs, ndevs);
    devs = NULL;
    ndevs = 0;

    /* Naming a device that shares its PCI function with another: mlx5_0 and
     * ib0 are one device, and only one of the two names it.  Asking for
     * either has to find it, or "--map-by device=mlx5_0" - the whole reason
     * a caller names a device - finds nothing on a machine where the
     * network device happened to sort first. */
    rc = pmix_hwloc_get_devices(&topo, TESTHOST, PMIX_DEVTYPE_UNKNOWN, "mlx5_0", &devs, &ndevs);
    ok(PMIX_SUCCESS == rc && 1 == ndevs, "topo2: the fabric device is found by its own name");
    if (1 == ndevs) {
        ok(NULL != devs[0].dev.osname && 0 == strcmp(devs[0].dev.osname, "mlx5_0"),
           "topo2: and it is reported under the name that was asked for");
    }
    pmix_hwloc_release_devices(devs, ndevs);
    devs = NULL;
    ndevs = 0;

    rc = pmix_hwloc_get_devices(&topo, TESTHOST, PMIX_DEVTYPE_UNKNOWN, "ib0", &devs, &ndevs);
    ok(PMIX_SUCCESS == rc && 1 == ndevs, "topo2: the network device on that same function too");
    pmix_hwloc_release_devices(devs, ndevs);
    devs = NULL;
    ndevs = 0;

    /* the fabric and network devices sit on different functions */
    rc = pmix_hwloc_get_devices(&topo, TESTHOST, PMIX_DEVTYPE_OPENFABRICS, NULL, &devs, &ndevs);
    ok(PMIX_SUCCESS == rc && 1 == ndevs, "topo2: one openfabrics device");
    pmix_hwloc_release_devices(devs, ndevs);
    devs = NULL;
    ndevs = 0;

    rc = pmix_hwloc_get_devices(&topo, TESTHOST, PMIX_DEVTYPE_NETWORK, NULL, &devs, &ndevs);
    ok(PMIX_SUCCESS == rc && 2 == ndevs, "topo2: two network devices");
    pmix_hwloc_release_devices(devs, ndevs);
    devs = NULL;
    ndevs = 0;

    /* The dedup rule, stated as a subtraction: one of those two network
     * devices (ib0) shares its PCI function with the openfabrics device
     * (mlx5_0).  Asked for both types at once they are one device, so the
     * union is 2 rather than the 3 an OS-device count would give. */
    rc = pmix_hwloc_get_devices(&topo, TESTHOST, PMIX_DEVTYPE_NETWORK | PMIX_DEVTYPE_OPENFABRICS,
                                NULL, &devs, &ndevs);
    ok(PMIX_SUCCESS == rc && 2 == ndevs,
       "topo2: network+openfabrics dedup to one device per PCI function");
    pmix_hwloc_release_devices(devs, ndevs);
    devs = NULL;
    ndevs = 0;

    /* everything, in PCI order */
    rc = pmix_hwloc_get_devices(&topo, TESTHOST, PMIX_DEVTYPE_UNKNOWN, NULL, &devs, &ndevs);
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
    rc = pmix_hwloc_get_devices(&topo, TESTHOST, PMIX_DEVTYPE_GPU, NULL, &devs, &ndevs);
    ok(PMIX_SUCCESS == rc && 0 == ndevs, "topo1: no GPUs is empty, not an error");
    ok(NULL == devs, "topo1: an empty result hands back no array");
    pmix_hwloc_release_devices(devs, ndevs);
    devs = NULL;
    ndevs = 0;

    rc = pmix_hwloc_get_devices(&topo, TESTHOST, PMIX_DEVTYPE_UNKNOWN, NULL, &devs, &ndevs);
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

/* The enumerator and the distance array are handed to the same application:
 * one says which devices exist, the other how far away they are.  A device
 * the two disagree about - in name, or in existence - is worse than either
 * answer alone, because nothing the application can do will reconcile them.
 * Since compute_distances() is built on get_devices(), that agreement is
 * structural; this pins it so it stays that way. */
static void test_distances_agree(const char *dir)
{
    pmix_topology_t topo = PMIX_TOPOLOGY_STATIC_INIT;
    pmix_cpuset_t cpuset = PMIX_CPUSET_STATIC_INIT;
    pmix_hwloc_device_t *devs = NULL;
    pmix_device_distance_t *dist = NULL;
    size_t ndevs = 0, ndist = 0, i, j;
    char path[1024];
    pmix_status_t rc;
    bool matched;
    hwloc_topology_t t;

    snprintf(path, sizeof(path), "%s/test-topo2.xml", dir);
    if (0 != load_topo_file(path, &topo)) {
        fprintf(stderr, "FAIL: could not load %s\n", path);
        ++failures;
        return;
    }
    t = (hwloc_topology_t) topo.topology;

    /* bind to the whole machine so every device is measurable */
    cpuset.source = strdup("hwloc");
    cpuset.bitmap = hwloc_bitmap_dup(hwloc_get_root_obj(t)->cpuset);

    /* pmix_globals.hostname, not TESTHOST, and that is the point: distances
     * are only ever computed for the local node, so this is the one caller
     * whose hostname is not a choice.  Handing the enumerator anything else
     * here would make the two disagree on every uuid. */
    rc = pmix_hwloc_get_devices(&topo, pmix_globals.hostname,
                                PMIX_DEVTYPE_GPU | PMIX_DEVTYPE_NETWORK
                                       | PMIX_DEVTYPE_OPENFABRICS,
                                NULL, &devs, &ndevs);
    ok(PMIX_SUCCESS == rc && 0 < ndevs, "agree: the enumerator finds devices");

    rc = pmix_hwloc_compute_distances(&topo, &cpuset, NULL, 0, &dist, &ndist);
    ok(PMIX_SUCCESS == rc, "agree: distances computed");
    ok(ndist == ndevs, "agree: the two report the same number of devices");

    /* every device the enumerator found has a distance entry under the same
     * name - the string an application correlates the two by */
    for (i = 0; i < ndevs; i++) {
        matched = false;
        for (j = 0; j < ndist; j++) {
            if (NULL != dist[j].uuid && NULL != devs[i].dev.uuid
                && 0 == strcmp(dist[j].uuid, devs[i].dev.uuid)) {
                matched = true;
                break;
            }
        }
        ok(matched, "agree: each enumerated device has a distance under the same uuid");
    }

    /* and the GPU is there under its vendor compute node, not dropped for
     * being a coprocessor rather than a "gpu" OS device */
    matched = false;
    for (j = 0; j < ndist; j++) {
        if (NULL != dist[j].osname && 0 == strcmp(dist[j].osname, "rsmi0")) {
            matched = true;
        }
    }
    ok(matched, "agree: the GPU appears in the distances under its compute node");

    if (NULL != dist) {
        PMIx_Device_distance_free(dist, ndist);
    }
    pmix_hwloc_release_devices(devs, ndevs);
    hwloc_bitmap_free(cpuset.bitmap);
    free(cpuset.source);
    free_topo(&topo);
}

/* A device uuid names the node the device is on, not the node doing the
 * reading.
 *
 * This is the property that makes the uuid worth carrying at all: a process
 * is handed the uuid of the device it was assigned, computes the same string
 * locally from PMIX_DEVICE_DISTANCES, and correlates the two.  A caller
 * reading a remote topology - the mapper on the head node, which is the only
 * caller that reads one - therefore has to supply the node's name, because
 * the alternative (this process's own hostname) would stamp the head node on
 * every device in the job and nothing would ever correlate.  Enumerating one
 * topology under two names is the whole test.
 *
 * Not every uuid moves, and that is correct rather than an exception to work
 * around: a fabric or network device is named by its own GUIDs or MAC, which
 * identify the card wherever it is plugged in.  Only the grammars that spell
 * a hostname - gpu:// and blk:// - may differ between the two readings, and
 * they must. */
static void test_uuid_names_the_node(const char *dir)
{
    pmix_topology_t topo = PMIX_TOPOLOGY_STATIC_INIT;
    pmix_hwloc_device_t *a = NULL, *b = NULL;
    size_t na = 0, nb = 0, i, nhosted = 0;
    char path[1024], expect[512];
    const char *grammar;
    pmix_status_t rc;
    bool same_osname = true, moved = true, stable = true, named = true;

    snprintf(path, sizeof(path), "%s/test-topo2.xml", dir);
    if (0 != load_topo_file(path, &topo)) {
        fprintf(stderr, "FAIL: could not load %s\n", path);
        ++failures;
        return;
    }

    /* a hostname is required, not defaulted: silently substituting the local
     * one is precisely the bug this parameter exists to make impossible */
    rc = pmix_hwloc_get_devices(&topo, NULL, PMIX_DEVTYPE_UNKNOWN, NULL, &a, &na);
    ok(PMIX_ERR_BAD_PARAM == rc, "hostname: a NULL hostname is refused");
    ok(NULL == a && 0 == na, "hostname: a refused call hands back nothing");

    rc = pmix_hwloc_get_devices(&topo, "node-a", PMIX_DEVTYPE_UNKNOWN, NULL, &a, &na);
    ok(PMIX_SUCCESS == rc && 0 < na, "hostname: enumeration as node-a succeeds");
    rc = pmix_hwloc_get_devices(&topo, "node-b", PMIX_DEVTYPE_UNKNOWN, NULL, &b, &nb);
    ok(PMIX_SUCCESS == rc && 0 < nb, "hostname: enumeration as node-b succeeds");
    ok(na == nb, "hostname: one topology yields the same device count either way");

    if (na == nb) {
        for (i = 0; i < na; i++) {
            if (NULL == a[i].dev.osname || NULL == b[i].dev.osname
                || 0 != strcmp(a[i].dev.osname, b[i].dev.osname)) {
                same_osname = false;
            }
            if (NULL == a[i].dev.uuid || NULL == b[i].dev.uuid) {
                same_osname = false;
                continue;
            }
            grammar = NULL;
            if (0 == strncmp(a[i].dev.uuid, "gpu://", 6)) {
                grammar = "gpu";
            } else if (0 == strncmp(a[i].dev.uuid, "blk://", 6)) {
                grammar = "blk";
            }
            if (NULL == grammar) {
                /* named by the hardware itself - it must NOT move */
                if (0 != strcmp(a[i].dev.uuid, b[i].dev.uuid)) {
                    stable = false;
                }
                continue;
            }
            ++nhosted;
            if (0 == strcmp(a[i].dev.uuid, b[i].dev.uuid)) {
                moved = false;
            }
            snprintf(expect, sizeof(expect), "%s://node-a::%s", grammar, a[i].dev.osname);
            if (0 != strcmp(a[i].dev.uuid, expect)) {
                named = false;
            }
            snprintf(expect, sizeof(expect), "%s://node-b::%s", grammar, b[i].dev.osname);
            if (0 != strcmp(b[i].dev.uuid, expect)) {
                named = false;
            }
        }
        ok(same_osname, "hostname: the devices themselves are unchanged");
        /* without this the three checks below would pass on a topology
         * carrying nothing the hostname reaches */
        ok(0 < nhosted, "hostname: the topology has devices whose uuid names a node");
        ok(moved, "hostname: no two nodes share a host-named device uuid");
        ok(stable, "hostname: a device named by its own hardware ids does not move");
        ok(named, "hostname: each uuid names the node it was asked about");
    }

    pmix_hwloc_release_devices(a, na);
    pmix_hwloc_release_devices(b, nb);
    free_topo(&topo);
}

/* The vendor's own identity for a device, which is the only handle a GPU
 * runtime will accept.
 *
 * Three properties, and the middle one is the whole reason this is not a
 * one-line read off the named device:
 *
 *   - nvml-4gpu names each function by "cuda0" (the first vendor compute
 *     node it meets) while NVIDIAUUID lives on the sibling "nvml0" of the
 *     same PCI function.  A scan of the named device alone reports "no
 *     identity" on exactly the machines that have one.
 *   - test-topo2 is the other arrangement - AMDUUID sits on "rsmi0", which
 *     is also the device that names the function - so both paths are
 *     covered rather than just whichever one the first fixture happened to
 *     use.
 *   - drm-4gpu is the same machine as nvml-4gpu seen by an hwloc built
 *     without the vendor backends.  Its GPUs are found and placed exactly
 *     as well; they simply cannot be named, and NULL is the honest answer
 *     rather than a failure.
 */
static void test_vendor_identity(const char *dir)
{
    struct {
        const char *file;
        const char *osname;   /* the device that names the function */
        const char *prefix;   /* what its vendor id must start with, or NULL */
        size_t ndevs;
    } cases[] = {
        {"nvml-4gpu.xml", "cuda0", "GPU-", 4},
        {"test-topo2.xml", "rsmi0", "d364", 1},
        {"drm-4gpu.xml", NULL, NULL, 4},
    };
    pmix_topology_t topo = PMIX_TOPOLOGY_STATIC_INIT;
    pmix_hwloc_device_t *devs = NULL;
    size_t ndevs = 0, i, c, nident;
    char path[1024];
    pmix_status_t rc;

    for (c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        snprintf(path, sizeof(path), "%s/%s", dir, cases[c].file);
        if (0 != load_topo_file(path, &topo)) {
            fprintf(stderr, "FAIL: could not load %s\n", path);
            ++failures;
            continue;
        }
        rc = pmix_hwloc_get_devices(&topo, TESTHOST,
                                    PMIX_DEVTYPE_GPU | PMIX_DEVTYPE_COPROC,
                                    NULL, &devs, &ndevs);
        ok(PMIX_SUCCESS == rc, "vendor: enumeration succeeds");
        ok(cases[c].ndevs == ndevs, "vendor: the expected number of GPUs");

        nident = 0;
        for (i = 0; i < ndevs; i++) {
            if (NULL != devs[i].vendor_id) {
                ++nident;
            }
        }
        if (NULL == cases[c].prefix) {
            ok(0 == nident, "vendor: no backend means no identity, not a failure");
        } else {
            ok(nident == ndevs, "vendor: every GPU carries a vendor identity");
            if (0 < ndevs) {
                ok(NULL != devs[0].dev.osname
                       && 0 == strcmp(devs[0].dev.osname, cases[c].osname),
                   "vendor: the function is named as expected");
                ok(NULL != devs[0].vendor_id
                       && 0 == strncmp(devs[0].vendor_id, cases[c].prefix,
                                       strlen(cases[c].prefix)),
                   "vendor: the identity is the vendor's own, in its own grammar");
            }
            /* two GPUs on one machine are never the same GPU */
            if (1 < ndevs) {
                ok(NULL != devs[0].vendor_id && NULL != devs[1].vendor_id
                       && 0 != strcmp(devs[0].vendor_id, devs[1].vendor_id),
                   "vendor: distinct devices carry distinct identities");
            }
        }
        pmix_hwloc_release_devices(devs, ndevs);
        devs = NULL;
        ndevs = 0;
        free_topo(&topo);
    }
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

    /* compute_distances() reads this for the local node it measures; the
     * enumerator no longer reads it at all */
    if (NULL == pmix_globals.hostname) {
        pmix_globals.hostname = strdup("testhost");
    }

    test_topo2(dir);
    test_topo1(dir);
    test_distances_agree(dir);
    test_uuid_names_the_node(dir);
    test_vendor_identity(dir);

    fprintf(stderr, "%s: %d checks, %d failures\n",
            (0 == failures) ? "PASS" : "FAIL", checks, failures);
    return (0 == failures) ? 0 : 1;
}
