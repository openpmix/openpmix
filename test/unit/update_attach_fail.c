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
 * AND WHAT IT MUST NOT READ. A client that could not map the update is
 * NOT entitled to go on answering from what it mapped before. An update
 * publishes a segment carrying the values that CHANGED, and a read
 * stops at the newest segment holding the key - so a client missing
 * that segment does not miss, it answers with the value the segment was
 * published to REPLACE, and goes on doing so while every peer that
 * mapped it reads the new one. That is a silent wrong answer and a
 * divergence between processes on one node, so the realm declines
 * locally while a delivery is known to be incomplete, and the ordinary
 * PMIx_Get is answered by the server. Both halves are asserted here:
 * the tracker survives (OPTIONAL still reads the client's own job
 * data), and the revised value is what a plain get returns.
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
#define UNIV_GROWN   64
/* A job-level key revised by a JOB-segment update, which is the other
 * delivery force_update_attach_failure reaches and the one this branch
 * of the library was built for. The test covered only the session. */
#define JOBKEY   "sut.jobupd.key"
#define JOB_WAS  1
#define JOB_NOW  7
#define JOB_NOW2 9
/* TWO local ranks, and only one of them rigged to fail.
 *
 * One rank cannot show what this defect costs. The harm is not that a
 * client is slower - it is that two processes on the same node, in the
 * same job, answer the same question differently, because one of them
 * kept reading a value the other had already seen replaced. Both ranks
 * read the same keys here and both must get the same answers. */
#define NPROCS   2

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
    if (0 != read_local_job_size(&sz) || NPROCS != sz) {
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
    if (NPROCS != sz) {
        fprintf(stderr, "  client: job size read back as %u, expected %u\n",
                (unsigned) sz, (unsigned) NPROCS);
        PMIx_Finalize(NULL, 0);
        return 3;
    }
    fprintf(stdout, "  client: own job data still readable after the "
                    "failed update attach\n");

    /* THE OTHER HALF. An ordinary PMIx_Get - no PMIX_OPTIONAL - must
     * return the REVISED session value. The client could not map the
     * segment carrying it, so it has to decline locally and let the
     * server answer; if it answers from what it still holds, it returns
     * the value that segment was published to replace and never asks
     * anyone. That is the silent wrong answer this exists to prevent. */
    {
        pmix_proc_t sp;
        pmix_value_t *sv = NULL;
        uint32_t got = 0;
        pmix_status_t r;

        PMIX_LOAD_PROCID(&sp, myproc.nspace, PMIX_RANK_WILDCARD);
        r = PMIx_Get(&sp, PMIX_UNIV_SIZE, NULL, 0, &sv);
        if (PMIX_SUCCESS != r || NULL == sv) {
            fprintf(stderr, "  client: universe size unreadable after the "
                            "failed update attach: %s\n",
                    PMIx_Error_string(r));
            PMIx_Finalize(NULL, 0);
            return 4;
        }
        r = PMIx_Value_get_number(sv, &got, PMIX_UINT32);
        PMIX_VALUE_RELEASE(sv);
        if (PMIX_SUCCESS != r || UNIV_GROWN != got) {
            fprintf(stderr, "  client: universe size read back as %u, "
                            "expected the revised %u - the client answered "
                            "from the segment the update replaced\n",
                    (unsigned) got, (unsigned) UNIV_GROWN);
            PMIx_Finalize(NULL, 0);
            return 5;
        }
        fprintf(stdout, "  client: reads the revised universe size %u\n",
                (unsigned) got);
    }

    /* THE JOB-SEGMENT HALF. Everything above rigged a SESSION segment.
     * force_update_attach_failure fails a job segment too, and that is
     * the delivery server_add_job_data() produces - so fail one and
     * require the same two things of it: the client still holds its own
     * job data, and an ordinary get returns the revised value rather
     * than the one the segment it could not map was published to
     * replace. */
    c = 'r';
    if (1 != write(readyfd, &c, 1) || 1 != read(gofd, &c, 1)) {
        PMIx_Finalize(NULL, 0);
        return 1;
    }
    {
        pmix_proc_t jp;
        pmix_value_t *jv = NULL;
        uint32_t got = 0;
        pmix_status_t r;
        int i;

        PMIX_LOAD_PROCID(&jp, myproc.nspace, PMIX_RANK_WILDCARD);
        /* the notice and the pipe are different channels */
        for (i = 0; i < 200; i++) {
            jv = NULL;
            r = PMIx_Get(&jp, JOBKEY, NULL, 0, &jv);
            if (PMIX_SUCCESS == r && NULL != jv) {
                if (PMIX_SUCCESS == PMIx_Value_get_number(jv, &got,
                                                          PMIX_UINT32) &&
                    JOB_NOW == got) {
                    PMIX_VALUE_RELEASE(jv);
                    break;
                }
                PMIX_VALUE_RELEASE(jv);
                jv = NULL;
            }
            usleep(25000);
        }
        if (JOB_NOW != got) {
            fprintf(stderr, "  client: job value read back as %u, expected "
                            "the revised %u - the client answered from the "
                            "segment the update replaced\n",
                    (unsigned) got, (unsigned) JOB_NOW);
            PMIx_Finalize(NULL, 0);
            return 6;
        }
        fprintf(stdout, "  client: reads the revised job value %u after a "
                        "failed JOB segment attach\n", (unsigned) got);

        /* And the tracker survived - which is asked differently here
         * than in the session half above.
         *
         * There, PMIX_OPTIONAL is the right question: a session failure
         * leaves the JOB realm untouched, so the client must still
         * answer job-level reads out of its own store. Here the failure
         * IS the job realm, so that realm is declining locally on
         * purpose, and PMIX_OPTIONAL - which forbids asking the server -
         * has no way to be answered. Requiring it to succeed would be
         * requiring the client to answer from data it has just been
         * told may be superseded.
         *
         * So ask without it. What distinguishes "the tracker went with
         * the segment" from "this realm is declining" is that the first
         * takes the namespace with it: pmix_gds_shmem3_fetch() answers
         * PMIX_ERR_INVALID_NAMESPACE and the server round trip cannot
         * repair it. A plain get returning the registered job size says
         * the tracker is there. */
        {
            pmix_value_t *sv = NULL;
            uint32_t jobsz = 0;

            if (PMIX_SUCCESS != PMIx_Get(&jp, PMIX_JOB_SIZE, NULL, 0, &sv) ||
                NULL == sv ||
                PMIX_SUCCESS != PMIx_Value_get_number(sv, &jobsz, PMIX_UINT32) ||
                NPROCS != jobsz) {
                fprintf(stderr, "  client: own job data lost after the "
                                "failed job-segment attach\n");
                if (NULL != sv) {
                    PMIX_VALUE_RELEASE(sv);
                }
                PMIx_Finalize(NULL, 0);
                return 7;
            }
            PMIX_VALUE_RELEASE(sv);
        }
        fprintf(stdout, "  client: its namespace and job data survived it\n");
    }

    /* the repeat: a second update over the one that was refused */
    c = 'r';
    if (1 != write(readyfd, &c, 1) || 1 != read(gofd, &c, 1)) {
        PMIx_Finalize(NULL, 0);
        return 1;
    }
    {
        pmix_proc_t jp;
        pmix_value_t *jv;
        uint32_t got = 0;
        int i;

        PMIX_LOAD_PROCID(&jp, myproc.nspace, PMIX_RANK_WILDCARD);
        for (i = 0; i < 200; i++) {
            jv = NULL;
            if (PMIX_SUCCESS == PMIx_Get(&jp, JOBKEY, NULL, 0, &jv) &&
                NULL != jv) {
                if (PMIX_SUCCESS == PMIx_Value_get_number(jv, &got,
                                                          PMIX_UINT32) &&
                    JOB_NOW2 == got) {
                    PMIX_VALUE_RELEASE(jv);
                    break;
                }
                PMIX_VALUE_RELEASE(jv);
            }
            usleep(25000);
        }
        if (JOB_NOW2 != got) {
            fprintf(stderr, "  client: after a second update the value is "
                            "%u, expected the newest %u - a re-offered "
                            "older segment must not answer in front of a "
                            "newer one\n",
                    (unsigned) got, (unsigned) JOB_NOW2);
            PMIx_Finalize(NULL, 0);
            return 8;
        }
        fprintf(stdout, "  client: reads the newest value %u after a repeat "
                        "notice\n", (unsigned) got);
    }

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
    pmix_info_t info[6], *sptr;
    pmix_data_array_t *array;
    pmix_proc_t proc;
    pmix_nspace_t ns;
    pmix_status_t rc;
    char *noderegex = NULL, *ppnregex = NULL;
    uint32_t nprocs = NPROCS, univ = FIRST, sid = SESSION, jv = JOB_WAS;
    int i;

    PMIx_generate_regex(pmix_globals.hostname, &noderegex);
    PMIx_generate_ppn("0,1", &ppnregex);
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

    /* a job-level value for the job-segment half to revise */
    PMIX_INFO_LOAD(&info[5], JOBKEY, &jv, PMIX_UINT32);

    PMIX_LOAD_NSPACE(ns, NSPACE);
    rc = PMIx_server_register_nspace(ns, 1, info, 6, NULL, NULL);
    for (i = 0; i < 6; i++) {
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

    for (i = 0; i < NPROCS; i++) {
        PMIX_LOAD_PROCID(&proc, NSPACE, (pmix_rank_t) i);
        regdone = false;
        rc = PMIx_server_register_client(&proc, geteuid(), getegid(), NULL,
                                         regcbfunc, NULL);
        if (PMIX_OPERATION_SUCCEEDED != rc && PMIX_SUCCESS != rc) {
            return rc;
        }
        if (PMIX_SUCCESS == rc) {
            int w;
            for (w = 0; w < 400 && !regdone; w++) {
                usleep(50000);
            }
        }
    }
    return PMIX_SUCCESS;
}

/* Revise a JOB-level value, which publishes a job segment - the other
 * delivery the forced attach failure reaches. */
static pmix_status_t revise_job_value(void)
{
    pmix_info_t upd;
    pmix_nspace_t ns;
    pmix_status_t rc;
    uint32_t v = JOB_NOW;

    PMIX_LOAD_NSPACE(ns, NSPACE);
    PMIX_INFO_LOAD(&upd, JOBKEY, &v, PMIX_UINT32);
    rc = PMIx_server_register_nspace(ns, -1, &upd, 1, NULL, NULL);
    PMIX_INFO_DESTRUCT(&upd);
    if (PMIX_OPERATION_SUCCEEDED == rc) {
        rc = PMIX_SUCCESS;
    }
    return rc;
}

/* A second revision, published after the first was refused. */
static pmix_status_t revise_job_value_again(void)
{
    pmix_info_t upd;
    pmix_nspace_t ns;
    pmix_status_t rc;
    uint32_t v = JOB_NOW2;

    PMIX_LOAD_NSPACE(ns, NSPACE);
    PMIX_INFO_LOAD(&upd, JOBKEY, &v, PMIX_UINT32);
    rc = PMIx_server_register_nspace(ns, -1, &upd, 1, NULL, NULL);
    PMIX_INFO_DESTRUCT(&upd);
    if (PMIX_OPERATION_SUCCEEDED == rc) {
        rc = PMIX_SUCCESS;
    }
    return rc;
}

int main(int argc, char **argv)
{
    pmix_info_t sinfo, upd;
    pmix_proc_t proc;
    pmix_status_t rc;
    char **client_env = NULL;
    char *client_argv[5];
    char fdbuf[32];
    int readypipe[NPROCS][2], gopipe[NPROCS][2], status = 0;
    pid_t child[NPROCS];
    int k;
    bool flag = true;
    uint32_t grown = UNIV_GROWN;
    char c;
    bool completed = false;

    setvbuf(stdout, NULL, _IOLBF, 0);

    if (1 < argc && 0 == strcmp(argv[1], "client")) {
        return run_client(atoi(argv[2]), atoi(argv[3]));
    }

    fprintf(stdout, "=== a failed update attach costs only that segment ===\n");

    for (k = 0; k < NPROCS; k++) {
        if (0 != pipe(readypipe[k]) || 0 != pipe(gopipe[k])) {
            fprintf(stderr, "pipe() failed\n");
            return 1;
        }
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

    /* Seed the child's environment with our own first - see the note in
     * event_forward.c: without it the child runs against the INSTALLED
     * PMIx rather than the one under test. */
    client_env = PMIx_Argv_copy(environ);
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
    for (k = 0; k < NPROCS; k++) {
        char **kenv;

        PMIX_LOAD_PROCID(&proc, NSPACE, (pmix_rank_t) k);
        kenv = PMIx_Argv_copy(client_env);
        rc = PMIx_server_setup_fork(&proc, &kenv);
        if (PMIX_SUCCESS != rc) {
            fprintf(stderr, "PMIx_server_setup_fork failed: %s\n",
                    PMIx_Error_string(rc));
            PMIx_Argv_free(kenv);
            goto done;
        }
        /* ONLY RANK 0 is rigged. Rank 1 maps everything and is the
         * control: what it reads is what the host set, so if rank 0
         * answers differently the two have diverged - which is the harm,
         * and what a single-client test cannot show. */
        if (0 == k) {
            PMIx_Argv_append_nosize(
                &kenv, "PMIX_MCA_gds_shmem3_force_update_attach_failure=1"
            );
        }

        child[k] = fork();
        if (0 > child[k]) {
            fprintf(stderr, "fork() failed\n");
            PMIx_Argv_free(kenv);
            goto done;
        }
        if (0 == child[k]) {
            /* exec rather than run in the fork: this process has an
             * initialized PMIx server in it */
            close(readypipe[k][0]);
            close(gopipe[k][1]);
            client_argv[0] = argv[0];
            client_argv[1] = (char *) "client";
            snprintf(fdbuf, sizeof(fdbuf), "%d", readypipe[k][1]);
            client_argv[2] = strdup(fdbuf);
            snprintf(fdbuf, sizeof(fdbuf), "%d", gopipe[k][0]);
            client_argv[3] = strdup(fdbuf);
            client_argv[4] = NULL;
            execve(argv[0], client_argv, kenv);
            fprintf(stderr, "exec of %s failed\n", argv[0]);
            _exit(127);
        }
        close(readypipe[k][1]);
        close(gopipe[k][0]);
        PMIx_Argv_free(kenv);
    }

    /* wait until the client is up and reading its own job data */
    for (k = 0; k < NPROCS; k++) {
        if (1 != read(readypipe[k][0], &c, 1)) {
            fprintf(stderr, "the client never reported that it was up\n");
            goto done;
        }
    }
    report("a running client reads its own job data", true);

    /* publish a session update, which the client is rigged to fail to map */
    PMIX_INFO_LOAD(&upd, PMIX_UNIV_SIZE, &grown, PMIX_UINT32);
    rc = PMIx_server_register_session(SESSION, &upd, 1, NULL, NULL);
    PMIX_INFO_DESTRUCT(&upd);
    report("the host publishes a session update", PMIX_SUCCESS == rc);

    /* release both ranks to read again */
    c = 'g';
    for (k = 0; k < NPROCS; k++) {
        if (1 != write(gopipe[k][1], &c, 1)) {
            fprintf(stderr, "could not release the clients\n");
            goto done;
        }
    }

    /* the job-segment half */
    for (k = 0; k < NPROCS; k++) {
        if (1 != read(readypipe[k][0], &c, 1)) {
            fprintf(stderr, "the client never reported its job-segment read\n");
            goto done;
        }
    }
    rc = revise_job_value();
    report("the host revises a job value, which the client cannot map",
           PMIX_SUCCESS == rc);
    c = 'g';
    for (k = 0; k < NPROCS; k++) {
        if (1 != write(gopipe[k][1], &c, 1)) {
            goto done;
        }
    }

    /* A SECOND update, after one has already been refused.
     *
     * The server re-sends the whole chain on every notice, so this
     * delivery offers the segment that failed before as well as the new
     * one. That is the retry the recovery depends on - and it is also
     * how a chain can end up out of order, since the older segment
     * would be published in front of the newer one if a client took it
     * on its second offer. Both ranks must end up reading the NEWEST
     * value either way. */
    for (k = 0; k < NPROCS; k++) {
        if (1 != read(readypipe[k][0], &c, 1)) {
            fprintf(stderr, "a client never reported its repeat read\n");
            goto done;
        }
    }
    rc = revise_job_value_again();
    report("the host publishes a second update over the refused one",
           PMIX_SUCCESS == rc);
    c = 'g';
    for (k = 0; k < NPROCS; k++) {
        if (1 != write(gopipe[k][1], &c, 1)) {
            goto done;
        }
    }

    {
        bool allok = true;
        for (k = 0; k < NPROCS; k++) {
            waitpid(child[k], &status, 0);
            if (WIFEXITED(status)) {
                if (0 != WEXITSTATUS(status)) {
                    allok = false;
                    fprintf(stdout, "        (rank %d exited %d)\n", k,
                            WEXITSTATUS(status));
                }
            } else {
                allok = false;
                fprintf(stdout, "        (rank %d died on a signal)\n", k);
            }
        }
        report("both ranks agree, and each still holds its own job data",
               allok);
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
