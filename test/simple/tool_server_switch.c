/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Regression test for the tool multi-server "switch" path: the code that
 * moves pmix_client_globals.myserver between the servers a tool is
 * connected to. That path lives in pmix_tool_retry_attach (attach a new
 * server as primary), pmix_tool_retry_set (switch the primary to an
 * already-known server, or back to ourselves), and disc (disconnect a
 * server). Each of those repoints "myserver", and each must keep the peer
 * reference counts balanced against PMIx_tool_finalize, which releases a
 * server peer once for its clients-array entry AND once for the myserver
 * global. A missing PMIX_RETAIN/PMIX_RELEASE around a switch shows up as a
 * double-free/use-after-free at finalize (or a slow peer leak), so this
 * test drives the whole switch API in a loop and finalizes every cycle.
 *
 * Because a tool addresses its servers by {nspace,rank}, two connections
 * to the *same* server process are indistinguishable to set_server /
 * disconnect - so a faithful test needs two DISTINCT servers. This one
 * binary plays three roles, dispatched by argv:
 *
 *   (default)      - orchestrator / "server A": inits a PMIx server
 *                    (scheduler + tool support), fork+execs a second
 *                    independent server ("server B"), then fork+execs a
 *                    tool child seeded with A's rendezvous (via setup_fork)
 *                    and B's URI. Waits for the child, tears B down, and
 *                    reports PASS/FAIL.
 *   --server-b F   - a second, independent PMIx server. Writes its own
 *                    connection URI to file F, then idles until F.done
 *                    appears, then finalizes.
 *   --tool-child N - the tool. Loops N times: init (-> A), attach B as
 *                    primary, query the server list, switch the primary
 *                    among B / A / self, disconnect both, finalize.
 *
 * A crash (the double-free), a failed API call, a wrong server count, or a
 * hang all surface as a non-zero child exit or a timeout from the harness.
 */

#include "src/include/pmix_config.h"
#include "include/pmix_server.h"
#include "include/pmix_tool.h"
#include "src/include/pmix_globals.h"
#include "src/mca/ptl/base/base.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "src/util/pmix_argv.h"

#define DEFAULT_CYCLES 30

/* ------------------------------------------------------------------ */
/* shared helpers                                                     */
/* ------------------------------------------------------------------ */

/* hand every connecting tool a fresh identity so repeated attaches across
 * cycles never collide on the server side */
static int idcounter = 0;
static void tool_connect_fn(pmix_info_t *info, size_t ninfo,
                            pmix_tool_connection_cbfunc_t cbfunc, void *cbdata)
{
    pmix_proc_t proc;
    PMIX_HIDE_UNUSED_PARAMS(info, ninfo);

    snprintf(proc.nspace, PMIX_MAX_NSLEN, "TOOL%d", idcounter++);
    proc.rank = 0;
    if (NULL != cbfunc) {
        cbfunc(PMIX_SUCCESS, &proc, cbdata);
    }
}

static pmix_server_module_t mymodule = {
    .tool_connected = tool_connect_fn
};

/* ------------------------------------------------------------------ */
/* role: server B (a second, independent PMIx server)                 */
/* ------------------------------------------------------------------ */

static int run_server_b(const char *urifile)
{
    pmix_status_t rc;
    pmix_info_t info[1];
    char donefile[4096];
    char tmpfile[4096];
    char bdir[4096];
    FILE *fp;
    struct timespec ts = {0, 50 * 1000 * 1000}; /* 50ms */
    struct stat sb;

    /* server B must not share a rendezvous directory with server A, or the
     * two collide - give it its own tmpdir. B is reached only by explicit
     * URI (never as the scheduler/system server), so tool support alone is
     * enough and it does not fight A for the system rendezvous. */
    snprintf(bdir, sizeof(bdir), "%s.d", urifile);
    (void) mkdir(bdir, 0700);
    setenv("PMIX_SERVER_TMPDIR", bdir, 1);

    PMIX_INFO_LOAD(&info[0], PMIX_SERVER_TOOL_SUPPORT, NULL, PMIX_BOOL);
    rc = PMIx_server_init(&mymodule, info, 1);
    PMIX_INFO_DESTRUCT(&info[0]);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "server-b: PMIx_server_init failed: %s\n", PMIx_Error_string(rc));
        return 1;
    }

    if (NULL == pmix_ptl_base.listener.uri) {
        fprintf(stderr, "server-b: no listener URI available\n");
        PMIx_server_finalize();
        return 1;
    }

    /* publish our URI atomically: write to a temp file then rename, so the
     * parent never reads a half-written path */
    snprintf(tmpfile, sizeof(tmpfile), "%s.tmp", urifile);
    fp = fopen(tmpfile, "w");
    if (NULL == fp) {
        fprintf(stderr, "server-b: cannot write %s: %s\n", tmpfile, strerror(errno));
        PMIx_server_finalize();
        return 1;
    }
    fprintf(fp, "%s\n", pmix_ptl_base.listener.uri);
    fclose(fp);
    if (0 != rename(tmpfile, urifile)) {
        fprintf(stderr, "server-b: cannot rename %s: %s\n", urifile, strerror(errno));
        PMIx_server_finalize();
        return 1;
    }

    /* idle until the parent signals us to shut down */
    snprintf(donefile, sizeof(donefile), "%s.done", urifile);
    while (0 != stat(donefile, &sb)) {
        nanosleep(&ts, NULL);
    }

    PMIx_server_finalize();
    return 0;
}

/* ------------------------------------------------------------------ */
/* role: the tool child - drive the whole switch API in a loop         */
/* ------------------------------------------------------------------ */

static int find_other(pmix_proc_t *servers, size_t n, const pmix_proc_t *known,
                      pmix_proc_t *out)
{
    size_t i;
    for (i = 0; i < n; i++) {
        if (!PMIX_CHECK_PROCID(&servers[i], known)) {
            PMIX_LOAD_PROCID(out, servers[i].nspace, servers[i].rank);
            return 0;
        }
    }
    return -1;
}

static int tool_one_cycle(long cyc, const char *burl)
{
    pmix_proc_t myproc, srvrA, srvrB, tmpproc;
    pmix_proc_t *servers = NULL;
    size_t nservers = 0;
    pmix_status_t rc;
    pmix_info_t info[3];
    int itmo = 2;

    /* come up with a server-assigned nspace/rank each cycle: clear any
     * identity vars a previous init/attach left in our environment, or a
     * later init sees a namespace without a rank (or vice versa) and fails
     * PMIX_ERR_BAD_PARAM */
    unsetenv("PMIX_NAMESPACE");
    unsetenv("PMIX_RANK");

    /* connect to server A as the scheduler; a scheduler-connected tool
     * skips its job-info request, so A need not register a job for us */
    PMIX_INFO_LOAD(&info[0], PMIX_CONNECT_TO_SCHEDULER, NULL, PMIX_BOOL);
    rc = PMIx_tool_init(&myproc, info, 1);
    PMIX_INFO_DESTRUCT(&info[0]);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "cycle %ld: PMIx_tool_init failed: %s\n", cyc, PMIx_Error_string(rc));
        return 1;
    }
    if (!PMIx_tool_is_connected()) {
        fprintf(stderr, "cycle %ld: not connected after init\n", cyc);
        goto err;
    }

    /* only server A is known so far - capture its identity */
    rc = PMIx_tool_get_servers(&servers, &nservers);
    if (PMIX_SUCCESS != rc || 1 != nservers) {
        fprintf(stderr, "cycle %ld: get_servers after init: rc=%s n=%zu (want 1)\n",
                cyc, PMIx_Error_string(rc), nservers);
        goto err;
    }
    PMIX_LOAD_PROCID(&srvrA, servers[0].nspace, servers[0].rank);
    PMIX_PROC_FREE(servers, nservers);
    servers = NULL;

    /* attach to server B as the new primary: this is the
     * pmix_tool_retry_attach switch path (the one that double-freed) */
    PMIX_INFO_LOAD(&info[0], PMIX_SERVER_URI, burl, PMIX_STRING);
    PMIX_INFO_LOAD(&info[1], PMIX_PRIMARY_SERVER, NULL, PMIX_BOOL);
    PMIX_INFO_LOAD(&info[2], PMIX_TIMEOUT, &itmo, PMIX_INT);
    rc = PMIx_tool_attach_to_server(&myproc, &srvrB, info, 3);
    PMIX_INFO_DESTRUCT(&info[0]);
    PMIX_INFO_DESTRUCT(&info[1]);
    PMIX_INFO_DESTRUCT(&info[2]);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "cycle %ld: attach_to_server(B) failed: %s\n", cyc,
                PMIx_Error_string(rc));
        goto err;
    }
    if (!PMIx_tool_is_connected()) {
        fprintf(stderr, "cycle %ld: not connected after attach\n", cyc);
        goto err;
    }

    /* both A and B must now be known, with distinct identities */
    rc = PMIx_tool_get_servers(&servers, &nservers);
    if (PMIX_SUCCESS != rc || 2 != nservers) {
        fprintf(stderr, "cycle %ld: get_servers after attach: rc=%s n=%zu (want 2)\n",
                cyc, PMIx_Error_string(rc), nservers);
        goto err;
    }
    /* sanity: the "other" server (not B) must be A */
    if (0 != find_other(servers, nservers, &srvrB, &tmpproc) ||
        !PMIX_CHECK_PROCID(&tmpproc, &srvrA)) {
        fprintf(stderr, "cycle %ld: server list does not hold both A and B\n", cyc);
        goto err;
    }
    PMIX_PROC_FREE(servers, nservers);
    servers = NULL;

    /* switch the primary back to ourselves: pmix_tool_retry_set
     * "switch back to me" branch (points myserver at our own peer) */
    rc = PMIx_tool_set_server(&myproc, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "cycle %ld: set_server(self) failed: %s\n", cyc, PMIx_Error_string(rc));
        goto err;
    }

    /* switch to A, then to B: pmix_tool_retry_set main (known-server) path */
    rc = PMIx_tool_set_server(&srvrA, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "cycle %ld: set_server(A) failed: %s\n", cyc, PMIx_Error_string(rc));
        goto err;
    }
    rc = PMIx_tool_set_server(&srvrB, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "cycle %ld: set_server(B) failed: %s\n", cyc, PMIx_Error_string(rc));
        goto err;
    }

    /* disconnect A while B is primary: disc non-primary branch */
    rc = PMIx_tool_disconnect(&srvrA);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "cycle %ld: disconnect(A) failed: %s\n", cyc, PMIx_Error_string(rc));
        goto err;
    }
    /* disconnect B, the primary: disc primary branch (myserver -> self) */
    rc = PMIx_tool_disconnect(&srvrB);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "cycle %ld: disconnect(B) failed: %s\n", cyc, PMIx_Error_string(rc));
        goto err;
    }

    /* teardown: the finalize path a switch-refcount bug double-frees, now
     * that myserver has been pointed back at our own peer by the last
     * disconnect */
    rc = PMIx_tool_finalize();
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "cycle %ld: PMIx_tool_finalize failed: %s\n", cyc,
                PMIx_Error_string(rc));
        return 1;
    }
    return 0;

err:
    if (NULL != servers) {
        PMIX_PROC_FREE(servers, nservers);
    }
    PMIx_tool_finalize();
    return 1;
}

static int run_tool_child(long ncycles, const char *burl)
{
    long i;

    for (i = 0; i < ncycles; i++) {
        if (0 != tool_one_cycle(i, burl)) {
            return 1;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* role: orchestrator / server A                                       */
/* ------------------------------------------------------------------ */

static char *read_uri(const char *urifile)
{
    FILE *fp;
    static char buf[4096];
    char *p;

    fp = fopen(urifile, "r");
    if (NULL == fp) {
        return NULL;
    }
    p = fgets(buf, sizeof(buf), fp);
    fclose(fp);
    if (NULL == p) {
        return NULL;
    }
    /* strip trailing newline */
    p = strchr(buf, '\n');
    if (NULL != p) {
        *p = '\0';
    }
    if (0 == strlen(buf)) {
        return NULL;
    }
    return buf;
}

static pid_t spawn_server_b(const char *self, const char *urifile)
{
    pid_t pid = fork();
    if (pid < 0) {
        return -1;
    }
    if (0 == pid) {
        char *argv[4];
        argv[0] = (char *) self;
        argv[1] = (char *) "--server-b";
        argv[2] = (char *) urifile;
        argv[3] = NULL;
        execv(self, argv);
        fprintf(stderr, "server-b execv failed: %s\n", strerror(errno));
        _exit(127);
    }
    return pid;
}

static pid_t spawn_tool(const char *self, long ncycles, const char *burl, char **base_env)
{
    pmix_status_t rc;
    pmix_proc_t proc;
    char **child_env = NULL;
    char **child_argv = NULL;
    char cycles_str[32];
    char burl_env[4200];
    pid_t pid;
    int n;

    for (n = 0; NULL != base_env[n]; n++) {
        PMIx_Argv_append_nosize(&child_env, base_env[n]);
    }
    /* seed the child with server A's rendezvous */
    PMIX_LOAD_PROCID(&proc, "foobar", 0);
    if (PMIX_SUCCESS != (rc = PMIx_server_setup_fork(&proc, &child_env))) {
        fprintf(stderr, "setup_fork failed: %s\n", PMIx_Error_string(rc));
        PMIx_Argv_free(child_env);
        return -1;
    }
    /* and with server B's URI */
    snprintf(burl_env, sizeof(burl_env), "PMIX_TEST_SERVERB_URI=%s", burl);
    PMIx_Argv_append_nosize(&child_env, burl_env);

    snprintf(cycles_str, sizeof(cycles_str), "%ld", ncycles);
    PMIx_Argv_append_nosize(&child_argv, (char *) self);
    PMIx_Argv_append_nosize(&child_argv, (char *) "--tool-child");
    PMIx_Argv_append_nosize(&child_argv, cycles_str);

    pid = fork();
    if (pid < 0) {
        PMIx_Argv_free(child_env);
        PMIx_Argv_free(child_argv);
        return -1;
    }
    if (0 == pid) {
        execve(self, child_argv, child_env);
        fprintf(stderr, "tool execve failed: %s\n", strerror(errno));
        _exit(127);
    }
    PMIx_Argv_free(child_env);
    PMIx_Argv_free(child_argv);
    return pid;
}

static int run_orchestrator(char *self, long ncycles)
{
    pmix_status_t rc;
    pmix_info_t info[2];
    pid_t bpid, tpid;
    char urifile[256];
    char donefile[512];
    char *burl;
    int wstatus, i, toolrc;
    struct timespec ts = {0, 50 * 1000 * 1000}; /* 50ms */
    FILE *fp;
    extern char **environ;

    fprintf(stdout, "\n=== tool multi-server switch test (%ld cycles) ===\n\n", ncycles);

    snprintf(urifile, sizeof(urifile), "/tmp/pmix-toolswitch-b-%lu.uri",
             (unsigned long) getpid());
    snprintf(donefile, sizeof(donefile), "%s.done", urifile);
    unlink(urifile);
    unlink(donefile);

    /* start server B first so its URI is ready before the tool runs */
    bpid = spawn_server_b(self, urifile);
    if (bpid < 0) {
        fprintf(stderr, "failed to spawn server B: %s\n", strerror(errno));
        return 1;
    }

    /* bring up server A (scheduler + tool support) */
    PMIX_INFO_LOAD(&info[0], PMIX_SERVER_TOOL_SUPPORT, NULL, PMIX_BOOL);
    PMIX_INFO_LOAD(&info[1], PMIX_SERVER_SCHEDULER, NULL, PMIX_BOOL);
    rc = PMIx_server_init(&mymodule, info, 2);
    PMIX_INFO_DESTRUCT(&info[0]);
    PMIX_INFO_DESTRUCT(&info[1]);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "server A PMIx_server_init failed: %s\n", PMIx_Error_string(rc));
        kill(bpid, SIGKILL);
        waitpid(bpid, &wstatus, 0);
        return 1;
    }

    /* wait (bounded) for server B to publish its URI */
    burl = NULL;
    for (i = 0; i < 400; i++) { /* up to ~20s */
        burl = read_uri(urifile);
        if (NULL != burl) {
            break;
        }
        nanosleep(&ts, NULL);
    }
    if (NULL == burl) {
        fprintf(stderr, "server B never published a URI\n");
        kill(bpid, SIGKILL);
        waitpid(bpid, &wstatus, 0);
        PMIx_server_finalize();
        return 1;
    }

    /* run the tool child */
    tpid = spawn_tool(self, ncycles, burl, environ);
    if (tpid < 0) {
        fprintf(stderr, "failed to spawn tool child\n");
        kill(bpid, SIGKILL);
        waitpid(bpid, &wstatus, 0);
        PMIx_server_finalize();
        return 1;
    }
    toolrc = 1;
    if (tpid == waitpid(tpid, &wstatus, 0)) {
        if (WIFEXITED(wstatus) && 0 == WEXITSTATUS(wstatus)) {
            toolrc = 0;
        } else {
            fprintf(stderr, "tool child exited abnormally (status %d)\n", wstatus);
        }
    }

    /* tell server B to shut down, then reap it */
    fp = fopen(donefile, "w");
    if (NULL != fp) {
        fclose(fp);
    }
    for (i = 0; i < 200; i++) {
        if (bpid == waitpid(bpid, &wstatus, WNOHANG)) {
            break;
        }
        nanosleep(&ts, NULL);
    }
    if (0 == i) {
        /* already reaped above */
    } else if (200 == i) {
        kill(bpid, SIGKILL);
        waitpid(bpid, &wstatus, 0);
    }

    unlink(urifile);
    unlink(donefile);

    PMIx_server_finalize();

    if (0 == toolrc) {
        fprintf(stdout, "\ntool multi-server switch: PASS\n\n");
    } else {
        fprintf(stdout, "\ntool multi-server switch: FAIL\n\n");
    }
    return toolrc;
}

int main(int argc, char **argv)
{
    long ncycles = DEFAULT_CYCLES;

    if (2 < argc && 0 == strcmp(argv[1], "--tool-child")) {
        char *burl = getenv("PMIX_TEST_SERVERB_URI");
        ncycles = strtol(argv[2], NULL, 10);
        if (0 >= ncycles) {
            ncycles = DEFAULT_CYCLES;
        }
        if (NULL == burl) {
            fprintf(stderr, "tool child: PMIX_TEST_SERVERB_URI not set\n");
            return 1;
        }
        return run_tool_child(ncycles, burl);
    }

    if (2 < argc && 0 == strcmp(argv[1], "--server-b")) {
        return run_server_b(argv[2]);
    }

    if (1 < argc) {
        ncycles = strtol(argv[1], NULL, 10);
        if (0 >= ncycles) {
            ncycles = DEFAULT_CYCLES;
        }
    }
    return run_orchestrator(argv[0], ncycles);
}
