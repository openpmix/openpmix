/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * White-box unit tests for how the server registers an IOF pull request
 * (pmix_server_iofreg and its completion, _iofreg).
 *
 * A pull request names the sources whose output the requester wants. The
 * host decides whether to allow it - the requester's PMIX_USERID and
 * PMIX_GRPID are in the directives it is handed - so the request matches
 * no output until the host approves it. Output that arrives while the host
 * is deciding is not lost: it is cached, and replayed to the requester
 * once the request is approved.
 *
 * The requester is a stand-in peer with a namespace of its own, so output
 * forwarded to it is not mistaken for output sent back to the server. It
 * has no socket: the registration reply comes to rest on its send queue,
 * where the cases read it, but forwarded output is dropped on the way out.
 * So the server's IOF cache is the witness for forwarding - output that
 * matches no approved registration is cached, and output that matches one
 * is not. The stub host holds on to the approval callback, so each case
 * decides when - and how - the host answers.
 *
 * Test cases:
 *
 *   a source with no namespace   -> refused, and the host is not asked
 *   output while the host decides -> not forwarded, but cached
 *   the host approves             -> the requester gets its reply, the
 *                                    output cached meanwhile is handed
 *                                    over, and new output matches
 *   the host refuses              -> the request is removed, and nothing
 *                                    is forwarded
 *   the requester finalizes while
 *      the host decides, and its
 *      slot is reused             -> the answer leaves the new request
 *                                    alone
 *   the directives                -> carry the requester's uid and gid
 */

#include "src/include/pmix_config.h"

#include "include/pmix.h"
#include "include/pmix_server.h"

#include "src/include/pmix_globals.h"
#include "src/mca/bfrops/bfrops.h"
#include "src/mca/ptl/ptl_types.h"
#include "src/server/pmix_server_ops.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IPA_REQ_NSPACE  "iofpa-requester"
#define IPA_REQ2_NSPACE "iofpa-requester2"
#define IPA_SRC_NSPACE  "iofpa-source"
#define IPA_UID         5151
#define IPA_GID         5252

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
/* the stub host                                                       */
/* ------------------------------------------------------------------ */

static bool pull_called = false;
static pmix_op_cbfunc_t pull_cbfunc = NULL;
static void *pull_cbdata = NULL;
static size_t pull_nuid = 0;
static size_t pull_ngid = 0;
static uint32_t pull_uid = 0;
static uint32_t pull_gid = 0;

/* Record the request and hold on to the callback: the case decides when
 * the host answers */
static pmix_status_t stub_iof_pull(const pmix_proc_t procs[], size_t nprocs,
                                   const pmix_info_t directives[], size_t ndirs,
                                   pmix_iof_channel_t channels, pmix_op_cbfunc_t cbfunc,
                                   void *cbdata)
{
    size_t n;

    (void) procs;
    (void) nprocs;
    (void) channels;

    pull_called = true;
    pull_cbfunc = cbfunc;
    pull_cbdata = cbdata;
    pull_nuid = 0;
    pull_ngid = 0;
    for (n = 0; n < ndirs; n++) {
        if (PMIX_CHECK_KEY(&directives[n], PMIX_USERID)) {
            ++pull_nuid;
            pull_uid = directives[n].value.data.uint32;
        } else if (PMIX_CHECK_KEY(&directives[n], PMIX_GRPID)) {
            ++pull_ngid;
            pull_gid = directives[n].value.data.uint32;
        }
    }
    return PMIX_SUCCESS;
}

static void reset_pull(void)
{
    pull_called = false;
    pull_cbfunc = NULL;
    pull_cbdata = NULL;
}

/* ------------------------------------------------------------------ */
/* the requesting peers                                                */
/* ------------------------------------------------------------------ */

static pmix_peer_t *make_peer(const char *nspace)
{
    pmix_peer_t *p;

    p = PMIX_NEW(pmix_peer_t);
    if (NULL == p) {
        return NULL;
    }
    PMIX_RETAIN(pmix_globals.mypeer->nptr);
    p->nptr = pmix_globals.mypeer->nptr;
    memcpy(&p->proc_type, &pmix_globals.mypeer->proc_type, sizeof(pmix_proc_type_t));
    p->info = PMIX_NEW(pmix_rank_info_t);
    if (NULL == p->info) {
        PMIX_RELEASE(p);
        return NULL;
    }
    p->info->pname.nspace = strdup(nspace);
    p->info->pname.rank = 0;
    p->info->uid = IPA_UID;
    p->info->gid = IPA_GID;
    return p;
}

/* Replies queued to a socketless peer - the first sits in send_msg, the
 * rest on send_queue */
static size_t count_replies(pmix_peer_t *p)
{
    pmix_ptl_send_t *snd;
    size_t n = 0;

    if (NULL != p->send_msg) {
        ++n;
    }
    PMIX_LIST_FOREACH (snd, &p->send_queue, pmix_ptl_send_t) {
        ++n;
    }
    return n;
}

static size_t cached(void)
{
    return pmix_list_get_size(&pmix_server_globals.iof);
}

/* Everything the server handles for us is thread-shifted, so let anything
 * queued behind us run before looking. PMIx_Store_internal blocks on the
 * progress thread, so all that was queued ahead of it has run by the time
 * it returns. */
static void progress_barrier(void)
{
    pmix_value_t v;

    PMIX_VALUE_LOAD(&v, "barrier", PMIX_STRING);
    PMIx_Store_internal(&pmix_globals.myid, "iofpa-ut.barrier", &v);
    PMIX_VALUE_DESTRUCT(&v);
}

/* ------------------------------------------------------------------ */
/* driving the server                                                  */
/* ------------------------------------------------------------------ */

/* Send an IOF PULL for one source on stdout, as the switchyard would */
static pmix_status_t do_pull(pmix_peer_t *p, const char *srcnspace)
{
    pmix_buffer_t *buf;
    pmix_server_caddy_t *cd;
    pmix_proc_t src;
    pmix_iof_channel_t channels = PMIX_FWD_STDOUT_CHANNEL;
    pmix_status_t rc;
    size_t nprocs = 1, ninfo = 0, refid = 7;

    PMIX_LOAD_PROCID(&src, srcnspace, PMIX_RANK_WILDCARD);
    buf = PMIX_NEW(pmix_buffer_t);
    if (NULL == buf) {
        return PMIX_ERR_NOMEM;
    }
    PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, buf, &nprocs, 1, PMIX_SIZE);
    if (PMIX_SUCCESS == rc) {
        PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, buf, &src, 1, PMIX_PROC);
    }
    if (PMIX_SUCCESS == rc) {
        PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, buf, &ninfo, 1, PMIX_SIZE);
    }
    if (PMIX_SUCCESS == rc) {
        PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, buf, &channels, 1, PMIX_IOF_CHANNEL);
    }
    if (PMIX_SUCCESS == rc) {
        PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, buf, &refid, 1, PMIX_SIZE);
    }
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(buf);
        return rc;
    }

    cd = PMIX_NEW(pmix_server_caddy_t);
    if (NULL == cd) {
        PMIX_RELEASE(buf);
        return PMIX_ERR_NOMEM;
    }
    PMIX_RETAIN(p);
    cd->peer = p;
    cd->hdr.tag = 0;
    rc = pmix_server_iofreg(p, buf, pmix_server_iofreg_cbfunc, cd);
    PMIX_RELEASE(buf);
    if (PMIX_SUCCESS != rc) {
        /* the switchyard owns the caddy on a non-success return */
        PMIX_RELEASE(cd);
    }
    progress_barrier();
    return rc;
}

/* Deliver one chunk of stdout from the source, as the host would */
static void deliver(const char *text)
{
    pmix_proc_t src;
    pmix_byte_object_t bo;

    PMIX_LOAD_PROCID(&src, IPA_SRC_NSPACE, 0);
    bo.bytes = (char *) text;
    bo.size = strlen(text);
    (void) PMIx_server_IOF_deliver(&src, PMIX_FWD_STDOUT_CHANNEL, &bo, NULL, 0, NULL, NULL);
    progress_barrier();
}

/* The host answers the pull it is holding */
static void host_answers(pmix_status_t status)
{
    if (NULL != pull_cbfunc) {
        pull_cbfunc(status, pull_cbdata);
    }
    progress_barrier();
}

/* the registration a peer holds, if any */
static pmix_iof_req_t *find_req(pmix_peer_t *p)
{
    pmix_iof_req_t *req;
    int i;

    for (i = 0; i < pmix_globals.iof_requests.size; i++) {
        req = (pmix_iof_req_t *) pmix_pointer_array_get_item(&pmix_globals.iof_requests, i);
        if (NULL != req && req->requestor == p) {
            return req;
        }
    }
    return NULL;
}

static void drop_peer_requests(pmix_peer_t *p)
{
    pmix_iof_req_t *req;
    int i;

    for (i = 0; i < pmix_globals.iof_requests.size; i++) {
        req = (pmix_iof_req_t *) pmix_pointer_array_get_item(&pmix_globals.iof_requests, i);
        if (NULL != req && req->requestor == p) {
            pmix_pointer_array_set_item(&pmix_globals.iof_requests, i, NULL);
            PMIX_RELEASE(req);
        }
    }
}

/* ------------------------------------------------------------------ */
/* the cases                                                           */
/* ------------------------------------------------------------------ */

static void test_empty_nspace(void)
{
    pmix_peer_t *p = make_peer(IPA_REQ_NSPACE);
    pmix_status_t rc;

    reset_pull();
    rc = do_pull(p, "");
    report("a pull naming a source with no namespace is refused", PMIX_ERR_BAD_PARAM == rc);
    report("and the host was not asked", !pull_called);
    report("and nothing was registered", NULL == find_req(p));
    PMIX_RELEASE(p);
}

static void test_approved(void)
{
    pmix_peer_t *p = make_peer(IPA_REQ_NSPACE);
    pmix_iof_req_t *req;
    size_t ncached;
    pmix_status_t rc;

    reset_pull();
    rc = do_pull(p, IPA_SRC_NSPACE);
    report("a pull is accepted and handed to the host", PMIX_SUCCESS == rc && pull_called);
    report("the host is given the requester's uid and gid",
           1 == pull_nuid && 1 == pull_ngid && IPA_UID == pull_uid && IPA_GID == pull_gid);
    req = find_req(p);
    report("the request holds its slot but matches nothing yet",
           NULL != req && PMIX_FWD_NO_CHANNELS == req->channels);

    ncached = cached();
    deliver("early\n");
    report("output while the host decides is not sent - it is cached",
           0 == count_replies(p) && ncached + 1 == cached());

    host_answers(PMIX_SUCCESS);
    report("once approved, the requester gets its reply", 1 == count_replies(p));
    report("and the output cached meanwhile is handed over", ncached == cached());
    req = find_req(p);
    report("the request now matches its channels",
           NULL != req && PMIX_FWD_STDOUT_CHANNEL == req->channels);

    deliver("later\n");
    report("later output matches the request and is not cached", ncached == cached());

    drop_peer_requests(p);
    PMIX_RELEASE(p);
}

static void test_refused(void)
{
    pmix_peer_t *p = make_peer(IPA_REQ_NSPACE);
    size_t ncached;
    pmix_status_t rc;

    reset_pull();
    rc = do_pull(p, IPA_SRC_NSPACE);
    report("a pull the host will refuse is accepted for now", PMIX_SUCCESS == rc);
    ncached = cached();
    deliver("secret\n");

    host_answers(PMIX_ERR_NO_PERMISSIONS);
    report("once refused, the requester gets its reply", 1 == count_replies(p));
    report("the refused request is removed", NULL == find_req(p));
    report("the cached output is not handed over", ncached + 1 == cached());

    deliver("more secret\n");
    report("and later output matches nothing", ncached + 2 == cached());
    PMIX_RELEASE(p);
}

static void test_slot_reused(void)
{
    pmix_peer_t *p1 = make_peer(IPA_REQ_NSPACE);
    pmix_peer_t *p2 = make_peer(IPA_REQ2_NSPACE);
    pmix_op_cbfunc_t cb1;
    void *cbd1;
    pmix_iof_req_t *req2;
    pmix_status_t rc;

    /* the first requester asks, and finalizes before the host answers */
    reset_pull();
    rc = do_pull(p1, IPA_SRC_NSPACE);
    cb1 = pull_cbfunc;
    cbd1 = pull_cbdata;
    drop_peer_requests(p1);

    /* a second requester takes the freed slot */
    reset_pull();
    rc = do_pull(p2, IPA_SRC_NSPACE);
    req2 = find_req(p2);
    report("a second requester registers", PMIX_SUCCESS == rc && NULL != req2);

    /* the host refuses the first request: the second must survive */
    if (NULL != cb1) {
        cb1(PMIX_ERR_NO_PERMISSIONS, cbd1);
    }
    progress_barrier();
    report("refusing a departed request leaves the new one in its slot",
           NULL != find_req(p2) && req2 == find_req(p2));
    report("and still waiting for its own answer",
           NULL != req2 && PMIX_FWD_NO_CHANNELS == req2->channels);

    host_answers(PMIX_ERR_NO_PERMISSIONS);
    PMIX_RELEASE(p1);
    PMIX_RELEASE(p2);
}

int main(int argc, char **argv)
{
    static pmix_server_module_t mymodule = {0};
    pmix_status_t rc;

    (void) argc;
    (void) argv;

    fprintf(stdout, "iof_pull_approval: IOF pull registration unit tests\n");

    mymodule.iof_pull = stub_iof_pull;
    rc = PMIx_server_init(&mymodule, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "PMIx_server_init failed: %s\n", PMIx_Error_string(rc));
        return 1;
    }

    test_empty_nspace();
    test_approved();
    test_refused();
    test_slot_reused();

    PMIx_server_finalize();

    fprintf(stdout, "iof_pull_approval: %d passed, %d failed\n", npass, nfail);
    return (0 == nfail) ? 0 : 1;
}
