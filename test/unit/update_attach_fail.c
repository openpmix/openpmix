/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/*
 * A segment that arrives AFTER PMIx_Init and cannot be mapped must cost
 * only itself.
 *
 * gds/shmem3 hands a client the address of each segment it must map, and
 * the client has to map it at exactly that address. That can fail: the
 * address is chosen by the server, and the client is a different process
 * whose address space has moved on since it started.
 *
 * When it fails during PMIx_Init the client has a move to make - it
 * abandons the module, switches to gds/hash and re-requests its job data
 * - and dropping the job tracker is part of that. After PMIx_Init there
 * is no such move: an update to a session's description arrives on a
 * one-way notification with no re-request behind it, and a modex cannot
 * be re-delivered in another module's format either. So a failure there
 * has to cost only the segment that failed.
 *
 * It did not. The cleanup keyed off the SEGMENT ID rather than off how
 * the segment arrived, so only the modex was spared; a session update
 * that failed to map took the whole job tracker down with it - and that
 * tracker owns the job and session segments the client had been reading
 * happily since PMIx_Init. Every job-level lookup for the client's OWN
 * namespace then failed. That is openpmix#4156, which was found and
 * fixed for the modex; this is the same failure reached by a different
 * delivery.
 *
 * WHAT THIS ASSERTS, and why it needs PMIX_OPTIONAL.
 *
 * A plain PMIx_Get hides the bug. With the tracker gone the local lookup
 * misses, the request goes up to the server, and the server answers
 * correctly - so the application sees the right value and pays a round
 * trip. PMIX_OPTIONAL is what confines the question to the client's own
 * datastore: after the failed attach the client must still be able to
 * read its own job-level data WITHOUT asking anyone. Before the fix that
 * read fails, because there is no tracker left to read from.
 *
 * The stale session value is NOT a failure and is not asserted against.
 * A client that could not map the update is entitled to go on reading
 * what it mapped before - that is the degradation, and it is the point.
 *
 * ON gds/hash this runs and passes without exercising anything: the MCA
 * parameter belongs to shmem3 and hash has no segments to fail. That is
 * deliberate rather than a gap - the test is written against behavior
 * every module owes, so it is honest on macOS instead of absent.
 */

#include "src/include/pmix_config.h"

#include "include/pmix.h"
#include "include/pmix_server.h"
#include "src/include/pmix_globals.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

#define NSPACE   "upd-fail"
#define SESSION  8765
#define FIRST    8
#define SECOND   64

static int npass = 0, nfail = 0;

static void report(const char *what, bool ok)
{
    fprintf(stdout, "  %s: %s\n", ok ? "PASS" : "FAIL", what);
    if (ok) {
        npass++;
    } else {
        nfail++;
    }
}

/* ------------------------------------------------------------------ */
/* the client                                                          */
/* ------------------------------------------------------------------ */

/* Read a job-level key from THIS CLIENT'S OWN datastore.
 *
 * PMIX_OPTIONAL is the whole point: it tells the library not to ask the
 * server, so a success here means the client still holds the job data it
 * mapped at PMIx_Init. That is exactly what a dropped job tracker takes
 * away, and exactly what a round trip to the server would hide.
 */
static int read_local_job_size(uint32_t *out)
{
    pmix_proc_t proc;
    pmix_value_t *val = NULL;
    pmix_info_t opt;
    pmix_status_t rc;

    PMIX_LOAD_PROCID(&proc, NSPACE, PMIX_RANK_WILDCARD);
    PMIX_INFO_LOAD(&opt, PMIX_OPTIONAL, NULL, PMIX_BOOL);
    rc = PMIx_Get(&proc, PMIX_JOB_SIZE, &opt, 1, &val);
    PMIX_INFO_DESTRUCT(&opt);
    if (PMIX_SUCCESS != rc || NULL == val) {
        fprintf(stderr, "  client: local PMIx_Get(JOB_SIZE) failed: %s\n",
                PMIx_Error_string(rc));
        return -1;
    }
    rc = PMIx_Value_get_number(val, out, PMIX_UINT32);
    PMIX_VALUE_RELEASE(val);
    return (PMIX_SUCCESS == rc) ? 0 : -1;
}

static int run_client(int readyfd, int gofd)
{
    pmix_proc_t myproc;
    pmix_status_t rc;
    uint32_t sz = 0;
    char c;

    rc = PMIx_Init(&myproc, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "  client: PMIx_Init failed: %s\n", PMIx_Error_string(rc));
        return 1;
    }

    /* before the update: the client obviously holds its own job data */
    if (0 != read_local_job_size(&sz) || 1 != sz) {
        fprintf(stderr, "  client: job data unreadable BEFORE the update\n");
        PMIx_Finalize(NULL, 0);
        return 1;
    }

    /* tell the server we are up, then wait to be released */
    c = 'r';
    if (1 != write(readyfd, &c, 1)) {
        PMIx_Finalize(NULL, 0);
        return 1;
    }
    if (1 != read(gofd, &c, 1)) {
        PMIx_Finalize(NULL, 0);
        return 1;
    }

    /* The update has been published and the notice sent. Give the
     * progress thread a moment to take delivery of it - the pipe that
     * released us and the socket the notice travels on are different
     * channels, so without this we could run ahead of the very failure
     * we are testing and pass for the wrong reason.
     *
     * There is nothing to poll for here, unlike session_update.c: a
     * client that could not map the update goes on reading the value it
     * already had, so no observable value changes. Sleeping is the
     * honest way to wait for something that is meant to leave no trace. */
    usleep(500000);

    /* THE ASSERTION. The update failed to map - that is what the MCA
     * parameter forced - and the client must still be able to read its
     * own job-level data out of its own store. */
    if (0 != read_local_job_size(&sz)) {
        fprintf(stderr, "  client: job data unreadable AFTER the failed "
                        "update attach - the job tracker went with it\n");
        PMIx_Finalize(NULL, 0);
        return 2;
    }
    if (1 != sz) {
        fprintf(stderr, "  client: job size read back as %u, expected 1\n",
                (unsigned) sz);
        PMIx_Finalize(NULL, 0);
        return 3;
    }
    fprintf(stdout, "  client: own job data still readable after the "
                    "failed update attach\n");

    PMIx_Finalize(NULL, 0);
    return 0;
}

/* ------------------------------------------------------------------ */
/* the server                                                          */
/* ------------------------------------------------------------------ */

static volatile bool regdone = false;
static void regcbfunc(pmix_status_t status, void *cbdata)
{
    (void) status;
    (void) cbdata;
    regdone = true;
}

static pmix_server_module_t mymodule = {0};

static pmix_status_t register_job(void)
{
    pmix_info_t info[5], *sptr;
    pmix_data_array_t *array;
    pmix_proc_t proc;
    pmix_nspace_t ns;
    pmix_status_t rc;
    char *noderegex = NULL, *ppnregex = NULL;
    uint32_t nprocs = 1, univ = FIRST, sid = SESSION;
    int i;

    PMIx_generate_regex(pmix_globals.hostname, &noderegex);
    PMIx_generate_ppn("0", &ppnregex);
    PMIX_INFO_LOAD(&info[0], PMIX_NODE_MAP, noderegex, PMIX_REGEX);
    PMIX_INFO_LOAD(&info[1], PMIX_PROC_MAP, ppnregex, PMIX_REGEX);
    PMIX_INFO_LOAD(&info[2], PMIX_JOB_SIZE, &nprocs, PMIX_UINT32);
    PMIX_INFO_LOAD(&info[3], PMIX_MAX_PROCS, &nprocs, PMIX_UINT32);

    /* a session array, so there is a session segment for the update
     * below to publish a successor to */
    PMIX_INFO_CREATE(sptr, 2);
    PMIX_INFO_LOAD(&sptr[0], PMIX_SESSION_ID, &sid, PMIX_UINT32);
    PMIX_INFO_LOAD(&sptr[1], PMIX_UNIV_SIZE, &univ, PMIX_UINT32);
    PMIX_DATA_ARRAY_CREATE(array, 2, PMIX_INFO);
    memcpy(array->array, sptr, 2 * sizeof(pmix_info_t));
    free(sptr);
    PMIX_LOAD_KEY(info[4].key, PMIX_SESSION_INFO_ARRAY);
    info[4].value.type = PMIX_DATA_ARRAY;
    info[4].value.data.darray = array;

    PMIX_LOAD_NSPACE(ns, NSPACE);
    rc = PMIx_server_register_nspace(ns, 1, info, 5, NULL, NULL);
    for (i = 0; i < 5; i++) {
        PMIX_INFO_DESTRUCT(&info[i]);
    }
    free(noderegex);
    free(ppnregex);
    if (PMIX_OPERATION_SUCCEEDED == rc) {
        rc = PMIX_SUCCESS;
    }
    if (PMIX_SUCCESS != rc) {
        return rc;
    }

    PMIX_LOAD_PROCID(&proc, NSPACE, 0);
    rc = PMIx_server_register_client(&proc, geteuid(), getegid(), NULL,
                                     regcbfunc, NULL);
    if (PMIX_OPERATION_SUCCEEDED == rc) {
        return PMIX_SUCCESS;
    }
    if (PMIX_SUCCESS != rc) {
        return rc;
    }
    for (i = 0; i < 400 && !regdone; i++) {
        usleep(50000);
    }
    return PMIX_SUCCESS;
}

int main(int argc, char **argv)
{
    pmix_info_t sinfo, upd;
    pmix_proc_t proc;
    pmix_status_t rc;
    char **client_env = NULL;
    char *client_argv[5];
    char fdbuf[32];
    int readypipe[2], gopipe[2], status = 0;
    pid_t child;
    bool flag = true;
    uint32_t grown = SECOND;
    char c;
    bool completed = false;

    setvbuf(stdout, NULL, _IOLBF, 0);

    if (1 < argc && 0 == strcmp(argv[1], "client")) {
        return run_client(atoi(argv[2]), atoi(argv[3]));
    }

    fprintf(stdout, "=== a failed update attach costs only that segment ===\n");

    if (0 != pipe(readypipe) || 0 != pipe(gopipe)) {
        fprintf(stderr, "pipe() failed\n");
        return 1;
    }

    PMIX_INFO_LOAD(&sinfo, PMIX_SERVER_TOOL_SUPPORT, &flag, PMIX_BOOL);
    rc = PMIx_server_init(&mymodule, &sinfo, 1);
    PMIX_INFO_DESTRUCT(&sinfo);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "PMIx_server_init failed: %s\n", PMIx_Error_string(rc));
        return 1;
    }

    rc = register_job();
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "could not register the job: %s\n", PMIx_Error_string(rc));
        goto done;
    }

    PMIX_LOAD_PROCID(&proc, NSPACE, 0);
    /* Seed the child's environment with our own first - see the note in
     * event_forward.c: without it the child runs against the INSTALLED
     * PMIx rather than the one under test. */
    client_env = PMIx_Argv_copy(environ);
    rc = PMIx_server_setup_fork(&proc, &client_env);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "PMIx_server_setup_fork failed: %s\n",
                PMIx_Error_string(rc));
        goto done;
    }
    /* Force the client's attach of the update to fail.
     *
     * This is the only way to reach the case from a test: whether a
     * fixed-address map succeeds depends on the client's own virtual
     * memory layout, which nothing here controls. The neighbouring
     * force_client_attach_failure cannot stand in - it fails the attach
     * during PMIx_Init too, so the client leaves PMIx_Init on hash and
     * never takes delivery of an update on shmem3 at all.
     *
     * It is set only in the CHILD's environment; this server has to go
     * on mapping its own segments. */
    PMIx_Argv_append_nosize(
        &client_env, "PMIX_MCA_gds_shmem3_force_update_attach_failure=1"
    );

    child = fork();
    if (0 > child) {
        fprintf(stderr, "fork() failed\n");
        goto done;
    }
    if (0 == child) {
        /* exec rather than run in the fork: this process has an
         * initialized PMIx server in it */
        close(readypipe[0]);
        close(gopipe[1]);
        client_argv[0] = argv[0];
        client_argv[1] = (char *) "client";
        snprintf(fdbuf, sizeof(fdbuf), "%d", readypipe[1]);
        client_argv[2] = strdup(fdbuf);
        snprintf(fdbuf, sizeof(fdbuf), "%d", gopipe[0]);
        client_argv[3] = strdup(fdbuf);
        client_argv[4] = NULL;
        execve(argv[0], client_argv, client_env);
        fprintf(stderr, "exec of %s failed\n", argv[0]);
        _exit(127);
    }
    close(readypipe[1]);
    close(gopipe[0]);

    /* wait until the client is up and reading its own job data */
    if (1 != read(readypipe[0], &c, 1)) {
        fprintf(stderr, "the client never reported that it was up\n");
        goto done;
    }
    report("a running client reads its own job data", true);

    /* publish a session update, which the client is rigged to fail to map */
    PMIX_INFO_LOAD(&upd, PMIX_UNIV_SIZE, &grown, PMIX_UINT32);
    rc = PMIx_server_register_session(SESSION, &upd, 1, NULL, NULL);
    PMIX_INFO_DESTRUCT(&upd);
    report("the host publishes a session update", PMIX_SUCCESS == rc);

    /* release the client to read again */
    c = 'g';
    if (1 != write(gopipe[1], &c, 1)) {
        fprintf(stderr, "could not release the client\n");
        goto done;
    }

    waitpid(child, &status, 0);
    if (WIFEXITED(status)) {
        report("the client still holds its own job data afterwards",
               0 == WEXITSTATUS(status));
        if (0 != WEXITSTATUS(status)) {
            fprintf(stdout, "        (client exited %d)\n", WEXITSTATUS(status));
        }
    } else {
        report("the client still holds its own job data afterwards", false);
        fprintf(stdout, "        (client died on a signal)\n");
    }

    completed = true;

done:
    if (!completed) {
        /* Every early exit above lands here, and most of them happen
         * BEFORE the case has reported anything - so without this the
         * program returns 0 having asserted nothing. That is not a
         * hypothetical: the client exiting at its first failed check
         * makes the parent's next read() fail, which jumps here, and
         * the run then printed the mismatch and still exited zero. A
         * test that cannot fail is worse than no test. */
        report("the exchange ran to completion", false);
    }
    /* the parent's copy of the child's environment - the child got its
     * own through execve */
    if (NULL != client_env) {
        PMIx_Argv_free(client_env);
    }
    PMIx_server_finalize();
    fprintf(stdout, "\n=== %d passed, %d failed ===\n", npass, nfail);
    return (0 == nfail) ? 0 : 1;
}
