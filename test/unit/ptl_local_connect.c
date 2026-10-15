/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Clients and tools of this release reach a server on their host over its
 * local socket, and check the server is who it should be.
 *
 * A client the server starts is given PMIX_SERVER_LOCAL_URI: the local
 * socket's URI and the uid and gid the kernel must report for the server.
 * It connects there - not over TCP - and fails its init, rather than
 * running as a singleton, if it cannot reach the socket, if the variable
 * cannot be read, or if the server is not the uid and gid it names. A tool
 * that finds the server through its rendezvous file connects to the file's
 * "local:" line. ptl_base_server_access, which names further servers a
 * client or tool may talk to, is read as "users=...,groups=..." and
 * anything else refused.
 *
 * The client and tool run in children forked before any PMIx call, which
 * are handed their environment through a pipe; the server's look at the
 * host's up-calls happens while they are connected.
 */

#include "src/include/pmix_config.h"

#include "include/pmix.h"
#include "include/pmix_server.h"
#include "include/pmix_tool.h"
#include "src/client/pmix_client_ops.h"
#include "src/include/pmix_globals.h"
#include "src/mca/ptl/base/base.h"
#include "src/mca/ptl/usock/ptl_usock.h"
#include "src/util/pmix_net.h"

#include <fcntl.h>
#if defined(__linux__)
#    include <sched.h>
#endif
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define CL_NSPACE "local.connect"

/* a child's exit status */
#define KID_OK          0
#define KID_FAILED      1
#define KID_NOT_LOCAL   3   // connected, but not over the local transport
#define KID_INIT_FAILED 4   // init failed - the status is written to its pipe
#define KID_CLOEXEC     5   // its connection is not close-on-exec
#define KID_NO_USERNS   6   // it could not enter a user namespace of its own

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

static volatile int nconnected = 0;

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
    return PMIX_OPERATION_SUCCEEDED;
}

static void tool_connected(pmix_info_t *info, size_t ninfo,
                           pmix_tool_connection_cbfunc_t cbfunc, void *cbdata)
{
    pmix_proc_t proc;
    PMIX_HIDE_UNUSED_PARAMS(info, ninfo);

    ++nconnected;
    PMIX_LOAD_PROCID(&proc, "local.tool", 0);
    cbfunc(PMIX_SUCCESS, &proc, cbdata);
}

static pmix_server_module_t mymodule = {
    .client_connected2 = client_connected2,
    .client_finalized = client_finalized,
    .tool_connected = tool_connected
};

/* ---- the child: a client or a tool ---- */

typedef struct {
    bool tool;
    pid_t pid;
    int env;    // our end of the pipe its environment goes down
    int result; // our end of the pipe its init's status comes back on
    int go;     // our end of the pipe that lets it finalize
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

/* Enter a user namespace of our own, as a rootless container runs a
 * process: who we are outside is another id inside - never the same one,
 * so the kernel never reports the server by the ids it was named by.
 * Linux only */
static bool enter_userns(void)
{
#if defined(__linux__) && defined(CLONE_NEWUSER)
    char map[64];
    uid_t uid = geteuid();
    gid_t gid = getegid();
    int fd;
    bool ok = true;

    if (0 != unshare(CLONE_NEWUSER)) {
        return false;
    }
    if (0 <= (fd = open("/proc/self/setgroups", O_WRONLY))) {
        ok = (4 == write(fd, "deny", 4));
        close(fd);
    }
    snprintf(map, sizeof(map), "%lu %lu 1", (unsigned long) (4242 == uid ? 4243 : 4242),
             (unsigned long) uid);
    if (!ok || 0 > (fd = open("/proc/self/uid_map", O_WRONLY)) ||
        (ssize_t) strlen(map) != write(fd, map, strlen(map))) {
        return false;
    }
    close(fd);
    snprintf(map, sizeof(map), "%lu %lu 1", (unsigned long) (4242 == gid ? 4243 : 4242),
             (unsigned long) gid);
    if (0 > (fd = open("/proc/self/gid_map", O_WRONLY)) ||
        (ssize_t) strlen(map) != write(fd, map, strlen(map))) {
        return false;
    }
    close(fd);
    return true;
#else
    return false;
#endif
}

/* write all of buf to fd */
static bool put(int fd, const void *buf, size_t len)
{
    return (ssize_t) len == write(fd, buf, len);
}

static int run_kid(bool tool, int envfd, int resultfd, int gofd)
{
    pmix_proc_t me;
    pmix_info_t info;
    pmix_status_t rc;
    pmix_peer_t *srv;
    char c = 'r';
    int ret = KID_OK;
    pid_t spid;

    if (!read_env(envfd)) {
        return KID_FAILED;
    }
    if (NULL != getenv("CL_USERNS") && !enter_userns()) {
        rc = PMIX_ERR_NOT_SUPPORTED;
        return put(resultfd, &rc, sizeof(rc)) ? KID_NO_USERNS : KID_FAILED;
    }
    if (tool && NULL != getenv("CL_TOOL_URI")) {
        /* the server's URI, or file:<path>, given */
        PMIX_INFO_LOAD(&info, PMIX_SERVER_URI, getenv("CL_TOOL_URI"), PMIX_STRING);
        rc = PMIx_tool_init(&me, &info, 1);
        PMIX_INFO_DESTRUCT(&info);
    } else if (tool) {
        /* find the server by its pid's rendezvous file */
        spid = (pid_t) strtol(getenv("CL_SERVER_PID"), NULL, 10);
        PMIX_INFO_LOAD(&info, PMIX_SERVER_PIDINFO, &spid, PMIX_PID);
        rc = PMIx_tool_init(&me, &info, 1);
        PMIX_INFO_DESTRUCT(&info);
    } else {
        rc = PMIx_Init(&me, NULL, 0);
    }
    if (PMIX_SUCCESS != rc) {
        return put(resultfd, &rc, sizeof(rc)) ? KID_INIT_FAILED : KID_FAILED;
    }
    srv = pmix_client_globals.myserver;
    if (NULL == srv || &pmix_ptl_usock_module != srv->transport) {
        ret = KID_NOT_LOCAL;
    }
    /* the connection is close-on-exec, whatever its transport and kind */
    if (NULL != srv && 0 <= srv->sd && 0 == (fcntl(srv->sd, F_GETFD) & FD_CLOEXEC)) {
        ret = KID_CLOEXEC;
    }
    if (!put(resultfd, &rc, sizeof(rc)) || 1 != read(gofd, &c, 1)) {
        ret = KID_FAILED;
    }
    if (tool) {
        PMIx_tool_finalize();
    } else {
        PMIx_Finalize(NULL, 0);
    }
    return ret;
}

static kid_t *started[16];
static int nstarted = 0;

static bool start_kid(kid_t *k, bool tool)
{
    int envp[2], resp[2], gop[2], n;

    k->tool = tool;
    if (0 != pipe(envp) || 0 != pipe(resp) || 0 != pipe(gop)) {
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
            close(started[n]->go);
        }
        close(envp[1]);
        close(resp[0]);
        close(gop[1]);
        alarm(60);
        _exit(run_kid(tool, envp[0], resp[1], gop[0]));
    }
    close(envp[0]);
    close(resp[1]);
    close(gop[0]);
    k->env = envp[1];
    k->result = resp[0];
    k->go = gop[1];
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
    /* the blank line ends the environment: a kid that does not get it
     * fails */
    if (NULL == env || NULL == env[n]) {
        (void) put(k->env, &nl, 1);
    }
    close(k->env);
}

/* the kid's init status, and then its exit */
static pmix_status_t finish_kid(kid_t *k, int *status)
{
    pmix_status_t rc = PMIX_ERROR;
    char c = 'g';

    if (sizeof(rc) != read(k->result, &rc, sizeof(rc))) {
        rc = PMIX_ERROR;
    }
    /* a kid that does not get the go fails */
    (void) put(k->go, &c, 1);
    close(k->go);
    close(k->result);
    waitpid(k->pid, status, 0);
    return rc;
}

/* replace (or add) one variable in an environment array */
static void set_var(char ***env, const char *name, const char *value)
{
    PMIx_Setenv(name, value, true, env);
}

/* A client given the environment setup_fork builds - with `edit` changing
 * the local URI variable first: NULL leaves it, else it is the value */
static void client_case(kid_t *kp, const char *name, const char *edit, pmix_status_t want,
                        bool want_local)
{
    kid_t k = *kp;
    pmix_proc_t proc;
    char **env = NULL;
    int status = 0, before = nconnected;
    pmix_status_t rc;
    char detail[256];

    PMIX_LOAD_PROCID(&proc, CL_NSPACE, 0);
    if (PMIX_SUCCESS != PMIx_server_setup_fork(&proc, &env)) {
        report(name, 0, "setup_fork failed");
        return;
    }
    if (NULL != edit) {
        set_var(&env, PMIX_PTL_LOCAL_URI_ENV, edit);
    }
    send_env(&k, env);
    PMIx_Argv_free(env);
    rc = finish_kid(&k, &status);
    snprintf(detail, sizeof(detail), "init %s, exit %d, host asked %d times",
             PMIx_Error_string(rc), WIFEXITED(status) ? WEXITSTATUS(status) : -1,
             nconnected - before);
    if (PMIX_SUCCESS == want) {
        report(name, PMIX_SUCCESS == rc && WIFEXITED(status) &&
                         (want_local ? KID_OK : KID_NOT_LOCAL) == WEXITSTATUS(status),
               detail);
    } else {
        report(name, want == rc && before == nconnected, detail);
    }
}

static void tool_case(kid_t *kp, const char *name)
{
    kid_t k = *kp;
    char *env[2] = {NULL, NULL};
    int status = 0;
    pmix_status_t rc;
    char detail[256];

    if (0 > asprintf(&env[0], "CL_SERVER_PID=%lu", (unsigned long) getpid())) {
        return;
    }
    send_env(&k, env);
    free(env[0]);
    rc = finish_kid(&k, &status);
    snprintf(detail, sizeof(detail), "init %s, exit %d", PMIx_Error_string(rc),
             WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    report(name, PMIX_SUCCESS == rc && WIFEXITED(status) && KID_OK == WEXITSTATUS(status),
           detail);
}

/* A tool given the server's URI, or a file holding it: over the local
 * socket or TCP, as `want_local` says */
static void tool_uri_case(kid_t *kp, const char *name, const char *uri, bool want_local)
{
    kid_t k = *kp;
    char *env[2] = {NULL, NULL};
    int status = 0;
    pmix_status_t rc;
    char detail[256];

    if (0 > asprintf(&env[0], "CL_TOOL_URI=%s", uri)) {
        return;
    }
    send_env(&k, env);
    free(env[0]);
    rc = finish_kid(&k, &status);
    snprintf(detail, sizeof(detail), "init %s, exit %d", PMIx_Error_string(rc),
             WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    report(name, PMIX_SUCCESS == rc && WIFEXITED(status) &&
                     (want_local ? KID_OK : KID_NOT_LOCAL) == WEXITSTATUS(status),
           detail);
}

/* A client the server starts that runs in a user namespace of its own:
 * the kernel reports the server to it by the ids inside, and it maps them
 * back to the ones the server named */
static void userns_case(kid_t *kp, const char *name)
{
    kid_t k = *kp;
    pmix_proc_t proc;
    char **env = NULL;
    int status = 0;
    pmix_status_t rc;
    char detail[256];

    PMIX_LOAD_PROCID(&proc, CL_NSPACE, 0);
    if (PMIX_SUCCESS != PMIx_server_setup_fork(&proc, &env)) {
        report(name, 0, "setup_fork failed");
        return;
    }
    set_var(&env, "CL_USERNS", "1");
    send_env(&k, env);
    PMIx_Argv_free(env);
    rc = finish_kid(&k, &status);
    if (WIFEXITED(status) && KID_NO_USERNS == WEXITSTATUS(status)) {
        fprintf(stdout, "  SKIP: %s (no user namespaces here)\n", name);
        return;
    }
    snprintf(detail, sizeof(detail), "init %s, exit %d", PMIx_Error_string(rc),
             WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    report(name, PMIX_SUCCESS == rc && WIFEXITED(status) && KID_OK == WEXITSTATUS(status),
           detail);
}

/* A process started with nothing in its environment: a singleton, which
 * names itself "singleton.<host>.<pid>". The server was started for it,
 * as a singleton starts one: told its name with PMIX_SINGLETON, and then
 * registering it - but not preparing it, as nothing was started */
static void singleton_case(kid_t *kp, const char *name)
{
    kid_t k = *kp;
    int status = 0;
    pmix_status_t rc;
    pmix_proc_t proc;
    pmix_nspace_t ns;
    char detail[256];

    snprintf(detail, sizeof(detail), "singleton.%s.%lu", pmix_globals.hostname,
             (unsigned long) k.pid);
    PMIX_LOAD_NSPACE(ns, detail);
    PMIX_LOAD_PROCID(&proc, detail, 0);
    rc = PMIx_server_register_nspace(ns, 1, NULL, 0, NULL, NULL);
    if ((PMIX_SUCCESS != rc && PMIX_OPERATION_SUCCEEDED != rc) ||
        (PMIX_SUCCESS != (rc = PMIx_server_register_client(&proc, geteuid(), getegid(), NULL,
                                                           NULL, NULL)) &&
         PMIX_OPERATION_SUCCEEDED != rc)) {
        report(name, 0, "could not register the singleton");
        return;
    }
    send_env(&k, NULL);
    rc = finish_kid(&k, &status);
    snprintf(detail, sizeof(detail), "init %s, exit %d", PMIx_Error_string(rc),
             WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    report(name, PMIX_SUCCESS == rc && WIFEXITED(status) && KID_OK == WEXITSTATUS(status),
           detail);
}

/* a copy of the report file whose local line names another host */
static char *other_host_copy(const char *path)
{
    FILE *in, *out;
    char line[4096], *copy = NULL;
    size_t tlen = strlen(PMIX_PTL_LOCAL_URI_TAG);

    if (0 > asprintf(&copy, "%s.otherhost", path) || NULL == (in = fopen(path, "r"))) {
        return NULL;
    }
    if (NULL == (out = fopen(copy, "w"))) {
        fclose(in);
        return NULL;
    }
    while (NULL != fgets(line, sizeof(line), in)) {
        if (0 == strncmp(line, PMIX_PTL_LOCAL_URI_TAG, tlen) && NULL != strchr(line, ';')) {
            fprintf(out, "%snot-%s%s", PMIX_PTL_LOCAL_URI_TAG, pmix_globals.hostname,
                    strchr(line, ';'));
        } else {
            fputs(line, out);
        }
    }
    fclose(in);
    fclose(out);
    return copy;
}

/* the server's TCP URI, as setup_fork gives it */
static char *tcp_uri(void)
{
    pmix_proc_t proc;
    char **env = NULL, *uri = NULL, *eq;
    size_t n;

    PMIX_LOAD_PROCID(&proc, CL_NSPACE, 0);
    if (PMIX_SUCCESS != PMIx_server_setup_fork(&proc, &env)) {
        return NULL;
    }
    for (n = 0; NULL != env && NULL != env[n] && NULL == uri; n++) {
        if (0 == strncmp(env[n], "PMIX_SERVER_URI", 15) && '0' <= env[n][15] &&
            '9' >= env[n][15] && NULL != (eq = strchr(env[n], '='))) {
            uri = strdup(eq + 1);
        }
    }
    PMIx_Argv_free(env);
    return uri;
}

static void access_cases(void)
{
    struct {
        const char *spec;
        bool ok;
    } cases[] = {
        {"users=0", true},
        {"USERS=0:1,groups=0", true},
        {"groups=0", true},
        {"users=", false},
        {"users=0,users=1", false},
        {"people=0", false},
        {"users=pmix-no-such-user-xyz", false},
        {"groups=pmix-no-such-group-xyz", false},
    };
    char name[160];
    size_t n;
    pmix_status_t rc;

    for (n = 0; n < sizeof(cases) / sizeof(cases[0]); n++) {
        rc = pmix_ptl_base_parse_server_access(cases[n].spec);
        snprintf(name, sizeof(name), "ptl_base_server_access \"%s\" is %s", cases[n].spec,
                 cases[n].ok ? "taken" : "refused");
        report(name, cases[n].ok == (PMIX_SUCCESS == rc), PMIx_Error_string(rc));
    }
    /* leave none in force */
    (void) pmix_ptl_base_parse_server_access(NULL);
}

int main(int argc, char **argv)
{
    pmix_info_t info[6];
    pmix_proc_t proc;
    char *value = NULL, *uri = NULL, *report = NULL, *copy, host[256], sgltn[512];
    bool flag = true;
    pmix_status_t rc;
    kid_t kids[12];
    pmix_nspace_t ns;
    int n;
    PMIX_HIDE_UNUSED_PARAMS(argc, argv);

    fprintf(stdout, "\n=== clients and tools on the local socket ===\n\n");
#if !PMIX_ENABLE_LEGACY_PEERS
    /* several cases use the server's TCP listener, which it opens for
     * clients and tools of earlier releases */
    fprintf(stdout, "this PMIx takes no clients or tools of earlier releases, so the "
                    "server has no TCP listener for these cases\n");
    return 77;
#endif
    signal(SIGPIPE, SIG_IGN);

    /* every child is forked before any PMIx call: a child of an
     * initialized server has a copy of its state but none of its threads.
     * Each later one lets go of the pipes of those before it */
    /* 0-3, 5, 7 and 11 are clients; the rest tools */
    for (n = 0; n < 12; n++) {
        if (!start_kid(&kids[n], 4 == n || 6 == n || (8 <= n && 10 >= n))) {
            fprintf(stderr, "could not start the children\n");
            return 1;
        }
    }

    if (0 > asprintf(&report, "%s/pmix_local_connect.%lu.uri",
                     (NULL == getenv("TMPDIR")) ? "/tmp" : getenv("TMPDIR"),
                     (unsigned long) getpid())) {
        return 1;
    }
    PMIX_INFO_LOAD(&info[0], PMIX_SERVER_TOOL_SUPPORT, &flag, PMIX_BOOL);
    PMIX_INFO_LOAD(&info[1], PMIX_SERVER_NSPACE, "local.server", PMIX_STRING);
    PMIX_INFO_LOAD(&info[2], PMIX_SERVER_SYSTEM_SUPPORT, &flag, PMIX_BOOL);
    PMIX_INFO_LOAD(&info[3], PMIX_TCP_REPORT_URI, report, PMIX_STRING);
    /* a TCP listener too, for the cases that use it - one the server opens
     * for clients and tools of earlier releases */
    PMIX_INFO_LOAD(&info[4], PMIX_SERVER_LEGACY_PEERS, &flag, PMIX_BOOL);
    /* the singleton's name, from the hostname its library will use: the
     * host's, short of its domain */
    if (NULL != getenv("PMIX_HOSTNAME")) {
        snprintf(host, sizeof(host), "%s", getenv("PMIX_HOSTNAME"));
    } else if (0 != gethostname(host, sizeof(host) - 1)) {
        return 1;
    }
    host[sizeof(host) - 1] = '\0';
    if (!pmix_net_isaddr(host) && NULL != strchr(host, '.')) {
        *strchr(host, '.') = '\0';
    }
    snprintf(sgltn, sizeof(sgltn), "singleton.%s.%lu.0", host, (unsigned long) kids[7].pid);
    PMIX_INFO_LOAD(&info[5], PMIX_SINGLETON, sgltn, PMIX_STRING);
    rc = PMIx_server_init(&mymodule, info, 6);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "PMIx_server_init failed: %s\n", PMIx_Error_string(rc));
        return 1;
    }
    PMIX_LOAD_NSPACE(ns, CL_NSPACE);
    rc = PMIx_server_register_nspace(ns, 1, NULL, 0, NULL, NULL);
    if (PMIX_SUCCESS != rc && PMIX_OPERATION_SUCCEEDED != rc) {
        fprintf(stderr, "register_nspace failed: %s\n", PMIx_Error_string(rc));
        return 1;
    }
    PMIX_LOAD_PROCID(&proc, CL_NSPACE, 0);
    rc = PMIx_server_register_client(&proc, geteuid(), getegid(), NULL, NULL, NULL);
    if (PMIX_SUCCESS != rc && PMIX_OPERATION_SUCCEEDED != rc) {
        fprintf(stderr, "register_client failed: %s\n", PMIx_Error_string(rc));
        return 1;
    }

    client_case(&kids[0], "a client the server starts connects over the local socket", NULL,
                PMIX_SUCCESS, true);

    /* the same URI, naming another uid for the server */
    value = pmix_ptl_base_local_uri_value();
    uri = strdup(value);
    *strrchr(uri, ';') = '\0';
    free(value);
    if (0 > asprintf(&value, "%s;%lu:%lu", uri, (unsigned long) geteuid() + 1,
                     (unsigned long) getegid())) {
        return 1;
    }
    client_case(&kids[1], "a client whose server is not the uid it was told fails init", value,
                PMIX_ERR_INVALID_CRED, false);
    free(value);

    /* the server's name, as its own URIs give it */
    *strchr(uri, ';') = '\0';
    if (0 > asprintf(&value, "%s;usock:///tmp/pmix-no-such-dir/pmix-1;%lu:%lu", uri,
                     (unsigned long) geteuid(), (unsigned long) getegid())) {
        return 1;
    }
    client_case(&kids[2], "a client that cannot reach the socket fails init, and is not a singleton",
                value, PMIX_ERR_COMM_FAILURE, false);
    free(value);

    client_case(&kids[3], "a client whose local URI cannot be read fails init", "no-identity-here",
                PMIX_ERR_SILENT, false);
    free(uri);

    tool_case(&kids[4], "a tool finding the server by its file connects over the local socket");

    client_case(&kids[5], "a client given another server's local URI ignores it and uses TCP",
                "other.server.7;usock:///tmp/pmix-no-such-dir/pmix-1;0:0", PMIX_SUCCESS, false);

    value = tcp_uri();
    if (NULL == value) {
        return 1;
    }
    tool_uri_case(&kids[6], "a tool given the TCP URI connects over TCP, close-on-exec", value,
                  false);
    free(value);

    singleton_case(&kids[7], "a singleton reaches the server started for it over its local "
                             "socket, with no token");

    if (0 > asprintf(&value, "file:%s", report)) {
        return 1;
    }
    tool_uri_case(&kids[8], "a tool given the report file connects over the local socket", value,
                  true);
    free(value);
    copy = other_host_copy(report);
    if (NULL == copy || 0 > asprintf(&value, "file:%s", copy)) {
        return 1;
    }
    tool_uri_case(&kids[9], "a tool reading a file whose local socket is another host's uses TCP",
                  value, false);
    free(value);
    unlink(copy);
    free(copy);
    tool_case(&kids[10], "a second tool by the server's pid, after them all");

    userns_case(&kids[11], "a client in a user namespace of its own knows its server");

    access_cases();

    PMIx_server_finalize();
    free(report);
    fprintf(stdout, "\n%d passed, %d failed\n", npass, nfail);
    return (0 == nfail) ? 0 : 1;
}
