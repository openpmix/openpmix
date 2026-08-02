/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * PMIx_Spawn with PMIX_SETUP_APP_ENVARS, from a client.
 *
 * Two defects, both in PMIx_Spawn_nb (src/client/pmix_client_spawn.c):
 *
 * 1. The directive failed the whole spawn with PMIX_ERR_INIT. The pmdl
 *    framework, which does the harvest, is opened only by the server and tool
 *    roles - so its stub answers a client with PMIX_ERR_INIT, meaning "no
 *    programming-model support is open here", and that was treated as a fatal
 *    error. A client could therefore not use the attribute at all, and the
 *    reason it was given ("the library is not initialized") pointed nowhere
 *    near the cause. This is the check that fails against the unfixed
 *    library, and it is the main subject of this program.
 *
 * 2. The harvest ran against the CALLER's apps array. "apps" is declared
 *    const, and PMIx_Spawn_nb copies it precisely because it modifies what it
 *    spawns - but the harvest happened while scanning the job-level
 *    directives, above the point where that copy is made, so PMIx_Setenv()
 *    edited apps[m].env in place. Reusing the same pmix_app_t for a second
 *    spawn then accumulated the envars again.
 *
 *    Note what this program can and cannot show about (2): with no pmdl open
 *    in the client role there is nothing to harvest, so the array-length
 *    checks below hold either way here. They are a guard against the
 *    mutation being reintroduced, not a reproducer. The roles that DO harvest
 *    - tool and server, e.g. PRRTE's prun - are where it actually bit.
 *
 * All of this needs a real server: PMIx_Spawn_nb stops at the "am I
 * connected?" check long before it looks at the directives, so a singleton
 * (test/unit/client_api.c) reaches none of it. test/unit/run_spawnenvars.pl
 * drives this under test/simple/simptest, which fakes the launch itself but
 * runs the whole client-side path.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "examples.h"
#include <pmix.h>

static pmix_proc_t myproc;
static int nfail = 0;

static void check(int ok, const char *what)
{
    if (ok) {
        fprintf(stderr, "[%s:%u] ok    : %s\n", myproc.nspace, myproc.rank, what);
    } else {
        fprintf(stderr, "[%s:%u] FAILED: %s\n", myproc.nspace, myproc.rank, what);
        nfail++;
    }
}

#define MARKER "SPAWN_REUSE_MARKER=1"

int main(int argc, char **argv)
{
    pmix_status_t rc;
    pmix_app_t *app;
    pmix_info_t jinfo[1];
    pmix_nspace_t child;
    pmix_value_t bogus;
    size_t nenv_before, nenv_after;

    EXAMPLES_HIDE_UNUSED_PARAMS(argc, argv);

    rc = PMIx_Init(&myproc, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "PMIx_Init failed: %s\n", PMIx_Error_string(rc));
        exit(1);
    }
    fprintf(stderr, "[%s:%u] spawn_reuse running\n", myproc.nspace, myproc.rank);

    PMIX_APP_CREATE(app, 1);
    app->cmd = strdup("/bin/true");
    app->maxprocs = 1;
    PMIx_Argv_append_nosize(&app->argv, "/bin/true");
    /* one known envar of our own - anything the library adds lands beside it */
    PMIx_Argv_append_nosize(&app->env, MARKER);
    nenv_before = PMIx_Argv_count(app->env);
    check(1 == nenv_before, "the app starts with exactly our own envar");

    /* ask for the model envars to be harvested - this is the directive whose
     * handling used to reach across into the caller's array */
    PMIX_INFO_LOAD(&jinfo[0], PMIX_SETUP_APP_ENVARS, NULL, PMIX_BOOL);

    rc = PMIx_Spawn(jinfo, 1, app, 1, child);
    fprintf(stderr, "[%s:%u] PMIx_Spawn returned %s\n",
            myproc.nspace, myproc.rank, PMIx_Error_string(rc));
    check(PMIX_SUCCESS == rc, "PMIx_Spawn with PMIX_SETUP_APP_ENVARS succeeded");

    /* the whole point: our array is exactly as we left it */
    nenv_after = PMIx_Argv_count(app->env);
    check(nenv_before == nenv_after,
          "PMIx_Spawn left the caller's app env array at its original length");
    check(NULL != app->env && NULL != app->env[0] && 0 == strcmp(app->env[0], MARKER),
          "PMIx_Spawn left the caller's own envar untouched");

    PMIX_INFO_DESTRUCT(&jinfo[0]);

    /* A PMIX_PARENT_ID carrying something that is not a pmix_proc_t used to
     * be handed straight to PMIX_XFER_PROCID, which dereferences the union's
     * proc pointer - whatever scalar happened to occupy that slot. */
    PMIX_VALUE_LOAD(&bogus, &nenv_after, PMIX_SIZE);
    PMIX_INFO_CONSTRUCT(&jinfo[0]);
    PMIx_Load_key(jinfo[0].key, PMIX_PARENT_ID);
    jinfo[0].value.type = bogus.type;
    jinfo[0].value.data = bogus.data;
    rc = PMIx_Spawn(jinfo, 1, app, 1, child);
    check(PMIX_ERR_BAD_PARAM == rc, "a mistyped PMIX_PARENT_ID is rejected, not dereferenced");

    PMIX_APP_FREE(app, 1);

    rc = PMIx_Finalize(NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "[%s:%u] PMIx_Finalize failed: %s\n",
                myproc.nspace, myproc.rank, PMIx_Error_string(rc));
        nfail++;
    }

    if (0 == nfail) {
        fprintf(stderr, "[%s:%u] spawn_reuse: PASS\n", myproc.nspace, myproc.rank);
    } else {
        fprintf(stderr, "[%s:%u] spawn_reuse: FAIL (%d)\n", myproc.nspace, myproc.rank, nfail);
    }
    fflush(stderr);
    return (0 == nfail) ? 0 : 1;
}
