/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * A tool and a TCP server on its own host whose kernel keeps no table of
 * TCP connections (pmix_util_getid_test_no_tcp_table), so nothing names
 * the server's user, nor the tool's. The server takes legacy peers, so it
 * takes the tool on its claim. The tool connects - by PMIx_tool_init, and
 * by an event-driven PMIx_tool_attach_to_server - only when the file it
 * was given for the server vouches for it:
 *
 *   - its bare TCP URI: refused, saying why
 *   - a file naming this release, with the lock beside it held: taken
 *   - ...with nothing holding that lock: refused
 *   - ...or one that names no lock: refused
 *   - a file naming an earlier release: taken
 *   - a file its group may write, or one reached through a link: refused
 *
 * and a server at an address that is not this host's is not dialled at
 * all: PMIX_ERR_UNREACH, saying so.
 *
 * Each server and tool runs in a child of its own.
 */

#include "src/include/pmix_config.h"

#include "include/pmix.h"
#include "include/pmix_server.h"
#include "include/pmix_tool.h"
#include "src/include/pmix_globals.h"
#include "src/mca/ptl/base/base.h"
#include "src/util/pmix_getid.h"

#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

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

    PMIX_LOAD_PROCID(&proc, "unnamed.tool", ntools++);
    cbfunc(PMIX_SUCCESS, &proc, cbdata);
}

static pmix_server_module_t mymodule = {
    .tool_connected = tool_connected
};

/* ---- the server: a TCP listener for legacy peers, taking peers the
 * kernel cannot name on their claim ---- */

typedef struct {
    pid_t pid;
    int go;
    char uri[2048];
} server_t;

static void run_server(int urifd, int gofd)
{
    pmix_info_t info[2];
    bool yes = true;
    char c;

    pmix_util_getid_test_no_tcp_table = true;
    PMIX_INFO_LOAD(&info[0], PMIX_SERVER_TOOL_SUPPORT, &yes, PMIX_BOOL);
    PMIX_INFO_LOAD(&info[1], PMIX_SERVER_LEGACY_PEERS, &yes, PMIX_BOOL);
    if (PMIX_SUCCESS != PMIx_server_init(&mymodule, info, 2) ||
        NULL == pmix_ptl_base.listener.uri ||
        NULL == strstr(pmix_ptl_base.listener.uri, "tcp")) {
        _exit(2);
    }
    if ((ssize_t) strlen(pmix_ptl_base.listener.uri) !=
        write(urifd, pmix_ptl_base.listener.uri, strlen(pmix_ptl_base.listener.uri))) {
        _exit(2);
    }
    close(urifd);
    if (0 > read(gofd, &c, 1)) {
        _exit(2);
    }
    PMIx_server_finalize();
    _exit(0);
}

static void stop_server(server_t *srv)
{
    if (0 <= srv->go) {
        if (1 != write(srv->go, "x", 1) && 0 < srv->pid) {
            kill(srv->pid, SIGKILL);
        }
        close(srv->go);
        srv->go = -1;
    }
    if (0 < srv->pid) {
        (void) waitpid(srv->pid, NULL, 0);
        srv->pid = -1;
    }
}

static int start_server(server_t *srv)
{
    int urip[2], gop[2];
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
        run_server(urip[1], gop[0]);
    }
    close(urip[1]);
    close(gop[0]);
    srv->go = gop[1];
    if (0 > srv->pid) {
        close(urip[0]);
        stop_server(srv);
        return 1;
    }
    while (got < sizeof(srv->uri) - 1 &&
           0 < (n = read(urip[0], srv->uri + got, sizeof(srv->uri) - 1 - got))) {
        got += (size_t) n;
    }
    close(urip[0]);
    srv->uri[got] = '\0';
    if (0 == got) {
        stop_server(srv);
        return 1;
    }
    return 0;
}

/* ---- a real tool: it exits with its connect's status, negated ---- */

static pmix_status_t run_tool(const char *target, bool attach, const char *errpath)
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
        setenv("PMIX_MCA_psec", "native", 1);
        setenv("PMIX_MCA_ptl_base_handshake_wait_time", "10", 1);
        pmix_util_getid_test_no_tcp_table = true;
        if (attach) {
            PMIX_INFO_LOAD(&info, PMIX_TOOL_DO_NOT_CONNECT, &flag, PMIX_BOOL);
            rc = PMIx_tool_init(&me, &info, 1);
            PMIX_INFO_DESTRUCT(&info);
            if (PMIX_SUCCESS == rc) {
                PMIX_INFO_LOAD(&info, PMIX_SERVER_URI, target, PMIX_STRING);
                rc = PMIx_tool_attach_to_server(&me, &server, &info, 1);
                PMIX_INFO_DESTRUCT(&info);
                PMIx_tool_finalize();
            }
        } else {
            PMIX_INFO_LOAD(&info, PMIX_SERVER_URI, target, PMIX_STRING);
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

/* ---- the cases ---- */

typedef struct {
    const char *version; // the release the file names; NULL: the bare URI
    const char *tail;    // what follows that line
    mode_t mode;
    bool held;           // something holds the lock beside the file
    bool linked;         // the tool is given a link to the file
    bool offhost;        // the URI names an address that is not this host's
} case_t;

static void one_case(const char *name, const case_t *c, bool attach, pmix_status_t want,
                     const char *says, const char *dir)
{
    server_t srv;
    char errpath[1024], file[1024], link[1024], target[4096], body[4096], detail[512];
    char *lname = NULL, *port;
    pmix_status_t rc;
    int lfd = -1;
    bool ok;
    FILE *fp;

    if (0 != start_server(&srv)) {
        report(name, 0, "the server did not start");
        return;
    }
    snprintf(errpath, sizeof(errpath), "%s/tool.err", dir);
    snprintf(file, sizeof(file), "%s/server.uri", dir);
    snprintf(link, sizeof(link), "%s/server.link", dir);
    if (c->offhost) {
        /* the server's own port, at an address no host holds */
        port = strrchr(srv.uri, ':');
        snprintf(target, sizeof(target), "unnamed.0;tcp4://192.0.2.1%s",
                 (NULL == port) ? ":1" : port);
    } else if (NULL == c->version) {
        snprintf(target, sizeof(target), "%s", srv.uri);
    } else {
        snprintf(body, sizeof(body), "%s\n%s\n%s", srv.uri, c->version, c->tail);
        fp = fopen(file, "w");
        if (NULL == fp) {
            stop_server(&srv);
            report(name, 0, "could not write the file");
            return;
        }
        fputs(body, fp);
        fclose(fp);
        chmod(file, c->mode);
        lname = pmix_ptl_base_lock_name(file);
        if (c->held && NULL != lname) {
            /* as the server that wrote the file holds it while it runs */
            lfd = open(lname, O_RDWR | O_CREAT, 0600);
            if (0 > lfd || 0 != flock(lfd, LOCK_EX | LOCK_NB)) {
                stop_server(&srv);
                report(name, 0, "could not hold the lock");
                return;
            }
        }
        if (c->linked && 0 != symlink(file, link)) {
            stop_server(&srv);
            report(name, 0, "could not link to the file");
            return;
        }
        snprintf(target, sizeof(target), "file:%s", c->linked ? link : file);
    }
    rc = run_tool(target, attach, errpath);
    stop_server(&srv);
    if (0 <= lfd) {
        close(lfd);
    }
    if (NULL != lname) {
        unlink(lname);
        free(lname);
    }
    unlink(file);
    unlink(link);
    ok = (want == rc) && (NULL == says || said(errpath, says));
    snprintf(detail, sizeof(detail), "the tool's connect returned %s, not %s; said \"%s\": %s",
             PMIx_Error_string(rc), PMIx_Error_string(want), (NULL == says) ? "" : says,
             (NULL != says && said(errpath, says)) ? "yes" : "no");
    report(name, ok, detail);
    unlink(errpath);
}

static void both(const char *name, const case_t *c, pmix_status_t want, const char *says,
                 const char *dir)
{
    char aname[512];

    one_case(name, c, false, want, says, dir);
    snprintf(aname, sizeof(aname), "...and by an event-driven attach");
    one_case(aname, c, true, want, says, dir);
}

int main(void)
{
    char dir[512], cur[32], earlier[32];
    const char *base = getenv("TMPDIR");
    const char *unnamed = "keeps no table of TCP connections";
    case_t c;

    fprintf(stdout, "\n=== a tool and a TCP server the kernel cannot name ===\n\n");
#if !PMIX_ENABLE_LEGACY_PEERS
    fprintf(stdout, "this PMIx takes no clients or tools of earlier releases, so a "
                    "server has no TCP listener\n");
    return SKIP;
#endif
    snprintf(dir, sizeof(dir), "%s/pmix-unnamed-XXXXXX", (NULL == base) ? "/tmp" : base);
    if (NULL == mkdtemp(dir)) {
        fprintf(stdout, "no scratch directory - skipping\n");
        return SKIP;
    }
    setenv("TMPDIR", dir, 1);
    setenv("PMIX_MCA_mca_base_param_files", "none", 1);
    signal(SIGPIPE, SIG_IGN);
    alarm(600);
    snprintf(cur, sizeof(cur), "v%d.0.0", PMIX_PTL_LOCAL_MAJOR);
    snprintf(earlier, sizeof(earlier), "v%d.0.0", PMIX_PTL_LOCAL_MAJOR - 2);

    memset(&c, 0, sizeof(c));
    c.mode = 0644;
    both("a tool given a server's bare TCP URI is refused", &c, PMIX_ERR_INVALID_CRED, unnamed,
         dir);

    c.version = cur;
    c.tail = PMIX_PTL_LOCK_TAG PMIX_PTL_LOCK_FLOCK "\n";
    c.held = true;
    both("a file of this release whose server is running is taken", &c,
         PMIX_SUCCESS, NULL, dir);
    c.held = false;
    both("...but not one whose lock nothing holds", &c, PMIX_ERR_INVALID_CRED, unnamed, dir);
    c.held = true;
    c.mode = 0664;
    one_case("...nor one its group may write", &c, false, PMIX_ERR_INVALID_CRED, unnamed, dir);
    c.mode = 0644;
    c.linked = true;
    one_case("...nor one reached through a link", &c, false, PMIX_ERR_INVALID_CRED, unnamed, dir);
    c.linked = false;
    c.tail = "";
    one_case("...nor one that names no lock", &c, false, PMIX_ERR_INVALID_CRED, unnamed, dir);

    c.version = earlier;
    c.tail = "";
    c.held = false;
    both("a file naming an earlier release is taken", &c, PMIX_SUCCESS, NULL, dir);

    memset(&c, 0, sizeof(c));
    c.offhost = true;
    both("a server at an address that is not this host's is not dialled", &c, PMIX_ERR_UNREACH,
         "not this host's", dir);

    rmdir(dir);
    fprintf(stdout, "\n%d passed, %d failed\n", npass, nfail);
    return (0 == nfail) ? 0 : 1;
}
