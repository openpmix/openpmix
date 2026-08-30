/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/*
 * A session's description changes while a client is running.
 *
 * A session's resources are not fixed: PMIX_SESSION_EXTEND grows one
 * "in terms of time or resources", PMIX_SESSION_PREEMPT takes them
 * back, a node goes down. A client that attached before such a change
 * holds whatever the session said at the time, and gds/shmem3 cannot
 * rewrite the segment it is reading - so the server publishes the
 * change as a new segment and tells its local clients, which map it and
 * read through the new one first.
 *
 * That last step - the telling - is the only part of the mechanism that
 * cannot be checked in one process, and it is the part most likely to
 * be wrong: it is a PTL message, a posted receive, and a module hand-off
 * on the far side of a socket. So this test does what event_forward.c
 * does, and for the same reason: it registers a job, forks, and EXECS
 * ITSELF as a real client, so there is a real client behind a real
 * socket to be told.
 *
 * The ordering is what makes it an assertion rather than a race. The
 * client reads the session size, reports it, and waits. Only then does
 * the server update the session. The client is released afterwards and
 * reads again - so a library that never delivered the notice would
 * return the first value, and the case fails rather than flaking.
 *
 * PMIX_GET_REFRESH_CACHE is deliberately NOT used on the second read.
 * The point is that the client's own datastore has been brought up to
 * date, not that it can be made to ask the server again.
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

#define NSPACE   "sess-upd"
#define SESSION  4321
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

static int read_univ_size(uint32_t *out)
{
    pmix_proc_t proc;
    pmix_value_t *val = NULL;
    pmix_status_t rc;

    PMIX_LOAD_PROCID(&proc, NSPACE, PMIX_RANK_WILDCARD);
    rc = PMIx_Get(&proc, PMIX_UNIV_SIZE, NULL, 0, &val);
    if (PMIX_SUCCESS != rc || NULL == val) {
        fprintf(stderr, "  client: PMIx_Get(UNIV_SIZE) failed: %s\n",
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

    if (0 != read_univ_size(&sz)) {
        PMIx_Finalize(NULL, 0);
        return 1;
    }
    fprintf(stdout, "  client: session size before the update = %u\n",
            (unsigned) sz);
    if (FIRST != sz) {
        fprintf(stderr, "  client: expected %u before the update\n",
                (unsigned) FIRST);
        PMIx_Finalize(NULL, 0);
        return 1;
    }

    /* tell the server we have read it, then wait to be released */
    c = 'r';
    if (1 != write(readyfd, &c, 1)) {
        PMIx_Finalize(NULL, 0);
        return 1;
    }
    if (1 != read(gofd, &c, 1)) {
        PMIx_Finalize(NULL, 0);
        return 1;
    }

    /* Poll until the notice has been applied, or give up.
     *
     * The pipe that released us and the socket the notice travels on are
     * different channels, so the release can arrive first - a single
     * read here would race the notice rather than test it.
     *
     * Polling rather than a blocking round trip to the server, which is
     * how event_forward.c orders its case: a round trip does not just
     * order things, it REFRESHES them. A PMIx_Query_info reply brings
     * the client's data up to date on its own, so with one in here the
     * case passed even with the notification compiled out - it was
     * measuring the query, not the notice. Checked, by doing exactly
     * that.
     *
     * A plain PMIx_Get for this key is answered out of the client's own
     * mapped segments and never reaches the server, so polling it cannot
     * fetch the answer by itself. If the notice never arrives the value
     * never changes and this gives up, which is the failure it should
     * report. */
    for (int i = 0; i < 200; i++) {
        if (0 != read_univ_size(&sz)) {
            PMIx_Finalize(NULL, 0);
            return 1;
        }
        if (SECOND == sz) {
            break;
        }
        usleep(25000);
    }
    fprintf(stdout, "  client: session size after the update = %u\n",
            (unsigned) sz);

    PMIx_Finalize(NULL, 0);
    /* the exit status carries the answer back to the parent */
    return (SECOND == sz) ? 0 : 2;
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

    /* the session array: id first, then its description */
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

    setvbuf(stdout, NULL, _IOLBF, 0);

    if (1 < argc && 0 == strcmp(argv[1], "client")) {
        return run_client(atoi(argv[2]), atoi(argv[3]));
    }

    fprintf(stdout, "=== session update reaches a running client ===\n");

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

    /* wait until the client has read the session size */
    if (1 != read(readypipe[0], &c, 1)) {
        fprintf(stderr, "the client never reported its first read\n");
        goto done;
    }
    report("a running client reads the session's original size", true);

    /* now change it under them */
    PMIX_INFO_LOAD(&upd, PMIX_UNIV_SIZE, &grown, PMIX_UINT32);
    rc = PMIx_server_register_session(SESSION, &upd, 1, NULL, NULL);
    PMIX_INFO_DESTRUCT(&upd);
    report("the host restates the session's size", PMIX_SUCCESS == rc);

    /* release the client to read again */
    c = 'g';
    if (1 != write(gopipe[1], &c, 1)) {
        fprintf(stderr, "could not release the client\n");
        goto done;
    }

    waitpid(child, &status, 0);
    if (WIFEXITED(status)) {
        report("the running client sees the updated size",
               0 == WEXITSTATUS(status));
        if (0 != WEXITSTATUS(status)) {
            fprintf(stdout, "        (client exited %d)\n", WEXITSTATUS(status));
        }
    } else {
        report("the running client sees the updated size", false);
        fprintf(stdout, "        (client died on a signal)\n");
    }

done:
    /* the parent's copy of the child's environment - the child got its
     * own through execve */
    if (NULL != client_env) {
        PMIx_Argv_free(client_env);
    }
    PMIx_server_finalize();
    fprintf(stdout, "\n=== %d passed, %d failed ===\n", npass, nfail);
    return (0 == nfail) ? 0 : 1;
}
