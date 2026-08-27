/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * White-box unit tests for src/server/pmix_server_registration.c.
 *
 * Five behaviors, each of which was wrong and none of which any other
 * suite reaches:
 *
 *   1. PMIX_REGISTER_NODATA suppresses the *data* half of a registration
 *      and nothing else. The call still carries nlocalprocs, which is
 *      exactly what a pending collective is waiting on - and the two
 *      sweeps at the foot of _register_nspace have no other callers in
 *      the tree, so nothing later re-drives them. Returning early on the
 *      directive left every tracker naming the namespace incomplete for
 *      the life of the server.
 *
 *   2. An empty namespace is a wildcard, not a no-op. PMIX_CHECK_NSPACE
 *      answers true whenever either name is NULL or empty, and
 *      _deregister_nspace was the one lookup in the file using it - so
 *      PMIx_server_deregister_nspace("") tore down whichever namespace
 *      sat first on pmix_globals.nspaces. The entry points now refuse an
 *      empty name and the handler compares exactly.
 *
 *   3. A PMIX_PROC_INFO_ARRAY says which proc it describes with a
 *      PMIX_RANK or a PMIX_PROCID, in any position. The update path read
 *      iptr[0].value.data.rank outright, so a host that identified the
 *      proc any other way had its data stored against whatever the first
 *      element's union happened to hold.
 *
 *   4. pmix_globals.iof_requests holds entries this process registered
 *      itself, whose requestor is NULL. pmix_server_purge_events treated
 *      those as corrupt and freed them, so a launcher lost its own IOF
 *      registration the first time any local client finalized.
 *
 *   5. All four entry points screen the *server* library's init flag.
 *      pmix_globals.initialized is set by PMIx_Init and PMIx_tool_init
 *      too, and _register_nspace walks pmix_server_globals.collectives,
 *      which is only PMIX_LIST_STATIC_INIT until PMIx_server_init runs -
 *      a NULL sentinel, so the walk dereferences NULL. That crash lands
 *      on the progress thread after the call returned, so those cases run
 *      in a forked child.
 */

#include "src/include/pmix_config.h"

#include "include/pmix.h"
#include "include/pmix_server.h"
#include "include/pmix_tool.h"

#include "src/include/pmix_globals.h"
#include "src/mca/gds/gds.h"
#include "src/server/pmix_server_ops.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

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

/* PMIx_Store_internal blocks on the progress thread, so anything queued
 * ahead of it has run by the time it returns. Same idiom as
 * test/unit/server_fence.c. */
static void progress_barrier(void)
{
    pmix_value_t v;

    PMIX_VALUE_LOAD(&v, "barrier", PMIX_STRING);
    PMIx_Store_internal(&pmix_globals.myid, "server-reg-ut.barrier", &v);
    PMIX_VALUE_DESTRUCT(&v);
}

static pmix_lock_t oplock;
static pmix_status_t opstatus = PMIX_ERR_NOT_SUPPORTED;

static void opcbfunc(pmix_status_t status, void *cbdata)
{
    PMIX_HIDE_UNUSED_PARAMS(cbdata);

    opstatus = status;
    PMIX_WAKEUP_THREAD(&oplock);
}

/* ------------------------------------------------------------------ *
 * 1. the NODATA sweep
 * ------------------------------------------------------------------ */

#define NODATANS "regut-nodata"

typedef struct {
    pmix_event_t ev;
    pmix_lock_t lock;
    pmix_server_trkr_t *trk;
} trkbuild_t;

/* new_tracker touches pmix_server_globals.collectives, so it runs on the
 * progress thread. */
static void build_tracker(int sd, short args, void *cbdata)
{
    trkbuild_t *b = (trkbuild_t *) cbdata;
    pmix_proc_t procs[1];

    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    PMIX_LOAD_PROCID(&procs[0], NODATANS, PMIX_RANK_WILDCARD);
    b->trk = pmix_server_new_tracker(NULL, procs, 1, PMIX_FENCENB_CMD);

    PMIX_WAKEUP_THREAD(&b->lock);
}

static void drop_tracker(int sd, short args, void *cbdata)
{
    trkbuild_t *b = (trkbuild_t *) cbdata;

    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    /* unlink before releasing - being on the list is not a reference */
    pmix_list_remove_item(&pmix_server_globals.collectives, &b->trk->super);
    PMIX_RELEASE(b->trk);

    PMIX_WAKEUP_THREAD(&b->lock);
}

/* ------------------------------------------------------------------ *
 * 3. reading a proc-info array back out of the datastore
 * ------------------------------------------------------------------ */

#define UPDNS "regut-update"
#define UPDHOST "regut-node-7"

typedef struct {
    pmix_event_t ev;
    pmix_lock_t lock;
    pmix_status_t status;
    char *host;
} fetchchk_t;

static void fetch_hostname(int sd, short args, void *cbdata)
{
    fetchchk_t *f = (fetchchk_t *) cbdata;
    pmix_cb_t cb;
    pmix_proc_t proc;
    pmix_kval_t *kv;

    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    PMIX_CONSTRUCT(&cb, pmix_cb_t);
    /* the update path stores through mypeer's gds at PMIX_REMOTE scope */
    PMIX_LOAD_PROCID(&proc, UPDNS, 1);
    cb.proc = &proc;
    cb.key = PMIX_HOSTNAME;
    cb.scope = PMIX_REMOTE;
    PMIX_GDS_FETCH_KV(f->status, pmix_globals.mypeer, &cb);
    if (PMIX_SUCCESS == f->status) {
        kv = (pmix_kval_t *) pmix_list_get_first(&cb.kvs);
        if (NULL != kv && NULL != kv->value && PMIX_STRING == kv->value->type
            && NULL != kv->value->data.string) {
            f->host = strdup(kv->value->data.string);
        }
    }
    cb.proc = NULL;
    cb.key = NULL;
    PMIX_DESTRUCT(&cb);

    PMIX_WAKEUP_THREAD(&f->lock);
}

/* ------------------------------------------------------------------ *
 * 4. an IOF request this process registered itself
 * ------------------------------------------------------------------ */

typedef struct {
    pmix_event_t ev;
    pmix_lock_t lock;
    int idx;
    bool survived;
} iofchk_t;

static void check_iof_purge(int sd, short args, void *cbdata)
{
    iofchk_t *c = (iofchk_t *) cbdata;
    pmix_iof_req_t *req;
    pmix_proc_t proc;

    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    /* the shape PMIx_IOF_pull leaves behind in a process that has an
     * upstream server of its own: no requestor peer, because the
     * registration was forwarded rather than served here */
    req = PMIX_NEW(pmix_iof_req_t);
    if (NULL == req) {
        c->idx = -1;
        PMIX_WAKEUP_THREAD(&c->lock);
        return;
    }
    req->channels = PMIX_FWD_STDOUT_CHANNEL;
    c->idx = pmix_pointer_array_add(&pmix_globals.iof_requests, req);
    if (0 > c->idx) {
        PMIX_RELEASE(req);
        PMIX_WAKEUP_THREAD(&c->lock);
        return;
    }

    /* a namespace departing - the deregister path's own call */
    PMIX_LOAD_PROCID(&proc, "regut-someone-else", PMIX_RANK_WILDCARD);
    pmix_server_purge_events(NULL, &proc, PMIX_ERR_NOT_FOUND);

    c->survived = (req
                   == (pmix_iof_req_t *) pmix_pointer_array_get_item(
                       &pmix_globals.iof_requests, c->idx));
    if (c->survived) {
        pmix_pointer_array_set_item(&pmix_globals.iof_requests, c->idx, NULL);
        PMIX_RELEASE(req);
    }

    PMIX_WAKEUP_THREAD(&c->lock);
}

/* ------------------------------------------------------------------ *
 * 5. the entry points, reached from a process with no server library
 * ------------------------------------------------------------------ */

/* May or may not be called: what these entry points do for a role with
 * no server library behind them is not a contract, so this exists only
 * to give the request somewhere to land. */
static void tolerant_op(pmix_status_t status, void *cbdata)
{
    PMIX_HIDE_UNUSED_PARAMS(status, cbdata);
}

static int tool_child(int which)
{
    pmix_proc_t myproc, target;
    pmix_info_t tinfo;
    pmix_status_t rc;
    pmix_nspace_t ns;

    PMIX_INFO_LOAD(&tinfo, PMIX_TOOL_DO_NOT_CONNECT, NULL, PMIX_BOOL);
    rc = PMIx_tool_init(&myproc, &tinfo, 1);
    PMIX_INFO_DESTRUCT(&tinfo);
    if (PMIX_SUCCESS != rc) {
        return 3;
    }

    PMIX_LOAD_NSPACE(ns, "regut-child");
    PMIX_LOAD_PROCID(&target, "regut-child", 0);

    if (0 == which) {
        rc = PMIx_server_register_nspace(ns, 1, NULL, 0, tolerant_op, NULL);
    } else {
        rc = PMIx_server_register_client(&target, geteuid(), getegid(), NULL,
                                         tolerant_op, NULL);
    }
    /* give the progress thread a turn: the shifted handler is what used
     * to walk the NULL sentinel, and it has not run yet. What the entry
     * point answered a tool is not asserted - it is not a contract. */
    (void) PMIx_Get(&myproc, PMIX_UNIV_SIZE, NULL, 0, NULL);
    PMIx_tool_finalize();
    (void) rc;
    return 0;
}

static void check_tool_refusal(const char *name, int which)
{
    pid_t child;
    int status = 0;

    fflush(stdout);
    child = fork();
    if (0 == child) {
        _exit(tool_child(which));
    }
    if (0 > child) {
        report(name, 0);
        return;
    }
    if (0 > waitpid(child, &status, 0)) {
        report(name, 0);
        return;
    }
    report(name, WIFEXITED(status) && 0 == WEXITSTATUS(status));
}

/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    static pmix_server_module_t mymodule = {0};
    pmix_status_t rc;
    pmix_proc_t pr;
    pmix_nspace_t ns;
    pmix_namespace_t *nptr, *tmp;
    size_t nsbefore, nsafter;
    bool found;

    PMIX_HIDE_UNUSED_PARAMS(argc, argv);

    fprintf(stdout, "server_registration: namespace/client registration unit tests\n");

    /* the refusal cases fork, so they run before the library is up */
    check_tool_refusal("register_nspace from a tool is not fatal", 0);
    check_tool_refusal("register_client from a tool is not fatal", 1);

    rc = PMIx_server_init(&mymodule, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "PMIx_server_init failed: %s\n", PMIx_Error_string(rc));
        return 1;
    }

    /* --- argument screens ------------------------------------------- */
    PMIX_LOAD_NSPACE(ns, "");
    rc = PMIx_server_register_nspace(ns, 1, NULL, 0, NULL, NULL);
    report("register_nspace refuses an empty namespace", PMIX_ERR_BAD_PARAM == rc);

    rc = PMIx_server_register_client(NULL, geteuid(), getegid(), NULL, NULL, NULL);
    report("register_client refuses a NULL proc", PMIX_ERR_BAD_PARAM == rc);

    PMIX_LOAD_PROCID(&pr, "", 0);
    rc = PMIx_server_register_client(&pr, geteuid(), getegid(), NULL, NULL, NULL);
    report("register_client refuses an empty namespace", PMIX_ERR_BAD_PARAM == rc);

    /* --- an empty deregistration must not take a real namespace ------ *
     * Against an unfixed library PMIX_CHECK_NSPACE matches the first
     * namespace on the list and this tears it down. */
    PMIX_LOAD_PROCID(&pr, "regut-bystander", 0);
    PMIx_server_register_client(&pr, geteuid(), getegid(), NULL, NULL, NULL);
    PMIX_LOAD_NSPACE(ns, "regut-bystander");
    PMIx_server_register_nspace(ns, 1, NULL, 0, NULL, NULL);
    progress_barrier();

    nsbefore = pmix_list_get_size(&pmix_globals.nspaces);

    PMIX_CONSTRUCT_LOCK(&oplock);
    opstatus = PMIX_ERR_NOT_SUPPORTED;
    PMIX_LOAD_NSPACE(ns, "");
    PMIx_server_deregister_nspace(ns, opcbfunc, NULL);
    PMIX_WAIT_THREAD(&oplock);
    PMIX_DESTRUCT_LOCK(&oplock);
    report("deregister_nspace refuses an empty namespace", PMIX_ERR_BAD_PARAM == opstatus);

    progress_barrier();
    nsafter = pmix_list_get_size(&pmix_globals.nspaces);
    found = false;
    PMIX_LIST_FOREACH (tmp, &pmix_globals.nspaces, pmix_namespace_t) {
        if (0 == strcmp(tmp->nspace, "regut-bystander")) {
            found = true;
            break;
        }
    }
    report("no namespace was destroyed by it", nsbefore == nsafter && found);

    /* --- PMIX_REGISTER_NODATA still finishes a pending collective ---- */
    {
        trkbuild_t b;
        pmix_info_t nodata;

        PMIX_LOAD_PROCID(&pr, NODATANS, 0);
        PMIx_server_register_client(&pr, geteuid(), getegid(), NULL, NULL, NULL);
        PMIX_LOAD_PROCID(&pr, NODATANS, 1);
        PMIx_server_register_client(&pr, geteuid(), getegid(), NULL, NULL, NULL);
        progress_barrier();

        memset(&b, 0, sizeof(b));
        PMIX_CONSTRUCT_LOCK(&b.lock);
        PMIX_THREADSHIFT(&b, build_tracker);
        PMIX_WAIT_THREAD(&b.lock);
        PMIX_DESTRUCT_LOCK(&b.lock);

        report("a tracker parked on the unregistered namespace was built", NULL != b.trk);
        if (NULL != b.trk) {
            report("the parked tracker starts undefined and uncounted",
                   !b.trk->def_complete && 0 == b.trk->nlocal);

            PMIX_INFO_LOAD(&nodata, PMIX_REGISTER_NODATA, NULL, PMIX_BOOL);
            PMIX_LOAD_NSPACE(ns, NODATANS);
            PMIx_server_register_nspace(ns, 2, &nodata, 1, NULL, NULL);
            PMIX_INFO_DESTRUCT(&nodata);
            progress_barrier();

            report("a NODATA registration still defines the tracker",
                   b.trk->def_complete);
            report("a NODATA registration still contributes its local count",
                   2 == b.trk->nlocal);

            PMIX_CONSTRUCT_LOCK(&b.lock);
            PMIX_THREADSHIFT(&b, drop_tracker);
            PMIX_WAIT_THREAD(&b.lock);
            PMIX_DESTRUCT_LOCK(&b.lock);
        }
    }

    /* --- a proc-info array identified by PMIX_PROCID, not first ------ */
    {
        pmix_info_t upd;
        pmix_data_array_t *da;
        pmix_info_t *iptr;
        pmix_proc_t tgt;
        fetchchk_t f;

        PMIX_LOAD_PROCID(&pr, UPDNS, 0);
        PMIx_server_register_client(&pr, geteuid(), getegid(), NULL, NULL, NULL);
        PMIX_LOAD_NSPACE(ns, UPDNS);
        PMIx_server_register_nspace(ns, 1, NULL, 0, NULL, NULL);
        progress_barrier();

        /* the identifier is a PMIX_PROCID and it is NOT in slot 0 - both
         * of which the old code got wrong */
        da = PMIx_Data_array_create(2, PMIX_INFO);
        iptr = (pmix_info_t *) da->array;
        PMIX_INFO_LOAD(&iptr[0], PMIX_HOSTNAME, UPDHOST, PMIX_STRING);
        PMIX_LOAD_PROCID(&tgt, UPDNS, 1);
        PMIX_INFO_LOAD(&iptr[1], PMIX_PROCID, &tgt, PMIX_PROC);

        PMIX_INFO_CONSTRUCT(&upd);
        PMIX_LOAD_KEY(upd.key, PMIX_PROC_INFO_ARRAY);
        upd.value.type = PMIX_DATA_ARRAY;
        upd.value.data.darray = da;

        PMIX_LOAD_NSPACE(ns, UPDNS);
        rc = PMIx_server_register_nspace(ns, -1, &upd, 1, NULL, NULL);
        report("an update carrying a proc-info array is accepted",
               PMIX_OPERATION_SUCCEEDED == rc || PMIX_SUCCESS == rc);
        PMIX_INFO_DESTRUCT(&upd);
        progress_barrier();

        memset(&f, 0, sizeof(f));
        PMIX_CONSTRUCT_LOCK(&f.lock);
        PMIX_THREADSHIFT(&f, fetch_hostname);
        PMIX_WAIT_THREAD(&f.lock);
        PMIX_DESTRUCT_LOCK(&f.lock);

        report("the array's data landed on the rank the PMIX_PROCID named",
               NULL != f.host && 0 == strcmp(f.host, UPDHOST));
        if (NULL != f.host) {
            free(f.host);
        }
    }

    /* --- a purge must not reap this process's own IOF registration --- */
    {
        iofchk_t c;

        memset(&c, 0, sizeof(c));
        PMIX_CONSTRUCT_LOCK(&c.lock);
        PMIX_THREADSHIFT(&c, check_iof_purge);
        PMIX_WAIT_THREAD(&c.lock);
        PMIX_DESTRUCT_LOCK(&c.lock);

        report("an IOF request with no requestor was recorded", 0 <= c.idx);
        report("purging a departing namespace leaves our own request alone",
               c.survived);
    }

    /* an ordinary deregistration still works */
    PMIX_CONSTRUCT_LOCK(&oplock);
    opstatus = PMIX_ERR_NOT_SUPPORTED;
    PMIX_LOAD_NSPACE(ns, "regut-bystander");
    PMIx_server_deregister_nspace(ns, opcbfunc, NULL);
    PMIX_WAIT_THREAD(&oplock);
    PMIX_DESTRUCT_LOCK(&oplock);
    progress_barrier();

    nptr = NULL;
    PMIX_LIST_FOREACH (tmp, &pmix_globals.nspaces, pmix_namespace_t) {
        if (0 == strcmp(tmp->nspace, "regut-bystander")) {
            nptr = tmp;
            break;
        }
    }
    report("a named deregistration removes that namespace", NULL == nptr);

    PMIx_server_finalize();

    fprintf(stdout, "server_registration: %d passed, %d failed\n", npass, nfail);
    return (0 == nfail) ? 0 : 1;
}
