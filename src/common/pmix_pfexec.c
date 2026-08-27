/*
 * Copyright (c) 2004-2010 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2011 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2007-2011 Oracle and/or its affiliates.  All rights reserved.
 * Copyright (c) 2011      Oak Ridge National Labs.  All rights reserved.
 * Copyright (c) 2011-2013 Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2011-2017 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2013-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2014-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2017      Mellanox Technologies Ltd. All rights reserved.
 * Copyright (c) 2017      IBM Corporation. All rights reserved.
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
 * Copyright (c) 2022      Amazon.com, Inc. or its affiliates.
 *                         All Rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "pmix_config.h"

#include <stdlib.h>
#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif
#include <errno.h>
#include <sys/types.h>
#ifdef HAVE_SYS_WAIT_H
#    include <sys/wait.h>
#endif
#include <signal.h>
#ifdef HAVE_UTIL_H
#    include <util.h>
#endif
#ifdef HAVE_PTY_H
#    include <pty.h>
#endif
#ifdef HAVE_FCNTL_H
#    include <fcntl.h>
#endif
#ifdef HAVE_TERMIOS_H
#    include <termios.h>
#    ifdef HAVE_TERMIO_H
#        include <termio.h>
#    endif
#endif
#ifdef HAVE_LIBUTIL_H
#    include <libutil.h>
#endif

#include "include/pmix.h"
#include "pmix_common.h"
#include "include/pmix_server.h"

#include "src/include/pmix_globals.h"
#include "src/include/pmix_stdint.h"
#include "src/mca/gds/base/base.h"
#include "src/mca/ptl/base/base.h"
#include "src/threads/pmix_threads.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_context_fns.h"
#include "src/util/pmix_error.h"
#include "src/util/pmix_name_fns.h"
#include "src/util/pmix_os_dirpath.h"
#include "src/util/pmix_os_path.h"
#include "src/util/pmix_path.h"
#include "src/util/pmix_environ.h"
#include "src/util/pmix_pty.h"
#include "src/util/pmix_printf.h"
#include "src/util/pmix_show_help.h"

#include "src/client/pmix_client_ops.h"
#include "src/common/pmix_pfexec.h"
#include "src/server/pmix_server_ops.h"

#ifndef MAXPATHLEN /* Hurd */
#define MAXPATHLEN 1024
#endif

static pmix_status_t setup_prefork(pmix_pfexec_child_t *child);
static pmix_status_t register_nspace(char *nspace, pmix_setup_caddy_t *fcd);
static void child_poll(int sd, short args, void *cbdata);
static void pfexec_poll_arm(bool reset);
static void pfexec_poll_disarm(void);
static int fork_proc(pmix_app_t *app, pmix_pfexec_child_t *child, char **env);

pmix_pfexec_globals_t pmix_pfexec_globals = {
    .poll_ev = NULL,
    .poll_active = false,
    .poll_delay = 0,
    .poll_interval = PMIX_PFEXEC_POLL_INTERVAL,
    .poll_max_interval = PMIX_PFEXEC_POLL_MAX_INTERVAL,
    .children = PMIX_LIST_STATIC_INIT(pmix_pfexec_globals.children),
    .timeout_before_sigkill = 0,
    .nextid = 0,
    .selected = false
};

int pmix_pfexec_base_close(void)
{
    /* nothing to tear down if we were never opened - the children list
     * has not been constructed, so destructing it would walk garbage */
    if (!pmix_pfexec_globals.initialized) {
        return PMIX_SUCCESS;
    }
    if (pmix_pfexec_globals.poll_active) {
        pmix_event_del(pmix_pfexec_globals.poll_ev);
        pmix_pfexec_globals.poll_active = false;
    }
    PMIX_LIST_DESTRUCT(&pmix_pfexec_globals.children);
    free(pmix_pfexec_globals.poll_ev);
    pmix_pfexec_globals.poll_ev = NULL;
    pmix_pfexec_globals.poll_delay = 0;
    pmix_pfexec_globals.selected = false;
    pmix_pfexec_globals.initialized = false;

    return PMIX_SUCCESS;
}

/* Take a child off the children list if it is still on it, and drop the
 * reference the list was holding.
 *
 * Both the completion path and the kill sequence remove children, and
 * either can reach a given child first: a completion posted by the IOF
 * read handler or by child_poll sits queued on the event base, and the
 * kill sequence the tool's finalize drives can be dispatched in between.
 * Whoever gets here first does the removal and drops the list reference;
 * the second call is a no-op. Callers must hold a reference of their own
 * - this may drop the last one. */
static void child_delist(pmix_pfexec_child_t *child)
{
    if (!child->onlist) {
        return;
    }
    child->onlist = false;
    pmix_list_remove_item(&pmix_pfexec_globals.children, &child->super);
    PMIX_RELEASE(child);
}

static void _ntfy_done(pmix_status_t status, void *cbdata)
{
    pmix_pfexec_cmpl_caddy_t *cd = (pmix_pfexec_cmpl_caddy_t*)cbdata;
    PMIX_HIDE_UNUSED_PARAMS(status);

    PMIX_RELEASE(cd->child);
    PMIX_RELEASE(cd);
}

void pmix_pfexec_check_complete(int sd, short args, void *cbdata)
{
    pmix_pfexec_cmpl_caddy_t *cd = (pmix_pfexec_cmpl_caddy_t *) cbdata;
    pmix_status_t rc;
    pmix_pfexec_child_t *child;
    bool stillalive = false;
    pmix_proc_t wildcard;
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    child_delist(cd->child);
    /* see if any more children from this nspace are alive */
    PMIX_LIST_FOREACH (child, &pmix_pfexec_globals.children, pmix_pfexec_child_t) {
        if (PMIX_CHECK_NSPACE(child->proc.nspace, cd->child->proc.nspace)) {
            stillalive = true;
        }
    }
    if (!stillalive) {
        /* generate a local event indicating job terminated */
        PMIX_INFO_LOAD(&cd->info[0], PMIX_EVENT_NON_DEFAULT, NULL, PMIX_BOOL);
        PMIX_LOAD_NSPACE(wildcard.nspace, cd->child->proc.nspace);
        PMIX_INFO_LOAD(&cd->info[1], PMIX_EVENT_AFFECTED_PROC, &wildcard, PMIX_PROC);
        rc = PMIx_Notify_event(PMIX_ERR_JOB_TERMINATED, &pmix_globals.myid, PMIX_RANGE_PROC_LOCAL,
                               cd->info, 2, _ntfy_done, cd);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_RELEASE(cd->child);
            PMIX_RELEASE(cd);
        }
    } else {
        /* this proc is done, but other procs in its nspace are still
         * running - release the completed child and the caddy so they
         * are not leaked */
        PMIX_RELEASE(cd->child);
        PMIX_RELEASE(cd);
    }
}

int pmix_pfexec_register(void)
{
    pmix_pfexec_globals.timeout_before_sigkill = 1;
    pmix_mca_base_var_register("pmix", "pfexec", "base", "sigkill_timeout",
                               "Time to wait for a process to die after issuing a kill signal to it",
                               PMIX_MCA_BASE_VAR_TYPE_INT,
                               &pmix_pfexec_globals.timeout_before_sigkill);
    pmix_pfexec_globals.poll_interval = PMIX_PFEXEC_POLL_INTERVAL;
    pmix_mca_base_var_register("pmix", "pfexec", "base", "poll_interval",
                               "Milliseconds before the first check of a forked child's exit "
                               "status [default: 50]",
                               PMIX_MCA_BASE_VAR_TYPE_INT,
                               &pmix_pfexec_globals.poll_interval);
    if (0 >= pmix_pfexec_globals.poll_interval) {
        pmix_pfexec_globals.poll_interval = PMIX_PFEXEC_POLL_INTERVAL;
    }
    pmix_pfexec_globals.poll_max_interval = PMIX_PFEXEC_POLL_MAX_INTERVAL;
    pmix_mca_base_var_register("pmix", "pfexec", "base", "poll_max_interval",
                               "Ceiling in milliseconds that the child exit-status check backs "
                               "off to [default: 1000]",
                               PMIX_MCA_BASE_VAR_TYPE_INT,
                               &pmix_pfexec_globals.poll_max_interval);
    if (pmix_pfexec_globals.poll_max_interval < pmix_pfexec_globals.poll_interval) {
        pmix_pfexec_globals.poll_max_interval = pmix_pfexec_globals.poll_interval;
    }
    return PMIX_SUCCESS;
}

/**
 * Function for finding and opening either all MCA components, or the one
 * that was specifically requested via a MCA parameter.
 */
int pmix_pfexec_base_open(void)
{
    memset(&pmix_pfexec_globals, 0, sizeof(pmix_pfexec_globals_t));

    /* setup the list of children */
    PMIX_CONSTRUCT(&pmix_pfexec_globals.children, pmix_list_t);
    pmix_pfexec_globals.nextid = 1;
    pmix_pfexec_globals.initialized = true;

    /* Open up all available components */
    return PMIX_SUCCESS;
}

static pmix_status_t setup_path(pmix_app_t *app)
{
    pmix_status_t rc;
    char dir[MAXPATHLEN];

    /* see if the app specifies a working dir */
    if (NULL != app->cwd) {
        /* Try to change to the app's cwd and check that the app
           exists and is executable The function will
           take care of outputting a pretty error message, if required
        */
        if (PMIX_SUCCESS != (rc = pmix_util_check_context_cwd(&app->cwd, true, true))) {
            /* do not ERROR_LOG - it will be reported elsewhere */
            return rc;
        }

        /* The prior function will have done a chdir() to jump us to
         * wherever the app is to be executed. It seems that chdir doesn't
         * adjust the $PWD enviro variable when it changes the directory. This
         * can cause a user to get a different response when doing getcwd vs
         * looking at the enviro variable. To keep this consistent, we explicitly
         * ensure that the PWD enviro variable matches the CWD we moved to.
         *
         * NOTE: if a user's program does a chdir(), then $PWD will once
         * again not match getcwd! This is beyond our control - we are only
         * ensuring they start out matching.
         */
        if (NULL == getcwd(dir, sizeof(dir))) {
            return PMIX_ERR_OUT_OF_RESOURCE;
        }
        PMIx_Setenv("PWD", dir, true, &app->env);
    }

    /* ensure the app is pointing to a full path */
    rc = pmix_util_check_context_app(&app->cmd, app->cwd, app->env);

    return rc;
}

pmix_status_t pmix_pfexec_base_spawn_job(pmix_setup_caddy_t *fcd)
{
    pmix_output_verbose(5, pmix_client_globals.spawn_output,
                        "%s pfexec spawning child job",
                        PMIX_NAME_PRINT(&pmix_globals.myid));

    /* The exit-status poll is armed once we actually have a child - see
     * the note on poll_ev in pmix_pfexec.h for why this is a poll and
     * not the SIGCHLD handler it used to be. */
    if (NULL == pmix_pfexec_globals.poll_ev) {
        pmix_pfexec_globals.poll_ev = (pmix_event_t *) malloc(sizeof(pmix_event_t));
        if (NULL == pmix_pfexec_globals.poll_ev) {
            return PMIX_ERR_NOMEM;
        }
    }

    PMIX_PFEXEC_SPAWN(fcd);

    return PMIX_SUCCESS;
}

void pmix_pfexec_base_spawn_proc(int sd, short args, void *cbdata)
{
    pmix_setup_caddy_t *fcd = (pmix_setup_caddy_t *) cbdata;
    pmix_app_t *app;
    int i, n;
    size_t m, k;
    pmix_status_t rc;
    char **argv = NULL, **env = NULL;
    /* the completion callback below reports this even on the error
     * paths that jump there before a namespace has been composed */
    pmix_nspace_t nspace = {0};
    char basedir[MAXPATHLEN], sock[10];
    pmix_pfexec_child_t *child;
    pmix_rank_info_t *info;
    pmix_namespace_t *nptr;
    pmix_rank_t rank = 0;
    char tmp[2048];
    bool nohup = false;
    char *security_mode;
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    pmix_output_verbose(5, pmix_client_globals.spawn_output,
                        "%s pfexec:base spawn proc",
                        PMIX_NAME_PRINT(&pmix_globals.myid));

    /* establish our baseline working directory - we will be potentially
     * bouncing around as we execute various apps, but we will always return
     * to this place as our default directory
     */
    if (NULL == getcwd(basedir, sizeof(basedir))) {
        rc = PMIX_ERROR;
        goto complete;
    }

    /* create a namespace for the new job */
    memset(tmp, 0, 2048);
    (void) pmix_snprintf(tmp, 2047, "%s:%lu", pmix_globals.myid.nspace,
                    (unsigned long) pmix_pfexec_globals.nextid);
    PMIX_LOAD_NSPACE(nspace, tmp);
    ++pmix_pfexec_globals.nextid;

    /* add the nspace to the server global list */
    nptr = PMIX_NEW(pmix_namespace_t);
    nptr->nspace = strdup(nspace);
    pmix_list_append(&pmix_globals.nspaces, &nptr->super);

    /* locally cache any job info that will later need to
     * be communicated to the spawned job */
    rc = register_nspace(nspace, fcd);
    if (PMIX_SUCCESS != rc) {
        pmix_list_remove_item(&pmix_globals.nspaces, &nptr->super);
        PMIX_RELEASE(nptr);
        goto complete;
    }

    for (k = 0; k < fcd->ninfo; k++) {
        if (PMIX_CHECK_KEY(&fcd->info[k], PMIX_SET_ENVAR)) {

        } else if (PMIX_CHECK_KEY(&fcd->info[k], PMIX_ADD_ENVAR)) {

        } else if (PMIX_CHECK_KEY(&fcd->info[k], PMIX_UNSET_ENVAR)) {
        } else if (PMIX_CHECK_KEY(&fcd->info[k], PMIX_PREPEND_ENVAR)) {
        } else if (PMIX_CHECK_KEY(&fcd->info[k], PMIX_APPEND_ENVAR)) {
        } else if (PMIX_CHECK_KEY(&fcd->info[k], PMIX_NOHUP)) {
            nohup = PMIX_INFO_TRUE(&fcd->info[k]);
        }
    }

    /* cycle across the apps to prep their environment and
     * launch their procs */
    for (m = 0; m < fcd->napps; m++) {
        app = (pmix_app_t *) &fcd->apps[m];
        /* merge our launch environment into the app's */
        rc = pmix_environ_merge_inplace(&app->env, environ);
        if (PMIX_SUCCESS != rc) {
            goto complete;
        }

        /* check for a fork/exec agent we should use */
        if (NULL != app->info) {
            for (k = 0; k < app->ninfo; k++) {
                if (PMIX_CHECK_KEY(&app->info[k], PMIX_FORKEXEC_AGENT)) {
                    /* we were given a fork agent - use it. We have to put its
                     * argv at the beginning of the app argv array. The value
                     * comes from the caller, so confirm it really is a string
                     * before splitting it, and that the split produced
                     * something - an empty agent leaves us nothing to exec */
                    if (PMIX_STRING != app->info[k].value.type ||
                        NULL == app->info[k].value.data.string) {
                        rc = PMIX_ERR_BAD_PARAM;
                        goto complete;
                    }
                    argv = PMIx_Argv_split(app->info[k].value.data.string, ' ');
                    if (NULL == argv || NULL == argv[0]) {
                        if (NULL != argv) {
                            PMIx_Argv_free(argv);
                            argv = NULL;
                        }
                        rc = PMIX_ERR_BAD_PARAM;
                        goto complete;
                    }
                    /* add in the argv from the app */
                    for (i = 0; NULL != argv[i]; i++) {
                        PMIx_Argv_prepend_nosize(&app->argv, argv[i]);
                    }
                    if (NULL != app->cmd) {
                        free(app->cmd);
                    }
                    app->cmd = pmix_path_findv(argv[0], X_OK, app->env, NULL);
                    if (NULL == app->cmd) {
                        pmix_show_help("help-pfexec-base.txt", "fork-agent-not-found", true,
                                       pmix_globals.hostname, argv[0]);
                        rc = PMIX_ERR_NOT_FOUND;
                        PMIx_Argv_free(argv);
                        goto complete;
                    }
                    PMIx_Argv_free(argv);
                }
            }
        }

        /* setup the path */
        if (PMIX_SUCCESS != (rc = setup_path(app))) {
            goto complete;
        }

        for (n = 0; n < app->maxprocs; n++) {
            /* create a tracker for this child */
            child = PMIX_NEW(pmix_pfexec_child_t);
            PMIX_LOAD_PROCID(&child->proc, nspace, rank);
            ++rank;
            pmix_list_append(&pmix_pfexec_globals.children, &child->super);
            child->onlist = true;
            /* we now have something to watch for; start at the floor of
             * the backoff so a short-lived child is noticed promptly */
            pfexec_poll_arm(true);

            /* setup any IOF */
            child->opts.usepty = PMIX_ENABLE_PTY_SUPPORT;
            if (PMIX_SUCCESS != (rc = setup_prefork(child))) {
                PMIX_ERROR_LOG(rc);
                child_delist(child);
                goto complete;
            }

            /* track some details about the child from our
             * perspective - note that the child may determine
             * its own nspace/rank, but that is irrelevant here
             * as we just need an ID for our own internal tracking */
            info = PMIX_NEW(pmix_rank_info_t);
            if (NULL == info) {
                rc = PMIX_ERR_NOMEM;
                child_delist(child);
                goto complete;
            }
            info->pname.nspace = strdup(child->proc.nspace);
            info->pname.rank = child->proc.rank;
            info->uid = pmix_globals.uid;
            info->gid = pmix_globals.gid;
            pmix_list_append(&nptr->ranks, &info->super);

            /* setup the environment */
            env = PMIx_Argv_copy(app->env);

            /* we only support the start of tools and servers, not apps,
             * so we don't register this nspace or child. However, we do
             * still want to pass along our connection info and active
             * modules so the forked process can connect back to us */

            /* pass the nspace */
            PMIx_Setenv("PMIX_NAMESPACE", child->proc.nspace, true, &env);
            PMIx_Setenv("PMIX_SERVER_NSPACE", child->proc.nspace, true, &env);

            /* pass the rank */
            memset(tmp, 0, 2048);
            (void) pmix_snprintf(tmp, 2047, "%u", child->proc.rank);
            PMIx_Setenv("PMIX_RANK", tmp, true, &env);
            PMIx_Setenv("PMIX_SERVER_RANK", tmp, true, &env);

            /* pass our active security modules */
            security_mode = pmix_psec_base_get_available_modules();
            PMIx_Setenv("PMIX_SECURITY_MODE", security_mode, true, &env);
            free(security_mode);
            /* pass the type of buffer we are using */
            if (PMIX_BFROP_BUFFER_FULLY_DESC == pmix_globals.mypeer->nptr->compat.type) {
                PMIx_Setenv("PMIX_BFROP_BUFFER_TYPE", "PMIX_BFROP_BUFFER_FULLY_DESC", true, &env);
            } else {
                PMIx_Setenv("PMIX_BFROP_BUFFER_TYPE", "PMIX_BFROP_BUFFER_NON_DESC", true, &env);
            }
            /* get any PTL contribution such as tmpdir settings for session files */
            if (PMIX_SUCCESS != (rc = pmix_ptl.setup_fork(&child->proc, &env))) {
                PMIX_ERROR_LOG(rc);
                child_delist(child);
                goto complete;
            }

            /* ensure we agree on our hostname */
            PMIx_Setenv("PMIX_HOSTNAME", pmix_globals.hostname, true, &env);

            /* communicate our version */
            PMIx_Setenv("PMIX_VERSION", PMIX_VERSION, true, &env);

            /* setup a keepalive pipe unless "nohup" was given */
            if (!nohup) {
                rc = pipe(child->keepalive);
                if (0 != rc) {
                    PMIX_ERROR_LOG(PMIX_ERR_SYS_OTHER);
                    child_delist(child);
                    goto complete;
                }
                pmix_snprintf(sock, 10, "%d", child->keepalive[1]);
                PMIx_Setenv("PMIX_KEEPALIVE_PIPE", sock, true, &env);
            }

            pmix_output_verbose(5, pmix_client_globals.spawn_output,
                                "%s pfexec:base spawning child %s",
                                PMIX_NAME_PRINT(&pmix_globals.myid), app->cmd);

            rc = fork_proc(app, child, env);
            PMIx_Argv_free(env);
            env = NULL;
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                child_delist(child);
                goto complete;
            }
            PMIX_IOF_READ_ACTIVATE(child->stdoutev);
            PMIX_IOF_READ_ACTIVATE(child->stderrev);
        }
    }
    rc = PMIX_SUCCESS;

complete:
    /* the copy of the app's environment is made per proc and freed once
     * the fork is done with it - the error paths in between jump here
     * with it still live */
    if (NULL != env) {
        PMIx_Argv_free(env);
    }
    /* ensure we reset our working directory back to our default location  */
    if (0 != chdir(basedir)) {
        PMIX_ERROR_LOG(PMIX_ERROR);
    }

    /* execute the callback - screened for NULL, as the other two
     * completion paths for a spawn are (_lclcbfunc() and wait_cbfunc()
     * in pmix_client_spawn.c). PMIx_Spawn_nb takes the callback straight
     * from its caller without a screen, so this was the one place a
     * spawn with none took the process down rather than simply telling
     * nobody. */
    if (NULL != fcd->spcbfunc) {
        fcd->spcbfunc(rc, nspace, fcd->cbdata);
    }
    PMIX_RELEASE(fcd);
    return;
}

/* deliver a signal to a specified pid. */
static pmix_status_t sigproc(pid_t pd, int signum)
{
    pid_t pgrp;
    pid_t pid;

    pid = pd;

#if HAVE_SETPGID
    pgrp = getpgid(pd);
    if (-1 != pgrp) {
        /* target the lead process of the process
         * group so we ensure that the signal is
         * seen by all members of that group. This
         * ensures that the signal is seen by any
         * child processes our child may have
         * started
         */
        pid = -pgrp;
    }
#endif

    if (0 != kill(pid, signum)) {
        if (ESRCH != errno) {
            pmix_output_verbose(2, pmix_client_globals.spawn_output,
                                 "%s pfexec:linux:SENT SIGNAL %d TO PID %d GOT ERRNO %d",
                                 PMIX_NAME_PRINT(&pmix_globals.myid), signum, (int) pid, errno);
            return errno;
        }
    }
    pmix_output_verbose(2, pmix_client_globals.spawn_output,
                         "%s pfexec:linux:SENT SIGNAL %d TO PID %d SUCCESS",
                         PMIX_NAME_PRINT(&pmix_globals.myid), signum, (int) pid);
    return 0;
}

/* The kill sequence (SIGCONT, pause, SIGTERM, pause, SIGKILL) must not
 * block the progress thread with sleep(); instead it is driven as a
 * timer-based state machine. Each pause is an evtimer of
 * timeout_before_sigkill seconds scheduled on evbase; the caddy carries
 * the target child across the stages, and the blocking caller stays
 * parked on scd->lock until the sequence finishes. */

static void kill_stage2(int sd, short args, void *cbdata);
static void kill_stage3(int sd, short args, void *cbdata);

static void kill_pause(pmix_pfexec_signal_caddy_t *scd, event_callback_fn next)
{
    struct timeval tv = {0, 0};

    tv.tv_sec = pmix_pfexec_globals.timeout_before_sigkill;
    pmix_event_evtimer_set(pmix_globals.evbase, &scd->ev, next, scd);
    PMIX_POST_OBJECT(scd);
    pmix_event_evtimer_add(&scd->ev, &tv);
}

static void kill_finish(pmix_pfexec_signal_caddy_t *scd)
{
    PMIX_RELEASE(scd->child);
    PMIX_WAKEUP_THREAD(scd->lock);
    PMIX_RELEASE(scd);
}

void pmix_pfexec_base_kill_proc(int sd, short args, void *cbdata)
{
    pmix_pfexec_signal_caddy_t *scd = (pmix_pfexec_signal_caddy_t *) cbdata;
    pmix_pfexec_child_t *child, *cd;
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    /* find the process */
    child = NULL;
    PMIX_LIST_FOREACH (cd, &pmix_pfexec_globals.children, pmix_pfexec_child_t) {
        if (PMIX_CHECK_PROCID(scd->proc, &cd->proc)) {
            child = cd;
            break;
        }
    }
    if (NULL == child) {
        scd->lock->status = PMIX_SUCCESS;
        PMIX_WAKEUP_THREAD(scd->lock);
        PMIX_RELEASE(scd);
        return;
    }

    /* Take the child off the list so the waitpid callback won't find it -
     * that induces unmanageable race conditions when we are deliberately
     * killing the process - but take a reference of our own first. The
     * caddy holds this child across two evtimers, and a completion posted
     * before we got here is still queued behind us with a reference of
     * its own; whichever of the two runs last is the one that frees it. */
    PMIX_RETAIN(child);
    scd->child = child;
    child_delist(child);

    /* First send a SIGCONT in case the process is in stopped state.
       If it is in a stopped state and we do not first change it to
       running, then SIGTERM will not get delivered.  Ignore return
       value. */
    pmix_output_verbose(5, pmix_client_globals.spawn_output, "%s SENDING SIGCONT",
                         PMIX_NAME_PRINT(&pmix_globals.myid));
    /* through the caddy, which is the reference that keeps this alive
     * now that the list's is gone */
    sigproc(scd->child->pid, SIGCONT);

    /* wait a little to give the proc a chance to wakeup, then continue
     * to the SIGTERM stage */
    kill_pause(scd, kill_stage2);
}

static void kill_stage2(int sd, short args, void *cbdata)
{
    pmix_pfexec_signal_caddy_t *scd = (pmix_pfexec_signal_caddy_t *) cbdata;
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    /* issue a SIGTERM */
    pmix_output_verbose(5, pmix_client_globals.spawn_output, "%s SENDING SIGTERM",
                         PMIX_NAME_PRINT(&pmix_globals.myid));
    scd->lock->status = sigproc(scd->child->pid, SIGTERM);

    /* Wait a little to give the proc a chance to exit cleanly, then
     * escalate to SIGKILL. The escalation must NOT be conditional on
     * the SIGTERM having failed to be delivered: the case the sequence
     * exists for is a child that RECEIVES SIGTERM and ignores it, and
     * such a child was previously left running forever - and, having
     * already been taken off the children list here, never reaped
     * either. A proc that did exit on the SIGTERM simply makes the
     * SIGKILL a no-op, since sigproc tolerates ESRCH. */
    kill_pause(scd, kill_stage3);
}

static void kill_stage3(int sd, short args, void *cbdata)
{
    pmix_pfexec_signal_caddy_t *scd = (pmix_pfexec_signal_caddy_t *) cbdata;
    pmix_status_t rc;
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    /* issue a SIGKILL */
    pmix_output_verbose(5, pmix_client_globals.spawn_output, "%s SENDING SIGKILL",
                         PMIX_NAME_PRINT(&pmix_globals.myid));
    rc = sigproc(scd->child->pid, SIGKILL);

    /* a SIGKILL that lands rescues a SIGTERM that did not, so only
     * report a failure when neither signal could be delivered */
    if (0 != scd->lock->status) {
        scd->lock->status = rc;
    }

    kill_finish(scd);
}

void pmix_pfexec_base_signal_proc(int sd, short args, void *cbdata)
{
    pmix_pfexec_signal_caddy_t *scd = (pmix_pfexec_signal_caddy_t *) cbdata;
    pmix_pfexec_child_t *child, *cd;
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    /* find the process */
    child = NULL;
    PMIX_LIST_FOREACH (cd, &pmix_pfexec_globals.children, pmix_pfexec_child_t) {
        if (PMIX_CHECK_PROCID(scd->proc, &cd->proc)) {
            child = cd;
            break;
        }
    }
    if (NULL == child) {
        scd->lock->status = PMIX_SUCCESS;
        PMIX_WAKEUP_THREAD(scd->lock);
        PMIX_RELEASE(scd);
        return;
    }

    pmix_output_verbose(5, pmix_client_globals.spawn_output, "%s SIGNALING %d",
                         PMIX_NAME_PRINT(&pmix_globals.myid), scd->signal);
    scd->lock->status = sigproc(child->pid, scd->signal);

    PMIX_WAKEUP_THREAD(scd->lock);
    PMIX_RELEASE(scd);
}

static pmix_status_t setup_prefork(pmix_pfexec_child_t *child)
{
    int ret = -1;
    pmix_pfexec_base_io_conf_t *opts = &child->opts;

    fflush(stdout);

    /* first check to make sure we can do ptys */
#if PMIX_ENABLE_PTY_SUPPORT
    if (opts->usepty) {
        ret = pmix_openpty(&(opts->p_stdout[0]), &(opts->p_stdout[1]), (char *) NULL,
                           (struct termios *) NULL, (struct winsize *) NULL);
    }
#else
    opts->usepty = 0;
#endif

    if (ret < 0) {
        opts->usepty = 0;
        if (pipe(opts->p_stdout) < 0) {
            PMIX_ERROR_LOG(PMIX_ERR_SYS_OTHER);
            return PMIX_ERR_SYS_OTHER;
        }
    }
    /* always leave stdin available in case we forward to it */
    if (pipe(opts->p_stdin) < 0) {
        PMIX_ERROR_LOG(PMIX_ERR_SYS_OTHER);
        /* release the stdout pipe/pty we already opened */
        close(opts->p_stdout[0]);
        close(opts->p_stdout[1]);
        opts->p_stdout[0] = -1;
        opts->p_stdout[1] = -1;
        return PMIX_ERR_SYS_OTHER;
    }

    if (pipe(opts->p_stderr) < 0) {
        PMIX_ERROR_LOG(PMIX_ERR_SYS_OTHER);
        /* release the stdout and stdin pipes we already opened */
        close(opts->p_stdout[0]);
        close(opts->p_stdout[1]);
        close(opts->p_stdin[0]);
        close(opts->p_stdin[1]);
        opts->p_stdout[0] = -1;
        opts->p_stdout[1] = -1;
        opts->p_stdin[0] = -1;
        opts->p_stdin[1] = -1;
        return PMIX_ERR_SYS_OTHER;
    }

    /* connect read/write ends to IOF */
    PMIX_IOF_SINK_DEFINE(&child->stdinsink, &child->proc, opts->p_stdin[1],
                         PMIX_FWD_STDIN_CHANNEL, pmix_iof_write_handler);

    PMIX_IOF_READ_EVENT_LOCAL(&child->stdoutev, opts->p_stdout[0],
                              pmix_iof_read_local_handler, false);
    PMIX_LOAD_PROCID(&child->stdoutev->name, child->proc.nspace, child->proc.rank);
    child->stdoutev->childproc = (void *) child;
    child->stdoutev->channel = PMIX_FWD_STDOUT_CHANNEL;
    PMIX_IOF_READ_EVENT_LOCAL(&child->stderrev, opts->p_stderr[0],
                              pmix_iof_read_local_handler, false);
    PMIX_LOAD_PROCID(&child->stderrev->name, child->proc.nspace, child->proc.rank);
    child->stderrev->childproc = (void *) child;
    child->stderrev->channel = PMIX_FWD_STDERR_CHANNEL;

    return PMIX_SUCCESS;
}

pmix_status_t pmix_pfexec_base_setup_child(pmix_pfexec_child_t *child)
{
    int ret;
    pmix_pfexec_base_io_conf_t *opts = &child->opts;

    if (0 <= opts->p_stdin[1]) {
        close(opts->p_stdin[1]);
        opts->p_stdin[1] = -1;
    }
    if (0 <= opts->p_stdout[0]) {
        close(opts->p_stdout[0]);
        opts->p_stdout[0] = -1;
    }
    if (0 <= opts->p_stderr[0]) {
        close(opts->p_stderr[0]);
        opts->p_stderr[0] = -1;
    }

    if (opts->usepty) {
        /* disable echo */
        struct termios term_attrs;
        if (tcgetattr(opts->p_stdout[1], &term_attrs) < 0) {
            return PMIX_ERR_SYS_OTHER;
        }
        term_attrs.c_lflag &= ~(ECHO | ECHOE | ECHOK | ECHOCTL | ECHOKE | ECHONL);
        term_attrs.c_iflag &= ~(ICRNL | INLCR | ISTRIP | INPCK | IXON);
        term_attrs.c_oflag &= ~(OCRNL | ONLCR);
        if (tcsetattr(opts->p_stdout[1], TCSANOW, &term_attrs) == -1) {
            return PMIX_ERR_SYS_OTHER;
        }
        ret = dup2(opts->p_stdout[1], fileno(stdout));
        if (ret < 0) {
            return PMIX_ERR_SYS_OTHER;
        }
        if (0 <= opts->p_stdout[1]) {
            close(opts->p_stdout[1]);
            opts->p_stdout[1] = -1;
        }
    } else {
        if (opts->p_stdout[1] != fileno(stdout)) {
            ret = dup2(opts->p_stdout[1], fileno(stdout));
            if (ret < 0) {
                return PMIX_ERR_SYS_OTHER;
            }
            if (0 <= opts->p_stdout[1]) {
                close(opts->p_stdout[1]);
                opts->p_stdout[1] = -1;
            }
        }
    }
    if (opts->p_stdin[0] != fileno(stdin)) {
        ret = dup2(opts->p_stdin[0], fileno(stdin));
        if (ret < 0) {
            return PMIX_ERR_SYS_OTHER;
        }
        if (0 <= opts->p_stdin[0]) {
            close(opts->p_stdin[0]);
            opts->p_stdin[0] = -1;
        }
    }

    if (opts->p_stderr[1] != fileno(stderr)) {
        ret = dup2(opts->p_stderr[1], fileno(stderr));
        if (ret < 0) {
            return PMIX_ERR_SYS_OTHER;
        }
        if (0 <= opts->p_stderr[1]) {
            close(opts->p_stderr[1]);
            opts->p_stderr[1] = -1;
        }
    }

    return PMIX_SUCCESS;
}

static pmix_status_t register_nspace(char *nspace, pmix_setup_caddy_t *fcd)
{
    pmix_status_t rc;
    size_t n, ninfo;
    int m;
    uint32_t nprocs, u32;
    uint16_t u16;
    pmix_proc_t proc;
    pmix_rank_t zero = 0, rk;
    pmix_info_t *info = NULL;
    pmix_namespace_t *nptr, *tmp;
    void *jinfo, *tmpinfo, *pinfo;
    pmix_data_array_t darray;
    char *str;

    /* quick compute the number of procs to be started */
    nprocs = 0;
    for (n = 0; n < fcd->napps; n++) {
        nprocs += fcd->apps[n].maxprocs;
    }
    if (0 == nprocs) {
        return PMIX_ERR_BAD_PARAM;
    }

    /* see if we already have this nspace */
    nptr = NULL;
    PMIX_LIST_FOREACH (tmp, &pmix_globals.nspaces, pmix_namespace_t) {
        if (0 == strcmp(tmp->nspace, nspace)) {
            nptr = tmp;
            break;
        }
    }
    if (NULL == nptr) {
        nptr = PMIX_NEW(pmix_namespace_t);
        if (NULL == nptr) {
            return PMIX_ERR_NOMEM;
        }
        nptr->nspace = strdup(nspace);
        pmix_list_append(&pmix_globals.nspaces, &nptr->super);
    }
    nptr->nlocalprocs = nprocs;

    /* start assembling the list */
    PMIX_INFO_LIST_START(jinfo);

    /* jobid */
    PMIX_LOAD_PROCID(&proc, nspace, PMIX_RANK_UNDEF);
    PMIX_INFO_LIST_ADD(rc, jinfo, PMIX_JOBID, &proc, PMIX_PROC);
    if (PMIX_SUCCESS != rc) {
        PMIX_INFO_LIST_RELEASE(jinfo);
        return rc;
    }

    /* hostname */
    PMIX_INFO_LIST_ADD(rc, jinfo, PMIX_HOSTNAME, pmix_globals.hostname, PMIX_STRING);
    if (PMIX_SUCCESS != rc) {
        PMIX_INFO_LIST_RELEASE(jinfo);
        return rc;
    }

    /* mark us as the parent */
    PMIX_INFO_LIST_ADD(rc, jinfo, PMIX_PARENT_ID, &pmix_globals.myid, PMIX_PROC);
    if (PMIX_SUCCESS != rc) {
        PMIX_INFO_LIST_RELEASE(jinfo);
        return rc;
    }

    /* node size */
    PMIX_INFO_LIST_ADD(rc, jinfo, PMIX_NODE_SIZE, &nprocs, PMIX_UINT32);
    if (PMIX_SUCCESS != rc) {
        PMIX_INFO_LIST_RELEASE(jinfo);
        return rc;
    }

    /* local size */
    PMIX_INFO_LIST_ADD(rc, jinfo, PMIX_LOCAL_SIZE, &nprocs, PMIX_UINT32);
    if (PMIX_SUCCESS != rc) {
        PMIX_INFO_LIST_RELEASE(jinfo);
        return rc;
    }

    /* local leader */
    PMIX_INFO_LIST_ADD(rc, jinfo, PMIX_LOCALLDR, &zero, PMIX_PROC_RANK);
    if (PMIX_SUCCESS != rc) {
        PMIX_INFO_LIST_RELEASE(jinfo);
        return rc;
    }

    /* add in any info provided by the caller */
    for (n = 0; n < fcd->ninfo; n++) {
        if (PMIX_ENVAR == fcd->info[n].value.type) {
            continue; // take care of these elsewhere
        }
        PMIX_INFO_LIST_XFER(rc, jinfo, &fcd->info[n]);
    }

    /* for each app in the job, create an app-array */
    proc.rank = 0;
    rk = 0;
    for (n = 0; n < fcd->napps; n++) {
        PMIX_INFO_LIST_START(tmpinfo);
        u32 = n;
        PMIX_INFO_LIST_ADD(rc, tmpinfo, PMIX_APPNUM, &u32, PMIX_UINT32);
        u32 = fcd->apps[n].maxprocs;
        PMIX_INFO_LIST_ADD(rc, tmpinfo, PMIX_APP_SIZE, &u32, PMIX_UINT32);
        PMIX_INFO_LIST_ADD(rc, tmpinfo, PMIX_APPLDR, &proc.rank, PMIX_PROC_RANK);
        proc.rank += fcd->apps[n].maxprocs;
        if (NULL != fcd->apps[n].cwd) {
            PMIX_INFO_LIST_ADD(rc, tmpinfo, PMIX_WDIR, fcd->apps[n].cwd, PMIX_STRING);
        }
        str = PMIx_Argv_join(fcd->apps[n].argv, ' ');
        PMIX_INFO_LIST_ADD(rc, tmpinfo, PMIX_APP_ARGV, str, PMIX_STRING);
        /* the list took its own copy */
        if (NULL != str) {
            free(str);
        }
        /* convert the list into an array */
        PMIX_INFO_LIST_CONVERT(rc, tmpinfo, &darray);
        /* release the list */
        PMIX_INFO_LIST_RELEASE(tmpinfo);
        /* add it to the job info array */
        PMIX_INFO_LIST_ADD(rc, jinfo, PMIX_APP_INFO_ARRAY, &darray, PMIX_DATA_ARRAY);
        /* release the memory - the array was copied */
        PMIX_DATA_ARRAY_DESTRUCT(&darray);
        /* for each proc in this app, create a proc-array */
        for (m = 0; m < fcd->apps[n].maxprocs; m++) {
            PMIX_INFO_LIST_START(pinfo);
            /* must start with the rank */
            PMIX_INFO_LIST_ADD(rc, pinfo, PMIX_RANK, &rk, PMIX_PROC_RANK);
            /* app number for this proc */
            u32 = n;
            PMIX_INFO_LIST_ADD(rc, pinfo, PMIX_APPNUM, &u32, PMIX_UINT32);
            /* local rank is the same as rank since we only fork locally */
            u16 = rk;
            PMIX_INFO_LIST_ADD(rc, pinfo, PMIX_LOCAL_RANK, &u16, PMIX_UINT16);
            ++rk;
            /* convert the list into an array */
            PMIX_INFO_LIST_CONVERT(rc, pinfo, &darray);
            /* release the list */
            PMIX_INFO_LIST_RELEASE(pinfo);
            /* add it to the job info array */
            PMIX_INFO_LIST_ADD(rc, jinfo, PMIX_PROC_INFO_ARRAY, &darray, PMIX_DATA_ARRAY);
            /* release the memory - the array was copied */
            PMIX_DATA_ARRAY_DESTRUCT(&darray);
        }
    }

    /* convert the full list of job info into an array */
    PMIX_INFO_LIST_CONVERT(rc, jinfo, &darray);
    PMIX_INFO_LIST_RELEASE(jinfo);

    info = (pmix_info_t *) darray.array;
    ninfo = darray.size;

    /* register nspace for each activate components */
    PMIX_GDS_ADD_NSPACE(rc, nptr->nspace, nprocs, info, ninfo);
    if (PMIX_SUCCESS == rc) {
        /* store this data in our own GDS module - we will retrieve
         * it later so it can be passed down to the launched procs
         * once they connect to us and we know what GDS module they
         * are using */
        PMIX_GDS_CACHE_JOB_INFO(rc, pmix_globals.mypeer, nptr, info, ninfo);
    }

    PMIX_DATA_ARRAY_DESTRUCT(&darray);
    return rc;
}

static void set_handler_linux(int sig)
{
    struct sigaction act;

    act.sa_handler = SIG_DFL;
    act.sa_flags = 0;
    sigemptyset(&act.sa_mask);

    sigaction(sig, &act, (struct sigaction *) 0);
}

/* Called from the child, in the async-signal-safe window between fork()
 * and execve(), to report a failure to the waiting parent and exit.
 *
 * It writes only a fixed-size record - no strings, no allocation, no
 * show_help - because formatting a message in the child (malloc, stdio,
 * reading the help file) can deadlock: fork() leaves any lock another
 * thread held frozen in the child. The parent renders the diagnostic
 * from `which` and `errnum`. We use _exit() rather than exit() so no
 * atexit handlers run and no stdio buffers are flushed in the child. */
static void child_fail(int fd, int exit_status, pmix_pfexec_child_err_t which, int errnum)
{
    pmix_pfexec_pipe_err_msg_t msg;

    msg.fatal = true;
    msg.exit_status = exit_status;
    msg.which = (int) which;
    msg.errnum = errnum;

    /* best effort - if the write fails there is nothing safe we can do
     * here, and the parent will still see the pipe close */
    (void) pmix_fd_write(fd, sizeof(msg), &msg);

    _exit(exit_status);
}

static void do_child(pmix_app_t *app, char **env, pmix_pfexec_child_t *child, int write_fd)
{
    int errval;
    sigset_t sigs;
    long fd, fdmax = sysconf(_SC_OPEN_MAX);

#if HAVE_SETPGID
    /* Set a new process group for this child, so that any
     * signals we send to it will reach any children it spawns */
    setpgid(0, 0);
#endif

    /* Setup the pipe to be close-on-exec */
    pmix_fd_set_cloexec(write_fd);

    /* setup stdout/stderr so that any error messages that we
       may print out will get displayed back at pmixrun.

       NOTE: Definitely do this AFTER we check contexts so
       that any error message from those two functions doesn't
       come out to the user. IF we didn't do it in this order,
       THEN a user who gives us a bad executable name or
       working directory would get N error messages, where
       N=num_procs. This would be very annoying for large
       jobs, so instead we set things up so that pmixrun
       always outputs a nice, single message indicating what
       happened
    */
    if (PMIX_SUCCESS != pmix_pfexec_base_setup_child(child)) {
        child_fail(write_fd, 1, PMIX_PFEXEC_CHILD_ERR_SETUP, errno);
        /* Does not return */
    }

    /* Close all open file descriptors except stdin/stdout/stderr, the
       pipe up to the parent, and the keepalive pipe. We use a plain
       close() loop rather than scanning /proc/self/fd with
       opendir/readdir: those allocate, and we are between fork() and
       execve() where only async-signal-safe calls are permitted. */
    for (fd = 3; fd < fdmax; fd++) {
        if (fd != write_fd && fd != child->keepalive[1]) {
            close(fd);
        }
    }

    /* Set signal handlers back to the default.  Do this close to
       the exev() because the event library may (and likely will)
       reset them.  If we don't do this, the event library may
       have left some set that, at least on some OS's, don't get
       reset via fork() or exec().  Hence, the launched process
       could be unkillable (for example). */

    set_handler_linux(SIGTERM);
    set_handler_linux(SIGINT);
    set_handler_linux(SIGHUP);
    set_handler_linux(SIGPIPE);
    set_handler_linux(SIGCHLD);

    /* Unblock all signals, for many of the same reasons that we
       set the default handlers, above.  This is noticeable on
       Linux where the event library blocks SIGTERM, but we don't
       want that blocked by the launched process. */
    sigprocmask(0, 0, &sigs);
    sigprocmask(SIG_UNBLOCK, &sigs, 0);

    /* take us to the correct wdir */
    if (NULL != app->cwd) {
        if (0 != chdir(app->cwd)) {
            child_fail(write_fd, 1, PMIX_PFEXEC_CHILD_ERR_WDIR, errno);
            /* Does not return */
        }
    }

    /* Exec the new executable */
    execve(app->cmd, app->argv, env);
    errval = errno;
    child_fail(write_fd, 1, PMIX_PFEXEC_CHILD_ERR_EXEC, errval);
    /* Does not return */
}

static pmix_status_t do_parent(pmix_app_t *app, pmix_pfexec_child_t *child, int read_fd)
{
    pmix_status_t rc;
    pmix_pfexec_pipe_err_msg_t msg;

    /* close the child-side ends of the stdio pipes - the parent keeps
     * only the read ends of stdout/stderr and the write end of stdin
     * (wired to the IOF sink/read events). These are marked closed so
     * the child destructor does not close them a second time. */
    if (0 <= child->opts.p_stdin[0]) {
        close(child->opts.p_stdin[0]);
        child->opts.p_stdin[0] = -1;
    }
    if (0 <= child->opts.p_stdout[1]) {
        close(child->opts.p_stdout[1]);
        child->opts.p_stdout[1] = -1;
    }
    if (0 <= child->opts.p_stderr[1]) {
        close(child->opts.p_stderr[1]);
        child->opts.p_stderr[1] = -1;
    }
    if (0 <= child->keepalive[1]) {
        close(child->keepalive[1]);
        /* mark it closed - the child destructor closes this end too, and
         * a double close in a multithreaded process can take down an
         * unrelated descriptor that was opened in between */
        child->keepalive[1] = -1;
    }

    /* Read the child's failure record. A pipe that closes with no data
       means the child reached execve() successfully. */
    rc = pmix_fd_read(read_fd, sizeof(msg), &msg);
    close(read_fd);
    if (PMIX_ERR_TIMEOUT == rc) {
        return PMIX_SUCCESS;
    }
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }

    /* The child restricted itself to async-signal-safe calls before
       exec, so it sent a failure code plus errno rather than a formatted
       message. Render the human-readable diagnostic here in the parent,
       where allocation and show_help are safe. */
    switch (msg.which) {
    case PMIX_PFEXEC_CHILD_ERR_SETUP:
        pmix_show_help("help-pfexec-base.txt", "child-setup-failed", true,
                       pmix_globals.hostname, app->cmd, strerror(msg.errnum));
        break;
    case PMIX_PFEXEC_CHILD_ERR_WDIR:
        pmix_show_help("help-pfexec-base.txt", "wdir-not-found", true,
                       pmix_globals.hostname,
                       (NULL != app->cwd) ? app->cwd : "<none>",
                       strerror(msg.errnum));
        break;
    case PMIX_PFEXEC_CHILD_ERR_EXEC:
    default: {
        char cwd[MAXPATHLEN];
        const char *wdir;
        /* the child's working dir is the app's cwd if one was given,
           otherwise ours (it never chdir'd) */
        if (NULL != app->cwd) {
            wdir = app->cwd;
        } else if (NULL != getcwd(cwd, sizeof(cwd))) {
            wdir = cwd;
        } else {
            wdir = "<unknown>";
        }
        pmix_show_help("help-pfexec-base.txt", "execve error", true,
                       pmix_globals.hostname, wdir, app->cmd, strerror(msg.errnum));
        break;
    }
    }

    return PMIX_ERR_SYS_OTHER;
}

/**
 *  Fork/exec the specified processes
 */
static int fork_proc(pmix_app_t *app, pmix_pfexec_child_t *child, char **env)
{
    int p[2];

    /* A pipe is used to communicate between the parent and child to
       indicate whether the exec ultimately succeeded or failed.  The
       child sets the pipe to be close-on-exec; the child only ever
       writes anything to the pipe if there is an error (e.g.,
       executable not found, exec() fails, etc.).  The parent does a
       blocking read on the pipe; if the pipe closed with no data,
       then the exec() succeeded.  If the parent reads something from
       the pipe, then the child was letting us know why it failed. */
    if (pipe(p) < 0) {
        PMIX_ERROR_LOG(PMIX_ERR_SYS_OTHER);
        return PMIX_ERR_SYS_OTHER;
    }

    /* Fork off the child */
    child->pid = fork();

    if (child->pid < 0) {
        PMIX_ERROR_LOG(PMIX_ERR_SYS_OTHER);
        close(p[0]);
        close(p[1]);
        return PMIX_ERR_SYS_OTHER;
    }

    if (child->pid == 0) {
        if (0 <= p[0]) {
            close(p[0]);
        }
        if (0 <= child->keepalive[0]) {
            close(child->keepalive[0]);
            child->keepalive[0] = -1;
        }
        do_child(app, env, child, p[1]);
        /* Does not return */
    }

    close(p[1]);
    return do_parent(app, child, p[0]);
}

/* Ask about each of our own children, and only our own.
 *
 * Runs on evbase, which is also where the children list lives, so unlike
 * the SIGCHLD handler this replaced there is nothing to thread-shift.
 * waitpid(pid, WNOHANG) gives a complete three-way answer:
 *
 *   0       still running
 *   pid     exited, and the status is ours
 *   ECHILD  gone, but somebody else reaped it first - a host sweeping
 *           with waitpid(-1), or one that set SIGCHLD to SIG_IGN and let
 *           the kernel do it. The child is definitively dead; only its
 *           exit status is lost, and we say so rather than reporting a
 *           zero we did not observe.
 *
 * That last case is why per-pid matters even though it cannot stop a
 * host from taking our children: we always learn of the death. The old
 * waitpid(-1) sweep could not even do that - whichever of us and the
 * host lost the race got no pid, no status and no notification at all.
 */
static void child_poll(int sd, short args, void *cbdata)
{
    pmix_pfexec_child_t *child, *next;
    pid_t r;
    int status;
    bool watching = false;
    PMIX_HIDE_UNUSED_PARAMS(sd, args, cbdata);

    /* the one-shot timer that brought us here (if any) has fired */
    pmix_pfexec_globals.poll_active = false;

    PMIX_LIST_FOREACH_SAFE (child, next, &pmix_pfexec_globals.children, pmix_pfexec_child_t) {
        if (child->completed || 0 >= child->pid) {
            continue;
        }
        status = 0;
        r = waitpid(child->pid, &status, WNOHANG);
        if (0 == r) {
            /* still running - keep the timer going for it */
            watching = true;
            continue;
        }
        if (0 > r) {
            if (EINTR == errno) {
                watching = true;
                continue;
            }
            if (ECHILD != errno) {
                /* nothing we can do with this, but do not spin on it */
                watching = true;
                continue;
            }
            /* reaped by somebody else: dead, status unknown */
            pmix_output_verbose(5, pmix_client_globals.spawn_output,
                                "%s pfexec child %s (pid %lu) was reaped elsewhere - "
                                "exit status unavailable",
                                PMIX_NAME_PRINT(&pmix_globals.myid),
                                PMIX_NAME_PRINT(&child->proc), (unsigned long) child->pid);
            child->exitcode = 0;
        } else if (WIFEXITED(status)) {
            child->exitcode = WEXITSTATUS(status);
        } else if (WIFSIGNALED(status)) {
            child->exitcode = WTERMSIG(status) + 128;
        }

        child->completed = true;
        if ((NULL == child->stdoutev || !child->stdoutev->active)
            && (NULL == child->stderrev || !child->stderrev->active)) {
            PMIX_PFEXEC_CHK_COMPLETE(child);
        }
    }

    if (watching) {
        pfexec_poll_arm(false);
    } else {
        /* nothing left to watch: no timer, and the next spawn starts
         * the backoff over from the floor */
        pmix_pfexec_globals.poll_delay = 0;
    }
}

/* Arm the exit-status poll, backing the delay off unless "reset" says to
 * start from the floor again. Noticing a child has exited carries no
 * latency requirement -- and the IOF EOF path kicks us through
 * pmix_pfexec_reap_check() at the moment a child that forwards its
 * output usually dies -- so the ceiling is what governs the cost of a
 * long-running child, and it is one wakeup per second by default. */
static void pfexec_poll_disarm(void)
{
    if (pmix_pfexec_globals.poll_active) {
        pmix_event_del(pmix_pfexec_globals.poll_ev);
        pmix_pfexec_globals.poll_active = false;
    }
}

static void pfexec_poll_arm(bool reset)
{
    struct timeval tv;
    int delay;

    if (NULL == pmix_pfexec_globals.poll_ev) {
        return;
    }
    /* pmix_event_evtimer_set is event_assign, which must never be applied
     * to an event that is still pending - so always take it down first
     * rather than trusting poll_active to say it is not armed */
    if (pmix_pfexec_globals.poll_active && !reset) {
        return;
    }
    pfexec_poll_disarm();
    if (reset) {
        pmix_pfexec_globals.poll_delay = 0;
    }

    /* a zero floor would give a zero timeout and spin the event loop,
     * which is what an unregistered parameter would otherwise produce */
    if (0 >= pmix_pfexec_globals.poll_interval) {
        pmix_pfexec_globals.poll_interval = PMIX_PFEXEC_POLL_INTERVAL;
    }
    if (pmix_pfexec_globals.poll_max_interval < pmix_pfexec_globals.poll_interval) {
        pmix_pfexec_globals.poll_max_interval = pmix_pfexec_globals.poll_interval;
    }

    delay = pmix_pfexec_globals.poll_delay;
    if (0 == delay) {
        delay = pmix_pfexec_globals.poll_interval;
    } else if (delay < pmix_pfexec_globals.poll_max_interval) {
        delay *= 2;
        if (delay > pmix_pfexec_globals.poll_max_interval) {
            delay = pmix_pfexec_globals.poll_max_interval;
        }
    }
    pmix_pfexec_globals.poll_delay = delay;

    tv.tv_sec = delay / 1000;
    tv.tv_usec = (delay % 1000) * 1000;
    pmix_event_evtimer_set(pmix_globals.evbase, pmix_pfexec_globals.poll_ev,
                           child_poll, NULL);
    if (0 == pmix_event_evtimer_add(pmix_pfexec_globals.poll_ev, &tv)) {
        pmix_pfexec_globals.poll_active = true;
    }
}

void pmix_pfexec_reap_check(void)
{
    if (!pmix_pfexec_globals.initialized || NULL == pmix_pfexec_globals.poll_ev) {
        return;
    }
    /* We are on evbase here (the IOF read handler), so just look now -
     * but take the pending timer down first: child_poll re-arms, and
     * arming assigns the same event. */
    pfexec_poll_disarm();
    child_poll(-1, 0, NULL);
}

/**** FRAMEWORK CLASS INSTANTIATIONS ****/

static void chcon(pmix_pfexec_child_t *p)
{
    memset(&p->ev, 0, sizeof(pmix_event_t));
    PMIX_LOAD_PROCID(&p->proc, NULL, PMIX_RANK_UNDEF);
    p->pid = 0;
    p->completed = false;
    p->onlist = false;
    /* objects are malloc'd, not calloc'd - a child that completes
     * through a path that never records an exit status must report
     * zero, not whatever was on the heap */
    p->exitcode = 0;
    p->keepalive[0] = -1;
    p->keepalive[1] = -1;
    memset(&p->opts, 0, sizeof(pmix_pfexec_base_io_conf_t));
    p->opts.p_stdin[0] = -1;
    p->opts.p_stdin[1] = -1;
    p->opts.p_stdout[0] = -1;
    p->opts.p_stdout[1] = -1;
    p->opts.p_stderr[0] = -1;
    p->opts.p_stderr[1] = -1;
    PMIX_CONSTRUCT(&p->stdinsink, pmix_iof_sink_t);
    p->stdoutev = NULL;
    p->stderrev = NULL;
}
static void chdes(pmix_pfexec_child_t *p)
{
    /* the IOF sink/read events own and close the parent-side ends of the
     * stdio pipes (p_stdin[1], p_stdout[0], p_stderr[0]) */
    PMIX_DESTRUCT(&p->stdinsink);
    if (NULL != p->stdoutev) {
        PMIX_RELEASE(p->stdoutev);
    }
    if (NULL != p->stderrev) {
        PMIX_RELEASE(p->stderrev);
    }
    /* close any child-side pipe ends still open - normally do_parent
     * closes these, but on an error path between setup_prefork and the
     * fork they can still be open here */
    if (0 <= p->opts.p_stdin[0]) {
        close(p->opts.p_stdin[0]);
    }
    if (0 <= p->opts.p_stdout[1]) {
        close(p->opts.p_stdout[1]);
    }
    if (0 <= p->opts.p_stderr[1]) {
        close(p->opts.p_stderr[1]);
    }
    if (0 <= p->keepalive[0]) {
        close(p->keepalive[0]);
    }
    if (0 <= p->keepalive[1]) {
        close(p->keepalive[1]);
    }
}
PMIX_CLASS_INSTANCE(pmix_pfexec_child_t,
                    pmix_list_item_t,
                    chcon, chdes);

static void scdcon(pmix_pfexec_signal_caddy_t *p)
{
    p->proc = NULL;
    p->signal = 0;
    p->lock = NULL;
    p->child = NULL;
}
PMIX_CLASS_INSTANCE(pmix_pfexec_signal_caddy_t,
                    pmix_object_t,
                    scdcon, NULL);

static void cmplcon(pmix_pfexec_cmpl_caddy_t *p)
{
    p->child = NULL;
    PMIX_INFO_CONSTRUCT(&p->info[0]);
    PMIX_INFO_CONSTRUCT(&p->info[1]);
}
static void cmpldes(pmix_pfexec_cmpl_caddy_t *p)
{
    PMIX_INFO_DESTRUCT(&p->info[0]);
    PMIX_INFO_DESTRUCT(&p->info[1]);
}
PMIX_CLASS_INSTANCE(pmix_pfexec_cmpl_caddy_t,
                    pmix_object_t,
                    cmplcon, cmpldes);
