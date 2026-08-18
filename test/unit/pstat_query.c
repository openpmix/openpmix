/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/*
 * Unit tests for the pstat framework's query contract - the entry point
 * every component implements (pmix_pstat.query).
 *
 * Everything a query is handed can have come off the wire. A client's
 * PMIx_Process_monitor request is unpacked by pmix_server_monitor() and
 * passed down without anything in between looking at it, and
 * PMIx_Check_key compares only the key - it says nothing about the type
 * the sender declared for the value. So a value typed as an integer was
 * being read as a pmix_data_array_t, and a PMIX_MONITOR_ID typed as an
 * integer was being handed to strdup: in both cases the union's bytes
 * become a pointer. The malformed cases below are those, and they fail
 * by crashing rather than by returning the wrong code.
 *
 * These assertions are framework contract, not component behavior, so
 * they hold for whichever component this host selects - plinux, pmacos
 * or test. That is deliberate: the three implement query separately, and
 * a rule that only one of them honors is not a rule.
 *
 * This needs the MCA up but no server: pmix_init_util() establishes the
 * install dirs, the variable system and the component repository, and
 * only the per-process branch of query touches server state - which is
 * why every case here asks for node, disk or network statistics.
 */

#include "src/include/pmix_config.h"

#include "pmix_common.h"
#include "src/mca/base/pmix_base.h"
#include "src/mca/base/pmix_mca_base_framework.h"
#include "src/mca/pstat/base/base.h"
#include "src/runtime/pmix_init_util.h"

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

/* the value a malformed case declares as an integer; any component that
 * reads it as a pointer dies here rather than returning a status */
#define BOGUS_POINTER_BITS 42

int main(int argc, char **argv)
{
    pmix_proc_t requestor;
    pmix_info_t monitor, directive;
    pmix_info_t *results;
    size_t nresults;
    pmix_status_t rc;
    int ret;

    (void) argc;
    (void) argv;

    /* keep the caller's own MCA parameter files out of the results, as
     * the mca/ suite does */
    setenv("PMIX_MCA_mca_base_param_files", "none", 1);

    ret = pmix_init_util(NULL, 0, NULL);
    if (PMIX_SUCCESS != ret) {
        fprintf(stderr, "pmix_init_util failed: %d\n", ret);
        return 1;
    }
    ret = pmix_mca_base_framework_open(&pmix_pstat_base_framework, PMIX_MCA_BASE_OPEN_DEFAULT);
    if (PMIX_SUCCESS != ret) {
        fprintf(stderr, "pstat framework failed to open: %d\n", ret);
        return 1;
    }
    ret = pmix_pstat_base_select();
    if (PMIX_SUCCESS != ret) {
        fprintf(stderr, "pstat selection failed: %d\n", ret);
        return 1;
    }
    fprintf(stdout, "\npstat component in use: %s\n\n",
            (NULL == pmix_pstat_base_component) ? "none (unsupported stubs)"
                                                : pmix_pstat_base_component->pmix_mca_component_name);

    PMIx_Load_procid(&requestor, "pstat.query.test", 0);

    /* ------------------------------------------------------------------
     * A cancel names the monitor to stop in the monitor's own value, so
     * that value has to actually be a string.
     */
    PMIX_INFO_CONSTRUCT(&monitor);
    PMIX_LOAD_KEY(monitor.key, PMIX_MONITOR_CANCEL);
    monitor.value.type = PMIX_SIZE;
    monitor.value.data.size = BOGUS_POINTER_BITS;
    results = NULL;
    nresults = 0;
    rc = pmix_pstat.query(&requestor, &monitor, PMIX_SUCCESS, NULL, 0, &results, &nresults);
    report("cancel: a non-string id is rejected", PMIX_ERR_BAD_PARAM == rc);
    PMIX_INFO_DESTRUCT(&monitor);

    /* A correctly typed value can still carry no string at all. */
    PMIX_INFO_CONSTRUCT(&monitor);
    PMIX_LOAD_KEY(monitor.key, PMIX_MONITOR_CANCEL);
    monitor.value.type = PMIX_STRING;
    monitor.value.data.string = NULL;
    rc = pmix_pstat.query(&requestor, &monitor, PMIX_SUCCESS, NULL, 0, &results, &nresults);
    report("cancel: a NULL id is rejected", PMIX_ERR_BAD_PARAM == rc);
    PMIX_INFO_DESTRUCT(&monitor);

    /* Cancelling something that is not running is deliberately not an
     * error - the caller cannot know whether its monitor already ended.
     * With nothing on the ops list this also covers the empty-list
     * shortcut; with an op on it that carries no id (a rate given
     * without a PMIX_MONITOR_ID is legal) it covers the walk that must
     * not hand strcmp a NULL. */
    PMIX_INFO_CONSTRUCT(&monitor);
    PMIX_LOAD_KEY(monitor.key, PMIX_MONITOR_CANCEL);
    PMIX_INFO_LOAD(&monitor, PMIX_MONITOR_CANCEL, "no.such.monitor", PMIX_STRING);
    rc = pmix_pstat.query(&requestor, &monitor, PMIX_SUCCESS, NULL, 0, &results, &nresults);
    report("cancel: an unknown id is not an error", PMIX_SUCCESS == rc);
    PMIX_INFO_DESTRUCT(&monitor);

    /* ------------------------------------------------------------------
     * The monitor's value carries the list of fields being asked for as
     * a data array. Anything else is malformed - and must not be read as
     * one.
     */
    PMIX_INFO_CONSTRUCT(&monitor);
    PMIX_LOAD_KEY(monitor.key, PMIX_MONITOR_NET_RESOURCE_USAGE);
    monitor.value.type = PMIX_SIZE;
    monitor.value.data.size = BOGUS_POINTER_BITS;
    results = NULL;
    nresults = 0;
    rc = pmix_pstat.query(&requestor, &monitor, PMIX_SUCCESS, NULL, 0, &results, &nresults);
    report("fields: a non-array monitor value is rejected", PMIX_ERR_BAD_PARAM == rc);
    report("fields: a rejected request returns no results",
           NULL == results && 0 == nresults);
    PMIX_INFO_DESTRUCT(&monitor);

    /* An absent value is not malformed: it is how a caller asks for
     * every field in the category. */
    PMIX_INFO_CONSTRUCT(&monitor);
    PMIX_LOAD_KEY(monitor.key, PMIX_MONITOR_NET_RESOURCE_USAGE);
    monitor.value.type = PMIX_UNDEF;
    results = NULL;
    nresults = 0;
    rc = pmix_pstat.query(&requestor, &monitor, PMIX_SUCCESS, NULL, 0, &results, &nresults);
    report("fields: an undefined monitor value means every field", PMIX_SUCCESS == rc);
    if (NULL != results) {
        PMIX_INFO_FREE(results, nresults);
    }
    PMIX_INFO_DESTRUCT(&monitor);

    /* ------------------------------------------------------------------
     * PMIX_MONITOR_ID is the caller's handle for the monitor. It is
     * strdup'ed, so it too has to really be a string.
     */
    PMIX_INFO_CONSTRUCT(&monitor);
    PMIX_LOAD_KEY(monitor.key, PMIX_MONITOR_NET_RESOURCE_USAGE);
    monitor.value.type = PMIX_UNDEF;
    PMIX_INFO_CONSTRUCT(&directive);
    PMIX_LOAD_KEY(directive.key, PMIX_MONITOR_ID);
    directive.value.type = PMIX_SIZE;
    directive.value.data.size = BOGUS_POINTER_BITS;
    results = NULL;
    nresults = 0;
    rc = pmix_pstat.query(&requestor, &monitor, PMIX_SUCCESS, &directive, 1, &results, &nresults);
    report("directives: a non-string monitor id is rejected", PMIX_ERR_BAD_PARAM == rc);
    PMIX_INFO_DESTRUCT(&directive);
    PMIX_INFO_DESTRUCT(&monitor);

    /* ------------------------------------------------------------------
     * A monitor key naming statistics this framework does not collect
     * has to say so, rather than reporting a success it did nothing for
     * - the caller falls back to the host environment on that answer.
     */
    PMIX_INFO_CONSTRUCT(&monitor);
    PMIX_INFO_LOAD(&monitor, PMIX_JOB_SIZE, NULL, PMIX_BOOL);
    results = NULL;
    nresults = 0;
    rc = pmix_pstat.query(&requestor, &monitor, PMIX_SUCCESS, NULL, 0, &results, &nresults);
    report("dispatch: an unrecognized monitor key is not supported",
           PMIX_ERR_NOT_SUPPORTED == rc);
    PMIX_INFO_DESTRUCT(&monitor);

    (void) pmix_mca_base_framework_close(&pmix_pstat_base_framework);

    fprintf(stdout, "\nResults: %d passed, %d failed\n\n", npass, nfail);

    return (nfail > 0) ? 1 : 0;
}
