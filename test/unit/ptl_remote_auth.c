/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * A tool and a server on different hosts authenticate each other, the
 * server first: the tool asks the server to prove who it is, checks the
 * credential it sends back and whom it names, and only then sends its
 * connect-ack, with a credential made for that connection. The server
 * takes no peer on another host that has not been through this.
 *
 * Both run here, on one host, told to treat every TCP peer as on another
 * (pmix_ptl_base_test_remote), with a PKI made for the test. A tool
 * connects to the server's TCP URI:
 *
 *   - by its blocking connect, and by an event-driven attach: taken - with
 *     ssl, and with munge where a munged is running
 *   - taking the server to be on its own host, as through a port forward:
 *     the server tells it to have the server prove who it is, and it does
 *     - or, with no mechanism that can, fails saying so
 *   - with no mechanism that authenticates across hosts: it sends nothing
 *   - to a server that takes no tools, or no tools from other hosts:
 *     refused
 *   - to a server whose certificate it does not trust, or one for clients
 *     only: it refuses the server
 *   - with a certificate the server does not trust: the server refuses it
 *   - to a server that closes the connection on its opening, as one of an
 *     earlier release does: it fails, saying so
 *   - to a TCP server on its own host when told the kernel names no
 *     server's user (pmix_ptl_base_test_no_tcp_table): it has the server
 *     prove who it is, and refuses one that does not - or, with no
 *     mechanism that can, fails saying so - unless the server's
 *     rendezvous file names an earlier release, which it connects to as
 *     before. A server that takes no tools from other hosts proves who it
 *     is to it all the same. The proof is bound to the connection's two
 *     ends: through a relay on this host, it fails
 *
 * The credential a server sends a tool with munge is refused as an
 * ordinary one, by munge and by PMIx_Validate_credential.
 *
 * and a peer speaking the wire directly is refused when it sends its
 * opening twice, with a credential, or over the local socket; when its
 * connect-ack names another mechanism than its opening, carries a
 * credential bound to another connection or made as a server's, or names
 * an earlier release; and, if it goes quiet once the server has proved
 * who it is, is dropped by the time the connect-ack timeout runs out from
 * its arrival.
 *
 * Built only where psec/ssl is built against a real OpenSSL; it skips in
 * a build without remote tools.
 */

#include "src/include/pmix_config.h"

#include "include/pmix.h"
#include "include/pmix_server.h"
#include "include/pmix_tool.h"
#include "src/include/pmix_globals.h"
#include "src/mca/psec/base/base.h"
#include "src/mca/ptl/base/base.h"
#include "src/util/pmix_error.h"
#include "src/util/pmix_getid.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include "ssl_test_pki.h"

#define SKIP 77

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

    PMIX_LOAD_PROCID(&proc, "remoteauth.tool", ntools++);
    cbfunc(PMIX_SUCCESS, &proc, cbdata);
}

static pmix_server_module_t mymodule = {
    .tool_connected = tool_connected
};

/* the PKI's files */
static char cafile[1024], ca2file[1024], bothcert[1024], clientcert[1024], keyfile[1024],
    untrustedcert[1024], untrustedkey[1024];

static void use_ssl(const char *ca, const char *cert, const char *key)
{
    setenv("PMIX_MCA_psec_ssl_ca_file", ca, 1);
    setenv("PMIX_MCA_psec_ssl_cert_file", cert, 1);
    setenv("PMIX_MCA_psec_ssl_key_file", key, 1);
}

/* ---------------------------------------------------------------- *
 * the server
 * ---------------------------------------------------------------- */

typedef struct {
    bool remote;       // PMIX_SERVER_REMOTE_CONNECTIONS
    bool tools;        // PMIX_SERVER_TOOL_SUPPORT
    bool legacy;       // PMIX_SERVER_LEGACY_PEERS - a TCP listener without remote
    bool onhost;       // take its TCP peers to be on this host
    const char *psec;  // the mechanisms it has, if not every one
    bool table;        // ...on this host, whose kernel names its TCP peers' users
    bool notable;      // ...anywhere, whose kernel names none
    const char *cert;  // its certificate; NULL for munge
    int ack_timeout;   // ptl_base_connect_ack_timeout, if not 0
} srvcfg_t;

/* listen over TCP, say where - and where the local socket is - and wait to
 * be told to go. Exits 3 when it was to use munge and there is none */
static void run_server(const srvcfg_t *cfg, int urifd, int gofd)
{
    pmix_info_t info[3];
    bool yes = true, remote = cfg->remote, tools = cfg->tools;
    char c, buf[2048], num[16];
    const char *usock;
    size_t n = 2;

    /* with no certificate of its own to name, the server is to use munge
     * - it has ssl too, so it starts where munge is not built or no munged
     * runs, and says below that munge is not there. The tool names munge */
    use_ssl(cafile, (NULL == cfg->cert) ? bothcert : cfg->cert, keyfile);
    if (NULL != cfg->psec) {
        setenv("PMIX_MCA_psec", cfg->psec, 1);
    }
    if (cfg->onhost) {
        /* a tool that does not prove itself - one connecting as before to
         * what it takes for an earlier release - is checked by native,
         * which takes it on its claim where this platform has no table */
        setenv("PMIX_MCA_psec_native_legacy_auth", "1", 1);
    }
    if (0 < cfg->ack_timeout) {
        snprintf(num, sizeof(num), "%d", cfg->ack_timeout);
        setenv("PMIX_MCA_ptl_base_connect_ack_timeout", num, 1);
    }
    pmix_ptl_base_test_remote = !cfg->onhost;
    /* on this host, the server's kernel names no TCP peer's user exactly
     * when the tool's names no server's */
    pmix_ptl_base_test_no_tcp_table = (cfg->onhost && !cfg->table) || cfg->notable;
    PMIX_INFO_LOAD(&info[0], PMIX_SERVER_TOOL_SUPPORT, &tools, PMIX_BOOL);
    PMIX_INFO_LOAD(&info[1], PMIX_SERVER_REMOTE_CONNECTIONS, &remote, PMIX_BOOL);
    if (cfg->legacy) {
        PMIX_INFO_LOAD(&info[2], PMIX_SERVER_LEGACY_PEERS, &yes, PMIX_BOOL);
        n = 3;
    }
    if (PMIX_SUCCESS != PMIx_server_init(&mymodule, info, n) ||
        NULL == pmix_ptl_base.listener.uri ||
        NULL == strstr(pmix_ptl_base.listener.uri, "tcp")) {
        _exit(2);
    }
    if (NULL == cfg->cert && NULL == pmix_psec_base_assign_module("munge")) {
        PMIx_server_finalize();
        _exit(3);
    }
    usock = (NULL == pmix_ptl_base.local_listener.uri)
                ? NULL : strstr(pmix_ptl_base.local_listener.uri, "usock://");
    snprintf(buf, sizeof(buf), "%s\n%s\n", pmix_ptl_base.listener.uri,
             (NULL == usock) ? "" : usock + strlen("usock://"));
    if ((ssize_t) strlen(buf) != write(urifd, buf, strlen(buf))) {
        _exit(2);
    }
    close(urifd);
    if (0 > read(gofd, &c, 1)) {
        _exit(2);
    }
    PMIx_server_finalize();
    _exit(0);
}

typedef struct {
    pid_t pid;
    int go;
    char uri[2048];
    char usock[2048];
} server_t;

static void stop_server(server_t *srv)
{
    int st;

    if (0 <= srv->go) {
        if (1 != write(srv->go, "x", 1) && 0 < srv->pid) {
            kill(srv->pid, SIGKILL);
        }
        close(srv->go);
        srv->go = -1;
    }
    if (0 < srv->pid) {
        (void) waitpid(srv->pid, &st, 0);
        srv->pid = -1;
    }
}

/* 0 when it is up, SKIP when it was to use munge and there is none, 1
 * otherwise */
static int start_server(server_t *srv, const srvcfg_t *cfg)
{
    int urip[2], gop[2], st;
    char buf[2048], *nl;
    size_t got = 0;
    ssize_t n;

    srv->pid = -1;
    srv->go = -1;
    if (0 != pipe(urip)) {
        return 1;
    }
    if (0 != pipe(gop)) {
        close(urip[0]);
        close(urip[1]);
        return 1;
    }
    fflush(stdout);
    srv->pid = fork();
    if (0 == srv->pid) {
        alarm(120);
        close(urip[0]);
        close(gop[1]);
        run_server(cfg, urip[1], gop[0]);
    }
    close(urip[1]);
    close(gop[0]);
    srv->go = gop[1];
    if (0 > srv->pid) {
        close(urip[0]);
        stop_server(srv);
        return 1;
    }
    while (got < sizeof(buf) - 1 && 0 < (n = read(urip[0], buf + got, sizeof(buf) - 1 - got))) {
        got += (size_t) n;
    }
    close(urip[0]);
    buf[got] = '\0';
    nl = strchr(buf, '\n');
    if (NULL == nl) {
        /* it did not start - and says why in its status */
        if (srv->pid == waitpid(srv->pid, &st, 0) && WIFEXITED(st) && 3 == WEXITSTATUS(st)) {
            srv->pid = -1;
            stop_server(srv);
            return SKIP;
        }
        srv->pid = -1;
        stop_server(srv);
        return 1;
    }
    *nl = '\0';
    snprintf(srv->uri, sizeof(srv->uri), "%s", buf);
    snprintf(srv->usock, sizeof(srv->usock), "%s", nl + 1);
    nl = strchr(srv->usock, '\n');
    if (NULL != nl) {
        *nl = '\0';
    }
    return 0;
}

/* ---------------------------------------------------------------- *
 * a real tool
 * ---------------------------------------------------------------- */

typedef struct {
    const char *ca;
    const char *cert;
    const char *key;
    const char *mech;  // the mechanisms to have, if not native and ssl
    bool local;        // take the server to be on this host
    bool notable;      // take the kernel to name no TCP server's user
    bool attach;       // connect by an event-driven attach after init
    const char *version; // reach the server through a rendezvous file naming it
    bool foreign;      // ...owned by a user whose server it may not talk to
    const char *srvpsec; // ...listing these mechanisms for the server
    bool nopid;        // ...as a report-URI file is, with no pid line
    bool linked;       // ...reached through a symbolic link
    bool every;        // have every mechanism there is, munge first where it runs
    bool singleton;    // not a tool: a singleton finding the URI as its system server's
} tool_t;

/* a tool connecting to the URI; it exits with the status's negation, and
 * what it says on stderr goes to errpath */
static pmix_status_t run_tool(const char *uri, const tool_t *t, const char *errpath)
{
    pmix_info_t info;
    pmix_proc_t me, server;
    pmix_status_t rc;
    bool flag = true;
    pid_t pid;
    int status, fd;

    fflush(stdout);
    pid = fork();
    if (0 == pid) {
        alarm(120);
        fd = open(errpath, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (0 <= fd) {
            dup2(fd, 2);
            close(fd);
        }
        if (t->every) {
            unsetenv("PMIX_MCA_psec");
            use_ssl(t->ca, t->cert, t->key);
        } else if (NULL != t->mech) {
            setenv("PMIX_MCA_psec", t->mech, 1);
        } else {
            /* ssl, which the tool picks itself for a server on another
             * host - and not munge, which outranks it where a munged
             * runs */
            setenv("PMIX_MCA_psec", "native,ssl", 1);
            use_ssl(t->ca, t->cert, t->key);
        }
        pmix_ptl_base_test_remote = !t->local;
        pmix_ptl_base_test_no_tcp_table = t->notable;
        if (t->singleton) {
            char host[256], *dot, path[2048], body[2560];
            const char *tmp = getenv("TMPDIR");

            /* the system server's file, as PMIx names it on this host */
            gethostname(host, sizeof(host));
            host[sizeof(host) - 1] = '\0';
            if (NULL != (dot = strchr(host, '.'))) {
                *dot = '\0';
            }
            snprintf(path, sizeof(path), "%s/pmix.sys.%s", tmp, host);
            snprintf(body, sizeof(body), "%s\n%s\n%lu\n%lu:%lu\nnow\n", uri,
                     (NULL == t->version) ? PMIX_VERSION : t->version,
                     (unsigned long) getpid(), (unsigned long) geteuid(),
                     (unsigned long) getegid());
            write_file(path, body, strlen(body), 0600);
            setenv("PMIX_SYSTEM_TMPDIR", tmp, 1);
            rc = PMIx_Init(&me, NULL, 0);
            if (PMIX_SUCCESS == rc) {
                PMIx_Finalize(NULL, 0);
            }
            unlink(path);
            _exit((-rc) & 0xff);
        }
        if (t->notable) {
            /* so that it says whom the server proved it is */
            setenv("PMIX_MCA_ptl_base_verbose", "2", 1);
        }
        if (t->attach) {
            PMIX_INFO_LOAD(&info, PMIX_TOOL_DO_NOT_CONNECT, &flag, PMIX_BOOL);
            rc = PMIx_tool_init(&me, &info, 1);
            PMIX_INFO_DESTRUCT(&info);
            if (PMIX_SUCCESS == rc) {
                PMIX_INFO_LOAD(&info, PMIX_SERVER_URI, uri, PMIX_STRING);
                rc = PMIx_tool_attach_to_server(&me, &server, &info, 1);
                PMIX_INFO_DESTRUCT(&info);
                PMIx_tool_finalize();
            }
        } else {
            PMIX_INFO_LOAD(&info, PMIX_SERVER_URI, uri, PMIX_STRING);
            rc = PMIx_tool_init(&me, &info, 1);
            PMIX_INFO_DESTRUCT(&info);
            if (PMIX_SUCCESS == rc) {
                PMIx_tool_finalize();
            }
        }
        _exit((-rc) & 0xff);
    }
    if (0 > pid || pid != waitpid(pid, &status, 0) || !WIFEXITED(status)) {
        return PMIX_ERROR;
    }
    return -(pmix_status_t) WEXITSTATUS(status);
}

/* whether what the tool wrote to stderr says `what` */
static bool said(const char *path, const char *what)
{
    char buf[16384];
    size_t n;
    FILE *fp;

    fp = fopen(path, "r");
    if (NULL == fp) {
        return false;
    }
    n = fread(buf, 1, sizeof(buf) - 1, fp);
    fclose(fp);
    buf[n] = '\0';
    return NULL != strstr(buf, what);
}

/* whether this kernel names the user of a TCP peer on this host */
static bool tcp_table_answers(void)
{
    struct sockaddr_in sa;
    pmix_socklen_t len = sizeof(sa);
    int lsd, csd = -1, asd = -1;
    uid_t uid;
    bool answers = false;

    lsd = socket(AF_INET, SOCK_STREAM, 0);
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (0 <= lsd && 0 == bind(lsd, (struct sockaddr *) &sa, sizeof(sa)) &&
        0 == listen(lsd, 1) && 0 == getsockname(lsd, (struct sockaddr *) &sa, &len) &&
        0 <= (csd = socket(AF_INET, SOCK_STREAM, 0)) &&
        0 == connect(csd, (struct sockaddr *) &sa, sizeof(sa)) &&
        0 <= (asd = accept(lsd, NULL, NULL))) {
        answers = (PMIX_ERR_NOT_SUPPORTED != pmix_util_getid_tcp(csd, &uid));
    }
    if (0 <= asd) {
        close(asd);
    }
    if (0 <= csd) {
        close(csd);
    }
    if (0 <= lsd) {
        close(lsd);
    }
    return answers;
}

/* a tool against a server; `says` is what the tool must say, `not` what it
 * must not */
static void one_case(const char *name, const srvcfg_t *cfg, const tool_t *t, pmix_status_t want,
                     const char *says, const char *not, const char *dir)
{
    server_t srv;
    char detail[512], errpath[1024], rndz[1024], link[1024], uri[2048 + 8], body[4096],
        pidline[32];
    const char *target;
    pmix_status_t rc;
    bool ok;
    int up;

    if (t->foreign && 0 != geteuid()) {
        /* only root can give a file away */
        fprintf(stdout, "  SKIP: %s (not root)\n", name);
        return;
    }
    up = start_server(&srv, cfg);
    if (SKIP == up) {
        fprintf(stdout, "  SKIP: %s (no munged)\n", name);
        return;
    }
    if (0 != up) {
        report(name, 0, "the server did not start");
        return;
    }
    snprintf(errpath, sizeof(errpath), "%s/tool.err", dir);
    target = srv.uri;
    rndz[0] = '\0';
    if (NULL != t->version) {
        /* the server's address, and the release the file says it is */
        snprintf(rndz, sizeof(rndz), "%s/server.rndz", dir);
        snprintf(pidline, sizeof(pidline), "%lu\n", (unsigned long) srv.pid);
        snprintf(body, sizeof(body), "%s\n%s\n%s%s%s%s", srv.uri, t->version,
                 t->nopid ? "" : pidline,
                 (NULL == t->srvpsec) ? "" : PMIX_PTL_PSEC_TAG,
                 (NULL == t->srvpsec) ? "" : t->srvpsec, (NULL == t->srvpsec) ? "" : "\n");
        write_file(rndz, body, strlen(body), 0644);
        if (t->foreign && 0 != chown(rndz, 65534, 65534)) {
            stop_server(&srv);
            unlink(rndz);
            report(name, 0, "could not give the file away");
            return;
        }
        snprintf(uri, sizeof(uri), "file:%s", rndz);
        if (t->linked) {
            snprintf(link, sizeof(link), "%s/server.link", dir);
            if (0 != symlink(rndz, link)) {
                stop_server(&srv);
                unlink(rndz);
                report(name, 0, "could not link to the file");
                return;
            }
            snprintf(uri, sizeof(uri), "file:%s", link);
        }
        target = uri;
    }
    rc = run_tool(target, t, errpath);
    stop_server(&srv);
    if ('\0' != rndz[0]) {
        unlink(rndz);
        if (t->linked) {
            unlink(link);
        }
    }
    ok = (want == rc) && (NULL == says || said(errpath, says)) &&
         (NULL == not || !said(errpath, not));
    snprintf(detail, sizeof(detail), "the tool's connect returned %s, not %s; said \"%s\": %s; "
             "said \"%s\": %s", PMIx_Error_string(rc), PMIx_Error_string(want),
             (NULL == says) ? "" : says, (NULL != says && said(errpath, says)) ? "yes" : "no",
             (NULL == not) ? "" : not, (NULL != not && said(errpath, not)) ? "yes" : "no");
    report(name, ok, detail);
    unlink(errpath);
}

/* ---------------------------------------------------------------- *
 * a relay on this host: it takes the tool's connection, opens one of its
 * own to the server, and carries every byte between them
 * ---------------------------------------------------------------- */

static bool send_all(int sd, const char *buf, size_t len);

static void run_relay(int lsd, const struct sockaddr_in *to)
{
    struct pollfd pfd[2];
    char buf[4096];
    ssize_t n;
    int a, b, i;

    alarm(120);
    a = accept(lsd, NULL, NULL);
    b = socket(AF_INET, SOCK_STREAM, 0);
    if (0 > a || 0 > b || 0 != connect(b, (const struct sockaddr *) to, sizeof(*to))) {
        _exit(1);
    }
    pfd[0].fd = a;
    pfd[1].fd = b;
    pfd[0].events = pfd[1].events = POLLIN;
    while (0 < poll(pfd, 2, 30000)) {
        for (i = 0; i < 2; i++) {
            if (0 == (pfd[i].revents & (POLLIN | POLLHUP | POLLERR))) {
                continue;
            }
            n = read(pfd[i].fd, buf, sizeof(buf));
            if (0 >= n || !send_all(pfd[1 - i].fd, buf, (size_t) n)) {
                _exit(0);
            }
        }
    }
    _exit(0);
}

static void relay_case(const char *name, const srvcfg_t *cfg, const tool_t *t,
                       pmix_status_t want, const char *says, const char *dir)
{
    struct sockaddr_in sa, to;
    pmix_socklen_t len = sizeof(sa);
    server_t srv;
    char uri[2048 + 64], addr[64], errpath[1024], detail[512];
    const char *semi;
    unsigned port;
    pmix_status_t rc;
    pid_t pid;
    int lsd, st;
    bool ok;

    if (0 != start_server(&srv, cfg)) {
        report(name, 0, "the server did not start");
        return;
    }
    semi = strchr(srv.uri, ';');
    memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    if (NULL == semi || 2 != sscanf(semi + 1, "tcp4://%63[^:]:%u", addr, &port) ||
        1 != inet_pton(AF_INET, addr, &to.sin_addr)) {
        stop_server(&srv);
        report(name, 0, "the server has no IPv4 address");
        return;
    }
    to.sin_port = htons((uint16_t) port);
    lsd = socket(AF_INET, SOCK_STREAM, 0);
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (0 > lsd || 0 != bind(lsd, (struct sockaddr *) &sa, sizeof(sa)) || 0 != listen(lsd, 4) ||
        0 != getsockname(lsd, (struct sockaddr *) &sa, &len)) {
        stop_server(&srv);
        report(name, 0, "could not make the relay's listener");
        if (0 <= lsd) {
            close(lsd);
        }
        return;
    }
    /* the server's name, at the relay's address */
    snprintf(uri, sizeof(uri), "%.*s;tcp4://127.0.0.1:%u", (int) (semi - srv.uri), srv.uri,
             (unsigned) ntohs(sa.sin_port));
    fflush(stdout);
    pid = fork();
    if (0 == pid) {
        run_relay(lsd, &to);
    }
    close(lsd);
    snprintf(errpath, sizeof(errpath), "%s/tool.err", dir);
    rc = run_tool(uri, t, errpath);
    if (0 < pid) {
        kill(pid, SIGKILL);
        (void) waitpid(pid, &st, 0);
    }
    stop_server(&srv);
    ok = (want == rc) && (NULL == says || said(errpath, says));
    snprintf(detail, sizeof(detail), "the tool's connect returned %s, not %s; said \"%s\": %s",
             PMIx_Error_string(rc), PMIx_Error_string(want), (NULL == says) ? "" : says,
             (NULL != says && said(errpath, says)) ? "yes" : "no");
    report(name, ok, detail);
    unlink(errpath);
}

/* ---------------------------------------------------------------- *
 * a fake server: a plain TCP listener on loopback
 * ---------------------------------------------------------------- */

typedef enum {
    FAKE_CLOSE_ON_OPENING,  // read the tool's opening, then close - as an earlier release does
    FAKE_COUNT_BYTES        // read until the tool closes, and count what it sent
} fake_t;

static void fake_case(const char *name, fake_t kind, const tool_t *t, pmix_status_t want,
                      const char *says, const char *dir)
{
    struct sockaddr_in sa;
    pmix_socklen_t len = sizeof(sa);
    struct pollfd pfd;
    char uri[128], buf[4096], detail[512], errpath[1024];
    ssize_t n;
    size_t total = 0;
    int lsd, sd = -1, status;
    pid_t pid;
    pmix_status_t rc = PMIX_ERROR;
    bool ok;

    lsd = socket(AF_INET, SOCK_STREAM, 0);
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (0 > lsd || 0 != bind(lsd, (struct sockaddr *) &sa, sizeof(sa)) || 0 != listen(lsd, 4) ||
        0 != getsockname(lsd, (struct sockaddr *) &sa, &len)) {
        report(name, 0, "could not make the listener");
        if (0 <= lsd) {
            close(lsd);
        }
        return;
    }
    snprintf(uri, sizeof(uri), "fake.server.0;tcp4://127.0.0.1:%u", (unsigned) ntohs(sa.sin_port));
    snprintf(errpath, sizeof(errpath), "%s/tool.err", dir);

    fflush(stdout);
    pid = fork();
    if (0 == pid) {
        close(lsd);
        _exit((-run_tool(uri, t, errpath)) & 0xff);
    }
    pfd.fd = lsd;
    pfd.events = POLLIN;
    if (1 == poll(&pfd, 1, 30000)) {
        sd = accept(lsd, NULL, NULL);
    }
    if (0 <= sd) {
        pfd.fd = sd;
        if (FAKE_CLOSE_ON_OPENING == kind) {
            /* the header and whatever of the message arrives in a moment */
            if (1 == poll(&pfd, 1, 30000) && 0 > read(sd, buf, sizeof(buf))) {
                /* the opening did not arrive - closing is all that is left */
                total = 0;
            }
        } else {
            while (1 == poll(&pfd, 1, 30000) && 0 < (n = read(sd, buf, sizeof(buf)))) {
                total += (size_t) n;
            }
        }
        close(sd);
    }
    close(lsd);
    if (0 < pid && pid == waitpid(pid, &status, 0) && WIFEXITED(status)) {
        rc = -(pmix_status_t) WEXITSTATUS(status);
    }
    ok = (0 <= sd) && (want == rc) && (NULL == says || said(errpath, says)) &&
         (FAKE_COUNT_BYTES != kind || 0 == total);
    snprintf(detail, sizeof(detail), "accepted: %s; the tool's connect returned %s, not %s; "
             "%lu bytes sent; said \"%s\": %s", (0 <= sd) ? "yes" : "no", PMIx_Error_string(rc),
             PMIx_Error_string(want), (unsigned long) total, (NULL == says) ? "" : says,
             (NULL != says && said(errpath, says)) ? "yes" : "no");
    report(name, ok, detail);
    unlink(errpath);
}

/* ---------------------------------------------------------------- *
 * a peer speaking the wire directly
 * ---------------------------------------------------------------- */

typedef enum {
    RAW_OPENING_TWICE,
    RAW_OPENING_WITH_CRED,
    RAW_OPENING_ON_USOCK,
    RAW_OPENING_BOUND,      // binding the connection, from another host
    RAW_ACK_OTHER_MECH,
    RAW_ACK_OTHER_REMOTE_MECH, // munge after an ssl opening - both can authenticate across hosts
    RAW_ACK_OTHER_NONCE,
    RAW_ACK_AS_SERVER,
    RAW_ACK_EARLIER_RELEASE,
    RAW_QUIET,
    RAW_MUNGE_BOUND_AS_PLAIN  // the server's munge credential, presented as an ordinary one
} raw_t;

#define RAW_NO_MUNGE 246

static bool send_all(int sd, const char *buf, size_t len)
{
    ssize_t n;

    while (0 < len) {
        n = send(sd, buf, len, 0);
        if (0 >= n) {
            return false;
        }
        buf += n;
        len -= (size_t) n;
    }
    return true;
}

static bool recv_all(int sd, char *buf, size_t len)
{
    ssize_t n;

    while (0 < len) {
        n = recv(sd, buf, len, 0);
        if (0 >= n) {
            return false;
        }
        buf += n;
        len -= (size_t) n;
    }
    return true;
}

/* the status word a server sends first, as our exit status - or 250 if
 * none came */
static int status_exit(int sd)
{
    uint32_t u32;

    if (!recv_all(sd, (char *) &u32, sizeof(u32))) {
        return 250;
    }
    return (-(pmix_status_t) ntohl(u32)) & 0xff;
}

static int connect_tcp(const char *uri)
{
    struct sockaddr_in sa;
    const char *p;
    char host[64];
    unsigned port;
    struct timeval tv = {30, 0};
    int sd;

    p = strstr(uri, "tcp4://");
    if (NULL == p || 2 != sscanf(p + strlen("tcp4://"), "%63[^:]:%u", host, &port)) {
        return -1;
    }
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t) port);
    inet_pton(AF_INET, host, &sa.sin_addr);
    sd = socket(AF_INET, SOCK_STREAM, 0);
    if (0 > sd || 0 != connect(sd, (struct sockaddr *) &sa, sizeof(sa))) {
        return -1;
    }
    setsockopt(sd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    return sd;
}

static int connect_usock(const char *path)
{
    struct sockaddr_un sa;
    struct timeval tv = {30, 0};
    int sd;

    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    if (sizeof(sa.sun_path) <= strlen(path)) {
        return -1;
    }
    memcpy(sa.sun_path, path, strlen(path) + 1);
    sd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (0 > sd || 0 != connect(sd, (struct sockaddr *) &sa, sizeof(sa))) {
        return -1;
    }
    setsockopt(sd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    return sd;
}

/* an opening carrying a credential, which none may */
static char *opening_with_cred(const unsigned char *nonce, size_t *out)
{
    pmix_ptl_hdr_t hdr;
    const char *psec = "ssl", *cr = "cred";
    uint8_t flag = PMIX_REMOTE_AUTH;
    size_t csize = sizeof(hdr);
    char *msg;

    memset(&hdr, 0, sizeof(hdr));
    hdr.pindex = -1;
    hdr.tag = UINT32_MAX;
    hdr.nbytes = strlen(psec) + 1 + 4 + 4 + 1 + strlen(PMIX_VERSION) + 1 + 4 +
                 PMIX_PTL_AUTH_NONCE_LEN + 4;
    msg = (char *) calloc(1, sizeof(hdr) + hdr.nbytes);
    memcpy(msg, &hdr, sizeof(hdr));
    PMIX_PTL_PUT_STRING(psec);
    PMIX_PTL_PUT_U32(4);
    PMIX_PTL_PUT_BLOB(cr, 4);
    PMIX_PTL_PUT_U8(flag);
    PMIX_PTL_PUT_STRING(PMIX_VERSION);
    PMIX_PTL_PUT_U32(PMIX_PTL_AUTH_NONCE_LEN);
    PMIX_PTL_PUT_BLOB(nonce, PMIX_PTL_AUTH_NONCE_LEN);
    PMIX_PTL_PUT_U32(geteuid());
    *out = csize;
    return msg;
}

/* a connect-ack from a tool that needs an identity */
static char *connect_ack(const char *psec, const pmix_byte_object_t *cred, const char *version,
                         size_t *out)
{
    pmix_ptl_hdr_t hdr;
    const char *bfrops = pmix_globals.mypeer->nptr->compat.bfrops->version, *gds = "hash";
    uint8_t flag = PMIX_TOOL_NEEDS_ID, bt = (uint8_t) pmix_globals.mypeer->nptr->compat.type;
    size_t csize = sizeof(hdr);
    char *msg;

    memset(&hdr, 0, sizeof(hdr));
    hdr.pindex = -1;
    hdr.tag = UINT32_MAX;
    hdr.nbytes = strlen(psec) + 1 + 4 + cred->size + 1 + 4 + 4 + strlen(version) + 1 +
                 strlen(bfrops) + 1 + 1 + strlen(gds) + 1;
    msg = (char *) calloc(1, sizeof(hdr) + hdr.nbytes);
    memcpy(msg, &hdr, sizeof(hdr));
    PMIX_PTL_PUT_STRING(psec);
    PMIX_PTL_PUT_U32(cred->size);
    PMIX_PTL_PUT_BLOB(cred->bytes, cred->size);
    PMIX_PTL_PUT_U8(flag);
    PMIX_PTL_PUT_U32(geteuid());
    PMIX_PTL_PUT_U32(getegid());
    PMIX_PTL_PUT_STRING(version);
    PMIX_PTL_PUT_STRING(bfrops);
    PMIX_PTL_PUT_U8(bt);
    PMIX_PTL_PUT_STRING(gds);
    *out = csize;
    return msg;
}

/* the server's answer to an opening: its nonce, once the status is a
 * success, and - if `keep` - its credential. Returns the status, or 250 */
static int read_answer_cred(int sd, unsigned char *srvnonce, pmix_byte_object_t *keep)
{
    uint32_t u32;
    char *cred;
    int st;

    st = status_exit(sd);
    if (0 != st) {
        return st;
    }
    if (!recv_all(sd, (char *) srvnonce, PMIX_PTL_AUTH_NONCE_LEN) ||
        !recv_all(sd, (char *) &u32, sizeof(u32))) {
        return 250;
    }
    u32 = ntohl(u32);
    cred = (char *) malloc(u32);
    if (NULL == cred || !recv_all(sd, cred, u32)) {
        free(cred);
        return 250;
    }
    if (NULL != keep) {
        keep->bytes = cred;
        keep->size = u32;
    } else {
        free(cred);
    }
    return 0;
}

static int read_answer(int sd, unsigned char *srvnonce)
{
    return read_answer_cred(sd, srvnonce, NULL);
}

/* one raw peer, in a child: exits with the status the server sent it, or
 * 250 for a failure of its own, or - RAW_QUIET - the seconds from its
 * connect to the server dropping it */
static int raw_peer(raw_t kind, const server_t *srv)
{
    unsigned char tool[PMIX_PTL_AUTH_NONCE_LEN], srvn[PMIX_PTL_AUTH_NONCE_LEN];
    pmix_byte_object_t bind, cred;
    pmix_psec_module_t *mod;
    struct timeval t0, t1;
    pmix_info_t info;
    pmix_proc_t me;
    bool yes = true;
    char *msg, buf[4];
    size_t len;
    int sd, st;

    alarm(60);
    if (RAW_MUNGE_BOUND_AS_PLAIN != kind && RAW_ACK_OTHER_REMOTE_MECH != kind) {
        setenv("PMIX_MCA_psec", "native,ssl", 1);
    }
    use_ssl(cafile, bothcert, keyfile);
    PMIX_INFO_LOAD(&info, PMIX_TOOL_DO_NOT_CONNECT, &yes, PMIX_BOOL);
    if (PMIX_SUCCESS != PMIx_tool_init(&me, &info, 1)) {
        return 250;
    }
    if (RAW_ACK_OTHER_REMOTE_MECH == kind && NULL == pmix_psec_base_assign_module("munge")) {
        return RAW_NO_MUNGE;
    }
    memset(tool, 0x5a, sizeof(tool));
    gettimeofday(&t0, NULL);
    sd = (RAW_OPENING_ON_USOCK == kind) ? connect_usock(srv->usock) : connect_tcp(srv->uri);
    if (0 > sd) {
        return 250;
    }
    if (RAW_OPENING_WITH_CRED == kind) {
        msg = opening_with_cred(tool, &len);
        st = send_all(sd, msg, len) ? status_exit(sd) : 250;
        free(msg);
        return st;
    }
    if (RAW_QUIET == kind) {
        sleep(2);
    }
    if (RAW_MUNGE_BOUND_AS_PLAIN == kind) {
        pmix_info_t *results = NULL;
        size_t nresults = 0;
        pmix_status_t vrc, prc;
        uid_t buid;
        gid_t bgid;

        /* the server's credential, made for this exchange as a server's,
         * must not pass for an ordinary one - not with munge itself, and
         * not through the public call */
        mod = pmix_psec_base_assign_module("munge");
        if (NULL == mod) {
            return RAW_NO_MUNGE;
        }
        PMIX_BYTE_OBJECT_CONSTRUCT(&cred);
        if (PMIX_SUCCESS != pmix_ptl_base_auth_request("munge", tool, false, &msg, &len) ||
            !send_all(sd, msg, len) || 0 != read_answer_cred(sd, srvn, &cred)) {
            return 249;
        }
        free(msg);
        vrc = mod->validate_cred((struct pmix_peer_t *) pmix_globals.mypeer, NULL, 0, &results,
                                 &nresults, &cred);
        if (NULL != results) {
            PMIX_INFO_FREE(results, nresults);
            results = NULL;
            nresults = 0;
        }
        prc = PMIx_Validate_credential(&cred, NULL, 0, &results, &nresults);
        if (NULL != results) {
            PMIX_INFO_FREE(results, nresults);
        }
        PMIX_BYTE_OBJECT_DESTRUCT(&cred);
        if (PMIX_SUCCESS == vrc || PMIX_SUCCESS == prc) {
            return 1;
        }
        /* and munge holds a bound credential to the role it was made as,
         * whatever bytes it carries */
        if (PMIX_SUCCESS != pmix_ptl_base_auth_bind(false, tool, srvn, NULL, &bind) ||
            PMIX_SUCCESS != mod->create_bound(&bind, true, (uid_t) -1, &cred)) {
            return 250;
        }
        vrc = mod->validate_bound(&bind, false, &cred, &buid, &bgid);
        PMIX_BYTE_OBJECT_DESTRUCT(&cred);
        PMIX_BYTE_OBJECT_DESTRUCT(&bind);
        return (PMIX_SUCCESS != vrc) ? 0 : 2;
    }
    if (PMIX_SUCCESS !=
            pmix_ptl_base_auth_request("ssl", tool, RAW_OPENING_BOUND == kind, &msg, &len) ||
        !send_all(sd, msg, len)) {
        return 250;
    }
    if (RAW_OPENING_TWICE == kind) {
        st = read_answer(sd, srvn);
        if (0 != st) {
            free(msg);
            return 249;
        }
        st = send_all(sd, msg, len) ? status_exit(sd) : 250;
        free(msg);
        return st;
    }
    free(msg);
    st = read_answer(sd, srvn);
    if (0 != st || RAW_OPENING_ON_USOCK == kind || RAW_OPENING_BOUND == kind) {
        return (RAW_OPENING_ON_USOCK == kind || RAW_OPENING_BOUND == kind) ? st : 249;
    }
    if (RAW_QUIET == kind) {
        /* the server drops us with a status, then closes */
        st = status_exit(sd);
        gettimeofday(&t1, NULL);
        if ((-(PMIX_ERR_TIMEOUT)) != st || 0 != recv(sd, buf, 1, 0)) {
            return 250;
        }
        return (int) (t1.tv_sec - t0.tv_sec);
    }

    /* the connect-ack, wrong in one respect */
    if (RAW_ACK_OTHER_MECH == kind) {
        /* the server refuses it for naming another mechanism before it
         * reads the credential at all */
        PMIX_BYTE_OBJECT_CONSTRUCT(&cred);
        cred.bytes = strdup("not-a-credential");
        if (NULL == cred.bytes) {
            return 247;
        }
        cred.size = strlen(cred.bytes) + 1;
        msg = connect_ack("native", &cred, PMIX_VERSION, &len);
    } else if (RAW_ACK_OTHER_REMOTE_MECH == kind) {
        /* a good credential for this exchange - from the other mechanism */
        mod = pmix_psec_base_assign_module("munge");
        if (NULL == mod ||
            PMIX_SUCCESS != pmix_ptl_base_auth_bind(false, tool, srvn, NULL, &bind) ||
            PMIX_SUCCESS != mod->create_bound(&bind, false, (uid_t) -1, &cred)) {
            return 250;
        }
        PMIX_BYTE_OBJECT_DESTRUCT(&bind);
        msg = connect_ack("munge", &cred, PMIX_VERSION, &len);
    } else {
        if (RAW_ACK_OTHER_NONCE == kind) {
            srvn[0] ^= 0xff;
        }
        mod = pmix_psec_base_assign_module("ssl");
        if (NULL == mod ||
            PMIX_SUCCESS != pmix_ptl_base_auth_bind(false, tool, srvn, NULL, &bind) ||
            PMIX_SUCCESS != mod->create_bound(&bind, RAW_ACK_AS_SERVER == kind, geteuid(),
                                              &cred)) {
            return 250;
        }
        PMIX_BYTE_OBJECT_DESTRUCT(&bind);
        msg = connect_ack("ssl", &cred, (RAW_ACK_EARLIER_RELEASE == kind) ? "5.0.0" : PMIX_VERSION,
                          &len);
    }
    PMIX_BYTE_OBJECT_DESTRUCT(&cred);
    st = send_all(sd, msg, len) ? status_exit(sd) : 250;
    free(msg);
    return st;
}

static void raw_case(const char *name, raw_t kind, pmix_status_t want)
{
    srvcfg_t cfg = {.remote = true, .tools = true, .legacy = false,
                    .cert = (RAW_MUNGE_BOUND_AS_PLAIN == kind) ? NULL : bothcert,
                    .ack_timeout = (RAW_QUIET == kind) ? 3 : 0,
                    /* so that only where the peer is refuses it */
                    .notable = (RAW_OPENING_BOUND == kind)};
    server_t srv;
    char detail[256];
    pid_t pid;
    int status, got = -1, up;
    bool ok;

    up = start_server(&srv, &cfg);
    if (SKIP == up) {
        fprintf(stdout, "  SKIP: %s (no munged)\n", name);
        return;
    }
    if (0 != up) {
        report(name, 0, "the server did not start");
        return;
    }
    fflush(stdout);
    pid = fork();
    if (0 == pid) {
        _exit(raw_peer(kind, &srv));
    }
    if (0 < pid && pid == waitpid(pid, &status, 0) && WIFEXITED(status)) {
        got = WEXITSTATUS(status);
    }
    stop_server(&srv);
    if (RAW_NO_MUNGE == got) {
        fprintf(stdout, "  SKIP: %s (no munged)\n", name);
        return;
    }
    if (RAW_MUNGE_BOUND_AS_PLAIN == kind) {
        report(name, 0 == got, (2 == got) ? "munge took a server's credential as a tool's"
                                          : "munge, or PMIx_Validate_credential, accepted it");
        return;
    }
    if (RAW_QUIET == kind) {
        /* dropped once 3 seconds have passed from its arrival - not 3 from
         * its opening, which came 2 seconds in */
        ok = (2 <= got && 4 >= got);
        snprintf(detail, sizeof(detail), "dropped after %d seconds (250: not dropped as "
                 "expected)", got);
    } else {
        ok = ((-want) & 0xff) == got;
        snprintf(detail, sizeof(detail), "the server answered %s, not %s (%d; 250: no answer, "
                 "249: the opening was refused, 248/247: no mechanism or credential to send)",
                 (0 <= got && 247 > got) ? PMIx_Error_string(-got) : "nothing",
                 PMIx_Error_string(want), got);
    }
    report(name, ok, detail);
}

/* ---------------------------------------------------------------- */

int main(void)
{
    char dir[512];
    const char *base = getenv("TMPDIR");
    EVP_PKEY *cakey, *ca2key, *key, *ukey;
    X509 *ca, *ca2, *both, *client, *untrusted;
    struct passwd *pw;
    char *pem;
    size_t n;
    tool_t t;
    srvcfg_t remote = {.remote = true, .tools = true, .legacy = false, .cert = NULL,
                       .ack_timeout = 0};
    srvcfg_t cfg;

    fprintf(stdout, "\n=== a tool and a server on different hosts ===\n\n");
#if !PMIX_ENABLE_REMOTE_TOOLS
    fprintf(stdout, "this PMIx takes no tools from other hosts\n");
    return SKIP;
#endif
    pw = getpwuid(geteuid());
    snprintf(dir, sizeof(dir), "%s/pmix-rauth-XXXXXX", (NULL == base) ? "/tmp" : base);
    if (NULL == pw || NULL == mkdtemp(dir)) {
        fprintf(stdout, "no passwd entry or scratch directory - skipping\n");
        return SKIP;
    }
    setenv("TMPDIR", dir, 1);
    setenv("PMIX_MCA_mca_base_param_files", "none", 1);
    signal(SIGPIPE, SIG_IGN);
    alarm(900);

    /* the PKI: a CA, and a second nobody here trusts. Every leaf names
     * this user, who runs both ends */
    cakey = gen_key(EVP_PKEY_EC);
    ca2key = gen_key(EVP_PKEY_EC);
    key = gen_key(EVP_PKEY_EC);
    ukey = gen_key(EVP_PKEY_EC);
    if (NULL == cakey || NULL == ca2key || NULL == key || NULL == ukey) {
        fprintf(stdout, "could not make the keys\n");
        return 1;
    }
    ca = make_cert(cakey, "PMIx test root", NULL, NULL, 1, -60, 86400, NULL);
    ca2 = make_cert(ca2key, "Nobody's root", NULL, NULL, 1, -60, 86400, NULL);
    both = make_cert(key, pw->pw_name, ca, cakey, 10, -60, 86400, "clientAuth,serverAuth");
    client = make_cert(key, pw->pw_name, ca, cakey, 11, -60, 86400, "clientAuth");
    untrusted = make_cert(ukey, pw->pw_name, ca2, ca2key, 12, -60, 86400,
                          "clientAuth,serverAuth");
    snprintf(cafile, sizeof(cafile), "%s/ca.pem", dir);
    pem = chain_pem(ca, NULL, &n);
    write_file(cafile, pem, n, 0644);
    free(pem);
    snprintf(ca2file, sizeof(ca2file), "%s/ca2.pem", dir);
    pem = chain_pem(ca2, NULL, &n);
    write_file(ca2file, pem, n, 0644);
    free(pem);
    snprintf(bothcert, sizeof(bothcert), "%s/both.pem", dir);
    pem = chain_pem(both, NULL, &n);
    write_file(bothcert, pem, n, 0644);
    free(pem);
    snprintf(clientcert, sizeof(clientcert), "%s/client.pem", dir);
    pem = chain_pem(client, NULL, &n);
    write_file(clientcert, pem, n, 0644);
    free(pem);
    snprintf(keyfile, sizeof(keyfile), "%s/me.key", dir);
    write_key(keyfile, key, 0600);
    snprintf(untrustedcert, sizeof(untrustedcert), "%s/untrusted.pem", dir);
    pem = chain_pem(untrusted, NULL, &n);
    write_file(untrustedcert, pem, n, 0644);
    free(pem);
    snprintf(untrustedkey, sizeof(untrustedkey), "%s/untrusted.key", dir);
    write_key(untrustedkey, ukey, 0600);

    /* ---- taken ---- */
    memset(&t, 0, sizeof(t));
    t.ca = cafile;
    t.cert = bothcert;
    t.key = keyfile;
    cfg = remote;
    cfg.cert = bothcert;
    one_case("a tool and a server that authenticate each other connect", &cfg, &t, PMIX_SUCCESS,
             NULL, NULL, dir);
    t.attach = true;
    one_case("...and by an event-driven attach", &cfg, &t, PMIX_SUCCESS, NULL, NULL, dir);
    t.attach = false;
    t.mech = "munge";
    one_case("...and with munge", &remote, &t, PMIX_SUCCESS, NULL, NULL, dir);
    t.attach = true;
    one_case("...and with munge, by an event-driven attach", &remote, &t, PMIX_SUCCESS, NULL, NULL,
             dir);
    t.attach = false;
    t.mech = NULL;

    /* ---- through a port forward: the server says to prove it first ---- */
    t.local = true;
    one_case("a tool that took the server to be on its own host proves it when told to", &cfg,
             &t, PMIX_SUCCESS, NULL, NULL, dir);
    t.attach = true;
    one_case("...and by an event-driven attach", &cfg, &t, PMIX_SUCCESS, NULL, NULL, dir);
    t.attach = false;
    t.mech = "native";
    one_case("...and with no mechanism that can, fails saying so", &cfg, &t,
             PMIX_ERR_NOT_SUPPORTED, "has neither as configured", NULL, dir);
    t.mech = NULL;
    t.local = false;

    /* ---- a server on the tool's own host its kernel cannot name ---- */
    t.local = true;
    t.notable = true;
    cfg.onhost = true;
    one_case("a tool has a server on its own host the kernel cannot name prove who it is", &cfg,
             &t, PMIX_SUCCESS, "proved it is uid", NULL, dir);
    t.attach = true;
    one_case("...and by an event-driven attach", &cfg, &t, PMIX_SUCCESS, "proved it is uid", NULL,
             dir);
    t.attach = false;
    t.ca = ca2file;
    one_case("...and refuses one whose certificate it does not trust", &cfg, &t,
             PMIX_ERR_INVALID_CRED, "did not prove who it is", NULL, dir);
    t.attach = true;
    one_case("...and by an event-driven attach", &cfg, &t, PMIX_ERR_INVALID_CRED,
             "did not prove who it is", NULL, dir);
    t.attach = false;
    t.ca = cafile;
    t.mech = "native";
    one_case("...and with no mechanism that can, fails saying so", &cfg, &t,
             PMIX_ERR_NOT_SUPPORTED, "does not say which user runs", NULL, dir);
    t.attach = true;
    one_case("...and by an event-driven attach", &cfg, &t, PMIX_ERR_NOT_SUPPORTED,
             "does not say which user runs", NULL, dir);
    t.attach = false;
    t.version = PMIX_VERSION;
    one_case("...also when its rendezvous file names this release", &cfg, &t,
             PMIX_ERR_NOT_SUPPORTED, "does not say which user runs", NULL, dir);
    t.version = "6.0.0";
    one_case("...but connects as before to one whose file names an earlier release", &cfg, &t,
             PMIX_SUCCESS, "connecting as before", NULL, dir);
    t.attach = true;
    one_case("...and by an event-driven attach", &cfg, &t, PMIX_SUCCESS, "connecting as before",
             NULL, dir);
    t.attach = false;
    t.foreign = true;
    one_case("...unless that file is not one its server may own", &cfg, &t,
             PMIX_ERR_NOT_SUPPORTED, "does not say which user runs", NULL, dir);
    t.foreign = false;
    t.nopid = true;
    one_case("...as it does to one whose report file names no pid", &cfg, &t, PMIX_SUCCESS,
             "connecting as before", NULL, dir);
    t.nopid = false;
    t.linked = true;
    one_case("...or is reached through a link", &cfg, &t, PMIX_ERR_NOT_SUPPORTED,
             "does not say which user runs", NULL, dir);
    t.linked = false;
    t.version = NULL;
    t.mech = NULL;
    /* where munged runs, munge outranks ssl - but this server has none */
    cfg.psec = "native,ssl";
    t.every = true;
    t.version = PMIX_VERSION;
    t.srvpsec = "native,ssl";
    one_case("...and proves it with a mechanism the server's file lists", &cfg, &t,
             PMIX_SUCCESS, "proved it is uid", NULL, dir);
    t.attach = true;
    one_case("...and by an event-driven attach", &cfg, &t, PMIX_SUCCESS, "proved it is uid", NULL,
             dir);
    t.attach = false;
    t.srvpsec = NULL;
    t.version = NULL;
    t.every = false;
    cfg.psec = NULL;
    /* where this kernel has a table of TCP connections to ask */
    if (tcp_table_answers()) {
        cfg.table = true;
        one_case("...while a server whose kernel names its peers will not prove who it is to "
                 "it", &cfg, &t, PMIX_ERR_NOT_SUPPORTED, "and it did not", NULL, dir);
        cfg.table = false;
    } else {
        fprintf(stdout, "  SKIP: a server whose kernel names its peers (this one does not)\n");
    }
    relay_case("...and refuses one reached through a relay on this host", &cfg, &t,
               PMIX_ERR_INVALID_CRED, "did not prove who it is", dir);
    t.attach = true;
    relay_case("...and by an event-driven attach", &cfg, &t, PMIX_ERR_INVALID_CRED,
               "did not prove who it is", dir);
    t.attach = false;
    t.mech = "native";
    t.singleton = true;
    /* and comes up on its own: PMIx_Init's PMIX_ERR_UNREACH */
    fake_case("...as does a singleton, which finds it in a file - and sends nothing without "
              "a mechanism that can", FAKE_COUNT_BYTES, &t, PMIX_ERR_UNREACH,
              "does not say which user runs", dir);
    /* a file at a fixed name in a directory anyone writes to is no
     * caller's word for the server's release */
    t.version = "6.0.0";
    fake_case("...also when that file says an earlier release", FAKE_COUNT_BYTES, &t,
              PMIX_ERR_UNREACH, "does not say which user runs", dir);
    t.version = NULL;
    t.singleton = false;
    t.mech = NULL;
    fake_case("...and reports one that closes on the opening, naming the rendezvous file",
              FAKE_CLOSE_ON_OPENING, &t, PMIX_ERR_UNREACH, "rendezvous file", dir);
#if PMIX_ENABLE_LEGACY_PEERS
    cfg.remote = false;
    cfg.legacy = true;
    one_case("...and a server that takes no tools from other hosts proves who it is to it",
             &cfg, &t, PMIX_SUCCESS, "proved it is uid", NULL, dir);
    cfg.tools = false;
    one_case("...while one that takes no tools refuses, and is reported", &cfg, &t,
             PMIX_ERR_NOT_SUPPORTED, "rendezvous file", NULL, dir);
    cfg.tools = true;
    cfg.remote = true;
    cfg.legacy = false;
#endif
    t.notable = false;
    t.local = false;
    relay_case("a tool on another host reaches a server through a proxy on the server's host",
               &cfg, &t, PMIX_SUCCESS, NULL, dir);
    cfg.onhost = false;

    /* ---- refused before anything is sent ---- */
    t.mech = "native";
    fake_case("a tool with no mechanism that authenticates across hosts sends nothing",
              FAKE_COUNT_BYTES, &t, PMIX_ERR_NOT_SUPPORTED, "has neither as configured", dir);
    t.attach = true;
    fake_case("...and by an event-driven attach", FAKE_COUNT_BYTES, &t, PMIX_ERR_NOT_SUPPORTED,
              "has neither as configured", dir);
    t.attach = false;
    t.mech = NULL;
    fake_case("a server that closes on the opening, as an earlier release does, is reported",
              FAKE_CLOSE_ON_OPENING, &t, PMIX_ERR_UNREACH, "and it did not", dir);
    t.attach = true;
    fake_case("...and by an event-driven attach", FAKE_CLOSE_ON_OPENING, &t, PMIX_ERR_UNREACH,
              "and it did not", dir);
    t.attach = false;

    /* ---- the server will not prove who it is ---- */
    cfg.tools = false;
    one_case("a server that takes no tools refuses to prove who it is", &cfg, &t,
             PMIX_ERR_NOT_SUPPORTED, "and it did not", NULL, dir);
    cfg.tools = true;
#if PMIX_ENABLE_LEGACY_PEERS
    cfg.remote = false;
    cfg.legacy = true;
    one_case("a server that takes no tools from other hosts refuses to prove who it is", &cfg,
             &t, PMIX_ERR_NOT_SUPPORTED, "and it did not", NULL, dir);
    t.local = true;
    one_case("...and refuses a tool from another host that did not ask it to", &cfg, &t,
             PMIX_ERR_NO_PERMISSIONS, NULL, NULL, dir);
    t.local = false;
    cfg.remote = true;
    cfg.legacy = false;
#endif

    /* ---- the tool refuses the server ---- */
    t.ca = ca2file;
    one_case("a tool refuses a server whose certificate it does not trust", &cfg, &t,
             PMIX_ERR_INVALID_CRED, "did not prove who it is", NULL, dir);
    t.attach = true;
    one_case("...and by an event-driven attach", &cfg, &t, PMIX_ERR_INVALID_CRED,
             "did not prove who it is", NULL, dir);
    t.attach = false;
    t.ca = cafile;
    cfg.cert = clientcert;
    one_case("a tool refuses a server whose certificate is for clients only", &cfg, &t,
             PMIX_ERR_INVALID_CRED, "did not prove who it is", NULL, dir);
    cfg.cert = bothcert;

    /* ---- the server refuses the tool ---- */
    t.cert = untrustedcert;
    t.key = untrustedkey;
    one_case("a server refuses a tool whose certificate it does not trust", &cfg, &t,
             PMIX_ERR_INVALID_CRED, NULL, "did not prove who it is", dir);
    t.cert = bothcert;
    t.key = keyfile;

    /* ---- the wire, spoken directly ---- */
    raw_case("an opening sent twice is refused", RAW_OPENING_TWICE, PMIX_ERR_BAD_PARAM);
    raw_case("an opening carrying a credential is refused", RAW_OPENING_WITH_CRED,
             PMIX_ERR_BAD_PARAM);
    raw_case("an opening that binds the connection, from another host, is refused",
             RAW_OPENING_BOUND, PMIX_ERR_NOT_SUPPORTED);
    raw_case("an opening over the local socket is refused", RAW_OPENING_ON_USOCK,
             PMIX_ERR_NOT_SUPPORTED);
    raw_case("a connect-ack naming another mechanism than its opening is refused",
             RAW_ACK_OTHER_MECH, PMIX_ERR_NO_PERMISSIONS);
    raw_case("...and one naming another that can authenticate across hosts",
             RAW_ACK_OTHER_REMOTE_MECH, PMIX_ERR_NO_PERMISSIONS);
    raw_case("a connect-ack bound to another connection is refused", RAW_ACK_OTHER_NONCE,
             PMIX_ERR_INVALID_CRED);
    raw_case("a connect-ack whose credential was made as a server's is refused",
             RAW_ACK_AS_SERVER, PMIX_ERR_INVALID_CRED);
    raw_case("a connect-ack naming an earlier release after the exchange is refused",
             RAW_ACK_EARLIER_RELEASE, PMIX_ERR_BAD_PARAM);
    raw_case("a peer quiet after the proof is dropped on its original deadline", RAW_QUIET,
             PMIX_ERR_TIMEOUT);
    raw_case("a server's munge credential is no ordinary credential, nor a tool's",
             RAW_MUNGE_BOUND_AS_PLAIN,
             PMIX_SUCCESS);

    EVP_PKEY_free(cakey);
    EVP_PKEY_free(ca2key);
    EVP_PKEY_free(key);
    EVP_PKEY_free(ukey);
    X509_free(ca);
    X509_free(ca2);
    X509_free(both);
    X509_free(client);
    X509_free(untrusted);
    unlink(cafile);
    unlink(ca2file);
    unlink(bothcert);
    unlink(clientcert);
    unlink(keyfile);
    unlink(untrustedcert);
    unlink(untrustedkey);
    (void) rmdir(dir);
    fprintf(stdout, "\n%d passed, %d failed\n", npass, nfail);
    return (0 == nfail) ? 0 : 1;
}
