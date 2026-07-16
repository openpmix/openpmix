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
 *  first-overall handler with multiple codes honors its
 *      affected-proc interest list.
 */

#include "src/include/pmix_config.h"

#include "include/pmix.h"
#include "include/pmix_server.h"

#include "src/event/pmix_event.h"
#include "src/include/pmix_globals.h"
#include "src/server/pmix_server_ops.h"

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
    test_first_affected();

    fprintf(stdout, "\nResults: %d passed, %d failed\n\n", npass, nfail);

    PMIx_server_finalize();

    return (nfail > 0) ? 1 : 0;
}
