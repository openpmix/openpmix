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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEFAULT_CYCLES 1200

int main(int argc, char **argv)
{
    pmix_proc_t myproc;
    pmix_status_t rc;
    pmix_info_t tinfo;
    long ncycles = DEFAULT_CYCLES;
    long i;
    int nfail = 0;

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

    if (0 == nfail) {
        fprintf(stdout, "Completed %ld tool init/finalize cycles: PASS\n\n", ncycles);
    } else {
        fprintf(stdout, "tool init/finalize cycling: FAIL\n\n");
    }

    return nfail;
}
