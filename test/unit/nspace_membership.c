/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Who may join a namespace on a server.
 *
 * A namespace holds either processes the host (or the server itself)
 * started - clients and tool-clients - or self-started tools, never
 * both:
 *
 *  - a client or tool-client was registered before it started, so its
 *    namespace and rank are already on the server, and it has to be the
 *    identity they were registered with. A rank has one connection at a
 *    time;
 *  - a self-started tool builds its own namespace, and may join only a
 *    namespace another self-started tool built, as the same user. The
 *    server drops a rank no connected process holds, and the namespace
 *    once none is left in it, however its tools left - the host did not
 *    start them and may never hear they went. A tool that comes back
 *    builds them again;
 *  - the host may not register a client into a self-started tool's
 *    namespace, and registering a rank it already registered updates it;
 *  - the host's full registration of a name a self-started tool holds
 *    takes it back: the tool is closed, and the job is built fresh;
 *  - deregistering a rank or a namespace closes the connection on it.
 *
 * The server runs here; every connecting process is this binary re-exec'd
 * with a role, so it starts with an uninitialized library. A child
 * reports through its exit status whether it connected.
 */

#include "src/include/pmix_config.h"

#include "include/pmix.h"
#include "include/pmix_server.h"
#include "include/pmix_tool.h"
#include "src/include/pmix_globals.h"
#include "src/mca/ptl/base/base.h"
#include "src/server/pmix_server_ops.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_printf.h"

#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

#define NSM_JOB   "nsm.job"    // a job the host registered
#define NSM_NONE  "nsm.none"   // a job nobody registered
#define NSM_SELF  "nsm.self"   // a self-started tool's job
#define NSM_FRESH "nsm.fresh"  // a tool's job the host registers during the upcall
#define NSM_LOST  "nsm.lost"   // a self-started tool's job, left without finalizing
#define NSM_GONE  "nsm.gone"   // a self-started tool's job, deregistered during an upcall
#define NSM_DEREG "nsm.dereg"  // a job the host deregisters with a process still on it
#define NSM_SQUAT "nsm.squat"  // a name a self-started tool takes before the host's job

/* child exit statuses */
#define CHILD_CONNECTED 0
#define CHILD_REFUSED   2

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
/* the connecting processes                                            */
/* ------------------------------------------------------------------ */

/* Connect as a tool. The environment decides what kind: PMIX_NAMESPACE
 * and PMIX_RANK make it a tool-client, NSM_NSPACE/NSM_RANK a self-started
 * tool with an identity of its own, and neither one that asks for one.
 * With NSM_READYFD/NSM_GOFD it stays connected until told to go, and with
 * NSM_NOFINAL it exits without finalizing. */
static int run_tool(void)
{
    pmix_proc_t me;
    pmix_info_t info[3];
    size_t ninfo = 0;
    pmix_rank_t rank;
    pmix_status_t rc;
    char *ns, *rk, *rfd, *gfd;
    char c = 'r';

    alarm(60);
    PMIX_INFO_LOAD(&info[ninfo], PMIX_SERVER_URI, getenv("NSM_URI"), PMIX_STRING);
    ++ninfo;
    ns = getenv("NSM_NSPACE");
    rk = getenv("NSM_RANK");
    if (NULL != ns && NULL != rk) {
        rank = (pmix_rank_t) strtoul(rk, NULL, 10);
        PMIX_INFO_LOAD(&info[ninfo], PMIX_TOOL_NSPACE, ns, PMIX_STRING);
        ++ninfo;
        PMIX_INFO_LOAD(&info[ninfo], PMIX_TOOL_RANK, &rank, PMIX_PROC_RANK);
        ++ninfo;
    }
    rc = PMIx_tool_init(&me, info, ninfo);
    while (0 < ninfo) {
        --ninfo;
        PMIX_INFO_DESTRUCT(&info[ninfo]);
    }
    if (PMIX_SUCCESS != rc) {
        return CHILD_REFUSED;
    }
    if (NULL != getenv("NSM_NOFINAL")) {
        _exit(CHILD_CONNECTED);
    }
    rfd = getenv("NSM_READYFD");
    gfd = getenv("NSM_GOFD");
    if (NULL != rfd && NULL != gfd) {
        if (1 != write(atoi(rfd), &c, 1) || 1 != read(atoi(gfd), &c, 1)) {
            PMIx_tool_finalize();
            return 1;
        }
    }
    PMIx_tool_finalize();
    return CHILD_CONNECTED;
}

static int run_client(void)
{
    pmix_proc_t me;

    alarm(60);
    if (PMIX_SUCCESS != PMIx_Init(&me, NULL, 0)) {
        return CHILD_REFUSED;
    }
    PMIx_Finalize(NULL, 0);
    return CHILD_CONNECTED;
}

/* ------------------------------------------------------------------ */
/* the host                                                            */
/* ------------------------------------------------------------------ */

/* what the tool_connected upcall answers a tool that asks for an id */
static enum {
    ASSIGN_JOB,      // the host registered job - which the tool may not join
    ASSIGN_FRESH     // a name the host registers for the tool, as PRRTE does
} assign = ASSIGN_JOB;

/* a tool naming this namespace has it deregistered while the host
 * decides on the tool */
static const char *volatile dereg_in_upcall = NULL;

typedef struct {
    pmix_tool_connection_cbfunc_t cbfunc;
    void *cbdata;
    pmix_proc_t proc;
} upcall_t;

static void dereg_done(pmix_status_t status, void *cbdata)
{
    upcall_t *up = (upcall_t *) cbdata;

    (void) status;
    up->cbfunc(PMIX_SUCCESS, &up->proc, up->cbdata);
    free(up);
}

static void fresh_registered(pmix_status_t status, void *cbdata)
{
    upcall_t *up = (upcall_t *) cbdata;
    pmix_proc_t proc;

    PMIX_LOAD_PROCID(&proc, NSM_FRESH, 0);
    up->cbfunc(status, &proc, up->cbdata);
    free(up);
}

static void tool_connected_fn(pmix_info_t *info, size_t ninfo,
                              pmix_tool_connection_cbfunc_t cbfunc, void *cbdata)
{
    pmix_proc_t proc;
    pmix_nspace_t fresh;
    upcall_t *up;
    pmix_status_t rc;
    bool have_ns = false;
    size_t n;

    PMIX_LOAD_PROCID(&proc, NULL, 0);
    for (n = 0; n < ninfo; n++) {
        if (PMIX_CHECK_KEY(&info[n], PMIX_NSPACE)) {
            PMIX_LOAD_NSPACE(proc.nspace, info[n].value.data.string);
            have_ns = true;
        } else if (PMIX_CHECK_KEY(&info[n], PMIX_RANK)) {
            proc.rank = info[n].value.data.rank;
        }
    }
    if (have_ns && NULL != dereg_in_upcall && PMIX_CHECK_NSPACE(proc.nspace, dereg_in_upcall)) {
        up = (upcall_t *) malloc(sizeof(upcall_t));
        up->cbfunc = cbfunc;
        up->cbdata = cbdata;
        up->proc = proc;
        PMIx_server_deregister_nspace(proc.nspace, dereg_done, up);
        return;
    }
    if (have_ns) {
        cbfunc(PMIX_SUCCESS, &proc, cbdata);
        return;
    }
    if (ASSIGN_JOB == assign) {
        PMIX_LOAD_PROCID(&proc, NSM_JOB, 0);
        cbfunc(PMIX_SUCCESS, &proc, cbdata);
        return;
    }
    /* register the tool's job before answering, as PRRTE does */
    up = (upcall_t *) malloc(sizeof(upcall_t));
    up->cbfunc = cbfunc;
    up->cbdata = cbdata;
    PMIX_LOAD_NSPACE(fresh, NSM_FRESH);
    rc = PMIx_server_register_nspace(fresh, 1, NULL, 0, fresh_registered, up);
    if (PMIX_SUCCESS != rc) {
        free(up);
        cbfunc(rc, NULL, cbdata);
    }
}

/* the host's handler for a lost connection: notes that a tool of
 * NSM_LOST was reported gone */
static volatile int lost_seen = 0;

static void lost_fn(size_t id, pmix_status_t status, const pmix_proc_t *source,
                    pmix_info_t info[], size_t ninfo, pmix_info_t results[],
                    size_t nresults, pmix_event_notification_cbfunc_fn_t cbfunc,
                    void *cbdata)
{
    (void) id;
    (void) status;
    (void) info;
    (void) ninfo;
    (void) results;
    (void) nresults;
    if (NULL != source && 0 == strncmp(source->nspace, NSM_LOST, PMIX_MAX_NSLEN)) {
        lost_seen = 1;
    }
    if (NULL != cbfunc) {
        cbfunc(PMIX_EVENT_ACTION_COMPLETE, NULL, 0, NULL, NULL, cbdata);
    }
}

static bool wait_lost(void)
{
    int n;

    for (n = 0; n < 1000 && !lost_seen; n++) {
        usleep(10000);
    }
    return (0 != lost_seen);
}

static pmix_server_module_t mymodule = {
    .tool_connected = tool_connected_fn
};

static char *server_uri = NULL;
static char *self_argv0 = NULL;

/* the environment a child starts with: ours, less anything that would
 * tell it who it is, plus what the case adds */
static char **child_env(char **extra)
{
    char **env = NULL;
    size_t n;

    for (n = 0; NULL != environ[n]; n++) {
        if (0 == strncmp(environ[n], "PMIX_NAMESPACE=", 15) ||
            0 == strncmp(environ[n], "PMIX_RANK=", 10) ||
            0 == strncmp(environ[n], "PMIX_SERVER_URI", 15) ||
            0 == strncmp(environ[n], "NSM_", 4)) {
            continue;
        }
        PMIx_Argv_append_nosize(&env, environ[n]);
    }
    for (n = 0; NULL != extra && NULL != extra[n]; n++) {
        PMIx_Argv_append_nosize(&env, extra[n]);
    }
    return env;
}

/* start this binary as role with env, without waiting */
static pid_t launch(const char *role, char **env)
{
    char *argv[3];
    pid_t child;

    argv[0] = self_argv0;
    argv[1] = (char *) role;
    argv[2] = NULL;
    child = fork();
    if (0 == child) {
        execve(self_argv0, argv, env);
        _exit(127);
    }
    return child;
}

static int reap(pid_t child)
{
    int status;

    if (0 > child || child != waitpid(child, &status, 0) || !WIFEXITED(status)) {
        return -1;
    }
    return WEXITSTATUS(status);
}

/* the environment of a tool: NULL nspace asks for an identity */
static char **tool_env(const char *nspace, const char *rank, bool tool_client, char **more)
{
    char **extra = NULL, **env;
    char *tmp;
    size_t n;

    pmix_asprintf(&tmp, "NSM_URI=%s", server_uri);
    PMIx_Argv_append_nosize(&extra, tmp);
    free(tmp);
    if (NULL != nspace) {
        pmix_asprintf(&tmp, "%s=%s", tool_client ? "PMIX_NAMESPACE" : "NSM_NSPACE", nspace);
        PMIx_Argv_append_nosize(&extra, tmp);
        free(tmp);
        pmix_asprintf(&tmp, "%s=%s", tool_client ? "PMIX_RANK" : "NSM_RANK", rank);
        PMIx_Argv_append_nosize(&extra, tmp);
        free(tmp);
    }
    for (n = 0; NULL != more && NULL != more[n]; n++) {
        PMIx_Argv_append_nosize(&extra, more[n]);
    }
    env = child_env(extra);
    PMIx_Argv_free(extra);
    /* a tool-client is a process the host prepared, as it prepares a
     * client - which hands it its rank's connection token. A rank nobody
     * registered has none to hand out, and the tool connects without */
    if (tool_client && NULL != nspace) {
        pmix_proc_t proc;

        PMIX_LOAD_PROCID(&proc, nspace, (pmix_rank_t) strtoul(rank, NULL, 10));
        (void) PMIx_server_setup_fork(&proc, &env);
    }
    return env;
}

/* run a tool to completion and return its exit status */
static int tool(const char *nspace, const char *rank, bool tool_client)
{
    char **env;
    int rc;

    env = tool_env(nspace, rank, tool_client, NULL);
    rc = reap(launch("tool", env));
    PMIx_Argv_free(env);
    return rc;
}

/* run a self-started tool that exits without finalizing */
static int tool_nofinal(const char *nspace, const char *rank)
{
    char *more[] = {"NSM_NOFINAL=1", NULL};
    char **env;
    int rc;

    env = tool_env(nspace, rank, false, more);
    rc = reap(launch("tool", env));
    PMIx_Argv_free(env);
    return rc;
}

/* a self-started tool held connected until let go */
typedef struct {
    pid_t pid;
    int ready;
    int go;
} held_t;

static bool hold_as(const char *nspace, const char *rank, bool tool_client, held_t *h)
{
    int ready[2], go[2];
    char *more[3] = {NULL, NULL, NULL};
    char **env, c;

    h->pid = -1;
    h->ready = -1;
    h->go = -1;
    if (0 != pipe(ready)) {
        return false;
    }
    if (0 != pipe(go)) {
        close(ready[0]);
        close(ready[1]);
        return false;
    }
    fcntl(ready[0], F_SETFD, FD_CLOEXEC);
    fcntl(go[1], F_SETFD, FD_CLOEXEC);
    pmix_asprintf(&more[0], "NSM_READYFD=%d", ready[1]);
    pmix_asprintf(&more[1], "NSM_GOFD=%d", go[0]);
    env = tool_env(nspace, rank, tool_client, more);
    free(more[0]);
    free(more[1]);
    h->pid = launch("tool", env);
    PMIx_Argv_free(env);
    close(ready[1]);
    close(go[0]);
    h->ready = ready[0];
    h->go = go[1];
    return (1 == read(h->ready, &c, 1));
}

static bool hold(const char *nspace, const char *rank, held_t *h)
{
    return hold_as(nspace, rank, false, h);
}

static int let_go(held_t *h)
{
    char c = 'g';

    if (1 != write(h->go, &c, 1)) {
        /* it is gone already */
    }
    close(h->go);
    close(h->ready);
    return reap(h->pid);
}

/* What the server holds for a namespace, read on its progress thread:
 * whether the namespace is listed, whether a self-started tool's, how
 * many entries it has for a rank, and how many connected peers are on
 * it. */
typedef struct {
    pmix_event_t ev;
    pmix_lock_t lock;
    const char *nspace;
    pmix_rank_t rank;
    bool listed;
    bool self_started;
    size_t nranks;
    size_t nconnected;
} probe_t;

static void probe_fn(int sd, short args, void *cbdata)
{
    probe_t *p = (probe_t *) cbdata;
    pmix_namespace_t *ns, *nptr = NULL;
    pmix_rank_info_t *info;
    pmix_peer_t *peer;
    int i;

    (void) sd;
    (void) args;
    PMIX_LIST_FOREACH (ns, &pmix_globals.nspaces, pmix_namespace_t) {
        if (NULL != ns->nspace && 0 == strcmp(ns->nspace, p->nspace)) {
            nptr = ns;
            break;
        }
    }
    if (NULL != nptr) {
        p->listed = true;
        p->self_started = nptr->self_started;
        PMIX_LIST_FOREACH (info, &nptr->ranks, pmix_rank_info_t) {
            if (info->pname.rank == p->rank) {
                ++p->nranks;
            }
        }
        for (i = 0; i < pmix_server_globals.clients.size; i++) {
            peer = (pmix_peer_t *) pmix_pointer_array_get_item(&pmix_server_globals.clients, i);
            if (NULL != peer && peer->nptr == nptr && 0 <= peer->sd) {
                ++p->nconnected;
            }
        }
    }
    PMIX_WAKEUP_THREAD(&p->lock);
}

static void probe(const char *nspace, pmix_rank_t rank, probe_t *p)
{
    memset(p, 0, sizeof(*p));
    PMIX_CONSTRUCT_LOCK(&p->lock);
    p->nspace = nspace;
    p->rank = rank;
    PMIX_THREADSHIFT(p, probe_fn);
    PMIX_WAIT_THREAD(&p->lock);
    PMIX_DESTRUCT_LOCK(&p->lock);
}

/* a departure is seen when the server reads the socket closing, which
 * need not have happened by the time the child is reaped */
static bool wait_gone(const char *nspace)
{
    probe_t p;
    int n;

    for (n = 0; n < 1000; n++) {
        probe(nspace, 0, &p);
        if (!p.listed) {
            return true;
        }
        usleep(10000);
    }
    return false;
}

static bool wait_rank_gone(const char *nspace, pmix_rank_t rank)
{
    probe_t p;
    int n;

    for (n = 0; n < 1000; n++) {
        probe(nspace, rank, &p);
        if (0 == p.nranks) {
            return p.listed;
        }
        usleep(10000);
    }
    return false;
}

/* how many connected peers the server holds under this namespace's name,
 * whether or not the namespace is still listed */
typedef struct {
    pmix_event_t ev;
    pmix_lock_t lock;
    const char *nspace;
    size_t count;
} named_t;

static void named_fn(int sd, short args, void *cbdata)
{
    named_t *q = (named_t *) cbdata;
    pmix_peer_t *peer;
    int i;

    (void) sd;
    (void) args;
    for (i = 0; i < pmix_server_globals.clients.size; i++) {
        peer = (pmix_peer_t *) pmix_pointer_array_get_item(&pmix_server_globals.clients, i);
        if (NULL != peer && NULL != peer->info && 0 <= peer->sd &&
            0 == strncmp(peer->info->pname.nspace, q->nspace, PMIX_MAX_NSLEN)) {
            ++q->count;
        }
    }
    PMIX_WAKEUP_THREAD(&q->lock);
}

static size_t connected_as(const char *nspace)
{
    named_t q;

    memset(&q, 0, sizeof(q));
    PMIX_CONSTRUCT_LOCK(&q.lock);
    q.nspace = nspace;
    PMIX_THREADSHIFT(&q, named_fn);
    PMIX_WAIT_THREAD(&q.lock);
    PMIX_DESTRUCT_LOCK(&q.lock);
    return q.count;
}

static pmix_namespace_t *find_nspace(const char *nspace)
{
    pmix_namespace_t *ns;

    PMIX_LIST_FOREACH (ns, &pmix_globals.nspaces, pmix_namespace_t) {
        if (NULL != ns->nspace && 0 == strcmp(ns->nspace, nspace)) {
            return ns;
        }
    }
    return NULL;
}


static size_t count_rank(pmix_namespace_t *ns, pmix_rank_t rank, pmix_rank_info_t **out)
{
    pmix_rank_info_t *info;
    size_t count = 0;

    PMIX_LIST_FOREACH (info, &ns->ranks, pmix_rank_info_t) {
        if (info->pname.rank == rank) {
            ++count;
            *out = info;
        }
    }
    return count;
}

static void opcb(pmix_status_t status, void *cbdata)
{
    pmix_status_t *st = (pmix_status_t *) cbdata;
    *st = status;
}

/* register a client, waiting for the outcome */
static pmix_status_t register_client(const char *nspace, pmix_rank_t rank, uid_t uid, gid_t gid)
{
    pmix_proc_t proc;
    volatile pmix_status_t st = PMIX_ERR_NOT_AVAILABLE + 1000;
    pmix_status_t rc;
    int n;

    PMIX_LOAD_PROCID(&proc, nspace, rank);
    rc = PMIx_server_register_client(&proc, uid, gid, NULL, opcb, (void *) &st);
    if (PMIX_SUCCESS != rc) {
        return rc;
    }
    for (n = 0; n < 1000 && PMIX_ERR_NOT_AVAILABLE + 1000 == st; n++) {
        usleep(10000);
    }
    return st;
}

static void self_started_cases(void)
{
    held_t h1;
    char **env;
    probe_t p;
    pmix_proc_t proc;

    /* a self-started tool builds nsm.self and stays connected */
    if (!hold(NSM_SELF, "0", &h1)) {
        report("self-started tool connects with its own namespace", 0);
        (void) let_go(&h1);
        return;
    }
    report("self-started tool connects with its own namespace", 1);
    probe(NSM_SELF, 0, &p);
    report("its namespace is marked as a self-started tool's", p.listed && p.self_started);

    report("the host cannot register a client into it",
           PMIX_ERR_NOT_SUPPORTED == register_client(NSM_SELF, 1, getuid(), getgid()));

    /* a client naming the tool's rank - the env a client of the host's
     * job gets, retargeted at the tool's namespace */
    PMIX_LOAD_PROCID(&proc, NSM_JOB, 0);
    env = child_env(NULL);
    if (PMIX_SUCCESS == PMIx_server_setup_fork(&proc, &env)) {
        PMIx_Setenv("PMIX_NAMESPACE", NSM_SELF, true, &env);
        PMIx_Setenv("PMIX_RANK", "0", true, &env);
        report("a client cannot connect into it",
               CHILD_REFUSED == reap(launch("client", env)));
    } else {
        report("a client cannot connect into it", 0);
    }
    PMIx_Argv_free(env);

    report("a second self-started tool of the same user joins it",
           CHILD_CONNECTED == tool(NSM_SELF, "1", false));
    report("the rank it held goes when it leaves, and the namespace stays",
           wait_rank_gone(NSM_SELF, 1));
    report("a tool-client cannot connect into it",
           CHILD_REFUSED == tool(NSM_SELF, "0", true));

    /* a rank has one connection at a time: a second process as the same
     * rank is refused while the first holds it */
    report("a second process as its rank is refused while the first holds it",
           CHILD_REFUSED == tool(NSM_SELF, "0", false));

    report("the first self-started tool finalized cleanly", CHILD_CONNECTED == let_go(&h1));
    report("the namespace goes when its last process leaves", wait_gone(NSM_SELF));
    report("a tool can come back as the same rank of it",
           CHILD_CONNECTED == tool(NSM_SELF, "0", false));
    report("and it goes again when that tool leaves", wait_gone(NSM_SELF));

    /* leaving without finalizing */
    report("a self-started tool that will not finalize connects",
           CHILD_CONNECTED == tool_nofinal(NSM_LOST, "0"));
    report("the host is told at once that it left", wait_lost());
    report("its namespace goes when it exits", wait_gone(NSM_LOST));
    report("and it can come back", CHILD_CONNECTED == tool(NSM_LOST, "0", false));
}

/* The namespace a tool joins is deregistered while the host decides on
 * the tool. The tool connects on a namespace of its own that the server
 * holds - not on the one that went. */
static void vanished_case(void)
{
    held_t h1, h2;
    probe_t p;
    bool ok;

    if (!hold(NSM_GONE, "0", &h1)) {
        report("a tool builds the namespace another will join", 0);
        (void) let_go(&h1);
        return;
    }
    dereg_in_upcall = NSM_GONE;
    ok = hold(NSM_GONE, "1", &h2);
    dereg_in_upcall = NULL;
    report("a tool whose namespace goes while the host decides connects", ok);
    probe(NSM_GONE, 1, &p);
    report("on a namespace the server holds",
           ok && p.listed && p.self_started && 1 == p.nranks && 1 == p.nconnected);
    report("and leaves cleanly", CHILD_CONNECTED == let_go(&h2));
    /* the first one's server dropped it, so how it exits is not asked */
    (void) let_go(&h1);
    report("that namespace goes too", wait_gone(NSM_GONE));
}

/* wait for an operation that answers through opcb */
static pmix_status_t await(pmix_status_t rc, volatile pmix_status_t *st)
{
    int n;

    for (n = 0; PMIX_SUCCESS == rc && n < 1000 && PMIX_ERR_NOT_AVAILABLE + 1000 == *st; n++) {
        usleep(10000);
    }
    return (PMIX_SUCCESS == rc) ? *st : rc;
}

/* The host deregisters a rank, then a namespace, while a clone of the
 * rank is still connected. Both connections close. */
/* A self-started tool takes a name before the host's job of that name is
 * registered. An update leaves it be; the host's full registration takes
 * the name back - the tool is closed, and the job is built fresh, with
 * none of the tool's ranks in it */
static void takeback_cases(void)
{
    held_t h;
    pmix_nspace_t job;
    volatile pmix_status_t st;
    probe_t p;
    bool ok;

    PMIX_LOAD_NSPACE(job, NSM_SQUAT);
    if (!hold(NSM_SQUAT, "0", &h)) {
        report("a self-started tool takes a name the host will use", 0);
        (void) let_go(&h);
        return;
    }
    report("a self-started tool takes a name the host will use", 1);

    st = PMIX_ERR_NOT_AVAILABLE + 1000;
    ok = PMIX_SUCCESS ==
         await(PMIx_server_register_nspace(job, -1, NULL, 0, opcb, (void *) &st), &st);
    probe(NSM_SQUAT, 0, &p);
    report("an update of the tool's namespace leaves the tool in it",
           ok && p.listed && p.self_started && 1 == p.nranks && 1 == p.nconnected);

    st = PMIX_ERR_NOT_AVAILABLE + 1000;
    ok = PMIX_SUCCESS ==
         await(PMIx_server_register_nspace(job, 1, NULL, 0, opcb, (void *) &st), &st);
    probe(NSM_SQUAT, 0, &p);
    report("the host's full registration takes the name back",
           ok && p.listed && !p.self_started && 0 == p.nranks);
    report("and the tool that held it is closed", 0 == p.nconnected);
    /* the server dropped it, so how it exits is not asked */
    (void) let_go(&h);

    report("the host registers its client into the name",
           PMIX_SUCCESS == register_client(NSM_SQUAT, 0, getuid(), getgid()));
    report("and its tool-client connects there", CHILD_CONNECTED == tool(NSM_SQUAT, "0", true));
    report("a self-started tool can no longer take the name",
           CHILD_REFUSED == tool(NSM_SQUAT, "1", false));
    st = PMIX_ERR_NOT_AVAILABLE + 1000;
    PMIx_server_deregister_nspace(job, opcb, (void *) &st);
    (void) await(PMIX_SUCCESS, &st);
}

static void dereg_cases(void)
{
    held_t h1;
    pmix_nspace_t job;
    pmix_proc_t proc;
    volatile pmix_status_t st;
    bool ok;

    st = PMIX_ERR_NOT_AVAILABLE + 1000;
    PMIX_LOAD_NSPACE(job, NSM_DEREG);
    ok = PMIX_SUCCESS == await(PMIx_server_register_nspace(job, 1, NULL, 0, opcb, (void *) &st), &st) &&
         PMIX_SUCCESS == register_client(NSM_DEREG, 0, getuid(), getgid());
    ok = ok && hold_as(NSM_DEREG, "0", true, &h1);
    report("a tool-client connects as its registered rank", ok && 1 == connected_as(NSM_DEREG));
    PMIX_LOAD_PROCID(&proc, NSM_DEREG, 0);
    st = PMIX_ERR_NOT_AVAILABLE + 1000;
    PMIx_server_deregister_client(&proc, opcb, (void *) &st);
    (void) await(PMIX_SUCCESS, &st);
    report("deregistering the rank closes its connection", 0 == connected_as(NSM_DEREG));
    /* the server dropped it, so how it exits is not asked */
    (void) let_go(&h1);

    ok = PMIX_SUCCESS == register_client(NSM_DEREG, 0, getuid(), getgid());
    ok = ok && hold_as(NSM_DEREG, "0", true, &h1);
    report("it connects again once the rank is registered again",
           ok && 1 == connected_as(NSM_DEREG));
    st = PMIX_ERR_NOT_AVAILABLE + 1000;
    PMIx_server_deregister_nspace(job, opcb, (void *) &st);
    (void) await(PMIX_SUCCESS, &st);
    report("deregistering the namespace closes its connection",
           0 == connected_as(NSM_DEREG) && NULL == find_nspace(NSM_DEREG));
    (void) let_go(&h1);
}

int main(int argc, char **argv)
{
    pmix_status_t rc;
    pmix_info_t sinfo;
    pmix_namespace_t *ns;
    pmix_rank_info_t *info = NULL;
    pmix_nspace_t job;
    volatile pmix_status_t st;
    bool flag = true, ok;
    pmix_status_t code;
    held_t h;
    probe_t p;
    int n;

    if (1 < argc && 0 == strcmp(argv[1], "tool")) {
        return run_tool();
    }
    if (1 < argc && 0 == strcmp(argv[1], "client")) {
        return run_client();
    }
    self_argv0 = argv[0];
    /* a held tool may be gone when it is told to go */
    signal(SIGPIPE, SIG_IGN);

    fprintf(stdout, "\n=== namespace membership unit test ===\n\n");

    /* lost connections are held this long to be reported together - long
     * enough that one held for it cannot pass for one reported at once */
    setenv("PMIX_MCA_pmix_event_caching_window", "30", 1);
    PMIX_INFO_LOAD(&sinfo, PMIX_SERVER_TOOL_SUPPORT, &flag, PMIX_BOOL);
    rc = PMIx_server_init(&mymodule, &sinfo, 1);
    PMIX_INFO_DESTRUCT(&sinfo);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "PMIx_server_init failed: %s\n", PMIx_Error_string(rc));
        return 1;
    }
    code = PMIX_ERR_LOST_CONNECTION;
    rc = PMIx_Register_event_handler(&code, 1, NULL, 0, lost_fn, NULL, NULL);
    if (0 > rc) {
        fprintf(stderr, "cannot register for lost connections: %s\n", PMIx_Error_string(rc));
        PMIx_server_finalize();
        return 1;
    }
    /* the address the server reports: its TCP listener's when it has
     * one, else its local socket's */
    server_uri = (NULL != pmix_ptl_base.listener.uri) ? pmix_ptl_base.listener.uri
                                                      : pmix_ptl_base.local_listener.uri;
    if (NULL == server_uri) {
        fprintf(stderr, "the server has no URI\n");
        PMIx_server_finalize();
        return 1;
    }

    /* the host's job: rank 0 is us, rank 2 another user, rank 1 nobody */
    st = PMIX_ERR_NOT_AVAILABLE + 1000;
    PMIX_LOAD_NSPACE(job, NSM_JOB);
    rc = PMIx_server_register_nspace(job, 3, NULL, 0, opcb, (void *) &st);
    for (n = 0; PMIX_SUCCESS == rc && n < 1000 && PMIX_ERR_NOT_AVAILABLE + 1000 == st; n++) {
        usleep(10000);
    }
    report("the host registers its job",
           PMIX_SUCCESS == rc && PMIX_SUCCESS == st &&
           PMIX_SUCCESS == register_client(NSM_JOB, 0, getuid(), getgid()) &&
           PMIX_SUCCESS == register_client(NSM_JOB, 2, getuid() + 1, getgid()));

    /* tool-clients */
    report("a tool-client of an unregistered namespace is refused",
           CHILD_REFUSED == tool(NSM_NONE, "0", true));
    report("a tool-client of an unregistered rank is refused",
           CHILD_REFUSED == tool(NSM_JOB, "1", true));
    report("a tool-client that is not its rank's user is refused",
           CHILD_REFUSED == tool(NSM_JOB, "2", true));
    report("a tool-client of its own registered rank connects",
           CHILD_CONNECTED == tool(NSM_JOB, "0", true));

    /* self-started tools */
    report("a self-started tool cannot join the host's job",
           CHILD_REFUSED == tool(NSM_JOB, "5", false));
    self_started_cases();

    /* a tool the host hands an identity during the upcall */
    assign = ASSIGN_JOB;
    report("a tool cannot be handed the host's job as its identity",
           CHILD_REFUSED == tool(NULL, NULL, false));
    assign = ASSIGN_FRESH;
    ok = hold(NULL, NULL, &h);
    report("a tool whose job the host registers during the upcall connects", ok);
    probe(NSM_FRESH, 0, &p);
    report("that namespace is marked as a self-started tool's", ok && p.listed && p.self_started);
    report("and leaves cleanly", CHILD_CONNECTED == let_go(&h));
    report("and the namespace goes with it, though the host registered it",
           wait_gone(NSM_FRESH));

    vanished_case();
    dereg_cases();
    takeback_cases();

    /* registering a rank again updates it, and says so */
    report("registering a rank again reports PMIX_UPDATED",
           PMIX_UPDATED == register_client(NSM_JOB, 0, getuid() + 7, getgid()));
    ns = find_nspace(NSM_JOB);
    report("and updates the one entry for it",
           NULL != ns && 1 == count_rank(ns, 0, &info) && NULL != info &&
           getuid() + 7 == info->uid);
    /* one held by a connection of another identity is left alone */
    {
        held_t hr;
        bool held;

        report("registering it back to us is an update too",
               PMIX_UPDATED == register_client(NSM_JOB, 0, getuid(), getgid()));
        held = hold_as(NSM_JOB, "0", true, &hr);
        report("a tool-client holds the rank", held);
        report("re-registering a held rank as another user changes nothing",
               held && PMIX_ERR_EXISTS == register_client(NSM_JOB, 0, getuid() + 7, getgid()) &&
               NULL != (ns = find_nspace(NSM_JOB)) && 1 == count_rank(ns, 0, &info) &&
               getuid() == info->uid);
        report("re-registering it as its own user updates it",
               held && PMIX_UPDATED == register_client(NSM_JOB, 0, getuid(), getgid()));
        report("and the tool-client leaves cleanly", held && CHILD_CONNECTED == let_go(&hr));
    }

    PMIx_server_finalize();

    fprintf(stdout, "\n%d passed, %d failed\n", npass, nfail);
    return (0 == nfail) ? 0 : 1;
}
