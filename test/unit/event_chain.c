/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Unit tests for the event notification system (src/event).
 *
 * The process is initialized as a PMIx server with a stub host
 * module. In the server role, registrations for ordinary status
 * codes never leave the process and locally generated notifications
 * drive the full local event chain, so the registration placement
 * logic, chain progression, caching, and completion protocols can
 * all be exercised in a single process. The host-module upcall for
 * environmental (system) event registrations is exercised by
 * pointing pmix_host_server.register_events at a stub.
 *
 * Test cases:
 *
 *  pmix_notify_check_affected: NULL interest/affected lists, overlap,
 *      disjoint sets, wildcard ranks.
 *  pmix_notify_check_range: UNDEF/GLOBAL, NAMESPACE, PROC_LOCAL
 *      (source must be this process), CUSTOM (listed proc, wildcard
 *      rank, unlisted proc).
 *
 *  chain order: first-overall (registered with no codes), single-code
 *      handlers positioned with BEFORE/AFTER (the AFTER registrant
 *      deliberately carries no name of its own), a multi-code handler,
 *      two default handlers (the earlier one filtered out by its
 *      affected-proc interest), and a last-overall handler must be
 *      invoked in exactly the order first -> singles -> multi ->
 *      unfiltered default -> last, and the last handler must see one
 *      accumulated result per prior handler.
 *
 *  PMIX_EVENT_ACTION_COMPLETE from a handler ends the chain.
 *  PMIX_EVENT_ONESHOT handlers fire exactly once.
 *  deregistered handlers are not invoked.
 *  a handler registered with PMIX_RANGE_PROC_LOCAL fires for locally
 *      generated events.
 *
 *  cached replay: an event notified before any handler is registered
 *      is replayed to a subsequently registered handler with the
 *      original source preserved.
 *
 *  PMIX_RANGE_RM notifications complete the caller's callback.
 *
 *  host register_events upcall: the registration callback fires
 *      exactly once whether the host returns PMIX_OPERATION_SUCCEEDED
 *      or PMIX_SUCCESS followed by its own callback.
 *
 *  enviro event handshake: a system code is activated with the host on
 *      the first registration for it and deactivated when the last one
 *      goes away, counting the server's own registrations and those of
 *      its local clients together; intervening registrations do not
 *      repeat the host call, a code is re-activated if it is registered
 *      again after deactivation, and non-system codes are never handed
 *      to the host.
 *
 *  first-overall handler with multiple codes honors its
 *      affected-proc interest list.
 *
 *  default-handler registration: a client registering with no codes is
 *      recorded against the server's PMIX_MAX_ERR_CONSTANT entry, which
 *      is created if this is the first such registration; a second one
 *      joins the same entry; and deregistration finds what registration
 *      created.
 *
 *  internal observers (openpmix#4059): a library observer records the
 *      same per-code interest a handler does, sees an event even when an
 *      application handler ends the chain with PMIX_EVENT_ACTION_COMPLETE,
 *      runs ahead of the entire chain, may deregister itself from inside
 *      its own callback, and hands its object back through the release
 *      callback the registry holds for it.
 */

#include "src/include/pmix_config.h"

#include "include/pmix.h"
#include "include/pmix_server.h"

#include "src/event/pmix_event.h"
#include "src/include/pmix_globals.h"
#include "src/server/pmix_server_ops.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* arbitrary test status codes - outside both the reserved system
 * event range (-230..-330) and the range of defined PMIx constants */
#define EVUT_CODE_ORDER    -9001
#define EVUT_CODE_ORDER2   -9002
#define EVUT_CODE_COMPLETE -9003
#define EVUT_CODE_ONESHOT  -9004
#define EVUT_CODE_DEREG    -9005
#define EVUT_CODE_CACHED   -9006
#define EVUT_CODE_RM       -9007
#define EVUT_CODE_FIRSTAFF -9008
#define EVUT_CODE_FIRSTAFF2 -9009
#define EVUT_CODE_PROCLOCAL -9010
#define EVUT_CODE_OBSERVER  -9011
#define EVUT_CODE_OBSERVER2 -9012
#define EVUT_CODE_BLOCKING  -9013

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

/* poll for a counter to reach a target value - the event chain
 * progresses asynchronously on the progress thread */
static int wait_for_count(volatile int *ctr, int target)
{
    int i;

    for (i = 0; i < 500; i++) {
        if (target <= *ctr) {
            return 1;
        }
        usleep(10000);
    }
    return target <= *ctr;
}

/* give the progress thread time to deliver anything still pending
 * before checking that a counter did NOT advance */
static void grace(void)
{
    usleep(250000);
}

/* ------------------------------------------------------------------ */
/* Event handlers                                                      */
/* ------------------------------------------------------------------ */

#define EVUT_MAX_ORDER 16

static volatile int norder = 0;
static size_t order[EVUT_MAX_ORDER];
static pmix_proc_t last_source;
static volatile int last_nresults = -1;

/* record our registration index and the event source, then let the
 * chain continue */
static void chain_hdlr(size_t evhdlr_registration_id, pmix_status_t status,
                       const pmix_proc_t *source, pmix_info_t info[], size_t ninfo,
                       pmix_info_t *results, size_t nresults,
                       pmix_event_notification_cbfunc_fn_t cbfunc, void *cbdata)
{
    PMIX_HIDE_UNUSED_PARAMS(status, info, ninfo, results, nresults);

    if (norder < EVUT_MAX_ORDER) {
        order[norder] = evhdlr_registration_id;
    }
    PMIX_LOAD_PROCID(&last_source, source->nspace, source->rank);
    ++norder;
    if (NULL != cbfunc) {
        cbfunc(PMIX_SUCCESS, NULL, 0, NULL, NULL, cbdata);
    }
}

/* same as chain_hdlr, but also record how many accumulated results
 * we were handed */
static void last_hdlr(size_t evhdlr_registration_id, pmix_status_t status,
                      const pmix_proc_t *source, pmix_info_t info[], size_t ninfo,
                      pmix_info_t *results, size_t nresults,
                      pmix_event_notification_cbfunc_fn_t cbfunc, void *cbdata)
{
    PMIX_HIDE_UNUSED_PARAMS(status, source, info, ninfo, results);

    if (norder < EVUT_MAX_ORDER) {
        order[norder] = evhdlr_registration_id;
    }
    ++norder;
    last_nresults = (int) nresults;
    if (NULL != cbfunc) {
        cbfunc(PMIX_SUCCESS, NULL, 0, NULL, NULL, cbdata);
    }
}

/* count invocations and let the chain continue */
static volatile int counted = 0;

static void count_hdlr(size_t evhdlr_registration_id, pmix_status_t status,
                       const pmix_proc_t *source, pmix_info_t info[], size_t ninfo,
                       pmix_info_t *results, size_t nresults,
                       pmix_event_notification_cbfunc_fn_t cbfunc, void *cbdata)
{
    PMIX_HIDE_UNUSED_PARAMS(evhdlr_registration_id, status, source, info, ninfo,
                            results, nresults);

    ++counted;
    if (NULL != cbfunc) {
        cbfunc(PMIX_SUCCESS, NULL, 0, NULL, NULL, cbdata);
    }
}

/* count invocations and terminate the chain */
static volatile int completed = 0;

static void complete_hdlr(size_t evhdlr_registration_id, pmix_status_t status,
                          const pmix_proc_t *source, pmix_info_t info[], size_t ninfo,
                          pmix_info_t *results, size_t nresults,
                          pmix_event_notification_cbfunc_fn_t cbfunc, void *cbdata)
{
    PMIX_HIDE_UNUSED_PARAMS(evhdlr_registration_id, status, source, info, ninfo,
                            results, nresults);

    ++completed;
    if (NULL != cbfunc) {
        cbfunc(PMIX_EVENT_ACTION_COMPLETE, NULL, 0, NULL, NULL, cbdata);
    }
}

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

/* blocking registration - the blocking form returns the handler
 * reference as the status on success */
static size_t reghdlr(pmix_status_t *codes, size_t ncodes, pmix_info_t *info,
                      size_t ninfo, pmix_notification_fn_t fn)
{
    pmix_status_t rc;

    rc = PMIx_Register_event_handler(codes, ncodes, info, ninfo, fn, NULL, NULL);
    if (0 > rc) {
        return SIZE_MAX;
    }
    return (size_t) rc;
}

static pmix_status_t notify_nocache(pmix_status_t code, pmix_info_t *xtra, size_t nxtra)
{
    pmix_info_t info[4];
    size_t n, ninfo = 0;
    pmix_status_t rc;

    PMIX_INFO_LOAD(&info[ninfo], PMIX_EVENT_DO_NOT_CACHE, NULL, PMIX_BOOL);
    ++ninfo;
    for (n = 0; NULL != xtra && n < nxtra; n++) {
        PMIX_INFO_XFER(&info[ninfo], &xtra[n]);
        ++ninfo;
    }
    rc = PMIx_Notify_event(code, &pmix_globals.myid, PMIX_RANGE_UNDEF, info, ninfo,
                           NULL, NULL);
    for (n = 0; n < ninfo; n++) {
        PMIX_INFO_DESTRUCT(&info[n]);
    }
    return rc;
}

/* ------------------------------------------------------------------ */
/* pmix_notify_check_affected                                          */
/* ------------------------------------------------------------------ */

static void test_check_affected(void)
{
    pmix_proc_t interested[2], affected[2];

    PMIX_LOAD_PROCID(&interested[0], "aff-nsp", 1);
    PMIX_LOAD_PROCID(&interested[1], "aff-nsp", 3);
    PMIX_LOAD_PROCID(&affected[0], "aff-nsp", 3);
    PMIX_LOAD_PROCID(&affected[1], "other-nsp", 3);

    report("check_affected: no interest list accepts all",
           pmix_notify_check_affected(NULL, 0, affected, 2));
    report("check_affected: no affected list accepts all",
           pmix_notify_check_affected(interested, 2, NULL, 0));
    report("check_affected: overlapping procs accepted",
           pmix_notify_check_affected(interested, 2, affected, 2));
    report("check_affected: disjoint procs rejected",
           !pmix_notify_check_affected(interested, 1, &affected[1], 1));

    /* wildcard rank in the interest list matches any rank in
     * the same nspace */
    PMIX_LOAD_PROCID(&interested[0], "aff-nsp", PMIX_RANK_WILDCARD);
    PMIX_LOAD_PROCID(&affected[0], "aff-nsp", 42);
    report("check_affected: wildcard rank matches nspace",
           pmix_notify_check_affected(interested, 1, affected, 1));
}

/* ------------------------------------------------------------------ */
/* pmix_notify_check_range                                             */
/* ------------------------------------------------------------------ */

static void test_check_range(void)
{
    pmix_range_trkr_t rng;
    pmix_proc_t procs[2], src;

    /* undefined and global ranges accept any source */
    rng.range = PMIX_RANGE_UNDEF;
    rng.procs = NULL;
    rng.nprocs = 0;
    PMIX_LOAD_PROCID(&src, "some-nsp", 4);
    report("check_range: UNDEF accepts any source",
           pmix_notify_check_range(&rng, &src));
    rng.range = PMIX_RANGE_GLOBAL;
    report("check_range: GLOBAL accepts any source",
           pmix_notify_check_range(&rng, &src));

    /* namespace range compares nspaces */
    rng.range = PMIX_RANGE_NAMESPACE;
    rng.procs = procs;
    rng.nprocs = 1;
    PMIX_LOAD_PROCID(&procs[0], "some-nsp", PMIX_RANK_WILDCARD);
    report("check_range: NAMESPACE accepts matching nspace",
           pmix_notify_check_range(&rng, &src));
    PMIX_LOAD_PROCID(&src, "other-nsp", 4);
    report("check_range: NAMESPACE rejects other nspace",
           !pmix_notify_check_range(&rng, &src));

    /* proc-local range accepts only events generated by this
     * very process */
    rng.range = PMIX_RANGE_PROC_LOCAL;
    rng.procs = NULL;
    rng.nprocs = 0;
    report("check_range: PROC_LOCAL accepts our own process",
           pmix_notify_check_range(&rng, &pmix_globals.myid));
    PMIX_LOAD_PROCID(&src, "other-nsp", 4);
    report("check_range: PROC_LOCAL rejects other process",
           !pmix_notify_check_range(&rng, &src));

    /* custom range accepts only the listed procs */
    rng.range = PMIX_RANGE_CUSTOM;
    rng.procs = procs;
    rng.nprocs = 2;
    PMIX_LOAD_PROCID(&procs[0], "cust-nsp", 3);
    PMIX_LOAD_PROCID(&procs[1], "wild-nsp", PMIX_RANK_WILDCARD);
    PMIX_LOAD_PROCID(&src, "cust-nsp", 3);
    report("check_range: CUSTOM accepts listed proc",
           pmix_notify_check_range(&rng, &src));
    PMIX_LOAD_PROCID(&src, "cust-nsp", 4);
    report("check_range: CUSTOM rejects unlisted rank",
           !pmix_notify_check_range(&rng, &src));
    PMIX_LOAD_PROCID(&src, "wild-nsp", 123);
    report("check_range: CUSTOM wildcard rank matches nspace",
           pmix_notify_check_range(&rng, &src));
}

/* ------------------------------------------------------------------ */
/* Chain invocation order                                              */
/* ------------------------------------------------------------------ */

static void test_chain_order(void)
{
    pmix_status_t code = EVUT_CODE_ORDER;
    pmix_status_t mcodes[2] = {EVUT_CODE_ORDER, EVUT_CODE_ORDER2};
    pmix_info_t info[3];
    pmix_proc_t nomatch, affected;
    size_t idx_s1, idx_s2, idx_s3, idx_m1, idx_d1, idx_d2, idx_first, idx_last;
    size_t handlers[8];
    size_t n;
    int ordered;
    pmix_status_t rc;

    norder = 0;
    last_nresults = -1;

    /* single-code handler named S1 */
    PMIX_INFO_LOAD(&info[0], PMIX_EVENT_HDLR_NAME, "S1", PMIX_STRING);
    idx_s1 = reghdlr(&code, 1, info, 1, chain_hdlr);
    PMIX_INFO_DESTRUCT(&info[0]);

    /* S3 goes before S1 */
    PMIX_INFO_LOAD(&info[0], PMIX_EVENT_HDLR_NAME, "S3", PMIX_STRING);
    PMIX_INFO_LOAD(&info[1], PMIX_EVENT_HDLR_BEFORE, "S1", PMIX_STRING);
    idx_s3 = reghdlr(&code, 1, info, 2, chain_hdlr);
    PMIX_INFO_DESTRUCT(&info[0]);
    PMIX_INFO_DESTRUCT(&info[1]);

    /* S2 goes after S1 and deliberately carries no name of its
     * own - registering this way must not crash and must position
     * relative to S1, not to the (absent) new name */
    PMIX_INFO_LOAD(&info[0], PMIX_EVENT_HDLR_AFTER, "S1", PMIX_STRING);
    idx_s2 = reghdlr(&code, 1, info, 1, chain_hdlr);
    PMIX_INFO_DESTRUCT(&info[0]);

    /* multi-code handler */
    PMIX_INFO_LOAD(&info[0], PMIX_EVENT_HDLR_NAME, "M1", PMIX_STRING);
    idx_m1 = reghdlr(mcodes, 2, info, 1, chain_hdlr);
    PMIX_INFO_DESTRUCT(&info[0]);

    /* two default handlers - D1 is registered second so it sits at
     * the head of the default list, and its affected-proc interest
     * will not match the event, so the chain must skip it and still
     * reach D2 */
    PMIX_INFO_LOAD(&info[0], PMIX_EVENT_HDLR_NAME, "D2", PMIX_STRING);
    idx_d2 = reghdlr(NULL, 0, info, 1, chain_hdlr);
    PMIX_INFO_DESTRUCT(&info[0]);

    PMIX_LOAD_PROCID(&nomatch, "no-such-nsp", 99);
    PMIX_INFO_LOAD(&info[0], PMIX_EVENT_HDLR_NAME, "D1", PMIX_STRING);
    PMIX_INFO_LOAD(&info[1], PMIX_EVENT_AFFECTED_PROC, &nomatch, PMIX_PROC);
    idx_d1 = reghdlr(NULL, 0, info, 2, chain_hdlr);
    PMIX_INFO_DESTRUCT(&info[0]);
    PMIX_INFO_DESTRUCT(&info[1]);

    /* a codeless first-overall handler - the chain must still visit
     * the single and multi code lists after it completes */
    PMIX_INFO_LOAD(&info[0], PMIX_EVENT_HDLR_NAME, "FIRST", PMIX_STRING);
    PMIX_INFO_LOAD(&info[1], PMIX_EVENT_HDLR_FIRST, NULL, PMIX_BOOL);
    idx_first = reghdlr(NULL, 0, info, 2, chain_hdlr);
    PMIX_INFO_DESTRUCT(&info[0]);
    PMIX_INFO_DESTRUCT(&info[1]);

    /* a codeless last-overall handler */
    PMIX_INFO_LOAD(&info[0], PMIX_EVENT_HDLR_NAME, "LAST", PMIX_STRING);
    PMIX_INFO_LOAD(&info[1], PMIX_EVENT_HDLR_LAST, NULL, PMIX_BOOL);
    idx_last = reghdlr(NULL, 0, info, 2, last_hdlr);
    PMIX_INFO_DESTRUCT(&info[0]);
    PMIX_INFO_DESTRUCT(&info[1]);

    report("chain order: all registrations succeeded",
           SIZE_MAX != idx_s1 && SIZE_MAX != idx_s2 && SIZE_MAX != idx_s3
           && SIZE_MAX != idx_m1 && SIZE_MAX != idx_d1 && SIZE_MAX != idx_d2
           && SIZE_MAX != idx_first && SIZE_MAX != idx_last);

    /* generate the event, marking an affected proc that does not
     * match D1's interest */
    PMIX_LOAD_PROCID(&affected, "affected-nsp", 1);
    PMIX_INFO_LOAD(&info[0], PMIX_EVENT_AFFECTED_PROC, &affected, PMIX_PROC);
    rc = notify_nocache(code, info, 1);
    PMIX_INFO_DESTRUCT(&info[0]);
    report("chain order: notify succeeded", PMIX_SUCCESS == rc);

    wait_for_count(&norder, 7);
    grace();

    ordered = (7 == norder)
              && order[0] == idx_first
              && order[1] == idx_s3
              && order[2] == idx_s1
              && order[3] == idx_s2
              && order[4] == idx_m1
              && order[5] == idx_d2
              && order[6] == idx_last;
    report("chain order: first -> singles -> multi -> default -> last", ordered);
    if (!ordered) {
        fprintf(stdout, "    expected [%zu %zu %zu %zu %zu %zu %zu] got %d entries [",
                idx_first, idx_s3, idx_s1, idx_s2, idx_m1, idx_d2, idx_last,
                norder);
        for (n = 0; n < (size_t) norder && n < EVUT_MAX_ORDER; n++) {
            fprintf(stdout, " %zu", order[n]);
        }
        fprintf(stdout, " ]\n");
    }
    report("chain order: last handler saw one result per prior handler",
           6 == last_nresults);

    /* clean up every handler we registered */
    handlers[0] = idx_s1;
    handlers[1] = idx_s2;
    handlers[2] = idx_s3;
    handlers[3] = idx_m1;
    handlers[4] = idx_d1;
    handlers[5] = idx_d2;
    handlers[6] = idx_first;
    handlers[7] = idx_last;
    for (n = 0; n < 8; n++) {
        if (SIZE_MAX != handlers[n]) {
            PMIx_Deregister_event_handler(handlers[n], NULL, NULL);
        }
    }
}

/* ------------------------------------------------------------------ */
/* PMIX_EVENT_ACTION_COMPLETE terminates the chain                     */
/* ------------------------------------------------------------------ */

static void test_action_complete(void)
{
    pmix_status_t code = EVUT_CODE_COMPLETE;
    size_t idx_a, idx_b;

    counted = 0;
    completed = 0;

    /* register the continuing handler first; the terminating handler
     * is registered second and so sits at the head of the list and
     * runs first */
    idx_a = reghdlr(&code, 1, NULL, 0, count_hdlr);
    idx_b = reghdlr(&code, 1, NULL, 0, complete_hdlr);

    notify_nocache(code, NULL, 0);
    wait_for_count(&completed, 1);
    grace();

    report("action-complete: terminating handler ran",
           1 == completed);
    report("action-complete: chain stopped before later handler",
           0 == counted);

    if (SIZE_MAX != idx_a) {
        PMIx_Deregister_event_handler(idx_a, NULL, NULL);
    }
    if (SIZE_MAX != idx_b) {
        PMIx_Deregister_event_handler(idx_b, NULL, NULL);
    }
}

/* ------------------------------------------------------------------ */
/* Oneshot handlers fire exactly once                                  */
/* ------------------------------------------------------------------ */

static void test_oneshot(void)
{
    pmix_status_t code = EVUT_CODE_ONESHOT;
    pmix_info_t info[1];
    size_t idx;

    completed = 0;

    PMIX_INFO_LOAD(&info[0], PMIX_EVENT_ONESHOT, NULL, PMIX_BOOL);
    idx = reghdlr(&code, 1, info, 1, complete_hdlr);
    PMIX_INFO_DESTRUCT(&info[0]);
    report("oneshot: registration succeeded", SIZE_MAX != idx);

    notify_nocache(code, NULL, 0);
    wait_for_count(&completed, 1);
    report("oneshot: handler fired on first event", 1 == completed);

    notify_nocache(code, NULL, 0);
    grace();
    report("oneshot: handler not fired again", 1 == completed);
}

/* ------------------------------------------------------------------ */
/* Deregistered handlers are not invoked                               */
/* ------------------------------------------------------------------ */

static void test_dereg(void)
{
    pmix_status_t code = EVUT_CODE_DEREG;
    size_t idx;
    pmix_status_t rc;

    counted = 0;

    idx = reghdlr(&code, 1, NULL, 0, count_hdlr);
    report("dereg: registration succeeded", SIZE_MAX != idx);

    rc = PMIx_Deregister_event_handler(idx, NULL, NULL);
    report("dereg: deregistration succeeded", PMIX_SUCCESS == rc);

    notify_nocache(code, NULL, 0);
    grace();
    report("dereg: handler not invoked after deregistration", 0 == counted);
}

/* ------------------------------------------------------------------ */
/* PROC_LOCAL registrations fire for locally generated events          */
/* ------------------------------------------------------------------ */

static void test_proc_local_registration(void)
{
    pmix_status_t code = EVUT_CODE_PROCLOCAL;
    pmix_data_range_t range = PMIX_RANGE_PROC_LOCAL;
    pmix_info_t info[1];
    size_t idx;

    counted = 0;

    PMIX_INFO_LOAD(&info[0], PMIX_RANGE, &range, PMIX_DATA_RANGE);
    idx = reghdlr(&code, 1, info, 1, count_hdlr);
    PMIX_INFO_DESTRUCT(&info[0]);
    report("proc-local: registration succeeded", SIZE_MAX != idx);

    notify_nocache(code, NULL, 0);
    wait_for_count(&counted, 1);
    report("proc-local: handler fired for our own event", 1 == counted);

    if (SIZE_MAX != idx) {
        PMIx_Deregister_event_handler(idx, NULL, NULL);
    }
}

/* ------------------------------------------------------------------ */
/* Cached events replay to late registrants with source intact         */
/* ------------------------------------------------------------------ */

static void test_cached_replay(void)
{
    pmix_status_t code = EVUT_CODE_CACHED;
    pmix_proc_t src;
    size_t idx;
    pmix_status_t rc;

    norder = 0;
    memset(&last_source, 0, sizeof(last_source));

    /* notify before anyone has registered - the event must be cached */
    PMIX_LOAD_PROCID(&src, "cached-src", 7);
    rc = PMIx_Notify_event(code, &src, PMIX_RANGE_UNDEF, NULL, 0, NULL, NULL);
    report("cached replay: notify succeeded", PMIX_SUCCESS == rc);

    /* now register - the cached event must be replayed to us */
    idx = reghdlr(&code, 1, NULL, 0, chain_hdlr);
    report("cached replay: registration succeeded", SIZE_MAX != idx);

    wait_for_count(&norder, 1);
    report("cached replay: handler received the cached event", 1 == norder);
    report("cached replay: original source preserved",
           PMIX_CHECK_PROCID(&last_source, &src)
           && 0 == strcmp(last_source.nspace, "cached-src"));

    if (SIZE_MAX != idx) {
        PMIx_Deregister_event_handler(idx, NULL, NULL);
    }
}

/* ------------------------------------------------------------------ */
/* PMIX_RANGE_RM notifications complete the caller                     */
/* ------------------------------------------------------------------ */

static volatile int rm_done = 0;

static void rm_cbfunc(pmix_status_t status, void *cbdata)
{
    PMIX_HIDE_UNUSED_PARAMS(status, cbdata);
    ++rm_done;
}

static void test_range_rm_completion(void)
{
    pmix_status_t rc;

    rm_done = 0;
    rc = PMIx_Notify_event(EVUT_CODE_RM, &pmix_globals.myid, PMIX_RANGE_RM,
                           NULL, 0, rm_cbfunc, NULL);
    report("range-rm: notify accepted",
           PMIX_SUCCESS == rc || PMIX_OPERATION_SUCCEEDED == rc);
    if (PMIX_OPERATION_SUCCEEDED == rc) {
        /* no callback will be coming */
        return;
    }
    wait_for_count(&rm_done, 1);
    report("range-rm: completion callback fired", 1 == rm_done);
}

/* ------------------------------------------------------------------ */
/* Host register_events upcall acks exactly once                       */
/* ------------------------------------------------------------------ */

static volatile int host_calls = 0;
static volatile int reg_acks = 0;
static volatile pmix_status_t reg_ack_status = PMIX_ERROR;
static volatile size_t reg_ack_ref = SIZE_MAX;

static void regev_cbfunc(pmix_status_t status, size_t refid, void *cbdata)
{
    PMIX_HIDE_UNUSED_PARAMS(cbdata);
    reg_ack_status = status;
    reg_ack_ref = refid;
    ++reg_acks;
}

static pmix_status_t stub_regevs_opsucceeded(pmix_status_t codes[], size_t ncodes,
                                             const pmix_info_t info[], size_t ninfo,
                                             pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    PMIX_HIDE_UNUSED_PARAMS(codes, ncodes, info, ninfo, cbfunc, cbdata);
    ++host_calls;
    return PMIX_OPERATION_SUCCEEDED;
}

static pmix_status_t stub_regevs_async(pmix_status_t codes[], size_t ncodes,
                                       const pmix_info_t info[], size_t ninfo,
                                       pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    PMIX_HIDE_UNUSED_PARAMS(codes, ncodes, info, ninfo);
    ++host_calls;
    if (NULL != cbfunc) {
        cbfunc(PMIX_SUCCESS, cbdata);
    }
    return PMIX_SUCCESS;
}

static void test_host_regevents(void)
{
    pmix_status_t code;
    pmix_status_t rc;

    /* system-event codes force the server to up-call the host */

    /* host completes the request atomically */
    host_calls = 0;
    reg_acks = 0;
    pmix_host_server.register_events = stub_regevs_opsucceeded;
    code = PMIX_EVENT_NODE_DOWN;
    rc = PMIx_Register_event_handler(&code, 1, NULL, 0, count_hdlr,
                                     regev_cbfunc, NULL);
    report("host regevents: nonblocking registration accepted",
           PMIX_SUCCESS == rc);
    wait_for_count(&reg_acks, 1);
    grace();
    report("host regevents: atomic host completion acks exactly once",
           1 == reg_acks && 1 == host_calls && PMIX_SUCCESS == reg_ack_status);
    if (1 <= reg_acks && PMIX_SUCCESS == reg_ack_status) {
        PMIx_Deregister_event_handler(reg_ack_ref, NULL, NULL);
    }

    /* host defers and then invokes the provided callback */
    host_calls = 0;
    reg_acks = 0;
    reg_ack_status = PMIX_ERROR;
    pmix_host_server.register_events = stub_regevs_async;
    code = PMIX_EVENT_NODE_OFFLINE;
    rc = PMIx_Register_event_handler(&code, 1, NULL, 0, count_hdlr,
                                     regev_cbfunc, NULL);
    report("host regevents: deferred registration accepted",
           PMIX_SUCCESS == rc);
    wait_for_count(&reg_acks, 1);
    grace();
    report("host regevents: deferred host completion acks exactly once",
           1 == reg_acks && 1 == host_calls && PMIX_SUCCESS == reg_ack_status);
    if (1 <= reg_acks && PMIX_SUCCESS == reg_ack_status) {
        PMIx_Deregister_event_handler(reg_ack_ref, NULL, NULL);
    }

    pmix_host_server.register_events = NULL;
}

/* ------------------------------------------------------------------ */
/* Enviro event activation/deactivation handshake with the host        */
/* ------------------------------------------------------------------ */

#define EVUT_MAX_HSCODES 8

static volatile int hs_regs = 0;
static volatile int hs_deregs = 0;
static size_t hs_last_ncodes = 0;
static pmix_status_t hs_last_codes[EVUT_MAX_HSCODES];

static void hs_record(pmix_status_t codes[], size_t ncodes)
{
    size_t n;

    hs_last_ncodes = ncodes;
    for (n = 0; n < ncodes && n < EVUT_MAX_HSCODES; n++) {
        hs_last_codes[n] = codes[n];
    }
}

static pmix_status_t hs_regevs(pmix_status_t codes[], size_t ncodes,
                               const pmix_info_t info[], size_t ninfo,
                               pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    PMIX_HIDE_UNUSED_PARAMS(info, ninfo, cbfunc, cbdata);
    ++hs_regs;
    hs_record(codes, ncodes);
    return PMIX_OPERATION_SUCCEEDED;
}

static pmix_status_t hs_deregevs(pmix_status_t codes[], size_t ncodes,
                                 pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    PMIX_HIDE_UNUSED_PARAMS(cbfunc, cbdata);
    ++hs_deregs;
    hs_record(codes, ncodes);
    return PMIX_OPERATION_SUCCEEDED;
}

/* Drive the server-side handlers for a client's REGEVENTS_CMD /
 * DEREGEVENTS_CMD on the progress thread, using this process's own peer
 * to stand in for a local client. Those handlers touch
 * pmix_server_globals directly, so they must not be called from here. */
typedef struct {
    pmix_event_t ev;
    pmix_lock_t lock;
    pmix_status_t *codes;
    size_t ncodes;
    pmix_status_t status;
} evut_clreq_t;

static void do_client_reg(int sd, short args, void *cbdata)
{
    evut_clreq_t *r = (evut_clreq_t *) cbdata;
    pmix_buffer_t buf;
    size_t ninfo = 0;
    pmix_status_t rc;

    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    PMIX_CONSTRUCT(&buf, pmix_buffer_t);
    PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, &buf, &r->ncodes, 1, PMIX_SIZE);
    /* zero codes is a default-handler registration - the server unpacks
     * the code array only when the count is non-zero, so do not pack one */
    if (PMIX_SUCCESS == rc && 0 < r->ncodes) {
        PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, &buf, r->codes, r->ncodes, PMIX_STATUS);
    }
    if (PMIX_SUCCESS == rc) {
        PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, &buf, &ninfo, 1, PMIX_SIZE);
    }
    if (PMIX_SUCCESS == rc) {
        rc = pmix_server_register_events(pmix_globals.mypeer, &buf, NULL, NULL);
    }
    PMIX_DESTRUCT(&buf);
    r->status = rc;
    PMIX_WAKEUP_THREAD(&r->lock);
}

static void do_client_dereg(int sd, short args, void *cbdata)
{
    evut_clreq_t *r = (evut_clreq_t *) cbdata;
    pmix_buffer_t buf;
    pmix_status_t rc = PMIX_SUCCESS;
    size_t n;

    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    PMIX_CONSTRUCT(&buf, pmix_buffer_t);
    for (n = 0; n < r->ncodes && PMIX_SUCCESS == rc; n++) {
        PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, &buf, &r->codes[n], 1, PMIX_STATUS);
    }
    if (PMIX_SUCCESS == rc) {
        pmix_server_deregister_events(pmix_globals.mypeer, &buf);
    }
    PMIX_DESTRUCT(&buf);
    r->status = rc;
    PMIX_WAKEUP_THREAD(&r->lock);
}

static pmix_status_t client_evreq(pmix_status_t *codes, size_t ncodes, bool dereg)
{
    evut_clreq_t req;
    pmix_status_t rc;

    memset(&req, 0, sizeof(req));
    PMIX_CONSTRUCT_LOCK(&req.lock);
    req.codes = codes;
    req.ncodes = ncodes;
    if (dereg) {
        PMIX_THREADSHIFT(&req, do_client_dereg);
    } else {
        PMIX_THREADSHIFT(&req, do_client_reg);
    }
    PMIX_WAIT_THREAD(&req.lock);
    rc = req.status;
    PMIX_DESTRUCT_LOCK(&req.lock);
    return rc;
}

/* Count how many peers are recorded against the server's entry for a
 * code, and say whether the entry exists at all. */
static size_t code_reg_peers(pmix_status_t code, bool *exists)
{
    pmix_regevents_info_t *reginfo;
    pmix_peer_events_info_t *pr;
    size_t n = 0;

    *exists = false;
    PMIX_LIST_FOREACH (reginfo, &pmix_server_globals.events, pmix_regevents_info_t) {
        if (code != reginfo->code) {
            continue;
        }
        *exists = true;
        PMIX_LIST_FOREACH (pr, &reginfo->peers, pmix_peer_events_info_t) {
            ++n;
        }
        break;
    }
    return n;
}

/* A client registering a DEFAULT handler - no codes at all - has to be
 * recorded in the server's dispatch list, including when it is the first
 * such registration the server has seen.
 *
 * This is the case that was broken: the handler walked the list looking
 * for an existing PMIX_MAX_ERR_CONSTANT entry and, finding none, returned
 * PMIX_OPERATION_SUCCEEDED having stored nothing. The peer was told its
 * registration succeeded and then received no default-routed event ever
 * again, which is precisely the fate of the first client or tool to
 * attach to a server - it necessarily finds the list empty.
 *
 * Run this before anything else registers, so the entry really is absent
 * to begin with. */
static void test_default_registration(void)
{
    pmix_status_t wildcard = PMIX_MAX_ERR_CONSTANT;
    size_t before, after;
    bool existed = false, exists = false;
    pmix_status_t rc;

    fprintf(stdout, "default-handler registration:\n");

    before = code_reg_peers(PMIX_MAX_ERR_CONSTANT, &existed);
    report("no default entry exists yet", !existed && 0 == before);

    rc = client_evreq(NULL, 0, false);
    report("registering with no codes succeeds",
           PMIX_SUCCESS == rc || PMIX_OPERATION_SUCCEEDED == rc);

    after = code_reg_peers(PMIX_MAX_ERR_CONSTANT, &exists);
    report("the default entry was created", exists);
    report("the registrant is on it", 1 == after);

    /* a second default registration joins the same entry rather than
     * making another one */
    rc = client_evreq(NULL, 0, false);
    after = code_reg_peers(PMIX_MAX_ERR_CONSTANT, &exists);
    report("a second default registration joins the same entry",
           exists && 2 == after);

    /* And deregistration finds what registration created - all of it. A
     * client drops a default handler by naming PMIX_MAX_ERR_CONSTANT
     * explicitly, not by sending an empty code list (see
     * pmix_deregister_event_hdlr), and it sends that deregistration only
     * once its _last_ handler for the code is gone. So every entry this
     * peer holds for the code is stale by then. Removing just the first
     * left one behind for the life of the connection: the server kept
     * forwarding the code to a peer with no handler for it, and the
     * entry could never be pruned, so the host was never told to stop
     * either. */
    rc = client_evreq(&wildcard, 1, true);
    after = code_reg_peers(PMIX_MAX_ERR_CONSTANT, &exists);
    report("deregistering clears every registration this peer holds",
           !exists && 0 == after);

    /* and a deregistration with nothing left to find is harmless */
    rc = client_evreq(&wildcard, 1, true);
    after = code_reg_peers(PMIX_MAX_ERR_CONSTANT, &exists);
    report("a repeated deregistration is harmless", !exists && 0 == after);
}

/* The duplicate entries above are not an artifact of the default-handler
 * case: they are how an ordinary coded registration behaves too, because
 * the client library packs a handler's whole code list whenever any code
 * in that list is new to the server. So a client that registers one
 * handler for {A} and a second for {A, B} sends both lists, and the
 * server records two registrations for A - deliberately, since each
 * carries its own PMIX_EVENT_AFFECTED_PROC filter. It then sends exactly
 * one deregistration for A, when its last handler for A goes away. */
static void test_duplicate_code_registrations(void)
{
    pmix_status_t one[1] = {EVUT_CODE_ORDER};
    pmix_status_t two[2] = {EVUT_CODE_ORDER, EVUT_CODE_ORDER2};
    bool exists = false;
    size_t n;

    fprintf(stdout, "duplicate code registrations:\n");

    client_evreq(one, 1, false);
    client_evreq(two, 2, false);
    n = code_reg_peers(EVUT_CODE_ORDER, &exists);
    report("a code named by two handlers is recorded twice",
           exists && 2 == n);
    n = code_reg_peers(EVUT_CODE_ORDER2, &exists);
    report("the code named by only one is recorded once", exists && 1 == n);

    /* the single deregistration the client sends has to clear both */
    client_evreq(one, 1, true);
    n = code_reg_peers(EVUT_CODE_ORDER, &exists);
    report("one deregistration clears both entries", !exists && 0 == n);

    client_evreq(&two[1], 1, true);
    n = code_reg_peers(EVUT_CODE_ORDER2, &exists);
    report("the untouched code deregisters normally", !exists && 0 == n);
}

static void test_enviro_handshake(void)
{
    pmix_status_t s1 = PMIX_EVENT_SYS_BASE;
    pmix_status_t s2 = PMIX_EVENT_SYS_OTHER;
    pmix_status_t mixed[2] = {EVUT_CODE_ORDER, PMIX_EVENT_SYS_OTHER};
    pmix_data_range_t local = PMIX_RANGE_PROC_LOCAL;
    pmix_info_t info;
    size_t a, b, c;
    pmix_status_t rc;

    hs_regs = 0;
    hs_deregs = 0;
    pmix_host_server.register_events = hs_regevs;
    pmix_host_server.deregister_events = hs_deregevs;

    /* the first registration for a system code activates it */
    a = reghdlr(&s1, 1, NULL, 0, count_hdlr);
    report("enviro: first registration accepted", SIZE_MAX != a);
    report("enviro: first registration activates the code",
           1 == hs_regs && 1 == hs_last_ncodes && s1 == hs_last_codes[0]);

    /* a second registration for the same code must not repeat it */
    b = reghdlr(&s1, 1, NULL, 0, count_hdlr);
    report("enviro: second registration accepted", SIZE_MAX != b);
    report("enviro: second registration does not repeat the host call",
           1 == hs_regs);

    /* a code the host is not yet forwarding is activated even when it
     * arrives alongside one that it already is - and the non-system
     * code it is bundled with is never handed to the host */
    c = reghdlr(mixed, 2, NULL, 0, count_hdlr);
    report("enviro: mixed registration accepted", SIZE_MAX != c);
    report("enviro: only the unactivated system code is sent to the host",
           2 == hs_regs && 1 == hs_last_ncodes && s2 == hs_last_codes[0]);

    /* dropping one of two registrants leaves the code active */
    if (SIZE_MAX != a) {
        PMIx_Deregister_event_handler(a, NULL, NULL);
    }
    report("enviro: code stays active while a registrant remains",
           0 == hs_deregs);

    /* dropping the last registrant for a code deactivates just it */
    if (SIZE_MAX != b) {
        PMIx_Deregister_event_handler(b, NULL, NULL);
    }
    report("enviro: last registrant deactivates the code",
           1 == hs_deregs && 1 == hs_last_ncodes && s1 == hs_last_codes[0]);

    /* registering again after deactivation re-activates the code */
    a = reghdlr(&s1, 1, NULL, 0, count_hdlr);
    report("enviro: re-registration accepted", SIZE_MAX != a);
    report("enviro: re-registration re-activates the code",
           3 == hs_regs && 1 == hs_last_ncodes && s1 == hs_last_codes[0]);

    /* a local client registering a code the server itself already holds
     * must not repeat the host call, and must not release the code when
     * it goes away while the server is still registered */
    rc = client_evreq(&s1, 1, false);
    report("enviro: client registration accepted",
           PMIX_SUCCESS == rc || PMIX_OPERATION_SUCCEEDED == rc);
    report("enviro: client does not repeat the host call for a held code",
           3 == hs_regs);
    rc = client_evreq(&s1, 1, true);
    report("enviro: client deregistration processed", PMIX_SUCCESS == rc);
    report("enviro: client departure leaves the server's code active",
           1 == hs_deregs);

    /* conversely, the server dropping its own registration must not
     * release a code a local client still wants */
    rc = client_evreq(&s1, 1, false);
    report("enviro: second client registration accepted",
           PMIX_SUCCESS == rc || PMIX_OPERATION_SUCCEEDED == rc);
    if (SIZE_MAX != a) {
        PMIx_Deregister_event_handler(a, NULL, NULL);
    }
    report("enviro: code stays active while a client wants it",
           1 == hs_deregs);
    rc = client_evreq(&s1, 1, true);
    report("enviro: last client departure deactivates the code",
           2 == hs_deregs && 1 == hs_last_ncodes && s1 == hs_last_codes[0]);

    /* a PMIX_RANGE_PROC_LOCAL registration never leaves the process, so
     * it neither activates a code nor releases anyone else's interest
     * in one when it goes away */
    PMIX_INFO_LOAD(&info, PMIX_RANGE, &local, PMIX_DATA_RANGE);
    a = reghdlr(&s1, 1, &info, 1, count_hdlr);
    PMIX_INFO_DESTRUCT(&info);
    report("enviro: proc-local registration accepted", SIZE_MAX != a);
    report("enviro: proc-local registration does not reach the host",
           3 == hs_regs);
    b = reghdlr(&s1, 1, NULL, 0, count_hdlr);
    report("enviro: registration alongside proc-local accepted", SIZE_MAX != b);
    report("enviro: registration alongside proc-local activates the code",
           4 == hs_regs && 1 == hs_last_ncodes && s1 == hs_last_codes[0]);
    if (SIZE_MAX != a) {
        PMIx_Deregister_event_handler(a, NULL, NULL);
    }
    report("enviro: proc-local departure does not release the code",
           2 == hs_deregs);
    if (SIZE_MAX != b) {
        PMIx_Deregister_event_handler(b, NULL, NULL);
    }
    report("enviro: code released once the real registrant departs",
           3 == hs_deregs && 1 == hs_last_ncodes && s1 == hs_last_codes[0]);

    /* and the mixed registration still holds the remaining code */
    if (SIZE_MAX != c) {
        PMIx_Deregister_event_handler(c, NULL, NULL);
    }
    report("enviro: final deregistration releases the remaining code",
           4 == hs_deregs && 1 == hs_last_ncodes && s2 == hs_last_codes[0]);

    pmix_host_server.register_events = NULL;
    pmix_host_server.deregister_events = NULL;
}

/* ------------------------------------------------------------------ */
/* First-overall handler honors its affected-proc interests            */
/* ------------------------------------------------------------------ */

static void test_first_affected(void)
{
    pmix_status_t codes[2] = {EVUT_CODE_FIRSTAFF, EVUT_CODE_FIRSTAFF2};
    pmix_info_t info[2];
    pmix_proc_t interest, affected;
    size_t idx;

    counted = 0;

    /* a multi-code first-overall handler interested only in events
     * affecting a specific proc */
    PMIX_LOAD_PROCID(&interest, "first-aff", 1);
    PMIX_INFO_LOAD(&info[0], PMIX_EVENT_HDLR_FIRST, NULL, PMIX_BOOL);
    PMIX_INFO_LOAD(&info[1], PMIX_EVENT_AFFECTED_PROC, &interest, PMIX_PROC);
    idx = reghdlr(codes, 2, info, 2, count_hdlr);
    PMIX_INFO_DESTRUCT(&info[0]);
    PMIX_INFO_DESTRUCT(&info[1]);
    report("first affected: registration succeeded", SIZE_MAX != idx);

    /* an event affecting some other proc must not reach the handler */
    PMIX_LOAD_PROCID(&affected, "other-proc", 2);
    PMIX_INFO_LOAD(&info[0], PMIX_EVENT_AFFECTED_PROC, &affected, PMIX_PROC);
    notify_nocache(codes[0], info, 1);
    PMIX_INFO_DESTRUCT(&info[0]);
    grace();
    report("first affected: filtered event not delivered", 0 == counted);

    /* an event affecting the proc of interest must reach it */
    PMIX_INFO_LOAD(&info[0], PMIX_EVENT_AFFECTED_PROC, &interest, PMIX_PROC);
    notify_nocache(codes[0], info, 1);
    PMIX_INFO_DESTRUCT(&info[0]);
    wait_for_count(&counted, 1);
    report("first affected: matching event delivered", 1 == counted);

    if (SIZE_MAX != idx) {
        PMIx_Deregister_event_handler(idx, NULL, NULL);
    }
}

/* ------------------------------------------------------------------ */
/* Library-internal observers (openpmix#4059)                          */
/* ------------------------------------------------------------------ */

static volatile int observed = 0;
static volatile int obs_order = -1;
static volatile int obs_released = 0;
static size_t obs_selfid = SIZE_MAX;
static volatile int obs_self_ran = 0;

/* record that we saw the event, and where in the sequence */
static void count_obs(pmix_status_t status, const pmix_proc_t *source,
                      const pmix_info_t info[], size_t ninfo,
                      const pmix_proc_t *affected, size_t naffected,
                      void *cbobject)
{
    (void) status;
    (void) source;
    (void) info;
    (void) ninfo;
    (void) affected;
    (void) naffected;
    (void) cbobject;

    obs_order = norder;
    ++observed;
}

/* an observer that removes itself the first time it fires */
static void self_dereg_obs(pmix_status_t status, const pmix_proc_t *source,
                           const pmix_info_t info[], size_t ninfo,
                           const pmix_proc_t *affected, size_t naffected,
                           void *cbobject)
{
    (void) status;
    (void) source;
    (void) info;
    (void) ninfo;
    (void) affected;
    (void) naffected;
    (void) cbobject;

    ++obs_self_ran;
    if (SIZE_MAX != obs_selfid) {
        pmix_event_deregister_observer(obs_selfid, NULL, NULL);
        obs_selfid = SIZE_MAX;
    }
}

/* release callback for an observer object owned by the registry */
static void obs_relfn(void *cbdata)
{
    free(cbdata);
    ++obs_released;
}

static void obsregcb(pmix_status_t status, size_t refid, void *cbdata)
{
    size_t *idp = (size_t *) cbdata;

    *idp = (PMIX_SUCCESS == status) ? refid : SIZE_MAX;
}

/* register an observer and wait for the registration to land - the
 * entry point is non-blocking by design */
static size_t regobs(const char *name, pmix_status_t *codes, size_t ncodes,
                     pmix_event_observer_fn_t fn, void *cbobject,
                     pmix_release_cbfunc_t relfn)
{
    volatile size_t id = SIZE_MAX - 1;
    pmix_status_t rc;
    int i;

    rc = pmix_event_register_observer(name, codes, ncodes, fn, cbobject, relfn,
                                      obsregcb, (void *) &id);
    if (PMIX_SUCCESS != rc) {
        return SIZE_MAX;
    }
    for (i = 0; i < 500 && (SIZE_MAX - 1) == id; i++) {
        usleep(10000);
    }
    return id;
}

/* count the entries in the actives list for a given code */
static size_t actives_for(pmix_status_t code)
{
    pmix_active_code_t *active;
    size_t n = 0;

    PMIX_LIST_FOREACH (active, &pmix_globals.events.actives, pmix_active_code_t) {
        if (active->code == code) {
            n += active->nregs;
        }
    }
    return n;
}

static void test_observer(void)
{
    pmix_status_t code = EVUT_CODE_OBSERVER;
    pmix_status_t code2 = EVUT_CODE_OBSERVER2;
    size_t obsid, hdlrid, hdlrid2;
    size_t nactive;
    char *owned;
    pmix_status_t rc;

    fprintf(stdout, "internal observers:\n");

    /* an observer must name at least one code, and must have a function */
    rc = pmix_event_register_observer("bad", NULL, 0, count_obs, NULL, NULL, NULL, NULL);
    report("observer: registration with no codes rejected", PMIX_ERR_BAD_PARAM == rc);
    rc = pmix_event_register_observer("bad", &code, 1, NULL, NULL, NULL, NULL, NULL);
    report("observer: registration with no callback rejected", PMIX_ERR_BAD_PARAM == rc);

    /* an observer records the same per-code interest a handler does -
     * without it, the server would never forward the code at all */
    nactive = actives_for(code);
    obsid = regobs("evut-observer", &code, 1, count_obs, NULL, NULL);
    report("observer: registration succeeded", SIZE_MAX != obsid);
    report("observer: registration recorded an active code",
           nactive + 1 == actives_for(code));

    /* THE regression: an application handler that ends the chain the
     * normal way must not be able to suppress the library's observer.
     *
     * Two application handlers, so the ordering assertion below has
     * something to measure: the terminating one is registered first and
     * so ends up behind the counting one (registration prepends), which
     * means the chain really does advance norder before it stops. */
    observed = 0;
    completed = 0;
    norder = 0;
    obs_order = -1;
    hdlrid = reghdlr(&code, 1, NULL, 0, complete_hdlr);
    hdlrid2 = reghdlr(&code, 1, NULL, 0, chain_hdlr);
    report("observer: application handlers registered",
           SIZE_MAX != hdlrid && SIZE_MAX != hdlrid2);

    notify_nocache(code, NULL, 0);
    wait_for_count(&observed, 1);
    wait_for_count(&completed, 1);
    grace();

    report("observer: application handler ran and ended the chain",
           1 == completed && 1 == norder);
    report("observer: observer saw the event anyway", 1 == observed);

    /* and it saw it first - observers run ahead of the entire chain, so
     * no handler had advanced norder by the time the observer ran */
    report("observer: observer ran before the chain", 0 == obs_order);

    if (SIZE_MAX != hdlrid) {
        PMIx_Deregister_event_handler(hdlrid, NULL, NULL);
    }
    if (SIZE_MAX != hdlrid2) {
        PMIx_Deregister_event_handler(hdlrid2, NULL, NULL);
    }

    /* deregistration removes it, and gives back the code interest */
    observed = 0;
    rc = pmix_event_deregister_observer(obsid, NULL, NULL);
    report("observer: deregistration accepted", PMIX_SUCCESS == rc);
    grace();
    report("observer: deregistration released the active code",
           nactive == actives_for(code));

    notify_nocache(code, NULL, 0);
    grace();
    report("observer: not invoked after deregistration", 0 == observed);

    /* an observer may tear itself down from inside its own callback */
    obs_self_ran = 0;
    obs_selfid = regobs("evut-self", &code2, 1, self_dereg_obs, NULL, NULL);
    report("observer: self-dereg registration succeeded", SIZE_MAX != obs_selfid);

    notify_nocache(code2, NULL, 0);
    wait_for_count(&obs_self_ran, 1);
    grace();
    report("observer: self-deregistering observer ran once", 1 == obs_self_ran);

    notify_nocache(code2, NULL, 0);
    grace();
    report("observer: self-deregistered observer did not run again",
           1 == obs_self_ran);

    /* the registry owns the observer's object when given a release fn,
     * so a subsystem does not need its own survivor list for finalize */
    obs_released = 0;
    owned = strdup("owned-by-the-registry");
    obsid = regobs("evut-owned", &code, 1, count_obs, owned, obs_relfn);
    report("observer: registration with an owned object succeeded",
           SIZE_MAX != obsid);
    pmix_event_deregister_observer(obsid, NULL, NULL);
    grace();
    report("observer: the registry released the observer's object",
           1 == obs_released);
}

/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* A blocking API called from the progress thread                      */
/* ------------------------------------------------------------------ */

/* Event handlers are dispatched from the progress thread, so a handler
 * is the natural place an application reaches a blocking PMIx API from
 * the one thread that cannot service it. PMIx_Get hands its work to
 * that thread and then PMIX_WAIT_THREADs for it, so from here the work
 * is queued behind the caller and the caller never returns.
 *
 * Both entry points are checked. PMIx_Get always blocks; PMIx_Get_nb
 * blocks only under PMIX_GET_REFRESH_CACHE, which round-trips to the
 * server and waits on this thread before the get is ever posted. */

static volatile int guard_fired = 0;
static volatile pmix_status_t guard_get_status = PMIX_SUCCESS;
static volatile pmix_status_t guard_refresh_status = PMIX_SUCCESS;

static void guard_valuecb(pmix_status_t status, pmix_value_t *kv, void *cbdata)
{
    PMIX_HIDE_UNUSED_PARAMS(status, kv, cbdata);
}

static void blocking_get_hdlr(size_t evhdlr_registration_id, pmix_status_t status,
                              const pmix_proc_t *source, pmix_info_t info[], size_t ninfo,
                              pmix_info_t *results, size_t nresults,
                              pmix_event_notification_cbfunc_fn_t cbfunc, void *cbdata)
{
    pmix_value_t *val = NULL;
    pmix_info_t refresh;
    PMIX_HIDE_UNUSED_PARAMS(evhdlr_registration_id, status, source, info, ninfo,
                            results, nresults);

    guard_get_status = PMIx_Get(&pmix_globals.myid, PMIX_JOB_SIZE, NULL, 0, &val);
    if (NULL != val) {
        PMIX_VALUE_RELEASE(val);
    }

    PMIX_INFO_LOAD(&refresh, PMIX_GET_REFRESH_CACHE, NULL, PMIX_BOOL);
    guard_refresh_status = PMIx_Get_nb(&pmix_globals.myid, PMIX_JOB_SIZE, &refresh, 1,
                                       guard_valuecb, NULL);
    PMIX_INFO_DESTRUCT(&refresh);

    guard_fired = 1;
    if (NULL != cbfunc) {
        cbfunc(PMIX_SUCCESS, NULL, 0, NULL, NULL, cbdata);
    }
}

static void alarm_died(int sig)
{
    PMIX_HIDE_UNUSED_PARAMS(sig);
    /* Without the guard this is where we end up: the handler is parked
     * in PMIX_WAIT_THREAD on the progress thread, waiting for an event
     * that same thread has to run. Report it rather than hanging the
     * whole suite - write(2) because we are in a signal handler. */
    static const char msg[] = "  FAIL: blocking PMIx_Get from the progress thread hung\n";
    ssize_t ignored = write(STDERR_FILENO, msg, sizeof(msg) - 1);
    (void) ignored;
    _exit(1);
}

static void test_blocking_from_progress_thread(void)
{
    pmix_status_t code = EVUT_CODE_BLOCKING;
    size_t ref;
    pmix_status_t rc;

    guard_fired = 0;
    guard_get_status = PMIX_SUCCESS;
    guard_refresh_status = PMIX_SUCCESS;

    ref = reghdlr(&code, 1, NULL, 0, blocking_get_hdlr);
    if (SIZE_MAX == ref) {
        report("progress-thread guard: handler registered", false);
        return;
    }

    /* a regression here is a permanent hang, not a wrong answer */
    signal(SIGALRM, alarm_died);
    alarm(30);

    rc = notify_nocache(code, NULL, 0);
    report("progress-thread guard: notification accepted", PMIX_SUCCESS == rc);
    wait_for_count(&guard_fired, 1);
    grace();

    alarm(0);
    signal(SIGALRM, SIG_DFL);

    report("progress-thread guard: handler ran", 1 == guard_fired);
    report("progress-thread guard: blocking PMIx_Get refuses rather than hangs",
           PMIX_ERR_WOULD_BLOCK == guard_get_status);
    report("progress-thread guard: PMIx_Get_nb refresh refuses rather than hangs",
           PMIX_ERR_WOULD_BLOCK == guard_refresh_status);

    PMIx_Deregister_event_handler(ref, NULL, NULL);
}

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

    fprintf(stdout, "\n=== Event notification unit tests ===\n\n");

    /* pure predicate checks */
    test_check_affected();
    test_check_range();

    /* must run before anything registers, so the default entry really is
     * absent when we ask for one */
    test_default_registration();
    test_duplicate_code_registrations();

    /* registration placement and chain progression */
    test_chain_order();
    test_action_complete();
    test_oneshot();
    test_dereg();
    test_proc_local_registration();

    /* caching and completion protocols */
    test_cached_replay();
    test_range_rm_completion();
    test_host_regevents();
    test_enviro_handshake();
    test_first_affected();

    /* a blocking API reached from the progress thread */
    test_blocking_from_progress_thread();

    /* library-internal observers */
    test_observer();

    fprintf(stdout, "\nResults: %d passed, %d failed\n\n", npass, nfail);

    PMIx_server_finalize();

    return (nfail > 0) ? 1 : 0;
}
