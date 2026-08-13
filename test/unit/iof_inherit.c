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

    drop_watches("parentjob");
    PMIX_RELEASE(watcher);
    PMIX_RELEASE(client);
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

    PMIx_server_finalize();

    printf("\n%s: %d failure(s)\n", 0 == failures ? "SUCCESS" : "FAILURE", failures);
    return (0 == failures) ? 0 : 1;
}
