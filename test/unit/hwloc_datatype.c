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

/* PMIx_Value_get_size on a PMIX_PROC_CPUSET must report the storage the
 * cpuset actually carries. It used to ignore the cpuset entirely and measure
 * hwloc_bitmap_weight() of a FILLED bitmap - which is infinitely set, so
 * hwloc returns -1, which became SIZE_MAX on the way into a size_t and then
 * wrapped when the caller added sizeof(pmix_cpuset_t). Two different cpusets
 * therefore reported the same absurd size, and the data_array size walker in
 * bfrops overflowed its running total. Assert both properties: the size is
 * sane, and it tracks the content. */
static void test_cpuset_get_size(void)
{
    pmix_value_t val;
    pmix_cpuset_t small, big;
    size_t szsmall = 0, szbig = 0;
    int ok = 0;

    PMIX_CPUSET_CONSTRUCT(&small);
    PMIX_CPUSET_CONSTRUCT(&big);
    if (PMIX_SUCCESS != PMIx_Parse_cpuset_string("hwloc:0", &small) ||
        PMIX_SUCCESS != PMIx_Parse_cpuset_string("hwloc:0-7,16-23,64-71", &big)) {
        report("cpuset size is finite and tracks content (setup)", 0);
        goto cleanup;
    }

    PMIX_VALUE_CONSTRUCT(&val);
    val.type = PMIX_PROC_CPUSET;
    val.data.cpuset = &small;
    if (PMIX_SUCCESS != PMIx_Value_get_size(&val, &szsmall)) {
        fprintf(stdout, "    get_size(small) failed\n");
        goto cleanup;
    }
    val.data.cpuset = &big;
    if (PMIX_SUCCESS != PMIx_Value_get_size(&val, &szbig)) {
        fprintf(stdout, "    get_size(big) failed\n");
        goto cleanup;
    }

    /* PMIx_Value_get_size reports the value struct plus the type's own
     * storage, so the floor is sizeof(pmix_value_t) + sizeof(pmix_cpuset_t).
     * The load-bearing assertion is szbig > szsmall: the old code returned the
     * same (wrapped) number for every cpuset, so any test that only bounded
     * one measurement would have passed against it. */
    if (szsmall > sizeof(pmix_value_t) + sizeof(pmix_cpuset_t) &&
        szbig > szsmall && szbig < 4096) {
        ok = 1;
    } else {
        fprintf(stdout, "    implausible sizes: small=%zu big=%zu (floor=%zu)\n",
                szsmall, szbig, sizeof(pmix_value_t) + sizeof(pmix_cpuset_t));
    }

cleanup:
    PMIX_CPUSET_DESTRUCT(&small);
    PMIX_CPUSET_DESTRUCT(&big);
    report("cpuset size is finite and tracks content", ok);
}

/* An unbound cpuset packs as a NULL string, so it adds nothing beyond the
 * struct - but it must still be reported, not treated as an error. */
static void test_cpuset_get_size_unbound(void)
{
    pmix_value_t val;
    pmix_cpuset_t cpuset;
    size_t want = sizeof(pmix_value_t) + sizeof(pmix_cpuset_t);
    size_t sz = SIZE_MAX;
    int ok;

    PMIX_CPUSET_CONSTRUCT(&cpuset);
    cpuset.source = strdup("hwloc");
    cpuset.bitmap = NULL;

    PMIX_VALUE_CONSTRUCT(&val);
    val.type = PMIX_PROC_CPUSET;
    val.data.cpuset = &cpuset;

    ok = (PMIX_SUCCESS == PMIx_Value_get_size(&val, &sz) && sz == want);
    if (!ok) {
        fprintf(stdout, "    unbound cpuset size=%zu (want %zu)\n", sz, want);
    }
    free(cpuset.source);
    report("unbound cpuset reports just the struct size", ok);
}

/* ------------------------------------------------------------------ */
/* parse_cpuset_string edge cases                                      */
/* ------------------------------------------------------------------ */

/* PMIx_Parse_cpuset_string hands its arguments straight through to
 * src/hwloc with no screening of its own, so anything a caller can pass has
 * to be handled here. A NULL string used to reach strchr and crash. */
static void test_cpuset_parse_bad_input(void)
{
    pmix_cpuset_t cpuset;
    pmix_status_t rc;
    int ok;

    PMIX_CPUSET_CONSTRUCT(&cpuset);
    rc = PMIx_Parse_cpuset_string(NULL, &cpuset);
    ok = (PMIX_SUCCESS != rc);
    report("parse_cpuset_string rejects a NULL string, not dereferenced", ok);

    rc = PMIx_Parse_cpuset_string("hwloc:0-3", NULL);
    ok = (PMIX_SUCCESS != rc);
    report("parse_cpuset_string rejects a NULL cpuset, not dereferenced", ok);

    /* no delimiter at all */
    PMIX_CPUSET_CONSTRUCT(&cpuset);
    rc = PMIx_Parse_cpuset_string("no-delimiter-here", &cpuset);
    ok = (PMIX_SUCCESS != rc);
    report("parse_cpuset_string rejects a string with no delimiter", ok);

    /* another provider's string is "not mine", which is not an error */
    PMIX_CPUSET_CONSTRUCT(&cpuset);
    rc = PMIx_Parse_cpuset_string("someoneelse:0-3", &cpuset);
    ok = (PMIX_ERR_TAKE_NEXT_OPTION == rc);
    if (!ok) {
        fprintf(stdout, "    foreign source gave rc=%s\n", PMIx_Error_string(rc));
    }
    report("parse_cpuset_string passes on a foreign provider's string", ok);

    /* a well-formed prefix with an unparseable payload must leave nothing
     * half-built behind - the caller has no reason to destruct after an
     * error, so anything allocated here would simply leak */
    PMIX_CPUSET_CONSTRUCT(&cpuset);
    rc = PMIx_Parse_cpuset_string("hwloc:not-a-bitmap", &cpuset);
    if (PMIX_SUCCESS == rc) {
        /* hwloc tolerated it; nothing to assert about the failure path */
        PMIX_CPUSET_DESTRUCT(&cpuset);
        ok = 1;
    } else {
        ok = (NULL == cpuset.bitmap && NULL == cpuset.source);
        if (!ok) {
            fprintf(stdout, "    failed parse left bitmap=%p source=%p behind\n",
                    cpuset.bitmap, (void *) cpuset.source);
        }
    }
    report("a failed parse leaves no allocation behind", ok);
}

/* ------------------------------------------------------------------ */
/* printing a topology far wider than the render buffer                */
/* ------------------------------------------------------------------ */

/* The print handler renders each object's cpuset into a fixed stack buffer.
 * It used to declare that buffer half the size it then passed as the length
 * to hwloc_bitmap_snprintf, so a machine with enough PUs to need more than
 * the buffer's real size overran the stack. Build a synthetic topology whose
 * machine-level cpuset renders well past that boundary and confirm the render
 * is both complete and correct - a truncated (or corrupted) render is the
 * observable symptom short of an actual crash. */
static void test_topology_print_wide(void)
{
    pmix_topology_t topo = PMIX_TOPOLOGY_STATIC_INIT;
    hwloc_topology_t t = NULL;
    char *output = NULL;
    char *expected = NULL;
    int len;
    int ok = 0;

    if (0 != hwloc_topology_init(&t)) {
        report("wide topology prints its full cpuset (setup)", 0);
        return;
    }
    /* 4096 PUs -> a machine cpuset rendering to ~1400 characters */
    if (0 != hwloc_topology_set_synthetic(t, "pack:64 core:16 pu:4") ||
        0 != hwloc_topology_load(t)) {
        hwloc_topology_destroy(t);
        fprintf(stdout, "  SKIP: hwloc would not build the synthetic topology\n");
        return;
    }
    topo.source = strdup("hwloc");
    topo.topology = (void *) t;

    /* what a correct render of the root cpuset looks like */
    len = hwloc_bitmap_snprintf(NULL, 0, hwloc_get_root_obj(t)->cpuset);
    if (0 >= len) {
        goto cleanup;
    }
    expected = malloc((size_t) len + 1);
    if (NULL == expected) {
        goto cleanup;
    }
    hwloc_bitmap_snprintf(expected, (size_t) len + 1, hwloc_get_root_obj(t)->cpuset);
    if (1024 >= len) {
        fprintf(stdout, "  NOTE: synthetic cpuset is only %d chars - not past the old bound\n",
                len);
    }

    if (PMIX_SUCCESS != PMIx_Data_print(&output, NULL, &topo, PMIX_TOPO) || NULL == output) {
        fprintf(stdout, "    print of the wide topology failed\n");
        goto cleanup;
    }
    if (NULL != strstr(output, expected)) {
        ok = 1;
    } else {
        fprintf(stdout, "    %d-char cpuset was not rendered intact\n", len);
    }

cleanup:
    free(expected);
    free(output);
    PMIx_Topology_destruct(&topo);
    report("wide topology prints its full cpuset", ok);
}

/* ------------------------------------------------------------------ */
/* compute_distances argument screening                                */
/* ------------------------------------------------------------------ */

static void test_compute_distances_bad_params(void)
{
    pmix_device_distance_t *dist = (pmix_device_distance_t *) 0x1;
    size_t ndist = 1;
    pmix_cpuset_t cpuset;
    pmix_status_t rc;
    int ok;

    PMIX_CPUSET_CONSTRUCT(&cpuset);
    if (PMIX_SUCCESS != PMIx_Parse_cpuset_string("hwloc:0", &cpuset)) {
        report("compute_distances screens its arguments (setup)", 0);
        return;
    }

    /* a NULL topology must be reported, not dereferenced */
    rc = PMIx_Compute_distances(NULL, &cpuset, NULL, 0, &dist, &ndist);
    ok = (PMIX_SUCCESS != rc);
    report("compute_distances rejects a NULL topology", ok);

    PMIX_CPUSET_DESTRUCT(&cpuset);
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

    /* The bare, unprefixed form -- which is what
     * PMIx_server_generate_locality_string actually emits and what a host
     * environment stores as PMIX_LOCALITY_STRING. The literals above all
     * carry an "hwloc:" prefix, which is exactly why this gap survived: the
     * consumer demanded a prefix the producer never wrote, so the usage
     * documented for PMIx_Get_relative_locality returned
     * PMIX_ERR_TAKE_NEXT_OPTION and no locality at all. */
    ok = 0;
    rc = PMIx_Get_relative_locality("NM0:SK0:CR0:HT0", "NM0:SK0:CR0:HT1", &loc);
    if (PMIX_SUCCESS == rc &&
        (loc & PMIX_LOCALITY_SHARE_NUMA) && (loc & PMIX_LOCALITY_SHARE_PACKAGE) &&
        (loc & PMIX_LOCALITY_SHARE_CORE) && !(loc & PMIX_LOCALITY_SHARE_HWTHREAD)) {
        ok = 1;
    } else {
        fprintf(stdout, "    unprefixed locality rejected: rc=%s bits=0x%x\n",
                PMIx_Error_string(rc), (unsigned) loc);
    }
    report("relative locality: unprefixed strings are accepted", ok);

    /* both spellings must yield the same answer */
    ok = 0;
    {
        pmix_locality_t bare = 0, pref = 0;
        pmix_status_t rc1, rc2;
        rc1 = PMIx_Get_relative_locality("SK0:L20:L10:CR1:HT1:NM0",
                                         "SK0:L20:L10:CR0:HT0:NM0", &bare);
        rc2 = PMIx_Get_relative_locality("hwloc:SK0:L20:L10:CR1:HT1:NM0",
                                         "hwloc:SK0:L20:L10:CR0:HT0:NM0", &pref);
        if (PMIX_SUCCESS == rc1 && PMIX_SUCCESS == rc2 && bare == pref && 0 != bare) {
            ok = 1;
        } else {
            fprintf(stdout, "    bare=0x%x (%s) vs prefixed=0x%x (%s)\n",
                    (unsigned) bare, PMIx_Error_string(rc1),
                    (unsigned) pref, PMIx_Error_string(rc2));
        }
    }
    report("relative locality: bare and prefixed agree", ok);

    /* a string belonging to some other provider must still be passed along,
     * not misparsed as one of ours */
    ok = 0;
    rc = PMIx_Get_relative_locality("someoneelse:NM0:SK0",
                                    "someoneelse:NM0:SK0", &loc);
    if (PMIX_ERR_TAKE_NEXT_OPTION == rc) {
        ok = 1;
    } else {
        fprintf(stdout, "    foreign locality was not passed on: rc=%s\n",
                PMIx_Error_string(rc));
    }
    report("relative locality: a foreign provider's string is passed on", ok);

    /* and a NULL must be reported rather than dereferenced */
    ok = 0;
    rc = PMIx_Get_relative_locality(NULL, "NM0:SK0", &loc);
    if (PMIX_SUCCESS != rc) {
        ok = 1;
    }
    report("relative locality: NULL input is rejected, not dereferenced", ok);
}

/* ------------------------------------------------------------------ */
/* malformed cpusets handed to the string generators                   */
/* ------------------------------------------------------------------ */

/* A cpuset carries a "source" naming the provider that produced its
 * bitmap. A NULL source is legal on an input cpuset the caller wants
 * filled in, but not on one we are asked to serialize or interpret:
 * both generators used to read it with strncasecmp before checking it,
 * so a host handing over a bitmap it had built itself - or simply a
 * zeroed struct - crashed the library. They must report an error. */
static void test_cpuset_string_bad_source(void)
{
    pmix_cpuset_t cpuset;
    pmix_status_t rc;
    char *str;
    int ok;

    /* a bitmap with no source at all */
    PMIX_CPUSET_CONSTRUCT(&cpuset);
    if (PMIX_SUCCESS != PMIx_Parse_cpuset_string("hwloc:0-3", &cpuset)) {
        report("cpuset generators reject a NULL source (setup)", 0);
        return;
    }
    free(cpuset.source);
    cpuset.source = NULL;

    str = (char *) 0x1;
    rc = PMIx_server_generate_cpuset_string(&cpuset, &str);
    ok = (PMIX_SUCCESS != rc && NULL == str);
    if (!ok) {
        fprintf(stdout, "    generate_cpuset_string(NULL source): rc=%s str=%p\n",
                PMIx_Error_string(rc), (void *) str);
    }
    report("generate_cpuset_string rejects a NULL source", ok);

    str = (char *) 0x1;
    rc = PMIx_server_generate_locality_string(&cpuset, &str);
    ok = (PMIX_SUCCESS != rc && NULL == str);
    if (!ok) {
        fprintf(stdout, "    generate_locality_string(NULL source): rc=%s str=%p\n",
                PMIx_Error_string(rc), (void *) str);
    }
    report("generate_locality_string rejects a NULL source", ok);

    /* restore a source so the destructor reclaims the bitmap */
    cpuset.source = strdup("hwloc");
    PMIX_CPUSET_DESTRUCT(&cpuset);

    /* a NULL cpuset pointer */
    str = (char *) 0x1;
    rc = PMIx_server_generate_cpuset_string(NULL, &str);
    ok = (PMIX_SUCCESS != rc && NULL == str);
    report("generate_cpuset_string rejects a NULL cpuset", ok);

    str = (char *) 0x1;
    rc = PMIx_server_generate_locality_string(NULL, &str);
    ok = (PMIX_SUCCESS != rc && NULL == str);
    report("generate_locality_string rejects a NULL cpuset", ok);

    /* a source naming some other provider - not an error, but the
     * output pointer must still not be left holding garbage */
    PMIX_CPUSET_CONSTRUCT(&cpuset);
    if (PMIX_SUCCESS != PMIx_Parse_cpuset_string("hwloc:0-3", &cpuset)) {
        report("cpuset generators handle a foreign source (setup)", 0);
        return;
    }
    free(cpuset.source);
    cpuset.source = strdup("someoneelse");

    str = (char *) 0x1;
    rc = PMIx_server_generate_cpuset_string(&cpuset, &str);
    ok = (PMIX_SUCCESS != rc && NULL == str);
    report("generate_cpuset_string passes on a foreign source", ok);

    str = (char *) 0x1;
    rc = PMIx_server_generate_locality_string(&cpuset, &str);
    ok = (PMIX_SUCCESS != rc && NULL == str);
    report("generate_locality_string passes on a foreign source", ok);

    /* the destructor only reclaims what it recognizes as its own */
    hwloc_bitmap_free((hwloc_bitmap_t) cpuset.bitmap);
    free(cpuset.source);
}

/* The producer/consumer pair, driven end to end with no literals in the
 * middle. The unit test missed the prefix mismatch between
 * PMIx_server_generate_locality_string and PMIx_Get_relative_locality for
 * years precisely because it hand-wrote "hwloc:NM0:..." strings that matched
 * what the consumer wanted rather than what the producer emits. Build the
 * cpusets here, run them through the real generator, and hand the generator's
 * own output to the consumer. */
static void test_locality_generator_to_consumer(void)
{
    pmix_cpuset_t c1, c2;
    char *loc1 = NULL, *loc2 = NULL;
    pmix_locality_t bits = 0;
    pmix_status_t rc;
    int ok = 0;

    PMIX_CPUSET_CONSTRUCT(&c1);
    PMIX_CPUSET_CONSTRUCT(&c2);

    /* two single-PU cpusets on this machine's own topology */
    if (PMIX_SUCCESS != PMIx_Parse_cpuset_string("hwloc:0", &c1) ||
        PMIX_SUCCESS != PMIx_Parse_cpuset_string("hwloc:0", &c2)) {
        report("generator output is accepted by get_relative_locality (setup)", 0);
        goto cleanup;
    }

    rc = PMIx_server_generate_locality_string(&c1, &loc1);
    if (PMIX_SUCCESS != rc || NULL == loc1) {
        fprintf(stdout, "  SKIP: no locality for cpu 0 on this machine (rc=%s)\n",
                PMIx_Error_string(rc));
        goto cleanup;
    }
    rc = PMIx_server_generate_locality_string(&c2, &loc2);
    if (PMIX_SUCCESS != rc || NULL == loc2) {
        goto cleanup;
    }

    /* the generator writes a bare, unprefixed token list - if that ever
     * changes, the assertion below is the thing that should be revisited,
     * not quietly relaxed */
    if (0 == strncasecmp(loc1, "hwloc:", 6)) {
        fprintf(stdout, "  NOTE: generator now emits a prefixed string (%s)\n", loc1);
    }

    rc = PMIx_Get_relative_locality(loc1, loc2, &bits);
    if (PMIX_SUCCESS == rc && (bits & PMIX_LOCALITY_SHARE_NODE) &&
        (bits & PMIX_LOCALITY_SHARE_HWTHREAD)) {
        ok = 1;
    } else {
        fprintf(stdout, "    generator output '%s' rejected/misread: rc=%s bits=0x%x\n",
                loc1, PMIx_Error_string(rc), (unsigned) bits);
    }

cleanup:
    free(loc1);
    free(loc2);
    PMIX_CPUSET_DESTRUCT(&c1);
    PMIX_CPUSET_DESTRUCT(&c2);
    report("generator output is accepted by get_relative_locality", ok);
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
    test_cpuset_get_size();
    test_cpuset_get_size_unbound();
    test_cpuset_parse_bad_input();
    test_cpuset_string_bad_source();
    test_relative_locality();
    test_locality_generator_to_consumer();
    test_compute_distances_bad_params();
    test_topology_print_wide();

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
