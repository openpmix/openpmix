/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 */

/* spawn_iof - does the output of a spawned job reach whoever is watching
 * the job that spawned it?
 *
 * The parent job is launched by a tool (prun, prterun) and so its output is
 * already being forwarded to that tool. Rank 0 then calls PMIx_Spawn naming
 * NONE of PMIX_FWD_STDOUT/STDERR/STDDIAG, which is the request to be treated
 * the way the parent is being treated: the child's output should come out of
 * the same terminal the parent's does. See
 * docs/how-things-work/iof_inheritance.rst.
 *
 * Every line this program writes is prefixed so a harness can tell the two
 * jobs apart without knowing either namespace in advance:
 *
 *    SPAWN_IOF parent <nspace>:<rank> host <host>   (parent, stdout)
 *    SPAWN_IOF spawned <child-nspace>               (rank 0, stdout)
 *    SPAWN_IOF child <nspace>:<rank> host <host>    (child, stdout)
 *    SPAWN_IOF childerr <nspace>:<rank> host <host> (child, stderr)
 *
 * The interesting question is a routing one, so WHERE each line was written
 * is part of the answer and every line carries its host. A run in which the
 * spawning rank sits on the same daemon as the tool proves much less than one
 * in which it does not; contrib/dockerswarm/run-spawn-iof.sh drives both.
 *
 * Rank 0 waits for the child job to terminate before it finalizes, because
 * the parent job going away first would tear down the very forwarding under
 * test. The wait is bounded: a missing termination event is a failure to
 * report, not a reason to hang a suite.
 *
 * One more thing is needed to read a NEGATIVE result. If the child's lines
 * never appear, the run alone cannot say whether the forwarding dropped them
 * or the child never ran at all - and those want opposite repairs. So with
 *
 *    spawn_iof --markers <dir>
 *
 * each process also drops a file of the same content into <dir> on the node
 * it is running on, by a channel that has nothing to do with IOF, and rank 0
 * passes the option down to the child. Without it - the ordinary way to run
 * this - no files are written. The directory travels in argv rather than in
 * the environment because argv is what a spawn is guaranteed to carry: what
 * a launcher forwards of its own environment is its business, and the child
 * job does not inherit it in any case.
 */

#define _GNU_SOURCE
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "examples.h"
#include <pmix.h>

static pmix_proc_t myproc;
static char hostname[1024];
static char *markerdir = NULL;
static volatile bool child_done = false;

/* the child job ended - stop waiting */
static void tercbfunc(size_t evhdlr_registration_id, pmix_status_t status,
                      const pmix_proc_t *source, pmix_info_t info[], size_t ninfo,
                      pmix_info_t results[], size_t nresults,
                      pmix_event_notification_cbfunc_fn_t cbfunc, void *cbdata)
{
    EXAMPLES_HIDE_UNUSED_PARAMS(evhdlr_registration_id, status, source,
                                info, ninfo, results, nresults);

    child_done = true;
    if (NULL != cbfunc) {
        cbfunc(PMIX_EVENT_ACTION_COMPLETE, NULL, 0, NULL, NULL, cbdata);
    }
}

/* Say the same thing again, out of band, so that a run in which no output
 * arrives can still be told apart from a run in which nothing happened. */
static void drop_marker(const char *what)
{
    char path[1024];
    FILE *fp;

    if (NULL == markerdir) {
        return;
    }
    snprintf(path, sizeof(path), "%s/spawn_iof.%s.%s.%u",
             markerdir, what, myproc.nspace, myproc.rank);
    fp = fopen(path, "w");
    if (NULL == fp) {
        fprintf(stderr, "SPAWN_IOF %s:%u cannot write %s\n",
                myproc.nspace, myproc.rank, path);
        return;
    }
    fprintf(fp, "SPAWN_IOF %s %s:%u host %s\n", what, myproc.nspace, myproc.rank, hostname);
    fclose(fp);
}

static void regcbfunc(pmix_status_t status, size_t evhandler_ref, void *cbdata)
{
    mylock_t *lock = (mylock_t *) cbdata;

    EXAMPLES_HIDE_UNUSED_PARAMS(evhandler_ref);

    lock->status = status;
    DEBUG_WAKEUP_THREAD(lock);
}

/* the child job: say who and where we are, on both channels */
static int be_the_child(void)
{
    pmix_status_t rc;

    printf("SPAWN_IOF child %s:%u host %s\n", myproc.nspace, myproc.rank, hostname);
    fflush(stdout);
    fprintf(stderr, "SPAWN_IOF childerr %s:%u host %s\n",
            myproc.nspace, myproc.rank, hostname);
    fflush(stderr);
    drop_marker("child");

    rc = PMIx_Finalize(NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "SPAWN_IOF child %s:%u PMIx_Finalize failed: %s\n",
                myproc.nspace, myproc.rank, PMIx_Error_string(rc));
        return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    pmix_status_t rc, code = PMIX_ERR_JOB_TERMINATED;
    pmix_app_t *app;
    pmix_info_t jinfo[1];
    pmix_nspace_t child;
    mylock_t mylock;
    bool am_child = false;
    int i, secs;

    for (i = 1; i < argc; i++) {
        if (0 == strcmp(argv[i], "child")) {
            am_child = true;
        } else if (0 == strcmp(argv[i], "--markers")) {
            if (argc == ++i) {
                fprintf(stderr, "--markers requires a directory\n");
                exit(1);
            }
            markerdir = argv[i];
        } else {
            fprintf(stderr, "usage: %s [child] [--markers <dir>]\n", argv[0]);
            exit(1);
        }
    }

    if (0 > gethostname(hostname, sizeof(hostname))) {
        strcpy(hostname, "unknown");
    }
    hostname[sizeof(hostname) - 1] = '\0';

    rc = PMIx_Init(&myproc, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "SPAWN_IOF PMIx_Init failed: %s\n", PMIx_Error_string(rc));
        exit(1);
    }

    if (am_child) {
        return be_the_child();
    }

    /* the parent job - our own output is what the tool is already watching */
    printf("SPAWN_IOF parent %s:%u host %s\n", myproc.nspace, myproc.rank, hostname);
    fflush(stdout);
    drop_marker("parent");

    if (0 != myproc.rank) {
        /* only rank 0 spawns; the rest are here to be somewhere else */
        goto done;
    }

    /* ask to be told when the child job ends, and be ready to hear it
     * BEFORE the spawn - a short-lived child can finish first */
    DEBUG_CONSTRUCT_LOCK(&mylock);
    PMIx_Register_event_handler(&code, 1, NULL, 0, tercbfunc, regcbfunc, (void *) &mylock);
    DEBUG_WAIT_THREAD(&mylock);
    rc = mylock.status;
    DEBUG_DESTRUCT_LOCK(&mylock);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "SPAWN_IOF %s:%u could not register for job termination: %s\n",
                myproc.nspace, myproc.rank, PMIx_Error_string(rc));
        goto done;
    }

    PMIX_APP_CREATE(app, 1);
    app->cmd = strdup(argv[0]);
    app->maxprocs = 1;
    PMIx_Argv_append_nosize(&app->argv, argv[0]);
    PMIx_Argv_append_nosize(&app->argv, "child");
    if (NULL != markerdir) {
        PMIx_Argv_append_nosize(&app->argv, "--markers");
        PMIx_Argv_append_nosize(&app->argv, markerdir);
    }

    /* NOTHING about output forwarding is named here - that is the point.
     * PMIX_NOTIFY_COMPLETION is not an output directive and so does not
     * turn inheritance off. */
    PMIX_INFO_LOAD(&jinfo[0], PMIX_NOTIFY_COMPLETION, NULL, PMIX_BOOL);

    rc = PMIx_Spawn(jinfo, 1, app, 1, child);
    PMIX_INFO_DESTRUCT(&jinfo[0]);
    PMIX_APP_FREE(app, 1);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "SPAWN_IOF %s:%u PMIx_Spawn failed: %s\n",
                myproc.nspace, myproc.rank, PMIx_Error_string(rc));
        goto done;
    }
    printf("SPAWN_IOF spawned %s\n", child);
    fflush(stdout);

    /* Stay alive until the child is done: the parent job ending is what
     * tears down the forwarding we are asking about. Bounded, so a lost
     * notification shows up as a stated timeout instead of a hung suite. */
    for (secs = 0; !child_done && secs < 30; ++secs) {
        sleep(1);
    }
    if (!child_done) {
        fprintf(stderr, "SPAWN_IOF %s:%u timed out waiting for %s to end\n",
                myproc.nspace, myproc.rank, child);
    }

done:
    rc = PMIx_Finalize(NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "SPAWN_IOF parent %s:%u PMIx_Finalize failed: %s\n",
                myproc.nspace, myproc.rank, PMIx_Error_string(rc));
        return 1;
    }
    return 0;
}
