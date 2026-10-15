/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Every connection records the ptl transport that carries it.
 *
 * The server's listener records the transport that opened it, and each
 * peer it accepts is given its listener's. A tool records the transport
 * whose address it connected to, by the blocking connect PMIx_tool_init
 * makes and by the event-driven one PMIx_tool_attach_to_server makes.
 * Today every one is tcp.
 *
 * Each tool runs in a child forked before any PMIx call, one after the
 * other, and reports what it recorded through its exit status; the pipes
 * keep the server's look inside the window where the tool is connected.
 */

#include "src/include/pmix_config.h"

#include "include/pmix.h"
#include "include/pmix_server.h"
#include "include/pmix_tool.h"
#include "src/client/pmix_client_ops.h"
#include "src/include/pmix_globals.h"
#include "src/mca/ptl/base/base.h"
#include "src/mca/ptl/tcp/ptl_tcp.h"
#include "src/server/pmix_server_ops.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define SERVER_NSPACE "transport.server"
/* a tool's exit status when its server peer had no transport, or not tcp */
#define TOOL_EXIT_WRONG_TRANSPORT 3

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

static void tool_connected_fn(pmix_info_t *info, size_t ninfo,
                              pmix_tool_connection_cbfunc_t cbfunc, void *cbdata)
{
    pmix_proc_t proc;
    char ns[64];
    PMIX_HIDE_UNUSED_PARAMS(info, ninfo);

    snprintf(ns, sizeof(ns), "transport.tool.%d", ntools++);
    PMIX_LOAD_PROCID(&proc, ns, 0);
    cbfunc(PMIX_SUCCESS, &proc, cbdata);
}

static pmix_server_module_t mymodule = {
    .tool_connected = tool_connected_fn
};

/* the tool half - runs in a child, which has made no PMIx call yet. With
 * `attach`, it initializes unconnected and attaches, which takes the
 * event-driven connect */
static int run_tool(bool attach, int urifd, int readyfd, int gofd)
{
    pmix_proc_t myproc, server;
    pmix_info_t tinfo, ainfo[2];
    pmix_status_t rc;
    pmix_peer_t *srv;
    char uri[2048], c = 'r';
    bool flag = true;
    ssize_t n;
    int ret = 0;

    n = read(urifd, uri, sizeof(uri) - 1);
    if (0 >= n) {
        /* the server never started */
        return 1;
    }
    uri[n] = '\0';

    if (attach) {
        PMIX_INFO_LOAD(&tinfo, PMIX_TOOL_DO_NOT_CONNECT, &flag, PMIX_BOOL);
    } else {
        PMIX_INFO_LOAD(&tinfo, PMIX_SERVER_URI, uri, PMIX_STRING);
    }
    rc = PMIx_tool_init(&myproc, &tinfo, 1);
    PMIX_INFO_DESTRUCT(&tinfo);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "  tool: PMIx_tool_init failed: %s\n", PMIx_Error_string(rc));
        return 1;
    }
    if (attach) {
        PMIX_INFO_LOAD(&ainfo[0], PMIX_SERVER_URI, uri, PMIX_STRING);
        PMIX_INFO_LOAD(&ainfo[1], PMIX_PRIMARY_SERVER, &flag, PMIX_BOOL);
        rc = PMIx_tool_attach_to_server(NULL, &server, ainfo, 2);
        PMIX_INFO_DESTRUCT(&ainfo[0]);
        PMIX_INFO_DESTRUCT(&ainfo[1]);
        if (PMIX_SUCCESS != rc) {
            fprintf(stderr, "  tool: attach failed: %s\n", PMIx_Error_string(rc));
            PMIx_tool_finalize();
            return 1;
        }
    }

    srv = pmix_client_globals.myserver;
    if (NULL == srv || &pmix_ptl_tcp_module != srv->transport) {
        fprintf(stderr, "  tool: its server peer records transport %s\n",
                (NULL == srv || NULL == srv->transport) ? "(none)" : srv->transport->name);
        ret = TOOL_EXIT_WRONG_TRANSPORT;
    }

    /* tell the server we are up, and stay connected until it has looked */
    if (1 != write(readyfd, &c, 1) || 1 != read(gofd, &c, 1)) {
        ret = 1;
    }
    PMIx_tool_finalize();
    return ret;
}

/* the server's look: every peer it holds records tcp */
static bool peers_record_tcp(int *npeers)
{
    pmix_peer_t *pr;
    int i;

    *npeers = 0;
    for (i = 0; i < pmix_server_globals.clients.size; i++) {
        pr = (pmix_peer_t *) pmix_pointer_array_get_item(&pmix_server_globals.clients, i);
        if (NULL == pr) {
            continue;
        }
        ++*npeers;
        if (&pmix_ptl_tcp_module != pr->transport) {
            return false;
        }
    }
    return true;
}

typedef struct {
    bool attach;
    pid_t pid;
    int uri, ready, go; // our ends of its pipes
} tool_t;

/* fork a tool before any PMIx call, so it does not inherit an initialized
 * library - a forked child of an initialized server has a copy of its
 * state but none of its threads. The tools started before it are not its
 * to hold open: their pipes must reach end-of-file when we close ours */
static bool start_tool(tool_t *tools, int n, bool attach)
{
    tool_t *t = &tools[n];
    int uripipe[2], readypipe[2], gopipe[2], k;

    t->attach = attach;
    if (0 != pipe(uripipe) || 0 != pipe(readypipe) || 0 != pipe(gopipe)) {
        return false;
    }
    fflush(stdout);
    t->pid = fork();
    if (0 > t->pid) {
        return false;
    }
    if (0 == t->pid) {
        for (k = 0; k < n; k++) {
            close(tools[k].uri);
            close(tools[k].ready);
            close(tools[k].go);
        }
        close(uripipe[1]);
        close(readypipe[0]);
        close(gopipe[1]);
        _exit(run_tool(attach, uripipe[0], readypipe[1], gopipe[0]));
    }
    close(uripipe[0]);
    close(readypipe[1]);
    close(gopipe[0]);
    t->uri = uripipe[1];
    t->ready = readypipe[0];
    t->go = gopipe[1];
    return true;
}

/* hand the tool the URI, look while it is connected, and reap it. A NULL
 * uri releases a tool whose server never started */
static void run_one(tool_t *t, const char *uri)
{
    const char *how = t->attach ? "an attaching tool" : "a tool connecting at init";
    char name[128], c = 'g';
    int status = 0, npeers = 0;

    if (NULL != uri && (ssize_t) strlen(uri) == write(t->uri, uri, strlen(uri))) {
        close(t->uri);
        if (1 != read(t->ready, &c, 1)) {
            snprintf(name, sizeof(name), "%s connected", how);
            report(name, 0, "the tool never reported ready");
        } else {
            snprintf(name, sizeof(name), "the server's peer for %s records tcp", how);
            report(name, peers_record_tcp(&npeers) && 0 < npeers,
                   "a peer records another, or none");
        }
    } else {
        close(t->uri);
    }
    if (1 != write(t->go, &c, 1)) {
        /* the tool is gone already */
        ;
    }
    close(t->go);
    close(t->ready);
    waitpid(t->pid, &status, 0);
    if (NULL == uri) {
        return;
    }
    snprintf(name, sizeof(name), "%s records tcp for its server", how);
    report(name, WIFEXITED(status) && 0 == WEXITSTATUS(status),
           (WIFEXITED(status) && TOOL_EXIT_WRONG_TRANSPORT == WEXITSTATUS(status))
               ? "it records another, or none" : "the tool failed");
}

int main(int argc, char **argv)
{
    pmix_status_t rc;
    pmix_info_t sinfo[2];
    char *uri = NULL;
    bool flag = true;
    tool_t tools[2];
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

    fprintf(stdout, "\n=== ptl transport recorded per connection ===\n\n");

    if (!start_tool(tools, 0, false) || !start_tool(tools, 1, true)) {
        fprintf(stderr, "could not start the tools\n");
        return 1;
    }

    PMIX_INFO_LOAD(&sinfo[0], PMIX_SERVER_TOOL_SUPPORT, &flag, PMIX_BOOL);
    PMIX_INFO_LOAD(&sinfo[1], PMIX_SERVER_NSPACE, SERVER_NSPACE, PMIX_STRING);
    rc = PMIx_server_init(&mymodule, sinfo, 2);
    PMIX_INFO_DESTRUCT(&sinfo[0]);
    PMIX_INFO_DESTRUCT(&sinfo[1]);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "PMIx_server_init failed: %s\n", PMIx_Error_string(rc));
        run_one(&tools[0], NULL);
        run_one(&tools[1], NULL);
        return 1;
    }

    report("the listener records the transport that opened it",
           &pmix_ptl_tcp_module == pmix_ptl_base.listener.transport, "not tcp");

    /* white-box: the URI is stored against our own rank under internal
     * scope, so read it off the listener */
    uri = pmix_ptl_base.listener.uri;
    report("the server published its URI", NULL != uri, "the listener has no URI");
    run_one(&tools[0], uri);
    run_one(&tools[1], uri);

    PMIx_server_finalize();
    fprintf(stdout, "\n%d passed, %d failed\n", npass, nfail);
    return (0 == nfail) ? 0 : 1;
}
