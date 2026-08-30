/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Unit tests for the GDS datastore: the framework's module-selection
 * entry points, the base modex envelope walker, and the assigned
 * module's cache/store/fetch behavior.
 *
 * The component-internal symbols (pmix_gds_hash_*) are not exported from
 * libpmix, so everything here drives the *assigned module* the way the
 * rest of the library does: through the PMIX_GDS_* macros in gds.h,
 * which call through the module's function-pointer table, and through
 * PMIx_server_register_nspace(), which reaches cache_job_info.
 *
 * Each case names the specific behavior it pins down. Several of them
 * cover inputs that arrive from a host or a peer and were previously
 * taken on trust:
 *
 *   - a PMIX_GDS_MODULE directive whose value is not a usable string
 *     (module selection split it and walked the result unconditionally),
 *   - a node or proc map presented as PMIX_STRING or PMIX_REGEX2 rather
 *     than PMIX_REGEX, inside a PMIX_JOB_INFO_ARRAY (only the byte-object
 *     form was decoded there),
 *   - a PMIX_NODE_INFO_ARRAY that identifies its node by nodeid alone,
 *     followed by a hostname-qualified lookup (the stored NULL hostname
 *     reached strcmp),
 *   - a PMIX_QUALIFIED_VALUE carrying an empty data array (the primary
 *     value was read out of bounds and SIZE_MAX qualifiers requested).
 *
 * A different class of case covers what the datastore *computes* rather
 * than what it is given: the per-proc hostname, nodeid, local rank and
 * node rank derived from the node and proc maps are assumptions, so the
 * host's own PMIX_PROC_INFO_ARRAY has to win - but per rank and per key.
 * That decision used to be made once for the whole job, which lost those
 * keys for every proc a host did not fully describe.
 *
 * The base modex walker case pins the contract every component's
 * store_modex callback has to honor: it is called once per proc blob and
 * then once per involved nspace with a NULL buffer, and a callback that
 * reports anything but PMIX_SUCCESS for a blob it consumed successfully
 * silently truncates the rest of that server's contribution.
 */

#include "src/include/pmix_config.h"

#include "include/pmix.h"
#include "include/pmix_server.h"
#include "src/include/pmix_globals.h"

#include "src/class/pmix_list.h"
#include "src/mca/bfrops/bfrops.h"
#include "src/mca/gds/base/base.h"
#include "src/mca/gds/gds.h"
#include "src/server/pmix_server_ops.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int npass = 0;
static int nfail = 0;

static void report(const char *name, int passed)
{
    if (passed) {
        fprintf(stdout, "  PASS: %s\n", name);
        ++npass;
    } else {
        fprintf(stdout, "  FAIL: %s\n", name);
        ++nfail;
    }
}

static pmix_server_module_t mymodule = {0};

/* PMIx_server_register_nspace() reports a request it was able to satisfy
 * without deferring as PMIX_OPERATION_SUCCEEDED rather than
 * PMIX_SUCCESS. Both mean the job was registered. */
static bool registered(pmix_status_t rc)
{
    return (PMIX_SUCCESS == rc || PMIX_OPERATION_SUCCEEDED == rc);
}

/* ------------------------------------------------------------------ */
/* module selection                                                     */
/* ------------------------------------------------------------------ */

/* pmix_gds_base_assign_module() hands each module's assign_module the
 * caller's directives. A PMIX_GDS_MODULE directive reaches us from the
 * application or from the environment, so its value is not guaranteed to
 * be the string the attribute calls for. Every one of these must still
 * resolve to a module rather than dereferencing a NULL split result. */
static void test_assign_module(void)
{
    pmix_gds_base_module_t *mod;
    pmix_info_t info;
    uint32_t bogus = 42;

    fprintf(stdout, "\n-- module selection --\n");

    mod = pmix_gds_base_assign_module(NULL, 0);
    report("no directives resolves to a module", NULL != mod);

    /* the well-formed request, so the malformed cases below are being
     * compared against something known to work */
    PMIX_INFO_LOAD(&info, PMIX_GDS_MODULE, "hash", PMIX_STRING);
    mod = pmix_gds_base_assign_module(&info, 1);
    report("PMIX_GDS_MODULE=\"hash\" selects hash",
           NULL != mod && 0 == strcmp(mod->name, "hash"));
    PMIX_INFO_DESTRUCT(&info);

    /* a string value that splits to nothing: PMIx_Argv_split("") returns
     * NULL, which was then indexed */
    PMIX_INFO_LOAD(&info, PMIX_GDS_MODULE, "", PMIX_STRING);
    mod = pmix_gds_base_assign_module(&info, 1);
    report("PMIX_GDS_MODULE=\"\" resolves to a module", NULL != mod);
    PMIX_INFO_DESTRUCT(&info);

    /* a string of nothing but delimiters splits to nothing as well */
    PMIX_INFO_LOAD(&info, PMIX_GDS_MODULE, ",,,", PMIX_STRING);
    mod = pmix_gds_base_assign_module(&info, 1);
    report("PMIX_GDS_MODULE=\",,,\" resolves to a module", NULL != mod);
    PMIX_INFO_DESTRUCT(&info);

    /* declared PMIX_STRING but carrying no string */
    PMIX_INFO_LOAD(&info, PMIX_GDS_MODULE, NULL, PMIX_STRING);
    mod = pmix_gds_base_assign_module(&info, 1);
    report("PMIX_GDS_MODULE with a NULL string resolves to a module",
           NULL != mod);
    PMIX_INFO_DESTRUCT(&info);

    /* the wrong type entirely - data.string would be a small integer */
    PMIX_INFO_LOAD(&info, PMIX_GDS_MODULE, &bogus, PMIX_UINT32);
    mod = pmix_gds_base_assign_module(&info, 1);
    report("PMIX_GDS_MODULE of the wrong type resolves to a module",
           NULL != mod);
    PMIX_INFO_DESTRUCT(&info);

    /* a name no component answers to still has to produce a module -
     * every module offers itself, a match only raises its bid */
    PMIX_INFO_LOAD(&info, PMIX_GDS_MODULE, "no-such-gds-module", PMIX_STRING);
    mod = pmix_gds_base_assign_module(&info, 1);
    report("an unknown PMIX_GDS_MODULE name resolves to a module",
           NULL != mod);
    PMIX_INFO_DESTRUCT(&info);
}

/* ------------------------------------------------------------------ */
/* the base modex envelope walker                                       */
/* ------------------------------------------------------------------ */

static size_t modex_nblobs = 0;   /* callbacks carrying a buffer */
static size_t modex_ndone = 0;    /* callbacks carrying NULL     */
static pmix_rank_t modex_ranks[8];
static char modex_nspaces[8][PMIX_MAX_NSLEN + 1];
static pmix_status_t modex_cb_rc = PMIX_SUCCESS;
static pmix_collect_t modex_last_kind = PMIX_COLLECT_INVALID;

static pmix_status_t modex_cb(pmix_proc_t *proc, pmix_buffer_t *pbkt, uint8_t kind)
{
    modex_last_kind = (pmix_collect_t) kind;
    if (NULL == pbkt) {
        ++modex_ndone;
        return PMIX_SUCCESS;
    }
    if (modex_nblobs < sizeof(modex_ranks) / sizeof(modex_ranks[0])) {
        modex_ranks[modex_nblobs] = proc->rank;
        pmix_strncpy(modex_nspaces[modex_nblobs], proc->nspace, PMIX_MAX_NSLEN);
    }
    ++modex_nblobs;
    return modex_cb_rc;
}

/* true if every blob the callback saw came from the named nspace */
static bool modex_all_from(const char *nspace)
{
    size_t n, lim = modex_nblobs;

    if (lim > sizeof(modex_ranks) / sizeof(modex_ranks[0])) {
        lim = sizeof(modex_ranks) / sizeof(modex_ranks[0]);
    }
    for (n = 0; n < lim; n++) {
        if (0 != strcmp(modex_nspaces[n], nspace)) {
            return false;
        }
    }
    return true;
}

/* Build the envelope pmix_gds_base_store_modex() expects: an outer byte
 * object per contributing server, holding a compression flag and a byte
 * object of rank-level data; that inner object holds a collect-flag byte
 * followed by one byte object per contributing proc. */
static pmix_status_t build_modex(pmix_buffer_t *out, const char *nspace,
                                 pmix_rank_t *ranks, size_t nranks,
                                 uint8_t collect)
{
    pmix_buffer_t ranklevel, serverlevel, pbkt;
    pmix_byte_object_t bo;
    pmix_status_t rc;
    pmix_proc_t proc;
    bool compressed = false;
    size_t n;

    PMIX_CONSTRUCT(&ranklevel, pmix_buffer_t);
    PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, &ranklevel, &collect, 1, PMIX_BYTE);
    if (PMIX_SUCCESS != rc) {
        PMIX_DESTRUCT(&ranklevel);
        return rc;
    }
    for (n = 0; n < nranks; n++) {
        PMIX_LOAD_PROCID(&proc, nspace, ranks[n]);
        PMIX_CONSTRUCT(&pbkt, pmix_buffer_t);
        PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, &pbkt, &proc, 1, PMIX_PROC);
        if (PMIX_SUCCESS != rc) {
            PMIX_DESTRUCT(&pbkt);
            PMIX_DESTRUCT(&ranklevel);
            return rc;
        }
        PMIX_UNLOAD_BUFFER(&pbkt, bo.bytes, bo.size);
        PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, &ranklevel, &bo, 1, PMIX_BYTE_OBJECT);
        PMIX_BYTE_OBJECT_DESTRUCT(&bo);
        PMIX_DESTRUCT(&pbkt);
        if (PMIX_SUCCESS != rc) {
            PMIX_DESTRUCT(&ranklevel);
            return rc;
        }
    }

    PMIX_CONSTRUCT(&serverlevel, pmix_buffer_t);
    PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, &serverlevel, &compressed, 1, PMIX_BOOL);
    if (PMIX_SUCCESS != rc) {
        PMIX_DESTRUCT(&serverlevel);
        PMIX_DESTRUCT(&ranklevel);
        return rc;
    }
    PMIX_UNLOAD_BUFFER(&ranklevel, bo.bytes, bo.size);
    PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, &serverlevel, &bo, 1, PMIX_BYTE_OBJECT);
    PMIX_BYTE_OBJECT_DESTRUCT(&bo);
    PMIX_DESTRUCT(&ranklevel);
    if (PMIX_SUCCESS != rc) {
        PMIX_DESTRUCT(&serverlevel);
        return rc;
    }

    PMIX_UNLOAD_BUFFER(&serverlevel, bo.bytes, bo.size);
    PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, out, &bo, 1, PMIX_BYTE_OBJECT);
    PMIX_BYTE_OBJECT_DESTRUCT(&bo);
    PMIX_DESTRUCT(&serverlevel);
    return rc;
}

/* Same envelope as build_modex(), but each proc blob carries one
 * key/value pair after the proc - which is what a real contribution
 * looks like, and what a fetch needs in order to have anything to find.
 * The blob format after the proc is pmix_kval_t until end of buffer;
 * see server_store_modex_cb() in gds/shmem3. */
static pmix_status_t build_modex_kv(pmix_buffer_t *out, const char *nspace,
                                    pmix_rank_t *ranks, size_t nranks,
                                    const char *key, uint32_t value,
                                    uint8_t collect)
{
    pmix_buffer_t ranklevel, serverlevel, pbkt;
    pmix_byte_object_t bo;
    pmix_status_t rc;
    pmix_proc_t proc;
    pmix_kval_t kv;
    pmix_value_t val;
    bool compressed = false;
    size_t n;

    PMIX_CONSTRUCT(&ranklevel, pmix_buffer_t);
    PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, &ranklevel, &collect, 1, PMIX_BYTE);
    if (PMIX_SUCCESS != rc) {
        PMIX_DESTRUCT(&ranklevel);
        return rc;
    }
    for (n = 0; n < nranks; n++) {
        PMIX_LOAD_PROCID(&proc, nspace, ranks[n]);
        PMIX_CONSTRUCT(&pbkt, pmix_buffer_t);
        PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, &pbkt, &proc, 1, PMIX_PROC);
        if (PMIX_SUCCESS == rc) {
            PMIX_VALUE_LOAD(&val, &value, PMIX_UINT32);
            kv.key = (char *) key;
            kv.value = &val;
            PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, &pbkt, &kv, 1, PMIX_KVAL);
            PMIX_VALUE_DESTRUCT(&val);
        }
        if (PMIX_SUCCESS != rc) {
            PMIX_DESTRUCT(&pbkt);
            PMIX_DESTRUCT(&ranklevel);
            return rc;
        }
        PMIX_UNLOAD_BUFFER(&pbkt, bo.bytes, bo.size);
        PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, &ranklevel, &bo, 1, PMIX_BYTE_OBJECT);
        PMIX_BYTE_OBJECT_DESTRUCT(&bo);
        PMIX_DESTRUCT(&pbkt);
        if (PMIX_SUCCESS != rc) {
            PMIX_DESTRUCT(&ranklevel);
            return rc;
        }
    }

    PMIX_CONSTRUCT(&serverlevel, pmix_buffer_t);
    PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, &serverlevel, &compressed, 1, PMIX_BOOL);
    if (PMIX_SUCCESS != rc) {
        PMIX_DESTRUCT(&serverlevel);
        PMIX_DESTRUCT(&ranklevel);
        return rc;
    }
    PMIX_UNLOAD_BUFFER(&ranklevel, bo.bytes, bo.size);
    PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, &serverlevel, &bo, 1, PMIX_BYTE_OBJECT);
    PMIX_BYTE_OBJECT_DESTRUCT(&bo);
    PMIX_DESTRUCT(&ranklevel);
    if (PMIX_SUCCESS != rc) {
        PMIX_DESTRUCT(&serverlevel);
        return rc;
    }

    PMIX_UNLOAD_BUFFER(&serverlevel, bo.bytes, bo.size);
    PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, out, &bo, 1, PMIX_BYTE_OBJECT);
    PMIX_BYTE_OBJECT_DESTRUCT(&bo);
    PMIX_DESTRUCT(&serverlevel);
    return rc;
}

/* Same envelope, but the rank-level block carries procs from more than
 * one nspace - which is what a fence spanning two local nspaces
 * produces, and what the nspace filter has to sort out. */
static pmix_status_t build_modex_mixed(pmix_buffer_t *out,
                                       pmix_proc_t *procs, size_t nprocs)
{
    pmix_buffer_t ranklevel, serverlevel, pbkt;
    pmix_byte_object_t bo;
    pmix_status_t rc;
    uint8_t collect = 1;
    bool compressed = false;
    size_t n;

    PMIX_CONSTRUCT(&ranklevel, pmix_buffer_t);
    PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, &ranklevel, &collect, 1, PMIX_BYTE);
    if (PMIX_SUCCESS != rc) {
        PMIX_DESTRUCT(&ranklevel);
        return rc;
    }
    for (n = 0; n < nprocs; n++) {
        PMIX_CONSTRUCT(&pbkt, pmix_buffer_t);
        PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, &pbkt, &procs[n], 1, PMIX_PROC);
        if (PMIX_SUCCESS != rc) {
            PMIX_DESTRUCT(&pbkt);
            PMIX_DESTRUCT(&ranklevel);
            return rc;
        }
        PMIX_UNLOAD_BUFFER(&pbkt, bo.bytes, bo.size);
        PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, &ranklevel, &bo, 1, PMIX_BYTE_OBJECT);
        PMIX_BYTE_OBJECT_DESTRUCT(&bo);
        PMIX_DESTRUCT(&pbkt);
        if (PMIX_SUCCESS != rc) {
            PMIX_DESTRUCT(&ranklevel);
            return rc;
        }
    }

    PMIX_CONSTRUCT(&serverlevel, pmix_buffer_t);
    PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, &serverlevel, &compressed, 1, PMIX_BOOL);
    if (PMIX_SUCCESS != rc) {
        PMIX_DESTRUCT(&serverlevel);
        PMIX_DESTRUCT(&ranklevel);
        return rc;
    }
    PMIX_UNLOAD_BUFFER(&ranklevel, bo.bytes, bo.size);
    PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, &serverlevel, &bo, 1, PMIX_BYTE_OBJECT);
    PMIX_BYTE_OBJECT_DESTRUCT(&bo);
    PMIX_DESTRUCT(&ranklevel);
    if (PMIX_SUCCESS != rc) {
        PMIX_DESTRUCT(&serverlevel);
        return rc;
    }

    PMIX_UNLOAD_BUFFER(&serverlevel, bo.bytes, bo.size);
    PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, out, &bo, 1, PMIX_BYTE_OBJECT);
    PMIX_BYTE_OBJECT_DESTRUCT(&bo);
    PMIX_DESTRUCT(&serverlevel);
    return rc;
}

/* A fence can span local nspaces that were assigned different gds
 * modules, so the same payload is walked once per nspace, each pass
 * storing only that nspace's blobs. Passing NULL still stores
 * everything, which is what a single-module caller wants. */
static void test_store_modex_nspace_filter(void)
{
    pmix_buffer_t buf;
    pmix_server_trkr_t trk;
    pmix_proc_t procs[4];
    pmix_status_t rc;

    fprintf(stdout, "\n-- modex walker: per-nspace filtering --\n");

    memset(&trk, 0, sizeof(trk));
    trk.collect_type = PMIX_COLLECT_YES;

    PMIX_LOAD_PROCID(&procs[0], "gds-modex-nsA", 0);
    PMIX_LOAD_PROCID(&procs[1], "gds-modex-nsB", 0);
    PMIX_LOAD_PROCID(&procs[2], "gds-modex-nsA", 1);
    PMIX_LOAD_PROCID(&procs[3], "gds-modex-nsB", 1);

    /* filtering to one nspace delivers only its blobs, and signals done
     * once - for that nspace alone */
    modex_nblobs = modex_ndone = 0;
    modex_cb_rc = PMIX_SUCCESS;
    PMIX_CONSTRUCT(&buf, pmix_buffer_t);
    rc = build_modex_mixed(&buf, procs, 4);
    report("mixed-nspace envelope builds", PMIX_SUCCESS == rc);
    if (PMIX_SUCCESS == rc) {
        rc = pmix_gds_base_store_modex(&buf, "gds-modex-nsA", modex_cb, &trk);
        report("filtered store_modex succeeds", PMIX_SUCCESS == rc);
        report("filtered store_modex delivers only that nspace's blobs",
               2 == modex_nblobs);
        report("filtered store_modex delivers no other nspace",
               modex_all_from("gds-modex-nsA"));
        report("filtered store_modex signals done once", 1 == modex_ndone);
    }
    PMIX_DESTRUCT(&buf);

    /* the other nspace's blobs are still reachable on its own pass -
     * i.e. filtering does not consume or skip past them permanently */
    modex_nblobs = modex_ndone = 0;
    PMIX_CONSTRUCT(&buf, pmix_buffer_t);
    rc = build_modex_mixed(&buf, procs, 4);
    if (PMIX_SUCCESS == rc) {
        rc = pmix_gds_base_store_modex(&buf, "gds-modex-nsB", modex_cb, &trk);
        report("the second nspace's pass sees its own blobs",
               PMIX_SUCCESS == rc && 2 == modex_nblobs
                   && modex_all_from("gds-modex-nsB"));
    }
    PMIX_DESTRUCT(&buf);

    /* a NULL filter stores everything, and signals done once per nspace */
    modex_nblobs = modex_ndone = 0;
    PMIX_CONSTRUCT(&buf, pmix_buffer_t);
    rc = build_modex_mixed(&buf, procs, 4);
    if (PMIX_SUCCESS == rc) {
        rc = pmix_gds_base_store_modex(&buf, NULL, modex_cb, &trk);
        report("an unfiltered walk delivers every blob",
               PMIX_SUCCESS == rc && 4 == modex_nblobs);
        report("an unfiltered walk signals done once per nspace",
               2 == modex_ndone);
    }
    PMIX_DESTRUCT(&buf);

    /* a filter naming an nspace not present delivers nothing at all */
    modex_nblobs = modex_ndone = 0;
    PMIX_CONSTRUCT(&buf, pmix_buffer_t);
    rc = build_modex_mixed(&buf, procs, 4);
    if (PMIX_SUCCESS == rc) {
        rc = pmix_gds_base_store_modex(&buf, "gds-modex-absent", modex_cb, &trk);
        report("a filter matching no blob delivers none",
               PMIX_SUCCESS == rc && 0 == modex_nblobs && 0 == modex_ndone);
    }
    PMIX_DESTRUCT(&buf);
}

/* The per-server flag byte says what kind of contribution follows. Until
 * August 2026 the walker only asked whether the servers agreed with each
 * other, so a value they all agreed on and none of us could act on went
 * straight through and its blobs were stored as though they were an
 * ordinary full contribution.
 *
 * That matters because of what a PMIX_MODEX_DELTA contribution is: only
 * what the sender published since it last took part in a collecting
 * fence. Storing one as a full set drops every key the sender left out,
 * and for gds/shmem3 - which retires the previous modex generation on
 * the strength of the new one standing alone - drops that generation
 * too. Nothing emits it yet (openpmix#4087); what is pinned here is that
 * a peer which does emit one is refused rather than half-believed.
 *
 * Note each case asserts the *status*, not the blob count: a walk that
 * stops has usually delivered some blobs already, and how many is not
 * the contract. */
static void test_store_modex_blob_info(void)
{
    pmix_buffer_t buf;
    pmix_server_trkr_t trk;
    pmix_rank_t ranks[1] = {0};
    pmix_status_t rc;

    fprintf(stdout, "\n-- modex walker: the per-server flag byte --\n");

    memset(&trk, 0, sizeof(trk));
    trk.collect_type = PMIX_COLLECT_YES;

    /* the ordinary value still works - so the screens below are being
     * compared against something known to pass through them */
    modex_nblobs = modex_ndone = 0;
    modex_cb_rc = PMIX_SUCCESS;
    PMIX_CONSTRUCT(&buf, pmix_buffer_t);
    rc = build_modex(&buf, "gds-modex-ns", ranks, 1, PMIX_COLLECT_YES);
    if (PMIX_SUCCESS == rc) {
        rc = pmix_gds_base_store_modex(&buf, NULL, modex_cb, &trk);
        report("PMIX_COLLECT_YES is still stored",
               PMIX_SUCCESS == rc && 1 == modex_nblobs);
        report("the callback is told it was cumulative",
               PMIX_COLLECT_YES == modex_last_kind);
    }
    PMIX_DESTRUCT(&buf);

    /* a delta contribution is stored, and the kind reaches the callback
     * so a datastore that retires what an earlier modex left behind can
     * tell the difference */
    modex_nblobs = modex_ndone = 0;
    modex_last_kind = PMIX_COLLECT_INVALID;
    PMIX_CONSTRUCT(&buf, pmix_buffer_t);
    rc = build_modex(&buf, "gds-modex-ns", ranks, 1, PMIX_MODEX_DELTA);
    if (PMIX_SUCCESS == rc) {
        rc = pmix_gds_base_store_modex(&buf, NULL, modex_cb, &trk);
        report("a delta contribution is stored",
               PMIX_SUCCESS == rc && 1 == modex_nblobs);
        report("the callback is told it was a delta",
               PMIX_MODEX_DELTA == modex_last_kind);
    }
    PMIX_DESTRUCT(&buf);

    /* a value no release ever defined */
    modex_nblobs = modex_ndone = 0;
    PMIX_CONSTRUCT(&buf, pmix_buffer_t);
    rc = build_modex(&buf, "gds-modex-ns", ranks, 1, 99);
    if (PMIX_SUCCESS == rc) {
        rc = pmix_gds_base_store_modex(&buf, NULL, modex_cb, &trk);
        report("an undefined flag byte is refused", PMIX_ERR_BAD_PARAM == rc);
    }
    PMIX_DESTRUCT(&buf);

    /* Two servers disagreeing. This is the arm that makes a mixed-version
     * job fail loudly rather than silently: it is already implemented, so
     * an *older* receiver rejects a delta from a newer peer with no
     * change to its own code. Appending two single-server envelopes to
     * one buffer is exactly what the host's all-gather produces. */
    modex_nblobs = modex_ndone = 0;
    PMIX_CONSTRUCT(&buf, pmix_buffer_t);
    rc = build_modex(&buf, "gds-modex-ns", ranks, 1, PMIX_COLLECT_YES);
    if (PMIX_SUCCESS == rc) {
        rc = build_modex(&buf, "gds-modex-ns", ranks, 1, PMIX_COLLECT_NO);
    }
    if (PMIX_SUCCESS == rc) {
        rc = pmix_gds_base_store_modex(&buf, NULL, modex_cb, &trk);
        report("servers disagreeing about the flag byte is refused",
               PMIX_ERR_BAD_PARAM == rc);
    }
    PMIX_DESTRUCT(&buf);
}

static void test_store_modex(void)
{
    pmix_buffer_t buf;
    pmix_server_trkr_t trk;
    pmix_rank_t ranks[3] = {0, 1, 2};
    pmix_status_t rc;

    fprintf(stdout, "\n-- base modex envelope walker --\n");

    memset(&trk, 0, sizeof(trk));
    trk.collect_type = PMIX_COLLECT_YES;

    /* a callback that reports success for each blob sees every proc */
    modex_nblobs = modex_ndone = 0;
    modex_cb_rc = PMIX_SUCCESS;
    PMIX_CONSTRUCT(&buf, pmix_buffer_t);
    rc = build_modex(&buf, "gds-modex-ns", ranks, 3, PMIX_COLLECT_YES);
    report("modex envelope builds", PMIX_SUCCESS == rc);
    if (PMIX_SUCCESS == rc) {
        rc = pmix_gds_base_store_modex(&buf, NULL, modex_cb, &trk);
        report("store_modex walks the envelope successfully", PMIX_SUCCESS == rc);
        report("store_modex delivers every proc blob", 3 == modex_nblobs);
        report("store_modex delivers rank 0 first",
               modex_nblobs > 0 && 0 == modex_ranks[0]);
        report("store_modex delivers rank 2 last",
               modex_nblobs > 2 && 2 == modex_ranks[2]);
        report("store_modex signals done once for the one nspace",
               1 == modex_ndone);
    }
    PMIX_DESTRUCT(&buf);

    /* A callback that reports a non-success status for a blob it
     * consumed stops the walk at that blob. This is the failure a
     * component sees if its callback returns the unpack "end of buffer"
     * code instead of PMIX_SUCCESS: the remaining procs are dropped, and
     * the caller is told the whole operation succeeded. */
    modex_nblobs = modex_ndone = 0;
    modex_cb_rc = PMIX_ERR_UNPACK_READ_PAST_END_OF_BUFFER;
    PMIX_CONSTRUCT(&buf, pmix_buffer_t);
    rc = build_modex(&buf, "gds-modex-ns", ranks, 3, PMIX_COLLECT_YES);
    if (PMIX_SUCCESS == rc) {
        rc = pmix_gds_base_store_modex(&buf, NULL, modex_cb, &trk);
        report("a callback that does not report success truncates the walk",
               1 == modex_nblobs);
    }
    PMIX_DESTRUCT(&buf);
    modex_cb_rc = PMIX_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* job registration: node/proc map forms                                */
/* ------------------------------------------------------------------ */

/* Register an nspace whose node and proc maps are carried in the given
 * pmix_data_type_t, either at the top level of the info array or nested
 * inside a PMIX_JOB_INFO_ARRAY. Returns the registration status. */
static pmix_status_t register_with_maps(const char *nspace, int nprocs,
                                        pmix_data_type_t maptype, bool nested)
{
    char *noderegex = NULL, *ppnregex = NULL;
    char *nodestr = NULL, *ppnstr = NULL;
    char rankstr[64];
    pmix_info_t *info, *iptr;
    pmix_data_array_t *array = NULL;
    pmix_nspace_t ns;
    pmix_status_t rc;
    size_t ninfo;
    int m;

    /* the plain-string forms the regex generators consume */
    nodestr = strdup(pmix_globals.hostname);
    ppnstr = NULL;
    for (m = 0; m < nprocs; m++) {
        snprintf(rankstr, sizeof(rankstr), "%s%d", (0 == m) ? "" : ",", m);
        if (NULL == ppnstr) {
            ppnstr = strdup(rankstr);
        } else {
            char *t = NULL;
            if (0 > asprintf(&t, "%s%s", ppnstr, rankstr)) {
                free(ppnstr);
                free(nodestr);
                return PMIX_ERR_NOMEM;
            }
            free(ppnstr);
            ppnstr = t;
        }
    }

    PMIx_generate_regex(nodestr, &noderegex);
    PMIx_generate_ppn(ppnstr, &ppnregex);

    ninfo = 4;
    PMIX_INFO_CREATE(iptr, ninfo);
    if (PMIX_STRING == maptype) {
        PMIX_INFO_LOAD(&iptr[0], PMIX_NODE_MAP, nodestr, PMIX_STRING);
        PMIX_INFO_LOAD(&iptr[1], PMIX_PROC_MAP, ppnstr, PMIX_STRING);
    } else {
        /* PMIx_generate_regex hands back whichever encoded form the
         * active preg component produces; load it under the requested
         * type so the datastore has to decode that form */
        PMIX_INFO_LOAD(&iptr[0], PMIX_NODE_MAP, noderegex, maptype);
        PMIX_INFO_LOAD(&iptr[1], PMIX_PROC_MAP, ppnregex, maptype);
    }
    PMIX_INFO_LOAD(&iptr[2], PMIX_JOB_SIZE, &nprocs, PMIX_UINT32);
    PMIX_INFO_LOAD(&iptr[3], PMIX_UNIV_SIZE, &nprocs, PMIX_UINT32);

    if (nested) {
        PMIX_INFO_CREATE(info, 1);
        PMIX_DATA_ARRAY_CREATE(array, ninfo, PMIX_INFO);
        memcpy(array->array, iptr, ninfo * sizeof(pmix_info_t));
        /* the elements moved wholesale into the array */
        free(iptr);
        PMIX_LOAD_KEY(info[0].key, PMIX_JOB_INFO_ARRAY);
        info[0].value.type = PMIX_DATA_ARRAY;
        info[0].value.data.darray = array;
        ninfo = 1;
    } else {
        info = iptr;
    }

    PMIX_LOAD_NSPACE(ns, nspace);
    rc = PMIx_server_register_nspace(ns, nprocs, info, ninfo, NULL, NULL);

    PMIX_INFO_FREE(info, ninfo);
    free(nodestr);
    free(ppnstr);
    free(noderegex);
    free(ppnregex);
    return rc;
}

/* Ask the assigned module for a key on behalf of the server itself. */
static pmix_status_t fetch_key(const char *nspace, pmix_rank_t rank,
                               const char *key, pmix_list_t *kvs)
{
    pmix_cb_t cb;
    pmix_proc_t proc;
    pmix_kval_t *kv;
    pmix_status_t rc;

    PMIX_CONSTRUCT(&cb, pmix_cb_t);
    PMIX_LOAD_PROCID(&proc, nspace, rank);
    cb.proc = &proc;
    cb.key = (char *) key;
    cb.copy = true;
    cb.scope = PMIX_SCOPE_UNDEF;
    PMIX_GDS_FETCH_KV(rc, pmix_globals.mypeer, &cb);
    /* move the results out before the caddy is torn down */
    while (NULL != (kv = (pmix_kval_t *) pmix_list_remove_first(&cb.kvs))) {
        pmix_list_append(kvs, &kv->super);
    }
    cb.key = NULL;
    cb.proc = NULL;
    PMIX_DESTRUCT(&cb);
    return rc;
}

static void test_map_forms(void)
{
    pmix_list_t kvs;
    pmix_kval_t *kv;
    pmix_status_t rc;
    struct {
        const char *nspace;
        pmix_data_type_t type;
        bool nested;
        const char *what;
    } cases[] = {
        {"gds-map-regex", PMIX_REGEX, false, "top-level PMIX_REGEX maps"},
        {"gds-map-string", PMIX_STRING, false, "top-level PMIX_STRING maps"},
        {"gds-map-regex-nested", PMIX_REGEX, true,
         "PMIX_REGEX maps inside a PMIX_JOB_INFO_ARRAY"},
        {"gds-map-string-nested", PMIX_STRING, true,
         "PMIX_STRING maps inside a PMIX_JOB_INFO_ARRAY"},
    };
    size_t i;
    char label[160];

    fprintf(stdout, "\n-- node/proc map forms --\n");

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        rc = register_with_maps(cases[i].nspace, 2, cases[i].type,
                                cases[i].nested);
        snprintf(label, sizeof(label), "%s are accepted", cases[i].what);
        report(label, registered(rc));
        if (!registered(rc)) {
            fprintf(stdout, "        (register_nspace: %s)\n",
                    PMIx_Error_string(rc));
            continue;
        }
        /* decoding the maps is what produces a per-rank hostname, so a
         * form that was not decoded shows up as a missing key here */
        PMIX_CONSTRUCT(&kvs, pmix_list_t);
        rc = fetch_key(cases[i].nspace, 1, PMIX_HOSTNAME, &kvs);
        kv = (pmix_kval_t *) pmix_list_get_first(&kvs);
        snprintf(label, sizeof(label), "%s yield a per-rank hostname",
                 cases[i].what);
        report(label,
               PMIX_SUCCESS == rc && NULL != kv &&
                   NULL != kv->value && PMIX_STRING == kv->value->type &&
                   NULL != kv->value->data.string &&
                   0 == strcmp(kv->value->data.string, pmix_globals.hostname));
        PMIX_LIST_DESTRUCT(&kvs);
    }
}

/* ------------------------------------------------------------------ */
/* derivation of per-proc info the host did not supply                  */
/* ------------------------------------------------------------------ */

/* Register a job whose node/proc maps describe every rank, but whose
 * PMIX_PROC_INFO_ARRAY entries cover only the first "ndescribed" ranks -
 * and, for those, name only PMIX_LOCAL_RANK, carrying a sentinel value
 * no derivation would ever produce. Everything else the maps imply has
 * to be filled in by the datastore. */
static pmix_status_t register_with_proc_info(const char *nspace, int nprocs,
                                             int ndescribed, uint16_t sentinel)
{
    char *noderegex = NULL, *ppnregex = NULL;
    char *ppnstr = NULL, **agg = NULL;
    char rankstr[64];
    pmix_info_t *info, *pdata;
    pmix_data_array_t *array;
    pmix_nspace_t ns;
    pmix_status_t rc;
    size_t ninfo, n;
    uint16_t lrank;
    int m;

    for (m = 0; m < nprocs; m++) {
        snprintf(rankstr, sizeof(rankstr), "%d", m);
        PMIx_Argv_append_nosize(&agg, rankstr);
    }
    ppnstr = PMIx_Argv_join(agg, ',');
    PMIx_Argv_free(agg);

    PMIx_generate_regex(pmix_globals.hostname, &noderegex);
    PMIx_generate_ppn(ppnstr, &ppnregex);
    free(ppnstr);

    ninfo = 3 + (size_t) ndescribed;
    PMIX_INFO_CREATE(info, ninfo);
    PMIX_INFO_LOAD(&info[0], PMIX_NODE_MAP, noderegex, PMIX_REGEX);
    PMIX_INFO_LOAD(&info[1], PMIX_PROC_MAP, ppnregex, PMIX_REGEX);
    PMIX_INFO_LOAD(&info[2], PMIX_JOB_SIZE, &nprocs, PMIX_UINT32);
    free(noderegex);
    free(ppnregex);

    n = 3;
    for (m = 0; m < ndescribed; m++) {
        PMIX_LOAD_KEY(info[n].key, PMIX_PROC_INFO_ARRAY);
        info[n].value.type = PMIX_DATA_ARRAY;
        PMIX_DATA_ARRAY_CREATE(array, 2, PMIX_INFO);
        info[n].value.data.darray = array;
        pdata = (pmix_info_t *) array->array;
        PMIX_LOAD_KEY(pdata[0].key, PMIX_RANK);
        pdata[0].value.type = PMIX_PROC_RANK;
        pdata[0].value.data.rank = (pmix_rank_t) m;
        lrank = sentinel + (uint16_t) m;
        PMIX_INFO_LOAD(&pdata[1], PMIX_LOCAL_RANK, &lrank, PMIX_UINT16);
        ++n;
    }

    PMIX_LOAD_NSPACE(ns, nspace);
    rc = PMIx_server_register_nspace(ns, nprocs, info, ninfo, NULL, NULL);
    PMIX_INFO_FREE(info, ninfo);
    return rc;
}

/* Fetch one key for one rank as a number. Returns false if the key is
 * absent, which is the failure mode these cases are about. */
static bool fetch_number(const char *nspace, pmix_rank_t rank,
                         const char *key, uint32_t *out)
{
    pmix_list_t kvs;
    pmix_kval_t *kv;
    pmix_status_t rc;
    bool found = false;

    PMIX_CONSTRUCT(&kvs, pmix_list_t);
    rc = fetch_key(nspace, rank, key, &kvs);
    kv = (pmix_kval_t *) pmix_list_get_first(&kvs);
    if (PMIX_SUCCESS == rc && NULL != kv && NULL != kv->value) {
        found = (PMIX_SUCCESS == PMIx_Value_get_number(kv->value, out, PMIX_UINT32));
    }
    PMIX_LIST_DESTRUCT(&kvs);
    return found;
}

static void check_number(const char *what, const char *nspace, pmix_rank_t rank,
                         const char *key, uint32_t expect)
{
    uint32_t got = UINT32_MAX;
    char label[200];
    bool found;

    found = fetch_number(nspace, rank, key, &got);
    snprintf(label, sizeof(label), "%s", what);
    report(label, found && got == expect);
    if (!found) {
        fprintf(stdout, "        (%s for rank %u: not found)\n", key, rank);
    } else if (got != expect) {
        fprintf(stdout, "        (%s for rank %u: got %u, expected %u)\n",
                key, rank, got, expect);
    }
}

/* The datastore derives PMIX_HOSTNAME, PMIX_NODEID, PMIX_LOCAL_RANK and
 * PMIX_NODE_RANK for each rank from the node and proc maps. Those values
 * are assumptions, so anything the host stated itself in a
 * PMIX_PROC_INFO_ARRAY must win - but the decision has to be made per
 * rank and per key.
 *
 * It used to be made once for the whole job: a single proc-info array
 * anywhere in the array set a flag that suppressed the derivation of
 * nodeid, local rank and node rank for *every* rank. A host that
 * described one proc lost those three keys for all the others, and a
 * host that described every proc but named only some of the keys lost
 * the rest for all of them - PMIx_Get returning PMIX_ERR_NOT_FOUND, with
 * no fallback, since the node-info fallback in fetch applies only to
 * wildcard ranks. PMIX_HOSTNAME was stored outside the same gate, so it
 * was the one key that survived - and, conversely, the one key a host
 * could not state for itself without having it overwritten. */
static void test_derived_proc_info(void)
{
    pmix_list_t kvs;
    pmix_kval_t *kv;
    pmix_status_t rc;
    const uint16_t sentinel = 100;

    fprintf(stdout, "\n-- per-proc info derived from the maps --\n");

    /* two ranks, only rank 0 described */
    rc = register_with_proc_info("gds-derive-partial", 2, 1, sentinel);
    report("job with proc info for some ranks is accepted", registered(rc));
    if (registered(rc)) {
        /* the described rank keeps what the host said... */
        check_number("host-supplied local rank survives derivation",
                     "gds-derive-partial", 0, PMIX_LOCAL_RANK, sentinel);
        /* ...and still gets the keys the host did not name */
        check_number("described rank still gets a derived node rank",
                     "gds-derive-partial", 0, PMIX_NODE_RANK, 0);
        check_number("described rank still gets a derived nodeid",
                     "gds-derive-partial", 0, PMIX_NODEID, 0);

        /* the rank the host never mentioned gets the whole set */
        check_number("undescribed rank gets a derived local rank",
                     "gds-derive-partial", 1, PMIX_LOCAL_RANK, 1);
        check_number("undescribed rank gets a derived node rank",
                     "gds-derive-partial", 1, PMIX_NODE_RANK, 1);
        check_number("undescribed rank gets a derived nodeid",
                     "gds-derive-partial", 1, PMIX_NODEID, 0);

        PMIX_CONSTRUCT(&kvs, pmix_list_t);
        rc = fetch_key("gds-derive-partial", 1, PMIX_HOSTNAME, &kvs);
        kv = (pmix_kval_t *) pmix_list_get_first(&kvs);
        report("undescribed rank gets a derived hostname",
               PMIX_SUCCESS == rc && NULL != kv && NULL != kv->value &&
                   PMIX_STRING == kv->value->type &&
                   NULL != kv->value->data.string &&
                   0 == strcmp(kv->value->data.string, pmix_globals.hostname));
        PMIX_LIST_DESTRUCT(&kvs);
    }

    /* every rank described, but none of them named a node rank */
    rc = register_with_proc_info("gds-derive-full", 2, 2, sentinel);
    report("job with proc info for every rank is accepted", registered(rc));
    if (registered(rc)) {
        check_number("rank 0 keeps its host-supplied local rank",
                     "gds-derive-full", 0, PMIX_LOCAL_RANK, sentinel);
        check_number("rank 1 keeps its host-supplied local rank",
                     "gds-derive-full", 1, PMIX_LOCAL_RANK, sentinel + 1);
        check_number("rank 0 gets the node rank the host omitted",
                     "gds-derive-full", 0, PMIX_NODE_RANK, 0);
        check_number("rank 1 gets the node rank the host omitted",
                     "gds-derive-full", 1, PMIX_NODE_RANK, 1);
    }
}

/* ------------------------------------------------------------------ */
/* malformed job-level input                                            */
/* ------------------------------------------------------------------ */

static void test_malformed_job_info(void)
{
    pmix_info_t *info, *iptr;
    pmix_data_array_t *array;
    pmix_nspace_t ns;
    pmix_status_t rc;
    uint32_t nodeid = 7;
    int nprocs = 1;
    pmix_list_t kvs;

    fprintf(stdout, "\n-- malformed job-level input --\n");

    /* two node maps for one job is not allowed. The rejection has to
     * unwind through the same exit that frees the argv arrays already
     * parsed out of the first map. */
    PMIX_INFO_CREATE(info, 3);
    PMIX_INFO_LOAD(&info[0], PMIX_NODE_MAP, pmix_globals.hostname, PMIX_STRING);
    PMIX_INFO_LOAD(&info[1], PMIX_NODE_MAP, pmix_globals.hostname, PMIX_STRING);
    PMIX_INFO_LOAD(&info[2], PMIX_JOB_SIZE, &nprocs, PMIX_UINT32);
    PMIX_LOAD_NSPACE(ns, "gds-dup-nodemap");
    rc = PMIx_server_register_nspace(ns, nprocs, info, 3, NULL, NULL);
    report("a repeated PMIX_NODE_MAP is rejected", PMIX_ERR_BAD_PARAM == rc);
    PMIX_INFO_FREE(info, 3);

    /* a node map of a type that is neither a regex nor a string */
    PMIX_INFO_CREATE(info, 2);
    PMIX_INFO_LOAD(&info[0], PMIX_NODE_MAP, &nodeid, PMIX_UINT32);
    PMIX_INFO_LOAD(&info[1], PMIX_JOB_SIZE, &nprocs, PMIX_UINT32);
    PMIX_LOAD_NSPACE(ns, "gds-badtype-nodemap");
    rc = PMIx_server_register_nspace(ns, nprocs, info, 2, NULL, NULL);
    report("a PMIX_NODE_MAP of the wrong type is rejected",
           PMIX_ERR_TYPE_MISMATCH == rc);
    PMIX_INFO_FREE(info, 2);

    /* a PMIX_NODE_INFO_ARRAY may identify its node by nodeid alone, so
     * the stored node carries no hostname. A later hostname-qualified
     * lookup then compares against that missing name. */
    PMIX_INFO_CREATE(info, 2);
    PMIX_INFO_CREATE(iptr, 2);
    PMIX_INFO_LOAD(&iptr[0], PMIX_NODEID, &nodeid, PMIX_UINT32);
    PMIX_INFO_LOAD(&iptr[1], PMIX_NODE_SIZE, &nprocs, PMIX_UINT32);
    PMIX_DATA_ARRAY_CREATE(array, 2, PMIX_INFO);
    memcpy(array->array, iptr, 2 * sizeof(pmix_info_t));
    free(iptr);
    PMIX_LOAD_KEY(info[0].key, PMIX_NODE_INFO_ARRAY);
    info[0].value.type = PMIX_DATA_ARRAY;
    info[0].value.data.darray = array;
    PMIX_INFO_LOAD(&info[1], PMIX_JOB_SIZE, &nprocs, PMIX_UINT32);
    PMIX_LOAD_NSPACE(ns, "gds-nodeid-only");
    rc = PMIx_server_register_nspace(ns, nprocs, info, 2, NULL, NULL);
    report("a node identified by nodeid alone is accepted", registered(rc));
    if (!registered(rc)) {
        fprintf(stdout, "        (register_nspace: %s)\n", PMIx_Error_string(rc));
    }
    PMIX_INFO_FREE(info, 2);

    if (registered(rc)) {
        pmix_cb_t cb;
        pmix_proc_t proc;
        pmix_info_t qual;
        char *other = "a-host-that-is-not-here";

        PMIX_CONSTRUCT(&cb, pmix_cb_t);
        PMIX_LOAD_PROCID(&proc, "gds-nodeid-only", PMIX_RANK_WILDCARD);
        PMIX_INFO_LOAD(&qual, PMIX_HOSTNAME, other, PMIX_STRING);
        cb.proc = &proc;
        cb.key = PMIX_NODE_SIZE;
        cb.copy = true;
        cb.scope = PMIX_SCOPE_UNDEF;
        cb.info = &qual;
        cb.ninfo = 1;
        PMIX_GDS_FETCH_KV(rc, pmix_globals.mypeer, &cb);
        report("a hostname-qualified lookup against a nameless node is safe",
               PMIX_SUCCESS != rc);
        cb.key = NULL;
        cb.proc = NULL;
        cb.info = NULL;
        cb.ninfo = 0;
        PMIX_DESTRUCT(&cb);
        PMIX_INFO_DESTRUCT(&qual);
    }

    /* a PMIX_QUALIFIED_VALUE whose data array is empty: there is no
     * primary value to extract and no qualifiers to build */
    PMIX_INFO_CREATE(info, 2);
    PMIX_DATA_ARRAY_CREATE(array, 0, PMIX_INFO);
    PMIX_LOAD_KEY(info[0].key, PMIX_QUALIFIED_VALUE);
    info[0].value.type = PMIX_DATA_ARRAY;
    info[0].value.data.darray = array;
    PMIX_INFO_LOAD(&info[1], PMIX_JOB_SIZE, &nprocs, PMIX_UINT32);
    PMIX_LOAD_NSPACE(ns, "gds-empty-qualified");
    rc = PMIx_server_register_nspace(ns, nprocs, info, 2, NULL, NULL);
    report("an empty PMIX_QUALIFIED_VALUE is rejected", !registered(rc));
    PMIX_INFO_FREE(info, 2);

    /* and the store path reached by PMIx_Put must reject it too */
    {
        pmix_kval_t kv;
        pmix_value_t val;
        pmix_proc_t proc;

        PMIX_DATA_ARRAY_CREATE(array, 0, PMIX_INFO);
        val.type = PMIX_DATA_ARRAY;
        val.data.darray = array;
        kv.key = PMIX_QUALIFIED_VALUE;
        kv.value = &val;
        PMIX_LOAD_PROCID(&proc, "gds-empty-qualified", 0);
        PMIX_GDS_STORE_KV(rc, pmix_globals.mypeer, &proc, PMIX_INTERNAL, &kv);
        report("storing an empty PMIX_QUALIFIED_VALUE is rejected",
               PMIX_SUCCESS != rc);
        PMIX_DATA_ARRAY_FREE(array);
    }

    /* nothing above should have left the datastore unusable */
    PMIX_CONSTRUCT(&kvs, pmix_list_t);
    rc = fetch_key("gds-nodeid-only", PMIX_RANK_WILDCARD, PMIX_JOB_SIZE, &kvs);
    report("the datastore still answers after the rejected input",
           PMIX_SUCCESS == rc && 0 < pmix_list_get_size(&kvs));
    PMIX_LIST_DESTRUCT(&kvs);
}

/* ------------------------------------------------------------------ */
/* store/fetch scope routing                                            */
/* ------------------------------------------------------------------ */

static pmix_status_t store_one(const char *nspace, pmix_rank_t rank,
                               pmix_scope_t scope, const char *key,
                               const char *value)
{
    pmix_kval_t kv;
    pmix_value_t val;
    pmix_proc_t proc;
    pmix_status_t rc;

    val.type = PMIX_STRING;
    val.data.string = (char *) value;
    kv.key = (char *) key;
    kv.value = &val;
    PMIX_LOAD_PROCID(&proc, nspace, rank);
    PMIX_GDS_STORE_KV(rc, pmix_globals.mypeer, &proc, scope, &kv);
    return rc;
}

static pmix_status_t fetch_scoped(const char *nspace, pmix_rank_t rank,
                                  pmix_scope_t scope, const char *key,
                                  char **result)
{
    pmix_cb_t cb;
    pmix_proc_t proc;
    pmix_kval_t *kv;
    pmix_status_t rc;

    *result = NULL;
    PMIX_CONSTRUCT(&cb, pmix_cb_t);
    PMIX_LOAD_PROCID(&proc, nspace, rank);
    cb.proc = &proc;
    cb.key = (char *) key;
    cb.copy = true;
    cb.scope = scope;
    PMIX_GDS_FETCH_KV(rc, pmix_globals.mypeer, &cb);
    /* pmix_list_get_first() returns the list's SENTINEL when the list is
     * empty, never NULL - so the "NULL != kv" below is not the guard it
     * looks like, and reading kv->value off the sentinel is a read of
     * whatever happens to sit there. Several cases here fetch nothing on
     * purpose (EXISTS_OUTSIDE_SCOPE drains what it found, NOT_FOUND
     * never had any), so this is reached routinely. It survived only
     * because the bytes were harmless; under -fsanitize=address it is a
     * SEGV on address 0x1, and a change to an unrelated static made it
     * fault in an ordinary build too. Ask the list whether it has
     * anything first. */
    if (!pmix_list_is_empty(&cb.kvs)) {
        kv = (pmix_kval_t *) pmix_list_get_first(&cb.kvs);
        if (NULL != kv && NULL != kv->value && PMIX_STRING == kv->value->type &&
            NULL != kv->value->data.string) {
            *result = strdup(kv->value->data.string);
        }
    }
    cb.key = NULL;
    cb.proc = NULL;
    PMIX_DESTRUCT(&cb);
    return rc;
}

static void test_scope_routing(void)
{
    const char *ns = "gds-scopes";
    pmix_status_t rc;
    char *got = NULL;
    int nprocs = 2;
    pmix_info_t info;
    pmix_nspace_t nsp;

    fprintf(stdout, "\n-- store/fetch scope routing --\n");

    PMIX_INFO_LOAD(&info, PMIX_JOB_SIZE, &nprocs, PMIX_UINT32);
    PMIX_LOAD_NSPACE(nsp, ns);
    rc = PMIx_server_register_nspace(nsp, nprocs, &info, 1, NULL, NULL);
    PMIX_INFO_DESTRUCT(&info);
    report("scope-test nspace registers", registered(rc));
    if (!registered(rc)) {
        fprintf(stdout, "        (register_nspace: %s)\n", PMIx_Error_string(rc));
        return;
    }

    rc = store_one(ns, 0, PMIX_LOCAL, "gds.local", "L");
    report("a PMIX_LOCAL value stores", PMIX_SUCCESS == rc);
    rc = store_one(ns, 0, PMIX_REMOTE, "gds.remote", "R");
    report("a PMIX_REMOTE value stores", PMIX_SUCCESS == rc);
    rc = store_one(ns, 0, PMIX_GLOBAL, "gds.global", "G");
    report("a PMIX_GLOBAL value stores", PMIX_SUCCESS == rc);
    rc = store_one(ns, 0, PMIX_INTERNAL, "gds.internal", "I");
    report("a PMIX_INTERNAL value stores", PMIX_SUCCESS == rc);

    rc = fetch_scoped(ns, 0, PMIX_LOCAL, "gds.local", &got);
    report("a local value is found in local scope",
           PMIX_SUCCESS == rc && NULL != got && 0 == strcmp(got, "L"));
    free(got);

    rc = fetch_scoped(ns, 0, PMIX_REMOTE, "gds.remote", &got);
    report("a remote value is found in remote scope",
           PMIX_SUCCESS == rc && NULL != got && 0 == strcmp(got, "R"));
    free(got);

    /* a global value is written to both tables, so either scope finds it */
    rc = fetch_scoped(ns, 0, PMIX_LOCAL, "gds.global", &got);
    report("a global value is found in local scope",
           PMIX_SUCCESS == rc && NULL != got && 0 == strcmp(got, "G"));
    free(got);
    rc = fetch_scoped(ns, 0, PMIX_REMOTE, "gds.global", &got);
    report("a global value is found in remote scope",
           PMIX_SUCCESS == rc && NULL != got && 0 == strcmp(got, "G"));
    free(got);

    /* asking for a local-scope value in remote scope must say so rather
     * than leaving the caller to time out waiting for data that is
     * already present under a different scope */
    rc = fetch_scoped(ns, 0, PMIX_REMOTE, "gds.local", &got);
    report("a local value asked for remotely reports EXISTS_OUTSIDE_SCOPE",
           PMIX_ERR_EXISTS_OUTSIDE_SCOPE == rc);
    free(got);

    rc = fetch_scoped(ns, 0, PMIX_LOCAL, "gds.remote", &got);
    report("a remote value asked for locally reports EXISTS_OUTSIDE_SCOPE",
           PMIX_ERR_EXISTS_OUTSIDE_SCOPE == rc);
    free(got);

    rc = fetch_scoped(ns, 0, PMIX_SCOPE_UNDEF, "gds.nosuchkey", &got);
    report("an absent key reports NOT_FOUND", PMIX_ERR_NOT_FOUND == rc);
    free(got);
}

/* ------------------------------------------------------------------ */
/* realm classifiers                                                    */
/* ------------------------------------------------------------------ */

/* pmix_check_node_info() and its two siblings decide which realm a key
 * belongs to when the caller gave no qualifier, and gds_fetch consults
 * them on every keyed fetch. They short-circuit on
 * PMIx_Check_reserved_key() before searching, which is only sound
 * while every key they list is "pmix"-prefixed.
 *
 * That is true today and there is nothing to keep it true: a
 * node-scoped attribute added to the standard without the prefix would
 * be silently dropped by the short-circuit and quietly land in the
 * wrong realm. Assert each listed key still classifies, so the day
 * that happens this fails rather than the key going missing. */
static void test_realm_classifiers(void)
{
    static const char *const nodekeys[] = {PMIX_HOSTNAME, PMIX_NODEID, PMIX_LOCAL_PEERS,
                                           PMIX_LOCAL_SIZE, PMIX_NODE_SIZE, PMIX_LOCALLDR,
                                           PMIX_AVAIL_PHYS_MEMORY, PMIX_FABRIC_DEVICES, NULL};
    static const char *const appkeys[] = {PMIX_APP_SIZE, PMIX_APPLDR, PMIX_APP_ARGV, PMIX_WDIR,
                                          PMIX_PSET_NAME, PMIX_PSET_MEMBERS, PMIX_APP_MAP_TYPE,
                                          PMIX_APP_MAP_REGEX, NULL};
    static const char *const sesskeys[] = {PMIX_SESSION_ID, PMIX_CLUSTER_ID, PMIX_UNIV_SIZE,
                                           PMIX_TMPDIR, PMIX_TDIR_RMCLEAN,
                                           PMIX_HOSTNAME_KEEP_FQDN, PMIX_RM_NAME,
                                           PMIX_RM_VERSION, NULL};
    size_t n;
    bool ok;

    for (n = 0, ok = true; NULL != nodekeys[n]; n++) {
        if (!pmix_check_node_info(nodekeys[n])) {
            fprintf(stdout, "    node key not classified: %s\n", nodekeys[n]);
            ok = false;
        }
    }
    report("realm: every listed node key classifies", ok);

    for (n = 0, ok = true; NULL != appkeys[n]; n++) {
        if (!pmix_check_app_info(appkeys[n])) {
            fprintf(stdout, "    app key not classified: %s\n", appkeys[n]);
            ok = false;
        }
    }
    report("realm: every listed app key classifies", ok);

    for (n = 0, ok = true; NULL != sesskeys[n]; n++) {
        if (!pmix_check_session_info(sesskeys[n])) {
            fprintf(stdout, "    session key not classified: %s\n", sesskeys[n]);
            ok = false;
        }
    }
    report("realm: every listed session key classifies", ok);

    /* the realms stay distinct - a node key is not an app key */
    report("realm: a node key is not app or session",
           !pmix_check_app_info(PMIX_HOSTNAME) && !pmix_check_session_info(PMIX_HOSTNAME));

    /* an ordinary application key belongs to none of them, which is the
     * case the reserved-key short-circuit is there to make cheap */
    report("realm: an application key belongs to no realm",
           !pmix_check_node_info("my.app.key") && !pmix_check_app_info("my.app.key")
               && !pmix_check_session_info("my.app.key"));

    /* NULL is legal on this path - a PMIx_Get with no key means "all
     * data for this proc" - and must not be searched for */
    report("realm: a NULL key belongs to no realm",
           !pmix_check_node_info(NULL) && !pmix_check_app_info(NULL)
               && !pmix_check_session_info(NULL) && !pmix_check_special_key(NULL));
}

/* ------------------------------------------------------------------ */
/* the shmem3 module's job segment                                      */
/* ------------------------------------------------------------------ */

/* Build a peer bound to a specific gds module, packable enough for
 * register_job_info() to write a reply into.
 *
 * The wire format and role are borrowed from the local server's own
 * peer: nothing here is a real client, but pack_shmem3_connection_info()
 * does call PMIX_BFROPS_PACK against this peer and reads peer->info, so
 * both have to be real. */
static pmix_peer_t *mkgdspeer(const char *nspace, pmix_rank_t nprocs,
                              pmix_gds_base_module_t *mod)
{
    pmix_peer_t *p;
    pmix_namespace_t *ns;

    p = PMIX_NEW(pmix_peer_t);
    ns = PMIX_NEW(pmix_namespace_t);
    ns->nspace = strdup(nspace);
    ns->nprocs = nprocs;
    memcpy(&ns->compat, &pmix_globals.mypeer->nptr->compat, sizeof(ns->compat));
    ns->compat.gds = mod;
    p->nptr = ns;
    p->info = PMIX_NEW(pmix_rank_info_t);
    p->info->pname.nspace = strdup(nspace);
    p->info->pname.rank = 0;
    p->info->peerid = 0;
    memcpy(&p->proc_type, &pmix_globals.mypeer->proc_type, sizeof(p->proc_type));
    return p;
}

/* Drive the shmem3 module end to end: register a job, have it build its
 * shared segments, then read them back through its own fetch.
 *
 * shmem3 is not built on macOS at all and disqualifies itself where
 * /proc/self/maps is absent, so this reports a skip rather than a
 * failure when it is not the module that answers. That is the whole
 * reason contrib/dockerswarm/run-gds-tests.sh exists - but the parts
 * that a single process CAN reach are worth reaching from make check,
 * because on Linux this is the only place they run without a DVM.
 *
 * The job info deliberately carries three shapes a host is entitled to
 * send and this component used to take on trust:
 *
 *   - a job-level PMIX_DATA_ARRAY whose elements are NOT pmix_info_t.
 *     The segment sizing pass read element zero of every data array as
 *     one, so PMIX_CHECK_KEY walked up to PMIX_MAX_KEYLEN bytes past an
 *     eight-byte allocation.
 *   - PMIX_USERID as a string. add_nspace read the union directly, so
 *     the low half of a pointer became the job's uid AND set chown,
 *     which the segment's backing files were then handed to.
 *   - a session array, so the session segment is built too.
 */
static void test_shmem3_job_segment(void)
{
    pmix_gds_base_module_t *mod;
    pmix_info_t *info, *iptr, dir;
    pmix_data_array_t *array;
    pmix_nspace_t ns;
    pmix_status_t rc;
    pmix_peer_t *peer;
    pmix_buffer_t *reply;
    pmix_cb_t cb;
    pmix_proc_t proc;
    uint32_t nprocs = 2, sid = 11, univ = 4;
    /* Bytes, not infos - and chosen so that reading element zero as a
     * pmix_info_t finds a key that MATCHES. PMIx_Check_key() stops at the
     * first differing byte, so an array of arbitrary bytes is read one
     * byte past its start and no further; it takes a leading string that
     * really is the key to carry the read on into info[0].value, which
     * sits 512 bytes into a 16-byte allocation. */
    char impostor[16] = PMIX_SESSION_ID;
    char *nodemap, *procmap;

    fprintf(stdout, "\n-- shmem3 job segment --\n");

    PMIX_INFO_LOAD(&dir, PMIX_GDS_MODULE, "shmem3", PMIX_STRING);
    mod = pmix_gds_base_assign_module(&dir, 1);
    PMIX_INFO_DESTRUCT(&dir);
    if (NULL == mod || 0 != strcmp(mod->name, "shmem3")) {
        fprintf(stdout, "    SKIP  shmem3 is not available in this build\n");
        return;
    }

    nodemap = strdup(pmix_globals.hostname);
    procmap = strdup("0-1");

    PMIX_INFO_CREATE(info, 7);
    PMIX_INFO_LOAD(&info[0], PMIX_JOB_SIZE, &nprocs, PMIX_UINT32);
    PMIX_INFO_LOAD(&info[1], PMIX_NODE_MAP, nodemap, PMIX_STRING);
    PMIX_INFO_LOAD(&info[2], PMIX_PROC_MAP, procmap, PMIX_STRING);
    PMIX_INFO_LOAD(&info[3], PMIX_UNIV_SIZE, &univ, PMIX_UINT32);
    /* PMIX_USERID with a value that is not a uid */
    PMIX_INFO_LOAD(&info[4], PMIX_USERID, "not-a-uid", PMIX_STRING);
    /* a data array of bytes, not of infos */
    PMIX_DATA_ARRAY_CREATE(array, sizeof(impostor), PMIX_BYTE);
    memcpy(array->array, impostor, sizeof(impostor));
    PMIX_LOAD_KEY(info[5].key, "gds.test.bytes");
    info[5].value.type = PMIX_DATA_ARRAY;
    info[5].value.data.darray = array;
    /* a session array, so the session segment is built as well */
    PMIX_INFO_CREATE(iptr, 1);
    PMIX_INFO_LOAD(&iptr[0], PMIX_SESSION_ID, &sid, PMIX_UINT32);
    PMIX_DATA_ARRAY_CREATE(array, 1, PMIX_INFO);
    memcpy(array->array, iptr, sizeof(pmix_info_t));
    free(iptr);
    PMIX_LOAD_KEY(info[6].key, PMIX_SESSION_INFO_ARRAY);
    info[6].value.type = PMIX_DATA_ARRAY;
    info[6].value.data.darray = array;

    PMIX_LOAD_NSPACE(ns, "gds-shmem3-job");
    rc = PMIx_server_register_nspace(ns, nprocs, info, 7, NULL, NULL);
    report("a job carrying a non-info data array registers", registered(rc));
    if (!registered(rc)) {
        fprintf(stdout, "        (register_nspace: %s)\n", PMIx_Error_string(rc));
    }
    PMIX_INFO_FREE(info, 7);
    free(nodemap);
    free(procmap);
    if (!registered(rc)) {
        return;
    }

    /* Building the segments happens here, on the first peer to ask. */
    peer = mkgdspeer("gds-shmem3-job", nprocs, mod);
    /* heap, not stack - see the note on register_session_job() */
    reply = PMIX_NEW(pmix_buffer_t);
    PMIX_GDS_REGISTER_JOB_INFO(rc, peer, reply);
    report("shmem3 registers job info and describes its segments",
           PMIX_SUCCESS == rc && 0 < reply->bytes_used);
    if (PMIX_SUCCESS != rc) {
        fprintf(stdout, "        (register_job_info: %s)\n",
                PMIx_Error_string(rc));
    }
    PMIX_RELEASE(reply);

    /* Read a plain job-level key back out of the segment we just built,
     * through shmem3's own fetch. */
    PMIX_CONSTRUCT(&cb, pmix_cb_t);
    PMIX_LOAD_PROCID(&proc, "gds-shmem3-job", PMIX_RANK_WILDCARD);
    cb.proc = &proc;
    /* PMIX_JOB_SIZE and not, say, PMIX_UNIV_SIZE: the datastore stores
     * the latter under its replacement's name, so asking for it here
     * would be testing the deprecation mapping rather than the segment. */
    cb.key = PMIX_JOB_SIZE;
    cb.copy = true;
    cb.scope = PMIX_SCOPE_UNDEF;
    PMIX_GDS_FETCH_KV(rc, peer, &cb);
    report("a job-level key reads back out of the shared segment",
           PMIX_SUCCESS == rc && 1 == pmix_list_get_size(&cb.kvs));
    if (PMIX_SUCCESS != rc || 1 != pmix_list_get_size(&cb.kvs)) {
        fprintf(stdout, "        (fetch: %s, %zu values)\n",
                PMIx_Error_string(rc), pmix_list_get_size(&cb.kvs));
    }
    cb.key = NULL;
    cb.proc = NULL;
    PMIX_DESTRUCT(&cb);

    /* The whole-job form, which walks the node, app and session lists
     * and rebuilds a proc array per rank. */
    PMIX_CONSTRUCT(&cb, pmix_cb_t);
    PMIX_LOAD_PROCID(&proc, "gds-shmem3-job", PMIX_RANK_WILDCARD);
    cb.proc = &proc;
    cb.key = NULL;
    cb.copy = true;
    cb.scope = PMIX_SCOPE_UNDEF;
    PMIX_GDS_FETCH_KV(rc, peer, &cb);
    report("the whole-job fetch returns the segment's contents",
           PMIX_SUCCESS == rc && 0 < pmix_list_get_size(&cb.kvs));
    cb.proc = NULL;
    PMIX_DESTRUCT(&cb);

    /* A key nobody stored is a miss, not a fault. */
    PMIX_CONSTRUCT(&cb, pmix_cb_t);
    PMIX_LOAD_PROCID(&proc, "gds-shmem3-job", PMIX_RANK_WILDCARD);
    cb.proc = &proc;
    cb.key = "gds.test.absent";
    cb.copy = true;
    cb.scope = PMIX_SCOPE_UNDEF;
    PMIX_GDS_FETCH_KV(rc, peer, &cb);
    report("an absent key misses rather than faults", PMIX_SUCCESS != rc);
    cb.key = NULL;
    cb.proc = NULL;
    PMIX_DESTRUCT(&cb);

    /* Deregistering has to give the segments back - the tracker owns
     * their mappings, and the arena reservation goes with it. */
    PMIX_GDS_DEL_NSPACE(rc, "gds-shmem3-job");
    report("deregistering the nspace releases the segments",
           PMIX_SUCCESS == rc);

    PMIX_RELEASE(peer);
}

/* Does haystack contain needle? The register_job_info reply is packed
 * kvals, and what this test needs from it is only whether a particular
 * namespace name appears in it - which is a plain byte search, and keeps
 * the test out of the component's private blob format. */
static bool buffer_mentions(pmix_buffer_t *b, const char *needle)
{
    const size_t nlen = strlen(needle);

    if (NULL == b->base_ptr || b->bytes_used < nlen) {
        return false;
    }
    for (size_t i = 0; i + nlen <= b->bytes_used; i++) {
        if (0 == memcmp(&b->base_ptr[i], needle, nlen)) {
            return true;
        }
    }
    return false;
}

/* Register a job in a named session and build its segments. Returns the
 * peer, with the register_job_info reply in *reply for the caller. */
/* NOTE the heap reply. pmix_server_switchyard.c passes PMIX_NEW(pmix_buffer_t)
 * here, and it has to: gds/hash may PMIX_RETAIN the reply into ns->jobbkt so
 * it can hand the same packed job description to the job's other local
 * clients. A stack buffer passed here is retained, destructed by the caller,
 * and then released again when the namespace is torn down - which asserts on
 * the object magic in a debug build and is silent corruption in an optimized
 * one. Do not "simplify" this back to a stack buffer. */
static pmix_peer_t *register_session_job(const char *nsname, uint32_t sid,
                                         pmix_gds_base_module_t *mod,
                                         pmix_buffer_t **reply,
                                         pmix_status_t *rc)
{
    pmix_info_t *info, *iptr;
    pmix_data_array_t *array;
    pmix_nspace_t ns;
    pmix_peer_t *peer;
    uint32_t nprocs = 2;
    char *nodemap = strdup(pmix_globals.hostname);
    char *procmap = strdup("0-1");

    PMIX_INFO_CREATE(info, 4);
    PMIX_INFO_LOAD(&info[0], PMIX_JOB_SIZE, &nprocs, PMIX_UINT32);
    PMIX_INFO_LOAD(&info[1], PMIX_NODE_MAP, nodemap, PMIX_STRING);
    PMIX_INFO_LOAD(&info[2], PMIX_PROC_MAP, procmap, PMIX_STRING);
    PMIX_INFO_CREATE(iptr, 1);
    PMIX_INFO_LOAD(&iptr[0], PMIX_SESSION_ID, &sid, PMIX_UINT32);
    PMIX_DATA_ARRAY_CREATE(array, 1, PMIX_INFO);
    memcpy(array->array, iptr, sizeof(pmix_info_t));
    free(iptr);
    PMIX_LOAD_KEY(info[3].key, PMIX_SESSION_INFO_ARRAY);
    info[3].value.type = PMIX_DATA_ARRAY;
    info[3].value.data.darray = array;

    PMIX_LOAD_NSPACE(ns, nsname);
    *rc = PMIx_server_register_nspace(ns, nprocs, info, 4, NULL, NULL);
    PMIX_INFO_FREE(info, 4);
    free(nodemap);
    free(procmap);
    if (!registered(*rc)) {
        return NULL;
    }

    peer = mkgdspeer(nsname, nprocs, mod);
    *reply = PMIX_NEW(pmix_buffer_t);
    if (NULL == *reply) {
        *rc = PMIX_ERR_NOMEM;
        PMIX_RELEASE(peer);
        return NULL;
    }
    PMIX_GDS_REGISTER_JOB_INFO(*rc, peer, *reply);
    return peer;
}

/* Two jobs in one session share one session segment.
 *
 * This is what pmix_mca_gds_shmem3_component.sessions exists for, and it
 * did not happen before: each job built a private session object and its
 * own segment, so N jobs in a session cost N copies of the same session
 * data. A persistent DVM running many jobs under one allocation is the
 * case that makes that matter.
 *
 * The observable is the backing path the server hands each job's clients.
 * A session segment's path is built from the namespace of the job that
 * created it, so the second job's reply naming the FIRST job's namespace
 * can only mean it is describing the segment that job built. */
static void test_shmem3_shared_session(void)
{
    pmix_gds_base_module_t *mod;
    pmix_info_t dir;
    pmix_peer_t *peer_a, *peer_b;
    pmix_buffer_t *reply_a, *reply_b;
    pmix_status_t rc_a, rc_b, rc;
    pmix_cb_t cb;
    pmix_proc_t proc;
    const uint32_t sid = 77;

    fprintf(stdout, "\n-- shmem3 shared session --\n");

    PMIX_INFO_LOAD(&dir, PMIX_GDS_MODULE, "shmem3", PMIX_STRING);
    mod = pmix_gds_base_assign_module(&dir, 1);
    PMIX_INFO_DESTRUCT(&dir);
    if (NULL == mod || 0 != strcmp(mod->name, "shmem3")) {
        fprintf(stdout, "    SKIP  shmem3 is not available in this build\n");
        return;
    }

    peer_a = register_session_job("gds-sesh-a", sid, mod, &reply_a, &rc_a);
    if (NULL == peer_a || PMIX_SUCCESS != rc_a) {
        report("the first job in a session registers", false);
        if (NULL != peer_a) {
            PMIX_RELEASE(reply_a);
            PMIX_RELEASE(peer_a);
        }
        return;
    }
    report("the first job in a session registers", true);

    peer_b = register_session_job("gds-sesh-b", sid, mod, &reply_b, &rc_b);
    if (NULL == peer_b || PMIX_SUCCESS != rc_b) {
        report("the second job in the same session registers", false);
        PMIX_RELEASE(reply_a);
        PMIX_RELEASE(peer_a);
        if (NULL != peer_b) {
            PMIX_RELEASE(reply_b);
            PMIX_RELEASE(peer_b);
        }
        return;
    }
    report("the second job in the same session registers", true);

    /* The point of the case. */
    report("the second job is given the first job's session segment",
           buffer_mentions(reply_b, "gds-sesh-a"));
    /* ...and the converse, so a pass cannot come from the two replies
     * simply naming everything. */
    report("the first job is not given the second job's segments",
           !buffer_mentions(reply_a, "gds-sesh-b"));

    PMIX_RELEASE(reply_a);
    PMIX_RELEASE(reply_b);

    /* Dropping the job that built the segment must not take it away from
     * the one still in the session: the segment belongs to the session
     * object, which job B still holds a reference on. Before the segment
     * moved out of job_destruct(), this unmapped it under B. */
    PMIX_GDS_DEL_NSPACE(rc, "gds-sesh-a");
    report("deregistering the first job leaves the session standing",
           PMIX_SUCCESS == rc);

    /* The whole-job form specifically, and not a job-level key: only this
     * one walks the session list, which lives in the shared segment. A
     * keyed job-level fetch is answered out of job B's own segment and
     * would pass whether or not the session mapping survived. */
    PMIX_CONSTRUCT(&cb, pmix_cb_t);
    PMIX_LOAD_PROCID(&proc, "gds-sesh-b", PMIX_RANK_WILDCARD);
    cb.proc = &proc;
    cb.key = NULL;
    cb.copy = true;
    cb.scope = PMIX_SCOPE_UNDEF;
    PMIX_GDS_FETCH_KV(rc, peer_b, &cb);
    report("the surviving job still reads the shared session segment",
           PMIX_SUCCESS == rc && 0 < pmix_list_get_size(&cb.kvs));
    cb.proc = NULL;
    PMIX_DESTRUCT(&cb);

    /* And the last job out gives the session segment back. */
    PMIX_GDS_DEL_NSPACE(rc, "gds-sesh-b");
    report("deregistering the last job releases the session segment",
           PMIX_SUCCESS == rc);

    PMIX_RELEASE(peer_a);
    PMIX_RELEASE(peer_b);
}

/* Sessions are registered and deregistered, not inferred.
 *
 * The asymmetry this closes: nspaces have always been established and
 * torn down explicitly, while a session existed only as a side effect of
 * a PMIX_SESSION_INFO_ARRAY riding inside some job's registration. That
 * gave a session no way to be described before its first job, and no way
 * to survive its last - and a session with no jobs running in it is an
 * ordinary state, not an ended one, so the library must not infer the
 * end from the last job leaving. Only the host can say.
 */
static void test_session_registration(void)
{
    pmix_gds_base_module_t *mod;
    pmix_info_t sinfo[2];
    pmix_peer_t *peer;
    pmix_buffer_t *reply;
    pmix_status_t rc;
    pmix_cb_t cb;
    pmix_proc_t proc;
    uint32_t univ = 8, maxprocs = 16;
    const uint32_t sid = 4242;

    fprintf(stdout, "\n-- session registration --\n");

    /* whichever component is active here - hash everywhere, shmem3 where
     * it builds - so this case covers both implementations */
    mod = pmix_gds_base_assign_module(NULL, 0);
    if (NULL == mod) {
        fprintf(stdout, "    SKIP  no gds module available\n");
        return;
    }

    /* A session can be described before any job exists in it. */
    PMIX_INFO_LOAD(&sinfo[0], PMIX_UNIV_SIZE, &univ, PMIX_UINT32);
    PMIX_INFO_LOAD(&sinfo[1], PMIX_MAX_PROCS, &maxprocs, PMIX_UINT32);
    rc = PMIx_server_register_session(sid, sinfo, 2, NULL, NULL);
    report("a session registers with no job in it", PMIX_SUCCESS == rc);
    if (PMIX_SUCCESS != rc) {
        fprintf(stdout, "        (register_session: %s)\n", PMIx_Error_string(rc));
    }

    /* UINT32_MAX is the library's own spelling of "no session named", so
     * it cannot also be a session id. */
    rc = PMIx_server_register_session(UINT32_MAX, sinfo, 2, NULL, NULL);
    report("UINT32_MAX is refused as a session id", PMIX_ERR_BAD_PARAM == rc);

    /* Re-registering is not an error and does not disturb what is held:
     * a session's description belongs to the session, and by now it may
     * already have been handed to clients. */
    rc = PMIx_server_register_session(sid, sinfo, 2, NULL, NULL);
    report("re-registering a known session succeeds", PMIX_SUCCESS == rc);

    /* Now launch a job into it, the ordinary way. */
    peer = register_session_job("gds-sesreg-a", sid, mod, &reply, &rc);
    if (NULL == peer || PMIX_SUCCESS != rc) {
        report("a job launches into a registered session", false);
        if (NULL != peer) {
            PMIX_RELEASE(reply);
            PMIX_RELEASE(peer);
        }
        PMIX_INFO_DESTRUCT(&sinfo[0]);
        PMIX_INFO_DESTRUCT(&sinfo[1]);
        return;
    }
    report("a job launches into a registered session", true);
    PMIX_RELEASE(reply);

    /* The host's description reaches a client of that job. The whole-job
     * fetch is what walks the session's data. */
    PMIX_CONSTRUCT(&cb, pmix_cb_t);
    PMIX_LOAD_PROCID(&proc, "gds-sesreg-a", PMIX_RANK_WILDCARD);
    cb.proc = &proc;
    cb.key = NULL;
    cb.copy = true;
    cb.scope = PMIX_SCOPE_UNDEF;
    PMIX_GDS_FETCH_KV(rc, peer, &cb);
    report("the session's registered data reaches the job",
           PMIX_SUCCESS == rc && 0 < pmix_list_get_size(&cb.kvs));
    cb.proc = NULL;
    PMIX_DESTRUCT(&cb);

    /* The point of the whole exercise: the last job leaving does NOT end
     * the session. Another job may be about to be launched into it. */
    PMIX_GDS_DEL_NSPACE(rc, "gds-sesreg-a");
    report("deregistering the only job succeeds", PMIX_SUCCESS == rc);
    PMIX_RELEASE(peer);

    /* So a job launched afterwards still finds the session, and still
     * gets its data - without the host having to describe it again. */
    peer = register_session_job("gds-sesreg-b", sid, mod, &reply, &rc);
    if (NULL == peer || PMIX_SUCCESS != rc) {
        report("a later job still finds the session", false);
        if (NULL != peer) {
            PMIX_RELEASE(reply);
            PMIX_RELEASE(peer);
        }
        PMIX_INFO_DESTRUCT(&sinfo[0]);
        PMIX_INFO_DESTRUCT(&sinfo[1]);
        return;
    }
    PMIX_RELEASE(reply);

    PMIX_CONSTRUCT(&cb, pmix_cb_t);
    PMIX_LOAD_PROCID(&proc, "gds-sesreg-b", PMIX_RANK_WILDCARD);
    cb.proc = &proc;
    cb.key = NULL;
    cb.copy = true;
    cb.scope = PMIX_SCOPE_UNDEF;
    PMIX_GDS_FETCH_KV(rc, peer, &cb);
    report("a job launched after the session emptied still reads it",
           PMIX_SUCCESS == rc && 0 < pmix_list_get_size(&cb.kvs));
    cb.proc = NULL;
    PMIX_DESTRUCT(&cb);

    /* And the host ends it explicitly. */
    PMIx_server_deregister_session(sid, NULL, NULL);
    report("the host deregisters the session", true);

    /* Deregistering a session this server never had is not an error - a
     * host issues the call on every daemon. */
    PMIx_server_deregister_session(99999, NULL, NULL);
    report("deregistering an unknown session is not an error", true);

    PMIX_GDS_DEL_NSPACE(rc, "gds-sesreg-b");
    PMIX_RELEASE(peer);

    /* A session registered after being deregistered is a new one, and
     * takes a fresh description. */
    rc = PMIx_server_register_session(sid, sinfo, 2, NULL, NULL);
    report("the session id can be reused after deregistration",
           PMIX_SUCCESS == rc);
    PMIx_server_deregister_session(sid, NULL, NULL);

    PMIX_INFO_DESTRUCT(&sinfo[0]);
    PMIX_INFO_DESTRUCT(&sinfo[1]);
}

/* shmem3's modex generations, in one process.
 *
 * Each modex gets a segment of its own, and whether the previous one can
 * be dropped depends on what arrived: a cumulative contribution repeats
 * everything, so it supersedes what came before; a delta repeats
 * nothing, so the generation before it is still the only copy of what
 * the delta left out and has to stay answerable.
 *
 * That chain had no in-process coverage at all - it lives in
 * contrib/dockerswarm, which needs several nodes - so a change to how
 * generations are held could only be checked by reading. This drives it
 * with hand-built envelopes against a peer bound to shmem3, which is
 * enough to reach every part except the client's attach.
 *
 * The load-bearing case is the last one: after a DELTA that carries only
 * gen2, a fetch of gen1 must still find it. That value exists only in
 * the retired generation, so finding it is proof the chain was walked
 * rather than just the newest segment read.
 */
static void test_shmem3_modex_generations(void)
{
    pmix_gds_base_module_t *mod;
    pmix_info_t *info, dir;
    pmix_nspace_t ns;
    pmix_peer_t *peer;
    pmix_buffer_t *reply, buf;
    pmix_server_trkr_t trk;
    pmix_status_t rc;
    pmix_cb_t cb;
    pmix_proc_t proc;
    pmix_rank_t remote[2] = {2, 3};
    uint32_t nprocs = 4;
    char *nodemap, *procmap;
    pmix_list_t nslist;
    pmix_nspace_caddy_t *nsc;
    pmix_namespace_t *nsptr;

    fprintf(stdout, "\n-- shmem3 modex generations --\n");

    PMIX_INFO_LOAD(&dir, PMIX_GDS_MODULE, "shmem3", PMIX_STRING);
    mod = pmix_gds_base_assign_module(&dir, 1);
    PMIX_INFO_DESTRUCT(&dir);
    if (NULL == mod || 0 != strcmp(mod->name, "shmem3")) {
        fprintf(stdout, "    SKIP  shmem3 is not available in this build\n");
        return;
    }

    /* Two nodes, so ranks 2 and 3 are genuinely remote and their data
     * can only come from a modex. One node would make every rank local
     * and the modex would never be consulted. */
    if (0 > asprintf(&nodemap, "%s,gds-modexgen-node1", pmix_globals.hostname)) {
        return;
    }
    procmap = strdup("0-1;2-3");

    PMIX_INFO_CREATE(info, 3);
    PMIX_INFO_LOAD(&info[0], PMIX_JOB_SIZE, &nprocs, PMIX_UINT32);
    PMIX_INFO_LOAD(&info[1], PMIX_NODE_MAP, nodemap, PMIX_STRING);
    PMIX_INFO_LOAD(&info[2], PMIX_PROC_MAP, procmap, PMIX_STRING);
    PMIX_LOAD_NSPACE(ns, "gds-modexgen");
    rc = PMIx_server_register_nspace(ns, 2, info, 3, NULL, NULL);
    PMIX_INFO_FREE(info, 3);
    free(nodemap);
    free(procmap);
    if (!registered(rc)) {
        report("a two-node job registers", false);
        return;
    }
    report("a two-node job registers", true);

    peer = mkgdspeer("gds-modexgen", nprocs, mod);
    reply = PMIX_NEW(pmix_buffer_t);
    PMIX_GDS_REGISTER_JOB_INFO(rc, peer, reply);
    PMIX_RELEASE(reply);
    if (PMIX_SUCCESS != rc) {
        report("its job segment builds", false);
        PMIX_RELEASE(peer);
        return;
    }
    report("its job segment builds", true);

    memset(&trk, 0, sizeof(trk));
    trk.collect_type = PMIX_COLLECT_YES;

    /* mark_modex_complete packs a seg blob per namespace in this list -
     * pmix_server_op_replies.c builds the same thing from the fence's
     * participants. It is not optional: the implementation walks it. */
    PMIX_CONSTRUCT(&nslist, pmix_list_t);
    nsc = PMIX_NEW(pmix_nspace_caddy_t);
    PMIX_LIST_FOREACH (nsptr, &pmix_globals.nspaces, pmix_namespace_t) {
        if (0 == strcmp(nsptr->nspace, "gds-modexgen")) {
            PMIX_RETAIN(nsptr);
            nsc->ns = nsptr;
            break;
        }
    }
    pmix_list_append(&nslist, &nsc->super);

    /* generation 1, cumulative, carrying gen1 */
    PMIX_CONSTRUCT(&buf, pmix_buffer_t);
    rc = build_modex_kv(&buf, "gds-modexgen", remote, 2,
                        "gds.modex.gen1", 111, PMIX_COLLECT_YES);
    if (PMIX_SUCCESS == rc) {
        PMIX_GDS_STORE_MODEX(rc, peer, "gds-modexgen", &buf, &trk);
    }
    PMIX_DESTRUCT(&buf);
    report("the first modex generation stores", PMIX_SUCCESS == rc);
    if (PMIX_SUCCESS != rc) {
        fprintf(stdout, "        (store_modex: %s)\n", PMIx_Error_string(rc));
        PMIX_RELEASE(peer);
        return;
    }
    PMIX_CONSTRUCT(&buf, pmix_buffer_t);
    PMIX_GDS_MARK_MODEX_COMPLETE(rc, peer, &nslist, &buf);
    PMIX_DESTRUCT(&buf);
    report("the first generation is published", PMIX_SUCCESS == rc);

    PMIX_CONSTRUCT(&cb, pmix_cb_t);
    PMIX_LOAD_PROCID(&proc, "gds-modexgen", 2);
    cb.proc = &proc;
    cb.key = "gds.modex.gen1";
    cb.copy = true;
    cb.scope = PMIX_SCOPE_UNDEF;
    PMIX_GDS_FETCH_KV(rc, peer, &cb);
    report("a remote rank's key reads out of the modex segment",
           PMIX_SUCCESS == rc && 1 == pmix_list_get_size(&cb.kvs));
    cb.key = NULL;
    cb.proc = NULL;
    PMIX_DESTRUCT(&cb);

    /* generation 2, DELTA, carrying only gen2 - so gen1 survives only in
     * the generation retired behind it */
    PMIX_CONSTRUCT(&buf, pmix_buffer_t);
    rc = build_modex_kv(&buf, "gds-modexgen", remote, 2,
                        "gds.modex.gen2", 222, PMIX_MODEX_DELTA);
    if (PMIX_SUCCESS == rc) {
        PMIX_GDS_STORE_MODEX(rc, peer, "gds-modexgen", &buf, &trk);
    }
    PMIX_DESTRUCT(&buf);
    report("a delta generation stores", PMIX_SUCCESS == rc);
    PMIX_CONSTRUCT(&buf, pmix_buffer_t);
    PMIX_GDS_MARK_MODEX_COMPLETE(rc, peer, &nslist, &buf);
    PMIX_DESTRUCT(&buf);

    PMIX_CONSTRUCT(&cb, pmix_cb_t);
    PMIX_LOAD_PROCID(&proc, "gds-modexgen", 2);
    cb.proc = &proc;
    cb.key = "gds.modex.gen2";
    cb.copy = true;
    cb.scope = PMIX_SCOPE_UNDEF;
    PMIX_GDS_FETCH_KV(rc, peer, &cb);
    report("the newest generation answers for its own key",
           PMIX_SUCCESS == rc && 1 == pmix_list_get_size(&cb.kvs));
    cb.key = NULL;
    cb.proc = NULL;
    PMIX_DESTRUCT(&cb);

    /* the case the chain exists for */
    PMIX_CONSTRUCT(&cb, pmix_cb_t);
    PMIX_LOAD_PROCID(&proc, "gds-modexgen", 2);
    cb.proc = &proc;
    cb.key = "gds.modex.gen1";
    cb.copy = true;
    cb.scope = PMIX_SCOPE_UNDEF;
    PMIX_GDS_FETCH_KV(rc, peer, &cb);
    report("a retired generation still answers for a key the delta omitted",
           PMIX_SUCCESS == rc && 1 == pmix_list_get_size(&cb.kvs));
    if (PMIX_SUCCESS != rc || 1 != pmix_list_get_size(&cb.kvs)) {
        fprintf(stdout, "        (fetch: %s, %zu values)\n",
                PMIx_Error_string(rc), pmix_list_get_size(&cb.kvs));
    }
    cb.key = NULL;
    cb.proc = NULL;
    PMIX_DESTRUCT(&cb);

    /* generation 3, CUMULATIVE. This used to supersede every generation
     * behind it - release the current one and drop the retired chain -
     * so gen1 vanished here. Nothing is unmapped any more: a cumulative
     * contribution makes the older generations redundant rather than
     * wrong, and the walk simply stops at the newest segment holding the
     * key. Keeping them is what lets a reader walk the chain with no
     * lock, which is the whole reason for the change. */
    PMIX_CONSTRUCT(&buf, pmix_buffer_t);
    rc = build_modex_kv(&buf, "gds-modexgen", remote, 2,
                        "gds.modex.gen3", 333, PMIX_COLLECT_YES);
    if (PMIX_SUCCESS == rc) {
        PMIX_GDS_STORE_MODEX(rc, peer, "gds-modexgen", &buf, &trk);
    }
    PMIX_DESTRUCT(&buf);
    report("a cumulative generation stores", PMIX_SUCCESS == rc);
    PMIX_CONSTRUCT(&buf, pmix_buffer_t);
    PMIX_GDS_MARK_MODEX_COMPLETE(rc, peer, &nslist, &buf);
    PMIX_DESTRUCT(&buf);

    PMIX_CONSTRUCT(&cb, pmix_cb_t);
    PMIX_LOAD_PROCID(&proc, "gds-modexgen", 2);
    cb.proc = &proc;
    cb.key = "gds.modex.gen1";
    cb.copy = true;
    cb.scope = PMIX_SCOPE_UNDEF;
    PMIX_GDS_FETCH_KV(rc, peer, &cb);
    report("a cumulative generation does not take the chain away",
           PMIX_SUCCESS == rc && 1 == pmix_list_get_size(&cb.kvs));
    if (PMIX_SUCCESS != rc || 1 != pmix_list_get_size(&cb.kvs)) {
        fprintf(stdout, "        (fetch: %s, %zu values)\n",
                PMIx_Error_string(rc), pmix_list_get_size(&cb.kvs));
    }
    cb.key = NULL;
    cb.proc = NULL;
    PMIX_DESTRUCT(&cb);

    PMIX_LIST_DESTRUCT(&nslist);
    PMIX_GDS_DEL_NSPACE(rc, "gds-modexgen");
    report("deregistering releases every generation", PMIX_SUCCESS == rc);
    PMIX_RELEASE(peer);
}

/* Register a job whose session array carries a caller-supplied set of
 * keys - which is how a host that has not adopted
 * PMIx_server_register_session describes a session, and the only way it
 * can ever update one. */
static pmix_peer_t *register_job_with_session_info(const char *nsname,
                                                   uint32_t sid,
                                                   pmix_info_t *sess,
                                                   size_t nsess,
                                                   pmix_gds_base_module_t *mod,
                                                   pmix_buffer_t **reply,
                                                   pmix_status_t *rc)
{
    pmix_info_t *info, *iptr;
    pmix_data_array_t *array;
    pmix_nspace_t ns;
    pmix_peer_t *peer;
    uint32_t nprocs = 2;
    char *nodemap = strdup(pmix_globals.hostname);
    char *procmap = strdup("0-1");

    PMIX_INFO_CREATE(info, 4);
    PMIX_INFO_LOAD(&info[0], PMIX_JOB_SIZE, &nprocs, PMIX_UINT32);
    PMIX_INFO_LOAD(&info[1], PMIX_NODE_MAP, nodemap, PMIX_STRING);
    PMIX_INFO_LOAD(&info[2], PMIX_PROC_MAP, procmap, PMIX_STRING);

    /* session id first, then the caller's keys - the shape every host
     * uses for a session array */
    PMIX_INFO_CREATE(iptr, nsess + 1);
    PMIX_INFO_LOAD(&iptr[0], PMIX_SESSION_ID, &sid, PMIX_UINT32);
    for (size_t n = 0; n < nsess; n++) {
        PMIX_INFO_XFER(&iptr[n + 1], &sess[n]);
    }
    PMIX_DATA_ARRAY_CREATE(array, nsess + 1, PMIX_INFO);
    memcpy(array->array, iptr, (nsess + 1) * sizeof(pmix_info_t));
    free(iptr);
    PMIX_LOAD_KEY(info[3].key, PMIX_SESSION_INFO_ARRAY);
    info[3].value.type = PMIX_DATA_ARRAY;
    info[3].value.data.darray = array;

    PMIX_LOAD_NSPACE(ns, nsname);
    *rc = PMIx_server_register_nspace(ns, nprocs, info, 4, NULL, NULL);
    PMIX_INFO_FREE(info, 4);
    free(nodemap);
    free(procmap);
    if (!registered(*rc)) {
        return NULL;
    }
    peer = mkgdspeer(nsname, nprocs, mod);
    *reply = PMIX_NEW(pmix_buffer_t);
    PMIX_GDS_REGISTER_JOB_INFO(*rc, peer, *reply);
    return peer;
}

/* A host that never calls PMIx_server_register_session must still be
 * able to update a session, because that API is new and hosts will take
 * a long time to adopt it. Such a host describes a session only through
 * the PMIX_SESSION_INFO_ARRAY in each job registration - so a second
 * job carrying a CHANGED array has to take effect, while one carrying
 * the same array must cost nothing.
 */
static void test_session_update_via_job(void)
{
    pmix_gds_base_module_t *mod;
    pmix_info_t sess[1];
    pmix_peer_t *p1, *p2, *p3;
    pmix_buffer_t *r1, *r2, *r3;
    pmix_status_t rc;
    pmix_cb_t cb;
    pmix_proc_t proc;
    pmix_kval_t *kv;
    uint32_t univ = 12, grown = 96, got = 0;
    const uint32_t sid = 5150;

    fprintf(stdout, "\n-- session update through a job registration --\n");

    mod = pmix_gds_base_assign_module(NULL, 0);
    if (NULL == mod) {
        fprintf(stdout, "    SKIP  no gds module available\n");
        return;
    }

    /* first job describes the session */
    PMIX_INFO_LOAD(&sess[0], PMIX_UNIV_SIZE, &univ, PMIX_UINT32);
    p1 = register_job_with_session_info("gds-sjob-a", sid, sess, 1,
                                        mod, &r1, &rc);
    PMIX_INFO_DESTRUCT(&sess[0]);
    if (NULL == p1 || PMIX_SUCCESS != rc) {
        report("a job describes its session", false);
        return;
    }
    report("a job describes its session", true);
    PMIX_RELEASE(r1);

    /* a second job restating the SAME description changes nothing */
    PMIX_INFO_LOAD(&sess[0], PMIX_UNIV_SIZE, &univ, PMIX_UINT32);
    p2 = register_job_with_session_info("gds-sjob-b", sid, sess, 1,
                                        mod, &r2, &rc);
    PMIX_INFO_DESTRUCT(&sess[0]);
    report("a second job restating the same description registers",
           NULL != p2 && PMIX_SUCCESS == rc);
    if (NULL != p2) {
        PMIX_RELEASE(r2);
    }

    /* a third job says the session has GROWN - this is the case a host
     * that never adopts register_session depends on */
    PMIX_INFO_LOAD(&sess[0], PMIX_UNIV_SIZE, &grown, PMIX_UINT32);
    p3 = register_job_with_session_info("gds-sjob-c", sid, sess, 1,
                                        mod, &r3, &rc);
    PMIX_INFO_DESTRUCT(&sess[0]);
    if (NULL == p3 || PMIX_SUCCESS != rc) {
        report("a job carrying a changed description registers", false);
        return;
    }
    report("a job carrying a changed description registers", true);
    PMIX_RELEASE(r3);

    /* every job in the session now sees the new value, including the
     * one that was registered before the change */
    PMIX_CONSTRUCT(&cb, pmix_cb_t);
    PMIX_LOAD_PROCID(&proc, "gds-sjob-a", PMIX_RANK_WILDCARD);
    cb.proc = &proc;
    cb.key = PMIX_UNIV_SIZE;
    cb.copy = true;
    cb.scope = PMIX_SCOPE_UNDEF;
    PMIX_GDS_FETCH_KV(rc, p1, &cb);
    if (PMIX_SUCCESS == rc && 1 == pmix_list_get_size(&cb.kvs)) {
        kv = (pmix_kval_t *) pmix_list_get_first(&cb.kvs);
        PMIx_Value_get_number(kv->value, &got, PMIX_UINT32);
    }
    report("a job registration can update a session", grown == got);
    if (grown != got) {
        fprintf(stdout, "        (expected %u, got %u)\n",
                (unsigned) grown, (unsigned) got);
    }
    cb.key = NULL;
    cb.proc = NULL;
    PMIX_DESTRUCT(&cb);

    PMIx_server_deregister_session(sid, NULL, NULL);
    PMIX_GDS_DEL_NSPACE(rc, "gds-sjob-a");
    PMIX_GDS_DEL_NSPACE(rc, "gds-sjob-b");
    PMIX_GDS_DEL_NSPACE(rc, "gds-sjob-c");
    PMIX_RELEASE(p1);
    if (NULL != p2) {
        PMIX_RELEASE(p2);
    }
    PMIX_RELEASE(p3);
}

/* A session's description changes under it.
 *
 * This is the case the whole session series exists for: a session's
 * resources are not fixed. PMIX_SESSION_EXTEND grows one "in terms of
 * time or resources", PMIX_SESSION_PREEMPT takes them back, a node goes
 * down - so PMIX_UNIV_SIZE, defined as "#slots in this session", is a
 * value a host restates.
 *
 * A published segment can never be rewritten - local clients have it
 * mapped - so an update is a NEW segment carrying only what changed,
 * published at the head of the session's chain. A read walks newest
 * first, so the restated key answers from the update and everything the
 * update did not mention still answers from the segments behind it.
 */
static void test_session_update(void)
{
    pmix_gds_base_module_t *mod;
    pmix_info_t sinfo[2], upd[1];
    pmix_peer_t *peer;
    pmix_buffer_t *reply;
    pmix_status_t rc;
    pmix_cb_t cb;
    pmix_proc_t proc;
    pmix_kval_t *kv;
    uint32_t univ = 8, grown = 64;
    uint32_t got = 0;
    const uint32_t sid = 909;
    bool found_univ = false, found_max = false;

    fprintf(stdout, "\n-- session update --\n");

    mod = pmix_gds_base_assign_module(NULL, 0);
    if (NULL == mod) {
        fprintf(stdout, "    SKIP  no gds module available\n");
        return;
    }

    /* describe the session, then launch a job into it */
    PMIX_INFO_LOAD(&sinfo[0], PMIX_UNIV_SIZE, &univ, PMIX_UINT32);
    /* A key from pmix_check_session_info()'s list, so that a KEYED get
     * routes to the session realm - PMIX_MAX_PROCS is stored on the
     * session but classified job-level, so asking for it by key never
     * reaches here. */
    PMIX_INFO_LOAD(&sinfo[1], PMIX_CLUSTER_ID, "gds-test-cluster",
                   PMIX_STRING);
    rc = PMIx_server_register_session(sid, sinfo, 2, NULL, NULL);
    report("a session registers", PMIX_SUCCESS == rc);

    peer = register_session_job("gds-sesupd", sid, mod, &reply, &rc);
    if (NULL == peer || PMIX_SUCCESS != rc) {
        report("a job launches into it", false);
        PMIX_INFO_DESTRUCT(&sinfo[0]);
        PMIX_INFO_DESTRUCT(&sinfo[1]);
        return;
    }
    report("a job launches into it", true);
    PMIX_RELEASE(reply);

    /* the session grows */
    PMIX_INFO_LOAD(&upd[0], PMIX_UNIV_SIZE, &grown, PMIX_UINT32);
    rc = PMIx_server_register_session(sid, upd, 1, NULL, NULL);
    report("the host restates the session's size", PMIX_SUCCESS == rc);

    /* the restated key answers with the NEW value */
    PMIX_CONSTRUCT(&cb, pmix_cb_t);
    PMIX_LOAD_PROCID(&proc, "gds-sesupd", PMIX_RANK_WILDCARD);
    cb.proc = &proc;
    cb.key = PMIX_UNIV_SIZE;
    cb.copy = true;
    cb.scope = PMIX_SCOPE_UNDEF;
    PMIX_GDS_FETCH_KV(rc, peer, &cb);
    if (PMIX_SUCCESS == rc && 1 == pmix_list_get_size(&cb.kvs)) {
        kv = (pmix_kval_t *) pmix_list_get_first(&cb.kvs);
        PMIx_Value_get_number(kv->value, &got, PMIX_UINT32);
    }
    report("the updated value is what a read returns", grown == got);
    if (grown != got) {
        fprintf(stdout, "        (expected %u, got %u)\n",
                (unsigned) grown, (unsigned) got);
    }
    cb.key = NULL;
    cb.proc = NULL;
    PMIX_DESTRUCT(&cb);

    /* a key the update did NOT mention still answers from behind it -
     * this is what says the update carried only what changed rather
     * than replacing the session wholesale */
    bool kept = false;
    PMIX_CONSTRUCT(&cb, pmix_cb_t);
    PMIX_LOAD_PROCID(&proc, "gds-sesupd", PMIX_RANK_WILDCARD);
    cb.proc = &proc;
    cb.key = PMIX_CLUSTER_ID;
    cb.copy = true;
    cb.scope = PMIX_SCOPE_UNDEF;
    PMIX_GDS_FETCH_KV(rc, peer, &cb);
    if (PMIX_SUCCESS == rc && 1 == pmix_list_get_size(&cb.kvs)) {
        kv = (pmix_kval_t *) pmix_list_get_first(&cb.kvs);
        kept = (PMIX_STRING == kv->value->type &&
                NULL != kv->value->data.string &&
                0 == strcmp(kv->value->data.string, "gds-test-cluster"));
    }
    report("a key the update did not restate survives", kept);
    cb.key = NULL;
    cb.proc = NULL;
    PMIX_DESTRUCT(&cb);

    /* the whole-session form reports each key ONCE, with the newest
     * value - the shadowed copy must not come back too */
    PMIX_CONSTRUCT(&cb, pmix_cb_t);
    PMIX_LOAD_PROCID(&proc, "gds-sesupd", PMIX_RANK_WILDCARD);
    cb.proc = &proc;
    cb.key = NULL;
    cb.copy = true;
    cb.scope = PMIX_SCOPE_UNDEF;
    PMIX_GDS_FETCH_KV(rc, peer, &cb);
    if (PMIX_SUCCESS == rc) {
        PMIX_LIST_FOREACH (kv, &cb.kvs, pmix_kval_t) {
            if (NULL == kv->key) {
                continue;
            }
            if (PMIX_CHECK_KEY(kv, PMIX_SESSION_INFO_ARRAY) &&
                PMIX_DATA_ARRAY == kv->value->type &&
                NULL != kv->value->data.darray) {
                pmix_info_t *ia =
                    (pmix_info_t *) kv->value->data.darray->array;
                size_t nia = kv->value->data.darray->size, u = 0, m = 0;
                for (size_t i = 0; i < nia; i++) {
                    if (PMIX_CHECK_KEY(&ia[i], PMIX_UNIV_SIZE)) {
                        u++;
                        PMIx_Value_get_number(&ia[i].value, &got,
                                              PMIX_UINT32);
                    }
                    else if (PMIX_CHECK_KEY(&ia[i], PMIX_CLUSTER_ID)) {
                        m++;
                    }
                }
                found_univ = (1 == u && grown == got);
                found_max = (1 == m);
            }
        }
    }
    report("the whole-session form reports the updated key once",
           found_univ);
    report("the whole-session form still carries the untouched key",
           found_max);
    cb.proc = NULL;
    PMIX_DESTRUCT(&cb);

    PMIx_server_deregister_session(sid, NULL, NULL);
    PMIX_GDS_DEL_NSPACE(rc, "gds-sesupd");
    PMIX_RELEASE(peer);
    PMIX_INFO_DESTRUCT(&sinfo[0]);
    PMIX_INFO_DESTRUCT(&sinfo[1]);
    PMIX_INFO_DESTRUCT(&upd[0]);
}

/* A host adds a resource after the job is already registered.
 *
 * PMIx_server_register_resources puts job-level data in a global cache
 * that is copied into a namespace's datastore ONCE, when that namespace
 * is first registered - so a call afterwards used to govern only the
 * namespaces registered later, and every job already running kept the
 * description it was given. The deletion half of that was fixed by
 * del_key and tombstones; this is the addition.
 *
 * The value is read back through the job's own peer, which is what says
 * it reached the namespace's datastore rather than only the cache.
 */
static void test_register_resources_after_nspace(void)
{
    pmix_gds_base_module_t *mod;
    pmix_info_t res[1], *info;
    pmix_nspace_t ns;
    pmix_peer_t *peer;
    pmix_buffer_t *reply;
    pmix_status_t rc;
    pmix_cb_t cb;
    pmix_proc_t proc;
    pmix_kval_t *kv;
    uint32_t nprocs = 2, added = 7, got = 0;
    char *nodemap, *procmap;

    fprintf(stdout, "\n-- resources added after registration --\n");

    mod = pmix_gds_base_assign_module(NULL, 0);
    if (NULL == mod) {
        fprintf(stdout, "    SKIP  no gds module available\n");
        return;
    }

    nodemap = strdup(pmix_globals.hostname);
    procmap = strdup("0-1");
    PMIX_INFO_CREATE(info, 3);
    PMIX_INFO_LOAD(&info[0], PMIX_JOB_SIZE, &nprocs, PMIX_UINT32);
    PMIX_INFO_LOAD(&info[1], PMIX_NODE_MAP, nodemap, PMIX_STRING);
    PMIX_INFO_LOAD(&info[2], PMIX_PROC_MAP, procmap, PMIX_STRING);
    PMIX_LOAD_NSPACE(ns, "gds-lateres");
    rc = PMIx_server_register_nspace(ns, nprocs, info, 3, NULL, NULL);
    PMIX_INFO_FREE(info, 3);
    free(nodemap);
    free(procmap);
    if (!registered(rc)) {
        report("a job registers", false);
        return;
    }
    report("a job registers", true);

    /* build its segments, as a first client connecting would */
    peer = mkgdspeer("gds-lateres", nprocs, mod);
    reply = PMIX_NEW(pmix_buffer_t);
    PMIX_GDS_REGISTER_JOB_INFO(rc, peer, reply);
    PMIX_RELEASE(reply);
    report("its job data is published", PMIX_SUCCESS == rc);

    /* now the host adds a resource - AFTER the namespace exists */
    PMIX_INFO_LOAD(&res[0], "gds.test.lateres", &added, PMIX_UINT32);
    rc = PMIx_server_register_resources(res, 1, NULL, NULL);
    report("a resource registers after the job", PMIX_SUCCESS == rc ||
           PMIX_OPERATION_SUCCEEDED == rc);

    PMIX_CONSTRUCT(&cb, pmix_cb_t);
    PMIX_LOAD_PROCID(&proc, "gds-lateres", PMIX_RANK_WILDCARD);
    cb.proc = &proc;
    cb.key = "gds.test.lateres";
    cb.copy = true;
    cb.scope = PMIX_SCOPE_UNDEF;
    PMIX_GDS_FETCH_KV(rc, peer, &cb);
    if (PMIX_SUCCESS == rc && 1 == pmix_list_get_size(&cb.kvs)) {
        kv = (pmix_kval_t *) pmix_list_get_first(&cb.kvs);
        PMIx_Value_get_number(kv->value, &got, PMIX_UINT32);
    }
    report("it reaches a namespace that was already registered",
           added == got);
    if (added != got) {
        fprintf(stdout, "        (expected %u, got %u; fetch %s)\n",
                (unsigned) added, (unsigned) got, PMIx_Error_string(rc));
    }
    cb.key = NULL;
    cb.proc = NULL;
    PMIX_DESTRUCT(&cb);

    PMIx_server_deregister_resources(res, 1, NULL, NULL);
    PMIX_INFO_DESTRUCT(&res[0]);
    PMIX_GDS_DEL_NSPACE(rc, "gds-lateres");
    PMIX_RELEASE(peer);
}

int main(int argc, char **argv)
{
    pmix_status_t rc;
    PMIX_HIDE_UNUSED_PARAMS(argc, argv);

    /* line-buffer so a case that segfaults still shows which one it was -
     * several of these crash rather than fail against an unfixed library */
    setvbuf(stdout, NULL, _IOLBF, 0);

    rc = PMIx_server_init(&mymodule, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "PMIx_server_init failed: %s\n", PMIx_Error_string(rc));
        return 1;
    }

    fprintf(stdout, "=== gds datastore unit tests ===\n");

    test_assign_module();
    test_store_modex();
    test_store_modex_nspace_filter();
    test_store_modex_blob_info();
    test_map_forms();
    test_derived_proc_info();
    test_malformed_job_info();
    test_scope_routing();
    test_realm_classifiers();
    test_shmem3_job_segment();
    test_shmem3_shared_session();
    test_shmem3_modex_generations();
    test_session_registration();
    test_session_update();
    test_session_update_via_job();
    test_register_resources_after_nspace();

    fprintf(stdout, "\n=== %d passed, %d failed ===\n", npass, nfail);

    PMIx_server_finalize();
    return (0 == nfail) ? 0 : 1;
}
