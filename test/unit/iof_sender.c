/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/*
 * Who forwarded output and IOF flow control are taken from.
 *
 * Both travel down: a server sends forwarded output to a process that
 * asked for it (PMIX_PTL_TAG_IOF), and tells a process pushing stdin to
 * it to stop or resume (PMIX_PTL_TAG_IOF_CONTROL). A process acts on
 * either only when it comes from a server it connected to, or from
 * itself.
 *
 *   parent - a PMIx server with tool support and a push_stdin upcall,
 *            holding a counting IOF handler of its own;
 *   child  - a tool forwarding its stdin (a pipe from the parent) to the
 *            server, which then sends the server one forwarded-output
 *            message addressed to that handler and one XOFF naming
 *            itself.
 *
 * The server must act on neither: its handler must not fire, and the
 * tool's stdin must keep flowing. Then the other direction, so neither
 * check can pass without the path working: the server delivers the same
 * output message to itself and must see it, and sends the tool a real
 * XOFF, after which the tool's stdin must stop.
 *
 * The same holds for the other messages that only travel down - an
 * event (PMIX_PTL_TAG_NOTIFY), a key deletion (PMIX_PTL_TAG_DATA_DELETE)
 * and a store update (PMIX_PTL_TAG_GDS_UPDATE). Last, the tool puts one
 * of each through its own receive path as though a peer it did not
 * connect to had sent it - a peer that is its server in every respect
 * but that one - and then the same from its server, and must act on the
 * second only.
 */

#include "src/include/pmix_config.h"

#include "include/pmix.h"
#include "include/pmix_server.h"
#include "include/pmix_tool.h"
#include "src/client/pmix_client_ops.h"
#include "src/include/pmix_globals.h"
#include "src/mca/bfrops/bfrops.h"
#include "src/mca/gds/gds.h"
#include "src/mca/ptl/base/base.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define TOOL_NSPACE   "iof-sender-tool"
#define SERVER_NSPACE "iof-sender-server"

#define WAIT_USEC  50000
#define WAIT_TRIES 100

/* the events the tool is sent on its own receive path, one from a peer
 * it did not connect to and one from its server */
#define CODE_DOWNSTREAM (PMIX_EXTERNAL_ERR_BASE - 61)
#define CODE_UPSTREAM   (PMIX_EXTERNAL_ERR_BASE - 62)
#define DELKEY          "iof-sender.delete.key"
#define UPDKEY_DOWN     "iof-sender.update.downstream"
#define UPDKEY_UP       "iof-sender.update.upstream"

/* what the tool reports of those, one bit each */
#define DS_SETUP        0x01
#define DS_EVENT_DOWN   0x02 // an event from the other peer was ignored
#define DS_EVENT_UP     0x04 // one from the server was delivered
#define DS_DELETE_DOWN  0x08
#define DS_DELETE_UP    0x10
#define DS_UPDATE_DOWN  0x20
#define DS_UPDATE_UP    0x40
#define DS_UPDATE_SKIP  0x80 // the server's store is not one these updates are packed for

static int npass = 0;
static int nfail = 0;

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

/* ---------------- the server's host ---------------- */

static volatile size_t hostbytes = 0;

static void tool_connected_fn(pmix_info_t *info, size_t ninfo,
                              pmix_tool_connection_cbfunc_t cbfunc, void *cbdata)
{
    pmix_proc_t proc;
    PMIX_HIDE_UNUSED_PARAMS(info, ninfo);

    PMIX_LOAD_PROCID(&proc, TOOL_NSPACE, 0);
    cbfunc(PMIX_SUCCESS, &proc, cbdata);
}

static pmix_status_t push_stdin_fn(const pmix_proc_t *source,
                                   const pmix_proc_t targets[], size_t ntargets,
                                   const pmix_info_t directives[], size_t ndirs,
                                   const pmix_byte_object_t *bo,
                                   pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    PMIX_HIDE_UNUSED_PARAMS(source, targets, ntargets, directives, ndirs);

    if (NULL != bo) {
        hostbytes += bo->size;
    }
    if (NULL != cbfunc) {
        cbfunc(PMIX_SUCCESS, cbdata);
    }
    return PMIX_SUCCESS;
}

static pmix_server_module_t mymodule = {
    .tool_connected = tool_connected_fn,
    .push_stdin = push_stdin_fn
};

/* a forwarded-output message for handler "refid", packed for "peer" */
static pmix_buffer_t *output_msg(pmix_peer_t *peer, size_t refid)
{
    pmix_buffer_t *msg;
    pmix_proc_t source;
    pmix_iof_channel_t channel = PMIX_FWD_STDOUT_CHANNEL;
    pmix_byte_object_t bo;
    size_t ninfo = 0;
    pmix_status_t rc;

    msg = PMIX_NEW(pmix_buffer_t);
    PMIX_LOAD_PROCID(&source, "iof-sender-output", 0);
    bo.bytes = "output\n";
    bo.size = strlen(bo.bytes);
    PMIX_BFROPS_PACK(rc, peer, msg, &source, 1, PMIX_PROC);
    if (PMIX_SUCCESS == rc) {
        PMIX_BFROPS_PACK(rc, peer, msg, &channel, 1, PMIX_IOF_CHANNEL);
    }
    if (PMIX_SUCCESS == rc) {
        PMIX_BFROPS_PACK(rc, peer, msg, &refid, 1, PMIX_SIZE);
    }
    if (PMIX_SUCCESS == rc) {
        PMIX_BFROPS_PACK(rc, peer, msg, &ninfo, 1, PMIX_SIZE);
    }
    if (PMIX_SUCCESS == rc) {
        PMIX_BFROPS_PACK(rc, peer, msg, &bo, 1, PMIX_BYTE_OBJECT);
    }
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(msg);
        return NULL;
    }
    return msg;
}

/* ---------------- the tool half ---------------- */

static volatile int nxoff = 0;
static pmix_ptl_cbfunc_t flow_control_handler = NULL;

/* counts flow-control messages once the library has finished with them */
static void count_flow_control(struct pmix_peer_t *peer, pmix_ptl_hdr_t *hdr,
                               pmix_buffer_t *buf, void *cbdata)
{
    flow_control_handler(peer, hdr, buf, cbdata);
    ++nxoff;
}

static bool hook_flow_control(void)
{
    pmix_ptl_posted_recv_t *rcv;

    PMIX_LIST_FOREACH (rcv, &pmix_ptl_base.posted_recvs, pmix_ptl_posted_recv_t) {
        if (PMIX_PTL_TAG_IOF_CONTROL == rcv->tag) {
            flow_control_handler = rcv->cbfunc;
            rcv->cbfunc = count_flow_control;
            return true;
        }
    }
    return false;
}

static bool send_to_server(pmix_peer_t *server, pmix_buffer_t *msg, pmix_ptl_tag_t tag)
{
    pmix_status_t rc;

    if (NULL == msg) {
        return false;
    }
    PMIX_PTL_SEND_ONEWAY(rc, server, msg, tag);
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(msg);
        return false;
    }
    return true;
}

/* ---------- messages that only travel down, put on our recv path ---------- */

typedef struct {
    pmix_object_t super;
    pmix_event_t ev;
    pmix_lock_t lock;
} barrier_t;
static PMIX_CLASS_INSTANCE(barrier_t, pmix_object_t, NULL, NULL);

static void barrier_cb(int sd, short args, void *cbdata)
{
    barrier_t *b = (barrier_t *) cbdata;
    PMIX_HIDE_UNUSED_PARAMS(sd, args);
    PMIX_WAKEUP_THREAD(&b->lock);
}

/* everything already posted to the progress thread has run */
static void progress_barrier(void)
{
    barrier_t *b = PMIX_NEW(barrier_t);

    PMIX_CONSTRUCT_LOCK(&b->lock);
    PMIX_THREADSHIFT(b, barrier_cb);
    PMIX_WAIT_THREAD(&b->lock);
    PMIX_DESTRUCT_LOCK(&b->lock);
    PMIX_RELEASE(b);
}

/* hand "payload" to our receive path as a message "peer" sent on "tag" -
 * where a message read off that peer's socket goes */
static void deliver(pmix_peer_t *peer, pmix_ptl_tag_t tag, pmix_buffer_t *payload)
{
    pmix_ptl_recv_t *msg;
    size_t nbytes = 0;

    msg = PMIX_NEW(pmix_ptl_recv_t);
    PMIX_RETAIN(peer);
    msg->peer = peer;
    msg->hdr.tag = tag;
    PMIX_UNLOAD_BUFFER(payload, msg->data, nbytes);
    msg->hdr.nbytes = (uint32_t) nbytes;
    PMIX_RELEASE(payload);
    PMIX_THREADSHIFT(msg, pmix_ptl_base_process_msg);
    progress_barrier();
}

/* a peer that is our server in every respect but one: we did not open
 * the connection to it */
static pmix_peer_t *downstream_copy(pmix_peer_t *server)
{
    pmix_peer_t *p = PMIX_NEW(pmix_peer_t);

    PMIX_RETAIN(server->nptr);
    p->nptr = server->nptr;
    PMIX_RETAIN(server->info);
    p->info = server->info;
    p->gds = server->gds;
    memcpy(&p->proc_type, &server->proc_type, sizeof(pmix_proc_type_t));
    p->upstream = false;
    return p;
}

static pmix_buffer_t *event_msg(pmix_peer_t *server, pmix_status_t code)
{
    pmix_buffer_t *msg = PMIX_NEW(pmix_buffer_t);
    pmix_cmd_t cmd = PMIX_NOTIFY_CMD;
    pmix_proc_t source;
    pmix_data_range_t range = PMIX_RANGE_LOCAL;
    size_t ninfo = 0;
    pmix_status_t rc;

    PMIX_LOAD_PROCID(&source, SERVER_NSPACE, 0);
    PMIX_BFROPS_PACK(rc, server, msg, &cmd, 1, PMIX_COMMAND);
    if (PMIX_SUCCESS == rc) {
        PMIX_BFROPS_PACK(rc, server, msg, &code, 1, PMIX_STATUS);
    }
    if (PMIX_SUCCESS == rc) {
        PMIX_BFROPS_PACK(rc, server, msg, &source, 1, PMIX_PROC);
    }
    if (PMIX_SUCCESS == rc) {
        PMIX_BFROPS_PACK(rc, server, msg, &ninfo, 1, PMIX_SIZE);
    }
    if (PMIX_SUCCESS == rc) {
        PMIX_BFROPS_PACK(rc, server, msg, &range, 1, PMIX_DATA_RANGE);
    }
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(msg);
        return NULL;
    }
    return msg;
}

static pmix_buffer_t *delete_msg(pmix_peer_t *server, const pmix_proc_t *proc)
{
    pmix_buffer_t *msg = PMIX_NEW(pmix_buffer_t);
    pmix_scope_t scope = PMIX_DEL_INTERNAL;
    char *key = DELKEY;
    pmix_status_t rc;

    PMIX_BFROPS_PACK(rc, server, msg, proc, 1, PMIX_PROC);
    if (PMIX_SUCCESS == rc) {
        PMIX_BFROPS_PACK(rc, server, msg, &scope, 1, PMIX_SCOPE);
    }
    if (PMIX_SUCCESS == rc) {
        PMIX_BFROPS_PACK(rc, server, msg, &key, 1, PMIX_STRING);
    }
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(msg);
        return NULL;
    }
    return msg;
}

/* a store update as gds/hash sends one: job-level values in a
 * PMIX_JOB_INFO_ARRAY */
static pmix_buffer_t *update_msg(pmix_peer_t *server, const char *key)
{
    pmix_buffer_t *msg = PMIX_NEW(pmix_buffer_t);
    pmix_info_t *iptr;
    pmix_data_array_t darray;
    pmix_value_t val;
    pmix_kval_t kv;
    pmix_status_t rc;

    PMIX_INFO_CREATE(iptr, 1);
    PMIX_INFO_LOAD(&iptr[0], key, "updated", PMIX_STRING);
    darray.type = PMIX_INFO;
    darray.size = 1;
    darray.array = iptr;
    PMIX_VALUE_LOAD(&val, &darray, PMIX_DATA_ARRAY);
    PMIX_INFO_FREE(iptr, 1);
    PMIX_CONSTRUCT(&kv, pmix_kval_t);
    kv.key = strdup(PMIX_JOB_INFO_ARRAY);
    kv.value = &val;
    PMIX_BFROPS_PACK(rc, server, msg, &kv, 1, PMIX_KVAL);
    kv.value = NULL;
    PMIX_DESTRUCT(&kv);
    PMIX_VALUE_DESTRUCT(&val);
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(msg);
        return NULL;
    }
    return msg;
}

static volatile int down_events = 0;
static volatile int up_events = 0;

static void count_events(size_t evhdlr_registration_id, pmix_status_t status,
                         const pmix_proc_t *source, pmix_info_t info[], size_t ninfo,
                         pmix_info_t results[], size_t nresults,
                         pmix_event_notification_cbfunc_fn_t cbfunc, void *cbdata)
{
    PMIX_HIDE_UNUSED_PARAMS(evhdlr_registration_id, source, info, ninfo, results, nresults);

    if (CODE_DOWNSTREAM == status) {
        ++down_events;
    } else if (CODE_UPSTREAM == status) {
        ++up_events;
    }
    if (NULL != cbfunc) {
        cbfunc(PMIX_EVENT_ACTION_COMPLETE, NULL, 0, NULL, NULL, cbdata);
    }
}

/* can we read this key out of our own store? Optional, so a miss comes
 * straight back rather than going to the server */
static bool can_read(const pmix_proc_t *proc, const char *key)
{
    pmix_value_t *val = NULL;
    pmix_info_t opt;
    pmix_status_t rc;
    bool flag = true;
    bool found;

    PMIX_INFO_LOAD(&opt, PMIX_OPTIONAL, &flag, PMIX_BOOL);
    rc = PMIx_Get(proc, key, &opt, 1, &val);
    PMIX_INFO_DESTRUCT(&opt);
    found = (PMIX_SUCCESS == rc && NULL != val);
    if (NULL != val) {
        PMIX_VALUE_RELEASE(val);
    }
    return found;
}

static unsigned char check_downward_tags(const pmix_proc_t *myproc)
{
    pmix_peer_t *server = pmix_client_globals.myserver;
    pmix_peer_t *other;
    pmix_status_t codes[2] = {CODE_DOWNSTREAM, CODE_UPSTREAM};
    pmix_proc_t jobproc;
    pmix_value_t val;
    pmix_buffer_t *msg;
    pmix_status_t rc;
    unsigned char result = 0;
    int i;

    if (NULL == server || !server->upstream) {
        return 0;
    }
    rc = PMIx_Register_event_handler(codes, 2, NULL, 0, count_events, NULL, NULL);
    if (0 > rc) {
        return 0;
    }
    PMIX_VALUE_LOAD(&val, "before", PMIX_STRING);
    rc = PMIx_Put(PMIX_INTERNAL, DELKEY, &val);
    PMIX_VALUE_DESTRUCT(&val);
    if (PMIX_SUCCESS != rc || !can_read(myproc, DELKEY)) {
        return 0;
    }
    other = downstream_copy(server);
    result |= DS_SETUP;

    /* an event: the server's is delivered after the other's, on the same
     * thread, so once it has fired the other has had its chance */
    msg = event_msg(server, CODE_DOWNSTREAM);
    if (NULL != msg) {
        deliver(other, PMIX_PTL_TAG_NOTIFY, msg);
    }
    msg = event_msg(server, CODE_UPSTREAM);
    if (NULL != msg) {
        deliver(server, PMIX_PTL_TAG_NOTIFY, msg);
    }
    for (i = 0; i < WAIT_TRIES && 0 == up_events; i++) {
        usleep(WAIT_USEC);
    }
    progress_barrier();
    if (0 == down_events) {
        result |= DS_EVENT_DOWN;
    }
    if (0 < up_events) {
        result |= DS_EVENT_UP;
    }

    /* a key deletion, applied as it is received */
    msg = delete_msg(server, myproc);
    if (NULL != msg) {
        deliver(other, PMIX_PTL_TAG_DATA_DELETE, msg);
        if (can_read(myproc, DELKEY)) {
            result |= DS_DELETE_DOWN;
        }
    }
    msg = delete_msg(server, myproc);
    if (NULL != msg) {
        deliver(server, PMIX_PTL_TAG_DATA_DELETE, msg);
        if (!can_read(myproc, DELKEY)) {
            result |= DS_DELETE_UP;
        }
    }

    /* a store update - packed as gds/hash packs one, so only a server
     * whose store is hash gets these */
    if (PMIX_GDS_CHECK_COMPONENT(server, "hash")) {
        PMIX_LOAD_PROCID(&jobproc, myproc->nspace, PMIX_RANK_WILDCARD);
        msg = update_msg(server, UPDKEY_DOWN);
        if (NULL != msg) {
            deliver(other, PMIX_PTL_TAG_GDS_UPDATE, msg);
            if (!can_read(&jobproc, UPDKEY_DOWN)) {
                result |= DS_UPDATE_DOWN;
            }
        }
        msg = update_msg(server, UPDKEY_UP);
        if (NULL != msg) {
            deliver(server, PMIX_PTL_TAG_GDS_UPDATE, msg);
            if (can_read(&jobproc, UPDKEY_UP)) {
                result |= DS_UPDATE_UP;
            }
        }
    } else {
        result |= DS_UPDATE_SKIP;
    }

    PMIX_RELEASE(other);
    return result;
}

static int run_tool(int urifd, int readyfd, int gofd)
{
    char line[2048], *uri;
    unsigned long refid;
    ssize_t n;
    pmix_proc_t myproc, target;
    pmix_info_t tinfo, dir;
    pmix_byte_object_t bo;
    pmix_peer_t *server;
    pmix_buffer_t *msg;
    pmix_iof_channel_t channel = PMIX_FWD_STDIN_CHANNEL;
    bool xoff = true;
    size_t ndirs = 0;
    pmix_status_t rc;
    char c;
    int i, before;

    n = read(urifd, line, sizeof(line) - 1);
    if (0 >= n) {
        return 1;
    }
    line[n] = '\0';
    refid = strtoul(line, &uri, 10);
    if (' ' != *uri) {
        return 1;
    }
    ++uri;

    PMIX_INFO_LOAD(&tinfo, PMIX_SERVER_URI, uri, PMIX_STRING);
    rc = PMIx_tool_init(&myproc, &tinfo, 1);
    PMIX_INFO_DESTRUCT(&tinfo);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "  tool: PMIx_tool_init failed: %s\n", PMIx_Error_string(rc));
        return 1;
    }
    if (!hook_flow_control()) {
        goto fail;
    }
    server = pmix_client_globals.myserver;

    /* forward our stdin to the server */
    PMIX_LOAD_PROCID(&target, SERVER_NSPACE, 0);
    PMIX_INFO_LOAD(&dir, PMIX_IOF_PUSH_STDIN, NULL, PMIX_BOOL);
    rc = PMIx_IOF_push(&target, 1, NULL, &dir, 1, NULL, NULL);
    PMIX_INFO_DESTRUCT(&dir);
    if (PMIX_SUCCESS != rc && PMIX_OPERATION_SUCCEEDED != rc) {
        fprintf(stderr, "  tool: PMIx_IOF_push failed: %s\n", PMIx_Error_string(rc));
        goto fail;
    }

    /* the server takes us for a stdin producer once it has been handed
     * some of our input */
    bo.bytes = "x";
    bo.size = 1;
    rc = PMIx_IOF_push(&target, 1, &bo, NULL, 0, NULL, NULL);
    if (PMIX_SUCCESS != rc && PMIX_OPERATION_SUCCEEDED != rc) {
        goto fail;
    }

    /* forwarded output, sent up to the server */
    if (!send_to_server(server, output_msg(server, refid), PMIX_PTL_TAG_IOF)) {
        goto fail;
    }

    /* an XOFF naming ourselves, sent up to the server */
    msg = PMIX_NEW(pmix_buffer_t);
    PMIX_BFROPS_PACK(rc, server, msg, &myproc, 1, PMIX_PROC);
    if (PMIX_SUCCESS == rc) {
        PMIX_BFROPS_PACK(rc, server, msg, &channel, 1, PMIX_IOF_CHANNEL);
    }
    if (PMIX_SUCCESS == rc) {
        PMIX_BFROPS_PACK(rc, server, msg, &xoff, 1, PMIX_BOOL);
    }
    if (PMIX_SUCCESS == rc) {
        PMIX_BFROPS_PACK(rc, server, msg, &ndirs, 1, PMIX_SIZE);
    }
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(msg);
        goto fail;
    }
    if (!send_to_server(server, msg, PMIX_PTL_TAG_IOF_CONTROL)) {
        goto fail;
    }

    /* the server handles a connection's messages in order, and answers
     * this only after it has handled both of the above - so whatever
     * they caused has reached us by the time it returns */
    rc = PMIx_IOF_push(&target, 1, &bo, NULL, 0, NULL, NULL);
    if (PMIX_SUCCESS != rc && PMIX_OPERATION_SUCCEEDED != rc) {
        goto fail;
    }
    c = (char) nxoff;
    if (1 != write(readyfd, &c, 1)) {
        goto fail;
    }

    /* the server now sends an XOFF of its own; say when it has been
     * handled */
    before = nxoff;
    if (1 != read(gofd, &c, 1)) {
        goto fail;
    }
    for (i = 0; i < WAIT_TRIES && nxoff == before; i++) {
        usleep(WAIT_USEC);
    }
    c = (nxoff > before) ? 1 : 0;
    if (1 != write(readyfd, &c, 1)) {
        goto fail;
    }

    /* wait until the parent has watched our stdin, then put the
     * messages that only travel down through our own receive path */
    if (1 != read(gofd, &c, 1)) {
        goto fail;
    }
    c = (char) check_downward_tags(&myproc);
    if (1 != write(readyfd, &c, 1)) {
        goto fail;
    }

    /* and stay up until the parent has read that */
    if (1 != read(gofd, &c, 1)) {
        goto fail;
    }
    PMIx_tool_finalize();
    return 0;

fail:
    PMIx_tool_finalize();
    return 1;
}

/* ---------------- the server half ---------------- */

static volatile int noutput = 0;

static void count_output(size_t iofhdlr, pmix_iof_channel_t channel,
                         pmix_proc_t *source, pmix_byte_object_t *payload,
                         pmix_info_t info[], size_t ninfo)
{
    PMIX_HIDE_UNUSED_PARAMS(iofhdlr, channel, source, payload, info, ninfo);
    ++noutput;
}

/* feed the tool's stdin and say whether any of it reached the host */
static bool stdin_flows(int fd)
{
    size_t start = hostbytes;
    int i;

    if (6 != write(fd, "input\n", 6)) {
        return false;
    }
    for (i = 0; i < WAIT_TRIES && hostbytes == start; i++) {
        usleep(WAIT_USEC);
    }
    return hostbytes != start;
}

int main(int argc, char **argv)
{
    pmix_status_t rc;
    pmix_info_t sinfo[2];
    pmix_proc_t toolproc;
    pmix_iof_req_t *req;
    pmix_buffer_t *msg;
    char *line = NULL;
    int refid, i, before;
    int uripipe[2], readypipe[2], gopipe[2], inpipe[2];
    pid_t child;
    int status = 0;
    char c = 'g';
    unsigned char bits;
    bool flag = true;
    PMIX_HIDE_UNUSED_PARAMS(argc, argv);

    fprintf(stdout, "\n=== IOF sender unit test ===\n\n");

    if (0 != pipe(uripipe) || 0 != pipe(readypipe) || 0 != pipe(gopipe) ||
        0 != pipe(inpipe)) {
        fprintf(stderr, "pipe() failed\n");
        return 1;
    }

    child = fork();
    if (0 > child) {
        fprintf(stderr, "fork() failed\n");
        return 1;
    }
    if (0 == child) {
        close(uripipe[1]);
        close(readypipe[0]);
        close(gopipe[1]);
        close(inpipe[1]);
        if (0 > dup2(inpipe[0], STDIN_FILENO)) {
            _exit(1);
        }
        close(inpipe[0]);
        _exit(run_tool(uripipe[0], readypipe[1], gopipe[0]));
    }

    close(uripipe[0]);
    close(readypipe[1]);
    close(gopipe[0]);
    close(inpipe[0]);

    PMIX_INFO_LOAD(&sinfo[0], PMIX_SERVER_TOOL_SUPPORT, &flag, PMIX_BOOL);
    PMIX_INFO_LOAD(&sinfo[1], PMIX_SERVER_NSPACE, SERVER_NSPACE, PMIX_STRING);
    rc = PMIx_server_init(&mymodule, sinfo, 2);
    PMIX_INFO_DESTRUCT(&sinfo[0]);
    PMIX_INFO_DESTRUCT(&sinfo[1]);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "PMIx_server_init failed: %s\n", PMIx_Error_string(rc));
        goto reap;
    }

    /* an output handler of our own, registered before anyone connects;
     * finalize releases it */
    req = PMIX_NEW(pmix_iof_req_t);
    req->cbfunc = count_output;
    refid = pmix_pointer_array_add(&pmix_globals.iof_requests, req);
    if (0 > refid) {
        report("registered an output handler", 0, "array add failed");
        PMIX_RELEASE(req);
        goto done;
    }

    /* the address the server reports: its TCP listener's when it has
     * one, else its local socket's */
    if (NULL == ((NULL != pmix_ptl_base.listener.uri) ? pmix_ptl_base.listener.uri : pmix_ptl_base.local_listener.uri) ||
        0 > asprintf(&line, "%d %s", refid, ((NULL != pmix_ptl_base.listener.uri) ? pmix_ptl_base.listener.uri : pmix_ptl_base.local_listener.uri))) {
        report("server published its URI", 0, "no URI");
        goto done;
    }
    if ((ssize_t) strlen(line) != write(uripipe[1], line, strlen(line))) {
        report("tool received the URI", 0, "short write");
        goto done;
    }
    close(uripipe[1]);
    uripipe[1] = -1;

    if (1 != read(readypipe[0], &c, 1)) {
        report("tool connected and sent its messages", 0, "tool never reported ready");
        goto done;
    }
    report("tool connected and sent its messages", 1, NULL);
    report("server ignores forwarded output sent up by a tool", 0 == noutput,
           "the tool's message reached the server's output handler");
    report("server ignores flow control sent up by a tool", 0 == c,
           "the tool's XOFF was acted on and relayed back to it");
    report("the tool's stdin still reaches the host", stdin_flows(inpipe[1]),
           "the tool's stdin was suspended");

    /* the same output message, delivered by the server to itself */
    before = noutput;
    msg = output_msg(pmix_globals.mypeer, (size_t) refid);
    if (NULL == msg) {
        report("server delivers output to itself", 0, "pack failed");
        goto done;
    }
    PMIX_PTL_SEND_ONEWAY(rc, pmix_globals.mypeer, msg, PMIX_PTL_TAG_IOF);
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(msg);
    }
    for (i = 0; i < WAIT_TRIES && before == noutput; i++) {
        usleep(WAIT_USEC);
    }
    report("server delivers output to itself", before + 1 == noutput,
           "the server's own output message did not reach its handler");

    /* and a real XOFF to the tool */
    PMIX_LOAD_PROCID(&toolproc, TOOL_NSPACE, 0);
    rc = PMIx_server_IOF_flow_control(&toolproc, PMIX_FWD_STDIN_CHANNEL, true,
                                      NULL, 0, NULL, NULL);
    report("server sent the tool an XOFF",
           PMIX_SUCCESS == rc || PMIX_OPERATION_SUCCEEDED == rc, PMIx_Error_string(rc));
    if (1 != write(gopipe[1], &c, 1) || 1 != read(readypipe[0], &c, 1) || 1 != c) {
        report("the tool received the XOFF", 0, "it never arrived");
        goto done;
    }
    report("the tool's stdin stops on its server's XOFF", !stdin_flows(inpipe[1]),
           "the tool kept forwarding its stdin");

    /* what the tool saw of the messages it put on its own receive path */
    if (1 != write(gopipe[1], &c, 1) || 1 != read(readypipe[0], &c, 1)) {
        report("the tool checked the messages that only travel down", 0,
               "it never reported");
        goto done;
    }
    bits = (unsigned char) c;
    report("the tool set up the messages that only travel down", 0 != (DS_SETUP & bits),
           "setup failed");
    report("a tool ignores an event from a peer it did not connect to",
           0 != (DS_EVENT_DOWN & bits), "its handler fired");
    report("a tool acts on an event from its server", 0 != (DS_EVENT_UP & bits),
           "its handler did not fire");
    report("a tool ignores a key deletion from a peer it did not connect to",
           0 != (DS_DELETE_DOWN & bits), "the key was deleted");
    report("a tool acts on a key deletion from its server", 0 != (DS_DELETE_UP & bits),
           "the key is still there");
    if (0 != (DS_UPDATE_SKIP & bits)) {
        fprintf(stdout, "  SKIP: store updates (the server's store is not gds/hash)\n");
    } else {
        report("a tool ignores a store update from a peer it did not connect to",
               0 != (DS_UPDATE_DOWN & bits), "the update was stored");
        report("a tool acts on a store update from its server", 0 != (DS_UPDATE_UP & bits),
               "the update was not stored");
    }

done:
    free(line);
    close(inpipe[1]);
    if (0 <= gopipe[1]) {
        if (1 != write(gopipe[1], &c, 1)) {
            ;
        }
        close(gopipe[1]);
        gopipe[1] = -1;
    }
    waitpid(child, &status, 0);
    child = -1;
    report("the tool exited cleanly", WIFEXITED(status) && 0 == WEXITSTATUS(status),
           "tool failed");
    PMIx_server_finalize();

reap:
    if (0 <= uripipe[1]) {
        close(uripipe[1]);
    }
    if (0 <= gopipe[1]) {
        close(gopipe[1]);
    }
    close(readypipe[0]);
    if (0 < child) {
        waitpid(child, &status, 0);
    }

    fprintf(stdout, "\n%d passed, %d failed\n", npass, nfail);
    return (0 == nfail) ? 0 : 1;
}
