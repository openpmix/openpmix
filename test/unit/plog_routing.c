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
 */

#include "src/include/pmix_config.h"

#include "include/pmix_server.h"
#include "src/include/pmix_globals.h"
#include "src/mca/plog/base/base.h"
#include "src/server/pmix_server_ops.h"

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
    test_unserviced_channel();
    test_stdout_wrong_type();
    test_stdout_empty_string();
    test_aggregated_duplicate();

    fprintf(stdout, "\nResults: %d passed, %d failed\n\n", npass, nfail);

    PMIx_server_finalize();

    return (nfail > 0) ? 1 : 0;
}
