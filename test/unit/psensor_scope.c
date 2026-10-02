/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/*
 * What a liveness monitor may watch, and how many a process may hold.
 *
 * A file monitor stats its file with the server's privilege, so only a
 * file the requester owns is watched; any other is treated exactly as a
 * file that is not there - the monitor starts, and never alerts. Each
 * process may hold at most psensor_base_max_monitors_per_peer monitors
 * across the heartbeat and file components.
 *
 * This runs a real server with a stub host module and calls psensor on
 * the progress thread, where the server calls it. The alerts are raised
 * with PMIX_RANGE_PROC_LOCAL, so this process's own handlers see them.
 */

#include "src/include/pmix_config.h"

#include "include/pmix_server.h"
#include "src/include/pmix_globals.h"
#include "src/mca/psensor/base/base.h"
#include "src/mca/psensor/psensor.h"
#include "src/server/pmix_server_ops.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define NS_MINE "pss-mine"

/* the codes the two file monitors raise, so an alert says which fired */
#define CODE_OWNED   (PMIX_EXTERNAL_ERR_BASE - 11)
#define CODE_UNOWNED (PMIX_EXTERNAL_ERR_BASE - 12)

static int npass = 0;
static int nfail = 0;
static volatile int owned_alerts = 0;
static volatile int unowned_alerts = 0;

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

static void nothing(void *arg)
{
    (void) arg;
}

/* ------------------------------------------------------------------ */
/* the job and peers                                                   */
/* ------------------------------------------------------------------ */

static pmix_status_t reg(const char *name)
{
    pmix_info_t info[4];
    pmix_nspace_t ns;
    pmix_status_t rc;
    char *noderegex = NULL, *ppnregex = NULL;
    uint32_t nprocs = 2;
    size_t n;

    PMIx_generate_regex(pmix_globals.hostname, &noderegex);
    PMIx_generate_ppn("0,1", &ppnregex);
    PMIX_INFO_LOAD(&info[0], PMIX_NODE_MAP, noderegex, PMIX_REGEX);
    PMIX_INFO_LOAD(&info[1], PMIX_PROC_MAP, ppnregex, PMIX_REGEX);
    PMIX_INFO_LOAD(&info[2], PMIX_JOB_SIZE, &nprocs, PMIX_UINT32);
    PMIX_INFO_LOAD(&info[3], PMIX_UNIV_SIZE, &nprocs, PMIX_UINT32);
    free(noderegex);
    free(ppnregex);
    PMIX_LOAD_NSPACE(ns, name);
    rc = PMIx_server_register_nspace(ns, 2, info, 4, NULL, NULL);
    for (n = 0; n < 4; n++) {
        PMIX_INFO_DESTRUCT(&info[n]);
    }
    return (PMIX_OPERATION_SUCCEEDED == rc) ? PMIX_SUCCESS : rc;
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
    p->sd = -1;
    p->index = pmix_pointer_array_add(&pmix_server_globals.clients, p);
    return p;
}

static void drop_peer(pmix_peer_t *p)
{
    pmix_pointer_array_set_item(&pmix_server_globals.clients, p->index, NULL);
    PMIX_RELEASE(p);
}

/* ------------------------------------------------------------------ */
/* starting and stopping monitors, on the progress thread              */
/* ------------------------------------------------------------------ */

typedef struct {
    pmix_peer_t *peer;
    const char *file;     // a file monitor of this path; NULL - a heartbeat
    pmix_status_t code;   // the status the monitor raises
    uint32_t interval;
    pmix_status_t rc;
} start_t;

static void do_start(void *arg)
{
    start_t *st = (start_t *) arg;
    pmix_info_t monitor, dirs[4];
    pmix_data_range_t range = PMIX_RANGE_PROC_LOCAL;
    bool yes = true;
    size_t n, ndirs;

    if (NULL != st->file) {
        PMIX_INFO_LOAD(&monitor, PMIX_MONITOR_FILE, st->file, PMIX_STRING);
        PMIX_INFO_LOAD(&dirs[0], PMIX_MONITOR_FILE_MODIFY, &yes, PMIX_BOOL);
        PMIX_INFO_LOAD(&dirs[1], PMIX_MONITOR_FILE_CHECK_TIME, &st->interval, PMIX_UINT32);
        PMIX_INFO_LOAD(&dirs[2], PMIX_RANGE, &range, PMIX_DATA_RANGE);
        ndirs = 3;
    } else {
        PMIX_INFO_LOAD(&monitor, PMIX_MONITOR_HEARTBEAT, NULL, PMIX_POINTER);
        PMIX_INFO_LOAD(&dirs[0], PMIX_MONITOR_HEARTBEAT_TIME, &st->interval, PMIX_UINT32);
        PMIX_INFO_LOAD(&dirs[1], PMIX_RANGE, &range, PMIX_DATA_RANGE);
        ndirs = 2;
    }
    st->rc = pmix_psensor.start(st->peer, st->code, &monitor, dirs, ndirs);
    PMIX_INFO_DESTRUCT(&monitor);
    for (n = 0; n < ndirs; n++) {
        PMIX_INFO_DESTRUCT(&dirs[n]);
    }
}

static pmix_status_t start_one(pmix_peer_t *peer, const char *file, pmix_status_t code,
                               uint32_t interval)
{
    start_t st;

    st.peer = peer;
    st.file = file;
    st.code = code;
    st.interval = interval;
    st.rc = PMIX_ERROR;
    on_progress(do_start, &st);
    return st.rc;
}

static void do_stop(void *arg)
{
    (void) pmix_psensor.stop((pmix_peer_t *) arg, NULL);
}

/* stop every monitor peer holds, and let the removal land */
static void stop_all(pmix_peer_t *peer)
{
    on_progress(do_stop, peer);
    on_progress(nothing, NULL);
}

/* ------------------------------------------------------------------ */

static void alert(size_t evhdlr_registration_id, pmix_status_t status,
                  const pmix_proc_t *source, pmix_info_t info[], size_t ninfo,
                  pmix_info_t results[], size_t nresults,
                  pmix_event_notification_cbfunc_fn_t cbfunc, void *cbdata)
{
    (void) evhdlr_registration_id;
    (void) source;
    (void) info;
    (void) ninfo;
    (void) results;
    (void) nresults;
    if (CODE_OWNED == status) {
        ++owned_alerts;
    } else if (CODE_UNOWNED == status) {
        ++unowned_alerts;
    }
    if (NULL != cbfunc) {
        cbfunc(PMIX_EVENT_ACTION_COMPLETE, NULL, 0, NULL, NULL, cbdata);
    }
}

/* A file that never changes alerts its owner's monitor; the same file
 * watched for another user never alerts, though its monitor starts */
static void test_owner(pmix_peer_t *me, pmix_peer_t *stranger, const char *path)
{
    pmix_status_t codes[2] = {CODE_OWNED, CODE_UNOWNED};
    pmix_status_t rc;
    int k;

    rc = PMIx_Register_event_handler(codes, 2, NULL, 0, alert, NULL, NULL);
    if (0 > rc) {
        report("register the alert handler", 0);
        return;
    }

    report("a monitor of a file the requester owns starts",
           PMIX_SUCCESS == start_one(me, path, CODE_OWNED, 1));
    report("a monitor of another user's file starts all the same",
           PMIX_SUCCESS == start_one(stranger, path, CODE_UNOWNED, 1));

    /* the owner's alert is the sentinel: once it has fired, the other
     * monitor has had every sample it needed to fire as well */
    for (k = 0; k < 100 && 0 == owned_alerts; k++) {
        usleep(100000);
    }
    usleep(2500000);
    report("the owner's monitor of an unchanging file alerts", 1 == owned_alerts);
    report("another user's monitor of the same file never alerts", 0 == unowned_alerts);
    stop_all(stranger);
}

/* each process may hold only so many monitors, of either kind */
static void test_cap(pmix_peer_t *me, pmix_peer_t *other, const char *path)
{
    int k, max = pmix_psensor_base.max_per_peer;
    bool ok = true;

    for (k = 0; k < max; k++) {
        /* alternate the kinds - the cap counts both */
        if (PMIX_SUCCESS != start_one(me, (k % 2) ? NULL : path, PMIX_SUCCESS, 3600)) {
            ok = false;
        }
    }
    report("a process may hold up to the limit of monitors", ok);
    report("a file monitor past the limit is refused",
           PMIX_ERR_OUT_OF_RESOURCE == start_one(me, path, PMIX_SUCCESS, 3600));
    report("a heartbeat monitor past the limit is refused",
           PMIX_ERR_OUT_OF_RESOURCE == start_one(me, NULL, PMIX_SUCCESS, 3600));
    report("another process is not held to the first one's limit",
           PMIX_SUCCESS == start_one(other, path, PMIX_SUCCESS, 3600));

    stop_all(me);
    report("stopping a process's monitors frees its limit",
           PMIX_SUCCESS == start_one(me, path, PMIX_SUCCESS, 3600));
    stop_all(me);
    stop_all(other);
}

int main(int argc, char **argv)
{
    static pmix_server_module_t mymodule = {0};
    pmix_peer_t *me, *stranger, *other;
    char dir[] = "/tmp/pss.XXXXXX";
    char path[64];
    FILE *fp;
    pmix_status_t rc;

    (void) argc;
    (void) argv;

    fprintf(stdout, "psensor_scope: what a liveness monitor may watch\n");

    if (NULL == mkdtemp(dir)) {
        fprintf(stderr, "could not make a directory\n");
        return 1;
    }
    snprintf(path, sizeof(path), "%s/watched", dir);
    fp = fopen(path, "w");
    if (NULL == fp) {
        fprintf(stderr, "could not make the watched file\n");
        return 1;
    }
    fprintf(fp, "unchanging\n");
    fclose(fp);

    rc = PMIx_server_init(&mymodule, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "PMIx_server_init failed: %s\n", PMIx_Error_string(rc));
        return 1;
    }
    if (PMIX_SUCCESS != reg(NS_MINE)) {
        fprintf(stderr, "could not register the job\n");
        PMIx_server_finalize();
        return 1;
    }
    me = make_peer(NS_MINE, 0, geteuid(), getegid());
    stranger = make_peer(NS_MINE, 1, geteuid() + 1, getegid() + 1);
    other = make_peer(NS_MINE, 1, geteuid(), getegid());
    if (NULL == me || NULL == stranger || NULL == other) {
        fprintf(stderr, "could not create the requesters\n");
        PMIx_server_finalize();
        return 1;
    }

    test_owner(me, stranger, path);
    test_cap(me, other, path);

    drop_peer(other);
    drop_peer(stranger);
    drop_peer(me);
    PMIx_server_finalize();
    unlink(path);
    rmdir(dir);

    fprintf(stdout, "psensor_scope: %d passed, %d failed\n", npass, nfail);
    return (0 == nfail) ? 0 : 1;
}
