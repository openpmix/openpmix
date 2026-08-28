/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Regression test: pfexec's MCA parameters must still be there once the
 * framework has been opened.
 *
 * pmix_pfexec_register() runs from pmix_register_params(), i.e. inside
 * pmix_rte_init(), because only a launcher opens pfexec and registering
 * from pmix_pfexec_base_open() left the parameters with no registrar at
 * all. But pmix_pfexec_base_open() then ran a
 * memset(&pmix_pfexec_globals, 0, ...) over the whole struct, which is
 * where those parameters live - so every value the registration had just
 * supplied was zeroed a few hundred microseconds later:
 *
 *   timeout_before_sigkill -> 0, so the SIGCONT/SIGTERM/SIGKILL sequence
 *      lost the grace period it exists to give, and a child that would
 *      have exited on the SIGTERM is SIGKILLed on the next turn of the
 *      event loop instead;
 *   poll_interval / poll_max_interval -> 0, which pfexec_poll_arm()
 *      repairs to the compile-time floor - so the reaping poll ran at a
 *      fixed 50 ms with no backoff, and both parameters were inert.
 *
 * None of that is visible in a return code, so the test asks the library
 * directly. It sets all three parameters to values that are neither the
 * built-in defaults nor zero, comes up as a LAUNCHER (which is what opens
 * pfexec), and reads them back.
 */

#include "src/include/pmix_config.h"

#include "include/pmix.h"
#include "include/pmix_tool.h"
#include "src/common/pmix_pfexec.h"
#include "src/include/pmix_globals.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* deliberately unlike PMIX_PFEXEC_POLL_INTERVAL / _MAX_INTERVAL and the
 * 1-second sigkill default, so a pass cannot be a coincidence */
#define TEST_SIGKILL_TIMEOUT 7
#define TEST_POLL_INTERVAL   111
#define TEST_POLL_MAX        2222

static int failures = 0;

static void report(const char *what, int ok, const char *why)
{
    if (ok) {
        fprintf(stderr, "PASS: %s\n", what);
    } else {
        fprintf(stderr, "FAIL: %s%s%s\n", what, (NULL == why) ? "" : " - ",
                (NULL == why) ? "" : why);
        ++failures;
    }
}

static void report_int(const char *what, int expected, int actual)
{
    char buf[128];

    if (expected == actual) {
        fprintf(stderr, "PASS: %s\n", what);
        return;
    }
    snprintf(buf, sizeof(buf), "expected %d, got %d", expected, actual);
    report(what, 0, buf);
}

int main(int argc, char **argv)
{
    pmix_proc_t myproc;
    pmix_info_t tinfo[2];
    pmix_status_t rc;
    char val[32];

    (void) argc;
    (void) argv;

    snprintf(val, sizeof(val), "%d", TEST_SIGKILL_TIMEOUT);
    setenv("PMIX_MCA_pfexec_base_sigkill_timeout", val, 1);
    snprintf(val, sizeof(val), "%d", TEST_POLL_INTERVAL);
    setenv("PMIX_MCA_pfexec_base_poll_interval", val, 1);
    snprintf(val, sizeof(val), "%d", TEST_POLL_MAX);
    setenv("PMIX_MCA_pfexec_base_poll_max_interval", val, 1);

    /* a LAUNCHER opens pfexec; DO_NOT_CONNECT keeps the test from
     * needing a server anywhere */
    PMIX_INFO_LOAD(&tinfo[0], PMIX_LAUNCHER, NULL, PMIX_BOOL);
    PMIX_INFO_LOAD(&tinfo[1], PMIX_TOOL_DO_NOT_CONNECT, NULL, PMIX_BOOL);
    rc = PMIx_tool_init(&myproc, tinfo, 2);
    PMIX_INFO_DESTRUCT(&tinfo[0]);
    PMIX_INFO_DESTRUCT(&tinfo[1]);
    if (PMIX_SUCCESS != rc) {
        report("launcher initialized", 0, PMIx_Error_string(rc));
        return 1;
    }
    report("launcher initialized", 1, NULL);

    /* without this the rest of the test is vacuous: the values would
     * trivially still be whatever the registration left, because nothing
     * had run that could disturb them */
    report("pfexec framework was opened", pmix_pfexec_globals.initialized,
           "a LAUNCHER should have opened pfexec");

    report_int("sigkill timeout survived pfexec_base_open",
               TEST_SIGKILL_TIMEOUT, pmix_pfexec_globals.timeout_before_sigkill);
    report_int("poll interval survived pfexec_base_open",
               TEST_POLL_INTERVAL, pmix_pfexec_globals.poll_interval);
    report_int("poll max interval survived pfexec_base_open",
               TEST_POLL_MAX, pmix_pfexec_globals.poll_max_interval);

    rc = PMIx_tool_finalize();
    report("launcher finalized", PMIX_SUCCESS == rc, PMIx_Error_string(rc));

    fprintf(stderr, "%s: %d failure(s)\n", (0 == failures) ? "SUCCESS" : "FAILURE",
            failures);
    return (0 == failures) ? 0 : 1;
}
