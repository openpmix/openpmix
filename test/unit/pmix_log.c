/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Unit tests for PMIx_Log and PMIx_Log_nb.
 *
 * The process is initialized as a non-gateway PMIx server.  Gateway
 * behavior is exercised by temporarily setting the PMIX_PROC_GATEWAY_ACT
 * flag on pmix_globals.mypeer, which is safe in a single-process unit
 * test.  The pmix_log_host_only global is manipulated directly between
 * test cases; it is reset to false after every test that sets it.
 *
 * Test cases:
 *
 *  bad-param: PMIx_Log rejects NULL/zero-length data.
 *  bad-param: PMIx_Log_nb rejects NULL/zero-length data.
 *
 *  non-gateway + log2 registered + host_only=false → log2 invoked
 *  non-gateway + log2 registered + host_only=true  → log2 invoked
 *  non-gateway + no host log    + host_only=false  → falls local (plog)
 *  non-gateway + no host log    + host_only=true   → PMIX_ERR_NOT_SUPPORTED
 *
 *  gateway + log2 registered + host_only=false → local plog (unchanged)
 *  gateway + log2 registered + host_only=true  → log2 invoked
 *  gateway + no host log     + host_only=false → local plog (unchanged)
 *  gateway + no host log     + host_only=true  → PMIX_ERR_NOT_SUPPORTED
 *
 *  PMIx_Log_nb: cbfunc is invoked on completion.
 */

#include "src/include/pmix_config.h"

#include "include/pmix_server.h"
#include "src/include/pmix_globals.h"
#include "src/mca/ptl/ptl_types.h"
#include "src/runtime/pmix_rte.h"
#include "src/server/pmix_server_ops.h"
#include "src/threads/pmix_threads.h"

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
/* Stub host log2 callback                                             */
/* ------------------------------------------------------------------ */

static int log2_call_count = 0;

static pmix_status_t stub_log2(const pmix_proc_t *client,
                                const pmix_info_t data[], size_t ndata,
                                const pmix_info_t directives[], size_t ndirs,
                                pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    PMIX_HIDE_UNUSED_PARAMS(client, data, ndata, directives, ndirs);
    ++log2_call_count;
    if (NULL != cbfunc) {
        cbfunc(PMIX_SUCCESS, cbdata);
    }
    return PMIX_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Test helpers                                                        */
/* ------------------------------------------------------------------ */

static void install_log2(void)
{
    log2_call_count = 0;
    pmix_host_server.log2 = stub_log2;
    pmix_host_server.log = NULL;
}

static void clear_host_log(void)
{
    pmix_host_server.log2 = NULL;
    pmix_host_server.log = NULL;
}

static void set_gateway(bool gw)
{
    if (gw) {
        pmix_globals.mypeer->proc_type.type |= PMIX_PROC_GATEWAY_ACT;
    } else {
        pmix_globals.mypeer->proc_type.type &= ~((uint32_t) PMIX_PROC_GATEWAY_ACT);
    }
}

static void make_data(pmix_info_t *info)
{
    PMIX_INFO_LOAD(info, PMIX_LOG_STDERR, "pmix_log unit test", PMIX_STRING);
}

/* ------------------------------------------------------------------ */
/* Parameter-validation tests                                          */
/* ------------------------------------------------------------------ */

static void test_log_null_data(void)
{
    pmix_status_t rc = PMIx_Log(NULL, 0, NULL, 0);
    report("PMIx_Log: NULL data returns PMIX_ERR_BAD_PARAM",
           PMIX_ERR_BAD_PARAM == rc);
}

static void test_lognb_null_data(void)
{
    pmix_status_t rc = PMIx_Log_nb(NULL, 0, NULL, 0, NULL, NULL);
    report("PMIx_Log_nb: NULL data returns PMIX_ERR_BAD_PARAM",
           PMIX_ERR_BAD_PARAM == rc);
}

/* ------------------------------------------------------------------ */
/* Non-gateway dispatch tests                                          */
/* ------------------------------------------------------------------ */

/* Non-gateway server always enters the host-dispatch branch regardless
 * of host_only; when log2 is registered it must be called. */
static void test_nongateway_log2_hostonly_false(void)
{
    pmix_info_t data[1];
    pmix_status_t rc;

    set_gateway(false);
    install_log2();
    pmix_log_host_only = false;
    make_data(data);

    rc = PMIx_Log(data, 1, NULL, 0);
    report("non-gateway + log2 registered + host_only=false: log2 invoked",
           PMIX_SUCCESS == rc && 1 == log2_call_count);
    PMIX_INFO_DESTRUCT(data);
}

static void test_nongateway_log2_hostonly_true(void)
{
    pmix_info_t data[1];
    pmix_status_t rc;

    set_gateway(false);
    install_log2();
    pmix_log_host_only = true;
    make_data(data);

    rc = PMIx_Log(data, 1, NULL, 0);
    report("non-gateway + log2 registered + host_only=true: log2 invoked",
           PMIX_SUCCESS == rc && 1 == log2_call_count);
    PMIX_INFO_DESTRUCT(data);
    pmix_log_host_only = false;
}

/* No host log functions and host_only=false: falls through to the local
 * plog framework.  We cannot predict what plog returns in a minimal test
 * environment, so only assert that the stricter host_only error was NOT
 * returned. */
static void test_nongateway_nohost_hostonly_false(void)
{
    pmix_info_t data[1];
    pmix_status_t rc;

    set_gateway(false);
    clear_host_log();
    pmix_log_host_only = false;
    make_data(data);

    rc = PMIx_Log(data, 1, NULL, 0);
    report("non-gateway + no host log + host_only=false: falls local (not ERR_NOT_SUPPORTED)",
           PMIX_ERR_NOT_SUPPORTED != rc);
    PMIX_INFO_DESTRUCT(data);
}

/* No host log functions and host_only=true: must return NOT_SUPPORTED. */
static void test_nongateway_nohost_hostonly_true(void)
{
    pmix_info_t data[1];
    pmix_status_t rc;

    set_gateway(false);
    clear_host_log();
    pmix_log_host_only = true;
    make_data(data);

    rc = PMIx_Log(data, 1, NULL, 0);
    report("non-gateway + no host log + host_only=true: PMIX_ERR_NOT_SUPPORTED",
           PMIX_ERR_NOT_SUPPORTED == rc);
    PMIX_INFO_DESTRUCT(data);
    pmix_log_host_only = false;
}

/* ------------------------------------------------------------------ */
/* Gateway dispatch tests                                              */
/* ------------------------------------------------------------------ */

/* Gateway + host_only=false: plog framework is used regardless of
 * whether a host log function is registered (original behavior). */
static void test_gateway_log2_hostonly_false(void)
{
    pmix_info_t data[1];
    pmix_status_t rc;

    set_gateway(true);
    install_log2();
    pmix_log_host_only = false;
    make_data(data);

    rc = PMIx_Log(data, 1, NULL, 0);
    report("gateway + log2 registered + host_only=false: falls local (log2 not called)",
           PMIX_ERR_NOT_SUPPORTED != rc && 0 == log2_call_count);
    PMIX_INFO_DESTRUCT(data);
    set_gateway(false);
}

/* Gateway + log2 registered + host_only=true: log2 must be called. */
static void test_gateway_log2_hostonly_true(void)
{
    pmix_info_t data[1];
    pmix_status_t rc;

    set_gateway(true);
    install_log2();
    pmix_log_host_only = true;
    make_data(data);

    rc = PMIx_Log(data, 1, NULL, 0);
    report("gateway + log2 registered + host_only=true: log2 invoked",
           PMIX_SUCCESS == rc && 1 == log2_call_count);
    PMIX_INFO_DESTRUCT(data);
    set_gateway(false);
    pmix_log_host_only = false;
}

/* Gateway + no host log + host_only=false: falls local (original behavior). */
static void test_gateway_nohost_hostonly_false(void)
{
    pmix_info_t data[1];
    pmix_status_t rc;

    set_gateway(true);
    clear_host_log();
    pmix_log_host_only = false;
    make_data(data);

    rc = PMIx_Log(data, 1, NULL, 0);
    report("gateway + no host log + host_only=false: falls local (not ERR_NOT_SUPPORTED)",
           PMIX_ERR_NOT_SUPPORTED != rc);
    PMIX_INFO_DESTRUCT(data);
    set_gateway(false);
}

/* Gateway + no host log + host_only=true: must return NOT_SUPPORTED. */
static void test_gateway_nohost_hostonly_true(void)
{
    pmix_info_t data[1];
    pmix_status_t rc;

    set_gateway(true);
    clear_host_log();
    pmix_log_host_only = true;
    make_data(data);

    rc = PMIx_Log(data, 1, NULL, 0);
    report("gateway + no host log + host_only=true: PMIX_ERR_NOT_SUPPORTED",
           PMIX_ERR_NOT_SUPPORTED == rc);
    PMIX_INFO_DESTRUCT(data);
    set_gateway(false);
    pmix_log_host_only = false;
}

/* Gateway + host_only=false with a data item whose channel no active
 * plog module services.  The request falls to the local plog framework,
 * which must report that it could not service the request rather than
 * falsely claiming success.  PMIX_LOG_JOB_RECORD ("jrec") is not claimed
 * by any of the built-in channels (stdout/stderr, syslog, email). */
static void test_gateway_unavailable_channel(void)
{
    pmix_info_t data[1];
    pmix_status_t rc;
    bool flag = true;

    set_gateway(true);
    clear_host_log();
    pmix_log_host_only = false;
    PMIX_INFO_LOAD(&data[0], PMIX_LOG_JOB_RECORD, &flag, PMIX_BOOL);

    rc = PMIx_Log(data, 1, NULL, 0);
    report("gateway + unavailable channel: PMIX_ERR_NOT_AVAILABLE",
           PMIX_ERR_NOT_AVAILABLE == rc);
    PMIX_INFO_DESTRUCT(data);
    set_gateway(false);
}

/* ------------------------------------------------------------------ */
/* PMIx_Log_nb: cbfunc is invoked on completion                       */
/* ------------------------------------------------------------------ */

static void nb_cbfunc(pmix_status_t status, void *cbdata)
{
    pmix_lock_t *lock = (pmix_lock_t *) cbdata;
    PMIX_HIDE_UNUSED_PARAMS(status);
    PMIX_WAKEUP_THREAD(lock);
}

static void test_lognb_cbfunc_invoked(void)
{
    pmix_info_t data[1];
    pmix_status_t rc;
    pmix_lock_t lock;

    set_gateway(false);
    install_log2();
    pmix_log_host_only = false;
    make_data(data);
    PMIX_CONSTRUCT_LOCK(&lock);

    rc = PMIx_Log_nb(data, 1, NULL, 0, nb_cbfunc, &lock);
    if (PMIX_SUCCESS == rc) {
        PMIX_WAIT_THREAD(&lock);
    }
    PMIX_DESTRUCT_LOCK(&lock);

    report("PMIx_Log_nb: cbfunc invoked on completion",
           PMIX_SUCCESS == rc && 1 == log2_call_count);
    PMIX_INFO_DESTRUCT(data);
}

/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    pmix_status_t rc;
    static pmix_server_module_t mymodule = {0};
    PMIX_HIDE_UNUSED_PARAMS(argc, argv);

    rc = PMIx_server_init(&mymodule, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "PMIx_server_init failed: %s\n", PMIx_Error_string(rc));
        return 1;
    }

    fprintf(stdout, "\n=== PMIx_Log / PMIx_Log_nb unit tests ===\n\n");

    /* Parameter validation */
    test_log_null_data();
    test_lognb_null_data();

    /* Non-gateway dispatch */
    test_nongateway_log2_hostonly_false();
    test_nongateway_log2_hostonly_true();
    test_nongateway_nohost_hostonly_false();
    test_nongateway_nohost_hostonly_true();

    /* Gateway dispatch */
    test_gateway_log2_hostonly_false();
    test_gateway_log2_hostonly_true();
    test_gateway_nohost_hostonly_false();
    test_gateway_nohost_hostonly_true();
    test_gateway_unavailable_channel();

    /* Non-blocking completion */
    test_lognb_cbfunc_invoked();

    fprintf(stdout, "\nResults: %d passed, %d failed\n\n", npass, nfail);

    PMIx_server_finalize();

    return (nfail > 0) ? 1 : 0;
}
