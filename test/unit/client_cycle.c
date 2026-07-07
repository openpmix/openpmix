/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Regression test for client-side init/finalize cycling.
 *
 * A client must be able to cycle PMIx_Init -> (work) -> PMIx_Finalize and
 * then PMIx_Init again, any number of times, with each new PMIx_Init
 * presenting a clean slate. The client library tears itself all the way
 * down on the final PMIx_Finalize, so a subsequent PMIx_Init is a fresh
 * library instance; nothing from one cycle may leak into the next. See
 * docs/how-things-work/init-finalize.rst.
 *
 * This exercises the teardown/reinit path directly. It runs as a
 * singleton (no server), so it needs no launcher and is safe under
 * "make check": PMIx_Fence and PMIx_Commit short-circuit to success in
 * singleton mode, while PMIx_Init/PMIx_Finalize, the framework
 * open/close, the thread-specific-data keys, the event base, and the
 * per-cycle mode flags all run for real. A cross-cycle regression -
 * a stale one-time-init latch, a dangling event base or progress
 * tracker, a leaked TSD key slot, or a stale singleton flag - shows up
 * here as a failed return code, a wrong PMIx_Initialized() state, or a
 * crash on a later cycle rather than the first.
 *
 * The cycle count can be overridden with a single command-line argument;
 * the default is chosen to stay quick while still cycling enough times to
 * shake out per-cycle resource leaks.
 */

#include "src/include/pmix_config.h"

#include "include/pmix.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEFAULT_CYCLES 1200

int main(int argc, char **argv)
{
    pmix_proc_t myproc;
    pmix_status_t rc;
    pmix_value_t val;
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

    /* force the singleton path: with no namespace or server contact
     * information in the environment there is nothing to connect to, so
     * each PMIx_Init comes up self-contained */
    unsetenv("PMIX_NAMESPACE");
    unsetenv("PMIX_RANK");
    unsetenv("PMIX_SERVER_URI");
    unsetenv("PMIX_SERVER_URI2");
    unsetenv("PMIX_SERVER_URI3");
    unsetenv("PMIX_SERVER_URI21");
    unsetenv("PMIX_SERVER_URI41");
    unsetenv("PMIX_SERVER_URI51");

    fprintf(stdout, "\n=== client init/finalize cycling test (%ld cycles) ===\n\n",
            ncycles);

    for (i = 0; i < ncycles; i++) {
        /* PMIx_Initialized() must report false before we init */
        if (PMIx_Initialized()) {
            fprintf(stderr, "cycle %ld: PMIx_Initialized() true before init\n", i);
            nfail = 1;
            break;
        }

        /* PMIx_Init returns PMIX_ERR_UNREACH - not a failure - when it
         * comes up as a singleton with no reachable server; the library
         * is fully initialized in that case. Either result is fine here. */
        rc = PMIx_Init(&myproc, NULL, 0);
        if (PMIX_SUCCESS != rc && PMIX_ERR_UNREACH != rc) {
            fprintf(stderr, "cycle %ld: PMIx_Init failed: %s\n", i,
                    PMIx_Error_string(rc));
            nfail = 1;
            break;
        }

        if (!PMIx_Initialized()) {
            fprintf(stderr, "cycle %ld: PMIx_Initialized() false after init\n", i);
            nfail = 1;
            break;
        }

        /* do some work that populates per-cycle state which finalize must
         * tear down: stage a value into the datastore and commit it */
        val.type = PMIX_UINT32;
        val.data.uint32 = (uint32_t) i;
        rc = PMIx_Put(PMIX_GLOBAL, "cycle.key", &val);
        if (PMIX_SUCCESS != rc) {
            fprintf(stderr, "cycle %ld: PMIx_Put failed: %s\n", i,
                    PMIx_Error_string(rc));
            nfail = 1;
            break;
        }
        rc = PMIx_Commit();
        if (PMIX_SUCCESS != rc) {
            fprintf(stderr, "cycle %ld: PMIx_Commit failed: %s\n", i,
                    PMIx_Error_string(rc));
            nfail = 1;
            break;
        }

        /* the operation the user's scenario names explicitly */
        rc = PMIx_Fence(&myproc, 1, NULL, 0);
        if (PMIX_SUCCESS != rc) {
            fprintf(stderr, "cycle %ld: PMIx_Fence failed: %s\n", i,
                    PMIx_Error_string(rc));
            nfail = 1;
            break;
        }

        /* pass PMIX_EMBED_BARRIER so finalize exercises the embedded
         * fence path as well */
        PMIX_INFO_LOAD(&tinfo, PMIX_EMBED_BARRIER, NULL, PMIX_BOOL);
        rc = PMIx_Finalize(&tinfo, 1);
        PMIX_INFO_DESTRUCT(&tinfo);
        if (PMIX_SUCCESS != rc) {
            fprintf(stderr, "cycle %ld: PMIx_Finalize failed: %s\n", i,
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
        fprintf(stdout, "Completed %ld init/finalize cycles: PASS\n\n", ncycles);
    } else {
        fprintf(stdout, "init/finalize cycling: FAIL\n\n");
    }

    return nfail;
}
