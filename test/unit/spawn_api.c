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
 */

#include "src/include/pmix_config.h"

#include "include/pmix.h"
#include "include/pmix_server.h"

#include "src/include/pmix_globals.h"
#include "src/server/pmix_server_ops.h"

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

    PMIx_server_finalize();

    fprintf(stdout, "spawn_api: %d passed, %d failed\n", npass, nfail);
    return (0 == nfail) ? 0 : 1;
}
