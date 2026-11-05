/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * What a tool makes of the values it is handed for its identity and its
 * keepalive pipe, and the pause between connect attempts.
 *
 *   PMIX_RANK "abc"           -> PMIx_tool_init fails, PMIX_ERR_BAD_PARAM,
 *                                rather than taking rank 0
 *   PMIX_TOOL_RANK a string   -> PMIX_ERR_BAD_PARAM
 *   PMIX_KEEPALIVE_PIPE "abc" -> the init goes on, and stdin - descriptor 0,
 *                                what the value used to be read as - is
 *                                left as it was
 *   the connect back-off      -> 10 ms, doubling, at most 1 s
 *
 * Each tool runs in a forked child, initialized with
 * PMIX_TOOL_DO_NOT_CONNECT.
 */

#include "src/include/pmix_config.h"

#include "include/pmix.h"
#include "include/pmix_tool.h"
#include "src/mca/ptl/base/base.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

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

/* the case a child runs */
#define CASE_RANK_ENV  0
#define CASE_TOOL_RANK 1
#define CASE_KEEPALIVE 2

typedef struct {
    pmix_status_t rc;
    int stdin_flags_before;
    int stdin_flags_after;
} result_t;

static void child(int which, int wfd)
{
    pmix_info_t info[3];
    pmix_proc_t me;
    result_t res;
    size_t ninfo = 1;

    memset(&res, 0, sizeof(res));
    PMIX_INFO_LOAD(&info[0], PMIX_TOOL_DO_NOT_CONNECT, NULL, PMIX_BOOL);
    if (CASE_RANK_ENV == which) {
        setenv("PMIX_NAMESPACE", "connect.inputs", 1);
        setenv("PMIX_RANK", "abc", 1);
    } else if (CASE_TOOL_RANK == which) {
        PMIX_INFO_LOAD(&info[1], PMIX_TOOL_NSPACE, "connect.inputs", PMIX_STRING);
        /* the rank, given as a string */
        PMIX_INFO_LOAD(&info[2], PMIX_TOOL_RANK, "7", PMIX_STRING);
        ninfo = 3;
    } else {
        setenv("PMIX_KEEPALIVE_PIPE", "abc", 1);
    }
    res.stdin_flags_before = fcntl(0, F_GETFD);
    res.rc = PMIx_tool_init(&me, info, ninfo);
    res.stdin_flags_after = fcntl(0, F_GETFD);
    if (sizeof(res) != write(wfd, &res, sizeof(res))) {
        _exit(1);
    }
    if (PMIX_SUCCESS == res.rc) {
        PMIx_tool_finalize();
    }
    _exit(0);
}

static bool run(int which, result_t *res)
{
    int fds[2], status;
    pid_t pid;
    bool ok;

    if (0 != pipe(fds)) {
        return false;
    }
    fflush(stdout);
    pid = fork();
    if (0 == pid) {
        close(fds[0]);
        alarm(60);
        child(which, fds[1]);
    }
    close(fds[1]);
    ok = (sizeof(*res) == read(fds[0], res, sizeof(*res)));
    close(fds[0]);
    waitpid(pid, &status, 0);
    return ok;
}

int main(void)
{
    result_t res;
    struct timeval tv;
    char detail[256];
    long ms[12];
    int n;
    bool ok;

    fprintf(stdout, "\n=== a tool's inputs, and the pause between connect attempts ===\n\n");

    ok = run(CASE_RANK_ENV, &res);
    snprintf(detail, sizeof(detail), "init %s", ok ? PMIx_Error_string(res.rc) : "no report");
    report("a PMIX_RANK that is not a number fails the tool's init",
           ok && PMIX_ERR_BAD_PARAM == res.rc, detail);

    ok = run(CASE_TOOL_RANK, &res);
    snprintf(detail, sizeof(detail), "init %s", ok ? PMIx_Error_string(res.rc) : "no report");
    report("a PMIX_TOOL_RANK given as a string fails the tool's init",
           ok && PMIX_ERR_BAD_PARAM == res.rc, detail);

    ok = run(CASE_KEEPALIVE, &res);
    snprintf(detail, sizeof(detail), "init %s; stdin flags %d before, %d after",
             ok ? PMIx_Error_string(res.rc) : "no report", ok ? res.stdin_flags_before : -1,
             ok ? res.stdin_flags_after : -1);
    report("a PMIX_KEEPALIVE_PIPE that names no descriptor leaves stdin alone",
           ok && PMIX_SUCCESS == res.rc && res.stdin_flags_before == res.stdin_flags_after,
           detail);

    for (n = 1; n <= 10; n++) {
        pmix_ptl_base_connect_backoff(n, &tv);
        ms[n] = tv.tv_sec * 1000L + tv.tv_usec / 1000L;
    }
    snprintf(detail, sizeof(detail), "%ld %ld %ld %ld %ld %ld %ld %ld ms", ms[1], ms[2], ms[3],
             ms[4], ms[7], ms[8], ms[9], ms[10]);
    report("the pause between connect attempts starts at 10 ms, doubles, and stops at 1 s",
           10 == ms[1] && 20 == ms[2] && 40 == ms[3] && 80 == ms[4] && 640 == ms[7] &&
               1000 == ms[8] && 1000 == ms[10],
           detail);

    fprintf(stdout, "\n%d passed, %d failed\n", npass, nfail);
    return (0 == nfail) ? 0 : 1;
}
