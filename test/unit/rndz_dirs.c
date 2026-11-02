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
 *   - a file a tool finds is believed only of its writer: one renamed for
 *     another pid leads to a server the kernel says is not that process,
 *     which is refused; one others may write is passed over; and, as
 *     root, one owned by another user leads to a server the kernel says
 *     is not that user, refused, and a system server's file owned by a
 *     user the tool does not list is not taken
 *   - as root, a file of another user's at a server's name is left alone:
 *     the server starts, writes no file there, and does not touch it
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

/* a tool in a child that asks for the system server, the system tmpdir
 * named - returns its init status */
static pmix_status_t tool_to_system(const char *sys)
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
        bool flag = true;

        close(fds[0]);
        setenv("TMPDIR", sys, 1);
        setenv("PMIX_SYSTEM_TMPDIR", sys, 1);
        PMIX_INFO_LOAD(&info[0], PMIX_CONNECT_TO_SYSTEM, &flag, PMIX_BOOL);
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
        struct stat st;

        /* root, with no system tmpdir named: /run/pmix, where /run is root's */
        if (0 != stat("/run", &st) || !S_ISDIR(st.st_mode) || 0 != st.st_uid) {
            fprintf(stdout, "  SKIP: a root system server's file - no /run of root's here\n");
        } else {
            scratch(sys, sizeof(sys), "sys3");
            scratch(sess, sizeof(sess), "sess3");
            spid = start_server(sys, false, sess, true, &ready, &go);
            if (0 > spid || !server_up(ready)) {
                report("root system file: the server starts", 0, "init failed");
                return;
            }
            report("a root system server's file goes in /run/pmix, made 0755",
                   find_file("/run/pmix", "pmix.sys.", "", path, sizeof(path)) &&
                       0755 == mode_of("/run/pmix"),
                   "not there");
            stop_server(spid, ready, go);
        }
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

/* the server's file by pid, renamed for `newpid`, with `mode` and owner
 * `owner` ((uid_t) -1: as it is); returns the new path in to */
static bool move_pidfile(const char *sys, pid_t spid, pid_t newpid, mode_t mode, uid_t owner,
                         char *to, size_t len)
{
    char dir[800], from[1100], suffix[64], *host, *end;

    snprintf(dir, sizeof(dir), "%s/pmix-user.%lu", sys, (unsigned long) geteuid());
    snprintf(suffix, sizeof(suffix), ".tool.%lu", (unsigned long) spid);
    if (!find_file(dir, "pmix.", suffix, from, sizeof(from))) {
        return false;
    }
    /* pmix.<host>.tool.<pid> - keep the host */
    host = strstr(from, "/pmix.") + strlen("/pmix.");
    end = strstr(host, ".tool.");
    snprintf(to, len, "%s/pmix.%.*s.tool.%lu", dir, (int) (end - host), host,
             (unsigned long) newpid);
    if (0 != rename(from, to) || 0 != chmod(to, mode)) {
        return false;
    }
    return (uid_t) -1 == owner || 0 == chown(to, owner, (gid_t) -1);
}

/* the file at `from` back at the name for `spid`, with `mode` */
static bool move_pidfile_back(const char *from, const char *sys, pid_t spid, mode_t mode)
{
    char dir[800], to[1100];
    const char *host, *end;

    snprintf(dir, sizeof(dir), "%s/pmix-user.%lu", sys, (unsigned long) geteuid());
    host = strstr(from, "/pmix.") + strlen("/pmix.");
    end = strstr(host, ".tool.");
    snprintf(to, sizeof(to), "%s/pmix.%.*s.tool.%lu", dir, (int) (end - host), host,
             (unsigned long) spid);
    return 0 == rename(from, to) && 0 == chmod(to, mode);
}

static void test_claims(void)
{
    char sys[700], sess[700], to[1100], path[1024];
    pid_t spid, other = 999999;
    int ready, go;

    scratch(sys, sizeof(sys), "sys6");
    scratch(sess, sizeof(sess), "sess6");
    spid = start_server(sys, false, sess, false, &ready, &go);
    if (0 > spid || !server_up(ready)) {
        report("claims: the server starts", 0, "init failed");
        return;
    }
    /* its file, now named for a process it is not */
    if (!move_pidfile(sys, spid, other, 0644, (uid_t) -1, to, sizeof(to))) {
        report("claims: fixture", 0, strerror(errno));
    } else {
        report("a file found by pid whose server the kernel says is another process is refused",
               PMIX_SUCCESS != tool_by_pid(sys, other, false), "taken");
        /* back at its own name, but one its group may write: passed over */
        if (!move_pidfile_back(to, sys, spid, 0664)) {
            report("claims: fixture", 0, strerror(errno));
        } else {
            report("a file found by pid that others may write is passed over",
                   PMIX_SUCCESS != tool_by_pid(sys, spid, false), "taken");
        }
    }
    stop_server(spid, ready, go);

    /* one owned by another user - as root, who can give it one */
    if (0 != geteuid()) {
        fprintf(stdout, "  SKIP: a tool file owned by another user - not running as root\n");
        fprintf(stdout, "  SKIP: a system server's file owned by an unlisted user - not running "
                        "as root\n");
        return;
    }
    scratch(sys, sizeof(sys), "sys7");
    scratch(sess, sizeof(sess), "sess7");
    spid = start_server(sys, false, sess, false, &ready, &go);
    if (0 > spid || !server_up(ready)) {
        report("claims as root: the server starts", 0, "init failed");
        return;
    }
    if (!move_pidfile(sys, spid, spid, 0644, 4242, to, sizeof(to))) {
        report("claims as root: fixture", 0, strerror(errno));
    } else {
        report("a file owned by another user whose server the kernel says is ours is refused",
               PMIX_SUCCESS != tool_by_pid(sys, spid, false), "taken");
    }
    stop_server(spid, ready, go);

    scratch(sys, sizeof(sys), "sys8");
    scratch(sess, sizeof(sess), "sess8");
    spid = start_server(sys, true, sess, true, &ready, &go);
    if (0 > spid || !server_up(ready)) {
        report("system file as root: the server starts", 0, "init failed");
        return;
    }
    if (!find_file(sys, "pmix.sys.", "", path, sizeof(path)) ||
        0 != chown(path, 4242, (gid_t) -1)) {
        report("system file as root: fixture", 0, "no system file to give away");
    } else {
        report("a system server's file owned by a user this process does not list is not taken",
               PMIX_SUCCESS != tool_to_system(sys), "taken");
    }
    stop_server(spid, ready, go);
}

static void test_foreign(void)
{
    char sys[700], sess[700], path[1024], buf[64];
    const char *body = "another user's file\n";
    struct stat st;
    pid_t spid;
    int ready, go, fd;
    ssize_t n;

    if (0 != geteuid()) {
        fprintf(stdout, "  SKIP: another user's file at a server's name - not running as "
                        "root\n");
        return;
    }
    scratch(sys, sizeof(sys), "sys9");
    scratch(sess, sizeof(sess), "sess9");
    /* learn the name: the system server's file, which it removes again */
    spid = start_server(sys, true, sess, true, &ready, &go);
    if (0 > spid || !server_up(ready) || !find_file(sys, "pmix.sys.", "", path, sizeof(path))) {
        report("foreign: fixture", 0, "no system file to learn the name from");
        return;
    }
    stop_server(spid, ready, go);
    fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (0 > fd || (ssize_t) strlen(body) != write(fd, body, strlen(body)) ||
        0 != fchown(fd, 4242, 4242)) {
        report("foreign: fixture", 0, strerror(errno));
        return;
    }
    close(fd);
    spid = start_server(sys, true, sess, true, &ready, &go);
    report("a server finding another user's file at its name starts",
           0 < spid && server_up(ready), "init failed");
    stop_server(spid, ready, go);
    fd = open(path, O_RDONLY);
    n = (0 > fd) ? -1 : read(fd, buf, sizeof(buf) - 1);
    if (0 <= fd) {
        close(fd);
    }
    buf[(0 < n) ? n : 0] = '\0';
    report("...and leaves the file as it was",
           0 == stat(path, &st) && 4242 == st.st_uid && 0 == strcmp(buf, body),
           "it was changed or removed");
    unlink(path);
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
    test_claims();
    test_foreign();

    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", base);
    if (0 != system(cmd)) {
        fprintf(stderr, "could not remove %s\n", base);
    }
    fprintf(stdout, "\n%d passed, %d failed\n", npass, nfail);
    return (0 == nfail) ? 0 : 1;
}
