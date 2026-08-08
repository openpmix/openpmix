/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Regression test for tool-side init/finalize cycling.
 *
 * A tool must be able to cycle PMIx_tool_init -> (work) ->
 * PMIx_tool_finalize and then PMIx_tool_init again, any number of times,
 * with each new init presenting a clean slate. The tool library tears
 * itself all the way down on the final PMIx_tool_finalize, so a
 * subsequent PMIx_tool_init is a fresh library instance; nothing from one
 * cycle may leak into the next. See docs/how-things-work/init-finalize.rst.
 *
 * The tool is initialized with PMIX_TOOL_DO_NOT_CONNECT so it comes up
 * self-contained, with no server to reach - which makes the test safe
 * under "make check" (no launcher or rendezvous is needed) while still
 * running the whole tool-library teardown/reinit path for real: the
 * one-time-init latch and reference counter, the framework open/close,
 * the thread-specific-data keys, the event base and progress thread, and
 * the server_globals bookkeeping the tool always constructs. A
 * cross-cycle regression - a stale one-time-init latch (PMIX_ERR_INIT on
 * the second init), a leaked TSD key slot, a dangling event base, or a
 * destructed-but-not-reconstructed list/array - shows up here as a failed
 * return code, a wrong PMIx_Initialized() state, or a crash on a later
 * cycle rather than the first.
 *
 * The cycle count can be overridden with a single command-line argument;
 * the default is chosen to cycle past the per-process pthread-key limit
 * (macOS ~512, Linux ~1024) so a leaked TSD key slot is caught, while
 * still finishing quickly.
 */

#include "src/include/pmix_config.h"

#include "include/pmix.h"
#include "include/pmix_tool.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define DEFAULT_CYCLES 1200
/* the stdin-forwarding pass is about the teardown existing, not about
 * volume - keep it short so the default run stays quick */
#define STDIN_CYCLES   20

int main(int argc, char **argv)
{
    pmix_proc_t myproc;
    pmix_status_t rc;
    pmix_info_t tinfo;
    long ncycles = DEFAULT_CYCLES;
    long i;
    int nfail = 0;
    struct stat stdin_before;

    if (1 < argc) {
        ncycles = strtol(argv[1], NULL, 10);
        if (0 >= ncycles) {
            ncycles = DEFAULT_CYCLES;
        }
    }

    /* make sure nothing in the environment points us at a server: with
     * PMIX_TOOL_DO_NOT_CONNECT set below we will not try to connect, but
     * clearing these keeps the run hermetic regardless of the caller's
     * environment */
    unsetenv("PMIX_NAMESPACE");
    unsetenv("PMIX_RANK");
    unsetenv("PMIX_SERVER_URI");
    unsetenv("PMIX_SERVER_URI2");
    unsetenv("PMIX_SERVER_URI3");
    unsetenv("PMIX_SERVER_URI21");
    unsetenv("PMIX_SERVER_URI41");
    unsetenv("PMIX_SERVER_URI51");

    fprintf(stdout, "\n=== tool init/finalize cycling test (%ld cycles) ===\n\n",
            ncycles);

    for (i = 0; i < ncycles; i++) {
        /* PMIx_Initialized() must report false before we init */
        if (PMIx_Initialized()) {
            fprintf(stderr, "cycle %ld: PMIx_Initialized() true before init\n", i);
            nfail = 1;
            break;
        }

        PMIX_INFO_LOAD(&tinfo, PMIX_TOOL_DO_NOT_CONNECT, NULL, PMIX_BOOL);
        rc = PMIx_tool_init(&myproc, &tinfo, 1);
        PMIX_INFO_DESTRUCT(&tinfo);
        if (PMIX_SUCCESS != rc) {
            fprintf(stderr, "cycle %ld: PMIx_tool_init failed: %s\n", i,
                    PMIx_Error_string(rc));
            nfail = 1;
            break;
        }

        if (!PMIx_Initialized()) {
            fprintf(stderr, "cycle %ld: PMIx_Initialized() false after init\n", i);
            nfail = 1;
            break;
        }

        rc = PMIx_tool_finalize();
        if (PMIX_SUCCESS != rc) {
            fprintf(stderr, "cycle %ld: PMIx_tool_finalize failed: %s\n", i,
                    PMIx_Error_string(rc));
            nfail = 1;
            break;
        }

        if (PMIx_Initialized()) {
            fprintf(stderr, "cycle %ld: PMIx_Initialized() true after finalize\n", i);
            nfail = 1;
            break;
        }
    }

    /* Now the same cycle with PMIX_FWD_STDIN, which is a different init
     * and a different teardown: it has the library build a read event
     * over our stdin and register it (plus, on a tty, a SIGCONT handler)
     * on the event base. Both are process-wide, so a cycle that does not
     * take them back down leaves an event registered on a base
     * pmix_rte_finalize has already destroyed, and leaves the next init
     * building a second read event on the same descriptor.
     *
     * Two rules meet on that descriptor, and the second is why this
     * stands a pipe of its own on fd 0 rather than trusting whatever the
     * caller's stdin happens to be:
     *
     *  - stdin belongs to the *application*. The library did not open
     *    fd 0 and must not close it at shutdown; only descriptors it
     *    opened itself are its to close.
     *  - "still open" is not enough to prove that. If the library closes
     *    fd 0 and anything later in the same cycle opens a descriptor,
     *    fd 0 is handed straight back and an is-it-open test passes. So
     *    compare the identity of what is on fd 0, not just its presence.
     *
     * Nothing is ever written to the pipe, so the read event stays quiet.
     *
     * Far fewer cycles than above: this one is about the teardown
     * existing at all, and every cycle re-reads the same stdin. */
    if (0 == nfail) {
        int pipefd[2];

        if (0 != pipe(pipefd) || 0 > dup2(pipefd[0], STDIN_FILENO)) {
            fprintf(stderr, "stdin cycles: could not put a pipe on stdin\n");
            nfail = 1;
        } else {
            close(pipefd[0]);
            if (0 != fstat(STDIN_FILENO, &stdin_before)) {
                fprintf(stderr, "stdin cycles: fstat of stdin failed\n");
                nfail = 1;
            }
        }
    }
    for (i = 0; 0 == nfail && i < STDIN_CYCLES; i++) {
        pmix_info_t sinfo[2];
        struct stat stdin_after;

        PMIX_INFO_LOAD(&sinfo[0], PMIX_TOOL_DO_NOT_CONNECT, NULL, PMIX_BOOL);
        PMIX_INFO_LOAD(&sinfo[1], PMIX_FWD_STDIN, NULL, PMIX_BOOL);
        rc = PMIx_tool_init(&myproc, sinfo, 2);
        PMIX_INFO_DESTRUCT(&sinfo[0]);
        PMIX_INFO_DESTRUCT(&sinfo[1]);
        if (PMIX_SUCCESS != rc) {
            fprintf(stderr, "stdin cycle %ld: PMIx_tool_init failed: %s\n", i,
                    PMIx_Error_string(rc));
            nfail = 1;
            break;
        }
        rc = PMIx_tool_finalize();
        if (PMIX_SUCCESS != rc) {
            fprintf(stderr, "stdin cycle %ld: PMIx_tool_finalize failed: %s\n", i,
                    PMIx_Error_string(rc));
            nfail = 1;
            break;
        }
        /* the library must not have closed our stdin on the way out, and
         * what is on fd 0 must still be the pipe we put there */
        if (0 != fstat(STDIN_FILENO, &stdin_after)) {
            fprintf(stderr, "stdin cycle %ld: finalize closed our stdin\n", i);
            nfail = 1;
            break;
        }
        if (stdin_before.st_dev != stdin_after.st_dev ||
            stdin_before.st_ino != stdin_after.st_ino) {
            fprintf(stderr, "stdin cycle %ld: fd 0 is no longer our pipe\n", i);
            nfail = 1;
            break;
        }
    }

    if (0 == nfail) {
        fprintf(stdout, "Completed %ld tool init/finalize cycles (%d with stdin "
                "forwarding): PASS\n\n", ncycles, STDIN_CYCLES);
    } else {
        fprintf(stdout, "tool init/finalize cycling: FAIL\n\n");
    }

    return nfail;
}
