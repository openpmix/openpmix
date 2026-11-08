/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Unit test: the usock listener's socket is made in the directory the
 * server made for it, even when that directory's name has been given to
 * another directory by the time the socket is bound.
 *
 * This program supplies its own bind(). Armed, it renames the server's
 * directory and puts an empty directory at the old name before calling
 * the real one. The server must make no socket in that directory, and
 * must fail its init: the URI it would publish names the path, which no
 * longer leads to its socket. Unarmed, it changes nothing, and the server
 * starts.
 *
 * Where the library's bind() does not come here - a platform whose
 * libraries bind their symbols to the library that defines them, as
 * macOS does, or a build where the real one cannot be found - the program
 * skips.
 */

#include "src/include/pmix_config.h"

#include "include/pmix.h"
#include "include/pmix_server.h"
#include "src/include/pmix_globals.h"

#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define SKIP 77

static int npass = 0;
static int nfail = 0;
static pmix_server_module_t mymodule = {0};

static bool armed = false;
static bool reached = false;
static char sess[256];

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

/* the server's directory in sess: pmix-XXXXXX, ours, 0700 */
static bool find_dir(char *name, size_t len)
{
    char path[1400];
    struct dirent *ep;
    struct stat st;
    DIR *dp = opendir(sess);
    bool found = false;

    if (NULL == dp) {
        return false;
    }
    while (!found && NULL != (ep = readdir(dp))) {
        if (11 != strlen(ep->d_name) || 0 != strncmp(ep->d_name, "pmix-", 5)) {
            continue;
        }
        snprintf(path, sizeof(path), "%s/%s", sess, ep->d_name);
        if (0 == lstat(path, &st) && S_ISDIR(st.st_mode) && geteuid() == st.st_uid &&
            S_IRWXU == (st.st_mode & 07777)) {
            snprintf(name, len, "%s", ep->d_name);
            found = true;
        }
    }
    closedir(dp);
    return found;
}

/* its directory renamed "moved", and an empty one at its name */
static void move_dir(void)
{
    char name[64], from[512], to[512];

    if (!find_dir(name, sizeof(name))) {
        return;
    }
    snprintf(from, sizeof(from), "%s/%s", sess, name);
    snprintf(to, sizeof(to), "%s/moved", sess);
    if (0 == rename(from, to)) {
        (void) mkdir(from, 0700);
    }
}

/* bind()'s address as the system's header declares it: glibc's, with
 * __USE_GNU, is a union holding the pointer (__CONST_SOCKADDR_ARG, a type
 * there and a macro naming the pointer otherwise) */
#if defined(__GLIBC__) && defined(__USE_GNU)
#    define BIND_ADDR_T __CONST_SOCKADDR_ARG
#else
#    define BIND_ADDR_T const struct sockaddr *
#endif

int bind(int sd, BIND_ADDR_T addr, socklen_t len)
{
    static int (*real_bind)(int, BIND_ADDR_T, socklen_t) = NULL;
    const struct sockaddr *sa;

    if (NULL == real_bind) {
        *(void **) (&real_bind) = dlsym(RTLD_NEXT, "bind");
    }
    if (NULL == real_bind) {
        errno = ENOSYS;
        return -1;
    }
    memcpy(&sa, &addr, sizeof(sa));
    if (armed && NULL != sa && AF_UNIX == sa->sa_family) {
        armed = false;
        reached = true;
        move_dir();
    }
    return real_bind(sd, addr, len);
}

/* the server, in a child: exit 0 if its init succeeded, 1 if not, SKIP if
 * the library's bind() did not come here */
static int server_child(bool arm)
{
    pmix_info_t info;
    pmix_status_t rc;

    alarm(60);
    armed = arm;
    PMIX_INFO_LOAD(&info, PMIX_SERVER_TMPDIR, sess, PMIX_STRING);
    rc = PMIx_server_init(&mymodule, &info, 1);
    if (arm && !reached) {
        return SKIP;
    }
    if (PMIX_SUCCESS != rc) {
        return 1;
    }
    PMIx_server_finalize();
    return 0;
}

static int run_child(bool arm)
{
    int status;
    pid_t pid;

    fflush(stdout);
    pid = fork();
    if (0 == pid) {
        _exit(server_child(arm));
    }
    if (0 > pid || pid != waitpid(pid, &status, 0) || !WIFEXITED(status)) {
        return -1;
    }
    return WEXITSTATUS(status);
}

/* the entries in dir, besides . and .., in names; -1 if it cannot be read */
static int entries(const char *dir, char *names, size_t len)
{
    struct dirent *ep;
    DIR *dp = opendir(dir);
    int n = 0;

    names[0] = '\0';
    if (NULL == dp) {
        return -1;
    }
    while (NULL != (ep = readdir(dp))) {
        if (0 == strcmp(ep->d_name, ".") || 0 == strcmp(ep->d_name, "..")) {
            continue;
        }
        ++n;
        if (strlen(names) + strlen(ep->d_name) + 2 < len) {
            strcat(names, " ");
            strcat(names, ep->d_name);
        }
    }
    closedir(dp);
    return n;
}

int main(int argc, char **argv)
{
    const char *tmp = getenv("TMPDIR");
    char name[64], decoy[512], detail[640], names[256], cmd[300];
    int rc, n;

    PMIX_HIDE_UNUSED_PARAMS(argc, argv);
    setvbuf(stdout, NULL, _IONBF, 0);
    fprintf(stdout, "\n=== usock: the socket is made in the directory made for it ===\n\n");
    setenv("PMIX_MCA_mca_base_param_files", "none", 1);

    snprintf(sess, sizeof(sess), "%s/usockbind.XXXXXX", (NULL == tmp) ? "/tmp" : tmp);
    if (60 < strlen(sess) || NULL == mkdtemp(sess)) {
        fprintf(stdout, "no short scratch directory - skipping\n");
        return SKIP;
    }

    rc = run_child(false);
    report("unarmed, the server starts", 0 == rc, "init failed");
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'/*", sess);
    (void) !system(cmd);

    rc = run_child(true);
    if (SKIP == rc) {
        fprintf(stdout, "  SKIP: the library's bind() does not come to this program\n");
    } else if (!find_dir(name, sizeof(name))) {
        report("the directory at the old name is there to look in", 0, "it is gone");
    } else {
        snprintf(decoy, sizeof(decoy), "%s/%s", sess, name);
        n = entries(decoy, names, sizeof(names));
        snprintf(detail, sizeof(detail), "it holds%s", names);
        report("no socket is made in the directory now at the old name", 0 == n, detail);
        report("...and the server's init fails, its URI naming that path", 1 == rc,
               (0 == rc) ? "init succeeded" : "the child failed otherwise");
    }

    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", sess);
    (void) !system(cmd);
    fprintf(stdout, "\n%d passed, %d failed\n", npass, nfail);
    if (0 == nfail && 1 == npass && SKIP == rc) {
        return SKIP;
    }
    return (0 == nfail) ? 0 : 1;
}
