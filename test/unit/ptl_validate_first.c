/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/*
 * A connecting peer is validated before anything is done for it.
 *
 * The connection handler reads a peer's connect-ack, and until its
 * credential has been validated it records nothing and asks nothing of
 * the host: not the namespace's wire format or version, not the pid and
 * real ids in its info blob, not a tool's identity. A namespace's wire
 * format is set by the first of its peers to connect and validate, and a
 * later peer that disagrees is refused.
 *
 * Each case runs a real server in a forked child and sends it a hand-built
 * connect-ack on a raw socket, with a psec/native credential over TCP:
 *
 *   a client claiming a registered rank with a bad credential, an unusual
 *   wire format and a pid in its info blob - refused, and the namespace
 *   and rank left exactly as they were;
 *
 *   a second, validated connection for a rank whose namespace already has
 *   a different wire format - refused, the format unchanged;
 *
 *   a tool with a bad credential - refused in the first reply it reads,
 *   and the host never asked to give it an identity;
 *
 *   and, so none of that passes by refusing everything, a validated client
 *   and a validated tool, which connect.
 */

#include "src/include/pmix_config.h"

#include "include/pmix_server.h"
#include "src/include/pmix_globals.h"
#include "src/mca/bfrops/base/base.h"
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

#define VF_NSPACE  "vfirst.ns"
#define VF_VERSION "6.1.0"
#define VF_PSEC    "native"
#define VF_GDS     "hash"
#define VF_PID     424242

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
    fflush(stdout);
}

static volatile int ntool_connected = 0;

static void tool_connected(pmix_info_t *info, size_t ninfo, pmix_tool_connection_cbfunc_t cbfunc,
                           void *cbdata)
{
    pmix_proc_t proc;
    PMIX_HIDE_UNUSED_PARAMS(info, ninfo);

    ++ntool_connected;
    PMIX_LOAD_PROCID(&proc, "vfirst.tool", 0);
    cbfunc(PMIX_SUCCESS, &proc, cbdata);
}

static pmix_server_module_t mymodule = {
    .tool_connected = tool_connected
};

/* an info blob holding our pid claim, packed in the named wire format */
static bool build_blob(const char *bfrops, uint8_t bftype, pmix_byte_object_t *out)
{
    pmix_bfrops_module_t *mod = pmix_bfrops_base_assign_module(bfrops);
    pmix_buffer_t buf;
    pmix_info_t info;
    pid_t pid = VF_PID;
    size_t n = 1;
    pmix_status_t rc;

    if (NULL == mod) {
        return false;
    }
    PMIX_CONSTRUCT(&buf, pmix_buffer_t);
    buf.type = bftype;
    PMIX_INFO_LOAD(&info, PMIX_PROC_PID, &pid, PMIX_PID);
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

/* the connect-ack a client or a self-started tool sends */
static char *build_ack(bool tool, bool goodcred, const char *bfrops, uint8_t bftype,
                       const pmix_byte_object_t *blob, size_t *sz)
{
    pmix_ptl_hdr_t hdr;
    pmix_proc_t proc;
    char credbytes[sizeof(uid_t) + sizeof(gid_t)];
    uid_t euid = geteuid();
    gid_t egid = getegid();
    char *msg;
    size_t csize, payload;
    uint8_t flag;

    /* a native credential over TCP is the effective uid then gid - a bad
     * one claims a uid this process does not have */
    if (!goodcred) {
        euid += 1;
    }
    memcpy(credbytes, &euid, sizeof(uid_t));
    memcpy(credbytes + sizeof(uid_t), &egid, sizeof(gid_t));

    PMIX_LOAD_PROCID(&proc, VF_NSPACE, 0);
    payload = strlen(VF_PSEC) + 1 + sizeof(uint32_t) + sizeof(credbytes) + 1;
    if (tool) {
        flag = PMIX_TOOL_NEEDS_ID;
        payload += 2 * sizeof(uint32_t);
    } else {
        flag = PMIX_SIMPLE_CLIENT;
        payload += strlen(proc.nspace) + 1 + sizeof(uint32_t);
    }
    payload += strlen(VF_VERSION) + 1 + strlen(bfrops) + 1 + 1 + strlen(VF_GDS) + 1;
    if (NULL != blob) {
        payload += blob->size;
    }

    memset(&hdr, 0, sizeof(hdr));
    hdr.pindex = -1;
    hdr.tag = UINT32_MAX;
    hdr.nbytes = (uint32_t) payload;

    *sz = sizeof(hdr) + payload;
    msg = (char *) calloc(1, *sz);
    memcpy(msg, &hdr, sizeof(hdr));
    csize = sizeof(hdr);

    PMIX_PTL_PUT_STRING(VF_PSEC);
    PMIX_PTL_PUT_U32(sizeof(credbytes));
    PMIX_PTL_PUT_BLOB(credbytes, sizeof(credbytes));
    PMIX_PTL_PUT_U8(flag);
    if (tool) {
        PMIX_PTL_PUT_U32(getuid());
        PMIX_PTL_PUT_U32(getgid());
    } else {
        PMIX_PTL_PUT_PROCID(proc);
    }
    PMIX_PTL_PUT_STRING(VF_VERSION);
    PMIX_PTL_PUT_STRING(bfrops);
    PMIX_PTL_PUT_U8(bftype);
    PMIX_PTL_PUT_STRING(VF_GDS);
    if (NULL != blob) {
        PMIX_PTL_PUT_BLOB(blob->bytes, blob->size);
    }
    return msg;
}

/* connect, send the ack, and return the first reply: PMIX_ERR_UNREACH if
 * the connection was closed without one. The socket is left open in *sdp
 * so an accepted peer stays connected. */
static pmix_status_t send_ack(char *msg, size_t sz, int *sdp)
{
    struct sockaddr_in sa;
    struct timeval tv = {10, 0};
    uint32_t reply;
    char *p;
    int sd;

    if (NULL == pmix_ptl_base.listener.uri ||
        NULL == (p = strrchr(pmix_ptl_base.listener.uri, ':'))) {
        return PMIX_ERROR;
    }
    sd = socket(AF_INET, SOCK_STREAM, 0);
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(atoi(p + 1));
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (0 > sd || 0 != connect(sd, (struct sockaddr *) &sa, sizeof(sa))) {
        return PMIX_ERROR;
    }
    setsockopt(sd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    *sdp = sd;
    if ((ssize_t) sz != write(sd, msg, sz)) {
        return PMIX_ERROR;
    }
    if (sizeof(reply) != read(sd, &reply, sizeof(reply))) {
        return PMIX_ERR_UNREACH;
    }
    return (pmix_status_t) ntohl(reply);
}

static pmix_namespace_t *find_nspace(void)
{
    pmix_namespace_t *ns;

    PMIX_LIST_FOREACH (ns, &pmix_globals.nspaces, pmix_namespace_t) {
        if (NULL != ns->nspace && 0 == strcmp(ns->nspace, VF_NSPACE)) {
            return ns;
        }
    }
    return NULL;
}

static pmix_status_t start_server(bool client)
{
    pmix_status_t rc;
    pmix_proc_t proc;
    pmix_nspace_t ns;

    rc = PMIx_server_init(&mymodule, NULL, 0);
    if (PMIX_SUCCESS != rc || !client) {
        return rc;
    }
    PMIX_LOAD_NSPACE(ns, VF_NSPACE);
    rc = PMIx_server_register_nspace(ns, 1, NULL, 0, NULL, NULL);
    if (PMIX_SUCCESS != rc && PMIX_OPERATION_SUCCEEDED != rc) {
        return rc;
    }
    PMIX_LOAD_PROCID(&proc, VF_NSPACE, 0);
    rc = PMIx_server_register_client(&proc, getuid(), getgid(), NULL, NULL, NULL);
    if (PMIX_OPERATION_SUCCEEDED == rc) {
        rc = PMIX_SUCCESS;
    }
    return rc;
}

/* ---- the cases, each in a server of its own ---- */

static int case_bad_client(void)
{
    pmix_byte_object_t blob;
    pmix_namespace_t *ns;
    pmix_rank_info_t *info;
    pmix_status_t reply;
    char *msg;
    size_t sz;
    int sd = -1;

    if (PMIX_SUCCESS != start_server(true) ||
        !build_blob("v21", PMIX_BFROP_BUFFER_FULLY_DESC, &blob)) {
        return CHILD_SETUP;
    }
    msg = build_ack(false, false, "v21", PMIX_BFROP_BUFFER_FULLY_DESC, &blob, &sz);
    reply = send_ack(msg, sz, &sd);
    free(msg);
    free(blob.bytes);
    usleep(200000);
    ns = find_nspace();
    if (NULL == ns) {
        return CHILD_SETUP;
    }
    info = (pmix_rank_info_t *) pmix_list_get_first(&ns->ranks);
    if (PMIX_SUCCESS == reply) {
        fprintf(stdout, "    the connection was accepted\n");
        return CHILD_FAIL;
    }
    if (NULL != ns->compat.bfrops || NULL != ns->compat.gds || NULL != ns->compat.psec ||
        0 != ns->version.major || ns->version_stored) {
        fprintf(stdout, "    the namespace took the refused peer's settings\n");
        return CHILD_FAIL;
    }
    if (NULL != info && (VF_PID == info->pid || 0 != info->proc_cnt)) {
        fprintf(stdout, "    the rank took the refused peer's info\n");
        return CHILD_FAIL;
    }
    return CHILD_PASS;
}

static int case_disagreeing_client(void)
{
    pmix_namespace_t *ns;
    pmix_status_t reply;
    char *msg;
    size_t sz;
    int sd1 = -1, sd2 = -1;

    if (PMIX_SUCCESS != start_server(true)) {
        return CHILD_SETUP;
    }
    msg = build_ack(false, true, "v61", PMIX_BFROP_BUFFER_NON_DESC, NULL, &sz);
    reply = send_ack(msg, sz, &sd1);
    free(msg);
    if (PMIX_SUCCESS != reply) {
        fprintf(stdout, "    a validated client was refused: %s\n", PMIx_Error_string(reply));
        return CHILD_FAIL;
    }
    msg = build_ack(false, true, "v21", PMIX_BFROP_BUFFER_FULLY_DESC, NULL, &sz);
    reply = send_ack(msg, sz, &sd2);
    free(msg);
    usleep(200000);
    ns = find_nspace();
    if (PMIX_SUCCESS == reply) {
        fprintf(stdout, "    a client disagreeing with its namespace's wire format was accepted\n");
        return CHILD_FAIL;
    }
    if (NULL == ns || NULL == ns->compat.bfrops ||
        0 != strcmp(ns->compat.bfrops->version, "v61") ||
        PMIX_BFROP_BUFFER_NON_DESC != ns->compat.type) {
        fprintf(stdout, "    the namespace's wire format was changed\n");
        return CHILD_FAIL;
    }
    return CHILD_PASS;
}

static int case_tool(bool goodcred)
{
    pmix_status_t reply;
    char *msg;
    size_t sz;
    int sd = -1;

    if (PMIX_SUCCESS != start_server(false)) {
        return CHILD_SETUP;
    }
    msg = build_ack(true, goodcred, "v61", PMIX_BFROP_BUFFER_NON_DESC, NULL, &sz);
    reply = send_ack(msg, sz, &sd);
    free(msg);
    usleep(200000);
    if (goodcred) {
        if (PMIX_SUCCESS != reply || 1 != ntool_connected) {
            fprintf(stdout, "    a validated tool did not connect: %s, host asked %d times\n",
                    PMIx_Error_string(reply), ntool_connected);
            return CHILD_FAIL;
        }
        return CHILD_PASS;
    }
    if (PMIX_SUCCESS == reply) {
        fprintf(stdout, "    the tool's first reply was success\n");
        return CHILD_FAIL;
    }
    if (0 != ntool_connected) {
        fprintf(stdout, "    the host was asked to give the tool an identity\n");
        return CHILD_FAIL;
    }
    return CHILD_PASS;
}

static void run_case(const char *name, int which)
{
    pid_t pid;
    int status, rc = CHILD_SETUP;
    char detail[64];

    fflush(stdout);
    fflush(stderr);
    pid = fork();
    if (0 == pid) {
        alarm(30);
        switch (which) {
        case 0:
            rc = case_bad_client();
            break;
        case 1:
            rc = case_disagreeing_client();
            break;
        case 2:
            rc = case_tool(false);
            break;
        default:
            rc = case_tool(true);
            break;
        }
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

int main(int argc, char **argv)
{
    PMIX_HIDE_UNUSED_PARAMS(argc, argv);

    fprintf(stdout, "\n=== a connecting peer is validated first ===\n\n");
    /* the credential below is psec/native's, so select it - a build with
     * a higher-priority module would otherwise validate with that */
    setenv("PMIX_MCA_psec", "native", 1);
    run_case("a client with a bad credential changes nothing", 0);
    run_case("a client disagreeing with its namespace's wire format is refused", 1);
    run_case("a tool with a bad credential is refused before the host is asked", 2);
    run_case("a tool with a good credential connects", 3);
    fprintf(stdout, "\n%d passed, %d failed\n", npass, nfail);
    return (0 == nfail) ? 0 : 1;
}
