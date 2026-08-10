/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * White-box unit tests for two of the directive/service command handlers
 * in src/server/pmix_server_control.c: pmix_server_log() and
 * pmix_server_job_ctrl(). Both are driven exactly as the switchyard drives
 * them - a wire buffer packed by hand and a peer - so no DVM, no client,
 * and no second process is involved.
 *
 * What is pinned down here:
 *
 *   log: the counts that arrive off the wire
 *      Both counts are carried in a size_t and then consumed through the
 *      int32_t count that PMIX_BFROPS_UNPACK takes. Every sibling handler
 *      in that file survives a count that does not fit, because it only
 *      ever uses the value to size an allocation that is immediately
 *      unpacked into - and the unpack screens a NULL destination and
 *      returns PMIX_ERR_BAD_PARAM. This handler is the exception twice
 *      over: it *indexes* the directive array to append the log source
 *      before anything has been unpacked into it, and it hands
 *      (info, ninfo) straight to plog even when the unpack was skipped.
 *      So a directive count of 0x80000000 truncated to a negative int32_t
 *      and the source was written at that negative offset off a NULL
 *      array. The bad-directive-count case therefore *crashes* an
 *      unfixed library rather than failing - the same bargain
 *      test/unit/iof_output.c makes.
 *
 *   log: the source directive is still appended
 *      The screen above must not cost the handler its actual job. A
 *      well-formed request has to come out of it with the caller's
 *      directives intact plus PMIX_LOG_SOURCE appended, and with
 *      PMIX_LOG_TIMESTAMP appended as well when the sender supplied a
 *      non-zero timestamp.
 *
 *   job control: a repeated cleanup directory upgrades the registered entry
 *      PMIX_REGISTER_CLEANUP_DIR for a path already on the epilog is a
 *      duplicate, and the RFC precedence rule is that the more permissive
 *      of the two flag sets wins. The upgrade has to be applied to the
 *      entry sitting on the epilog, not to the request-local copy that is
 *      discarded moments later - the latter compiles, runs, and silently
 *      does nothing, so a second PMIx_Job_control asking for recursive
 *      cleanup of an already-registered directory never took effect.
 *      This case fails, rather than crashes, against an unfixed library.
 */

#include "src/include/pmix_config.h"

#include "include/pmix.h"
#include "include/pmix_server.h"

#include "src/class/pmix_list.h"
#include "src/include/pmix_globals.h"
#include "src/mca/bfrops/bfrops.h"
#include "src/runtime/pmix_rte.h"
#include "src/server/pmix_server_ops.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CTLUT_DIR "/tmp/pmix-server-control-ut-no-such-dir"

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

/* what the host's log2 entry point was handed */
static bool log_fired = false;
static size_t log_ndata = 0;
static size_t log_ndirs = 0;
static bool log_saw_source = false;
static bool log_saw_timestamp = false;

static pmix_status_t stub_log2(const pmix_proc_t *client,
                               const pmix_info_t data[], size_t ndata,
                               const pmix_info_t directives[], size_t ndirs,
                               pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    size_t n;
    (void) client;
    (void) data;

    log_fired = true;
    log_ndata = ndata;
    log_ndirs = ndirs;
    log_saw_source = false;
    log_saw_timestamp = false;
    for (n = 0; n < ndirs; n++) {
        if (PMIX_CHECK_KEY(&directives[n], PMIX_LOG_SOURCE)) {
            log_saw_source = true;
        } else if (PMIX_CHECK_KEY(&directives[n], PMIX_LOG_TIMESTAMP)) {
            log_saw_timestamp = true;
        }
    }
    /* complete synchronously - the arrays above belong to the caddy the
     * completion is about to release, so everything we want out of them
     * has already been copied */
    if (NULL != cbfunc) {
        cbfunc(PMIX_SUCCESS, cbdata);
    }
    return PMIX_SUCCESS;
}

/* the handler refuses to run at all without one of these, but none of the
 * cases below is supposed to reach it: they are all pure cleanup requests,
 * which the server answers itself */
static bool jobctrl_fired = false;
static pmix_status_t stub_job_control(const pmix_proc_t *requestor,
                                      const pmix_proc_t targets[], size_t ntargets,
                                      const pmix_info_t directives[], size_t ndirs,
                                      pmix_info_cbfunc_t cbfunc, void *cbdata)
{
    (void) requestor;
    (void) targets;
    (void) ntargets;
    (void) directives;
    (void) ndirs;
    (void) cbfunc;
    (void) cbdata;

    jobctrl_fired = true;
    return PMIX_ERR_NOT_SUPPORTED;
}

static void op_stub(pmix_status_t status, void *cbdata)
{
    (void) status;
    (void) cbdata;
}

/* Build the wire form of a LOG request as a >= 3.0 client sends it:
 * timestamp, ninfo, info[], ndirs, directives[]. The two counts are passed
 * separately from the arrays so a caller can lie about them. */
static pmix_buffer_t *build_log_request(time_t timestamp,
                                        size_t claimed_ninfo, pmix_info_t *info,
                                        size_t actual_ninfo,
                                        size_t claimed_ndirs, pmix_info_t *dirs,
                                        size_t actual_ndirs)
{
    pmix_buffer_t *buf;
    pmix_status_t rc;

    buf = PMIX_NEW(pmix_buffer_t);
    if (NULL == buf) {
        return NULL;
    }
    PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, buf, &timestamp, 1, PMIX_TIME);
    if (PMIX_SUCCESS != rc) {
        goto err;
    }
    PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, buf, &claimed_ninfo, 1, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        goto err;
    }
    if (0 < actual_ninfo) {
        PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, buf, info, actual_ninfo, PMIX_INFO);
        if (PMIX_SUCCESS != rc) {
            goto err;
        }
    }
    PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, buf, &claimed_ndirs, 1, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        goto err;
    }
    if (0 < actual_ndirs) {
        PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, buf, dirs, actual_ndirs, PMIX_INFO);
        if (PMIX_SUCCESS != rc) {
            goto err;
        }
    }
    return buf;

err:
    PMIX_RELEASE(buf);
    return NULL;
}

static pmix_status_t do_log(time_t timestamp,
                            size_t claimed_ninfo, pmix_info_t *info, size_t actual_ninfo,
                            size_t claimed_ndirs, pmix_info_t *dirs, size_t actual_ndirs)
{
    pmix_buffer_t *buf;
    pmix_status_t rc;

    log_fired = false;
    log_ndata = 0;
    log_ndirs = 0;
    log_saw_source = false;
    log_saw_timestamp = false;

    buf = build_log_request(timestamp, claimed_ninfo, info, actual_ninfo,
                            claimed_ndirs, dirs, actual_ndirs);
    if (NULL == buf) {
        return PMIX_ERR_NOMEM;
    }
    rc = pmix_server_log(pmix_globals.mypeer, buf, op_stub, NULL);
    PMIX_RELEASE(buf);
    return rc;
}

/* Build and drive a JOB_CONTROL request carrying no targets, so the
 * cleanup directives land on the requesting peer's namespace epilog. */
static pmix_status_t do_job_ctrl(pmix_info_t *info, size_t ninfo)
{
    pmix_buffer_t *buf;
    pmix_status_t rc;
    size_t ntargets = 0;

    jobctrl_fired = false;

    buf = PMIX_NEW(pmix_buffer_t);
    if (NULL == buf) {
        return PMIX_ERR_NOMEM;
    }
    PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, buf, &ntargets, 1, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(buf);
        return rc;
    }
    PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, buf, &ninfo, 1, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(buf);
        return rc;
    }
    PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, buf, info, ninfo, PMIX_INFO);
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(buf);
        return rc;
    }
    rc = pmix_server_job_ctrl(pmix_globals.mypeer, buf, NULL, NULL);
    PMIX_RELEASE(buf);
    return rc;
}

static pmix_cleanup_dir_t *only_epilog_dir(size_t *count)
{
    pmix_epilog_t *epi = &pmix_globals.mypeer->nptr->epilog;
    pmix_cleanup_dir_t *cd, *found = NULL;

    *count = 0;
    PMIX_LIST_FOREACH (cd, &epi->cleanup_dirs, pmix_cleanup_dir_t) {
        ++(*count);
        if (NULL == found) {
            found = cd;
        }
    }
    return found;
}

static void drain_epilog_dirs(void)
{
    pmix_epilog_t *epi = &pmix_globals.mypeer->nptr->epilog;
    pmix_list_item_t *item;

    /* drain rather than PMIX_LIST_DESTRUCT: the epilog owns the list and
     * will destruct it itself, and PMIX_LIST_DESTRUCT is not idempotent */
    while (NULL != (item = pmix_list_remove_first(&epi->cleanup_dirs))) {
        PMIX_RELEASE(item);
    }
}

int main(int argc, char **argv)
{
    static pmix_server_module_t mymodule = {0};
    pmix_status_t rc;
    pmix_info_t data, dirs[1], jc[2];
    pmix_cleanup_dir_t *cd;
    size_t ndirs;
    bool flag = true;

    (void) argc;
    (void) argv;

    fprintf(stdout, "server_control: server-side log and job-control unit tests\n");

    mymodule.log2 = stub_log2;
    mymodule.job_control = stub_job_control;

    rc = PMIx_server_init(&mymodule, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "PMIx_server_init failed: %s\n", PMIx_Error_string(rc));
        return 1;
    }
    /* take the gateway question out of the picture: with this set the
     * handler always passes the record up to the host, which is the arm
     * these cases are about */
    pmix_log_host_only = true;

    PMIX_INFO_LOAD(&data, PMIX_LOG_STDERR, "hello", PMIX_STRING);
    PMIX_INFO_LOAD(&dirs[0], PMIX_LOG_GENERATE_TIMESTAMP, &flag, PMIX_BOOL);

    /* --- a well-formed record reaches the host with the source added --- */
    rc = do_log(0, 1, &data, 1, 1, dirs, 1);
    report("well-formed log accepted", PMIX_SUCCESS == rc);
    report("well-formed log reached the host", log_fired);
    report("well-formed log kept the caller's data", 1 == log_ndata);
    report("well-formed log appended the source", 2 == log_ndirs && log_saw_source);
    report("well-formed log added no timestamp", !log_saw_timestamp);

    /* --- a record carrying a timestamp gets that appended too --------- */
    rc = do_log(1234567, 1, &data, 1, 1, dirs, 1);
    report("timestamped log accepted", PMIX_SUCCESS == rc);
    report("timestamped log appended source and timestamp",
           3 == log_ndirs && log_saw_source && log_saw_timestamp);

    /* --- a record with no directives at all still gets a source ------- */
    rc = do_log(0, 0, NULL, 0, 0, NULL, 0);
    report("empty log accepted", PMIX_SUCCESS == rc);
    report("empty log still carries the source", 1 == log_ndirs && log_saw_source);

    /* --- counts that do not survive the trip through int32_t ---------- */
    rc = do_log(0, (size_t) 0x80000000UL, &data, 1, 1, dirs, 1);
    report("log rejects an unusable info count", PMIX_ERR_BAD_PARAM == rc);
    report("log did not hand the host a bogus info count", !log_fired);

    rc = do_log(0, 1, &data, 1, (size_t) 0x80000000UL, dirs, 1);
    report("log rejects an unusable directive count", PMIX_ERR_BAD_PARAM == rc);
    report("log did not hand the host a bogus directive count", !log_fired);

    PMIX_INFO_DESTRUCT(&data);
    PMIX_INFO_DESTRUCT(&dirs[0]);

    /* --- job control: register a cleanup directory -------------------- */
    PMIX_INFO_LOAD(&jc[0], PMIX_REGISTER_CLEANUP_DIR, CTLUT_DIR, PMIX_STRING);
    rc = do_job_ctrl(jc, 1);
    report("cleanup directory registered", PMIX_OPERATION_SUCCEEDED == rc);
    report("cleanup registration stayed local to us", !jobctrl_fired);
    cd = only_epilog_dir(&ndirs);
    report("epilog holds exactly one directory", 1 == ndirs && NULL != cd);
    report("registered directory is not recursive",
           NULL != cd && !cd->recurse && !cd->leave_topdir);

    /* --- the same directory again, now asking for recursion ----------- */
    PMIX_INFO_LOAD(&jc[1], PMIX_CLEANUP_RECURSIVE, &flag, PMIX_BOOL);
    rc = do_job_ctrl(jc, 2);
    report("repeated cleanup directory accepted", PMIX_OPERATION_SUCCEEDED == rc);
    cd = only_epilog_dir(&ndirs);
    report("repeat did not duplicate the entry", 1 == ndirs && NULL != cd);
    report("repeat upgraded the registered entry to recursive",
           NULL != cd && cd->recurse);
    PMIX_INFO_DESTRUCT(&jc[1]);

    /* --- and once more asking to leave the top directory -------------- */
    PMIX_INFO_LOAD(&jc[1], PMIX_CLEANUP_LEAVE_TOPDIR, &flag, PMIX_BOOL);
    rc = do_job_ctrl(jc, 2);
    report("third cleanup directive accepted", PMIX_OPERATION_SUCCEEDED == rc);
    cd = only_epilog_dir(&ndirs);
    report("third request did not duplicate the entry", 1 == ndirs && NULL != cd);
    report("third request added leave-topdir", NULL != cd && cd->leave_topdir);
    report("third request did not undo recursion", NULL != cd && cd->recurse);
    PMIX_INFO_DESTRUCT(&jc[1]);
    PMIX_INFO_DESTRUCT(&jc[0]);

    drain_epilog_dirs();

    PMIx_server_finalize();

    fprintf(stdout, "server_control: %d passed, %d failed\n", npass, nfail);
    return (0 == nfail) ? 0 : 1;
}
