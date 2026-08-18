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
#include "src/include/pmix_globals.h"
#include "src/mca/pstat/base/base.h"
#include "src/runtime/pmix_init_util.h"
#include "src/runtime/pmix_progress_threads.h"
#include "src/util/pmix_printf.h"

#include "src/include/pmix_stdatomic.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

/* A periodic op's timer belongs to the framework's event base, and it is
 * the base that re-arms it - PMIX_PSTAT_OP_START asks for EV_PERSIST
 * precisely so that no sampler has to. That is not a style choice, and
 * this is the case that shows why.
 *
 * opdes() ends an op with pmix_event_del(). From a thread that is not the
 * base's own - which is what a PMIX_MONITOR_CANCEL is once the framework
 * has its own progress thread - that call waits for a callback already
 * running on the event, so the op is safe to free when it returns. But it
 * removes the event once, up front. While a sampler did its own re-arming
 * on the way out, that re-arm ran after the removal and outside the base's
 * lock, so pmix_event_del returned to a freshly armed timer and the op was
 * freed with a live timer pointing into it. The next tick then ran on
 * freed memory.
 *
 * The stub deliberately ignores its cbdata, so a tick that should not have
 * happened is counted and reported rather than dereferencing a freed op and
 * taking the test out with it. */
static pmix_atomic_int32_t stub_fires;
static pmix_atomic_bool_t stub_inside;

static void stub_update(int sd, short args, void *cbdata)
{
    PMIX_HIDE_UNUSED_PARAMS(sd, args, cbdata);

    atomic_fetch_add(&stub_fires, 1);
    pmix_atomic_set_bool(&stub_inside);
    /* hold the callback open long enough that the release below lands
     * while we are still inside it - that is the interleaving that broke */
    usleep(300000);
    pmix_atomic_unset_bool(&stub_inside);
}

static void op_timer_lifetime(void)
{
    pmix_pstat_op_t *op;
    int32_t fires;
    int waited;
    int rc;

    /* opdes() deletes an op's timer through pmix_event_del_checked(),
     * which declines to delete anything while the library itself has no
     * event base - see its definition in pmix_globals.c. pmix_init_util()
     * does not create one, but the library always has one by the time a
     * pstat op can exist (pmix_rte_init makes it, and pmix_rte_finalize
     * does not clear it until long after the frameworks have closed), so
     * stand one up here. Without it this test would be watching a no-op
     * and would leave a live timer on a freed op. */
    pmix_globals.evbase = pmix_progress_thread_init(NULL);
    if (NULL == pmix_globals.evbase) {
        report("op timer: the library's progress thread starts", 0);
        return;
    }

    rc = pmix_mca_base_framework_open(&pmix_pstat_base_framework, PMIX_MCA_BASE_OPEN_DEFAULT);
    report("op timer: the framework opens", PMIX_SUCCESS == rc);
    if (PMIX_SUCCESS != rc) {
        (void) pmix_progress_thread_stop(NULL);
        pmix_globals.evbase = NULL;
        return;
    }

    atomic_store(&stub_fires, 0);
    pmix_atomic_unset_bool(&stub_inside);

    op = PMIX_NEW(pmix_pstat_op_t);
    PMIX_PSTAT_OP_START(op, 1, stub_update);

    /* Nothing here re-arms the timer, so a second sample can only come
     * from the event base - which is the assertion that the op really was
     * armed as a persistent timer. A one-shot fires once and stops. */
    for (waited = 0; waited < 6000 && atomic_load(&stub_fires) < 2; waited += 10) {
        usleep(10000);
    }
    report("op timer: the event base re-arms a periodic op",
           2 <= atomic_load(&stub_fires));

    /* wait until the sampler is inside the callback, then end the op from
     * this thread - the one the base is not running on */
    for (waited = 0; waited < 2000 && !pmix_atomic_check_bool(&stub_inside); waited += 10) {
        usleep(10000);
    }
    PMIX_RELEASE(op);

    report("op timer: releasing an op waits for the sample in flight",
           !pmix_atomic_check_bool(&stub_inside));

    /* Nothing may fire after the release, so the count is stable from
     * here. Two intervals is long enough for a timer that survived the
     * delete to give itself away. */
    fires = atomic_load(&stub_fires);
    sleep(2);
    report("op timer: a released op never samples again",
           fires == atomic_load(&stub_fires));

    rc = pmix_mca_base_framework_close(&pmix_pstat_base_framework);
    report("op timer: the framework closes after a released op",
           PMIX_SUCCESS == rc);

    (void) pmix_progress_thread_stop(NULL);
    pmix_globals.evbase = NULL;
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

    /* The library does this in pmix_rte_init(), before it creates any
     * event base; pmix_init_util() below does not, because nothing that
     * stops at util-init has a second thread. This program does, and
     * op_timer_lifetime() ends an op from a thread that is not the base's
     * own - which libevent only makes safe when its locking is turned on.
     * It has to be turned on before any base is created, so it goes here
     * rather than beside the framework open. */
    pmix_event_use_threads();

    rc = pmix_init_util(NULL, 0, NULL);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "pmix_init_util failed: %d\n", rc);
        return 1;
    }

    cycle("first cycle");
    cycle("second cycle");
    op_peer_refs();
    op_timer_lifetime();

    fprintf(stdout, "\nResults: %d passed, %d failed\n\n", npass, nfail);

    return (nfail > 0) ? 1 : 0;
}
