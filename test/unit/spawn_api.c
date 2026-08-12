/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Unit tests for PMIx_Spawn's handling of its two input arrays
 * (src/client/pmix_client_spawn.c).
 *
 * The process comes up as a PMIx server with a stub host module, and that
 * is what makes these reachable at all. A singleton *client* is turned
 * away at PMIx_Spawn_nb's "am I connected" gate with PMIX_ERR_UNREACH
 * before either array is looked at. A server is not: the gate lets a
 * server through so it can hand the request to its own host, so the
 * directive scan and the apps copy both run, and the call then stops at
 * the missing pmix_host_server.spawn with PMIX_ERR_NOT_SUPPORTED. That
 * status is therefore the "got all the way through the argument handling"
 * marker every case below tests for.
 *
 *   a non-NULL job_info with a zero count
 *      (info, ninfo) is a pair and a zero count means "no directives"
 *      whatever the pointer is. PMIx_Spawn_nb entered its directive scan
 *      on the pointer alone, built an empty info list from it, and handed
 *      that to PMIx_Info_list_convert() - which reports PMIX_ERR_EMPTY for
 *      an empty list. The spawn failed with that status.
 *
 *   the caller's apps array is const
 *      An app may declare its directives by terminating them with an
 *      end-marked info instead of setting ninfo. PMIx_Spawn_nb counts them
 *      and used to write the count back into apps[n].ninfo - a store into
 *      the caller's "const pmix_app_t apps[]", which segfaults outright
 *      for a caller whose apps sit in read-only storage. The count now
 *      lives in a local. Note the sentinel has to sit past index 0 for
 *      this to be visible: a sentinel at index 0 counts zero, and writing
 *      zero over a zero proves nothing.
 *
 *   an app directive that cannot be copied
 *      The apps copy transferred each directive with PMIX_INFO_XFER,
 *      which discards its status by definition, so a directive the copy
 *      could not carry was dropped in silence - while the job-level ones
 *      have always failed the spawn, through PMIx_Info_list_xfer. Both
 *      halves of one message now report the same way.
 *
 *   the argv / cmd / env shapes
 *      These reach the strdup, PMIx_Argv_copy, pmix_basename and
 *      PMIx_Argv_prepend_nosize results in the copy loop, all of which are
 *      now checked for failure. Nothing here can *make* an allocation
 *      fail, so these do not test the hardening directly; they hold the
 *      ordinary paths down so a screen written the wrong way round cannot
 *      turn a working spawn into an error.
 */

#include "src/include/pmix_config.h"

#include "include/pmix.h"
#include "include/pmix_server.h"

#include "src/include/pmix_globals.h"
#include "src/server/pmix_server_ops.h"
#include "src/threads/pmix_threads.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int npass = 0;
static int nfail = 0;

static void report(const char *name, int passed)
{
    if (passed) {
        fprintf(stdout, "  PASS: %s\n", name);
        ++npass;
    } else {
        fprintf(stdout, "  FAIL: %s\n", name);
        ++nfail;
    }
}

/* A host module entry point, so the server-role branch gets past its
 * "can this host spawn at all?" screen and reaches the thread-shifted
 * tail. It is never actually called by the cases below - each of them
 * fails at the parent lookup, which happens first. */
static bool host_spawn_called = false;
static pmix_status_t stub_spawn(const pmix_proc_t *proc, const pmix_info_t job_info[], size_t ninfo,
                                const pmix_app_t apps[], size_t napps,
                                pmix_spawn_cbfunc_t cbfunc, void *cbdata)
{
    (void) proc; (void) job_info; (void) ninfo; (void) apps; (void) napps;
    (void) cbfunc; (void) cbdata;
    host_spawn_called = true;
    return PMIX_ERR_NOT_SUPPORTED;
}

/* completion recorder for the non-blocking form */
static pmix_lock_t spawnlock;
static pmix_status_t spawn_status = PMIX_SUCCESS;
static void spawn_done(pmix_status_t status, char nspace[], void *cbdata)
{
    (void) nspace; (void) cbdata;
    spawn_status = status;
    PMIX_WAKEUP_THREAD(&spawnlock);
}

int main(int argc, char **argv)
{
    static pmix_server_module_t mymodule = {0};
    pmix_status_t rc;
    pmix_nspace_t nspace;
    pmix_app_t app;
    pmix_info_t jobinfo[2];
    pmix_info_t *appinfo;

    (void) argc;
    (void) argv;

    fprintf(stdout, "spawn_api: PMIx_Spawn argument-handling unit tests\n");

    rc = PMIx_server_init(&mymodule, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "PMIx_server_init failed: %s\n", PMIx_Error_string(rc));
        return 1;
    }
    /* the stub module has no spawn entry point - that is what makes
     * PMIX_ERR_NOT_SUPPORTED the marker for "reached the end of the
     * argument handling", so assert it rather than assume it */
    if (NULL != pmix_host_server.spawn) {
        fprintf(stderr, "test setup error: host module advertises spawn\n");
        PMIx_server_finalize();
        return 1;
    }

    /* --- control: no directives at all ------------------------------ */
    PMIX_APP_CONSTRUCT(&app);
    app.cmd = strdup("true");
    app.maxprocs = 1;
    rc = PMIx_Spawn(NULL, 0, &app, 1, nspace);
    report("a spawn with no directives reaches the host up-call",
           PMIX_ERR_NOT_SUPPORTED == rc);
    PMIX_APP_DESTRUCT(&app);

    /* --- a non-NULL job_info carrying a zero count ------------------- */
    PMIX_INFO_LOAD(&jobinfo[0], PMIX_PERSONALITY, "ompi", PMIX_STRING);
    PMIX_INFO_LOAD(&jobinfo[1], PMIX_PERSONALITY, "ompi", PMIX_STRING);
    PMIX_APP_CONSTRUCT(&app);
    app.cmd = strdup("true");
    app.maxprocs = 1;
    rc = PMIx_Spawn(jobinfo, 0, &app, 1, nspace);
    report("a non-NULL job_info with a zero count is not an empty list",
           PMIX_ERR_NOT_SUPPORTED == rc);
    PMIX_APP_DESTRUCT(&app);

    /* and the ordinary case still works - the guard above must gate on
     * the count, not stop reading directives altogether */
    PMIX_APP_CONSTRUCT(&app);
    app.cmd = strdup("true");
    app.maxprocs = 1;
    rc = PMIx_Spawn(jobinfo, 2, &app, 1, nspace);
    report("job-level directives are still carried when the count is set",
           PMIX_ERR_NOT_SUPPORTED == rc);
    PMIX_APP_DESTRUCT(&app);
    PMIX_INFO_DESTRUCT(&jobinfo[0]);
    PMIX_INFO_DESTRUCT(&jobinfo[1]);

    /* --- an app whose directives are end-marked rather than counted -- */
    PMIX_INFO_CREATE(appinfo, 2);
    if (NULL == appinfo) {
        fprintf(stderr, "test setup error: out of memory\n");
        PMIx_server_finalize();
        return 1;
    }
    PMIX_INFO_LOAD(&appinfo[0], PMIX_PERSONALITY, "ompi", PMIX_STRING);
    PMIX_INFO_SET_END(&appinfo[1]);
    PMIX_APP_CONSTRUCT(&app);
    app.cmd = strdup("true");
    app.maxprocs = 1;
    app.info = appinfo;
    app.ninfo = 0;
    rc = PMIx_Spawn(NULL, 0, &app, 1, nspace);
    report("an end-marked app info array reaches the host up-call",
           PMIX_ERR_NOT_SUPPORTED == rc);
    report("counting an end-marked app info array leaves the caller's app alone",
           0 == app.ninfo && appinfo == app.info);
    /* the app owns nothing we did not give it - hand the info array back
     * to ourselves rather than letting PMIX_APP_DESTRUCT free it twice */
    app.info = NULL;
    app.ninfo = 0;
    PMIX_APP_DESTRUCT(&app);
    PMIX_INFO_FREE(appinfo, 2);

    /* --- an app directive the copy cannot carry ---------------------- */
    /* The apps copy used to transfer each app directive with
     * PMIX_INFO_XFER, which discards its status by definition, so a
     * directive that could not be copied was dropped in silence and the
     * spawn went up carrying an empty slot where the caller had put
     * something. The job-level directives a few lines above have always
     * failed the spawn in that case, through PMIx_Info_list_xfer. This
     * case holds the two halves of one message to the same answer.
     *
     * The library prints "PMIX-XFER-VALUE: UNSUPPORTED TYPE" on the way
     * through, which is it declining rather than faulting - the same
     * bargain test/unit/bfrops_null_object.c makes. */
    PMIX_INFO_CREATE(appinfo, 1);
    if (NULL == appinfo) {
        fprintf(stderr, "test setup error: out of memory\n");
        PMIx_server_finalize();
        return 1;
    }
    PMIX_LOAD_KEY(appinfo[0].key, PMIX_PERSONALITY);
    appinfo[0].value.type = (pmix_data_type_t) (PMIX_DATA_TYPE_MAX + 1);
    PMIX_APP_CONSTRUCT(&app);
    app.cmd = strdup("true");
    app.maxprocs = 1;
    app.info = appinfo;
    app.ninfo = 1;
    rc = PMIx_Spawn(NULL, 0, &app, 1, nspace);
    report("an app directive that cannot be copied fails the spawn",
           PMIX_ERR_NOT_SUPPORTED != rc && PMIX_SUCCESS != rc);
    app.info = NULL;
    app.ninfo = 0;
    PMIX_APP_DESTRUCT(&app);
    PMIX_INFO_FREE(appinfo, 1);

    /* --- the argv/cmd/env paths through the copy loop ---------------- */
    /* These three shapes are what the copy loop's strdup, PMIx_Argv_copy,
     * pmix_basename and PMIx_Argv_prepend_nosize results are reached
     * through, and every one of those is now checked. Nothing here can
     * make an allocation fail - that is why the hardening has no direct
     * regression test - but a screen written the wrong way round turns a
     * working spawn into an error, and these catch that. */
    PMIX_APP_CONSTRUCT(&app);
    PMIx_Argv_append_nosize(&app.argv, "true");
    app.maxprocs = 1;
    rc = PMIx_Spawn(NULL, 0, &app, 1, nspace);
    report("an app with argv and no cmd reaches the host up-call",
           PMIX_ERR_NOT_SUPPORTED == rc);
    PMIX_APP_DESTRUCT(&app);

    /* argv[0] naming something other than the cmd is what drives the
     * prepend, whose status the loop now reads */
    PMIX_APP_CONSTRUCT(&app);
    app.cmd = strdup("true");
    PMIx_Argv_append_nosize(&app.argv, "-x");
    app.maxprocs = 1;
    rc = PMIx_Spawn(NULL, 0, &app, 1, nspace);
    report("an app whose argv does not start with the cmd reaches the host up-call",
           PMIX_ERR_NOT_SUPPORTED == rc);
    PMIX_APP_DESTRUCT(&app);

    /* an env array is copied only when the caller supplied one, since
     * PMIx_Argv_copy answers NULL both for a NULL source and for a
     * failure - so the guard has to be on the source, not the result */
    PMIX_APP_CONSTRUCT(&app);
    app.cmd = strdup("true");
    PMIx_Argv_append_nosize(&app.env, "PMIX_SPAWNUT_VAR=1");
    app.maxprocs = 1;
    rc = PMIx_Spawn(NULL, 0, &app, 1, nspace);
    report("an app carrying an environment reaches the host up-call",
           PMIX_ERR_NOT_SUPPORTED == rc);
    PMIX_APP_DESTRUCT(&app);

    /* --- the server-role tail runs on the progress thread ------------ */
    /* Resolving a PMIX_PARENT_ID means walking pmix_server_globals.clients,
     * which belongs to the progress thread, so that walk and the host
     * up-call that depends on it were moved off the caller's thread. Two
     * things follow, and both are asserted below: the request is now
     * *accepted* rather than refused synchronously, and the failure comes
     * back through the callback instead of the return.
     *
     * Give the module a spawn entry point first, or the branch stops at
     * the "can this host spawn?" screen above the shift and none of this
     * is reached. The parent named is deliberately not one of our clients
     * - there are none - so the lookup fails, which is the one error this
     * handler produces without involving the host at all. */
    pmix_host_server.spawn = stub_spawn;
    host_spawn_called = false;

    PMIX_INFO_LOAD(&jobinfo[0], PMIX_PARENT_ID, &pmix_globals.myid, PMIX_PROC);
    PMIX_APP_CONSTRUCT(&app);
    app.cmd = strdup("true");
    app.maxprocs = 1;

    PMIX_CONSTRUCT_LOCK(&spawnlock);
    spawn_status = PMIX_SUCCESS;
    rc = PMIx_Spawn_nb(jobinfo, 1, &app, 1, spawn_done, NULL);
    report("a spawn naming a parent is accepted rather than refused",
           PMIX_SUCCESS == rc);
    if (PMIX_SUCCESS == rc) {
        PMIX_WAIT_THREAD(&spawnlock);
        report("an unresolvable parent is reported through the callback",
               PMIX_ERR_NOT_FOUND == spawn_status);
        report("the host was not asked to spawn for a parent we could not find",
               !host_spawn_called);
    }
    PMIX_DESTRUCT_LOCK(&spawnlock);
    PMIX_APP_DESTRUCT(&app);

    /* and the blocking form still reports it as its own return, because
     * its wrapper reads the status the callback delivered */
    PMIX_APP_CONSTRUCT(&app);
    app.cmd = strdup("true");
    app.maxprocs = 1;
    rc = PMIx_Spawn(jobinfo, 1, &app, 1, nspace);
    report("the blocking form still returns the parent failure itself",
           PMIX_ERR_NOT_FOUND == rc);
    PMIX_APP_DESTRUCT(&app);
    PMIX_INFO_DESTRUCT(&jobinfo[0]);
    pmix_host_server.spawn = NULL;

    PMIx_server_finalize();

    fprintf(stdout, "spawn_api: %d passed, %d failed\n", npass, nfail);
    return (0 == nfail) ? 0 : 1;
}
