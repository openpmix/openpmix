/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * A helper for output-files.sh: spawn a job whose output goes to files in
 * a directory, and wait for it to end.
 *
 *   mu_spawn --dir <dir> [--tool <uri file>] [--nofwd] -- <cmd> [args...]
 *
 * Without --tool it is a client, run under prun, whose rank 0 spawns the
 * job - a child job with files of its own. With --tool it is a tool that
 * attaches to the DVM whose URI is on the first line of <uri file> and
 * spawns the job itself; with --nofwd as well, it asks that none of the
 * job's output be forwarded to it, so it is not the job's file writer.
 *
 * Exits 0 when the spawn succeeded and the job's end was seen.
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <pmix.h>
#include <pmix_tool.h>

static volatile bool job_ended = false;
static volatile bool registered = false;

static void endfn(size_t evhdlr_registration_id, pmix_status_t status,
                  const pmix_proc_t *source, pmix_info_t info[], size_t ninfo,
                  pmix_info_t results[], size_t nresults,
                  pmix_event_notification_cbfunc_fn_t cbfunc, void *cbdata)
{
    (void) evhdlr_registration_id;
    (void) status;
    (void) source;
    (void) info;
    (void) ninfo;
    (void) results;
    (void) nresults;

    job_ended = true;
    if (NULL != cbfunc) {
        cbfunc(PMIX_EVENT_ACTION_COMPLETE, NULL, 0, NULL, NULL, cbdata);
    }
}

static void regfn(pmix_status_t status, size_t ref, void *cbdata)
{
    (void) ref;
    (void) cbdata;

    registered = (PMIX_SUCCESS == status);
}

/* the first line of a report-URI file */
static char *read_uri(const char *path)
{
    char buf[1024], *nl;
    FILE *fp = fopen(path, "r");

    if (NULL == fp) {
        return NULL;
    }
    if (NULL == fgets(buf, sizeof(buf), fp)) {
        fclose(fp);
        return NULL;
    }
    fclose(fp);
    nl = strchr(buf, '\n');
    if (NULL != nl) {
        *nl = '\0';
    }
    return strdup(buf);
}

static int usage(const char *me)
{
    fprintf(stderr, "usage: %s --dir <dir> [--tool <uri file>] [--nofwd] -- <cmd> [args...]\n",
            me);
    return 2;
}

int main(int argc, char **argv)
{
    const char *dir = NULL, *urifile = NULL;
    bool nofwd = false, tool;
    pmix_proc_t me;
    pmix_info_t info[2], jinfo[5];
    pmix_app_t app;
    pmix_nspace_t child;
    pmix_status_t rc, code = PMIX_EVENT_JOB_END;
    size_t nj = 0;
    bool flag;
    int i, n, ret = 1;
    char *uri = NULL;

    for (i = 1; i < argc; i++) {
        if (0 == strcmp(argv[i], "--dir") && i + 1 < argc) {
            dir = argv[++i];
        } else if (0 == strcmp(argv[i], "--tool") && i + 1 < argc) {
            urifile = argv[++i];
        } else if (0 == strcmp(argv[i], "--nofwd")) {
            nofwd = true;
        } else if (0 == strcmp(argv[i], "--")) {
            ++i;
            break;
        } else {
            return usage(argv[0]);
        }
    }
    if (NULL == dir || i >= argc) {
        return usage(argv[0]);
    }
    tool = (NULL != urifile);

    if (tool) {
        uri = read_uri(urifile);
        if (NULL == uri) {
            fprintf(stderr, "mu_spawn: no URI in %s\n", urifile);
            return 1;
        }
        PMIX_INFO_LOAD(&info[0], PMIX_SERVER_URI, uri, PMIX_STRING);
        rc = PMIx_tool_init(&me, info, 1);
        PMIX_INFO_DESTRUCT(&info[0]);
    } else {
        rc = PMIx_Init(&me, NULL, 0);
    }
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "mu_spawn: init failed: %s\n", PMIx_Error_string(rc));
        free(uri);
        return 1;
    }
    if (!tool && 0 != me.rank) {
        /* only rank 0 spawns */
        ret = 0;
        goto done;
    }

    /* be ready to hear of the job's end before it can happen */
    PMIx_Register_event_handler(&code, 1, NULL, 0, endfn, regfn, NULL);
    for (n = 0; n < 100 && !registered; n++) {
        usleep(10000);
    }

    PMIX_APP_CONSTRUCT(&app);
    app.cmd = strdup(argv[i]);
    app.argv = &argv[i];
    app.maxprocs = 1;
    PMIX_INFO_LOAD(&jinfo[nj], PMIX_IOF_OUTPUT_TO_DIRECTORY, dir, PMIX_STRING);
    nj++;
    flag = true;
    PMIX_INFO_LOAD(&jinfo[nj], PMIX_NOTIFY_COMPLETION, &flag, PMIX_BOOL);
    nj++;
    if (nofwd) {
        flag = false;
        PMIX_INFO_LOAD(&jinfo[nj], PMIX_FWD_STDOUT, &flag, PMIX_BOOL);
        nj++;
        PMIX_INFO_LOAD(&jinfo[nj], PMIX_FWD_STDERR, &flag, PMIX_BOOL);
        nj++;
    }

    rc = PMIx_Spawn(jinfo, nj, &app, 1, child);
    app.argv = NULL;
    free(app.cmd);
    app.cmd = NULL;
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "mu_spawn: spawn failed: %s\n", PMIx_Error_string(rc));
        goto done;
    }
    for (n = 0; n < 3000 && !job_ended; n++) {
        usleep(10000);
    }
    if (!job_ended) {
        fprintf(stderr, "mu_spawn: job %s did not end within 30s\n", child);
        goto done;
    }
    ret = 0;

done:
    if (tool) {
        PMIx_tool_finalize();
    } else {
        PMIx_Finalize(NULL, 0);
    }
    free(uri);
    return ret;
}
