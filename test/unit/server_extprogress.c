/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * A host can ask to drive the PMIx event base itself, by passing
 * PMIX_EXTERNAL_PROGRESS to PMIx_server_init and stepping the library
 * with PMIx_Progress(). No engine thread exists in that mode, and the
 * progress-thread-stopped flag is deliberately left clear because the
 * library is open for business - so a library API that posts an event to
 * the base and then blocks waiting for it stops the one thread that would
 * ever have run it. That is a hang, not an error return.
 *
 * PMIx_server_setup_fork is the API that cannot dodge this. Every other
 * blocking server call has a non-blocking form such a host can use
 * instead; setup_fork has neither a callback nor an environment it could
 * hand back later, so a host in this mode has no alternative to calling
 * it. It therefore runs its body inline here rather than shifting to the
 * progress thread.
 *
 * Nothing else in the test suite drives this mode, so this also serves as
 * the smoke test that a server can be brought up, register a namespace,
 * and be torn down under host-driven progress at all.
 *
 * A regression shows up as a hang rather than a wrong answer, so the whole
 * run is fenced with an alarm: dying on SIGALRM beats blocking a CI run
 * forever.
 */

#include "src/include/pmix_config.h"

#include "include/pmix.h"
#include "include/pmix_server.h"

#include "src/include/pmix_globals.h"
#include "src/util/pmix_argv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SUT_NSPACE "server-extprog-ut"
#define SUT_NPROCS 2

static int npass = 0;
static int nfail = 0;

static void report(const char *name, int passed)
{
    if (passed) {
        ++npass;
        fprintf(stdout, "  PASS: %s\n", name);
    } else {
        ++nfail;
        fprintf(stdout, "  FAIL: %s\n", name);
    }
}

/* the host's own completion flag - set from a PMIx callback, which in this
 * mode is dispatched by our own PMIx_Progress() call, so no atomics are
 * needed: there is only ever one thread here */
static volatile bool opdone = false;
static pmix_status_t opstatus = PMIX_ERR_NOT_SUPPORTED;

static void opcb(pmix_status_t status, void *cbdata)
{
    (void) cbdata;
    opstatus = status;
    opdone = true;
}

/* step the library until the operation we posted has completed */
static pmix_status_t drive(pmix_status_t posted)
{
    if (PMIX_OPERATION_SUCCEEDED == posted) {
        /* completed inline; the callback will not be invoked */
        return PMIX_SUCCESS;
    }
    if (PMIX_SUCCESS != posted) {
        return posted;
    }
    while (!opdone) {
        PMIx_Progress();
    }
    return opstatus;
}

int main(int argc, char **argv)
{
    pmix_info_t iinfo;
    pmix_info_t jinfo[2];
    pmix_nspace_t ns;
    pmix_proc_t proc;
    pmix_status_t rc;
    static pmix_server_module_t mymodule = {0};
    char **env = NULL;
    uint32_t nprocs = SUT_NPROCS;
    bool t = true;

    (void) argc;
    (void) argv;

    fprintf(stdout, "server_extprogress: host-driven progress unit tests\n");

    /* a regression here hangs; do not let it hang forever */
    alarm(60);

    PMIX_INFO_LOAD(&iinfo, PMIX_EXTERNAL_PROGRESS, &t, PMIX_BOOL);
    rc = PMIx_server_init(&mymodule, &iinfo, 1);
    PMIX_INFO_DESTRUCT(&iinfo);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "SKIP: PMIx_server_init(external progress) failed: %s\n",
                PMIx_Error_string(rc));
        return 77;
    }
    report("a server initializes under host-driven progress", 1);

    /* the non-blocking registration form is the one such a host must use -
     * the blocking form would wait on an event nobody is running */
    PMIX_INFO_LOAD(&jinfo[0], PMIX_JOB_SIZE, &nprocs, PMIX_UINT32);
    PMIX_INFO_LOAD(&jinfo[1], PMIX_UNIV_SIZE, &nprocs, PMIX_UINT32);
    PMIX_LOAD_NSPACE(ns, SUT_NSPACE);
    opdone = false;
    rc = drive(PMIx_server_register_nspace(ns, SUT_NPROCS, jinfo, 2, opcb, NULL));
    PMIX_INFO_DESTRUCT(&jinfo[0]);
    PMIX_INFO_DESTRUCT(&jinfo[1]);
    report("a namespace registers under host-driven progress", PMIX_SUCCESS == rc);

    /* the point of the test: setup_fork has no non-blocking form, so it
     * must not post-and-wait here */
    PMIX_LOAD_PROCID(&proc, SUT_NSPACE, 0);
    rc = PMIx_server_setup_fork(&proc, &env);
    report("setup_fork returns under host-driven progress rather than hanging",
           PMIX_SUCCESS == rc);
    report("and it still produced an environment", 0 < PMIx_Argv_count(env));
    PMIx_Argv_free(env);

    PMIx_server_finalize();

    alarm(0);

    fprintf(stdout, "server_extprogress: %d passed, %d failed\n", npass, nfail);
    return (0 == nfail) ? 0 : 1;
}
