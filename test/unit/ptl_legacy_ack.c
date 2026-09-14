/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * A connect-ack laid out the way a released v4.1 peer lays it out must
 * still connect.
 *
 * The v4.1 series counted its connector-flag byte twice when sizing the
 * message, so its connect-ack ends with one zero byte after the gds name.
 * A peer sending no info blob therefore has that byte where the blob would
 * be. The server used to ignore the failure to unpack an info count from
 * it; hardening the blob parse turned that failure into a refused
 * connection, and every v4.1 client failed PMIx_Init against the new
 * server - which only the cross-version CI leg noticed. See
 * PMIX_PTL_LEGACY_PAD in ptl_base_connection_hdlr.c.
 *
 * Each case sends the message on a raw socket to a real server in a forked
 * child and requires the status reply to be success: a refused connection
 * closes the socket without one. The client case covers
 * pmix_ptl_base_connection_handler's parse; the tool case covers
 * process_tool_request's, which carried the same strict check.
 */

#include "src/include/pmix_config.h"

#include "include/pmix_server.h"
#include "src/include/pmix_globals.h"
#include "src/mca/bfrops/bfrops_types.h"
#include "src/mca/ptl/base/base.h"
#include "src/mca/ptl/base/ptl_base_handshake.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

#define LEGACY_NSPACE  "legacy.ns"
#define LEGACY_VERSION "4.1.3"
#define LEGACY_PSEC    "native"
#define LEGACY_BFROPS  "v41"
#define LEGACY_GDS     "hash"

#define CHILD_PASS  0
#define CHILD_FAIL  1
#define CHILD_SETUP 2

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
}

static void tool_connected(pmix_info_t *info, size_t ninfo, pmix_tool_connection_cbfunc_t cbfunc,
                           void *cbdata)
{
    pmix_proc_t proc;
    PMIX_HIDE_UNUSED_PARAMS(info, ninfo);

    PMIX_LOAD_PROCID(&proc, "legacy.tool", 0);
    cbfunc(PMIX_SUCCESS, &proc, cbdata);
}

static pmix_server_module_t mymodule = {
    .tool_connected = tool_connected
};

/* Build the connect-ack a v4.1 simple client or self-started tool sends,
 * with no info blob - and the trailing pad byte. */
static char *build_legacy_ack(bool tool, const pmix_byte_object_t *cred, size_t *sz)
{
    pmix_ptl_hdr_t hdr;
    pmix_proc_t proc;
    char *msg;
    size_t csize, payload;
    uint8_t flag, bftype = PMIX_BFROP_BUFFER_NON_DESC;

    PMIX_LOAD_PROCID(&proc, LEGACY_NSPACE, 0);
    payload = strlen(LEGACY_PSEC) + 1 + sizeof(uint32_t) + cred->size + 1;
    if (tool) {
        flag = PMIX_TOOL_NEEDS_ID;
        payload += 2 * sizeof(uint32_t);
    } else {
        flag = PMIX_SIMPLE_CLIENT;
        payload += strlen(proc.nspace) + 1 + sizeof(uint32_t);
    }
    payload += strlen(LEGACY_VERSION) + 1 + strlen(LEGACY_BFROPS) + 1 + 1
               + strlen(LEGACY_GDS) + 1;
    payload += 1; // the byte v4.1 counted twice

    memset(&hdr, 0, sizeof(hdr));
    hdr.pindex = -1;
    hdr.tag = UINT32_MAX;
    hdr.nbytes = (uint32_t) payload; // host order, as every release sends it

    *sz = sizeof(hdr) + payload;
    msg = (char *) calloc(1, *sz);
    memcpy(msg, &hdr, sizeof(hdr));
    csize = sizeof(hdr);

    PMIX_PTL_PUT_STRING(LEGACY_PSEC);
    PMIX_PTL_PUT_U32(cred->size);
    PMIX_PTL_PUT_BLOB(cred->bytes, cred->size);
    PMIX_PTL_PUT_U8(flag);
    if (tool) {
        PMIX_PTL_PUT_U32(getuid());
        PMIX_PTL_PUT_U32(getgid());
    } else {
        PMIX_PTL_PUT_PROCID(proc);
    }
    PMIX_PTL_PUT_STRING(LEGACY_VERSION);
    PMIX_PTL_PUT_STRING(LEGACY_BFROPS);
    PMIX_PTL_PUT_U8(bftype);
    PMIX_PTL_PUT_STRING(LEGACY_GDS);
    /* the pad byte is already zero */
    return msg;
}

static int legacy_child(bool tool)
{
    pmix_status_t rc;
    pmix_proc_t proc;
    pmix_nspace_t ns;
    pmix_byte_object_t cred;
    char credbytes[sizeof(uid_t) + sizeof(gid_t)];
    uid_t euid;
    gid_t egid;
    struct sockaddr_in sa;
    struct timeval tv = {10, 0};
    char *p, *msg;
    size_t sz;
    uint32_t reply;
    int port, sd;

    rc = PMIx_server_init(&mymodule, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        return CHILD_SETUP;
    }
    if (!tool) {
        PMIX_LOAD_NSPACE(ns, LEGACY_NSPACE);
        rc = PMIx_server_register_nspace(ns, 1, NULL, 0, NULL, NULL);
        PMIX_LOAD_PROCID(&proc, LEGACY_NSPACE, 0);
        if (PMIX_SUCCESS != rc && PMIX_OPERATION_SUCCEEDED != rc) {
            fprintf(stderr, "register_nspace: %s\n", PMIx_Error_string(rc));
            return CHILD_SETUP;
        }
        rc = PMIx_server_register_client(&proc, getuid(), getgid(), NULL, NULL, NULL);
        if (PMIX_SUCCESS != rc && PMIX_OPERATION_SUCCEEDED != rc) {
            fprintf(stderr, "register_client: %s\n", PMIx_Error_string(rc));
            return CHILD_SETUP;
        }
    }
    if (NULL == pmix_ptl_base.listener.uri ||
        NULL == (p = strrchr(pmix_ptl_base.listener.uri, ':'))) {
        return CHILD_SETUP;
    }
    port = atoi(p + 1);

    sd = socket(AF_INET, SOCK_STREAM, 0);
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (0 > sd || 0 != connect(sd, (struct sockaddr *) &sa, sizeof(sa))) {
        return CHILD_SETUP;
    }
    setsockopt(sd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    /* a native credential over TCP, as psec/native builds one: the
     * effective uid, then the effective gid */
    euid = geteuid();
    egid = getegid();
    memcpy(credbytes, &euid, sizeof(uid_t));
    memcpy(credbytes + sizeof(uid_t), &egid, sizeof(gid_t));
    cred.bytes = credbytes;
    cred.size = sizeof(uid_t) + sizeof(gid_t);
    msg = build_legacy_ack(tool, &cred, &sz);
    if ((ssize_t) sz != write(sd, msg, sz)) {
        return CHILD_SETUP;
    }
    free(msg);

    /* the first thing back is the status, in network order - a refused
     * connection is closed without one */
    if (sizeof(reply) != read(sd, &reply, sizeof(reply))) {
        fprintf(stderr, "no reply to the legacy connect-ack\n");
        return CHILD_FAIL;
    }
    reply = ntohl(reply);
    if (PMIX_SUCCESS != (pmix_status_t) reply) {
        fprintf(stderr, "legacy connect-ack answered %s\n",
                PMIx_Error_string((pmix_status_t) reply));
        return CHILD_FAIL;
    }
    /* no finalize: the server would wait on a peer this test never runs */
    return CHILD_PASS;
}

static void run_case(const char *name, bool tool)
{
    pid_t pid;
    int status;
    char detail[64];

    fflush(stdout);
    fflush(stderr);
    pid = fork();
    if (0 == pid) {
        alarm(30);
        _exit(legacy_child(tool));
    }
    if (0 > pid || pid != waitpid(pid, &status, 0)) {
        report(name, 0, "fork/wait failed");
    } else if (WIFSIGNALED(status)) {
        snprintf(detail, sizeof(detail), "child died on signal %d", WTERMSIG(status));
        report(name, 0, detail);
    } else if (CHILD_SETUP == WEXITSTATUS(status)) {
        report(name, 0, "test setup failed");
    } else {
        report(name, CHILD_PASS == WEXITSTATUS(status), "connection refused");
    }
}

int main(int argc, char **argv)
{
    PMIX_HIDE_UNUSED_PARAMS(argc, argv);

    fprintf(stdout, "\n=== v4.1-shaped connect-ack unit tests ===\n\n");
    run_case("a v4.1 client's padded connect-ack connects", false);
    run_case("a v4.1 tool's padded connect-ack connects", true);
    fprintf(stdout, "\n%d passed, %d failed\n", npass, nfail);
    return (0 == nfail) ? 0 : 1;
}
