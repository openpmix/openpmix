/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Unit test: the variables that say who a process is and how it reaches
 * its server are taken out of the environment, and kept for the process:
 *
 *   - every one of them, each PMIX_SERVER_URI<version> among them, leaves
 *     the environment, and is read back from what was kept;
 *   - the token is overwritten where the environment held it;
 *   - the variables a process it starts still needs - the tmpdirs, the
 *     hostname, MCA parameters - stay;
 *   - taking them again keeps what was kept; a variable in the
 *     environment replaces the kept one of its name; and a namespace in
 *     it replaces everything kept;
 *   - a name the environment holds twice is taken as getenv() answers
 *     it, and both entries leave;
 *   - a token in memory the process may not write is taken, not
 *     overwritten;
 *   - a process forked from this one finds none kept, and none in the
 *     environment;
 *   - a variable never taken is read from the environment;
 *   - on Linux, run as root: a process forked into a pid namespace of its
 *     own, with the pid the process that kept them has in its own, finds
 *     none of them.
 */

#include "src/include/pmix_config.h"

#include "include/pmix.h"
#include "src/common/pmix_idenv.h"
#include "src/util/pmix_environ.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__linux__)
#    include <sched.h>
#endif

static int npass = 0;
static int nfail = 0;

static void report(const char *name, int passed)
{
    if (passed) {
        fprintf(stdout, "  PASS: %s\n", name);
        ++npass;
    } else {
        fprintf(stdout, "  FAIL: %s\n", name);
        ++nfail;
    }
}

static const char *const taken[] = {
    "PMIX_NAMESPACE", "PMIX_RANK", "PMIX_SERVER_NSPACE", "PMIX_SERVER_RANK",
    "PMIX_SERVER_URI", "PMIX_SERVER_URI41", "PMIX_SERVER_URI5", "PMIX_SERVER_LOCAL_URI",
    "PMIX_CONNECT_TOKEN", "PMIX_LAUNCHER_RNDZ_URI", "PMIX_SECURITY_MODE",
    "PMIX_BFROP_BUFFER_TYPE", "PMIX_GDS_MODULE", "PMIX_VERSION", NULL};
static const char *const kept_in_env[] = {
    "PMIX_SERVER_TMPDIR", "PMIX_SYSTEM_TMPDIR", "PMIX_HOSTNAME", "PMIX_MCA_ptl_base_verbose",
    NULL};

#define TOKEN "srv.0;000102030405060708090a0b0c0d0e0f"

/* value of the n'th variable */
static void value_of(const char *name, char *buf, size_t len)
{
    if (0 == strcmp(name, "PMIX_CONNECT_TOKEN")) {
        snprintf(buf, len, "%s", TOKEN);
    } else {
        snprintf(buf, len, "v-%s", name);
    }
}

int main(void)
{
    char buf[256], *token;
    const char *v;
    bool ok;
    pid_t pid;
    int n, status;

    fprintf(stdout, "\n=== the identity variables ===\n\n");
    for (n = 0; NULL != taken[n]; n++) {
        value_of(taken[n], buf, sizeof(buf));
        setenv(taken[n], buf, 1);
    }
    for (n = 0; NULL != kept_in_env[n]; n++) {
        setenv(kept_in_env[n], "stays", 1);
    }
    token = getenv("PMIX_CONNECT_TOKEN");

    pmix_idenv_take();

    ok = true;
    for (n = 0; NULL != taken[n]; n++) {
        if (NULL != getenv(taken[n])) {
            fprintf(stdout, "    %s is in the environment still\n", taken[n]);
            ok = false;
        }
    }
    report("each of them leaves the environment", ok);

    ok = true;
    for (n = 0; NULL != taken[n]; n++) {
        value_of(taken[n], buf, sizeof(buf));
        v = pmix_idenv_get(taken[n]);
        if (NULL == v || 0 != strcmp(v, buf)) {
            fprintf(stdout, "    %s reads %s\n", taken[n], (NULL == v) ? "nothing" : v);
            ok = false;
        }
    }
    report("each is read back as it was", ok);

    report("the token is overwritten where the environment held it",
           NULL != token && NULL == strstr(token, "0a0b0c0d"));

    ok = true;
    for (n = 0; NULL != kept_in_env[n]; n++) {
        if (NULL == getenv(kept_in_env[n])) {
            fprintf(stdout, "    %s left the environment\n", kept_in_env[n]);
            ok = false;
        }
    }
    report("the tmpdirs, the hostname and MCA parameters stay", ok);

    /* taken again: nothing in the environment, so nothing changes; then a
     * new value replaces the kept one */
    pmix_idenv_take();
    v = pmix_idenv_get("PMIX_RANK");
    report("taking them again keeps what was kept", NULL != v && 0 == strcmp(v, "v-PMIX_RANK"));
    setenv("PMIX_RANK", "7", 1);
    pmix_idenv_take();
    v = pmix_idenv_get("PMIX_RANK");
    report("a value in the environment replaces the kept one of its name",
           NULL != v && 0 == strcmp(v, "7") && NULL == getenv("PMIX_RANK") &&
               NULL != pmix_idenv_get("PMIX_NAMESPACE") &&
               NULL != pmix_idenv_get("PMIX_SERVER_URI41"));
    setenv("PMIX_NAMESPACE", "other.ns", 1);
    setenv("PMIX_RANK", "8", 1);
    pmix_idenv_take();
    v = pmix_idenv_get("PMIX_NAMESPACE");
    report("a namespace in the environment replaces everything kept",
           NULL != v && 0 == strcmp(v, "other.ns") && NULL != pmix_idenv_get("PMIX_RANK") &&
               NULL == pmix_idenv_get("PMIX_SERVER_URI41") &&
               NULL == pmix_idenv_get("PMIX_CONNECT_TOKEN"));

    setenv("PMIX_UT_IDENV_OTHER", "other", 1);
    v = pmix_idenv_get("PMIX_UT_IDENV_OTHER");
    report("a variable never taken is read from the environment",
           NULL != v && 0 == strcmp(v, "other"));

    fflush(stdout);
    pid = fork();
    if (0 == pid) {
        /* as a forked process's own init does */
        pmix_idenv_take();
        for (n = 0; NULL != taken[n]; n++) {
            if (NULL != pmix_idenv_get(taken[n])) {
                _exit(1);
            }
        }
        _exit(0);
    }
    report("a forked process finds none of them",
           0 < pid && pid == waitpid(pid, &status, 0) && WIFEXITED(status) &&
               0 == WEXITSTATUS(status));

    fflush(stdout);
    pid = fork();
    if (0 == pid) {
        static char *dup[] = {(char *) "PMIX_RANK=1", (char *) "PMIX_UT_IDENV_X=x",
                              (char *) "PMIX_RANK=2", NULL};

        environ = dup;
        pmix_idenv_take();
        v = pmix_idenv_get("PMIX_RANK");
        _exit((NULL != v && 0 == strcmp(v, "1") && NULL == getenv("PMIX_RANK") &&
               NULL != getenv("PMIX_UT_IDENV_X")) ? 0 : 1);
    }
    report("a name held twice is taken as getenv() answers it, and both entries leave",
           0 < pid && pid == waitpid(pid, &status, 0) && WIFEXITED(status) &&
               0 == WEXITSTATUS(status));

    fflush(stdout);
    pid = fork();
    if (0 == pid) {
        /* a string literal: not this process's to write */
        putenv((char *) "PMIX_CONNECT_TOKEN=" TOKEN);
        pmix_idenv_take();
        v = pmix_idenv_get("PMIX_CONNECT_TOKEN");
        _exit((NULL != v && 0 == strcmp(v, TOKEN) && NULL == getenv("PMIX_CONNECT_TOKEN"))
                  ? 0 : 1);
    }
    report("a token the process may not write is taken, not overwritten",
           0 < pid && pid == waitpid(pid, &status, 0) && WIFEXITED(status) &&
               0 == WEXITSTATUS(status));

#if defined(__linux__) && defined(CLONE_NEWPID)
    if (0 == geteuid()) {
        /* a process that is pid 1 of a pid namespace keeps them, and forks
         * a process that is pid 1 of a namespace of its own */
        fflush(stdout);
        pid = fork();
        if (0 == pid) {
            pid_t first, second;

            if (0 != unshare(CLONE_NEWPID)) {
                _exit(77);
            }
            first = fork();
            if (0 == first) {
                setenv("PMIX_RANK", "3", 1);
                pmix_idenv_take();
                if (1 != getpid() || NULL == pmix_idenv_get("PMIX_RANK") ||
                    0 != unshare(CLONE_NEWPID)) {
                    _exit(77);
                }
                second = fork();
                if (0 == second) {
                    _exit((1 == getpid() && NULL == pmix_idenv_get("PMIX_RANK")) ? 0 : 1);
                }
                _exit((0 < second && second == waitpid(second, &status, 0) &&
                       WIFEXITED(status)) ? WEXITSTATUS(status) : 1);
            }
            _exit((0 < first && first == waitpid(first, &status, 0) && WIFEXITED(status))
                      ? WEXITSTATUS(status) : 1);
        }
        if (0 < pid && pid == waitpid(pid, &status, 0) && WIFEXITED(status) &&
            77 == WEXITSTATUS(status)) {
            fprintf(stdout, "  SKIP: a pid namespace of its own could not be made\n");
        } else {
            report("a process forked into a pid namespace of its own, with the pid the "
                   "keeper has in its own, finds none of them",
                   0 < pid && WIFEXITED(status) && 0 == WEXITSTATUS(status));
        }
    }
#endif

    fprintf(stdout, "\n%d passed, %d failed\n", npass, nfail);
    return (0 == nfail) ? 0 : 1;
}
