/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Unit tests for the global-syslog routing contract.
 *
 * PMIX_LOG_GLOBAL_SYSLOG asks for the message to be recorded in the
 * system-wide syslog, which lives on the gateway node. The library has
 * no way of knowing which daemon is on that node, so it never relays
 * the request itself: a server that is not the gateway passes the whole
 * request up to its host, and only a gateway writes the entry with its
 * own syslog module. When there is no host to take it - the case this
 * test runs in - the request cannot be serviced, and the caller has to
 * be told so.
 *
 * This is a separate program from plog_routing.c because the module set
 * is fixed by plog_base_order before PMIx_server_init, and these cases
 * need the syslog module active while that test needs it dropped.
 *
 * Test cases:
 *
 *   a gsys entry on a non-gateway peer with no host is declined, not
 *   silently swallowed.
 *
 *   the same entry alongside a local-syslog entry the module *can*
 *   service is still reported as unserviced. The module returns one
 *   status for the whole array, so success on the local entry used to
 *   cover for the global one and the caller was told its message had
 *   been logged when nothing had been written.
 *
 *   a gsys entry on a gateway peer is written locally, because on the
 *   gateway the local syslog is the global one.
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

static void set_gateway(bool gway)
{
    if (gway) {
        pmix_globals.mypeer->proc_type.type |= (uint32_t) PMIX_PROC_GATEWAY_ACT;
    } else {
        pmix_globals.mypeer->proc_type.type &= ~((uint32_t) PMIX_PROC_GATEWAY_ACT);
    }
}

static void test_gsys_alone_declined(void)
{
    pmix_info_t data[1];
    pmix_status_t rc;

    set_gateway(false);
    PMIX_INFO_LOAD(&data[0], PMIX_LOG_GLOBAL_SYSLOG, "gsys unit test", PMIX_STRING);
    rc = PMIx_Log(data, 1, NULL, 0);
    report("global syslog on a non-gateway reports PMIX_ERR_NOT_AVAILABLE",
           PMIX_ERR_NOT_AVAILABLE == rc);
    PMIX_INFO_DESTRUCT(&data[0]);
}

static void test_gsys_with_lsys_declined(void)
{
    pmix_info_t data[2];
    pmix_status_t rc;

    set_gateway(false);
    PMIX_INFO_LOAD(&data[0], PMIX_LOG_GLOBAL_SYSLOG, "gsys unit test", PMIX_STRING);
    PMIX_INFO_LOAD(&data[1], PMIX_LOG_LOCAL_SYSLOG, "lsys unit test", PMIX_STRING);
    rc = PMIx_Log(data, 2, NULL, 0);
    /* the local entry was written and the global one was not, so this
     * is a partial success - reporting plain success would tell the
     * caller the global message had been logged */
    report("global syslog alongside a serviced entry still reports partial success",
           PMIX_ERR_PARTIAL_SUCCESS == rc);
    report("the local syslog entry was marked serviced",
           PMIX_INFO_OP_IS_COMPLETE(&data[1]));
    report("the global syslog entry was left unserviced",
           !PMIX_INFO_OP_IS_COMPLETE(&data[0]));
    PMIX_INFO_DESTRUCT(&data[0]);
    PMIX_INFO_DESTRUCT(&data[1]);
}

static void test_gsys_on_gateway(void)
{
    pmix_info_t data[1];
    pmix_status_t rc;

    set_gateway(true);
    PMIX_INFO_LOAD(&data[0], PMIX_LOG_GLOBAL_SYSLOG, "gsys unit test", PMIX_STRING);
    rc = PMIx_Log(data, 1, NULL, 0);
    report("global syslog on a gateway is written locally",
           PMIX_SUCCESS == rc);
    PMIX_INFO_DESTRUCT(&data[0]);
    set_gateway(false);
}

/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    pmix_status_t rc;
    static pmix_server_module_t mymodule = {0};
    PMIX_HIDE_UNUSED_PARAMS(argc, argv);

    setvbuf(stdout, NULL, _IONBF, 0);

    /* the syslog module is the only one these cases need, and leaving
     * stdfd out keeps a stray channel from covering for it */
    putenv((char *) "PMIX_MCA_plog_base_order=syslog");

    rc = PMIx_server_init(&mymodule, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "PMIx_server_init failed: %s\n", PMIx_Error_string(rc));
        return 1;
    }
    /* with no host log function, PMIx_Log processes locally and returns
     * the framework's own status rather than the host's */
    pmix_host_server.log = NULL;
    pmix_host_server.log2 = NULL;

    fprintf(stdout, "\n=== plog global syslog tests ===\n\n");

    test_gsys_alone_declined();
    test_gsys_with_lsys_declined();
    test_gsys_on_gateway();

    fprintf(stdout, "\nResults: %d passed, %d failed\n\n", npass, nfail);

    PMIx_server_finalize();

    return (nfail > 0) ? 1 : 0;
}
