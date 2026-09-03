/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 */

/* iof_watcher - a tool that attaches where it is started and asks for a
 * RUNNING job's output.
 *
 *    iof_watcher <nspace> [seconds]
 *
 * This is the third of the cases in
 * docs/how-things-work/iof_inheritance.rst, and it is about *which
 * daemon* is asked. PMIx_tool_init with no directives attaches to the
 * server that is local to wherever this runs - so starting it on a node
 * that is not the DVM master gives a tool whose IOF subscription is held
 * by a NON-master daemon. Only that daemon's PMIx server can deliver to
 * it, so unless the runtime relays that job's output there, this tool
 * sees only the processes that daemon happens to host, and nothing at
 * all if it hosts none.
 *
 * Everything received is printed with a WATCHER prefix and the source, so
 * a harness can tell what reached the tool from what merely ran. The
 * payload is not NUL-terminated - it is a byte object - so it is written
 * by length rather than as a string.
 */

#define _GNU_SOURCE
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "examples.h"
#include <pmix_tool.h>

static void iofcb(size_t iofhdlr, pmix_iof_channel_t channel, pmix_proc_t *source,
                  pmix_byte_object_t *payload, pmix_info_t info[], size_t ninfo)
{
    EXAMPLES_HIDE_UNUSED_PARAMS(iofhdlr, info, ninfo);

    if (NULL == payload || NULL == payload->bytes || 0 == payload->size) {
        return;     // an end-of-stream marker carries no bytes
    }
    printf("WATCHER %s %s:%u: %.*s",
           (PMIX_FWD_STDERR_CHANNEL & channel) ? "stderr" : "stdout",
           (NULL == source) ? "unknown" : source->nspace,
           (NULL == source) ? 0 : source->rank,
           (int) payload->size, payload->bytes);
    fflush(stdout);
}

static void regcb(pmix_status_t status, size_t refid, void *cbdata)
{
    mylock_t *lock = (mylock_t *) cbdata;

    /* the registration id is what a tool would keep in order to
     * PMIx_IOF_deregister later; this one watches until it exits, so it
     * has no use for the id and does not record it */
    EXAMPLES_HIDE_UNUSED_PARAMS(refid);

    lock->status = status;
    DEBUG_WAKEUP_THREAD(lock);
}

int main(int argc, char **argv)
{
    pmix_proc_t myproc, target;
    pmix_status_t rc;
    mylock_t mylock;
    int seconds = 30;

    if (2 > argc) {
        fprintf(stderr, "usage: %s <nspace> [seconds]\n", argv[0]);
        exit(1);
    }
    if (2 < argc) {
        seconds = atoi(argv[2]);
    }

    rc = PMIx_tool_init(&myproc, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "WATCHER PMIx_tool_init failed: %s\n", PMIx_Error_string(rc));
        exit(1);
    }
    printf("WATCHER attached as %s:%u\n", myproc.nspace, myproc.rank);
    fflush(stdout);

    /* the whole job, not one rank */
    PMIX_LOAD_PROCID(&target, argv[1], PMIX_RANK_WILDCARD);

    DEBUG_CONSTRUCT_LOCK(&mylock);
    rc = PMIx_IOF_pull(&target, 1, NULL, 0,
                       PMIX_FWD_STDOUT_CHANNEL | PMIX_FWD_STDERR_CHANNEL,
                       iofcb, regcb, (void *) &mylock);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "WATCHER PMIx_IOF_pull failed: %s\n", PMIx_Error_string(rc));
        DEBUG_DESTRUCT_LOCK(&mylock);
        PMIx_tool_finalize();
        exit(1);
    }
    DEBUG_WAIT_THREAD(&mylock);
    rc = mylock.status;
    DEBUG_DESTRUCT_LOCK(&mylock);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "WATCHER IOF registration failed: %s\n", PMIx_Error_string(rc));
        PMIx_tool_finalize();
        exit(1);
    }
    printf("WATCHER watching %s\n", argv[1]);
    fflush(stdout);

    /* Nothing to wait ON: output arrives through the callback for as long
     * as we stay attached, and the interesting part is what shows up
     * during that window. The harness decides how long that is. */
    sleep(seconds);

    printf("WATCHER done\n");
    fflush(stdout);

    rc = PMIx_tool_finalize();
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "WATCHER PMIx_tool_finalize failed: %s\n", PMIx_Error_string(rc));
        return 1;
    }
    return 0;
}
