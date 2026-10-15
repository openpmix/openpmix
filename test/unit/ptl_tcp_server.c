/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * A client or tool that connects to a server on its own host over TCP
 * checks who the server is, as it does on the local socket: the kernel's
 * table of TCP connections names the owner of the server's end. It
 * records no group, so the server must be the process's own user, root,
 * or a user ptl_base_server_access names. Any other is refused with
 * PMIX_ERR_INVALID_CRED before anything is sent to it.
 *
 * A tool connects here to a server's TCP listener - which a server opens
 * for clients and tools of earlier releases - by its URI.
 *
 * A server at an address of this host whose end of the connection the
 * kernel's table does not show - one that defers its accept until data
 * arrives, here, where the platform can - is refused the same way, and is
 * sent nothing.
 *
 * Run as root, the server runs as another user (65534) and two tools as
 * a third (1): one is refused by that check, and one that names the
 * server's user in ptl_base_server_access gets past it, to be refused by
 * the server as a tool of another user. A tool of the server's user is
 * taken - which, run as anyone else, is the only case.
 */

#include "src/include/pmix_config.h"

#include "include/pmix_server.h"
#include "include/pmix_tool.h"
#include "src/include/pmix_globals.h"
#include "src/mca/ptl/base/base.h"

#include <grp.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define SERVER_UID 65534
#define TOOL_UID   1

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

static int ntools = 0;

static void tool_connected(pmix_info_t *info, size_t ninfo, pmix_tool_connection_cbfunc_t cbfunc,
                           void *cbdata)
{
    pmix_proc_t proc;
    PMIX_HIDE_UNUSED_PARAMS(info, ninfo);

    PMIX_LOAD_PROCID(&proc, "tcpserver.tool", ntools++);
    cbfunc(PMIX_SUCCESS, &proc, cbdata);
}

static pmix_server_module_t mymodule = {
    .tool_connected = tool_connected
};

/* become uid, with the scratch directory as home - root's is not theirs
 * to read */
static bool become(uid_t uid)
{
    gid_t gid = (gid_t) uid;

    if (0 != setgroups(1, &gid) || 0 != setgid(gid) || 0 != setuid(uid)) {
        return false;
    }
    setenv("HOME", getenv("TMPDIR"), 1);
    return true;
}

/* the server: listen with a TCP listener, say where, and wait to be told
 * to go */
static void run_server(int urifd, int gofd)
{
    pmix_info_t info[2];
    bool flag = true;
    char c;

    if (0 == getuid() && !become(SERVER_UID)) {
        _exit(2);
    }
    PMIX_INFO_LOAD(&info[0], PMIX_SERVER_TOOL_SUPPORT, &flag, PMIX_BOOL);
    PMIX_INFO_LOAD(&info[1], PMIX_SERVER_LEGACY_PEERS, &flag, PMIX_BOOL);
    if (PMIX_SUCCESS != PMIx_server_init(&mymodule, info, 2) ||
        NULL == pmix_ptl_base.listener.uri) {
        _exit(2);
    }
    if ((ssize_t) strlen(pmix_ptl_base.listener.uri) + 1 !=
        write(urifd, pmix_ptl_base.listener.uri, strlen(pmix_ptl_base.listener.uri) + 1)) {
        _exit(2);
    }
    close(urifd);
    if (0 > read(gofd, &c, 1)) {
        _exit(2);
    }
    PMIx_server_finalize();
    _exit(0);
}

/* a tool, as uid `uid` (none if 0 and we are not root), connecting to the
 * URI; it exits with the status's negation */
static int run_tool(const char *uri, uid_t uid, const char *access)
{
    pmix_info_t info;
    pmix_proc_t me;
    pmix_status_t rc;
    pid_t pid;
    int status;

    fflush(stdout);
    pid = fork();
    if (0 == pid) {
        if (0 == getuid() && 0 != uid && !become(uid)) {
            _exit(255);
        }
        if (NULL != access) {
            setenv("PMIX_MCA_ptl_base_server_access", access, 1);
        }
        PMIX_INFO_LOAD(&info, PMIX_SERVER_URI, uri, PMIX_STRING);
        rc = PMIx_tool_init(&me, &info, 1);
        if (PMIX_SUCCESS == rc) {
            PMIx_tool_finalize();
        }
        _exit((-rc) & 0xff);
    }
    if (0 > pid || pid != waitpid(pid, &status, 0) || !WIFEXITED(status)) {
        return 255;
    }
    return WEXITSTATUS(status);
}

#if defined(TCP_DEFER_ACCEPT)
/* a listener on loopback that the kernel shows no connection of until data
 * arrives; a tool is pointed at it */
static void case_deferred(void)
{
    struct sockaddr_in sa;
    pmix_socklen_t len = sizeof(sa);
    char uri[128], detail[128];
    int sd, on = 30, st;

    sd = socket(AF_INET, SOCK_STREAM, 0);
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (0 > sd || 0 != setsockopt(sd, IPPROTO_TCP, TCP_DEFER_ACCEPT, &on, sizeof(on)) ||
        0 != bind(sd, (struct sockaddr *) &sa, sizeof(sa)) || 0 != listen(sd, 4) ||
        0 != getsockname(sd, (struct sockaddr *) &sa, &len)) {
        report("a server on this host the kernel does not show is refused", 0,
               "could not make the listener");
        return;
    }
    snprintf(uri, sizeof(uri), "deferred.server.0;tcp4://127.0.0.1:%u",
             (unsigned) ntohs(sa.sin_port));
    /* without the check the tool would send its request and wait for a
     * reply that never comes - not long */
    setenv("PMIX_MCA_ptl_base_handshake_wait_time", "2", 1);
    st = run_tool(uri, 0, NULL);
    unsetenv("PMIX_MCA_ptl_base_handshake_wait_time");
    close(sd);
    snprintf(detail, sizeof(detail), "the tool's init returned %d", -st);
    report("a server on this host the kernel does not show is refused",
           -st == PMIX_ERR_INVALID_CRED, detail);
}
#endif

int main(void)
{
    char tmpdir[1024], uri[1024], detail[128];
    const char *base = getenv("TMPDIR");
    int urip[2], gop[2], st;
    pid_t server;
    ssize_t n;
    size_t got = 0;

    fprintf(stdout, "\n=== a server on this host, over TCP ===\n\n");
#if defined(TCP_DEFER_ACCEPT)
    case_deferred();
#endif
#if !PMIX_ENABLE_LEGACY_PEERS
    /* the server's TCP listener is the one it opens for clients and tools
     * of earlier releases */
    fprintf(stdout, "this PMIx takes no clients or tools of earlier releases, so the "
                    "server has no TCP listener to test\n");
    fprintf(stdout, "\n%d passed, %d failed\n", npass, nfail);
    return (0 < nfail) ? 1 : ((0 < npass) ? 0 : 77);
#endif
    /* as root, the other users must reach it: not beneath a TMPDIR of
     * root's own, which they may not search */
    if (NULL == base || 0 == getuid()) {
        base = "/tmp";
    }
    snprintf(tmpdir, sizeof(tmpdir), "%s/pmix-tcps-XXXXXX", base);
    if (NULL == mkdtemp(tmpdir) || 0 != chmod(tmpdir, 01777)) {
        fprintf(stderr, "could not make a scratch directory\n");
        return 1;
    }
    setenv("TMPDIR", tmpdir, 1);
    signal(SIGPIPE, SIG_IGN);

    if (0 != pipe(urip) || 0 != pipe(gop)) {
        return 1;
    }
    server = fork();
    if (0 == server) {
        close(urip[0]);
        close(gop[1]);
        run_server(urip[1], gop[0]);
    }
    close(urip[1]);
    close(gop[0]);
    while (got < sizeof(uri) &&
           0 < (n = read(urip[0], uri + got, sizeof(uri) - got))) {
        got += (size_t) n;
        if ('\0' == uri[got - 1]) {
            break;
        }
    }
    if (0 == got || '\0' != uri[got - 1]) {
        fprintf(stderr, "the server did not start\n");
        return 1;
    }

    if (0 == getuid()) {
        st = run_tool(uri, TOOL_UID, NULL);
        snprintf(detail, sizeof(detail), "the tool's init returned %d", -st);
        report("a tool refuses a server of another user over TCP",
               -st == PMIX_ERR_INVALID_CRED, detail);
        st = run_tool(uri, TOOL_UID, "users=65534");
        snprintf(detail, sizeof(detail), "the tool's init returned %d", -st);
        report("a tool takes one ptl_base_server_access names - and that server refuses "
               "a tool of another user",
               -st == PMIX_ERR_NOT_SUPPORTED, detail);
    } else {
        fprintf(stdout, "  (not root: the server is this user's)\n");
    }
    st = run_tool(uri, (0 == getuid()) ? SERVER_UID : 0, NULL);
    snprintf(detail, sizeof(detail), "the tool's init returned %d", -st);
    report("a tool takes a server of its own user over TCP", 0 == st, detail);

    if (1 != write(gop[1], "x", 1)) {
        return 1;
    }
    (void) waitpid(server, &st, 0);
    rmdir(tmpdir);
    fprintf(stdout, "\n%d passed, %d failed\n", npass, nfail);
    return (0 == nfail) ? 0 : 1;
}
