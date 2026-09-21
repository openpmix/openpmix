/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Unit test: PMIx_Init tells "there is no server for me" apart from
 * "I was told where my server is and could not reach it".
 *
 * PMIX_ERR_UNREACH from PMIx_Init means the first: no server was
 * described to this process, so it comes up as a singleton - initialized,
 * with an identity of its own, owing a PMIx_Finalize. Consumers act on
 * that; Open MPI's MPI_Init, for one, carries on as a singleton when it
 * sees it.
 *
 * A process launched by a server is told where that server is, in its
 * environment. If that connection failed, PMIx_Init used to take the same
 * singleton path and return the same PMIX_ERR_UNREACH - leaving a process
 * that carried the launcher's namespace and rank, none of its job data,
 * and a status its caller read as "fine, run alone". The failure then
 * surfaced far from its cause: in CI, a server that dropped one client's
 * connection became an MPI_Init that could not pair its TCP interfaces
 * with those of peers on its own node, because that one rank had no
 * locality information for anybody.
 *
 * The contract now: a process given a server - by its environment or by a
 * PMIX_SERVER_URI directive - that cannot reach it gets
 * PMIX_ERR_COMM_FAILURE, and the library is left uninitialized. Only a
 * process given no server at all is a singleton.
 *
 * Each case runs PMIx_Init in a forked child, which reports the status and
 * whether the library was left initialized. The cases:
 *
 *   - no server described             -> PMIX_ERR_UNREACH, initialized
 *   - environment names a port with
 *     nothing listening                -> PMIX_ERR_COMM_FAILURE, not initialized
 *   - environment names a server that
 *     accepts and closes without a
 *     reply (the shape of the CI
 *     failure)                         -> PMIX_ERR_COMM_FAILURE, not initialized
 *   - a PMIX_SERVER_URI directive
 *     names a port with nothing
 *     listening                        -> PMIX_ERR_COMM_FAILURE, not initialized
 */

#include "src/include/pmix_config.h"

#include "include/pmix.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#define TEST_NSPACE "client-connect-fail@1"
#define SERVER_ID   "client-connect-fail@0.0"

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

/* Remove everything that tells a process where its server is, and who it
 * is, so each case starts from exactly what it sets. Other PMIX_* variables
 * - the test environment's MCA settings among them - are left alone. */
static void clear_server_env(void)
{
    extern char **environ;
    char **names = NULL;
    size_t n, count = 0;

    for (n = 0; NULL != environ[n]; n++) {
        ++count;
    }
    names = calloc(count + 1, sizeof(char *));
    if (NULL == names) {
        return;
    }
    count = 0;
    for (n = 0; NULL != environ[n]; n++) {
        const char *eq = strchr(environ[n], '=');
        if (NULL == eq) {
            continue;
        }
        if (0 == strncmp(environ[n], "PMIX_SERVER_URI", strlen("PMIX_SERVER_URI"))
            || 0 == strncmp(environ[n], "PMIX_NAMESPACE=", strlen("PMIX_NAMESPACE="))
            || 0 == strncmp(environ[n], "PMIX_RANK=", strlen("PMIX_RANK="))
            || 0 == strncmp(environ[n], "PMIX_VERSION=", strlen("PMIX_VERSION="))) {
            names[count] = strndup(environ[n], (size_t) (eq - environ[n]));
            if (NULL != names[count]) {
                ++count;
            }
        }
    }
    for (n = 0; n < count; n++) {
        unsetenv(names[n]);
        free(names[n]);
    }
    free(names);
}

/* the environment a launcher gives a client it starts, pointing at port */
static void set_launched_env(int port)
{
    char uri[128];

    snprintf(uri, sizeof(uri), SERVER_ID ";tcp4://127.0.0.1:%d", port);
    setenv("PMIX_SERVER_URI41", uri, 1);
    setenv("PMIX_NAMESPACE", TEST_NSPACE, 1);
    setenv("PMIX_RANK", "0", 1);
}

/* a loopback TCP socket bound to an ephemeral port, listening or not */
static int bound_socket(int *port, bool listening)
{
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    int sd, one = 1;

    sd = socket(AF_INET, SOCK_STREAM, 0);
    if (0 > sd) {
        return -1;
    }
    (void) setsockopt(sd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (0 != bind(sd, (struct sockaddr *) &addr, sizeof(addr))
        || 0 != getsockname(sd, (struct sockaddr *) &addr, &len)
        || (listening && 0 != listen(sd, 4))) {
        close(sd);
        return -1;
    }
    *port = ntohs(addr.sin_port);
    return sd;
}

/* A port nothing is listening on. Binding and closing leaves the port
 * unused, so a connect to it is refused - which is what a client sees when
 * the server it was told about is no longer there. */
static int dead_port(void)
{
    int port = 0;
    int sd = bound_socket(&port, false);

    if (0 > sd) {
        return -1;
    }
    close(sd);
    return port;
}

typedef struct {
    int status;
    int initialized;
} result_t;

/* In the child: run PMIx_Init, report what it said and whether the library
 * is initialized, and clean up only what PMIx_Init says is ours to clean. */
static void child_init(int wfd, pmix_info_t *info, size_t ninfo)
{
    pmix_proc_t me;
    result_t res;
    ssize_t w;

    res.status = PMIx_Init(&me, info, ninfo);
    res.initialized = PMIx_Initialized() ? 1 : 0;
    w = write(wfd, &res, sizeof(res));
    (void) w;
    if (PMIX_SUCCESS == res.status || PMIX_ERR_UNREACH == res.status) {
        PMIx_Finalize(NULL, 0);
    }
    _exit(0);
}

/* In the parent: wait for the child's result. A child that never answers
 * is killed and counted as having hung. */
static bool collect(pid_t child, int rfd, result_t *res)
{
    struct pollfd pfd;
    ssize_t r = -1;
    int status;

    pfd.fd = rfd;
    pfd.events = POLLIN;
    if (0 < poll(&pfd, 1, 60000)) {
        r = read(rfd, res, sizeof(*res));
    }
    close(rfd);
    if ((ssize_t) sizeof(*res) != r) {
        kill(child, SIGKILL);
        waitpid(child, &status, 0);
        return false;
    }
    waitpid(child, &status, 0);
    return true;
}

static void expect(const char *name, bool got, const result_t *res,
                   pmix_status_t want, int want_init)
{
    char detail[256];

    if (!got) {
        report(name, 0, "the child hung or died without reporting");
        return;
    }
    snprintf(detail, sizeof(detail), "PMIx_Init returned %s with the library %s; "
             "expected %s with it %s",
             PMIx_Error_string(res->status), res->initialized ? "initialized" : "not initialized",
             PMIx_Error_string(want), want_init ? "initialized" : "not initialized");
    report(name, want == res->status && want_init == res->initialized, detail);
}

static void test_no_server(void)
{
    int fds[2];
    pid_t child;
    result_t res;
    bool got;

    if (0 != pipe(fds)) {
        report("no server described", 0, "pipe failed");
        return;
    }
    child = fork();
    if (0 == child) {
        close(fds[0]);
        clear_server_env();
        child_init(fds[1], NULL, 0);
    }
    close(fds[1]);
    got = collect(child, fds[0], &res);
    expect("no server described: singleton", got, &res, PMIX_ERR_UNREACH, 1);
}

static void test_env_dead_port(void)
{
    int fds[2], port;
    pid_t child;
    result_t res;
    bool got;

    port = dead_port();
    if (0 > port || 0 != pipe(fds)) {
        report("environment names a dead port", 0, "setup failed");
        return;
    }
    child = fork();
    if (0 == child) {
        close(fds[0]);
        clear_server_env();
        set_launched_env(port);
        child_init(fds[1], NULL, 0);
    }
    close(fds[1]);
    got = collect(child, fds[0], &res);
    expect("environment names a port with nothing listening", got, &res,
           PMIX_ERR_COMM_FAILURE, 0);
}

static void test_env_dropped_connection(void)
{
    int fds[2], port = 0, lsd, csd;
    pid_t child;
    result_t res;
    bool got;
    struct pollfd pfd;
    char buf[4096];

    lsd = bound_socket(&port, true);
    if (0 > lsd || 0 != pipe(fds)) {
        report("server drops the connection", 0, "setup failed");
        return;
    }
    child = fork();
    if (0 == child) {
        close(fds[0]);
        close(lsd);
        clear_server_env();
        set_launched_env(port);
        child_init(fds[1], NULL, 0);
    }
    close(fds[1]);
    /* Be the server that accepts the connection and never answers it: take
     * whatever the client sends, then close without a reply. The client
     * reads EOF where the status of its connect-ack should be. */
    pfd.fd = lsd;
    pfd.events = POLLIN;
    if (0 < poll(&pfd, 1, 30000)) {
        csd = accept(lsd, NULL, NULL);
        if (0 <= csd) {
            pfd.fd = csd;
            if (0 < poll(&pfd, 1, 2000)) {
                ssize_t r = read(csd, buf, sizeof(buf));
                (void) r;
            }
            close(csd);
        }
    }
    close(lsd);
    got = collect(child, fds[0], &res);
    expect("server accepts and closes without a reply", got, &res,
           PMIX_ERR_COMM_FAILURE, 0);
}

static void test_directive_dead_port(void)
{
    int fds[2], port;
    pid_t child;
    result_t res;
    bool got;
    char uri[160];
    pmix_info_t info;

    port = dead_port();
    if (0 > port || 0 != pipe(fds)) {
        report("directive names a dead port", 0, "setup failed");
        return;
    }
    child = fork();
    if (0 == child) {
        close(fds[0]);
        clear_server_env();
        snprintf(uri, sizeof(uri), "PMIX_SERVER_URI41;" SERVER_ID ";tcp4://127.0.0.1:%d", port);
        PMIX_INFO_LOAD(&info, PMIX_SERVER_URI, uri, PMIX_STRING);
        child_init(fds[1], &info, 1);
    }
    close(fds[1]);
    got = collect(child, fds[0], &res);
    expect("PMIX_SERVER_URI directive names a port with nothing listening", got, &res,
           PMIX_ERR_COMM_FAILURE, 0);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    signal(SIGPIPE, SIG_IGN);

    fprintf(stdout, "client_connect_fail:\n");
    test_no_server();
    test_env_dead_port();
    test_env_dropped_connection();
    test_directive_dead_port();

    fprintf(stdout, "client_connect_fail: %d passed, %d failed\n", npass, nfail);
    return (0 == nfail) ? 0 : 1;
}
