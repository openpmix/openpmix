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
 *   get_credential: a host that refuses must still be answered
 *      pmix_credential_cbfunc_t's contract is an error status and no
 *      credential. The copy of that credential screens a NULL source, so
 *      the ordinary refusal used to take an error return that queued
 *      nothing and the client's PMIx_Get_credential never came back.
 *
 *   get_credential: the host's reply info is copied, not borrowed
 *      pmix_credential_cbfunc_t carries no (release_fn, release_cbdata)
 *      pair, so nothing the host passes to it survives the return of the
 *      call - and pmix_server_cred_cbfunc only thread-shifts, packing the
 *      reply later on the progress thread. The credential itself was
 *      always copied; the info array beside it was parked by pointer, so
 *      a host that built it on its stack (the natural thing to do for a
 *      one-element answer) had the server pack that frame after it had
 *      been reused. The stub below deliberately destructs and overwrites
 *      its array the instant the callback returns, so the queued reply
 *      carries a recognizable key only if the copy really happened.
 *      Read back from the peer's send queue, which is where a reply to a
 *      socketless peer comes to rest.
 *
 *   query: the counts and the keys array both arrive off the wire
 *      The query count sizes PMIX_QUERY_CREATE, which multiplies it by
 *      sizeof(pmix_query_t) with no overflow guard and then constructs
 *      every element it claimed, so it has to be refused before it
 *      reaches the allocator rather than after. And a query declaring no
 *      keys leaves the array NULL - the unpacker reports success for it -
 *      where pmix_parse_localquery walks it to a terminator that is not
 *      there. PMIx_Query_info_nb screens that in the requestor's own
 *      process, which buys a server nothing. The keyless case takes an
 *      unfixed library down rather than failing it.
 *
 *   job control: the target count the peer declared is not the one to walk
 *      PMIX_BFROPS_UNPACK fills min(packed, provided) and reports
 *      success, so declaring more targets than are sent leaves the tail
 *      of the array default-constructed - an empty nspace at
 *      PMIX_RANK_UNDEF. The handler creates a pmix_namespace_t for every
 *      target nspace it does not recognize, so each phantom appended one
 *      named "" to pmix_globals.nspaces for the life of the server - and
 *      PMIX_CHECK_NSPACE reports an empty name as matching every
 *      namespace, so the list walks in src/server that stop at the first
 *      match could stop there. These cases fail rather than crash.
 *
 *   job control: a repeated ignore directive must not accumulate
 *      The three epilog loops are near-copies of one another, and each
 *      has to scan the list it appends to. The ignores loop kept the
 *      files loop's scan list, so it asked whether the path was already
 *      registered for cleanup and appended to "ignores" either way: a
 *      repeated PMIX_CLEANUP_IGNORE was never seen as a duplicate and
 *      grew a list that lives as long as the namespace, and an ignore
 *      naming an already-registered cleanup path was dropped, so the
 *      epilog went on deleting a file the client asked it to keep. Both
 *      halves fail, rather than crash, against an unfixed library.
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
 *
 *   job control: a refused cleanup request must apply none of itself
 *      The three epilog lists outlive the request and a client has no
 *      way to take an entry back off one, so a request refused with
 *      PMIX_ERR_CONFLICTING_CLEANUP_DIRECTIVES has to leave nothing
 *      behind. It used to keep its own ignores (applied first, which is
 *      what the conflict is detected against), every directory accepted
 *      ahead of the conflicting one, and any flag widening it had
 *      applied to an entry already registered - that last being the
 *      destructive one, since a directory the client believes untouched
 *      becomes recursive and takes its subtree with it at termination.
 *      All three cases fail, rather than crash, against an unfixed
 *      library.
 *
 *   job control: a comma-delimited path list, and what the epilog does
 *      The three path attributes are documented as comma-delimited
 *      lists, and the expansion used to happen only in
 *      pmix_execute_epilog - so an ignore of "/a,/b" matched no filename
 *      the epilog ever constructed, and a value of "," reached the
 *      epilog as an entry whose own split returns NULL and was then
 *      indexed. The last case here is the one the ignore list exists
 *      for: real files on disk, one registered for cleanup and then
 *      ignored, which the epilog deleted anyway because its
 *      cleanup_files loop never consulted the ignores at all. These
 *      cases fail rather than crash - the "," case drains the entry it
 *      registered so that it reports as a failure instead of taking the
 *      epilog below it down.
 *
 *   job control: the two cleanup modes, and the epilog's identity
 *      PMIX_CLEANUP_RECURSIVE and PMIX_CLEANUP_EMPTY are mutually
 *      exclusive - the first removes every file and then the
 *      directories that held them, the second removes only the
 *      directories that are empty and leaves every file alone - so a
 *      request naming both is refused. Both are driven over a real tree
 *      on disk, because what each one spares is as much the point as
 *      what it removes. The last case is the privilege drop: the epilog
 *      records the uid the host registered the peer with, and walks
 *      under it rather than as whatever user the server happens to be.
 *      We are unprivileged, so pointing the epilog at anyone else is a
 *      drop we cannot make, and the file must survive - paired with the
 *      same removal under our own identity, so the case cannot pass by
 *      the epilog simply never working.
 */

#include "src/include/pmix_config.h"

#include "include/pmix.h"
#include "include/pmix_server.h"

#include "src/class/pmix_list.h"
#include "src/include/pmix_globals.h"
#include "src/mca/bfrops/bfrops.h"
#include "src/mca/ptl/ptl_types.h"
#include "src/runtime/pmix_rte.h"
#include "src/server/pmix_server_ops.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CTLUT_DIR "/tmp/pmix-server-control-ut-no-such-dir"

/* the sentinel the credential case looks for in the queued reply */
#define CTLUT_CREDKEY "credut.key"
#define CTLUT_CREDVAL "credut-value"

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

static void skip(const char *name, const char *why)
{
    fprintf(stdout, "  SKIP: %s (%s)\n", name, why);
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

/* what the switchyard's query callback was told */
static bool qry_fired = false;
static pmix_status_t qry_status = PMIX_SUCCESS;

static void qry_cbfunc(pmix_status_t status, pmix_info_t *info, size_t ninfo,
                       void *cbdata, pmix_release_cbfunc_t relfn, void *relcbd)
{
    pmix_server_caddy_t *cd = (pmix_server_caddy_t *) cbdata;

    (void) info;
    (void) ninfo;
    qry_fired = true;
    qry_status = status;
    if (NULL != relfn) {
        relfn(relcbd);
    }
    PMIX_RELEASE(cd);
}

/* the handler refuses to run at all without one of these, but none of the
 * cases below is supposed to reach it: they are all pure cleanup requests,
 * which the server answers itself */
static bool jobctrl_fired = false;
static size_t jobctrl_ntargets = 0;
static pmix_status_t stub_job_control(const pmix_proc_t *requestor,
                                      const pmix_proc_t targets[], size_t ntargets,
                                      const pmix_info_t directives[], size_t ndirs,
                                      pmix_info_cbfunc_t cbfunc, void *cbdata)
{
    (void) requestor;
    (void) targets;
    (void) directives;
    (void) ndirs;
    (void) cbfunc;
    (void) cbdata;

    jobctrl_fired = true;
    jobctrl_ntargets = ntargets;
    return PMIX_ERR_NOT_SUPPORTED;
}

static void op_stub(pmix_status_t status, void *cbdata)
{
    (void) status;
    (void) cbdata;
}

/* Answer the credential request from data this frame owns, then take it
 * all back. pmix_credential_cbfunc_t hands down no release function, so
 * this is exactly what the contract permits - and it is what a server
 * that parked the array rather than copying it cannot survive. */
static bool cred_fired = false;

/* Overwrite through a volatile pointer: a plain memset over a local the
 * function never reads again is dead code the compiler may drop, and the
 * whole point of the case is that the overwrite really happens. */
static void scribble(void *addr, size_t len)
{
    volatile unsigned char *p = (volatile unsigned char *) addr;
    size_t n;

    for (n = 0; n < len; n++) {
        p[n] = 0x5a;
    }
}

/* when set, the stub answers the way a host that cannot issue a
 * credential does: an error status and NO credential */
static bool cred_refuse = false;

static pmix_status_t stub_get_credential(const pmix_proc_t *proc,
                                         const pmix_info_t directives[], size_t ndirs,
                                         pmix_credential_cbfunc_t cbfunc, void *cbdata)
{
    pmix_info_t reply[1];
    pmix_byte_object_t cred;
    char bytes[4] = {'c', 'r', 'e', 'd'};

    (void) proc;
    (void) directives;
    (void) ndirs;

    cred_fired = true;
    if (cred_refuse) {
        /* pmix_credential_cbfunc_t's contract: PMIX_SUCCESS if a
         * credential could be assigned, "or else an appropriate error
         * code indicating the problem" - with nothing to point at */
        cbfunc(PMIX_ERR_NOT_SUPPORTED, NULL, NULL, 0, cbdata);
        return PMIX_SUCCESS;
    }
    cred.bytes = bytes;
    cred.size = sizeof(bytes);
    PMIX_INFO_LOAD(&reply[0], CTLUT_CREDKEY, CTLUT_CREDVAL, PMIX_STRING);

    cbfunc(PMIX_SUCCESS, &cred, reply, 1, cbdata);

    /* everything above is ours again the moment that call returns */
    PMIX_INFO_DESTRUCT(&reply[0]);
    scribble(reply, sizeof(reply));
    scribble(bytes, sizeof(bytes));
    return PMIX_SUCCESS;
}

/* Take the reply the server queued to a peer that has no socket. With
 * sd < 0 the send event is never armed, so the message simply comes to
 * rest on the peer and can be read back here. */
static pmix_buffer_t *take_queued_reply(void)
{
    pmix_peer_t *peer = pmix_globals.mypeer;
    pmix_ptl_send_t *snd = peer->send_msg;
    pmix_buffer_t *buf;

    if (NULL == snd) {
        return NULL;
    }
    peer->send_msg = NULL;
    buf = snd->data;
    /* the buffer is ours now - the send's destructor would release it */
    snd->data = NULL;
    PMIX_RELEASE(snd);
    return buf;
}

/* Drive one GET_CREDENTIAL request. The wire form is just the directive
 * count followed by the directives. */
static pmix_status_t do_get_credential(void)
{
    pmix_buffer_t *buf;
    pmix_server_caddy_t *cd;
    pmix_status_t rc;
    size_t ndirs = 0;

    cred_fired = false;

    buf = PMIX_NEW(pmix_buffer_t);
    if (NULL == buf) {
        return PMIX_ERR_NOMEM;
    }
    PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, buf, &ndirs, 1, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(buf);
        return rc;
    }

    cd = PMIX_NEW(pmix_server_caddy_t);
    if (NULL == cd) {
        PMIX_RELEASE(buf);
        return PMIX_ERR_NOMEM;
    }
    PMIX_RETAIN(pmix_globals.mypeer);
    cd->peer = pmix_globals.mypeer;
    cd->hdr.tag = 0;

    rc = pmix_server_get_credential(pmix_globals.mypeer, buf,
                                    pmix_server_cred_cbfunc, cd);
    if (PMIX_SUCCESS != rc) {
        /* the switchyard owns the caddy on a non-success return */
        PMIX_RELEASE(cd);
    }
    PMIX_RELEASE(buf);
    return rc;
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

/* Drive one server-side QUERY carrying a single key and a single
 * qualifier, packed with whatever value type the caller names. The
 * server unpacks these off the wire, so it cannot lean on the screening
 * PMIx_Query_info_nb does in the requestor's own process. */
static pmix_status_t do_query(const char *key, const char *qualkey,
                              const void *qualval, pmix_data_type_t qualtype)
{
    pmix_buffer_t *buf;
    pmix_server_caddy_t *cd;
    pmix_query_t qry;
    pmix_status_t rc;
    size_t nqueries = 1;

    qry_fired = false;
    qry_status = PMIX_SUCCESS;

    PMIX_QUERY_CONSTRUCT(&qry);
    PMIX_ARGV_APPEND(rc, qry.keys, key);
    if (PMIX_SUCCESS != rc) {
        PMIX_QUERY_DESTRUCT(&qry);
        return rc;
    }
    qry.nqual = 1;
    PMIX_INFO_CREATE(qry.qualifiers, 1);
    PMIX_INFO_LOAD(&qry.qualifiers[0], qualkey, qualval, qualtype);

    buf = PMIX_NEW(pmix_buffer_t);
    if (NULL == buf) {
        PMIX_QUERY_DESTRUCT(&qry);
        return PMIX_ERR_NOMEM;
    }
    PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, buf, &nqueries, 1, PMIX_SIZE);
    if (PMIX_SUCCESS == rc) {
        PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, buf, &qry, 1, PMIX_QUERY);
    }
    PMIX_QUERY_DESTRUCT(&qry);
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(buf);
        return rc;
    }

    cd = PMIX_NEW(pmix_server_caddy_t);
    if (NULL == cd) {
        PMIX_RELEASE(buf);
        return PMIX_ERR_NOMEM;
    }
    PMIX_RETAIN(pmix_globals.mypeer);
    cd->peer = pmix_globals.mypeer;
    cd->hdr.tag = 0;

    rc = pmix_server_query(pmix_globals.mypeer, buf, qry_cbfunc, cd);
    if (PMIX_SUCCESS != rc) {
        /* the switchyard owns the caddy on a non-success return */
        PMIX_RELEASE(cd);
    }
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

/* Drive a server-side QUERY whose declared query count is whatever the
 * caller names, with nothing packed behind it. The count sizes
 * PMIX_QUERY_CREATE, which multiplies it by sizeof(pmix_query_t) with no
 * overflow guard and then constructs every element it claimed, so a count
 * that does not survive the round trip through the int32_t the unpack
 * consumes has to be refused before it reaches the allocator. */
static pmix_status_t do_query_count(size_t nqueries)
{
    pmix_buffer_t *buf;
    pmix_server_caddy_t *cd;
    pmix_status_t rc;

    qry_fired = false;
    qry_status = PMIX_SUCCESS;

    buf = PMIX_NEW(pmix_buffer_t);
    if (NULL == buf) {
        return PMIX_ERR_NOMEM;
    }
    PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, buf, &nqueries, 1, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(buf);
        return rc;
    }

    cd = PMIX_NEW(pmix_server_caddy_t);
    if (NULL == cd) {
        PMIX_RELEASE(buf);
        return PMIX_ERR_NOMEM;
    }
    PMIX_RETAIN(pmix_globals.mypeer);
    cd->peer = pmix_globals.mypeer;
    cd->hdr.tag = 0;

    rc = pmix_server_query(pmix_globals.mypeer, buf, qry_cbfunc, cd);
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(cd);
    }
    PMIX_RELEASE(buf);
    return rc;
}

/* Drive a server-side QUERY carrying one query that declares no keys at
 * all. PMIX_QUERY_CONSTRUCT leaves "keys" NULL and the unpacker leaves it
 * that way for a zero count, reporting success - and pmix_parse_localquery
 * walks that array to its NULL terminator. PMIx_Query_info_nb screens this
 * in the requestor's own process, which buys a server nothing when the
 * query arrived off the wire. This case takes an unfixed library down
 * rather than failing it. */
static pmix_status_t do_query_nokeys(void)
{
    pmix_buffer_t *buf;
    pmix_server_caddy_t *cd;
    pmix_query_t qry;
    pmix_status_t rc;
    size_t nqueries = 1;

    qry_fired = false;
    qry_status = PMIX_SUCCESS;

    PMIX_QUERY_CONSTRUCT(&qry);  // keys stays NULL, nqual stays 0

    buf = PMIX_NEW(pmix_buffer_t);
    if (NULL == buf) {
        PMIX_QUERY_DESTRUCT(&qry);
        return PMIX_ERR_NOMEM;
    }
    PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, buf, &nqueries, 1, PMIX_SIZE);
    if (PMIX_SUCCESS == rc) {
        PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, buf, &qry, 1, PMIX_QUERY);
    }
    PMIX_QUERY_DESTRUCT(&qry);
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(buf);
        return rc;
    }

    cd = PMIX_NEW(pmix_server_caddy_t);
    if (NULL == cd) {
        PMIX_RELEASE(buf);
        return PMIX_ERR_NOMEM;
    }
    PMIX_RETAIN(pmix_globals.mypeer);
    cd->peer = pmix_globals.mypeer;
    cd->hdr.tag = 0;

    rc = pmix_server_query(pmix_globals.mypeer, buf, qry_cbfunc, cd);
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(cd);
    }
    PMIX_RELEASE(buf);
    return rc;
}

/* Build a JOB_CONTROL request that declares more targets than it packs.
 * The unpack fills min(packed, provided) and reports success, so the tail
 * of the array is left default-constructed - an empty nspace at
 * PMIX_RANK_UNDEF - and the handler's walk creates a pmix_namespace_t for
 * every target nspace it does not recognize. An empty name is one
 * PMIX_CHECK_NSPACE reports as matching every namespace, so each phantom
 * is a permanent trap on a list every lookup in src/server walks. */
static pmix_status_t do_job_ctrl_targets(size_t declared, size_t packed)
{
    pmix_buffer_t *buf;
    pmix_proc_t *targets = NULL;
    pmix_status_t rc;
    size_t ninfo = 0, n;

    jobctrl_fired = false;
    jobctrl_ntargets = SIZE_MAX;

    buf = PMIX_NEW(pmix_buffer_t);
    if (NULL == buf) {
        return PMIX_ERR_NOMEM;
    }
    PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, buf, &declared, 1, PMIX_SIZE);
    if (PMIX_SUCCESS == rc && 0 < packed) {
        PMIX_PROC_CREATE(targets, packed);
        if (NULL == targets) {
            PMIX_RELEASE(buf);
            return PMIX_ERR_NOMEM;
        }
        for (n = 0; n < packed; n++) {
            PMIX_LOAD_PROCID(&targets[n], pmix_globals.myid.nspace, PMIX_RANK_WILDCARD);
        }
        PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, buf, targets, packed, PMIX_PROC);
        PMIX_PROC_FREE(targets, packed);
    }
    if (PMIX_SUCCESS == rc) {
        PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, buf, &ninfo, 1, PMIX_SIZE);
    }
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(buf);
        return rc;
    }
    rc = pmix_server_job_ctrl(pmix_globals.mypeer, buf, NULL, NULL);
    PMIX_RELEASE(buf);
    return rc;
}

/* How many namespaces on the global list carry an empty name. Nothing
 * legitimate ever puts one there. */
static size_t count_empty_nspaces(void)
{
    pmix_namespace_t *ns;
    size_t n = 0;

    PMIX_LIST_FOREACH (ns, &pmix_globals.nspaces, pmix_namespace_t) {
        if (NULL == ns->nspace || 0 == strlen(ns->nspace)) {
            ++n;
        }
    }
    return n;
}

/* How many entries sit on our own namespace's epilog ignore list. */
static size_t count_epilog_ignores(void)
{
    pmix_epilog_t *epi = &pmix_globals.mypeer->nptr->epilog;
    pmix_cleanup_file_t *cf;
    size_t n = 0;

    PMIX_LIST_FOREACH (cf, &epi->ignores, pmix_cleanup_file_t) {
        ++n;
    }
    return n;
}

/* How many entries sit on our own namespace's epilog cleanup-file list. */
static size_t count_epilog_files(void)
{
    pmix_epilog_t *epi = &pmix_globals.mypeer->nptr->epilog;
    pmix_cleanup_file_t *cf;
    size_t n = 0;

    PMIX_LIST_FOREACH (cf, &epi->cleanup_files, pmix_cleanup_file_t) {
        ++n;
    }
    return n;
}

static void drain_epilog_files(void)
{
    pmix_epilog_t *epi = &pmix_globals.mypeer->nptr->epilog;
    pmix_list_item_t *item;

    while (NULL != (item = pmix_list_remove_first(&epi->cleanup_files))) {
        PMIX_RELEASE(item);
    }
}

static void drain_epilog_ignores(void)
{
    pmix_epilog_t *epi = &pmix_globals.mypeer->nptr->epilog;
    pmix_list_item_t *item;

    while (NULL != (item = pmix_list_remove_first(&epi->ignores))) {
        PMIX_RELEASE(item);
    }
}

/* A blocking round trip through the progress thread. pmix_server_query
 * thread-shifts, so this is what makes its completion observable without
 * a sleep: anything queued ahead of this call has run by the time it
 * returns. */
static void progress_barrier(void)
{
    pmix_value_t v;

    PMIX_VALUE_LOAD(&v, "barrier", PMIX_STRING);
    PMIx_Store_internal(&pmix_globals.myid, "server-control-ut.barrier", &v);
    PMIX_VALUE_DESTRUCT(&v);
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

/* PMIx_Abort's server-role branch, which hands the request to the host
 * through pmix_server_abort_fn_t.
 *
 * It lives here rather than with the other client-API cases because it
 * needs a server with a host module behind it, which is what this file
 * already stands up. The typedef defines that up-call as completing
 * through the callback it is given, with the requestor held until it
 * does. PMIx_Abort used to pass NULL for it and return whatever the
 * up-call returned, so a host honoring the contract got a NULL
 * dereference, one that guards it (as PRRTE does) answered every abort
 * with "queued" and its later rejection never reached the caller, and a
 * host completing inline had PMIX_OPERATION_SUCCEEDED handed to the
 * application as a failure. */
static int abort_mode = 0;
static int abort_saw_null_cbfunc = 0;
static int abort_fired = 0;
static pmix_op_cbfunc_t abort_saved_cb = NULL;
static void *abort_saved_cbdata = NULL;

static void *abort_late_thread(void *arg)
{
    (void) arg;
    /* answer well after the up-call returned, so the only way PMIx_Abort
     * can report this status is by having waited for it */
    usleep(50000);
    abort_saved_cb(PMIX_ERR_TIMEOUT, abort_saved_cbdata);
    return NULL;
}

static pmix_status_t stub_abort(const pmix_proc_t *proc, void *server_object, int status,
                                const char msg[], pmix_proc_t procs[], size_t nprocs,
                                pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    pthread_t tid;

    (void) proc;
    (void) server_object;
    (void) status;
    (void) msg;
    (void) procs;
    (void) nprocs;

    abort_fired++;
    if (NULL == cbfunc) {
        abort_saw_null_cbfunc = 1;
        return PMIX_SUCCESS;
    }
    if (0 == abort_mode) {
        /* answer immediately, the way a host that has the answer to hand
         * does - the wakeup lands before the wait */
        cbfunc(PMIX_ERR_NOT_FOUND, cbdata);
        return PMIX_SUCCESS;
    }
    if (1 == abort_mode) {
        abort_saved_cb = cbfunc;
        abort_saved_cbdata = cbdata;
        if (0 != pthread_create(&tid, NULL, abort_late_thread, NULL)) {
            return PMIX_ERROR;
        }
        pthread_detach(tid);
        return PMIX_SUCCESS;
    }
    /* did the work itself; no callback is coming */
    return PMIX_OPERATION_SUCCEEDED;
}

static void test_abort_upcall(void)
{
    pmix_status_t rc;

    abort_mode = 0;
    abort_fired = 0;
    rc = PMIx_Abort(1, "inline", NULL, 0);
    report("abort reached the host", 1 == abort_fired);
    report("abort was given a callback", !abort_saw_null_cbfunc);
    report("an inline answer reaches the caller", PMIX_ERR_NOT_FOUND == rc);

    abort_mode = 1;
    abort_fired = 0;
    rc = PMIx_Abort(1, "deferred", NULL, 0);
    report("a deferred answer is waited for", PMIX_ERR_TIMEOUT == rc);

    abort_mode = 2;
    abort_fired = 0;
    rc = PMIx_Abort(1, "inline-complete", NULL, 0);
    report("PMIX_OPERATION_SUCCEEDED reads as success", PMIX_SUCCESS == rc);
}

int main(int argc, char **argv)
{
    static pmix_server_module_t mymodule = {0};
    pmix_status_t rc;
    pmix_info_t data, dirs[1], jc[2];
    char cleanupval[600];
    pmix_cleanup_dir_t *cd;
    size_t ndirs;
    bool flag = true;

    (void) argc;
    (void) argv;

    fprintf(stdout, "server_control: server-side log and job-control unit tests\n");

    mymodule.log2 = stub_log2;
    mymodule.job_control = stub_job_control;
    mymodule.get_credential = stub_get_credential;
    mymodule.abort = stub_abort;

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

    /* --- query qualifiers arrive off the wire and must be screened ---- */
    {
        pmix_proc_t qp;
        pmix_rank_t qr = 1;

        PMIX_LOAD_PROCID(&qp, "server-control-ut-nspace", 0);
        rc = do_query(PMIX_QUERY_NAMESPACES, PMIX_PROCID, &qp, PMIX_PROC);
        progress_barrier();
        report("well-formed PROCID qualifier is accepted",
               PMIX_SUCCESS == rc && qry_fired && PMIX_ERR_BAD_PARAM != qry_status);

        /* a PROCID tagged as a string makes the union a char* - reading
         * it as a pmix_proc_t* walks off the end of that string */
        rc = do_query(PMIX_QUERY_NAMESPACES, PMIX_PROCID, "not-a-proc", PMIX_STRING);
        progress_barrier();
        report("mistyped PROCID qualifier is rejected",
               PMIX_SUCCESS == rc && qry_fired && PMIX_ERR_BAD_PARAM == qry_status);

        rc = do_query(PMIX_QUERY_NAMESPACES, PMIX_NSPACE, &qr, PMIX_PROC_RANK);
        progress_barrier();
        report("mistyped NSPACE qualifier is rejected",
               PMIX_SUCCESS == rc && qry_fired && PMIX_ERR_BAD_PARAM == qry_status);

        rc = do_query(PMIX_QUERY_NAMESPACES, PMIX_RANK, "not-a-rank", PMIX_STRING);
        progress_barrier();
        report("mistyped RANK qualifier is rejected",
               PMIX_SUCCESS == rc && qry_fired && PMIX_ERR_BAD_PARAM == qry_status);

        /* the query count sizes an allocation that constructs every
         * element it claimed, so it has to be refused before it gets
         * there rather than after */
        rc = do_query_count((size_t) 0x80000000UL);
        report("query rejects an unusable query count", PMIX_ERR_BAD_PARAM == rc);

        /* a query naming no keys at all reaches the key walk in
         * pmix_parse_localquery, which has no terminator to find */
        rc = do_query_nokeys();
        progress_barrier();
        report("keyless query is rejected",
               PMIX_SUCCESS == rc && qry_fired && PMIX_ERR_BAD_PARAM == qry_status);
    }

    /* --- the credential reply must carry a copy of the host's info ---- */
    {
        pmix_buffer_t *reply;
        pmix_status_t ret = PMIX_SUCCESS;
        pmix_byte_object_t bo;
        pmix_info_t *rinfo = NULL;
        size_t rninfo = 0;
        int32_t cnt;

        /* nothing should be parked on us before this */
        reply = take_queued_reply();
        if (NULL != reply) {
            PMIX_RELEASE(reply);
        }

        rc = do_get_credential();
        progress_barrier();
        report("credential request accepted", PMIX_SUCCESS == rc);
        report("credential request reached the host", cred_fired);

        reply = take_queued_reply();
        if (NULL == reply) {
            skip("credential reply carries the host's info",
                 "no reply came to rest on this peer");
        } else {
            PMIX_BYTE_OBJECT_CONSTRUCT(&bo);
            cnt = 1;
            PMIX_BFROPS_UNPACK(rc, pmix_globals.mypeer, reply, &ret, &cnt, PMIX_STATUS);
            report("credential reply carries a status", PMIX_SUCCESS == rc);
            report("credential reply reports success", PMIX_SUCCESS == ret);
            if (PMIX_SUCCESS == rc && PMIX_SUCCESS == ret) {
                cnt = 1;
                PMIX_BFROPS_UNPACK(rc, pmix_globals.mypeer, reply, &bo, &cnt,
                                   PMIX_BYTE_OBJECT);
                report("credential reply carries the credential",
                       PMIX_SUCCESS == rc && 4 == bo.size && NULL != bo.bytes
                           && 0 == memcmp(bo.bytes, "cred", 4));
                cnt = 1;
                PMIX_BFROPS_UNPACK(rc, pmix_globals.mypeer, reply, &rninfo, &cnt, PMIX_SIZE);
                if (PMIX_SUCCESS == rc && 1 == rninfo) {
                    PMIX_INFO_CREATE(rinfo, rninfo);
                    cnt = (int32_t) rninfo;
                    PMIX_BFROPS_UNPACK(rc, pmix_globals.mypeer, reply, rinfo, &cnt,
                                       PMIX_INFO);
                }
                report("credential reply carries one info", PMIX_SUCCESS == rc
                           && 1 == rninfo && NULL != rinfo);
                /* the host destructed and overwrote its array before it
                 * returned, so this only survives a real copy */
                report("credential reply kept the host's key",
                       NULL != rinfo && PMIX_CHECK_KEY(&rinfo[0], CTLUT_CREDKEY));
                report("credential reply kept the host's value",
                       NULL != rinfo && PMIX_STRING == rinfo[0].value.type
                           && NULL != rinfo[0].value.data.string
                           && 0 == strcmp(rinfo[0].value.data.string, CTLUT_CREDVAL));
                if (NULL != rinfo) {
                    PMIX_INFO_FREE(rinfo, rninfo);
                }
            }
            PMIX_BYTE_OBJECT_DESTRUCT(&bo);
            PMIX_RELEASE(reply);
        }
    }

    /* --- a host that cannot issue a credential must still answer ------ */
    /* The contract on pmix_credential_cbfunc_t is an error status and no
     * credential, and _cred_cbfunc packs a credential only under a
     * success status - so the reply path was written for this. But the
     * outer callback copied the credential unconditionally, and the copy
     * screens a NULL source and reports PMIX_ERR_BAD_PARAM, so the
     * ordinary refusal took an error return that released everything and
     * queued nothing. The client's PMIx_Get_credential never came back.
     * Against an unfixed library the first assertion below reports no
     * reply at all. */
    {
        pmix_buffer_t *reply;
        pmix_status_t ret = PMIX_SUCCESS;
        int32_t cnt;

        reply = take_queued_reply();
        if (NULL != reply) {
            PMIX_RELEASE(reply);
        }

        cred_refuse = true;
        rc = do_get_credential();
        progress_barrier();
        cred_refuse = false;
        report("refused credential request accepted", PMIX_SUCCESS == rc);
        report("refused credential request reached the host", cred_fired);

        reply = take_queued_reply();
        report("a refused credential request is still answered", NULL != reply);
        if (NULL != reply) {
            cnt = 1;
            PMIX_BFROPS_UNPACK(rc, pmix_globals.mypeer, reply, &ret, &cnt, PMIX_STATUS);
            report("the refusal reply carries the host's status",
                   PMIX_SUCCESS == rc && PMIX_ERR_NOT_SUPPORTED == ret);
            PMIX_RELEASE(reply);
        }
    }

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

    /* --- a repeated ignore directive must not accumulate -------------- */
    {
        size_t nign;

        drain_epilog_ignores();
        PMIX_INFO_LOAD(&jc[0], PMIX_CLEANUP_IGNORE, CTLUT_DIR "/keepme", PMIX_STRING);
        rc = do_job_ctrl(jc, 1);
        report("ignore directive accepted", PMIX_OPERATION_SUCCEEDED == rc);
        nign = count_epilog_ignores();
        report("ignore directive reached the epilog", 1 == nign);

        /* the duplicate scan used to read cleanup_files - the list this
         * loop does not append to - so the same directive registered
         * again was never seen as a repeat */
        rc = do_job_ctrl(jc, 1);
        report("repeated ignore directive accepted", PMIX_OPERATION_SUCCEEDED == rc);
        nign = count_epilog_ignores();
        report("repeated ignore did not duplicate the entry", 1 == nign);
        PMIX_INFO_DESTRUCT(&jc[0]);

        /* an ignore naming a path already registered for cleanup used to
         * be dropped on the floor by that same scan, so the epilog went
         * on deleting a file the client had asked it to leave alone */
        drain_epilog_ignores();
        PMIX_INFO_LOAD(&jc[0], PMIX_REGISTER_CLEANUP, CTLUT_DIR "/gone", PMIX_STRING);
        rc = do_job_ctrl(jc, 1);
        report("cleanup file registered", PMIX_OPERATION_SUCCEEDED == rc);
        PMIX_INFO_DESTRUCT(&jc[0]);
        PMIX_INFO_LOAD(&jc[0], PMIX_CLEANUP_IGNORE, CTLUT_DIR "/gone", PMIX_STRING);
        rc = do_job_ctrl(jc, 1);
        report("ignore of a registered cleanup path accepted",
               PMIX_OPERATION_SUCCEEDED == rc);
        nign = count_epilog_ignores();
        report("ignore of a registered cleanup path was recorded", 1 == nign);
        PMIX_INFO_DESTRUCT(&jc[0]);
        drain_epilog_ignores();
    }

    /* --- a conflicting request must apply none of itself --------------- */
    {
        pmix_info_t at[4];
        size_t nign, nd;
        pmix_cleanup_dir_t *rd;
        bool yes = true;

        drain_epilog_ignores();
        drain_epilog_dirs();

        /* the simplest shape: one request naming the same path as both an
         * ignore and a cleanup directory. The ignores are applied first,
         * so the conflict is discovered with this request's own ignore
         * already on a list that outlives it - and the client is told the
         * request failed. A refused request must leave nothing behind, or
         * that path is poisoned for every later request naming it */
        PMIX_INFO_LOAD(&at[0], PMIX_CLEANUP_IGNORE, CTLUT_DIR "/both", PMIX_STRING);
        PMIX_INFO_LOAD(&at[1], PMIX_REGISTER_CLEANUP_DIR, CTLUT_DIR "/both", PMIX_STRING);
        rc = do_job_ctrl(at, 2);
        report("a self-conflicting cleanup request is refused",
               PMIX_ERR_CONFLICTING_CLEANUP_DIRECTIVES == rc);
        nign = count_epilog_ignores();
        report("a refused request registered no ignore", 0 == nign);
        only_epilog_dir(&nd);
        report("a refused request registered no directory", 0 == nd);
        PMIX_INFO_DESTRUCT(&at[0]);
        PMIX_INFO_DESTRUCT(&at[1]);

        /* and the same for a directory accepted ahead of the conflicting
         * one - it used to be registered for deletion by a request the
         * client was told had failed, which is the destructive direction */
        PMIX_INFO_LOAD(&at[0], PMIX_REGISTER_CLEANUP_DIR, CTLUT_DIR "/ok", PMIX_STRING);
        PMIX_INFO_LOAD(&at[1], PMIX_CLEANUP_IGNORE, CTLUT_DIR "/bad", PMIX_STRING);
        PMIX_INFO_LOAD(&at[2], PMIX_REGISTER_CLEANUP_DIR, CTLUT_DIR "/bad", PMIX_STRING);
        rc = do_job_ctrl(at, 3);
        report("a partly conflicting cleanup request is refused",
               PMIX_ERR_CONFLICTING_CLEANUP_DIRECTIVES == rc);
        only_epilog_dir(&nd);
        report("a refused request kept no directory accepted ahead of the conflict",
               0 == nd);
        report("a refused request kept none of its ignores",
               0 == count_epilog_ignores());
        PMIX_INFO_DESTRUCT(&at[0]);
        PMIX_INFO_DESTRUCT(&at[1]);
        PMIX_INFO_DESTRUCT(&at[2]);

        /* the sharpest arm: the flag widening a duplicate directive
         * applies to the entry already on the epilog. Left applied by a
         * request that then failed, a directory the client believes
         * untouched has become recursive and takes a whole subtree with
         * it at termination */
        PMIX_INFO_LOAD(&at[0], PMIX_REGISTER_CLEANUP_DIR, CTLUT_DIR "/keep", PMIX_STRING);
        rc = do_job_ctrl(at, 1);
        report("directory registered for the widening case",
               PMIX_OPERATION_SUCCEEDED == rc);
        rd = only_epilog_dir(&nd);
        report("the registered directory starts non-recursive",
               1 == nd && NULL != rd && !rd->recurse);
        PMIX_INFO_DESTRUCT(&at[0]);

        PMIX_INFO_LOAD(&at[0], PMIX_REGISTER_CLEANUP_DIR, CTLUT_DIR "/keep", PMIX_STRING);
        PMIX_INFO_LOAD(&at[1], PMIX_CLEANUP_RECURSIVE, &yes, PMIX_BOOL);
        PMIX_INFO_LOAD(&at[2], PMIX_CLEANUP_IGNORE, CTLUT_DIR "/bad", PMIX_STRING);
        PMIX_INFO_LOAD(&at[3], PMIX_REGISTER_CLEANUP_DIR, CTLUT_DIR "/bad", PMIX_STRING);
        rc = do_job_ctrl(at, 4);
        report("a conflicting request carrying a flag upgrade is refused",
               PMIX_ERR_CONFLICTING_CLEANUP_DIRECTIVES == rc);
        rd = only_epilog_dir(&nd);
        report("a refused request did not widen an existing entry",
               1 == nd && NULL != rd && !rd->recurse);
        report("a refused request added no second directory", 1 == nd);
        report("a refused request added no ignore", 0 == count_epilog_ignores());
        PMIX_INFO_DESTRUCT(&at[0]);
        PMIX_INFO_DESTRUCT(&at[1]);
        PMIX_INFO_DESTRUCT(&at[2]);
        PMIX_INFO_DESTRUCT(&at[3]);

        drain_epilog_dirs();
        drain_epilog_ignores();
    }

    /* --- the epilog must honor an ignore, and a comma-delimited list --- */
    {
        char tdir[] = "/tmp/pmix-ctlut-XXXXXX";
        char *base, pgone[256], pkeep[256];
        pmix_info_t at[2];
        FILE *fp;
        size_t nign;

        drain_epilog_ignores();
        drain_epilog_dirs();
        drain_epilog_files();

        /* PMIX_CLEANUP_IGNORE is documented as a comma-delimited list,
         * and the expansion used to happen only inside the epilog - which
         * compares an ignore against a filename it has constructed, so a
         * list of more than one path matched nothing at all */
        PMIX_INFO_LOAD(&at[0], PMIX_CLEANUP_IGNORE,
                       CTLUT_DIR "/one," CTLUT_DIR "/two", PMIX_STRING);
        rc = do_job_ctrl(at, 1);
        report("a comma-delimited ignore is accepted", PMIX_OPERATION_SUCCEEDED == rc);
        nign = count_epilog_ignores();
        report("a comma-delimited ignore registers one entry per path", 2 == nign);
        PMIX_INFO_DESTRUCT(&at[0]);
        drain_epilog_ignores();

        /* a value naming no path at all reached the epilog as an entry
         * whose own split returned NULL and was then indexed */
        PMIX_INFO_LOAD(&at[0], PMIX_REGISTER_CLEANUP, ",", PMIX_STRING);
        rc = do_job_ctrl(at, 1);
        report("a cleanup value naming no path is refused", PMIX_ERR_BAD_PARAM == rc);
        report("a cleanup value naming no path registered nothing",
               0 == count_epilog_files());
        PMIX_INFO_DESTRUCT(&at[0]);
        /* an unfixed library accepted it, and the entry it left behind
         * takes the epilog below down with a NULL dereference. Drain it
         * so this case reports as a failure rather than a signal */
        drain_epilog_files();

        /* and the whole point of an ignore: the epilog must not delete
         * the file. The ignores list was consulted only for the contents
         * of a cleanup *directory* - the cleanup_files loop unlinked
         * every path on it unconditionally, so an ignore naming a path
         * already registered for cleanup was recorded and then ignored */
        base = mkdtemp(tdir);
        if (NULL == base) {
            report("made a scratch directory for the epilog case", false);
        } else {
            snprintf(pgone, sizeof(pgone), "%s/gone", base);
            snprintf(pkeep, sizeof(pkeep), "%s/keep", base);
            fp = fopen(pgone, "w");
            if (NULL != fp) {
                fclose(fp);
            }
            fp = fopen(pkeep, "w");
            if (NULL != fp) {
                fclose(fp);
            }
            report("scratch files created",
                   0 == access(pgone, F_OK) && 0 == access(pkeep, F_OK));

            snprintf(cleanupval, sizeof(cleanupval), "%s,%s", pgone, pkeep);
            PMIX_INFO_LOAD(&at[0], PMIX_REGISTER_CLEANUP, cleanupval, PMIX_STRING);
            rc = do_job_ctrl(at, 1);
            report("both scratch files registered for cleanup",
                   PMIX_OPERATION_SUCCEEDED == rc && 2 == count_epilog_files());
            PMIX_INFO_DESTRUCT(&at[0]);

            PMIX_INFO_LOAD(&at[0], PMIX_CLEANUP_IGNORE, pkeep, PMIX_STRING);
            rc = do_job_ctrl(at, 1);
            report("ignore of one of them accepted", PMIX_OPERATION_SUCCEEDED == rc);
            PMIX_INFO_DESTRUCT(&at[0]);

            pmix_execute_epilog(&pmix_globals.mypeer->nptr->epilog);
            report("the epilog removed the file it was asked to remove",
                   0 != access(pgone, F_OK));
            report("the epilog kept the file it was asked to ignore",
                   0 == access(pkeep, F_OK));

            unlink(pgone);
            unlink(pkeep);
            rmdir(base);
        }
        drain_epilog_ignores();
        drain_epilog_files();
    }

    /* --- PMIX_CLEANUP_EMPTY, and its exclusion with RECURSIVE --------- */
    {
        char tdir[] = "/tmp/pmix-ctlut-e-XXXXXX";
        char tdir2[] = "/tmp/pmix-ctlut-r-XXXXXX";
        char tdir3[] = "/tmp/pmix-ctlut-p-XXXXXX";
        char *base;
        char demptyp[300], dfullp[300], dnestp[300], fkeepp[300];
        pmix_info_t at[3];
        FILE *fp;
        bool yes = true;
        pmix_cleanup_dir_t *rd;
        size_t nd;

        drain_epilog_ignores();
        drain_epilog_dirs();
        drain_epilog_files();

        /* the two modes say opposite things about what happens to a
         * file, so a request naming both has not said what it wants */
        PMIX_INFO_LOAD(&at[0], PMIX_REGISTER_CLEANUP_DIR, CTLUT_DIR, PMIX_STRING);
        PMIX_INFO_LOAD(&at[1], PMIX_CLEANUP_RECURSIVE, &yes, PMIX_BOOL);
        PMIX_INFO_LOAD(&at[2], PMIX_CLEANUP_EMPTY, &yes, PMIX_BOOL);
        rc = do_job_ctrl(at, 3);
        report("recursive plus empty is refused",
               PMIX_ERR_CONFLICTING_CLEANUP_DIRECTIVES == rc);
        only_epilog_dir(&nd);
        report("the refused mode combination registered nothing", 0 == nd);
        PMIX_INFO_DESTRUCT(&at[1]);
        PMIX_INFO_DESTRUCT(&at[2]);

        /* a request carrying nothing but cleanup directives is one we
         * answer ourselves - PMIX_CLEANUP_EMPTY was not counted as one
         * of them, so such a request went to the host as well */
        PMIX_INFO_LOAD(&at[1], PMIX_CLEANUP_EMPTY, &yes, PMIX_BOOL);
        rc = do_job_ctrl(at, 2);
        report("a cleanup request naming empty is answered locally",
               PMIX_OPERATION_SUCCEEDED == rc && !jobctrl_fired);
        rd = only_epilog_dir(&nd);
        report("the empty flag reached the registered entry",
               1 == nd && NULL != rd && rd->empty && !rd->recurse);
        PMIX_INFO_DESTRUCT(&at[0]);
        PMIX_INFO_DESTRUCT(&at[1]);
        drain_epilog_dirs();

        /* and what it actually does: remove the empty directories at any
         * depth, and leave every file exactly where it is */
        base = mkdtemp(tdir);
        if (NULL == base) {
            report("made a scratch tree for the empty-cleanup case", false);
        } else {
            snprintf(demptyp, sizeof(demptyp), "%s/empty", base);
            snprintf(dfullp, sizeof(dfullp), "%s/full", base);
            snprintf(dnestp, sizeof(dnestp), "%s/empty/deeper", base);
            snprintf(fkeepp, sizeof(fkeepp), "%s/full/keep", base);
            mkdir(dfullp, 0700);
            mkdir(demptyp, 0700);
            mkdir(dnestp, 0700);
            fp = fopen(fkeepp, "w");
            if (NULL != fp) {
                fclose(fp);
            }
            report("scratch tree built",
                   0 == access(dnestp, F_OK) && 0 == access(fkeepp, F_OK));

            PMIX_INFO_LOAD(&at[0], PMIX_REGISTER_CLEANUP_DIR, base, PMIX_STRING);
            PMIX_INFO_LOAD(&at[1], PMIX_CLEANUP_EMPTY, &yes, PMIX_BOOL);
            PMIX_INFO_LOAD(&at[2], PMIX_CLEANUP_LEAVE_TOPDIR, &yes, PMIX_BOOL);
            rc = do_job_ctrl(at, 3);
            report("the empty-cleanup directory registered",
                   PMIX_OPERATION_SUCCEEDED == rc);
            PMIX_INFO_DESTRUCT(&at[0]);
            PMIX_INFO_DESTRUCT(&at[1]);
            PMIX_INFO_DESTRUCT(&at[2]);

            pmix_execute_epilog(&pmix_globals.mypeer->nptr->epilog);
            report("an empty directory at depth was removed", 0 != access(dnestp, F_OK));
            report("the directory that held only empty ones went too",
                   0 != access(demptyp, F_OK));
            report("a directory holding a file was left alone", 0 == access(dfullp, F_OK));
            report("the file itself was left alone", 0 == access(fkeepp, F_OK));
            report("leave-topdir kept the top directory", 0 == access(base, F_OK));

            unlink(fkeepp);
            rmdir(dnestp);
            rmdir(demptyp);
            rmdir(dfullp);
            rmdir(base);
        }
        drain_epilog_dirs();

        /* the other mode, over the same shape of tree - it takes the
         * files as well as the directories, and this pins the branch the
         * empty mode was threaded through */
        base = mkdtemp(tdir2);
        if (NULL == base) {
            report("made a scratch tree for the recursive case", false);
        } else {
            snprintf(dfullp, sizeof(dfullp), "%s/full", base);
            snprintf(fkeepp, sizeof(fkeepp), "%s/full/gone", base);
            mkdir(dfullp, 0700);
            fp = fopen(fkeepp, "w");
            if (NULL != fp) {
                fclose(fp);
            }
            PMIX_INFO_LOAD(&at[0], PMIX_REGISTER_CLEANUP_DIR, base, PMIX_STRING);
            PMIX_INFO_LOAD(&at[1], PMIX_CLEANUP_RECURSIVE, &yes, PMIX_BOOL);
            rc = do_job_ctrl(at, 2);
            report("the recursive directory registered", PMIX_OPERATION_SUCCEEDED == rc);
            PMIX_INFO_DESTRUCT(&at[0]);
            PMIX_INFO_DESTRUCT(&at[1]);

            pmix_execute_epilog(&pmix_globals.mypeer->nptr->epilog);
            report("recursive cleanup removed the nested file", 0 != access(fkeepp, F_OK));
            report("recursive cleanup removed the directory holding it",
                   0 != access(dfullp, F_OK));
            report("recursive cleanup removed the top directory", 0 != access(base, F_OK));

            unlink(fkeepp);
            rmdir(dfullp);
            rmdir(base);
        }
        drain_epilog_dirs();
        drain_epilog_files();

        /* The privilege drop. We are unprivileged, so naming any other
         * uid is a drop we cannot make - and the right answer to that is
         * to remove nothing at all, rather than to remove it as
         * ourselves, which is the behavior pmix_execute_epilog exists to
         * stop. Point the epilog at a uid that is not ours and check the
         * file survives; then put the identity back and check the same
         * file goes, so the case cannot pass by simply never working. */
        base = mkdtemp(tdir3);
        if (NULL == base) {
            report("made a scratch file for the privilege case", false);
        } else {
            uid_t saveuid = pmix_globals.mypeer->nptr->epilog.uid;

            snprintf(fkeepp, sizeof(fkeepp), "%s/owned", base);
            fp = fopen(fkeepp, "w");
            if (NULL != fp) {
                fclose(fp);
            }
            PMIX_INFO_LOAD(&at[0], PMIX_REGISTER_CLEANUP, fkeepp, PMIX_STRING);
            rc = do_job_ctrl(at, 1);
            report("the privilege-case file registered", PMIX_OPERATION_SUCCEEDED == rc);
            PMIX_INFO_DESTRUCT(&at[0]);

            /* nobody's real uid, and certainly not ours */
            pmix_globals.mypeer->nptr->epilog.uid = saveuid + 1;
            pmix_execute_epilog(&pmix_globals.mypeer->nptr->epilog);
            report("a cleanup we cannot drop for removes nothing",
                   0 == access(fkeepp, F_OK));
            pmix_globals.mypeer->nptr->epilog.uid = saveuid;

            /* and the same request under our own identity does go through,
             * so the case above is not passing on a broken epilog */
            PMIX_INFO_LOAD(&at[0], PMIX_REGISTER_CLEANUP, fkeepp, PMIX_STRING);
            rc = do_job_ctrl(at, 1);
            report("the privilege-case file registered again",
                   PMIX_OPERATION_SUCCEEDED == rc);
            PMIX_INFO_DESTRUCT(&at[0]);
            pmix_execute_epilog(&pmix_globals.mypeer->nptr->epilog);
            report("a cleanup under our own identity does remove it",
                   0 != access(fkeepp, F_OK));

            unlink(fkeepp);
            rmdir(base);
        }
        drain_epilog_dirs();
        drain_epilog_ignores();
        drain_epilog_files();
    }

    /* --- a target count larger than the packed array ------------------ */
    {
        size_t before, after;

        before = count_empty_nspaces();
        rc = do_job_ctrl_targets(4, 1);
        /* the host is entitled to the count that actually arrived, not
         * the one the peer declared - the tail of that array is
         * default-constructed, not sent */
        report("over-declared request still reached the host", jobctrl_fired);
        report("host was handed the count that actually arrived",
               1 == jobctrl_ntargets);
        after = count_empty_nspaces();
        report("over-declared targets created no phantom namespace",
               before == after);

        /* a count that cannot survive the trip through int32_t must not
         * reach PMIX_PROC_CREATE at all */
        rc = do_job_ctrl_targets((size_t) 0x80000000UL, 1);
        report("job control rejects an unusable target count",
               PMIX_ERR_BAD_PARAM == rc);
        report("job control did not hand the host a bogus target count",
               !jobctrl_fired);
        report("rejected target count created no phantom namespace",
               before == count_empty_nspaces());
    }

    /* --- the host's abort up-call --------------------------------- */
    test_abort_upcall();

    PMIx_server_finalize();

    fprintf(stdout, "server_control: %d passed, %d failed\n", npass, nfail);
    return (0 == nfail) ? 0 : 1;
}
