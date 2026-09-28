/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/*
 * White-box tests of what the leader's server counts toward a pending
 * invitation.
 *
 * The server runs an invite/join construct for the leader: it raises
 * PMIX_GROUP_INVITED, counts the members' answers, and raises
 * PMIX_GROUP_CONSTRUCT_COMPLETE - carrying every acceptor's endpoint
 * contribution - once everyone has answered. The test drives an invitation
 * with the server's own peer as leader, answers it with events raised
 * locally under chosen sources, and watches for the completion through the
 * host's notify_event up-call. What is pinned down:
 *
 *   an answer counts only for the group it names
 *      An acceptance naming another group, or no group, is not an answer
 *      to this invitation.
 *
 *   a contribution counts only for its contributor
 *      Every member stores a contribution under the process it names, so
 *      one naming anyone but the accepting member is dropped.
 *
 *   a termination counts only from the terminated process's host
 *      The host reports a loss with the lost process as the source; one
 *      raised by some other process naming it is not a loss.
 *
 * Separately, a client may not raise the group events only servers raise:
 * that is covered in test/unit/server_events.c.
 */

#include "src/include/pmix_config.h"

#include "include/pmix.h"
#include "include/pmix_server.h"
#include "src/include/pmix_globals.h"
#include "src/server/pmix_server_ops.h"
#include "src/threads/pmix_threads.h"
#include "src/util/pmix_string_copy.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define INVNS "srvinv-x"

static int npass = 0;
static int nfail = 0;

static void report(const char *name, int passed)
{
    fprintf(stdout, "  %s: %s\n", passed ? "PASS" : "FAIL", name);
    fflush(stdout);
    if (!passed) {
        ++nfail;
    } else {
        ++npass;
    }
}

/* ---- the host: what completed, and whose contributions it carried ---- */

static pthread_mutex_t hostlock = PTHREAD_MUTEX_INITIALIZER;
static char completed_grp[PMIX_MAX_KEYLEN + 1];
static bool completed = false;
/* any outcome - completion, abort or failure - and the group it names */
static char resolved_grp[PMIX_MAX_KEYLEN + 1];
static bool resolved = false;
static pmix_proc_t contributors[8];
static size_t ncontributors = 0;

static pmix_status_t notify_event_fn(pmix_status_t code, const pmix_proc_t *source,
                                     pmix_data_range_t range, pmix_info_t info[],
                                     size_t ninfo, pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    size_t n;
    pmix_info_t *iptr;
    PMIX_HIDE_UNUSED_PARAMS(source, range, cbfunc, cbdata);

    if (PMIX_GROUP_CONSTRUCT_ABORT == code || PMIX_GROUP_INVITE_FAILED == code) {
        pthread_mutex_lock(&hostlock);
        resolved = true;
        resolved_grp[0] = '\0';
        for (n = 0; n < ninfo; n++) {
            if (PMIX_CHECK_KEY(&info[n], PMIX_GROUP_ID) && PMIX_STRING == info[n].value.type) {
                pmix_strncpy(resolved_grp, info[n].value.data.string, PMIX_MAX_KEYLEN);
            }
        }
        pthread_mutex_unlock(&hostlock);
        return PMIX_OPERATION_SUCCEEDED;
    }
    if (PMIX_GROUP_CONSTRUCT_COMPLETE != code) {
        return PMIX_OPERATION_SUCCEEDED;
    }
    pthread_mutex_lock(&hostlock);
    completed = true;
    completed_grp[0] = '\0';
    ncontributors = 0;
    for (n = 0; n < ninfo; n++) {
        if (PMIX_CHECK_KEY(&info[n], PMIX_GROUP_ID) && PMIX_STRING == info[n].value.type) {
            pmix_strncpy(completed_grp, info[n].value.data.string, PMIX_MAX_KEYLEN);
        } else if (PMIX_CHECK_KEY(&info[n], PMIX_PROC_INFO_ARRAY) &&
                   PMIX_DATA_ARRAY == info[n].value.type &&
                   NULL != info[n].value.data.darray &&
                   0 < info[n].value.data.darray->size && 8 > ncontributors) {
            iptr = (pmix_info_t *) info[n].value.data.darray->array;
            if (PMIX_PROC == iptr[0].value.type) {
                memcpy(&contributors[ncontributors], iptr[0].value.data.proc,
                       sizeof(pmix_proc_t));
                ++ncontributors;
            }
        }
    }
    pthread_mutex_unlock(&hostlock);
    return PMIX_OPERATION_SUCCEEDED;
}

static pmix_server_module_t mymodule = {
    .notify_event = notify_event_fn
};

static void reset_host(void)
{
    pthread_mutex_lock(&hostlock);
    completed = false;
    completed_grp[0] = '\0';
    resolved = false;
    resolved_grp[0] = '\0';
    ncontributors = 0;
    pthread_mutex_unlock(&hostlock);
}

/* did the invitation for grp resolve at all - completed, aborted or
 * failed? */
static bool saw_outcome(const char *grp, int tries)
{
    bool done = false;
    int i;

    for (i = 0; i < tries && !done; i++) {
        pthread_mutex_lock(&hostlock);
        done = (completed && 0 == strcmp(completed_grp, grp)) ||
               (resolved && 0 == strcmp(resolved_grp, grp));
        pthread_mutex_unlock(&hostlock);
        if (!done) {
            usleep(50000);
        }
    }
    return done;
}

/* did the invitation for grp complete, waiting up to about a second? */
static bool saw_complete(const char *grp, int tries)
{
    bool done = false;
    int i;

    for (i = 0; i < tries && !done; i++) {
        pthread_mutex_lock(&hostlock);
        done = completed && 0 == strcmp(completed_grp, grp);
        pthread_mutex_unlock(&hostlock);
        if (!done) {
            usleep(50000);
        }
    }
    return done;
}

static bool contributed(const pmix_proc_t *proc)
{
    size_t n;
    bool found = false;

    pthread_mutex_lock(&hostlock);
    for (n = 0; n < ncontributors; n++) {
        if (0 == strncmp(contributors[n].nspace, proc->nspace, PMIX_MAX_NSLEN) &&
            contributors[n].rank == proc->rank) {
            found = true;
        }
    }
    pthread_mutex_unlock(&hostlock);
    return found;
}

/* ---- starting an invitation, on the progress thread ---- */

typedef struct {
    pmix_event_t ev;
    pmix_lock_t lock;
    const char *grp;
    pmix_proc_t *procs;
    size_t nprocs;
    pmix_status_t status;
} invreq_t;

/* the handler answers through this before it returns */
static pmix_status_t invite_status = PMIX_ERROR;

static void invite_done_cb(pmix_status_t status, void *cbdata)
{
    pmix_server_caddy_t *cd = (pmix_server_caddy_t *) cbdata;

    invite_status = status;
    PMIX_RELEASE(cd);
}

static void do_invite(int sd, short args, void *cbdata)
{
    invreq_t *r = (invreq_t *) cbdata;
    pmix_server_caddy_t *cd;
    pmix_buffer_t buf;
    uint32_t timeout = 0;
    bool no = false;
    pmix_status_t rc;
    char *grp = (char *) r->grp;

    (void) sd;
    (void) args;
    PMIX_CONSTRUCT(&buf, pmix_buffer_t);
    PMIX_BFROPS_ASSIGN_TYPE(pmix_globals.mypeer, &buf);
    PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, &buf, &grp, 1, PMIX_STRING);
    if (PMIX_SUCCESS == rc) {
        PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, &buf, &r->nprocs, 1, PMIX_SIZE);
    }
    if (PMIX_SUCCESS == rc) {
        PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, &buf, r->procs, r->nprocs, PMIX_PROC);
    }
    if (PMIX_SUCCESS == rc) {
        PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, &buf, &timeout, 1, PMIX_UINT32);
    }
    if (PMIX_SUCCESS == rc) {
        PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, &buf, &no, 1, PMIX_BOOL);
    }
    if (PMIX_SUCCESS == rc) {
        PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, &buf, &no, 1, PMIX_BOOL);
    }
    if (PMIX_SUCCESS != rc) {
        PMIX_DESTRUCT(&buf);
        r->status = rc;
        PMIX_WAKEUP_THREAD(&r->lock);
        return;
    }
    cd = PMIX_NEW(pmix_server_caddy_t);
    PMIX_RETAIN(pmix_globals.mypeer);
    cd->peer = pmix_globals.mypeer;
    invite_status = PMIX_ERROR;
    pmix_server_group_invite(cd, &buf, invite_done_cb);
    r->status = invite_status;
    PMIX_DESTRUCT(&buf);
    PMIX_WAKEUP_THREAD(&r->lock);
}

static pmix_status_t invite(const char *grp, pmix_proc_t *procs, size_t nprocs)
{
    invreq_t r;

    memset(&r, 0, sizeof(r));
    PMIX_CONSTRUCT_LOCK(&r.lock);
    r.grp = grp;
    r.procs = procs;
    r.nprocs = nprocs;
    r.status = PMIX_ERROR;
    PMIX_THREADSHIFT(&r, do_invite);
    PMIX_WAIT_THREAD(&r.lock);
    PMIX_DESTRUCT_LOCK(&r.lock);
    return r.status;
}

/* ---- answers, raised locally under a chosen source ---- */

/* a contribution naming "who", carrying one value */
static void load_contribution(pmix_info_t *dst, const pmix_proc_t *who)
{
    pmix_data_array_t darray;
    pmix_info_t *iptr;

    PMIX_DATA_ARRAY_CONSTRUCT(&darray, 2, PMIX_INFO);
    iptr = (pmix_info_t *) darray.array;
    PMIX_INFO_LOAD(&iptr[0], PMIX_PROCID, who, PMIX_PROC);
    PMIX_INFO_LOAD(&iptr[1], "srvinv.endpt", "addr", PMIX_STRING);
    PMIX_INFO_LOAD(dst, PMIX_PROC_INFO_ARRAY, &darray, PMIX_DATA_ARRAY);
    PMIX_DATA_ARRAY_DESTRUCT(&darray);
}

/* Answers are raised do-not-cache: a cached event is replayed to every
 * observer registered after it, so a later invitation would otherwise see
 * an earlier case's answers too. */
static void answer(pmix_status_t code, const pmix_proc_t *source, const char *grp,
                   const pmix_proc_t *contributor)
{
    pmix_info_t info[3];
    size_t n = 0;

    PMIX_INFO_LOAD(&info[n], PMIX_EVENT_DO_NOT_CACHE, NULL, PMIX_BOOL);
    ++n;

    if (NULL != grp) {
        PMIX_INFO_LOAD(&info[n], PMIX_GROUP_ID, grp, PMIX_STRING);
        ++n;
    }
    if (NULL != contributor) {
        load_contribution(&info[n], contributor);
        ++n;
    }
    PMIx_Notify_event(code, source, PMIX_RANGE_LOCAL, info, n, NULL, NULL);
    while (0 < n) {
        --n;
        PMIX_INFO_DESTRUCT(&info[n]);
    }
}

static void terminated(const pmix_proc_t *source, const pmix_proc_t *lost)
{
    pmix_info_t info[2];

    PMIX_INFO_LOAD(&info[0], PMIX_EVENT_DO_NOT_CACHE, NULL, PMIX_BOOL);
    PMIX_INFO_LOAD(&info[1], PMIX_EVENT_AFFECTED_PROC, lost, PMIX_PROC);
    PMIx_Notify_event(PMIX_PROC_TERMINATED, source, PMIX_RANGE_LOCAL, info, 2, NULL, NULL);
    PMIX_INFO_DESTRUCT(&info[0]);
    PMIX_INFO_DESTRUCT(&info[1]);
}

int main(int argc, char **argv)
{
    pmix_proc_t procs[3], p1, p2;
    pmix_status_t rc;
    PMIX_HIDE_UNUSED_PARAMS(argc, argv);

    fprintf(stdout, "\n=== what a pending invitation counts ===\n\n");
    rc = PMIx_server_init(&mymodule, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "PMIx_server_init failed: %s\n", PMIx_Error_string(rc));
        return 1;
    }
    PMIX_LOAD_PROCID(&procs[0], pmix_globals.myid.nspace, pmix_globals.myid.rank);
    PMIX_LOAD_PROCID(&p1, INVNS, 1);
    PMIX_LOAD_PROCID(&p2, INVNS, 2);
    memcpy(&procs[1], &p1, sizeof(pmix_proc_t));
    memcpy(&procs[2], &p2, sizeof(pmix_proc_t));

    /* --- an answer counts only for the group it names --- */
    reset_host();
    rc = invite("srvinv-g1", procs, 2);
    report("an invitation starts", PMIX_SUCCESS == rc);
    answer(PMIX_GROUP_INVITE_ACCEPTED, &p1, "srvinv-other", NULL);
    answer(PMIX_GROUP_INVITE_ACCEPTED, &p1, NULL, NULL);
    report("an acceptance naming another group, or none, is not counted",
           !saw_outcome("srvinv-g1", 6));
    answer(PMIX_GROUP_INVITE_ACCEPTED, &p1, "srvinv-g1", NULL);
    report("an acceptance naming the group completes it", saw_complete("srvinv-g1", 40));

    /* --- a contribution counts only for its contributor --- */
    reset_host();
    rc = invite("srvinv-g2", procs, 2);
    report("a second invitation starts", PMIX_SUCCESS == rc);
    answer(PMIX_GROUP_INVITE_ACCEPTED, &p1, "srvinv-g2", &p2);
    report("an acceptance still completes the group", saw_complete("srvinv-g2", 40));
    report("a contribution naming another process is dropped", !contributed(&p2));

    reset_host();
    rc = invite("srvinv-g3", procs, 2);
    answer(PMIX_GROUP_INVITE_ACCEPTED, &p1, "srvinv-g3", &p1);
    report("the accepting member's own contribution is kept",
           saw_complete("srvinv-g3", 40) && contributed(&p1));

    /* --- a termination counts only from the terminated process's host --- */
    reset_host();
    rc = invite("srvinv-g4", procs, 3);
    report("a third invitation starts", PMIX_SUCCESS == rc);
    answer(PMIX_GROUP_INVITE_ACCEPTED, &p2, "srvinv-g4", NULL);
    terminated(&p2, &p1);
    report("a loss raised by another process is not counted",
           !saw_outcome("srvinv-g4", 6));
    /* the lost member never accepted, so counting its loss resolves the
     * invitation - as a failure, the membership being incomplete */
    terminated(&p1, &p1);
    report("a loss raised for the lost process is counted", saw_outcome("srvinv-g4", 40));

    PMIx_server_finalize();
    fprintf(stdout, "\nserver_invite: %d passed, %d failed\n", npass, nfail);
    return (0 == nfail) ? 0 : 1;
}
