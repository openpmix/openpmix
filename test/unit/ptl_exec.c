/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Unit test: what becomes of a client's rank when its process execs, and
 * of the processes a client starts.
 *
 * A client's sockets are close-on-exec, so its connection closes when the
 * process execs - on the local socket and over TCP alike, and over TCP
 * also where the kernel cannot name the server's user, which a client
 * does not have prove who it is. The server
 * handles that as it handles any connection that ends without a
 * finalize: the rank is lost at once, as it is for a client that dies.
 *
 * A client takes the variables that say who it is and how it reaches its
 * server out of its environment as it initializes, so nothing it starts
 * inherits them. The program it execs, and a process it forks and execs,
 * find none of them, and their PMIx_Init comes up as a singleton. A tool
 * the client starts has no identity either, finds the server by its
 * rendezvous file, and comes up under the namespace the host gives it. A
 * tool whose caller names a client's rank keeps the identity it was asked
 * for, and is refused: it has no token for that rank.
 *
 * A tool that does find a connected rank's namespace and rank in its
 * environment - one a job script starts, which inherits the script's -
 * connects again as a tool that needs an identity, and comes up under the
 * namespace the host gives it; a tool the host will not give one is told
 * its rank is taken.
 *
 * Each case runs in a server of its own, which execs this program as its
 * clients. They signal the server over pipes whose descriptors are in
 * their environment.
 */

#include "src/include/pmix_config.h"

#include "include/pmix.h"
#include "include/pmix_server.h"
#include "include/pmix_tool.h"
#include "src/include/pmix_globals.h"
#include "src/mca/ptl/base/base.h"
#include "src/util/pmix_environ.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

#define RI_NSPACE "reinit.job"
/* the namespace the host gives a tool that needs one */
#define RI_TOOLNS "reinit.tool"
/* the descriptors a client and the server signal each other on */
#define RI_TOSERVER   "PMIX_UT_REINIT_TOSERVER"
#define RI_FROMSERVER "PMIX_UT_REINIT_FROMSERVER"
/* set: a client takes its kernel to name no TCP server's user */
#define RI_NOTABLE    "PMIX_UT_REINIT_NOTABLE"

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

/* ---- the clients ---- */

static int env_fd(const char *name)
{
    char *v = getenv(name);

    return (NULL == v) ? -1 : atoi(v);
}

/* initialize, say so, and wait for the go-ahead */
static int init_and_wait(void)
{
    pmix_proc_t me;
    pmix_status_t rc;
    char c = 'r';

    if (NULL != getenv(RI_NOTABLE)) {
        pmix_ptl_base_test_no_tcp_table = true;
    }
    rc = PMIx_Init(&me, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stdout, "    PMIx_Init failed: %s\n", PMIx_Error_string(rc));
        return CHILD_SETUP;
    }
    if (1 != write(env_fd(RI_TOSERVER), &c, 1) || 1 != read(env_fd(RI_FROMSERVER), &c, 1)) {
        return CHILD_SETUP;
    }
    return CHILD_PASS;
}

/* whether the environment holds none of the variables a client takes out
 * of its own - nor the security parameter its init makes of one of them */
static bool no_identity(void)
{
    static const char *const names[] = {"PMIX_NAMESPACE", "PMIX_RANK", "PMIX_SERVER_LOCAL_URI",
                                        "PMIX_CONNECT_TOKEN", "PMIX_SECURITY_MODE",
                                        "PMIX_BFROP_BUFFER_TYPE", "PMIX_GDS_MODULE",
                                        "PMIX_VERSION", "PMIX_MCA_psec", NULL};
    bool none = true;
    int n;

    for (n = 0; NULL != names[n]; n++) {
        if (NULL != getenv(names[n])) {
            fprintf(stdout, "    %s is in the environment\n", names[n]);
            none = false;
        }
    }
    for (n = 0; NULL != environ[n]; n++) {
        if (0 == strncmp(environ[n], "PMIX_SERVER_URI", strlen("PMIX_SERVER_URI"))) {
            fprintf(stdout, "    %.*s is in the environment\n",
                    (int) strcspn(environ[n], "="), environ[n]);
            none = false;
        }
    }
    if (NULL == getenv("PMIX_SERVER_TMPDIR")) {
        fprintf(stdout, "    PMIX_SERVER_TMPDIR is not in the environment\n");
        none = false;
    }
    return none;
}

/* a process started by an initialized client, with its environment: it
 * has none of the client's identity, and comes up as a singleton */
static int run_singleton(void)
{
    pmix_proc_t me;
    pmix_status_t rc;

    if (!no_identity()) {
        return CHILD_FAIL;
    }
    /* a singleton with no server to reach says so */
    rc = PMIx_Init(&me, NULL, 0);
    if (PMIX_SUCCESS != rc && PMIX_ERR_UNREACH != rc) {
        fprintf(stdout, "    PMIx_Init returned %s\n", PMIx_Error_string(rc));
        return CHILD_FAIL;
    }
    if (0 != strncmp(me.nspace, "singleton.", strlen("singleton."))) {
        fprintf(stdout, "    it came up as %s:%u\n", me.nspace, me.rank);
        PMIx_Finalize(NULL, 0);
        return CHILD_FAIL;
    }
    PMIx_Finalize(NULL, 0);
    return CHILD_PASS;
}

/* initialize, then exec a program that initializes in turn */
static int run_execer(char *self)
{
    char *args[3];
    int rc;

    if (CHILD_PASS != (rc = init_and_wait())) {
        return rc;
    }
    args[0] = self;
    args[1] = (char *) "singleton";
    args[2] = NULL;
    execve(self, args, environ);
    return CHILD_SETUP;
}

/* initialize, then fork a process that execs a program initializing with
 * this one's environment, as `role` */
static int run_forker(char *self, const char *role)
{
    char *args[3];
    pid_t child;
    int ret, status = 0;

    if (CHILD_PASS != (ret = init_and_wait())) {
        return ret;
    }
    child = fork();
    if (0 > child) {
        return CHILD_SETUP;
    }
    if (0 == child) {
        args[0] = self;
        args[1] = (char *) role;
        args[2] = NULL;
        execve(self, args, environ);
        _exit(CHILD_SETUP);
    }
    if (child != waitpid(child, &status, 0) || !WIFEXITED(status)) {
        ret = CHILD_SETUP;
    } else {
        ret = WEXITSTATUS(status);
    }
    PMIx_Finalize(NULL, 0);
    return ret;
}

/* a tool started by the client, with its environment */
static int run_tool(void)
{
    pmix_proc_t me;
    pmix_status_t rc;

    rc = PMIx_tool_init(&me, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stdout, "    the tool's PMIx_tool_init returned %s\n", PMIx_Error_string(rc));
        return CHILD_FAIL;
    }
    if (!PMIx_Check_nspace(me.nspace, RI_TOOLNS)) {
        fprintf(stdout, "    the tool came up as %s:%u\n", me.nspace, me.rank);
        PMIx_tool_finalize();
        return CHILD_FAIL;
    }
    PMIx_tool_finalize();
    return CHILD_PASS;
}

/* a tool started by an initialized client: it has none of the client's
 * identity */
static int run_clean_tool(void)
{
    if (!no_identity()) {
        return CHILD_FAIL;
    }
    return run_tool();
}

/* a tool started by a client, whose caller names the rank another client
 * holds */
static int run_named_tool(void)
{
    pmix_info_t info[2];
    pmix_proc_t me;
    pmix_rank_t rank = 0;
    pmix_status_t rc;

    PMIX_INFO_LOAD(&info[0], PMIX_TOOL_NSPACE, RI_NSPACE, PMIX_STRING);
    PMIX_INFO_LOAD(&info[1], PMIX_TOOL_RANK, &rank, PMIX_PROC_RANK);
    rc = PMIx_tool_init(&me, info, 2);
    PMIX_INFO_DESTRUCT(&info[0]);
    PMIX_INFO_DESTRUCT(&info[1]);
    /* it has no token for the rank - its caller's was not passed on - and
     * nothing of its caller's environment says how the rank's job formats
     * its messages: it is refused for the one or the other, as the build's
     * default format happens to differ from the server's or not */
    if (PMIX_ERR_INVALID_CRED != rc && PMIX_ERR_NOT_SUPPORTED != rc) {
        fprintf(stdout, "    the tool's PMIx_tool_init returned %s%s%s:%u\n",
                PMIx_Error_string(rc), (PMIX_SUCCESS == rc) ? " as " : "",
                (PMIX_SUCCESS == rc) ? me.nspace : "", (PMIX_SUCCESS == rc) ? me.rank : 0);
        if (PMIX_SUCCESS == rc) {
            PMIx_tool_finalize();
        }
        return CHILD_FAIL;
    }
    return CHILD_PASS;
}

/* a tool started by a client, which the host will not give a namespace of
 * its own */
static int run_refused_tool(void)
{
    pmix_proc_t me;
    pmix_status_t rc;

    rc = PMIx_tool_init(&me, NULL, 0);
    if (PMIX_ERR_EXISTS != rc) {
        fprintf(stdout, "    the tool's PMIx_tool_init returned %s\n", PMIx_Error_string(rc));
        if (PMIX_SUCCESS == rc) {
            PMIx_tool_finalize();
        }
        return CHILD_FAIL;
    }
    return CHILD_PASS;
}

/* initialize, then exec a program that never initializes and waits a
 * while: the process lives on without a connection */
static int run_idler(char *self)
{
    char *args[3];
    int rc;

    if (CHILD_PASS != (rc = init_and_wait())) {
        return rc;
    }
    args[0] = self;
    args[1] = (char *) "idle";
    args[2] = NULL;
    execve(self, args, environ);
    return CHILD_SETUP;
}

/* initialize, then die without finalizing */
static int run_dier(void)
{
    int rc;

    if (CHILD_PASS != (rc = init_and_wait())) {
        return rc;
    }
    _exit(CHILD_PASS);
}

/* initialize, wait for the go-ahead, and finalize */
static int run_holder(void)
{
    int ret;

    if (CHILD_PASS != (ret = init_and_wait())) {
        return ret;
    }
    PMIx_Finalize(NULL, 0);
    return CHILD_PASS;
}

/* ---- the server ---- */

static volatile int nconnected = 0;
static volatile int ntools = 0;
static volatile int nfinalized = 0;
static volatile int nlost = 0;

static pmix_status_t client_connected2(const pmix_proc_t *proc, void *server_object,
                                       pmix_info_t info[], size_t ninfo,
                                       pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    PMIX_HIDE_UNUSED_PARAMS(proc, server_object, info, ninfo, cbfunc, cbdata);

    ++nconnected;
    return PMIX_OPERATION_SUCCEEDED;
}

static pmix_status_t client_finalized(const pmix_proc_t *proc, void *server_object,
                                      pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    PMIX_HIDE_UNUSED_PARAMS(proc, server_object, cbfunc, cbdata);

    ++nfinalized;
    return PMIX_OPERATION_SUCCEEDED;
}

static void lost_handler(size_t evhdlr_registration_id, pmix_status_t status,
                         const pmix_proc_t *source, pmix_info_t info[], size_t ninfo,
                         pmix_info_t results[], size_t nresults,
                         pmix_event_notification_cbfunc_fn_t cbfunc, void *cbdata)
{
    PMIX_HIDE_UNUSED_PARAMS(evhdlr_registration_id, status, source, info, ninfo, results,
                            nresults);
    ++nlost;
    if (NULL != cbfunc) {
        cbfunc(PMIX_EVENT_ACTION_COMPLETE, NULL, 0, NULL, NULL, cbdata);
    }
}

/* a tool that brings no identity is given RI_TOOLNS - unless the host is
 * to refuse such tools */
static bool refuse_new_tools = false;
static pmix_status_t tool_connected2(pmix_info_t *info, size_t ninfo,
                                     pmix_tool_connection_cbfunc_t cbfunc, void *cbdata)
{
    pmix_proc_t proc;
    bool named = false;
    size_t n;

    ++ntools;
    PMIX_LOAD_PROCID(&proc, RI_TOOLNS, 0);
    for (n = 0; n < ninfo; n++) {
        if (PMIx_Check_key(info[n].key, PMIX_NSPACE)) {
            PMIX_LOAD_NSPACE(proc.nspace, info[n].value.data.string);
            named = true;
        } else if (PMIx_Check_key(info[n].key, PMIX_RANK)) {
            proc.rank = info[n].value.data.rank;
        }
    }
    if (refuse_new_tools && !named) {
        return PMIX_ERR_NO_PERMISSIONS;
    }
    cbfunc(PMIX_SUCCESS, &proc, cbdata);
    return PMIX_SUCCESS;
}

static pmix_server_module_t mymodule = {
    .client_connected2 = client_connected2,
    .client_finalized = client_finalized,
    .tool_connected2 = tool_connected2
};

static pmix_status_t start_server(pmix_rank_t nprocs)
{
    pmix_status_t rc, code = PMIX_ERR_LOST_CONNECTION;
    pmix_proc_t proc;
    pmix_nspace_t ns;
    pmix_rank_t r;

    {
        /* tools connect here, which a server takes only with tool support */
        pmix_info_t tsinfo;
        bool tsflag = true;

        PMIX_INFO_LOAD(&tsinfo, PMIX_SERVER_TOOL_SUPPORT, &tsflag, PMIX_BOOL);
        rc = PMIx_server_init(&mymodule, &tsinfo, 1);
        PMIX_INFO_DESTRUCT(&tsinfo);
    }
    if (PMIX_SUCCESS != rc) {
        return rc;
    }
    PMIx_Register_event_handler(&code, 1, NULL, 0, lost_handler, NULL, NULL);
    PMIX_LOAD_NSPACE(ns, RI_NSPACE);
    rc = PMIx_server_register_nspace(ns, (int) nprocs, NULL, 0, NULL, NULL);
    if (PMIX_SUCCESS != rc && PMIX_OPERATION_SUCCEEDED != rc) {
        return rc;
    }
    for (r = 0; r < nprocs; r++) {
        PMIX_LOAD_PROCID(&proc, RI_NSPACE, r);
        rc = PMIx_server_register_client(&proc, geteuid(), getegid(), NULL, NULL, NULL);
        if (PMIX_SUCCESS != rc && PMIX_OPERATION_SUCCEEDED != rc) {
            return rc;
        }
    }
    return PMIX_SUCCESS;
}

typedef struct {
    pid_t pid;
    int to;   // the server reads what the client says here
    int from; // and writes the go-ahead here
} client_t;

/* start this program as `rank`, in `role` */
/* set to start clients without the local socket's address, so they
 * connect over TCP as a client of an earlier release does */
static bool client_on_tcp = false;

static bool start_client(char *self, pmix_rank_t rank, const char *role, client_t *cl)
{
    pmix_proc_t proc;
    char **env, *args[3], num[16];
    int to[2], from[2];

    if (0 != pipe(to) || 0 != pipe(from)) {
        return false;
    }
    /* our own environment first - it carries the library search path the
     * libtool wrapper set - then the client's PMIx variables */
    env = PMIx_Argv_copy(environ);
    PMIX_LOAD_PROCID(&proc, RI_NSPACE, rank);
    if (PMIX_SUCCESS != PMIx_server_setup_fork(&proc, &env)) {
        return false;
    }
    if (client_on_tcp) {
        (void) pmix_unsetenv(PMIX_PTL_LOCAL_URI_ENV, &env);
    }
    snprintf(num, sizeof(num), "%d", to[1]);
    PMIx_Setenv(RI_TOSERVER, num, true, &env);
    snprintf(num, sizeof(num), "%d", from[0]);
    PMIx_Setenv(RI_FROMSERVER, num, true, &env);
    cl->pid = fork();
    if (0 > cl->pid) {
        return false;
    }
    if (0 == cl->pid) {
        args[0] = self;
        args[1] = (char *) role;
        args[2] = NULL;
        execve(self, args, env);
        _exit(CHILD_SETUP);
    }
    PMIx_Argv_free(env);
    close(to[1]);
    close(from[0]);
    cl->to = to[0];
    cl->from = from[1];
    return true;
}

static bool hear(client_t *cl)
{
    char c;

    return 1 == read(cl->to, &c, 1);
}

static bool go(client_t *cl)
{
    char c = 'g';

    return 1 == write(cl->from, &c, 1);
}

static int finish(client_t *cl)
{
    int status = 0;

    if (cl->pid != waitpid(cl->pid, &status, 0) || !WIFEXITED(status)) {
        fprintf(stdout, "    a client did not exit normally\n");
        return CHILD_FAIL;
    }
    return WEXITSTATUS(status);
}

/* what the server counted, against what it should have */
static int expect(int connected, int finalized, size_t nlocal)
{
    pmix_namespace_t *ns, *found = NULL;

    /* past the window a lost-connection event is gathered in */
    sleep(2);
    PMIX_LIST_FOREACH (ns, &pmix_globals.nspaces, pmix_namespace_t) {
        if (PMIx_Check_nspace(ns->nspace, RI_NSPACE)) {
            found = ns;
        }
    }
    if (connected != nconnected || finalized != nfinalized || 0 != nlost ||
        NULL == found || nlocal != found->nlocalprocs) {
        fprintf(stdout, "    the host was asked about %d connections (want %d), told of %d "
                        "finalizes (want %d); %d lost-connection "
                        "events; the job counts %zu local processes (want %zu)\n",
                nconnected, connected, nfinalized, finalized, nlost,
                (NULL == found) ? 0 : found->nlocalprocs, nlocal);
        return CHILD_FAIL;
    }
    return CHILD_PASS;
}

/* The seconds after the go-ahead in which the server raised its one
 * lost-connection event, or -1 if it raised none within `limit` */
static double lost_after(double limit)
{
    struct timespec t0, t1;
    double secs = 0.0;

    clock_gettime(CLOCK_MONOTONIC, &t0);
    while (0 == nlost && secs < limit) {
        usleep(50000);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        secs = (double) (t1.tv_sec - t0.tv_sec) + (double) (t1.tv_nsec - t0.tv_nsec) / 1.0e9;
    }
    return (0 == nlost) ? -1.0 : secs;
}

/* a process the client forked and exec'd has none of its identity */
static int run_forked_child(char *self)
{
    client_t cl;
    int rc;

    if (PMIX_SUCCESS != start_server(1) || !start_client(self, 0, "forker", &cl)) {
        return CHILD_SETUP;
    }
    if (!hear(&cl) || !go(&cl)) {
        return CHILD_SETUP;
    }
    if (CHILD_PASS != (rc = finish(&cl))) {
        return rc;
    }
    return expect(1, 1, 1);
}

/* a tool a client started has none of its identity, and is given one */
static int run_child_tool(char *self)
{
    client_t cl;
    int rc;

    if (PMIX_SUCCESS != start_server(1) || !start_client(self, 0, "toolparent", &cl)) {
        return CHILD_SETUP;
    }
    if (!hear(&cl) || !go(&cl)) {
        return CHILD_SETUP;
    }
    if (CHILD_PASS != (rc = finish(&cl))) {
        return rc;
    }
    if (1 != ntools) {
        fprintf(stdout, "    the host was asked about %d tools\n", ntools);
        return CHILD_FAIL;
    }
    /* the tool's finalize is told to the host as well */
    return expect(1, 2, 1);
}

/* rank 1 starts a tool whose caller names rank 0, which is connected */
static int run_named_child_tool(char *self)
{
    client_t cl0, cl1;
    int rc0, rc1;

    if (PMIX_SUCCESS != start_server(2) || !start_client(self, 0, "holder", &cl0) ||
        !start_client(self, 1, "namedtoolparent", &cl1)) {
        return CHILD_SETUP;
    }
    if (!hear(&cl0) || !hear(&cl1) || !go(&cl1)) {
        return CHILD_SETUP;
    }
    rc1 = finish(&cl1);
    if (!go(&cl0)) {
        return CHILD_SETUP;
    }
    rc0 = finish(&cl0);
    if (CHILD_PASS != rc0 || CHILD_PASS != rc1) {
        return (CHILD_SETUP == rc0 || CHILD_SETUP == rc1) ? CHILD_SETUP : CHILD_FAIL;
    }
    if (0 != ntools) {
        fprintf(stdout, "    the host was asked about %d tools\n", ntools);
        return CHILD_FAIL;
    }
    return expect(2, 2, 2);
}

/* rank 0 is connected; a tool is started with rank 0's environment, as a
 * job script would start one. `refuse`: the host gives it no namespace */
static int case_script_tool(char *self, bool refuse)
{
    client_t cl0, cl1;
    int rc0, rc1;

    refuse_new_tools = refuse;
    if (PMIX_SUCCESS != start_server(1) || !start_client(self, 0, "holder", &cl0) ||
        !hear(&cl0)) {
        return CHILD_SETUP;
    }
    if (!start_client(self, 0, refuse ? "refusedtool" : "tool", &cl1)) {
        return CHILD_SETUP;
    }
    rc1 = finish(&cl1);
    if (!go(&cl0)) {
        return CHILD_SETUP;
    }
    rc0 = finish(&cl0);
    if (CHILD_PASS != rc0 || CHILD_PASS != rc1) {
        return (CHILD_SETUP == rc0 || CHILD_SETUP == rc1) ? CHILD_SETUP : CHILD_FAIL;
    }
    if (1 != ntools) {
        fprintf(stdout, "    the host was asked about %d tools\n", ntools);
        return CHILD_FAIL;
    }
    return expect(1, refuse ? 1 : 2, 1);
}
static int run_script_tool(char *self)
{
    return case_script_tool(self, false);
}
static int run_refused_script_tool(char *self)
{
    return case_script_tool(self, true);
}

/* a client execs a program that initializes: the client is lost, and the
 * program comes up as a singleton */
static int run_exec_singleton(char *self)
{
    client_t cl;
    double secs;
    int rc;

    if (PMIX_SUCCESS != start_server(1) || !start_client(self, 0, "execer", &cl)) {
        return CHILD_SETUP;
    }
    if (!hear(&cl) || !go(&cl)) {
        return CHILD_SETUP;
    }
    secs = lost_after(10.0);
    if (CHILD_PASS != (rc = finish(&cl))) {
        return rc;
    }
    if (0 > secs || 1 != nlost || 1 != nconnected || 0 != nfinalized) {
        fprintf(stdout, "    %d lost-connection events; the host was asked about %d "
                        "connections and told of %d finalizes\n",
                nlost, nconnected, nfinalized);
        return CHILD_FAIL;
    }
    return CHILD_PASS;
}

/* a client execs a program that never connects: its connection closed in
 * the exec, it is lost at once */
static int run_exec_lost(char *self)
{
    client_t cl;
    double secs;
    int rc;

    if (PMIX_SUCCESS != start_server(1) || !start_client(self, 0, "idler", &cl)) {
        return CHILD_SETUP;
    }
    if (!hear(&cl) || !go(&cl)) {
        return CHILD_SETUP;
    }
    secs = lost_after(10.0);
    if (CHILD_PASS != (rc = finish(&cl))) {
        return rc;
    }
    /* the program it exec'd waits five seconds: the loss comes before */
    if (0 > secs || 3.0 < secs || 1 != nlost) {
        fprintf(stdout, "    %d lost-connection events, the first %.1f seconds after the "
                        "exec\n",
                nlost, secs);
        return CHILD_FAIL;
    }
    return CHILD_PASS;
}

#if PMIX_ENABLE_LEGACY_PEERS
/* the same over TCP - where the server listens only when it takes clients
 * of earlier releases */
static int run_exec_lost_tcp(char *self)
{
    setenv("PMIX_MCA_ptl_base_legacy_peers", "1", 1);
    client_on_tcp = true;
    return run_exec_lost(self);
}

/* and where the kernel cannot name the server's user: a client connects
 * as before - only a process its server did not start has the server
 * prove who it is - and is checked by the server as before */
static int run_exec_lost_tcp_notable(char *self)
{
    setenv(RI_NOTABLE, "1", 1);
    setenv("PMIX_MCA_psec_native_legacy_auth", "1", 1);
    return run_exec_lost_tcp(self);
}
#endif

/* a client dies without finalizing: it is lost at once */
static int run_died(char *self)
{
    client_t cl;
    double secs;

    if (PMIX_SUCCESS != start_server(1) || !start_client(self, 0, "dier", &cl)) {
        return CHILD_SETUP;
    }
    if (!hear(&cl) || !go(&cl)) {
        return CHILD_SETUP;
    }
    (void) finish(&cl);
    secs = lost_after(10.0);
    if (0 > secs || 3.0 < secs || 1 != nlost) {
        fprintf(stdout, "    %d lost-connection events, the first %.1f seconds after the "
                        "client died\n",
                nlost, secs);
        return CHILD_FAIL;
    }
    return CHILD_PASS;
}

typedef int (*case_fn_t)(char *self);

static void run_case(const char *name, case_fn_t fn, char *self)
{
    pid_t pid;
    int status, rc;

    fflush(stdout);
    fflush(stderr);
    pid = fork();
    if (0 == pid) {
        alarm(30);
        rc = fn(self);
        fflush(stdout);
        PMIx_server_finalize();
        _exit(rc);
    }
    if (0 > pid || pid != waitpid(pid, &status, 0)) {
        report(name, 0, "fork/wait failed");
    } else if (WIFSIGNALED(status)) {
        report(name, 0, "the server died on a signal");
    } else if (CHILD_SETUP == WEXITSTATUS(status)) {
        report(name, 0, "test setup failed");
    } else {
        report(name, CHILD_PASS == WEXITSTATUS(status), "see above");
    }
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);

    if (1 < argc) {
        if (0 == strcmp(argv[1], "execer")) {
            return run_execer(argv[0]);
        }
        if (0 == strcmp(argv[1], "singleton")) {
            return run_singleton();
        }
        if (0 == strcmp(argv[1], "forker")) {
            return run_forker(argv[0], "singleton");
        }
        if (0 == strcmp(argv[1], "toolparent")) {
            return run_forker(argv[0], "cleantool");
        }
        if (0 == strcmp(argv[1], "cleantool")) {
            return run_clean_tool();
        }
        if (0 == strcmp(argv[1], "tool")) {
            return run_tool();
        }
        if (0 == strcmp(argv[1], "idler")) {
            return run_idler(argv[0]);
        }
        if (0 == strcmp(argv[1], "idle")) {
            sleep(5);
            return CHILD_PASS;
        }
        if (0 == strcmp(argv[1], "dier")) {
            return run_dier();
        }
        if (0 == strcmp(argv[1], "holder")) {
            return run_holder();
        }
        if (0 == strcmp(argv[1], "namedtoolparent")) {
            return run_forker(argv[0], "namedtool");
        }
        if (0 == strcmp(argv[1], "namedtool")) {
            return run_named_tool();
        }
        if (0 == strcmp(argv[1], "refusedtool")) {
            return run_refused_tool();
        }
        return CHILD_SETUP;
    }

    fprintf(stdout, "\n=== a client that execs, and the processes a client starts ===\n\n");
    run_case("a client whose process execs is lost at once", run_exec_lost, argv[0]);
#if PMIX_ENABLE_LEGACY_PEERS
    run_case("a client on TCP whose process execs is lost at once", run_exec_lost_tcp,
             argv[0]);
    run_case("...also where the kernel cannot name its server", run_exec_lost_tcp_notable,
             argv[0]);
#endif
    run_case("a program a client execs has none of its identity, and is a singleton",
             run_exec_singleton, argv[0]);
    run_case("a process a client forks and execs has none of its identity, and is a singleton",
             run_forked_child, argv[0]);
    run_case("a tool a client starts has none of its identity, and is given one by the host",
             run_child_tool, argv[0]);
    run_case("a tool whose caller names another rank keeps that identity and is refused",
             run_named_child_tool, argv[0]);
    run_case("a tool with a connected rank's identity comes up under one the host gives it",
             run_script_tool, argv[0]);
    run_case("a tool with a connected rank's identity the host gives none is told it is taken",
             run_refused_script_tool, argv[0]);
    run_case("a client that dies without finalizing is lost at once", run_died, argv[0]);

    fprintf(stdout, "\n%d passed, %d failed\n", npass, nfail);
    return (0 == nfail) ? 0 : 1;
}
