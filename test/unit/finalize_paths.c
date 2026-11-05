/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * What PMIx_Finalize and PMIx_tool_finalize do and return.
 *
 * Clients are real processes, connected to a server in this one. Each
 * case's host answers the client's finalize its own way:
 *
 *   embedded barrier -> PMIx_Finalize given PMIX_EMBED_BARRIER runs a
 *                       fence first: the host's fence up-call is reached
 *   host error       -> the host answers the finalize with an error, and
 *                       PMIx_Finalize returns it
 *   no answer        -> the host never answers: PMIx_Finalize returns
 *                       PMIX_ERR_TIMEOUT once pmix_finalize_timeout runs out
 *   not supported    -> the host's client_finalized answers
 *                       PMIX_ERR_NOT_SUPPORTED, which is no failure:
 *                       PMIx_Finalize returns PMIX_SUCCESS
 *
 * and a tool, with no server:
 *
 *   in a handler     -> PMIx_tool_finalize called from an event handler,
 *                       on the progress thread, returns
 *                       PMIX_ERR_WOULD_BLOCK and finalizes nothing; called
 *                       afterwards from the main thread it succeeds
 *
 * Every child is forked before any PMIx call, and is handed its
 * environment through a pipe.
 */

#include "src/include/pmix_config.h"

#include "include/pmix.h"
#include "include/pmix_server.h"
#include "include/pmix_tool.h"
#include "src/include/pmix_globals.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define FP_NSPACE "finalize.paths"
#define NKIDS     4

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

static bool put(int fd, const void *buf, size_t len)
{
    return (ssize_t) len == write(fd, buf, len);
}

/* ---- the host ---- */

typedef enum {
    FIN_OK,        // answers at once
    FIN_ERROR,     // answers with an error
    FIN_SILENT,    // never answers
    FIN_UNSUPPORTED
} fin_mode_t;

static volatile fin_mode_t fin_mode = FIN_OK;
static volatile int nfences = 0;

static pmix_status_t client_connected2(const pmix_proc_t *proc, void *server_object,
                                       pmix_info_t info[], size_t ninfo,
                                       pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    PMIX_HIDE_UNUSED_PARAMS(proc, server_object, info, ninfo, cbfunc, cbdata);
    return PMIX_OPERATION_SUCCEEDED;
}

static pmix_status_t client_finalized(const pmix_proc_t *proc, void *server_object,
                                      pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    PMIX_HIDE_UNUSED_PARAMS(proc, server_object);

    switch (fin_mode) {
    case FIN_ERROR:
        cbfunc(PMIX_ERR_NO_PERMISSIONS, cbdata);
        return PMIX_SUCCESS;
    case FIN_SILENT:
        return PMIX_SUCCESS;
    case FIN_UNSUPPORTED:
        return PMIX_ERR_NOT_SUPPORTED;
    default:
        return PMIX_OPERATION_SUCCEEDED;
    }
}

static pmix_status_t fence_nb(const pmix_proc_t procs[], size_t nprocs, const pmix_info_t info[],
                              size_t ninfo, char *data, size_t ndata,
                              pmix_modex_cbfunc_t cbfunc, void *cbdata)
{
    PMIX_HIDE_UNUSED_PARAMS(procs, nprocs, info, ninfo, data, ndata);

    ++nfences;
    cbfunc(PMIX_SUCCESS, NULL, 0, cbdata, NULL, NULL);
    return PMIX_SUCCESS;
}

static pmix_server_module_t mymodule = {
    .client_connected2 = client_connected2,
    .client_finalized = client_finalized,
    .fence_nb = fence_nb
};

/* ---- the child: a client ---- */

typedef struct {
    pid_t pid;
    int env;    // our end of the pipe its environment goes down
    int result; // our end of the pipe its finalize's status comes back on
} kid_t;

/* read "NAME=VALUE" lines, ended by an empty one, into the environment */
static bool read_env(int fd)
{
    char line[4096], *eq;
    size_t n = 0;
    char c;

    while (1 == read(fd, &c, 1)) {
        if ('\n' != c) {
            if (n < sizeof(line) - 1) {
                line[n++] = c;
            }
            continue;
        }
        line[n] = '\0';
        if (0 == n) {
            return true;
        }
        n = 0;
        eq = strchr(line, '=');
        if (NULL != eq) {
            *eq = '\0';
            setenv(line, eq + 1, 1);
        }
    }
    return false;
}

static int kid_main(int envfd, int resultfd)
{
    pmix_proc_t me;
    pmix_info_t info;
    pmix_status_t rc;
    bool flag = true;

    if (!read_env(envfd)) {
        return 1;
    }
    alarm(60);
    rc = PMIx_Init(&me, NULL, 0);
    if (PMIX_SUCCESS == rc) {
        if (NULL != getenv("FP_BARRIER")) {
            PMIX_INFO_LOAD(&info, PMIX_EMBED_BARRIER, &flag, PMIX_BOOL);
            rc = PMIx_Finalize(&info, 1);
            PMIX_INFO_DESTRUCT(&info);
        } else {
            rc = PMIx_Finalize(NULL, 0);
        }
    }
    return put(resultfd, &rc, sizeof(rc)) ? 0 : 1;
}

static kid_t *started[NKIDS];
static int nstarted = 0;

static bool start_kid(kid_t *k)
{
    int envp[2], resp[2], n;

    if (0 != pipe(envp) || 0 != pipe(resp)) {
        return false;
    }
    fflush(stdout);
    k->pid = fork();
    if (0 > k->pid) {
        return false;
    }
    if (0 == k->pid) {
        for (n = 0; n < nstarted; n++) {
            close(started[n]->env);
            close(started[n]->result);
        }
        close(envp[1]);
        close(resp[0]);
        _exit(kid_main(envp[0], resp[1]));
    }
    close(envp[0]);
    close(resp[1]);
    k->env = envp[1];
    k->result = resp[0];
    started[nstarted++] = k;
    return true;
}

static void send_env(kid_t *k, char **env)
{
    size_t n;
    char nl = '\n';

    for (n = 0; NULL != env && NULL != env[n]; n++) {
        if (!put(k->env, env[n], strlen(env[n])) || !put(k->env, &nl, 1)) {
            break;
        }
    }
    (void) put(k->env, &nl, 1);
    close(k->env);
}

/* run a client as rank 0 with the environment setup_fork gives it, plus
 * `extra` (NAME=VALUE, or NULL), and return what its finalize returned */
static pmix_status_t run_client(kid_t *k, const char *extra, const char *extra2)
{
    pmix_proc_t proc;
    char **env = NULL;
    pmix_status_t rc = PMIX_ERROR;
    int status;

    PMIX_LOAD_PROCID(&proc, FP_NSPACE, 0);
    if (PMIX_SUCCESS != PMIx_server_setup_fork(&proc, &env)) {
        return PMIX_ERROR;
    }
    if (NULL != extra) {
        PMIx_Argv_append_nosize(&env, extra);
    }
    if (NULL != extra2) {
        PMIx_Argv_append_nosize(&env, extra2);
    }
    send_env(k, env);
    PMIx_Argv_free(env);
    if (sizeof(rc) != read(k->result, &rc, sizeof(rc))) {
        rc = PMIX_ERROR;
    }
    close(k->result);
    waitpid(k->pid, &status, 0);
    return rc;
}

/* ---- the tool ---- */

static volatile bool hdlr_ran = false;
static volatile pmix_status_t hdlr_rc = PMIX_SUCCESS;

static void finalize_in_handler(size_t evhdlr_registration_id, pmix_status_t status,
                                const pmix_proc_t *source, pmix_info_t info[], size_t ninfo,
                                pmix_info_t *results, size_t nresults,
                                pmix_event_notification_cbfunc_fn_t cbfunc, void *cbdata)
{
    PMIX_HIDE_UNUSED_PARAMS(evhdlr_registration_id, status, source, info, ninfo, results,
                            nresults);

    hdlr_rc = PMIx_tool_finalize();
    hdlr_ran = true;
    if (NULL != cbfunc) {
        cbfunc(PMIX_EVENT_ACTION_COMPLETE, NULL, 0, NULL, NULL, cbdata);
    }
}

static int tool_main(int resultfd)
{
    pmix_proc_t me;
    pmix_info_t info;
    pmix_status_t rc, code = PMIX_EXTERNAL_ERR_BASE - 77, out[3];
    bool flag = true;
    int n;

    alarm(60);
    PMIX_INFO_LOAD(&info, PMIX_TOOL_DO_NOT_CONNECT, &flag, PMIX_BOOL);
    rc = PMIx_tool_init(&me, &info, 1);
    PMIX_INFO_DESTRUCT(&info);
    if (PMIX_SUCCESS != rc) {
        out[0] = rc;
        out[1] = out[2] = PMIX_ERROR;
        return put(resultfd, out, sizeof(out)) ? 0 : 1;
    }
    (void) PMIx_Register_event_handler(&code, 1, NULL, 0, finalize_in_handler, NULL, NULL);
    (void) PMIx_Notify_event(code, &me, PMIX_RANGE_PROC_LOCAL, NULL, 0, NULL, NULL);
    for (n = 0; n < 200 && !hdlr_ran; n++) {
        usleep(10000);
    }
    out[0] = hdlr_ran ? hdlr_rc : PMIX_ERR_TIMEOUT;
    out[1] = PMIx_Initialized() ? PMIX_SUCCESS : PMIX_ERR_INIT;
    out[2] = PMIx_tool_finalize();
    return put(resultfd, out, sizeof(out)) ? 0 : 1;
}

int main(int argc, char **argv)
{
    kid_t kids[NKIDS];
    pmix_proc_t proc;
    pmix_nspace_t ns;
    pmix_status_t rc, out[3];
    pmix_info_t jinfo[2];
    uint32_t one = 1;
    char detail[256];
    int n, resp[2], status;
    pid_t tool;
    PMIX_HIDE_UNUSED_PARAMS(argc, argv);

    fprintf(stdout, "\n=== what finalize does and returns ===\n\n");
    signal(SIGPIPE, SIG_IGN);

    /* the tool first, in a child of its own */
    if (0 != pipe(resp)) {
        return 1;
    }
    fflush(stdout);
    tool = fork();
    if (0 == tool) {
        close(resp[0]);
        _exit(tool_main(resp[1]));
    }
    close(resp[1]);
    for (n = 0; n < NKIDS; n++) {
        if (!start_kid(&kids[n])) {
            fprintf(stderr, "could not start the children\n");
            return 1;
        }
    }
    if (sizeof(out) != read(resp[0], out, sizeof(out))) {
        out[0] = out[1] = out[2] = PMIX_ERROR;
    }
    close(resp[0]);
    waitpid(tool, &status, 0);
    snprintf(detail, sizeof(detail), "in the handler %s; still initialized %s; afterwards %s",
             PMIx_Error_string(out[0]), PMIx_Error_string(out[1]), PMIx_Error_string(out[2]));
    report("a tool's finalize from an event handler is refused, and finalizes nothing",
           PMIX_ERR_WOULD_BLOCK == out[0] && PMIX_SUCCESS == out[1], detail);
    report("...and the tool finalizes afterwards", PMIX_SUCCESS == out[2], detail);

    /* a fence of local processes alone completes without the host; this
     * one is to reach it, so the fence is seen */
    setenv("PMIX_MCA_pmix_server_fence_localonly_opt", "0", 1);
    rc = PMIx_server_init(&mymodule, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "PMIx_server_init failed: %s\n", PMIx_Error_string(rc));
        return 1;
    }
    PMIX_LOAD_NSPACE(ns, FP_NSPACE);
    /* one process, on this node: a fence over the job waits for it alone */
    PMIX_INFO_LOAD(&jinfo[0], PMIX_JOB_SIZE, &one, PMIX_UINT32);
    PMIX_INFO_LOAD(&jinfo[1], PMIX_LOCAL_SIZE, &one, PMIX_UINT32);
    rc = PMIx_server_register_nspace(ns, 1, jinfo, 2, NULL, NULL);
    PMIX_INFO_DESTRUCT(&jinfo[0]);
    PMIX_INFO_DESTRUCT(&jinfo[1]);
    PMIX_LOAD_PROCID(&proc, FP_NSPACE, 0);
    if (PMIX_SUCCESS == rc || PMIX_OPERATION_SUCCEEDED == rc) {
        rc = PMIx_server_register_client(&proc, geteuid(), getegid(), NULL, NULL, NULL);
    }
    if (PMIX_SUCCESS != rc && PMIX_OPERATION_SUCCEEDED != rc) {
        fprintf(stderr, "registration failed: %s\n", PMIx_Error_string(rc));
        return 1;
    }

    fin_mode = FIN_OK;
    nfences = 0;
    rc = run_client(&kids[0], "FP_BARRIER=1", NULL);
    snprintf(detail, sizeof(detail), "finalize %s, %d fences", PMIx_Error_string(rc), nfences);
    report("an embedded barrier reaches the host's fence", PMIX_SUCCESS == rc && 1 == nfences,
           detail);

    fin_mode = FIN_ERROR;
    rc = run_client(&kids[1], NULL, NULL);
    snprintf(detail, sizeof(detail), "finalize %s", PMIx_Error_string(rc));
    report("the host's error answer is what finalize returns", PMIX_ERR_NO_PERMISSIONS == rc,
           detail);

    fin_mode = FIN_SILENT;
    rc = run_client(&kids[2], "PMIX_MCA_pmix_finalize_timeout=1", NULL);
    snprintf(detail, sizeof(detail), "finalize %s", PMIx_Error_string(rc));
    report("a host that never answers: finalize returns PMIX_ERR_TIMEOUT",
           PMIX_ERR_TIMEOUT == rc, detail);

    fin_mode = FIN_UNSUPPORTED;
    rc = run_client(&kids[3], NULL, NULL);
    snprintf(detail, sizeof(detail), "finalize %s", PMIx_Error_string(rc));
    report("a host whose client_finalized is not supported: finalize succeeds",
           PMIX_SUCCESS == rc, detail);

    PMIx_server_finalize();
    fprintf(stdout, "\n%d passed, %d failed\n", npass, nfail);
    return (0 == nfail) ? 0 : 1;
}
