/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Unit tests for PMIx_Resolve_peers / PMIx_Resolve_nodes
 * (src/client/pmix_client_resolve.c) against a node that hosts none of a
 * namespace's processes.
 *
 * That state is ordinary - a namespace's map can name a node on which it
 * placed nothing - and the datastore reports it as a PMIX_LOCAL_PEERS
 * value that is an empty string rather than a NULL one. Both of the
 * defects below follow from the same fact about PMIx_Argv_split(): given a
 * string with no tokens it returns NULL, not an empty array.
 *
 *   the aggregate walk
 *      PMIx_Resolve_peers() with a NULL namespace visits every namespace
 *      the library knows, records "nspace:peers" for each, and then splits
 *      every recorded entry back apart to fill the proc array. An entry
 *      whose peer list was empty split back to NULL and was indexed
 *      immediately - a SIGSEGV on the progress thread, taken as soon as
 *      any *other* namespace contributed a proc and made the transfer
 *      pass run at all.
 *
 *   the single-namespace path
 *      Asking for that namespace by name counted zero peers and handed
 *      the count to PMIX_PROC_CREATE, which returns NULL for a zero count
 *      exactly as it does for an allocation failure. The call therefore
 *      reported PMIX_ERR_NOMEM where PMIx_Resolve_peers(3) documents an
 *      empty result as PMIX_SUCCESS with a NULL array and a zero count.
 *
 * The process comes up as a PMIx server with a stub host module. That is
 * what puts these on the local path: PMIx_Resolve_peers consults
 * pmix_host_server.query first, and a stub module has none, so the call
 * falls through to the thread-shifted local computation under test. It
 * also means no client, no socket and no DVM are involved.
 *
 * Two namespaces are registered on this one node:
 *
 *   RESOLVE_UT_PEERS  a node map and proc map naming this host with ranks
 *                     0 and 1, so the datastore derives PMIX_LOCAL_PEERS
 *                     of "0,1" for it.
 *   RESOLVE_UT_EMPTY  a PMIX_NODE_INFO_ARRAY naming this host with an
 *                     explicitly empty PMIX_LOCAL_PEERS, and deliberately
 *                     *no* proc map - the map-driven derivation replaces a
 *                     host-supplied PMIX_LOCAL_PEERS, so supplying a proc
 *                     map here would overwrite the value under test.
 *
 * What this file does not reach: the matching empty-list guard in
 * resolve_nodes(). PMIX_NODE_LIST is derived by joining the node map, so
 * the hash datastore cannot produce an empty one through
 * PMIx_server_register_nspace; that guard is hardening against a host that
 * stores the key itself, not a reproduced defect. The node cases below
 * therefore only pin the ordinary answers.
 */

#include "src/include/pmix_config.h"

#include "include/pmix.h"
#include "include/pmix_server.h"

#include "src/include/pmix_globals.h"
#include "src/server/pmix_server_ops.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define RESOLVE_UT_PEERS "resolve-ut-peers"
#define RESOLVE_UT_EMPTY "resolve-ut-empty"

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

/* Register a namespace whose map places ranks 0 and 1 on this node. The
 * datastore derives PMIX_LOCAL_PEERS from the proc map, so this is the
 * namespace that actually has peers here. */
static pmix_status_t register_with_peers(void)
{
    pmix_info_t info[3];
    pmix_nspace_t ns;
    pmix_status_t rc;
    char *noderegex = NULL, *ppnregex = NULL;
    uint32_t nprocs = 2;

    PMIx_generate_regex(pmix_globals.hostname, &noderegex);
    PMIx_generate_ppn("0,1", &ppnregex);

    PMIX_INFO_LOAD(&info[0], PMIX_NODE_MAP, noderegex, PMIX_REGEX);
    PMIX_INFO_LOAD(&info[1], PMIX_PROC_MAP, ppnregex, PMIX_REGEX);
    PMIX_INFO_LOAD(&info[2], PMIX_JOB_SIZE, &nprocs, PMIX_UINT32);

    PMIX_LOAD_NSPACE(ns, RESOLVE_UT_PEERS);
    rc = PMIx_server_register_nspace(ns, 2, info, 3, NULL, NULL);
    if (PMIX_OPERATION_SUCCEEDED == rc) {
        rc = PMIX_SUCCESS;
    }

    PMIX_INFO_DESTRUCT(&info[0]);
    PMIX_INFO_DESTRUCT(&info[1]);
    PMIX_INFO_DESTRUCT(&info[2]);
    free(noderegex);
    free(ppnregex);
    return rc;
}

/* Register a namespace that names this node but places nothing on it, by
 * handing the server a node-info array whose PMIX_LOCAL_PEERS is the empty
 * string. No proc map: store_map() would derive its own PMIX_LOCAL_PEERS
 * and replace this one. */
static pmix_status_t register_without_peers(void)
{
    pmix_info_t info[2];
    pmix_data_array_t *darray;
    pmix_info_t *nd;
    pmix_nspace_t ns;
    pmix_status_t rc;
    uint32_t nprocs = 0;

    PMIX_DATA_ARRAY_CREATE(darray, 2, PMIX_INFO);
    if (NULL == darray) {
        return PMIX_ERR_NOMEM;
    }
    nd = (pmix_info_t *) darray->array;
    PMIX_INFO_LOAD(&nd[0], PMIX_HOSTNAME, pmix_globals.hostname, PMIX_STRING);
    PMIX_INFO_LOAD(&nd[1], PMIX_LOCAL_PEERS, "", PMIX_STRING);

    PMIX_INFO_LOAD(&info[0], PMIX_JOB_SIZE, &nprocs, PMIX_UINT32);
    PMIX_LOAD_KEY(info[1].key, PMIX_NODE_INFO_ARRAY);
    info[1].flags = 0;
    info[1].value.type = PMIX_DATA_ARRAY;
    info[1].value.data.darray = darray;

    PMIX_LOAD_NSPACE(ns, RESOLVE_UT_EMPTY);
    rc = PMIx_server_register_nspace(ns, 0, info, 2, NULL, NULL);
    if (PMIX_OPERATION_SUCCEEDED == rc) {
        rc = PMIX_SUCCESS;
    }

    PMIX_INFO_DESTRUCT(&info[0]);
    PMIX_INFO_DESTRUCT(&info[1]);
    return rc;
}

int main(int argc, char **argv)
{
    static pmix_server_module_t mymodule = {0};
    pmix_status_t rc;
    pmix_proc_t *procs = NULL;
    pmix_nspace_t ns_peers, ns_empty;
    size_t nprocs = 0;
    char *nodelist = NULL;
    size_t n;
    bool allpeers;

    (void) argc;
    (void) argv;

    fprintf(stdout, "resolve_api: PMIx_Resolve_peers / PMIx_Resolve_nodes unit tests\n");

    /* these APIs take a pmix_nspace_t - a fixed-size array - so they must
     * be handed one, not a string literal */
    PMIX_LOAD_NSPACE(ns_peers, RESOLVE_UT_PEERS);
    PMIX_LOAD_NSPACE(ns_empty, RESOLVE_UT_EMPTY);

    rc = PMIx_server_init(&mymodule, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "PMIx_server_init failed: %s\n", PMIx_Error_string(rc));
        return 1;
    }
    /* the stub module has no query entry point, which is what routes these
     * calls to the local computation under test - assert it rather than
     * assume it */
    if (NULL != pmix_host_server.query) {
        fprintf(stderr, "test setup error: host module advertises query\n");
        PMIx_server_finalize();
        return 1;
    }

    rc = register_with_peers();
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "register_with_peers failed: %s\n", PMIx_Error_string(rc));
        PMIx_server_finalize();
        return 1;
    }
    rc = register_without_peers();
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "register_without_peers failed: %s\n", PMIx_Error_string(rc));
        PMIx_server_finalize();
        return 1;
    }

    /* --- baseline: the namespace that does have peers here ---------- */
    rc = PMIx_Resolve_peers(NULL, ns_peers, &procs, &nprocs);
    report("namespace with peers succeeds", PMIX_SUCCESS == rc);
    report("namespace with peers returns both ranks", 2 == nprocs && NULL != procs);
    if (NULL != procs) {
        PMIX_PROC_FREE(procs, nprocs);
        procs = NULL;
    }
    nprocs = 0;

    /* --- a node this namespace placed nothing on -------------------- */
    /* PMIX_PROC_CREATE(0) returns the same NULL an allocation failure
     * does, so this used to be reported as PMIX_ERR_NOMEM */
    rc = PMIx_Resolve_peers(NULL, ns_empty, &procs, &nprocs);
    report("namespace with no peers here succeeds", PMIX_SUCCESS == rc);
    report("namespace with no peers here returns an empty array",
           0 == nprocs && NULL == procs);
    if (NULL != procs) {
        PMIX_PROC_FREE(procs, nprocs);
        procs = NULL;
    }
    nprocs = 0;

    /* --- the aggregate walk across both namespaces ------------------ */
    /* this is the one that segfaulted: the empty namespace was recorded
     * on the results list, and the transfer pass split it back to the
     * NULL that PMIx_Argv_split() returns and indexed it */
    rc = PMIx_Resolve_peers(NULL, NULL, &procs, &nprocs);
    report("aggregate walk survives a namespace with no peers here",
           PMIX_SUCCESS == rc);
    report("aggregate walk returns only the peers that exist",
           2 == nprocs && NULL != procs);
    allpeers = (NULL != procs);
    for (n = 0; n < nprocs && NULL != procs; n++) {
        if (!PMIX_CHECK_NSPACE(procs[n].nspace, ns_peers)) {
            allpeers = false;
        }
    }
    report("aggregate walk attributes every proc to the right namespace", allpeers);
    if (NULL != procs) {
        PMIX_PROC_FREE(procs, nprocs);
        procs = NULL;
    }
    nprocs = 0;

    /* --- nodes, by namespace and aggregated ------------------------- */
    rc = PMIx_Resolve_nodes(ns_peers, &nodelist);
    report("resolve_nodes for a mapped namespace succeeds", PMIX_SUCCESS == rc);
    report("resolve_nodes names this host",
           NULL != nodelist && NULL != strstr(nodelist, pmix_globals.hostname));
    if (NULL != nodelist) {
        free(nodelist);
        nodelist = NULL;
    }

    rc = PMIx_Resolve_nodes(NULL, &nodelist);
    report("aggregate resolve_nodes succeeds", PMIX_SUCCESS == rc);
    report("aggregate resolve_nodes names this host",
           NULL != nodelist && NULL != strstr(nodelist, pmix_globals.hostname));
    if (NULL != nodelist) {
        free(nodelist);
        nodelist = NULL;
    }

    PMIx_server_finalize();

    fprintf(stdout, "resolve_api: %d passed, %d failed\n", npass, nfail);
    return (0 == nfail) ? 0 : 1;
}
