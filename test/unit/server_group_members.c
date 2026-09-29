/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * White-box unit tests for the group membership the server holds for its
 * clients - src/server/pmix_server_grpmbr.c - and the places it is used.
 *
 * The clients are stand-ins with no socket, put on the server's client
 * list as connected clients are. Two jobs of four processes each are
 * registered, on this node, so a group member given by job (a wildcard
 * rank) can be counted.
 *
 * Test cases:
 *
 *   recording        -> a synchronous construct keeps the host's order, an
 *                       invited group is sorted; the same ID recorded
 *                       again replaces it
 *   expansion        -> the ID with PMIX_RANK_WILDCARD gives every member,
 *                       once; a group rank gives one, counting across a
 *                       member given by job; a group rank past the
 *                       membership, or a member job of unknown size, is
 *                       PMIX_ERR_NOT_FOUND; a name that is not a group,
 *                       and an empty one, pass through
 *   leaving          -> a member that leaves is removed, and the others
 *                       are the rest of the group; the group is dropped
 *                       once no member is a client here
 *   sweep            -> a group whose clients are gone is dropped
 *   host events      -> an announced group is recorded only if a member
 *                       is ours; a leave counts only for its own source
 *   participants     -> by rank, or by job with a wildcard, local-node or
 *                       local-peers rank; an empty namespace names nobody;
 *                       the server itself need not be named
 *   destruct         -> naming no procs, the group we hold supplies the
 *                       members and its failure policy; a group we do not
 *                       hold is PMIX_ERR_NOT_FOUND
 *   collectives      -> a construct naming a group the caller is in goes
 *                       to the host with the group's members; one naming
 *                       a group the caller is not in, or a fence, connect
 *                       or disconnect doing so, is PMIX_ERR_NOT_A_MEMBER
 *   get              -> the whole group, or a group rank past it, refused
 */

#include "src/include/pmix_config.h"

#include "include/pmix.h"
#include "include/pmix_server.h"

#include "src/include/pmix_globals.h"
#include "src/mca/bfrops/bfrops.h"
#include "src/mca/ptl/base/base.h"
#include "src/server/pmix_server_ops.h"
#include "src/util/pmix_name_fns.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define NS_A   "grpmbr-a"
#define NS_B   "grpmbr-b"
#define NPROCS 4

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
    PMIx_Store_internal(&pmix_globals.myid, "grpmbr-ut.barrier", &v);
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

static pmix_status_t reg(const char *name)
{
    pmix_info_t info[4];
    pmix_nspace_t ns;
    pmix_status_t rc;
    char *noderegex = NULL, *ppnregex = NULL;
    uint32_t nprocs = NPROCS;
    size_t n;

    PMIx_generate_regex(pmix_globals.hostname, &noderegex);
    PMIx_generate_ppn("0,1,2,3", &ppnregex);
    PMIX_INFO_LOAD(&info[0], PMIX_NODE_MAP, noderegex, PMIX_REGEX);
    PMIX_INFO_LOAD(&info[1], PMIX_PROC_MAP, ppnregex, PMIX_REGEX);
    PMIX_INFO_LOAD(&info[2], PMIX_JOB_SIZE, &nprocs, PMIX_UINT32);
    PMIX_INFO_LOAD(&info[3], PMIX_UNIV_SIZE, &nprocs, PMIX_UINT32);
    free(noderegex);
    free(ppnregex);
    PMIX_LOAD_NSPACE(ns, name);
    rc = PMIx_server_register_nspace(ns, NPROCS, info, 4, NULL, NULL);
    if (PMIX_OPERATION_SUCCEEDED == rc) {
        rc = PMIX_SUCCESS;
    }
    progress_barrier();
    for (n = 0; n < 4; n++) {
        PMIX_INFO_DESTRUCT(&info[n]);
    }
    /* and its clients, so a collective knows which ranks are here */
    for (n = 0; PMIX_SUCCESS == rc && n < NPROCS; n++) {
        pmix_proc_t p;

        PMIX_LOAD_PROCID(&p, name, (pmix_rank_t) n);
        rc = PMIx_server_register_client(&p, geteuid(), getegid(), NULL, NULL, NULL);
        if (PMIX_OPERATION_SUCCEEDED == rc) {
            rc = PMIX_SUCCESS;
        }
    }
    progress_barrier();
    return rc;
}

/* a connected client of job nsname, with no socket */
static pmix_peer_t *make_client(const char *nsname, pmix_rank_t rank)
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
    p->index = pmix_pointer_array_add(&pmix_server_globals.clients, p);
    return p;
}

static void drop_client(pmix_peer_t *p)
{
    pmix_pointer_array_set_item(&pmix_server_globals.clients, p->index, NULL);
    PMIX_RELEASE(p);
}

static bool same(const pmix_proc_t *a, const char *ns, pmix_rank_t r)
{
    return 0 == strncmp(a->nspace, ns, PMIX_MAX_NSLEN) && a->rank == r;
}

/* ------------------------------------------------------------------ */

static void test_record(void)
{
    pmix_proc_t mbrs[3];
    pmix_group_t *grp;

    PMIX_LOAD_PROCID(&mbrs[0], NS_A, 2);
    PMIX_LOAD_PROCID(&mbrs[1], NS_A, 0);
    PMIX_LOAD_PROCID(&mbrs[2], NS_A, 1);

    pmix_server_grp_record("rec-sync", mbrs, 3, 7, true, false);
    grp = pmix_server_grp_find("rec-sync");
    report("a synchronous construct keeps the host's order",
           NULL != grp && 3 == grp->nmbrs && same(&grp->members[0], NS_A, 2) &&
               same(&grp->members[1], NS_A, 0) && same(&grp->members[2], NS_A, 1) &&
               7 == grp->ctxid && grp->notterm);

    pmix_server_grp_record("rec-inv", mbrs, 3, SIZE_MAX, false, true);
    grp = pmix_server_grp_find("rec-inv");
    report("an invited group is sorted",
           NULL != grp && 3 == grp->nmbrs && same(&grp->members[0], NS_A, 0) &&
               same(&grp->members[1], NS_A, 1) && same(&grp->members[2], NS_A, 2));

    pmix_server_grp_record("rec-sync", mbrs, 1, 8, false, false);
    grp = pmix_server_grp_find("rec-sync");
    report("the same ID recorded again replaces it",
           NULL != grp && 1 == grp->nmbrs && 8 == grp->ctxid && !grp->notterm);

    pmix_server_grp_drop("rec-sync");
    pmix_server_grp_drop("rec-inv");
    report("a dropped group is gone", NULL == pmix_server_grp_find("rec-sync"));
}

static void test_expand(void)
{
    pmix_proc_t mbrs[3], in[3], *out = NULL;
    size_t nout = 0;
    pmix_status_t rc;

    /* B:1, all of A, B:3 - group ranks 0 | 1..4 | 5 */
    PMIX_LOAD_PROCID(&mbrs[0], NS_B, 1);
    PMIX_LOAD_PROCID(&mbrs[1], NS_A, PMIX_RANK_WILDCARD);
    PMIX_LOAD_PROCID(&mbrs[2], NS_B, 3);
    pmix_server_grp_record("exp", mbrs, 3, SIZE_MAX, false, false);

    PMIX_LOAD_PROCID(&in[0], "exp", PMIX_RANK_WILDCARD);
    PMIX_LOAD_PROCID(&in[1], NS_B, 1);
    rc = pmix_server_grp_expand(in, 2, &out, &nout);
    report("the ID with a wildcard gives every member, once",
           PMIX_SUCCESS == rc && 3 == nout && same(&out[0], NS_B, 1) &&
               same(&out[1], NS_A, PMIX_RANK_WILDCARD) && same(&out[2], NS_B, 3));
    PMIX_PROC_FREE(out, nout);

    PMIX_LOAD_PROCID(&in[0], "exp", 0);
    PMIX_LOAD_PROCID(&in[1], "exp", 3);
    PMIX_LOAD_PROCID(&in[2], "exp", 5);
    rc = pmix_server_grp_expand(in, 3, &out, &nout);
    report("a group rank gives one member, counting across a job",
           PMIX_SUCCESS == rc && 3 == nout && same(&out[0], NS_B, 1) &&
               same(&out[1], NS_A, 2) && same(&out[2], NS_B, 3));
    PMIX_PROC_FREE(out, nout);

    PMIX_LOAD_PROCID(&in[0], "exp", 6);
    rc = pmix_server_grp_expand(in, 1, &out, &nout);
    report("a group rank past the membership is not found", PMIX_ERR_NOT_FOUND == rc);

    PMIX_LOAD_PROCID(&in[0], NS_A, 1);
    PMIX_LOAD_PROCID(&in[1], "", PMIX_RANK_WILDCARD);
    rc = pmix_server_grp_expand(in, 2, &out, &nout);
    report("a name that is not a group, and an empty one, pass through",
           PMIX_SUCCESS == rc && 2 == nout && same(&out[0], NS_A, 1) &&
               same(&out[1], "", PMIX_RANK_WILDCARD));
    PMIX_PROC_FREE(out, nout);
    pmix_server_grp_drop("exp");

    PMIX_LOAD_PROCID(&mbrs[0], "grpmbr-unregistered", PMIX_RANK_WILDCARD);
    pmix_server_grp_record("exp2", mbrs, 1, SIZE_MAX, false, false);
    PMIX_LOAD_PROCID(&in[0], "exp2", 0);
    rc = pmix_server_grp_expand(in, 1, &out, &nout);
    report("a member job of unknown size cannot be counted", PMIX_ERR_NOT_FOUND == rc);
    pmix_server_grp_drop("exp2");
}

static void test_leave_sweep(pmix_peer_t *a0, pmix_peer_t *a1)
{
    pmix_proc_t mbrs[3], *others = NULL;
    size_t nothers = 0;
    pmix_group_t *grp;
    pmix_status_t rc;

    PMIX_LOAD_PROCID(&mbrs[0], NS_A, 0);
    PMIX_LOAD_PROCID(&mbrs[1], NS_A, 1);
    PMIX_LOAD_PROCID(&mbrs[2], NS_B, 2);
    pmix_server_grp_record("lv", mbrs, 3, SIZE_MAX, false, false);

    rc = pmix_server_grp_others("lv", &mbrs[0], &others, &nothers);
    report("the others are the rest of the group",
           PMIX_SUCCESS == rc && 2 == nothers && same(&others[0], NS_A, 1) &&
               same(&others[1], NS_B, 2));
    PMIX_PROC_FREE(others, nothers);
    report("a group we do not hold has no others",
           PMIX_ERR_NOT_FOUND == pmix_server_grp_others("nope", &mbrs[0], &others, &nothers));

    pmix_server_grp_remove_member("lv", &mbrs[0]);
    grp = pmix_server_grp_find("lv");
    report("a member that leaves is removed",
           NULL != grp && 2 == grp->nmbrs && same(&grp->members[0], NS_A, 1));
    /* B:2 is not a client here - A:1 is the last local member */
    pmix_server_grp_remove_member("lv", &mbrs[1]);
    report("the group is dropped once no member is ours", NULL == pmix_server_grp_find("lv"));

    /* a group of A:0 and A:1, both ours - then both go */
    pmix_server_grp_record("sw", mbrs, 2, SIZE_MAX, false, false);
    pmix_server_grp_sweep();
    report("the sweep keeps a group with a member here", NULL != pmix_server_grp_find("sw"));
    pmix_pointer_array_set_item(&pmix_server_globals.clients, a0->index, NULL);
    pmix_pointer_array_set_item(&pmix_server_globals.clients, a1->index, NULL);
    pmix_server_grp_sweep();
    report("the sweep drops a group whose clients are gone", NULL == pmix_server_grp_find("sw"));
    pmix_pointer_array_set_item(&pmix_server_globals.clients, a0->index, a0);
    pmix_pointer_array_set_item(&pmix_server_globals.clients, a1->index, a1);
}

static void announce(const char *grpid, const pmix_proc_t *mbrs, size_t n,
                     const pmix_proc_t *source)
{
    pmix_info_t info[2];
    pmix_data_array_t darray;

    darray.type = PMIX_PROC;
    darray.array = (void *) mbrs;
    darray.size = n;
    PMIX_INFO_LOAD(&info[0], PMIX_GROUP_ID, grpid, PMIX_STRING);
    PMIX_INFO_LOAD(&info[1], PMIX_GROUP_MEMBERSHIP, &darray, PMIX_DATA_ARRAY);
    pmix_server_grp_host_event(PMIX_GROUP_CONSTRUCT_COMPLETE, source, info, 2);
    PMIX_INFO_DESTRUCT(&info[0]);
    PMIX_INFO_DESTRUCT(&info[1]);
}

static void test_host_events(void)
{
    pmix_proc_t mbrs[2], src, other;
    pmix_info_t info[2];
    pmix_group_t *grp;

    PMIX_LOAD_PROCID(&src, NS_B, 3);
    PMIX_LOAD_PROCID(&mbrs[0], NS_B, 3);
    PMIX_LOAD_PROCID(&mbrs[1], NS_B, 2);
    announce("ev-none", mbrs, 2, &src);
    report("an announced group with no member here is not recorded",
           NULL == pmix_server_grp_find("ev-none"));

    PMIX_LOAD_PROCID(&mbrs[1], NS_A, 1);
    announce("ev-ours", mbrs, 2, &src);
    grp = pmix_server_grp_find("ev-ours");
    report("an announced group with a member here is recorded, sorted",
           NULL != grp && 2 == grp->nmbrs && same(&grp->members[0], NS_A, 1) &&
               same(&grp->members[1], NS_B, 3));

    /* a leave naming someone other than its source changes nothing */
    PMIX_LOAD_PROCID(&other, NS_B, 3);
    PMIX_INFO_LOAD(&info[0], PMIX_GROUP_ID, "ev-ours", PMIX_STRING);
    PMIX_INFO_LOAD(&info[1], PMIX_EVENT_AFFECTED_PROC, &other, PMIX_PROC);
    PMIX_LOAD_PROCID(&src, NS_A, 1);
    pmix_server_grp_host_event(PMIX_GROUP_LEFT, &src, info, 2);
    grp = pmix_server_grp_find("ev-ours");
    report("a leave counts only for its own source", NULL != grp && 2 == grp->nmbrs);
    PMIX_INFO_DESTRUCT(&info[1]);

    PMIX_LOAD_PROCID(&src, NS_B, 3);
    pmix_server_grp_host_event(PMIX_GROUP_LEFT, &src, info, 1);
    grp = pmix_server_grp_find("ev-ours");
    report("a leave removes its source", NULL != grp && 1 == grp->nmbrs &&
                                              same(&grp->members[0], NS_A, 1));
    PMIX_INFO_DESTRUCT(&info[0]);
    pmix_server_grp_drop("ev-ours");
}

static void test_participants(pmix_peer_t *a1)
{
    pmix_proc_t procs[2];
    pmix_peer_t *me;

    PMIX_LOAD_PROCID(&procs[0], NS_B, 0);
    PMIX_LOAD_PROCID(&procs[1], NS_A, 1);
    report("a participant by rank", pmix_server_grp_is_participant(a1, procs, 2));
    PMIX_LOAD_PROCID(&procs[1], NS_A, PMIX_RANK_WILDCARD);
    report("a participant by its job", pmix_server_grp_is_participant(a1, procs, 2));
    PMIX_LOAD_PROCID(&procs[1], NS_A, PMIX_RANK_LOCAL_NODE);
    report("a participant by its job on this node", pmix_server_grp_is_participant(a1, procs, 2));
    PMIX_LOAD_PROCID(&procs[1], NS_A, PMIX_RANK_LOCAL_PEERS);
    report("a participant by its local peers", pmix_server_grp_is_participant(a1, procs, 2));
    PMIX_LOAD_PROCID(&procs[1], NS_A, 2);
    report("a process not named is not", !pmix_server_grp_is_participant(a1, procs, 2));
    PMIX_LOAD_PROCID(&procs[1], "", PMIX_RANK_WILDCARD);
    report("an empty namespace names nobody", !pmix_server_grp_is_participant(a1, procs, 2));

    me = PMIX_NEW(pmix_peer_t);
    PMIX_RETAIN(pmix_globals.mypeer->info);
    me->info = pmix_globals.mypeer->info;
    report("the server itself need not be named", pmix_server_grp_is_participant(me, procs, 2));
    PMIX_RELEASE(me);
}

/* ------------------------------------------------------------------ */
/* the collectives                                                     */
/* ------------------------------------------------------------------ */

static bool host_called = false;
static size_t host_nprocs = 0;
static pmix_proc_t host_proc0;
static bool host_notterm = false;

static pmix_status_t stub_group(pmix_group_operation_t op, char grp[], const pmix_proc_t procs[],
                                size_t nprocs, const pmix_info_t directives[], size_t ndirs,
                                pmix_info_cbfunc_t cbfunc, void *cbdata)
{
    size_t n;

    (void) op;
    (void) grp;
    (void) cbfunc;
    (void) cbdata;
    host_called = true;
    host_nprocs = nprocs;
    if (0 < nprocs) {
        memcpy(&host_proc0, &procs[0], sizeof(pmix_proc_t));
    }
    host_notterm = false;
    for (n = 0; n < ndirs; n++) {
        if (PMIX_CHECK_KEY(&directives[n], PMIX_GROUP_NOTIFY_TERMINATION)) {
            host_notterm = true;
        }
    }
    return PMIX_ERR_NOT_SUPPORTED;
}

static pmix_server_caddy_t *make_caddy(pmix_peer_t *peer)
{
    pmix_server_caddy_t *cd;

    cd = PMIX_NEW(pmix_server_caddy_t);
    PMIX_RETAIN(peer);
    cd->peer = peer;
    cd->hdr.tag = 0;
    return cd;
}

/* drop whatever a handler queued to a peer */
static void drain(pmix_peer_t *peer)
{
    pmix_ptl_send_t *snd;

    if (NULL != peer->send_msg) {
        PMIX_RELEASE(peer->send_msg);
        peer->send_msg = NULL;
    }
    while (NULL != (snd = (pmix_ptl_send_t *) pmix_list_remove_first(&peer->send_queue))) {
        PMIX_RELEASE(snd);
    }
}

/* a group construct or destruct naming procs[0..nprocs) */
static pmix_status_t do_group(pmix_peer_t *peer, pmix_group_operation_t op, const char *grpid,
                              const pmix_proc_t *procs, size_t nprocs)
{
    pmix_server_caddy_t *cd;
    pmix_buffer_t *buf;
    pmix_status_t rc;
    size_t zero = 0;
    char *id = (char *) grpid;

    buf = PMIX_NEW(pmix_buffer_t);
    PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, buf, &id, 1, PMIX_STRING);
    if (PMIX_SUCCESS == rc) {
        PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, buf, &nprocs, 1, PMIX_SIZE);
    }
    if (PMIX_SUCCESS == rc && 0 < nprocs) {
        PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, buf, procs, nprocs, PMIX_PROC);
    }
    if (PMIX_SUCCESS == rc) {
        PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, buf, &zero, 1, PMIX_SIZE);
    }
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(buf);
        return rc;
    }
    host_called = false;
    cd = make_caddy(peer);
    rc = pmix_server_group(cd, buf, op);
    PMIX_RELEASE(buf);
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(cd);
    }
    progress_barrier();
    drain(peer);
    return rc;
}

static pmix_status_t do_fence(pmix_peer_t *peer, const pmix_proc_t *procs, size_t nprocs)
{
    pmix_server_caddy_t *cd;
    pmix_buffer_t *buf;
    pmix_status_t rc;
    size_t zero = 0;

    buf = PMIX_NEW(pmix_buffer_t);
    PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, buf, &nprocs, 1, PMIX_SIZE);
    if (PMIX_SUCCESS == rc) {
        PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, buf, procs, nprocs, PMIX_PROC);
    }
    if (PMIX_SUCCESS == rc) {
        PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, buf, &zero, 1, PMIX_SIZE);
    }
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(buf);
        return rc;
    }
    cd = make_caddy(peer);
    rc = pmix_server_fence(cd, buf, pmix_server_modex_cbfunc);
    PMIX_RELEASE(buf);
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(cd);
    }
    return rc;
}

static void op_cbfunc(pmix_status_t status, void *cbdata)
{
    (void) status;
    (void) cbdata;
}

/* a connect or disconnect naming procs[0..nprocs) */
static pmix_status_t do_connect(pmix_peer_t *peer, bool connect, const pmix_proc_t *procs,
                                size_t nprocs)
{
    pmix_server_caddy_t *cd;
    pmix_buffer_t *buf;
    pmix_status_t rc;
    size_t zero = 0;

    buf = PMIX_NEW(pmix_buffer_t);
    PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, buf, &nprocs, 1, PMIX_SIZE);
    if (PMIX_SUCCESS == rc) {
        PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, buf, procs, nprocs, PMIX_PROC);
    }
    if (PMIX_SUCCESS == rc) {
        PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, buf, &zero, 1, PMIX_SIZE);
    }
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(buf);
        return rc;
    }
    cd = make_caddy(peer);
    if (connect) {
        rc = pmix_server_connect(cd, buf, op_cbfunc);
    } else {
        rc = pmix_server_disconnect(cd, buf, op_cbfunc);
    }
    PMIX_RELEASE(buf);
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(cd);
    }
    return rc;
}

static pmix_status_t do_get(pmix_peer_t *peer, const char *nspace, pmix_rank_t rank)
{
    pmix_server_caddy_t *cd;
    pmix_buffer_t *buf;
    pmix_info_t info;
    pmix_status_t rc;
    size_t one = 1;
    char *cptr = (char *) nspace;

    buf = PMIX_NEW(pmix_buffer_t);
    PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, buf, &cptr, 1, PMIX_STRING);
    if (PMIX_SUCCESS == rc) {
        PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, buf, &rank, 1, PMIX_PROC_RANK);
    }
    if (PMIX_SUCCESS == rc) {
        PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, buf, &one, 1, PMIX_SIZE);
    }
    if (PMIX_SUCCESS == rc) {
        PMIX_INFO_LOAD(&info, PMIX_IMMEDIATE, NULL, PMIX_BOOL);
        PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, buf, &info, 1, PMIX_INFO);
        PMIX_INFO_DESTRUCT(&info);
    }
    if (PMIX_SUCCESS == rc) {
        cptr = (char *) "grpmbr.key";
        PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, buf, &cptr, 1, PMIX_STRING);
    }
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(buf);
        return rc;
    }
    cd = make_caddy(peer);
    rc = pmix_server_get(buf, pmix_server_get_cbfunc, cd);
    PMIX_RELEASE(buf);
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(cd);
    }
    progress_barrier();
    drain(peer);
    return rc;
}

static void test_collectives(pmix_peer_t *a1)
{
    pmix_proc_t mbrs[2], proc;
    pmix_status_t rc;

    /* a group of A:1 alone, constructed asking for termination notice */
    PMIX_LOAD_PROCID(&mbrs[0], NS_A, 1);
    pmix_server_grp_record("coll", mbrs, 1, SIZE_MAX, true, false);

    rc = do_group(a1, PMIX_GROUP_DESTRUCT, "coll", NULL, 0);
    report("a destruct naming no procs is given the group's members",
           PMIX_SUCCESS == rc && host_called && 1 == host_nprocs &&
               same(&host_proc0, NS_A, 1));
    report("and the failure policy it was constructed with", host_notterm);

    rc = do_group(a1, PMIX_GROUP_DESTRUCT, "coll-unknown", NULL, 0);
    report("a destruct of a group we do not hold is not found", PMIX_ERR_NOT_FOUND == rc);

    /* the destruct above ended the group, whatever the host answered, as
     * it does for each member */
    report("a destruct drops the group", NULL == pmix_server_grp_find("coll"));

    /* a construct naming, by its ID, a group the caller is in */
    PMIX_LOAD_PROCID(&mbrs[0], NS_A, 1);
    pmix_server_grp_record("coll", mbrs, 1, SIZE_MAX, false, false);
    PMIX_LOAD_PROCID(&proc, "coll", PMIX_RANK_WILDCARD);
    rc = do_group(a1, PMIX_GROUP_CONSTRUCT, "coll-new", &proc, 1);
    report("a construct naming a group is given its members",
           PMIX_SUCCESS == rc && host_called && 1 == host_nprocs &&
               same(&host_proc0, NS_A, 1));

    /* a group without A:1 */
    PMIX_LOAD_PROCID(&mbrs[0], NS_A, 2);
    PMIX_LOAD_PROCID(&mbrs[1], NS_A, 3);
    pmix_server_grp_record("coll-other", mbrs, 2, SIZE_MAX, false, false);
    PMIX_LOAD_PROCID(&proc, "coll-other", PMIX_RANK_WILDCARD);
    rc = do_group(a1, PMIX_GROUP_CONSTRUCT, "coll-new2", &proc, 1);
    report("a construct naming a group the caller is not in is refused",
           PMIX_ERR_NOT_A_MEMBER == rc && !host_called);
    rc = do_fence(a1, &proc, 1);
    report("so is a fence", PMIX_ERR_NOT_A_MEMBER == rc);
    rc = do_connect(a1, true, &proc, 1);
    report("so is a connect", PMIX_ERR_NOT_A_MEMBER == rc);
    rc = do_connect(a1, false, &proc, 1);
    report("so is a disconnect", PMIX_ERR_NOT_A_MEMBER == rc);

    rc = do_get(a1, "coll-other", PMIX_RANK_WILDCARD);
    report("a get of a whole group is refused", PMIX_ERR_BAD_PARAM == rc);
    rc = do_get(a1, "coll-other", 2);
    report("a get past the group is not found", PMIX_ERR_NOT_FOUND == rc);
    pmix_server_grp_drop("coll-other");
    pmix_server_grp_drop("coll");
}

int main(int argc, char **argv)
{
    static pmix_server_module_t mymodule = {0};
    pmix_peer_t *a0, *a1;
    pmix_status_t rc;

    (void) argc;
    (void) argv;

    fprintf(stdout, "server_group_members: the group membership a server holds\n");

    mymodule.group = stub_group;
    rc = PMIx_server_init(&mymodule, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "PMIx_server_init failed: %s\n", PMIx_Error_string(rc));
        return 1;
    }
    if (PMIX_SUCCESS != reg(NS_A) || PMIX_SUCCESS != reg(NS_B)) {
        fprintf(stderr, "could not register the jobs\n");
        PMIx_server_finalize();
        return 1;
    }
    a0 = make_client(NS_A, 0);
    a1 = make_client(NS_A, 1);
    if (NULL == a0 || NULL == a1) {
        fprintf(stderr, "could not create the clients\n");
        PMIx_server_finalize();
        return 1;
    }

    test_record();
    test_expand();
    test_leave_sweep(a0, a1);
    test_host_events();
    test_participants(a1);
    test_collectives(a1);

    drop_client(a0);
    drop_client(a1);
    PMIx_server_finalize();

    fprintf(stdout, "server_group_members: %d passed, %d failed\n", npass, nfail);
    return (0 == nfail) ? 0 : 1;
}
