/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * White-box unit tests for access by user and group to data held on
 * another node - see docs/security-plan.rst, phase 3.
 *
 * One process plays both servers. As the server a requester asks, it takes
 * GETs for a job it does not host and hands them to a stub host's
 * direct_modex, which records each up-call and answers when the test says.
 * As the server that holds a job, it takes PMIx_server_dmodex_request2
 * calls naming a requester.
 *
 * Test cases:
 *
 *   the up-call        -> names the requester by its connection's uid and
 *                         gid and its process ID - not by any it put in
 *                         its own directives
 *   sharing a fetch    -> requesters of one identity share an up-call;
 *                         another identity gets its own
 *   the answer         -> a requester the holder approved is answered, and
 *                         remembered; one it refused is told so, and not
 *   the copy           -> later, an approved requester is answered from
 *                         it; any other requester is asked about again;
 *                         refresh and resolve do not answer from it for a
 *                         requester not approved
 *   the holder         -> answers a requester the rule allows, and one of
 *                         the job's own processes; refuses anyone else;
 *                         answers a request naming no one, and the older
 *                         PMIx_server_dmodex_request, as before
 */

#include "src/include/pmix_config.h"

#include "include/pmix.h"
#include "include/pmix_server.h"

#include "src/include/pmix_globals.h"
#include "src/mca/bfrops/bfrops.h"
#include "src/mca/ptl/base/base.h"
#include "src/server/pmix_server_ops.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ids no real account should have */
#define A_UID 4242
#define A_GID 4343
#define B_UID 5151
#define B_GID 6161

#define NS_A      "dmx-a"       /* requester A's job, owned by A */
#define NS_B      "dmx-b"       /* requester B's job, owned by B */
#define NS_REMOTE "dmx-remote"  /* a job hosted elsewhere */
#define NS_HELD   "dmx-held"    /* a job we hold, owned by A */
#define DMX_KEY   "dmx.val"

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

static void progress_barrier(void)
{
    pmix_value_t v;

    PMIX_VALUE_LOAD(&v, "barrier", PMIX_STRING);
    PMIx_Store_internal(&pmix_globals.myid, "dmx-ut.barrier", &v);
    PMIX_VALUE_DESTRUCT(&v);
}

static pmix_namespace_t *find_ns(const char *name)
{
    pmix_namespace_t *ns;

    PMIX_LIST_FOREACH (ns, &pmix_globals.nspaces, pmix_namespace_t) {
        if (0 == strcmp(ns->nspace, name)) {
            return ns;
        }
    }
    return NULL;
}

static pmix_status_t reg(const char *name, uint32_t uid, uint32_t gid)
{
    pmix_info_t info[3];
    pmix_nspace_t ns;
    pmix_status_t rc;
    uint32_t nprocs = 2;

    PMIX_INFO_LOAD(&info[0], PMIX_JOB_SIZE, &nprocs, PMIX_UINT32);
    PMIX_INFO_LOAD(&info[1], PMIX_USERID, &uid, PMIX_UINT32);
    PMIX_INFO_LOAD(&info[2], PMIX_GRPID, &gid, PMIX_UINT32);
    PMIX_LOAD_NSPACE(ns, name);
    rc = PMIx_server_register_nspace(ns, 0, info, 3, NULL, NULL);
    if (PMIX_OPERATION_SUCCEEDED == rc) {
        rc = PMIX_SUCCESS;
    }
    progress_barrier();
    PMIX_INFO_DESTRUCT(&info[0]);
    PMIX_INFO_DESTRUCT(&info[1]);
    PMIX_INFO_DESTRUCT(&info[2]);
    return rc;
}

static pmix_peer_t *make_peer(const char *nsname, pmix_rank_t rank, uid_t uid, gid_t gid)
{
    pmix_namespace_t *nptr;
    pmix_peer_t *p;

    nptr = find_ns(nsname);
    if (NULL == nptr) {
        return NULL;
    }
    if (NULL == nptr->compat.bfrops) {
        memcpy(&nptr->compat, &pmix_globals.mypeer->nptr->compat, sizeof(pmix_personality_t));
    }
    p = PMIX_NEW(pmix_peer_t);
    PMIX_RETAIN(nptr);
    p->nptr = nptr;
    memcpy(&p->proc_type, &pmix_globals.mypeer->proc_type, sizeof(pmix_proc_type_t));
    p->info = PMIX_NEW(pmix_rank_info_t);
    p->info->pname.nspace = strdup(nsname);
    p->info->pname.rank = rank;
    p->info->uid = uid;
    p->info->gid = gid;
    return p;
}

/* ------------------------------------------------------------------ */
/* the stub host                                                       */
/* ------------------------------------------------------------------ */

#define MAXCALLS 8

typedef struct {
    pmix_proc_t proc;
    uint32_t uid, gid;
    int nuid, ngid, nreq;
    pmix_proc_t requestor;
    pmix_modex_cbfunc_t cbfunc;
    void *cbdata;
} upcall_t;

static upcall_t calls[MAXCALLS] = {0};
static int ncalls = 0;

static pmix_status_t stub_dmodex(const pmix_proc_t *proc, const pmix_info_t info[], size_t ninfo,
                                 pmix_modex_cbfunc_t cbfunc, void *cbdata)
{
    upcall_t *c;
    size_t n;

    if (MAXCALLS <= ncalls) {
        return PMIX_ERR_NOT_SUPPORTED;
    }
    c = &calls[ncalls++];
    memset(c, 0, sizeof(*c));
    memcpy(&c->proc, proc, sizeof(pmix_proc_t));
    for (n = 0; n < ninfo; n++) {
        if (PMIX_CHECK_KEY(&info[n], PMIX_USERID)) {
            c->uid = info[n].value.data.uint32;
            c->nuid++;
        } else if (PMIX_CHECK_KEY(&info[n], PMIX_GRPID)) {
            c->gid = info[n].value.data.uint32;
            c->ngid++;
        } else if (PMIX_CHECK_KEY(&info[n], PMIX_REQUESTOR)) {
            memcpy(&c->requestor, info[n].value.data.proc, sizeof(pmix_proc_t));
            c->nreq++;
        }
    }
    c->cbfunc = cbfunc;
    c->cbdata = cbdata;
    return PMIX_SUCCESS;
}

/* answer an up-call as the holder would: its status, and on success the
 * target's value */
static void answer(upcall_t *c, pmix_status_t status)
{
    pmix_buffer_t buf;
    pmix_kval_t kv;
    pmix_value_t val;
    uint32_t v = 77;
    char *data = NULL;
    size_t sz = 0;
    pmix_status_t rc;

    /* an up-call the server never made has nobody to answer */
    if (NULL == c->cbfunc) {
        report("the up-call being answered was made", 0);
        return;
    }
    if (PMIX_SUCCESS == status) {
        PMIX_CONSTRUCT(&buf, pmix_buffer_t);
        PMIX_VALUE_LOAD(&val, &v, PMIX_UINT32);
        kv.key = (char *) DMX_KEY;
        kv.value = &val;
        PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, &buf, &kv, 1, PMIX_KVAL);
        PMIX_VALUE_DESTRUCT(&val);
        if (PMIX_SUCCESS == rc) {
            PMIX_UNLOAD_BUFFER(&buf, data, sz);
        }
        PMIX_DESTRUCT(&buf);
    }
    c->cbfunc(status, data, sz, c->cbdata, NULL, NULL);
    progress_barrier();
    free(data);
}

/* ------------------------------------------------------------------ */
/* the requests                                                        */
/* ------------------------------------------------------------------ */

typedef struct {
    bool fired;
    pmix_status_t status;
} getres_t;

/* which result each request's caddy reports into */
#define MAXREQS 16
static struct {
    pmix_server_caddy_t *cd;
    getres_t *res;
} pending[MAXREQS];

static void get_cbfunc(pmix_status_t status, const char *data, size_t ndata, void *cbdata,
                       pmix_release_cbfunc_t relfn, void *relcbd)
{
    pmix_server_caddy_t *cd = (pmix_server_caddy_t *) cbdata;
    int n;

    (void) data;
    (void) ndata;
    for (n = 0; n < MAXREQS; n++) {
        if (pending[n].cd == cd) {
            pending[n].res->fired = true;
            pending[n].res->status = status;
            pending[n].cd = NULL;
            break;
        }
    }
    if (NULL != relfn) {
        relfn(relcbd);
    }
    PMIX_RELEASE(cd);
}

static pmix_server_caddy_t *make_caddy(pmix_peer_t *peer, getres_t *res)
{
    pmix_server_caddy_t *cd;
    int n;

    cd = PMIX_NEW(pmix_server_caddy_t);
    PMIX_RETAIN(peer);
    cd->peer = peer;
    cd->hdr.tag = 0;
    for (n = 0; n < MAXREQS; n++) {
        if (NULL == pending[n].cd) {
            pending[n].cd = cd;
            pending[n].res = res;
            break;
        }
    }
    return cd;
}

/* a GET of DMX_KEY for nsname:rank, carrying a PMIX_USERID of its own */
static pmix_status_t do_get(pmix_peer_t *peer, const char *nsname, pmix_rank_t rank,
                            getres_t *res)
{
    pmix_buffer_t *buf;
    pmix_server_caddy_t *cd;
    pmix_info_t info;
    pmix_status_t rc;
    size_t ninfo = 1;
    uint32_t claimed = 1;
    char *cptr = (char *) nsname;

    memset(res, 0, sizeof(*res));
    buf = PMIX_NEW(pmix_buffer_t);
    PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, buf, &cptr, 1, PMIX_STRING);
    if (PMIX_SUCCESS == rc) {
        PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, buf, &rank, 1, PMIX_PROC_RANK);
    }
    if (PMIX_SUCCESS == rc) {
        PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, buf, &ninfo, 1, PMIX_SIZE);
    }
    if (PMIX_SUCCESS == rc) {
        PMIX_INFO_LOAD(&info, PMIX_USERID, &claimed, PMIX_UINT32);
        PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, buf, &info, 1, PMIX_INFO);
        PMIX_INFO_DESTRUCT(&info);
    }
    if (PMIX_SUCCESS == rc) {
        cptr = (char *) DMX_KEY;
        PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, buf, &cptr, 1, PMIX_STRING);
    }
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(buf);
        return rc;
    }
    cd = make_caddy(peer, res);
    rc = pmix_server_get(buf, get_cbfunc, cd);
    PMIX_RELEASE(buf);
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(cd);
        return rc;
    }
    progress_barrier();
    return PMIX_SUCCESS;
}

static pmix_status_t do_refresh(pmix_peer_t *peer, const char *nsname)
{
    pmix_buffer_t *buf;
    pmix_server_caddy_t *cd;
    pmix_rank_t rank = 0;
    pmix_status_t rc;
    pmix_ptl_send_t *snd;
    char *cptr = (char *) nsname;

    buf = PMIX_NEW(pmix_buffer_t);
    PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, buf, &cptr, 1, PMIX_STRING);
    if (PMIX_SUCCESS == rc) {
        PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, buf, &rank, 1, PMIX_PROC_RANK);
    }
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(buf);
        return rc;
    }
    cd = PMIX_NEW(pmix_server_caddy_t);
    PMIX_RETAIN(peer);
    cd->peer = peer;
    cd->hdr.tag = 0;
    rc = pmix_server_refresh_cache(cd, buf, NULL);
    PMIX_RELEASE(buf);
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(cd);
    }
    /* an answered refresh is queued to the peer - drop it */
    if (NULL != peer->send_msg) {
        PMIX_RELEASE(peer->send_msg);
        peer->send_msg = NULL;
    }
    while (NULL != (snd = (pmix_ptl_send_t *) pmix_list_remove_first(&peer->send_queue))) {
        PMIX_RELEASE(snd);
    }
    return rc;
}

/* ------------------------------------------------------------------ */

static void test_requesting(pmix_peer_t *a0, pmix_peer_t *a1, pmix_peer_t *b0)
{
    getres_t ra0, ra1, rb0, again;
    pmix_namespace_t *remote;
    pmix_proc_t a0proc;
    pmix_status_t rc;

    PMIX_LOAD_PROCID(&a0proc, NS_A, 0);

    ncalls = 0;
    rc = do_get(a0, NS_REMOTE, 0, &ra0);
    report("a get for a job hosted elsewhere goes to the host",
           PMIX_SUCCESS == rc && 1 == ncalls && !ra0.fired);
    report("the up-call names the requester by its connection",
           1 == calls[0].nuid && A_UID == calls[0].uid && 1 == calls[0].ngid &&
               A_GID == calls[0].gid && 1 == calls[0].nreq &&
               PMIX_CHECK_PROCID(&calls[0].requestor, &a0proc));

    rc = do_get(a1, NS_REMOTE, 0, &ra1);
    report("a requester of the same identity shares the up-call",
           PMIX_SUCCESS == rc && 1 == ncalls);
    rc = do_get(b0, NS_REMOTE, 0, &rb0);
    report("a requester of another identity gets its own",
           PMIX_SUCCESS == rc && 2 == ncalls && B_UID == calls[1].uid && B_GID == calls[1].gid);

    answer(&calls[0], PMIX_SUCCESS);
    report("the holder's answer reaches both requesters it was for",
           ra0.fired && PMIX_SUCCESS == ra0.status && ra1.fired && PMIX_SUCCESS == ra1.status &&
               !rb0.fired);
    remote = find_ns(NS_REMOTE);
    report("and the identity it approved is remembered",
           NULL != remote && !remote->access.registered && 1 == remote->access.napv &&
               A_UID == remote->access.apv_uids[0] && A_GID == remote->access.apv_gids[0]);

    answer(&calls[1], PMIX_ERR_NO_PERMISSIONS);
    report("a requester the holder refused is told so",
           rb0.fired && PMIX_ERR_NO_PERMISSIONS == rb0.status);
    report("and is not remembered", NULL != remote && 1 == remote->access.napv);

    /* the copy */
    rc = do_get(b0, NS_REMOTE, 0, &again);
    report("a requester not approved is asked about again, not answered from the copy",
           PMIX_SUCCESS == rc && 3 == ncalls && !again.fired && B_UID == calls[2].uid);
    answer(&calls[2], PMIX_ERR_NO_PERMISSIONS);

    rc = do_get(a1, NS_REMOTE, 0, &again);
    report("an approved requester is answered from the copy",
           PMIX_SUCCESS == rc && 3 == ncalls && again.fired && PMIX_SUCCESS == again.status);
    report("an approved requester may be answered from the copy",
           pmix_server_peer_may_use_copy(a1, remote));
    report("one not approved may not", !pmix_server_peer_may_use_copy(b0, remote));
    rc = do_refresh(b0, NS_REMOTE);
    report("a refresh does not answer from the copy for a requester not approved",
           PMIX_ERR_NOT_FOUND == rc);
}

static pmix_status_t dm_status;
static bool dm_fired;
static size_t dm_size;

static void dmresponse(pmix_status_t status, char *data, size_t sz, void *cbdata)
{
    (void) data;
    (void) cbdata;
    dm_status = status;
    dm_size = sz;
    dm_fired = true;
}

/* a job-level direct modex for NS_HELD, naming a requester when uid != 0 */
static pmix_status_t holder_request(uint32_t uid, uint32_t gid, const char *reqns)
{
    pmix_info_t info[3];
    pmix_proc_t target, req;
    pmix_status_t rc;
    size_t n = 0;

    dm_fired = false;
    dm_status = PMIX_ERR_TIMEOUT;
    PMIX_LOAD_PROCID(&target, NS_HELD, PMIX_RANK_WILDCARD);
    if (0 != uid) {
        PMIX_LOAD_PROCID(&req, reqns, 0);
        PMIX_INFO_LOAD(&info[n], PMIX_USERID, &uid, PMIX_UINT32);
        ++n;
        PMIX_INFO_LOAD(&info[n], PMIX_GRPID, &gid, PMIX_UINT32);
        ++n;
        PMIX_INFO_LOAD(&info[n], PMIX_REQUESTOR, &req, PMIX_PROC);
        ++n;
    }
    rc = PMIx_server_dmodex_request2(&target, (0 < n) ? info : NULL, n, dmresponse, NULL);
    progress_barrier();
    while (0 < n) {
        --n;
        PMIX_INFO_DESTRUCT(&info[n]);
    }
    if (PMIX_SUCCESS != rc) {
        return rc;
    }
    return dm_fired ? dm_status : PMIX_ERR_TIMEOUT;
}

static void test_holder(void)
{
    pmix_proc_t target;
    pmix_status_t rc;

    report("the holder answers a requester the rule allows",
           PMIX_SUCCESS == holder_request(A_UID, A_GID, NS_B));
    report("and refuses one it does not",
           PMIX_ERR_NO_PERMISSIONS == holder_request(B_UID, B_GID, NS_B));
    report("the job's own processes are answered",
           PMIX_SUCCESS == holder_request(B_UID, B_GID, NS_HELD));
    report("a request naming no one is answered as before",
           PMIX_SUCCESS == holder_request(0, 0, NULL));

    dm_fired = false;
    PMIX_LOAD_PROCID(&target, NS_HELD, PMIX_RANK_WILDCARD);
    rc = PMIx_server_dmodex_request(&target, dmresponse, NULL);
    progress_barrier();
    report("the older PMIx_server_dmodex_request is answered as before",
           PMIX_SUCCESS == rc && dm_fired && PMIX_SUCCESS == dm_status);
}

int main(int argc, char **argv)
{
    static pmix_server_module_t mymodule = {0};
    pmix_peer_t *a0, *a1, *b0;
    pmix_status_t rc;

    (void) argc;
    (void) argv;

    fprintf(stdout, "server_dmodex_access: access to data held on another node\n");

    mymodule.direct_modex = stub_dmodex;
    rc = PMIx_server_init(&mymodule, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "PMIx_server_init failed: %s\n", PMIx_Error_string(rc));
        return 1;
    }
    if (PMIX_SUCCESS != reg(NS_A, A_UID, A_GID) || PMIX_SUCCESS != reg(NS_B, B_UID, B_GID) ||
        PMIX_SUCCESS != reg(NS_HELD, A_UID, A_GID)) {
        fprintf(stderr, "could not register the jobs\n");
        PMIx_server_finalize();
        return 1;
    }
    a0 = make_peer(NS_A, 0, A_UID, A_GID);
    a1 = make_peer(NS_A, 1, A_UID, A_GID);
    b0 = make_peer(NS_B, 0, B_UID, B_GID);

    test_requesting(a0, a1, b0);
    test_holder();

    PMIX_RELEASE(a0);
    PMIX_RELEASE(a1);
    PMIX_RELEASE(b0);
    PMIx_server_finalize();

    fprintf(stdout, "server_dmodex_access: %d passed, %d failed\n", npass, nfail);
    return (0 == nfail) ? 0 : 1;
}
