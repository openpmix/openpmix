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
 * The op-object test at the end needs neither a framework nor a server:
 * it exercises the peer-reference discipline of pmix_pstat_op_t, which is
 * pure class construction and destruction.
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

/* An op's peer list has to OWN the peers it records. A periodic monitor
 * outlives the client it is sampling: the server releases that client's
 * pmix_peer_t the moment the connection drops, and nothing else holds
 * it. With a borrowed pointer the next timer fire read peer->info->pid
 * out of freed memory - so PMIX_PSTAT_APPEND_PEER_UNIQUE takes a
 * reference and opdes() gives it back. Both halves are checked here,
 * because either one alone is a bug: without the retain it is a
 * use-after-free, without the release it is a leak. */
static void op_peer_refs(void)
{
    pmix_pstat_op_t *op;
    pmix_peer_t *peer;

    op = PMIX_NEW(pmix_pstat_op_t);
    peer = PMIX_NEW(pmix_peer_t);
    report("op: a fresh peer starts with one reference",
           1 == peer->super.obj_reference_count);

    PMIX_PSTAT_APPEND_PEER_UNIQUE(&op->peers, peer);
    report("op: appending a peer takes a reference on it",
           2 == peer->super.obj_reference_count &&
           1 == pmix_list_get_size(&op->peers));

    /* the macro dedups, and must not take a second reference when it
     * declines to add a second entry */
    PMIX_PSTAT_APPEND_PEER_UNIQUE(&op->peers, peer);
    report("op: re-appending the same peer changes nothing",
           2 == peer->super.obj_reference_count &&
           1 == pmix_list_get_size(&op->peers));

    /* the client disconnects - the server drops the only other
     * reference, and the op's is what keeps the object alive for the
     * next sample */
    PMIX_RELEASE(peer);
    report("op: the peer outlives its client disconnecting",
           1 == peer->super.obj_reference_count);

    /* hold a probe reference so we can watch the op give its own back
     * rather than merely not crashing */
    PMIX_RETAIN(peer);
    PMIX_RELEASE(op);
    report("op: releasing the op releases its peers",
           1 == peer->super.obj_reference_count);
    PMIX_RELEASE(peer);
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
    op_peer_refs();

    fprintf(stdout, "\nResults: %d passed, %d failed\n\n", npass, nfail);

    return (nfail > 0) ? 1 : 0;
}
