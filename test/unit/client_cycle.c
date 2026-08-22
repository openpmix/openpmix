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
 *
 * A second, unrelated group of cases runs first: what PMIx_Init makes of
 * the rank the launcher hands it in PMIX_RANK. That variable used to be
 * read with a bare strtol() whose result was never inspected, and strtol
 * answers 0 for any string it cannot read at all - so an empty, negative,
 * or non-numeric PMIX_RANK made the process rank 0, colliding with the
 * real rank 0 and producing a job that was quietly wrong instead of one
 * that refused to start.
 *
 * Each case runs in a forked child, so that the rejection it asks for is
 * that process's first PMIx_Init and nothing carries between cases. The
 * children run before the parent's own cycling, while the parent is
 * still single-threaded, which is what makes fork() safe here.
 *
 * The last of them is about the other half of the same behavior: a
 * PMIx_Init that fails unwinds, so the caller can fix what was wrong and
 * ask again. It could not always - the "an init has been called" latch
 * and the half-built runtime behind it used to outlive the failure, and
 * every later attempt in that process was answered PMIX_ERR_INIT.
 */

#include "src/include/pmix_config.h"

#include "include/pmix.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define DEFAULT_CYCLES 1200

/* Run one PMIx_Init in a child with PMIX_RANK set to "rank", and report
 * whether it answered the way "expect_bad" asks for. There is no server,
 * so a rank the library accepts comes back as PMIX_ERR_UNREACH - the
 * point of each case is only whether the rank itself was refused. */
static int rank_case(const char *rank, bool expect_bad, const char *what)
{
    pid_t pid;
    int status = 0;

    pid = fork();
    if (0 > pid) {
        fprintf(stderr, "  ERROR: fork failed for %s\n", what);
        return 1;
    }
    if (0 == pid) {
        pmix_proc_t p;
        pmix_status_t crc;
        bool ok;

        setenv("PMIX_NAMESPACE", "rankcheck", 1);
        setenv("PMIX_RANK", rank, 1);
        memset(&p, 0, sizeof(p));
        crc = PMIx_Init(&p, NULL, 0);
        if (expect_bad) {
            ok = (PMIX_ERR_BAD_PARAM == crc);
        } else {
            /* accepted, and the value we were given is the value we use */
            ok = (PMIX_ERR_BAD_PARAM != crc && 7 == p.rank);
        }
        _exit(ok ? 0 : 1);
    }
    if (pid != waitpid(pid, &status, 0)) {
        fprintf(stderr, "  ERROR: waitpid failed for %s\n", what);
        return 1;
    }
    if (WIFEXITED(status) && 0 == WEXITSTATUS(status)) {
        fprintf(stdout, "  PASS: %s\n", what);
        return 0;
    }
    fprintf(stdout, "  FAIL: %s\n", what);
    return 1;
}

/* A PMIx_Init that fails has to leave the process able to try again.
 * The library used to keep the "an init has been called" latch, and the
 * half-built runtime behind it, for the life of the process - so a
 * caller that got an error back and wanted to retry with different
 * directives was answered PMIX_ERR_INIT forever. */
static int retry_after_failure(void)
{
    pid_t pid;
    int status = 0;

    pid = fork();
    if (0 > pid) {
        fprintf(stderr, "  ERROR: fork failed for the retry case\n");
        return 1;
    }
    if (0 == pid) {
        pmix_proc_t p;
        pmix_status_t crc;

        setenv("PMIX_NAMESPACE", "rankcheck", 1);
        setenv("PMIX_RANK", "not-a-rank", 1);
        memset(&p, 0, sizeof(p));
        if (PMIX_ERR_BAD_PARAM != PMIx_Init(&p, NULL, 0)) {
            _exit(1);
        }
        /* now fix what was wrong and ask again */
        setenv("PMIX_RANK", "5", 1);
        memset(&p, 0, sizeof(p));
        crc = PMIx_Init(&p, NULL, 0);
        if (PMIX_ERR_INIT == crc || PMIX_ERR_BAD_PARAM == crc) {
            _exit(1);
        }
        if (!PMIx_Initialized() || 5 != p.rank) {
            _exit(1);
        }
        if (PMIX_SUCCESS != PMIx_Finalize(NULL, 0)) {
            _exit(1);
        }
        _exit(0);
    }
    if (pid != waitpid(pid, &status, 0)) {
        fprintf(stderr, "  ERROR: waitpid failed for the retry case\n");
        return 1;
    }
    if (WIFEXITED(status) && 0 == WEXITSTATUS(status)) {
        fprintf(stdout, "  PASS: a failed init can be retried\n");
        return 0;
    }
    fprintf(stdout, "  FAIL: a failed init can be retried\n");
    return 1;
}

static int check_rank_screening(void)
{
    int nbad = 0;

    fprintf(stdout, "\n=== PMIX_RANK screening ===\n\n");
    nbad += rank_case("7", false, "a well-formed rank is accepted and used");
    nbad += rank_case("", true, "an empty PMIX_RANK is refused");
    nbad += rank_case("abc", true, "a non-numeric PMIX_RANK is refused");
    nbad += rank_case("3x", true, "trailing garbage is refused");
    nbad += rank_case("-1", true, "a negative PMIX_RANK is refused");
    nbad += rank_case("4294967295", true, "a reserved rank value is refused");
    nbad += retry_after_failure();
    return nbad;
}

int main(int argc, char **argv)
{
    pmix_proc_t myproc;
    pmix_status_t rc;
    pmix_value_t val;
    pmix_info_t tinfo;
    long ncycles = DEFAULT_CYCLES;
    long i;
    int nfail = 0;
    int nbadrank;

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

    nbadrank = check_rank_screening();

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

    return (0 == nfail && 0 == nbadrank) ? 0 : 1;
}
