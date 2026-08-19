/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/*
 * Unit tests for the pnet tcp component's static-port allocator.
 *
 * tcp is the only shipped component that hands a job a slice of a
 * finite pool and takes it back again, so the thing worth pinning down
 * is the accounting: what comes out of the pool, what the far end can
 * reconstruct from the blob, and - the part that has no other check on
 * it - that deregistering a job puts every port it held back.
 *
 * Everything is driven through the public server entry points, which is
 * what actually reaches the module: PMIx_server_setup_application calls
 * pnet's allocate, and PMIx_server_deregister_nspace calls its
 * deregister_nspace.  Both are non-blocking, so each case waits on its
 * own lock rather than sleeping.
 *
 * The pools are deliberately tiny - four tcp ports and four udp ports -
 * because an exhaustion boundary is the only way to prove a port came
 * back.  A pool of four that satisfies three successive two-port jobs
 * has recycled; one that does not is leaking.
 *
 * What each case pins down:
 *
 *   request placement - PMIX_ALLOC_FABRIC was matched against info[0]
 *     rather than info[n], so a host that put anything ahead of it (an
 *     envar directive, say) had its fabric request silently ignored and
 *     got no ports at all.  The request here is deliberately *not* first.
 *
 *   the udp arm - it selected on requests[n] after that loop had run to
 *     completion, so it read one past the end of the request array and
 *     compared a garbage pointer.  A plain "give me udp ports" request
 *     is the whole case.
 *
 *   recycling - a job's ports live on a tcp_port_tracker_t whose
 *     destructor returns them to the pool.  deregister_nspace stopped at
 *     the first tracker matching the nspace, so a request that produced
 *     more than one leaked the rest of the pool permanently.
 *
 *   security key only - the header comment says a caller may ask for a
 *     key without asking for endpoints.  Doing so drove process_request
 *     with a zero count, which declined with an error severe enough to
 *     abort the framework's whole fan-out, so no component got to run.
 *
 *   empty pool - with no static ports configured at all, the default
 *     path took pmix_list_get_first() of an empty list, which hands back
 *     the list's own sentinel rather than NULL, and treated that
 *     sentinel as a port pool.
 *
 * The component is opt-in at configure time (--with-tcp), so the test
 * asks the MCA whether it was built and exits 77 - automake's "skip" -
 * when it was not.  That probe is the parameter registration, which
 * happens for a built component whether or not the selection gate lets
 * it open, so it answers "built" and not "active".
 */

#include "src/include/pmix_config.h"

#include "include/pmix.h"
#include "include/pmix_server.h"

#include "src/class/pmix_list.h"
#include "src/include/pmix_globals.h"
#include "src/mca/base/pmix_mca_base_var.h"
#include "src/mca/bfrops/bfrops.h"
#include "src/threads/pmix_threads.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_environ.h"

#include <sys/wait.h>
#include <unistd.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TCP_SETUP_APP_KEY "pmix.tcp.setup.app.key"
#define FABRIC_ID         "unit.tcp"
#define NPORTS            4

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

/* what one setup_application call gave back */
typedef struct {
    pmix_lock_t lock;
    pmix_status_t status;
    bool gotblob;  // our blob key came back
    bool gotkey;   // a security key is inside it
    size_t nkv;    // the kval count the blob declares
    size_t nfound; // how many it actually carries
    int nports;    // ports named by the first entry keyed by our fabric id
} tcp_result_t;

/* walk the blob the component builds for the compute nodes. This is the
 * same shape setup_local_network reads, so getting at it here checks the
 * producer against the consumer's expectations without a second daemon */
static void examine(tcp_result_t *res, pmix_byte_object_t *bo)
{
    pmix_buffer_t bkt;
    pmix_kval_t *kv;
    pmix_status_t rc;
    int32_t cnt;
    char *p;

    PMIX_LOAD_BUFFER_NON_DESTRUCT(pmix_globals.mypeer, &bkt, bo->bytes, bo->size);
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, pmix_globals.mypeer, &bkt, &res->nkv, &cnt, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        return;
    }
    kv = PMIX_NEW(pmix_kval_t);
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, pmix_globals.mypeer, &bkt, kv, &cnt, PMIX_KVAL);
    while (PMIX_SUCCESS == rc) {
        ++res->nfound;
        if (PMIX_CHECK_KEY(kv, PMIX_ALLOC_FABRIC_SEC_KEY)) {
            res->gotkey = true;
        } else if (0 == strncmp(kv->key, FABRIC_ID, PMIX_MAX_KEYLEN) && 0 == res->nports
                   && PMIX_STRING == kv->value->type && NULL != kv->value->data.string) {
            /* the ports are the first thing filed under the fabric id */
            res->nports = 1;
            for (p = kv->value->data.string; '\0' != *p; p++) {
                if (',' == *p) {
                    ++res->nports;
                }
            }
        }
        PMIX_RELEASE(kv);
        kv = PMIX_NEW(pmix_kval_t);
        cnt = 1;
        PMIX_BFROPS_UNPACK(rc, pmix_globals.mypeer, &bkt, kv, &cnt, PMIX_KVAL);
    }
    PMIX_RELEASE(kv);
}

static void setupcb(pmix_status_t status, pmix_info_t info[], size_t ninfo, void *provided_cbdata,
                    pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    tcp_result_t *res = (tcp_result_t *) provided_cbdata;
    size_t n;

    res->status = status;
    for (n = 0; n < ninfo; n++) {
        if (0 == strncmp(info[n].key, TCP_SETUP_APP_KEY, PMIX_MAX_KEYLEN)
            && PMIX_BYTE_OBJECT == info[n].value.type) {
            res->gotblob = true;
            examine(res, &info[n].value.data.bo);
        }
    }
    /* the array belongs to the library - releasing it is what this
     * callback is for */
    if (NULL != cbfunc) {
        cbfunc(PMIX_SUCCESS, cbdata);
    }
    PMIX_WAKEUP_THREAD(&res->lock);
}

static void opcb(pmix_status_t status, void *cbdata)
{
    pmix_lock_t *lock = (pmix_lock_t *) cbdata;

    lock->status = status;
    PMIX_WAKEUP_THREAD(lock);
}

/* ask for nports endpoints of the given type, with the fabric request
 * deliberately placed second in the directive array */
static void ask(const char *nspace, const char *type, int nports, bool seckey, tcp_result_t *res)
{
    pmix_info_t directives[2], *reqs;
    pmix_data_array_t darray;
    pmix_nspace_t ns;
    size_t nreqs = 0, r = 0;
    bool flag = true;
    pmix_status_t rc;

    memset(res, 0, sizeof(*res));
    PMIX_CONSTRUCT_LOCK(&res->lock);
    res->status = PMIX_ERR_NOT_SUPPORTED;

    if (NULL != type) {
        ++nreqs;
    }
    if (0 < nports) {
        ++nreqs;
    }
    if (seckey) {
        ++nreqs;
    }
    ++nreqs; // the id key is always required
    PMIX_INFO_CREATE(reqs, nreqs);
    PMIX_INFO_LOAD(&reqs[r], PMIX_ALLOC_FABRIC_ID, FABRIC_ID, PMIX_STRING);
    ++r;
    if (NULL != type) {
        PMIX_INFO_LOAD(&reqs[r], PMIX_ALLOC_FABRIC_TYPE, type, PMIX_STRING);
        ++r;
    }
    if (0 < nports) {
        PMIX_INFO_LOAD(&reqs[r], PMIX_ALLOC_FABRIC_ENDPTS, &nports, PMIX_INT);
        ++r;
    }
    if (seckey) {
        PMIX_INFO_LOAD(&reqs[r], PMIX_ALLOC_FABRIC_SEC_KEY, &flag, PMIX_BOOL);
        ++r;
    }

    /* something ahead of the fabric request - this is the placement the
     * component used to ignore */
    PMIX_INFO_LOAD(&directives[0], PMIX_SETUP_APP_ENVARS, &flag, PMIX_BOOL);
    darray.type = PMIX_INFO;
    darray.array = reqs;
    darray.size = nreqs;
    PMIX_INFO_LOAD(&directives[1], PMIX_ALLOC_FABRIC, &darray, PMIX_DATA_ARRAY);

    PMIX_LOAD_NSPACE(ns, nspace);
    rc = PMIx_server_setup_application(ns, directives, 2, setupcb, res);
    if (PMIX_SUCCESS != rc) {
        res->status = rc;
    } else {
        PMIX_WAIT_THREAD(&res->lock);
    }

    PMIX_INFO_FREE(reqs, nreqs);
    PMIX_INFO_DESTRUCT(&directives[0]);
    PMIX_INFO_DESTRUCT(&directives[1]);
    PMIX_DESTRUCT_LOCK(&res->lock);
}

static void drop(const char *nspace)
{
    pmix_lock_t lock;
    pmix_nspace_t ns;

    PMIX_CONSTRUCT_LOCK(&lock);
    PMIX_LOAD_NSPACE(ns, nspace);
    PMIx_server_deregister_nspace(ns, opcb, &lock);
    PMIX_WAIT_THREAD(&lock);
    PMIX_DESTRUCT_LOCK(&lock);
}

/* ------------------------------------------------------------------ */
static void test_request_placement(void)
{
    tcp_result_t res;

    ask("tcp-place", "tcp", 2, false, &res);
    report("a fabric request that is not first is still seen",
           PMIX_SUCCESS == res.status && res.gotblob);
    report("the blob names the two ports that were asked for", 2 == res.nports);
    report("the blob's declared count matches what it carries",
           0 < res.nkv && res.nkv == res.nfound);
    drop("tcp-place");
}

static void test_udp(void)
{
    tcp_result_t res;

    ask("tcp-udp", "udp", 2, false, &res);
    report("a udp request allocates from the udp pool",
           PMIX_SUCCESS == res.status && res.gotblob && 2 == res.nports);
    drop("tcp-udp");
}

static void test_recycling(void)
{
    tcp_result_t res;
    int i;
    bool allok = true;

    /* the pool holds four ports. Three successive two-port jobs only fit
     * if each deregistration gives its pair back */
    for (i = 0; i < 3; i++) {
        char ns[32];
        (void) snprintf(ns, sizeof(ns), "tcp-cycle-%d", i);
        ask(ns, "tcp", 2, false, &res);
        if (PMIX_SUCCESS != res.status || !res.gotblob) {
            allok = false;
            break;
        }
        drop(ns);
    }
    report("deregistering a job returns its ports to the pool", allok);
}

static void test_exhaustion(void)
{
    tcp_result_t res;

    /* more than the pool holds - this must be reported, not silently
     * satisfied out of nowhere */
    ask("tcp-toobig", "tcp", NPORTS + 1, false, &res);
    report("a request larger than the pool is refused",
           PMIX_SUCCESS != res.status && !res.gotblob);
    drop("tcp-toobig");

    /* and the pool is intact afterwards - the failed request's partial
     * allocation went back */
    ask("tcp-after", "tcp", NPORTS, false, &res);
    report("a refused request leaves the pool whole",
           PMIX_SUCCESS == res.status && res.gotblob);
    drop("tcp-after");
}

static void test_seckey_only(void)
{
    tcp_result_t res;

    ask("tcp-key", "tcp", 0, true, &res);
    report("a key-only request is honored without endpoints",
           PMIX_SUCCESS == res.status && res.gotblob);
    report("the key-only request yields a key and no ports",
           res.gotkey && 0 == res.nports);
    drop("tcp-key");
}

/* a request naming neither type nor plane falls back to the configured
 * default allocation, which here is two groups - so one call builds two
 * trackers for the same job. Cycling it three times through a pool that
 * holds exactly two rounds' worth only fits if deregistration releases
 * both of them, and the first group only matches at all if the
 * "type:plane:count" parse finds the plane */
static void test_default_allocation(void)
{
    tcp_result_t res;
    bool allok = true;
    int i;

    for (i = 0; i < 3; i++) {
        char ns[32];
        (void) snprintf(ns, sizeof(ns), "tcp-dflt-%d", i);
        ask(ns, NULL, 0, false, &res);
        if (PMIX_SUCCESS != res.status || !res.gotblob || 2 != res.nports) {
            allok = false;
            break;
        }
        drop(ns);
    }
    report("the default allocation releases every tracker it built", allok);
}

/* the empty-pool case needs a server that never saw a static_ports
 * setting, and the MCA reads that once when the framework opens - so it
 * gets its own process, forked before this one configures anything.
 *
 * pmix_list_get_first() returns the list's sentinel, not NULL, when the
 * list is empty, and the default path took that sentinel for a port
 * pool: it retained it and read a port count out of it. In a debug build
 * the list's own assertion catches that and aborts, so what this checks
 * is that the child comes back at all */
static int empty_pool_child(void)
{
    static pmix_server_module_t mymodule = {0};
    tcp_result_t res;
    pmix_info_t sinfo;
    pmix_status_t rc;
    bool flag = true;

    setenv("PMIX_MCA_pnet", "tcp", 1);
    unsetenv("PMIX_MCA_pnet_tcp_static_ports");
    unsetenv("PMIX_MCA_pnet_tcp_default_network_allocation");

    PMIX_INFO_LOAD(&sinfo, PMIX_SERVER_GATEWAY, &flag, PMIX_BOOL);
    rc = PMIx_server_init(&mymodule, &sinfo, 1);
    PMIX_INFO_DESTRUCT(&sinfo);
    if (PMIX_SUCCESS != rc) {
        return 1;
    }
    /* no type and no plane, so this lands on the default path, which is
     * the one that reaches for the first entry of an empty list */
    ask("tcp-empty", NULL, 2, false, &res);
    /* declining is the only correct answer - there is nothing to hand
     * out, so the call succeeds with no tcp blob in it */
    rc = (PMIX_SUCCESS == res.status && !res.gotblob) ? 0 : 1;
    PMIx_server_finalize();
    return rc;
}

static void test_empty_pool(void)
{
    pid_t pid;
    int status = -1;

    pid = fork();
    if (0 == pid) {
        _exit(empty_pool_child());
    }
    if (0 > pid) {
        report("an unconfigured pool is declined, not dereferenced", 0);
        return;
    }
    if (pid != waitpid(pid, &status, 0)) {
        report("an unconfigured pool is declined, not dereferenced", 0);
        return;
    }
    report("an unconfigured pool is declined, not dereferenced",
           WIFEXITED(status) && 0 == WEXITSTATUS(status));
}

int main(int argc, char **argv)
{
    static pmix_server_module_t mymodule = {0};
    pmix_info_t sinfo;
    pmix_status_t rc;
    bool flag = true;

    (void) argc;
    (void) argv;

    fprintf(stdout, "pnet_tcp_ports: static-port allocation unit tests\n");

    /* before this process configures a pool of its own */
    test_empty_pool();

    /* select the component and give it a pool to work from. Both have to
     * be in place before the framework opens */
    setenv("PMIX_MCA_pnet", "tcp", 1);
    setenv("PMIX_MCA_pnet_tcp_static_ports", "tcp:plane0:32000-32003;udp:40000-40003", 1);
    /* two groups, so one request builds two trackers - and the first of
     * them names a plane, which is the part the parser used to lose */
    setenv("PMIX_MCA_pnet_tcp_default_network_allocation", "tcp:plane0:2;udp:2", 1);

    /* the port allocator only runs for the gateway role */
    PMIX_INFO_LOAD(&sinfo, PMIX_SERVER_GATEWAY, &flag, PMIX_BOOL);
    rc = PMIx_server_init(&mymodule, &sinfo, 1);
    PMIX_INFO_DESTRUCT(&sinfo);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "PMIx_server_init failed: %s\n", PMIx_Error_string(rc));
        return 1;
    }

    /* was the component built at all? Its parameters are registered for
     * any built component, so this answers "built", not "selected" */
    if (0 > pmix_mca_base_var_find("pmix", "pnet", "tcp", "static_ports")) {
        fprintf(stdout, "pnet/tcp was not built (--with-tcp) - skipping\n");
        PMIx_server_finalize();
        return 77;
    }

    test_request_placement();
    test_udp();
    test_recycling();
    test_exhaustion();
    test_seckey_only();
    test_default_allocation();

    PMIx_server_finalize();

    fprintf(stdout, "pnet_tcp_ports: %d passed, %d failed\n", npass, nfail);
    return (0 == nfail) ? 0 : 1;
}
