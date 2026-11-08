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
 * Exits 0 when the spawn succeeded and the job's end was seen. The end
 * waited for is the spawned job's own: a server's PMIX_EVENT_JOB_END
 * reaches every process of its allocation, and an earlier job's end -
 * replayed to a handler that registers after it - is not this one's.
 */

#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <pmix.h>
#include <pmix_tool.h>

static volatile bool registered = false;

/* the jobs whose end has been seen - the spawned job's may arrive before
 * PMIx_Spawn has said which job that is */
#define MAX_ENDED 64
static pthread_mutex_t endlock = PTHREAD_MUTEX_INITIALIZER;
static pmix_nspace_t ended[MAX_ENDED];
static size_t nended = 0;

static void note_ended(const char *nspace)
{
    if (NULL == nspace || '\0' == nspace[0]) {
        return;
    }
    pthread_mutex_lock(&endlock);
    if (nended < MAX_ENDED) {
        PMIX_LOAD_NSPACE(ended[nended], nspace);
        ++nended;
    }
    pthread_mutex_unlock(&endlock);
}

static bool has_ended(const char *nspace)
{
    bool seen = false;
    size_t n;

    pthread_mutex_lock(&endlock);
    for (n = 0; n < nended && !seen; n++) {
        seen = (0 == strncmp(ended[n], nspace, PMIX_MAX_NSLEN));
    }
    pthread_mutex_unlock(&endlock);
    return seen;
}

static void endfn(size_t evhdlr_registration_id, pmix_status_t status,
                  const pmix_proc_t *source, pmix_info_t info[], size_t ninfo,
                  pmix_info_t results[], size_t nresults,
                  pmix_event_notification_cbfunc_fn_t cbfunc, void *cbdata)
{
    size_t n;

    (void) evhdlr_registration_id;
    (void) status;
    (void) results;
    (void) nresults;

    /* the job that ended is the event's source, or the proc it affects */
    if (NULL != source) {
        note_ended(source->nspace);
    }
    for (n = 0; n < ninfo; n++) {
        if (PMIX_CHECK_KEY(&info[n], PMIX_EVENT_AFFECTED_PROC) &&
            PMIX_PROC == info[n].value.type && NULL != info[n].value.data.proc) {
            note_ended(info[n].value.data.proc->nspace);
        }
    }
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
    for (n = 0; n < 3000 && !has_ended(child); n++) {
        usleep(10000);
    }
    if (!has_ended(child)) {
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
