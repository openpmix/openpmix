/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * What PMIx_Init, PMIx_tool_init and PMIx_server_init do on the paths
 * that set up a process's connection.
 *
 * With no server running:
 *
 *   tool retry       -> a PMIx_tool_init refused for its input leaves
 *                       nothing behind: the next call succeeds
 *   launcher file    -> a launcher that does not connect writes the
 *                       rendezvous file it was given, and replaces one a
 *                       process no longer running left there
 *   namespace alone  -> a client given a namespace but no server URI
 *                       fails its init rather than run as a singleton
 *   named TCP URI    -> a client's "nspace.rank;uri" directive naming a
 *                       TCP address is refused
 *
 * With a system server in this process:
 *
 *   server lost      -> a later PMIx_Init after the client's server
 *                       connection was lost returns
 *                       PMIX_ERR_LOST_CONNECTION
 *   named URI        -> a client's "nspace.rank;uri" directive naming the
 *                       server's local socket connects
 *   singleton        -> a singleton the system server does not accept
 *                       says so, and runs as a singleton
 *   server attach    -> a server told to connect to another, by file or
 *                       by role, does so on a peer of its own and makes
 *                       it its server; its own name is unchanged
 *   bad directive    -> a server's malformed connect directive fails its
 *                       init, though the connection was optional
 *
 * Every child is forked before any PMIx call, and is handed its
 * environment through a pipe.
 */

#include "src/include/pmix_config.h"

#include "include/pmix.h"
#include "include/pmix_server.h"
#include "include/pmix_tool.h"
#include "src/client/pmix_client_ops.h"
#include "src/include/pmix_globals.h"
#include "src/mca/ptl/base/base.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define IP_NSPACE  "init.paths"
#define IP_SERVER  "init.paths.server"
#define NKIDS      11
#define NOUT       4

static int npass = 0;
static int nfail = 0;
static char topdir[] = "/tmp/pmix-init-paths.XXXXXX";

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

static bool put(int fd, const void *buf, size_t len)
{
    return (ssize_t) len == write(fd, buf, len);
}

/* ---- the host ---- */

static volatile int ntools = 0;

static pmix_status_t client_connected2(const pmix_proc_t *proc, void *server_object,
                                       pmix_info_t info[], size_t ninfo,
                                       pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    PMIX_HIDE_UNUSED_PARAMS(proc, server_object, info, ninfo, cbfunc, cbdata);
    return PMIX_OPERATION_SUCCEEDED;
}

static pmix_status_t client_finalized(const pmix_proc_t *proc, void *server_object,
                                      pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    PMIX_HIDE_UNUSED_PARAMS(proc, server_object, cbfunc, cbdata);
    return PMIX_OPERATION_SUCCEEDED;
}

/* a tool - here, a server connecting to us - keeps the name it gives, or
 * is given one of its own */
static void tool_connected(pmix_info_t *info, size_t ninfo,
                           pmix_tool_connection_cbfunc_t cbfunc, void *cbdata)
{
    pmix_proc_t proc;
    char name[PMIX_MAX_NSLEN + 1];
    size_t n;

    snprintf(name, sizeof(name), "init.paths.tool.%d", ntools);
    PMIX_LOAD_PROCID(&proc, name, 0);
    for (n = 0; n < ninfo; n++) {
        if (PMIX_CHECK_KEY(&info[n], PMIX_NSPACE) && PMIX_STRING == info[n].value.type &&
            NULL != info[n].value.data.string) {
            PMIX_LOAD_NSPACE(proc.nspace, info[n].value.data.string);
        } else if (PMIX_CHECK_KEY(&info[n], PMIX_RANK)) {
            proc.rank = info[n].value.data.rank;
        }
    }
    ++ntools;
    cbfunc(PMIX_SUCCESS, &proc, cbdata);
}

static pmix_server_module_t mymodule = {
    .client_connected2 = client_connected2,
    .client_finalized = client_finalized,
    .tool_connected = tool_connected
};

/* ---- the children ---- */

typedef struct {
    pid_t pid;
    int env;    // our end of the pipe its environment goes down
    int result; // our end of the pipe its results come back on
} kid_t;

/* read "NAME=VALUE" lines, ended by an empty one, into the environment */
static bool read_env(int fd)
{
    char line[4096], *eq;
    size_t n = 0;
    char c;

    while (1 == read(fd, &c, 1)) {
        if ('\n' != c) {
            if (n < sizeof(line) - 1) {
                line[n++] = c;
            }
            continue;
        }
        line[n] = '\0';
        if (0 == n) {
            return true;
        }
        n = 0;
        eq = strchr(line, '=');
        if (NULL != eq) {
            *eq = '\0';
            setenv(line, eq + 1, 1);
        }
    }
    return false;
}

static pmix_status_t ok_if(bool cond)
{
    return cond ? PMIX_SUCCESS : PMIX_ERROR;
}

/* a tool refused for its input, then a tool */
static void tool_retry(pmix_status_t *out)
{
    pmix_proc_t me;
    pmix_info_t info;
    bool flag = true;

    /* a namespace without a rank */
    PMIX_INFO_LOAD(&info, PMIX_TOOL_NSPACE, "init.paths.lone", PMIX_STRING);
    out[0] = PMIx_tool_init(&me, &info, 1);
    PMIX_INFO_DESTRUCT(&info);
    PMIX_INFO_LOAD(&info, PMIX_TOOL_DO_NOT_CONNECT, &flag, PMIX_BOOL);
    out[1] = PMIx_tool_init(&me, &info, 1);
    PMIX_INFO_DESTRUCT(&info);
    if (PMIX_SUCCESS == out[1]) {
        out[2] = PMIx_tool_finalize();
    }
}

/* a launcher that does not connect, given a rendezvous file */
static void launcher_file(pmix_status_t *out)
{
    pmix_proc_t me;
    pmix_info_t info[3];
    bool flag = true;
    const char *path = getenv("IP_FILE");
    char line[1024] = "";
    FILE *fp;

    if (NULL == path) {
        return;
    }
    if (NULL != getenv("IP_STALE")) {
        /* what a launcher that is gone left behind */
        fp = fopen(path, "w");
        if (NULL == fp) {
            return;
        }
        fprintf(fp, "stale\n");
        fclose(fp);
    }
    PMIX_INFO_LOAD(&info[0], PMIX_LAUNCHER, &flag, PMIX_BOOL);
    PMIX_INFO_LOAD(&info[1], PMIX_TOOL_DO_NOT_CONNECT, &flag, PMIX_BOOL);
    PMIX_INFO_LOAD(&info[2], PMIX_LAUNCHER_RENDEZVOUS_FILE, path, PMIX_STRING);
    out[0] = PMIx_tool_init(&me, info, 3);
    PMIX_INFO_DESTRUCT(&info[0]);
    PMIX_INFO_DESTRUCT(&info[1]);
    PMIX_INFO_DESTRUCT(&info[2]);
    fp = fopen(path, "r");
    if (NULL != fp) {
        if (NULL == fgets(line, sizeof(line), fp)) {
            line[0] = '\0';
        }
        fclose(fp);
    }
    /* our URI, not what was there */
    out[1] = ok_if(NULL != strstr(line, "://"));
    if (PMIX_SUCCESS == out[0]) {
        out[2] = PMIx_tool_finalize();
    }
}

/* a client, with what it was given */
static void client_init(pmix_status_t *out)
{
    pmix_proc_t me;
    pmix_info_t info;
    const char *uri = getenv("IP_URI");

    if (NULL != uri) {
        PMIX_INFO_LOAD(&info, PMIX_SERVER_URI, uri, PMIX_STRING);
        out[0] = PMIx_Init(&me, &info, 1);
        PMIX_INFO_DESTRUCT(&info);
    } else {
        out[0] = PMIx_Init(&me, NULL, 0);
    }
    if (PMIX_SUCCESS == out[0] || PMIX_ERR_UNREACH == out[0]) {
        out[1] = PMIx_Finalize(NULL, 0);
    }
}

/* a client whose server goes away: the first status goes back at once,
 * the rest once a later init stops succeeding */
static void server_lost(int resultfd, pmix_status_t *out)
{
    pmix_proc_t me;
    int n;

    out[0] = PMIx_Init(&me, NULL, 0);
    (void) put(resultfd, &out[0], sizeof(out[0]));
    if (PMIX_SUCCESS != out[0]) {
        return;
    }
    out[1] = PMIX_SUCCESS;
    for (n = 0; n < 500 && PMIX_SUCCESS == out[1]; n++) {
        usleep(10000);
        out[1] = PMIx_Init(&me, NULL, 0);
        if (PMIX_SUCCESS == out[1]) {
            /* this one was counted */
            (void) PMIx_Finalize(NULL, 0);
        }
    }
    /* the first init's own finalize */
    out[2] = PMIx_Finalize(NULL, 0);
}

/* a singleton, its messages kept */
static void singleton(pmix_status_t *out)
{
    pmix_proc_t me;
    const char *errfile = getenv("IP_ERRFILE");

    if (NULL == errfile || NULL == freopen(errfile, "w", stderr)) {
        return;
    }
    out[0] = PMIx_Init(&me, NULL, 0);
    if (PMIX_SUCCESS == out[0] || PMIX_ERR_UNREACH == out[0]) {
        out[1] = PMIx_Finalize(NULL, 0);
    }
    fflush(stderr);
}

/* a server told to connect to another */
static void server_attach(pmix_status_t *out)
{
    pmix_server_module_t none;
    pmix_info_t info[4];
    const char *own = getenv("IP_OWN");
    const char *path = getenv("IP_FILE");
    bool flag = true;
    size_t ninfo = 0;

    if (NULL == own) {
        return;
    }
    memset(&none, 0, sizeof(none));
    PMIX_INFO_LOAD(&info[ninfo], PMIX_SERVER_NSPACE, own, PMIX_STRING);
    ++ninfo;
    if (NULL != getenv("IP_BAD")) {
        /* a file name that is not a string */
        PMIX_INFO_LOAD(&info[ninfo], PMIX_TOOL_ATTACHMENT_FILE, &flag, PMIX_BOOL);
        ++ninfo;
        PMIX_INFO_LOAD(&info[ninfo], PMIX_TOOL_CONNECT_OPTIONAL, &flag, PMIX_BOOL);
        ++ninfo;
    } else if (NULL != path) {
        PMIX_INFO_LOAD(&info[ninfo], PMIX_TOOL_ATTACHMENT_FILE, path, PMIX_STRING);
        ++ninfo;
    } else {
        PMIX_INFO_LOAD(&info[ninfo], PMIX_CONNECT_TO_SYSTEM, &flag, PMIX_BOOL);
        ++ninfo;
    }
    out[0] = PMIx_server_init(&none, info, ninfo);
    while (0 < ninfo) {
        --ninfo;
        PMIX_INFO_DESTRUCT(&info[ninfo]);
    }
    if (PMIX_SUCCESS != out[0]) {
        return;
    }
    /* a server of its own, named as the other server is */
    out[1] = ok_if(pmix_client_globals.myserver != pmix_globals.mypeer &&
                   NULL != pmix_client_globals.myserver->info &&
                   NULL != pmix_client_globals.myserver->info->pname.nspace &&
                   0 == strcmp(pmix_client_globals.myserver->info->pname.nspace, IP_SERVER));
    /* and its own name as it was */
    out[2] = ok_if(NULL != pmix_globals.mypeer->info &&
                   NULL != pmix_globals.mypeer->info->pname.nspace &&
                   0 == strcmp(pmix_globals.mypeer->info->pname.nspace, own) &&
                   0 == strcmp(pmix_globals.myid.nspace, own));
    out[3] = PMIx_server_finalize();
}

static int kid_main(int envfd, int resultfd)
{
    pmix_status_t out[NOUT] = {PMIX_ERROR, PMIX_ERROR, PMIX_ERROR, PMIX_ERROR};
    const char *which;

    if (!read_env(envfd)) {
        return 1;
    }
    alarm(60);
    which = getenv("IP_CASE");
    if (NULL == which) {
        return 1;
    }
    if (0 == strcmp(which, "tool-retry")) {
        tool_retry(out);
    } else if (0 == strcmp(which, "launcher-file")) {
        launcher_file(out);
    } else if (0 == strcmp(which, "client")) {
        client_init(out);
    } else if (0 == strcmp(which, "server-lost")) {
        server_lost(resultfd, out);
        return put(resultfd, &out[1], sizeof(out[1]) * (NOUT - 1)) ? 0 : 1;
    } else if (0 == strcmp(which, "singleton")) {
        singleton(out);
    } else if (0 == strcmp(which, "server-attach")) {
        server_attach(out);
    }
    return put(resultfd, out, sizeof(out)) ? 0 : 1;
}

static kid_t *started[NKIDS];
static int nstarted = 0;

static bool start_kid(kid_t *k)
{
    int envp[2], resp[2], n;

    if (0 != pipe(envp) || 0 != pipe(resp)) {
        return false;
    }
    fflush(stdout);
    k->pid = fork();
    if (0 > k->pid) {
        return false;
    }
    if (0 == k->pid) {
        for (n = 0; n < nstarted; n++) {
            close(started[n]->env);
            close(started[n]->result);
        }
        close(envp[1]);
        close(resp[0]);
        _exit(kid_main(envp[0], resp[1]));
    }
    close(envp[0]);
    close(resp[1]);
    k->env = envp[1];
    k->result = resp[0];
    started[nstarted++] = k;
    return true;
}

static void send_env(kid_t *k, char **env)
{
    size_t n;
    char nl = '\n';

    for (n = 0; NULL != env && NULL != env[n]; n++) {
        if (!put(k->env, env[n], strlen(env[n])) || !put(k->env, &nl, 1)) {
            break;
        }
    }
    (void) put(k->env, &nl, 1);
    close(k->env);
}

static void add_env(char ***env, const char *name, const char *value)
{
    char *entry;

    if (0 < asprintf(&entry, "%s=%s", name, value)) {
        PMIx_Argv_append_nosize(env, entry);
        free(entry);
    }
}

/* a kid's results, once it is done */
static void finish_kid(kid_t *k, pmix_status_t *out, size_t nout)
{
    int status;
    size_t n;

    for (n = 0; n < nout; n++) {
        out[n] = PMIX_ERROR;
    }
    if ((ssize_t) (nout * sizeof(*out)) != read(k->result, out, nout * sizeof(*out))) {
        out[0] = PMIX_ERR_TIMEOUT;
    }
    close(k->result);
    waitpid(k->pid, &status, 0);
}

/* run a kid with `env` and the case named, and collect its results */
static void run_kid(kid_t *k, const char *which, char **env, pmix_status_t *out)
{
    add_env(&env, "IP_CASE", which);
    send_env(k, env);
    PMIx_Argv_free(env);
    finish_kid(k, out, NOUT);
}

/* the environment the server gives a client of rank `rank`, without the
 * variables that say how to reach the server */
static char **client_env(pmix_rank_t rank, bool with_uri)
{
    pmix_proc_t proc;
    char **env = NULL, **kept = NULL;
    size_t n;

    PMIX_LOAD_PROCID(&proc, IP_NSPACE, rank);
    if (PMIX_SUCCESS != PMIx_server_setup_fork(&proc, &env)) {
        return NULL;
    }
    for (n = 0; NULL != env && NULL != env[n]; n++) {
        if (!with_uri && (0 == strncmp(env[n], "PMIX_SERVER_URI", 15) ||
                          0 == strncmp(env[n], PMIX_PTL_LOCAL_URI_ENV "=",
                                       strlen(PMIX_PTL_LOCAL_URI_ENV) + 1))) {
            continue;
        }
        PMIx_Argv_append_nosize(&kept, env[n]);
    }
    PMIx_Argv_free(env);
    return kept;
}

static void dir_env(char ***env)
{
    add_env(env, "PMIX_SERVER_TMPDIR", topdir);
    add_env(env, "PMIX_SYSTEM_TMPDIR", topdir);
}

static volatile bool dereg_done = false;

static void dereg_cb(pmix_status_t status, void *cbdata)
{
    PMIX_HIDE_UNUSED_PARAMS(status, cbdata);
    dereg_done = true;
}

int main(int argc, char **argv)
{
    kid_t kids[NKIDS];
    pmix_proc_t proc;
    pmix_nspace_t ns;
    pmix_status_t rc, out[NOUT];
    pmix_info_t info[4];
    uint32_t two = 2;
    bool flag = true;
    char detail[512], path[1024], line[1024], *uri;
    char **env;
    FILE *fp;
    bool said;
    int n;
    PMIX_HIDE_UNUSED_PARAMS(argc, argv);

    fprintf(stdout, "\n=== what the init paths do ===\n\n");
    signal(SIGPIPE, SIG_IGN);
    if (NULL == mkdtemp(topdir)) {
        fprintf(stderr, "could not make a directory\n");
        return 1;
    }
    for (n = 0; n < NKIDS; n++) {
        if (!start_kid(&kids[n])) {
            fprintf(stderr, "could not start the children\n");
            return 1;
        }
    }

    /* ---- no server ---- */

    env = NULL;
    dir_env(&env);
    run_kid(&kids[0], "tool-retry", env, out);
    snprintf(detail, sizeof(detail), "refused %s, then %s, finalize %s",
             PMIx_Error_string(out[0]), PMIx_Error_string(out[1]), PMIx_Error_string(out[2]));
    report("a tool init refused for its input can be tried again",
           PMIX_ERR_BAD_PARAM == out[0] && PMIX_SUCCESS == out[1] && PMIX_SUCCESS == out[2],
           detail);

    env = NULL;
    dir_env(&env);
    snprintf(path, sizeof(path), "%s/launcher.fresh", topdir);
    add_env(&env, "IP_FILE", path);
    run_kid(&kids[1], "launcher-file", env, out);
    snprintf(detail, sizeof(detail), "init %s, file %s, finalize %s", PMIx_Error_string(out[0]),
             (PMIX_SUCCESS == out[1]) ? "written" : "not written", PMIx_Error_string(out[2]));
    report("a launcher that does not connect writes its rendezvous file",
           PMIX_SUCCESS == out[0] && PMIX_SUCCESS == out[1] && PMIX_SUCCESS == out[2], detail);

    env = NULL;
    dir_env(&env);
    snprintf(path, sizeof(path), "%s/launcher.stale", topdir);
    add_env(&env, "IP_FILE", path);
    add_env(&env, "IP_STALE", "1");
    run_kid(&kids[2], "launcher-file", env, out);
    snprintf(detail, sizeof(detail), "init %s, file %s, finalize %s", PMIx_Error_string(out[0]),
             (PMIX_SUCCESS == out[1]) ? "replaced" : "not replaced", PMIx_Error_string(out[2]));
    report("...and replaces one a launcher no longer running left",
           PMIX_SUCCESS == out[0] && PMIX_SUCCESS == out[1] && PMIX_SUCCESS == out[2], detail);

    env = NULL;
    dir_env(&env);
    add_env(&env, "PMIX_NAMESPACE", IP_NSPACE ".lone");
    add_env(&env, "PMIX_RANK", "0");
    run_kid(&kids[3], "client", env, out);
    snprintf(detail, sizeof(detail), "init %s", PMIx_Error_string(out[0]));
    report("a client given a namespace but no server URI fails its init",
           PMIX_ERR_COMM_FAILURE == out[0], detail);

    env = NULL;
    dir_env(&env);
    add_env(&env, "IP_URI", "init.paths.nowhere.0;tcp4://127.0.0.1:1");
    run_kid(&kids[4], "client", env, out);
    snprintf(detail, sizeof(detail), "init %s", PMIx_Error_string(out[0]));
    report("a client's \"nspace.rank;uri\" directive naming a TCP address is refused",
           PMIX_ERR_NOT_SUPPORTED == out[0], detail);

    /* ---- a system server ---- */

    setenv("PMIX_SERVER_TMPDIR", topdir, 1);
    setenv("PMIX_SYSTEM_TMPDIR", topdir, 1);
    PMIX_INFO_LOAD(&info[0], PMIX_SERVER_TOOL_SUPPORT, &flag, PMIX_BOOL);
    PMIX_INFO_LOAD(&info[1], PMIX_SERVER_SYSTEM_SUPPORT, &flag, PMIX_BOOL);
    PMIX_INFO_LOAD(&info[2], PMIX_SERVER_NSPACE, IP_SERVER, PMIX_STRING);
    rc = PMIx_server_init(&mymodule, info, 3);
    for (n = 0; n < 3; n++) {
        PMIX_INFO_DESTRUCT(&info[n]);
    }
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "PMIx_server_init failed: %s\n", PMIx_Error_string(rc));
        return 1;
    }
    PMIX_LOAD_NSPACE(ns, IP_NSPACE);
    PMIX_INFO_LOAD(&info[0], PMIX_JOB_SIZE, &two, PMIX_UINT32);
    PMIX_INFO_LOAD(&info[1], PMIX_LOCAL_SIZE, &two, PMIX_UINT32);
    rc = PMIx_server_register_nspace(ns, 2, info, 2, NULL, NULL);
    PMIX_INFO_DESTRUCT(&info[0]);
    PMIX_INFO_DESTRUCT(&info[1]);
    for (n = 0; n < 2 && (PMIX_SUCCESS == rc || PMIX_OPERATION_SUCCEEDED == rc); n++) {
        PMIX_LOAD_PROCID(&proc, IP_NSPACE, n);
        rc = PMIx_server_register_client(&proc, geteuid(), getegid(), NULL, NULL, NULL);
    }
    if (PMIX_SUCCESS != rc && PMIX_OPERATION_SUCCEEDED != rc) {
        fprintf(stderr, "registration failed: %s\n", PMIx_Error_string(rc));
        return 1;
    }

    /* rank 0 connects, and is deregistered */
    env = client_env(0, true);
    add_env(&env, "IP_CASE", "server-lost");
    send_env(&kids[5], env);
    PMIx_Argv_free(env);
    if (sizeof(rc) != read(kids[5].result, &rc, sizeof(rc))) {
        rc = PMIX_ERR_TIMEOUT;
    }
    if (PMIX_SUCCESS == rc) {
        PMIX_LOAD_PROCID(&proc, IP_NSPACE, 0);
        PMIx_server_deregister_client(&proc, dereg_cb, NULL);
        for (n = 0; n < 1000 && !dereg_done; n++) {
            usleep(10000);
        }
    }
    finish_kid(&kids[5], out, NOUT - 1);
    snprintf(detail, sizeof(detail), "first init %s; after the loss %s",
             PMIx_Error_string(rc), PMIx_Error_string(out[0]));
    report("a later init after the server was lost returns PMIX_ERR_LOST_CONNECTION",
           PMIX_SUCCESS == rc && PMIX_ERR_LOST_CONNECTION == out[0], detail);

    /* rank 1, given the server by name and local socket */
    env = client_env(1, false);
    uri = pmix_ptl_base.local_listener.uri;
    add_env(&env, "IP_URI", (NULL == uri) ? "" : uri);
    run_kid(&kids[6], "client", env, out);
    snprintf(detail, sizeof(detail), "init %s with %s", PMIx_Error_string(out[0]),
             (NULL == uri) ? "(no local socket)" : uri);
    report("a client's \"nspace.rank;uri\" directive naming the local socket connects",
           PMIX_SUCCESS == out[0] && PMIX_SUCCESS == out[1], detail);

    /* a singleton */
    env = NULL;
    add_env(&env, "PMIX_SYSTEM_TMPDIR", topdir);
    snprintf(path, sizeof(path), "%s/singleton.err", topdir);
    add_env(&env, "IP_ERRFILE", path);
    run_kid(&kids[7], "singleton", env, out);
    said = false;
    fp = fopen(path, "r");
    while (NULL != fp && NULL != fgets(line, sizeof(line), fp)) {
        if (NULL != strstr(line, "did not accept it")) {
            said = true;
        }
    }
    if (NULL != fp) {
        fclose(fp);
    }
    snprintf(detail, sizeof(detail), "init %s, %s", PMIx_Error_string(out[0]),
             said ? "said so" : "said nothing");
    report("a singleton the system server does not accept says so, and runs alone",
           PMIX_ERR_UNREACH == out[0] && said, detail);

    /* servers connecting to this one */
    snprintf(path, sizeof(path), "%s/pmix.sys.%s", topdir, pmix_globals.hostname);
    env = NULL;
    snprintf(line, sizeof(line), "%s/attach", topdir);
    (void) mkdir(line, 0700);
    add_env(&env, "PMIX_SERVER_TMPDIR", line);
    add_env(&env, "PMIX_SYSTEM_TMPDIR", line);
    add_env(&env, "IP_OWN", "init.paths.attach");
    add_env(&env, "IP_FILE", path);
    run_kid(&kids[8], "server-attach", env, out);
    snprintf(detail, sizeof(detail), "init %s, its server %s, its name %s, finalize %s",
             PMIx_Error_string(out[0]), (PMIX_SUCCESS == out[1]) ? "ours" : "not ours",
             (PMIX_SUCCESS == out[2]) ? "kept" : "changed", PMIx_Error_string(out[3]));
    report("a server given another's file connects to it, keeping its own name",
           PMIX_SUCCESS == out[0] && PMIX_SUCCESS == out[1] && PMIX_SUCCESS == out[2] &&
               PMIX_SUCCESS == out[3],
           detail);

    env = NULL;
    snprintf(line, sizeof(line), "%s/bysystem", topdir);
    (void) mkdir(line, 0700);
    add_env(&env, "PMIX_SERVER_TMPDIR", line);
    add_env(&env, "PMIX_SYSTEM_TMPDIR", topdir);
    add_env(&env, "IP_OWN", "init.paths.bysystem");
    run_kid(&kids[9], "server-attach", env, out);
    snprintf(detail, sizeof(detail), "init %s, its server %s, its name %s, finalize %s",
             PMIx_Error_string(out[0]), (PMIX_SUCCESS == out[1]) ? "ours" : "not ours",
             (PMIX_SUCCESS == out[2]) ? "kept" : "changed", PMIx_Error_string(out[3]));
    report("a server told to connect to the system server does, keeping its own name",
           PMIX_SUCCESS == out[0] && PMIX_SUCCESS == out[1] && PMIX_SUCCESS == out[2] &&
               PMIX_SUCCESS == out[3],
           detail);

    env = NULL;
    snprintf(line, sizeof(line), "%s/bad", topdir);
    (void) mkdir(line, 0700);
    add_env(&env, "PMIX_SERVER_TMPDIR", line);
    add_env(&env, "PMIX_SYSTEM_TMPDIR", line);
    add_env(&env, "IP_OWN", "init.paths.bad");
    add_env(&env, "IP_BAD", "1");
    run_kid(&kids[10], "server-attach", env, out);
    snprintf(detail, sizeof(detail), "init %s", PMIx_Error_string(out[0]));
    report("a server's malformed connect directive fails its init, though optional",
           PMIX_ERR_BAD_PARAM == out[0], detail);

    PMIx_server_finalize();
    snprintf(line, sizeof(line), "rm -rf %s", topdir);
    (void) !system(line);
    fprintf(stdout, "\n%d passed, %d failed\n", npass, nfail);
    return (0 == nfail) ? 0 : 1;
}
