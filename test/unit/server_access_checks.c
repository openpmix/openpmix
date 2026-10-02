/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * White-box unit tests for where the server library applies access by
 * user and group to what it answers itself - see docs/security-plan.rst,
 * phase 2. test/unit/server_access.c covers how a job's owner and access
 * list are recorded and the rule itself; this covers the places the rule
 * is applied.
 *
 * The requester is a stand-in for a connected client: its own job, a uid
 * and gid no real account has, and no socket - so a reply queued to it
 * comes to rest on its send_msg, where the cases read it back (the idiom
 * of test/unit/server_resolve.c). It is put on the server's client list,
 * as a connected client is.
 *
 * The jobs, all registered by the host except the last:
 *
 *   acc2-req      the requester's own job
 *   acc2-other    another user's, naming no one else
 *   acc2-shared   another user's, naming the requester's uid
 *   acc2-grp      another user's, naming the requester's group
 *   acc2-landing  known here only because data for it arrived - no
 *                 permissions to judge by
 *
 * Test cases:
 *
 *   the rule for a peer        -> own job, the server's own namespace,
 *                                 a listed uid or group allowed; another
 *                                 user's job refused; a job not
 *                                 registered by the host left to where
 *                                 its data is held when answering, and
 *                                 refused when acting on it
 *   GET                        -> another user's job refused; a listed
 *                                 one, its own, and an unregistered one
 *                                 not refused
 *   refresh cache              -> another user's job refused
 *   query                      -> naming another user's job refused
 *   resolve_peers              -> naming another user's job refused; with
 *                                 none named, that job left out
 *   resolve_node               -> naming another user's job refused
 *   monitoring                 -> who a request is for: a client, the
 *                                 host (not restricted), or the user a
 *                                 relayed request names, which is held to
 *                                 that user's jobs; a relayed request
 *                                 naming no user refused
 *   cleanup directives         -> on another user's job refused, on a
 *                                 listed one accepted
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
#define ACC_REQ_UID   4242
#define ACC_REQ_GID   4343
#define ACC_OTHER_UID 5151
#define ACC_OTHER_GID 6161

#define NS_REQ     "acc2-req"
#define NS_OTHER   "acc2-other"
#define NS_SHARED  "acc2-shared"
#define NS_GRP     "acc2-grp"
#define NS_LANDING "acc2-landing"
#define NS_UNKNOWN "acc2-never-heard-of-it"

#define ACC_NPROCS 2

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

/* A blocking round trip through the progress thread: anything queued
 * ahead of it has run when it returns */
static void progress_barrier(void)
{
    pmix_value_t v;

    PMIX_VALUE_LOAD(&v, "barrier", PMIX_STRING);
    PMIx_Store_internal(&pmix_globals.myid, "access-checks-ut.barrier", &v);
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

/* ------------------------------------------------------------------ */
/* the jobs                                                            */
/* ------------------------------------------------------------------ */

/* Register a job of ACC_NPROCS ranks, all on this node - the node map is
 * what gives resolve_peers something to find - owned by uid/gid and,
 * when key is given, naming the one id in that access list */
static pmix_status_t reg(const char *name, uint32_t uid, uint32_t gid, const char *key,
                         uint32_t id)
{
    pmix_info_t info[7], entry;
    pmix_data_array_t *ids, perms;
    pmix_nspace_t ns;
    pmix_status_t rc;
    char *noderegex = NULL, *ppnregex = NULL;
    uint32_t nprocs = ACC_NPROCS;
    size_t ninfo = 6, n;

    PMIx_generate_regex(pmix_globals.hostname, &noderegex);
    PMIx_generate_ppn("0,1", &ppnregex);
    PMIX_INFO_LOAD(&info[0], PMIX_NODE_MAP, noderegex, PMIX_REGEX);
    PMIX_INFO_LOAD(&info[1], PMIX_PROC_MAP, ppnregex, PMIX_REGEX);
    PMIX_INFO_LOAD(&info[2], PMIX_JOB_SIZE, &nprocs, PMIX_UINT32);
    PMIX_INFO_LOAD(&info[3], PMIX_UNIV_SIZE, &nprocs, PMIX_UINT32);
    PMIX_INFO_LOAD(&info[4], PMIX_USERID, &uid, PMIX_UINT32);
    PMIX_INFO_LOAD(&info[5], PMIX_GRPID, &gid, PMIX_UINT32);
    free(noderegex);
    free(ppnregex);
    if (NULL != key) {
        PMIX_DATA_ARRAY_CREATE(ids, 1, PMIX_UINT32);
        ((uint32_t *) ids->array)[0] = id;
        PMIX_INFO_LOAD(&entry, key, ids, PMIX_DATA_ARRAY);
        PMIX_DATA_ARRAY_FREE(ids);
        perms.type = PMIX_INFO;
        perms.size = 1;
        perms.array = &entry;
        PMIX_INFO_LOAD(&info[6], PMIX_ACCESS_PERMISSIONS, &perms, PMIX_DATA_ARRAY);
        PMIX_INFO_DESTRUCT(&entry);
        ninfo = 7;
    }

    PMIX_LOAD_NSPACE(ns, name);
    rc = PMIx_server_register_nspace(ns, ACC_NPROCS, info, ninfo, NULL, NULL);
    if (PMIX_OPERATION_SUCCEEDED == rc) {
        rc = PMIX_SUCCESS;
    }
    progress_barrier();
    for (n = 0; n < ninfo; n++) {
        PMIX_INFO_DESTRUCT(&info[n]);
    }
    return rc;
}

/* A namespace the host never registered: what a GET or dmodex for a
 * remote job leaves behind */
static void add_landing(void)
{
    pmix_namespace_t *ns;

    ns = PMIX_NEW(pmix_namespace_t);
    ns->nspace = strdup(NS_LANDING);
    pmix_list_append(&pmix_globals.nspaces, &ns->super);
}

/* ------------------------------------------------------------------ */
/* the requester                                                       */
/* ------------------------------------------------------------------ */

static pmix_peer_t *requester = NULL;

static pmix_peer_t *make_peer(const char *nsname, pmix_rank_t rank, uid_t uid, gid_t gid)
{
    pmix_namespace_t *nptr;
    pmix_peer_t *p;

    nptr = find_ns(nsname);
    if (NULL == nptr) {
        return NULL;
    }
    /* no client of this job ever connected, so give it the server's
     * wire format */
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

/* Take the reply a handler queued to the requester */
static pmix_buffer_t *take_queued_reply(void)
{
    pmix_ptl_send_t *snd = requester->send_msg;
    pmix_buffer_t *buf;

    if (NULL != snd) {
        requester->send_msg = NULL;
    } else {
        snd = (pmix_ptl_send_t *) pmix_list_remove_first(&requester->send_queue);
        if (NULL == snd) {
            return NULL;
        }
    }
    buf = snd->data;
    snd->data = NULL;
    PMIX_RELEASE(snd);
    return buf;
}

/* the status at the head of the queued reply */
static pmix_status_t queued_status(void)
{
    pmix_buffer_t *reply;
    pmix_status_t rc, ret;
    int32_t cnt = 1;

    reply = take_queued_reply();
    if (NULL == reply) {
        return PMIX_ERR_NOT_FOUND;
    }
    PMIX_BFROPS_UNPACK(rc, pmix_globals.mypeer, reply, &ret, &cnt, PMIX_STATUS);
    PMIX_RELEASE(reply);
    return (PMIX_SUCCESS == rc) ? ret : rc;
}

static pmix_server_caddy_t *make_caddy(void)
{
    pmix_server_caddy_t *cd;

    cd = PMIX_NEW(pmix_server_caddy_t);
    PMIX_RETAIN(requester);
    cd->peer = requester;
    cd->hdr.tag = 0;
    return cd;
}

/* ------------------------------------------------------------------ */
/* the rule for a peer                                                 */
/* ------------------------------------------------------------------ */

static void test_rule(void)
{
    report("a job's own process may access it",
           pmix_server_peer_permitted(requester, find_ns(NS_REQ)));
    report("the server's own namespace is readable",
           pmix_server_peer_permitted(requester, pmix_globals.mypeer->nptr));
    report("another user's job is refused",
           !pmix_server_peer_permitted(requester, find_ns(NS_OTHER)));
    report("a job naming the requester's uid is allowed",
           pmix_server_peer_permitted(requester, find_ns(NS_SHARED)));
    report("a job naming the requester's group is allowed",
           pmix_server_peer_permitted(requester, find_ns(NS_GRP)));
    report("an unregistered job is left to where its data is held",
           pmix_server_peer_may_access(requester, find_ns(NS_LANDING)));
    report("but acting on an unregistered job is refused",
           !pmix_server_peer_permitted(requester, find_ns(NS_LANDING)));
    report("an unknown namespace name is left alone",
           pmix_server_peer_may_access_nspace(requester, NS_UNKNOWN));
    report("another user's job is refused by name",
           !pmix_server_peer_may_access_nspace(requester, NS_OTHER));
}

/* ------------------------------------------------------------------ */
/* GET and refresh cache                                               */
/* ------------------------------------------------------------------ */

static bool get_fired;
static pmix_status_t get_status;

static void get_cbfunc(pmix_status_t status, const char *data, size_t ndata, void *cbdata,
                       pmix_release_cbfunc_t relfn, void *relcbd)
{
    pmix_server_caddy_t *cd = (pmix_server_caddy_t *) cbdata;

    (void) data;
    (void) ndata;
    get_fired = true;
    get_status = status;
    if (NULL != relfn) {
        relfn(relcbd);
    }
    PMIX_RELEASE(cd);
}

/* A GET for the job size of nsname that must not wait: the answer, or
 * that there is none, comes back at once */
static pmix_status_t do_get(const char *nsname)
{
    pmix_buffer_t *buf;
    pmix_server_caddy_t *cd;
    pmix_info_t info;
    pmix_rank_t rank = PMIX_RANK_WILDCARD;
    pmix_status_t rc;
    size_t ninfo = 1;
    char *cptr;

    get_fired = false;
    get_status = PMIX_SUCCESS;
    buf = PMIX_NEW(pmix_buffer_t);
    cptr = (char *) nsname;
    PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, buf, &cptr, 1, PMIX_STRING);
    if (PMIX_SUCCESS == rc) {
        PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, buf, &rank, 1, PMIX_PROC_RANK);
    }
    if (PMIX_SUCCESS == rc) {
        PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, buf, &ninfo, 1, PMIX_SIZE);
    }
    if (PMIX_SUCCESS == rc) {
        PMIX_INFO_LOAD(&info, PMIX_IMMEDIATE, NULL, PMIX_BOOL);
        PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, buf, &info, 1, PMIX_INFO);
        PMIX_INFO_DESTRUCT(&info);
    }
    if (PMIX_SUCCESS == rc) {
        cptr = (char *) PMIX_JOB_SIZE;
        PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, buf, &cptr, 1, PMIX_STRING);
    }
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(buf);
        return rc;
    }
    cd = make_caddy();
    rc = pmix_server_get(buf, get_cbfunc, cd);
    PMIX_RELEASE(buf);
    if (PMIX_SUCCESS != rc) {
        /* the switchyard owns the caddy on a non-success return */
        PMIX_RELEASE(cd);
        return rc;
    }
    progress_barrier();
    return get_fired ? get_status : PMIX_SUCCESS;
}

static pmix_status_t do_refresh(const char *nsname)
{
    pmix_buffer_t *buf;
    pmix_server_caddy_t *cd;
    pmix_rank_t rank = 0;
    pmix_status_t rc;
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
    cd = make_caddy();
    rc = pmix_server_refresh_cache(cd, buf, NULL);
    PMIX_RELEASE(buf);
    if (PMIX_SUCCESS != rc) {
        /* the switchyard owns the caddy on a non-success return */
        PMIX_RELEASE(cd);
    }
    /* an answered refresh is queued to the requester - drop it */
    while (NULL != (buf = take_queued_reply())) {
        PMIX_RELEASE(buf);
    }
    return rc;
}

static void test_get(void)
{
    pmix_status_t rc;

    rc = do_get(NS_OTHER);
    report("GET of another user's job is refused", PMIX_ERR_NO_PERMISSIONS == rc);
    rc = do_get(NS_SHARED);
    report("GET of a job naming the requester is not refused", PMIX_ERR_NO_PERMISSIONS != rc);
    rc = do_get(NS_REQ);
    report("GET of its own job is not refused", PMIX_ERR_NO_PERMISSIONS != rc);
    rc = do_get(NS_LANDING);
    report("GET of an unregistered job is not refused here", PMIX_ERR_NO_PERMISSIONS != rc);

    rc = do_refresh(NS_OTHER);
    report("refreshing another user's job is refused", PMIX_ERR_NO_PERMISSIONS == rc);
    rc = do_refresh(NS_REQ);
    report("refreshing its own job is not refused", PMIX_ERR_NO_PERMISSIONS != rc);
}

/* ------------------------------------------------------------------ */
/* query and resolve                                                   */
/* ------------------------------------------------------------------ */

static bool qry_fired;
static pmix_status_t qry_status;

static void qry_cbfunc(pmix_status_t status, pmix_info_t *info, size_t ninfo, void *cbdata,
                       pmix_release_cbfunc_t relfn, void *relcbd)
{
    pmix_server_caddy_t *cd = (pmix_server_caddy_t *) cbdata;

    (void) info;
    (void) ninfo;
    qry_fired = true;
    qry_status = status;
    if (NULL != relfn) {
        relfn(relcbd);
    }
    PMIX_RELEASE(cd);
}

/* A query for the job size of nsname */
static pmix_status_t do_query(const char *nsname)
{
    pmix_buffer_t *buf;
    pmix_server_caddy_t *cd;
    pmix_query_t qry;
    pmix_status_t rc;
    size_t nqueries = 1;

    qry_fired = false;
    qry_status = PMIX_SUCCESS;
    PMIX_QUERY_CONSTRUCT(&qry);
    PMIX_ARGV_APPEND(rc, qry.keys, PMIX_JOB_SIZE);
    qry.nqual = 1;
    PMIX_INFO_CREATE(qry.qualifiers, 1);
    PMIX_INFO_LOAD(&qry.qualifiers[0], PMIX_NSPACE, nsname, PMIX_STRING);

    buf = PMIX_NEW(pmix_buffer_t);
    PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, buf, &nqueries, 1, PMIX_SIZE);
    if (PMIX_SUCCESS == rc) {
        PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, buf, &qry, 1, PMIX_QUERY);
    }
    PMIX_QUERY_DESTRUCT(&qry);
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(buf);
        return rc;
    }
    cd = make_caddy();
    rc = pmix_server_query(requester, buf, qry_cbfunc, cd);
    PMIX_RELEASE(buf);
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(cd);
        return rc;
    }
    progress_barrier();
    return qry_fired ? qry_status : PMIX_ERR_TIMEOUT;
}

/* resolve_peers on this node, for nsname or - when NULL - every job. The
 * namespaces answered are counted into the three flags */
static pmix_status_t do_resolve_peers(const char *nsname, bool *req, bool *other, bool *shared)
{
    pmix_server_caddy_t *cd;
    pmix_buffer_t *buf, *reply;
    pmix_status_t rc, ret;
    pmix_proc_t *procs = NULL;
    size_t nprocs = 0, n;
    int32_t cnt;
    char *cptr;

    *req = *other = *shared = false;
    buf = PMIX_NEW(pmix_buffer_t);
    cptr = pmix_globals.hostname;
    PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, buf, &cptr, 1, PMIX_STRING);
    if (PMIX_SUCCESS == rc) {
        cptr = (char *) nsname;
        PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, buf, &cptr, 1, PMIX_STRING);
    }
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(buf);
        return rc;
    }
    cd = make_caddy();
    rc = pmix_server_resolve_peers(cd, buf, pmix_server_respeers_cbfunc);
    PMIX_RELEASE(buf);
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(cd);
        return rc;
    }
    progress_barrier();

    reply = take_queued_reply();
    if (NULL == reply) {
        return PMIX_ERR_NOT_FOUND;
    }
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, pmix_globals.mypeer, reply, &ret, &cnt, PMIX_STATUS);
    if (PMIX_SUCCESS == rc && PMIX_SUCCESS == ret) {
        cnt = 1;
        PMIX_BFROPS_UNPACK(rc, pmix_globals.mypeer, reply, &nprocs, &cnt, PMIX_SIZE);
        if (PMIX_SUCCESS == rc && 0 < nprocs) {
            PMIX_PROC_CREATE(procs, nprocs);
            cnt = (int32_t) nprocs;
            PMIX_BFROPS_UNPACK(rc, pmix_globals.mypeer, reply, procs, &cnt, PMIX_PROC);
            for (n = 0; PMIX_SUCCESS == rc && n < nprocs; n++) {
                if (PMIX_CHECK_NSPACE(procs[n].nspace, NS_REQ)) {
                    *req = true;
                } else if (PMIX_CHECK_NSPACE(procs[n].nspace, NS_OTHER)) {
                    *other = true;
                } else if (PMIX_CHECK_NSPACE(procs[n].nspace, NS_SHARED)) {
                    *shared = true;
                }
            }
            PMIX_PROC_FREE(procs, nprocs);
        }
    }
    PMIX_RELEASE(reply);
    return (PMIX_SUCCESS == rc) ? ret : rc;
}

static pmix_status_t do_resolve_node(const char *nsname)
{
    pmix_server_caddy_t *cd;
    pmix_buffer_t *buf;
    pmix_status_t rc;
    char *cptr = (char *) nsname;

    buf = PMIX_NEW(pmix_buffer_t);
    PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, buf, &cptr, 1, PMIX_STRING);
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(buf);
        return rc;
    }
    cd = make_caddy();
    rc = pmix_server_resolve_node(cd, buf, pmix_server_resnodes_cbfunc);
    PMIX_RELEASE(buf);
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(cd);
        return rc;
    }
    progress_barrier();
    return queued_status();
}

static void test_query_resolve(void)
{
    pmix_status_t rc;
    bool req, other, shared;

    rc = do_query(NS_OTHER);
    report("a query about another user's job is refused", PMIX_ERR_NO_PERMISSIONS == rc);
    rc = do_query(NS_SHARED);
    report("a query about a job naming the requester is answered", PMIX_SUCCESS == rc);

    rc = do_resolve_peers(NS_OTHER, &req, &other, &shared);
    report("resolve_peers naming another user's job is refused", PMIX_ERR_NO_PERMISSIONS == rc);
    rc = do_resolve_peers(NS_SHARED, &req, &other, &shared);
    report("resolve_peers naming a job naming the requester is answered",
           PMIX_SUCCESS == rc && shared && !other);
    rc = do_resolve_peers(NULL, &req, &other, &shared);
    report("resolve_peers naming no job answers only the jobs it may access",
           PMIX_SUCCESS == rc && req && shared && !other);

    rc = do_resolve_node(NS_OTHER);
    report("resolve_node naming another user's job is refused", PMIX_ERR_NO_PERMISSIONS == rc);
    rc = do_resolve_node(NS_SHARED);
    report("resolve_node naming a job naming the requester is answered", PMIX_SUCCESS == rc);
}

/* ------------------------------------------------------------------ */
/* monitoring local processes                                          */
/* ------------------------------------------------------------------ */

static void test_monitor(void)
{
    pmix_peer_t *rpeer;
    pmix_proc_t rproc;
    pmix_info_t relay[2];
    uint32_t requid = ACC_REQ_UID;
    bool host;
    uid_t uid;
    pmix_status_t rc;

    /* a connected client is its own requester */
    PMIX_LOAD_PROCID(&rproc, NS_REQ, 0);
    rc = pmix_server_access_requester(&rproc, NULL, 0, &rpeer, &host, &uid);
    report("monitoring: a client is the requester",
           PMIX_SUCCESS == rc && requester == rpeer && !host && ACC_REQ_UID == uid);
    report("monitoring: a client may see its own job and a job naming it, not another user's",
           pmix_server_access_requester_may(rpeer, host, uid, find_ns(NS_REQ)) &&
               pmix_server_access_requester_may(rpeer, host, uid, find_ns(NS_SHARED)) &&
               !pmix_server_access_requester_may(rpeer, host, uid, find_ns(NS_OTHER)));

    /* the host's own request */
    rc = pmix_server_access_requester(&pmix_globals.myid, NULL, 0, &rpeer, &host, &uid);
    report("monitoring: the host may see any job",
           PMIX_SUCCESS == rc && NULL == rpeer && host &&
               pmix_server_access_requester_may(rpeer, host, uid, find_ns(NS_OTHER)));

    /* a request the host relays from another node, for the requester's
     * user - the requester is not connected here */
    PMIX_LOAD_PROCID(&rproc, "acc2-remote", 0);
    PMIX_INFO_LOAD(&relay[0], PMIX_MONITOR_PROXY, &rproc, PMIX_PROC);
    PMIX_INFO_LOAD(&relay[1], PMIX_USERID, &requid, PMIX_UINT32);
    rc = pmix_server_access_requester(&rproc, relay, 2, &rpeer, &host, &uid);
    report("monitoring: a relayed request is held to the user it is for",
           PMIX_SUCCESS == rc && NULL == rpeer && !host && ACC_REQ_UID == uid &&
               pmix_server_access_requester_may(rpeer, host, uid, find_ns(NS_REQ)) &&
               pmix_server_access_requester_may(rpeer, host, uid, find_ns(NS_SHARED)) &&
               !pmix_server_access_requester_may(rpeer, host, uid, find_ns(NS_OTHER)));

    /* relayed, but not saying for whom - it must not pass as the host's */
    rc = pmix_server_access_requester(&rproc, relay, 1, &rpeer, &host, &uid);
    report("monitoring: a relayed request that does not name its user is refused",
           PMIX_ERR_NO_PERMISSIONS == rc);
    PMIX_INFO_DESTRUCT(&relay[0]);
    PMIX_INFO_DESTRUCT(&relay[1]);
}

/* ------------------------------------------------------------------ */
/* cleanup directives                                                  */
/* ------------------------------------------------------------------ */

/* The server handles cleanup directives itself, but accepts job control
 * at all only from a host that supports it */
static pmix_status_t stub_job_control(const pmix_proc_t *requestor, const pmix_proc_t targets[],
                                      size_t ntargets, const pmix_info_t directives[],
                                      size_t ndirs, pmix_info_cbfunc_t cbfunc, void *cbdata)
{
    (void) requestor;
    (void) targets;
    (void) ntargets;
    (void) directives;
    (void) ndirs;
    (void) cbfunc;
    (void) cbdata;
    return PMIX_ERR_NOT_SUPPORTED;
}

static pmix_status_t do_cleanup(const char *nsname, const char *path)
{
    pmix_buffer_t *buf;
    pmix_proc_t target;
    pmix_info_t info;
    pmix_status_t rc;
    size_t one = 1;

    buf = PMIX_NEW(pmix_buffer_t);
    PMIX_LOAD_PROCID(&target, nsname, PMIX_RANK_WILDCARD);
    PMIX_INFO_LOAD(&info, PMIX_REGISTER_CLEANUP, path, PMIX_STRING);
    PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, buf, &one, 1, PMIX_SIZE);
    if (PMIX_SUCCESS == rc) {
        PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, buf, &target, 1, PMIX_PROC);
    }
    if (PMIX_SUCCESS == rc) {
        PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, buf, &one, 1, PMIX_SIZE);
    }
    if (PMIX_SUCCESS == rc) {
        PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, buf, &info, 1, PMIX_INFO);
    }
    PMIX_INFO_DESTRUCT(&info);
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(buf);
        return rc;
    }
    rc = pmix_server_job_ctrl(requester, buf, NULL, NULL);
    PMIX_RELEASE(buf);
    return rc;
}

static void test_cleanup(void)
{
    char tmpl[] = "/tmp/acc2-ut.XXXXXX", *dir, *path = NULL;
    pmix_status_t rc;

    /* a file that does not exist, so running the epilog removes nothing */
    dir = mkdtemp(tmpl);
    if (NULL == dir || 0 > asprintf(&path, "%s/nothing-here", dir)) {
        report("could not make a scratch directory", 0);
        return;
    }
    rc = do_cleanup(NS_OTHER, path);
    report("cleanup put on another user's job is refused", PMIX_ERR_NO_PERMISSIONS == rc);
    rc = do_cleanup(NS_LANDING, path);
    report("cleanup put on an unregistered job is refused", PMIX_ERR_NO_PERMISSIONS == rc);
    rc = do_cleanup(NS_SHARED, path);
    report("cleanup put on a job naming the requester is accepted",
           PMIX_SUCCESS == rc || PMIX_OPERATION_SUCCEEDED == rc);
    free(path);
    rmdir(dir);
}

int main(int argc, char **argv)
{
    pmix_user_t *user;
    static pmix_server_module_t mymodule = {0};
    pmix_status_t rc;

    (void) argc;
    (void) argv;

    fprintf(stdout, "server_access_checks: where access by user and group is applied\n");

    mymodule.job_control = stub_job_control;
    rc = PMIx_server_init(&mymodule, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "PMIx_server_init failed: %s\n", PMIx_Error_string(rc));
        return 1;
    }

    if (PMIX_SUCCESS != reg(NS_REQ, ACC_REQ_UID, ACC_REQ_GID, NULL, 0) ||
        PMIX_SUCCESS != reg(NS_OTHER, ACC_OTHER_UID, ACC_OTHER_GID, NULL, 0) ||
        PMIX_SUCCESS != reg(NS_SHARED, ACC_OTHER_UID, ACC_OTHER_GID, PMIX_ACCESS_USERIDS,
                            ACC_REQ_UID) ||
        PMIX_SUCCESS != reg(NS_GRP, ACC_OTHER_UID, ACC_OTHER_GID, PMIX_ACCESS_GRPIDS,
                            ACC_REQ_GID)) {
        fprintf(stderr, "could not register the jobs\n");
        PMIx_server_finalize();
        return 1;
    }
    add_landing();

    requester = make_peer(NS_REQ, 0, ACC_REQ_UID, ACC_REQ_GID);
    if (NULL == requester) {
        fprintf(stderr, "could not create the requester\n");
        PMIx_server_finalize();
        return 1;
    }
    requester->index = pmix_pointer_array_add(&pmix_server_globals.clients, requester);
    /* the requester's user belongs to its group. It has no account for the
     * server to look that up in, so record it the way a lookup would */
    user = pmix_server_user_get((uid_t) ACC_REQ_UID);
    if (NULL == user || NULL == (user->gids = (gid_t *) malloc(sizeof(gid_t)))) {
        fprintf(stderr, "could not record the requester's groups\n");
        PMIx_server_finalize();
        return 1;
    }
    user->gids[0] = (gid_t) ACC_REQ_GID;
    user->ngids = 1;

    test_rule();
    test_get();
    test_query_resolve();
    test_monitor();
    test_cleanup();

    pmix_pointer_array_set_item(&pmix_server_globals.clients, requester->index, NULL);
    PMIX_RELEASE(requester);
    PMIx_server_finalize();

    fprintf(stdout, "server_access_checks: %d passed, %d failed\n", npass, nfail);
    return (0 == nfail) ? 0 : 1;
}
