/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 */

/*
 * A write event that has never been armed must be inert.
 *
 * pmix_iof_write_event_t holds its libevent record by value, so "has this
 * record been through pmix_event_set()" is no longer answerable by looking
 * for a NULL pointer - it is carried explicitly in wev.evset, and both
 * sink macros screen on it.  Handing libevent a zeroed record instead
 * earns a warning and an error return, and any output queued behind it is
 * silently lost.
 *
 * Two ways a sink reaches that state, and neither is exotic:
 *
 *   - it was constructed but defined on a negative descriptor, which is
 *     what PMIX_IOF_SINK_DEFINE is handed whenever a channel is not being
 *     forwarded; and
 *   - it was statically initialized (PMIX_IOF_SINK_STATIC_INIT) and has
 *     not run a constructor at all - which is how pmix_client_globals'
 *     stdout and stderr sinks exist between load time and pmix_rte_init.
 *
 * The delivery path itself is covered by test/unit/iof_output; what is
 * tested here is the state that path is never allowed to reach, plus the
 * positive control that a real descriptor does arm the record.
 */

#include "src/include/pmix_config.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "include/pmix_server.h"

#include "src/client/pmix_client_ops.h"
#include "src/common/pmix_iof.h"
#include "src/include/pmix_globals.h"

static int failures = 0;

static void report(const char *what, bool pass)
{
    fprintf(stdout, "%-62s : %s\n", what, pass ? "PASS" : "FAIL");
    fflush(stdout);
    if (!pass) {
        ++failures;
    }
}

/* A sink that has run no constructor - the shape pmix_client_globals uses
 * for its stdout and stderr sinks.  Deliberately never destructed: the
 * point of the case is the window before anything has been done to it. */
static pmix_iof_sink_t static_sink = PMIX_IOF_SINK_STATIC_INIT(static_sink);

static pmix_server_module_t mymodule = {0};

int main(int argc, char **argv)
{
    pmix_status_t rc;
    pmix_proc_t src;
    pmix_iof_sink_t snk;
    int pipefd[2];
    (void) argc;
    (void) argv;

    fprintf(stdout, "\n=== PMIx IOF sink arming unit test ===\n\n");

    rc = PMIx_server_init(&mymodule, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "PMIx_server_init failed: %s\n", PMIx_Error_string(rc));
        return 1;
    }
    PMIX_LOAD_PROCID(&src, "iof-sink-arm", 0);

    /* --- a statically initialized sink ------------------------------- */
    report("static sink: record is not armed", !static_sink.wev.evset);
    /* must not reach libevent, and must not claim to have been queued */
    PMIX_IOF_SINK_ACTIVATE(&static_sink.wev);
    report("static sink: activate is a no-op", !static_sink.wev.pending);

    /* --- constructed, but no descriptor to write to ------------------- */
    PMIX_CONSTRUCT(&snk, pmix_iof_sink_t);
    report("constructed sink: record is not armed yet", !snk.wev.evset);
    PMIX_IOF_SINK_DEFINE(&snk, &src, -1, PMIX_FWD_STDOUT_CHANNEL, pmix_iof_write_handler);
    report("sink defined on fd -1: record is still not armed", !snk.wev.evset);
    report("sink defined on fd -1: descriptor untouched", -1 == snk.wev.fd);
    PMIX_IOF_SINK_ACTIVATE(&snk.wev);
    report("sink defined on fd -1: activate is a no-op", !snk.wev.pending);
    PMIX_DESTRUCT(&snk);

    /* --- the positive control: a real descriptor arms it -------------- */
    if (0 != pipe(pipefd)) {
        fprintf(stderr, "pipe failed: %s\n", strerror(errno));
        PMIx_server_finalize();
        return 1;
    }
    PMIX_CONSTRUCT(&snk, pmix_iof_sink_t);
    PMIX_IOF_SINK_DEFINE(&snk, &src, pipefd[1], PMIX_FWD_STDOUT_CHANNEL, pmix_iof_write_handler);
    report("sink defined on a pipe: record is armed", snk.wev.evset);
    report("sink defined on a pipe: descriptor recorded", pipefd[1] == snk.wev.fd);
    /* the destructor owns the write end from here - it closes any fd above
     * 2 - so the read end is all that is left to clean up */
    PMIX_DESTRUCT(&snk);
    close(pipefd[0]);

    PMIx_server_finalize();

    fprintf(stdout, "\n%s: %d failure%s\n", (0 == failures) ? "PASS" : "FAIL", failures,
            (1 == failures) ? "" : "s");
    return (0 == failures) ? 0 : 1;
}
