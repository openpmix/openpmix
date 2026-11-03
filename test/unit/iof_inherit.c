/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * Copyright (c) 2026      Barcelona Supercomputing Center (BSC-CNS).
 *                         All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/*
 * A spawned job inherits its parent's output forwarding.
 *
 * Until this existed, pmix_server_spawn_parser() defaulted the output
 * channels on only when the spawn came from a TOOL. A spawn issued by an
 * ordinary client - an MPI application calling MPI_Comm_spawn is the case
 * that matters - therefore subscribed to nothing, and the child job's
 * output arrived at the server, matched no request, and was cached until
 * it aged out of the cache. Nobody ever saw it. See openpmix#4120.
 *
 * The fix is not to forward the child's output to the spawning client -
 * a client has nowhere to put it - but to give the child whatever the
 * parent job already has. "The parent's settings" are not stored
 * anywhere as settings; what exists is the set of live subscriptions
 * covering the spawning process's job, which is the same thing seen from
 * the other end: whoever receives the parent's output should receive the
 * child's. So the requests are cloned onto the child's namespace.
 *
 * The ordering under test is: a channel named on the spawn request wins,
 * otherwise the parent's subscriptions are inherited. There is no third
 * level: if nothing is watching the parent there is nowhere for the
 * child's output to go, and forwarding it to the spawner would be a
 * loopback rather than a fallback.
 *
 * This drives pmix_server_process_iof() and pmix_server_spawn_parser()
 * directly and reads pmix_globals.iof_requests back, rather than moving
 * real output - what is under test is which subscriptions get created
 * and for whom, and no spawn is needed to establish that. (Nor could one
 * be: test/simple/simptest cannot host a spawn - see src/server/AGENTS.md.)
 *
 * A subscription follows into the child job only if its requester may
 * access that job - see docs/hosts/access-control.rst. The watchers built here
 * are root unless a case says otherwise, so only the access cases turn
 * on who they are.
 */

#include "src/include/pmix_config.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "include/pmix.h"
#include "include/pmix_server.h"
#include "include/pmix_tool.h"

#include "src/common/pmix_iof.h"
#include "src/include/pmix_globals.h"
#include "src/mca/ptl/base/base.h"
#include "src/mca/ptl/ptl_types.h"
#include "src/server/pmix_server_ops.h"

static int failures = 0;

static void report(const char *what, bool pass)
{
    printf("%-64s : %s\n", what, pass ? "PASS" : "FAIL");
    fflush(stdout);
    if (!pass) {
        ++failures;
    }
}

/* Build a peer standing in for a process in the given namespace. Only
 * the identity and the role matter, so it has no bfrops module and
 * nothing may pack a message for it. A case that delivers output to a
 * watcher uses mkcounter() below. */
static pmix_peer_t *mkpeer(const char *nspace, pmix_rank_t rank, bool tool)
{
    pmix_peer_t *p;

    p = PMIX_NEW(pmix_peer_t);
    p->info = PMIX_NEW(pmix_rank_info_t);
    p->info->pname.nspace = strdup(nspace);
    p->info->pname.rank = rank;
    if (tool) {
        p->proc_type.type |= PMIX_PROC_TOOL;
    }
    return p;
}

/* A watcher that output is actually sent to, so a case can count what it
 * receives. It packs with our own namespace's bfrops. Its descriptor is
 * /dev/null, so a send gets past the "no connection" test, and its send
 * event is marked active, so nothing tries to arm it: each message comes
 * to rest on send_msg or send_queue, where count_sent() reads it. */
static pmix_peer_t *mkcounter(const char *nspace)
{
    pmix_peer_t *p = mkpeer(nspace, 0, true);

    PMIX_RETAIN(pmix_globals.mypeer->nptr);
    p->nptr = pmix_globals.mypeer->nptr;
    p->sd = open("/dev/null", O_WRONLY);
    p->send_ev_active = true;
    return p;
}

/* Release a counter. Its send event was never added, so the destructor
 * must not delete it - whichever release turns out to be the last. */
static void release_counter(pmix_peer_t *p)
{
    p->send_ev_active = false;
    PMIX_RELEASE(p);
}

/* Messages sent to a counter: the first rests on send_msg, the rest
 * queue behind it */
static size_t count_sent(pmix_peer_t *p)
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

/* A send is thread-shifted once more after the delivery that made it
 * returns. PMIx_Store_internal blocks on the progress thread, so all
 * that was queued ahead of it has run by the time it returns. */
static void progress_barrier(void)
{
    pmix_value_t v;

    PMIX_VALUE_LOAD(&v, "barrier", PMIX_STRING);
    PMIx_Store_internal(&pmix_globals.myid, "iofinh-ut.barrier", &v);
    PMIX_VALUE_DESTRUCT(&v);
}

/* Register a subscription covering an entire job, as a tool's spawn
 * does, and hand back the index so it can be removed again. */
static int watch_job(pmix_peer_t *requestor, const char *nspace,
                     pmix_iof_channel_t channels, size_t remote_id)
{
    pmix_iof_req_t *req;

    req = PMIX_NEW(pmix_iof_req_t);
    PMIX_RETAIN(requestor);
    req->requestor = requestor;
    req->nprocs = 1;
    PMIX_PROC_CREATE(req->procs, 1);
    PMIX_LOAD_PROCID(&req->procs[0], nspace, PMIX_RANK_WILDCARD);
    req->channels = channels;
    req->remote_id = remote_id;
    req->local_id = pmix_pointer_array_add(&pmix_globals.iof_requests, req);
    return (int) req->local_id;
}

/* Find the one subscription covering the given namespace. Returns NULL
 * if there is none, and sets *count to how many there were, so a test
 * can tell "none" from "more than we expected". */
static pmix_iof_req_t *find_watch(const char *nspace, int *count)
{
    pmix_iof_req_t *req, *found = NULL;
    int i;

    *count = 0;
    for (i = 0; i < pmix_globals.iof_requests.size; i++) {
        req = (pmix_iof_req_t *) pmix_pointer_array_get_item(&pmix_globals.iof_requests, i);
        if (NULL == req || 0 == req->nprocs) {
            continue;
        }
        if (PMIX_CHECK_NSPACE(req->procs[0].nspace, nspace)) {
            ++(*count);
            if (NULL == found) {
                found = req;
            }
        }
    }
    return found;
}

/* Drop every subscription naming a namespace, so each case starts clean */
static void drop_watches(const char *nspace)
{
    pmix_iof_req_t *req;
    int i;

    for (i = 0; i < pmix_globals.iof_requests.size; i++) {
        req = (pmix_iof_req_t *) pmix_pointer_array_get_item(&pmix_globals.iof_requests, i);
        if (NULL == req || 0 == req->nprocs) {
            continue;
        }
        if (PMIX_CHECK_NSPACE(req->procs[0].nspace, nspace)) {
            pmix_pointer_array_set_item(&pmix_globals.iof_requests, i, NULL);
            PMIX_RELEASE(req);
        }
    }
}

/* Run a spawn's IOF setup the way _spcbfunc does */
static pmix_status_t run_spawn_iof(pmix_peer_t *spawner, bool inherit,
                                   pmix_iof_channel_t channels,
                                   const char *child)
{
    pmix_setup_caddy_t *cd;
    pmix_status_t rc;

    cd = PMIX_NEW(pmix_setup_caddy_t);
    PMIX_RETAIN(spawner);
    cd->peer = spawner;
    cd->inherit_iof = inherit;
    cd->channels = channels;
    rc = pmix_server_process_iof(cd, (char *) child);
    PMIX_RELEASE(cd);
    return rc;
}

/* Register a namespace whose job data records who spawned it, which is
 * what a host supplies to every server the namespace is registered with
 * - including servers that had nothing to do with the spawn. */
static pmix_status_t register_child(const char *nspace, const char *parent_ns,
                                    pmix_rank_t parent_rank)
{
    pmix_info_t info[2];
    pmix_proc_t parent;
    pmix_nspace_t ns;
    pmix_status_t rc;
    uint32_t one = 1;

    /* a pmix_nspace_t, not the caller's string: this API takes a
     * fixed-size array, and gcc rejects a shorter literal outright under
     * -Werror=stringop-overread. Same reason as test/unit/resolve_api.c */
    PMIX_LOAD_NSPACE(ns, nspace);
    PMIX_LOAD_PROCID(&parent, parent_ns, parent_rank);
    PMIX_INFO_LOAD(&info[0], PMIX_PARENT_ID, &parent, PMIX_PROC);
    PMIX_INFO_LOAD(&info[1], PMIX_JOB_SIZE, &one, PMIX_UINT32);

    rc = PMIx_server_register_nspace(ns, 0, info, 2, NULL, NULL);
    if (PMIX_OPERATION_SUCCEEDED == rc) {
        rc = PMIX_SUCCESS;
    }
    PMIX_INFO_DESTRUCT(&info[0]);
    PMIX_INFO_DESTRUCT(&info[1]);
    return rc;
}

/* The same, plus the host's PMIX_IOF_INHERIT decision. Absence of that
 * attribute means "inherit", so a host only sends it to say no. */
static pmix_status_t register_child_noinherit(const char *nspace,
                                              const char *parent_ns,
                                              pmix_rank_t parent_rank)
{
    pmix_info_t info[3];
    pmix_proc_t parent;
    pmix_nspace_t ns;
    pmix_status_t rc;
    uint32_t one = 1;
    bool no = false;

    /* see register_child() above for why this is not the caller's string */
    PMIX_LOAD_NSPACE(ns, nspace);
    PMIX_LOAD_PROCID(&parent, parent_ns, parent_rank);
    PMIX_INFO_LOAD(&info[0], PMIX_PARENT_ID, &parent, PMIX_PROC);
    PMIX_INFO_LOAD(&info[1], PMIX_JOB_SIZE, &one, PMIX_UINT32);
    PMIX_INFO_LOAD(&info[2], PMIX_IOF_INHERIT, &no, PMIX_BOOL);

    rc = PMIx_server_register_nspace(ns, 0, info, 3, NULL, NULL);
    if (PMIX_OPERATION_SUCCEEDED == rc) {
        rc = PMIX_SUCCESS;
    }
    PMIX_INFO_DESTRUCT(&info[0]);
    PMIX_INFO_DESTRUCT(&info[1]);
    PMIX_INFO_DESTRUCT(&info[2]);
    return rc;
}

/* Like register_child(), plus extra job-level attributes: the spawn
 * directives a host passes through to the registration. */
static pmix_status_t register_child_info(const char *nspace,
                                         const char *parent_ns,
                                         pmix_rank_t parent_rank,
                                         const pmix_info_t *extra, size_t nextra)
{
    pmix_info_t *info;
    pmix_proc_t parent;
    pmix_nspace_t ns;
    pmix_status_t rc;
    uint32_t one = 1;
    size_t n;

    /* see register_child() above for why this is not the caller's string */
    PMIX_LOAD_NSPACE(ns, nspace);
    PMIX_LOAD_PROCID(&parent, parent_ns, parent_rank);
    PMIX_INFO_CREATE(info, 2 + nextra);
    PMIX_INFO_LOAD(&info[0], PMIX_PARENT_ID, &parent, PMIX_PROC);
    PMIX_INFO_LOAD(&info[1], PMIX_JOB_SIZE, &one, PMIX_UINT32);
    for (n = 0; n < nextra; n++) {
        PMIx_Info_xfer(&info[2 + n], &extra[n]);
    }

    rc = PMIx_server_register_nspace(ns, 0, info, 2 + nextra, NULL, NULL);
    if (PMIX_OPERATION_SUCCEEDED == rc) {
        rc = PMIX_SUCCESS;
    }
    PMIX_INFO_FREE(info, 2 + nextra);
    return rc;
}

static void test_spawn_parser(void)
{
    pmix_peer_t *client, *tool;
    pmix_iof_channel_t ch;
    pmix_iof_flags_t flags;
    pmix_info_t info;
    bool inherit;
    bool flag = false;

    client = mkpeer("parentjob", 0, false);
    tool = mkpeer("toolns", 0, true);

    /* a client that says nothing wants to inherit */
    pmix_iof_init_flags(&flags);
    inherit = false;
    pmix_server_spawn_parser(client, &ch, &flags, &inherit, NULL, 0);
    report("spawn_parser: silent client inherits",
           inherit && PMIX_FWD_NO_CHANNELS == ch);

    /* a tool that says nothing keeps its own default - it is not a member
     * of a job, so it has no parent to inherit from, and prun relies on
     * this being all channels */
    pmix_iof_init_flags(&flags);
    inherit = true;
    pmix_server_spawn_parser(tool, &ch, &flags, &inherit, NULL, 0);
    report("spawn_parser: silent tool defaults to all, does not inherit",
           !inherit
           && (PMIX_FWD_STDOUT_CHANNEL | PMIX_FWD_STDERR_CHANNEL
               | PMIX_FWD_STDDIAG_CHANNEL) == ch);

    /* naming a channel decides the matter - including naming it FALSE,
     * which is a request for silence and must not be overridden by an
     * inherited subscription */
    PMIX_INFO_LOAD(&info, PMIX_FWD_STDOUT, &flag, PMIX_BOOL);
    pmix_iof_init_flags(&flags);
    inherit = true;
    pmix_server_spawn_parser(client, &ch, &flags, &inherit, &info, 1);
    report("spawn_parser: an explicit 'false' suppresses inheritance",
           !inherit && PMIX_FWD_NO_CHANNELS == ch);
    PMIX_INFO_DESTRUCT(&info);

    flag = true;
    PMIX_INFO_LOAD(&info, PMIX_FWD_STDERR, &flag, PMIX_BOOL);
    pmix_iof_init_flags(&flags);
    inherit = true;
    pmix_server_spawn_parser(client, &ch, &flags, &inherit, &info, 1);
    report("spawn_parser: a named channel suppresses inheritance",
           !inherit && PMIX_FWD_STDERR_CHANNEL == ch);
    PMIX_INFO_DESTRUCT(&info);

    /* stdin is the other direction and says nothing about output */
    flag = true;
    PMIX_INFO_LOAD(&info, PMIX_FWD_STDIN, &flag, PMIX_BOOL);
    pmix_iof_init_flags(&flags);
    inherit = false;
    pmix_server_spawn_parser(client, &ch, &flags, &inherit, &info, 1);
    report("spawn_parser: FWD_STDIN alone still inherits output",
           inherit && PMIX_FWD_STDIN_CHANNEL == ch);
    PMIX_INFO_DESTRUCT(&info);

    PMIX_RELEASE(client);
    PMIX_RELEASE(tool);
}

/* A child owned by another user, naming allowed_uid in its access list
 * when that is not zero */
static pmix_status_t register_child_owned(const char *nspace, const char *parent_ns,
                                          uint32_t owner, uint32_t allowed_uid)
{
    pmix_info_t info[4], entry;
    pmix_data_array_t *ids, perms;
    pmix_proc_t parent;
    pmix_nspace_t ns;
    pmix_status_t rc;
    uint32_t one = 1;
    size_t n, ninfo = 3;

    /* see register_child() above for why this is not the caller's string */
    PMIX_LOAD_NSPACE(ns, nspace);
    PMIX_LOAD_PROCID(&parent, parent_ns, 0);
    PMIX_INFO_LOAD(&info[0], PMIX_PARENT_ID, &parent, PMIX_PROC);
    PMIX_INFO_LOAD(&info[1], PMIX_JOB_SIZE, &one, PMIX_UINT32);
    PMIX_INFO_LOAD(&info[2], PMIX_USERID, &owner, PMIX_UINT32);
    if (0 != allowed_uid) {
        PMIX_DATA_ARRAY_CREATE(ids, 1, PMIX_UINT32);
        ((uint32_t *) ids->array)[0] = allowed_uid;
        PMIX_INFO_LOAD(&entry, PMIX_ACCESS_USERIDS, ids, PMIX_DATA_ARRAY);
        PMIX_DATA_ARRAY_FREE(ids);
        perms.type = PMIX_INFO;
        perms.size = 1;
        perms.array = &entry;
        PMIX_INFO_LOAD(&info[3], PMIX_ACCESS_PERMISSIONS, &perms, PMIX_DATA_ARRAY);
        PMIX_INFO_DESTRUCT(&entry);
        ninfo = 4;
    }

    rc = PMIx_server_register_nspace(ns, 0, info, ninfo, NULL, NULL);
    if (PMIX_OPERATION_SUCCEEDED == rc) {
        rc = PMIX_SUCCESS;
    }
    for (n = 0; n < ninfo; n++) {
        PMIX_INFO_DESTRUCT(&info[n]);
    }
    return rc;
}

/* ids no real account should have */
#define INH_WATCHER_UID 4242
#define INH_OWNER_UID   5151

static void test_inherit_access(void)
{
    pmix_peer_t *watcher, *client;
    int count;

    watcher = mkpeer("acctoolns", 0, true);
    watcher->info->uid = INH_WATCHER_UID;
    watcher->info->gid = INH_WATCHER_UID;
    client = mkpeer("accparentjob", 0, false);
    watch_job(watcher, "accparentjob", PMIX_FWD_STDOUT_CHANNEL, 5);

    if (PMIX_SUCCESS != register_child_owned("accchild-private", "accparentjob",
                                             INH_OWNER_UID, 0) ||
        PMIX_SUCCESS != register_child_owned("accchild-shared", "accparentjob",
                                             INH_OWNER_UID, INH_WATCHER_UID)) {
        report("access: could not register the child namespaces", false);
        goto done;
    }

    run_spawn_iof(client, true, PMIX_FWD_NO_CHANNELS, "accchild-private");
    find_watch("accchild-private", &count);
    report("access: a watcher may not follow into another user's job", 0 == count);
    drop_watches("accchild-private");

    run_spawn_iof(client, true, PMIX_FWD_NO_CHANNELS, "accchild-shared");
    find_watch("accchild-shared", &count);
    report("access: a watcher follows into a job that names it", 1 == count);
    drop_watches("accchild-shared");

done:
    drop_watches("accparentjob");
    PMIX_RELEASE(watcher);
    PMIX_RELEASE(client);
}

static void test_inheritance(void)
{
    pmix_peer_t *watcher, *client;
    pmix_iof_req_t *req;
    int count;

    watcher = mkpeer("toolns", 0, true);
    client = mkpeer("parentjob", 0, false);

    /* the parent job is being watched by a tool, as it would be after
     * prun spawned it */
    watch_job(watcher, "parentjob",
              PMIX_FWD_STDOUT_CHANNEL | PMIX_FWD_STDERR_CHANNEL, 7);

    /* The host registers the spawned namespace with us; the spawn-time
     * path will not clone onto a namespace it has never been told about
     * (see below), so registering it is what makes these the ordinary
     * case rather than the deferral one. */
    if (PMIX_SUCCESS != register_child("childjob", "parentjob", 0)) {
        report("inherit: could not register the child namespace", false);
        goto done;
    }

    /* ---- the child inherits it ---- */
    run_spawn_iof(client, true, PMIX_FWD_NO_CHANNELS, "childjob");
    req = find_watch("childjob", &count);
    report("inherit: child job gets a subscription", NULL != req && 1 == count);
    if (NULL != req) {
        report("inherit: it goes to the parent's watcher, not the spawner",
               req->requestor == watcher);
        report("inherit: it carries the parent's channels",
               (PMIX_FWD_STDOUT_CHANNEL | PMIX_FWD_STDERR_CHANNEL) == req->channels);
        report("inherit: it keeps the watcher's handler id",
               7 == req->remote_id);
        report("inherit: it names the whole child job",
               PMIX_RANK_WILDCARD == req->procs[0].rank);
    }
    drop_watches("childjob");

    /* ---- an explicit request is not overridden ---- */
    run_spawn_iof(client, false, PMIX_FWD_STDDIAG_CHANNEL, "childjob");
    req = find_watch("childjob", &count);
    report("explicit: exactly one subscription", NULL != req && 1 == count);
    if (NULL != req) {
        report("explicit: it goes to the spawner that asked",
               req->requestor == client);
        report("explicit: it carries only what was asked for",
               PMIX_FWD_STDDIAG_CHANNEL == req->channels);
    }
    drop_watches("childjob");

    /* ---- nothing to inherit: nobody watches, and nobody may ----
     *
     * This is the case that must NOT fall back to forwarding the child's
     * output to the spawner. Output is never forwarded to an application
     * process: it would only come back out of that process's own stdout
     * for the runtime to pick up and forward again. */
    {
        pmix_peer_t *orphan = mkpeer("unwatchedjob", 0, false);

        run_spawn_iof(orphan, true, PMIX_FWD_NO_CHANNELS, "childjob");
        find_watch("childjob", &count);
        report("no parent: no subscription is created at all", 0 == count);
        drop_watches("childjob");
        PMIX_RELEASE(orphan);
    }

    /* ---- two watchers on the parent are both inherited ---- */
    {
        pmix_peer_t *watcher2 = mkpeer("toolns2", 0, true);

        watch_job(watcher2, "parentjob", PMIX_FWD_STDDIAG_CHANNEL, 3);
        run_spawn_iof(client, true, PMIX_FWD_NO_CHANNELS, "childjob");
        find_watch("childjob", &count);
        report("inherit: every watcher of the parent is carried over", 2 == count);
        drop_watches("childjob");
        PMIX_RELEASE(watcher2);
    }

    /* ---- a namespace we have not been told about is left alone ----
     *
     * The spawn completion can reach us before - or without - the host
     * registering the spawned namespace here: a daemon hosting none of
     * the child's processes may never register it at all. The host's
     * PMIX_IOF_INHERIT decision travels with that registration, so
     * cloning now would be deciding on the host's behalf with its answer
     * unread. Registration resolves it with a real answer; leave it to
     * that. */
    run_spawn_iof(client, true, PMIX_FWD_NO_CHANNELS, "unregisteredjob");
    find_watch("unregisteredjob", &count);
    report("inherit: an unregistered namespace defers to registration", 0 == count);
    drop_watches("unregisteredjob");

done:
    drop_watches("parentjob");
    PMIX_RELEASE(watcher);
    PMIX_RELEASE(client);
}

/* Hand the server a chunk of output from a namespace, as a host does.
 *
 * The channel is deliberately one the watcher did NOT subscribe to. What
 * is under test is the ANCESTRY decision - whether a subscription gets
 * cloned onto this namespace at all - and stopping short of the actual
 * forwarding is what lets these peers stay the identity-only stubs the
 * rest of this file builds: pmix_iof_process_iof() returns at its
 * channel test, before it reaches the pack that would need a bfrops
 * module and a socket. Do not "fix" the mismatch.
 */
static void deliver_from(const char *nspace, pmix_rank_t rank)
{
    pmix_proc_t source;
    pmix_byte_object_t bo;
    pmix_info_t local;
    char text[] = "output\n";
    bool flag = false;

    PMIX_LOAD_PROCID(&source, nspace, rank);
    bo.bytes = text;
    bo.size = strlen(text);
    /* not ours to emit - keeps the test's own stdout clean */
    PMIX_INFO_LOAD(&local, PMIX_IOF_LOCAL_OUTPUT, &flag, PMIX_BOOL);

    PMIx_server_IOF_deliver(&source, PMIX_FWD_STDOUT_CHANNEL, &bo, &local, 1, NULL, NULL);

    PMIX_INFO_DESTRUCT(&local);
}

/* The same on a given channel. Only for cases whose watchers are all
 * counters: a chunk a subscription takes is packed for its requestor. */
static void deliver_on(const char *nspace, pmix_rank_t rank, pmix_iof_channel_t channel)
{
    pmix_proc_t source;
    pmix_byte_object_t bo;
    pmix_info_t local;
    char text[] = "output\n";
    bool flag = false;

    PMIX_LOAD_PROCID(&source, nspace, rank);
    bo.bytes = text;
    bo.size = strlen(text);
    PMIX_INFO_LOAD(&local, PMIX_IOF_LOCAL_OUTPUT, &flag, PMIX_BOOL);

    PMIx_server_IOF_deliver(&source, channel, &bo, &local, 1, NULL, NULL);
    progress_barrier();

    PMIX_INFO_DESTRUCT(&local);
}

/* the host's half of a deregistration: done at once */
static pmix_status_t stub_iof_pull(const pmix_proc_t procs[], size_t nprocs,
                                   const pmix_info_t directives[], size_t ndirs,
                                   pmix_iof_channel_t channels,
                                   pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    PMIX_HIDE_UNUSED_PARAMS(procs, nprocs, directives, ndirs, channels, cbfunc, cbdata);
    return PMIX_OPERATION_SUCCEEDED;
}

/* pmix_server_iofdereg answers through this with its own caddy */
static void dereg_done(pmix_status_t status, void *cbdata)
{
    pmix_setup_caddy_t *cd = (pmix_setup_caddy_t *) cbdata;

    PMIX_HIDE_UNUSED_PARAMS(status);
    PMIX_RELEASE(cd);
}

/* A subscription follows into a job once, whichever generation it came
 * through, and its clones go when it is deregistered.
 *
 * Registering linB gives it a clone of the watcher's subscription on
 * linA. linC reaches that subscription twice: through linB's clone and
 * from linA directly, and its spawn completing here offers linB's clone
 * once more. All three descend from the same original. */
static void test_clone_lineage(void)
{
    pmix_peer_t *watcher, *spawner;
    pmix_iof_req_t *clone;
    pmix_buffer_t *buf;
    size_t ninfo = 0, refid, cloneid;
    pmix_status_t rc;
    int count;

    watcher = mkpeer("toolns", 0, true);
    PMIX_RETAIN(pmix_globals.mypeer->nptr);
    watcher->nptr = pmix_globals.mypeer->nptr;
    refid = (size_t) watch_job(watcher, "linA", PMIX_FWD_STDERR_CHANNEL, 21);
    if (PMIX_SUCCESS != register_child("linB", "linA", 0) ||
        PMIX_SUCCESS != register_child("linC", "linB", 0)) {
        report("lineage: could not register the namespaces", false);
        goto cleanup;
    }

    spawner = mkpeer("linB", 0, false);
    run_spawn_iof(spawner, true, PMIX_FWD_NO_CHANNELS, "linC");
    PMIX_RELEASE(spawner);
    clone = find_watch("linC", &count);
    report("lineage: a clone through another generation is not duplicated", 1 == count);

    /* a refid naming a clone's slot is not the requestor's to deregister */
    if (NULL != clone) {
        cloneid = clone->local_id;
        buf = PMIX_NEW(pmix_buffer_t);
        PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, buf, &ninfo, 1, PMIX_SIZE);
        if (PMIX_SUCCESS == rc) {
            PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, buf, &cloneid, 1, PMIX_SIZE);
        }
        if (PMIX_SUCCESS == rc) {
            rc = pmix_server_iofdereg(watcher, buf, dereg_done, NULL);
        }
        PMIX_RELEASE(buf);
        report("lineage: deregistering a clone's slot is refused", PMIX_ERR_NOT_FOUND == rc);
        find_watch("linC", &count);
        report("lineage: and the clone stays", 1 == count);
    }

    buf = PMIX_NEW(pmix_buffer_t);
    PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, buf, &ninfo, 1, PMIX_SIZE);
    if (PMIX_SUCCESS == rc) {
        PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, buf, &refid, 1, PMIX_SIZE);
    }
    if (PMIX_SUCCESS == rc) {
        rc = pmix_server_iofdereg(watcher, buf, dereg_done, NULL);
    }
    PMIX_RELEASE(buf);
    report("lineage: the original subscription is deregistered", PMIX_SUCCESS == rc);
    find_watch("linB", &count);
    report("lineage: its clone on the child goes with it", 0 == count);
    find_watch("linC", &count);
    report("lineage: and its clone on the grandchild", 0 == count);

cleanup:
    drop_watches("linC");
    drop_watches("linB");
    drop_watches("linA");
    PMIX_RELEASE(watcher);
}

/* A deregistered job's clones go with it: nothing can be forwarded for a
 * job that is gone, and a long-lived tool would otherwise collect one
 * clone per watcher for every job ever spawned under it. */
static void test_job_gone(void)
{
    pmix_peer_t *watcher;
    pmix_nspace_t ns;
    int count;

    watcher = mkpeer("toolgone", 0, true);
    watch_job(watcher, "goneP", PMIX_FWD_STDERR_CHANNEL, 61);
    if (PMIX_SUCCESS != register_child("goneC", "goneP", 0)) {
        report("gone: could not register the namespace", false);
    } else {
        find_watch("goneC", &count);
        report("gone: the child has its clone", 1 == count);
        PMIX_LOAD_NSPACE(ns, "goneC");
        PMIx_server_deregister_nspace(ns, NULL, NULL);
        find_watch("goneC", &count);
        report("gone: deregistering the child removes it", 0 == count);
    }
    drop_watches("goneC");
    drop_watches("goneP");
    PMIX_RELEASE(watcher);
}

/* One pull naming both the grandparent and the grandchild. The parent
 * gets a clone of it; that clone must not be cloned onto the grandchild,
 * which the original already covers. */
static void test_original_covers(void)
{
    pmix_peer_t *watcher, *spawner;
    pmix_iof_req_t *req;
    int idx, count;

    watcher = mkpeer("toolboth", 0, true);
    req = PMIX_NEW(pmix_iof_req_t);
    PMIX_RETAIN(watcher);
    req->requestor = watcher;
    req->nprocs = 2;
    PMIX_PROC_CREATE(req->procs, 2);
    PMIX_LOAD_PROCID(&req->procs[0], "bothG", PMIX_RANK_WILDCARD);
    PMIX_LOAD_PROCID(&req->procs[1], "bothC", PMIX_RANK_WILDCARD);
    req->channels = PMIX_FWD_STDERR_CHANNEL;
    req->remote_id = 91;
    idx = pmix_pointer_array_add(&pmix_globals.iof_requests, req);
    req->local_id = (size_t) idx;
    if (PMIX_SUCCESS != register_child("bothP", "bothG", 0) ||
        PMIX_SUCCESS != register_child("bothC", "bothP", 0)) {
        report("both: could not register the namespaces", false);
    } else {
        spawner = mkpeer("bothG", 0, false);
        run_spawn_iof(spawner, true, PMIX_FWD_NO_CHANNELS, "bothP");
        PMIX_RELEASE(spawner);
        spawner = mkpeer("bothP", 0, false);
        run_spawn_iof(spawner, true, PMIX_FWD_NO_CHANNELS, "bothC");
        PMIX_RELEASE(spawner);
        find_watch("bothC", &count);
        report("both: the grandchild the pull names gets no clone", 0 == count);
    }
    drop_watches("bothC");
    drop_watches("bothP");
    pmix_pointer_array_set_item(&pmix_globals.iof_requests, idx, NULL);
    PMIX_RELEASE(req);
    PMIX_RELEASE(watcher);
}

/* A job inherits from every ancestor, whatever the order.
 *
 * One tool watches colB, another colA, on the same channel. colC is
 * treated as colB is, and colB inherits colA's watcher - so colC gets
 * both, whether its registration or its spawn completing comes first. */
static void test_walk_collects(void)
{
    pmix_peer_t *near, *far, *spawner;
    int count, idx;

    near = mkpeer("toolnear", 0, true);
    far = mkpeer("toolfar", 0, true);
    watch_job(near, "colB", PMIX_FWD_STDERR_CHANNEL, 41);
    watch_job(far, "colA", PMIX_FWD_STDERR_CHANNEL, 42);
    spawner = mkpeer("colB", 0, false);
    if (PMIX_SUCCESS != register_child("colB", "colA", 0) ||
        PMIX_SUCCESS != register_child("colC", "colB", 0)) {
        report("collect: could not register the namespaces", false);
    } else {
        find_watch("colC", &count);
        report("collect: registration gives the grandchild both watchers", 2 == count);
        drop_watches("colC");
        run_spawn_iof(spawner, true, PMIX_FWD_NO_CHANNELS, "colC");
        (void) pmix_server_iof_resolve_job("colC");
        find_watch("colC", &count);
        report("collect: so does spawn completion first", 2 == count);
    }
    drop_watches("colC");
    drop_watches("colB");
    drop_watches("colA");

    /* a tool attaching later reaches the grandchild even when the
     * grandchild was registered before its parent */
    watch_job(near, "ordB", PMIX_FWD_STDERR_CHANNEL, 43);
    if (PMIX_SUCCESS != register_child("ordC", "ordB", 0) ||
        PMIX_SUCCESS != register_child("ordB", "ordA", 0)) {
        report("collect: could not register the reordered namespaces", false);
    } else {
        idx = watch_job(far, "ordA", PMIX_FWD_STDERR_CHANNEL, 44);
        pmix_server_iof_subscribed(pmix_pointer_array_get_item(&pmix_globals.iof_requests, idx));
        find_watch("ordC", &count);
        report("collect: a late tool reaches the grandchild in any order", 2 == count);
    }
    drop_watches("ordC");
    drop_watches("ordB");
    drop_watches("ordA");
    PMIX_RELEASE(spawner);
    PMIX_RELEASE(far);
    PMIX_RELEASE(near);
}

/* A job that does not inherit hides the watchers above it from its
 * children. refB refuses inheritance and has a watcher of its own; refC
 * gets that one and not refA's. So does refD, whose ancestry is copied
 * from refC's record. */
static void test_refusal_mid_chain(void)
{
    pmix_peer_t *near, *far;
    int count;

    near = mkpeer("toolrefb", 0, true);
    far = mkpeer("toolrefa", 0, true);
    watch_job(far, "refA", PMIX_FWD_STDERR_CHANNEL, 71);
    watch_job(near, "refB", PMIX_FWD_STDERR_CHANNEL, 72);
    if (PMIX_SUCCESS != register_child_noinherit("refB", "refA", 0) ||
        PMIX_SUCCESS != register_child("refC", "refB", 0)) {
        report("refuse: could not register the namespaces", false);
    } else {
        find_watch("refC", &count);
        report("refuse: the grandchild gets only its parent's watcher", 1 == count);
        if (PMIX_SUCCESS != register_child("refD", "refC", 0)) {
            report("refuse: could not register the last namespace", false);
        } else {
            find_watch("refD", &count);
            report("refuse: a copied ancestry stops there too", 1 == count);
        }
    }
    drop_watches("refD");
    drop_watches("refC");
    drop_watches("refB");
    drop_watches("refA");
    PMIX_RELEASE(far);
    PMIX_RELEASE(near);
}

/* A pull this server made for itself is delivered through its own
 * callback, and output is never forwarded to this server as a peer - so
 * there is nothing to clone onto its jobs' children. */
static void test_own_pull(void)
{
    pmix_iof_req_t *req;
    int idx, count;

    req = PMIX_NEW(pmix_iof_req_t);
    PMIX_RETAIN(pmix_globals.mypeer);
    req->requestor = pmix_globals.mypeer;
    req->nprocs = 1;
    PMIX_PROC_CREATE(req->procs, 1);
    PMIX_LOAD_PROCID(&req->procs[0], "ownP", PMIX_RANK_WILDCARD);
    req->channels = PMIX_FWD_STDERR_CHANNEL;
    idx = pmix_pointer_array_add(&pmix_globals.iof_requests, req);
    req->local_id = (size_t) idx;
    if (PMIX_SUCCESS != register_child("ownC", "ownP", 0)) {
        report("own pull: could not register the namespace", false);
    } else {
        find_watch("ownC", &count);
        report("own pull: this server's own pull is not cloned", 0 == count);
    }
    drop_watches("ownC");
    pmix_pointer_array_set_item(&pmix_globals.iof_requests, idx, NULL);
    PMIX_RELEASE(req);
}

/* Pre-register a namespace with PMIX_REGISTER_NODATA: the host will
 * send its job data later */
static pmix_status_t register_nodata(const char *nspace)
{
    pmix_info_t info;
    pmix_nspace_t ns;
    pmix_status_t rc;

    PMIX_LOAD_NSPACE(ns, nspace);
    PMIX_INFO_LOAD(&info, PMIX_REGISTER_NODATA, NULL, PMIX_BOOL);
    rc = PMIx_server_register_nspace(ns, 0, &info, 1, NULL, NULL);
    if (PMIX_OPERATION_SUCCEEDED == rc) {
        rc = PMIX_SUCCESS;
    }
    PMIX_INFO_DESTRUCT(&info);
    return rc;
}

/* A spawn that completes before the child's job data arrives leaves the
 * decision to the registration that brings it. Until then the child's
 * PMIX_IOF_INHERIT is unknown, and cloning would ignore a refusal the
 * data carries. */
static void test_spawn_before_data(void)
{
    pmix_peer_t *watcher, *spawner;
    int count;

    watcher = mkpeer("toolnd", 0, true);
    spawner = mkpeer("ndP", 0, false);
    watch_job(watcher, "ndP", PMIX_FWD_STDERR_CHANNEL, 161);
    if (PMIX_SUCCESS != register_nodata("ndRefused") ||
        PMIX_SUCCESS != register_nodata("ndAllowed")) {
        report("no data: could not pre-register the namespaces", false);
        goto cleanup;
    }

    run_spawn_iof(spawner, true, PMIX_FWD_NO_CHANNELS, "ndRefused");
    find_watch("ndRefused", &count);
    report("no data: a spawn completing first clones nothing", 0 == count);
    if (PMIX_SUCCESS != register_child_noinherit("ndRefused", "ndP", 0)) {
        report("no data: could not register the refused job's data", false);
    } else {
        find_watch("ndRefused", &count);
        report("no data: the refusal its data carries holds", 0 == count);
    }

    run_spawn_iof(spawner, true, PMIX_FWD_NO_CHANNELS, "ndAllowed");
    if (PMIX_SUCCESS != register_child("ndAllowed", "ndP", 0)) {
        report("no data: could not register the allowed job's data", false);
    } else {
        find_watch("ndAllowed", &count);
        report("no data: a job its data lets inherit inherits then", 1 == count);
    }

cleanup:
    drop_watches("ndRefused");
    drop_watches("ndAllowed");
    drop_watches("ndP");
    PMIX_RELEASE(spawner);
    PMIX_RELEASE(watcher);
}

/* The same when the job data comes as an update - a negative process
 * count and a PMIX_JOB_INFO_ARRAY - rather than a second registration */
static void test_data_by_update(void)
{
    pmix_peer_t *watcher;
    pmix_info_t info, *jinfo;
    pmix_data_array_t darray;
    pmix_proc_t parent;
    pmix_nspace_t ns;
    pmix_status_t rc;
    uint32_t one = 1;
    int count;

    watcher = mkpeer("toolupd", 0, true);
    watch_job(watcher, "updP", PMIX_FWD_STDERR_CHANNEL, 171);
    if (PMIX_SUCCESS != register_nodata("updC")) {
        report("update: could not pre-register the namespace", false);
        goto cleanup;
    }
    PMIX_LOAD_NSPACE(ns, "updC");
    PMIX_LOAD_PROCID(&parent, "updP", 0);
    PMIX_INFO_CREATE(jinfo, 3);
    PMIX_INFO_LOAD(&jinfo[0], PMIX_NSPACE, "updC", PMIX_STRING);
    PMIX_INFO_LOAD(&jinfo[1], PMIX_PARENT_ID, &parent, PMIX_PROC);
    PMIX_INFO_LOAD(&jinfo[2], PMIX_JOB_SIZE, &one, PMIX_UINT32);
    darray.type = PMIX_INFO;
    darray.size = 3;
    darray.array = jinfo;
    PMIX_INFO_LOAD(&info, PMIX_JOB_INFO_ARRAY, &darray, PMIX_DATA_ARRAY);
    PMIX_INFO_FREE(jinfo, 3);
    rc = PMIx_server_register_nspace(ns, -1, &info, 1, NULL, NULL);
    PMIX_INFO_DESTRUCT(&info);
    if (PMIX_SUCCESS != rc && PMIX_OPERATION_SUCCEEDED != rc) {
        report("update: could not send the job data as an update", false);
        goto cleanup;
    }
    find_watch("updC", &count);
    report("update: job data sent as an update lets the job inherit", 1 == count);

cleanup:
    drop_watches("updC");
    drop_watches("updP");
    PMIX_RELEASE(watcher);
}

/* A tool that attaches after a job was registered still reaches it: a
 * new subscription resolves the jobs below what it covers. */
static void test_late_subscriber(void)
{
    pmix_peer_t *watcher;
    int count, idx;

    watcher = mkpeer("toollate", 0, true);
    if (PMIX_SUCCESS != register_child("lateC", "lateP", 0)) {
        report("late: could not register the namespace", false);
    } else {
        deliver_from("lateC", 0);
        find_watch("lateC", &count);
        report("late: nobody watches the job yet", 0 == count);
        idx = watch_job(watcher, "lateP", PMIX_FWD_STDERR_CHANNEL, 81);
        pmix_server_iof_subscribed(pmix_pointer_array_get_item(&pmix_globals.iof_requests, idx));
        find_watch("lateC", &count);
        report("late: the new subscription reaches the job", 1 == count);
    }
    drop_watches("lateC");
    drop_watches("lateP");
    PMIX_RELEASE(watcher);
}

/* A job spawned before its parent's spawn completed here still reaches
 * the watcher that completion adds. ssJ2 is registered before the tool's
 * spawn of ssJ1 completes and adds the tool's subscription on ssJ1. */
static void test_spawn_completes_late(void)
{
    pmix_peer_t *tool;
    int count;

    tool = mkpeer("toolss", 0, true);
    if (PMIX_SUCCESS != register_child("ssJ1", "ssJ0", 0) ||
        PMIX_SUCCESS != register_child("ssJ2", "ssJ1", 0)) {
        report("spawnlate: could not register the namespaces", false);
    } else {
        find_watch("ssJ2", &count);
        report("spawnlate: nobody watches the grandchild yet", 0 == count);
        run_spawn_iof(tool, false, PMIX_FWD_STDERR_CHANNEL, "ssJ1");
        find_watch("ssJ2", &count);
        report("spawnlate: the spawner's subscription reaches the grandchild", 1 == count);
    }
    drop_watches("ssJ2");
    drop_watches("ssJ1");
    PMIX_RELEASE(tool);
}

/* Each handler of a tool gets each channel of a job once.
 *
 * Two pulls under two handlers each reach dupJ2, which descends from both.
 * Two pulls under one handler reach sameJ2 once; when the nearer one
 * goes, sameJ2 and sameJ1 are resolved again and the other takes over. */
static void test_one_copy_per_handler(void)
{
    pmix_peer_t *watcher;
    pmix_buffer_t *buf;
    pmix_iof_req_t *req;
    size_t ninfo = 0, refid;
    pmix_status_t rc;
    int count, idx;

    watcher = mkpeer("tooldup", 0, true);
    PMIX_RETAIN(pmix_globals.mypeer->nptr);
    watcher->nptr = pmix_globals.mypeer->nptr;
    watch_job(watcher, "dupJ0", PMIX_FWD_STDERR_CHANNEL, 101);
    watch_job(watcher, "dupJ1", PMIX_FWD_STDERR_CHANNEL, 102);
    idx = watch_job(watcher, "sameJ0", PMIX_FWD_STDERR_CHANNEL, 103);
    refid = (size_t) watch_job(watcher, "sameJ1", PMIX_FWD_STDERR_CHANNEL, 103);
    if (PMIX_SUCCESS != register_child("dupJ1", "dupJ0", 0) ||
        PMIX_SUCCESS != register_child("dupJ2", "dupJ1", 0) ||
        PMIX_SUCCESS != register_child("sameJ1", "sameJ0", 0) ||
        PMIX_SUCCESS != register_child("sameJ2", "sameJ1", 0)) {
        report("dup: could not register the namespaces", false);
        goto cleanup;
    }
    find_watch("dupJ2", &count);
    report("dup: two handlers each get the grandchild", 2 == count);
    find_watch("sameJ2", &count);
    report("dup: one handler gets it once", 1 == count);

    buf = PMIX_NEW(pmix_buffer_t);
    PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, buf, &ninfo, 1, PMIX_SIZE);
    if (PMIX_SUCCESS == rc) {
        PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, buf, &refid, 1, PMIX_SIZE);
    }
    if (PMIX_SUCCESS == rc) {
        rc = pmix_server_iofdereg(watcher, buf, dereg_done, NULL);
    }
    PMIX_RELEASE(buf);
    req = find_watch("sameJ2", &count);
    report("dup: after the nearer pull goes, the other still reaches it",
           PMIX_SUCCESS == rc && 1 == count && NULL != req &&
           ((pmix_iof_req_t *) pmix_pointer_array_get_item(&pmix_globals.iof_requests, idx))
                   ->clone_id == req->root_id);
    find_watch("sameJ1", &count);
    report("dup: and the job it named gets the other pull too", 1 == count);

cleanup:
    drop_watches("dupJ2");
    drop_watches("dupJ1");
    drop_watches("dupJ0");
    drop_watches("sameJ2");
    drop_watches("sameJ1");
    drop_watches("sameJ0");
    PMIX_RELEASE(watcher);
}

/* A tool attaching after a middle generation has been released still
 * reaches the job below it: the job recorded its ancestry when it was
 * registered. */
static void test_released_middle(void)
{
    pmix_peer_t *watcher;
    pmix_nspace_t ns;
    int idx, count;

    watcher = mkpeer("toolrel", 0, true);
    if (PMIX_SUCCESS != register_child("relJ1", "relJ0", 0) ||
        PMIX_SUCCESS != register_child("relJ2", "relJ1", 0)) {
        report("released: could not register the namespaces", false);
    } else {
        PMIX_LOAD_NSPACE(ns, "relJ1");
        PMIx_server_deregister_nspace(ns, NULL, NULL);
        idx = watch_job(watcher, "relJ0", PMIX_FWD_STDERR_CHANNEL, 111);
        pmix_server_iof_subscribed(pmix_pointer_array_get_item(&pmix_globals.iof_requests, idx));
        find_watch("relJ2", &count);
        report("released: a late tool reaches past a released middle job", 1 == count);
    }
    drop_watches("relJ2");
    drop_watches("relJ0");
    PMIX_RELEASE(watcher);
}

/* The same when the middle generation is released before the job below
 * it is registered, as when each generation of a spawn chain exits after
 * spawning the next. rbC recorded rbB and rbA while rbB was still here;
 * rbD takes them from rbC's record, since rbB's own data is gone. */
static void test_released_before_registration(void)
{
    pmix_peer_t *watcher;
    pmix_nspace_t ns;
    int idx, count;

    watcher = mkpeer("toolrb", 0, true);
    if (PMIX_SUCCESS != register_child("rbB", "rbA", 0) ||
        PMIX_SUCCESS != register_child("rbC", "rbB", 0)) {
        report("released early: could not register the namespaces", false);
        goto cleanup;
    }
    PMIX_LOAD_NSPACE(ns, "rbB");
    PMIx_server_deregister_nspace(ns, NULL, NULL);
    if (PMIX_SUCCESS != register_child("rbD", "rbC", 0)) {
        report("released early: could not register the last namespace", false);
        goto cleanup;
    }
    idx = watch_job(watcher, "rbA", PMIX_FWD_STDERR_CHANNEL, 131);
    pmix_server_iof_subscribed(pmix_pointer_array_get_item(&pmix_globals.iof_requests, idx));
    find_watch("rbC", &count);
    report("released early: a late tool reaches the job below the gap", 1 == count);
    find_watch("rbD", &count);
    report("released early: and the job registered after the gap", 1 == count);

cleanup:
    drop_watches("rbD");
    drop_watches("rbC");
    drop_watches("rbA");
    PMIX_RELEASE(watcher);
}

/* Output cached before a job is registered reaches every watcher the job
 * inherits at registration, from whichever ancestor.
 *
 * drnC descends from drnB, which descends from drnA. One tool watches
 * drnB; another watches drnA and may access drnC but not drnB, so drnB has
 * no clone of its subscription and drnC inherits it from drnA directly.
 * drnC's first line is cached before drnC is registered here. */
static void test_drain_all_ancestors(void)
{
    pmix_peer_t *near, *far;
    size_t ncached;

    near = mkcounter("tooldrnnear");
    far = mkcounter("tooldrnfar");
    far->info->uid = INH_WATCHER_UID;
    far->info->gid = INH_WATCHER_UID;
    watch_job(near, "drnB", PMIX_FWD_STDERR_CHANNEL, 121);
    watch_job(far, "drnA", PMIX_FWD_STDERR_CHANNEL, 122);
    if (PMIX_SUCCESS != register_child_owned("drnB", "drnA", INH_OWNER_UID, 0)) {
        report("drain: could not register the middle namespace", false);
        goto cleanup;
    }
    ncached = pmix_list_get_size(&pmix_server_globals.iof);
    deliver_on("drnC", 0, PMIX_FWD_STDERR_CHANNEL);
    report("drain: output from an unregistered job is cached",
           ncached + 1 == pmix_list_get_size(&pmix_server_globals.iof) &&
           0 == count_sent(near) && 0 == count_sent(far));
    if (PMIX_SUCCESS != register_child_owned("drnC", "drnB", INH_OWNER_UID, INH_WATCHER_UID)) {
        report("drain: could not register the namespace", false);
        goto cleanup;
    }
    progress_barrier();
    report("drain: the parent's watcher gets the cached line", 1 == count_sent(near));
    report("drain: so does the grandparent's", 1 == count_sent(far));
    report("drain: and the cache lets it go",
           ncached == pmix_list_get_size(&pmix_server_globals.iof));

cleanup:
    drop_watches("drnC");
    drop_watches("drnB");
    drop_watches("drnA");
    release_counter(far);
    release_counter(near);
}

/* A pull naming a whole job and one rank of its child gets each of the
 * child's lines once.
 *
 * The pull is cloned onto the child, because it names only rank 0 of it.
 * Rank 0's lines then match both the pull and its clone; the clone stands
 * aside for them. The first two lines are cached before the child is
 * registered and handed over when its clone is made, the rest arrive
 * after. */
static void test_rank_pull_overlap(void)
{
    pmix_peer_t *watcher;
    pmix_iof_req_t *req;
    size_t ncached;
    int idx, count;

    watcher = mkcounter("toolrk");
    ncached = pmix_list_get_size(&pmix_server_globals.iof);
    deliver_on("rkJ", 0, PMIX_FWD_STDERR_CHANNEL);
    deliver_on("rkJ", 1, PMIX_FWD_STDERR_CHANNEL);

    req = PMIX_NEW(pmix_iof_req_t);
    PMIX_RETAIN(watcher);
    req->requestor = watcher;
    req->nprocs = 2;
    PMIX_PROC_CREATE(req->procs, 2);
    PMIX_LOAD_PROCID(&req->procs[0], "rkP", PMIX_RANK_WILDCARD);
    PMIX_LOAD_PROCID(&req->procs[1], "rkJ", 0);
    req->channels = PMIX_FWD_STDERR_CHANNEL;
    req->remote_id = 141;
    idx = pmix_pointer_array_add(&pmix_globals.iof_requests, req);
    req->local_id = (size_t) idx;

    if (PMIX_SUCCESS != register_child("rkJ", "rkP", 0)) {
        report("rank pull: could not register the namespace", false);
        goto cleanup;
    }
    progress_barrier();
    find_watch("rkJ", &count);
    report("rank pull: the child gets a clone of the pull", 1 == count);
    report("rank pull: each cached line is handed over once",
           2 == count_sent(watcher) &&
           ncached == pmix_list_get_size(&pmix_server_globals.iof));
    deliver_on("rkJ", 0, PMIX_FWD_STDERR_CHANNEL);
    report("rank pull: a line from the pulled rank arrives once", 3 == count_sent(watcher));
    deliver_on("rkJ", 1, PMIX_FWD_STDERR_CHANNEL);
    report("rank pull: so does one from another rank", 4 == count_sent(watcher));

cleanup:
    drop_watches("rkJ");
    pmix_pointer_array_set_item(&pmix_globals.iof_requests, idx, NULL);
    PMIX_RELEASE(req);
    release_counter(watcher);
}

/* The same two generations down. gcC's clone is made from gcB's, and
 * whether it stands aside depends on the pull it descends from, which
 * names rank 0 of gcC. */
static void test_rank_pull_grandchild(void)
{
    pmix_peer_t *watcher;
    pmix_iof_req_t *req;
    int idx, count;

    watcher = mkcounter("toolgc");
    req = PMIX_NEW(pmix_iof_req_t);
    PMIX_RETAIN(watcher);
    req->requestor = watcher;
    req->nprocs = 2;
    PMIX_PROC_CREATE(req->procs, 2);
    PMIX_LOAD_PROCID(&req->procs[0], "gcA", PMIX_RANK_WILDCARD);
    PMIX_LOAD_PROCID(&req->procs[1], "gcC", 0);
    req->channels = PMIX_FWD_STDERR_CHANNEL;
    req->remote_id = 151;
    idx = pmix_pointer_array_add(&pmix_globals.iof_requests, req);
    req->local_id = (size_t) idx;

    if (PMIX_SUCCESS != register_child("gcB", "gcA", 0) ||
        PMIX_SUCCESS != register_child("gcC", "gcB", 0)) {
        report("rank pull: could not register the grandchild namespaces", false);
        goto cleanup;
    }
    find_watch("gcC", &count);
    report("rank pull: the grandchild gets one clone", 1 == count);
    deliver_on("gcC", 0, PMIX_FWD_STDERR_CHANNEL);
    report("rank pull: the grandchild's pulled rank arrives once", 1 == count_sent(watcher));
    deliver_on("gcC", 1, PMIX_FWD_STDERR_CHANNEL);
    report("rank pull: so does its other rank", 2 == count_sent(watcher));
    deliver_on("gcB", 0, PMIX_FWD_STDERR_CHANNEL);
    report("rank pull: the middle job, which the pull names no rank of, arrives once",
           3 == count_sent(watcher));

cleanup:
    drop_watches("gcC");
    drop_watches("gcB");
    pmix_pointer_array_set_item(&pmix_globals.iof_requests, idx, NULL);
    PMIX_RELEASE(req);
    release_counter(watcher);
}

/* A subscription that already covers the child itself is not cloned. A
 * pull naming every job covers the parent and the child alike. Run last:
 * while it exists it matches every job's output. */
static void test_covering_pull(void)
{
    pmix_peer_t *watcher, *spawner;
    pmix_iof_req_t *req;
    int idx, count;

    watcher = mkpeer("toolall", 0, true);
    idx = watch_job(watcher, "", PMIX_FWD_STDERR_CHANNEL, 51);
    if (PMIX_SUCCESS != register_child("covC", "covP", 0)) {
        report("cover: could not register the namespace", false);
    } else {
        spawner = mkpeer("covP", 0, false);
        run_spawn_iof(spawner, true, PMIX_FWD_NO_CHANNELS, "covC");
        PMIX_RELEASE(spawner);
        find_watch("covC", &count);
        report("cover: a pull covering every job is not cloned onto the child", 1 == count);
    }
    req = (pmix_iof_req_t *) pmix_pointer_array_get_item(&pmix_globals.iof_requests, idx);
    pmix_pointer_array_set_item(&pmix_globals.iof_requests, idx, NULL);
    PMIX_RELEASE(req);
    drop_watches("covC");
    PMIX_RELEASE(watcher);
}

/* Inheritance where the job is registered.
 *
 * The spawn-time clone above is made by the server that PROCESSED the
 * spawn. On a multi-node DVM that is not the server holding the watching
 * tool's subscription; that server learns of the child when the host
 * registers it there, with its PMIX_PARENT_ID, and resolves its
 * inheritance then.
 */
static void test_delivery_ancestry(void)
{
    pmix_peer_t *watcher, *spawner;
    pmix_iof_req_t *req;
    int count, idx;

    watcher = mkpeer("toolns", 0, true);

    /* somebody here watches the parent job's stderr, and nothing at all
     * names the child. The drop is not tidying: a childjob subscription
     * left behind by the spawn-time cases above would let every case
     * here pass without registration resolving anything. */
    drop_watches("childjob");
    watch_job(watcher, "parentjob", PMIX_FWD_STDERR_CHANNEL, 9);

    if (PMIX_SUCCESS != register_child("childjob", "parentjob", 0)) {
        report("ancestry: could not register the child namespace", false);
        goto cleanup;
    }

    /* ---- registering the child gives it its parent's watcher ---- */
    req = find_watch("childjob", &count);
    report("ancestry: registration creates the child's subscription",
           NULL != req && 1 == count);
    if (NULL != req) {
        report("ancestry: it goes to the ancestor's watcher",
               req->requestor == watcher);
        report("ancestry: it carries the ancestor's channels",
               PMIX_FWD_STDERR_CHANNEL == req->channels);
        report("ancestry: it keeps the watcher's handler id", 9 == req->remote_id);
    }

    /* ---- the two halves together clone once, in either order ----
     *
     * Registration and spawn completion both clone, in whatever order the
     * host runs them. A second clone forwarded every line twice, and a
     * spawn chain doubled it per generation, since each child copies all
     * of its parent's. */
    spawner = mkpeer("parentjob", 0, false);
    run_spawn_iof(spawner, true, PMIX_FWD_NO_CHANNELS, "childjob");
    find_watch("childjob", &count);
    report("ancestry: spawn-time after registration adds no second clone", 1 == count);
    drop_watches("childjob");
    run_spawn_iof(spawner, true, PMIX_FWD_NO_CHANNELS, "childjob");
    (void) pmix_server_iof_resolve_job("childjob");
    find_watch("childjob", &count);
    report("ancestry: registration after spawn-time adds no second clone", 1 == count);

    /* ---- a second subscription follows into the child too ----
     *
     * Same requestor and handler id, different channels: it is a distinct
     * subscription, and the child gets its own clone of it. Not stdout:
     * the cache holds stdout chunks from the cases above, and a clone
     * that matched them would try to forward them to these socketless
     * peers. */
    idx = watch_job(watcher, "parentjob", PMIX_FWD_STDDIAG_CHANNEL, 9);
    run_spawn_iof(spawner, true, PMIX_FWD_NO_CHANNELS, "childjob");
    find_watch("childjob", &count);
    report("ancestry: a subscription on other channels is cloned too", 2 == count);
    req = (pmix_iof_req_t *) pmix_pointer_array_get_item(&pmix_globals.iof_requests, idx);
    pmix_pointer_array_set_item(&pmix_globals.iof_requests, idx, NULL);
    PMIX_RELEASE(req);
    PMIX_RELEASE(spawner);

    /* ---- the walk is transitive ----
     *
     * A grandchild is watched too: "treated the way its parent is
     * treated" says nothing about depth. Note the child's own clone is
     * dropped first, so the grandchild's answer has to come from two
     * levels up rather than from the entry the case above created. */
    drop_watches("childjob");
    if (PMIX_SUCCESS != register_child("grandchild", "childjob", 0)) {
        report("ancestry: could not register the grandchild namespace", false);
        goto cleanup;
    }
    req = find_watch("grandchild", &count);
    report("ancestry: a grandchild inherits through an unwatched parent",
           NULL != req && 1 == count && (NULL == req || req->requestor == watcher));
    drop_watches("grandchild");

    /* ---- a job with no ancestry gets nothing ----
     *
     * There is nowhere for its output to go, and inventing a
     * subscription would be worse than caching it. */
    if (PMIX_SUCCESS != register_child("orphanjob", "unwatchedjob", 0)) {
        report("ancestry: could not register the orphan namespace", false);
        goto cleanup;
    }
    find_watch("orphanjob", &count);
    report("ancestry: an unwatched ancestry creates no subscription", 0 == count);

    /* ---- the host can turn inheritance off ----
     *
     * PMIX_IOF_INHERIT=false in the job data is how a host says a
     * spawned job is not to be treated the way its parent is - PRRTE's
     * "inherit" setting reaching us. The ancestry is intact and the
     * parent is watched, so only the attribute stands between this job
     * and a subscription. */
    if (PMIX_SUCCESS != register_child_noinherit("quietjob", "parentjob", 0)) {
        report("ancestry: could not register the quiet namespace", false);
        goto cleanup;
    }
    find_watch("quietjob", &count);
    report("ancestry: PMIX_IOF_INHERIT=false suppresses inheritance", 0 == count);

cleanup:
    drop_watches("quietjob");
    drop_watches("childjob");
    drop_watches("grandchild");
    drop_watches("orphanjob");
    drop_watches("parentjob");
    PMIX_RELEASE(watcher);
}

/* One spawn request, as the spawn server's parser sees it and as the
 * host passes it through to every other server. */
typedef struct {
    const char *name;
    bool tool;          // requested by a tool
    size_t ninfo;
    pmix_info_t info[2];
} agree_row_t;

#define AGREE_ROWS 13

static void build_rows(agree_row_t *rows)
{
    bool yes = true, no = false;
    int r = 0;

    memset(rows, 0, AGREE_ROWS * sizeof(agree_row_t));
    rows[r++].name = "nothing named";
    rows[r].name = "PMIX_FWD_STDOUT=false";
    PMIX_INFO_LOAD(&rows[r].info[0], PMIX_FWD_STDOUT, &no, PMIX_BOOL);
    rows[r++].ninfo = 1;
    rows[r].name = "PMIX_FWD_STDOUT=true";
    PMIX_INFO_LOAD(&rows[r].info[0], PMIX_FWD_STDOUT, &yes, PMIX_BOOL);
    rows[r++].ninfo = 1;
    rows[r].name = "PMIX_FWD_STDERR=true";
    PMIX_INFO_LOAD(&rows[r].info[0], PMIX_FWD_STDERR, &yes, PMIX_BOOL);
    rows[r++].ninfo = 1;
    rows[r].name = "PMIX_FWD_STDDIAG=false";
    PMIX_INFO_LOAD(&rows[r].info[0], PMIX_FWD_STDDIAG, &no, PMIX_BOOL);
    rows[r++].ninfo = 1;
    rows[r].name = "PMIX_FWD_STDOUT as a string";
    PMIX_INFO_LOAD(&rows[r].info[0], PMIX_FWD_STDOUT, "true", PMIX_STRING);
    rows[r++].ninfo = 1;
    rows[r].name = "PMIX_FWD_STDOUT with no value";
    PMIX_INFO_LOAD(&rows[r].info[0], PMIX_FWD_STDOUT, NULL, PMIX_UNDEF);
    rows[r++].ninfo = 1;
    rows[r].name = "only PMIX_FWD_STDIN";
    PMIX_INFO_LOAD(&rows[r].info[0], PMIX_FWD_STDIN, &yes, PMIX_BOOL);
    rows[r++].ninfo = 1;
    rows[r].name = "only a formatting directive";
    PMIX_INFO_LOAD(&rows[r].info[0], PMIX_IOF_TAG_OUTPUT, &yes, PMIX_BOOL);
    rows[r++].ninfo = 1;
    rows[r].name = "a tool, nothing named";
    rows[r++].tool = true;
    rows[r].name = "a tool, PMIX_FWD_STDOUT=false";
    rows[r].tool = true;
    PMIX_INFO_LOAD(&rows[r].info[0], PMIX_FWD_STDOUT, &no, PMIX_BOOL);
    rows[r++].ninfo = 1;
    /* a requester-supplied PMIX_IOF_INHERIT must not reach the host */
    rows[r].name = "a client's own PMIX_IOF_INHERIT=false, nothing named";
    PMIX_INFO_LOAD(&rows[r].info[0], PMIX_IOF_INHERIT, &no, PMIX_BOOL);
    rows[r++].ninfo = 1;
    rows[r].name = "a client's own PMIX_IOF_INHERIT=true, PMIX_FWD_STDOUT=false";
    PMIX_INFO_LOAD(&rows[r].info[0], PMIX_IOF_INHERIT, &yes, PMIX_BOOL);
    PMIX_INFO_LOAD(&rows[r].info[1], PMIX_FWD_STDOUT, &no, PMIX_BOOL);
    rows[r++].ninfo = 2;
}

/* The spawn server and every server the job is registered with agree on
 * every request. Each row runs a request through
 * pmix_server_spawn_prepare(), registers the result as a host does, and
 * asserts only that registration clones exactly when the parser
 * inherits. */
static void test_parser_agreement(void)
{
    agree_row_t rows[AGREE_ROWS];
    pmix_peer_t *watcher, *client, *tool, *spawner;
    pmix_setup_caddy_t *cd;
    pmix_nspace_t child;
    char what[160];
    bool parser_inherits;
    size_t n;
    int r, count;

    watcher = mkpeer("toolns", 0, true);
    client = mkpeer("agreejob", 0, false);
    tool = mkpeer("agreejob", 0, true);   // a tool carrying a job's name
    spawner = mkpeer("agreejob", 0, false);
    watch_job(watcher, "agreejob", PMIX_FWD_STDERR_CHANNEL, 11);
    build_rows(rows);

    for (r = 0; r < AGREE_ROWS; r++) {
        cd = PMIX_NEW(pmix_setup_caddy_t);
        cd->info = PMIx_Info_create(rows[r].ninfo);
        cd->ninfo = rows[r].ninfo;
        cd->copied = true;
        for (n = 0; n < rows[r].ninfo; n++) {
            PMIx_Info_xfer(&cd->info[n], &rows[r].info[n]);
        }
        if (PMIX_SUCCESS != pmix_server_spawn_prepare(rows[r].tool ? tool : client, cd, false)) {
            snprintf(what, sizeof(what), "agree: %s: could not prepare the request",
                     rows[r].name);
            report(what, false);
            PMIX_RELEASE(cd);
            for (n = 0; n < rows[r].ninfo; n++) {
                PMIX_INFO_DESTRUCT(&rows[r].info[n]);
            }
            continue;
        }
        parser_inherits = cd->inherit_iof;
        snprintf(child, sizeof(child), "agreechild%d", r);
        if (PMIX_SUCCESS != register_child_info(child, "agreejob", 0, cd->info, cd->ninfo)) {
            snprintf(what, sizeof(what), "agree: %s: could not register", rows[r].name);
            report(what, false);
        } else {
            find_watch(child, &count);
            snprintf(what, sizeof(what), "agree: %s: registration %s like the parser",
                     rows[r].name, parser_inherits ? "inherits" : "does not inherit");
            report(what, parser_inherits == (0 < count));
            drop_watches(child);

            /* the spawn-time half reads the same key; driven as if its
             * parser had inherited, which is how a host's refusal meets it */
            run_spawn_iof(spawner, true, PMIX_FWD_NO_CHANNELS, child);
            find_watch(child, &count);
            snprintf(what, sizeof(what), "agree: %s: spawn-time half %s too",
                     rows[r].name, parser_inherits ? "inherits" : "does not inherit");
            report(what, parser_inherits == (0 < count));
            drop_watches(child);
        }
        PMIX_RELEASE(cd);
        for (n = 0; n < rows[r].ninfo; n++) {
            PMIX_INFO_DESTRUCT(&rows[r].info[n]);
        }
    }

    drop_watches("agreejob");
    PMIX_RELEASE(spawner);
    PMIX_RELEASE(tool);
    PMIX_RELEASE(client);
    PMIX_RELEASE(watcher);
}

/* A job that did not inherit ends the ancestry walk.
 *
 * Its child names nothing and so inherits - but from its parent, which
 * forwards nothing on this server. Walking on past it hands the child the
 * grandparent's watcher, which the parent itself was refused. The parent
 * is registered as a host registers a job that does not inherit, with
 * PMIX_IOF_INHERIT=false. */
static void test_walk_stops(void)
{
    pmix_peer_t *watcher;
    pmix_info_t silence;
    bool no = false;
    int count;

    watcher = mkpeer("toolns", 0, true);
    watch_job(watcher, "walkjob", PMIX_FWD_STDERR_CHANNEL, 12);
    PMIX_INFO_LOAD(&silence, PMIX_IOF_INHERIT, &no, PMIX_BOOL);

    if (PMIX_SUCCESS != register_child_info("walksilent", "walkjob", 0, &silence, 1) ||
        PMIX_SUCCESS != register_child("walkchild", "walksilent", 0)) {
        report("walk: could not register the namespaces", false);
    } else {
        deliver_from("walkchild", 0);
        find_watch("walkchild", &count);
        report("walk: the walk stops at a job that did not inherit", 0 == count);
    }

    PMIX_INFO_DESTRUCT(&silence);
    drop_watches("walkchild");
    drop_watches("walksilent");
    drop_watches("walkjob");
    PMIX_RELEASE(watcher);
}

/* ---- a real tool connecting, for the automatic subscription ---- */

#define INH_TOOL_NSPACE "inhtool"

static void tool_connected_fn(pmix_info_t *info, size_t ninfo,
                              pmix_tool_connection_cbfunc_t cbfunc, void *cbdata)
{
    pmix_proc_t proc;
    PMIX_HIDE_UNUSED_PARAMS(info, ninfo);

    PMIX_LOAD_PROCID(&proc, INH_TOOL_NSPACE, 0);
    cbfunc(PMIX_SUCCESS, &proc, cbdata);
}

/* The forked tool: connect to the URI it is handed, say so, and stay
 * connected until told to go */
static int run_tool(int urifd, int readyfd, int gofd)
{
    char uri[1024], c = 'r';
    ssize_t n;
    pmix_proc_t myproc;
    pmix_info_t tinfo;
    pmix_status_t rc;

    n = read(urifd, uri, sizeof(uri) - 1);
    if (0 >= n) {
        return 1;
    }
    uri[n] = '\0';
    PMIX_INFO_LOAD(&tinfo, PMIX_SERVER_URI, uri, PMIX_STRING);
    rc = PMIx_tool_init(&myproc, &tinfo, 1);
    PMIX_INFO_DESTRUCT(&tinfo);
    if (PMIX_SUCCESS != rc) {
        return 1;
    }
    if (1 != write(readyfd, &c, 1) || 1 != read(gofd, &c, 1)) {
        PMIx_tool_finalize();
        return 1;
    }
    return (PMIX_SUCCESS == PMIx_tool_finalize()) ? 0 : 1;
}

/* A tool that connects is subscribed to this server's own output. A job
 * the server spawned for itself - its parent is the server's own proc -
 * and registered before the tool connected inherits that subscription,
 * as it would one the tool asked for. */
static void test_tool_connects(int uriw, int readyr, int gow, pid_t child)
{
    pmix_iof_req_t *req;
    const char *uri;
    char c = 'g';
    int i, count = 0, status = 0;

    if (PMIX_SUCCESS != register_child("srvJ", pmix_globals.myid.nspace,
                                       pmix_globals.myid.rank)) {
        report("tool: could not register the server's own job", false);
        return;
    }
    uri = (NULL != pmix_ptl_base.listener.uri) ? pmix_ptl_base.listener.uri
                                               : pmix_ptl_base.local_listener.uri;
    if (NULL == uri || (ssize_t) strlen(uri) != write(uriw, uri, strlen(uri))) {
        report("tool: could not hand the tool the server's URI", false);
        return;
    }
    if (1 != read(readyr, &c, 1)) {
        report("tool: the tool connected", false);
        return;
    }
    report("tool: the tool connected", true);
    /* the accept that answered the tool may still be running */
    progress_barrier();

    for (i = 0; i < pmix_globals.iof_requests.size; i++) {
        req = (pmix_iof_req_t *) pmix_pointer_array_get_item(&pmix_globals.iof_requests, i);
        if (NULL != req && 0 != req->root_id && 1 == req->nprocs &&
            PMIX_CHECK_NSPACE(req->procs[0].nspace, "srvJ") && NULL != req->requestor &&
            NULL != req->requestor->info &&
            PMIX_CHECK_NSPACE(req->requestor->info->pname.nspace, INH_TOOL_NSPACE)) {
            ++count;
        }
    }
    report("tool: the server's own job inherits the tool's subscription", 1 == count);

    c = 'g';
    if (1 != write(gow, &c, 1)) {
        report("tool: could not tell the tool to go", false);
    }
    waitpid(child, &status, 0);
    report("tool: the tool exited cleanly", WIFEXITED(status) && 0 == WEXITSTATUS(status));
    drop_watches("srvJ");
}

static pmix_server_module_t mymodule = {0};

int main(int argc, char **argv)
{
    pmix_status_t rc;
    pmix_info_t sinfo;
    int uripipe[2], readypipe[2], gopipe[2], status;
    pid_t child;
    bool flag = true;
    PMIX_HIDE_UNUSED_PARAMS(argc, argv);

    /* the tool is forked before the server starts its progress thread */
    if (0 != pipe(uripipe) || 0 != pipe(readypipe) || 0 != pipe(gopipe)) {
        fprintf(stderr, "pipe() failed\n");
        return 1;
    }
    child = fork();
    if (0 > child) {
        fprintf(stderr, "fork() failed\n");
        return 1;
    }
    if (0 == child) {
        close(uripipe[1]);
        close(readypipe[0]);
        close(gopipe[1]);
        _exit(run_tool(uripipe[0], readypipe[1], gopipe[0]));
    }
    close(uripipe[0]);
    close(readypipe[1]);
    close(gopipe[0]);

    mymodule.iof_pull = stub_iof_pull;
    mymodule.tool_connected = tool_connected_fn;
    PMIX_INFO_LOAD(&sinfo, PMIX_SERVER_TOOL_SUPPORT, &flag, PMIX_BOOL);
    rc = PMIx_server_init(&mymodule, &sinfo, 1);
    PMIX_INFO_DESTRUCT(&sinfo);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "PMIx_server_init failed: %s\n", PMIx_Error_string(rc));
        close(uripipe[1]);
        close(gopipe[1]);
        close(readypipe[0]);
        waitpid(child, &status, 0);
        return 1;
    }

    test_spawn_parser();
    test_inheritance();
    test_delivery_ancestry();
    test_parser_agreement();
    test_walk_stops();
    test_inherit_access();
    test_clone_lineage();
    test_job_gone();
    test_original_covers();
    test_walk_collects();
    test_refusal_mid_chain();
    test_late_subscriber();
    test_spawn_completes_late();
    test_spawn_before_data();
    test_own_pull();
    test_data_by_update();
    test_one_copy_per_handler();
    test_released_middle();
    test_released_before_registration();
    test_drain_all_ancestors();
    test_rank_pull_overlap();
    test_rank_pull_grandchild();
    test_covering_pull();
    test_tool_connects(uripipe[1], readypipe[0], gopipe[1], child);
    close(uripipe[1]);
    close(gopipe[1]);
    close(readypipe[0]);

    PMIx_server_finalize();

    printf("\n%s: %d failure(s)\n", 0 == failures ? "SUCCESS" : "FAILURE", failures);
    return (0 == failures) ? 0 : 1;
}
