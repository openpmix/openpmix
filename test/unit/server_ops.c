/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * White-box unit tests for the classic client commands in
 * src/server/pmix_server_ops.c - abort, publish, lookup and spawn -
 * driven from hand-packed wire buffers, the way the switchyard drives
 * them.
 *
 * The process comes up as a PMIx server whose stub host module supplies
 * just enough of pmix_server_module_t to reach the arms under test.
 *
 * Test cases:
 *
 *   abort proc count that
 *      wraps                 -> PMIX_ERR_BAD_PARAM (kills an unfixed
 *                               library rather than failing it)
 *   abort proc count that
 *      truncates negative    -> PMIX_ERR_BAD_PARAM
 *   well-formed abort        -> reaches the host with the procs we sent
 *   publish                  -> the host is given the uid the connection
 *                               handshake established, NOT the one the
 *                               command carried
 *   lookup answered with
 *      PMIX_OPERATION_SUCCEEDED -> PMIX_ERR_NOT_SUPPORTED
 *   spawn carrying no
 *      job-level directives  -> the IOF directives are still parsed, so a
 *                               tool's child job is forwarded to it
 *
 * What this file cannot reach: anything that needs a peer with a live
 * socket, and the multi-node half of IOF inheritance. Those are covered
 * by test/unit/iof_inherit.c and contrib/dockerswarm/run-spawn-iof.sh.
 */

#include "src/include/pmix_config.h"

#include "include/pmix.h"
#include "include/pmix_server.h"

#include "src/include/pmix_globals.h"
#include "src/mca/bfrops/bfrops.h"
#include "src/mca/ptl/ptl_types.h"
#include "src/server/pmix_server_ops.h"
#include "src/threads/pmix_threads.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define OPSUT_ABORT_MSG "server-ops-ut abort"
#define OPSUT_CHILD     "server-ops-ut-child"

/* a uid the command claims, deliberately not the one this process runs
 * as - "nobody" on every platform we build on, and in any case not
 * geteuid(), which is what the peer was registered with */
#define OPSUT_CLAIMED_UID ((uint32_t) 4242)

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

/* ------------------------------------------------------------------ */
/* what the host entry points were handed                             */
/* ------------------------------------------------------------------ */

static bool abort_fired = false;
static size_t abort_nprocs = 0;
static char *abort_msg = NULL;

static bool publish_fired = false;
static uint32_t publish_uid = 0;
static bool publish_uid_found = false;

static bool lookup_fired = false;

static bool spawn_fired = false;
static size_t spawn_ninfo = 0;

static pmix_status_t stub_abort(const pmix_proc_t *proc, void *server_object,
                                int status, const char msg[],
                                pmix_proc_t procs[], size_t nprocs,
                                pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    (void) proc;
    (void) server_object;
    (void) status;
    (void) procs;

    abort_fired = true;
    abort_nprocs = nprocs;
    if (NULL != abort_msg) {
        free(abort_msg);
        abort_msg = NULL;
    }
    if (NULL != msg) {
        abort_msg = strdup(msg);
    }
    if (NULL != cbfunc) {
        cbfunc(PMIX_SUCCESS, cbdata);
    }
    return PMIX_SUCCESS;
}

static pmix_status_t stub_publish(const pmix_proc_t *proc,
                                  const pmix_info_t info[], size_t ninfo,
                                  pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    size_t n;

    (void) proc;

    publish_fired = true;
    publish_uid_found = false;
    publish_uid = 0;
    for (n = 0; n < ninfo; n++) {
        if (PMIX_CHECK_KEY(&info[n], PMIX_USERID)) {
            publish_uid_found = true;
            publish_uid = info[n].value.data.uint32;
        }
    }
    if (NULL != cbfunc) {
        cbfunc(PMIX_SUCCESS, cbdata);
    }
    return PMIX_SUCCESS;
}

/* Answer the way a host that finished the work inline is tempted to.
 * There is no channel for the data in that answer, so the library has to
 * refuse it rather than let the client read a status-only reply as a
 * successful lookup. */
static pmix_status_t stub_lookup(const pmix_proc_t *proc, char **keys,
                                 const pmix_info_t info[], size_t ninfo,
                                 pmix_lookup_cbfunc_t cbfunc, void *cbdata)
{
    (void) proc;
    (void) keys;
    (void) info;
    (void) ninfo;
    (void) cbfunc;
    (void) cbdata;

    lookup_fired = true;
    return PMIX_OPERATION_SUCCEEDED;
}

static pmix_status_t stub_spawn(const pmix_proc_t *proc,
                                const pmix_info_t job_info[], size_t ninfo,
                                const pmix_app_t apps[], size_t napps,
                                pmix_spawn_cbfunc_t cbfunc, void *cbdata)
{
    (void) proc;
    (void) job_info;
    (void) apps;
    (void) napps;

    spawn_fired = true;
    spawn_ninfo = ninfo;
    /* the namespace of the job we "started" - the only channel for it is
     * this callback, which is why PMIX_OPERATION_SUCCEEDED is not an
     * answer a host may give here */
    if (NULL != cbfunc) {
        cbfunc(PMIX_SUCCESS, (char *) OPSUT_CHILD, cbdata);
    }
    return PMIX_SUCCESS;
}

/* the completion the switchyard would supply for a spawn - it owns the
 * server caddy on this path, so give it back */
static void spawn_done(pmix_status_t status, char nspace[], void *cbdata)
{
    pmix_server_caddy_t *cd = (pmix_server_caddy_t *) cbdata;

    (void) status;
    (void) nspace;

    if (NULL != cd) {
        PMIX_RELEASE(cd);
    }
}

/* ------------------------------------------------------------------ */
/* driving the handlers on the progress thread                        */
/* ------------------------------------------------------------------ */

typedef enum {
    OPSUT_ABORT,
    OPSUT_PUBLISH,
    OPSUT_LOOKUP,
    OPSUT_SPAWN
} opsut_cmd_t;

/* Carrier for driving a handler on the progress thread - every one of
 * them touches state the progress thread owns, so none may be called
 * from main(). The pmix_event_t is the thread-shift member required of
 * every caddy. */
typedef struct {
    pmix_event_t ev;
    pmix_lock_t lock;
    opsut_cmd_t cmd;
    size_t nprocs;      /* the count to put on the wire */
    size_t nprocs_real; /* how many procs actually to pack */
    size_t ninfo;       /* the count to put on the wire */
    size_t ninfo_real;  /* how many info structs actually to pack */
    bool as_tool;       /* present the requestor as a tool */
    pmix_status_t status;
} opsut_req_t;

static pmix_status_t pack_abort(pmix_buffer_t *buf, opsut_req_t *r)
{
    pmix_status_t rc, abstat = PMIX_ERR_JOB_ABORTED;
    pmix_proc_t proc;
    char *msg = (char *) OPSUT_ABORT_MSG;
    size_t n;

    PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, buf, &abstat, 1, PMIX_STATUS);
    if (PMIX_SUCCESS != rc) {
        return rc;
    }
    PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, buf, &msg, 1, PMIX_STRING);
    if (PMIX_SUCCESS != rc) {
        return rc;
    }
    /* the proc count and the procs may disagree on purpose - that is the
     * whole point of the malformed-count cases */
    PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, buf, &r->nprocs, 1, PMIX_SIZE);
    for (n = 0; PMIX_SUCCESS == rc && n < r->nprocs_real; n++) {
        PMIX_LOAD_PROCID(&proc, pmix_globals.myid.nspace, (pmix_rank_t) n);
        PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, buf, &proc, 1, PMIX_PROC);
    }
    return rc;
}

/* publish and lookup share a preamble: the effective user id, whose
 * value the server must discard in favor of the one the handshake
 * established */
static pmix_status_t pack_uid(pmix_buffer_t *buf)
{
    pmix_status_t rc;
    uint32_t uid = OPSUT_CLAIMED_UID;

    PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, buf, &uid, 1, PMIX_UINT32);
    return rc;
}

static pmix_status_t pack_publish(pmix_buffer_t *buf, opsut_req_t *r)
{
    pmix_status_t rc;
    pmix_info_t info;
    size_t n;

    rc = pack_uid(buf);
    if (PMIX_SUCCESS != rc) {
        return rc;
    }
    PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, buf, &r->ninfo, 1, PMIX_SIZE);
    for (n = 0; PMIX_SUCCESS == rc && n < r->ninfo_real; n++) {
        PMIX_INFO_LOAD(&info, "server-ops-ut.key", "value", PMIX_STRING);
        PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, buf, &info, 1, PMIX_INFO);
        PMIX_INFO_DESTRUCT(&info);
    }
    return rc;
}

static pmix_status_t pack_lookup(pmix_buffer_t *buf, opsut_req_t *r)
{
    pmix_status_t rc;
    char *key = (char *) "server-ops-ut.key";
    size_t nkeys = 1;

    rc = pack_uid(buf);
    if (PMIX_SUCCESS != rc) {
        return rc;
    }
    PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, buf, &nkeys, 1, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        return rc;
    }
    PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, buf, &key, 1, PMIX_STRING);
    if (PMIX_SUCCESS != rc) {
        return rc;
    }
    PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, buf, &r->ninfo, 1, PMIX_SIZE);
    return rc;
}

static pmix_status_t pack_spawn(pmix_buffer_t *buf, opsut_req_t *r)
{
    pmix_status_t rc;
    pmix_app_t app;
    size_t napps = 1;

    /* the job-level directive count - zero here, which is the case under
     * test: PMIx_Spawn(NULL, 0, ...) is a legal request */
    PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, buf, &r->ninfo, 1, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        return rc;
    }
    PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, buf, &napps, 1, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        return rc;
    }
    PMIX_APP_CONSTRUCT(&app);
    app.cmd = strdup("/bin/true");
    rc = PMIx_Argv_append_nosize(&app.argv, "true");
    if (PMIX_SUCCESS == rc) {
        app.maxprocs = 1;
        PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, buf, &app, 1, PMIX_APP);
    }
    PMIX_APP_DESTRUCT(&app);
    return rc;
}

static void do_cmd(int sd, short args, void *cbdata)
{
    opsut_req_t *r = (opsut_req_t *) cbdata;
    pmix_buffer_t buf;
    pmix_server_caddy_t *cd;
    pmix_status_t rc;
    pmix_proc_type_t saved;

    (void) sd;
    (void) args;

    PMIX_CONSTRUCT(&buf, pmix_buffer_t);
    PMIX_BFROPS_ASSIGN_TYPE(pmix_globals.mypeer, &buf);

    switch (r->cmd) {
    case OPSUT_ABORT:
        rc = pack_abort(&buf, r);
        break;
    case OPSUT_PUBLISH:
        rc = pack_publish(&buf, r);
        break;
    case OPSUT_LOOKUP:
        rc = pack_lookup(&buf, r);
        break;
    default:
        rc = pack_spawn(&buf, r);
        break;
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

    /* A tool is not a member of a job, so it has no parent to inherit
     * output forwarding from and keeps its own "forward everything to me"
     * default - which is the branch of pmix_server_spawn_parser under
     * test. This process is a server, so borrow the flag for the call. */
    saved = pmix_globals.mypeer->proc_type;
    if (r->as_tool) {
        pmix_globals.mypeer->proc_type.type |= PMIX_PROC_TOOL;
    }

    switch (r->cmd) {
    case OPSUT_ABORT:
        rc = pmix_server_abort(cd->peer, &buf, NULL, cd);
        break;
    case OPSUT_PUBLISH:
        rc = pmix_server_publish(cd->peer, &buf, NULL, cd);
        break;
    case OPSUT_LOOKUP:
        rc = pmix_server_lookup(cd->peer, &buf, NULL, cd);
        break;
    default:
        rc = pmix_server_spawn(cd->peer, &buf, spawn_done, cd);
        break;
    }
    pmix_globals.mypeer->proc_type = saved;

    if (PMIX_SUCCESS != rc || OPSUT_SPAWN != r->cmd) {
        /* the switchyard owns the caddy on a non-success return, and the
         * three status-only commands were given no completion of their
         * own above, so nothing else will let go of it */
        PMIX_RELEASE(cd);
    }
    PMIX_DESTRUCT(&buf);
    r->status = rc;
    PMIX_WAKEUP_THREAD(&r->lock);
}

static pmix_status_t drive(opsut_cmd_t cmd, size_t nprocs, size_t nprocs_real,
                           size_t ninfo, size_t ninfo_real, bool as_tool)
{
    opsut_req_t req;
    pmix_status_t rc;

    memset(&req, 0, sizeof(req));
    PMIX_CONSTRUCT_LOCK(&req.lock);
    req.cmd = cmd;
    req.nprocs = nprocs;
    req.nprocs_real = nprocs_real;
    req.ninfo = ninfo;
    req.ninfo_real = ninfo_real;
    req.as_tool = as_tool;
    PMIX_THREADSHIFT(&req, do_cmd);
    PMIX_WAIT_THREAD(&req.lock);
    rc = req.status;
    PMIX_DESTRUCT_LOCK(&req.lock);
    return rc;
}

/* The spawn completion thread-shifts, so let anything queued behind us
 * run before we look at the IOF request table. PMIx_Store_internal
 * blocks on the progress thread, so anything queued ahead of it has run
 * by the time it returns. */
static void progress_barrier(void)
{
    pmix_value_t v;

    PMIX_VALUE_LOAD(&v, "barrier", PMIX_STRING);
    PMIx_Store_internal(&pmix_globals.myid, "server-ops-ut.barrier", &v);
    PMIX_VALUE_DESTRUCT(&v);
}

/* Does anything forward the child job's output, and on which channels? */
static pmix_iof_channel_t child_iof_channels(void)
{
    pmix_iof_req_t *req;
    pmix_iof_channel_t channels = PMIX_FWD_NO_CHANNELS;
    int n;

    progress_barrier();
    for (n = 0; n < pmix_globals.iof_requests.size; n++) {
        req = (pmix_iof_req_t *) pmix_pointer_array_get_item(&pmix_globals.iof_requests, n);
        if (NULL == req || NULL == req->procs || 0 == req->nprocs) {
            continue;
        }
        if (0 == strncmp(req->procs[0].nspace, OPSUT_CHILD, PMIX_MAX_NSLEN)) {
            channels |= req->channels;
        }
    }
    return channels;
}

int main(int argc, char **argv)
{
    static pmix_server_module_t mymodule = {0};
    pmix_iof_channel_t channels;
    pmix_status_t rc;
    uint32_t myuid;

    (void) argc;
    (void) argv;

    fprintf(stdout, "server_ops: classic server command unit tests\n");

    mymodule.abort = stub_abort;
    mymodule.publish = stub_publish;
    mymodule.lookup = stub_lookup;
    mymodule.spawn = stub_spawn;

    rc = PMIx_server_init(&mymodule, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "PMIx_server_init failed: %s\n", PMIx_Error_string(rc));
        return 1;
    }

    /* --- an abort proc count that cannot survive the round trip --- *
     * PMIx_Proc_create computes "n * sizeof(pmix_proc_t)" with no
     * overflow guard and then constructs every one of the n elements, so
     * a count large enough to wrap that product yields a short allocation
     * whose constructor loop runs straight off the end - and scaddes then
     * walks the same size_t at PMIX_PROC_FREE. Against an unfixed library
     * this case does not fail, it smashes the heap. */
    rc = drive(OPSUT_ABORT, SIZE_MAX, 0, 0, 0, false);
    report("abort rejects a proc count of SIZE_MAX", PMIX_ERR_BAD_PARAM == rc);
    report("SIZE_MAX proc count never reached the host", !abort_fired);

    rc = drive(OPSUT_ABORT, (size_t) 0x80000000ULL, 0, 0, 0, false);
    report("abort rejects a proc count that truncates negative",
           PMIX_ERR_BAD_PARAM == rc);

    /* --- a well-formed abort --- */
    abort_fired = false;
    rc = drive(OPSUT_ABORT, 2, 2, 0, 0, false);
    report("well-formed abort is accepted", PMIX_SUCCESS == rc);
    report("well-formed abort reached the host with its procs",
           abort_fired && 2 == abort_nprocs);
    report("well-formed abort carried its message",
           NULL != abort_msg && 0 == strcmp(OPSUT_ABORT_MSG, abort_msg));

    /* --- publish takes the requestor's identity from the connection --- *
     * The user id is half of the pair the host stores the published data
     * under and later decides access by. It arrives on the wire, but a
     * per-command claim is exactly what the connection handshake exists
     * to rule out: it refuses a peer claiming a uid other than the one it
     * was registered with, so peer->info holds the pair the host itself
     * vouched for. Against an unfixed library the seeded PMIX_USERID is
     * whatever the command asked for. */
    myuid = (uint32_t) pmix_globals.mypeer->info->uid;
    rc = drive(OPSUT_PUBLISH, 0, 0, 1, 1, false);
    report("publish is accepted", PMIX_SUCCESS == rc);
    report("publish seeded a user id", publish_fired && publish_uid_found);
    report("publish used the handshake's uid, not the command's",
           publish_uid == myuid && publish_uid != OPSUT_CLAIMED_UID);

    /* --- a lookup the host answers atomically --- *
     * PMIX_OPERATION_SUCCEEDED means "done, and I will not call your
     * callback" - but the callback is the only channel this operation's
     * result has. Left alone, the client is sent a synthesized
     * status-only reply and then reads the value count out of the bytes
     * that are not there, so a lookup its host reported as complete comes
     * back as an unpack error. */
    rc = drive(OPSUT_LOOKUP, 0, 0, 0, 0, false);
    report("lookup refuses an atomic completion", PMIX_ERR_NOT_SUPPORTED == rc);
    report("the lookup did reach the host", lookup_fired);

    /* --- a spawn carrying no job-level directives --- *
     * PMIx_Spawn(NULL, 0, apps, napps, ...) is a legal request and
     * reaches us with a wire ninfo of zero. Naming no output channel is
     * precisely what selects the defaults pmix_server_spawn_parser
     * exists to apply, so skipping the parse for want of directives left
     * cd->channels and cd->inherit_iof at what the caddy constructor gave
     * them - forward nothing, inherit nothing - and a tool such as prun
     * never saw a line of its job's output. */
    rc = drive(OPSUT_SPAWN, 0, 0, 0, 0, true);
    report("a spawn with no directives is accepted", PMIX_SUCCESS == rc);
    report("it reached the host with no directives", spawn_fired && 0 == spawn_ninfo);
    channels = child_iof_channels();
    report("a tool's child job has its stdout forwarded",
           0 != (channels & PMIX_FWD_STDOUT_CHANNEL));
    report("a tool's child job has its stderr forwarded",
           0 != (channels & PMIX_FWD_STDERR_CHANNEL));
    report("a tool's child job has its stddiag forwarded",
           0 != (channels & PMIX_FWD_STDDIAG_CHANNEL));

    if (NULL != abort_msg) {
        free(abort_msg);
        abort_msg = NULL;
    }
    PMIx_server_finalize();

    fprintf(stdout, "server_ops: %d passed, %d failed\n", npass, nfail);
    return (0 == nfail) ? 0 : 1;
}
