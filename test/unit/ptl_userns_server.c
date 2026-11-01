/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * A client in a rootless container, whose server runs on the host as
 * root - Slurm's OCI path. The client's user namespace maps only its own
 * user, so the kernel shows it the root server as the overflow id
 * (65534): an id from outside its namespace, which names nobody. The
 * client connects all the same, because its server is the one named for
 * it, in the environment the server started it with.
 *
 * Run as root on Linux: the server gives its socket to a job user
 * (PMIX_USERID, as Slurm's does), and the client becomes that user and
 * then enters a user namespace mapping only itself. Anywhere else, or
 * where user namespaces cannot be made (a seccomp policy, say), the case
 * is skipped.
 */

#include "src/include/pmix_config.h"

#include "include/pmix.h"
#include "include/pmix_server.h"
#include "src/include/pmix_globals.h"
#include "src/util/pmix_argv.h"

#include <errno.h>
#include <fcntl.h>
#include <grp.h>
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
#include <unistd.h>

#define SKIP     77
#define JOB_UID  4242
#define JOB_GID  4242
#define NSPACE   "userns.job"

/* the client's exit status */
#define KID_OK       0
#define KID_FAILED   1
#define KID_NO_USERS 2   // could not become the job user, or make the namespace
#define KID_REFUSED  3   // PMIx_Init failed

static pmix_server_module_t mymodule = {0};

#if defined(__linux__) && defined(CLONE_NEWUSER)
/* write one line of a map file */
static bool put_map(const char *file, const char *line)
{
    int fd = open(file, O_WRONLY);
    bool ok;

    if (0 > fd) {
        return false;
    }
    ok = ((ssize_t) strlen(line) == write(fd, line, strlen(line)));
    close(fd);
    return ok;
}
#endif

/* the client: read its environment, become the job user in a namespace
 * of its own, and initialize */
static int run_kid(int envfd, int resultfd)
{
    char buf[16384], *line, *save = NULL, *eq, map[64];
    pmix_proc_t me;
    pmix_status_t rc;
    ssize_t n, got = 0;

    while (0 < (n = read(envfd, buf + got, sizeof(buf) - 1 - got))) {
        got += n;
    }
    buf[got] = '\0';
    for (line = strtok_r(buf, "\n", &save); NULL != line; line = strtok_r(NULL, "\n", &save)) {
        if (NULL != (eq = strchr(line, '='))) {
            *eq = '\0';
            setenv(line, eq + 1, 1);
        }
    }
#if defined(__linux__) && defined(CLONE_NEWUSER)
    if (0 != setgroups(0, NULL) || 0 != setgid(JOB_GID) || 0 != setuid(JOB_UID) ||
        0 != unshare(CLONE_NEWUSER)) {
        return KID_NO_USERS;
    }
    /* only our own user and group: root, outside, is unmapped */
    if (!put_map("/proc/self/setgroups", "deny")) {
        return KID_NO_USERS;
    }
    snprintf(map, sizeof(map), "%d %d 1", JOB_UID, JOB_UID);
    if (!put_map("/proc/self/uid_map", map)) {
        return KID_NO_USERS;
    }
    snprintf(map, sizeof(map), "%d %d 1", JOB_GID, JOB_GID);
    if (!put_map("/proc/self/gid_map", map)) {
        return KID_NO_USERS;
    }
#else
    (void) map;
    return KID_NO_USERS;
#endif
    rc = PMIx_Init(&me, NULL, 0);
    if ((ssize_t) sizeof(rc) != write(resultfd, &rc, sizeof(rc))) {
        return KID_FAILED;
    }
    if (PMIX_SUCCESS != rc) {
        return KID_REFUSED;
    }
    PMIx_Finalize(NULL, 0);
    return KID_OK;
}

int main(void)
{
    char dir[256], **env = NULL;
    const char *base = getenv("TMPDIR");
    int envp[2], resp[2], status = 0, n;
    uint32_t uid = JOB_UID, gid = JOB_GID, one = 1;
    pmix_info_t info[3];
    pmix_proc_t proc;
    pmix_nspace_t ns;
    pmix_status_t rc = PMIX_ERROR;
    size_t k;
    pid_t pid;

    fprintf(stdout, "\n=== a client in a rootless container, its server root on the host ===\n\n");
#if !defined(__linux__) || !defined(CLONE_NEWUSER)
    fprintf(stdout, "no user namespaces on this platform - skipping\n");
    return SKIP;
#endif
    if (0 != geteuid()) {
        fprintf(stdout, "not running as root - skipping\n");
        return SKIP;
    }
    /* a directory the job user may enter, as Slurm's is */
    snprintf(dir, sizeof(dir), "%s/pmix-userns-XXXXXX", (NULL == base) ? "/tmp" : base);
    if (NULL == mkdtemp(dir) || 0 != chmod(dir, 0755)) {
        fprintf(stdout, "no scratch directory - skipping\n");
        return SKIP;
    }
    if (0 != pipe(envp) || 0 != pipe(resp)) {
        return 1;
    }
    /* the client, forked before any PMIx call */
    pid = fork();
    if (0 == pid) {
        close(envp[1]);
        close(resp[0]);
        _exit(run_kid(envp[0], resp[1]));
    }
    close(envp[0]);
    close(resp[1]);

    PMIX_INFO_LOAD(&info[0], PMIX_SERVER_TMPDIR, dir, PMIX_STRING);
    PMIX_INFO_LOAD(&info[1], PMIX_USERID, &uid, PMIX_UINT32);
    PMIX_INFO_LOAD(&info[2], PMIX_GRPID, &gid, PMIX_UINT32);
    if (PMIX_SUCCESS != PMIx_server_init(&mymodule, info, 3)) {
        fprintf(stdout, "  FAIL: server init\n");
        close(envp[1]);
        waitpid(pid, &status, 0);
        return 1;
    }
    PMIX_INFO_LOAD(&info[0], PMIX_JOB_SIZE, &one, PMIX_UINT32);
    PMIX_INFO_LOAD(&info[1], PMIX_USERID, &uid, PMIX_UINT32);
    PMIX_INFO_LOAD(&info[2], PMIX_GRPID, &gid, PMIX_UINT32);
    PMIX_LOAD_NSPACE(ns, NSPACE);
    PMIX_LOAD_PROCID(&proc, NSPACE, 0);
    rc = PMIx_server_register_nspace(ns, 1, info, 3, NULL, NULL);
    if (PMIX_OPERATION_SUCCEEDED == rc) {
        rc = PMIX_SUCCESS;
    }
    if (PMIX_SUCCESS == rc) {
        rc = PMIx_server_register_client(&proc, JOB_UID, JOB_GID, NULL, NULL, NULL);
        if (PMIX_OPERATION_SUCCEEDED == rc) {
            rc = PMIX_SUCCESS;
        }
    }
    if (PMIX_SUCCESS == rc) {
        rc = PMIx_server_setup_fork(&proc, &env);
    }
    if (PMIX_SUCCESS != rc) {
        fprintf(stdout, "  FAIL: could not register or prepare the client: %s\n",
                PMIx_Error_string(rc));
        close(envp[1]);
        waitpid(pid, &status, 0);
        PMIx_server_finalize();
        return 1;
    }
    for (k = 0; NULL != env[k]; k++) {
        if (0 != strncmp(env[k], "PMIX_", 5)) {
            continue;
        }
        n = (int) strlen(env[k]);
        if ((ssize_t) n != write(envp[1], env[k], n) || 1 != write(envp[1], "\n", 1)) {
            break;
        }
    }
    PMIx_Argv_free(env);
    close(envp[1]);
    if ((ssize_t) sizeof(rc) != read(resp[0], &rc, sizeof(rc))) {
        rc = PMIX_ERROR;
    }
    waitpid(pid, &status, 0);
    PMIx_server_finalize();
    if (WIFEXITED(status) && KID_NO_USERS == WEXITSTATUS(status)) {
        fprintf(stdout, "could not become the job user in a user namespace here - skipping\n");
        rmdir(dir);
        return SKIP;
    }
    if (PMIX_SUCCESS == rc && WIFEXITED(status) && KID_OK == WEXITSTATUS(status)) {
        fprintf(stdout, "  PASS: the client connects to its root server, which its kernel cannot "
                        "name\n\n1 passed, 0 failed\n");
        rmdir(dir);
        return 0;
    }
    fprintf(stdout, "  FAIL: the client connects to its root server, which its kernel cannot name "
                    "(init %s, exit %d)\n\n0 passed, 1 failed\n",
            PMIx_Error_string(rc), WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    rmdir(dir);
    return 1;
}
