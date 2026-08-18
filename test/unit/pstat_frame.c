/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/*
 * Unit tests for the pstat framework's open/close lifecycle
 * (src/mca/pstat/base/pstat_base_frame.c).
 *
 * Everything here runs with pstat_base_use_separate_thread turned on,
 * because that is the branch the defects were in and it is off by
 * default - so an ordinary run of the rest of the suite never reaches
 * this code at all.
 *
 * With that parameter set, open() stands up a dedicated "PSTAT" progress
 * thread and close() takes it down. Taking it down used to call
 * pmix_progress_thread_stop() *and* pmix_event_base_free() on the same
 * base, but stop() drops the last reference to the progress tracker and
 * the tracker's destructor is what frees the base - so the base was
 * freed twice. A regression shows up as an allocator abort rather than a
 * failed assertion, which is why simply completing the cycle is the
 * assertion here.
 *
 * The second cycle is not redundant. Frameworks are opened and closed
 * once per PMIx_server_init/finalize pair, so a close that leaves the
 * framework or its globals in a bad state only shows up on the way back
 * up.
 *
 * This needs the MCA up but no server: pmix_init_util() establishes the
 * install dirs, the variable system and the component repository, which
 * is all that opening a framework requires.
 */

#include "src/include/pmix_config.h"

#include "pmix_common.h"
#include "src/mca/base/pmix_base.h"
#include "src/mca/base/pmix_mca_base_framework.h"
#include "src/mca/pstat/base/base.h"
#include "src/runtime/pmix_init_util.h"
#include "src/util/pmix_printf.h"

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

/* one full open/close of the framework, with every state check that can
 * be made from outside it */
static void cycle(const char *which)
{
    char *label;
    int rc;

    rc = pmix_mca_base_framework_open(&pmix_pstat_base_framework, PMIX_MCA_BASE_OPEN_DEFAULT);
    pmix_asprintf(&label, "%s: the framework opens", which);
    report(label, PMIX_SUCCESS == rc);
    free(label);
    if (PMIX_SUCCESS != rc) {
        return;
    }

    /* open() must have produced an event base for the sampling timers to
     * be armed on - a periodic monitor has nowhere to run without one */
    pmix_asprintf(&label, "%s: open established an event base", which);
    report(label, NULL != pmix_pstat_base.evbase);
    free(label);

    /* This is the call that used to free the event base a second time. */
    rc = pmix_mca_base_framework_close(&pmix_pstat_base_framework);
    pmix_asprintf(&label, "%s: the framework closes", which);
    report(label, PMIX_SUCCESS == rc && 0 == pmix_pstat_base_framework.framework_refcnt);
    free(label);

    /* close() gave the base back, so nothing may still be holding it */
    pmix_asprintf(&label, "%s: close released the event base", which);
    report(label, NULL == pmix_pstat_base.evbase);
    free(label);

    /* the selected module belongs to a component close() has just let go
     * of - under --enable-mca-dso that component's plugin is unloaded, so
     * a stale pointer here would name memory that is no longer mapped */
    pmix_asprintf(&label, "%s: close restored the unsupported module", which);
    report(label,
           NULL == pmix_pstat_base_component && PMIX_ERR_NOT_SUPPORTED == pmix_pstat.init()
               && PMIX_ERR_NOT_SUPPORTED == pmix_pstat.finalize());
    free(label);
}

int main(int argc, char **argv)
{
    int rc;

    (void) argc;
    (void) argv;

    /* keep the caller's own MCA parameter files out of the results, as
     * the mca/ suite does */
    setenv("PMIX_MCA_mca_base_param_files", "none", 1);
    /* the whole point of this program - take the dedicated-thread branch */
    setenv("PMIX_MCA_pstat_base_use_separate_thread", "1", 1);

    rc = pmix_init_util(NULL, 0, NULL);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "pmix_init_util failed: %d\n", rc);
        return 1;
    }

    cycle("first cycle");
    cycle("second cycle");

    fprintf(stdout, "\nResults: %d passed, %d failed\n\n", npass, nfail);

    return (nfail > 0) ? 1 : 0;
}
