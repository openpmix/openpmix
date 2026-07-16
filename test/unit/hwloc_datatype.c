/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Unit tests for the src/hwloc datatype layer: pack/unpack, copy, print,
 * and size of pmix_topology_t (PMIX_TOPO) and pmix_cpuset_t
 * (PMIX_PROC_CPUSET), plus PMIx_Get_relative_locality. These exercise the
 * public PMIx entry points that route into src/hwloc.
 *
 * The topology tests are run twice: once against the machine's own
 * (this-system) topology, and once against each synthetic topology XML in
 * PMIX_TEST_TOPO_DIR (test/topologies), so the pack/unpack/print/size code
 * is exercised against machine shapes other than the one the test happens
 * to run on. The topology pack/unpack path is the one that matters most:
 * the support flags now ride inside the exported XML (recovered on the far
 * side via HWLOC_TOPOLOGY_FLAG_IMPORT_SUPPORT) rather than being hand-
 * serialized as raw struct bytes, so the round-trip must faithfully
 * reproduce the topology structure.
 */

#include "src/include/pmix_config.h"
#include "include/pmix.h"
#include "include/pmix_server.h"
#include "src/include/pmix_globals.h"
#include "src/util/pmix_printf.h"

#include <hwloc.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static pmix_server_module_t mymodule = {
    .client_connected = NULL,
    .client_finalized = NULL,
    .abort = NULL,
    .fence_nb = NULL,
    .direct_modex = NULL,
    .publish = NULL,
    .lookup = NULL,
    .unpublish = NULL,
    .spawn = NULL,
    .connect = NULL,
    .disconnect = NULL,
    .register_events = NULL,
    .deregister_events = NULL,
    .notify_event = NULL,
    .query = NULL,
    .tool_connected = NULL,
    .log = NULL,
    .allocate = NULL,
    .job_control = NULL,
    .monitor = NULL,
    .group = NULL
};

static int npass = 0;
static int nfail = 0;

static void report(const char *name, int passed)
{
    if (passed) {
        fprintf(stdout, "  PASS: %s\n", name);
        npass++;
    } else {
        fprintf(stdout, "  FAIL: %s\n", name);
        nfail++;
    }
}

/* Load a topology from an XML file into a pmix_topology_t, as a plain
 * (non-this-system) topology. Returns 0 on success, -1 on failure. */
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
    (void) hwloc_topology_set_flags(t, HWLOC_TOPOLOGY_FLAG_INCLUDE_DISALLOWED);
    if (0 != hwloc_topology_load(t)) {
        hwloc_topology_destroy(t);
        return -1;
    }
    topo->source = strdup("hwloc");
    topo->topology = (void *) t;
    return 0;
}

/* ------------------------------------------------------------------ */
/* topology pack / unpack                                              */
/* ------------------------------------------------------------------ */

/* Pack a topology, unpack it into a fresh object, and confirm the
 * reconstructed topology matches the original structurally. This is the
 * direct test of the XML-carries-the-support-flags change. */
static void test_topology_pack_unpack(pmix_topology_t *topo, const char *label)
{
    pmix_topology_t dst = PMIX_TOPOLOGY_STATIC_INIT;
    pmix_data_buffer_t buf;
    pmix_status_t rc;
    int32_t count;
    char name[256];
    int ok = 0;

    snprintf(name, sizeof(name), "topology pack/unpack preserves structure [%s]", label);
    PMIx_Data_buffer_construct(&buf);

    rc = PMIx_Data_pack(NULL, &buf, topo, 1, PMIX_TOPO);
    if (PMIX_SUCCESS != rc) {
        fprintf(stdout, "    pack failed: %s\n", PMIx_Error_string(rc));
        goto cleanup;
    }

    count = 1;
    rc = PMIx_Data_unpack(NULL, &buf, &dst, &count, PMIX_TOPO);
    if (PMIX_SUCCESS != rc) {
        fprintf(stdout, "    unpack failed: %s\n", PMIx_Error_string(rc));
        goto cleanup;
    }

    if (NULL == dst.source || 0 != strncasecmp(dst.source, "hwloc", 5)) {
        fprintf(stdout, "    unpacked topology has unexpected source: %s\n",
                (NULL == dst.source) ? "(null)" : dst.source);
        goto cleanup;
    }
    if (NULL == dst.topology) {
        fprintf(stdout, "    unpacked topology is NULL\n");
        goto cleanup;
    }

    /* structural comparison against the original */
    hwloc_topology_t a = (hwloc_topology_t) topo->topology;
    hwloc_topology_t b = (hwloc_topology_t) dst.topology;
    if (hwloc_topology_get_depth(a) != hwloc_topology_get_depth(b)) {
        fprintf(stdout, "    depth mismatch: orig=%d unpacked=%d\n",
                hwloc_topology_get_depth(a), hwloc_topology_get_depth(b));
        goto cleanup;
    }
    int puda = hwloc_get_type_depth(a, HWLOC_OBJ_PU);
    int pudb = hwloc_get_type_depth(b, HWLOC_OBJ_PU);
    if (hwloc_get_nbobjs_by_depth(a, puda) != hwloc_get_nbobjs_by_depth(b, pudb)) {
        fprintf(stdout, "    PU count mismatch: orig=%u unpacked=%u\n",
                hwloc_get_nbobjs_by_depth(a, puda), hwloc_get_nbobjs_by_depth(b, pudb));
        goto cleanup;
    }
    ok = 1;

cleanup:
    PMIx_Topology_destruct(&dst);
    PMIx_Data_buffer_destruct(&buf);
    report(name, ok);
}

/* Pack several topologies into one buffer and unpack them all - guards
 * against buffer desync (the failure mode of the old raw-struct support
 * serialization when peers disagreed on struct sizes). */
static void test_topology_pack_unpack_multiple(pmix_topology_t *topo, const char *label)
{
    const int N = 3;
    pmix_topology_t dst[3];
    pmix_data_buffer_t buf;
    pmix_status_t rc;
    int32_t count;
    char name[256];
    int i, ok = 1;

    snprintf(name, sizeof(name), "multiple topologies pack/unpack without desync [%s]", label);
    for (i = 0; i < N; i++) {
        pmix_topology_t t = PMIX_TOPOLOGY_STATIC_INIT;
        dst[i] = t;
    }
    PMIx_Data_buffer_construct(&buf);

    for (i = 0; i < N; i++) {
        rc = PMIx_Data_pack(NULL, &buf, topo, 1, PMIX_TOPO);
        if (PMIX_SUCCESS != rc) {
            fprintf(stdout, "    pack[%d] failed: %s\n", i, PMIx_Error_string(rc));
            ok = 0;
            goto cleanup;
        }
    }
    for (i = 0; i < N; i++) {
        count = 1;
        rc = PMIx_Data_unpack(NULL, &buf, &dst[i], &count, PMIX_TOPO);
        if (PMIX_SUCCESS != rc) {
            fprintf(stdout, "    unpack[%d] failed: %s\n", i, PMIx_Error_string(rc));
            ok = 0;
            goto cleanup;
        }
        if (NULL == dst[i].topology) {
            fprintf(stdout, "    unpack[%d] produced NULL topology\n", i);
            ok = 0;
            goto cleanup;
        }
    }

cleanup:
    for (i = 0; i < N; i++) {
        PMIx_Topology_destruct(&dst[i]);
    }
    PMIx_Data_buffer_destruct(&buf);
    report(name, ok);
}

/* ------------------------------------------------------------------ */
/* topology print                                                      */
/* ------------------------------------------------------------------ */

/* The local topology is this-system, so support is available and the root
 * machine object must render the bind-support lines. */
static void test_topology_print_local(pmix_topology_t *topo)
{
    char *output = NULL;
    pmix_status_t rc;
    int ok = 0;

    rc = PMIx_Data_print(&output, NULL, topo, PMIX_TOPO);
    if (PMIX_SUCCESS != rc) {
        fprintf(stdout, "    print failed: %s\n", PMIx_Error_string(rc));
        goto cleanup;
    }
    if (NULL != output && '\0' != output[0] &&
        NULL != strstr(output, "Type:") &&
        NULL != strstr(output, "Bind CPU")) {
        ok = 1;
    } else {
        fprintf(stdout, "    print output missing expected content\n");
    }

cleanup:
    free(output);
    report("topology print renders machine + bind support [local]", ok);
}

/* A topology loaded from an older XML export is neither this-system nor
 * carries importable support, so print must render cleanly and report bind
 * support as "not available" rather than emit misleading values. This is
 * the deterministic exercise of the "not available" branch. */
static void test_topology_print_imported(pmix_topology_t *topo, const char *label)
{
    char *output = NULL;
    pmix_status_t rc;
    char name[256];
    int ok = 0;

    snprintf(name, sizeof(name), "imported topology prints, bind support not available [%s]", label);
    rc = PMIx_Data_print(&output, NULL, topo, PMIX_TOPO);
    if (PMIX_SUCCESS != rc) {
        fprintf(stdout, "    print failed: %s\n", PMIx_Error_string(rc));
        goto cleanup;
    }
    if (NULL != output && '\0' != output[0] &&
        NULL != strstr(output, "Type:") &&
        NULL != strstr(output, "not available")) {
        ok = 1;
    } else {
        fprintf(stdout, "    print output missing expected content\n");
    }

cleanup:
    free(output);
    report(name, ok);
}

/* ------------------------------------------------------------------ */
/* size                                                                */
/* ------------------------------------------------------------------ */

static void test_topology_get_size(pmix_topology_t *topo, const char *label)
{
    pmix_value_t val;
    size_t sz = 0;
    char name[256];
    int ok = 0;

    snprintf(name, sizeof(name), "topology size is reported [%s]", label);
    PMIX_VALUE_CONSTRUCT(&val);
    val.type = PMIX_TOPO;
    val.data.topo = topo;

    if (PMIX_SUCCESS == PMIx_Value_get_size(&val, &sz) && 0 < sz) {
        ok = 1;
    } else {
        fprintf(stdout, "    get_size returned sz=%zu\n", sz);
    }
    /* do not destruct val: data.topo aliases a topology owned elsewhere */
    report(name, ok);
}

/* ------------------------------------------------------------------ */
/* cpuset pack / unpack / copy                                         */
/* ------------------------------------------------------------------ */

static void test_cpuset_pack_unpack(void)
{
    pmix_cpuset_t src, dst;
    pmix_data_buffer_t buf;
    pmix_status_t rc;
    int32_t count;
    int ok = 0;

    memset(&src, 0, sizeof(src));
    memset(&dst, 0, sizeof(dst));
    PMIx_Data_buffer_construct(&buf);

    rc = PMIx_Parse_cpuset_string("hwloc:0-3", &src);
    if (PMIX_SUCCESS != rc) {
        fprintf(stdout, "    parse_cpuset_string failed: %s\n", PMIx_Error_string(rc));
        goto cleanup;
    }

    rc = PMIx_Data_pack(NULL, &buf, &src, 1, PMIX_PROC_CPUSET);
    if (PMIX_SUCCESS != rc) {
        fprintf(stdout, "    pack failed: %s\n", PMIx_Error_string(rc));
        goto cleanup;
    }
    count = 1;
    rc = PMIx_Data_unpack(NULL, &buf, &dst, &count, PMIX_PROC_CPUSET);
    if (PMIX_SUCCESS != rc) {
        fprintf(stdout, "    unpack failed: %s\n", PMIx_Error_string(rc));
        goto cleanup;
    }

    if (NULL != dst.source && 0 == strncasecmp(dst.source, "hwloc", 5) &&
        NULL != dst.bitmap &&
        hwloc_bitmap_isequal((hwloc_bitmap_t) src.bitmap, (hwloc_bitmap_t) dst.bitmap)) {
        ok = 1;
    } else {
        fprintf(stdout, "    cpuset content mismatch after unpack\n");
    }

cleanup:
    PMIx_Cpuset_destruct(&src);
    PMIx_Cpuset_destruct(&dst);
    PMIx_Data_buffer_destruct(&buf);
    report("cpuset pack/unpack round-trip", ok);
}

static void test_cpuset_copy(void)
{
    pmix_value_t vsrc, vdst;
    int ok = 0;

    PMIX_VALUE_CONSTRUCT(&vsrc);
    PMIX_VALUE_CONSTRUCT(&vdst);

    vsrc.type = PMIX_PROC_CPUSET;
    vsrc.data.cpuset = PMIx_Cpuset_create(1);
    if (NULL == vsrc.data.cpuset) {
        report("cpuset deep-copy (alloc)", 0);
        goto cleanup;
    }
    if (PMIX_SUCCESS != PMIx_Parse_cpuset_string("hwloc:2-5", vsrc.data.cpuset)) {
        report("cpuset deep-copy (setup)", 0);
        goto cleanup;
    }

    if (PMIX_SUCCESS != PMIx_Value_xfer(&vdst, &vsrc)) {
        report("cpuset deep-copy (xfer)", 0);
        goto cleanup;
    }

    if (PMIX_PROC_CPUSET == vdst.type && NULL != vdst.data.cpuset &&
        NULL != vdst.data.cpuset->bitmap &&
        vdst.data.cpuset->bitmap != vsrc.data.cpuset->bitmap &&
        hwloc_bitmap_isequal((hwloc_bitmap_t) vsrc.data.cpuset->bitmap,
                             (hwloc_bitmap_t) vdst.data.cpuset->bitmap)) {
        ok = 1;
    } else {
        fprintf(stdout, "    copied cpuset does not match / is not a deep copy\n");
    }

cleanup:
    PMIX_VALUE_DESTRUCT(&vsrc);
    PMIX_VALUE_DESTRUCT(&vdst);
    report("cpuset is deep-copied by PMIx_Value_xfer", ok);
}

/* ------------------------------------------------------------------ */
/* relative locality                                                   */
/* ------------------------------------------------------------------ */

static void test_relative_locality(void)
{
    pmix_locality_t loc;
    pmix_status_t rc;
    int ok = 0;

    /* same NUMA/package/core, different hardware thread */
    rc = PMIx_Get_relative_locality("hwloc:NM0:SK0:CR0:HT0",
                                    "hwloc:NM0:SK0:CR0:HT1", &loc);
    if (PMIX_SUCCESS != rc) {
        fprintf(stdout, "    get_relative_locality failed: %s\n", PMIx_Error_string(rc));
        goto step2;
    }
    if ((loc & PMIX_LOCALITY_SHARE_NUMA) && (loc & PMIX_LOCALITY_SHARE_PACKAGE) &&
        (loc & PMIX_LOCALITY_SHARE_CORE) && !(loc & PMIX_LOCALITY_SHARE_HWTHREAD)) {
        ok = 1;
    } else {
        fprintf(stdout, "    unexpected locality bits: 0x%x\n", (unsigned) loc);
    }
    report("relative locality: shared core, distinct hwthread", ok);

step2:
    /* identical locality strings share everything down to the hwthread */
    ok = 0;
    rc = PMIx_Get_relative_locality("hwloc:NM0:SK0:CR0:HT3",
                                    "hwloc:NM0:SK0:CR0:HT3", &loc);
    if (PMIX_SUCCESS == rc &&
        (loc & PMIX_LOCALITY_SHARE_CORE) && (loc & PMIX_LOCALITY_SHARE_HWTHREAD)) {
        ok = 1;
    } else {
        fprintf(stdout, "    identical locality did not share hwthread: rc=%s bits=0x%x\n",
                PMIx_Error_string(rc), (unsigned) loc);
    }
    report("relative locality: identical strings share hwthread", ok);
}

/* ------------------------------------------------------------------ */

/* Run the full topology datatype battery against one topology. */
static void run_topology_suite(pmix_topology_t *topo, const char *label, int this_system)
{
    test_topology_pack_unpack(topo, label);
    test_topology_pack_unpack_multiple(topo, label);
    test_topology_get_size(topo, label);
    if (this_system) {
        test_topology_print_local(topo);
    } else {
        test_topology_print_imported(topo, label);
    }
}

int main(int argc, char **argv)
{
    pmix_status_t rc;
    pmix_topology_t topo = PMIX_TOPOLOGY_STATIC_INIT;
    PMIX_HIDE_UNUSED_PARAMS(argc, argv);

    rc = PMIx_server_init(&mymodule, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "PMIx_server_init failed: %s\n", PMIx_Error_string(rc));
        return 1;
    }

    fprintf(stdout, "\n=== src/hwloc datatype unit tests ===\n\n");

    /* obtain the local topology for the this-system topology tests */
    rc = PMIx_Load_topology(&topo);
    if (PMIX_SUCCESS != rc || NULL == topo.topology) {
        fprintf(stdout, "SKIP: no hwloc topology available (%s)\n", PMIx_Error_string(rc));
        PMIx_server_finalize();
        return 77;
    }

    /* type-level tests that do not need a specific topology */
    test_cpuset_pack_unpack();
    test_cpuset_copy();
    test_relative_locality();

    /* the machine's own topology */
    run_topology_suite(&topo, "local", 1);

    /* every synthetic topology shipped in test/topologies */
#ifdef PMIX_TEST_TOPO_DIR
    {
        const char *files[] = {"test-topo.xml", "test-topo2.xml"};
        size_t i;
        for (i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
            char path[2048];
            pmix_topology_t ft = PMIX_TOPOLOGY_STATIC_INIT;
            snprintf(path, sizeof(path), "%s/%s", PMIX_TEST_TOPO_DIR, files[i]);
            if (0 != load_topo_file(path, &ft)) {
                fprintf(stdout, "  SKIP: could not load %s\n", path);
                continue;
            }
            run_topology_suite(&ft, files[i], 0);
            PMIx_Topology_destruct(&ft);
        }
    }
#endif

    fprintf(stdout, "\nResults: %d passed, %d failed\n\n", npass, nfail);

    PMIx_server_finalize();

    return (nfail > 0) ? 1 : 0;
}
