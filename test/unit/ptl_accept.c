/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/*
 * How a server accepts a connection - docs/how-things-work/connections.rst.
 *
 * Each case runs a real server in a forked child and drives it with a
 * hand-built connect-ack on a raw socket, using psec/native over TCP.
 *
 * A refused peer is told why. Every refusal sends a status before the
 * connection closes - an unknown namespace or rank, a security module the
 * server does not have, a connect-ack cut short, the host saying no, a
 * tool needing an identity nobody can give. Each case reads the specific
 * status.
 *
 * The identity is settled before the host is asked: a client registered
 * as another user is refused, and the host is never asked; one registered
 * as itself connects, and the host is asked once.
 *
 * A handshake does not hold the server. A peer that starts one and goes
 * quiet - with the handshake deadline disabled - leaves the server
 * answering a blocking call of its own.
 *
 * Nothing shared changes before the connection is accepted. A client the
 * host refuses leaves its namespace's wire format, version and its rank's
 * pid as they were; a refused second connection for a rank leaves the
 * rank pointing at its live connection; and a tool joining a known
 * namespace, whose host answers with some other name, neither renames the
 * namespace nor duplicates a rank entry.
 *
 * What the host is given: the security module that settled the identity
 * (PMIX_CRED_TYPE) for a client and a tool, and the pid a client's library
 * sent. A uid or gid in a client's own info is ignored - the identity is
 * the one the module settled.
 *
 * A host that breaks the one-answer contract does not cost the server its
 * memory: a callback followed by an error return refuses the client with
 * the error, a callback made twice accepts it once, and a tool up-call
 * that "succeeds" without giving the tool an identity refuses the tool.
 *
 * The listener's own limits are refusals too, and send a status: a
 * connect-ack too large to accept, one that does not arrive in time, and
 * the oldest unfinished connection when the cap on them is reached, and
 * the oldest handshake when handshakes reach their share of that cap.
 *
 * What the host may change while it is asked is re-checked: a client
 * deregistered, or its job deregistered, during its handshake is refused
 * before the host hears of it; a tool whose rank the host deregisters
 * during the up-call is refused. Two connections for a rank both waiting on
 * the host: only the first is passed to the host, the second is refused.
 * A bad credential is refused as such even with a malformed info
 * blob behind it - the blob is not read until the credential is checked.
 * A tool that needs an identity is refused if the host accepts it without
 * one.
 *
 * A tool of the legacy kind only v2.1 sends is sent the replies a v2.1
 * reader takes: after its status, the namespace the host gave it, then
 * the server's namespace and rank, then the last status.
 *
 * And the bookkeeping: a connect-ack whose version is not a version is
 * refused as malformed, and an accepted tool counts as a live process on
 * its rank.
 *
 * A rank has one connection at a time: a second one, while the first is
 * connected and has not finalized, is refused with PMIX_ERR_EXISTS before
 * the host is asked - a client's or a tool's, whichever holds the rank. A
 * rank whose first connection was lost, or finalized, can connect again -
 * including before the server has read the first one's close: with the
 * server's reads of it held, the close is found by looking. A connection
 * that closes while its process runs on - as an exec closes it - is lost
 * at once. A second connection from the pid the rank's live one was
 * accepted with is refused like any other, and leaves the first as it
 * was. A tool on a client's rank that finalizes leaves the rank to the
 * next connection.
 *
 * The host decides on one tool at a time. With its answers held back:
 * of two tools of one name - self-named, or launched into a namespace
 * the host did not register - it is asked once, and the second is refused
 * once the first is accepted; a tool it refuses makes way for the next,
 * which it is then asked about; two tools of different names are put to
 * it one after the other; a waiting tool whose name the host gives
 * another tool is refused; and one that closes its connection while it
 * waits is never put to it.
 *
 * The host is asked to answer within ptl_base_handshake_wait_time
 * (PMIX_TIMEOUT), and the server waits a little longer. A client or tool
 * whose answer is late is refused with PMIX_ERR_TIMEOUT; its rank, or
 * name, is held until the answer comes, and a late yes is followed by a
 * PMIX_ERR_LOST_CONNECTION naming it, while a late no is followed by
 * nothing. A client or tool that goes while the host decides, which the
 * host then accepts, is reported the same way.
 */

#include "src/include/pmix_config.h"

#include "include/pmix_server.h"
#include "src/include/pmix_globals.h"
#include "src/mca/bfrops/base/base.h"
#include "src/mca/bfrops/bfrops_types.h"
#include "src/mca/psec/base/base.h"
#include "src/mca/ptl/base/base.h"
#include "src/mca/ptl/base/ptl_base_handshake.h"
#include "src/server/pmix_server_ops.h"
#include "src/threads/pmix_threads.h"
#include "src/util/pmix_environ.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <ftw.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <sys/un.h>

#define AC_NSPACE  "accept.ns"
#define AC_TOOLNS  "accept.tool"
#define AC_OTHERNS "accept.other"
#define AC_VERSION "7.0.0"
/* an earlier release that speaks the same wire format */
#define AC_TCP_VERSION "6.1.0"
#define AC_GDS     "hash"
#define AC_BFROPS  "v61"
#define AC_PID     424242

#define CHILD_PASS  0
#define CHILD_FAIL  1
#define CHILD_SETUP 2

static int npass = 0;
static int nfail = 0;
/* the version a connect-ack names */
static const char *ack_version = AC_VERSION;
/* the wire format a connect-ack names */
static const char *ack_bfrops = AC_BFROPS;
/* send a credential native refuses - a malformed trailer */
static bool ack_badcred = false;

static void report(const char *name, int passed, const char *detail)
{
    if (passed) {
        fprintf(stdout, "  PASS: %s\n", name);
        ++npass;
    } else {
        fprintf(stdout, "  FAIL: %s (%s)\n", name, detail);
        ++nfail;
    }
    fflush(stdout);
}

/* ---- the host ---- */

static volatile int nclient_connected = 0;
static volatile int nclient_finalized = 0;
static volatile int ntool_connected = 0;
/* refuse every client connection with this, if set */
static pmix_status_t client_refusal = PMIX_SUCCESS;
/* the identity a tool is given; a tool's own when this is empty */
static pmix_proc_t tool_answer;
/* what the host was last given */
static char host_credtype[64];
static pid_t host_pid = 0;
static uint32_t host_uid = (uint32_t) -1;
static uint32_t host_gid = (uint32_t) -1;
static int host_timeout = -1;
/* how many copies of each key the host was last given */
#define NCOUNTED  8
#define NSPACE_AT 4
static int host_nkeys[NCOUNTED];
static const char *const counted_keys[NCOUNTED] = {
    PMIX_USERID, PMIX_GRPID,        PMIX_PROC_PID, PMIX_CRED_TYPE,
    PMIX_NSPACE, PMIX_VERSION_INFO, PMIX_RANK,     PMIX_TIMEOUT};
/* whether the host was given a connection token */
static bool host_token = false;
static char host_nspace[PMIX_MAX_NSLEN + 1];

static void record_info(pmix_info_t info[], size_t ninfo)
{
    size_t n;
    int k;

    memset(host_nkeys, 0, sizeof(host_nkeys));
    host_nspace[0] = '\0';
    for (n = 0; n < ninfo; n++) {
        for (k = 0; k < NCOUNTED; k++) {
            if (PMIx_Check_key(info[n].key, counted_keys[k])) {
                ++host_nkeys[k];
            }
        }
        if (PMIx_Check_key(info[n].key, PMIX_NSPACE) && PMIX_STRING == info[n].value.type) {
            snprintf(host_nspace, sizeof(host_nspace), "%s", info[n].value.data.string);
        }
        if (PMIx_Check_key(info[n].key, PMIX_PTL_TOKEN_KEY)) {
            host_token = true;
        }
        if (PMIx_Check_key(info[n].key, PMIX_CRED_TYPE)) {
            snprintf(host_credtype, sizeof(host_credtype), "%s", info[n].value.data.string);
        } else if (PMIx_Check_key(info[n].key, PMIX_PROC_PID)) {
            host_pid = info[n].value.data.pid;
        } else if (PMIx_Check_key(info[n].key, PMIX_USERID)) {
            host_uid = info[n].value.data.uint32;
        } else if (PMIx_Check_key(info[n].key, PMIX_GRPID)) {
            host_gid = info[n].value.data.uint32;
        } else if (PMIx_Check_key(info[n].key, PMIX_TIMEOUT)) {
            host_timeout = info[n].value.data.integer;
        }
    }
}

/* the live connections of a name - on the progress thread */
static int count_live(const char *nspace, pmix_rank_t rank)
{
    pmix_peer_t *pr;
    int n, count = 0;

    for (n = 0; n < pmix_server_globals.clients.size; n++) {
        pr = (pmix_peer_t *) pmix_pointer_array_get_item(&pmix_server_globals.clients, n);
        if (NULL != pr && NULL != pr->info && !pr->finalized && 0 <= pr->sd &&
            pr->info->pname.rank == rank && PMIx_Check_nspace(pr->info->pname.nspace, nspace)) {
            ++count;
        }
    }
    return count;
}

/* the lost-connection events the server raised, and how many live
 * connections had the name the last one named when it was delivered */
static volatile int nlost = 0;
static pmix_proc_t lost_source;
static volatile int live_at_lost = -1;
static void lost_handler(size_t evhdlr_registration_id, pmix_status_t status,
                         const pmix_proc_t *source, pmix_info_t info[], size_t ninfo,
                         pmix_info_t results[], size_t nresults,
                         pmix_event_notification_cbfunc_fn_t cbfunc, void *cbdata)
{
    PMIX_HIDE_UNUSED_PARAMS(evhdlr_registration_id, status, info, ninfo, results, nresults);
    ++nlost;
    if (NULL != source) {
        lost_source = *source;
        live_at_lost = count_live(source->nspace, source->rank);
    }
    if (NULL != cbfunc) {
        cbfunc(PMIX_EVENT_ACTION_COMPLETE, NULL, 0, NULL, NULL, cbdata);
    }
}

/* how the host breaks the contract, if it does */
#define HOST_KEEPS_CONTRACT     0
#define HOST_CALLS_BACK_THEN_ERRS 1
#define HOST_CALLS_BACK_TWICE   2
#define HOST_SUCCEEDS_NO_ID     3
#define HOST_DEFERS             4
#define HOST_GIVES_NO_ID        5
#define HOST_DEFERS_TOOLS       6
static int host_mode = HOST_KEEPS_CONTRACT;
/* answers HOST_DEFERS holds back */
static pmix_op_cbfunc_t deferred_cb[4];
static void *deferred_cbdata[4];
static int ndeferred = 0;
/* how many answers HOST_DEFERS and HOST_DEFERS_TOOLS hold back: later ones
 * are given at once */
static int max_deferred = 4;
/* tool answers HOST_DEFERS_TOOLS holds back, with the identity each
 * would be given */
static pmix_tool_connection_cbfunc_t deferred_tool_cb[4];
static void *deferred_tool_cbdata[4];
static pmix_proc_t deferred_tool_id[4];
static int ndeferred_tools = 0;
/* run inside tool_connected2, before it answers */
static void (*tool_hook)(void) = NULL;
/* run inside client_connected2, which then accepts through the callback */
static void (*client_hook)(void) = NULL;

static pmix_status_t client_connected2(const pmix_proc_t *proc, void *server_object,
                                       pmix_info_t info[], size_t ninfo,
                                       pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    PMIX_HIDE_UNUSED_PARAMS(proc, server_object);

    ++nclient_connected;
    record_info(info, ninfo);
    if (NULL != client_hook) {
        client_hook();
        cbfunc(PMIX_SUCCESS, cbdata);
        return PMIX_SUCCESS;
    }
    if (HOST_CALLS_BACK_THEN_ERRS == host_mode) {
        cbfunc(PMIX_SUCCESS, cbdata);
        return PMIX_ERR_NO_PERMISSIONS;
    }
    if (HOST_CALLS_BACK_TWICE == host_mode) {
        cbfunc(PMIX_SUCCESS, cbdata);
        cbfunc(PMIX_SUCCESS, cbdata);
        return PMIX_SUCCESS;
    }
    if (HOST_DEFERS == host_mode && max_deferred > ndeferred) {
        deferred_cb[ndeferred] = cbfunc;
        deferred_cbdata[ndeferred] = cbdata;
        ++ndeferred;
        return PMIX_SUCCESS;
    }
    if (PMIX_SUCCESS != client_refusal) {
        return client_refusal;
    }
    return PMIX_OPERATION_SUCCEEDED;
}

static pmix_status_t client_finalized(const pmix_proc_t *proc, void *server_object,
                                      pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    PMIX_HIDE_UNUSED_PARAMS(proc, server_object, cbfunc, cbdata);

    ++nclient_finalized;
    return PMIX_OPERATION_SUCCEEDED;
}

static pmix_status_t tool_connected2(pmix_info_t *info, size_t ninfo,
                                     pmix_tool_connection_cbfunc_t cbfunc, void *cbdata)
{
    pmix_proc_t proc;
    size_t n;

    ++ntool_connected;
    record_info(info, ninfo);
    if (HOST_SUCCEEDS_NO_ID == host_mode) {
        return PMIX_OPERATION_SUCCEEDED;
    }
    if (NULL != tool_hook) {
        tool_hook();
    }
    if (HOST_GIVES_NO_ID == host_mode) {
        cbfunc(PMIX_SUCCESS, NULL, cbdata);
        return PMIX_SUCCESS;
    }
    PMIX_LOAD_PROCID(&proc, "accept.assigned", 0);
    for (n = 0; n < ninfo; n++) {
        if (PMIx_Check_key(info[n].key, PMIX_NSPACE)) {
            PMIX_LOAD_NSPACE(proc.nspace, info[n].value.data.string);
        } else if (PMIx_Check_key(info[n].key, PMIX_RANK)) {
            proc.rank = info[n].value.data.rank;
        }
    }
    if ('\0' != tool_answer.nspace[0]) {
        proc = tool_answer;
    }
    if (HOST_DEFERS_TOOLS == host_mode && max_deferred > ndeferred_tools) {
        deferred_tool_cb[ndeferred_tools] = cbfunc;
        deferred_tool_cbdata[ndeferred_tools] = cbdata;
        deferred_tool_id[ndeferred_tools] = proc;
        ++ndeferred_tools;
        return PMIX_SUCCESS;
    }
    cbfunc(PMIX_SUCCESS, &proc, cbdata);
    return PMIX_SUCCESS;
}

static pmix_server_module_t mymodule = {
    .client_connected2 = client_connected2,
    .client_finalized = client_finalized,
    .tool_connected2 = tool_connected2
};

/* the finalize answer held back, for a host that holds it */
static pmix_op_cbfunc_t finalize_cb = NULL;
static void *finalize_cbdata = NULL;
static pmix_status_t client_finalized_deferred(const pmix_proc_t *proc, void *server_object,
                                               pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    PMIX_HIDE_UNUSED_PARAMS(proc, server_object);

    ++nclient_finalized;
    finalize_cb = cbfunc;
    finalize_cbdata = cbdata;
    return PMIX_SUCCESS;
}

static pmix_server_module_t finmodule = {
    .client_connected2 = client_connected2,
    .client_finalized = client_finalized_deferred,
    .tool_connected2 = tool_connected2
};

static pmix_server_module_t notoolmodule = {
    .client_connected2 = client_connected2
};

/* ---- the peer ---- */

/* The token start_server's registered rank was prepared with, which every
 * connection naming that rank presents - unless a case says otherwise */
static char *ac_token = NULL;

/* whether start_server leaves its registered rank unprepared */
static bool ac_unprepared = false;

static bool names_ac_rank(const char *nspace, pmix_rank_t rank)
{
    return NULL != ac_token && 0 == strcmp(nspace, AC_NSPACE) && 0 == rank;
}

/* an info blob holding `pid`, and a uid and gid not our own, in the wire
 * format we use */
static bool build_blob_pid(pmix_byte_object_t *out, pid_t pid)
{
    pmix_bfrops_module_t *mod = pmix_bfrops_base_assign_module(AC_BFROPS);
    pmix_buffer_t buf;
    pmix_info_t info[4];
    uint32_t other = (uint32_t) getuid() + 4242;
    size_t n = 3, k;
    pmix_status_t rc;

    if (NULL == mod) {
        return false;
    }
    PMIX_CONSTRUCT(&buf, pmix_buffer_t);
    buf.type = PMIX_BFROP_BUFFER_NON_DESC;
    PMIX_INFO_LOAD(&info[0], PMIX_PROC_PID, &pid, PMIX_PID);
    PMIX_INFO_LOAD(&info[1], PMIX_USERID, &other, PMIX_UINT32);
    PMIX_INFO_LOAD(&info[2], PMIX_GRPID, &other, PMIX_UINT32);
    if (NULL != ac_token) {
        PMIX_INFO_LOAD(&info[3], PMIX_PTL_TOKEN_KEY, ac_token, PMIX_STRING);
        n = 4;
    }
    k = n;
    rc = mod->pack(&buf, &n, 1, PMIX_SIZE);
    if (PMIX_SUCCESS == rc) {
        rc = mod->pack(&buf, info, (int32_t) k, PMIX_INFO);
    }
    for (n = 0; n < k; n++) {
        PMIX_INFO_DESTRUCT(&info[n]);
    }
    if (PMIX_SUCCESS != rc) {
        PMIX_DESTRUCT(&buf);
        return false;
    }
    out->size = buf.bytes_used;
    out->bytes = (char *) malloc(out->size);
    memcpy(out->bytes, buf.base_ptr, out->size);
    PMIX_DESTRUCT(&buf);
    return true;
}

static bool build_blob(pmix_byte_object_t *out)
{
    return build_blob_pid(out, AC_PID);
}

/* an info blob holding a connection token, in the wire format we use */
static bool build_blob_token(pmix_byte_object_t *out, const char *token)
{
    pmix_bfrops_module_t *mod = pmix_bfrops_base_assign_module(AC_BFROPS);
    pmix_buffer_t buf;
    pmix_info_t info;
    size_t n = 1;
    pmix_status_t rc;

    if (NULL == mod) {
        return false;
    }
    PMIX_CONSTRUCT(&buf, pmix_buffer_t);
    buf.type = PMIX_BFROP_BUFFER_NON_DESC;
    PMIX_INFO_LOAD(&info, PMIX_PTL_TOKEN_KEY, token, PMIX_STRING);
    rc = mod->pack(&buf, &n, 1, PMIX_SIZE);
    if (PMIX_SUCCESS == rc) {
        rc = mod->pack(&buf, &info, 1, PMIX_INFO);
    }
    PMIX_INFO_DESTRUCT(&info);
    if (PMIX_SUCCESS != rc) {
        PMIX_DESTRUCT(&buf);
        return false;
    }
    out->size = buf.bytes_used;
    out->bytes = (char *) malloc(out->size);
    memcpy(out->bytes, buf.base_ptr, out->size);
    PMIX_DESTRUCT(&buf);
    return true;
}

/* A tool's info naming, twice each, the identity the server sets itself -
 * with values that are not the tool's. The first pid is not a pid at all,
 * and the two that are differ */
#define NDUPS 17
static bool build_blob_dups(pmix_byte_object_t *out)
{
    pmix_bfrops_module_t *mod = pmix_bfrops_base_assign_module(AC_BFROPS);
    pmix_buffer_t buf;
    pmix_info_t info[NDUPS];
    uint32_t zero = 0, other = (uint32_t) getgid() + 4242;
    pmix_rank_t rank = 7;
    int tmo = 4242;
    pid_t pid;
    size_t n = NDUPS, k;
    pmix_status_t rc;

    if (NULL == mod) {
        return false;
    }
    PMIX_CONSTRUCT(&buf, pmix_buffer_t);
    buf.type = PMIX_BFROP_BUFFER_NON_DESC;
    PMIX_INFO_LOAD(&info[0], PMIX_PROC_PID, "not a pid", PMIX_STRING);
    for (k = 1; k < NDUPS; k += 8) {
        pid = (1 == k) ? AC_PID : AC_PID + 1;
        PMIX_INFO_LOAD(&info[k], PMIX_USERID, &zero, PMIX_UINT32);
        PMIX_INFO_LOAD(&info[k + 1], PMIX_GRPID, &other, PMIX_UINT32);
        PMIX_INFO_LOAD(&info[k + 2], PMIX_PROC_PID, &pid, PMIX_PID);
        PMIX_INFO_LOAD(&info[k + 3], PMIX_CRED_TYPE, "other", PMIX_STRING);
        PMIX_INFO_LOAD(&info[k + 4], PMIX_NSPACE, "other.ns", PMIX_STRING);
        PMIX_INFO_LOAD(&info[k + 5], PMIX_VERSION_INFO, "1.0.0", PMIX_STRING);
        PMIX_INFO_LOAD(&info[k + 6], PMIX_RANK, &rank, PMIX_PROC_RANK);
        PMIX_INFO_LOAD(&info[k + 7], PMIX_TIMEOUT, &tmo, PMIX_INT);
    }
    rc = mod->pack(&buf, &n, 1, PMIX_SIZE);
    if (PMIX_SUCCESS == rc) {
        rc = mod->pack(&buf, info, NDUPS, PMIX_INFO);
    }
    for (k = 0; k < NDUPS; k++) {
        PMIX_INFO_DESTRUCT(&info[k]);
    }
    if (PMIX_SUCCESS != rc) {
        PMIX_DESTRUCT(&buf);
        return false;
    }
    out->size = buf.bytes_used;
    out->bytes = (char *) malloc(out->size);
    memcpy(out->bytes, buf.base_ptr, out->size);
    PMIX_DESTRUCT(&buf);
    return true;
}

/* The connect-ack a client (PMIX_SIMPLE_CLIENT), or a tool that brings
 * its own identity (PMIX_TOOL_GIVEN_ID), needs one (PMIX_TOOL_NEEDS_ID) or
 * was launched by a server (PMIX_TOOL_CLIENT), sends. The credential is
 * native's: the effective uid and gid - or, with ack_badcred, a credential
 * too short to hold them, which native refuses as malformed. With
 * `truncate`, the payload stops after the security module's name */
/* a uid for native's credential to claim in place of our own */
static uid_t ack_cred_uid = 0;

static char *build_ack_as_given(uint8_t flag, const char *nspace, pmix_rank_t rank,
                                const char *psec, const pmix_byte_object_t *blob, bool truncate,
                                size_t *sz)
{
    pmix_ptl_hdr_t hdr;
    pmix_proc_t proc;
    char credbytes[sizeof(uid_t) + sizeof(gid_t)];
    size_t credlen = ack_badcred ? sizeof(uid_t) - 1 : sizeof(credbytes);
    uid_t euid = (0 != ack_cred_uid) ? ack_cred_uid : geteuid();
    gid_t egid = getegid();
    char *msg;
    size_t csize, payload;
    uint8_t bftype = PMIX_BFROP_BUFFER_NON_DESC;

    memcpy(credbytes, &euid, sizeof(uid_t));
    memcpy(credbytes + sizeof(uid_t), &egid, sizeof(gid_t));

    PMIX_LOAD_PROCID(&proc, nspace, rank);
    payload = strlen(psec) + 1;
    if (!truncate) {
        payload += sizeof(uint32_t) + credlen + 1;
        if (PMIX_SIMPLE_CLIENT != flag) {
            payload += 2 * sizeof(uint32_t);
        }
        if (PMIX_TOOL_NEEDS_ID != flag) {
            payload += strlen(proc.nspace) + 1 + sizeof(uint32_t);
        }
        payload += strlen(ack_version) + 1 + strlen(ack_bfrops) + 1 + 1 + strlen(AC_GDS) + 1;
        if (NULL != blob) {
            payload += blob->size;
        }
    }

    memset(&hdr, 0, sizeof(hdr));
    hdr.pindex = -1;
    hdr.tag = UINT32_MAX;
    hdr.nbytes = (uint32_t) payload;

    *sz = sizeof(hdr) + payload;
    msg = (char *) calloc(1, *sz);
    memcpy(msg, &hdr, sizeof(hdr));
    csize = sizeof(hdr);

    PMIX_PTL_PUT_STRING(psec);
    if (truncate) {
        return msg;
    }
    PMIX_PTL_PUT_U32(credlen);
    PMIX_PTL_PUT_BLOB(credbytes, credlen);
    PMIX_PTL_PUT_U8(flag);
    if (PMIX_SIMPLE_CLIENT != flag) {
        PMIX_PTL_PUT_U32(getuid());
        PMIX_PTL_PUT_U32(getgid());
    }
    if (PMIX_TOOL_NEEDS_ID != flag) {
        PMIX_PTL_PUT_PROCID(proc);
    }
    PMIX_PTL_PUT_STRING(ack_version);
    PMIX_PTL_PUT_STRING(ack_bfrops);
    PMIX_PTL_PUT_U8(bftype);
    PMIX_PTL_PUT_STRING(AC_GDS);
    if (NULL != blob) {
        PMIX_PTL_PUT_BLOB(blob->bytes, blob->size);
    }
    return msg;
}

/* The same, with the registered rank's token for an ack naming it that
 * carries no blob of its own */
static char *build_ack(uint8_t flag, const char *nspace, pmix_rank_t rank, const char *psec,
                       const pmix_byte_object_t *blob, bool truncate, size_t *sz)
{
    pmix_byte_object_t tblob = {NULL, 0};
    char *msg;

    if (NULL == blob && !truncate && PMIX_TOOL_NEEDS_ID != flag && names_ac_rank(nspace, rank) &&
        build_blob_token(&tblob, ac_token)) {
        blob = &tblob;
    }
    msg = build_ack_as_given(flag, nspace, rank, psec, blob, truncate, sz);
    free(tblob.bytes);
    return msg;
}

static bool read_u32(int sd, uint32_t *u32)
{
    size_t got = 0;
    ssize_t rd;

    while (got < sizeof(*u32)) {
        rd = read(sd, (char *) u32 + got, sizeof(*u32) - got);
        if (0 >= rd) {
            return false;
        }
        got += (size_t) rd;
    }
    *u32 = ntohl(*u32);
    return true;
}

/* connect to the server, leaving the socket in *sdp */
/* connect to the server's local listener rather than its TCP one */
static bool ack_local = false;

static bool connect_local(int *sdp)
{
    struct sockaddr_un sa;
    struct timeval tv = {10, 0};
    char *p;
    int sd;

    if (NULL == pmix_ptl_base.local_listener.uri ||
        NULL == (p = strstr(pmix_ptl_base.local_listener.uri, "usock://"))) {
        return false;
    }
    p += strlen("usock://");
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", p);
    sd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (0 > sd || 0 != connect(sd, (struct sockaddr *) &sa, sizeof(sa))) {
        return false;
    }
    setsockopt(sd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    *sdp = sd;
    return true;
}

static bool connect_server(int *sdp)
{
    struct sockaddr_in sa;
    struct timeval tv = {10, 0};
    char *p;
    int sd;

    if (ack_local) {
        return connect_local(sdp);
    }
    if (NULL == pmix_ptl_base.listener.uri ||
        NULL == (p = strrchr(pmix_ptl_base.listener.uri, ':'))) {
        return false;
    }
    sd = socket(AF_INET, SOCK_STREAM, 0);
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(atoi(p + 1));
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (0 > sd || 0 != connect(sd, (struct sockaddr *) &sa, sizeof(sa))) {
        return false;
    }
    setsockopt(sd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    *sdp = sd;
    return true;
}

/* connect and send the ack, leaving the socket in *sdp */
static bool open_ack(char *msg, size_t sz, int *sdp)
{
    return connect_server(sdp) && (ssize_t) sz == write(*sdp, msg, sz);
}

/* connect, send the ack, and return the first reply: PMIX_ERR_UNREACH if
 * the connection was closed without one */
static pmix_status_t send_ack(char *msg, size_t sz, int *sdp)
{
    uint32_t reply;

    if (!open_ack(msg, sz, sdp)) {
        return PMIX_ERROR;
    }
    if (!read_u32(*sdp, &reply)) {
        return PMIX_ERR_UNREACH;
    }
    return (pmix_status_t) reply;
}

/* the rest of what a tool that brought its own identity reads after its
 * first status: the server's nspace and rank, then the final status */
static pmix_status_t tool_replies(int sd)
{
    char nspace[PMIX_MAX_NSLEN + 1];
    size_t got = 0;
    ssize_t rd;
    uint32_t u32;

    while (got < sizeof(nspace)) {
        rd = read(sd, nspace + got, sizeof(nspace) - got);
        if (0 >= rd) {
            return PMIX_ERR_UNREACH;
        }
        got += (size_t) rd;
    }
    if (!read_u32(sd, &u32) || !read_u32(sd, &u32)) {
        return PMIX_ERR_UNREACH;
    }
    return (pmix_status_t) u32;
}

/* Send PMIX_FINALIZE_CMD on an accepted client connection, as the client
 * library does */
static bool send_finalize_request(int sd, uint32_t index)
{
    pmix_bfrops_module_t *mod = pmix_bfrops_base_assign_module(AC_BFROPS);
    pmix_buffer_t buf;
    pmix_cmd_t cmd = PMIX_FINALIZE_CMD;
    pmix_ptl_hdr_t hdr;
    bool ok;

    PMIX_CONSTRUCT(&buf, pmix_buffer_t);
    buf.type = PMIX_BFROP_BUFFER_NON_DESC;
    if (NULL == mod || PMIX_SUCCESS != mod->pack(&buf, &cmd, 1, PMIX_COMMAND)) {
        PMIX_DESTRUCT(&buf);
        return false;
    }
    memset(&hdr, 0, sizeof(hdr));
    hdr.pindex = htonl(index);
    hdr.tag = htonl(PMIX_PTL_TAG_DYNAMIC + 1);
    hdr.nbytes = htonl((uint32_t) buf.bytes_used);
    ok = (ssize_t) sizeof(hdr) == write(sd, &hdr, sizeof(hdr)) &&
         (ssize_t) buf.bytes_used == write(sd, buf.base_ptr, buf.bytes_used);
    PMIX_DESTRUCT(&buf);
    return ok;
}

/* ... and wait for the server's reply */
static bool send_finalize(int sd, uint32_t index)
{
    pmix_ptl_hdr_t hdr;
    char *reply;
    size_t got;
    ssize_t rd;

    if (!send_finalize_request(sd, index)) {
        return false;
    }
    /* the reply: a header, and what it says follows */
    for (got = 0; got < sizeof(hdr); got += (size_t) rd) {
        rd = read(sd, (char *) &hdr + got, sizeof(hdr) - got);
        if (0 >= rd) {
            return false;
        }
    }
    hdr.nbytes = ntohl(hdr.nbytes);
    reply = (char *) malloc(hdr.nbytes + 1);
    for (got = 0; got < hdr.nbytes; got += (size_t) rd) {
        rd = read(sd, reply + got, hdr.nbytes - got);
        if (0 >= rd) {
            free(reply);
            return false;
        }
    }
    free(reply);
    return true;
}

/* ---- the server ---- */

static pmix_namespace_t *find_ns(const char *name)
{
    pmix_namespace_t *ns;

    PMIX_LIST_FOREACH (ns, &pmix_globals.nspaces, pmix_namespace_t) {
        if (NULL != ns->nspace && 0 == strcmp(ns->nspace, name)) {
            return ns;
        }
    }
    return NULL;
}

/* start the server without tool support */
static bool no_tool_support = false;

static pmix_status_t start_server(pmix_server_module_t *module, bool client, uid_t uid)
{
    pmix_status_t rc;
    pmix_proc_t proc;
    pmix_nspace_t ns;
    char **env = NULL, *token;

    {
        /* tools connect here, which a server takes only with tool support -
         * unless a case asks for a server without it */
        pmix_info_t tsinfo;
        bool tsflag = !no_tool_support;

        PMIX_INFO_LOAD(&tsinfo, PMIX_SERVER_TOOL_SUPPORT, &tsflag, PMIX_BOOL);
        rc = PMIx_server_init(module, &tsinfo, 1);
        PMIX_INFO_DESTRUCT(&tsinfo);
    }
    if (PMIX_SUCCESS != rc || !client) {
        return rc;
    }
    PMIX_LOAD_NSPACE(ns, AC_NSPACE);
    rc = PMIx_server_register_nspace(ns, 1, NULL, 0, NULL, NULL);
    if (PMIX_SUCCESS != rc && PMIX_OPERATION_SUCCEEDED != rc) {
        return rc;
    }
    PMIX_LOAD_PROCID(&proc, AC_NSPACE, 0);
    rc = PMIx_server_register_client(&proc, uid, getgid(), NULL, NULL, NULL);
    if (PMIX_OPERATION_SUCCEEDED == rc) {
        rc = PMIX_SUCCESS;
    }
    if (PMIX_SUCCESS != rc || ac_unprepared) {
        return rc;
    }
    /* and prepared, as a host does before it starts the process */
    rc = PMIx_server_setup_fork(&proc, &env);
    if (PMIX_SUCCESS == rc) {
        token = pmix_getenv(PMIX_PTL_TOKEN_ENV, env);
        if (NULL == token || NULL == strrchr(token, ';')) {
            rc = PMIX_ERROR;
        } else {
            ac_token = strdup(strrchr(token, ';') + 1);
        }
    }
    PMIx_Argv_free(env);
    return rc;
}

/* ---- the cases, each in a server of its own ---- */

static int expect_status(const char *what, pmix_status_t got, pmix_status_t want)
{
    if (got != want) {
        fprintf(stdout, "    %s: read %s, expected %s\n", what, PMIx_Error_string(got),
                PMIx_Error_string(want));
        return CHILD_FAIL;
    }
    return CHILD_PASS;
}

static int case_unknown_nspace(void)
{
    char *msg;
    size_t sz;
    int sd = -1;
    pmix_status_t reply;

    if (PMIX_SUCCESS != start_server(&mymodule, false, getuid())) {
        return CHILD_SETUP;
    }
    msg = build_ack(PMIX_SIMPLE_CLIENT, AC_NSPACE, 0, "native", NULL, false, &sz);
    reply = send_ack(msg, sz, &sd);
    free(msg);
    return expect_status("unregistered namespace", reply, PMIX_ERR_NOT_FOUND);
}

static int case_unknown_rank(void)
{
    char *msg;
    size_t sz;
    int sd = -1;
    pmix_status_t reply;

    if (PMIX_SUCCESS != start_server(&mymodule, true, getuid())) {
        return CHILD_SETUP;
    }
    msg = build_ack(PMIX_SIMPLE_CLIENT, AC_NSPACE, 7, "native", NULL, false, &sz);
    reply = send_ack(msg, sz, &sd);
    free(msg);
    return expect_status("unregistered rank", reply, PMIX_ERR_NOT_FOUND);
}

static int case_unknown_psec(bool tool)
{
    char *msg;
    size_t sz;
    int sd = -1;
    pmix_status_t reply;

    if (PMIX_SUCCESS != start_server(&mymodule, true, getuid())) {
        return CHILD_SETUP;
    }
    msg = build_ack(tool ? PMIX_TOOL_NEEDS_ID : PMIX_SIMPLE_CLIENT, AC_NSPACE, 0, "nosuchsec",
                    NULL, false, &sz);
    reply = send_ack(msg, sz, &sd);
    free(msg);
    return expect_status("unavailable security module", reply, PMIX_ERR_NOT_SUPPORTED);
}

static int case_truncated(void)
{
    char *msg;
    size_t sz;
    int sd = -1;
    pmix_status_t reply;

    if (PMIX_SUCCESS != start_server(&mymodule, true, getuid())) {
        return CHILD_SETUP;
    }
    msg = build_ack(PMIX_SIMPLE_CLIENT, AC_NSPACE, 0, "native", NULL, true, &sz);
    reply = send_ack(msg, sz, &sd);
    free(msg);
    return expect_status("connect-ack cut short", reply, PMIX_ERR_BAD_PARAM);
}

static int case_host_refuses(void)
{
    char *msg;
    size_t sz;
    int sd = -1;
    pmix_status_t reply;

    client_refusal = PMIX_ERR_NO_PERMISSIONS;
    if (PMIX_SUCCESS != start_server(&mymodule, true, getuid())) {
        return CHILD_SETUP;
    }
    msg = build_ack(PMIX_SIMPLE_CLIENT, AC_NSPACE, 0, "native", NULL, false, &sz);
    reply = send_ack(msg, sz, &sd);
    free(msg);
    return expect_status("the host refused", reply, PMIX_ERR_NO_PERMISSIONS);
}

static int case_tool_no_upcall(void)
{
    char *msg;
    size_t sz;
    int sd = -1;
    pmix_status_t reply;

    if (PMIX_SUCCESS != start_server(&notoolmodule, false, getuid())) {
        return CHILD_SETUP;
    }
    msg = build_ack(PMIX_TOOL_NEEDS_ID, "", 0, "native", NULL, false, &sz);
    reply = send_ack(msg, sz, &sd);
    free(msg);
    return expect_status("no host to give a tool an identity", reply, PMIX_ERR_NOT_SUPPORTED);
}

/* a client the host refuses leaves its namespace and rank as they were */
static int case_refused_changes_nothing(void)
{
    pmix_byte_object_t blob;
    pmix_namespace_t *ns;
    pmix_rank_info_t *info;
    char *msg;
    size_t sz;
    int sd = -1;
    pmix_status_t reply;

    client_refusal = PMIX_ERR_NO_PERMISSIONS;
    if (PMIX_SUCCESS != start_server(&mymodule, true, getuid()) || !build_blob(&blob)) {
        return CHILD_SETUP;
    }
    msg = build_ack(PMIX_SIMPLE_CLIENT, AC_NSPACE, 0, "native", &blob, false, &sz);
    reply = send_ack(msg, sz, &sd);
    free(msg);
    free(blob.bytes);
    usleep(200000);
    if (PMIX_ERR_NO_PERMISSIONS != reply || 1 != nclient_connected) {
        fprintf(stdout, "    the host was not the one to refuse: %s\n", PMIx_Error_string(reply));
        return CHILD_FAIL;
    }
    ns = find_ns(AC_NSPACE);
    if (NULL == ns) {
        return CHILD_SETUP;
    }
    info = (pmix_rank_info_t *) pmix_list_get_first(&ns->ranks);
    if (NULL != ns->compat.bfrops || NULL != ns->compat.gds || NULL != ns->compat.psec ||
        0 != ns->version.major || ns->version_stored) {
        fprintf(stdout, "    the namespace took the refused peer's settings\n");
        return CHILD_FAIL;
    }
    if (AC_PID == info->pid || 0 != info->proc_cnt || -1 != info->peerid) {
        fprintf(stdout, "    the rank took the refused peer's details\n");
        return CHILD_FAIL;
    }
    return CHILD_PASS;
}

/* A refused second connection leaves the rank on its live one. It used
 * to reset the rank's peer to -1. A second connection is now refused
 * before the host is asked */
static int case_refused_second(void)
{
    pmix_namespace_t *ns;
    pmix_rank_info_t *info;
    char *msg;
    size_t sz;
    int sd1 = -1, sd2 = -1, live;
    uint32_t index;
    pmix_status_t reply;

    if (PMIX_SUCCESS != start_server(&mymodule, true, getuid())) {
        return CHILD_SETUP;
    }
    msg = build_ack(PMIX_SIMPLE_CLIENT, AC_NSPACE, 0, "native", NULL, false, &sz);
    reply = send_ack(msg, sz, &sd1);
    if (PMIX_SUCCESS != reply || !read_u32(sd1, &index)) {
        free(msg);
        fprintf(stdout, "    the first connection was refused: %s\n", PMIx_Error_string(reply));
        return CHILD_FAIL;
    }
    live = (int) index;
    reply = send_ack(msg, sz, &sd2);
    free(msg);
    usleep(200000);
    if (PMIX_SUCCESS == reply) {
        fprintf(stdout, "    the second connection was accepted\n");
        return CHILD_FAIL;
    }
    if (1 != nclient_connected) {
        fprintf(stdout, "    the host was asked about a second connection\n");
        return CHILD_FAIL;
    }
    ns = find_ns(AC_NSPACE);
    if (NULL == ns) {
        return CHILD_SETUP;
    }
    info = (pmix_rank_info_t *) pmix_list_get_first(&ns->ranks);
    if (live != info->peerid || 1 != info->proc_cnt) {
        fprintf(stdout, "    the rank points at %d (count %d) - its live connection is %d\n",
                info->peerid, info->proc_cnt, live);
        return CHILD_FAIL;
    }
    return CHILD_PASS;
}

/* a tool joining a known namespace, whose host names it otherwise,
 * neither renames the namespace nor duplicates a rank */
static int case_tool_joins(void)
{
    pmix_namespace_t *ns;
    pmix_rank_info_t *info;
    char *msg;
    size_t sz;
    int sd1 = -1, sd2 = -1, n0 = 0, n1 = 0, nother = 0;
    pmix_status_t reply;

    if (PMIX_SUCCESS != start_server(&mymodule, false, getuid())) {
        return CHILD_SETUP;
    }
    msg = build_ack(PMIX_TOOL_GIVEN_ID, AC_TOOLNS, 0, "native", NULL, false, &sz);
    reply = send_ack(msg, sz, &sd1);
    free(msg);
    if (PMIX_SUCCESS != reply || PMIX_SUCCESS != (reply = tool_replies(sd1))) {
        fprintf(stdout, "    the first tool was refused: %s\n", PMIx_Error_string(reply));
        return CHILD_FAIL;
    }
    PMIX_LOAD_PROCID(&tool_answer, AC_OTHERNS, 9);
    msg = build_ack(PMIX_TOOL_GIVEN_ID, AC_TOOLNS, 1, "native", NULL, false, &sz);
    reply = send_ack(msg, sz, &sd2);
    free(msg);
    if (PMIX_SUCCESS != reply || PMIX_SUCCESS != (reply = tool_replies(sd2))) {
        fprintf(stdout, "    the second tool was refused: %s\n", PMIx_Error_string(reply));
        return CHILD_FAIL;
    }
    usleep(200000);
    ns = find_ns(AC_TOOLNS);
    if (NULL == ns) {
        fprintf(stdout, "    the tools' namespace is gone - renamed?\n");
        return CHILD_FAIL;
    }
    PMIX_LIST_FOREACH (info, &ns->ranks, pmix_rank_info_t) {
        if (0 == info->pname.rank) {
            ++n0;
        } else if (1 == info->pname.rank) {
            ++n1;
        } else {
            ++nother;
        }
    }
    if (1 != n0 || 1 != n1 || 0 != nother || NULL != find_ns(AC_OTHERNS)) {
        fprintf(stdout, "    rank entries: %d for rank 0, %d for rank 1, %d others%s\n", n0, n1,
                nother, (NULL != find_ns(AC_OTHERNS)) ? "; the host's name was used" : "");
        return CHILD_FAIL;
    }
    return CHILD_PASS;
}

/* A tool joining a rank that already has a connection. `how`: 0, a
 * second tool naming the rank a live tool holds; 1, a tool the host
 * launched naming a client rank that is connected; 2, a tool the host
 * launched taking its registered rank first, and then a client for the
 * same rank; 3, as 1, with the client using an older wire format than
 * the tool - the tool is told its rank is taken, not that its format
 * does not fit. The second connection is refused each time, before the
 * host is asked about it */
static int case_tool_second(int how)
{
    char *msg;
    size_t sz;
    int sd1 = -1, sd2 = -1;
    uint32_t index;
    pmix_status_t reply;
    int asked;

    if (PMIX_SUCCESS != start_server(&mymodule, 0 != how, getuid())) {
        return CHILD_SETUP;
    }
    if (0 == how) {
        msg = build_ack(PMIX_TOOL_GIVEN_ID, AC_TOOLNS, 0, "native", NULL, false, &sz);
    } else if (1 == how || 3 == how) {
        if (3 == how) {
            ack_bfrops = "v51";
        }
        msg = build_ack(PMIX_SIMPLE_CLIENT, AC_NSPACE, 0, "native", NULL, false, &sz);
        ack_bfrops = AC_BFROPS;
    } else {
        msg = build_ack(PMIX_TOOL_CLIENT, AC_NSPACE, 0, "native", NULL, false, &sz);
    }
    reply = send_ack(msg, sz, &sd1);
    free(msg);
    if (PMIX_SUCCESS == reply) {
        reply = (1 == how || 3 == how) ? (read_u32(sd1, &index) ? PMIX_SUCCESS : PMIX_ERR_UNREACH)
                                       : tool_replies(sd1);
    }
    if (PMIX_SUCCESS != reply) {
        fprintf(stdout, "    the first connection was refused: %s\n", PMIx_Error_string(reply));
        return CHILD_FAIL;
    }
    asked = ntool_connected + nclient_connected;
    if (0 == how) {
        msg = build_ack(PMIX_TOOL_GIVEN_ID, AC_TOOLNS, 0, "native", NULL, false, &sz);
    } else if (1 == how || 3 == how) {
        msg = build_ack(PMIX_TOOL_CLIENT, AC_NSPACE, 0, "native", NULL, false, &sz);
    } else {
        msg = build_ack(PMIX_SIMPLE_CLIENT, AC_NSPACE, 0, "native", NULL, false, &sz);
    }
    reply = send_ack(msg, sz, &sd2);
    free(msg);
    if (asked != ntool_connected + nclient_connected) {
        fprintf(stdout, "    the host was asked about the second connection\n");
        return CHILD_FAIL;
    }
    return expect_status("the second connection", reply, PMIX_ERR_EXISTS);
}
static int run_tool_second_tool(void)
{
    return case_tool_second(0);
}
static int run_tool_second_client(void)
{
    return case_tool_second(1);
}
static int run_client_second_tool(void)
{
    return case_tool_second(2);
}
static int run_tool_second_client_format(void)
{
    return case_tool_second(3);
}

/* A client or tool on the local transport is the kernel's uid, gid and pid
 * - not the uid, gid and pid its connect request names, nor the pid its
 * library sent */
static int case_local_identity(bool tool)
{
    pmix_byte_object_t blob;
    char *msg;
    size_t sz;
    int sd = -1;
    uint32_t index;
    pmix_status_t reply;

    ack_local = true;
    if (PMIX_SUCCESS != start_server(&mymodule, !tool, getuid()) || !build_blob(&blob)) {
        return CHILD_SETUP;
    }
    if (tool) {
        msg = build_ack(PMIX_TOOL_GIVEN_ID, AC_TOOLNS, 0, "native", &blob, false, &sz);
    } else {
        msg = build_ack(PMIX_SIMPLE_CLIENT, AC_NSPACE, 0, "native", &blob, false, &sz);
    }
    reply = send_ack(msg, sz, &sd);
    free(msg);
    free(blob.bytes);
    if (PMIX_SUCCESS == reply) {
        reply = tool ? tool_replies(sd) : (read_u32(sd, &index) ? PMIX_SUCCESS : PMIX_ERR_UNREACH);
    }
    if (PMIX_SUCCESS != reply) {
        fprintf(stdout, "    the %s was refused: %s\n", tool ? "tool" : "client",
                PMIx_Error_string(reply));
        return CHILD_FAIL;
    }
    usleep(200000);
    if ((uint32_t) getuid() != host_uid || (uint32_t) getgid() != host_gid ||
        getpid() != host_pid) {
        fprintf(stdout, "    the host was given uid %u gid %u pid %d\n", host_uid, host_gid,
                (int) host_pid);
        return CHILD_FAIL;
    }
    return CHILD_PASS;
}
/* A client on the local transport whose credential claims the uid its
 * rank was registered with - another user's - is that other user in
 * nothing but its claim, and is refused */
static int case_local_claim(void)
{
    char *msg;
    size_t sz;
    int sd = -1;
    pmix_status_t reply;

    ack_local = true;
    ack_cred_uid = getuid() + 4242;
    if (PMIX_SUCCESS != start_server(&mymodule, true, getuid() + 4242)) {
        return CHILD_SETUP;
    }
    msg = build_ack(PMIX_SIMPLE_CLIENT, AC_NSPACE, 0, "native", NULL, false, &sz);
    reply = send_ack(msg, sz, &sd);
    free(msg);
    if (0 != nclient_connected) {
        fprintf(stdout, "    the host was asked about the client\n");
        return CHILD_FAIL;
    }
    return expect_status("a local client claiming another user's uid", reply,
                         PMIX_ERR_INVALID_CRED);
}

static int run_local_identity_client(void)
{
    return case_local_identity(false);
}
static int run_local_identity_tool(void)
{
    return case_local_identity(true);
}

/* A server whose host did not ask for tool support refuses every tool -
 * however it found the server - before the host is asked */
static int case_no_tool_support(void)
{
    char *msg;
    size_t sz;
    int sd = -1;
    pmix_status_t reply;

    no_tool_support = true;
    if (PMIX_SUCCESS != start_server(&mymodule, true, getuid())) {
        return CHILD_SETUP;
    }
    msg = build_ack(PMIX_TOOL_CLIENT, AC_NSPACE, 0, "native", NULL, false, &sz);
    reply = send_ack(msg, sz, &sd);
    free(msg);
    if (0 != ntool_connected) {
        fprintf(stdout, "    the host was asked about the tool\n");
        return CHILD_FAIL;
    }
    return expect_status("a tool on a server without tool support", reply,
                         PMIX_ERR_NOT_SUPPORTED);
}

static void dereg_client_rank(void);

/* wait up to `secs` for a lost-connection event - the server raises them
 * a moment after the loss - and return how many came */
static int wait_lost(int secs)
{
    int n;

    for (n = 0; n < secs * 10 && 0 == nlost; n++) {
        usleep(100000);
    }
    return nlost;
}

static void register_lost(void)
{
    pmix_status_t code = PMIX_ERR_LOST_CONNECTION;

    PMIx_Register_event_handler(&code, 1, NULL, 0, lost_handler, NULL, NULL);
}

/* A client accepted over the local socket, and left connected: sd. With
 * `close_it`, the connection is then closed while this process runs on,
 * as an exec closes it */
static bool local_client(int *sd, bool close_it)
{
    char *msg;
    size_t sz;
    uint32_t index;
    pmix_status_t reply;

    ack_local = true;
    msg = build_ack(PMIX_SIMPLE_CLIENT, AC_NSPACE, 0, "native", NULL, false, &sz);
    reply = send_ack(msg, sz, sd);
    free(msg);
    if (PMIX_SUCCESS != reply || !read_u32(*sd, &index)) {
        return false;
    }
    usleep(100000);
    if (close_it) {
        close(*sd);
        *sd = -1;
        usleep(300000);
    }
    return true;
}

/* A live client on the local socket is not taken over by a TCP
 * connection sending its pid */
static int case_live_local_tcp_pid(void)
{
    pmix_byte_object_t blob;
    char *msg;
    size_t sz;
    int sd1 = -1, sd2 = -1;
    pmix_status_t reply;

    if (PMIX_SUCCESS != start_server(&mymodule, true, getuid()) ||
        !build_blob_pid(&blob, getpid())) {
        return CHILD_SETUP;
    }
    register_lost();
    if (!local_client(&sd1, false)) {
        return CHILD_SETUP;
    }
    ack_local = false;
    msg = build_ack(PMIX_SIMPLE_CLIENT, AC_NSPACE, 0, "native", &blob, false, &sz);
    reply = send_ack(msg, sz, &sd2);
    free(msg);
    if (PMIX_ERR_EXISTS != reply) {
        fprintf(stdout, "    a TCP connection sending the live client's pid read %s\n",
                PMIx_Error_string(reply));
        return CHILD_FAIL;
    }
    usleep(300000);
    if (0 != nlost || 1 != count_live(AC_NSPACE, 0)) {
        fprintf(stdout, "    %d losses; %d live connections for the rank\n", nlost,
                count_live(AC_NSPACE, 0));
        return CHILD_FAIL;
    }
    return CHILD_PASS;
}

/* A tool that closes its connection while it runs on - a disconnect - is
 * gone at once */
static int case_tool_lost(void)
{
    char *msg;
    size_t sz;
    int sd = -1;
    pmix_status_t reply;

    if (PMIX_SUCCESS != start_server(&mymodule, false, getuid())) {
        return CHILD_SETUP;
    }
    register_lost();
    ack_local = true;
    msg = build_ack(PMIX_TOOL_GIVEN_ID, AC_TOOLNS, 0, "native", NULL, false, &sz);
    reply = send_ack(msg, sz, &sd);
    free(msg);
    if (PMIX_SUCCESS == reply) {
        reply = tool_replies(sd);
    }
    if (PMIX_SUCCESS != reply) {
        return CHILD_SETUP;
    }
    usleep(100000);
    close(sd);
    if (1 != wait_lost(3)) {
        fprintf(stdout, "    %d lost-connection events for the disconnected tool\n", nlost);
        return CHILD_FAIL;
    }
    return CHILD_PASS;
}

/* A client the server drops - here for a message past ptl_base_max_msg_size
 * - is lost at once, though its process runs on */
static int case_server_drop_lost(void)
{
    pmix_ptl_hdr_t hdr;
    int sd = -1;

    setenv("PMIX_MCA_ptl_base_max_msg_size", "1", 1);
    if (PMIX_SUCCESS != start_server(&mymodule, true, getuid())) {
        return CHILD_SETUP;
    }
    register_lost();
    if (!local_client(&sd, false)) {
        return CHILD_SETUP;
    }
    memset(&hdr, 0, sizeof(hdr));
    hdr.tag = htonl(PMIX_PTL_TAG_DYNAMIC + 1);
    hdr.nbytes = htonl(8u * 1024u * 1024u);
    if (sizeof(hdr) != write(sd, &hdr, sizeof(hdr))) {
        return CHILD_SETUP;
    }
    if (1 != wait_lost(3)) {
        fprintf(stdout, "    %d lost-connection events for the dropped client\n", nlost);
        return CHILD_FAIL;
    }
    return CHILD_PASS;
}

/* A client whose connection over the local socket closes while its
 * process runs on - this one, as an exec closes it - is lost at once, and
 * its rank may connect again */
static int case_closed_is_lost(void)
{
    char *msg;
    size_t sz;
    int sd1 = -1, sd2 = -1;
    pmix_status_t reply;

    if (PMIX_SUCCESS != start_server(&mymodule, true, getuid())) {
        return CHILD_SETUP;
    }
    register_lost();
    if (!local_client(&sd1, true)) {
        return CHILD_SETUP;
    }
    if (1 != wait_lost(3)) {
        fprintf(stdout, "    %d lost-connection events for the closed connection\n", nlost);
        return CHILD_FAIL;
    }
    ack_local = true;
    msg = build_ack(PMIX_SIMPLE_CLIENT, AC_NSPACE, 0, "native", NULL, false, &sz);
    reply = send_ack(msg, sz, &sd2);
    free(msg);
    return expect_status("the rank connecting again", reply, PMIX_SUCCESS);
}

/* A tool that brings its own identity must name a process: one sending
 * an empty namespace and no rank is refused before the host is asked */
static int case_tool_names_nobody(void)
{
    char *msg;
    size_t sz;
    int sd = -1;
    pmix_status_t reply;

    if (PMIX_SUCCESS != start_server(&mymodule, true, getuid())) {
        return CHILD_SETUP;
    }
    msg = build_ack(PMIX_TOOL_GIVEN_ID, "", PMIX_RANK_INVALID, "native", NULL, false, &sz);
    reply = send_ack(msg, sz, &sd);
    free(msg);
    if (0 != ntool_connected) {
        fprintf(stdout, "    the host was asked about the tool\n");
        return CHILD_FAIL;
    }
    return expect_status("a tool naming nobody", reply, PMIX_ERR_BAD_PARAM);
}

/* A tool on a client's rank naming a rank its namespace does not have is
 * given one - when the namespace is its own user's. In another user's
 * namespace it is refused before the host is asked: a rank there would
 * make it one of that job's own processes */
static int case_tool_new_rank(bool other)
{
    char *msg;
    size_t sz;
    int sd = -1;
    pmix_status_t reply;

    if (PMIX_SUCCESS != start_server(&mymodule, true, other ? getuid() + 4242 : getuid())) {
        return CHILD_SETUP;
    }
    msg = build_ack(PMIX_TOOL_CLIENT, AC_NSPACE, 7, "native", NULL, false, &sz);
    reply = send_ack(msg, sz, &sd);
    free(msg);
    if (!other) {
        if (PMIX_SUCCESS == reply) {
            reply = tool_replies(sd);
        }
        return expect_status("a tool on a new rank of its own user's namespace", reply,
                             PMIX_SUCCESS);
    }
    if (0 != ntool_connected) {
        fprintf(stdout, "    the host was asked about the tool\n");
        return CHILD_FAIL;
    }
    return expect_status("a tool on a new rank of another user's namespace", reply,
                         PMIX_ERR_NO_PERMISSIONS);
}
static int run_new_rank_own(void)
{
    return case_tool_new_rank(false);
}
static int run_new_rank_other(void)
{
    return case_tool_new_rank(true);
}

/* A tool holds a new rank of its own user's namespace, and the host then
 * registers that rank: for the tool's own uid and gid the registration
 * takes the entry over; for another uid it is refused, and the tool's
 * entry keeps the tool's identity */
static int case_register_held(bool other)
{
    pmix_namespace_t *ns;
    pmix_rank_info_t *info, *held = NULL;
    pmix_proc_t proc;
    char *msg;
    size_t sz;
    int sd = -1;
    pmix_status_t reply, rc;

    if (PMIX_SUCCESS != start_server(&mymodule, true, getuid())) {
        return CHILD_SETUP;
    }
    msg = build_ack(PMIX_TOOL_CLIENT, AC_NSPACE, 7, "native", NULL, false, &sz);
    reply = send_ack(msg, sz, &sd);
    free(msg);
    if (PMIX_SUCCESS == reply) {
        reply = tool_replies(sd);
    }
    if (PMIX_SUCCESS != reply) {
        return CHILD_SETUP;
    }
    usleep(100000);
    PMIX_LOAD_PROCID(&proc, AC_NSPACE, 7);
    rc = PMIx_server_register_client(&proc, other ? getuid() + 4242 : getuid(), getgid(), NULL,
                                     NULL, NULL);
    if (NULL != (ns = find_ns(AC_NSPACE))) {
        PMIX_LIST_FOREACH (info, &ns->ranks, pmix_rank_info_t) {
            if (7 == info->pname.rank) {
                held = info;
            }
        }
    }
    if (NULL == held || getuid() != held->uid) {
        fprintf(stdout, "    the rank's entry is %s uid %lu\n", (NULL == held) ? "gone," : "",
                (NULL == held) ? 0UL : (unsigned long) held->uid);
        return CHILD_FAIL;
    }
    if (!other) {
        return expect_status("registering the tool's own identity",
                             (PMIX_OPERATION_SUCCEEDED == rc) ? PMIX_SUCCESS : rc, PMIX_SUCCESS);
    }
    return expect_status("registering another uid on a rank a tool holds", rc, PMIX_ERR_EXISTS);
}
static int run_register_held_own(void)
{
    return case_register_held(false);
}
static int run_register_held_other(void)
{
    return case_register_held(true);
}

/* A tool of an earlier release with its server's address but no namespace
 * or rank says it is on a client's rank, and sends no name: the host gives
 * it one, and the tool reads the replies it always did. `how`:
 *   0 - a v5.0 tool;
 *   1 - a v7.0.0rc1 tool, which sends what a v5.0 tool does;
 *   2 - a v5.0 launcher */
static int case_nameless_tool(int how)
{
    char *msg;
    size_t sz;
    int sd = -1;
    pmix_status_t reply;

    ack_version = (1 == how) ? "7.0.0rc1" : "5.0.11";
    ack_bfrops = (1 == how) ? "v61" : "v41";
    if (PMIX_SUCCESS != start_server(&mymodule, true, getuid())) {
        return CHILD_SETUP;
    }
    msg = build_ack((2 == how) ? PMIX_LAUNCHER_CLIENT : PMIX_TOOL_CLIENT, "",
                    PMIX_RANK_INVALID, "native", NULL, false, &sz);
    reply = send_ack(msg, sz, &sd);
    free(msg);
    if (PMIX_SUCCESS == reply) {
        reply = tool_replies(sd);
    }
    if (PMIX_SUCCESS != reply || 1 != ntool_connected || 0 != host_nkeys[NSPACE_AT]) {
        fprintf(stdout, "    the tool read %s; the host was asked %d times, given %d names\n",
                PMIx_Error_string(reply), ntool_connected, host_nkeys[NSPACE_AT]);
        return CHILD_FAIL;
    }
    return CHILD_PASS;
}
static int run_nameless_v50(void)
{
    return case_nameless_tool(0);
}
static int run_nameless_v70rc(void)
{
    return case_nameless_tool(1);
}
static int run_nameless_launcher(void)
{
    return case_nameless_tool(2);
}

/* A rank its host prepared a process for with PMIx_server_setup_fork has
 * a token, which a connection on the local socket naming the rank must
 * present. `how`:
 *   0 - a client presenting it is taken;
 *   1 - a client presenting another is refused, the host not asked;
 *   2 - a client presenting none is refused, the host not asked;
 *   3 - a tool on the client's rank presenting none is refused, the host
 *       not asked;
 *   4 - a tool on the client's rank presenting it is taken, and the host
 *       is not given the token;
 *   5 - a client of this release over TCP presenting none is refused;
 *   6 - one presenting it over TCP is taken;
 *   7 - a client of an earlier release over TCP, which has none to
 *       present, is taken;
 *   8 - a tool of this release on the client's rank over TCP presenting
 *       none is refused;
 *   9 - a client naming a rank registered but not yet prepared is
 *       refused, the host not asked;
 *  10 - a client declaring an earlier release on the local socket - which
 *       no earlier release uses - is refused, the host not asked, though
 *       the server takes clients of earlier releases over TCP */
static int case_token(int how)
{
    pmix_byte_object_t blob = {NULL, 0};
    pmix_proc_t proc;
    char **env = NULL, *token, *msg, wrong[2 * PMIX_PTL_TOKEN_LEN + 1];
    size_t sz;
    int sd = -1;
    pmix_status_t reply;

    /* the token is checked on whichever socket the connection comes */
    ack_local = (5 > how || 10 == how);
    if (7 == how || 10 == how) {
        ack_version = AC_TCP_VERSION;
    }
    ac_unprepared = (9 == how);
    if (PMIX_SUCCESS != start_server(&mymodule, true, getuid())) {
        return CHILD_SETUP;
    }
    /* the rank prepared again: this case says what is presented */
    free(ac_token);
    ac_token = NULL;
    PMIX_LOAD_PROCID(&proc, AC_NSPACE, 0);
    if (9 != how) {
        if (PMIX_SUCCESS != PMIx_server_setup_fork(&proc, &env) ||
            NULL == (token = pmix_getenv(PMIX_PTL_TOKEN_ENV, env))) {
            return CHILD_SETUP;
        }
        /* the hex, after the server's name */
        token = strrchr(token, ';') + 1;
        snprintf(wrong, sizeof(wrong), "%s", token);
        wrong[0] = ('0' == wrong[0]) ? '1' : '0';
        if ((0 == how || 4 == how || 6 == how) && !build_blob_token(&blob, token)) {
            return CHILD_SETUP;
        }
        if (1 == how && !build_blob_token(&blob, wrong)) {
            return CHILD_SETUP;
        }
        PMIx_Argv_free(env);
    }
    msg = build_ack((3 == how || 4 == how || 8 == how) ? PMIX_TOOL_CLIENT : PMIX_SIMPLE_CLIENT,
                    AC_NSPACE, 0, "native",
                    (NULL == blob.bytes) ? NULL : &blob, false, &sz);
    reply = send_ack(msg, sz, &sd);
    free(msg);
    free(blob.bytes);
    if (0 == how || 4 == how || 6 == how || 7 == how) {
        if (4 == how && PMIX_SUCCESS == reply) {
            reply = tool_replies(sd);
        }
        if (4 == how && host_token) {
            fprintf(stdout, "    the host was given the token\n");
            return CHILD_FAIL;
        }
        return expect_status("presenting the rank's token", reply, PMIX_SUCCESS);
    }
    if (0 != nclient_connected || 0 != ntool_connected) {
        fprintf(stdout, "    the host was asked\n");
        return CHILD_FAIL;
    }
    if (10 == how) {
        return expect_status("an earlier release on the local socket", reply,
                             PMIX_ERR_NOT_SUPPORTED);
    }
    return expect_status((1 == how) ? "presenting another token" : "presenting none", reply,
                         PMIX_ERR_INVALID_CRED);
}
static int run_token_right(void)
{
    return case_token(0);
}
static int run_token_wrong(void)
{
    return case_token(1);
}
static int run_token_none(void)
{
    return case_token(2);
}
static int run_token_unprepared(void)
{
    return case_token(9);
}
static int run_token_tool_none(void)
{
    return case_token(3);
}
static int run_token_tool_right(void)
{
    return case_token(4);
}
static int run_token_tcp_none(void)
{
    return case_token(5);
}
static int run_token_tcp_right(void)
{
    return case_token(6);
}
static int run_token_tcp_earlier(void)
{
    return case_token(7);
}
static int run_token_tcp_tool_none(void)
{
    return case_token(8);
}
static int run_earlier_local(void)
{
    return case_token(10);
}

/* A server that takes no legacy peers refuses a client or tool of an
 * earlier release, with PMIX_ERR_NOT_SUPPORTED, and the host is not asked.
 * It has no TCP listener, so the connect-ack comes on the local socket -
 * as no process of an earlier release could send it, but the refusal
 * does not depend on how it came */
static int case_legacy_refused(bool tool)
{
    char *msg;
    size_t sz;
    int sd = -1;
    pmix_status_t reply;

    setenv("PMIX_MCA_ptl_base_legacy_peers", "0", 1);
    ack_local = true;
    ack_version = "5.0.11";
    ack_bfrops = "v41";
    if (PMIX_SUCCESS != start_server(&mymodule, true, getuid())) {
        return CHILD_SETUP;
    }
    if (0 <= pmix_ptl_base.listener.socket) {
        fprintf(stdout, "    the server has a TCP listener\n");
        return CHILD_FAIL;
    }
    msg = build_ack(tool ? PMIX_TOOL_GIVEN_ID : PMIX_SIMPLE_CLIENT, tool ? AC_TOOLNS : AC_NSPACE,
                    0, "native", NULL, false, &sz);
    reply = send_ack(msg, sz, &sd);
    free(msg);
    if (0 != nclient_connected || 0 != ntool_connected) {
        fprintf(stdout, "    the host was asked\n");
        return CHILD_FAIL;
    }
    return expect_status(tool ? "v5.0 tool" : "v5.0 client", reply, PMIX_ERR_NOT_SUPPORTED);
}
static int run_legacy_refused_client(void)
{
    return case_legacy_refused(false);
}
static int run_legacy_refused_tool(void)
{
    return case_legacy_refused(true);
}

/* A tool that names, in its own info, the identity the server sets - the
 * uid, gid, pid, credential type, namespace, version, rank and timeout,
 * twice each - has the host given the server's alone, once each. The pid
 * is the kernel's where it reports one, else the first pid the tool sent */
static int case_tool_dup_keys(void)
{
    pmix_byte_object_t blob;
    char *msg;
    size_t sz;
    int sd = -1, k;
    pmix_status_t reply;

    if (PMIX_SUCCESS != start_server(&mymodule, false, getuid()) || !build_blob_dups(&blob)) {
        return CHILD_SETUP;
    }
    msg = build_ack(PMIX_TOOL_GIVEN_ID, AC_TOOLNS, 0, "native", &blob, false, &sz);
    reply = send_ack(msg, sz, &sd);
    free(msg);
    if (PMIX_SUCCESS == reply) {
        reply = tool_replies(sd);
    }
    if (PMIX_SUCCESS != reply) {
        fprintf(stdout, "    the tool was refused: %s\n", PMIx_Error_string(reply));
        return CHILD_FAIL;
    }
    for (k = 0; k < NCOUNTED; k++) {
        /* the host is given a timeout only if one was set for it */
        if (1 != host_nkeys[k] && !(PMIx_Check_key(counted_keys[k], PMIX_TIMEOUT) &&
                                    0 == host_nkeys[k] && 0 > host_timeout)) {
            fprintf(stdout, "    the host was given %d copies of %s\n", host_nkeys[k],
                    counted_keys[k]);
            return CHILD_FAIL;
        }
    }
    if ((uint32_t) getuid() != host_uid || (uint32_t) getgid() != host_gid ||
        (getpid() != host_pid && AC_PID != host_pid) || 0 != strcmp(host_credtype, "native") ||
        0 != strcmp(host_nspace, AC_TOOLNS) || 4242 == host_timeout) {
        fprintf(stdout,
                "    the host was given uid %u gid %u pid %ld cred %s nspace %s timeout %d\n",
                host_uid, host_gid, (long) host_pid, host_credtype, host_nspace, host_timeout);
        return CHILD_FAIL;
    }
    return CHILD_PASS;
}

/* What the host is given for a client whose info claims another uid and
 * gid, and for a tool */
static int case_host_info(bool tool)
{
    pmix_byte_object_t blob;
    char *msg;
    size_t sz;
    int sd = -1;
    uint32_t index;
    pmix_status_t reply;

    if (PMIX_SUCCESS != start_server(&mymodule, !tool, getuid()) || !build_blob(&blob)) {
        return CHILD_SETUP;
    }
    if (tool) {
        msg = build_ack(PMIX_TOOL_NEEDS_ID, "", 0, "native", NULL, false, &sz);
    } else {
        msg = build_ack(PMIX_SIMPLE_CLIENT, AC_NSPACE, 0, "native", &blob, false, &sz);
    }
    reply = send_ack(msg, sz, &sd);
    free(msg);
    free(blob.bytes);
    if (PMIX_SUCCESS != reply) {
        fprintf(stdout, "    the %s was refused: %s\n", tool ? "tool" : "client",
                PMIx_Error_string(reply));
        return CHILD_FAIL;
    }
    if (!tool && !read_u32(sd, &index)) {
        return CHILD_FAIL;
    }
    usleep(200000);
    if (0 != strcmp(host_credtype, "native")) {
        fprintf(stdout, "    the host was given credential type \"%s\"\n", host_credtype);
        return CHILD_FAIL;
    }
    if ((uint32_t) getuid() != host_uid || (uint32_t) getgid() != host_gid) {
        fprintf(stdout, "    the host was given uid %u gid %u\n", host_uid, host_gid);
        return CHILD_FAIL;
    }
    /* the kernel names no pid at the far end of a TCP connection, so the
     * pid is the one the client's library sent */
    if (!tool && AC_PID != host_pid) {
        fprintf(stdout, "    the host was given pid %d\n", (int) host_pid);
        return CHILD_FAIL;
    }
    return CHILD_PASS;
}

static void noop_cb(pmix_status_t status, void *cbdata)
{
    PMIX_HIDE_UNUSED_PARAMS(status, cbdata);
}

/* the host deregisters a launched tool's rank during its up-call */
#define AC_TCNS "accept.tc"
static void dereg_tc_rank(void)
{
    pmix_proc_t proc;

    PMIX_LOAD_PROCID(&proc, AC_TCNS, 0);
    PMIx_server_deregister_client(&proc, noop_cb, NULL);
}

static int case_tool_rank_dereg(void)
{
    pmix_byte_object_t blob = {NULL, 0};
    char **env = NULL, *token;
    char *msg;
    size_t sz;
    int sd = -1, rc;
    pmix_status_t reply, code = PMIX_ERR_LOST_CONNECTION;
    pmix_proc_t proc;
    pmix_nspace_t ns;

    if (PMIX_SUCCESS != start_server(&mymodule, false, getuid())) {
        return CHILD_SETUP;
    }
    PMIX_LOAD_NSPACE(ns, AC_TCNS);
    reply = PMIx_server_register_nspace(ns, 1, NULL, 0, NULL, NULL);
    PMIX_LOAD_PROCID(&proc, AC_TCNS, 0);
    reply = PMIx_server_register_client(&proc, getuid(), getgid(), NULL, NULL, NULL);
    if (PMIX_SUCCESS != reply && PMIX_OPERATION_SUCCEEDED != reply) {
        return CHILD_SETUP;
    }
    /* launched: prepared, and presenting the token it was handed */
    if (PMIX_SUCCESS != PMIx_server_setup_fork(&proc, &env) ||
        NULL == (token = pmix_getenv(PMIX_PTL_TOKEN_ENV, env)) ||
        NULL == (token = strrchr(token, ';')) || !build_blob_token(&blob, token + 1)) {
        return CHILD_SETUP;
    }
    PMIx_Argv_free(env);
    PMIx_Register_event_handler(&code, 1, NULL, 0, lost_handler, NULL, NULL);
    tool_hook = dereg_tc_rank;
    msg = build_ack(PMIX_TOOL_CLIENT, AC_TCNS, 0, "native", &blob, false, &sz);
    reply = send_ack(msg, sz, &sd);
    free(msg);
    free(blob.bytes);
    if (PMIX_SUCCESS != reply) {
        fprintf(stdout, "    the tool's first status was %s\n", PMIx_Error_string(reply));
        return CHILD_SETUP;
    }
    rc = expect_status("a rank deregistered during the up-call", tool_replies(sd),
                       PMIX_ERR_NOT_FOUND);
    if (CHILD_PASS != rc) {
        return rc;
    }
    /* the host accepted it, so it is told the tool is not connected */
    usleep(300000);
    if (1 != nlost || !PMIx_Check_nspace(lost_source.nspace, AC_TCNS)) {
        fprintf(stdout, "    the host was told of %d lost connections\n", nlost);
        return CHILD_FAIL;
    }
    return CHILD_PASS;
}

static pmix_status_t tool_outcome(int sd, bool needs_id);

/* the host registers a namespace of another user while it decides on a
 * tool, and gives the tool a rank in it */
static void register_other_users(void)
{
    pmix_proc_t proc;
    pmix_nspace_t ns;

    PMIX_LOAD_NSPACE(ns, AC_OTHERNS);
    PMIx_server_register_nspace(ns, 1, NULL, 0, noop_cb, NULL);
    PMIX_LOAD_PROCID(&proc, AC_OTHERNS, 0);
    PMIx_server_register_client(&proc, getuid() + 1, getgid(), NULL, noop_cb, NULL);
}

/* A tool given a rank in a namespace that appeared while the host decided
 * joins it only if the namespace is its own user's */
static int case_tool_joins_other_user(void)
{
    char *msg;
    size_t sz;
    int sd = -1;
    pmix_status_t reply;

    if (PMIX_SUCCESS != start_server(&mymodule, false, getuid())) {
        return CHILD_SETUP;
    }
    tool_hook = register_other_users;
    PMIX_LOAD_PROCID(&tool_answer, AC_OTHERNS, 1);
    msg = build_ack(PMIX_TOOL_NEEDS_ID, "", 0, "native", NULL, false, &sz);
    if (!open_ack(msg, sz, &sd)) {
        return CHILD_SETUP;
    }
    free(msg);
    reply = tool_outcome(sd, true);
    return expect_status("a tool given a rank in another user's namespace", reply,
                         PMIX_ERR_NOT_SUPPORTED);
}

static void dereg_client_rank(void)
{
    pmix_proc_t proc;

    PMIX_LOAD_PROCID(&proc, AC_NSPACE, 0);
    PMIx_server_deregister_client(&proc, noop_cb, NULL);
}

/* the host deregisters a client while it accepts it: the client is
 * refused, and the host told it is not connected */
static int case_client_rank_dereg(void)
{
    char *msg;
    size_t sz;
    int sd = -1, rc;
    pmix_status_t reply, code = PMIX_ERR_LOST_CONNECTION;

    if (PMIX_SUCCESS != start_server(&mymodule, true, getuid())) {
        return CHILD_SETUP;
    }
    PMIx_Register_event_handler(&code, 1, NULL, 0, lost_handler, NULL, NULL);
    client_hook = dereg_client_rank;
    msg = build_ack(PMIX_SIMPLE_CLIENT, AC_NSPACE, 0, "native", NULL, false, &sz);
    reply = send_ack(msg, sz, &sd);
    free(msg);
    rc = expect_status("a client deregistered during the up-call", reply, PMIX_ERR_NOT_FOUND);
    if (CHILD_PASS != rc) {
        return rc;
    }
    usleep(300000);
    if (1 != nlost || !PMIx_Check_nspace(lost_source.nspace, AC_NSPACE)) {
        fprintf(stdout, "    the host was told of %d lost connections\n", nlost);
        return CHILD_FAIL;
    }
    return CHILD_PASS;
}

/* two connections for one rank, both reaching the host before it answers */
static int case_both_waiting(void)
{
    char *msg;
    size_t sz;
    int sd1 = -1, sd2 = -1, n, asked;
    uint32_t r1 = 0, r2 = 0, i1 = 0, i2 = 0;
    pmix_namespace_t *ns;
    pmix_rank_info_t *info;

    host_mode = HOST_DEFERS;
    if (PMIX_SUCCESS != start_server(&mymodule, true, getuid())) {
        return CHILD_SETUP;
    }
    msg = build_ack(PMIX_SIMPLE_CLIENT, AC_NSPACE, 0, "native", NULL, false, &sz);
    if (!open_ack(msg, sz, &sd1)) {
        return CHILD_SETUP;
    }
    usleep(200000);
    if (!open_ack(msg, sz, &sd2)) {
        return CHILD_SETUP;
    }
    free(msg);
    usleep(300000);
    asked = nclient_connected;
    for (n = 0; n < ndeferred; n++) {
        deferred_cb[n](PMIX_SUCCESS, deferred_cbdata[n]);
    }
    if (!read_u32(sd1, &r1) || (PMIX_SUCCESS == (pmix_status_t) r1 && !read_u32(sd1, &i1))) {
        return CHILD_FAIL;
    }
    if (!read_u32(sd2, &r2) || (PMIX_SUCCESS == (pmix_status_t) r2 && !read_u32(sd2, &i2))) {
        return CHILD_FAIL;
    }
    usleep(200000);
    if (1 != asked) {
        fprintf(stdout, "    the host was asked %d times\n", asked);
        return CHILD_FAIL;
    }
    ns = find_ns(AC_NSPACE);
    info = (pmix_rank_info_t *) pmix_list_get_first(&ns->ranks);
    if (PMIX_SUCCESS != (pmix_status_t) r1 || (int) i1 != info->peerid) {
        fprintf(stdout, "    the first connection read %s, and the rank points at %d not %u\n",
                PMIx_Error_string((pmix_status_t) r1), info->peerid, i1);
        return CHILD_FAIL;
    }
    return expect_status("the second connection", (pmix_status_t) r2, PMIX_ERR_EXISTS);
}

/* a bad credential with a malformed info blob behind it */
static int case_badcred_bad_blob(void)
{
    pmix_byte_object_t blob;
    char junk[16];
    char *msg;
    size_t sz;
    int sd = -1;
    pmix_status_t reply;

    if (PMIX_SUCCESS != start_server(&mymodule, true, getuid())) {
        return CHILD_SETUP;
    }
    /* a count far larger than the bytes behind it */
    memset(junk, 0x7f, sizeof(junk));
    blob.bytes = junk;
    blob.size = sizeof(junk);
    ack_badcred = true;
    msg = build_ack(PMIX_SIMPLE_CLIENT, AC_NSPACE, 0, "native", &blob, false, &sz);
    reply = send_ack(msg, sz, &sd);
    free(msg);
    return expect_status("a bad credential before a bad blob", reply, PMIX_ERR_INVALID_CRED);
}

/* a host that accepts a tool needing an identity without giving it one */
static int case_tool_no_identity(void)
{
    char *msg;
    size_t sz;
    int sd = -1;
    pmix_status_t reply;

    host_mode = HOST_GIVES_NO_ID;
    if (PMIX_SUCCESS != start_server(&mymodule, false, getuid())) {
        return CHILD_SETUP;
    }
    msg = build_ack(PMIX_TOOL_NEEDS_ID, "", 0, "native", NULL, false, &sz);
    reply = send_ack(msg, sz, &sd);
    free(msg);
    return expect_status("accepted with no identity", reply, PMIX_ERR_NOT_SUPPORTED);
}

/* the listener's limits: `which` 0 = oversized, 1 = too slow, 2 = the cap */
static int case_listener_limit(int which)
{
    pmix_ptl_hdr_t hdr;
    char *msg;
    size_t sz;
    int sd = -1, sd2 = -1;
    uint32_t st;

    if (1 == which) {
        setenv("PMIX_MCA_ptl_base_connect_ack_timeout", "1", 1);
    } else if (2 == which) {
        setenv("PMIX_MCA_ptl_base_max_pending_connections", "1", 1);
    }
    if (PMIX_SUCCESS != start_server(&mymodule, true, getuid())) {
        return CHILD_SETUP;
    }
    memset(&hdr, 0, sizeof(hdr));
    hdr.nbytes = (0 == which) ? PMIX_MAX_CRED_SIZE + 1 : 64;
    /* all of an oversized header; half of anything else */
    if (!open_ack((char *) &hdr, (0 == which) ? sizeof(hdr) : sizeof(hdr) / 2, &sd)) {
        return CHILD_SETUP;
    }
    if (2 == which) {
        /* a second unfinished connection pushes out the first */
        usleep(200000);
        msg = build_ack(PMIX_SIMPLE_CLIENT, AC_NSPACE, 0, "native", NULL, false, &sz);
        if (!open_ack(msg, sz / 2, &sd2)) {
            free(msg);
            return CHILD_SETUP;
        }
        free(msg);
    }
    if (!read_u32(sd, &st)) {
        fprintf(stdout, "    the connection was closed with no status\n");
        return CHILD_FAIL;
    }
    switch (which) {
    case 0:
        return expect_status("an oversized connect-ack", (pmix_status_t) st, PMIX_ERR_BAD_PARAM);
    case 1:
        return expect_status("a connect-ack too slow", (pmix_status_t) st, PMIX_ERR_TIMEOUT);
    default:
        return expect_status("pushed out at the cap", (pmix_status_t) st,
                             PMIX_ERR_OUT_OF_RESOURCE);
    }
}

/* true if the server closes sd within msec, whatever it sends first */
static bool closes_within(int sd, int msec)
{
    struct pollfd pfd;
    char buf[512];
    ssize_t rd;

    while (1) {
        pfd.fd = sd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        if (0 >= poll(&pfd, 1, msec)) {
            return false;
        }
        rd = recv(sd, buf, sizeof(buf), 0);
        if (0 >= rd) {
            return true;
        }
    }
}

/* a v2.1 tool: connector flag 1, a credential with no trailer, and the
 * v2.1 wire format. Count what follows its first status */
static int case_legacy_tool(void)
{
    pmix_ptl_hdr_t hdr;
    char credbytes[sizeof(uid_t) + sizeof(gid_t)];
    uid_t euid = geteuid();
    gid_t egid = getegid();
    char *msg, buf[2048];
    size_t csize, payload, sz, total = 0, want;
    uint8_t bftype = PMIX_BFROP_BUFFER_NON_DESC;
    uint8_t flag = PMIX_LEGACY_TOOL;
    const char *vers = "2.1.4", *bfr = "v21", *psec = "native";
    int sd = -1;
    uint32_t st;
    struct pollfd pfd;
    ssize_t rd;

    if (PMIX_SUCCESS != start_server(&mymodule, false, getuid())) {
        return CHILD_SETUP;
    }
    memcpy(credbytes, &euid, sizeof(uid_t));
    memcpy(credbytes + sizeof(uid_t), &egid, sizeof(gid_t));
    payload = strlen(psec) + 1 + sizeof(uint32_t) + sizeof(credbytes) + 1 + 2 * sizeof(uint32_t)
              + strlen(vers) + 1 + strlen(bfr) + 1 + 1 + strlen(AC_GDS) + 1;
    memset(&hdr, 0, sizeof(hdr));
    hdr.pindex = -1;
    hdr.tag = UINT32_MAX;
    hdr.nbytes = (uint32_t) payload;
    sz = sizeof(hdr) + payload;
    msg = (char *) calloc(1, sz);
    memcpy(msg, &hdr, sizeof(hdr));
    csize = sizeof(hdr);
    PMIX_PTL_PUT_STRING(psec);
    PMIX_PTL_PUT_U32(sizeof(credbytes));
    PMIX_PTL_PUT_BLOB(credbytes, sizeof(credbytes));
    PMIX_PTL_PUT_U8(flag);
    PMIX_PTL_PUT_U32(getuid());
    PMIX_PTL_PUT_U32(getgid());
    PMIX_PTL_PUT_STRING(vers);
    PMIX_PTL_PUT_STRING(bfr);
    PMIX_PTL_PUT_U8(bftype);
    PMIX_PTL_PUT_STRING(AC_GDS);
    if (!open_ack(msg, sz, &sd)) {
        free(msg);
        return CHILD_SETUP;
    }
    free(msg);
    if (!read_u32(sd, &st) || PMIX_SUCCESS != (pmix_status_t) st) {
        fprintf(stdout, "    the legacy tool was refused\n");
        return CHILD_FAIL;
    }
    while (1) {
        pfd.fd = sd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        if (0 >= poll(&pfd, 1, 1500)) {
            break;
        }
        rd = recv(sd, buf, sizeof(buf), 0);
        if (0 >= rd) {
            break;
        }
        total += (size_t) rd;
    }
    /* its namespace, the server's namespace and rank, the last status */
    want = 2 * (PMIX_MAX_NSLEN + 1) + 2 * sizeof(uint32_t);
    if (want != total) {
        fprintf(stdout, "    the legacy tool was sent %zu bytes after its status, not %zu\n",
                total, want);
        return CHILD_FAIL;
    }
    return CHILD_PASS;
}

/* a version string that is not a version */
static int case_bad_version(void)
{
    char *msg;
    size_t sz;
    int sd = -1;
    pmix_status_t reply;

    ack_version = "banana";
    if (PMIX_SUCCESS != start_server(&mymodule, true, getuid())) {
        return CHILD_SETUP;
    }
    msg = build_ack(PMIX_SIMPLE_CLIENT, AC_NSPACE, 0, "native", NULL, false, &sz);
    reply = send_ack(msg, sz, &sd);
    free(msg);
    return expect_status("a version that is not one", reply, PMIX_ERR_BAD_PARAM);
}

/* an accepted tool is a live process on its rank */
static int case_tool_counted(void)
{
    pmix_namespace_t *ns;
    pmix_rank_info_t *info;
    char *msg;
    size_t sz;
    int sd = -1;
    pmix_status_t reply;

    if (PMIX_SUCCESS != start_server(&mymodule, false, getuid())) {
        return CHILD_SETUP;
    }
    msg = build_ack(PMIX_TOOL_GIVEN_ID, AC_TOOLNS, 0, "native", NULL, false, &sz);
    reply = send_ack(msg, sz, &sd);
    free(msg);
    if (PMIX_SUCCESS != reply || PMIX_SUCCESS != (reply = tool_replies(sd))) {
        fprintf(stdout, "    the tool was refused: %s\n", PMIx_Error_string(reply));
        return CHILD_FAIL;
    }
    usleep(200000);
    ns = find_ns(AC_TOOLNS);
    if (NULL == ns || pmix_list_is_empty(&ns->ranks)) {
        return CHILD_SETUP;
    }
    info = (pmix_rank_info_t *) pmix_list_get_first(&ns->ranks);
    if (1 != info->proc_cnt) {
        fprintf(stdout, "    the tool's rank counts %d live processes\n", info->proc_cnt);
        return CHILD_FAIL;
    }
    return CHILD_PASS;
}

/* the clients-array entries serving a rank, and the rank's live count */
static int rank_peers(const char *nspace, pmix_rank_t rank, int *proc_cnt)
{
    pmix_namespace_t *ns = find_ns(nspace);
    pmix_rank_info_t *info;
    pmix_peer_t *pr;
    int n, count = 0;

    *proc_cnt = -1;
    if (NULL != ns) {
        PMIX_LIST_FOREACH (info, &ns->ranks, pmix_rank_info_t) {
            if (info->pname.rank == rank) {
                *proc_cnt = info->proc_cnt;
            }
        }
    }
    for (n = 0; n < pmix_server_globals.clients.size; n++) {
        pr = (pmix_peer_t *) pmix_pointer_array_get_item(&pmix_server_globals.clients, n);
        if (NULL != pr && NULL != pr->info && pr->info->pname.rank == rank &&
            PMIx_Check_nspace(pr->info->pname.nspace, nspace)) {
            ++count;
        }
    }
    return count;
}

/* A tool on a rank the host registered as a client finalizes and closes:
 * its connection is retired, and its rank's next connection takes its
 * place */
static int case_tool_client_retired(void)
{
    char *msg;
    size_t sz;
    int sd1 = -1, sd2 = -1, npeers, live;
    pmix_status_t reply;

    if (PMIX_SUCCESS != start_server(&mymodule, true, getuid())) {
        return CHILD_SETUP;
    }
    msg = build_ack(PMIX_TOOL_CLIENT, AC_NSPACE, 0, "native", NULL, false, &sz);
    reply = send_ack(msg, sz, &sd1);
    if (PMIX_SUCCESS == reply) {
        reply = tool_replies(sd1);
    }
    if (PMIX_SUCCESS != reply || !send_finalize(sd1, 0)) {
        free(msg);
        fprintf(stdout, "    the first tool did not connect and finalize: %s\n",
                PMIx_Error_string(reply));
        return CHILD_FAIL;
    }
    close(sd1);
    usleep(300000);
    npeers = rank_peers(AC_NSPACE, 0, &live);
    if (0 != live) {
        free(msg);
        fprintf(stdout, "    after the tool finalized and closed, its rank counts %d live "
                        "processes\n", live);
        return CHILD_FAIL;
    }
    reply = send_ack(msg, sz, &sd2);
    free(msg);
    if (PMIX_SUCCESS == reply) {
        reply = tool_replies(sd2);
    }
    if (PMIX_SUCCESS != reply) {
        fprintf(stdout, "    the rank's next tool was refused: %s\n", PMIx_Error_string(reply));
        return CHILD_FAIL;
    }
    usleep(200000);
    npeers = rank_peers(AC_NSPACE, 0, &live);
    if (1 != npeers || 1 != live) {
        fprintf(stdout, "    the rank has %d peers and counts %d live processes\n", npeers,
                live);
        return CHILD_FAIL;
    }
    return CHILD_PASS;
}

/* A rank's connection, and a second one from the same pid - as a program
 * the process exec'd would connect from - or from another. Either is
 * refused before the host is asked, and the first is left as it was.
 * `how`: 0, a client from the same pid; 1, a client from another pid; 2, a
 * tool from the same pid */
static int case_same_process(int how)
{
    pmix_byte_object_t blob1, blob2;
    pmix_status_t code = PMIX_ERR_LOST_CONNECTION, reply;
    pmix_namespace_t *ns;
    bool tool = (2 == how);
    uint8_t flag = tool ? PMIX_TOOL_GIVEN_ID : PMIX_SIMPLE_CLIENT;
    const char *nspace = tool ? AC_TOOLNS : AC_NSPACE;
    struct pollfd pfd;
    char *msg;
    size_t sz, nlocal = 0;
    int sd1 = -1, sd2 = -1, npeers, live, asked;
    uint32_t index;

    if (PMIX_SUCCESS != start_server(&mymodule, !tool, getuid()) || !build_blob(&blob1) ||
        !build_blob_pid(&blob2, (1 == how) ? AC_PID + 1 : AC_PID)) {
        return CHILD_SETUP;
    }
    PMIx_Register_event_handler(&code, 1, NULL, 0, lost_handler, NULL, NULL);
    msg = build_ack(flag, nspace, 0, "native", &blob1, false, &sz);
    reply = send_ack(msg, sz, &sd1);
    free(msg);
    if (PMIX_SUCCESS == reply) {
        reply = tool ? tool_replies(sd1) : (read_u32(sd1, &index) ? PMIX_SUCCESS
                                                                   : PMIX_ERR_UNREACH);
    }
    if (PMIX_SUCCESS != reply) {
        fprintf(stdout, "    the first connection was refused: %s\n", PMIx_Error_string(reply));
        return CHILD_FAIL;
    }
    usleep(100000);
    asked = nclient_connected + ntool_connected;
    if (NULL != (ns = find_ns(nspace))) {
        nlocal = ns->nlocalprocs;
    }

    msg = build_ack(flag, nspace, 0, "native", &blob2, false, &sz);
    reply = send_ack(msg, sz, &sd2);
    free(msg);
    if (PMIX_ERR_EXISTS != reply) {
        fprintf(stdout, "    the second connection read %s\n", PMIx_Error_string(reply));
        return CHILD_FAIL;
    }
    /* past the window a lost-connection event is gathered in */
    usleep(1500000);
    if (asked != nclient_connected + ntool_connected) {
        fprintf(stdout, "    the host was asked about the second connection\n");
        return CHILD_FAIL;
    }
    pfd.fd = sd1;
    pfd.events = POLLIN;
    pfd.revents = 0;
    if (0 != poll(&pfd, 1, 0)) {
        fprintf(stdout, "    the first connection was closed\n");
        return CHILD_FAIL;
    }
    if (0 != nlost) {
        fprintf(stdout, "    %d lost-connection events were raised\n", nlost);
        return CHILD_FAIL;
    }
    npeers = rank_peers(nspace, 0, &live);
    if (1 != npeers || 1 != live) {
        fprintf(stdout, "    the rank has %d peers and counts %d live processes\n", npeers,
                live);
        return CHILD_FAIL;
    }
    ns = find_ns(nspace);
    if (NULL == ns || nlocal != ns->nlocalprocs || 0 != ns->nfinalized) {
        fprintf(stdout, "    the job counts %zu local processes (was %zu), %zu finalized\n",
                (NULL == ns) ? 0 : ns->nlocalprocs, nlocal,
                (NULL == ns) ? 0 : ns->nfinalized);
        return CHILD_FAIL;
    }
    return CHILD_PASS;
}
static int run_same_pid_client(void)
{
    return case_same_process(0);
}
static int run_other_pid_client(void)
{
    return case_same_process(1);
}
static int run_same_pid_tool(void)
{
    return case_same_process(2);
}

/* A tool from the client's pid, on the client's rank, does not take the
 * client's rank over */
static int case_same_pid_other_kind(void)
{
    pmix_byte_object_t blob1, blob2;
    char *msg;
    size_t sz;
    int sd1 = -1, sd2 = -1;
    uint32_t index;
    pmix_status_t reply;

    if (PMIX_SUCCESS != start_server(&mymodule, true, getuid()) || !build_blob(&blob1) ||
        !build_blob(&blob2)) {
        return CHILD_SETUP;
    }
    msg = build_ack(PMIX_SIMPLE_CLIENT, AC_NSPACE, 0, "native", &blob1, false, &sz);
    reply = send_ack(msg, sz, &sd1);
    free(msg);
    if (PMIX_SUCCESS != reply || !read_u32(sd1, &index)) {
        return CHILD_SETUP;
    }
    usleep(100000);
    msg = build_ack(PMIX_TOOL_CLIENT, AC_NSPACE, 0, "native", &blob2, false, &sz);
    reply = send_ack(msg, sz, &sd2);
    free(msg);
    if (0 != ntool_connected) {
        fprintf(stdout, "    the host was asked about the tool\n");
        return CHILD_FAIL;
    }
    return expect_status("a tool from the client's pid on its rank", reply, PMIX_ERR_EXISTS);
}

/* answer the `n`th tool the host held back: `status`, and `id` if given,
 * else the identity it would have been given */
static void answer_tool(int n, pmix_status_t status, const pmix_proc_t *id)
{
    pmix_proc_t proc = (NULL == id) ? deferred_tool_id[n] : *id;

    deferred_tool_cb[n](status, &proc, deferred_tool_cbdata[n]);
}

/* the first status a tool reads, and if that is success, the rest */
static pmix_status_t tool_outcome(int sd, bool needs_id)
{
    char nspace[PMIX_MAX_NSLEN + 1];
    uint32_t u32;
    size_t got;
    ssize_t rd;

    if (!read_u32(sd, &u32)) {
        return PMIX_ERR_UNREACH;
    }
    if (PMIX_SUCCESS != (pmix_status_t) u32) {
        return (pmix_status_t) u32;
    }
    if (needs_id) {
        /* the identity it was given */
        for (got = 0; got < sizeof(nspace); got += (size_t) rd) {
            rd = read(sd, nspace + got, sizeof(nspace) - got);
            if (0 >= rd) {
                return PMIX_ERR_UNREACH;
            }
        }
        if (!read_u32(sd, &u32)) {
            return PMIX_ERR_UNREACH;
        }
    }
    return tool_replies(sd);
}

/* Tools for the host to decide on, held back until answered. The host
 * decides on one tool of a name at a time; tools of other names, and
 * tools it is to name, it is asked about together. `how`:
 *   0 - two tools of one name: the host is asked once, and the second is
 *       refused once the first is accepted;
 *   1 - the same, launched tools of a namespace the host did not register;
 *   2 - the host refuses the first: the second is then put to it, and
 *       accepted;
 *   3 - two tools of different names: the host is asked about both at
 *       once, and accepts both;
 *   4 - a tool the host is to name, and a tool of the name it then gives
 *       it, asked about together: the one answered second is refused;
 *   5 - a waiting tool that closes its connection is never put to the
 *       host;
 *   6 - a tool of a name, a second of that name waiting behind it, and a
 *       tool the host gives that name: the waiting one is refused at once,
 *       without the host being asked */
static int case_tool_queue(int how)
{
    pmix_proc_t named;
    char *msg;
    size_t sz;
    int sd1 = -1, sd2 = -1, sd3 = -1;
    pmix_status_t first, second, third;
    uint8_t flag = (1 == how) ? PMIX_TOOL_CLIENT : PMIX_TOOL_GIVEN_ID;
    int nasked = (3 == how || 4 == how) ? 2 : 1;

    host_mode = HOST_DEFERS_TOOLS;
    if (PMIX_SUCCESS != start_server(&mymodule, false, getuid())) {
        return CHILD_SETUP;
    }
    if (4 == how) {
        msg = build_ack(PMIX_TOOL_NEEDS_ID, "", 0, "native", NULL, false, &sz);
    } else {
        msg = build_ack(flag, AC_TOOLNS, 0, "native", NULL, false, &sz);
    }
    if (!open_ack(msg, sz, &sd1)) {
        return CHILD_SETUP;
    }
    free(msg);
    usleep(200000);
    msg = build_ack(flag, AC_TOOLNS, (3 == how) ? 1 : 0, "native", NULL, false, &sz);
    if (!open_ack(msg, sz, &sd2)) {
        return CHILD_SETUP;
    }
    free(msg);
    usleep(200000);
    if (6 == how) {
        /* a tool the host is to name, asked about at once */
        msg = build_ack(PMIX_TOOL_NEEDS_ID, "", 0, "native", NULL, false, &sz);
        if (!open_ack(msg, sz, &sd3)) {
            return CHILD_SETUP;
        }
        free(msg);
        usleep(200000);
        nasked = 2;
    }
    if (nasked != ntool_connected || nasked != ndeferred_tools) {
        fprintf(stdout, "    with two tools waiting, the host was asked %d times (want %d)\n",
                ntool_connected, nasked);
        return CHILD_FAIL;
    }
    if (6 == how) {
        /* the host gives the third tool the name the other two have */
        PMIX_LOAD_PROCID(&named, AC_TOOLNS, 0);
        answer_tool(1, PMIX_SUCCESS, &named);
        third = tool_outcome(sd3, true);
        if (PMIX_SUCCESS != third) {
            fprintf(stdout, "    the tool the host named read %s\n", PMIx_Error_string(third));
            return CHILD_FAIL;
        }
        second = tool_outcome(sd2, false);
        if (2 != ntool_connected) {
            fprintf(stdout, "    the host was asked about the waiting tool\n");
            return CHILD_FAIL;
        }
        if (PMIX_ERR_EXISTS != second) {
            return expect_status("the waiting tool of the taken name", second, PMIX_ERR_EXISTS);
        }
        answer_tool(0, PMIX_SUCCESS, NULL);
        first = tool_outcome(sd1, false);
        return expect_status("the first tool of the taken name", first, PMIX_ERR_EXISTS);
    }
    if (4 == how) {
        PMIX_LOAD_PROCID(&named, AC_TOOLNS, 0);
        answer_tool(0, PMIX_SUCCESS, &named);
        first = tool_outcome(sd1, true);
        if (PMIX_SUCCESS != first) {
            fprintf(stdout, "    the tool the host named read %s\n", PMIx_Error_string(first));
            return CHILD_FAIL;
        }
        answer_tool(1, PMIX_SUCCESS, NULL);
        second = tool_outcome(sd2, false);
        return expect_status("the tool of the name given meanwhile", second, PMIX_ERR_EXISTS);
    }
    if (3 == how) {
        answer_tool(1, PMIX_SUCCESS, NULL);
        answer_tool(0, PMIX_SUCCESS, NULL);
        first = tool_outcome(sd1, false);
        second = tool_outcome(sd2, false);
        if (PMIX_SUCCESS != first) {
            return expect_status("the first tool", first, PMIX_SUCCESS);
        }
        return expect_status("the second tool", second, PMIX_SUCCESS);
    }
    if (5 == how) {
        close(sd2);
        usleep(200000);
    }
    if (2 == how || 5 == how) {
        answer_tool(0, PMIX_ERR_NO_PERMISSIONS, NULL);
    } else {
        answer_tool(0, PMIX_SUCCESS, NULL);
    }
    first = tool_outcome(sd1, false);
    if (2 == how || 5 == how) {
        if (PMIX_ERR_NO_PERMISSIONS != first) {
            fprintf(stdout, "    the refused tool read %s\n", PMIx_Error_string(first));
            return CHILD_FAIL;
        }
    } else if (PMIX_SUCCESS != first) {
        fprintf(stdout, "    the first tool read %s\n", PMIx_Error_string(first));
        return CHILD_FAIL;
    }
    usleep(200000);
    if (5 == how) {
        if (1 != ntool_connected) {
            fprintf(stdout, "    the host was asked about a tool that had gone\n");
            return CHILD_FAIL;
        }
        return CHILD_PASS;
    }
    if (0 == how || 1 == how) {
        second = tool_outcome(sd2, false);
        if (1 != ntool_connected) {
            fprintf(stdout, "    the host was asked about the second tool too\n");
            return CHILD_FAIL;
        }
        return expect_status("the second tool of the name", second, PMIX_ERR_EXISTS);
    }
    /* the second tool is the host's to decide on now */
    if (2 != ntool_connected || 2 != ndeferred_tools) {
        fprintf(stdout, "    after the first answer, the host was asked %d times\n",
                ntool_connected);
        return CHILD_FAIL;
    }
    answer_tool(1, PMIX_SUCCESS, NULL);
    second = tool_outcome(sd2, false);
    return expect_status("the second tool", second, PMIX_SUCCESS);
}
/* Tools waiting on the host hold their connections, and have a cap of
 * their own, the pending-connection cap's size: with two waiting at a cap
 * of two, a third is refused before the host is asked */
static int case_tool_cap(void)
{
    char *msg;
    size_t sz;
    int sd[3] = {-1, -1, -1}, n;
    pmix_status_t third;

    setenv("PMIX_MCA_ptl_base_max_pending_connections", "2", 1);
    host_mode = HOST_DEFERS_TOOLS;
    if (PMIX_SUCCESS != start_server(&mymodule, false, getuid())) {
        return CHILD_SETUP;
    }
    for (n = 0; n < 2; n++) {
        msg = build_ack(PMIX_TOOL_GIVEN_ID, AC_TOOLNS, n, "native", NULL, false, &sz);
        if (!open_ack(msg, sz, &sd[n])) {
            return CHILD_SETUP;
        }
        free(msg);
        usleep(200000);
    }
    if (2 != ntool_connected) {
        fprintf(stdout, "    the host was asked about %d tools, not 2\n", ntool_connected);
        return CHILD_SETUP;
    }
    msg = build_ack(PMIX_TOOL_GIVEN_ID, AC_TOOLNS, 2, "native", NULL, false, &sz);
    third = send_ack(msg, sz, &sd[2]);
    free(msg);
    if (2 != ntool_connected) {
        fprintf(stdout, "    the host was asked about the third tool\n");
        return CHILD_FAIL;
    }
    return expect_status("a third tool at the cap", third, PMIX_ERR_OUT_OF_RESOURCE);
}

/* A tool waiting on the host does not count toward the cap the listener
 * keeps on connections still sending their connect-ack: with one tool
 * waiting at a cap of two, two connections that have sent nothing are
 * both held, and only a third pushes out the oldest of them. The tool is
 * never dropped */
static int case_tool_cap_listener(void)
{
    char *msg;
    size_t sz;
    int tsd = -1, a = -1, b = -1, c = -1;

    setenv("PMIX_MCA_ptl_base_max_pending_connections", "2", 1);
    host_mode = HOST_DEFERS_TOOLS;
    if (PMIX_SUCCESS != start_server(&mymodule, false, getuid())) {
        return CHILD_SETUP;
    }
    msg = build_ack(PMIX_TOOL_GIVEN_ID, AC_TOOLNS, 0, "native", NULL, false, &sz);
    if (!open_ack(msg, sz, &tsd)) {
        return CHILD_SETUP;
    }
    free(msg);
    usleep(200000);
    if (1 != ntool_connected || !connect_server(&a)) {
        return CHILD_SETUP;
    }
    usleep(200000);
    if (!connect_server(&b)) {
        return CHILD_SETUP;
    }
    if (closes_within(a, 1000)) {
        fprintf(stdout, "    the waiting tool counted toward the cap - an unfinished "
                        "connection was dropped below it\n");
        return CHILD_FAIL;
    }
    if (!connect_server(&c)) {
        return CHILD_SETUP;
    }
    if (!closes_within(a, 2000)) {
        fprintf(stdout, "    the oldest unfinished connection was kept at the cap\n");
        return CHILD_FAIL;
    }
    if (closes_within(b, 200)) {
        fprintf(stdout, "    a newer unfinished connection was dropped\n");
        return CHILD_FAIL;
    }
    if (closes_within(tsd, 200)) {
        fprintf(stdout, "    the waiting tool was dropped\n");
        return CHILD_FAIL;
    }
    return CHILD_PASS;
}

static int run_queue_same_name(void)
{
    return case_tool_queue(0);
}
static int run_queue_same_launched(void)
{
    return case_tool_queue(1);
}
static int run_queue_after_refusal(void)
{
    return case_tool_queue(2);
}
static int run_queue_other_names(void)
{
    return case_tool_queue(3);
}
static int run_queue_assigned_name(void)
{
    return case_tool_queue(4);
}
static int run_queue_gone(void)
{
    return case_tool_queue(5);
}
static int run_queue_conflict_at_once(void)
{
    return case_tool_queue(6);
}

/* A tool waits for the host's answer for ptl_base_handshake_wait_time -
 * two seconds here - from when it sent its request, and time it spent
 * waiting behind a tool of its name counts. `how`:
 *   0 - the tool ahead is refused after a second: the host is given what
 *       is left, one second;
 *   1 - the tool ahead is refused after the whole wait: the waiting tool
 *       is refused with PMIX_ERR_TIMEOUT, and the host is not asked */
static int case_wait_counts(int how)
{
    char *msg;
    size_t sz;
    int sd1 = -1, sd2 = -1;
    pmix_status_t reply;

    setenv("PMIX_MCA_ptl_base_handshake_wait_time", "2", 1);
    host_mode = HOST_DEFERS_TOOLS;
    max_deferred = 1;
    if (PMIX_SUCCESS != start_server(&mymodule, false, getuid())) {
        return CHILD_SETUP;
    }
    msg = build_ack(PMIX_TOOL_GIVEN_ID, AC_TOOLNS, 0, "native", NULL, false, &sz);
    if (!open_ack(msg, sz, &sd1) || !open_ack(msg, sz, &sd2)) {
        return CHILD_SETUP;
    }
    free(msg);
    usleep((0 == how) ? 1200000 : 2300000);
    if (1 != ntool_connected) {
        fprintf(stdout, "    the host was asked about %d tools of one name\n", ntool_connected);
        return CHILD_FAIL;
    }
    answer_tool(0, PMIX_ERR_NO_PERMISSIONS, NULL);
    reply = tool_outcome(sd2, false);
    if (1 == how) {
        if (1 != ntool_connected) {
            fprintf(stdout, "    the host was asked about a tool whose wait was over\n");
            return CHILD_FAIL;
        }
        return expect_status("the tool whose wait was over", reply, PMIX_ERR_TIMEOUT);
    }
    if (2 != ntool_connected || 1 != host_timeout) {
        fprintf(stdout, "    the host was asked %d times, the last to answer in %d seconds\n",
                ntool_connected, host_timeout);
        return CHILD_FAIL;
    }
    return expect_status("the waiting tool", reply, PMIX_SUCCESS);
}
static int run_wait_left(void)
{
    return case_wait_counts(0);
}
static int run_wait_over(void)
{
    return case_wait_counts(1);
}

/* A host that is slow, or a process that goes, while the host decides.
 * The server asks the host to answer within ptl_base_handshake_wait_time
 * - one second here - and waits two more. `how`:
 *   0 - a client the host answers late: it reads PMIX_ERR_TIMEOUT, its rank
 *       is held until the answer, and the late yes is followed by telling
 *       the host the client is not connected;
 *   1 - the same for a tool, and a second tool of its name is not put to
 *       the host until the late answer is in;
 *   2 - a client the host answers no to late: nothing more is said;
 *   3 - a client that goes while the host decides, and the host says yes:
 *       the host is told it is not connected;
 *   4 - the same for a tool */
static int case_host_late(int how)
{
    pmix_status_t code = PMIX_ERR_LOST_CONNECTION, reply;
    bool tool = (1 == how || 4 == how);
    const char *nspace = tool ? AC_TOOLNS : AC_NSPACE;
    char *msg;
    size_t sz;
    int sd = -1, sd2 = -1;
    uint32_t index;

    setenv("PMIX_MCA_ptl_base_handshake_wait_time", "1", 1);
    host_mode = tool ? HOST_DEFERS_TOOLS : HOST_DEFERS;
    if (PMIX_SUCCESS != start_server(&mymodule, !tool, getuid())) {
        return CHILD_SETUP;
    }
    PMIx_Register_event_handler(&code, 1, NULL, 0, lost_handler, NULL, NULL);
    msg = build_ack(tool ? PMIX_TOOL_GIVEN_ID : PMIX_SIMPLE_CLIENT, nspace, 0, "native", NULL,
                    false, &sz);
    if (!open_ack(msg, sz, &sd)) {
        return CHILD_SETUP;
    }
    usleep(300000);
    if (1 != ndeferred + ndeferred_tools) {
        return CHILD_SETUP;
    }
    if (1 != host_timeout) {
        fprintf(stdout, "    the host was asked to answer in %d seconds\n", host_timeout);
        return CHILD_FAIL;
    }
    if (3 == how || 4 == how) {
        /* the process goes; the host then says yes */
        close(sd);
        usleep(200000);
        if (tool) {
            answer_tool(0, PMIX_SUCCESS, NULL);
        } else {
            deferred_cb[0](PMIX_SUCCESS, deferred_cbdata[0]);
        }
    } else {
        reply = tool ? tool_outcome(sd, false)
                     : (read_u32(sd, &index) ? (pmix_status_t) index : PMIX_ERR_UNREACH);
        if (PMIX_ERR_TIMEOUT != reply) {
            fprintf(stdout, "    the process waiting on a slow host read %s\n",
                    PMIx_Error_string(reply));
            return CHILD_FAIL;
        }
        /* another connection of the name, while the answer is awaited */
        if (!open_ack(msg, sz, &sd2)) {
            return CHILD_SETUP;
        }
        usleep(300000);
        if (tool) {
            if (1 != ntool_connected) {
                fprintf(stdout, "    a second tool of the name was put to the host\n");
                return CHILD_FAIL;
            }
        } else if (!read_u32(sd2, &index) || PMIX_ERR_EXISTS != (pmix_status_t) index) {
            fprintf(stdout, "    a second client of the rank was not refused\n");
            return CHILD_FAIL;
        }
        if (tool) {
            answer_tool(0, PMIX_SUCCESS, NULL);
        } else {
            deferred_cb[0]((2 == how) ? PMIX_ERR_NO_PERMISSIONS : PMIX_SUCCESS,
                           deferred_cbdata[0]);
        }
    }
    free(msg);
    /* past the window a lost-connection event is gathered in: a process
     * that went may only be found gone once it was accepted */
    usleep(1500000);
    if (2 == how) {
        if (0 != nlost) {
            fprintf(stdout, "    the host was told of a client it refused\n");
            return CHILD_FAIL;
        }
        return CHILD_PASS;
    }
    if (1 != nlost || !PMIx_Check_nspace(lost_source.nspace, nspace) || 0 != lost_source.rank) {
        fprintf(stdout, "    the host was told of %d lost connections (last %s:%u)\n", nlost,
                lost_source.nspace, lost_source.rank);
        return CHILD_FAIL;
    }
    if (1 == how && 2 != ntool_connected) {
        fprintf(stdout, "    after the late answer, the waiting tool was not put to the host\n");
        return CHILD_FAIL;
    }
    return CHILD_PASS;
}
static int run_late_client(void)
{
    return case_host_late(0);
}
static int run_late_tool(void)
{
    return case_host_late(1);
}
static int run_late_no(void)
{
    return case_host_late(2);
}
static int run_gone_client(void)
{
    return case_host_late(3);
}
static int run_gone_tool(void)
{
    return case_host_late(4);
}

/* a host that breaks the one-answer contract */
static int case_contract(int mode)
{
    char *msg;
    size_t sz;
    int sd = -1;
    uint32_t index;
    pmix_status_t reply;

    host_mode = mode;
    if (PMIX_SUCCESS != start_server(&mymodule, HOST_SUCCEEDS_NO_ID != mode, getuid())) {
        return CHILD_SETUP;
    }
    if (HOST_SUCCEEDS_NO_ID == mode) {
        msg = build_ack(PMIX_TOOL_NEEDS_ID, "", 0, "native", NULL, false, &sz);
    } else {
        msg = build_ack(PMIX_SIMPLE_CLIENT, AC_NSPACE, 0, "native", NULL, false, &sz);
    }
    reply = send_ack(msg, sz, &sd);
    free(msg);
    /* give anything the host's extra answer left queued time to run */
    usleep(300000);
    switch (mode) {
    case HOST_CALLS_BACK_THEN_ERRS:
        return expect_status("callback, then an error", reply, PMIX_ERR_NO_PERMISSIONS);
    case HOST_CALLS_BACK_TWICE:
        if (PMIX_SUCCESS != reply || !read_u32(sd, &index)) {
            fprintf(stdout, "    a client answered twice was refused: %s\n",
                    PMIx_Error_string(reply));
            return CHILD_FAIL;
        }
        return CHILD_PASS;
    default:
        return expect_status("success with no identity", reply, PMIX_ERR_NOT_SUPPORTED);
    }
}

/* Stop the server's reads of the namespace's connections, on the progress
 * thread where they run, so a close on one is not seen by them */
typedef struct {
    pmix_event_t ev;
    pmix_lock_t lock;
} hold_t;

static void hold_fn(int sd, short args, void *cbdata)
{
    hold_t *h = (hold_t *) cbdata;
    pmix_peer_t *pr;
    int n;
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    for (n = 0; n < pmix_server_globals.clients.size; n++) {
        pr = (pmix_peer_t *) pmix_pointer_array_get_item(&pmix_server_globals.clients, n);
        if (NULL != pr && NULL != pr->info && pr->recv_ev_active &&
            PMIx_Check_nspace(pr->info->pname.nspace, AC_NSPACE)) {
            pmix_event_del(&pr->recv_event);
            pr->recv_ev_active = false;
        }
    }
    PMIX_WAKEUP_THREAD(&h->lock);
}

static void hold_reads(void)
{
    hold_t h;

    PMIX_CONSTRUCT_LOCK(&h.lock);
    PMIX_THREADSHIFT(&h, hold_fn);
    PMIX_WAIT_THREAD(&h.lock);
    PMIX_DESTRUCT_LOCK(&h.lock);
}

/* A second connection for a rank. `how`: 0, while the first is live -
 * refused; 1, after the first was lost; 2, after the first finalized, its
 * socket still open; 3, after the first was lost, its close unread by the
 * server - all three accepted */
static int case_second(int how)
{
    char *msg;
    size_t sz;
    int sd1 = -1, sd2 = -1;
    uint32_t index1, index2;
    pmix_status_t reply;

    if (PMIX_SUCCESS != start_server(&mymodule, true, getuid())) {
        return CHILD_SETUP;
    }
    msg = build_ack(PMIX_SIMPLE_CLIENT, AC_NSPACE, 0, "native", NULL, false, &sz);
    reply = send_ack(msg, sz, &sd1);
    if (PMIX_SUCCESS != reply || !read_u32(sd1, &index1)) {
        free(msg);
        fprintf(stdout, "    the first connection was refused: %s\n", PMIx_Error_string(reply));
        return CHILD_FAIL;
    }
    if (1 == how || 3 == how) {
        if (3 == how) {
            hold_reads();
        }
        close(sd1);
        sd1 = -1;
        /* let the server see it go - or, with its reads held, not */
        usleep(300000);
    } else if (2 == how && !send_finalize(sd1, index1)) {
        free(msg);
        fprintf(stdout, "    the first connection's finalize was not answered\n");
        return CHILD_FAIL;
    }
    reply = send_ack(msg, sz, &sd2);
    free(msg);
    if (0 == how) {
        if (PMIX_ERR_EXISTS != reply || 1 != nclient_connected) {
            fprintf(stdout, "    a second connection read %s, and the host was asked %d times\n",
                    PMIx_Error_string(reply), nclient_connected);
            return CHILD_FAIL;
        }
        return CHILD_PASS;
    }
    if (PMIX_SUCCESS != reply || !read_u32(sd2, &index2)) {
        fprintf(stdout, "    a rank connecting again was refused: %s\n",
                PMIx_Error_string(reply));
        return CHILD_FAIL;
    }
    if (2 != nclient_connected) {
        fprintf(stdout, "    the host was asked %d times\n", nclient_connected);
        return CHILD_FAIL;
    }
    return CHILD_PASS;
}

typedef int (*case_fn_t)(void);

static int run_bool_false_psec(void)
{
    return case_unknown_psec(false);
}
static int run_bool_true_psec(void)
{
    return case_unknown_psec(true);
}
static int run_host_info_client(void)
{
    return case_host_info(false);
}
static int run_host_info_tool(void)
{
    return case_host_info(true);
}
static int run_contract_errs(void)
{
    return case_contract(HOST_CALLS_BACK_THEN_ERRS);
}
static int run_contract_twice(void)
{
    return case_contract(HOST_CALLS_BACK_TWICE);
}
static int run_contract_noid(void)
{
    return case_contract(HOST_SUCCEEDS_NO_ID);
}
static int run_limit_size(void)
{
    return case_listener_limit(0);
}
static int run_limit_time(void)
{
    return case_listener_limit(1);
}
static int run_limit_cap(void)
{
    return case_listener_limit(2);
}
static int run_second_live(void)
{
    return case_second(0);
}
static int run_second_after_loss(void)
{
    return case_second(1);
}
static int run_second_after_finalize(void)
{
    return case_second(2);
}
static int run_second_close_unread(void)
{
    return case_second(3);
}

static void run_case(const char *name, case_fn_t fn)
{
    pid_t pid;
    int status, rc;
    char detail[64];

    fflush(stdout);
    fflush(stderr);
    pid = fork();
    if (0 == pid) {
        alarm(30);
        rc = fn();
        /* no finalize: the server would wait on peers this test never runs */
        fflush(stdout);
        _exit(rc);
    }
    if (0 > pid || pid != waitpid(pid, &status, 0)) {
        report(name, 0, "fork/wait failed");
    } else if (WIFSIGNALED(status)) {
        snprintf(detail, sizeof(detail), "server died on signal %d", WTERMSIG(status));
        report(name, 0, detail);
    } else if (CHILD_SETUP == WEXITSTATUS(status)) {
        report(name, 0, "test setup failed");
    } else {
        report(name, CHILD_PASS == WEXITSTATUS(status), "see above");
    }
}

/* Hold the server's progress thread for half a second, so that what a
 * case does meanwhile is all waiting when it next runs */
typedef struct {
    pmix_event_t ev;
} progress_hold_t;
static void hold_progress_cb(int sd, short args, void *cbdata)
{
    PMIX_HIDE_UNUSED_PARAMS(sd, args, cbdata);
    usleep(500000);
}
static void hold_progress(void)
{
    static progress_hold_t hold;

    PMIX_THREADSHIFT(&hold, hold_progress_cb);
    usleep(50000);
}

/* The host is told that a process it accepted is not connected - and when
 * it is told, nothing connected has that name: the name is held until the
 * notice has been delivered. `how`:
 *   0 - a tool the host says yes to late, with a second tool of its name
 *       waiting behind it, which the host accepts at once;
 *   1 - a tool that resets its connection while the host decides, so
 *       the replies to the host's yes cannot be sent, the same;
 *   2 - a client the host says yes to late, while a second client of its
 *       rank arrives with the answer */
static int case_notice_held(int how)
{
    pmix_status_t code = PMIX_ERR_LOST_CONNECTION, reply;
    pmix_byte_object_t blobA, blobB;
    struct linger lg = {1, 0};
    bool tool = (2 != how);
    const char *nspace = tool ? AC_TOOLNS : AC_NSPACE;
    char *msgA, *msgB;
    size_t szA, szB;
    int sdA = -1, sdB = -1;
    uint32_t u32;

    /* The second client's wait counts from its accept, and it spends
     * 0.75s of it connected before the server reads its request - the
     * pause, then the held progress thread. A second of it left that a
     * quarter-second short of a refusal on a loaded host, so it is given
     * two */
    if (0 == how) {
        setenv("PMIX_MCA_ptl_base_handshake_wait_time", "1", 1);
    } else if (2 == how) {
        setenv("PMIX_MCA_ptl_base_handshake_wait_time", "2", 1);
    }
    host_mode = tool ? HOST_DEFERS_TOOLS : HOST_DEFERS;
    max_deferred = 1;
    if (PMIX_SUCCESS != start_server(&mymodule, !tool, getuid()) || !build_blob(&blobA) ||
        !build_blob_pid(&blobB, AC_PID + 1)) {
        return CHILD_SETUP;
    }
    PMIx_Register_event_handler(&code, 1, NULL, 0, lost_handler, NULL, NULL);
    if (tool) {
        msgA = build_ack(PMIX_TOOL_GIVEN_ID, nspace, 0, "native", NULL, false, &szA);
        if (!open_ack(msgA, szA, &sdA)) {
            return CHILD_SETUP;
        }
        if (0 == how) {
            reply = tool_outcome(sdA, false);
            if (PMIX_ERR_TIMEOUT != reply) {
                fprintf(stdout, "    the first tool read %s\n", PMIx_Error_string(reply));
                return CHILD_FAIL;
            }
        } else {
            usleep(300000);
            setsockopt(sdA, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg));
            close(sdA);
        }
        /* the second tool of the name waits behind the first */
        if (!open_ack(msgA, szA, &sdB)) {
            return CHILD_SETUP;
        }
        usleep(300000);
        if (1 != ntool_connected) {
            fprintf(stdout, "    the second tool was put to the host before the first was "
                            "answered\n");
            return CHILD_FAIL;
        }
        answer_tool(0, PMIX_SUCCESS, NULL);
        reply = tool_outcome(sdB, false);
        free(msgA);
    } else {
        msgA = build_ack(PMIX_SIMPLE_CLIENT, nspace, 0, "native", &blobA, false, &szA);
        msgB = build_ack(PMIX_SIMPLE_CLIENT, nspace, 0, "native", &blobB, false, &szB);
        if (!open_ack(msgA, szA, &sdA) || !read_u32(sdA, &u32)) {
            return CHILD_SETUP;
        }
        if (PMIX_ERR_TIMEOUT != (pmix_status_t) u32) {
            fprintf(stdout, "    the first client read %s\n",
                    PMIx_Error_string((pmix_status_t) u32));
            return CHILD_FAIL;
        }
        if (!connect_server(&sdB)) {
            return CHILD_SETUP;
        }
        usleep(200000);
        /* the second client's request and the host's late yes reach the
         * server together */
        hold_progress();
        if ((ssize_t) szB != write(sdB, msgB, szB)) {
            return CHILD_SETUP;
        }
        deferred_cb[0](PMIX_SUCCESS, deferred_cbdata[0]);
        reply = read_u32(sdB, &u32) ? (pmix_status_t) u32 : PMIX_ERR_UNREACH;
        if (PMIX_SUCCESS == reply && !read_u32(sdB, &u32)) {
            reply = PMIX_ERR_UNREACH;
        }
        free(msgA);
        free(msgB);
    }
    /* past the window a lost-connection event is gathered in */
    usleep(1500000);
    if (2 != how && PMIX_SUCCESS != reply) {
        fprintf(stdout, "    the second tool read %s\n", PMIx_Error_string(reply));
        return CHILD_FAIL;
    }
    if (2 == how && PMIX_SUCCESS != reply && PMIX_ERR_EXISTS != reply) {
        fprintf(stdout, "    the second client read %s\n", PMIx_Error_string(reply));
        return CHILD_FAIL;
    }
    if (1 != nlost || !PMIx_Check_nspace(lost_source.nspace, nspace) || 0 != lost_source.rank) {
        fprintf(stdout, "    the host was told of %d lost connections (last %s:%u)\n", nlost,
                lost_source.nspace, lost_source.rank);
        return CHILD_FAIL;
    }
    if (0 != live_at_lost) {
        fprintf(stdout, "    the notice reached the host while %d live connection(s) had the "
                        "name\n",
                live_at_lost);
        return CHILD_FAIL;
    }
    return CHILD_PASS;
}
static int run_notice_late_tool(void)
{
    return case_notice_held(0);
}
static int run_notice_gone_tool(void)
{
    return case_notice_held(1);
}
static int run_notice_late_client(void)
{
    return case_notice_held(2);
}

/* A second connection for a connected rank, from the same process, that
 * fails before the server can answer it: the first is left as it was, and
 * the host hears nothing */
static int case_second_fails(void)
{
    pmix_status_t code = PMIX_ERR_LOST_CONNECTION, reply;
    pmix_byte_object_t blob;
    struct linger lg = {1, 0};
    struct pollfd pfd;
    char *msg;
    size_t sz;
    int sd1 = -1, sd2 = -1, npeers, live;
    uint32_t index;

    if (PMIX_SUCCESS != start_server(&mymodule, true, getuid()) || !build_blob(&blob)) {
        return CHILD_SETUP;
    }
    PMIx_Register_event_handler(&code, 1, NULL, 0, lost_handler, NULL, NULL);
    msg = build_ack(PMIX_SIMPLE_CLIENT, AC_NSPACE, 0, "native", &blob, false, &sz);
    reply = send_ack(msg, sz, &sd1);
    if (PMIX_SUCCESS != reply || !read_u32(sd1, &index)) {
        return CHILD_SETUP;
    }
    usleep(100000);
    /* the new connection's request and its reset reach the server together */
    hold_progress();
    if (!open_ack(msg, sz, &sd2)) {
        return CHILD_SETUP;
    }
    free(msg);
    setsockopt(sd2, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg));
    close(sd2);
    usleep(1500000);
    if (0 != nlost) {
        fprintf(stdout, "    the host was told of %d lost connections\n", nlost);
        return CHILD_FAIL;
    }
    npeers = rank_peers(AC_NSPACE, 0, &live);
    if (1 != npeers || 1 != live) {
        fprintf(stdout, "    the rank has %d peers and counts %d live processes\n", npeers,
                live);
        return CHILD_FAIL;
    }
    pfd.fd = sd1;
    pfd.events = POLLIN;
    pfd.revents = 0;
    if (0 != poll(&pfd, 1, 0)) {
        fprintf(stdout, "    the earlier connection was closed\n");
        return CHILD_FAIL;
    }
    return CHILD_PASS;
}

/* A process whose finalize the host has yet to answer still holds its
 * rank: a second connection from it is refused, its finalize is counted
 * once, and every local process of the job finalizing is noticed */
static int case_finalizing_holds(void)
{
    pmix_byte_object_t blob;
    pmix_namespace_t *ns;
    pmix_nspace_t nsname;
    pmix_status_t reply;
    char *msg;
    size_t sz;
    int sd1 = -1, sd2 = -1;
    uint32_t index1;

    if (PMIX_SUCCESS != start_server(&finmodule, true, getuid()) || !build_blob(&blob)) {
        return CHILD_SETUP;
    }
    msg = build_ack(PMIX_SIMPLE_CLIENT, AC_NSPACE, 0, "native", &blob, false, &sz);
    reply = send_ack(msg, sz, &sd1);
    if (PMIX_SUCCESS != reply || !read_u32(sd1, &index1)) {
        return CHILD_SETUP;
    }
    usleep(100000);
    /* the finalize, which the host holds */
    if (!send_finalize_request(sd1, index1)) {
        return CHILD_SETUP;
    }
    usleep(200000);
    /* the same process connects again */
    reply = send_ack(msg, sz, &sd2);
    free(msg);
    if (PMIX_ERR_EXISTS != reply) {
        fprintf(stdout, "    the second connection read %s\n", PMIx_Error_string(reply));
        return CHILD_FAIL;
    }
    /* the host answers the finalize */
    finalize_cb(PMIX_SUCCESS, finalize_cbdata);
    usleep(200000);
    close(sd1);
    usleep(300000);
    ns = find_ns(AC_NSPACE);
    if (NULL == ns || ns->nfinalized != ns->nlocalprocs) {
        fprintf(stdout, "    the job counts %zu finalized of %zu local processes\n",
                (NULL == ns) ? 0 : ns->nfinalized, (NULL == ns) ? 0 : ns->nlocalprocs);
        return CHILD_FAIL;
    }
    PMIX_RETAIN(ns);
    PMIX_LOAD_NSPACE(nsname, AC_NSPACE);
    PMIx_server_deregister_nspace(nsname, NULL, NULL);
    usleep(300000);
    if (!ns->local_app_fini_fired) {
        fprintf(stdout, "    every local process finalizing was not noticed\n");
        return CHILD_FAIL;
    }
    return CHILD_PASS;
}

/* The servers here exit without finalizing, which leaves their socket
 * directories and rendezvous files behind: give them a directory of their
 * own, removed when the test ends */
static char scratch[256];

static int scratch_rm(const char *path, const struct stat *st, int flag, struct FTW *ftw)
{
    PMIX_HIDE_UNUSED_PARAMS(st, flag, ftw);
    return remove(path);
}

static void scratch_done(void)
{
    if ('\0' != scratch[0]) {
        (void) nftw(scratch, scratch_rm, 16, FTW_DEPTH | FTW_PHYS);
    }
}

static bool scratch_start(void)
{
    const char *base = getenv("TMPDIR");

    snprintf(scratch, sizeof(scratch), "%s/pmix_ut.XXXXXX", (NULL == base) ? "/tmp" : base);
    if (NULL == mkdtemp(scratch)) {
        scratch[0] = '\0';
        return false;
    }
    setenv("TMPDIR", scratch, 1);
    atexit(scratch_done);
    return true;
}

int main(int argc, char **argv)
{
    PMIX_HIDE_UNUSED_PARAMS(argc, argv);
#if PMIX_ENABLE_LEGACY_PEERS
    /* these cases are about the TCP listener, which a server opens for
     * clients and tools of earlier releases */
    setenv("PMIX_MCA_ptl_base_legacy_peers", "1", 1);
#else
    fprintf(stdout, "this PMIx takes no clients or tools of earlier releases, so a "
                    "server has no TCP listener for these cases\n");
    return 77;
#endif
    if (!scratch_start()) {
        fprintf(stderr, "could not make a scratch directory\n");
        return 1;
    }

    setvbuf(stdout, NULL, _IONBF, 0);
    fprintf(stdout, "\n=== how a server accepts a connection ===\n\n");
    /* the credential here is psec/native's, so select it */
    setenv("PMIX_MCA_psec", "native", 1);
    memset(&tool_answer, 0, sizeof(tool_answer));

    run_case("an unregistered namespace is refused with a status", case_unknown_nspace);
    run_case("an unregistered rank is refused with a status", case_unknown_rank);
    run_case("a client asking for a missing security module is told so", run_bool_false_psec);
    run_case("a tool asking for a missing security module is told so", run_bool_true_psec);
    run_case("a connect-ack cut short is refused with a status", case_truncated);
    run_case("a client the host refuses is told the host's reason", case_host_refuses);
    run_case("a tool needing an identity no host can give is told so", case_tool_no_upcall);
    run_case("a client the host refuses changes nothing shared", case_refused_changes_nothing);
    run_case("a refused second connection leaves the rank on its live one",
             case_refused_second);
    run_case("a tool joining a known namespace neither renames it nor duplicates a rank",
             case_tool_joins);
    run_case("a client is given to the host with its credential type and pid, its own uid "
             "claim ignored",
             run_host_info_client);
    run_case("a tool is given to the host with its credential type", run_host_info_tool);
    run_case("a server without tool support refuses a tool before the host is asked",
             case_no_tool_support);
    run_case("a client on the local transport is the kernel's uid, gid and pid",
             run_local_identity_client);
    run_case("a tool on the local transport is the kernel's uid, gid and pid",
             run_local_identity_tool);
    run_case("a client on the local transport claiming another user's uid is refused",
             case_local_claim);
    run_case("a client whose connection closes while its process runs on is lost at once",
             case_closed_is_lost);
    run_case("a live client on the local socket is not taken over by a pid sent over TCP",
             case_live_local_tcp_pid);
    run_case("a tool that disconnects is lost at once", case_tool_lost);
    run_case("a client the server drops is lost at once", case_server_drop_lost);
    run_case("a tool naming the server's keys twice has the host given the server's alone",
             case_tool_dup_keys);
    run_case("a connect-ack too large to accept is refused with a status", run_limit_size);
    run_case("a connect-ack that does not arrive in time is refused with a status",
             run_limit_time);
    run_case("a connection pushed out at the cap is told so", run_limit_cap);
    run_case("a client the host deregisters as it accepts is refused, and reported lost",
             case_client_rank_dereg);
    run_case("a tool whose rank the host deregisters during the up-call is refused",
             case_tool_rank_dereg);
    run_case("of two connections waiting on the host, the second is refused",
             case_both_waiting);
    run_case("a bad credential is refused as such before its info blob is read",
             case_badcred_bad_blob);
    run_case("a tool the host accepts without an identity is refused", case_tool_no_identity);
    run_case("a v2.1 tool is sent the replies its reader takes", case_legacy_tool);
    run_case("a connect-ack whose version is not a version is refused", case_bad_version);
    run_case("an accepted tool counts as a live process on its rank", case_tool_counted);
    run_case("a host that calls back and then returns an error refuses with the error",
             run_contract_errs);
    run_case("a host that calls back twice accepts the client once", run_contract_twice);
    run_case("a tool up-call that succeeds with no identity refuses the tool", run_contract_noid);
    run_case("a second connection for a live rank is refused before the host is asked",
             run_second_live);
    run_case("a rank whose connection was lost can connect again", run_second_after_loss);
    run_case("a rank whose connection finalized can connect again", run_second_after_finalize);
    run_case("a rank whose lost connection's close is unread can connect again",
             run_second_close_unread);
    run_case("a tool naming a rank a live tool holds is refused before the host is asked",
             run_tool_second_tool);
    run_case("a tool naming a connected client's rank is refused before the host is asked",
             run_tool_second_client);
    run_case("a client whose rank a tool has taken is refused before the host is asked",
             run_client_second_tool);
    run_case("a tool naming a connected client's rank is told the rank is taken, whatever "
             "the client's wire format",
             run_tool_second_client_format);
    run_case("a tool bringing its own identity that names no namespace or rank is refused",
             case_tool_names_nobody);
    run_case("a tool naming a new rank in its own user's namespace is given it",
             run_new_rank_own);
    run_case("registering a rank a tool holds, for the tool's own uid, takes its entry over",
             run_register_held_own);
    run_case("registering a rank a tool holds, for another uid, is refused",
             run_register_held_other);
    run_case("a tool naming a new rank in another user's namespace is refused",
             run_new_rank_other);
    run_case("a nameless v5.0 tool on a client's rank is named by the host", run_nameless_v50);
    run_case("a client presenting its rank's token is taken", run_token_right);
    run_case("a client presenting another token is refused before the host is asked",
             run_token_wrong);
    run_case("a client presenting no token is refused before the host is asked",
             run_token_none);
    run_case("a client naming a rank registered but not yet prepared is refused before the "
             "host is asked",
             run_token_unprepared);
    run_case("a tool on a client's rank presenting no token is refused before the host is asked",
             run_token_tool_none);
    run_case("a tool on a client's rank presenting its token is taken, and the host is not "
             "given it",
             run_token_tool_right);
    run_case("a client of this release over TCP presenting no token is refused",
             run_token_tcp_none);
    run_case("a client of this release over TCP presenting its token is taken",
             run_token_tcp_right);
    run_case("a client of an earlier release over TCP, with no token to present, is taken",
             run_token_tcp_earlier);
    run_case("a client declaring an earlier release on the local socket is refused",
             run_earlier_local);
    run_case("a tool of this release on a client's rank over TCP presenting no token is refused",
             run_token_tcp_tool_none);
    run_case("a server taking no legacy peers refuses a v5.0 client", run_legacy_refused_client);
    run_case("a server taking no legacy peers refuses a v5.0 tool", run_legacy_refused_tool);
    run_case("a nameless v7.0.0rc1 tool on a client's rank is named by the host",
             run_nameless_v70rc);
    run_case("a nameless v5.0 launcher on a client's rank is named by the host",
             run_nameless_launcher);
    run_case("a tool on a client's rank that finalizes leaves the rank to the next",
             case_tool_client_retired);
    run_case("a client connecting from its rank's pid is refused", run_same_pid_client);
    run_case("a client connecting from another pid is refused", run_other_pid_client);
    run_case("a tool from a client's pid does not take the client's rank over",
             case_same_pid_other_kind);
    run_case("a tool connecting from its rank's pid is refused", run_same_pid_tool);
    run_case("of two tools of one name, the host is asked once and the second refused",
             run_queue_same_name);
    run_case("of two launched tools of one name, the host is asked once and the second "
             "refused",
             run_queue_same_launched);
    run_case("a tool the host refuses makes way for the next, which may be accepted",
             run_queue_after_refusal);
    run_case("tools of different names are put to the host together", run_queue_other_names);
    run_case("tools waiting on the host have a cap of their own, and a further one is refused",
             case_tool_cap);
    run_case("tools waiting on the host do not count toward the listener's cap",
             case_tool_cap_listener);
    run_case("a tool the host names into another user's new namespace is refused",
             case_tool_joins_other_user);
    run_case("a tool whose name the host gives another meanwhile is refused",
             run_queue_assigned_name);
    run_case("a waiting tool that goes away is never put to the host", run_queue_gone);
    run_case("a waiting tool whose name the host gives another is refused at once",
             run_queue_conflict_at_once);
    run_case("a client the host answers late is refused, and a late yes is reported lost",
             run_late_client);
    run_case("a tool the host answers late is refused, holding its name until the answer",
             run_late_tool);
    run_case("a late no is followed by nothing", run_late_no);
    run_case("a client that goes before the host says yes is reported lost", run_gone_client);
    run_case("a tool that goes before the host says yes is reported lost", run_gone_tool);
    run_case("a tool that waited for a tool of its name gives the host what is left of its "
             "wait",
             run_wait_left);
    run_case("a tool whose wait is over is refused without the host being asked",
             run_wait_over);
    run_case("a late yes for a tool reaches the host before a tool of its name connects",
             run_notice_late_tool);
    run_case("a yes for a tool that reset reaches the host before a tool of its name connects",
             run_notice_gone_tool);
    run_case("a late yes for a client reaches the host before a client of its rank connects",
             run_notice_late_client);
    run_case("a second connection that fails leaves the process its connection, and the host "
             "unaware",
             case_second_fails);
    run_case("a process whose finalize is unanswered holds its rank, and is counted once",
             case_finalizing_holds);

    fprintf(stdout, "\n%d passed, %d failed\n", npass, nfail);
    return (0 == nfail) ? 0 : 1;
}
