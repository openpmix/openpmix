/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * The usock listener every server opens for the processes on its host.
 *
 * It is a socket in a directory of its own beneath the server's tmpdir;
 * the directory is the server's user's alone, and the socket owner-only,
 * unless the host widens them (PMIX_USERID, PMIX_GRPID,
 * PMIX_SOCKET_MODE; the owner and group as numbers or names). Its URI reaches the server's children in
 * PMIX_SERVER_LOCAL_URI with the identity the kernel reports for the
 * server, and tools on a tagged line of the rendezvous file, after the
 * server's host name. A server that
 * cannot make it - the path too long, a directive of the wrong type -
 * fails init, and finalize removes the socket and its directory.
 *
 * Each case runs in a child of its own: PMIx_server_init once per process.
 */

#include "src/include/pmix_config.h"

#include "include/pmix.h"
#include "include/pmix_server.h"
#include "src/include/pmix_globals.h"
#include "src/mca/ptl/base/base.h"
#include "src/mca/ptl/usock/ptl_usock.h"

#include <errno.h>
#include <grp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHILD_PASS 0
#define CHILD_FAIL 1

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

static pmix_server_module_t mymodule = {0};

/* the socket's path, from the listener's URI */
static const char *socket_path(void)
{
    const char *p;

    if (NULL == pmix_ptl_base.local_listener.uri ||
        NULL == (p = strstr(pmix_ptl_base.local_listener.uri, PMIX_PTL_USOCK_SCHEME))) {
        return NULL;
    }
    return p + strlen(PMIX_PTL_USOCK_SCHEME);
}

/* the socket and its directory have these modes, and are ours */
static int check_modes(mode_t smode, mode_t dmode)
{
    const char *path = socket_path();
    char *dir, *slash;
    struct stat st;
    int ret = CHILD_PASS;

    if (NULL == path) {
        fprintf(stderr, "  no local listener URI\n");
        return CHILD_FAIL;
    }
    if (0 != lstat(path, &st) || !S_ISSOCK(st.st_mode)) {
        fprintf(stderr, "  %s is not a socket\n", path);
        return CHILD_FAIL;
    }
    if (smode != (st.st_mode & 0777) || geteuid() != st.st_uid) {
        fprintf(stderr, "  the socket is mode %o uid %lu\n", (unsigned) (st.st_mode & 0777),
                (unsigned long) st.st_uid);
        ret = CHILD_FAIL;
    }
    dir = strdup(path);
    slash = strrchr(dir, '/');
    *slash = '\0';
    if (0 != lstat(dir, &st) || !S_ISDIR(st.st_mode) || dmode != (st.st_mode & 0777) ||
        geteuid() != st.st_uid) {
        fprintf(stderr, "  the directory is mode %o\n", (unsigned) (st.st_mode & 0777));
        ret = CHILD_FAIL;
    }
    free(dir);
    return ret;
}

/* owner-only, in a directory that is ours alone; finalize removes both */
static int default_child(void)
{
    char *path, *dir, *slash;
    int ret;

    if (PMIX_SUCCESS != PMIx_server_init(&mymodule, NULL, 0)) {
        return CHILD_FAIL;
    }
    ret = check_modes(S_IRUSR | S_IWUSR, S_IRWXU);
    path = strdup(socket_path());
    dir = strdup(path);
    slash = strrchr(dir, '/');
    *slash = '\0';
    PMIx_server_finalize();
    if (0 == access(path, F_OK) || 0 == access(dir, F_OK)) {
        fprintf(stderr, "  %s or its directory is still there after finalize\n", path);
        ret = CHILD_FAIL;
    }
    free(path);
    free(dir);
    return ret;
}

/* the host widens it: a mode and a group, and the directory lets that
 * group in */
static int widened_child(void)
{
    pmix_info_t info[2];
    uint32_t mode = 0660, gid = (uint32_t) getegid();
    int ret;

    PMIX_INFO_LOAD(&info[0], PMIX_SOCKET_MODE, &mode, PMIX_UINT32);
    PMIX_INFO_LOAD(&info[1], PMIX_GRPID, &gid, PMIX_UINT32);
    if (PMIX_SUCCESS != PMIx_server_init(&mymodule, info, 2)) {
        return CHILD_FAIL;
    }
    ret = check_modes(0660, S_IRWXU | S_IXGRP);
    PMIx_server_finalize();
    return ret;
}

/* the socket's group is ours even in a tmpdir whose new files take its
 * group (the BSD rule - macOS's /tmp is group wheel) */
static int inherited_group_child(void)
{
    pmix_info_t info[2];
    uint32_t mode = 0660;
    char dir[64];
    struct stat st;
    const char *path;
    int ret = CHILD_PASS;

    snprintf(dir, sizeof(dir), "/tmp/pmix_usock_grp.%lu", (unsigned long) getpid());
    if (0 != mkdir(dir, 0700) || 0 != stat(dir, &st)) {
        return CHILD_FAIL;
    }
    PMIX_INFO_LOAD(&info[0], PMIX_SOCKET_MODE, &mode, PMIX_UINT32);
    PMIX_INFO_LOAD(&info[1], PMIX_SERVER_TMPDIR, dir, PMIX_STRING);
    if (PMIX_SUCCESS != PMIx_server_init(&mymodule, info, 2)) {
        rmdir(dir);
        return CHILD_FAIL;
    }
    path = socket_path();
    if (NULL == path || 0 != lstat(path, &st) || getegid() != st.st_gid) {
        fprintf(stderr, "  the socket's group is %lu, not %lu\n", (unsigned long) st.st_gid,
                (unsigned long) getegid());
        ret = CHILD_FAIL;
    }
    PMIx_server_finalize();
    rmdir(dir);
    return ret;
}

/* as root: a socket given to another user leaves the directory ours, which
 * that user may search but not list */
static int other_owner_child(void)
{
    pmix_info_t info;
    uint32_t uid = 65534;
    const char *path;
    char *dir, *slash;
    struct stat st;
    int ret = CHILD_PASS;

    if (0 != geteuid()) {
        /* only root can give a socket away */
        return CHILD_PASS;
    }
    PMIX_INFO_LOAD(&info, PMIX_USERID, &uid, PMIX_UINT32);
    if (PMIX_SUCCESS != PMIx_server_init(&mymodule, &info, 1)) {
        return CHILD_FAIL;
    }
    path = socket_path();
    if (NULL == path || 0 != lstat(path, &st) || uid != st.st_uid) {
        fprintf(stderr, "  the socket is not owned by %u\n", uid);
        ret = CHILD_FAIL;
    }
    dir = strdup(path);
    slash = strrchr(dir, '/');
    *slash = '\0';
    if (0 != lstat(dir, &st) || 0 != st.st_uid || (S_IRWXU | S_IXOTH) != (st.st_mode & 0777)) {
        fprintf(stderr, "  the directory is uid %lu mode %o\n", (unsigned long) st.st_uid,
                (unsigned) (st.st_mode & 0777));
        ret = CHILD_FAIL;
    }
    free(dir);
    PMIx_server_finalize();
    return ret;
}

/* the group named by name, as PMIX_GRPID allows; a name no group has fails
 * init */
static int group_name_child(void)
{
    pmix_info_t info[2];
    uint32_t mode = 0660;
    struct group *gr = getgrgid(getegid());
    int ret;

    if (NULL == gr) {
        /* nothing to name it by */
        return CHILD_PASS;
    }
    PMIX_INFO_LOAD(&info[0], PMIX_SOCKET_MODE, &mode, PMIX_UINT32);
    PMIX_INFO_LOAD(&info[1], PMIX_GRPID, "pmix-no-such-group-anywhere", PMIX_STRING);
    if (PMIX_SUCCESS == PMIx_server_init(&mymodule, info, 2)) {
        fprintf(stderr, "  a group name nobody has was taken\n");
        return CHILD_FAIL;
    }
    PMIX_INFO_DESTRUCT(&info[1]);
    PMIX_INFO_LOAD(&info[1], PMIX_GRPID, gr->gr_name, PMIX_STRING);
    if (PMIX_SUCCESS != PMIx_server_init(&mymodule, info, 2)) {
        fprintf(stderr, "  group %s was refused\n", gr->gr_name);
        return CHILD_FAIL;
    }
    ret = check_modes(0660, S_IRWXU | S_IXGRP);
    PMIx_server_finalize();
    return ret;
}

/* a mode given as anything but a uint32, or one past nine bits, fails init */
static int bad_mode_child(void)
{
    pmix_info_t info;
    uint32_t mode = 01777;

    PMIX_INFO_LOAD(&info, PMIX_SOCKET_MODE, "0660", PMIX_STRING);
    if (PMIX_SUCCESS == PMIx_server_init(&mymodule, &info, 1)) {
        fprintf(stderr, "  a string mode was taken\n");
        return CHILD_FAIL;
    }
    PMIX_INFO_LOAD(&info, PMIX_SOCKET_MODE, &mode, PMIX_UINT32);
    if (PMIX_SUCCESS == PMIx_server_init(&mymodule, &info, 1)) {
        fprintf(stderr, "  mode 01777 was taken\n");
        return CHILD_FAIL;
    }
    return CHILD_PASS;
}

/* a tmpdir that leaves no room for the socket's path fails init */
static int long_tmpdir_child(void)
{
    pmix_info_t info;
    char dir[256], *p;
    size_t n;

    snprintf(dir, sizeof(dir), "/tmp/pmix_usock_long.%lu", (unsigned long) getpid());
    p = dir + strlen(dir);
    for (n = strlen(dir); n < 140; n++) {
        *p++ = 'x';
    }
    *p = '\0';
    if (0 != mkdir(dir, 0700) && EEXIST != errno) {
        return CHILD_FAIL;
    }
    PMIX_INFO_LOAD(&info, PMIX_SERVER_TMPDIR, dir, PMIX_STRING);
    if (PMIX_SUCCESS == PMIx_server_init(&mymodule, &info, 1)) {
        fprintf(stderr, "  init succeeded with a socket path past its limit\n");
        rmdir(dir);
        return CHILD_FAIL;
    }
    rmdir(dir);
    return CHILD_PASS;
}

/* setup_fork gives a child the URI and the identity the kernel will report
 * for us; a tool's rendezvous file gives it on a tagged line */
static int advertise_child(void)
{
    pmix_info_t info;
    pmix_proc_t proc;
    char **env = NULL, *want = NULL, *line = NULL, buf[4096];
    bool flag = true, found = false;
    FILE *fp;
    size_t n;
    int ret = CHILD_PASS;

    PMIX_INFO_LOAD(&info, PMIX_SERVER_TOOL_SUPPORT, &flag, PMIX_BOOL);
    if (PMIX_SUCCESS != PMIx_server_init(&mymodule, &info, 1)) {
        return CHILD_FAIL;
    }
    PMIX_LOAD_PROCID(&proc, "usock.child", 0);
    if (PMIX_SUCCESS != PMIx_server_setup_fork(&proc, &env)) {
        return CHILD_FAIL;
    }
    if (0 > asprintf(&want, "%s=%s;%lu:%lu", PMIX_PTL_LOCAL_URI_ENV,
                     pmix_ptl_base.local_listener.uri, (unsigned long) geteuid(),
                     (unsigned long) getegid())) {
        return CHILD_FAIL;
    }
    for (n = 0; NULL != env && NULL != env[n]; n++) {
        if (0 == strcmp(env[n], want)) {
            found = true;
        }
    }
    if (!found) {
        fprintf(stderr, "  the child's environment has no %s\n", want);
        ret = CHILD_FAIL;
    }
    free(want);
    PMIx_Argv_free(env);

    found = false;
    if (NULL == pmix_ptl_base.pid_filename ||
        NULL == (fp = fopen(pmix_ptl_base.pid_filename, "r"))) {
        fprintf(stderr, "  no rendezvous file\n");
        ret = CHILD_FAIL;
    } else {
        if (0 > asprintf(&line, "%s%s;%s\n", PMIX_PTL_LOCAL_URI_TAG, pmix_globals.hostname,
                         pmix_ptl_base.local_listener.uri)) {
            return CHILD_FAIL;
        }
        while (NULL != fgets(buf, sizeof(buf), fp)) {
            if (0 == strcmp(buf, line)) {
                found = true;
            }
        }
        fclose(fp);
        free(line);
        if (!found) {
            fprintf(stderr, "  the rendezvous file has no local line\n");
            ret = CHILD_FAIL;
        }
    }
    PMIx_server_finalize();
    return ret;
}

static void run_case(const char *name, int (*fn)(void))
{
    pid_t child;
    int status = 0;

    fflush(stdout);
    child = fork();
    if (0 > child) {
        report(name, 0, "fork failed");
        return;
    }
    if (0 == child) {
        alarm(60);
        _exit(fn());
    }
    waitpid(child, &status, 0);
    report(name, WIFEXITED(status) && CHILD_PASS == WEXITSTATUS(status), "see above");
}

int main(int argc, char **argv)
{
    PMIX_HIDE_UNUSED_PARAMS(argc, argv);

    fprintf(stdout, "\n=== the usock listener ===\n\n");
    run_case("the socket is owner-only, in a directory that is ours alone, both gone at finalize",
             default_child);
    run_case("the host widens the socket's mode and group, and the directory lets the group in",
             widened_child);
    run_case("the host names the socket's group by name, and a name no group has fails init",
             group_name_child);
    run_case("the socket's group is ours, not the one its directory would give it",
             inherited_group_child);
    run_case("a socket given to another user leaves its directory ours (as root)",
             other_owner_child);
    run_case("a socket mode of the wrong type or past nine bits fails init", bad_mode_child);
    run_case("a tmpdir leaving no room for the socket's path fails init", long_tmpdir_child);
    run_case("a child is given the URI and our identity, and a tool's file the URI",
             advertise_child);

    fprintf(stdout, "\n%d passed, %d failed\n", npass, nfail);
    return (0 == nfail) ? 0 : 1;
}
