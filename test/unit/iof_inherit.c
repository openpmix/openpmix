/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
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
 */

#include "src/include/pmix_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "include/pmix.h"
#include "include/pmix_server.h"

#include "src/common/pmix_iof.h"
#include "src/include/pmix_globals.h"
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
 * the identity and the role matter here - nothing in this test packs a
 * message, so no bfrops module is needed. */
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
     * unread. The delivery-time path cannot run before the namespace
     * exists, so it always has a real answer; leave it to that. */
    run_spawn_iof(client, true, PMIX_FWD_NO_CHANNELS, "unregisteredjob");
    find_watch("unregisteredjob", &count);
    report("inherit: an unregistered namespace defers to delivery", 0 == count);
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

/* The delivery-time half of inheritance.
 *
 * The spawn-time clone above is made by the server that PROCESSED the
 * spawn - the one hosting the spawning process. On a multi-node DVM that
 * is not the server holding the watching tool's subscription, so the
 * clone is built where the output does not arrive and the server that
 * does receive it has nothing to match against. This is the other end:
 * output arrives for a namespace nobody here watches, and before it is
 * cached and lost the server asks whether it is the child of a job that
 * someone here does watch.
 */
static void test_delivery_ancestry(void)
{
    pmix_peer_t *watcher;
    pmix_iof_req_t *req;
    int count;

    watcher = mkpeer("toolns", 0, true);

    /* somebody here watches the parent job's stderr, and nothing at all
     * names the child. The drop is not tidying: a childjob subscription
     * left behind by the spawn-time cases above would let every case
     * here pass without the delivery-time path running at all. */
    drop_watches("childjob");
    watch_job(watcher, "parentjob", PMIX_FWD_STDERR_CHANNEL, 9);

    if (PMIX_SUCCESS != register_child("childjob", "parentjob", 0)) {
        report("ancestry: could not register the child namespace", false);
        goto cleanup;
    }

    /* ---- output from an unwatched child finds its parent's watcher ---- */
    deliver_from("childjob", 0);
    req = find_watch("childjob", &count);
    report("ancestry: delivery creates the child's subscription",
           NULL != req && 1 == count);
    if (NULL != req) {
        report("ancestry: it goes to the ancestor's watcher",
               req->requestor == watcher);
        report("ancestry: it carries the ancestor's channels",
               PMIX_FWD_STDERR_CHANNEL == req->channels);
        report("ancestry: it keeps the watcher's handler id", 9 == req->remote_id);
    }

    /* ---- the walk is transitive ----
     *
     * A grandchild is watched too: "treated the way its parent is
     * treated" says nothing about depth. Note the child's own clone is
     * dropped first, so the grandchild's answer has to come from two
     * levels up rather than from the entry the case above created. */
    if (PMIX_SUCCESS != register_child("grandchild", "childjob", 0)) {
        report("ancestry: could not register the grandchild namespace", false);
        goto cleanup;
    }
    drop_watches("childjob");
    deliver_from("grandchild", 0);
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
    deliver_from("orphanjob", 0);
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
    deliver_from("quietjob", 0);
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

static pmix_server_module_t mymodule = {0};

int main(int argc, char **argv)
{
    pmix_status_t rc;
    PMIX_HIDE_UNUSED_PARAMS(argc, argv);

    rc = PMIx_server_init(&mymodule, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "PMIx_server_init failed: %s\n", PMIx_Error_string(rc));
        return 1;
    }

    test_spawn_parser();
    test_inheritance();
    test_delivery_ancestry();

    PMIx_server_finalize();

    printf("\n%s: %d failure(s)\n", 0 == failures ? "SUCCESS" : "FAILURE", failures);
    return (0 == failures) ? 0 : 1;
}
