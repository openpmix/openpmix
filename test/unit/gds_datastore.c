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

static pmix_status_t modex_cb(pmix_proc_t *proc, pmix_buffer_t *pbkt)
{
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
                                 pmix_rank_t *ranks, size_t nranks)
{
    pmix_buffer_t ranklevel, serverlevel, pbkt;
    pmix_byte_object_t bo;
    pmix_status_t rc;
    pmix_proc_t proc;
    uint8_t collect = 1;
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
    rc = build_modex(&buf, "gds-modex-ns", ranks, 3);
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
    rc = build_modex(&buf, "gds-modex-ns", ranks, 3);
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
    test_map_forms();
    test_malformed_job_info();
    test_scope_routing();

    fprintf(stdout, "\n=== %d passed, %d failed ===\n", npass, nfail);

    PMIx_server_finalize();
    return (0 == nfail) ? 0 : 1;
}
