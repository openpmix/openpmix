/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * White-box unit tests for how the server identifies the process a
 * request came from when it passes that request up to its host.
 *
 * A server answers requests on behalf of its clients and tools, and the
 * host is the one that decides what each requester may see or do. So the
 * host is told who is asking: the requester's proc, and exactly one
 * PMIX_USERID and one PMIX_GRPID - always the ones recorded for the peer
 * when it connected, in place of any the requester put in the request -
 * and the pid the kernel reported for the connection, where it reported
 * one. A group the requester asks to have the work charged to goes up
 * beside them, as PMIX_REQUESTED_GRPID, for the host to judge. A relaying
 * host's claim about who first made the request (PMIX_ORIGIN_*) goes up
 * as it was given: only the host can judge whether the relay may speak
 * for another, and nothing it claims replaces the relay's own identity.
 *
 * The requesting peer here is a stand-in with a namespace, rank, uid, gid
 * and pid of its own, so nothing the server says about itself can be
 * mistaken for what it says about the requester.
 *
 * Test cases:
 *
 *   pmix_server_add_requester_id
 *      on an empty array        -> the connection's pair alone
 *      on an array that carries
 *         a uid and gid of its own -> both replaced by the connection's;
 *                                    the gid it gave goes up as the
 *                                    requested group, and everything
 *                                    else is kept in order
 *      a requested group given
 *         as a group name          -> resolved to its number
 *      one that cannot be
 *         resolved                 -> the request is refused, and the
 *                                    array left as it was
 *      a second requested group    -> refused
 *      the connection's pid        -> passed up on the local transport,
 *                                     never over TCP, and never the one
 *                                     the requester gave
 *      PMIX_ORIGIN_* claims        -> passed up untouched, beside the
 *                                     connection's own identity
 *      a second copy of one        -> refused
 *   query the server cannot
 *      answer itself            -> the host is given the requester's proc
 *                                  and, in the qualifiers, one uid and
 *                                  one gid - the connection's - and the
 *                                  requester's group choice beside them
 *      and no host query        -> refused: a peer's query is not passed
 *                                  on to any server of our own
 *   resolve_peers               -> the same, for the query the server
 *                                  builds itself
 *   pmix_server_add_requester_type
 *      a client                 -> PMIX_REQUESTOR_IS_CLIENT only, and one
 *                                  it claimed to be a tool is replaced
 *      a launcher               -> PMIX_REQUESTOR_IS_TOOL only
 *      a launcher that took a
 *         job's identity        -> both: it is a tool, though its name is
 *                                  a client's
 *      a relay's claim about
 *         the origin's type     -> passed up as PMIX_ORIGIN_IS_*, beside
 *                                  the relay's own type
 */

#include "src/include/pmix_config.h"

#include "include/pmix.h"
#include "include/pmix_server.h"

#include "src/include/pmix_globals.h"
#include "src/mca/bfrops/bfrops.h"
#include "src/mca/ptl/ptl.h"
#include "src/server/pmix_server_ops.h"

#include <grp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define RID_NSPACE      "reqid-ut-client"
#define RID_RANK        3
#define RID_UID         4242
#define RID_GID         4343
#define RID_PID         5151
#define RID_CLAIMED     7777
#define RID_UNKNOWN_KEY "reqid-ut.unknown-key"

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

/* ------------------------------------------------------------------ */
/* the requesting peer                                                 */
/* ------------------------------------------------------------------ */

static pmix_peer_t *requester = NULL;

static pmix_peer_t *make_requester(void)
{
    pmix_peer_t *p;

    p = PMIX_NEW(pmix_peer_t);
    if (NULL == p) {
        return NULL;
    }
    /* the server's own namespace object supplies the wire format */
    PMIX_RETAIN(pmix_globals.mypeer->nptr);
    p->nptr = pmix_globals.mypeer->nptr;
    memcpy(&p->proc_type, &pmix_globals.mypeer->proc_type, sizeof(pmix_proc_type_t));
    p->info = PMIX_NEW(pmix_rank_info_t);
    if (NULL == p->info) {
        PMIX_RELEASE(p);
        return NULL;
    }
    p->info->pname.nspace = strdup(RID_NSPACE);
    p->info->pname.rank = RID_RANK;
    p->info->uid = RID_UID;
    p->info->gid = RID_GID;
    /* a pid on record, which only the local transport vouches for - the
     * stand-in has no transport, as over TCP, until a case gives it one */
    p->info->pid = RID_PID;
    return p;
}

/* a transport whose sockets the kernel reports a peer for, as usock's do */
static pmix_status_t fake_identity(int sd, uid_t *uid, gid_t *gid, pid_t *pid)
{
    (void) sd;
    *uid = RID_UID;
    *gid = RID_GID;
    *pid = RID_PID;
    return PMIX_SUCCESS;
}

static pmix_ptl_module_t local_transport = {
    .name = "reqid-ut-local",
    .local = true,
    .peer_identity = fake_identity,
};

/* ------------------------------------------------------------------ */
/* what the host was handed                                            */
/* ------------------------------------------------------------------ */

typedef struct {
    bool fired;
    pmix_proc_t proct;
    size_t nuid;
    size_t ngid;
    uint32_t uid;
    uint32_t gid;
    size_t nreq;
    uint32_t reqgid;
    size_t nother;
} rid_seen_t;

static rid_seen_t seen;

static void record_identity(const pmix_info_t *info, size_t ninfo)
{
    size_t n;

    for (n = 0; n < ninfo; n++) {
        if (PMIx_Check_key(info[n].key, PMIX_USERID)) {
            ++seen.nuid;
            if (PMIX_UINT32 == info[n].value.type) {
                seen.uid = info[n].value.data.uint32;
            }
        } else if (PMIx_Check_key(info[n].key, PMIX_GRPID)) {
            ++seen.ngid;
            if (PMIX_UINT32 == info[n].value.type) {
                seen.gid = info[n].value.data.uint32;
            }
        } else if (PMIx_Check_key(info[n].key, PMIX_REQUESTED_GRPID)) {
            ++seen.nreq;
            if (PMIX_UINT32 == info[n].value.type) {
                seen.reqgid = info[n].value.data.uint32;
            }
        } else {
            ++seen.nother;
        }
    }
}

/* Record the requester and its identity, then decline: a declined query
 * is answered from what the server knows, which is all these cases need */
static pmix_status_t stub_query(pmix_proc_t *proct, pmix_query_t *queries, size_t nqueries,
                                pmix_info_cbfunc_t cbfunc, void *cbdata)
{
    size_t n;

    (void) cbfunc;
    (void) cbdata;

    seen.fired = true;
    if (NULL != proct) {
        memcpy(&seen.proct, proct, sizeof(pmix_proc_t));
    }
    for (n = 0; n < nqueries; n++) {
        record_identity(queries[n].qualifiers, queries[n].nqual);
    }
    return PMIX_ERR_NOT_SUPPORTED;
}

static void reset_seen(void)
{
    memset(&seen, 0, sizeof(seen));
}

static bool saw_requester(void)
{
    return seen.fired && PMIX_CHECK_NSPACE(seen.proct.nspace, RID_NSPACE) &&
           RID_RANK == seen.proct.rank;
}

static bool saw_identity_once(void)
{
    return 1 == seen.nuid && 1 == seen.ngid && RID_UID == seen.uid && RID_GID == seen.gid;
}

/* ------------------------------------------------------------------ */
/* driving the handlers                                                */
/* ------------------------------------------------------------------ */

/* The query and resolve handlers thread-shift, so let anything queued
 * behind us run before we look at what the host saw. PMIx_Store_internal
 * blocks on the progress thread, so everything queued ahead of it has run
 * by the time it returns. */
static void progress_barrier(void)
{
    pmix_value_t v;

    PMIX_VALUE_LOAD(&v, "barrier", PMIX_STRING);
    PMIx_Store_internal(&pmix_globals.myid, "reqid-ut.barrier", &v);
    PMIX_VALUE_DESTRUCT(&v);
}

static pmix_server_caddy_t *make_caddy(void)
{
    pmix_server_caddy_t *cd;

    cd = PMIX_NEW(pmix_server_caddy_t);
    if (NULL == cd) {
        return NULL;
    }
    PMIX_RETAIN(requester);
    cd->peer = requester;
    cd->hdr.tag = 0;
    return cd;
}

static pmix_status_t qry_status = PMIX_SUCCESS;

static void qry_cbfunc(pmix_status_t status, pmix_info_t *info, size_t ninfo, void *cbdata,
                       pmix_release_cbfunc_t relfn, void *relcbd)
{
    pmix_server_caddy_t *cd = (pmix_server_caddy_t *) cbdata;

    qry_status = status;
    (void) info;
    (void) ninfo;
    if (NULL != relfn) {
        relfn(relcbd);
    }
    PMIX_RELEASE(cd);
}

/* A query for a key nobody holds, optionally carrying a uid and gid of
 * the requester's own choosing */
static pmix_status_t do_query(bool claim)
{
    pmix_buffer_t *buf;
    pmix_server_caddy_t *cd;
    pmix_query_t qry;
    pmix_status_t rc;
    size_t nqueries = 1;
    uint32_t claimed = RID_CLAIMED;

    PMIX_QUERY_CONSTRUCT(&qry);
    PMIX_ARGV_APPEND(rc, qry.keys, RID_UNKNOWN_KEY);
    if (PMIX_SUCCESS != rc) {
        PMIX_QUERY_DESTRUCT(&qry);
        return rc;
    }
    if (claim) {
        qry.nqual = 2;
        PMIX_INFO_CREATE(qry.qualifiers, 2);
        PMIX_INFO_LOAD(&qry.qualifiers[0], PMIX_USERID, &claimed, PMIX_UINT32);
        PMIX_INFO_LOAD(&qry.qualifiers[1], PMIX_GRPID, &claimed, PMIX_UINT32);
    }

    buf = PMIX_NEW(pmix_buffer_t);
    if (NULL == buf) {
        PMIX_QUERY_DESTRUCT(&qry);
        return PMIX_ERR_NOMEM;
    }
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
    if (NULL == cd) {
        PMIX_RELEASE(buf);
        return PMIX_ERR_NOMEM;
    }
    rc = pmix_server_query(requester, buf, qry_cbfunc, cd);
    PMIX_RELEASE(buf);
    if (PMIX_SUCCESS != rc) {
        /* the switchyard owns the caddy on a non-success return */
        PMIX_RELEASE(cd);
        return rc;
    }
    progress_barrier();
    return PMIX_SUCCESS;
}

static pmix_status_t do_resolve_peers(void)
{
    pmix_buffer_t *buf;
    pmix_server_caddy_t *cd;
    pmix_status_t rc;
    char *cptr = NULL;

    buf = PMIX_NEW(pmix_buffer_t);
    if (NULL == buf) {
        return PMIX_ERR_NOMEM;
    }
    /* no nodename, and the requester's own namespace */
    PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, buf, &cptr, 1, PMIX_STRING);
    if (PMIX_SUCCESS == rc) {
        cptr = (char *) RID_NSPACE;
        PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, buf, &cptr, 1, PMIX_STRING);
    }
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(buf);
        return rc;
    }

    cd = make_caddy();
    if (NULL == cd) {
        PMIX_RELEASE(buf);
        return PMIX_ERR_NOMEM;
    }
    rc = pmix_server_resolve_peers(cd, buf, pmix_server_respeers_cbfunc);
    PMIX_RELEASE(buf);
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(cd);
        return rc;
    }
    progress_barrier();
    return PMIX_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* the cases                                                           */
/* ------------------------------------------------------------------ */

/* how many entries carry the key, and the uint32 or pid the last one held */
static size_t count_key(const pmix_info_t *info, size_t ninfo, const char *key, uint64_t *val)
{
    size_t n, cnt = 0;

    for (n = 0; n < ninfo; n++) {
        if (!PMIx_Check_key(info[n].key, key)) {
            continue;
        }
        ++cnt;
        if (NULL == val) {
            continue;
        }
        if (PMIX_UINT32 == info[n].value.type) {
            *val = info[n].value.data.uint32;
        } else if (PMIX_PID == info[n].value.type) {
            *val = (uint64_t) info[n].value.data.pid;
        } else if (PMIX_BOOL == info[n].value.type) {
            *val = info[n].value.data.flag ? 1 : 0;
        }
    }
    return cnt;
}

static void test_helper(void)
{
    pmix_info_t *info = NULL;
    size_t ninfo = 0;
    uint32_t claimed = RID_CLAIMED, other = RID_CLAIMED + 1;
    pid_t fakepid = RID_PID + 1;
    bool flag = true;
    uint64_t v = 0;
    struct group *gr;
    pmix_status_t rc;
    int ok;

    rc = pmix_server_add_requester_id(requester, &info, &ninfo);
    ok = (PMIX_SUCCESS == rc && 2 == ninfo && NULL != info &&
          PMIx_Check_key(info[0].key, PMIX_USERID) && RID_UID == info[0].value.data.uint32 &&
          PMIx_Check_key(info[1].key, PMIX_GRPID) && RID_GID == info[1].value.data.uint32);
    report("an empty array gets the connection's uid and gid", ok);
    PMIX_INFO_FREE(info, ninfo);

    ninfo = 4;
    PMIX_INFO_CREATE(info, ninfo);
    PMIX_INFO_LOAD(&info[0], "reqid-ut.first", "one", PMIX_STRING);
    PMIX_INFO_LOAD(&info[1], PMIX_USERID, &claimed, PMIX_UINT32);
    PMIX_INFO_LOAD(&info[2], "reqid-ut.second", "two", PMIX_STRING);
    PMIX_INFO_LOAD(&info[3], PMIX_GRPID, &claimed, PMIX_UINT32);
    rc = pmix_server_add_requester_id(requester, &info, &ninfo);
    ok = (PMIX_SUCCESS == rc && 5 == ninfo && NULL != info &&
          PMIx_Check_key(info[0].key, "reqid-ut.first") &&
          0 == strcmp("one", info[0].value.data.string) &&
          PMIx_Check_key(info[1].key, "reqid-ut.second") &&
          0 == strcmp("two", info[1].value.data.string) &&
          PMIx_Check_key(info[2].key, PMIX_USERID) && RID_UID == info[2].value.data.uint32 &&
          PMIx_Check_key(info[3].key, PMIX_GRPID) && RID_GID == info[3].value.data.uint32 &&
          PMIx_Check_key(info[4].key, PMIX_REQUESTED_GRPID) &&
          RID_CLAIMED == info[4].value.data.uint32);
    report("a supplied uid and gid are replaced, the gid goes up as the requested group, "
           "and the rest is kept in order", ok);
    PMIX_INFO_FREE(info, ninfo);

    ninfo = 1;
    PMIX_INFO_CREATE(info, ninfo);
    PMIX_INFO_LOAD(&info[0], PMIX_REQUESTED_GRPID, &claimed, PMIX_UINT32);
    rc = pmix_server_add_requester_id(requester, &info, &ninfo);
    ok = (PMIX_SUCCESS == rc && 1 == count_key(info, ninfo, PMIX_REQUESTED_GRPID, &v) &&
          RID_CLAIMED == v && 1 == count_key(info, ninfo, PMIX_GRPID, &v) && RID_GID == v);
    report("a requested group named as such goes up beside the connection's gid", ok);
    PMIX_INFO_FREE(info, ninfo);

    gr = getgrgid(getegid());
    if (NULL != gr && NULL != gr->gr_name) {
        ninfo = 1;
        PMIX_INFO_CREATE(info, ninfo);
        PMIX_INFO_LOAD(&info[0], PMIX_GRPID, gr->gr_name, PMIX_STRING);
        rc = pmix_server_add_requester_id(requester, &info, &ninfo);
        ok = (PMIX_SUCCESS == rc && 3 == ninfo && NULL != info &&
              PMIx_Check_key(info[2].key, PMIX_REQUESTED_GRPID) &&
              PMIX_UINT32 == info[2].value.type &&
              (uint32_t) getegid() == info[2].value.data.uint32);
        report("a requested group given by name is passed on as its number", ok);
        PMIX_INFO_FREE(info, ninfo);
    } else {
        fprintf(stdout, "  SKIP: this process's group has no name\n");
    }

    ninfo = 1;
    PMIX_INFO_CREATE(info, ninfo);
    PMIX_INFO_LOAD(&info[0], PMIX_GRPID, "reqid-ut-no-such-group", PMIX_STRING);
    rc = pmix_server_add_requester_id(requester, &info, &ninfo);
    ok = (PMIX_ERR_NOT_FOUND == rc && 1 == ninfo && NULL != info &&
          PMIx_Check_key(info[0].key, PMIX_GRPID) && PMIX_STRING == info[0].value.type);
    report("a requested group naming no group is refused, and the array left as it was", ok);
    PMIX_INFO_FREE(info, ninfo);

    ninfo = 1;
    PMIX_INFO_CREATE(info, ninfo);
    PMIX_INFO_LOAD(&info[0], PMIX_GRPID, &flag, PMIX_BOOL);
    rc = pmix_server_add_requester_id(requester, &info, &ninfo);
    report("a requested group of the wrong type is refused",
           PMIX_ERR_BAD_PARAM == rc && 1 == ninfo);
    PMIX_INFO_FREE(info, ninfo);

    ninfo = 2;
    PMIX_INFO_CREATE(info, ninfo);
    PMIX_INFO_LOAD(&info[0], PMIX_GRPID, &claimed, PMIX_UINT32);
    PMIX_INFO_LOAD(&info[1], PMIX_REQUESTED_GRPID, &other, PMIX_UINT32);
    rc = pmix_server_add_requester_id(requester, &info, &ninfo);
    report("two requested groups are refused", PMIX_ERR_BAD_PARAM == rc && 2 == ninfo);
    PMIX_INFO_FREE(info, ninfo);

    /* the pid: never one the requester gave, and never over TCP */
    ninfo = 1;
    PMIX_INFO_CREATE(info, ninfo);
    PMIX_INFO_LOAD(&info[0], PMIX_REQUESTOR_PID, &fakepid, PMIX_PID);
    rc = pmix_server_add_requester_id(requester, &info, &ninfo);
    report("over TCP no pid goes up, and one the requester gave is dropped",
           PMIX_SUCCESS == rc && 0 == count_key(info, ninfo, PMIX_REQUESTOR_PID, NULL));
    PMIX_INFO_FREE(info, ninfo);

    requester->transport = &local_transport;
    ninfo = 1;
    PMIX_INFO_CREATE(info, ninfo);
    PMIX_INFO_LOAD(&info[0], PMIX_REQUESTOR_PID, &fakepid, PMIX_PID);
    rc = pmix_server_add_requester_id(requester, &info, &ninfo);
    ok = (PMIX_SUCCESS == rc && 1 == count_key(info, ninfo, PMIX_REQUESTOR_PID, &v) &&
          RID_PID == v);
    report("on the local transport the connection's pid goes up, not the one given", ok);
    PMIX_INFO_FREE(info, ninfo);

    /* a publish's info is the data published - a host keeps every key it
     * does not know as part of it, so only the uid and gid are added */
    ninfo = 2;
    PMIX_INFO_CREATE(info, ninfo);
    PMIX_INFO_LOAD(&info[0], "reqid-ut.published", "v", PMIX_STRING);
    PMIX_INFO_LOAD(&info[1], PMIX_GRPID, &claimed, PMIX_UINT32);
    rc = pmix_server_add_requester_publisher(requester, &info, &ninfo);
    ok = (PMIX_SUCCESS == rc && 3 == ninfo &&
          1 == count_key(info, ninfo, "reqid-ut.published", NULL) &&
          1 == count_key(info, ninfo, PMIX_USERID, &v) && RID_UID == v &&
          1 == count_key(info, ninfo, PMIX_GRPID, &v) && RID_GID == v &&
          0 == count_key(info, ninfo, PMIX_REQUESTOR_PID, NULL) &&
          0 == count_key(info, ninfo, PMIX_REQUESTED_GRPID, NULL));
    report("a publish gets only the connection's uid and gid, beside its data", ok);
    PMIX_INFO_FREE(info, ninfo);
    requester->transport = NULL;

    /* a relaying host's claim about who first asked goes up as given,
     * and does not displace the relay's own identity */
    ninfo = 5;
    PMIX_INFO_CREATE(info, ninfo);
    PMIX_INFO_LOAD(&info[0], PMIX_ORIGIN_USERID, &claimed, PMIX_UINT32);
    PMIX_INFO_LOAD(&info[1], PMIX_ORIGIN_GRPID, &other, PMIX_UINT32);
    PMIX_INFO_LOAD(&info[2], PMIX_ORIGIN_PID, &fakepid, PMIX_PID);
    PMIX_INFO_LOAD(&info[3], PMIX_ORIGIN_IS_TOOL, &flag, PMIX_BOOL);
    PMIX_INFO_LOAD(&info[4], PMIX_USERID, &claimed, PMIX_UINT32);
    rc = pmix_server_add_requester_id(requester, &info, &ninfo);
    ok = (PMIX_SUCCESS == rc && 6 == ninfo &&
          1 == count_key(info, ninfo, PMIX_ORIGIN_USERID, &v) && RID_CLAIMED == v &&
          1 == count_key(info, ninfo, PMIX_ORIGIN_GRPID, &v) && RID_CLAIMED + 1 == v &&
          1 == count_key(info, ninfo, PMIX_ORIGIN_PID, &v) && (uint64_t) fakepid == v &&
          1 == count_key(info, ninfo, PMIX_ORIGIN_IS_TOOL, &v) && 1 == v &&
          1 == count_key(info, ninfo, PMIX_USERID, &v) && RID_UID == v &&
          1 == count_key(info, ninfo, PMIX_GRPID, &v) && RID_GID == v);
    report("a relay's origin claims go up untouched, beside its own identity", ok);
    PMIX_INFO_FREE(info, ninfo);

    ninfo = 2;
    PMIX_INFO_CREATE(info, ninfo);
    PMIX_INFO_LOAD(&info[0], PMIX_ORIGIN_USERID, &claimed, PMIX_UINT32);
    PMIX_INFO_LOAD(&info[1], PMIX_ORIGIN_USERID, &other, PMIX_UINT32);
    rc = pmix_server_add_requester_id(requester, &info, &ninfo);
    report("two copies of an origin claim are refused", PMIX_ERR_BAD_PARAM == rc && 2 == ninfo);
    PMIX_INFO_FREE(info, ninfo);
}

/* the pair the type helper appended, read off the end of the array */
static bool saw_type(const pmix_info_t *info, size_t ninfo, bool tool, bool client)
{
    size_t n, ntool = 0, nclient = 0;
    bool t = !tool, c = !client;

    for (n = 0; n < ninfo; n++) {
        if (PMIx_Check_key(info[n].key, PMIX_REQUESTOR_IS_TOOL)) {
            ++ntool;
            t = PMIX_INFO_TRUE(&info[n]);
        } else if (PMIx_Check_key(info[n].key, PMIX_REQUESTOR_IS_CLIENT)) {
            ++nclient;
            c = PMIX_INFO_TRUE(&info[n]);
        }
    }
    return (1 == ntool && 1 == nclient && tool == t && client == c);
}

static void test_type(void)
{
    pmix_info_t *info = NULL;
    size_t ninfo = 0;
    uint32_t saved = requester->proc_type.type;
    bool yes = true;
    pmix_status_t rc;
    int ok;

    requester->proc_type.type = PMIX_PROC_CLIENT;
    info = PMIx_Info_create(2);
    PMIx_Info_load(&info[0], RID_UNKNOWN_KEY, &yes, PMIX_BOOL);
    PMIx_Info_load(&info[1], PMIX_REQUESTOR_IS_TOOL, &yes, PMIX_BOOL);
    ninfo = 2;
    rc = pmix_server_add_requester_type(requester, &info, &ninfo);
    ok = (PMIX_SUCCESS == rc && 3 == ninfo && PMIx_Check_key(info[0].key, RID_UNKNOWN_KEY) &&
          saw_type(info, ninfo, false, true));
    report("a client is a client, whatever it claims, and the rest is kept", ok);
    PMIx_Info_free(info, ninfo);

    requester->proc_type.type = PMIX_PROC_LAUNCHER;
    info = NULL;
    ninfo = 0;
    rc = pmix_server_add_requester_type(requester, &info, &ninfo);
    report("a launcher is a tool", PMIX_SUCCESS == rc && saw_type(info, ninfo, true, false));
    PMIx_Info_free(info, ninfo);

    /* the prun in a job script: it took the job's name from its
     * environment, and its spawn is still not that job's */
    requester->proc_type.type = PMIX_PROC_CLIENT_LAUNCHER;
    info = NULL;
    ninfo = 0;
    rc = pmix_server_add_requester_type(requester, &info, &ninfo);
    report("a launcher holding a job's identity is a tool as well as a client",
           PMIX_SUCCESS == rc && saw_type(info, ninfo, true, true));
    PMIx_Info_free(info, ninfo);

    /* a relay said what the process that made the request is - that
     * goes up as its claim, and the relay is still what it is */
    requester->proc_type.type = PMIX_PROC_LAUNCHER;
    info = PMIx_Info_create(1);
    PMIx_Info_load(&info[0], PMIX_ORIGIN_IS_CLIENT, &yes, PMIX_BOOL);
    ninfo = 1;
    rc = pmix_server_add_requester_type(requester, &info, &ninfo);
    ok = (PMIX_SUCCESS == rc && saw_type(info, ninfo, true, false) && 3 == ninfo &&
          PMIx_Check_key(info[0].key, PMIX_ORIGIN_IS_CLIENT) && PMIX_INFO_TRUE(&info[0]));
    report("a relay's claim about the origin's type goes up beside its own", ok);
    PMIx_Info_free(info, ninfo);

    requester->proc_type.type = saved;
}

static void test_query(void)
{
    pmix_server_query_fn_t saved = pmix_host_server.query;
    pmix_status_t rc;

    reset_seen();
    rc = do_query(false);
    report("a query the server cannot answer is accepted", PMIX_SUCCESS == rc);
    report("the host is told the query is from the requester", saw_requester());
    report("the host is given the connection's uid and gid once each", saw_identity_once());

    reset_seen();
    rc = do_query(true);
    report("a query carrying its own uid and gid is accepted", PMIX_SUCCESS == rc);
    report("the host is given the connection's uid and gid, not the ones claimed",
           saw_identity_once());
    report("the gid the requester gave goes up as its requested group",
           1 == seen.nreq && RID_CLAIMED == seen.reqgid);

    /* with no host to ask, a peer's query goes no further - it is not
     * passed to any server of ours under our own identity */
    pmix_host_server.query = NULL;
    reset_seen();
    qry_status = PMIX_SUCCESS;
    rc = do_query(false);
    report("with no host query, a peer's query is taken", PMIX_SUCCESS == rc);
    report("and answered as not supported", PMIX_ERR_NOT_SUPPORTED == qry_status);
    pmix_host_server.query = saved;
}

static void test_resolve(void)
{
    pmix_status_t rc;

    reset_seen();
    rc = do_resolve_peers();
    report("resolve_peers is accepted", PMIX_SUCCESS == rc);
    report("the host is told resolve_peers is from the requester", saw_requester());
    report("the host is given the requester's uid and gid for resolve_peers",
           saw_identity_once());
    report("resolve_peers kept its own qualifiers", 2 == seen.nother);
}

int main(int argc, char **argv)
{
    static pmix_server_module_t mymodule = {0};
    pmix_status_t rc;

    (void) argc;
    (void) argv;

    fprintf(stdout, "server_requester_id: requester identity unit tests\n");

    mymodule.query = stub_query;
    rc = PMIx_server_init(&mymodule, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "PMIx_server_init failed: %s\n", PMIx_Error_string(rc));
        return 1;
    }
    requester = make_requester();
    if (NULL == requester) {
        fprintf(stderr, "could not create the requesting peer\n");
        PMIx_server_finalize();
        return 1;
    }

    test_helper();
    test_type();
    test_query();
    test_resolve();

    PMIX_RELEASE(requester);
    PMIx_server_finalize();

    fprintf(stdout, "server_requester_id: %d passed, %d failed\n", npass, nfail);
    return (0 == nfail) ? 0 : 1;
}
