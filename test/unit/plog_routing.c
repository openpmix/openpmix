/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Unit tests for the plog routing engine and its module contract.
 *
 * The process comes up as a non-gateway PMIx server with no host log
 * function registered, which is the configuration in which PMIx_Log
 * falls through to the local plog framework and hands its status back
 * to the caller unchanged. That makes the framework's synchronous
 * return code observable, which is what most of these cases check.
 *
 * The environment is set up before PMIx_server_init so that
 * plog_base_order names a single module. That parameter is parsed by
 * the framework's register function, which the MCA base runs *before*
 * the framework's open function - a regression here (open clearing the
 * parsed list) silently ignores the user's ordering, so the first test
 * looks at the resolved order directly.
 *
 * Test cases:
 *
 *   plog_base_order is honored, and may name a channel ("stdout")
 *   rather than the module that services it ("stdfd").
 *
 *   a channel no active module services -> PMIX_ERR_NOT_AVAILABLE.
 *
 *   a stdout/stderr entry whose value is not a string is rejected
 *   rather than handed to strlen().
 *
 *   an empty stdout string is accepted without being forwarded as a
 *   zero-length payload (which is how IOF is told a stream closed).
 *
 *   an aggregated duplicate reports success, not "not available" -
 *   the latter makes a client log the message a second time itself.
 *
 *   duplicate suppression (PMIX_LOG_AGG with PMIX_LOG_KEY/PMIX_LOG_VAL):
 *
 *     plog_base_max_suppressed bounds the number of recorded messages.
 *     A new one past the limit drops the one matched least recently -
 *     so the case checks both that the oldest entry goes and that a
 *     match keeps an entry from being the one that goes. This case runs
 *     first, while nothing else has been recorded, and is kept free of
 *     anything that would report accumulated duplicates in the middle
 *     of it: such a report is itself logged, and so recorded, which
 *     would change what the table holds.
 *
 *     a log request a server receives from a client is recorded under
 *     that client's namespace, whatever PMIX_NSPACE the request named -
 *     both when the server logs it itself (as the gateway) and when it
 *     passes it to its host.
 *
 *     a PMIX_NSPACE given by the process logging its own message still
 *     names the namespace it is recorded under.
 *
 *     a '*' in the key or value is an ordinary character.
 *
 *   The table is read back with pmix_help_check_dups() itself, run on
 *   the progress thread where the table lives: PMIX_SUCCESS says the
 *   message was recorded before, PMIX_ERR_NOT_FOUND that it was not
 *   (and records it now).
 */

#include "src/include/pmix_config.h"

#include "include/pmix_server.h"
#include "src/include/pmix_globals.h"
#include "src/mca/bfrops/bfrops.h"
#include "src/mca/plog/base/base.h"
#include "src/server/pmix_server_ops.h"
#include "src/util/pmix_show_help.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
/* Selection: plog_base_order must survive framework open              */
/* ------------------------------------------------------------------ */

static int count_actives(void)
{
    pmix_plog_base_active_module_t *mod;
    int n, cnt = 0;

    for (n = 0; n < pmix_plog_globals.actives.size; n++) {
        mod = (pmix_plog_base_active_module_t *)
                  pmix_pointer_array_get_item(&pmix_plog_globals.actives, n);
        if (NULL != mod) {
            ++cnt;
        }
    }
    return cnt;
}

static const char *first_active_name(void)
{
    pmix_plog_base_active_module_t *mod;
    int n;

    for (n = 0; n < pmix_plog_globals.actives.size; n++) {
        mod = (pmix_plog_base_active_module_t *)
                  pmix_pointer_array_get_item(&pmix_plog_globals.actives, n);
        if (NULL != mod) {
            return mod->module->name;
        }
    }
    return NULL;
}

static void test_order_parsed(void)
{
    report("plog_base_order survives framework open",
           NULL != pmix_plog_globals.channels &&
           NULL != pmix_plog_globals.channels[0] &&
           0 == strcmp("stdout", pmix_plog_globals.channels[0]));
}

static void test_order_matches_channel(void)
{
    const char *nm = first_active_name();

    report("plog_base_order accepts a channel name, not just a module name",
           1 == count_actives() && NULL != nm && 0 == strcmp("stdfd", nm));
}

/* ------------------------------------------------------------------ */
/* Routing                                                             */
/* ------------------------------------------------------------------ */

static void test_unserviced_channel(void)
{
    pmix_info_t data[1];
    pmix_status_t rc;

    /* the syslog module was dropped by our requested ordering, so
     * nothing here can service a syslog entry */
    PMIX_INFO_LOAD(&data[0], PMIX_LOG_LOCAL_SYSLOG, "unit test", PMIX_STRING);
    rc = PMIx_Log(data, 1, NULL, 0);
    report("unserviced channel reports PMIX_ERR_NOT_AVAILABLE",
           PMIX_ERR_NOT_AVAILABLE == rc);
    PMIX_INFO_DESTRUCT(&data[0]);
}

static void test_stdout_wrong_type(void)
{
    pmix_info_t data[1];
    pmix_status_t rc;
    size_t bogus = 0x4142434445464748;

    /* the value's type is the caller's to choose - and for a client's
     * PMIx_Log that caller is on the far end of a socket. Reading this
     * union as a char* is a segfault, so the module has to check */
    PMIX_INFO_LOAD(&data[0], PMIX_LOG_STDOUT, &bogus, PMIX_SIZE);
    rc = PMIx_Log(data, 1, NULL, 0);
    report("stdout entry with a non-string value is rejected, not dereferenced",
           PMIX_ERR_NOT_AVAILABLE == rc);
    PMIX_INFO_DESTRUCT(&data[0]);
}

static void test_stdout_empty_string(void)
{
    pmix_info_t data[1];
    pmix_status_t rc;

    PMIX_INFO_LOAD(&data[0], PMIX_LOG_STDOUT, "", PMIX_STRING);
    rc = PMIx_Log(data, 1, NULL, 0);
    report("empty stdout string is accepted without a zero-length forward",
           PMIX_SUCCESS == rc);
    PMIX_INFO_DESTRUCT(&data[0]);
}

static void test_aggregated_duplicate(void)
{
    pmix_info_t data[1], dirs[3];
    pmix_status_t rc1, rc2;
    bool flag = true;

    PMIX_INFO_LOAD(&dirs[0], PMIX_LOG_AGG, &flag, PMIX_BOOL);
    PMIX_INFO_LOAD(&dirs[1], PMIX_LOG_KEY, "plog-routing-test", PMIX_STRING);
    PMIX_INFO_LOAD(&dirs[2], PMIX_LOG_VAL, "duplicate-topic", PMIX_STRING);

    PMIX_INFO_LOAD(&data[0], PMIX_LOG_STDOUT, "plog aggregation test\n", PMIX_STRING);
    rc1 = PMIx_Log(data, 1, dirs, 3);
    PMIX_INFO_DESTRUCT(&data[0]);

    /* second time around this key/val pair is a known duplicate, so
     * every entry is withheld. The request was still serviced - saying
     * otherwise sends a client off to log it all over again locally */
    PMIX_INFO_LOAD(&data[0], PMIX_LOG_STDOUT, "plog aggregation test\n", PMIX_STRING);
    rc2 = PMIx_Log(data, 1, dirs, 3);
    PMIX_INFO_DESTRUCT(&data[0]);

    report("first aggregated message is logged",
           PMIX_SUCCESS == rc1);
    report("aggregated duplicate reports success, not PMIX_ERR_NOT_AVAILABLE",
           PMIX_SUCCESS == rc2);

    PMIX_INFO_DESTRUCT(&dirs[0]);
    PMIX_INFO_DESTRUCT(&dirs[1]);
    PMIX_INFO_DESTRUCT(&dirs[2]);
}

/* ------------------------------------------------------------------ */
/* Duplicate suppression                                               */
/* ------------------------------------------------------------------ */

#define PLUT_REQUESTER "plog-ut-requester"
#define PLUT_NAMED     "plog-ut-named"
#define PLUT_CAP       "plog-ut-cap"
#define PLUT_FILE      "plog-ut-file"

typedef enum {
    PLUT_OP_CHECK,
    PLUT_OP_PURGE,
    PLUT_OP_SERVER_LOG
} plut_op_t;

/* one operation, run on the progress thread */
typedef struct {
    pmix_event_t ev;
    pmix_lock_t lock;
    plut_op_t op;
    const char *nspace;
    const char *key;
    const char *val;
    pmix_peer_t *peer;
    pmix_buffer_t *buf;
    pmix_status_t status;
} plut_req_t;

static void op_stub(pmix_status_t status, void *cbdata)
{
    (void) status;
    (void) cbdata;
}

static void do_op(int sd, short args, void *cbdata)
{
    plut_req_t *r = (plut_req_t *) cbdata;
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    switch (r->op) {
    case PLUT_OP_CHECK:
        r->status = pmix_help_check_dups(r->nspace, r->key, r->val);
        break;
    case PLUT_OP_PURGE:
        pmix_show_help_purge_nspace(r->nspace);
        r->status = PMIX_SUCCESS;
        break;
    case PLUT_OP_SERVER_LOG:
        r->status = pmix_server_log(r->peer, r->buf, op_stub, NULL);
        break;
    }
    PMIX_WAKEUP_THREAD(&r->lock);
}

static pmix_status_t run_op(plut_req_t *r)
{
    PMIX_CONSTRUCT_LOCK(&r->lock);
    PMIX_THREADSHIFT(r, do_op);
    PMIX_WAIT_THREAD(&r->lock);
    PMIX_DESTRUCT_LOCK(&r->lock);
    return r->status;
}

/* has (nspace, key, val) been recorded? Records it if it has not */
static pmix_status_t check_dup(const char *nspace, const char *key, const char *val)
{
    plut_req_t r;

    memset(&r, 0, sizeof(r));
    r.op = PLUT_OP_CHECK;
    r.nspace = nspace;
    r.key = key;
    r.val = val;
    return run_op(&r);
}

static void purge(const char *nspace)
{
    plut_req_t r;

    memset(&r, 0, sizeof(r));
    r.op = PLUT_OP_PURGE;
    r.nspace = nspace;
    (void) run_op(&r);
}

static void test_suppression_cap(void)
{
    report("plog_base_max_suppressed sets the limit",
           4 == pmix_show_help_max_tuples);

    /* fill the table, then one more */
    report("cap: t1 is new", PMIX_ERR_NOT_FOUND == check_dup(PLUT_CAP, PLUT_FILE, "t1"));
    report("cap: t2 is new", PMIX_ERR_NOT_FOUND == check_dup(PLUT_CAP, PLUT_FILE, "t2"));
    report("cap: t3 is new", PMIX_ERR_NOT_FOUND == check_dup(PLUT_CAP, PLUT_FILE, "t3"));
    report("cap: t4 is new", PMIX_ERR_NOT_FOUND == check_dup(PLUT_CAP, PLUT_FILE, "t4"));
    report("cap: t5 is new", PMIX_ERR_NOT_FOUND == check_dup(PLUT_CAP, PLUT_FILE, "t5"));
    /* t1 was dropped to make room for t5 - asking for it again records
     * it once more, and drops t2 */
    report("past the limit, the oldest message is no longer recorded",
           PMIX_ERR_NOT_FOUND == check_dup(PLUT_CAP, PLUT_FILE, "t1"));
    /* the table is t3 t4 t5 t1: matching t3 makes t4 the least recently
     * matched, so t6 drops t4 and keeps t3 */
    report("a recorded message within the limit is still recorded",
           PMIX_SUCCESS == check_dup(PLUT_CAP, PLUT_FILE, "t3"));
    report("cap: t6 is new", PMIX_ERR_NOT_FOUND == check_dup(PLUT_CAP, PLUT_FILE, "t6"));
    report("a message matched since it was recorded is kept over an older match",
           PMIX_SUCCESS == check_dup(PLUT_CAP, PLUT_FILE, "t3"));
    report("the message matched least recently is the one dropped",
           PMIX_ERR_NOT_FOUND == check_dup(PLUT_CAP, PLUT_FILE, "t4"));

    purge(PLUT_CAP);
}

/* what the host's log2 entry point was handed for PMIX_NSPACE */
static bool host_log_fired = false;
static char host_log_nspace[PMIX_MAX_NSLEN + 1];

static pmix_status_t stub_log2(const pmix_proc_t *client,
                               const pmix_info_t data[], size_t ndata,
                               const pmix_info_t directives[], size_t ndirs,
                               pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    size_t n;
    (void) client;
    (void) data;
    (void) ndata;

    host_log_fired = true;
    host_log_nspace[0] = '\0';
    for (n = 0; n < ndirs; n++) {
        if (PMIX_CHECK_KEY(&directives[n], PMIX_NSPACE) &&
            PMIX_STRING == directives[n].value.type &&
            NULL != directives[n].value.data.string) {
            pmix_strncpy(host_log_nspace, directives[n].value.data.string, PMIX_MAX_NSLEN);
        }
    }
    if (NULL != cbfunc) {
        cbfunc(PMIX_SUCCESS, cbdata);
    }
    return PMIX_SUCCESS;
}

/* a connected client of another namespace, without a socket */
static pmix_peer_t *make_requester(void)
{
    pmix_peer_t *peer;

    peer = PMIX_NEW(pmix_peer_t);
    peer->nptr = PMIX_NEW(pmix_namespace_t);
    peer->nptr->nspace = strdup(PLUT_REQUESTER);
    memcpy(&peer->nptr->compat, &pmix_globals.mypeer->nptr->compat,
           sizeof(peer->nptr->compat));
    peer->info = PMIX_NEW(pmix_rank_info_t);
    peer->info->pname.nspace = strdup(PLUT_REQUESTER);
    peer->info->pname.rank = 0;
    peer->info->uid = pmix_globals.mypeer->info->uid;
    peer->info->gid = pmix_globals.mypeer->info->gid;
    memcpy(&peer->proc_type, &pmix_globals.mypeer->proc_type, sizeof(pmix_proc_type_t));
    peer->proc_type.type = PMIX_PROC_CLIENT;
    return peer;
}

/* the wire form of a client's PMIx_Log: timestamp, the data, the
 * directives */
static pmix_buffer_t *build_log_request(const char *key, const char *named)
{
    pmix_buffer_t *buf;
    pmix_info_t data, dirs[4];
    pmix_status_t rc;
    time_t timestamp = 0;
    size_t ndata = 1, ndirs = 4, n;
    bool flag = true;

    PMIX_INFO_LOAD(&data, PMIX_LOG_STDOUT, "plog requester test\n", PMIX_STRING);
    PMIX_INFO_LOAD(&dirs[0], PMIX_LOG_AGG, &flag, PMIX_BOOL);
    PMIX_INFO_LOAD(&dirs[1], PMIX_LOG_KEY, key, PMIX_STRING);
    PMIX_INFO_LOAD(&dirs[2], PMIX_LOG_VAL, "topic", PMIX_STRING);
    PMIX_INFO_LOAD(&dirs[3], PMIX_NSPACE, named, PMIX_STRING);

    buf = PMIX_NEW(pmix_buffer_t);
    PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, buf, &timestamp, 1, PMIX_TIME);
    if (PMIX_SUCCESS == rc) {
        PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, buf, &ndata, 1, PMIX_SIZE);
    }
    if (PMIX_SUCCESS == rc) {
        PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, buf, &data, 1, PMIX_INFO);
    }
    if (PMIX_SUCCESS == rc) {
        PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, buf, &ndirs, 1, PMIX_SIZE);
    }
    if (PMIX_SUCCESS == rc) {
        PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, buf, dirs, ndirs, PMIX_INFO);
    }
    PMIX_INFO_DESTRUCT(&data);
    for (n = 0; n < ndirs; n++) {
        PMIX_INFO_DESTRUCT(&dirs[n]);
    }
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(buf);
        return NULL;
    }
    return buf;
}

static void test_requester_namespace(void)
{
    plut_req_t r;
    pmix_peer_t *peer = make_requester();
    pmix_status_t rc;

    /* as the gateway the server logs the request itself */
    memset(&r, 0, sizeof(r));
    r.op = PLUT_OP_SERVER_LOG;
    r.peer = peer;
    r.buf = build_log_request("plog-ut-gateway", PLUT_NAMED);
    pmix_globals.mypeer->proc_type.type |= (uint32_t) PMIX_PROC_GATEWAY_ACT;
    rc = (NULL == r.buf) ? PMIX_ERR_NOMEM : run_op(&r);
    pmix_globals.mypeer->proc_type.type &= ~((uint32_t) PMIX_PROC_GATEWAY_ACT);
    if (NULL != r.buf) {
        PMIX_RELEASE(r.buf);
    }
    report("a client's log request is logged by the gateway",
           PMIX_OPERATION_SUCCEEDED == rc);
    report("a client's message is not recorded under the PMIX_NSPACE it named",
           PMIX_ERR_NOT_FOUND == check_dup(PLUT_NAMED, "plog-ut-gateway", "topic"));
    report("a client's message is recorded under the client's own namespace",
           PMIX_SUCCESS == check_dup(PLUT_REQUESTER, "plog-ut-gateway", "topic"));

    /* not the gateway: the request goes to the host */
    memset(&r, 0, sizeof(r));
    r.op = PLUT_OP_SERVER_LOG;
    r.peer = peer;
    r.buf = build_log_request("plog-ut-host", PLUT_NAMED);
    host_log_fired = false;
    pmix_host_server.log2 = stub_log2;
    rc = (NULL == r.buf) ? PMIX_ERR_NOMEM : run_op(&r);
    pmix_host_server.log2 = NULL;
    if (NULL != r.buf) {
        PMIX_RELEASE(r.buf);
    }
    report("a client's log request reaches the host",
           PMIX_SUCCESS == rc && host_log_fired);
    report("the host is handed the client's own namespace as PMIX_NSPACE",
           0 == strcmp(host_log_nspace, PLUT_REQUESTER));

    PMIX_RELEASE(peer);
    purge(PLUT_NAMED);
    purge(PLUT_REQUESTER);
}

static void test_local_named_namespace(void)
{
    pmix_info_t data[1], dirs[4];
    pmix_status_t rc;
    bool flag = true;
    size_t n;

    PMIX_INFO_LOAD(&data[0], PMIX_LOG_STDOUT, "plog local named test\n", PMIX_STRING);
    PMIX_INFO_LOAD(&dirs[0], PMIX_LOG_AGG, &flag, PMIX_BOOL);
    PMIX_INFO_LOAD(&dirs[1], PMIX_LOG_KEY, "plog-ut-local", PMIX_STRING);
    PMIX_INFO_LOAD(&dirs[2], PMIX_LOG_VAL, "topic", PMIX_STRING);
    PMIX_INFO_LOAD(&dirs[3], PMIX_NSPACE, PLUT_NAMED, PMIX_STRING);
    rc = PMIx_Log(data, 1, dirs, 4);
    PMIX_INFO_DESTRUCT(&data[0]);
    for (n = 0; n < 4; n++) {
        PMIX_INFO_DESTRUCT(&dirs[n]);
    }
    report("a message the server logs for itself is logged", PMIX_SUCCESS == rc);
    report("a message the server logs for itself is recorded under the PMIX_NSPACE it names",
           PMIX_SUCCESS == check_dup(PLUT_NAMED, "plog-ut-local", "topic"));
    purge(PLUT_NAMED);
}

static void test_literal_star(void)
{
    report("star: a key and value of '*' are new",
           PMIX_ERR_NOT_FOUND == check_dup(PLUT_NAMED, "*", "*"));
    report("a '*' key and value do not match another key and value",
           PMIX_ERR_NOT_FOUND == check_dup(PLUT_NAMED, "plog-ut-other", "topic"));
    report("star: a key and value ending in '*' are new",
           PMIX_ERR_NOT_FOUND == check_dup(PLUT_NAMED, PLUT_FILE "*", "t*"));
    report("a trailing '*' does not match a longer key and value",
           PMIX_ERR_NOT_FOUND == check_dup(PLUT_NAMED, PLUT_FILE "-abc", "t-abc"));
    report("a key and value holding '*' match themselves",
           PMIX_SUCCESS == check_dup(PLUT_NAMED, PLUT_FILE "*", "t*"));
    purge(PLUT_NAMED);
}

/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    pmix_status_t rc;
    static pmix_server_module_t mymodule = {0};
    PMIX_HIDE_UNUSED_PARAMS(argc, argv);

    /* several of these cases exist because the code under test used to
     * crash - buffered results would be lost with the process */
    setvbuf(stdout, NULL, _IONBF, 0);

    /* name a channel rather than the module that services it - both
     * have to resolve */
    putenv((char *) "PMIX_MCA_plog_base_order=stdout");
    /* a small limit on recorded messages, so the case can reach it */
    putenv((char *) "PMIX_MCA_plog_base_max_suppressed=4");

    rc = PMIx_server_init(&mymodule, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "PMIx_server_init failed: %s\n", PMIx_Error_string(rc));
        return 1;
    }
    /* with no host log function and no gateway role, PMIx_Log processes
     * locally and returns the framework's own status */
    pmix_host_server.log = NULL;
    pmix_host_server.log2 = NULL;
    pmix_globals.mypeer->proc_type.type &= ~((uint32_t) PMIX_PROC_GATEWAY_ACT);

    fprintf(stdout, "\n=== plog routing unit tests ===\n\n");

    test_order_parsed();
    test_order_matches_channel();
    /* first: see the header */
    test_suppression_cap();
    test_requester_namespace();
    test_local_named_namespace();
    test_literal_star();
    test_unserviced_channel();
    test_stdout_wrong_type();
    test_stdout_empty_string();
    test_aggregated_duplicate();

    fprintf(stdout, "\nResults: %d passed, %d failed\n\n", npass, nfail);

    PMIx_server_finalize();

    return (nfail > 0) ? 1 : 0;
}
