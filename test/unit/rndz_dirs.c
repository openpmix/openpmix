/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Where a server writes the rendezvous files tools find it by, and where a
 * tool looks for them - see src/mca/ptl/base/ptl_base_rndz.c.
 *
 *   - a server's tool files go in <system tmpdir>/pmix-user.<uid>, made
 *     mode 0700, and none in its session tmpdir
 *   - a pmix-user directory that is not the server's - a link, or one
 *     others can write - is reported; the server starts, and writes no
 *     tool files there
 *   - a non-root system server's file goes in its pmix-user directory, or
 *     in the system tmpdir when the host names one
 *   - a tool finds a server by its pid in the server's user's directory;
 *     a file elsewhere below the system tmpdir, where a server of an
 *     earlier release writes, is found only with legacy peers on
 *
 * Each server and tool runs in a child of its own.
 */

#include "src/include/pmix_config.h"

#include "include/pmix.h"
#include "include/pmix_server.h"
#include "include/pmix_tool.h"
#include "src/include/pmix_globals.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define SKIP 77

static int npass = 0;
static int nfail = 0;
static char base[512];
static int ntools = 0;

static void tool_connected_fn(pmix_info_t *info, size_t ninfo,
                              pmix_tool_connection_cbfunc_t cbfunc, void *cbdata)
{
    pmix_proc_t proc;
    char ns[64];

    (void) info;
    (void) ninfo;
    snprintf(ns, sizeof(ns), "rndzdirs.tool.%d", ntools++);
    PMIX_LOAD_PROCID(&proc, ns, 0);
    cbfunc(PMIX_SUCCESS, &proc, cbdata);
}

static pmix_server_module_t mymodule = {
    .tool_connected = tool_connected_fn
};

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

static bool is_file(const char *path)
{
    struct stat st;

    return 0 == lstat(path, &st) && S_ISREG(st.st_mode);
}

/* the path of a regular file in dir whose name starts with prefix and
 * ends with suffix - the host name between is the server's to choose -
 * in path, or "" */
static bool find_file(const char *dir, const char *prefix, const char *suffix, char *path,
                      size_t len)
{
    struct dirent *ep;
    size_t pl = strlen(prefix), sl = strlen(suffix), nl;
    DIR *dp = opendir(dir);
    bool found = false;

    path[0] = '\0';
    if (NULL == dp) {
        return false;
    }
    while (!found && NULL != (ep = readdir(dp))) {
        nl = strlen(ep->d_name);
        if (nl >= pl + sl && 0 == strncmp(ep->d_name, prefix, pl) &&
            0 == strcmp(ep->d_name + nl - sl, suffix)) {
            snprintf(path, len, "%s/%s", dir, ep->d_name);
            found = is_file(path);
        }
    }
    closedir(dp);
    return found;
}

static mode_t mode_of(const char *path)
{
    struct stat st;

    return (0 == lstat(path, &st)) ? (st.st_mode & 07777) : 0;
}

/* A server in a child: tool support, the system tmpdir `sys` (or the
 * default, $TMPDIR, when `name_sys` is false), the session tmpdir `sess`,
 * and the system server role with `system`. It says over `ready` whether
 * its init succeeded, and stays up until `go` is written or closed */
static pid_t start_server(const char *sys, bool name_sys, const char *sess, bool system,
                          int *readyfd, int *gofd)
{
    int rp[2], gp[2];
    pid_t pid;
    char c;

    if (0 != pipe(rp) || 0 != pipe(gp)) {
        return -1;
    }
    pid = fork();
    if (0 == pid) {
        pmix_info_t info[4];
        bool flag = true;
        size_t n = 0;
        pmix_status_t rc;

        close(rp[0]);
        close(gp[1]);
        setenv("TMPDIR", sys, 1);
        PMIX_INFO_LOAD(&info[n++], PMIX_SERVER_TOOL_SUPPORT, &flag, PMIX_BOOL);
        PMIX_INFO_LOAD(&info[n++], PMIX_SERVER_TMPDIR, sess, PMIX_STRING);
        if (name_sys) {
            PMIX_INFO_LOAD(&info[n++], PMIX_SYSTEM_TMPDIR, sys, PMIX_STRING);
        }
        if (system) {
            PMIX_INFO_LOAD(&info[n++], PMIX_SERVER_SYSTEM_SUPPORT, &flag, PMIX_BOOL);
        }
        rc = PMIx_server_init(&mymodule, info, n);
        c = (PMIX_SUCCESS == rc) ? 'y' : 'n';
        if (1 != write(rp[1], &c, 1)) {
            _exit(1);
        }
        if (0 > read(gp[0], &c, 1)) {
            /* told to go either way */
        }
        if (PMIX_SUCCESS == rc) {
            PMIx_server_finalize();
        }
        _exit(0);
    }
    close(rp[1]);
    close(gp[0]);
    *readyfd = rp[0];
    *gofd = gp[1];
    return pid;
}

static bool server_up(int readyfd)
{
    char c = 'n';

    return 1 == read(readyfd, &c, 1) && 'y' == c;
}

static void stop_server(pid_t pid, int readyfd, int gofd)
{
    int status;

    close(gofd);
    close(readyfd);
    waitpid(pid, &status, 0);
}

/* a tool in a child that asks for the server with this pid - returns its
 * init status */
static pmix_status_t tool_by_pid(const char *sys, pid_t spid, bool legacy)
{
    int fds[2], status;
    pmix_status_t rc = PMIX_ERROR;
    pid_t pid;

    if (0 != pipe(fds)) {
        return rc;
    }
    pid = fork();
    if (0 == pid) {
        pmix_info_t info[2];
        pmix_proc_t me;

        close(fds[0]);
        setenv("TMPDIR", sys, 1);
        setenv("PMIX_MCA_ptl_base_legacy_peers", legacy ? "1" : "0", 1);
        PMIX_INFO_LOAD(&info[0], PMIX_SERVER_PIDINFO, &spid, PMIX_PID);
        PMIX_INFO_LOAD(&info[1], PMIX_TOOL_CONNECT_OPTIONAL, NULL, PMIX_BOOL);
        rc = PMIx_tool_init(&me, info, 1);
        if ((ssize_t) sizeof(rc) != write(fds[1], &rc, sizeof(rc))) {
            _exit(1);
        }
        if (PMIX_SUCCESS == rc) {
            PMIx_tool_finalize();
        }
        _exit(0);
    }
    close(fds[1]);
    if ((ssize_t) sizeof(rc) != read(fds[0], &rc, sizeof(rc))) {
        rc = PMIX_ERROR;
    }
    close(fds[0]);
    waitpid(pid, &status, 0);
    return rc;
}

static void scratch(char *path, size_t len, const char *name)
{
    snprintf(path, len, "%s/%s", base, name);
    mkdir(path, 0700);
}

static void test_tool_files(void)
{
    char sys[700], sess[700], udir[800], pidfile[1024], detail[1200], suffix[64];
    pid_t spid;
    int ready, go;
    bool found;

    scratch(sys, sizeof(sys), "sys1");
    scratch(sess, sizeof(sess), "sess1");
    snprintf(udir, sizeof(udir), "%s/pmix-user.%lu", sys, (unsigned long) geteuid());
    spid = start_server(sys, false, sess, false, &ready, &go);
    if (0 > spid || !server_up(ready)) {
        report("tool files: the server starts", 0, "init failed");
        return;
    }
    snprintf(suffix, sizeof(suffix), ".tool.%lu", (unsigned long) spid);
    found = find_file(udir, "pmix.", suffix, pidfile, sizeof(pidfile));
    snprintf(detail, sizeof(detail), "%s: file %s, directory mode %04o", udir,
             found ? "there" : "missing", (unsigned) mode_of(udir));
    report("a server's tool files go in its pmix-user directory, made 0700",
           found && 0700 == mode_of(udir), detail);
    report("...and none in its session tmpdir",
           !find_file(sess, "pmix.", suffix, pidfile, sizeof(pidfile)), "one is there");

    /* a tool finds it there by its pid */
    report("a tool finds a server by its pid in that directory",
           PMIX_SUCCESS == tool_by_pid(sys, spid, false), "not found");
    stop_server(spid, ready, go);
}

static void test_not_ours(void)
{
    char sys[700], sess[700], udir[800], elsewhere[700], pidfile[1024], suffix[64];
    pid_t spid;
    int ready, go;
    bool up;

    /* a link at the name, to a directory of ours elsewhere */
    scratch(sys, sizeof(sys), "sys2");
    scratch(sess, sizeof(sess), "sess2");
    scratch(elsewhere, sizeof(elsewhere), "elsewhere");
    snprintf(udir, sizeof(udir), "%s/pmix-user.%lu", sys, (unsigned long) geteuid());
    if (0 != symlink(elsewhere, udir)) {
        report("not ours: fixture", 0, strerror(errno));
        return;
    }
    spid = start_server(sys, false, sess, false, &ready, &go);
    up = (0 < spid && server_up(ready));
    snprintf(suffix, sizeof(suffix), ".tool.%lu", (unsigned long) spid);
    report("a link at the pmix-user name: the server starts, writing no tool files through it",
           up && !find_file(elsewhere, "pmix.", suffix, pidfile, sizeof(pidfile)),
           up ? "a file went through the link" : "init failed");
    stop_server(spid, ready, go);
    unlink(udir);

    /* a directory others may write */
    if (0 != mkdir(udir, 0700) || 0 != chmod(udir, 0777)) {
        report("not ours: fixture", 0, strerror(errno));
        return;
    }
    spid = start_server(sys, false, sess, false, &ready, &go);
    up = (0 < spid && server_up(ready));
    snprintf(suffix, sizeof(suffix), ".tool.%lu", (unsigned long) spid);
    report("a pmix-user directory others may write: the server starts, writing no tool files "
           "in it",
           up && !find_file(udir, "pmix.", suffix, pidfile, sizeof(pidfile)),
           up ? "a file was written there" : "init failed");
    stop_server(spid, ready, go);
    rmdir(udir);
}

static void test_system_files(void)
{
    char sys[700], sess[700], path[1024], dir[800];
    pid_t spid;
    int ready, go;

    if (0 == geteuid()) {
        fprintf(stdout, "  SKIP: a non-root system server's file - running as root\n");
    } else {
        scratch(sys, sizeof(sys), "sys3");
        scratch(sess, sizeof(sess), "sess3");
        spid = start_server(sys, false, sess, true, &ready, &go);
        if (0 > spid || !server_up(ready)) {
            report("system file: the server starts", 0, "init failed");
            return;
        }
        snprintf(dir, sizeof(dir), "%s/pmix-user.%lu", sys, (unsigned long) geteuid());
        report("a non-root system server's file goes in its pmix-user directory",
               find_file(dir, "pmix.sys.", "", path, sizeof(path)), dir);
        stop_server(spid, ready, go);
    }

    scratch(sys, sizeof(sys), "sys4");
    scratch(sess, sizeof(sess), "sess4");
    spid = start_server(sys, true, sess, true, &ready, &go);
    if (0 > spid || !server_up(ready)) {
        report("system file, named tmpdir: the server starts", 0, "init failed");
        return;
    }
    report("...and in the system tmpdir the host names, when it names one",
           find_file(sys, "pmix.sys.", "", path, sizeof(path)), sys);
    stop_server(spid, ready, go);
}

static void test_legacy_walk(void)
{
    char sys[700], sess[700], old[800], from[1100], to[1100], dir[800], suffix[64];
    pid_t spid;
    int ready, go;

    scratch(sys, sizeof(sys), "sys5");
    scratch(sess, sizeof(sess), "sess5");
    spid = start_server(sys, false, sess, false, &ready, &go);
    if (0 > spid || !server_up(ready)) {
        report("legacy walk: the server starts", 0, "init failed");
        return;
    }
    /* its file moved to where a server of an earlier release would have
     * left it: below the system tmpdir, outside any pmix-user directory */
    snprintf(old, sizeof(old), "%s/olddir", sys);
    mkdir(old, 0700);
    snprintf(dir, sizeof(dir), "%s/pmix-user.%lu", sys, (unsigned long) geteuid());
    snprintf(suffix, sizeof(suffix), ".tool.%lu", (unsigned long) spid);
    if (!find_file(dir, "pmix.", suffix, from, sizeof(from))) {
        report("legacy walk: fixture", 0, "the server wrote no file by pid");
        stop_server(spid, ready, go);
        return;
    }
    snprintf(to, sizeof(to), "%s/%s", old, strrchr(from, '/') + 1);
    if (0 != rename(from, to)) {
        report("legacy walk: fixture", 0, strerror(errno));
        stop_server(spid, ready, go);
        return;
    }
    report("a server's file outside the pmix-user directories is not found without legacy peers",
           PMIX_SUCCESS != tool_by_pid(sys, spid, false), "found");
#if PMIX_ENABLE_LEGACY_PEERS
    report("...and is found with them", PMIX_SUCCESS == tool_by_pid(sys, spid, true),
           "not found");
#else
    fprintf(stdout, "  SKIP: ...and is found with them - no legacy peer support built\n");
#endif
    /* back, so the server removes it */
    (void) rename(to, from);
    stop_server(spid, ready, go);
}

int main(void)
{
    const char *tmp = getenv("TMPDIR");
    char cmd[600];

    fprintf(stdout, "\n=== where rendezvous files are written and looked for ===\n\n");
    snprintf(base, sizeof(base), "%s/rndzdirs.XXXXXX", (NULL == tmp) ? "/tmp" : tmp);
    if (NULL == mkdtemp(base)) {
        fprintf(stdout, "no scratch directory - skipping\n");
        return SKIP;
    }
    /* a socket's path must fit: a long TMPDIR leaves no room for ours */
    if (60 < strlen(base)) {
        fprintf(stdout, "the scratch directory's path is too long for a socket - skipping\n");
        rmdir(base);
        return SKIP;
    }
    setenv("PMIX_MCA_mca_base_param_files", "none", 1);
    signal(SIGPIPE, SIG_IGN);
    alarm(300);

    test_tool_files();
    test_not_ours();
    test_system_files();
    test_legacy_walk();

    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", base);
    if (0 != system(cmd)) {
        fprintf(stderr, "could not remove %s\n", base);
    }
    fprintf(stdout, "\n%d passed, %d failed\n", npass, nfail);
    return (0 == nfail) ? 0 : 1;
}
