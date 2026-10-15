/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * A process a launcher starts itself, through PMIx_Spawn with no host to
 * do it, connects back to the launcher by the address it is handed. Its
 * namespace is not one a host registered, so it is given no connection
 * token - and is taken without one - and one in the launcher's own
 * environment is not passed on to it. Nor is any other variable that says
 * who the launcher is or how it reaches its own server: the child is given
 * its own. The rest of the launcher's environment is.
 */

#include "src/include/pmix_config.h"

#include "include/pmix.h"
#include "include/pmix_tool.h"
#include "src/include/pmix_globals.h"
#include "src/mca/ptl/base/base.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* what the launcher puts in its environment after its init: values of the
 * variables that say who a process is, and how it reaches its server */
static const char *const stale[] = {"PMIX_SERVER_URI41", "PMIX_SERVER_LOCAL_URI",
                                    "PMIX_GDS_MODULE", "PMIX_LAUNCHER_RNDZ_URI",
                                    "PMIX_NAMESPACE", NULL};
#define STALE "stale.value"
#define KEPT  "PMIX_UT_PFEXEC_KEPT"

/* the child: attach back to the launcher by its URI, and say how it went,
 * whether it had a token, how many of the launcher's stale values it was
 * started with, and whether it had the launcher's other variable */
static int run_child(const char *uri, const char *out)
{
    pmix_proc_t me;
    pmix_info_t info;
    pmix_status_t rc;
    FILE *fp;
    const char *v;
    int had, nstale = 0, n, kept;

    /* what it was started with - its init takes these out */
    had = (NULL == getenv(PMIX_PTL_TOKEN_ENV)) ? 0 : 1;
    for (n = 0; NULL != stale[n]; n++) {
        if (NULL != (v = getenv(stale[n])) && 0 == strcmp(v, STALE)) {
            fprintf(stderr, "    the child has the launcher's %s\n", stale[n]);
            ++nstale;
        }
    }
    kept = (NULL != getenv(KEPT)) ? 1 : 0;
    PMIX_INFO_LOAD(&info, PMIX_SERVER_URI, uri, PMIX_STRING);
    rc = PMIx_tool_init(&me, &info, 1);
    fp = fopen(out, "w");
    if (NULL != fp) {
        fprintf(fp, "%d %d %d %d\n", (int) rc, had, nstale, kept);
        fclose(fp);
    }
    if (PMIX_SUCCESS == rc) {
        PMIx_tool_finalize();
    }
    return 0;
}

int main(int argc, char **argv)
{
    pmix_proc_t myproc;
    pmix_info_t tinfo[3];
    pmix_status_t rc;
    pmix_app_t *app;
    pmix_nspace_t child;
    char *uri, out[1024];
    const char *tmp = getenv("TMPDIR");
    FILE *fp;
    int i, crc = 1, hastoken = 1, nstale = -1, kept = 0, npass = 0, nfail = 0;

    if (4 == argc && 0 == strcmp(argv[1], "child")) {
        return run_child(argv[2], argv[3]);
    }
    fprintf(stdout, "\n=== a process a launcher starts itself connects back ===\n\n");
    snprintf(out, sizeof(out), "%s/ptl_pfexec_token.%d", (NULL == tmp) ? "/tmp" : tmp,
             (int) getpid());
    unlink(out);

    PMIX_INFO_LOAD(&tinfo[0], PMIX_LAUNCHER, NULL, PMIX_BOOL);
    PMIX_INFO_LOAD(&tinfo[1], PMIX_TOOL_DO_NOT_CONNECT, NULL, PMIX_BOOL);
    PMIX_INFO_LOAD(&tinfo[2], PMIX_SERVER_TOOL_SUPPORT, NULL, PMIX_BOOL);
    rc = PMIx_tool_init(&myproc, tinfo, 3);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "launcher init failed: %s\n", PMIx_Error_string(rc));
        return 1;
    }
    /* a token of someone else's, in our environment - set after our init,
     * which would have taken it out */
    setenv(PMIX_PTL_TOKEN_ENV, "other.server.0;000102030405060708090a0b0c0d0e0f", 1);
    /* and who we are, and how we reach a server, as such a process's own
     * environment may say - and a variable of no such kind */
    for (i = 0; NULL != stale[i]; i++) {
        setenv(stale[i], STALE, 1);
    }
    setenv(KEPT, "yes", 1);
    uri = (NULL != pmix_ptl_base.listener.uri) ? pmix_ptl_base.listener.uri
                                               : pmix_ptl_base.local_listener.uri;

    PMIX_APP_CREATE(app, 1);
    app[0].cmd = strdup(argv[0]);
    app[0].argv = (char **) calloc(5, sizeof(char *));
    app[0].argv[0] = strdup(argv[0]);
    app[0].argv[1] = strdup("child");
    app[0].argv[2] = strdup(uri);
    app[0].argv[3] = strdup(out);
    app[0].maxprocs = 1;
    memset(child, 0, sizeof(child));
    rc = PMIx_Spawn(NULL, 0, app, 1, child);
    PMIX_APP_FREE(app, 1);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "spawn failed: %s\n", PMIx_Error_string(rc));
        PMIx_tool_finalize();
        return 1;
    }

    for (i = 0; i < 300; i++) {
        usleep(100000);
        fp = fopen(out, "r");
        if (NULL != fp) {
            if (4 == fscanf(fp, "%d %d %d %d", &crc, &hastoken, &nstale, &kept)) {
                fclose(fp);
                break;
            }
            fclose(fp);
        }
    }
    unlink(out);

    if (0 == crc) {
        fprintf(stdout, "  PASS: the child connects back to its launcher\n");
        ++npass;
    } else {
        fprintf(stdout, "  FAIL: the child connects back to its launcher (%s)\n",
                (300 == i) ? "it never reported" : PMIx_Error_string((pmix_status_t) crc));
        ++nfail;
    }
    if (300 > i && 0 == hastoken) {
        fprintf(stdout, "  PASS: the child has no connection token\n");
        ++npass;
    } else {
        fprintf(stdout, "  FAIL: the child has no connection token\n");
        ++nfail;
    }
    if (300 > i && 0 == nstale) {
        fprintf(stdout, "  PASS: the child has none of the launcher's own identity\n");
        ++npass;
    } else {
        fprintf(stdout, "  FAIL: the child has none of the launcher's own identity (%d)\n",
                nstale);
        ++nfail;
    }
    if (300 > i && 1 == kept) {
        fprintf(stdout, "  PASS: the child has the rest of the launcher's environment\n");
        ++npass;
    } else {
        fprintf(stdout, "  FAIL: the child has the rest of the launcher's environment\n");
        ++nfail;
    }
    PMIx_tool_finalize();
    fprintf(stdout, "\n%d passed, %d failed\n", npass, nfail);
    return (0 == nfail) ? 0 : 1;
}
