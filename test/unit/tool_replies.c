/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/*
 * The query tools read what their server answers.
 *
 * pps and pquery each ask their server a question and print the
 * answer. What comes back is whatever the host put there, so each tool
 * checks the answer's type - and a data array's element type - before
 * reading it as the type it expected, and copes with a list that splits
 * into nothing.
 *
 * This is a PMIx server whose query upcall answers with the wrong shape:
 * a namespace list of nothing but separators, and a process table that
 * is an array of bytes. Each tool is run against it with --uri, and must
 * exit rather than die on a signal.
 */

#include "src/include/pmix_config.h"

#include "include/pmix.h"
#include "include/pmix_server.h"
#include "src/mca/ptl/base/base.h"

#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define TOOL_NSPACE "tool-replies-tool"
#define NBYTES      4096

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
    fflush(stdout);
}

/* what the host answers a namespace query with */
static const char *nspace_answer = ",";

static void tool_connected_fn(pmix_info_t *info, size_t ninfo,
                              pmix_tool_connection_cbfunc_t cbfunc, void *cbdata)
{
    pmix_proc_t proc;
    PMIX_HIDE_UNUSED_PARAMS(info, ninfo);

    PMIX_LOAD_PROCID(&proc, TOOL_NSPACE, 0);
    cbfunc(PMIX_SUCCESS, &proc, cbdata);
}

static void release_answer(void *cbdata)
{
    pmix_info_t *info = (pmix_info_t *) cbdata;

    PMIX_INFO_FREE(info, 1);
}

/* a data array of bytes, every one 'A' - read as anything holding a
 * pointer, it is the address 0x4141414141414141 */
static void load_bytes(pmix_info_t *info, const char *key)
{
    pmix_data_array_t darray;
    unsigned char *bytes;

    PMIX_DATA_ARRAY_CONSTRUCT(&darray, NBYTES, PMIX_UINT8);
    bytes = (unsigned char *) darray.array;
    memset(bytes, 'A', NBYTES);
    PMIX_INFO_LOAD(info, key, &darray, PMIX_DATA_ARRAY);
    PMIX_DATA_ARRAY_DESTRUCT(&darray);
}

static pmix_status_t query_fn(pmix_proc_t *proct, pmix_query_t *queries, size_t nqueries,
                              pmix_info_cbfunc_t cbfunc, void *cbdata)
{
    pmix_info_t *info;
    const char *key;
    PMIX_HIDE_UNUSED_PARAMS(proct);

    if (0 == nqueries || NULL == queries[0].keys || NULL == queries[0].keys[0]) {
        return PMIX_ERR_BAD_PARAM;
    }
    key = queries[0].keys[0];
    PMIX_INFO_CREATE(info, 1);

    if (0 == strcmp(key, PMIX_QUERY_NAMESPACES)) {
        PMIX_INFO_LOAD(info, key, nspace_answer, PMIX_STRING);
    } else if (0 == strcmp(key, PMIX_QUERY_PROC_TABLE)) {
        load_bytes(info, key);
    } else {
        PMIX_INFO_LOAD(info, key, ",", PMIX_STRING);
    }
    cbfunc(PMIX_SUCCESS, info, 1, cbdata, release_answer, info);
    return PMIX_SUCCESS;
}

static pmix_server_module_t mymodule = {
    .tool_connected = tool_connected_fn,
    .query = query_fn
};

/* run one tool against us and say whether it exited on its own */
static void run_tool(const char *label, const char *uri, const char *tool,
                     const char *arg1, const char *arg2)
{
    char path[1024];
    pid_t pid, r;
    int status = 0, i, fd;
    char detail[128];

    snprintf(path, sizeof(path), "%s/%s/%s", PMIX_TEST_TOOLS_DIR, tool, tool);
    if (0 != access(path, X_OK)) {
        report(label, 0, "tool not built");
        return;
    }
    pid = fork();
    if (0 > pid) {
        report(label, 0, "fork failed");
        return;
    }
    if (0 == pid) {
        fd = open("/dev/null", O_RDWR);
        if (0 <= fd) {
            dup2(fd, STDIN_FILENO);
            dup2(fd, STDOUT_FILENO);
            dup2(fd, STDERR_FILENO);
        }
        execl(path, tool, "--uri", uri, arg1, arg2, (char *) NULL);
        _exit(126);
    }
    for (i = 0; i < 600; i++) {
        r = waitpid(pid, &status, WNOHANG);
        if (pid == r) {
            break;
        }
        usleep(100000);
    }
    if (600 == i) {
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
        report(label, 0, "timed out");
        return;
    }
    if (WIFSIGNALED(status)) {
        snprintf(detail, sizeof(detail), "died on signal %d", WTERMSIG(status));
        report(label, 0, detail);
    } else if (WIFEXITED(status) && 126 == WEXITSTATUS(status)) {
        report(label, 0, "could not exec the tool");
    } else {
        report(label, 1, NULL);
    }
}

int main(int argc, char **argv)
{
    pmix_status_t rc;
    pmix_info_t sinfo[2];
    char *uri;
    bool flag = true;
    PMIX_HIDE_UNUSED_PARAMS(argc, argv);

    fprintf(stdout, "\n=== tools against malformed query answers ===\n\n");

    PMIX_INFO_LOAD(&sinfo[0], PMIX_SERVER_TOOL_SUPPORT, &flag, PMIX_BOOL);
    PMIX_INFO_LOAD(&sinfo[1], PMIX_SERVER_NSPACE, "tool-replies-server", PMIX_STRING);
    rc = PMIx_server_init(&mymodule, sinfo, 2);
    PMIX_INFO_DESTRUCT(&sinfo[0]);
    PMIX_INFO_DESTRUCT(&sinfo[1]);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "PMIx_server_init failed: %s\n", PMIx_Error_string(rc));
        return 1;
    }
    uri = pmix_ptl_base.listener.uri;
    if (NULL == uri) {
        report("server published its URI", 0, "listener has no URI");
        PMIx_server_finalize();
        return 1;
    }

    nspace_answer = ",";
    run_tool("pquery survives a list that splits into nothing", uri, "pquery",
             PMIX_QUERY_NAMESPACES, NULL);
    run_tool("pps survives a namespace list that splits into nothing", uri, "pps",
             NULL, NULL);

    nspace_answer = "tool-replies-job";
    run_tool("pps survives a process table that is not one", uri, "pps", NULL, NULL);

    PMIx_server_finalize();

    fprintf(stdout, "\n%d passed, %d failed\n", npass, nfail);
    return (0 == nfail) ? 0 : 1;
}
