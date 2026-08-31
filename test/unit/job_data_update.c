/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/*
 * A host revises a running job's data, and the clients see it.
 *
 * PMIx_server_register_nspace() called with a NEGATIVE nlocalprocs is
 * "this is an update to a namespace you already hold", and it is the
 * recommended way for a host to revise job-level data after the job is
 * running. It could not do that:
 *
 *   - the update branch handled PMIX_PROC_INFO_ARRAY,
 *     PMIX_GROUP_CONTEXT_ID and PMIX_JOB_INFO_ARRAY and had no arm for
 *     anything else, so a plain job-level key was silently dropped;
 *   - a PMIX_JOB_INFO_ARRAY was skipped outright once the namespace had
 *     job info, which for a registered namespace is always;
 *   - and the two arms that did store used pmix_globals.mypeer's module
 *     - the server's own hash - so nothing reached the segments a
 *     gds/shmem3 client reads, and no client was told anything.
 *
 * So the API existed, returned success, and did nothing observable.
 *
 * WHAT THIS ASSERTS, and why it needs PMIX_OPTIONAL.
 *
 * The read after the update asks with PMIX_OPTIONAL, which tells the
 * library not to go to the server. That is the whole point: a plain
 * PMIx_Get would miss locally, ask the server, and be answered out of
 * the server's own copy - so it would pass whether or not the update
 * ever reached this client's datastore, which is the thing being
 * tested.
 *
 * It runs against whichever gds module is assigned, and both have to
 * pass it: on Linux that is normally shmem3, which delivers the update
 * by publishing a new segment and telling the client to map it, and on
 * macOS it is hash, which sends the values themselves. The two answering
 * a host differently is the state this is here to prevent.
 *
 * The second phase asserts the other half of the rule: restating a value
 * that has not changed must be harmless. It cannot be observed directly
 * from a client - "nothing was published" leaves no trace by design - so
 * what is checked is that the value is still right afterwards, which is
 * what a caller would notice if a restatement had gone wrong.
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

#define NSPACE  "job-upd"
#define LATEKEY "sut.late.key"
#define FIRST   11
#define SECOND  22

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

/* Read the key from THIS CLIENT'S OWN datastore.
 *
 * PMIX_OPTIONAL keeps the request in this process. Without it the
 * server answers and the test proves nothing. */
static int read_local(uint32_t *out)
{
    pmix_proc_t proc;
    pmix_value_t *val = NULL;
    pmix_info_t opt;
    pmix_status_t rc;

    PMIX_LOAD_PROCID(&proc, NSPACE, PMIX_RANK_WILDCARD);
    PMIX_INFO_LOAD(&opt, PMIX_OPTIONAL, NULL, PMIX_BOOL);
    rc = PMIx_Get(&proc, LATEKEY, &opt, 1, &val);
    PMIX_INFO_DESTRUCT(&opt);
    if (PMIX_SUCCESS != rc || NULL == val) {
        return -1;
    }
    rc = PMIx_Value_get_number(val, out, PMIX_UINT32);
    PMIX_VALUE_RELEASE(val);
    return (PMIX_SUCCESS == rc) ? 0 : -1;
}

/* Poll for the value to become "want".
 *
 * The pipe that releases the client and the socket the notice travels
 * on are different channels, so a single read would race the notice
 * rather than test it. A local read cannot fetch the answer by itself -
 * PMIX_OPTIONAL sees to that - so if the notice never arrives this
 * gives up and the case fails, which is the report it should make. */
static int await_local(uint32_t want, uint32_t *got)
{
    for (int i = 0; i < 200; i++) {
        if (0 == read_local(got) && want == *got) {
            return 0;
        }
        usleep(25000);
    }
    return -1;
}

static int run_client(int readyfd, int gofd)
{
    pmix_proc_t myproc;
    pmix_status_t rc;
    uint32_t v = 0;
    char c;

    rc = PMIx_Init(&myproc, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "  client: PMIx_Init failed: %s\n", PMIx_Error_string(rc));
        return 1;
    }

    if (0 != read_local(&v) || FIRST != v) {
        fprintf(stderr, "  client: expected %u before the update, got %u\n",
                (unsigned) FIRST, (unsigned) v);
        PMIx_Finalize(NULL, 0);
        return 1;
    }

    /* phase 1: the host changes the value */
    c = 'r';
    if (1 != write(readyfd, &c, 1) || 1 != read(gofd, &c, 1)) {
        PMIx_Finalize(NULL, 0);
        return 1;
    }
    if (0 != await_local(SECOND, &v)) {
        fprintf(stderr, "  client: never saw the updated value in its own "
                        "store (last read %u, wanted %u)\n",
                (unsigned) v, (unsigned) SECOND);
        PMIx_Finalize(NULL, 0);
        return 2;
    }
    fprintf(stdout, "  client: sees the revised value %u\n", (unsigned) v);

    /* phase 2: the host restates the SAME value */
    c = 'r';
    if (1 != write(readyfd, &c, 1) || 1 != read(gofd, &c, 1)) {
        PMIx_Finalize(NULL, 0);
        return 1;
    }
    usleep(300000);
    if (0 != read_local(&v) || SECOND != v) {
        fprintf(stderr, "  client: a restatement disturbed the value "
                        "(read %u, wanted %u)\n",
                (unsigned) v, (unsigned) SECOND);
        PMIx_Finalize(NULL, 0);
        return 3;
    }
    fprintf(stdout, "  client: value unchanged after a restatement\n");

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
    pmix_info_t info[5];
    pmix_proc_t proc;
    pmix_nspace_t ns;
    pmix_status_t rc;
    char *noderegex = NULL, *ppnregex = NULL;
    uint32_t nprocs = 1, first = FIRST;
    int i;

    PMIx_generate_regex(pmix_globals.hostname, &noderegex);
    PMIx_generate_ppn("0", &ppnregex);
    PMIX_INFO_LOAD(&info[0], PMIX_NODE_MAP, noderegex, PMIX_REGEX);
    PMIX_INFO_LOAD(&info[1], PMIX_PROC_MAP, ppnregex, PMIX_REGEX);
    PMIX_INFO_LOAD(&info[2], PMIX_JOB_SIZE, &nprocs, PMIX_UINT32);
    PMIX_INFO_LOAD(&info[3], PMIX_MAX_PROCS, &nprocs, PMIX_UINT32);
    PMIX_INFO_LOAD(&info[4], LATEKEY, &first, PMIX_UINT32);

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

/* The update itself: a NEGATIVE nlocalprocs says "revise what you hold
 * for this namespace" rather than "register it". */
static pmix_status_t revise(uint32_t value)
{
    pmix_info_t upd;
    pmix_nspace_t ns;
    pmix_status_t rc;

    PMIX_LOAD_NSPACE(ns, NSPACE);
    PMIX_INFO_LOAD(&upd, LATEKEY, &value, PMIX_UINT32);
    rc = PMIx_server_register_nspace(ns, -1, &upd, 1, NULL, NULL);
    PMIX_INFO_DESTRUCT(&upd);
    if (PMIX_OPERATION_SUCCEEDED == rc) {
        rc = PMIX_SUCCESS;
    }
    return rc;
}

int main(int argc, char **argv)
{
    pmix_info_t sinfo;
    pmix_proc_t proc;
    pmix_status_t rc;
    char **client_env = NULL;
    char *client_argv[5];
    char fdbuf[32];
    int readypipe[2], gopipe[2], status = 0;
    pid_t child;
    bool flag = true;
    char c;
    bool completed = false;

    setvbuf(stdout, NULL, _IOLBF, 0);

    if (1 < argc && 0 == strcmp(argv[1], "client")) {
        return run_client(atoi(argv[2]), atoi(argv[3]));
    }

    fprintf(stdout, "=== a job-data update reaches a running client ===\n");

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

    /* phase 1 */
    if (1 != read(readypipe[0], &c, 1)) {
        fprintf(stderr, "the client never reported its first read\n");
        goto done;
    }
    report("a running client reads the original job value", true);

    rc = revise(SECOND);
    report("the host revises it with register_nspace(nlocalprocs<0)",
           PMIX_SUCCESS == rc);

    c = 'g';
    if (1 != write(gopipe[1], &c, 1)) {
        goto done;
    }

    /* phase 2: restate the same value */
    if (1 != read(readypipe[0], &c, 1)) {
        fprintf(stderr, "the client never reported its second read\n");
        goto done;
    }
    rc = revise(SECOND);
    report("the host restates the same value", PMIX_SUCCESS == rc);
    c = 'g';
    if (1 != write(gopipe[1], &c, 1)) {
        goto done;
    }

    waitpid(child, &status, 0);
    if (WIFEXITED(status)) {
        report("the client's own datastore carries the revision",
               0 == WEXITSTATUS(status));
        if (0 != WEXITSTATUS(status)) {
            fprintf(stdout, "        (client exited %d)\n", WEXITSTATUS(status));
        }
    } else {
        report("the client's own datastore carries the revision", false);
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
    if (NULL != client_env) {
        PMIx_Argv_free(client_env);
    }
    PMIx_server_finalize();
    fprintf(stdout, "\n=== %d passed, %d failed ===\n", npass, nfail);
    return (0 == nfail) ? 0 : 1;
}
