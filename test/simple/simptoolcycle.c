/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Regression test for the SERVER-side handling of a tool that cycles
 * PMIx_tool_init -> (work) -> PMIx_tool_finalize against a persistent
 * server.
 *
 * This is the server-side companion to test/unit/tool_cycle.c, which
 * exercises only the tool *library* teardown (with DO_NOT_CONNECT and no
 * server). Here a real tool connects to a real server every cycle, so the
 * server runs its finalized-peer teardown on each finalize and allocates a
 * fresh tool peer on each re-init. A correct server never accumulates
 * finalized tool peers in pmix_server_globals.clients; a per-cycle leak (a
 * never-reclaimed peer) grows that array by one per cycle.
 *
 * Two distinct server-side tool shapes are exercised, in two phases:
 *
 *   pure    - a standalone tool that gets a server-assigned nspace each
 *             init (a fresh nspace per cycle, never reused). Its finalized
 *             peer is freed at socket-close, so the live count returns to
 *             zero.
 *
 *   client  - a tool the host registered as a client (a launcher-child
 *             shape) that reuses one nspace and rank across cycles. Its
 *             finalized peer is left in place and reclaimed when the tool
 *             reconnects, so at most one lingers at a time.
 *
 * The process forks a fresh child (re-exec, so it starts from a clean
 * library state) for each phase; after the child exits, the parent counts
 * the peers left in its clients array and requires the count to stay within
 * a small bound no matter how many cycles ran. A crash, a failed re-init,
 * or a hang shows up as a non-zero child exit or a timeout.
 *
 * The parent advertises itself as the scheduler so the pure tool can init
 * without us registering a job for it (PMIx_tool_init skips its job-info
 * request when it reaches the scheduler), and it registers one job so the
 * client-tool has real job info to retrieve.
 */

#include "src/include/pmix_config.h"
#include "include/pmix_server.h"
#include "include/pmix_tool.h"
#include "src/include/pmix_globals.h"
#include "src/server/pmix_server_ops.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "src/util/pmix_argv.h"

#define DEFAULT_CYCLES 50

/* a phase may leave at most this many live peers behind while its final
 * socket-close is still settling; a per-cycle leak blows past it */
#define PEER_LEAK_BOUND 5

static volatile int regdone;
static void reg_cbfunc(pmix_status_t status, void *cbdata)
{
    PMIX_HIDE_UNUSED_PARAMS(status, cbdata);
    regdone = 1;
}
static void wait_reg(void)
{
    struct timespec ts = {0, 1000000};
    while (0 == regdone) {
        nanosleep(&ts, NULL);
    }
    regdone = 0;
}

static void tool_connect_fn(pmix_info_t *info, size_t ninfo,
                            pmix_tool_connection_cbfunc_t cbfunc, void *cbdata)
{
    pmix_proc_t proc;
    PMIX_HIDE_UNUSED_PARAMS(info, ninfo);

    /* hand every standalone tool the same identity so the server has to
     * cope with the identifier being reused cycle after cycle */
    pmix_strncpy(proc.nspace, "TOOL", PMIX_MAX_NSLEN);
    proc.rank = 0;
    if (NULL != cbfunc) {
        cbfunc(PMIX_SUCCESS, &proc, cbdata);
    }
}

static pmix_server_module_t mymodule = {
    .tool_connected = tool_connect_fn
};

/* register a minimal but complete job so the client-tool phase can pull
 * real job info; a bare, unregistered nspace hands back an empty job blob
 * that PMIx_tool_init cannot unpack */
static pmix_status_t register_job(const char *nspace)
{
    pmix_status_t rc;
    char *nodes, *ppn;
    pmix_info_t *iptr;
    pmix_info_t jinfo[1];
    pmix_proc_t client;
    uint32_t one = 1;

    PMIx_generate_regex(pmix_globals.hostname, &nodes);
    PMIx_generate_ppn("0", &ppn);

    PMIX_LOAD_KEY(jinfo[0].key, PMIX_JOB_INFO_ARRAY);
    jinfo[0].value.type = PMIX_DATA_ARRAY;
    PMIX_DATA_ARRAY_CREATE(jinfo[0].value.data.darray, 4, PMIX_INFO);
    iptr = (pmix_info_t *) jinfo[0].value.data.darray->array;
    PMIX_INFO_LOAD(&iptr[0], PMIX_NODE_MAP, nodes, PMIX_REGEX);
    PMIX_INFO_LOAD(&iptr[1], PMIX_PROC_MAP, ppn, PMIX_REGEX);
    PMIX_INFO_LOAD(&iptr[2], PMIX_JOB_SIZE, &one, PMIX_UINT32);
    PMIX_INFO_LOAD(&iptr[3], PMIX_UNIV_SIZE, &one, PMIX_UINT32);
    free(nodes);
    free(ppn);

    regdone = 0;
    rc = PMIx_server_register_nspace(nspace, 1, jinfo, 1, reg_cbfunc, NULL);
    PMIX_INFO_DESTRUCT(&jinfo[0]);
    if (PMIX_SUCCESS != rc && PMIX_OPERATION_SUCCEEDED != rc) {
        return rc;
    }
    if (PMIX_SUCCESS == rc) {
        wait_reg();
    }

    PMIX_LOAD_PROCID(&client, nspace, 0);
    rc = PMIx_server_register_client(&client, geteuid(), getegid(), NULL, reg_cbfunc, NULL);
    if (PMIX_SUCCESS != rc && PMIX_OPERATION_SUCCEEDED != rc) {
        return rc;
    }
    if (PMIX_SUCCESS == rc) {
        wait_reg();
    }
    return PMIX_SUCCESS;
}

/* count the live (non-NULL) peer objects in the server's clients array */
static int count_live_peers(void)
{
    int n, count = 0;
    pmix_peer_t *peer;

    for (n = 0; n < pmix_server_globals.clients.size; n++) {
        peer = (pmix_peer_t *) pmix_pointer_array_get_item(&pmix_server_globals.clients, n);
        if (NULL != peer) {
            ++count;
        }
    }
    return count;
}

static int run_tool_child(const char *mode, long ncycles)
{
    pmix_proc_t myproc;
    pmix_status_t rc;
    pmix_info_t tinfo[1];
    bool pure = (0 == strcmp(mode, "pure"));
    long i;

    if (pure) {
        /* a standalone tool: drop the client-identity vars so we come up
         * with a server-assigned nspace/rank each cycle */
        unsetenv("PMIX_NAMESPACE");
        unsetenv("PMIX_RANK");
    }

    for (i = 0; i < ncycles; i++) {
        if (pure) {
            /* connect to the parent as the scheduler; PMIx_tool_init then
             * skips its job-info request, so no job need be registered */
            PMIX_INFO_LOAD(&tinfo[0], PMIX_CONNECT_TO_SCHEDULER, NULL, PMIX_BOOL);
            rc = PMIx_tool_init(&myproc, tinfo, 1);
            PMIX_INFO_DESTRUCT(&tinfo[0]);
        } else {
            /* keep the PMIX_NAMESPACE/PMIX_RANK the parent seeded, so we
             * connect as the registered tool-client identity and retrieve
             * its job info */
            rc = PMIx_tool_init(&myproc, NULL, 0);
        }
        if (PMIX_SUCCESS != rc) {
            fprintf(stderr, "%s child cycle %ld: PMIx_tool_init failed: %s\n", mode, i,
                    PMIx_Error_string(rc));
            return 1;
        }
        if (!PMIx_Initialized()) {
            fprintf(stderr, "%s child cycle %ld: PMIx_Initialized() false after init\n", mode, i);
            return 1;
        }
        rc = PMIx_tool_finalize();
        if (PMIX_SUCCESS != rc) {
            fprintf(stderr, "%s child cycle %ld: PMIx_tool_finalize failed: %s\n", mode, i,
                    PMIx_Error_string(rc));
            return 1;
        }
    }
    return 0;
}

/* fork+exec a fresh tool child in the given mode, wait for it, and confirm
 * the server did not accumulate a peer per cycle. Returns 0 on success. */
static int run_phase(const char *self, const char *mode, long ncycles, char **base_env)
{
    pmix_status_t rc;
    pmix_proc_t proc;
    char **child_env = NULL;
    char **child_argv = NULL;
    char cycles_str[32];
    pid_t pid;
    int n, wstatus, live, settled;
    struct timespec ts;

    /* each child needs the rendezvous env plus, for the client phase, the
     * foobar:0 identity - both delivered through setup_fork on top of the
     * base (build-tree component path) env */
    for (n = 0; NULL != base_env[n]; n++) {
        PMIx_Argv_append_nosize(&child_env, base_env[n]);
    }
    PMIX_LOAD_PROCID(&proc, "foobar", 0);
    if (PMIX_SUCCESS != (rc = PMIx_server_setup_fork(&proc, &child_env))) {
        fprintf(stderr, "PMIx_server_setup_fork failed: %s\n", PMIx_Error_string(rc));
        PMIx_Argv_free(child_env);
        return 1;
    }

    snprintf(cycles_str, sizeof(cycles_str), "%ld", ncycles);
    PMIx_Argv_append_nosize(&child_argv, (char *) self);
    PMIx_Argv_append_nosize(&child_argv, "--tool-child");
    PMIx_Argv_append_nosize(&child_argv, (char *) mode);
    PMIx_Argv_append_nosize(&child_argv, cycles_str);

    pid = fork();
    if (pid < 0) {
        fprintf(stderr, "fork failed: %s\n", strerror(errno));
        PMIx_Argv_free(child_env);
        PMIx_Argv_free(child_argv);
        return 1;
    }
    if (0 == pid) {
        execve(self, child_argv, child_env);
        fprintf(stderr, "execve failed: %s\n", strerror(errno));
        _exit(127);
    }
    PMIx_Argv_free(child_env);
    PMIx_Argv_free(child_argv);

    if (pid != waitpid(pid, &wstatus, 0)) {
        fprintf(stderr, "waitpid failed: %s\n", strerror(errno));
        return 1;
    }
    if (!WIFEXITED(wstatus) || 0 != WEXITSTATUS(wstatus)) {
        fprintf(stderr, "%s tool child exited abnormally (status %d)\n", mode, wstatus);
        return 1;
    }

    /* let the progress thread process the child's final socket-close, then
     * confirm the client array did not accumulate a peer per cycle */
    ts.tv_sec = 0;
    ts.tv_nsec = 200 * 1000 * 1000; /* 200ms */
    nanosleep(&ts, NULL);
    live = count_live_peers();
    nanosleep(&ts, NULL);
    settled = count_live_peers();

    fprintf(stdout, "  %-6s phase: %ld cycles, live server peers = %d (settled %d)\n",
            mode, ncycles, live, settled);

    if (settled > PEER_LEAK_BOUND) {
        fprintf(stdout,
                "  %-6s phase: FAIL - %d live peers after %ld cycles (bound %d)\n",
                mode, settled, ncycles, PEER_LEAK_BOUND);
        return 1;
    }
    return 0;
}

static int run_server(char *self, long ncycles)
{
    pmix_status_t rc;
    pmix_info_t info[2];
    extern char **environ;

    fprintf(stdout, "\n=== server-side tool init/finalize cycling test (%ld cycles) ===\n\n",
            ncycles);

    /* A minimal server that accepts tool connections and advertises itself
     * as the scheduler (so the pure tool can init with no job registered
     * for it). */
    PMIX_INFO_LOAD(&info[0], PMIX_SERVER_TOOL_SUPPORT, NULL, PMIX_BOOL);
    PMIX_INFO_LOAD(&info[1], PMIX_SERVER_SCHEDULER, NULL, PMIX_BOOL);
    if (PMIX_SUCCESS != (rc = PMIx_server_init(&mymodule, info, 2))) {
        fprintf(stderr, "PMIx_server_init failed: %s\n", PMIx_Error_string(rc));
        return 1;
    }

    /* register one job so the client-tool phase has job info to retrieve */
    if (PMIX_SUCCESS != (rc = register_job("foobar"))) {
        fprintf(stderr, "register_job failed: %s\n", PMIx_Error_string(rc));
        PMIx_server_finalize();
        return 1;
    }

    if (0 != run_phase(self, "pure", ncycles, environ) ||
        0 != run_phase(self, "client", ncycles, environ)) {
        PMIx_server_finalize();
        fprintf(stdout, "\nserver-side tool init/finalize cycling: FAIL\n\n");
        return 1;
    }

    PMIx_server_finalize();
    fprintf(stdout, "\nserver-side tool init/finalize cycling: PASS\n\n");
    return 0;
}

int main(int argc, char **argv)
{
    long ncycles = DEFAULT_CYCLES;

    if (3 < argc && 0 == strcmp(argv[1], "--tool-child")) {
        ncycles = strtol(argv[3], NULL, 10);
        if (0 >= ncycles) {
            ncycles = DEFAULT_CYCLES;
        }
        return run_tool_child(argv[2], ncycles);
    }

    if (1 < argc) {
        ncycles = strtol(argv[1], NULL, 10);
        if (0 >= ncycles) {
            ncycles = DEFAULT_CYCLES;
        }
    }
    return run_server(argv[0], ncycles);
}
