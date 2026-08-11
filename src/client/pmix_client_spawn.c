/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2014-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2014      Artem Y. Polyakov <artpol84@gmail.com>.
 *                         All rights reserved.
 * Copyright (c) 2016      Mellanox Technologies, Inc.
 *                         All rights reserved.
 * Copyright (c) 2016      IBM Corporation.  All rights reserved.
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "src/include/pmix_config.h"

#include "src/include/pmix_prefetch.h"

#include "src/include/pmix_stdint.h"

#include "include/pmix.h"

#include "src/include/pmix_globals.h"

#ifdef HAVE_STRING_H
#    include <string.h>
#endif
#include <fcntl.h>
#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif
#ifdef HAVE_SYS_SOCKET_H
#    include <sys/socket.h>
#endif
#ifdef HAVE_SYS_UN_H
#    include <sys/un.h>
#endif
#ifdef HAVE_SYS_UIO_H
#    include <sys/uio.h>
#endif
#ifdef HAVE_SYS_TYPES_H
#    include <sys/types.h>
#endif
#include <event.h>

#include "src/class/pmix_list.h"
#include "src/common/pmix_pfexec.h"
#include "src/mca/bfrops/bfrops.h"
#include "src/mca/gds/gds.h"
#include "src/mca/pmdl/pmdl.h"
#include "src/mca/pnet/base/base.h"
#include "src/mca/ptl/ptl.h"
#include "src/threads/pmix_threads.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_basename.h"
#include "src/util/pmix_error.h"
#include "src/util/pmix_name_fns.h"
#include "src/util/pmix_output.h"
#include "src/util/pmix_environ.h"
#include "src/util/pmix_getcwd.h"
#include "src/util/pmix_printf.h"

#include "src/server/pmix_server_ops.h"
#include "pmix_client_ops.h"

static void wait_cbfunc(struct pmix_peer_t *pr, pmix_ptl_hdr_t *hdr, pmix_buffer_t *buf,
                        void *cbdata);
static void spawn_cbfunc(pmix_status_t status, char nspace[], void *cbdata);

/* Hold a spawn's output-formatting flags where output can find them before
 * the reply names the namespace they belong to.
 *
 * Only meaningful for a tool: a tool receives forwarded output for the jobs
 * it spawned, so output arriving for a namespace it has not been told about
 * yet can only be from the spawn it just issued. Until this existed, that
 * output fell back to the process-wide defaults and came out unformatted -
 * so "prun --output tag" lost the tag on whichever rank happened to be
 * quickest, which showed up as an intermittent failure whenever anything
 * slowed the reply down (two pruns against one DVM was enough).
 *
 * The string members are not copied: this stand-in owns nothing.
 */
static void stash_spawn_iof_flags(pmix_iof_flags_t *flags)
{
    if (!PMIX_PEER_IS_TOOL(pmix_globals.mypeer) || !flags->set) {
        return;
    }
    memcpy(&pmix_globals.spawn_iof_flags, flags, sizeof(pmix_iof_flags_t));
    pmix_globals.spawn_iof_flags.file = NULL;
    pmix_globals.spawn_iof_flags.directory = NULL;
    /* as in the reply handler: for a non-server, nocopy means no local output */
    if (flags->nocopy) {
        pmix_globals.spawn_iof_flags.local_output = false;
    }
}

PMIX_EXPORT pmix_status_t PMIx_Spawn(const pmix_info_t job_info[], size_t ninfo,
                                     const pmix_app_t apps[], size_t napps, pmix_nspace_t nspace)
{
    pmix_status_t rc;
    pmix_cb_t *cb;

    pmix_output_verbose(2, pmix_client_globals.spawn_output,
                        "%s pmix: spawn called",
                        PMIX_NAME_PRINT(&pmix_globals.myid));

    if (PMIX_UNLIKELY(!pmix_atomic_check_bool(&pmix_globals.initialized))) {
        return PMIX_ERR_INIT;
    }

    if (PMIX_UNLIKELY(pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped))) {
        return PMIX_ERR_NOT_AVAILABLE;
    }

    /* ensure the nspace is initialized - note that nspace
     * is a pmix_nspace_t object, and therefore it cannot
     * be NULL */
    memset(nspace, 0, PMIX_MAX_NSLEN + 1);

    /* create a callback object */
    cb = PMIX_NEW(pmix_cb_t);

    if (PMIX_UNLIKELY(PMIX_SUCCESS != (rc = PMIx_Spawn_nb(job_info, ninfo, apps, napps,
                                                           spawn_cbfunc, cb)))) {
        /* note: the call may have returned PMIX_OPERATION_SUCCEEDED thus indicating
         * that the spawn was atomically completed */
        if (PMIX_OPERATION_SUCCEEDED == rc) {
            if (NULL != cb->pname.nspace) {
                PMIX_LOAD_NSPACE(nspace, cb->pname.nspace);
            }
            rc = PMIX_SUCCESS;
        }
        PMIX_RELEASE(cb);
        return rc;
    }

    /* wait for the result */
    PMIX_WAIT_THREAD(&cb->lock);
    rc = cb->status;
    /* a failed spawn reports no namespace, so leave the caller's (already
     * zeroed) buffer empty rather than copying from a NULL */
    if (NULL != cb->pname.nspace) {
        pmix_strncpy(nspace, cb->pname.nspace, PMIX_MAX_NSLEN);
    }
    PMIX_RELEASE(cb);

    return rc;
}

static void _lclcbfunc(int sd, short args, void *cbdata)
{
    pmix_setup_caddy_t *fcd = (pmix_setup_caddy_t*)cbdata;
    pmix_status_t rc;
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    // set default status
    rc = fcd->status;

    // process IOF requests
    if (PMIX_SUCCESS == fcd->status) {
        rc = pmix_server_process_iof(fcd, fcd->nspace);
    }

    if (NULL != fcd->spcbfunc) {
        fcd->spcbfunc(rc, fcd->nspace, fcd->cbdata);
    }
    // cleanup
    if (NULL != fcd->nspace) {
        free(fcd->nspace);
    }
    PMIX_RELEASE(fcd);
}

static void localcbfunc(pmix_status_t status,
                        char nspace[], void *cbdata)
{
    pmix_setup_caddy_t *fcd = (pmix_setup_caddy_t*)cbdata;

    fcd->status = status;
    if (NULL != nspace) {
        fcd->nspace = strdup(nspace);
    }
    PMIX_THREADSHIFT(fcd, _lclcbfunc);
}

PMIX_EXPORT pmix_status_t PMIx_Spawn_nb(const pmix_info_t job_info[], size_t ninfo,
                                        const pmix_app_t apps[], size_t napps,
                                        pmix_spawn_cbfunc_t cbfunc, void *cbdata)
{
    pmix_buffer_t *msg;
    pmix_cmd_t cmd = PMIX_SPAWNNB_CMD;
    pmix_status_t rc;
    size_t n, m, nappinfo;
    pmix_setup_caddy_t *fcd = NULL;
    pmix_app_t *aptr;
    bool forkexec = false;
    pmix_kval_t *kv;
    pmix_list_t ilist;
    /* Set when the job-level directives asked us to harvest programming-model
     * envars. The harvest itself is deferred until the apps array has been
     * copied, so the result lands on *our* copy. Doing it up front, as this
     * used to, meant PMIx_Setenv() edited the caller's const pmix_app_t
     * array: a caller that reused those apps for a second spawn got the
     * envars a second time, on top of the ones we had already added. */
    bool joblevel_envars = false;
    char cwd[PMIX_PATH_MAX];
    char *tmp, *t2;
    bool proxy = false;
    void *xlist;
    pmix_proc_t parent;
    pmix_data_array_t darray;

    pmix_output_verbose(2, pmix_client_globals.spawn_output,
                        "%s pmix: spawn_nb called",
                        PMIX_NAME_PRINT(&pmix_globals.myid));

    if (PMIX_UNLIKELY(!pmix_atomic_check_bool(&pmix_globals.initialized))) {
        return PMIX_ERR_INIT;
    }

    if (PMIX_UNLIKELY(pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped))) {
        return PMIX_ERR_NOT_AVAILABLE;
    }

    /* check for bozo input - the apps array is walked below and there is
     * nothing to spawn without one */
    if (PMIX_UNLIKELY(NULL == apps || 0 == napps)) {
        return PMIX_ERR_BAD_PARAM;
    }

    /* if we aren't connected, don't attempt to send */
    if (PMIX_UNLIKELY(!pmix_atomic_check_bool(&pmix_globals.connected))) {
        /* if I am a launcher, we default to local fork/exec */
        if (PMIX_PEER_IS_LAUNCHER(pmix_globals.mypeer)) {
            forkexec = true;
        } else if (!PMIX_PEER_IS_SERVER(pmix_globals.mypeer) ||
                   PMIX_PEER_IS_TOOL(pmix_globals.mypeer)) {
            return PMIX_ERR_UNREACH;
        }
    }

    // setup the caddy
    fcd = PMIX_NEW(pmix_setup_caddy_t);
    fcd->spcbfunc = cbfunc;
    fcd->cbdata = cbdata;
    /* everything we are about to put in the caddy's info and apps fields is
     * built here and owned by us, so mark it copied now rather than after
     * the arrays are filled - this is what tells the destructor (scaddes)
     * to free them, and every error return below goes through it */
    fcd->copied = true;

    /* check job info for directives - a non-NULL pointer with a zero count
     * is a legal way of saying "no directives", and it must not be treated
     * as an empty list: PMIx_Info_list_convert() reports PMIX_ERR_EMPTY for
     * one, which used to come back to the caller as the spawn's failure */
    if (NULL != job_info && 0 < ninfo) {
        xlist = PMIx_Info_list_start();
        for (n = 0; n < ninfo; n++) {
            if (PMIX_CHECK_KEY(&job_info[n], PMIX_SETUP_APP_ENVARS)) {
                /* harvested and applied to our copy of the apps, below */
                joblevel_envars = true;
            } else if (PMIX_CHECK_KEY(&job_info[n], PMIX_PARENT_ID)) {
                /* the directive comes straight from the caller, so verify it
                 * really carries a proc before dereferencing the union - a
                 * mistyped PMIX_PARENT_ID used to reach PMIX_XFER_PROCID as
                 * whatever scalar happened to be in data.proc's slot */
                if (PMIX_PROC != job_info[n].value.type ||
                    NULL == job_info[n].value.data.proc) {
                    PMIx_Info_list_release(xlist);
                    PMIX_RELEASE(fcd);
                    return PMIX_ERR_BAD_PARAM;
                }
                PMIX_XFER_PROCID(&parent, job_info[n].value.data.proc);
                proxy = true;
            }
            rc = PMIx_Info_list_xfer(xlist, &job_info[n]);
            if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
                PMIx_Info_list_release(xlist);
                PMIX_RELEASE(fcd);
                return rc;
            }
        }
        // convert the list to an array
        rc = PMIx_Info_list_convert(xlist, &darray);
        if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
            PMIx_Info_list_release(xlist);
            PMIX_RELEASE(fcd);
            return rc;
        }
        fcd->info = darray.array;
        fcd->ninfo = darray.size;
        /* convert copies into darray; the list is ours to release */
        PMIx_Info_list_release(xlist);
    }

    /* sadly, we have to copy the apps array since we are
     * going to modify the individual app structs */
    fcd->napps = napps;
    PMIX_APP_CREATE(fcd->apps, fcd->napps);
    if (PMIX_UNLIKELY(NULL == fcd->apps)) {
        PMIX_RELEASE(fcd);
        return PMIX_ERR_NOMEM;
    }
    for (n = 0; n < napps; n++) {
        aptr = (pmix_app_t *) &apps[n];
        /* protect against bozo case - an argv whose first element is NULL
         * is as empty as no argv at all, and would otherwise reach
         * strdup()/pmix_basename() as a NULL below */
        if ((NULL == aptr->cmd && NULL == aptr->argv) ||
            (NULL != aptr->argv && NULL == aptr->argv[0])) {
            /* they gave us nothing to spawn! */
            PMIX_RELEASE(fcd);
            return PMIX_ERR_BAD_PARAM;
        }
        if (NULL == aptr->cmd) {
            // aptr->argv cannot be NULL as well or we
            // would have caught it above
            fcd->apps[n].cmd = strdup(aptr->argv[0]);
        } else {
            fcd->apps[n].cmd = strdup(aptr->cmd);
        }

        /* if they didn't give us a desired working directory, then
         * take the one we are in */
        if (NULL == aptr->cwd) {
            rc = pmix_getcwd(cwd, sizeof(cwd));
            if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
                PMIX_RELEASE(fcd);
                return rc;
            }
            fcd->apps[n].cwd = strdup(cwd);
        } else {
            fcd->apps[n].cwd = strdup(aptr->cwd);
        }

        /* if they didn't give us the cmd as the first argv, fix it */
        if (NULL == aptr->argv) {
            tmp = pmix_basename(aptr->cmd);
            fcd->apps[n].argv = (char **) malloc(2 * sizeof(char *));
            if (PMIX_UNLIKELY(NULL == fcd->apps[n].argv)) {
                if (NULL != tmp) {
                    free(tmp);
                }
                PMIX_RELEASE(fcd);
                return PMIX_ERR_NOMEM;
            }
            fcd->apps[n].argv[0] = tmp;
            fcd->apps[n].argv[1] = NULL;
        } else {
            fcd->apps[n].argv = PMIx_Argv_copy(aptr->argv);
            /* take the basename from our own copy of the cmd, not from
             * aptr->cmd: the caller is allowed to supply argv without a cmd
             * (we filled it in from argv[0] above), and pmix_basename(NULL)
             * returns NULL, which the strcmp below would then dereference */
            tmp = pmix_basename(fcd->apps[n].cmd);
            t2 = pmix_basename(aptr->argv[0]);
            if (0 != strcmp(tmp, t2)) {
                // assume that the user may have put the argv
                // for their cmd in the argv array, but not
                // started with the actual cmd - so add it
                // to the front of the array
                PMIx_Argv_prepend_nosize(&fcd->apps[n].argv, tmp);
            }
            free(tmp);
            free(t2);
        }

        // copy the env array
        fcd->apps[n].env = PMIx_Argv_copy(aptr->env);

        // copy the #procs
        fcd->apps[n].maxprocs = aptr->maxprocs;

        /* do a quick check of the apps directive array to ensure
         * the ninfo field has been set */
        if (NULL != aptr->info) {
            /* count the directives into a local of our own. "apps" is the
             * caller's const array, so the count we work out is not ours to
             * write back into it - a caller whose apps sit in read-only
             * storage would take a SIGSEGV on the store. Same boundary the
             * envar harvest below respects: above the copy we are running
             * on the caller's memory. */
            nappinfo = aptr->ninfo;
            if (0 == nappinfo) {
                /* look for the info marked as "end" - test the bound
                 * before the dereference, or the guard below can never
                 * be reached (we would have walked off the array first) */
                m = 0;
                while (m < SIZE_MAX && !(PMIX_INFO_IS_END(&aptr->info[m]))) {
                    ++m;
                }
                if (SIZE_MAX == m) {
                    /* nothing we can do */
                    PMIX_RELEASE(fcd);
                    return PMIX_ERR_BAD_PARAM;
                }
                nappinfo = m;
            }

            // copy the info array
            if (0 < nappinfo) {
                PMIX_INFO_CREATE(fcd->apps[n].info, nappinfo);
                if (PMIX_UNLIKELY(NULL == fcd->apps[n].info)) {
                    PMIX_RELEASE(fcd);
                    return PMIX_ERR_NOMEM;
                }
                /* only now that the array exists, so a failure above does
                 * not leave the app claiming directives it does not have */
                fcd->apps[n].ninfo = nappinfo;
                for (m = 0; m < nappinfo; m++) {
                    PMIX_INFO_XFER(&fcd->apps[n].info[m], &aptr->info[m]);
                }
            }
        }

        /* an app-level harvest applies to *this* app's environment only, so
         * it must not be recorded as having covered the job - doing so meant
         * the first app carrying the directive was the only one served, and
         * an MPMD spawn whose second app asked for its own programming
         * model's envars silently got none */
        if (!joblevel_envars) {
            for (m = 0; m < fcd->apps[n].ninfo; m++) {
                if (PMIX_CHECK_KEY(&fcd->apps[n].info[m], PMIX_SETUP_APP_ENVARS)) {
                    PMIX_CONSTRUCT(&ilist, pmix_list_t);
                    rc = pmix_pmdl.harvest_envars(NULL, fcd->apps[n].info, fcd->apps[n].ninfo, &ilist);
                    if (PMIX_ERR_INIT == rc) {
                        /* see the job-level harvest below: this role has no
                         * pmdl framework open, and the directive travels on
                         * to whoever launches for us */
                        PMIX_LIST_DESTRUCT(&ilist);
                        break;
                    }
                    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
                        PMIX_LIST_DESTRUCT(&ilist);
                        PMIX_RELEASE(fcd);
                        return rc;
                    }
                    PMIX_LIST_FOREACH (kv, &ilist, pmix_kval_t) {
                        PMIx_Setenv(kv->value->data.envar.envar, kv->value->data.envar.value, true,
                                    &fcd->apps[n].env);
                    }
                    PMIX_LIST_DESTRUCT(&ilist);
                    break;
                }
            }
        }
    }

    /* now apply any job-level envars the directives asked us to harvest. This
     * happens here, rather than while scanning job_info above, so it lands on
     * our copy of the apps instead of editing the caller's array - see the
     * note on joblevel_envars */
    if (joblevel_envars) {
        PMIX_CONSTRUCT(&ilist, pmix_list_t);
        rc = pmix_pmdl.harvest_envars(NULL, job_info, ninfo, &ilist);
        if (PMIX_ERR_INIT == rc) {
            /* The pmdl framework is opened only by the server and tool roles
             * (see pmix_server_init/pmix_tool_init), so a plain client asking
             * for PMIX_SETUP_APP_ENVARS gets PMIX_ERR_INIT from the stub -
             * "no programming-model support is open in this role", not "the
             * library is not initialized". Failing the spawn on it meant a
             * client could not use the directive at all, and reported a
             * thoroughly misleading reason. There is nothing lost by
             * continuing: the directive is carried in the job-level info we
             * are about to send, and whoever launches for us does the
             * harvest with its own pmdl (see PMIx_server_setup_application). */
            pmix_output_verbose(2, pmix_client_globals.spawn_output,
                                "%s pmix: no local programming-model support - "
                                "leaving PMIX_SETUP_APP_ENVARS to our host",
                                PMIX_NAME_PRINT(&pmix_globals.myid));
            PMIX_LIST_DESTRUCT(&ilist);
            goto envars_done;
        }
        if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
            PMIX_LIST_DESTRUCT(&ilist);
            PMIX_RELEASE(fcd);
            return rc;
        }
        PMIX_LIST_FOREACH (kv, &ilist, pmix_kval_t) {
            /* cycle across all the apps and set this envar */
            for (n = 0; n < fcd->napps; n++) {
                PMIx_Setenv(kv->value->data.envar.envar, kv->value->data.envar.value, true,
                            &fcd->apps[n].env);
            }
        }
        PMIX_LIST_DESTRUCT(&ilist);
    }

envars_done:
    /* if we are a server, then process this ourselves */
    if (PMIX_PEER_IS_SERVER(pmix_globals.mypeer) &&
        !PMIX_PEER_IS_LAUNCHER(pmix_globals.mypeer) &&
        !PMIX_PEER_IS_TOOL(pmix_globals.mypeer)) {

        if (PMIX_UNLIKELY(NULL == pmix_host_server.spawn)) {
            PMIX_RELEASE(fcd);
            return PMIX_ERR_NOT_SUPPORTED;
        }

        /* if I am spawning on behalf of someone else, then
         * that peer is the "spawner" */
        if (proxy) {
            /* find the parent's peer object */
            fcd->peer = pmix_get_peer_object(&parent);
            if (PMIX_UNLIKELY(NULL == fcd->peer)) {
                PMIX_ERROR_LOG(PMIX_ERR_NOT_FOUND);
                PMIX_RELEASE(fcd);
                return PMIX_ERR_NOT_FOUND;
            }
        } else {
            fcd->peer = pmix_globals.mypeer;
        }
        PMIX_RETAIN(fcd->peer);

        /* run a quick check of the directives to see if any IOF
         * requests were included so we can set that up now - helps
         * to catch any early output - and a request for notification
         * of job termination so we can setup the event registration */
        pmix_server_spawn_parser(fcd->peer, &fcd->channels, &fcd->flags,
                                 fcd->info, fcd->ninfo);
        stash_spawn_iof_flags(&fcd->flags);

        /* call the local host */
        rc = pmix_host_server.spawn(&pmix_globals.myid,
                                    fcd->info, fcd->ninfo,
                                    fcd->apps, fcd->napps,
                                    localcbfunc, fcd);
        if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
            PMIX_RELEASE(fcd);
        }
        return rc;
    }

    /* if we are not connected, then just fork/exec
     * the specified application */
    fcd->peer = pmix_globals.mypeer;
    PMIX_RETAIN(fcd->peer);

    if (forkexec) {
        rc = pmix_pfexec_base_spawn_job(fcd);
        if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
            PMIX_RELEASE(fcd);
        }
        return rc;
    }

    msg = PMIX_NEW(pmix_buffer_t);
    /* pack the cmd */
    PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, msg, &cmd, 1, PMIX_COMMAND);
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(msg);
        PMIX_RELEASE(fcd);
        return rc;
    }

    /* pack the job-level directives */
    PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, msg, &fcd->ninfo, 1, PMIX_SIZE);
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(msg);
        PMIX_RELEASE(fcd);
        return rc;
    }
    if (0 < fcd->ninfo) {
        PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, msg, fcd->info, fcd->ninfo, PMIX_INFO);
        if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
            PMIX_ERROR_LOG(rc);
            PMIX_RELEASE(msg);
            PMIX_RELEASE(fcd);
            return rc;
        }
    }

    /* pack the apps */
    PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, msg, &fcd->napps, 1, PMIX_SIZE);
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(msg);
        PMIX_RELEASE(fcd);
        return rc;
    }
    if (0 < fcd->napps) {
        PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, msg, fcd->apps, fcd->napps, PMIX_APP);
        if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
            PMIX_ERROR_LOG(rc);
            PMIX_RELEASE(msg);
            PMIX_RELEASE(fcd);
            return rc;
        }
    }

    /* check for IOF flags */
    pmix_server_spawn_parser(fcd->peer, &fcd->channels, &fcd->flags,
                             fcd->info, fcd->ninfo);
    stash_spawn_iof_flags(&fcd->flags);

    /* push the message into our event base to send to the server */
    PMIX_PTL_SEND_RECV(rc, pmix_client_globals.myserver, msg, wait_cbfunc, (void *) fcd);
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        PMIX_RELEASE(msg);
        PMIX_RELEASE(fcd);
    }

    return rc;
}

/* callback for wait completion */
static void wait_cbfunc(struct pmix_peer_t *pr, pmix_ptl_hdr_t *hdr, pmix_buffer_t *buf,
                        void *cbdata)
{
    pmix_setup_caddy_t *fcd = (pmix_setup_caddy_t *) cbdata;
    char nspace[PMIX_MAX_NSLEN + 1];
    char *n2 = NULL;
    pmix_status_t rc, ret;
    int32_t cnt;
    pmix_namespace_t *nptr, *ns;

    PMIX_ACQUIRE_OBJECT(fcd);

    pmix_output_verbose(2, pmix_globals.debug_output,
                        "pmix:client recv spawn callback activated with %d bytes",
                        (NULL == buf) ? -1 : (int) buf->bytes_used);
    PMIX_HIDE_UNUSED_PARAMS(pr, hdr);

    /* init */
    memset(nspace, 0, PMIX_MAX_NSLEN + 1);

    if (PMIX_UNLIKELY(NULL == buf)) {
        ret = PMIX_ERR_BAD_PARAM;
        goto report;
    }
    /* a zero-byte buffer indicates that this recv is being
     * completed due to a lost connection */
    if (PMIX_BUFFER_IS_EMPTY(buf)) {
        ret = PMIX_ERR_UNREACH;
        goto report;
    }

    /* unpack the returned status */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, pmix_client_globals.myserver, buf, &ret, &cnt, PMIX_STATUS);
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        PMIX_ERROR_LOG(rc);
        ret = rc;
    }
    /* unpack the namespace */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, pmix_client_globals.myserver, buf, &n2, &cnt, PMIX_STRING);
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc && PMIX_ERR_UNPACK_READ_PAST_END_OF_BUFFER != rc)) {
        PMIX_ERROR_LOG(rc);
        ret = rc;
    }
    /* a server with nothing to report ends the message before the namespace,
     * which the tolerated short read above leaves as NULL - and passing NULL
     * to a "%s" conversion is undefined, not a portable "(null)" */
    pmix_output_verbose(1, pmix_globals.debug_output,
                        "pmix:client recv '%s'", (NULL == n2) ? "NULL" : n2);

    if (NULL != n2) {
        /* protect length */
        pmix_strncpy(nspace, n2, PMIX_MAX_NSLEN);
        free(n2);
        PMIX_GDS_STORE_JOB_INFO(rc, pmix_globals.mypeer, nspace, buf);
        /* extract and process any job-related info for this nspace */
        if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
            PMIX_ERROR_LOG(rc);
            ret = rc;
        }
        /* process any IOF flags - we are only concerned if we are a TOOL
         * and need to know if/how we should output any IO */
        if (PMIX_PEER_IS_TOOL(pmix_globals.mypeer)) {
            nptr = NULL;
            PMIX_LIST_FOREACH (ns, &pmix_globals.nspaces, pmix_namespace_t)
            {
                if (PMIX_CHECK_NSPACE(ns->nspace, nspace)) {
                    nptr = ns;
                    break;
                }
            }
            if (PMIX_UNLIKELY(NULL == nptr)) {
                /* shouldn't happen, but protect us */
                nptr = PMIX_NEW(pmix_namespace_t);
                nptr->nspace = strdup(nspace);
                pmix_list_append(&pmix_globals.nspaces, &nptr->super);
            }
            /* as a client, we only handle a select set of the flags */
            memcpy(&nptr->iof_flags, &fcd->flags, sizeof(pmix_iof_flags_t));
            nptr->iof_flags.file = NULL;
            nptr->iof_flags.directory = NULL;
            /* since we are not a server, nocopy equates to no_local_output */
            if (fcd->flags.nocopy) {
                nptr->iof_flags.local_output = false;
            }
            /* the namespace now carries its own flags, so the stand-in for
             * output that arrived before this reply is no longer wanted */
            pmix_iof_init_flags(&pmix_globals.spawn_iof_flags);
            pmix_globals.spawn_iof_flags.set = false;
        }
    }

report:
    if (NULL != fcd->spcbfunc) {
        fcd->spcbfunc(ret, nspace, fcd->cbdata);
    }
    PMIX_RELEASE(fcd);
}

static void spawn_cbfunc(pmix_status_t status, char nspace[], void *cbdata)
{
    pmix_cb_t *cb = (pmix_cb_t *) cbdata;

    PMIX_ACQUIRE_OBJECT(cb);
    cb->status = status;
    if (NULL != nspace) {
        cb->pname.nspace = strdup(nspace);
    }
    PMIX_POST_OBJECT(cb);
    PMIX_WAKEUP_THREAD(&cb->lock);
}
