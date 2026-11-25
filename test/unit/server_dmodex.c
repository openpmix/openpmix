/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * White-box unit tests for src/server/pmix_server_dmodex.c - the two data
 * calls the host makes down into us.
 *
 * The case that matters is the deferral. PMIx_server_dmodex_request asks
 * for the modex data of one of our local clients on behalf of a remote
 * server; if that client has not committed its data yet, the request is
 * parked on pmix_server_globals.remote_pnd and the host's response
 * function is not called. The *only* thing that ever took a request back
 * off that list was the target committing - so a target that departs
 * without committing (it aborted, it never called PMIx_Put, its namespace
 * was torn down) left the host holding a request it would never hear
 * about again, and the remote client blocked in PMIx_Get behind it waited
 * forever. The mirror image on the other deferral list, local_reqs, has
 * been handled by pmix_server_purge_events for some time; this is the
 * same rule applied to the list going the other way.
 *
 * Finalize is the second way to reach that state, and it looks correct
 * where the departure case looked wrong. A bare PMIX_LIST_DESTRUCT of
 * remote_pnd frees everything on it - the entry's destructor releases
 * the caddy - so it leaks nothing. What it drops is the response
 * function, which is the only thing that will ever tell the host what
 * became of its request, and the host process outlives our finalize. So
 * the last two cases park a request and then finalize, and the answer is
 * the only observable: the list itself is gone by the time they run.
 *
 * Ordering here is deterministic rather than timed. PMIx_server_dmodex_request
 * thread-shifts and returns, so the parking happens on the progress
 * thread; PMIx_Store_internal is a blocking call onto that same thread,
 * so when it returns, everything queued ahead of it - including our
 * deferred request - has already run. PMIx_server_deregister_nspace is
 * then driven in its blocking form, so purge_events has completed by the
 * time it returns and the response function has either fired or never
 * will. No sleeps, no polling.
 *
 * Test cases:
 *
 *   store internal, then read it back      -> the value survives the xfer
 *   store internal with a NULL value       -> PMIX_ERR_BAD_PARAM
 *   dmodex for an uncommitted local rank   -> parked, host not answered
 *   that rank's nspace is deregistered     -> host answered with an error
 *   a rank that put at every scope         -> the dmodex answer carries its
 *                                             PMIX_REMOTE and PMIX_GLOBAL
 *                                             data and not its PMIX_LOCAL data,
 *                                             while this server's own PMIx_Get
 *                                             of the same rank - on its node -
 *                                             sees the PMIX_LOCAL value
 *
 * That last case holds two readers of one datastore apart. A PMIx_Get
 * applies the visibility rule of PMIx_Put(3) - a target on our node shows
 * its PMIX_LOCAL and PMIX_GLOBAL data only - while the direct-modex answer
 * is for a process on ANOTHER node and must read the target's PMIX_REMOTE
 * table exactly as stored. The rule is switched on per fetch
 * (pmix_cb_t.get_visibility); turning it on for the dmodex fetch would
 * hand every other node nothing but the target's PMIX_GLOBAL data, and
 * turning it off for the get would hand our own node the PMIX_REMOTE copy.
 */

#include "src/include/pmix_config.h"

#include "include/pmix.h"
#include "include/pmix_server.h"

#include "src/class/pmix_list.h"
#include "src/include/pmix_globals.h"
#include "src/server/pmix_server_ops.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DMUT_NSPACE "server-dmodex-ut"
#define DMUT_NPROCS 2

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

/* what the host's dmodex response function was told */
static bool dm_fired = false;
static pmix_status_t dm_status = PMIX_SUCCESS;

static char *dm_data = NULL;
static size_t dm_size = 0;

static void dmresponse(pmix_status_t status, char *data, size_t sz, void *cbdata)
{
    (void) cbdata;

    dm_fired = true;
    dm_status = status;
    /* the server frees the blob when we return */
    free(dm_data);
    dm_data = NULL;
    dm_size = 0;
    if (NULL != data && 0 < sz) {
        dm_data = (char *) malloc(sz);
        if (NULL != dm_data) {
            memcpy(dm_data, data, sz);
            dm_size = sz;
        }
    }
}


static size_t nparked(void)
{
    return pmix_list_get_size(&pmix_server_globals.remote_pnd);
}

/* A blocking round trip through the progress thread. Anything queued onto
 * that thread before this call has been serviced by the time it returns. */
static void progress_barrier(void)
{
    pmix_value_t v;

    PMIX_VALUE_LOAD(&v, "barrier", PMIX_STRING);
    PMIx_Store_internal(&pmix_globals.myid, "server-dmodex-ut.barrier", &v);
    PMIX_VALUE_DESTRUCT(&v);
}

static pmix_status_t register_job(void)
{
    pmix_info_t info[4];
    pmix_nspace_t ns;
    pmix_proc_t proc;
    pmix_status_t rc;
    uint32_t nprocs = DMUT_NPROCS;
    pmix_rank_t r;
    char *nodes = NULL, *ppn = NULL;
    size_t n;

    /* every rank on this node, by name, so a datastore can tell where
     * each one is (see "Who is on my node" in data_scope.rst). Without
     * the maps the visibility rule would have nothing to go on, and the
     * cases that hold it apart from the host's own reads would pass
     * against a library that applied it to both */
    rc = PMIx_generate_regex(pmix_globals.hostname, &nodes);
    if (PMIX_SUCCESS == rc) {
        rc = PMIx_generate_ppn("0,1", &ppn);
    }
    if (PMIX_SUCCESS != rc) {
        free(nodes);
        return rc;
    }
    PMIX_INFO_LOAD(&info[0], PMIX_JOB_SIZE, &nprocs, PMIX_UINT32);
    PMIX_INFO_LOAD(&info[1], PMIX_UNIV_SIZE, &nprocs, PMIX_UINT32);
    PMIX_INFO_LOAD(&info[2], PMIX_NODE_MAP, nodes, PMIX_REGEX);
    PMIX_INFO_LOAD(&info[3], PMIX_PROC_MAP, ppn, PMIX_REGEX);
    free(nodes);
    free(ppn);

    PMIX_LOAD_NSPACE(ns, DMUT_NSPACE);
    rc = PMIx_server_register_nspace(ns, DMUT_NPROCS, info, 4, NULL, NULL);
    if (PMIX_OPERATION_SUCCEEDED == rc) {
        rc = PMIX_SUCCESS;
    }
    for (n = 0; n < 4; n++) {
        PMIX_INFO_DESTRUCT(&info[n]);
    }
    if (PMIX_SUCCESS != rc) {
        return rc;
    }

    for (r = 0; r < DMUT_NPROCS; r++) {
        PMIX_LOAD_PROCID(&proc, DMUT_NSPACE, r);
        rc = PMIx_server_register_client(&proc, geteuid(), getegid(), NULL, NULL, NULL);
        if (PMIX_SUCCESS != rc && PMIX_OPERATION_SUCCEEDED != rc) {
            return rc;
        }
    }
    return PMIX_SUCCESS;
}

/* --------------------------------------------------------------------
 * One rank's data at every scope, read three ways.
 *
 * The values are stored the way pmix_server_commit stores a client's
 * commit: into this server's own datastore, at the scope the client put
 * them. "dual" is put twice, with a different value at each of
 * PMIX_LOCAL and PMIX_REMOTE. */
#define SC_DUAL   "server-dmodex-ut.dual"
#define SC_REMOTE "server-dmodex-ut.remote_only"
#define SC_LOCAL  "server-dmodex-ut.local_only"
#define SC_GLOBAL "server-dmodex-ut.glob"

typedef struct {
    pmix_event_t ev;
    pmix_lock_t lock;
    pmix_rank_t rank;
    bool ok;
} scope_store_t;

static bool store_one(const pmix_proc_t *p, pmix_scope_t scope, const char *key,
                      const char *value)
{
    pmix_kval_t kv;
    pmix_value_t v;
    pmix_status_t rc;

    PMIX_VALUE_LOAD(&v, value, PMIX_STRING);
    kv.key = (char *) key;
    kv.value = &v;
    PMIX_GDS_STORE_KV(rc, pmix_globals.mypeer, p, scope, &kv);
    PMIX_VALUE_DESTRUCT(&v);
    return PMIX_SUCCESS == rc;
}

static void store_scopes(int sd, short args, void *cbdata)
{
    scope_store_t *ss = (scope_store_t *) cbdata;
    pmix_namespace_t *ns, *nptr = NULL;
    pmix_rank_info_t *rinfo;
    pmix_proc_t p;

    (void) sd;
    (void) args;

    ss->ok = false;
    PMIX_LOAD_PROCID(&p, DMUT_NSPACE, ss->rank);
    if (!store_one(&p, PMIX_LOCAL, SC_DUAL, "L") || !store_one(&p, PMIX_REMOTE, SC_DUAL, "R") ||
        !store_one(&p, PMIX_REMOTE, SC_REMOTE, "R") ||
        !store_one(&p, PMIX_LOCAL, SC_LOCAL, "L") ||
        !store_one(&p, PMIX_GLOBAL, SC_GLOBAL, "G")) {
        goto done;
    }
    /* and mark the commit as received, so a dmodex request is answered
     * rather than parked */
    PMIX_LIST_FOREACH (ns, &pmix_globals.nspaces, pmix_namespace_t) {
        if (0 == strcmp(ns->nspace, DMUT_NSPACE)) {
            nptr = ns;
            break;
        }
    }
    if (NULL == nptr) {
        goto done;
    }
    PMIX_LIST_FOREACH (rinfo, &nptr->ranks, pmix_rank_info_t) {
        if (ss->rank == rinfo->pname.rank) {
            rinfo->modex_recvd = true;
            ss->ok = true;
            break;
        }
    }

done:
    PMIX_WAKEUP_THREAD(&ss->lock);
}

/* how many times "key" appears in the dmodex answer, and the value of the
 * last one */
static int blob_count(const char *key, char *value, size_t vlen)
{
    pmix_buffer_t buf;
    pmix_kval_t *kv;
    pmix_status_t rc;
    int32_t cnt;
    char *copy;
    int n = 0;

    if (NULL == dm_data || 0 == dm_size) {
        return 0;
    }
    copy = (char *) malloc(dm_size);
    if (NULL == copy) {
        return -1;
    }
    memcpy(copy, dm_data, dm_size);
    PMIX_CONSTRUCT(&buf, pmix_buffer_t);
    /* through a local: PMIX_LOAD_BUFFER clears the size it is handed */
    {
        size_t sz = dm_size;
        PMIX_LOAD_BUFFER(pmix_globals.mypeer, &buf, copy, sz);
    }
    kv = PMIX_NEW(pmix_kval_t);
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, pmix_globals.mypeer, &buf, kv, &cnt, PMIX_KVAL);
    while (PMIX_SUCCESS == rc) {
        if (NULL != kv->key && 0 == strcmp(kv->key, key)) {
            ++n;
            if (NULL != kv->value && PMIX_STRING == kv->value->type &&
                NULL != kv->value->data.string) {
                snprintf(value, vlen, "%s", kv->value->data.string);
            }
        }
        PMIX_RELEASE(kv);
        kv = PMIX_NEW(pmix_kval_t);
        cnt = 1;
        PMIX_BFROPS_UNPACK(rc, pmix_globals.mypeer, &buf, kv, &cnt, PMIX_KVAL);
    }
    PMIX_RELEASE(kv);
    PMIX_DESTRUCT(&buf);
    return n;
}

/* this server's own PMIx_Get of a key, as a string; "" when not found */
static void own_get(const pmix_proc_t *p, const char *key, int scope, char *value, size_t vlen,
                    pmix_status_t *status)
{
    pmix_info_t info[2];
    size_t ninfo = 0;
    pmix_scope_t sc = (pmix_scope_t) scope;
    pmix_value_t *val = NULL;
    bool optional = true;

    value[0] = '\0';
    /* only what is here: the target is local, so nothing would arrive */
    PMIX_INFO_LOAD(&info[ninfo], PMIX_OPTIONAL, &optional, PMIX_BOOL);
    ninfo++;
    if (0 <= scope) {
        PMIX_INFO_LOAD(&info[ninfo], PMIX_DATA_SCOPE, &sc, PMIX_SCOPE);
        ninfo++;
    }
    *status = PMIx_Get(p, key, info, ninfo, &val);
    PMIX_INFO_DESTRUCT(&info[0]);
    if (1 < ninfo) {
        PMIX_INFO_DESTRUCT(&info[1]);
    }
    if (NULL != val) {
        if (PMIX_STRING == val->type && NULL != val->data.string) {
            snprintf(value, vlen, "%s", val->data.string);
        } else {
            snprintf(value, vlen, "<type %s>", PMIx_Data_type_string(val->type));
        }
        PMIX_VALUE_RELEASE(val);
    }
}

/* PMIx_server_dmodex_request2 naming one PMIX_REQUIRED_KEY, as a server
 * asking on behalf of a PMIx_Get does (pmix_server_get) */
static void required_request(const pmix_proc_t *target, const char *key, pmix_status_t *rc)
{
    pmix_info_t info;

    dm_fired = false;
    dm_status = PMIX_ERROR;
    PMIX_INFO_LOAD(&info, PMIX_REQUIRED_KEY, key, PMIX_STRING);
    *rc = PMIx_server_dmodex_request2(target, &info, 1, dmresponse, NULL);
    progress_barrier();
    PMIX_INFO_DESTRUCT(&info);
}

static void test_scope_readers(void)
{
    scope_store_t ss;
    pmix_proc_t target;
    pmix_status_t rc;
    char value[64];

    memset(&ss, 0, sizeof(ss));
    PMIX_CONSTRUCT_LOCK(&ss.lock);
    ss.rank = 0;
    PMIX_THREADSHIFT(&ss, store_scopes);
    PMIX_WAIT_THREAD(&ss.lock);
    PMIX_DESTRUCT_LOCK(&ss.lock);
    report("a rank's data stored at every scope", ss.ok);
    if (!ss.ok) {
        return;
    }

    /* for another node: exactly the target's PMIX_REMOTE table */
    dm_fired = false;
    dm_status = PMIX_ERROR;
    PMIX_LOAD_PROCID(&target, DMUT_NSPACE, 0);
    rc = PMIx_server_dmodex_request(&target, dmresponse, NULL);
    progress_barrier();
    report("dmodex answers a committed rank", PMIX_SUCCESS == rc && dm_fired &&
                                                  PMIX_SUCCESS == dm_status);
    value[0] = '\0';
    report("dmodex carries the PMIX_REMOTE copy of a key put at both scopes, once",
           1 == blob_count(SC_DUAL, value, sizeof(value)) && 0 == strcmp(value, "R"));
    report("dmodex carries a PMIX_REMOTE-only key", 1 == blob_count(SC_REMOTE, value,
                                                                     sizeof(value)));
    report("dmodex carries a PMIX_GLOBAL key", 1 == blob_count(SC_GLOBAL, value,
                                                                sizeof(value)));
    report("dmodex does not carry a PMIX_LOCAL-only key", 0 == blob_count(SC_LOCAL, value,
                                                                            sizeof(value)));

    /* Our own PMIx_Get - the host's. The host is not a process of the
     * job, so it reads the data as stored rather than through the
     * visibility rule (see "Where the rules are applied" in
     * docs/how-things-work/data_scope.rst). Its direct-modex check needs
     * exactly that: it runs on the target's node and looks for the data
     * the target posted for OTHER nodes. Applying the rule here is what
     * hung every cross-node direct modex for a PMIX_REMOTE key. */
    own_get(&target, SC_DUAL, -1, value, sizeof(value), &rc);
    report("host get of a key put at both scopes finds the PMIX_LOCAL value first",
           PMIX_SUCCESS == rc && 0 == strcmp(value, "L"));
    own_get(&target, SC_DUAL, PMIX_REMOTE, value, sizeof(value), &rc);
    report("host PMIX_REMOTE get of it finds the PMIX_REMOTE value",
           PMIX_SUCCESS == rc && 0 == strcmp(value, "R"));
    own_get(&target, SC_REMOTE, -1, value, sizeof(value), &rc);
    report("host get of a PMIX_REMOTE-only key finds it",
           PMIX_SUCCESS == rc && 0 == strcmp(value, "R"));
    own_get(&target, SC_GLOBAL, PMIX_REMOTE, value, sizeof(value), &rc);
    report("host PMIX_REMOTE get of a PMIX_GLOBAL key finds it",
           PMIX_SUCCESS == rc && 0 == strcmp(value, "G"));
    own_get(&target, SC_LOCAL, PMIX_LOCAL, value, sizeof(value), &rc);
    report("host PMIX_LOCAL get of a PMIX_LOCAL-only key finds it",
           PMIX_SUCCESS == rc && 0 == strcmp(value, "L"));
    free(dm_data);
    dm_data = NULL;
    dm_size = 0;

    /* A request naming the key its reader needs. The target has
     * committed, but that is not enough: the key must be there for other
     * nodes to see (pmix_server_dmodex_answer). */
    required_request(&target, SC_REMOTE, &rc);
    report("dmodex naming a PMIX_REMOTE-only key answers with the data",
           PMIX_SUCCESS == rc && dm_fired && PMIX_SUCCESS == dm_status &&
               1 == blob_count(SC_REMOTE, value, sizeof(value)));
    free(dm_data);
    dm_data = NULL;
    dm_size = 0;
    required_request(&target, SC_LOCAL, &rc);
    report("dmodex naming a PMIX_LOCAL-only key answers outside-scope at once",
           PMIX_SUCCESS == rc && dm_fired && PMIX_ERR_EXISTS_OUTSIDE_SCOPE == dm_status);
    free(dm_data);
    dm_data = NULL;
    dm_size = 0;
    {
        size_t before = nparked();
        required_request(&target, "dmut.not.posted.yet", &rc);
        report("dmodex naming a key not posted yet waits for it",
               PMIX_SUCCESS == rc && !dm_fired && nparked() == before + 1);
    }
}

int main(int argc, char **argv)
{
    static pmix_server_module_t mymodule = {0};
    pmix_status_t rc;
    pmix_proc_t target;
    pmix_value_t val, *got = NULL;
    pmix_nspace_t ns;

    (void) argc;
    (void) argv;

    fprintf(stdout, "server_dmodex: host-facing data call unit tests\n");

    rc = PMIx_server_init(&mymodule, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "PMIx_server_init failed: %s\n", PMIx_Error_string(rc));
        return 1;
    }

    /* --- PMIx_Store_internal round trip ------------------------------ */
    PMIX_VALUE_LOAD(&val, "stored-value", PMIX_STRING);
    rc = PMIx_Store_internal(&pmix_globals.myid, "server-dmodex-ut.key", &val);
    report("store internal accepted", PMIX_SUCCESS == rc);
    PMIX_VALUE_DESTRUCT(&val);

    rc = PMIx_Get(&pmix_globals.myid, "server-dmodex-ut.key", NULL, 0, &got);
    report("stored value reads back", PMIX_SUCCESS == rc && NULL != got
                                          && PMIX_STRING == got->type
                                          && NULL != got->data.string
                                          && 0 == strcmp(got->data.string, "stored-value"));
    if (NULL != got) {
        PMIX_VALUE_RELEASE(got);
        got = NULL;
    }

    /* --- and it screens the value it is about to dereference --------- */
    rc = PMIx_Store_internal(&pmix_globals.myid, "server-dmodex-ut.bad", NULL);
    report("store internal rejects a NULL value", PMIX_ERR_BAD_PARAM == rc);

    /* --- a dmodex request for a rank that has not committed ---------- */
    rc = register_job();
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "register_job failed: %s\n", PMIx_Error_string(rc));
        PMIx_server_finalize();
        return 1;
    }

    dm_fired = false;
    PMIX_LOAD_PROCID(&target, DMUT_NSPACE, 0);
    rc = PMIx_server_dmodex_request(&target, dmresponse, NULL);
    report("dmodex request accepted", PMIX_SUCCESS == rc);
    progress_barrier();
    report("uncommitted rank defers the request", 1 == nparked());
    report("deferred request does not answer the host yet", !dm_fired);

    /* --- the target departs without ever committing ------------------ */
    PMIX_LOAD_NSPACE(ns, DMUT_NSPACE);
    PMIx_server_deregister_nspace(ns, NULL, NULL);
    progress_barrier();
    report("departure drained the deferred request", 0 == nparked());
    report("departure answered the host", dm_fired);
    report("departure answered with a failure status",
           dm_fired && PMIX_SUCCESS != dm_status);

    /* --- and a request still parked when the server finalizes --------- */
    /* remote_pnd used to be torn down with a bare PMIX_LIST_DESTRUCT,
     * which frees the caddies without ever calling the response function
     * the host supplied - so the host was left holding a request it would
     * never hear about again, and the host process outlives our finalize.
     * The local_reqs list beside it had always been drained with a status
     * for exactly this reason. */
    rc = register_job();
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "re-register_job failed: %s\n", PMIx_Error_string(rc));
        PMIx_server_finalize();
        return 1;
    }
    /* --- the readers of one rank's data at every scope ---------------- */
    test_scope_readers();

    /* measured against what is already parked: test_scope_readers()
     * leaves one request waiting for a key that is never posted */
    {
        size_t before = nparked();

        dm_fired = false;
        dm_status = PMIX_SUCCESS;
        PMIX_LOAD_PROCID(&target, DMUT_NSPACE, 1);
        rc = PMIx_server_dmodex_request(&target, dmresponse, NULL);
        report("second dmodex request accepted", PMIX_SUCCESS == rc);
        progress_barrier();
        report("second request is parked", before + 1 == nparked());
    }

    PMIx_server_finalize();

    /* the list is gone by now, so the answer is the only observable */
    report("finalize answered the parked request", dm_fired);
    report("finalize answered with a failure status",
           dm_fired && PMIX_SUCCESS != dm_status);

    fprintf(stdout, "server_dmodex: %d passed, %d failed\n", npass, nfail);
    return (0 == nfail) ? 0 : 1;
}
