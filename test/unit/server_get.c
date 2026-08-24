/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * White-box unit tests for the server side of PMIx_Get - pmix_server_get()
 * in src/server/pmix_server_get.c - and the deferred-request bookkeeping it
 * owns on pmix_server_globals.local_reqs.
 *
 * The process comes up as a PMIx server with a stub host module that
 * deliberately provides no direct_modex entry point, then registers an
 * nspace whose job size is larger than its local size. That combination is
 * what puts the two behaviors under test on the same code path:
 *
 *   local-vs-remote classification
 *      A target rank that is not one of our local ranks must be classified
 *      remote. The rank walk used to consult the loop variable after the
 *      loop had run to completion, i.e. after it had been left pointing at
 *      the list sentinel, and read a peer id out of it; whatever that
 *      garbage index happened to name in the client array decided the
 *      answer. When it named a live slot the request was treated as local
 *      and parked to wait for a commit that is never coming.
 *
 *      Be clear about what this half is worth: it pins the behavior, it
 *      does not reproduce the defect. The stale read stays inside the
 *      enclosing pmix_namespace_t, so it is undefined behavior that no
 *      sanitizer flags, and the value it picks up is usually out of range
 *      - which accidentally produces the right answer. These cases fail
 *      only when the garbage lands on a live client slot. They are here so
 *      that a future rework of the classification cannot regress the
 *      answer silently, not as a before/after reproducer.
 *
 *   deferred-request cleanup
 *      When the request cannot be launched - no host direct_modex support,
 *      or the host rejects the up-call - the tracker created to park it has
 *      to be fully discarded. Each parked requester holds its own reference
 *      on that tracker, so dropping only the creation reference left the
 *      tracker alive but unreachable: off local_reqs, so no later resolve
 *      could ever drain or free it, and still holding the caddy pointer the
 *      switchyard was about to free. The assertion here is that local_reqs
 *      is empty afterwards - which the old code also satisfied, because it
 *      did unlink the tracker; what it did not do was free it. So run this
 *      one under valgrind: the stranded tracker, its request, its info
 *      array and its key are what the leak report shows.
 *
 * Test cases:
 *
 *   remote rank, no direct_modex   -> PMIX_ERR_NOT_FOUND, nothing parked
 *   unknown nspace, no direct_modex-> PMIX_ERR_NOT_FOUND, nothing parked
 *   unknown nspace + IMMEDIATE     -> PMIX_ERR_NOT_FOUND, nothing parked
 *   local rank not yet connected,
 *      + IMMEDIATE                 -> PMIX_ERR_NOT_FOUND, nothing parked
 *   info count that wraps the
 *      allocation                  -> PMIX_ERR_BAD_PARAM (kills an
 *                                     unfixed library rather than
 *                                     failing it - see the case)
 *   info count that truncates      -> PMIX_ERR_BAD_PARAM
 *   WILDCARD rank                  -> job-level data handed to the callback
 *   WILDCARD, missing reserved key -> PMIX_ERR_NOT_FOUND, callback untouched
 *   WILDCARD, missing plain key    -> unchanged: success, empty payload
 *   local rank not yet connected   -> parked, and answered PMIX_ERR_UNREACH
 *                                     by PMIx_server_finalize
 *   empty namespace, with a tracker
 *      already parked on that rank -> PMIX_ERR_BAD_PARAM, and the parked
 *                                     tracker is left alone
 *
 * Every case but the last additionally asserts that the request left
 * local_reqs empty,
 * because a server that parks a request it can never answer hangs the
 * calling client rather than failing it.
 *
 * What this file cannot reach: the companion rule that a reserved key for
 * a *local* target fails fast instead of waiting out a timeout. Getting
 * there needs a local rank that has actually connected, because a
 * registered-but-unconnected one is deferred by the peerid check well
 * above that point - and this test has no real clients. The remote half
 * of the same change is covered by run-server-tests.sh in the swarm.
 */

#include "src/include/pmix_config.h"

#include "include/pmix.h"
#include "include/pmix_server.h"

#include "src/include/pmix_globals.h"
#include "src/mca/bfrops/bfrops.h"
#include "src/server/pmix_server_ops.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define GETUT_NSPACE  "server-get-ut"
#define GETUT_NPROCS  4
#define GETUT_NLOCAL  2   /* ranks 0 and 1 are ours; 2 and 3 are not */

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

/* Stand-in for the switchyard's get_cbfunc. The contract is the same:
 * whoever calls it owns answering the client and releasing the caddy, and
 * the release function (if any) frees the returned payload. */
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

/* Build the wire form of a GET request: nspace, rank, ninfo, info[],
 * and - when one is given - the key. */
static pmix_buffer_t *build_get_request(const char *nspace, pmix_rank_t rank,
                                        const char *key, pmix_info_t *info,
                                        size_t ninfo)
{
    pmix_buffer_t *buf;
    pmix_status_t rc;
    char *cptr = (char *) nspace;

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
    if (0 < ninfo) {
        PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, buf, info, ninfo, PMIX_INFO);
        if (PMIX_SUCCESS != rc) {
            PMIX_RELEASE(buf);
            return NULL;
        }
    }
    if (NULL != key) {
        cptr = (char *) key;
        PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, buf, &cptr, 1, PMIX_STRING);
        if (PMIX_SUCCESS != rc) {
            PMIX_RELEASE(buf);
            return NULL;
        }
    }
    return buf;
}

/* Build a GET request whose info count is a lie: the count is packed but
 * no info structs follow it. A handler that trusts the count never gets as
 * far as the unpack that would have caught it. */
static pmix_buffer_t *build_bad_ninfo_request(const char *nspace, pmix_rank_t rank,
                                              size_t ninfo)
{
    pmix_buffer_t *buf;
    pmix_status_t rc;
    char *cptr = (char *) nspace;

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
    return buf;
}

static pmix_status_t do_bad_ninfo_get(size_t ninfo)
{
    pmix_server_caddy_t *cd;
    pmix_buffer_t *buf;
    pmix_status_t rc;

    cb_fired = false;

    buf = build_bad_ninfo_request(GETUT_NSPACE, 3, ninfo);
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
        PMIX_RELEASE(cd);
    }
    PMIX_RELEASE(buf);
    return rc;
}

/* Drive one server-side GET. Returns what pmix_server_get() returned; the
 * caddy is released here on the error return exactly as the switchyard
 * does, and by the callback otherwise. */
static pmix_status_t do_get(const char *nspace, pmix_rank_t rank,
                            const char *key, pmix_info_t *info, size_t ninfo)
{
    pmix_server_caddy_t *cd;
    pmix_buffer_t *buf;
    pmix_status_t rc;

    cb_fired = false;
    cb_status = PMIX_SUCCESS;
    cb_ndata = 0;

    buf = build_get_request(nspace, rank, key, info, ninfo);
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

/* Register an nspace with GETUT_NPROCS ranks of which only the first
 * GETUT_NLOCAL are on this node, then register those as clients so the
 * nspace reaches all_registered and the rank walk has something to walk. */
static pmix_status_t register_job(void)
{
    pmix_info_t info[4];
    pmix_nspace_t ns;
    pmix_proc_t proc;
    pmix_status_t rc;
    char *noderegex = NULL, *ppnregex = NULL, *nodelist = NULL;
    uint32_t nprocs = GETUT_NPROCS;
    pmix_rank_t r;

    /* two nodes: ours holds ranks 0 and 1, a second node holds 2 and 3.
     * A single-node map would make every rank local and the local-vs-remote
     * decision this test is about would never be taken. */
    if (0 > asprintf(&nodelist, "%s,server-get-ut-elsewhere", pmix_globals.hostname)) {
        return PMIX_ERR_NOMEM;
    }
    PMIx_generate_regex(nodelist, &noderegex);
    PMIx_generate_ppn("0,1;2,3", &ppnregex);

    PMIX_INFO_LOAD(&info[0], PMIX_NODE_MAP, noderegex, PMIX_REGEX);
    PMIX_INFO_LOAD(&info[1], PMIX_PROC_MAP, ppnregex, PMIX_REGEX);
    PMIX_INFO_LOAD(&info[2], PMIX_JOB_SIZE, &nprocs, PMIX_UINT32);
    PMIX_INFO_LOAD(&info[3], PMIX_UNIV_SIZE, &nprocs, PMIX_UINT32);

    PMIX_LOAD_NSPACE(ns, GETUT_NSPACE);
    /* the second argument is the number of LOCAL procs, not the job size */
    rc = PMIx_server_register_nspace(ns, GETUT_NLOCAL, info, 4, NULL, NULL);
    if (PMIX_OPERATION_SUCCEEDED == rc) {
        rc = PMIX_SUCCESS;
    }

    PMIX_INFO_DESTRUCT(&info[0]);
    PMIX_INFO_DESTRUCT(&info[1]);
    PMIX_INFO_DESTRUCT(&info[2]);
    PMIX_INFO_DESTRUCT(&info[3]);
    free(noderegex);
    free(ppnregex);
    free(nodelist);
    if (PMIX_SUCCESS != rc) {
        return rc;
    }

    for (r = 0; r < GETUT_NLOCAL; r++) {
        PMIX_LOAD_PROCID(&proc, GETUT_NSPACE, r);
        rc = PMIx_server_register_client(&proc, geteuid(), getegid(), NULL, NULL, NULL);
        if (PMIX_SUCCESS != rc && PMIX_OPERATION_SUCCEEDED != rc) {
            return rc;
        }
    }
    return PMIX_SUCCESS;
}

static bool nothing_parked(void)
{
    return (0 == pmix_list_get_size(&pmix_server_globals.local_reqs));
}

/* How many requesters are parked on the first (and, where this is used,
 * only) direct-modex tracker. A request that joined a tracker it had no
 * business joining shows up here and nowhere else - the tracker count is
 * unchanged by a join. */
static size_t parked_requesters(void)
{
    pmix_dmdx_local_t *lcd;

    lcd = (pmix_dmdx_local_t *) pmix_list_get_first(&pmix_server_globals.local_reqs);
    if (NULL == lcd ||
        pmix_list_get_end(&pmix_server_globals.local_reqs) == (pmix_list_item_t *) lcd) {
        return 0;
    }
    return pmix_list_get_size(&lcd->loc_reqs);
}

int main(int argc, char **argv)
{
    static pmix_server_module_t mymodule = {0};
    pmix_status_t rc;
    pmix_info_t imm;
    pmix_info_t scopeinfo;
    pmix_scope_t scope;
    bool flag = true;

    (void) argc;
    (void) argv;

    fprintf(stdout, "server_get: server-side PMIx_Get unit tests\n");

    rc = PMIx_server_init(&mymodule, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "PMIx_server_init failed: %s\n", PMIx_Error_string(rc));
        return 1;
    }
    /* the stub module has no direct_modex, which is the configuration these
     * tests need - assert it rather than assume it */
    if (NULL != pmix_host_server.direct_modex) {
        fprintf(stderr, "test setup error: host module advertises direct_modex\n");
        PMIx_server_finalize();
        return 1;
    }

    rc = register_job();
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "register_job failed: %s\n", PMIx_Error_string(rc));
        PMIx_server_finalize();
        return 1;
    }

    /* --- a rank that is not one of ours is remote ------------------- */
    rc = do_get(GETUT_NSPACE, 3, "some.remote.key", NULL, 0);
    report("remote rank reports not-found", PMIX_ERR_NOT_FOUND == rc);
    report("remote rank parks nothing", nothing_parked());

    /* rank 2 is the other non-local one - same answer */
    rc = do_get(GETUT_NSPACE, 2, "some.remote.key", NULL, 0);
    report("second remote rank reports not-found", PMIX_ERR_NOT_FOUND == rc);
    report("second remote rank parks nothing", nothing_parked());

    /* --- an nspace we have never heard of --------------------------- */
    rc = do_get("no-such-nspace", 0, "some.key", NULL, 0);
    report("unknown nspace reports not-found", PMIX_ERR_NOT_FOUND == rc);
    report("unknown nspace parks nothing", nothing_parked());

    /* --- the same, but the caller refuses to wait ------------------- */
    PMIX_INFO_LOAD(&imm, PMIX_IMMEDIATE, &flag, PMIX_BOOL);
    rc = do_get("no-such-nspace", 0, "some.key", &imm, 1);
    report("unknown nspace with IMMEDIATE reports not-found", PMIX_ERR_NOT_FOUND == rc);
    report("unknown nspace with IMMEDIATE parks nothing", nothing_parked());

    /* --- a mistyped PMIX_DATA_SCOPE qualifier ----------------------- */
    /* the directive arrives off the wire, so its type tag is the peer's
     * word. Read raw, the scope would be the low bytes of a pointer -
     * which cannot crash (a scope is only ever compared, never used as an
     * index) but selects which table is searched, so the request would be
     * answered confidently and wrongly */
    PMIX_INFO_LOAD(&scopeinfo, PMIX_DATA_SCOPE, "not-a-scope", PMIX_STRING);
    rc = do_get(GETUT_NSPACE, 0, "some.local.key", &scopeinfo, 1);
    report("mistyped PMIX_DATA_SCOPE is rejected", PMIX_ERR_BAD_PARAM == rc);
    report("mistyped PMIX_DATA_SCOPE parks nothing", nothing_parked());
    PMIX_INFO_DESTRUCT(&scopeinfo);

    /* and a well-formed one still reaches the ordinary answer */
    scope = PMIX_LOCAL;
    PMIX_INFO_LOAD(&scopeinfo, PMIX_DATA_SCOPE, &scope, PMIX_SCOPE);
    rc = do_get(GETUT_NSPACE, 3, "some.remote.key", &scopeinfo, 1);
    report("well-formed PMIX_DATA_SCOPE is accepted", PMIX_ERR_BAD_PARAM != rc);
    PMIX_INFO_DESTRUCT(&scopeinfo);

    /* --- a local rank that has not connected yet -------------------- */
    rc = do_get(GETUT_NSPACE, 0, "some.local.key", &imm, 1);
    report("unconnected local rank with IMMEDIATE reports not-found",
           PMIX_ERR_NOT_FOUND == rc);
    report("unconnected local rank with IMMEDIATE parks nothing", nothing_parked());
    PMIX_INFO_DESTRUCT(&imm);

    /* --- a lying info count off the wire ---------------------------- */
    /* The count is packed and no info structs follow it, so nothing
     * downstream screens it. PMIx_Info_create multiplies it by
     * sizeof(pmix_info_t) with no overflow guard and then constructs that
     * many elements, so this value - chosen to wrap the product - hands
     * back a two-element allocation the constructor loop walks off the end
     * of. Like the info-count case in server_fence.c, this one kills an
     * unfixed library rather than failing it. */
    rc = do_bad_ninfo_get((SIZE_MAX / sizeof(pmix_info_t)) + 2);
    report("wrapping info count is rejected", PMIX_ERR_BAD_PARAM == rc);
    report("wrapping info count parks nothing", nothing_parked());

    /* the same screen catches the count that merely truncates: it names a
     * different number of directives than the scan below the unpack walks.
     * Express it in terms of INT32_MAX rather than a shift, so it is still
     * a rejected value where size_t is 32 bits wide */
    rc = do_bad_ninfo_get(((size_t) INT32_MAX) + 1);
    report("truncating info count is rejected", PMIX_ERR_BAD_PARAM == rc);
    report("truncating info count parks nothing", nothing_parked());

    /* --- job-level data still comes back ---------------------------- */
    rc = do_get(GETUT_NSPACE, PMIX_RANK_WILDCARD, PMIX_JOB_SIZE, NULL, 0);
    report("wildcard get succeeds", PMIX_SUCCESS == rc);
    report("wildcard get answered the caller", cb_fired && PMIX_SUCCESS == cb_status);
    report("wildcard get returned a payload", 0 < cb_ndata);
    report("wildcard get parks nothing", nothing_parked());

    /* --- a reserved key we were never given ------------------------- */
    /* The job registered above supplies no such key, so the job-level
     * fetch finds nothing. That must NOT be answered as a success with an
     * empty payload - the client cannot tell that apart from the value
     * being absent, and it forecloses the one party that may still have
     * it. The request goes up instead, and because this host module has
     * no direct_modex it comes straight back as not-found. */
    rc = do_get(GETUT_NSPACE, PMIX_RANK_WILDCARD, "pmix.getut.absent", NULL, 0);
    report("wildcard get of a missing reserved key reports not-found",
           PMIX_ERR_NOT_FOUND == rc);
    report("wildcard get of a missing reserved key answers nothing", !cb_fired);
    report("wildcard get of a missing reserved key parks nothing", nothing_parked());

    /* --- the same, for a key that is not reserved -------------------- */
    /* Only reserved keys are surfaced to the host: a non-reserved key is
     * put by a process, so a job-level miss on one says nothing about
     * what the host knows. This case is here to pin that narrowing - it
     * still takes the original path and answers with an empty payload. */
    rc = do_get(GETUT_NSPACE, PMIX_RANK_WILDCARD, "getut.absent", NULL, 0);
    report("wildcard get of a missing non-reserved key still succeeds",
           PMIX_SUCCESS == rc);
    report("wildcard get of a missing non-reserved key answered the caller",
           cb_fired && PMIX_SUCCESS == cb_status);
    report("wildcard get of a missing non-reserved key parks nothing",
           nothing_parked());

    /* --- a request still parked when the server finalizes ----------- */
    /* Same target as the IMMEDIATE case above, but willing to wait: rank 0
     * is registered and has not connected, so the request is parked on
     * local_reqs to await its commit. No PMIX_TIMEOUT is given and this
     * arm passes a zero timeval, so nothing is armed that could answer the
     * request behind our back - it is still sitting there when finalize
     * runs. This has to be the last case, since everything above asserts
     * that local_reqs is empty. */
    rc = do_get(GETUT_NSPACE, 0, "some.local.key", NULL, 0);
    report("unconnected local rank defers the request", PMIX_SUCCESS == rc);
    report("deferred request parks a tracker", !nothing_parked());
    report("deferred request is not answered while it waits", !cb_fired);

    /* --- a request naming an empty namespace ------------------------ */
    /* This is the reason the case sits here, with a tracker already
     * parked: PMIX_CHECK_NSPACE short-circuits to "true" for an invalid
     * name, so an empty namespace matches every namespace it is compared
     * against. Unscreened, this request walks local_reqs, finds the
     * tracker parked above on {GETUT_NSPACE, 0} - the ranks agree - and
     * joins it, so the caller is told SUCCESS and will be answered with
     * another proc's data. The contamination runs the other way too: had
     * the empty-namespace request come first, every later request for
     * rank 0 in a real namespace would have joined its tracker instead. */
    rc = do_get("", 0, "some.local.key", NULL, 0);
    report("empty namespace is rejected", PMIX_ERR_BAD_PARAM == rc);
    report("empty namespace parked nothing new",
           1 == pmix_list_get_size(&pmix_server_globals.local_reqs));
    report("empty namespace did not join the parked tracker",
           1 == parked_requesters());

    PMIx_server_finalize();

    /* Finalize owes that requester an answer, and owes itself the caddy
     * back. A bare list destruct gives neither: every request parked on a
     * tracker holds a reference on it, so destructing local_reqs drops
     * only the creation reference and leaves the tracker, the request, the
     * server caddy it holds and the peer that caddy retains alive and
     * unreachable. The callback firing is the visible half; the leak is
     * the half you need valgrind to see. */
    report("finalize answers a parked requester",
           cb_fired && PMIX_ERR_UNREACH == cb_status);

    fprintf(stdout, "server_get: %d passed, %d failed\n", npass, nfail);
    return (0 == nfail) ? 0 : 1;
}
