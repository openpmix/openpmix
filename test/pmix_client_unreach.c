/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 */

/* A client that was given an identity (PMIX_NAMESPACE and PMIX_RANK) but
 * cannot reach a server comes out of PMIx_Init as an unconnected
 * singleton with PMIX_ERR_UNREACH. Check that it can then finalize - and
 * initialize and finalize again - both when no server is advertised and
 * when the advertised server is not listening, and when such cycles
 * alternate with cycles of a client that has no identity at all.
 *
 * Each scenario runs in a forked child so that a crash in one is
 * reported as that scenario's failure and the others still run. */

#include "src/include/pmix_config.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "pmix.h"

#define NCYCLES 3

extern char **environ;

static void set_identity(bool named)
{
    if (named) {
        setenv("PMIX_NAMESPACE", "unreach.test", 1);
        setenv("PMIX_RANK", "0", 1);
    } else {
        unsetenv("PMIX_NAMESPACE");
        unsetenv("PMIX_RANK");
    }
}

static int cycle(const char *scenario, int n, bool named)
{
    pmix_proc_t me;
    pmix_status_t rc;

    set_identity(named);
    rc = PMIx_Init(&me, NULL, 0);
    /* a client with no identity may find a system server to talk to;
     * one with an identity no server knows cannot be accepted */
    if (PMIX_ERR_UNREACH != rc &&
        (named || PMIX_SUCCESS != rc)) {
        fprintf(stderr, "%s cycle %d: PMIx_Init returned %s\n",
                scenario, n, PMIx_Error_string(rc));
        return 1;
    }
    if (named &&
        (0 != strcmp(me.nspace, "unreach.test") || 0 != me.rank)) {
        fprintf(stderr, "%s cycle %d: identity %s:%u, expected unreach.test:0\n",
                scenario, n, me.nspace, me.rank);
        return 1;
    }
    rc = PMIx_Finalize(NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "%s cycle %d: PMIx_Finalize failed: %s\n",
                scenario, n, PMIx_Error_string(rc));
        return 1;
    }
    return 0;
}

/* identity given, no server advertised */
static int run_named(void)
{
    int n;

    for (n = 0; n < NCYCLES; n++) {
        if (0 != cycle("named", n, true)) {
            return 1;
        }
    }
    return 0;
}

/* identity given, the advertised server is not listening */
static int run_closed_port(void)
{
    int n;

    setenv("PMIX_SERVER_URI41", "unreach.srv.0;tcp4://127.0.0.1:1", 1);
    for (n = 0; n < NCYCLES; n++) {
        if (0 != cycle("closed port", n, true)) {
            return 1;
        }
    }
    return 0;
}

/* cycles with and without an identity, in both orders */
static int run_alternating(void)
{
    int n;

    for (n = 0; n < 2 * NCYCLES; n++) {
        if (0 != cycle("alternating", n, (0 == n % 2))) {
            return 1;
        }
    }
    for (n = 0; n < 2 * NCYCLES; n++) {
        if (0 != cycle("alternating", n, (1 == n % 2))) {
            return 1;
        }
    }
    return 0;
}

static struct {
    const char *name;
    int (*fn)(void);
} scenarios[] = {
    {"named", run_named},
    {"closed-port", run_closed_port},
    {"alternating", run_alternating},
    {NULL, NULL}
};

static int run_scenario(const char *name, int (*fn)(void))
{
    pid_t pid;
    int status;

    fflush(stdout);
    fflush(stderr);
    pid = fork();
    if (0 > pid) {
        perror("fork");
        return 1;
    }
    if (0 == pid) {
        exit(fn());
    }
    if (0 > waitpid(pid, &status, 0)) {
        perror("waitpid");
        return 1;
    }
    if (WIFSIGNALED(status)) {
        fprintf(stderr, "FAIL %s: killed by signal %d\n", name, WTERMSIG(status));
        return 1;
    }
    if (!WIFEXITED(status) || 0 != WEXITSTATUS(status)) {
        fprintf(stderr, "FAIL %s\n", name);
        return 1;
    }
    fprintf(stdout, "PASS %s\n", name);
    return 0;
}

int main(int argc, char *argv[])
{
    int failed = 0;
    int n;

    /* start with no connection information of any kind - only the
     * scenarios decide what the client is told */
    for (n = 0; NULL != environ[n];) {
        if (0 == strncmp(environ[n], "PMIX_SERVER_URI", strlen("PMIX_SERVER_URI")) ||
            0 == strncmp(environ[n], "PMIX_NAMESPACE=", strlen("PMIX_NAMESPACE=")) ||
            0 == strncmp(environ[n], "PMIX_RANK=", strlen("PMIX_RANK="))) {
            char *eq = strchr(environ[n], '=');
            char *name = strndup(environ[n], (size_t) (eq - environ[n]));
            unsetenv(name);
            free(name);
            n = 0;
            continue;
        }
        n++;
    }

    /* a scenario named on the command line runs by itself, in this
     * process, so it can be run under a debugger */
    for (n = 0; NULL != scenarios[n].name; n++) {
        if (1 < argc) {
            if (0 == strcmp(argv[1], scenarios[n].name)) {
                return scenarios[n].fn();
            }
            continue;
        }
        failed += run_scenario(scenarios[n].name, scenarios[n].fn);
    }
    if (1 < argc) {
        fprintf(stderr, "unknown scenario %s\n", argv[1]);
        return 1;
    }

    return (0 == failed) ? 0 : 1;
}
