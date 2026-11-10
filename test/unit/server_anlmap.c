/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * White-box unit tests for the server's computation of PMIX_ANL_MAP -
 * pmix_server_compute_anl_map() in src/server/pmix_server_anlmap.c, as
 * reached through pmix_server_get() in src/server/pmix_server_get.c.
 *
 * The process comes up as a PMIx server with a stub host module, then
 * registers jobs with no procs on this node, each laid out by a node map
 * and a proc map. A wildcard-rank request for PMIX_ANL_MAP that the host
 * did not supply must then be answered with a map the server works out
 * from that layout, and the map must be kept as job-level data so later
 * requests are answered from the store.
 *
 * Test cases:
 *
 *   by slot, by node, uneven, a short final pass over the nodes, nodes
 *   renumbered by rank order, a cycle of two blocks
 *                                  -> answered, and the expected map stored
 *   a placement too long for the
 *      1024 bytes of a PMI-2 value -> no map answered, nothing stored
 *   a job size the map does not
 *      fully place                 -> no map answered, nothing stored
 *   a map the host supplied        -> that map, untouched
 *
 * Every request must also leave local_reqs empty: a server that parks a
 * request it can never answer hangs the calling client.
 *
 * Not covered here: a node in the map that hosts none of the job's ranks.
 * Registration on this branch drops an empty entry from the proc map, so
 * such a job cannot be registered through PMIX_PROC_MAP.
 */

#include "src/include/pmix_config.h"

#include "include/pmix.h"
#include "include/pmix_server.h"

#include "src/include/pmix_globals.h"
#include "src/mca/bfrops/bfrops.h"
#include "src/mca/gds/gds.h"
#include "src/server/pmix_server_ops.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

/* what the modex callback recorded */
static bool cb_fired = false;
static pmix_status_t cb_status = PMIX_SUCCESS;
static size_t cb_ndata = 0;

/* Stand-in for the switchyard's get_cbfunc. Whoever calls it owns
 * answering the client and releasing the caddy, and the release function
 * (if any) frees the returned payload. */
static void get_cbfunc(pmix_status_t status, const char *data, size_t ndata,
                       void *cbdata, pmix_release_cbfunc_t relfn, void *relcbd)
{
    pmix_server_caddy_t *cd = (pmix_server_caddy_t *) cbdata;

    cb_fired = true;
    cb_status = status;
    cb_ndata = ndata;
    (void) data;
    if (NULL != relfn) {
        relfn(relcbd);
    }
    PMIX_RELEASE(cd);
}

/* Build the wire form of a GET request: nspace, rank, ninfo (zero), key */
static pmix_buffer_t *build_get_request(const char *nspace, pmix_rank_t rank,
                                        const char *key)
{
    pmix_buffer_t *buf;
    pmix_status_t rc;
    char *cptr = (char *) nspace;
    size_t ninfo = 0;

    buf = PMIX_NEW(pmix_buffer_t);
    if (NULL == buf) {
        return NULL;
    }
    PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, buf, &cptr, 1, PMIX_STRING);
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(buf);
        return NULL;
    }
    PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, buf, &rank, 1, PMIX_PROC_RANK);
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(buf);
        return NULL;
    }
    PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, buf, &ninfo, 1, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(buf);
        return NULL;
    }
    cptr = (char *) key;
    PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, buf, &cptr, 1, PMIX_STRING);
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(buf);
        return NULL;
    }
    return buf;
}

/* Drive one server-side GET. Returns what pmix_server_get() returned; the
 * caddy is released here on the error return exactly as the switchyard
 * does, and by the callback otherwise. */
static pmix_status_t do_get(const char *nspace, pmix_rank_t rank, const char *key)
{
    pmix_server_caddy_t *cd;
    pmix_buffer_t *buf;
    pmix_status_t rc;

    cb_fired = false;
    cb_status = PMIX_SUCCESS;
    cb_ndata = 0;

    buf = build_get_request(nspace, rank, key);
    if (NULL == buf) {
        return PMIX_ERR_NOMEM;
    }

    cd = PMIX_NEW(pmix_server_caddy_t);
    if (NULL == cd) {
        PMIX_RELEASE(buf);
        return PMIX_ERR_NOMEM;
    }
    PMIX_RETAIN(pmix_globals.mypeer);
    cd->peer = pmix_globals.mypeer;
    cd->hdr.tag = 0;

    rc = pmix_server_get(buf, get_cbfunc, cd);
    if (PMIX_SUCCESS != rc) {
        /* the switchyard owns the caddy on a non-success return */
        PMIX_RELEASE(cd);
    }
    PMIX_RELEASE(buf);
    return rc;
}

static bool nothing_parked(void)
{
    return (0 == pmix_list_get_size(&pmix_server_globals.local_reqs));
}

/* Register a job with no procs on this node, laid out over nodelist
 * (comma-delimited) as ppn says (';' between nodes, ',' between ranks) -
 * the form PMIx_generate_ppn takes. When anlmap is not NULL, the host
 * supplies it as the job's PMIX_ANL_MAP. */
static pmix_status_t register_map_job(const char *nspace, const char *nodelist,
                                      const char *ppn, uint32_t nprocs,
                                      const char *anlmap)
{
    pmix_info_t info[4];
    pmix_nspace_t ns;
    pmix_status_t rc;
    char *noderegex = NULL, *ppnregex = NULL;
    size_t n, ninfo = 3;

    rc = PMIx_generate_regex(nodelist, &noderegex);
    if (PMIX_SUCCESS != rc) {
        return rc;
    }
    rc = PMIx_generate_ppn(ppn, &ppnregex);
    if (PMIX_SUCCESS != rc) {
        free(noderegex);
        return rc;
    }
    PMIX_INFO_LOAD(&info[0], PMIX_NODE_MAP, noderegex, PMIX_REGEX);
    PMIX_INFO_LOAD(&info[1], PMIX_PROC_MAP, ppnregex, PMIX_REGEX);
    PMIX_INFO_LOAD(&info[2], PMIX_JOB_SIZE, &nprocs, PMIX_UINT32);
    if (NULL != anlmap) {
        PMIX_INFO_LOAD(&info[3], PMIX_ANL_MAP, anlmap, PMIX_STRING);
        ninfo = 4;
    }
    PMIX_LOAD_NSPACE(ns, nspace);
    rc = PMIx_server_register_nspace(ns, 0, info, ninfo, NULL, NULL);
    if (PMIX_OPERATION_SUCCEEDED == rc) {
        rc = PMIX_SUCCESS;
    }
    for (n = 0; n < ninfo; n++) {
        PMIX_INFO_DESTRUCT(&info[n]);
    }
    free(noderegex);
    free(ppnregex);
    return rc;
}

/* What this server now holds under PMIX_ANL_MAP for nspace - NULL when
 * it holds nothing. Read straight from the store rather than out of the
 * reply, so the case also shows the value was kept for later requests. */
static char *stored_anl_map(const char *nspace)
{
    pmix_cb_t cb;
    pmix_proc_t proc;
    pmix_info_t opt;
    pmix_kval_t *kv;
    pmix_status_t rc;
    char *map = NULL;

    PMIX_INFO_LOAD(&opt, PMIX_OPTIONAL, NULL, PMIX_BOOL);
    PMIX_LOAD_PROCID(&proc, nspace, PMIX_RANK_WILDCARD);
    PMIX_CONSTRUCT(&cb, pmix_cb_t);
    cb.proc = &proc;
    cb.key = PMIX_ANL_MAP;
    cb.scope = PMIX_INTERNAL;
    cb.info = &opt;
    cb.ninfo = 1;
    PMIX_GDS_FETCH_KV(rc, pmix_globals.mypeer, &cb);
    cb.key = NULL;
    cb.info = NULL;
    cb.ninfo = 0;
    if (PMIX_SUCCESS == rc && 1 == pmix_list_get_size(&cb.kvs)) {
        kv = (pmix_kval_t *) pmix_list_get_first(&cb.kvs);
        if (NULL != kv->value && PMIX_STRING == kv->value->type &&
            NULL != kv->value->data.string) {
            map = strdup(kv->value->data.string);
        }
    }
    PMIX_DESTRUCT(&cb);
    PMIX_INFO_DESTRUCT(&opt);
    return map;
}

/* Ask for PMIX_ANL_MAP for a job registered with the given layout, and
 * hold both the answer and what was stored against the expected string.
 * A second request must be answered too - from the store this time. */
static void check_anl_map(const char *name, const char *nspace, const char *nodelist,
                          const char *ppn, uint32_t nprocs, const char *supplied,
                          const char *expected)
{
    char label[256];
    char *map;
    pmix_status_t rc;

    rc = register_map_job(nspace, nodelist, ppn, nprocs, supplied);
    if (PMIX_SUCCESS != rc) {
        snprintf(label, sizeof(label), "%s: job registers (%s)", name, PMIx_Error_string(rc));
        report(label, false);
        return;
    }

    rc = do_get(nspace, PMIX_RANK_WILDCARD, PMIX_ANL_MAP);
    snprintf(label, sizeof(label), "%s: request answered", name);
    report(label, PMIX_SUCCESS == rc && cb_fired && PMIX_SUCCESS == cb_status && 0 < cb_ndata);
    snprintf(label, sizeof(label), "%s: request parks nothing", name);
    report(label, nothing_parked());

    map = stored_anl_map(nspace);
    snprintf(label, sizeof(label), "%s: stored map is %s (got %s)", name, expected,
             (NULL == map) ? "nothing" : map);
    report(label, NULL != map && 0 == strcmp(map, expected));
    free(map);

    rc = do_get(nspace, PMIX_RANK_WILDCARD, PMIX_ANL_MAP);
    snprintf(label, sizeof(label), "%s: repeat request answered", name);
    report(label, PMIX_SUCCESS == rc && cb_fired && PMIX_SUCCESS == cb_status && 0 < cb_ndata);
}

/* Ask for PMIX_ANL_MAP for a job whose layout cannot yield one. No map
 * may be answered and nothing may be stored. What the server answers
 * instead is its usual handling of an absent key, which is not what is
 * under test here. */
static void check_no_anl_map(const char *name, const char *nspace, const char *nodelist,
                             const char *ppn, uint32_t nprocs)
{
    char label[256];
    char *map;
    pmix_status_t rc;

    rc = register_map_job(nspace, nodelist, ppn, nprocs, NULL);
    snprintf(label, sizeof(label), "%s: job registers (%s)", name, PMIx_Error_string(rc));
    report(label, PMIX_SUCCESS == rc);
    if (PMIX_SUCCESS != rc) {
        return;
    }

    rc = do_get(nspace, PMIX_RANK_WILDCARD, PMIX_ANL_MAP);
    snprintf(label, sizeof(label), "%s: no map answered", name);
    report(label, PMIX_SUCCESS != rc || !cb_fired || PMIX_SUCCESS != cb_status ||
                  0 == cb_ndata);
    snprintf(label, sizeof(label), "%s: request parks nothing", name);
    report(label, nothing_parked());

    map = stored_anl_map(nspace);
    snprintf(label, sizeof(label), "%s: stores nothing (got %s)", name,
             (NULL == map) ? "nothing" : map);
    report(label, NULL == map);
    free(map);
}

int main(int argc, char **argv)
{
    static pmix_server_module_t mymodule = {0};
    pmix_status_t rc;
    char *nodes = NULL, *ppn = NULL, *tmp;
    uint32_t rank = 0, nd;
    bool ok = true;

    (void) argc;
    (void) argv;

    fprintf(stdout, "server_anlmap: server-side PMIX_ANL_MAP unit tests\n");

    rc = PMIx_server_init(&mymodule, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "PMIx_server_init failed: %s\n", PMIx_Error_string(rc));
        return 1;
    }

    /* --- PMIX_ANL_MAP worked out from the job's own map --------------- */
    /* None of these jobs is given PMIX_ANL_MAP by its host, so each
     * answer is computed by the server from the node list and per-node
     * peers, and stored for the job's other procs. Each layout is one a
     * PMI-2 reader expands back to the same placement: blocks are
     * expanded in order and cycled when ranks remain. */
    check_anl_map("ANL map by slot", "anl-ut-slot", "anlut-a,anlut-b", "0,1;2,3", 4, NULL,
                  "(vector,(0,2,2))");
    check_anl_map("ANL map by node", "anl-ut-node", "anlut-a,anlut-b", "0,2;1,3", 4, NULL,
                  "(vector,(0,2,1))");
    check_anl_map("ANL map uneven", "anl-ut-uneven", "anlut-a,anlut-b", "0,1,2;3", 4, NULL,
                  "(vector,(0,1,3),(1,1,1))");
    /* the last pass over the nodes is cut short, and is still one block */
    check_anl_map("ANL map short last pass", "anl-ut-wrap", "anlut-a,anlut-b,anlut-c",
                  "0,3,6;1,4;2,5", 7, NULL, "(vector,(0,3,1))");
    /* node ids follow rank order, not the order the host listed nodes */
    check_anl_map("ANL map renumbered", "anl-ut-renum", "anlut-a,anlut-b", "2,3;0,1", 4, NULL,
                  "(vector,(0,2,2))");
    /* a cycle of two different blocks */
    check_anl_map("ANL map two-block cycle", "anl-ut-twoblk", "anlut-a,anlut-b,anlut-c",
                  "0,1,4,5;2,6;3,7", 8, NULL, "(vector,(0,1,2),(1,2,1))");

    /* --- a map the host supplied is the one answered ----------------- */
    /* deliberately not what the layout would produce, so a value worked
     * out by the server cannot pass for it */
    check_anl_map("ANL map from the host", "anl-ut-host", "anlut-a,anlut-b", "0,1;2,3", 4,
                  "(vector,(0,1,4))", "(vector,(0,1,4))");

    /* --- a job size the map does not fully place --------------------- */
    /* rank 4 is on no node, so no map can describe the job */
    check_no_anl_map("ANL map with an unplaced rank", "anl-ut-unplaced", "anlut-a,anlut-b",
                     "0,1;2,3", 5);

    /* --- a placement too long to express ----------------------------- */
    /* Two hundred nodes taking one and two ranks in turn. Each node is a
     * block of its own, which runs past the 1024 bytes a PMI-2 value may
     * occupy - so nothing is offered and nothing is stored. */
    for (nd = 0; ok && nd < 200; nd++) {
        if (0 > asprintf(&tmp, "%s%sanlut-n%03u", (NULL == nodes) ? "" : nodes,
                         (NULL == nodes) ? "" : ",", nd)) {
            ok = false;
            break;
        }
        free(nodes);
        nodes = tmp;
        if (0 == nd % 2) {
            rc = asprintf(&tmp, "%s%s%u", (NULL == ppn) ? "" : ppn,
                          (NULL == ppn) ? "" : ";", rank);
            rank += 1;
        } else {
            rc = asprintf(&tmp, "%s%s%u,%u", (NULL == ppn) ? "" : ppn,
                          (NULL == ppn) ? "" : ";", rank, rank + 1);
            rank += 2;
        }
        if (0 > rc) {
            ok = false;
            break;
        }
        free(ppn);
        ppn = tmp;
    }
    report("ANL map too long: layout built", ok);
    if (ok) {
        check_no_anl_map("ANL map too long", "anl-ut-long", nodes, ppn, rank);
    }
    free(nodes);
    free(ppn);

    PMIx_server_finalize();

    fprintf(stdout, "server_anlmap: %d passed, %d failed\n", npass, nfail);
    return (0 == nfail) ? 0 : 1;
}
