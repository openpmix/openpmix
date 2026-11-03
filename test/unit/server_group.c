/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * White-box unit tests for the server side of the group collective -
 * pmix_server_group() in src/server/pmix_server_group.c - driven from
 * hand-packed wire buffers, the way the switchyard drives it.
 *
 * The process comes up as a PMIx server whose stub host module supplies a
 * "group" entry point that declines every request. Declining is
 * deliberate: the handler's refusal arm hands the whole block to
 * grpcbfunc, which replies to every participant and tears the block down,
 * so each case leaves pmix_server_globals.grp_collectives empty and the
 * next one starts from a known state. Accepting would leave the
 * completion to this program, and a host stub cannot drive one without a
 * peer that has a socket.
 *
 * Test cases:
 *
 *   empty group id           -> PMIX_ERR_BAD_PARAM (kills an unfixed
 *                               library rather than failing it - see the
 *                               case)
 *   proc count that wraps    -> PMIX_ERR_BAD_PARAM
 *   info count that wraps    -> PMIX_ERR_BAD_PARAM
 *   info count that
 *      truncates negative    -> PMIX_ERR_BAD_PARAM
 *   well-formed construct    -> reaches the host with the local-collective
 *                               status seeded in the last info slot, and
 *                               leaves nothing on grp_collectives
 *   participant namespace
 *      registered late       -> parks, and is forwarded to the host by the
 *                               registration rather than waiting forever
 *   two participants, one
 *      namespace late        -> the expected-local count is the membership,
 *                               not the membership times the number of
 *                               participants that named it
 *   two bootstrap blocks,
 *      host accepts both     -> each block survives until its own completion
 *                               arrives (kills an unfixed library)
 *   a held group's ID        -> the record keeps one group per ID: the same
 *                               membership again changes nothing, another
 *                               is PMIX_ERR_EXISTS; a construct of a held
 *                               ID is refused before the host sees it
 *   a held group's destruct  -> runs over the recorded members whatever
 *                               procs were named, and only for a member
 *   an ID that names a job   -> a construct is refused before the host
 *                               sees it when the server knows the job,
 *                               and goes to the host when it does not
 *   a construct the host
 *      completes over an ID
 *      recorded meanwhile    -> the record is kept, not replaced
 *   a member lost and back   -> counted once by the block, whether it was
 *                               gone when the block was counted, lost
 *                               while the block waited, or had already
 *                               called through the connection it lost
 *
 * Every case asserts that grp_collectives is empty afterwards, because a
 * block left parked there with participants on it hangs those clients for
 * the life of the server.
 *
 * What this file cannot reach: anything past the host up-call. A group
 * spanning servers, the departed-member accounting and the two-level
 * block/tracker engine under a real host are covered by
 * contrib/dockerswarm/run-tests.sh and run-server-tests.sh, and by the
 * run_grp*.pl programs under test/simple.
 */

#include "src/include/pmix_config.h"

#include "include/pmix.h"
#include "include/pmix_server.h"

#include "src/include/pmix_globals.h"
#include "src/mca/bfrops/bfrops.h"
#include "src/mca/ptl/base/base.h"
#include "src/server/pmix_server_ops.h"
#include "src/threads/pmix_threads.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define GRPUT_NSPACE "server-group-ut"
#define GRPUT_LATE   "server-group-ut-late"
#define GRPUT_TWO    "server-group-ut-two"
#define GRPUT_LATE2  "server-group-ut-late2"

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

/* what the host's group entry point was handed */
static bool group_fired = false;
static size_t group_ninfo = 0;
static bool group_status_last = false;
/* whether the host was handed a PMIX_USERID or PMIX_GRPID, and whether
 * drive_group puts both after its first directive */
static bool group_has_id = false;
static bool next_ids = false;
/* the PMIX_GROUP_INFO the host was given: its leading proc and size */
static bool group_contrib_seen = false;
static pmix_group_operation_t group_op = PMIX_GROUP_NONE;
static size_t group_nprocs = 0;
static pmix_proc_t group_contrib;
static size_t group_contrib_size = 0;
static char *group_id = NULL;

/* When set, the stub accepts the operation and parks the completion so the
 * test can drive it. A declining stub tears the block down on the spot and
 * so cannot reach any state in which the host owns a block. */
#define GRPUT_MAXHELD 4
static bool group_accept = false;
static pmix_info_cbfunc_t held_cbfunc[GRPUT_MAXHELD];
static void *held_cbdata[GRPUT_MAXHELD];
static size_t nheld = 0;

static pmix_status_t stub_group(pmix_group_operation_t op, char grp[],
                                const pmix_proc_t procs[], size_t nprocs,
                                const pmix_info_t directives[], size_t ndirs,
                                pmix_info_cbfunc_t cbfunc, void *cbdata)
{
    (void) procs;

    group_fired = true;
    group_op = op;
    group_nprocs = nprocs;
    group_ninfo = ndirs;
    group_status_last = false;
    group_contrib_seen = false;
    group_has_id = false;
    for (size_t d = 0; NULL != directives && d < ndirs; d++) {
        const pmix_info_t *gi;

        if (PMIX_CHECK_KEY(&directives[d], PMIX_USERID) ||
            PMIX_CHECK_KEY(&directives[d], PMIX_GRPID)) {
            group_has_id = true;
        }

        if (!PMIX_CHECK_KEY(&directives[d], PMIX_GROUP_INFO) ||
            PMIX_DATA_ARRAY != directives[d].value.type ||
            NULL == directives[d].value.data.darray) {
            continue;
        }
        gi = (const pmix_info_t *) directives[d].value.data.darray->array;
        group_contrib_size = directives[d].value.data.darray->size;
        if (0 < group_contrib_size && PMIX_CHECK_KEY(&gi[0], PMIX_PROCID) &&
            PMIX_PROC == gi[0].value.type) {
            memcpy(&group_contrib, gi[0].value.data.proc, sizeof(pmix_proc_t));
            group_contrib_seen = true;
        }
    }
    if (NULL != directives && 0 < ndirs) {
        group_status_last = PMIX_CHECK_KEY(&directives[ndirs - 1],
                                           PMIX_LOCAL_COLLECTIVE_STATUS);
    }
    if (NULL != group_id) {
        free(group_id);
        group_id = NULL;
    }
    if (NULL != grp) {
        group_id = strdup(grp);
    }
    if (group_accept) {
        /* accept and hold the completion, so the block stays on
         * grp_collectives with the host owning it */
        if (nheld < GRPUT_MAXHELD) {
            held_cbfunc[nheld] = cbfunc;
            held_cbdata[nheld] = cbdata;
            ++nheld;
        }
        return PMIX_SUCCESS;
    }
    (void) cbfunc;
    (void) cbdata;
    /* decline, so the handler takes its refusal arm: it hands the block to
     * grpcbfunc, which answers every participant and tears the block down.
     * Accepting would leave the completion to us. */
    return PMIX_ERR_NOT_SUPPORTED;
}

/* Carrier for driving pmix_server_group on the progress thread - it
 * touches pmix_server_globals.grp_collectives, so it must not be called
 * from main(). The pmix_event_t is the thread-shift member required of
 * every caddy. */
typedef struct {
    pmix_event_t ev;
    pmix_lock_t lock;
    const char *grpid;  /* NULL packs the empty string the unpacker
                         * turns back into a NULL pointer */
    const char *pns;    /* namespace the participants are named in */
    size_t nprocs;      /* the count to put on the wire */
    size_t nprocs_real; /* how many procs actually to pack */
    size_t ninf;        /* the count to put on the wire */
    size_t ninf_real;   /* how many info structs actually to pack */
    /* when set, pack these procs verbatim instead of synthesizing
     * pns:0..nprocs_real-1 - the multi-namespace and wildcard cases need
     * a membership the rank-counting form cannot express */
    const pmix_proc_t *plist;
    bool bootstrap;     /* pack PMIX_GROUP_BOOTSTRAP rather than FT_COLLECTIVE */
    /* when set, the first info is a PMIX_GROUP_INFO contribution led by
     * this proc, as a client's own contribution is led by itself */
    const pmix_proc_t *contributor;
    /* the same, but with no leading proc at all - as a client before
     * v6.0 sends the caller's array */
    bool bare_contribution;
    /* the second and third infos are a PMIX_USERID and a PMIX_GRPID */
    bool ids;
    /* the operation - a construct unless set - and the peer asking, the
     * server's own unless set */
    pmix_group_operation_t op;
    pmix_peer_t *peer;
    pmix_status_t status;
} grp_req_t;

static void do_group(int sd, short args, void *cbdata)
{
    grp_req_t *r = (grp_req_t *) cbdata;
    pmix_buffer_t buf;
    pmix_server_caddy_t *cd;
    pmix_proc_t *procs = NULL;
    pmix_info_t *dirs = NULL;
    pmix_status_t rc;
    size_t n;
    size_t nboot = 2;
    char *cptr;
    bool flag = true;

    (void) sd;
    (void) args;

    PMIX_CONSTRUCT(&buf, pmix_buffer_t);
    PMIX_BFROPS_ASSIGN_TYPE(pmix_globals.mypeer, &buf);

    /* the group id */
    cptr = (char *) r->grpid;
    PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, &buf, &cptr, 1, PMIX_STRING);

    /* The proc count, then the procs. The two may disagree on purpose:
     * that is the whole point of the malformed-count cases. Each array
     * goes out in ONE pack call, because that is what every PMIx client
     * does and it is what the handler's short-array screen is held
     * against - packing element by element writes N separate one-element
     * arrays, which the handler correctly rejects, so a harness that did
     * that would be testing itself. */
    if (PMIX_SUCCESS == rc) {
        PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, &buf, &r->nprocs, 1, PMIX_SIZE);
    }
    if (PMIX_SUCCESS == rc && 0 < r->nprocs_real) {
        PMIX_PROC_CREATE(procs, r->nprocs_real);
        if (NULL == procs) {
            PMIX_DESTRUCT(&buf);
            r->status = PMIX_ERR_NOMEM;
            PMIX_WAKEUP_THREAD(&r->lock);
            return;
        }
        for (n = 0; n < r->nprocs_real; n++) {
            if (NULL != r->plist) {
                memcpy(&procs[n], &r->plist[n], sizeof(pmix_proc_t));
            } else {
                PMIX_LOAD_PROCID(&procs[n], r->pns, (pmix_rank_t) n);
            }
        }
        PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, &buf, procs,
                         (int32_t) r->nprocs_real, PMIX_PROC);
        PMIX_PROC_FREE(procs, r->nprocs_real);
    }
    /* the info count, then the info structs */
    if (PMIX_SUCCESS == rc) {
        PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, &buf, &r->ninf, 1, PMIX_SIZE);
    }
    if (PMIX_SUCCESS == rc && 0 < r->ninf_real) {
        PMIX_INFO_CREATE(dirs, r->ninf_real);
        if (NULL == dirs) {
            PMIX_DESTRUCT(&buf);
            r->status = PMIX_ERR_NOMEM;
            PMIX_WAKEUP_THREAD(&r->lock);
            return;
        }
        for (n = 0; n < r->ninf_real; n++) {
            if (0 == n && r->bare_contribution) {
                pmix_data_array_t *gi;
                uint32_t gv = 7;

                PMIX_DATA_ARRAY_CREATE(gi, 1, PMIX_INFO);
                PMIX_INFO_LOAD((pmix_info_t *) gi->array, "grput.value", &gv, PMIX_UINT32);
                PMIX_INFO_LOAD(&dirs[n], PMIX_GROUP_INFO, gi, PMIX_DATA_ARRAY);
                PMIX_DATA_ARRAY_FREE(gi);
            } else if (0 == n && NULL != r->contributor) {
                pmix_data_array_t *gi;
                pmix_info_t *gpi;
                uint32_t gv = 7;

                PMIX_DATA_ARRAY_CREATE(gi, 2, PMIX_INFO);
                gpi = (pmix_info_t *) gi->array;
                PMIX_INFO_LOAD(&gpi[0], PMIX_PROCID, r->contributor, PMIX_PROC);
                PMIX_INFO_LOAD(&gpi[1], "grput.value", &gv, PMIX_UINT32);
                PMIX_INFO_LOAD(&dirs[n], PMIX_GROUP_INFO, gi, PMIX_DATA_ARRAY);
                PMIX_DATA_ARRAY_FREE(gi);
            } else if (r->ids && (1 == n || 2 == n)) {
                uint32_t id = 4242;

                PMIX_INFO_LOAD(&dirs[n], (1 == n) ? PMIX_USERID : PMIX_GRPID, &id,
                               PMIX_UINT32);
            } else if (r->bootstrap) {
                PMIX_INFO_LOAD(&dirs[n], PMIX_GROUP_BOOTSTRAP, &nboot, PMIX_SIZE);
            } else {
                PMIX_INFO_LOAD(&dirs[n], PMIX_GROUP_FT_COLLECTIVE, &flag, PMIX_BOOL);
            }
        }
        PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, &buf, dirs,
                         (int32_t) r->ninf_real, PMIX_INFO);
        PMIX_INFO_FREE(dirs, r->ninf_real);
    }
    if (PMIX_SUCCESS != rc) {
        PMIX_DESTRUCT(&buf);
        r->status = rc;
        PMIX_WAKEUP_THREAD(&r->lock);
        return;
    }

    cd = PMIX_NEW(pmix_server_caddy_t);
    if (NULL == cd) {
        PMIX_DESTRUCT(&buf);
        r->status = PMIX_ERR_NOMEM;
        PMIX_WAKEUP_THREAD(&r->lock);
        return;
    }
    cd->peer = (NULL != r->peer) ? r->peer : pmix_globals.mypeer;
    PMIX_RETAIN(cd->peer);
    cd->hdr.tag = 0;

    rc = pmix_server_group(cd, &buf, r->op);
    if (PMIX_SUCCESS != rc) {
        /* the switchyard owns the caddy on a non-success return */
        PMIX_RELEASE(cd);
    }
    PMIX_DESTRUCT(&buf);
    r->status = rc;
    PMIX_WAKEUP_THREAD(&r->lock);
}

static pmix_status_t drive_group(const char *grpid, const char *pns,
                                 size_t nprocs, size_t nprocs_real,
                                 size_t ninf, size_t ninf_real)
{
    grp_req_t req;
    pmix_status_t rc;

    memset(&req, 0, sizeof(req));
    PMIX_CONSTRUCT_LOCK(&req.lock);
    req.grpid = grpid;
    req.pns = pns;
    req.nprocs = nprocs;
    req.nprocs_real = nprocs_real;
    req.ninf = ninf;
    req.ninf_real = ninf_real;
    req.ids = next_ids;
    group_fired = false;
    PMIX_THREADSHIFT(&req, do_group);
    PMIX_WAIT_THREAD(&req.lock);
    rc = req.status;
    PMIX_DESTRUCT_LOCK(&req.lock);
    return rc;
}

/* Drive a construct whose group info names `contributor` as its sender.
 * The caddy's peer is this server's own, so only its own name is the
 * sender's. */
static pmix_status_t drive_group_contrib(const char *grpid, const pmix_proc_t *contributor,
                                         bool bare)
{
    grp_req_t req;
    pmix_status_t rc;

    memset(&req, 0, sizeof(req));
    PMIX_CONSTRUCT_LOCK(&req.lock);
    req.grpid = grpid;
    req.pns = GRPUT_NSPACE;
    req.nprocs = 1;
    req.nprocs_real = 1;
    req.ninf = 1;
    req.ninf_real = 1;
    req.contributor = contributor;
    req.bare_contribution = bare;
    group_fired = false;
    PMIX_THREADSHIFT(&req, do_group);
    PMIX_WAIT_THREAD(&req.lock);
    rc = req.status;
    PMIX_DESTRUCT_LOCK(&req.lock);
    return rc;
}

/* Drive one participant whose membership is given explicitly. */
static pmix_status_t drive_group_procs(const char *grpid,
                                       const pmix_proc_t *procs, size_t nprocs,
                                       bool bootstrap)
{
    grp_req_t req;
    pmix_status_t rc;

    memset(&req, 0, sizeof(req));
    PMIX_CONSTRUCT_LOCK(&req.lock);
    req.grpid = grpid;
    req.plist = procs;
    req.nprocs = nprocs;
    req.nprocs_real = nprocs;
    req.ninf = 1;
    req.ninf_real = 1;
    req.bootstrap = bootstrap;
    group_fired = false;
    PMIX_THREADSHIFT(&req, do_group);
    PMIX_WAIT_THREAD(&req.lock);
    rc = req.status;
    PMIX_DESTRUCT_LOCK(&req.lock);
    return rc;
}

/* Drive a construct or destruct naming these procs (none: NULL, 0), asked
 * by `peer` (NULL: the server's own) */
static pmix_status_t drive_op(pmix_group_operation_t op, const char *grpid,
                              const pmix_proc_t *procs, size_t nprocs, pmix_peer_t *peer)
{
    grp_req_t req;
    pmix_status_t rc;

    memset(&req, 0, sizeof(req));
    PMIX_CONSTRUCT_LOCK(&req.lock);
    req.op = op;
    req.grpid = grpid;
    req.plist = procs;
    req.nprocs = nprocs;
    req.nprocs_real = nprocs;
    req.peer = peer;
    group_fired = false;
    group_op = PMIX_GROUP_NONE;
    group_nprocs = 0;
    PMIX_THREADSHIFT(&req, do_group);
    PMIX_WAIT_THREAD(&req.lock);
    rc = req.status;
    PMIX_DESTRUCT_LOCK(&req.lock);
    return rc;
}

/* A peer for another process: a client of this server's namespace object,
 * so what it sends unpacks as the server's own does, but not the server -
 * which counts as a participant in anything it asks */
static pmix_peer_t *other_peer(const char *ns, pmix_rank_t rank)
{
    pmix_peer_t *peer = PMIX_NEW(pmix_peer_t);

    peer->info = PMIX_NEW(pmix_rank_info_t);
    peer->info->pname.nspace = strdup(ns);
    peer->info->pname.rank = rank;
    peer->nptr = pmix_globals.mypeer->nptr;
    PMIX_RETAIN(peer->nptr);
    peer->proc_type = pmix_globals.mypeer->proc_type;
    return peer;
}

/* A namespace known only as a dmodex reply leaves it: no local procs
 * registered, no clients - on the progress thread, where the list lives */
typedef struct {
    pmix_event_t ev;
    pmix_lock_t lock;
    const char *ns;
} bare_ns_t;

static void do_bare_ns(int sd, short args, void *cbdata)
{
    bare_ns_t *b = (bare_ns_t *) cbdata;
    pmix_namespace_t *nptr = PMIX_NEW(pmix_namespace_t);

    (void) sd;
    (void) args;
    nptr->nspace = strdup(b->ns);
    pmix_list_append(&pmix_globals.nspaces, &nptr->super);
    PMIX_WAKEUP_THREAD(&b->lock);
}

static void add_bare_nspace(const char *ns)
{
    bare_ns_t b;

    PMIX_CONSTRUCT_LOCK(&b.lock);
    b.ns = ns;
    PMIX_THREADSHIFT(&b, do_bare_ns);
    PMIX_WAIT_THREAD(&b.lock);
    PMIX_DESTRUCT_LOCK(&b.lock);
}

/* The group records live on the progress thread */
typedef struct {
    pmix_event_t ev;
    pmix_lock_t lock;
    const char *grpid;
    const pmix_proc_t *members;
    size_t nmembers;
    bool lookup;                /* only look the group up */
    pmix_status_t status;
    size_t held;                /* members recorded, after */
    bool has_first;             /* ...and whether the first given is one */
} rec_req_t;

static void do_record(int sd, short args, void *cbdata)
{
    rec_req_t *r = (rec_req_t *) cbdata;
    pmix_group_t *grp;
    size_t n;

    (void) sd;
    (void) args;
    if (!r->lookup && NULL != r->members) {
        r->status = pmix_server_grp_record(r->grpid, r->members, r->nmembers, SIZE_MAX,
                                           false, false);
    }
    grp = pmix_server_grp_find(r->grpid);
    r->held = (NULL == grp) ? 0 : grp->nmbrs;
    r->has_first = false;
    for (n = 0; NULL != grp && NULL != r->members && n < grp->nmbrs; n++) {
        if (0 == strcmp(grp->members[n].nspace, r->members[0].nspace) &&
            grp->members[n].rank == r->members[0].rank) {
            r->has_first = true;
        }
    }
    PMIX_WAKEUP_THREAD(&r->lock);
}

static pmix_status_t record_or_look(const char *grpid, const pmix_proc_t *members, size_t n,
                                    bool lookup, size_t *held, bool *has_first)
{
    rec_req_t req;

    memset(&req, 0, sizeof(req));
    PMIX_CONSTRUCT_LOCK(&req.lock);
    req.grpid = grpid;
    req.members = members;
    req.nmembers = n;
    req.lookup = lookup;
    PMIX_THREADSHIFT(&req, do_record);
    PMIX_WAIT_THREAD(&req.lock);
    PMIX_DESTRUCT_LOCK(&req.lock);
    if (NULL != held) {
        *held = req.held;
    }
    if (NULL != has_first) {
        *has_first = req.has_first;
    }
    return req.status;
}

/* record a group of these members */
static pmix_status_t record(const char *grpid, const pmix_proc_t *members, size_t n,
                            size_t *held, bool *has_first)
{
    return record_or_look(grpid, members, n, false, held, has_first);
}

/* how many members the group held under this ID has, and whether `first` is
 * one of them - recording nothing */
static size_t held_with(const char *grpid, const pmix_proc_t *first, bool *has_first)
{
    size_t held = 0;

    (void) record_or_look(grpid, first, (NULL == first) ? 0 : 1, true, &held, has_first);
    return held;
}

static size_t held_members(const char *grpid)
{
    return held_with(grpid, NULL, NULL);
}

/* Register a namespace that places no procs on this node. That is enough
 * for check_definition_complete: it needs the namespace to be known and
 * its local count to be settled, and a count of zero settles it with
 * nothing to wait for. */
static pmix_status_t register_nspace_n(const char *ns, int nlocal)
{
    pmix_nspace_t nspace;
    pmix_status_t rc;

    PMIX_LOAD_NSPACE(nspace, ns);
    rc = PMIx_server_register_nspace(nspace, nlocal, NULL, 0, NULL, NULL);
    if (PMIX_OPERATION_SUCCEEDED == rc) {
        rc = PMIX_SUCCESS;
    }
    return rc;
}

static pmix_status_t register_nspace(const char *ns)
{
    return register_nspace_n(ns, 0);
}

/* grpcbfunc thread-shifts, so let anything queued behind us run before we
 * look at grp_collectives. PMIx_Store_internal blocks on the progress
 * thread, so anything queued ahead of it has run by the time it returns. */
static void progress_barrier(void)
{
    pmix_value_t v;

    PMIX_VALUE_LOAD(&v, "barrier", PMIX_STRING);
    PMIx_Store_internal(&pmix_globals.myid, "server-group-ut.barrier", &v);
    PMIX_VALUE_DESTRUCT(&v);
}

static bool nothing_parked(void)
{
    progress_barrier();
    return (0 == pmix_list_get_size(&pmix_server_globals.grp_collectives));
}

/* --- a member that is lost and comes back ---------------------------
 *
 * A member of a job registered with three local clients, on that job's own
 * rank entry - so that losing it (its socket closes and the server reads
 * the close) and bringing it back (pmix_server_rank_returned, as the
 * connection handler does when the host's restarted process connects)
 * act on the state the block is counted from. */
#define GRPRET_NSPACE "grpret.ns"

typedef struct {
    pmix_event_t ev;
    pmix_lock_t lock;
    pmix_rank_t rank;
    int sd;
    bool returning;
    bool reap;
    pmix_peer_t *peer;
    bool result;
} member_t;

static pmix_namespace_t *find_ret_ns(void)
{
    pmix_namespace_t *ns;

    PMIX_LIST_FOREACH (ns, &pmix_globals.nspaces, pmix_namespace_t) {
        if (0 == strcmp(ns->nspace, GRPRET_NSPACE)) {
            return ns;
        }
    }
    return NULL;
}

static void do_member(int sd, short args, void *cbdata)
{
    member_t *m = (member_t *) cbdata;
    pmix_namespace_t *nptr = find_ret_ns();
    pmix_rank_info_t *info, *found = NULL;
    pmix_peer_t *p;

    (void) sd;
    (void) args;
    m->result = false;
    if (m->reap) {
        m->result = pmix_ptl_base_reap_if_closed(m->peer);
        PMIX_WAKEUP_THREAD(&m->lock);
        return;
    }
    m->peer = NULL;
    if (NULL != nptr) {
        PMIX_LIST_FOREACH (info, &nptr->ranks, pmix_rank_info_t) {
            if (info->pname.rank == m->rank) {
                found = info;
                break;
            }
        }
    }
    if (NULL == found) {
        PMIX_WAKEUP_THREAD(&m->lock);
        return;
    }
    if (NULL == nptr->compat.bfrops) {
        memcpy(&nptr->compat, &pmix_globals.mypeer->nptr->compat, sizeof(pmix_personality_t));
    }
    p = PMIX_NEW(pmix_peer_t);
    PMIX_RETAIN(nptr);
    p->nptr = nptr;
    PMIX_RETAIN(found);
    p->info = found;
    memcpy(&p->proc_type, &pmix_globals.mypeer->proc_type, sizeof(pmix_proc_type_t));
    p->sd = m->sd;
    p->index = pmix_pointer_array_add(&pmix_server_globals.clients, p);
    found->peerid = p->index;
    found->proc_cnt = 1;
    if (m->returning) {
        pmix_server_rank_returned(nptr, found);
    }
    m->peer = p;
    PMIX_WAKEUP_THREAD(&m->lock);
}

static void run_member(member_t *m)
{
    PMIX_CONSTRUCT_LOCK(&m->lock);
    PMIX_THREADSHIFT(m, do_member);
    PMIX_WAIT_THREAD(&m->lock);
    PMIX_DESTRUCT_LOCK(&m->lock);
}

/* a connected member - with a socket whose other end is in *other, so it
 * can be lost, when other is non-NULL - or one that is coming back */
static pmix_peer_t *member(pmix_rank_t rank, int *other, bool returning)
{
    member_t m;
    int sv[2] = {-1, -1};

    memset(&m, 0, sizeof(m));
    if (NULL != other) {
        if (0 != socketpair(AF_UNIX, SOCK_STREAM, 0, sv)) {
            return NULL;
        }
        *other = sv[1];
    }
    m.rank = rank;
    m.sd = sv[0];
    m.returning = returning;
    run_member(&m);
    return m.peer;
}

/* close the far end of a member's socket and have the server notice */
static bool lose(pmix_peer_t *peer, int other)
{
    member_t m;

    memset(&m, 0, sizeof(m));
    close(other);
    m.reap = true;
    m.peer = peer;
    run_member(&m);
    return m.result;
}

/* a construct over the job's three ranks, asked by peer */
static pmix_status_t construct_by(const char *grpid, pmix_peer_t *peer)
{
    pmix_proc_t mbrs[3];
    pmix_rank_t r;

    for (r = 0; r < 3; r++) {
        PMIX_LOAD_PROCID(&mbrs[r], GRPRET_NSPACE, r);
    }
    return drive_op(PMIX_GROUP_CONSTRUCT, grpid, mbrs, 3, peer);
}

/* A member lost and then back - the host restarted its process - must be
 * counted once by a block, whichever way it stands with that block. In
 * each case the returned member calls BEFORE some member still alive, which
 * is when an unfixed library hands the block to the host short of one call:
 * its call stood in for the live member's, and the host was told the group
 * was assembled while one of its members had not yet asked to join. */
static void test_returning_member(void)
{
    pmix_peer_t *r0, *r1, *r2;
    pmix_proc_t p;
    pmix_rank_t r;
    pmix_status_t rc;
    int far;
    size_t baseline;

    if (PMIX_SUCCESS != register_nspace_n(GRPRET_NSPACE, 3)) {
        report("returning member: the job registers", 0);
        return;
    }
    for (r = 0; r < 3; r++) {
        PMIX_LOAD_PROCID(&p, GRPRET_NSPACE, r);
        (void) PMIx_server_register_client(&p, geteuid(), getegid(), NULL, NULL, NULL);
    }
    progress_barrier();
    baseline = pmix_list_get_size(&pmix_server_globals.grp_collectives);
    r0 = member(0, NULL, false);
    r1 = member(1, NULL, false);

    /* left out: rank 2 is gone when the block is counted, and back by the
     * time it calls */
    r2 = member(2, &far, false);
    report("returning member: rank 2 is lost", NULL != r2 && lose(r2, far));
    rc = construct_by("grpret.excluded", r0);
    report("left out: the first call parks", PMIX_SUCCESS == rc && !group_fired);
    r2 = member(2, NULL, true);
    rc = construct_by("grpret.excluded", r2);
    report("left out: the returned member's call does not stand in for another's",
           PMIX_SUCCESS == rc && !group_fired);
    rc = construct_by("grpret.excluded", r1);
    report("left out: the block goes to the host once every member has called",
           PMIX_SUCCESS == rc && group_fired);
    progress_barrier();
    report("left out: nothing is parked",
           baseline == pmix_list_get_size(&pmix_server_globals.grp_collectives));

    /* departed: rank 2 is lost while the block waits, and comes back */
    r2 = member(2, &far, false);
    rc = construct_by("grpret.departed", r0);
    report("departed: the first call parks", PMIX_SUCCESS == rc && !group_fired);
    report("departed: rank 2 is lost while the block waits", lose(r2, far));
    r2 = member(2, NULL, true);
    rc = construct_by("grpret.departed", r2);
    report("departed: the returned member is not counted as departed and as called",
           PMIX_SUCCESS == rc && !group_fired);
    rc = construct_by("grpret.departed", r1);
    report("departed: the block goes to the host once every member has called",
           PMIX_SUCCESS == rc && group_fired);
    progress_barrier();
    report("departed: nothing is parked",
           baseline == pmix_list_get_size(&pmix_server_globals.grp_collectives));

    /* called twice: rank 0 calls, its connection is lost, and it calls
     * again once back */
    r0 = member(0, &far, false);
    rc = construct_by("grpret.twice", r0);
    report("twice: the first call parks", PMIX_SUCCESS == rc && !group_fired);
    report("twice: the caller's connection is lost", lose(r0, far));
    r0 = member(0, NULL, true);
    rc = construct_by("grpret.twice", r0);
    report("twice: its second call parks", PMIX_SUCCESS == rc && !group_fired);
    rc = construct_by("grpret.twice", r1);
    report("twice: one process is counted once, so rank 2 is still awaited",
           PMIX_SUCCESS == rc && !group_fired);
    rc = construct_by("grpret.twice", r2);
    report("twice: the block goes to the host once every member has called",
           PMIX_SUCCESS == rc && group_fired);
    progress_barrier();
    report("twice: nothing is parked",
           baseline == pmix_list_get_size(&pmix_server_globals.grp_collectives));
}

int main(int argc, char **argv)
{
    static pmix_server_module_t mymodule = {0};
    pmix_proc_t twoprocs[2], bootproc;
    size_t baseline;
    pmix_status_t rc;

    (void) argc;
    (void) argv;

    fprintf(stdout, "server_group: server-side group handler unit tests\n");

    mymodule.group = stub_group;

    rc = PMIx_server_init(&mymodule, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "PMIx_server_init failed: %s\n", PMIx_Error_string(rc));
        return 1;
    }

    /* --- an empty group id --- *
     * The string unpacker spells "zero length" as a NULL pointer and
     * reports success, and every use of the id below is a bare strcmp or
     * strdup against it - get_tracker searches grp_collectives with one
     * and then strdups it into the new block. So PMIx_Group_construct("")
     * from any local client took the server's progress thread down, and
     * with it every client on this node. The client library screens a
     * NULL pointer, which an empty string is not. Against an unfixed
     * library this case does not fail, it segfaults. */
    rc = drive_group(NULL, GRPUT_NSPACE, 1, 1, 0, 0);
    report("group rejects an empty group id", PMIX_ERR_BAD_PARAM == rc);
    report("empty group id parks nothing", nothing_parked());
    report("empty group id never reached the host", !group_fired);

    /* --- counts that cannot survive the round trip --- *
     * The status slot is seeded at info[ninf] before anything is unpacked
     * into the array, so a count near SIZE_MAX wraps "ninf + 1" to zero -
     * for which PMIx_Info_create answers NULL - and the seed is written at
     * info[SIZE_MAX]. The proc count is the mirror image: it sizes an
     * allocation and is the size_t, not the int32_t the unpack consumed,
     * that the memcpy in get_tracker walks. */
    rc = drive_group("grput.badninfo", GRPUT_NSPACE, 1, 1, SIZE_MAX, 0);
    report("group rejects an info count of SIZE_MAX", PMIX_ERR_BAD_PARAM == rc);
    report("SIZE_MAX info count parks nothing", nothing_parked());

    rc = drive_group("grput.truncninfo", GRPUT_NSPACE, 1, 1, (size_t) 0x80000000ULL, 0);
    report("group rejects an info count that truncates negative",
           PMIX_ERR_BAD_PARAM == rc);

    rc = drive_group("grput.badnprocs", GRPUT_NSPACE, (size_t) 1 << 40, 1, 0, 0);
    report("group rejects an oversized proc count", PMIX_ERR_BAD_PARAM == rc);
    report("oversized proc count parks nothing", nothing_parked());


    /* --- an array shorter than the count that introduced it --- *
     * The unpack reports SUCCESS when it finds fewer elements than the
     * count promised - it just writes back how many it found. Unchecked,
     * the trailing procs stay as PMIX_PROC_CREATE left them, and an empty
     * nspace is what PMIX_CHECK_NSPACE reads as a WILDCARD: those phantom
     * participants are copied into the tracker, counted against namespaces
     * that do not exist, and sent to the host as members of the group. The
     * info array is the same shape one field along. Fence and connect
     * carry this screen; the group handler did not. */
    rc = drive_group("grput.shortprocs", GRPUT_NSPACE, 3, 1, 0, 0);
    report("group rejects a proc array shorter than its count",
           PMIX_ERR_BAD_PARAM == rc);
    report("a short proc array parks nothing", nothing_parked());

    rc = drive_group("grput.shortinfo", GRPUT_NSPACE, 1, 1, 3, 1);
    report("group rejects an info array shorter than its count",
           PMIX_ERR_BAD_PARAM == rc);
    report("a short info array parks nothing", nothing_parked());

    /* --- a well-formed construct --- *
     * The participant namespace is registered with no local procs, so
     * check_definition_complete has nothing to wait for and the block
     * completes on this single contribution and goes up to the host. */
    rc = register_nspace(GRPUT_NSPACE);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "register_nspace failed: %s\n", PMIx_Error_string(rc));
        PMIx_server_finalize();
        return 1;
    }
    rc = drive_group("grput.good", GRPUT_NSPACE, 1, 1, 1, 1);
    report("well-formed construct is accepted", PMIX_SUCCESS == rc);
    report("well-formed construct reached the host", group_fired);
    report("the host was given the group id we sent",
           NULL != group_id && 0 == strcmp("grput.good", group_id));
    report("the host was given the seeded status slot last",
           2 == group_ninfo && group_status_last);
    /* the host declined, so the handler answered every participant and
     * tore the block down - nothing may be left waiting */
    report("a refused construct parks nothing", nothing_parked());

    /* --- a group operation names no requester --- *
     * it is made for all of its members, so a PMIX_USERID or PMIX_GRPID
     * a member sends is not passed up */
    group_has_id = true;
    next_ids = true;
    rc = drive_group("grput.ids", GRPUT_NSPACE, 1, 1, 3, 3);
    next_ids = false;
    report("a construct carrying an identity reached the host",
           PMIX_SUCCESS == rc && group_fired);
    report("the member's PMIX_USERID and PMIX_GRPID are not passed up", !group_has_id);
    report("the rest of its directives and the status slot are",
           2 == group_ninfo && group_status_last);
    report("and it parks nothing", nothing_parked());

    /* --- group info is the sender's own --- *
     * Every member stores a contribution's values under the proc its
     * first entry names, and that has to be the proc that sent it: the
     * host is given the sender's name whether the array named itself,
     * named another proc, or - as a client before v6.0 sends it - named
     * no proc at all. */
    {
        pmix_proc_t other;

        rc = drive_group_contrib("grput.owncontrib", &pmix_globals.myid, false);
        report("group info naming its sender reaches the host so named",
               PMIX_SUCCESS == rc && group_fired && group_contrib_seen &&
                   PMIX_CHECK_PROCID(&group_contrib, &pmix_globals.myid) &&
                   2 == group_contrib_size);
        report("own contribution parks nothing", nothing_parked());

        PMIX_LOAD_PROCID(&other, GRPUT_NSPACE, 3);
        rc = drive_group_contrib("grput.othercontrib", &other, false);
        report("group info naming another proc reaches the host as the sender's",
               PMIX_SUCCESS == rc && group_fired && group_contrib_seen &&
                   PMIX_CHECK_PROCID(&group_contrib, &pmix_globals.myid) &&
                   2 == group_contrib_size);
        report("renamed contribution parks nothing", nothing_parked());

        rc = drive_group_contrib("grput.barecontrib", NULL, true);
        report("group info naming no proc reaches the host as the sender's",
               PMIX_SUCCESS == rc && group_fired && group_contrib_seen &&
                   PMIX_CHECK_PROCID(&group_contrib, &pmix_globals.myid) &&
                   2 == group_contrib_size);
        report("bare contribution parks nothing", nothing_parked());
    }

    /* --- a participant namespace that registers late --- *
     * check_definition_complete gives up the moment it meets a
     * participant namespace this server has not been told about, and the
     * only thing that ever calls it again is the arrival of another local
     * participant. So once the last local participant has contributed -
     * here, the only one - nothing was left to complete the block: it sat
     * on grp_collectives for the life of the server with every one of
     * those participants blocked in PMIx_Group_construct. Against an
     * unfixed library the first two assertions below pass and the last
     * two fail, which is exactly the hang. */
    rc = drive_group("grput.late", GRPUT_LATE, 1, 1, 1, 1);
    report("a construct naming an unregistered namespace is accepted",
           PMIX_SUCCESS == rc);
    report("it parks rather than reaching the host",
           !group_fired && !nothing_parked());

    rc = register_nspace(GRPUT_LATE);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "late register_nspace failed: %s\n", PMIx_Error_string(rc));
        PMIx_server_finalize();
        return 1;
    }
    progress_barrier();
    report("registering that namespace forwards the block", group_fired);
    report("and leaves nothing parked", nothing_parked());

    /* --- two participants, one namespace registering late --- *
     * The expected-local count is computed by walking every tracker on the
     * block, and every non-bootstrap participant hands us the same
     * membership - so once more than one participant has arrived before the
     * definition can be settled, each of them re-counts the whole
     * membership. The block's target becomes (real count x participants),
     * which its own participants can never reach: it sat on
     * grp_collectives for the life of the server with both clients blocked
     * in PMIx_Group_construct. Here two participants name a namespace with
     * two local procs plus one that is not registered yet, so the
     * definition can only be settled by the registration - with both
     * trackers already on the block, which is the state that miscounts. */
    rc = register_nspace_n(GRPUT_TWO, 2);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "register_nspace_n failed: %s\n", PMIx_Error_string(rc));
        PMIx_server_finalize();
        return 1;
    }
    PMIX_LOAD_PROCID(&twoprocs[0], GRPUT_TWO, PMIX_RANK_WILDCARD);
    PMIX_LOAD_PROCID(&twoprocs[1], GRPUT_LATE2, PMIX_RANK_WILDCARD);

    rc = drive_group_procs("grput.two", twoprocs, 2, false);
    report("first of two participants is accepted", PMIX_SUCCESS == rc);
    rc = drive_group_procs("grput.two", twoprocs, 2, false);
    report("second of two participants is accepted", PMIX_SUCCESS == rc);
    report("both park while a participant namespace is unknown",
           !group_fired && !nothing_parked());

    rc = register_nspace(GRPUT_LATE2);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "late register_nspace failed: %s\n", PMIx_Error_string(rc));
        PMIx_server_finalize();
        return 1;
    }
    progress_barrier();
    /* against an unfixed library nlocal is 4 rather than 2, so the block
     * never reaches its own target and these two fail */
    report("two participants are counted once, not once per participant",
           group_fired);
    report("and the block does not park forever", nothing_parked());

    /* --- a membership that names its namespace twice --- *
     * Two local procs, two participants, and a membership that lists the
     * namespace's wildcard twice. Each proc contributes once whatever
     * the list says, so the block expects two - against an unfixed
     * library it expected one per entry, four, and never fired. */
    PMIX_LOAD_PROCID(&twoprocs[0], GRPUT_TWO, PMIX_RANK_WILDCARD);
    PMIX_LOAD_PROCID(&twoprocs[1], GRPUT_TWO, PMIX_RANK_WILDCARD);
    rc = drive_group_procs("grput.dup", twoprocs, 2, false);
    report("first participant of a repeated membership is accepted", PMIX_SUCCESS == rc);
    rc = drive_group_procs("grput.dup", twoprocs, 2, false);
    report("second participant of a repeated membership is accepted", PMIX_SUCCESS == rc);
    progress_barrier();
    report("a repeated membership is counted once per proc", group_fired);
    report("and its block does not park forever", nothing_parked());

    /* --- one group per ID --- *
     * A group's ID is its own until it is destructed. The same membership
     * reported again - in another order, as an invited group is sorted and
     * a construct is not - is the same formation, and changes nothing;
     * another membership is refused and the record kept. Against an
     * unfixed library the second membership replaced the first. */
    {
        pmix_proc_t mine[2], rev[2], theirs[1], me1[1], others[2];
        pmix_peer_t *outsider;
        size_t held = 0;
        bool kept = false;

        /* members of a namespace the server knows, so a request naming them
         * reaches the host rather than waiting on a registration; the
         * server's own peer asks, and counts as a participant */
        PMIX_LOAD_PROCID(&mine[0], GRPUT_NSPACE, 0);
        PMIX_LOAD_PROCID(&mine[1], GRPUT_NSPACE, 1);
        memcpy(&rev[0], &mine[1], sizeof(pmix_proc_t));
        memcpy(&rev[1], &mine[0], sizeof(pmix_proc_t));
        PMIX_LOAD_PROCID(&theirs[0], GRPUT_NSPACE, 5);
        memcpy(&me1[0], &mine[0], sizeof(pmix_proc_t));

        rc = record("grput.held", mine, 2, &held, NULL);
        report("a group is recorded", PMIX_SUCCESS == rc && 2 == held);
        rc = record("grput.held", rev, 2, &held, NULL);
        report("the same membership again, in another order, changes nothing",
               PMIX_SUCCESS == rc && 2 == held);
        rc = record("grput.held", theirs, 1, NULL, NULL);
        report("another membership under a held ID is refused",
               PMIX_ERR_EXISTS == rc);
        held = held_with("grput.held", &mine[0], &kept);
        report("...and the group that holds it is kept", 2 == held && kept);

        /* the screen at the request: the host never sees it */
        rc = drive_op(PMIX_GROUP_CONSTRUCT, "grput.held", mine, 2, NULL);
        report("a construct of a held ID is refused", PMIX_ERR_EXISTS == rc);
        report("...before it reaches the host", !group_fired);
        report("...and parks nothing", nothing_parked());

        /* a destruct of it runs over the record - two members - though it
         * names the server alone */
        rc = drive_op(PMIX_GROUP_DESTRUCT, "grput.held", me1, 1, NULL);
        report("a member's destruct naming a subset reaches the host",
               PMIX_SUCCESS == rc && group_fired && PMIX_GROUP_DESTRUCT == group_op);
        report("...with the recorded membership, not the procs it named",
               2 == group_nprocs);
        report("...and ends the group", nothing_parked() && 0 == held_members("grput.held"));

        /* a client not in a held group may not end it, though it names
         * itself - against an unfixed library its own list was all that was
         * checked, and the reply dropped the record */
        PMIX_LOAD_PROCID(&others[0], GRPUT_NSPACE, 1);
        PMIX_LOAD_PROCID(&others[1], GRPUT_NSPACE, 2);
        rc = record("grput.theirs", others, 2, &held, NULL);
        report("another group is recorded", PMIX_SUCCESS == rc && 2 == held);
        outsider = other_peer(GRPUT_NSPACE, 7);
        PMIX_LOAD_PROCID(&me1[0], GRPUT_NSPACE, 7);
        rc = drive_op(PMIX_GROUP_DESTRUCT, "grput.theirs", me1, 1, outsider);
        report("a destruct by a process not in a held group is refused",
               PMIX_ERR_NOT_A_MEMBER == rc && !group_fired);
        report("...and the group is kept", 2 == held_members("grput.theirs"));
        rc = drive_op(PMIX_GROUP_DESTRUCT, "grput.theirs", NULL, 0, outsider);
        report("...as it is when it names no procs", PMIX_ERR_NOT_A_MEMBER == rc &&
               !group_fired && 2 == held_members("grput.theirs"));
        PMIX_RELEASE(outsider);
        /* the record's own end, for the cases after this one */
        rc = drive_op(PMIX_GROUP_DESTRUCT, "grput.theirs", NULL, 0, NULL);
        report("its member's destruct ends it", PMIX_SUCCESS == rc &&
               nothing_parked() && 0 == held_members("grput.theirs"));
    }

    /* --- an ID that names a job --- *
     * A group and a job must not answer to one name: a construct under the
     * namespace of a job the server knows is refused before the host sees
     * it - one the host registered, or the server's own. One known only
     * from a dmodex reply's data is not the server's to judge, and goes to
     * the host as before. */
    {
        pmix_proc_t mine[1];

        PMIX_LOAD_PROCID(&mine[0], GRPUT_NSPACE, 0);
        rc = drive_op(PMIX_GROUP_CONSTRUCT, GRPUT_NSPACE, mine, 1, NULL);
        report("a construct under a registered job's namespace is refused",
               PMIX_ERR_EXISTS == rc);
        report("...before it reaches the host", !group_fired);
        report("...and parks nothing", nothing_parked());
        rc = drive_op(PMIX_GROUP_CONSTRUCT, pmix_globals.myid.nspace, mine, 1, NULL);
        report("a construct under the server's own namespace is refused",
               PMIX_ERR_EXISTS == rc && !group_fired && nothing_parked());

        add_bare_nspace("grput.remote");
        group_accept = true;
        nheld = 0;
        rc = drive_op(PMIX_GROUP_CONSTRUCT, "grput.remote", mine, 1, NULL);
        report("a construct under a namespace known only from remote data reaches the host",
               PMIX_SUCCESS == rc && group_fired && 1 == nheld);
        if (1 == nheld) {
            held_cbfunc[0](PMIX_ERROR, NULL, 0, held_cbdata[0], NULL, NULL);
            progress_barrier();
            report("...and, failed by the host, parks nothing", nothing_parked());
        }
        group_accept = false;
        nheld = 0;
    }

    /* --- a construct the host completes over an ID recorded meanwhile --- *
     * The screen runs at the request; a group can still be recorded under
     * the ID before the host answers - by an invited group's announcement,
     * say, which the host should have refused. Its answer must not replace
     * that group. */
    group_accept = true;
    nheld = 0;
    {
        pmix_proc_t mine[1], others[2];
        pmix_info_t *ans;
        pmix_data_array_t darray;
        size_t held = 0;
        bool kept = false;

        PMIX_LOAD_PROCID(&mine[0], GRPUT_NSPACE, 0);
        PMIX_LOAD_PROCID(&others[0], GRPUT_NSPACE, 1);
        PMIX_LOAD_PROCID(&others[1], GRPUT_NSPACE, 2);
        rc = drive_op(PMIX_GROUP_CONSTRUCT, "grput.race", mine, 1, NULL);
        report("a construct of a free ID reaches the host",
               PMIX_SUCCESS == rc && group_fired && 1 == nheld);
        rc = record("grput.race", others, 2, &held, NULL);
        report("a group is recorded under the ID meanwhile", PMIX_SUCCESS == rc);
        if (1 == nheld) {
            PMIX_INFO_CREATE(ans, 1);
            PMIX_DATA_ARRAY_CONSTRUCT(&darray, 1, PMIX_PROC);
            memcpy(darray.array, mine, sizeof(pmix_proc_t));
            PMIX_INFO_LOAD(&ans[0], PMIX_GROUP_MEMBERSHIP, &darray, PMIX_DATA_ARRAY);
            PMIX_DATA_ARRAY_DESTRUCT(&darray);
            held_cbfunc[0](PMIX_SUCCESS, ans, 1, held_cbdata[0], NULL, NULL);
            progress_barrier();
            held = held_with("grput.race", &others[0], &kept);
            report("the host's answer does not replace it", 2 == held && kept);
            report("...and the construct parks nothing", nothing_parked());
            PMIX_INFO_FREE(ans, 1);
        }
    }
    group_accept = false;
    nheld = 0;

    /* --- two bootstrap blocks the host accepts separately --- *
     * A bootstrap participant gets a block of its own, so two of them on
     * one node put two blocks carrying the same group id on
     * grp_collectives, and each is handed to the host separately. The
     * completion sweep answers every block with that id - it has to, since
     * a host that treats them as one operation answers only one of them -
     * but it also freed them, including blocks whose own completion had not
     * arrived yet. The host then completed on a freed block: grpcbfunc
     * writes through it and _grpcbfunc strdup's its id. Against an unfixed
     * library the second completion below takes the process down. */
    group_accept = true;
    nheld = 0;
    /* measured against a baseline rather than against zero, so an earlier
     * case that stranded a block reports itself once rather than making
     * every case after it look broken too */
    progress_barrier();
    baseline = pmix_list_get_size(&pmix_server_globals.grp_collectives);
    PMIX_LOAD_PROCID(&bootproc, GRPUT_NSPACE, 0);
    rc = drive_group_procs("grput.boot", &bootproc, 1, true);
    report("first bootstrap participant is accepted", PMIX_SUCCESS == rc);
    rc = drive_group_procs("grput.boot", &bootproc, 1, true);
    report("second bootstrap participant is accepted", PMIX_SUCCESS == rc);
    report("the host was given both blocks", 2 == nheld);
    report("both blocks are parked with the host",
           baseline + 2 == pmix_list_get_size(&pmix_server_globals.grp_collectives));

    if (2 == nheld) {
        /* the first completion answers both participants and must leave the
         * second block alive for the completion it is still owed */
        held_cbfunc[0](PMIX_SUCCESS, NULL, 0, held_cbdata[0], NULL, NULL);
        progress_barrier();
        report("one completion clears both blocks off the list",
               baseline == pmix_list_get_size(&pmix_server_globals.grp_collectives));
        /* and this one must not run on freed memory */
        held_cbfunc[1](PMIX_SUCCESS, NULL, 0, held_cbdata[1], NULL, NULL);
        progress_barrier();
        report("the second completion is absorbed safely",
               baseline == pmix_list_get_size(&pmix_server_globals.grp_collectives));
    }
    group_accept = false;

    test_returning_member();

    if (NULL != group_id) {
        free(group_id);
        group_id = NULL;
    }
    PMIx_server_finalize();

    fprintf(stdout, "server_group: %d passed, %d failed\n", npass, nfail);
    return (0 == nfail) ? 0 : 1;
}
