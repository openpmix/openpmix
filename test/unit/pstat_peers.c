/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/*
 * Which processes a resource monitor samples, and on whose behalf.
 *
 * A process is known by a pid and read from the kernel. A PMIx process
 * named in PMIX_MONITOR_TARGET_PROCS (or covered by "every process") is
 * sampled at the pid its host recorded with PMIx_Store_internal
 * (PMIX_PROC_PID), never at one the process claimed, and is skipped when
 * there is none. A pid named in PMIX_MONITOR_TARGET_PIDS must belong to the
 * requester - to the host, anything - and -1 is every process with the
 * requester's uid. Each process must still belong to the expected owner
 * when it is read. A request relayed by the host (PMIX_MONITOR_PROXY) must
 * say whom it is for, and a requester's periodic monitors end when it
 * leaves.
 *
 * This runs a real server with a stub host module. Every call into pstat
 * is made on the progress thread, where the server makes them.
 */

#include "src/include/pmix_config.h"

#include "include/pmix_server.h"
#include "src/include/pmix_globals.h"
#include "src/mca/pstat/base/base.h"
#include "src/mca/pstat/pstat.h"
#include "src/server/pmix_server_ops.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define NS_MINE  "pstp-mine"  // owned by us
#define NS_OTHER "pstp-other" // owned by another uid

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

/* ------------------------------------------------------------------ */
/* running a call on the progress thread                               */
/* ------------------------------------------------------------------ */

typedef struct {
    pmix_object_t super;
    pmix_event_t ev;
    pmix_lock_t lock;
    void (*fn)(void *);
    void *arg;
} shift_t;
static PMIX_CLASS_INSTANCE(shift_t, pmix_object_t, NULL, NULL);

static void shifted(int sd, short args, void *cbdata)
{
    shift_t *s = (shift_t *) cbdata;
    (void) sd;
    (void) args;
    s->fn(s->arg);
    PMIX_WAKEUP_THREAD(&s->lock);
}

static void on_progress(void (*fn)(void *), void *arg)
{
    shift_t *s = PMIX_NEW(shift_t);

    PMIX_CONSTRUCT_LOCK(&s->lock);
    s->fn = fn;
    s->arg = arg;
    PMIX_THREADSHIFT(s, shifted);
    PMIX_WAIT_THREAD(&s->lock);
    PMIX_DESTRUCT_LOCK(&s->lock);
    PMIX_RELEASE(s);
}

/* ------------------------------------------------------------------ */
/* the jobs and peers                                                  */
/* ------------------------------------------------------------------ */

static pmix_status_t reg(const char *name, uid_t uid, gid_t gid)
{
    pmix_info_t info[6];
    pmix_nspace_t ns;
    pmix_proc_t proc;
    pmix_status_t rc;
    char *noderegex = NULL, *ppnregex = NULL;
    uint32_t nprocs = 2, u32;
    size_t n;

    PMIx_generate_regex(pmix_globals.hostname, &noderegex);
    PMIx_generate_ppn("0,1", &ppnregex);
    PMIX_INFO_LOAD(&info[0], PMIX_NODE_MAP, noderegex, PMIX_REGEX);
    PMIX_INFO_LOAD(&info[1], PMIX_PROC_MAP, ppnregex, PMIX_REGEX);
    PMIX_INFO_LOAD(&info[2], PMIX_JOB_SIZE, &nprocs, PMIX_UINT32);
    PMIX_INFO_LOAD(&info[3], PMIX_UNIV_SIZE, &nprocs, PMIX_UINT32);
    u32 = (uint32_t) uid;
    PMIX_INFO_LOAD(&info[4], PMIX_USERID, &u32, PMIX_UINT32);
    u32 = (uint32_t) gid;
    PMIX_INFO_LOAD(&info[5], PMIX_GRPID, &u32, PMIX_UINT32);
    free(noderegex);
    free(ppnregex);
    PMIX_LOAD_NSPACE(ns, name);
    rc = PMIx_server_register_nspace(ns, 2, info, 6, NULL, NULL);
    for (n = 0; n < 6; n++) {
        PMIX_INFO_DESTRUCT(&info[n]);
    }
    if (PMIX_SUCCESS != rc && PMIX_OPERATION_SUCCEEDED != rc) {
        return rc;
    }
    /* both ranks are local */
    for (n = 0; n < 2; n++) {
        PMIX_LOAD_PROCID(&proc, name, (pmix_rank_t) n);
        rc = PMIx_server_register_client(&proc, uid, gid, NULL, NULL, NULL);
        if (PMIX_SUCCESS != rc && PMIX_OPERATION_SUCCEEDED != rc) {
            return rc;
        }
    }
    return PMIX_SUCCESS;
}

/* what a launcher does once it has started a process */
static void record_pid(const char *name, pmix_rank_t rank, pid_t pid)
{
    pmix_proc_t proc;
    pmix_value_t v;

    PMIX_LOAD_PROCID(&proc, name, rank);
    PMIX_VALUE_LOAD(&v, &pid, PMIX_PID);
    (void) PMIx_Store_internal(&proc, PMIX_PROC_PID, &v);
    PMIX_VALUE_DESTRUCT(&v);
}

static pmix_namespace_t *find_ns(const char *name)
{
    pmix_namespace_t *ns;

    PMIX_LIST_FOREACH (ns, &pmix_globals.nspaces, pmix_namespace_t) {
        if (NULL != ns->nspace && 0 == strcmp(ns->nspace, name)) {
            return ns;
        }
    }
    return NULL;
}

/* a connected client of ours, as far as the server's tables go */
static pmix_peer_t *make_peer(const char *nsname, pmix_rank_t rank, uid_t uid, gid_t gid)
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
    p->info->uid = uid;
    p->info->gid = gid;
    /* "connected" - nothing reads from it */
    p->sd = dup(STDIN_FILENO);
    p->index = pmix_pointer_array_add(&pmix_server_globals.clients, p);
    return p;
}

static void drop_peer(pmix_peer_t *p)
{
    pmix_pointer_array_set_item(&pmix_server_globals.clients, p->index, NULL);
    PMIX_RELEASE(p); /* closes the descriptor */
}

/* ------------------------------------------------------------------ */
/* calls, made on the progress thread                                  */
/* ------------------------------------------------------------------ */

typedef struct {
    pmix_proc_t requestor;
    pmix_info_t *dirs;
    size_t ndirs;
    pmix_list_t targets;
    pmix_status_t rc;
    /* a query */
    bool query;
    pmix_info_t *results;
    size_t nresults;
} call_t;

static void do_call(void *arg)
{
    call_t *c = (call_t *) arg;
    pmix_info_t monitor;

    if (!c->query) {
        c->rc = pmix_pstat_base_targets(&c->requestor, c->dirs, c->ndirs, &c->targets);
        return;
    }
    /* every field */
    PMIX_INFO_CONSTRUCT(&monitor);
    PMIX_LOAD_KEY(monitor.key, PMIX_MONITOR_PROC_RESOURCE_USAGE);
    c->rc = pmix_pstat.query(&c->requestor, &monitor, PMIX_SUCCESS, c->dirs, c->ndirs,
                             &c->results, &c->nresults);
}

static void call_init(call_t *c, const char *ns, pmix_rank_t rank)
{
    memset(c, 0, sizeof(*c));
    PMIX_LOAD_PROCID(&c->requestor, ns, rank);
    PMIX_CONSTRUCT(&c->targets, pmix_list_t);
}

static void call_done(call_t *c)
{
    PMIX_LIST_DESTRUCT(&c->targets);
    if (NULL != c->results) {
        PMIX_INFO_FREE(c->results, c->nresults);
    }
}

/* the requester the host is - not one of our clients or tools */
static void as_host(call_t *c)
{
    call_init(c, pmix_globals.myid.nspace, pmix_globals.myid.rank);
}

static pmix_info_t *target_procs(const char *ns, pmix_rank_t rank)
{
    pmix_info_t *dir;
    pmix_data_array_t da;
    pmix_proc_t proc;

    PMIX_LOAD_PROCID(&proc, ns, rank);
    da.type = PMIX_PROC;
    da.size = 1;
    da.array = &proc;
    PMIX_INFO_CREATE(dir, 1);
    PMIX_INFO_LOAD(&dir[0], PMIX_MONITOR_TARGET_PROCS, &da, PMIX_DATA_ARRAY);
    return dir;
}

static pmix_info_t *target_pid(pid_t pid)
{
    pmix_info_t *dir;
    pmix_data_array_t da;
    pmix_node_pid_t np;

    memset(&np, 0, sizeof(np));
    np.hostname = pmix_globals.hostname;
    np.nodeid = pmix_globals.nodeid;
    np.pid = pid;
    da.type = PMIX_NODE_PID;
    da.size = 1;
    da.array = &np;
    PMIX_INFO_CREATE(dir, 1);
    PMIX_INFO_LOAD(&dir[0], PMIX_MONITOR_TARGET_PIDS, &da, PMIX_DATA_ARRAY);
    return dir;
}

static pmix_pstat_target_t *only_target(call_t *c)
{
    if (1 != pmix_list_get_size(&c->targets)) {
        return NULL;
    }
    return (pmix_pstat_target_t *) pmix_list_get_first(&c->targets);
}

/* How many processes a query's answer reports, and whether our own pid is
 * among them */
static size_t sampled(call_t *c, bool *self)
{
    size_t n, m, count = 0;
    pmix_data_array_t *da;
    pmix_info_t *fields;
    pid_t pid;

    *self = false;
    for (n = 0; NULL != c->results && n < c->nresults; n++) {
        if (!PMIx_Check_key(c->results[n].key, PMIX_PROC_RESOURCE_USAGE) ||
            PMIX_DATA_ARRAY != c->results[n].value.type) {
            continue;
        }
        ++count;
        da = c->results[n].value.data.darray;
        fields = (pmix_info_t *) da->array;
        for (m = 0; m < da->size; m++) {
            if (PMIx_Check_key(fields[m].key, PMIX_PROC_PID) &&
                PMIX_SUCCESS == PMIx_Value_get_number(&fields[m].value, &pid, PMIX_PID) &&
                getpid() == pid) {
                *self = true;
            }
        }
    }
    return count;
}

/* ------------------------------------------------------------------ */

static void test_named(void)
{
    call_t c;
    pmix_pstat_target_t *t;
    bool self;

    /* rank 0 of our job was launched at our own pid; rank 1 has no
     * recorded pid */
    as_host(&c);
    c.dirs = target_procs(NS_MINE, PMIX_RANK_WILDCARD);
    c.ndirs = 1;
    on_progress(do_call, &c);
    t = only_target(&c);
    report("a named job covers the processes its host recorded a pid for",
           PMIX_SUCCESS == c.rc && NULL != t && t->named && 0 == t->name.rank &&
               getpid() == t->pid && geteuid() == t->owner);
    PMIX_INFO_FREE(c.dirs, c.ndirs);
    call_done(&c);

    as_host(&c);
    c.dirs = target_procs(NS_MINE, 1);
    c.ndirs = 1;
    on_progress(do_call, &c);
    report("a named process with no recorded pid is skipped",
           PMIX_SUCCESS == c.rc && 0 == pmix_list_get_size(&c.targets));
    PMIX_INFO_FREE(c.dirs, c.ndirs);
    call_done(&c);

    /* sampled at the recorded pid, as the job's owner's process */
    as_host(&c);
    c.dirs = target_procs(NS_MINE, 0);
    c.ndirs = 1;
    c.query = true;
    on_progress(do_call, &c);
    report("a named process is sampled at its recorded pid",
           PMIX_SUCCESS == c.rc && 1 == sampled(&c, &self) && self);
    PMIX_INFO_FREE(c.dirs, c.ndirs);
    call_done(&c);

    /* another user's process whose recorded pid is ours: the process
     * there is not the user the host registered it as, so it is not read */
    as_host(&c);
    c.dirs = target_procs(NS_OTHER, 0);
    c.ndirs = 1;
    c.query = true;
    on_progress(do_call, &c);
    report("a recorded pid whose process is not the registered user's is not sampled",
           PMIX_SUCCESS == c.rc && 0 == sampled(&c, &self));
    PMIX_INFO_FREE(c.dirs, c.ndirs);
    call_done(&c);
}

/* Job access. The server's own user may access every job, so the
 * requester here is another user's client - a process of NS_OTHER */
static void test_jobs(pmix_peer_t *stranger)
{
    call_t c;
    pmix_pstat_target_t *t;

    call_init(&c, stranger->info->pname.nspace, stranger->info->pname.rank);
    c.dirs = target_procs(NS_MINE, PMIX_RANK_WILDCARD);
    c.ndirs = 1;
    on_progress(do_call, &c);
    report("a client naming another user's job is refused", PMIX_ERR_NO_PERMISSIONS == c.rc);
    PMIX_INFO_FREE(c.dirs, c.ndirs);
    call_done(&c);

    /* "every process" leaves it out */
    call_init(&c, stranger->info->pname.nspace, stranger->info->pname.rank);
    on_progress(do_call, &c);
    t = only_target(&c);
    report("every process, for a client, is only those of jobs it may access",
           PMIX_SUCCESS == c.rc && NULL != t && 0 == strcmp(t->name.nspace, NS_OTHER));
    call_done(&c);
}

/* Bare pids. These are the requester's by ownership, which the kernel
 * reports - so the requester runs as our own user */
static void test_pids(pmix_peer_t *me)
{
    call_t c;
    pmix_pstat_target_t *t;
    bool self;

    /* a bare pid of its own */
    call_init(&c, me->info->pname.nspace, me->info->pname.rank);
    c.dirs = target_pid(getpid());
    c.ndirs = 1;
    c.query = true;
    on_progress(do_call, &c);
    report("a client may monitor a process it owns",
           PMIX_SUCCESS == c.rc && 1 == sampled(&c, &self) && self);
    PMIX_INFO_FREE(c.dirs, c.ndirs);
    call_done(&c);

    /* a bare pid of another user's - pid 1 is root's */
    if (0 != geteuid()) {
        call_init(&c, me->info->pname.nspace, me->info->pname.rank);
        c.dirs = target_pid(1);
        c.ndirs = 1;
        on_progress(do_call, &c);
        report("a client may not monitor a process it does not own",
               PMIX_ERR_NO_PERMISSIONS == c.rc);
        PMIX_INFO_FREE(c.dirs, c.ndirs);
        call_done(&c);

        as_host(&c);
        c.dirs = target_pid(1);
        c.ndirs = 1;
        on_progress(do_call, &c);
        t = only_target(&c);
        report("the host may monitor any process", PMIX_SUCCESS == c.rc && NULL != t &&
                                                       1 == t->pid && 0 == t->owner);
        PMIX_INFO_FREE(c.dirs, c.ndirs);
        call_done(&c);
    }

    /* -1: every process with the requester's uid, found when sampled */
    call_init(&c, me->info->pname.nspace, me->info->pname.rank);
    c.dirs = target_pid(-1);
    c.ndirs = 1;
    on_progress(do_call, &c);
    t = only_target(&c);
    report("pid -1 is every process with the requester's uid",
           PMIX_SUCCESS == c.rc && NULL != t && -1 == t->pid && geteuid() == t->owner);
    /* named twice, sampled once */
    c.rc = PMIX_ERROR;
    on_progress(do_call, &c);
    report("pid -1 named twice is one target",
           PMIX_SUCCESS == c.rc && 1 == pmix_list_get_size(&c.targets));
    PMIX_INFO_FREE(c.dirs, c.ndirs);
    call_done(&c);
    call_init(&c, me->info->pname.nspace, me->info->pname.rank);
    c.dirs = target_pid(-1);
    c.ndirs = 1;
    c.query = true;
    on_progress(do_call, &c);
    report("pid -1 samples the requester's own processes",
           PMIX_SUCCESS == c.rc && 1 <= sampled(&c, &self) && self);
    PMIX_INFO_FREE(c.dirs, c.ndirs);
    call_done(&c);
}

static void test_relay(void)
{
    call_t c;
    pmix_proc_t remote;
    pmix_info_t *dirs;
    /* a user who owns neither job */
    uint32_t uid = (uint32_t) geteuid() + 2;

    /* relayed, but not saying for whom: not the host's own */
    PMIX_LOAD_PROCID(&remote, "pstp-remote", 0);
    call_init(&c, "pstp-remote", 0);
    PMIX_INFO_CREATE(dirs, 1);
    PMIX_INFO_LOAD(&dirs[0], PMIX_MONITOR_PROXY, &remote, PMIX_PROC);
    c.dirs = dirs;
    c.ndirs = 1;
    on_progress(do_call, &c);
    report("a relayed request that does not say for whom is refused",
           PMIX_ERR_NO_PERMISSIONS == c.rc);
    PMIX_INFO_FREE(dirs, 1);
    call_done(&c);

    /* relayed for another user: our job is not theirs */
    call_init(&c, "pstp-remote", 0);
    PMIX_INFO_CREATE(dirs, 2);
    PMIX_INFO_LOAD(&dirs[0], PMIX_MONITOR_PROXY, &remote, PMIX_PROC);
    PMIX_INFO_LOAD(&dirs[1], PMIX_USERID, &uid, PMIX_UINT32);
    c.dirs = dirs;
    c.ndirs = 2;
    on_progress(do_call, &c);
    report("a relayed request is held to the user it is for",
           PMIX_SUCCESS == c.rc && 0 == pmix_list_get_size(&c.targets));
    PMIX_INFO_FREE(dirs, 2);
    call_done(&c);
}

static void lost(void *arg)
{
    pmix_pstat_base_peer_lost((struct pmix_peer_t *) arg);
}

static void start_monitor(pmix_peer_t *me)
{
    call_t c;
    uint32_t rate = 3600;

    call_init(&c, me->info->pname.nspace, me->info->pname.rank);
    PMIX_INFO_CREATE(c.dirs, 2);
    PMIX_INFO_LOAD(&c.dirs[0], PMIX_MONITOR_RESOURCE_RATE, &rate, PMIX_UINT32);
    PMIX_INFO_LOAD(&c.dirs[1], PMIX_MONITOR_ID, "pstp-mon", PMIX_STRING);
    c.ndirs = 2;
    c.query = true;
    on_progress(do_call, &c);
    PMIX_INFO_FREE(c.dirs, c.ndirs);
    call_done(&c);
}

static void test_departure(pmix_peer_t *me)
{
    pmix_peer_t *clone;

    start_monitor(me);
    report("a periodic monitor is kept", 1 == pmix_list_get_size(&pmix_pstat_base.ops));

    /* a fork/exec'd clone shares the requester's name: the monitor is the
     * name's until the last of them leaves */
    clone = make_peer(NS_MINE, me->info->pname.rank, me->info->uid, me->info->gid);
    on_progress(lost, me);
    report("a monitor outlives its requester while a clone is connected",
           1 == pmix_list_get_size(&pmix_pstat_base.ops));
    drop_peer(clone);
    on_progress(lost, me);
    report("a monitor ends when its requester leaves",
           0 == pmix_list_get_size(&pmix_pstat_base.ops));
}

int main(int argc, char **argv)
{
    static pmix_server_module_t mymodule = {0};
    pmix_peer_t *me, *stranger;
    pmix_status_t rc;

    (void) argc;
    (void) argv;

    fprintf(stdout, "pstat_peers: which processes a monitor samples\n");

    rc = PMIx_server_init(&mymodule, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "PMIx_server_init failed: %s\n", PMIx_Error_string(rc));
        return 1;
    }
    if (PMIX_SUCCESS != reg(NS_MINE, geteuid(), getegid()) ||
        PMIX_SUCCESS != reg(NS_OTHER, geteuid() + 1, getegid() + 1)) {
        fprintf(stderr, "could not register the jobs\n");
        PMIx_server_finalize();
        return 1;
    }
    record_pid(NS_MINE, 0, getpid());
    record_pid(NS_OTHER, 0, getpid());

    me = make_peer(NS_MINE, 0, geteuid(), getegid());
    stranger = make_peer(NS_OTHER, 1, geteuid() + 1, getegid() + 1);
    if (NULL == me || NULL == stranger) {
        fprintf(stderr, "could not create the requester\n");
        PMIx_server_finalize();
        return 1;
    }

    test_named();
    test_jobs(stranger);
    test_pids(me);
    test_relay();
    test_departure(me);

    drop_peer(stranger);
    drop_peer(me);
    PMIx_server_finalize();

    fprintf(stdout, "pstat_peers: %d passed, %d failed\n", npass, nfail);
    return (0 == nfail) ? 0 : 1;
}
