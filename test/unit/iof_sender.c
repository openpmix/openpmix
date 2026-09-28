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
 */

#include "src/include/pmix_config.h"

#include "include/pmix.h"
#include "include/pmix_server.h"
#include "include/pmix_tool.h"
#include "src/client/pmix_client_ops.h"
#include "src/include/pmix_globals.h"
#include "src/mca/bfrops/bfrops.h"
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

    /* and stay up until the parent has watched our stdin */
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

    if (NULL == pmix_ptl_base.listener.uri ||
        0 > asprintf(&line, "%d %s", refid, pmix_ptl_base.listener.uri)) {
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
