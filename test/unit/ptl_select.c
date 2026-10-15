/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * A process's ptl transports are not chosen by the ptl selection
 * parameter. Whatever PMIX_MCA_ptl says - excluding tcp or usock, or
 * naming only one of them - and whether it is in the environment or a
 * parameter file, both transports are used. Client, server and tool init
 * also remove PMIX_MCA_ptl from the environment, so their children do not
 * inherit it.
 *
 * Each case runs in a child of its own, forked before any PMIx call.
 */

#include "src/include/pmix_config.h"

#include "include/pmix.h"
#include "include/pmix_server.h"
#include "include/pmix_tool.h"
#include "src/include/pmix_globals.h"
#include "src/mca/ptl/base/base.h"
#include "src/mca/ptl/tcp/ptl_tcp.h"
#include "src/mca/ptl/usock/ptl_usock.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

/* tcp and usock are in use, and PMIX_MCA_ptl is gone from the
 * environment */
static int check_state(void)
{
    pmix_ptl_base_active_t *active;
    bool tcp = false, usock = false;

    PMIX_LIST_FOREACH (active, &pmix_ptl_base.actives, pmix_ptl_base_active_t) {
        if (&pmix_ptl_tcp_module == active->module) {
            tcp = true;
        } else if (&pmix_ptl_usock_module == active->module) {
            usock = true;
        }
    }
    if (!tcp || !usock) {
        fprintf(stderr, "  in use: tcp %s, usock %s\n", tcp ? "yes" : "no",
                usock ? "yes" : "no");
        return CHILD_FAIL;
    }
    if (NULL != getenv("PMIX_MCA_ptl")) {
        fprintf(stderr, "  PMIX_MCA_ptl is still set to \"%s\"\n", getenv("PMIX_MCA_ptl"));
        return CHILD_FAIL;
    }
    return CHILD_PASS;
}

typedef enum {
    AS_SERVER,
    AS_CLIENT,
    AS_TOOL
} role_t;

static int run_child(role_t role, const char *env, const char *file)
{
    pmix_server_module_t module;
    pmix_proc_t me;
    pmix_info_t info;
    pmix_status_t rc;
    int ret;

    if (NULL != env) {
        setenv("PMIX_MCA_ptl", env, 1);
    }
    if (NULL != file) {
        setenv("PMIX_MCA_mca_base_param_files", file, 1);
    }
    switch (role) {
    case AS_SERVER:
        memset(&module, 0, sizeof(module));
        rc = PMIx_server_init(&module, NULL, 0);
        if (PMIX_SUCCESS != rc) {
            fprintf(stderr, "  PMIx_server_init: %s\n", PMIx_Error_string(rc));
            return CHILD_FAIL;
        }
        ret = check_state();
        PMIx_server_finalize();
        return ret;
    case AS_CLIENT:
        /* with no server to connect to, a singleton */
        unsetenv("PMIX_NAMESPACE");
        rc = PMIx_Init(&me, NULL, 0);
        if (PMIX_SUCCESS != rc && PMIX_ERR_UNREACH != rc) {
            fprintf(stderr, "  PMIx_Init: %s\n", PMIx_Error_string(rc));
            return CHILD_FAIL;
        }
        ret = check_state();
        PMIx_Finalize(NULL, 0);
        return ret;
    case AS_TOOL:
        PMIX_INFO_LOAD(&info, PMIX_TOOL_DO_NOT_CONNECT, NULL, PMIX_BOOL);
        rc = PMIx_tool_init(&me, &info, 1);
        PMIX_INFO_DESTRUCT(&info);
        if (PMIX_SUCCESS != rc) {
            fprintf(stderr, "  PMIx_tool_init: %s\n", PMIx_Error_string(rc));
            return CHILD_FAIL;
        }
        ret = check_state();
        PMIx_tool_finalize();
        return ret;
    }
    return CHILD_FAIL;
}

static void run_case(const char *name, role_t role, const char *env, const char *file)
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
        _exit(run_child(role, env, file));
    }
    waitpid(child, &status, 0);
    report(name, WIFEXITED(status) && CHILD_PASS == WEXITSTATUS(status), "see above");
}

int main(int argc, char **argv)
{
    char path[] = "/tmp/pmix_ptl_select.XXXXXX";
    FILE *fp;
    int fd;
    PMIX_HIDE_UNUSED_PARAMS(argc, argv);

    fprintf(stdout, "\n=== ptl transports are not chosen by PMIX_MCA_ptl ===\n\n");

    run_case("a server excluding tcp in its environment uses both", AS_SERVER, "^tcp", NULL);
    run_case("a server excluding usock in its environment uses both", AS_SERVER, "^usock", NULL);
    run_case("a server naming usock in its environment uses both", AS_SERVER, "usock", NULL);
    run_case("a client excluding tcp in its environment uses both", AS_CLIENT, "^tcp", NULL);
    run_case("a client excluding usock in its environment uses both", AS_CLIENT, "^usock", NULL);
    run_case("a client naming tcp in its environment uses both", AS_CLIENT, "tcp", NULL);
    run_case("a tool excluding tcp in its environment uses both", AS_TOOL, "^tcp", NULL);
    run_case("a tool excluding usock in its environment uses both", AS_TOOL, "^usock", NULL);

    fd = mkstemp(path);
    if (0 > fd || NULL == (fp = fdopen(fd, "w"))) {
        report("a server excluding usock in a parameter file uses both", 0, "no parameter file");
    } else {
        fprintf(fp, "ptl = ^usock\n");
        fclose(fp);
        run_case("a server excluding usock in a parameter file uses both", AS_SERVER, NULL, path);
        unlink(path);
    }

    fprintf(stdout, "\n%d passed, %d failed\n", npass, nfail);
    return (0 == nfail) ? 0 : 1;
}
