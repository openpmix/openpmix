/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 */

/* Run complete init/finalize cycles more than once in the same process,
 * in each role that acquires the node topology (client, server, server
 * sharing its topology, server given its topology by the host, launcher
 * tool), and check that every cycle has a usable topology of its own.
 *
 * Each scenario runs in a forked child so that an abort in one is
 * reported as that scenario's failure and the others still run. */

#include "src/include/pmix_config.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <hwloc.h>

#include "pmix.h"
#include "pmix_server.h"
#include "pmix_tool.h"

#define NCYCLES 3

extern char **environ;

/* confirm the library hands back a topology that hwloc can walk - and,
 * when the host provided one, that it is the host's */
static int check_topology(const char *scenario, int cycle, hwloc_topology_t expected)
{
    pmix_topology_t topo = {NULL, NULL};
    pmix_status_t rc;
    int npus;

    rc = PMIx_Load_topology(&topo);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "%s cycle %d: PMIx_Load_topology failed: %s\n",
                scenario, cycle, PMIx_Error_string(rc));
        return 1;
    }
    if (NULL == topo.topology) {
        fprintf(stderr, "%s cycle %d: PMIx_Load_topology returned no topology\n",
                scenario, cycle);
        free(topo.source);
        return 1;
    }
    if (NULL != expected && expected != (hwloc_topology_t) topo.topology) {
        fprintf(stderr, "%s cycle %d: returned topology is not the one the host provided\n",
                scenario, cycle);
        free(topo.source);
        return 1;
    }
    npus = hwloc_get_nbobjs_by_type((hwloc_topology_t) topo.topology, HWLOC_OBJ_PU);
    free(topo.source);
    if (0 >= npus) {
        fprintf(stderr, "%s cycle %d: topology has no PUs\n", scenario, cycle);
        return 1;
    }
    return 0;
}

static int run_client(void)
{
    pmix_proc_t me;
    pmix_status_t rc;
    int n;

    for (n = 0; n < NCYCLES; n++) {
        /* nest a second init inside each cycle - only the outermost
         * finalize may release anything */
        rc = PMIx_Init(&me, NULL, 0);
        if (PMIX_SUCCESS != rc && PMIX_ERR_UNREACH != rc) {
            fprintf(stderr, "client cycle %d: PMIx_Init failed: %s\n",
                    n, PMIx_Error_string(rc));
            return 1;
        }
        rc = PMIx_Init(&me, NULL, 0);
        if (PMIX_SUCCESS != rc && PMIX_ERR_UNREACH != rc) {
            fprintf(stderr, "client cycle %d: nested PMIx_Init failed: %s\n",
                    n, PMIx_Error_string(rc));
            return 1;
        }
        if (0 != check_topology("client", n, NULL)) {
            return 1;
        }
        PMIx_Finalize(NULL, 0);
        if (0 != check_topology("client (nested)", n, NULL)) {
            return 1;
        }
        rc = PMIx_Finalize(NULL, 0);
        if (PMIX_SUCCESS != rc) {
            fprintf(stderr, "client cycle %d: PMIx_Finalize failed: %s\n",
                    n, PMIx_Error_string(rc));
            return 1;
        }
    }
    return 0;
}

static int run_server(bool share, hwloc_topology_t external)
{
    pmix_server_module_t module;
    pmix_info_t info[2];
    pmix_topology_t topo;
    size_t ninfo = 0;
    pmix_status_t rc;
    const char *scenario;
    int n;

    memset(&module, 0, sizeof(module));
    if (share) {
        scenario = "server (shared topology)";
    } else if (NULL != external) {
        scenario = "server (host topology)";
    } else {
        scenario = "server";
    }

    for (n = 0; n < NCYCLES; n++) {
        ninfo = 0;
        if (share) {
            PMIX_INFO_LOAD(&info[ninfo], PMIX_SERVER_SHARE_TOPOLOGY, NULL, PMIX_BOOL);
            ++ninfo;
        }
        if (NULL != external) {
            /* hand over the host's own topology object - loading the
             * info with PMIX_INFO_LOAD would pass a duplicate */
            topo.source = "hwloc";
            topo.topology = external;
            PMIX_INFO_CONSTRUCT(&info[ninfo]);
            PMIX_LOAD_KEY(info[ninfo].key, PMIX_TOPOLOGY2);
            info[ninfo].value.type = PMIX_TOPO;
            info[ninfo].value.data.topo = &topo;
            ++ninfo;
        }
        rc = PMIx_server_init(&module, (0 < ninfo) ? info : NULL, ninfo);
        if (share) {
            PMIX_INFO_DESTRUCT(&info[0]);
        }
        if (PMIX_SUCCESS != rc) {
            fprintf(stderr, "%s cycle %d: PMIx_server_init failed: %s\n",
                    scenario, n, PMIx_Error_string(rc));
            return 1;
        }
        if (0 != check_topology(scenario, n, external)) {
            return 1;
        }
        rc = PMIx_server_finalize();
        if (PMIX_SUCCESS != rc) {
            fprintf(stderr, "%s cycle %d: PMIx_server_finalize failed: %s\n",
                    scenario, n, PMIx_Error_string(rc));
            return 1;
        }
        /* a topology the host provided remains the host's */
        if (NULL != external &&
            0 >= hwloc_get_nbobjs_by_type(external, HWLOC_OBJ_PU)) {
            fprintf(stderr, "%s cycle %d: host topology unusable after finalize\n",
                    scenario, n);
            return 1;
        }
    }
    return 0;
}

static int run_server_external(void)
{
    hwloc_topology_t t;
    int ret;

    if (0 != hwloc_topology_init(&t)) {
        fprintf(stderr, "server (host topology): hwloc_topology_init failed\n");
        return 1;
    }
    if (0 != hwloc_topology_load(t)) {
        fprintf(stderr, "server (host topology): hwloc_topology_load failed\n");
        hwloc_topology_destroy(t);
        return 1;
    }
    ret = run_server(false, t);
    hwloc_topology_destroy(t);
    return ret;
}

static int run_launcher(void)
{
    pmix_proc_t me;
    pmix_info_t info[2];
    pmix_status_t rc;
    int n;

    for (n = 0; n < NCYCLES; n++) {
        PMIX_INFO_LOAD(&info[0], PMIX_LAUNCHER, NULL, PMIX_BOOL);
        PMIX_INFO_LOAD(&info[1], PMIX_TOOL_DO_NOT_CONNECT, NULL, PMIX_BOOL);
        rc = PMIx_tool_init(&me, info, 2);
        PMIX_INFO_DESTRUCT(&info[0]);
        PMIX_INFO_DESTRUCT(&info[1]);
        if (PMIX_SUCCESS != rc) {
            fprintf(stderr, "launcher cycle %d: PMIx_tool_init failed: %s\n",
                    n, PMIx_Error_string(rc));
            return 1;
        }
        if (0 != check_topology("launcher", n, NULL)) {
            return 1;
        }
        rc = PMIx_tool_finalize();
        if (PMIX_SUCCESS != rc) {
            fprintf(stderr, "launcher cycle %d: PMIx_tool_finalize failed: %s\n",
                    n, PMIx_Error_string(rc));
            return 1;
        }
    }
    return 0;
}

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

static int run_server_plain(void)
{
    return run_server(false, NULL);
}

static int run_server_shared(void)
{
    return run_server(true, NULL);
}

static struct {
    const char *name;
    const char *label;
    int (*fn)(void);
} scenarios[] = {
    {"client", "client", run_client},
    {"server", "server", run_server_plain},
    {"server-shared", "server (shared topology)", run_server_shared},
    {"server-host", "server (host topology)", run_server_external},
    {"launcher", "launcher", run_launcher},
    {NULL, NULL, NULL}
};

int main(int argc, char *argv[])
{
    int failed = 0;
    int n;

    /* nothing here is to connect to an outside server - drop any
     * connection information we may have inherited */
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
        failed += run_scenario(scenarios[n].label, scenarios[n].fn);
    }
    if (1 < argc) {
        fprintf(stderr, "unknown scenario %s\n", argv[1]);
        return 1;
    }

    return (0 == failed) ? 0 : 1;
}
