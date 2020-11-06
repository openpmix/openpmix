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
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */


#include "pmix_config.h"

#include <stdlib.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <errno.h>
#include <sys/types.h>
#ifdef HAVE_SYS_WAIT_H
#include <sys/wait.h>
#endif
#include <signal.h>
#ifdef HAVE_UTIL_H
#include <util.h>
#endif
#ifdef HAVE_PTY_H
#include <pty.h>
#endif
#ifdef HAVE_FCNTL_H
#include <fcntl.h>
#endif
#ifdef HAVE_TERMIOS_H
#include <termios.h>
# ifdef HAVE_TERMIO_H
#  include <termio.h>
# endif
#endif
#ifdef HAVE_LIBUTIL_H
#include <libutil.h>
#endif

#include "include/pmix.h"
#include "include/pmix_server.h"
#include "include/pmix_common.h"

#include "src/include/pmix_stdint.h"
#include "src/include/pmix_globals.h"
#include "src/threads/threads.h"
#include "src/util/argv.h"
#include "src/util/context_fns.h"
#include "src/util/error.h"
#include "src/util/name_fns.h"
#include "src/util/os_dirpath.h"
#include "src/util/os_path.h"
#include "src/util/path.h"
#include "src/util/pmix_environ.h"
#include "src/util/pmix_pty.h"
#include "src/util/printf.h"
#include "src/util/show_help.h"
#include "src/mca/ptl/base/base.h"

#include "src/client/pmix_client_ops.h"
#include "src/server/pmix_server_ops.h"
#include "src/mca/pfexec/base/base.h"

static pmix_status_t setup_prefork(pmix_pfexec_child_t *child);


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
        if (PMIX_SUCCESS != (rc = pmix_util_check_context_cwd(app))) {
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
        pmix_setenv("PWD", dir, true, &app->env);
    }

    /* ensure the app is pointing to a full path */
    rc = pmix_util_check_context_app(app, app->env);

    return rc;
}


void pmix_pfexec_base_spawn_proc(int sd, short args, void *cbdata)
{
    (void)sd;
    (void)args;
    pmix_pfexec_fork_caddy_t *fcd = (pmix_pfexec_fork_caddy_t*)cbdata;
    pmix_app_t *app;
    int i, n;
    size_t m, k;
    pmix_status_t rc;
    char **argv = NULL, **env = NULL;
    pmix_nspace_t nspace;
    char basedir[MAXPATHLEN];
    pmix_pfexec_child_t *child;
    pmix_rank_info_t *info;
    pmix_namespace_t *nptr;
    pmix_rank_t rank=0;
    char tmp[2048], *ptr;

    pmix_output_verbose(5, pmix_pfexec_base_framework.framework_output,
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
    (void)snprintf(tmp, 2047, "%s:%lu", pmix_globals.myid.nspace, (unsigned long)pmix_pfexec_globals.nextid);
    PMIX_LOAD_NSPACE(nspace, tmp);
    ++pmix_pfexec_globals.nextid;

    /* add the nspace to the server global list */
    nptr = PMIX_NEW(pmix_namespace_t);
    nptr->nspace = strdup(nspace);
    pmix_list_append(&pmix_globals.nspaces, &nptr->super);

    for (m=0; m < fcd->napps; m++) {
        app = (pmix_app_t*)&fcd->apps[m];

        /* merge our launch environment into the app's */
        for (i=0; NULL != environ[i]; i++) {
            ptr = strchr(environ[i], '=');
            *ptr = '\0';
            ++ptr;
            pmix_setenv(environ[i], ptr, false, &app->env);  // do not overwrite a given value
            --ptr;
            *ptr = '=';
        }

        /* process any job-info */
        if (NULL != fcd->jobinfo) {
            for (k=0; k < fcd->njinfo; k++) {
                if (PMIX_CHECK_KEY(&fcd->jobinfo[k], PMIX_SET_ENVAR)) {

                } else if (PMIX_CHECK_KEY(&fcd->jobinfo[k], PMIX_ADD_ENVAR)) {

                } else if (PMIX_CHECK_KEY(&fcd->jobinfo[k], PMIX_UNSET_ENVAR)) {
                } else if (PMIX_CHECK_KEY(&fcd->jobinfo[k], PMIX_PREPEND_ENVAR)) {
                } else if (PMIX_CHECK_KEY(&fcd->jobinfo[k], PMIX_APPEND_ENVAR)) {
                }
            }
        }

        /* check for a fork/exec agent we should use */
        if (NULL != app->info) {
            for (k=0; k < app->ninfo; k++) {
                if (PMIX_CHECK_KEY(&app->info[k], PMIX_FORKEXEC_AGENT)) {
                    /* we were given a fork agent - use it. We have to put its
                     * argv at the beginning of the app argv array */
                    argv = pmix_argv_split(app->info[k].value.data.string, ' ');
                    /* add in the argv from the app */
                    for (i=0; NULL != argv[i]; i++) {
                        pmix_argv_prepend_nosize(&app->argv, argv[i]);
                    }
                    if (NULL != app->cmd) {
                        free(app->cmd);
                    }
                    app->cmd = pmix_path_findv(argv[0], X_OK, app->env, NULL);
                    if (NULL == app->cmd) {
                        pmix_show_help("help-pfexec-base.txt",
                                       "fork-agent-not-found",
                                       true, pmix_globals.hostname, argv[0]);
                        rc = PMIX_ERR_NOT_FOUND;
                        pmix_argv_free(argv);
                        goto complete;
                    }
                    pmix_argv_free(argv);
                }
            }
        }

        /* setup the path */
        if (PMIX_SUCCESS != (rc = setup_path(app))) {
            goto complete;
        }

        for (n=0; n < app->maxprocs; n++) {
            /* create a tracker for this child */
            child = PMIX_NEW(pmix_pfexec_child_t);
            PMIX_LOAD_PROCID(&child->proc, nspace, rank);
            ++rank;
            pmix_list_append(&pmix_pfexec_globals.children, &child->super);

            /* setup any IOF */
            child->opts.usepty = PMIX_ENABLE_PTY_SUPPORT;
            if (PMIX_SUCCESS != (rc = setup_prefork(child))) {
                PMIX_ERROR_LOG(rc);
                pmix_list_remove_item(&pmix_pfexec_globals.children, &child->super);
                PMIX_RELEASE(child);
                goto complete;
            }

            /* track some details about the child from our
             * perspective - note that the child may determine
             * its own nspace/rank, but that is irrelevant here
             * as we just need an ID for our own internal tracking */
            info = PMIX_NEW(pmix_rank_info_t);
            if (NULL == info) {
                rc = PMIX_ERR_NOMEM;
                pmix_list_remove_item(&pmix_pfexec_globals.children, &child->super);
                PMIX_RELEASE(child);
                goto complete;
            }
            info->pname.nspace = strdup(child->proc.nspace);
            info->pname.rank = child->proc.rank;
            info->uid = pmix_globals.uid;
            info->gid = pmix_globals.gid;
            pmix_list_append(&nptr->ranks, &info->super);

            /* setup the environment */
            env = pmix_argv_copy(app->env);

            /* we only support the start of tools and servers, not apps,
             * so we don't register this nspace or child. However, we do
             * still want to pass along our connection info and active
             * modules so the forked process can connect back to us */

            /* pass the nspace */
            pmix_setenv("PMIX_NAMESPACE", child->proc.nspace, true, &env);
            pmix_setenv("PMIX_SERVER_NSPACE", child->proc.nspace, true, &env);

            /* pass the rank */
            memset(tmp, 0, 2048);
            (void)snprintf(tmp, 2047, "%u", child->proc.rank);
            pmix_setenv("PMIX_RANK", tmp, true, &env);
            pmix_setenv("PMIX_SERVER_RANK", tmp, true, &env);

            /* get any PTL contribution such as tmpdir settings for session files */
            if (PMIX_SUCCESS != (rc = pmix_ptl_base_setup_fork(&child->proc, &env))) {
                PMIX_ERROR_LOG(rc);
                pmix_list_remove_item(&pmix_pfexec_globals.children, &child->super);
                PMIX_RELEASE(child);
                goto complete;
            }

            /* ensure we agree on our hostname */
            pmix_setenv("PMIX_HOSTNAME", pmix_globals.hostname, true, &env);

            /* communicate our version */
            pmix_setenv("PMIX_VERSION", PMIX_VERSION, true, &env);

            pmix_output_verbose(5, pmix_pfexec_base_framework.framework_output,
                                "%s pfexec:base spawning child %s",
                                PMIX_NAME_PRINT(&pmix_globals.myid), app->cmd);

            rc = fcd->frkfn(app, child, env);
            pmix_argv_free(env);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                pmix_list_remove_item(&pmix_pfexec_globals.children, &child->super);
                PMIX_RELEASE(child);
                goto complete;
            }
            PMIX_IOF_READ_ACTIVATE(child->stdoutev);
            PMIX_IOF_READ_ACTIVATE(child->stderrev);
        }
    }
    rc = PMIX_SUCCESS;

  complete:
    /* ensure we reset our working directory back to our default location  */
    if (0 != chdir(basedir)) {
        PMIX_ERROR_LOG(PMIX_ERROR);
    }

    /* execute the callback */
    fcd->cbfunc(rc, nspace, fcd->cbdata);
    PMIX_RELEASE(fcd);
    return;
}

void pmix_pfexec_base_kill_proc(int sd, short args, void *cbdata)
{
    (void)sd;
    (void)args;
    pmix_pfexec_signal_caddy_t *scd = (pmix_pfexec_signal_caddy_t*)cbdata;
    pmix_pfexec_child_t *child, *cd;

    /* find the process */
    child = NULL;
    PMIX_LIST_FOREACH(cd, &pmix_pfexec_globals.children, pmix_pfexec_child_t) {
        if (PMIX_CHECK_PROCID(scd->proc, &cd->proc)) {
            child = cd;
            break;
        }
    }
    if (NULL == child) {
        scd->lock->status = PMIX_SUCCESS;
        PMIX_WAKEUP_THREAD(scd->lock);
        return;
    }

#if 0
    /* if we opened the stdin IOF channel, be sure
     * we close it */
    if (NULL != orte_iof.close) {
        orte_iof.close(&child->name, ORTE_IOF_STDIN);
    }
#endif

    /* remove the child from the list so waitpid callback won't
     * find it as this induces unmanageable race
     * conditions when we are deliberately killing the process
     */
    pmix_list_remove_item(&pmix_pfexec_globals.children, &child->super);

    /* First send a SIGCONT in case the process is in stopped state.
       If it is in a stopped state and we do not first change it to
       running, then SIGTERM will not get delivered.  Ignore return
       value. */
    PMIX_OUTPUT_VERBOSE((5, pmix_pfexec_base_framework.framework_output,
                         "%s SENDING SIGCONT",
                         PMIX_NAME_PRINT(&pmix_globals.myid)));
    scd->sigfn(child->pid, SIGCONT);

    /* wait a little to give the proc a chance to wakeup */
    sleep(pmix_pfexec_globals.timeout_before_sigkill);
    /* issue a SIGTERM */
    PMIX_OUTPUT_VERBOSE((5, pmix_pfexec_base_framework.framework_output,
                         "%s SENDING SIGTERM",
                         PMIX_NAME_PRINT(&pmix_globals.myid)));
    scd->lock->status = scd->sigfn(child->pid, SIGTERM);

    if (0 != scd->lock->status) {
        /* wait a little again */
        sleep(pmix_pfexec_globals.timeout_before_sigkill);
        /* issue a SIGKILL */
        PMIX_OUTPUT_VERBOSE((5, pmix_pfexec_base_framework.framework_output,
                             "%s SENDING SIGKILL",
                             PMIX_NAME_PRINT(&pmix_globals.myid)));
        scd->lock->status = scd->sigfn(child->pid, SIGKILL);
    }

    /* cleanup */
    PMIX_RELEASE(child);
    PMIX_WAKEUP_THREAD(scd->lock);

#if 0
    /* ensure the child's session directory is cleaned up */
    orte_session_dir_finalize(&child->name);
#endif

    return;
}

void pmix_pfexec_base_signal_proc(int sd, short args, void *cbdata)
{
    (void)sd;
    (void)args;
    pmix_pfexec_signal_caddy_t *scd = (pmix_pfexec_signal_caddy_t*)cbdata;
    pmix_pfexec_child_t *child, *cd;

    /* find the process */
    child = NULL;
    PMIX_LIST_FOREACH(cd, &pmix_pfexec_globals.children, pmix_pfexec_child_t) {
        if (PMIX_CHECK_PROCID(scd->proc, &cd->proc)) {
            child = cd;
            break;
        }
    }
    if (NULL == child) {
        scd->lock->status = PMIX_SUCCESS;
        PMIX_WAKEUP_THREAD(scd->lock);
        return;
    }

    PMIX_OUTPUT_VERBOSE((5, pmix_pfexec_base_framework.framework_output,
                         "%s SIGNALING %d",
                         PMIX_NAME_PRINT(&pmix_globals.myid), scd->signal));
    scd->lock->status = scd->sigfn(child->pid, scd->signal);

    PMIX_WAKEUP_THREAD(scd->lock);
}

static pmix_status_t setup_prefork(pmix_pfexec_child_t *child)
{
    int ret = -1;
    pmix_pfexec_base_io_conf_t *opts = &child->opts;
    pmix_proc_t *targets = NULL;
    pmix_info_t *directives = NULL;

    fflush(stdout);

    /* first check to make sure we can do ptys */
#if PMIX_ENABLE_PTY_SUPPORT
    if (opts->usepty) {
        ret = pmix_openpty(&(opts->p_stdout[0]), &(opts->p_stdout[1]),
                           (char*)NULL, (struct termios*)NULL, (struct winsize*)NULL);
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
    if (opts->connect_stdin) {
        if (pipe(opts->p_stdin) < 0) {
            PMIX_ERROR_LOG(PMIX_ERR_SYS_OTHER);
            return PMIX_ERR_SYS_OTHER;
        }
    }
    if (pipe(opts->p_stderr) < 0) {
        PMIX_ERROR_LOG(PMIX_ERR_SYS_OTHER);
        return PMIX_ERR_SYS_OTHER;
    }

#if 0
    /* connect stdin endpoint */
    if (opts->connect_stdin) {
        /* and connect the pty to stdin */
        ret = orte_iof.pull(name, ORTE_IOF_STDIN, opts->p_stdin[1]);
        if(ORTE_SUCCESS != ret) {
            ORTE_ERROR_LOG(ret);
            return ret;
        }
    }
#endif
    /* connect read ends to IOF */
    PMIX_IOF_READ_EVENT(&child->stdoutev,
                        targets, 0, directives, 0, opts->p_stdout[0],
                        pmix_iof_read_local_handler, false);
    PMIX_LOAD_PROCID(&child->stdoutev->name, child->proc.nspace, child->proc.rank);
    child->stdoutev->childproc = (void*)child;
    child->stdoutev->channel = PMIX_FWD_STDOUT_CHANNEL;
    PMIX_IOF_READ_EVENT(&child->stderrev,
                        targets, 0, directives, 0, opts->p_stderr[0],
                        pmix_iof_read_local_handler, false);
    PMIX_LOAD_PROCID(&child->stderrev->name, child->proc.nspace, child->proc.rank);
    child->stderrev->childproc = (void*)child;
    child->stderrev->channel = PMIX_FWD_STDERR_CHANNEL;

    return PMIX_SUCCESS;
}


pmix_status_t pmix_pfexec_base_setup_child(pmix_pfexec_child_t *child)
{
    int ret;
    pmix_pfexec_base_io_conf_t *opts = &child->opts;

    if (opts->connect_stdin) {
        close(opts->p_stdin[1]);
    }
    close(opts->p_stdout[0]);
    close(opts->p_stderr[0]);

    if (opts->usepty) {
        /* disable echo */
        struct termios term_attrs;
        if (tcgetattr(opts->p_stdout[1], &term_attrs) < 0) {
            return PMIX_ERR_SYS_OTHER;
        }
        term_attrs.c_lflag &= ~ (ECHO | ECHOE | ECHOK |
                                 ECHOCTL | ECHOKE | ECHONL);
        term_attrs.c_iflag &= ~ (ICRNL | INLCR | ISTRIP | INPCK | IXON);
        term_attrs.c_oflag &= ~ (OCRNL | ONLCR);
        if (tcsetattr(opts->p_stdout[1], TCSANOW, &term_attrs) == -1) {
            return PMIX_ERR_SYS_OTHER;
        }
        ret = dup2(opts->p_stdout[1], fileno(stdout));
        if (ret < 0) {
            return PMIX_ERR_SYS_OTHER;
        }
        close(opts->p_stdout[1]);
    } else {
        if(opts->p_stdout[1] != fileno(stdout)) {
            ret = dup2(opts->p_stdout[1], fileno(stdout));
            if (ret < 0) {
                return PMIX_ERR_SYS_OTHER;
            }
            close(opts->p_stdout[1]);
        }
    }
    if (opts->connect_stdin) {
        if(opts->p_stdin[0] != fileno(stdin)) {
            ret = dup2(opts->p_stdin[0], fileno(stdin));
            if (ret < 0) {
                return PMIX_ERR_SYS_OTHER;
            }
            close(opts->p_stdin[0]);
        }
    } else {
        int fd;

        /* connect input to /dev/null */
        fd = open("/dev/null", O_RDONLY, 0);
        if (0 > fd) {
            return PMIX_ERROR;
        }
        if (fd != fileno(stdin)) {
            ret = dup2(fd, fileno(stdin));
            if (ret < 0) {
                close(fd);
                return PMIX_ERR_SYS_OTHER;
            }
        }
        close(fd);
    }

    if (opts->p_stderr[1] != fileno(stderr)) {
        ret = dup2(opts->p_stderr[1], fileno(stderr));
        if (ret < 0) {
            return PMIX_ERR_SYS_OTHER;
        }
        close(opts->p_stderr[1]);
    }

    return PMIX_SUCCESS;
}
