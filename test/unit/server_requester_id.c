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
 * PMIX_USERID and one PMIX_GRPID. The uid is always the one recorded for
 * the peer when it connected, in place of any the requester put in the
 * request. The gid is the requester's own choice if the request names one
 * - a group to charge the work to, which the host decides whether to
 * accept - and otherwise the one recorded at connection.
 *
 * The requesting peer here is a stand-in with a namespace, rank, uid and
 * gid of its own, so nothing the server says about itself can be
 * mistaken for what it says about the requester.
 *
 * Test cases:
 *
 *   pmix_server_add_requester_id
 *      on an empty array        -> the connection's pair alone
 *      on an array that carries
 *         a uid and gid of its own -> the uid is replaced by the
 *                                    connection's, the gid is kept, and
 *                                    everything else is kept in order
 *      a gid given as a group name -> resolved to its number
 *      a gid that cannot be
 *         resolved                 -> the request is refused, and the
 *                                    array left as it was
 *      a second gid                -> dropped
 *   query the server cannot
 *      answer itself            -> the host is given the requester's proc
 *                                  and, in the qualifiers, one uid and
 *                                  one gid - the requester's own gid
 *                                  choice when it made one
 *   resolve_peers               -> the same, for the query the server
 *                                  builds itself
 */

#include "src/include/pmix_config.h"

#include "include/pmix.h"
#include "include/pmix_server.h"

#include "src/include/pmix_globals.h"
#include "src/mca/bfrops/bfrops.h"
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
    return p;
}

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

static bool saw_identity_once(uint32_t gid)
{
    return 1 == seen.nuid && 1 == seen.ngid && RID_UID == seen.uid && gid == seen.gid;
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

static void qry_cbfunc(pmix_status_t status, pmix_info_t *info, size_t ninfo, void *cbdata,
                       pmix_release_cbfunc_t relfn, void *relcbd)
{
    pmix_server_caddy_t *cd = (pmix_server_caddy_t *) cbdata;

    (void) status;
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

static void test_helper(void)
{
    pmix_info_t *info = NULL;
    size_t ninfo = 0;
    uint32_t claimed = RID_CLAIMED, other = RID_CLAIMED + 1;
    bool flag = true;
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
    ok = (PMIX_SUCCESS == rc && 4 == ninfo && NULL != info &&
          PMIx_Check_key(info[0].key, "reqid-ut.first") &&
          0 == strcmp("one", info[0].value.data.string) &&
          PMIx_Check_key(info[1].key, "reqid-ut.second") &&
          0 == strcmp("two", info[1].value.data.string) &&
          PMIx_Check_key(info[2].key, PMIX_USERID) && RID_UID == info[2].value.data.uint32 &&
          PMIx_Check_key(info[3].key, PMIX_GRPID) && RID_CLAIMED == info[3].value.data.uint32);
    report("a supplied uid is replaced, a supplied gid kept, the rest kept in order", ok);
    PMIX_INFO_FREE(info, ninfo);

    gr = getgrgid(getegid());
    if (NULL != gr && NULL != gr->gr_name) {
        ninfo = 1;
        PMIX_INFO_CREATE(info, ninfo);
        PMIX_INFO_LOAD(&info[0], PMIX_GRPID, gr->gr_name, PMIX_STRING);
        rc = pmix_server_add_requester_id(requester, &info, &ninfo);
        ok = (PMIX_SUCCESS == rc && 2 == ninfo && NULL != info &&
              PMIx_Check_key(info[1].key, PMIX_GRPID) && PMIX_UINT32 == info[1].value.type &&
              (uint32_t) getegid() == info[1].value.data.uint32);
        report("a gid given as a group name is passed on as its number", ok);
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
    report("a gid naming no group is refused, and the array left as it was", ok);
    PMIX_INFO_FREE(info, ninfo);

    ninfo = 1;
    PMIX_INFO_CREATE(info, ninfo);
    PMIX_INFO_LOAD(&info[0], PMIX_GRPID, &flag, PMIX_BOOL);
    rc = pmix_server_add_requester_id(requester, &info, &ninfo);
    report("a gid of the wrong type is refused", PMIX_ERR_BAD_PARAM == rc && 1 == ninfo);
    PMIX_INFO_FREE(info, ninfo);

    ninfo = 2;
    PMIX_INFO_CREATE(info, ninfo);
    PMIX_INFO_LOAD(&info[0], PMIX_GRPID, &claimed, PMIX_UINT32);
    PMIX_INFO_LOAD(&info[1], PMIX_GRPID, &other, PMIX_UINT32);
    rc = pmix_server_add_requester_id(requester, &info, &ninfo);
    ok = (PMIX_SUCCESS == rc && 2 == ninfo && NULL != info &&
          PMIx_Check_key(info[0].key, PMIX_USERID) && RID_UID == info[0].value.data.uint32 &&
          PMIx_Check_key(info[1].key, PMIX_GRPID) && RID_CLAIMED == info[1].value.data.uint32);
    report("of two supplied gids only the first is kept", ok);
    PMIX_INFO_FREE(info, ninfo);
}

static void test_query(void)
{
    pmix_status_t rc;

    reset_seen();
    rc = do_query(false);
    report("a query the server cannot answer is accepted", PMIX_SUCCESS == rc);
    report("the host is told the query is from the requester", saw_requester());
    report("the host is given the connection's uid and gid once each",
           saw_identity_once(RID_GID));

    reset_seen();
    rc = do_query(true);
    report("a query carrying its own uid and gid is accepted", PMIX_SUCCESS == rc);
    report("the host is given the connection's uid and the requester's gid",
           saw_identity_once(RID_CLAIMED));
}

static void test_resolve(void)
{
    pmix_status_t rc;

    reset_seen();
    rc = do_resolve_peers();
    report("resolve_peers is accepted", PMIX_SUCCESS == rc);
    report("the host is told resolve_peers is from the requester", saw_requester());
    report("the host is given the requester's uid and gid for resolve_peers",
           saw_identity_once(RID_GID));
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
    test_query();
    test_resolve();

    PMIX_RELEASE(requester);
    PMIx_server_finalize();

    fprintf(stdout, "server_requester_id: %d passed, %d failed\n", npass, nfail);
    return (0 == nfail) ? 0 : 1;
}
