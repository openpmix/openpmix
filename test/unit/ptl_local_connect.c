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
 * A tool given a file that is not a regular file - a FIFO, or a symbolic
 * link to the server's report file - at PMIX_SERVER_URI "file:<path>" or
 * PMIX_TOOL_ATTACHMENT_FILE fails its init at once, rather than waiting
 * on the FIFO; the report file itself, named either way, connects. A
 * launcher whose PMIX_LAUNCHER_RENDEZVOUS_FILE name has a FIFO at it
 * finishes its init promptly.
 *
 * A tool that disconnects from its server while a request to it is
 * unanswered sees the request complete with an error.
 *
 * A client that dies without finalizing, followed by another the host
 * starts for the same rank, leaves one peer for the rank on the server's
 * client list, not two.
 *
 * Every connect-ack buffer a client releases - the packed info that holds
 * its connection token, and the message - is zero when it is released
 * (pmix_ptl_base_wiped_hook).
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
#include "src/mca/ptl/base/ptl_base_handshake.h"
#include "src/mca/ptl/usock/ptl_usock.h"
#include "src/server/pmix_server_ops.h"
#include "src/util/pmix_environ.h"
#include "src/util/pmix_net.h"

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#if defined(__linux__)
#    include <sched.h>
#endif
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CL_NSPACE "local.connect"

/* a child's exit status */
#define KID_OK          0
#define KID_FAILED      1
#define KID_NOT_LOCAL   3   // connected, but not over the local transport
#define KID_INIT_FAILED 4   // init failed - the status is written to its pipe
#define KID_CLOEXEC     5   // its connection is not close-on-exec
#define KID_NO_USERNS   6   // it could not enter a user namespace of its own
#define KID_NOT_WIPED   7   // a connect-ack buffer it released was not zero

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

/* a query the host holds, and never answers */
static pmix_status_t query_held(pmix_proc_t *proct, pmix_query_t *queries, size_t nqueries,
                                pmix_info_cbfunc_t cbfunc, void *cbdata)
{
    PMIX_HIDE_UNUSED_PARAMS(proct, queries, nqueries, cbfunc, cbdata);
    return PMIX_SUCCESS;
}

static pmix_server_module_t mymodule = {
    .client_connected2 = client_connected2,
    .client_finalized = client_finalized,
    .tool_connected = tool_connected,
    .query = query_held
};

/* what became of a query a disconnect cut short */
static volatile bool held_done = false;
static volatile pmix_status_t held_status = PMIX_SUCCESS;

static void held_cbfunc(pmix_status_t status, pmix_info_t *info, size_t ninfo, void *cbdata,
                        pmix_release_cbfunc_t release_fn, void *release_cbdata)
{
    PMIX_HIDE_UNUSED_PARAMS(info, ninfo, cbdata);
    held_status = status;
    held_done = true;
    if (NULL != release_fn) {
        release_fn(release_cbdata);
    }
}

/* A tool asks its server something the host never answers, then
 * disconnects from the server: the request completes with an error, as
 * it would had the connection been lost */
static int disconnect_pending(void)
{
    pmix_query_t q;
    pmix_proc_t server;
    pmix_status_t rc;
    int n;

    PMIX_LOAD_PROCID(&server, pmix_client_globals.myserver->info->pname.nspace,
                     pmix_client_globals.myserver->info->pname.rank);
    PMIX_QUERY_CONSTRUCT(&q);
    PMIx_Argv_append_nosize(&q.keys, "local.connect.held");
    rc = PMIx_Query_info_nb(&q, 1, held_cbfunc, NULL);
    if (PMIX_SUCCESS != rc) {
        fprintf(stdout, "    the query was refused: %s\n", PMIx_Error_string(rc));
        fflush(stdout);
        PMIX_QUERY_DESTRUCT(&q);
        return KID_FAILED;
    }
    usleep(200000);
    if (held_done || PMIX_SUCCESS != PMIx_tool_disconnect(&server)) {
        fprintf(stdout, "    the query %s before the disconnect\n",
                held_done ? "was answered" : "could not be cut short");
        fflush(stdout);
        return KID_FAILED;
    }
    for (n = 0; n < 100 && !held_done; n++) {
        usleep(50000);
    }
    fprintf(stdout, "    after the disconnect the query %s%s\n",
            held_done ? "completed with " : "still waits",
            held_done ? PMIx_Error_string(held_status) : "");
    fflush(stdout);
    PMIX_QUERY_DESTRUCT(&q);
    return (held_done && PMIX_SUCCESS != held_status) ? KID_OK : KID_FAILED;
}

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

/* what the kid's pmix_ptl_base_wiped_hook saw */
static int nwiped = 0;
static bool wiped_dirty = false;

static void wiped(const void *p, size_t len)
{
    const unsigned char *c = (const unsigned char *) p;
    size_t n;

    ++nwiped;
    for (n = 0; n < len; n++) {
        if (0 != c[n]) {
            wiped_dirty = true;
            return;
        }
    }
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
    if (NULL != getenv("CL_ALARM")) {
        alarm((unsigned) strtoul(getenv("CL_ALARM"), NULL, 10));
    }
    if (NULL != getenv("CL_WIPE_CHECK")) {
        pmix_ptl_base_wiped_hook = wiped;
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
    } else if (tool && NULL != getenv("CL_TOOL_ATTACH")) {
        /* the server's file, as an attachment file */
        PMIX_INFO_LOAD(&info, PMIX_TOOL_ATTACHMENT_FILE, getenv("CL_TOOL_ATTACH"), PMIX_STRING);
        rc = PMIx_tool_init(&me, &info, 1);
        PMIX_INFO_DESTRUCT(&info);
    } else if (tool && NULL != getenv("CL_LAUNCHER_RNDZ")) {
        /* a launcher, attaching to the server by its file and then
         * writing its own rendezvous file */
        pmix_info_t linfo[3];
        PMIX_INFO_LOAD(&linfo[0], PMIX_LAUNCHER, NULL, PMIX_BOOL);
        PMIX_INFO_LOAD(&linfo[1], PMIX_SERVER_URI, getenv("CL_LAUNCHER_SERVER"), PMIX_STRING);
        PMIX_INFO_LOAD(&linfo[2], PMIX_LAUNCHER_RENDEZVOUS_FILE, getenv("CL_LAUNCHER_RNDZ"),
                       PMIX_STRING);
        rc = PMIx_tool_init(&me, linfo, 3);
        PMIX_INFO_DESTRUCT(&linfo[0]);
        PMIX_INFO_DESTRUCT(&linfo[1]);
        PMIX_INFO_DESTRUCT(&linfo[2]);
        if (PMIX_SUCCESS == rc) {
            if (!put(resultfd, &rc, sizeof(rc)) || 1 != read(gofd, &c, 1)) {
                ret = KID_FAILED;
            }
            PMIx_tool_finalize();
            return ret;
        }
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
    if (NULL != getenv("CL_DIE")) {
        /* a process that dies without finalizing */
        ret = put(resultfd, &rc, sizeof(rc)) ? KID_OK : KID_FAILED;
        _exit(ret);
    }
    srv = pmix_client_globals.myserver;
    if (NULL == srv || &pmix_ptl_usock_module != srv->transport) {
        ret = KID_NOT_LOCAL;
    } else if (tool && NULL != getenv("CL_DISC_PENDING")) {
        ret = disconnect_pending();
        /* the server's peer is released by the disconnect - nothing below
         * may look at it */
        srv = NULL;
    }
    /* the connection is close-on-exec, whatever its transport and kind */
    if (NULL != srv && 0 <= srv->sd && 0 == (fcntl(srv->sd, F_GETFD) & FD_CLOEXEC)) {
        ret = KID_CLOEXEC;
    }
    /* the packed info and the message, at least, and every one zero */
    if (NULL != getenv("CL_WIPE_CHECK") && (2 > nwiped || wiped_dirty)) {
        fprintf(stdout, "    %d connect-ack buffers released, %s\n", nwiped,
                wiped_dirty ? "one of them not zero" : "all zero");
        ret = KID_NOT_WIPED;
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

static kid_t *started[24];
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
 * socket - or, at a TCP address, refused as `want` says: by the tool
 * itself, before connecting, when it was given the TCP URI alone, and by
 * the server when a file gave it, since a server of this release takes a
 * tool of this release only on its local socket */
static void tool_uri_case(kid_t *kp, const char *name, const char *uri, bool want_local,
                          pmix_status_t want)
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
    if (PMIX_SUCCESS != want) {
        report(name, want == rc, detail);
        return;
    }
    report(name, PMIX_SUCCESS == rc && WIFEXITED(status) &&
                     (want_local ? KID_OK : KID_NOT_LOCAL) == WEXITSTATUS(status),
           detail);
}

/* A tool told of a file that is not a regular one - by `var` (CL_TOOL_URI
 * for "file:<path>", or CL_TOOL_ATTACH for PMIX_TOOL_ATTACHMENT_FILE) -
 * fails its init within `limit` seconds, rather than waiting on it. One
 * that waits is stopped by its alarm, a little after the limit */
static void tool_file_refused_case(kid_t *kp, const char *name, const char *var,
                                   const char *value, unsigned limit)
{
    kid_t k = *kp;
    char *env[3] = {NULL, NULL, NULL};
    int status = 0;
    pmix_status_t rc;
    char detail[256];
    time_t start;
    double took;

    if (0 > asprintf(&env[0], "%s=%s", var, value) ||
        0 > asprintf(&env[1], "CL_ALARM=%u", limit + 5)) {
        return;
    }
    start = time(NULL);
    send_env(&k, env);
    free(env[0]);
    free(env[1]);
    rc = finish_kid(&k, &status);
    took = difftime(time(NULL), start);
    snprintf(detail, sizeof(detail), "init %s, %s %d, after %.0f seconds",
             PMIx_Error_string(rc), WIFSIGNALED(status) ? "signal" : "exit",
             WIFSIGNALED(status) ? WTERMSIG(status)
                                 : (WIFEXITED(status) ? WEXITSTATUS(status) : -1),
             took);
    report(name, PMIX_SUCCESS != rc && WIFEXITED(status) &&
                     KID_INIT_FAILED == WEXITSTATUS(status) && took <= (double) limit,
           detail);
}

/* A tool told of the server's report file by PMIX_TOOL_ATTACHMENT_FILE
 * connects over the local socket */
static void tool_attach_case(kid_t *kp, const char *name, const char *path)
{
    kid_t k = *kp;
    char *env[2] = {NULL, NULL};
    int status = 0;
    pmix_status_t rc;
    char detail[256];

    if (0 > asprintf(&env[0], "CL_TOOL_ATTACH=%s", path)) {
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

/* A launcher, attached to the server by `server` ("file:<path>"), whose
 * rendezvous file name has a FIFO at it finishes its init within `limit`
 * seconds */
static void launcher_fifo_case(kid_t *kp, const char *name, const char *server,
                               const char *path, unsigned limit)
{
    kid_t k = *kp;
    char *env[4] = {NULL, NULL, NULL, NULL};
    int status = 0;
    pmix_status_t rc;
    char detail[256];
    time_t start;
    double took;

    if (0 > asprintf(&env[0], "CL_LAUNCHER_RNDZ=%s", path) ||
        0 > asprintf(&env[1], "CL_ALARM=%u", limit + 5) ||
        0 > asprintf(&env[2], "CL_LAUNCHER_SERVER=%s", server)) {
        return;
    }
    start = time(NULL);
    send_env(&k, env);
    free(env[0]);
    free(env[1]);
    free(env[2]);
    rc = finish_kid(&k, &status);
    took = difftime(time(NULL), start);
    snprintf(detail, sizeof(detail), "init %s, %s %d, after %.0f seconds",
             PMIx_Error_string(rc), WIFSIGNALED(status) ? "signal" : "exit",
             WIFSIGNALED(status) ? WTERMSIG(status)
                                 : (WIFEXITED(status) ? WEXITSTATUS(status) : -1),
             took);
    report(name, PMIX_SUCCESS == rc && WIFEXITED(status) && took <= (double) limit, detail);
}

/* A client whose connect-ack buffers are all zero when it releases them */
static void wipe_case(kid_t *kp, const char *name)
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
    if (NULL == pmix_getenv(PMIX_PTL_TOKEN_ENV, env)) {
        PMIx_Argv_free(env);
        report(name, 0, "the client was given no token");
        return;
    }
    set_var(&env, "CL_WIPE_CHECK", "1");
    send_env(&k, env);
    PMIx_Argv_free(env);
    rc = finish_kid(&k, &status);
    snprintf(detail, sizeof(detail), "init %s, exit %d", PMIx_Error_string(rc),
             WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    report(name, PMIX_SUCCESS == rc && WIFEXITED(status) && KID_OK == WEXITSTATUS(status),
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
/* A client whose server chooses a component it did not offer fails init.
 * The "server" is a socket of ours, at a URI the client is given: it
 * reads the offer and answers with `choice` (len bytes) */
static void choice_case(kid_t *kp, const char *name, const char *choice, size_t len)
{
    kid_t k = *kp;
    pmix_proc_t proc;
    struct sockaddr_un sa;
    struct pollfd pfd;
    pmix_ptl_hdr_t hdr;
    char **env = NULL, *value = NULL, *srvname, path[64], buf[4096];
    int status = 0, before = nconnected, lsd, sd = -1;
    uint32_t u32;
    size_t got;
    ssize_t rd;
    pmix_status_t rc;
    char detail[256];

    snprintf(path, sizeof(path), "/tmp/pmix-choice.%lu", (unsigned long) getpid());
    unlink(path);
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", path);
    lsd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (0 > lsd || 0 != bind(lsd, (struct sockaddr *) &sa, sizeof(sa)) || 0 != listen(lsd, 1)) {
        report(name, 0, "could not make the socket");
        return;
    }
    /* the server's name, as its own URI gives it */
    srvname = strdup(pmix_ptl_base.local_listener.uri);
    *strchr(srvname, ';') = '\0';
    if (0 > asprintf(&value, "%s;usock://%s;%lu:%lu", srvname, path, (unsigned long) geteuid(),
                     (unsigned long) getegid())) {
        return;
    }
    free(srvname);
    PMIX_LOAD_PROCID(&proc, CL_NSPACE, 0);
    if (PMIX_SUCCESS != PMIx_server_setup_fork(&proc, &env)) {
        report(name, 0, "setup_fork failed");
        return;
    }
    set_var(&env, PMIX_PTL_LOCAL_URI_ENV, value);
    free(value);
    send_env(&k, env);
    PMIx_Argv_free(env);

    pfd.fd = lsd;
    pfd.events = POLLIN;
    if (1 == poll(&pfd, 1, 20000)) {
        sd = accept(lsd, NULL, NULL);
    }
    if (0 <= sd) {
        /* the offer: its header, and as many bytes as that names */
        for (got = 0, rd = 1; 0 < rd && got < sizeof(hdr); got += (size_t) rd) {
            rd = read(sd, (char *) &hdr + got, sizeof(hdr) - got);
        }
        for (got = 0; 0 < rd && got < hdr.nbytes && hdr.nbytes <= sizeof(buf);
             got += (size_t) rd) {
            rd = read(sd, buf + got, hdr.nbytes - got);
        }
        u32 = htonl((uint32_t) PMIX_SUCCESS);
        (void) !write(sd, &u32, sizeof(u32));
        u32 = htonl((uint32_t) len);
        (void) !write(sd, &u32, sizeof(u32));
        (void) !write(sd, choice, len);
        /* the client hangs up rather than send its credential */
        pfd.fd = sd;
        pfd.events = POLLIN;
        while (1 == poll(&pfd, 1, 20000) && 0 < read(sd, buf, sizeof(buf))) {
        }
        close(sd);
    }
    close(lsd);
    unlink(path);
    rc = finish_kid(&k, &status);
    snprintf(detail, sizeof(detail), "init %s, exit %d, host asked %d times",
             PMIx_Error_string(rc), WIFEXITED(status) ? WEXITSTATUS(status) : -1,
             nconnected - before);
    report(name, PMIX_ERR_NOT_SUPPORTED == rc && before == nconnected, detail);
}

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

/* the rank's peers on the server's client list, and whether its entry
 * says it is lost - read on the progress thread */
typedef struct {
    pmix_event_t ev;
    pmix_lock_t lock;
    int npeers;
    bool lost;
} rankpeers_t;

static void count_rank_peers(int sd, short args, void *cbdata)
{
    rankpeers_t *r = (rankpeers_t *) cbdata;
    pmix_peer_t *pr;
    int i;
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    r->npeers = 0;
    r->lost = false;
    for (i = 0; i < pmix_server_globals.clients.size; i++) {
        pr = (pmix_peer_t *) pmix_pointer_array_get_item(&pmix_server_globals.clients, i);
        if (NULL != pr && NULL != pr->info && 0 == pr->info->pname.rank &&
            0 == strcmp(pr->info->pname.nspace, CL_NSPACE)) {
            ++r->npeers;
            r->lost = r->lost || pr->info->lost;
        }
    }
    PMIX_WAKEUP_THREAD(&r->lock);
}

static rankpeers_t rank_peers(void)
{
    rankpeers_t r;

    memset(&r, 0, sizeof(r));
    PMIX_CONSTRUCT_LOCK(&r.lock);
    PMIX_THREADSHIFT(&r, count_rank_peers);
    PMIX_WAIT_THREAD(&r.lock);
    PMIX_DESTRUCT_LOCK(&r.lock);
    return r;
}

/* A client dies without finalizing, and the host starts another for the
 * same rank without deregistering it. The dead one's peer is released
 * when the new one connects, rather than left on the client list */
static void dead_reclaim_case(kid_t *dp, kid_t *np, const char *name)
{
    kid_t dk = *dp, nk = *np;
    pmix_proc_t proc;
    char **env = NULL;
    int status = 0, n;
    pmix_status_t rc;
    rankpeers_t r;
    char detail[256];

    PMIX_LOAD_PROCID(&proc, CL_NSPACE, 0);
    if (PMIX_SUCCESS != PMIx_server_setup_fork(&proc, &env)) {
        report(name, 0, "setup_fork failed");
        return;
    }
    set_var(&env, "CL_DIE", "1");
    send_env(&dk, env);
    PMIx_Argv_free(env);
    env = NULL;
    rc = finish_kid(&dk, &status);
    if (PMIX_SUCCESS != rc) {
        snprintf(detail, sizeof(detail), "the first client's init returned %s",
                 PMIx_Error_string(rc));
        report(name, 0, detail);
        return;
    }
    /* the server reads its close */
    for (n = 0; n < 100; n++) {
        r = rank_peers();
        if (r.lost) {
            break;
        }
        usleep(50000);
    }
    if (!r.lost) {
        report(name, 0, "the server did not see the first client go");
        return;
    }
    if (PMIX_SUCCESS != PMIx_server_setup_fork(&proc, &env)) {
        report(name, 0, "second setup_fork failed");
        return;
    }
    send_env(&nk, env);
    PMIx_Argv_free(env);
    rc = finish_kid(&nk, &status);
    r = rank_peers();
    snprintf(detail, sizeof(detail), "second init %s, exit %d, %d peers for the rank",
             PMIx_Error_string(rc), WIFEXITED(status) ? WEXITSTATUS(status) : -1, r.npeers);
    report(name, PMIX_SUCCESS == rc && WIFEXITED(status) && KID_OK == WEXITSTATUS(status) &&
                     1 == r.npeers, detail);
}

static void tool_disc_case(kid_t *kp, const char *name)
{
    kid_t k = *kp;
    char *env[3] = {NULL, NULL, NULL};
    int status = 0;
    pmix_status_t rc;
    char detail[256];

    if (0 > asprintf(&env[0], "CL_SERVER_PID=%lu", (unsigned long) getpid()) ||
        0 > asprintf(&env[1], "CL_DISC_PENDING=1")) {
        return;
    }
    send_env(&k, env);
    free(env[0]);
    free(env[1]);
    rc = finish_kid(&k, &status);
    snprintf(detail, sizeof(detail), "init %s, exit %d", PMIx_Error_string(rc),
             WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    report(name, PMIX_SUCCESS == rc && WIFEXITED(status) && KID_OK == WEXITSTATUS(status),
           detail);
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
    char *fifo = NULL, *link = NULL, *lfifo = NULL;
    bool flag = true;
    pmix_status_t rc;
    kid_t kids[23];
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
    /* 0-3, 5, 7, 11, 16 and 18-21 are clients; the rest tools */
    for (n = 0; n < 23; n++) {
        if (!start_kid(&kids[n], 4 == n || 6 == n || (8 <= n && 10 >= n) ||
                                     (12 <= n && 15 >= n) || 17 == n || 22 == n)) {
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

    client_case(&kids[5], "a client given another server's local URI ignores it, and is sent "
                "back to the local socket from TCP",
                "other.server.7;usock:///tmp/pmix-no-such-dir/pmix-1;0:0",
                PMIX_ERR_LOCAL_URI_REQUIRED, false);

    value = tcp_uri();
    if (NULL == value) {
        return 1;
    }
    tool_uri_case(&kids[6], "a tool given the TCP URI alone is refused before connecting", value,
                  false, PMIX_ERR_NOT_SUPPORTED);
    free(value);

    singleton_case(&kids[7], "a singleton reaches the server started for it over its local "
                             "socket, with no token");

    if (0 > asprintf(&value, "file:%s", report)) {
        return 1;
    }
    tool_uri_case(&kids[8], "a tool given the report file connects over the local socket", value,
                  true, PMIX_SUCCESS);
    free(value);
    copy = other_host_copy(report);
    if (NULL == copy || 0 > asprintf(&value, "file:%s", copy)) {
        return 1;
    }
    tool_uri_case(&kids[9], "a tool reading a file whose local socket is another host's is "
                  "sent to the local socket from TCP",
                  value, false, PMIX_ERR_LOCAL_URI_REQUIRED);
    free(value);
    unlink(copy);
    free(copy);
    tool_case(&kids[10], "a second tool by the server's pid, after them all");

    userns_case(&kids[11], "a client in a user namespace of its own knows its server");

    /* a FIFO, and a symbolic link to the report file, where a tool is told
     * its server's file is */
    if (0 > asprintf(&fifo, "%s.fifo", report) || 0 > asprintf(&link, "%s.link", report) ||
        0 > asprintf(&lfifo, "%s.rndz.fifo", report)) {
        return 1;
    }
    unlink(fifo);
    unlink(link);
    unlink(lfifo);
    if (0 != mkfifo(fifo, 0600) || 0 != symlink(report, link) || 0 != mkfifo(lfifo, 0600) ||
        0 > asprintf(&value, "file:%s", fifo)) {
        fprintf(stderr, "could not make the FIFO or the link\n");
        return 1;
    }
    tool_file_refused_case(&kids[12], "a tool given a FIFO as its server's file fails init at "
                           "once", "CL_TOOL_URI", value, 5);
    free(value);
    tool_file_refused_case(&kids[13], "a tool given a FIFO as its attachment file fails init at "
                           "once", "CL_TOOL_ATTACH", fifo, 5);
    tool_file_refused_case(&kids[14], "a tool given a symbolic link to the report file as its "
                           "attachment file fails init", "CL_TOOL_ATTACH", link, 5);
    tool_attach_case(&kids[15], "a tool given the report file as its attachment file connects",
                     report);
    unlink(fifo);
    unlink(link);
    free(fifo);
    free(link);

    wipe_case(&kids[16], "a client releases its connect-ack buffers zeroed");

    if (0 > asprintf(&value, "file:%s", report)) {
        return 1;
    }
    launcher_fifo_case(&kids[17], "a launcher whose rendezvous file name has a FIFO at it "
                       "finishes its init promptly", value, lfifo, 5);
    free(value);
    unlink(lfifo);
    free(lfifo);

    /* security\0wire format\0<buffer type>datastore\0 */
    choice_case(&kids[18], "a client whose server chooses a security method it did not offer "
                "fails init", "no-such-method\0" "v70\0" "\1" "hash", 25);
    choice_case(&kids[19], "a client whose server chooses a wire format it does not have "
                "fails init", "native\0" "v99\0" "\1" "hash", 17);

    dead_reclaim_case(&kids[20], &kids[21], "a client that died without finalizing is "
                      "released when its rank connects again");

    tool_disc_case(&kids[22], "a request a tool's server has not answered completes "
                   "with an error when the tool disconnects from it");

    access_cases();

    PMIx_server_finalize();
    free(report);
    fprintf(stdout, "\n%d passed, %d failed\n", npass, nfail);
    return (0 == nfail) ? 0 : 1;
}
