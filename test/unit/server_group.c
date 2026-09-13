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
#include "src/server/pmix_server_ops.h"
#include "src/threads/pmix_threads.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
    (void) op;
    (void) procs;
    (void) nprocs;

    group_fired = true;
    group_ninfo = ndirs;
    group_status_last = false;
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
            if (r->bootstrap) {
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
    PMIX_RETAIN(pmix_globals.mypeer);
    cd->peer = pmix_globals.mypeer;
    cd->hdr.tag = 0;

    rc = pmix_server_group(cd, &buf, PMIX_GROUP_CONSTRUCT);
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

    if (NULL != group_id) {
        free(group_id);
        group_id = NULL;
    }
    PMIx_server_finalize();

    fprintf(stdout, "server_group: %d passed, %d failed\n", npass, nfail);
    return (0 == nfail) ? 0 : 1;
}
