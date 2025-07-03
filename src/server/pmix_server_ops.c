/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2014-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2014-2015 Artem Y. Polyakov <artpol84@gmail.com>.
 *                         All rights reserved.
 * Copyright (c) 2016-2019 Mellanox Technologies, Inc.
 *                         All rights reserved.
 * Copyright (c) 2016-2020 IBM Corporation.  All rights reserved.
 * Copyright (c) 2021-2025 Nanook Consulting  All rights reserved.
 * Copyright (c) 2022-2023 Triad National Security, LLC. All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "src/include/pmix_config.h"

#include "src/include/pmix_socket_errno.h"
#include "src/include/pmix_stdint.h"

#include "include/pmix_server.h"
#include "src/mca/pcompress/pcompress.h"
#include "src/include/pmix_globals.h"

#ifdef HAVE_STRING_H
#    include <string.h>
#endif
#ifdef HAVE_SYS_STAT_H
#    include <sys/stat.h>
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
#ifdef HAVE_TIME_H
#    include <time.h>
#endif
#include <event.h>

#ifndef MAX
#    define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif

#include "src/class/pmix_hotel.h"
#include "src/class/pmix_list.h"
#include "src/common/pmix_attributes.h"
#include "src/common/pmix_iof.h"
#include "src/common/pmix_monitor.h"
#include "src/hwloc/pmix_hwloc.h"
#include "src/mca/bfrops/base/base.h"
#include "src/mca/gds/base/base.h"
#include "src/mca/plog/plog.h"
#include "src/mca/pnet/pnet.h"
#include "src/mca/psensor/psensor.h"
#include "src/mca/ptl/base/base.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_error.h"
#include "src/util/pmix_name_fns.h"
#include "src/util/pmix_output.h"
#include "src/util/pmix_environ.h"

#include "src/client/pmix_client_ops.h"
#include "pmix_server_ops.h"

pmix_server_module_t pmix_host_server = {
    .client_connected = NULL,
    .client_finalized = NULL,
    .abort = NULL,
    .fence_nb = NULL,
    .direct_modex = NULL,
    .publish = NULL,
    .lookup = NULL,
    .unpublish = NULL,
    .spawn = NULL,
    .connect = NULL,
    .disconnect = NULL,
    .register_events = NULL,
    .deregister_events = NULL,
    .listener = NULL,
    .notify_event = NULL,
    .query = NULL,
    .tool_connected = NULL,
    .log = NULL,
    .allocate = NULL,
    .job_control = NULL,
    .monitor = NULL,
    .get_credential = NULL,
    .validate_credential = NULL,
    .iof_pull = NULL,
    .push_stdin = NULL,
    .group = NULL,
    .fabric = NULL,
    .client_connected2 = NULL,
    .session_control = NULL
};

pmix_status_t pmix_server_abort(pmix_peer_t *peer, pmix_buffer_t *buf,
                                pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    int32_t cnt;
    pmix_status_t rc;
    int status;
    char *msg;
    size_t nprocs;
    pmix_proc_t *procs = NULL;
    pmix_proc_t proc;

    pmix_output_verbose(2, pmix_server_globals.base_output,
                        "recvd ABORT");

    /* unpack the status */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, peer, buf, &status, &cnt, PMIX_STATUS);
    if (PMIX_SUCCESS != rc) {
        return rc;
    }
    /* unpack the message */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, peer, buf, &msg, &cnt, PMIX_STRING);
    if (PMIX_SUCCESS != rc) {
        return rc;
    }
    /* unpack the number of procs */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, peer, buf, &nprocs, &cnt, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        return rc;
    }

    /* unpack any provided procs - these are the procs the caller
     * wants aborted */
    if (0 < nprocs) {
        PMIX_PROC_CREATE(procs, nprocs);
        if (NULL == procs) {
            if (NULL != msg) {
                free(msg);
            }
            return PMIX_ERR_NOMEM;
        }
        cnt = nprocs;
        PMIX_BFROPS_UNPACK(rc, peer, buf, procs, &cnt, PMIX_PROC);
        if (PMIX_SUCCESS != rc) {
            if (NULL != msg) {
                free(msg);
            }
            return rc;
        }
    }

    /* let the local host's server execute it */
    if (NULL != pmix_host_server.abort) {
        pmix_strncpy(proc.nspace, peer->info->pname.nspace, PMIX_MAX_NSLEN);
        proc.rank = peer->info->pname.rank;
        rc = pmix_host_server.abort(&proc, peer->info->server_object, status,
                                    msg, procs, nprocs,
                                    cbfunc, cbdata);
    } else {
        rc = PMIX_ERR_NOT_SUPPORTED;
    }
    PMIX_PROC_FREE(procs, nprocs);

    /* the client passed this msg to us so we could give
     * it to the host server - we are done with it now */
    if (NULL != msg) {
        free(msg);
    }

    return rc;
}

static void opcbfunc(pmix_status_t status, void *cbdata)
{
    pmix_setup_caddy_t *cd = (pmix_setup_caddy_t *) cbdata;

    if (NULL != cd->keys) {
        PMIx_Argv_free(cd->keys);
    }
    if (NULL != cd->codes) {
        free(cd->codes);
    }
    if (NULL != cd->info) {
        PMIX_INFO_FREE(cd->info, cd->ninfo);
    }
    if (NULL != cd->opcbfunc) {
        cd->opcbfunc(status, cd->cbdata);
    }
    PMIX_RELEASE(cd);
}

pmix_status_t pmix_server_publish(pmix_peer_t *peer, pmix_buffer_t *buf,
                                  pmix_op_cbfunc_t cbfunc,
                                  void *cbdata)
{
    pmix_setup_caddy_t *cd;
    pmix_status_t rc;
    int32_t cnt;
    size_t ninfo;
    pmix_proc_t proc;
    uint32_t uid;

    pmix_output_verbose(2, pmix_server_globals.pub_output, "recvd PUBLISH");

    if (NULL == pmix_host_server.publish) {
        return PMIX_ERR_NOT_SUPPORTED;
    }

    /* unpack the effective user id */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, peer, buf, &uid, &cnt, PMIX_UINT32);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    /* unpack the number of info objects */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, peer, buf, &ninfo, &cnt, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    /* we will be adding one for the user id */
    cd = PMIX_NEW(pmix_setup_caddy_t);
    if (NULL == cd) {
        return PMIX_ERR_NOMEM;
    }
    cd->opcbfunc = cbfunc;
    cd->cbdata = cbdata;
    cd->ninfo = ninfo + 1;
    PMIX_INFO_CREATE(cd->info, cd->ninfo);
    if (NULL == cd->info) {
        rc = PMIX_ERR_NOMEM;
        goto cleanup;
    }
    /* unpack the array of info objects */
    if (0 < cd->ninfo) {
        cnt = cd->ninfo;
        PMIX_BFROPS_UNPACK(rc, peer, buf, cd->info, &cnt, PMIX_INFO);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            goto cleanup;
        }
    }
    pmix_strncpy(cd->info[cd->ninfo - 1].key, PMIX_USERID, PMIX_MAX_KEYLEN);
    cd->info[cd->ninfo - 1].value.type = PMIX_UINT32;
    cd->info[cd->ninfo - 1].value.data.uint32 = uid;

    /* call the local server */
    pmix_strncpy(proc.nspace, peer->info->pname.nspace, PMIX_MAX_NSLEN);
    proc.rank = peer->info->pname.rank;
    rc = pmix_host_server.publish(&proc, cd->info, cd->ninfo, opcbfunc, cd);

cleanup:
    if (PMIX_SUCCESS != rc) {
        if (NULL != cd->info) {
            PMIX_INFO_FREE(cd->info, cd->ninfo);
        }
        PMIX_RELEASE(cd);
    }
    return rc;
}

static void lkcbfunc(pmix_status_t status, pmix_pdata_t data[],
                     size_t ndata, void *cbdata)
{
    pmix_setup_caddy_t *cd = (pmix_setup_caddy_t *) cbdata;

    /* cleanup the caddy */
    if (NULL != cd->keys) {
        PMIx_Argv_free(cd->keys);
    }
    if (NULL != cd->info) {
        PMIX_INFO_FREE(cd->info, cd->ninfo);
    }

    /* return the results */
    if (NULL != cd->lkcbfunc) {
        cd->lkcbfunc(status, data, ndata, cd->cbdata);
    }
    PMIX_RELEASE(cd);
}
pmix_status_t pmix_server_lookup(pmix_peer_t *peer, pmix_buffer_t *buf,
                                 pmix_lookup_cbfunc_t cbfunc,
                                 void *cbdata)
{
    pmix_setup_caddy_t *cd;
    int32_t cnt;
    pmix_status_t rc;
    size_t nkeys, i;
    char *sptr;
    size_t ninfo;
    pmix_proc_t proc;
    uint32_t uid;

    pmix_output_verbose(2, pmix_server_globals.pub_output, "recvd LOOKUP");

    if (NULL == pmix_host_server.lookup) {
        return PMIX_ERR_NOT_SUPPORTED;
    }

    /* unpack the effective user id */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, peer, buf, &uid, &cnt, PMIX_UINT32);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    /* unpack the number of keys */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, peer, buf, &nkeys, &cnt, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    /* setup the caddy */
    cd = PMIX_NEW(pmix_setup_caddy_t);
    if (NULL == cd) {
        return PMIX_ERR_NOMEM;
    }
    cd->lkcbfunc = cbfunc;
    cd->cbdata = cbdata;
    /* unpack the array of keys */
    for (i = 0; i < nkeys; i++) {
        cnt = 1;
        PMIX_BFROPS_UNPACK(rc, peer, buf, &sptr, &cnt, PMIX_STRING);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            goto cleanup;
        }
        PMIx_Argv_append_nosize(&cd->keys, sptr);
        free(sptr);
    }
    /* unpack the number of info objects */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, peer, buf, &ninfo, &cnt, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto cleanup;
    }
    /* we will be adding one for the user id */
    cd->ninfo = ninfo + 1;
    PMIX_INFO_CREATE(cd->info, cd->ninfo);
    if (NULL == cd->info) {
        rc = PMIX_ERR_NOMEM;
        goto cleanup;
    }
    /* unpack the array of info objects */
    if (0 < ninfo) {
        cnt = ninfo;
        PMIX_BFROPS_UNPACK(rc, peer, buf, cd->info, &cnt, PMIX_INFO);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            goto cleanup;
        }
    }
    pmix_strncpy(cd->info[cd->ninfo - 1].key, PMIX_USERID, PMIX_MAX_KEYLEN);
    cd->info[cd->ninfo - 1].value.type = PMIX_UINT32;
    cd->info[cd->ninfo - 1].value.data.uint32 = uid;

    /* call the local server */
    pmix_strncpy(proc.nspace, peer->info->pname.nspace, PMIX_MAX_NSLEN);
    proc.rank = peer->info->pname.rank;
    rc = pmix_host_server.lookup(&proc, cd->keys, cd->info, cd->ninfo, lkcbfunc, cd);

cleanup:
    if (PMIX_SUCCESS != rc) {
        if (NULL != cd->keys) {
            PMIx_Argv_free(cd->keys);
        }
        if (NULL != cd->info) {
            PMIX_INFO_FREE(cd->info, cd->ninfo);
        }
        PMIX_RELEASE(cd);
    }
    return rc;
}

pmix_status_t pmix_server_unpublish(pmix_peer_t *peer, pmix_buffer_t *buf,
                                    pmix_op_cbfunc_t cbfunc,
                                    void *cbdata)
{
    pmix_setup_caddy_t *cd;
    int32_t cnt;
    pmix_status_t rc;
    size_t i, nkeys, ninfo;
    char *sptr;
    pmix_proc_t proc;
    uint32_t uid;

    pmix_output_verbose(2, pmix_server_globals.pub_output, "recvd UNPUBLISH");

    if (NULL == pmix_host_server.unpublish) {
        return PMIX_ERR_NOT_SUPPORTED;
    }

    /* unpack the effective user id */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, peer, buf, &uid, &cnt, PMIX_UINT32);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    /* unpack the number of keys */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, peer, buf, &nkeys, &cnt, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    /* setup the caddy */
    cd = PMIX_NEW(pmix_setup_caddy_t);
    if (NULL == cd) {
        return PMIX_ERR_NOMEM;
    }
    cd->opcbfunc = cbfunc;
    cd->cbdata = cbdata;
    /* unpack the array of keys */
    for (i = 0; i < nkeys; i++) {
        cnt = 1;
        PMIX_BFROPS_UNPACK(rc, peer, buf, &sptr, &cnt, PMIX_STRING);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            goto cleanup;
        }
        PMIx_Argv_append_nosize(&cd->keys, sptr);
        free(sptr);
    }
    /* unpack the number of info objects */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, peer, buf, &ninfo, &cnt, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto cleanup;
    }
    /* we will be adding one for the user id */
    cd->ninfo = ninfo + 1;
    PMIX_INFO_CREATE(cd->info, cd->ninfo);
    if (NULL == cd->info) {
        rc = PMIX_ERR_NOMEM;
        goto cleanup;
    }
    /* unpack the array of info objects */
    if (0 < ninfo) {
        cnt = ninfo;
        PMIX_BFROPS_UNPACK(rc, peer, buf, cd->info, &cnt, PMIX_INFO);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            goto cleanup;
        }
    }
    pmix_strncpy(cd->info[cd->ninfo - 1].key, PMIX_USERID, PMIX_MAX_KEYLEN);
    cd->info[cd->ninfo - 1].value.type = PMIX_UINT32;
    cd->info[cd->ninfo - 1].value.data.uint32 = uid;

    /* call the local server */
    pmix_strncpy(proc.nspace, peer->info->pname.nspace, PMIX_MAX_NSLEN);
    proc.rank = peer->info->pname.rank;
    rc = pmix_host_server.unpublish(&proc, cd->keys, cd->info, cd->ninfo, opcbfunc, cd);

cleanup:
    if (PMIX_SUCCESS != rc) {
        if (NULL != cd->keys) {
            PMIx_Argv_free(cd->keys);
        }
        if (NULL != cd->info) {
            PMIX_INFO_FREE(cd->info, cd->ninfo);
        }
        PMIX_RELEASE(cd);
    }
    return rc;
}

pmix_status_t pmix_server_process_iof(pmix_setup_caddy_t *cd,
                                      char nspace[])
{
    pmix_iof_req_t *req;
    pmix_status_t rc;
    pmix_iof_cache_t *iof, *ionext;

    // if no channels to forward, just return success
    if (PMIX_FWD_NO_CHANNELS == cd->channels) {
        return PMIX_SUCCESS;
    }

    /* record the request */
    req = PMIX_NEW(pmix_iof_req_t);
    if (NULL == req) {
        return PMIX_ERR_NOMEM;
    }
    PMIX_RETAIN(cd->peer);
    req->requestor = cd->peer;
    req->nprocs = 1;
    PMIX_PROC_CREATE(req->procs, req->nprocs);
    PMIX_LOAD_PROCID(&req->procs[0], nspace, PMIX_RANK_WILDCARD);
    req->channels = cd->channels;
    req->flags = cd->flags;
    req->local_id = pmix_pointer_array_add(&pmix_globals.iof_requests, req);
    /* process any cached IO */
    PMIX_LIST_FOREACH_SAFE (iof, ionext, &pmix_server_globals.iof, pmix_iof_cache_t) {
        rc = pmix_iof_process_iof(iof->channel, &iof->source, iof->bo,
                                  iof->info, iof->ninfo, req);
        if (PMIX_OPERATION_SUCCEEDED == rc) {
            /* remove it from the list since it has now been forwarded */
            pmix_list_remove_item(&pmix_server_globals.iof, &iof->super);
            PMIX_RELEASE(iof);
        }
    }
    return PMIX_SUCCESS;
}

static void _spcbfunc(int sd, short args, void *cbdata)
{
    pmix_setup_caddy_t *cd = (pmix_setup_caddy_t *) cbdata;
    pmix_status_t rc;
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    // pass along default status
    rc = cd->status;

    /* if it was successful, and there are IOF requests, then
     * register them now */
    if (PMIX_SUCCESS == cd->status) {
        rc = pmix_server_process_iof(cd, cd->nspace);
    }

    if (NULL != cd->spcbfunc) {
        cd->spcbfunc(rc, cd->nspace, cd->cbdata);
    }
    if (NULL != cd->nspace) {
        free(cd->nspace);
    }
    PMIX_RELEASE(cd);
}

void pmix_server_spcbfunc(pmix_status_t status, char nspace[], void *cbdata)
{
    pmix_setup_caddy_t *cd = (pmix_setup_caddy_t *) cbdata;

    cd->status = status;
    if (NULL != nspace) {
        cd->nspace = strdup(nspace);
    }
    PMIX_THREADSHIFT(cd, _spcbfunc);
}

void pmix_server_spawn_parser(pmix_peer_t *peer,
                              pmix_iof_channel_t *channels,
                              pmix_iof_flags_t *flags,
                              pmix_info_t *info,
                              size_t ninfo)
{
    size_t n;
    bool stdout_found = false, stderr_found = false, stddiag_found = false;

    /* run a quick check of the directives to see if any IOF
     * requests were included so we can set that up now - helps
     * to catch any early output - and a request for notification
     * of job termination so we can setup the event registration */
    *channels = PMIX_FWD_NO_CHANNELS;
    for (n = 0; n < ninfo; n++) {
        if (PMIX_CHECK_KEY(&info[n], PMIX_FWD_STDIN)) {
            if (PMIX_INFO_TRUE(&info[n])) {
                *channels |= PMIX_FWD_STDIN_CHANNEL;
            }
        } else if (PMIX_CHECK_KEY(&info[n], PMIX_FWD_STDOUT)) {
            stdout_found = true;
            if (PMIX_INFO_TRUE(&info[n])) {
                *channels |= PMIX_FWD_STDOUT_CHANNEL;
            }
        } else if (PMIX_CHECK_KEY(&info[n], PMIX_FWD_STDERR)) {
            stderr_found = true;
            if (PMIX_INFO_TRUE(&info[n])) {
                *channels |= PMIX_FWD_STDERR_CHANNEL;
            }
        } else if (PMIX_CHECK_KEY(&info[n], PMIX_FWD_STDDIAG)) {
            stddiag_found = true;
            if (PMIX_INFO_TRUE(&info[n])) {
                *channels |= PMIX_FWD_STDDIAG_CHANNEL;
            }
        } else {
            pmix_iof_check_flags(&info[n], flags);
        }
    }
    /* we will construct any required iof request tracker upon completion of the spawn
     * as we need the nspace of the spawned application! */

    if (PMIX_PEER_IS_TOOL(peer)) {
        /* if the requestor is a tool, we default to forwarding all
         * output IO channels */
        if (!stdout_found) {
            *channels |= PMIX_FWD_STDOUT_CHANNEL;
        }
        if (!stderr_found) {
            *channels |= PMIX_FWD_STDERR_CHANNEL;
        }
        if (!stddiag_found) {
            *channels |= PMIX_FWD_STDDIAG_CHANNEL;
        }
    }
}

pmix_status_t pmix_server_spawn(pmix_peer_t *peer, pmix_buffer_t *buf,
                                pmix_spawn_cbfunc_t cbfunc,
                                void *cbdata)
{
    pmix_setup_caddy_t *cd;
    int32_t cnt;
    pmix_status_t rc;
    pmix_proc_t proc;

    pmix_output_verbose(2, pmix_server_globals.spawn_output,
                        "recvd SPAWN from %s",
                        PMIX_PNAME_PRINT(&peer->info->pname));

    if (NULL == pmix_host_server.spawn) {
        return PMIX_ERR_NOT_SUPPORTED;
    }

    /* setup */
    cd = PMIX_NEW(pmix_setup_caddy_t);
    if (NULL == cd) {
        return PMIX_ERR_NOMEM;
    }
    PMIX_RETAIN(peer);
    cd->peer = peer;
    cd->spcbfunc = cbfunc;
    cd->cbdata = cbdata;

    /* unpack the number of job-level directives */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, peer, buf, &cd->ninfo, &cnt, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(cd);
        return rc;
    }
    if (0 < cd->ninfo) {
        PMIX_INFO_CREATE(cd->info, cd->ninfo);
        if (NULL == cd->info) {
            rc = PMIX_ERR_NOMEM;
            goto cleanup;
        }
        cnt = cd->ninfo;
        PMIX_BFROPS_UNPACK(rc, peer, buf, cd->info, &cnt, PMIX_INFO);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            goto cleanup;
        }
        /* run a quick check of the directives to see if any IOF
         * requests were included so we can set that up now - helps
         * to catch any early output - and a request for notification
         * of job termination so we can setup the event registration */
        pmix_server_spawn_parser(peer, &cd->channels, &cd->flags,
                                 cd->info, cd->ninfo);
    }

    /* unpack the number of apps */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, peer, buf, &cd->napps, &cnt, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto cleanup;
    }
    /* unpack the array of apps */
    if (0 < cd->napps) {
        PMIX_APP_CREATE(cd->apps, cd->napps);
        if (NULL == cd->apps) {
            rc = PMIX_ERR_NOMEM;
            goto cleanup;
        }
        cnt = cd->napps;
        PMIX_BFROPS_UNPACK(rc, peer, buf, cd->apps, &cnt, PMIX_APP);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            goto cleanup;
        }
    }
    /* mark that we created the data */
    cd->copied = true;

    /* call the local server */
    PMIX_LOAD_PROCID(&proc, peer->info->pname.nspace, peer->info->pname.rank);
    rc = pmix_host_server.spawn(&proc, cd->info, cd->ninfo,
                                cd->apps, cd->napps,
                                pmix_server_spcbfunc, cd);

cleanup:
    if (PMIX_SUCCESS != rc) {
        if (NULL != cd->info) {
            PMIX_INFO_FREE(cd->info, cd->ninfo);
        }
        if (NULL != cd->apps) {
            PMIX_APP_FREE(cd->apps, cd->napps);
        }
        PMIX_RELEASE(cd);
    }
    return rc;
}

static void _check_cached_events(int sd, short args, void *cbdata)
{
    pmix_setup_caddy_t *scd = (pmix_setup_caddy_t *) cbdata;
    pmix_notify_caddy_t *cd;
    pmix_range_trkr_t rngtrk;
    pmix_proc_t proc;
    int i;
    size_t k, n;
    bool found, matched;
    pmix_buffer_t *relay;
    pmix_status_t ret = PMIX_SUCCESS;
    pmix_cmd_t cmd = PMIX_NOTIFY_CMD;

    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    /* check if any matching notifications have been cached */
    rngtrk.procs = NULL;
    rngtrk.nprocs = 0;
    for (i = 0; i < pmix_globals.max_events; i++) {
        pmix_hotel_knock(&pmix_globals.notifications, i, (void **) &cd);
        if (NULL == cd) {
            continue;
        }
        found = false;
        if (NULL == scd->codes) {
            if (!cd->nondefault) {
                /* they registered a default event handler - always matches */
                found = true;
            }
        } else {
            for (k = 0; k < scd->ncodes; k++) {
                if (scd->codes[k] == cd->status) {
                    found = true;
                    break;
                }
            }
        }
        if (!found) {
            continue;
        }
        /* check if the affected procs (if given) match those they
         * wanted to know about */
        if (!pmix_notify_check_affected(cd->affected, cd->naffected, scd->procs, scd->nprocs)) {
            continue;
        }
        /* check the range */
        if (NULL == cd->targets) {
            rngtrk.procs = &cd->source;
            rngtrk.nprocs = 1;
        } else {
            rngtrk.procs = cd->targets;
            rngtrk.nprocs = cd->ntargets;
        }
        rngtrk.range = cd->range;
        PMIX_LOAD_PROCID(&proc, scd->peer->info->pname.nspace, scd->peer->info->pname.rank);
        if (!pmix_notify_check_range(&rngtrk, &proc)) {
            continue;
        }
        /* if we were given specific targets, check if this is one */
        found = false;
        if (NULL != cd->targets) {
            matched = false;
            for (n = 0; n < cd->ntargets; n++) {
                /* if the source of the event is the same peer just registered, then ignore it
                 * as the event notification system will have already locally
                 * processed it */
                if (PMIX_CHECK_NAMES(&cd->source, &scd->peer->info->pname)) {
                    continue;
                }
                if (PMIX_CHECK_NAMES(&scd->peer->info->pname, &cd->targets[n])) {
                    matched = true;
                    /* track the number of targets we have left to notify */
                    --cd->nleft;
                    /* if this is the last one, then evict this event
                     * from the cache */
                    if (0 == cd->nleft) {
                        pmix_hotel_checkout(&pmix_globals.notifications, cd->room);
                        found = true; // mark that we should release cd
                    }
                    break;
                }
            }
            if (!matched) {
                /* do not notify this one */
                continue;
            }
        }

        /* all matches - notify */
        relay = PMIX_NEW(pmix_buffer_t);
        if (NULL == relay) {
            /* nothing we can do */
            PMIX_ERROR_LOG(PMIX_ERR_NOMEM);
            ret = PMIX_ERR_NOMEM;
            break;
        }
        /* pack the info data stored in the event */
        PMIX_BFROPS_PACK(ret, scd->peer, relay, &cmd, 1, PMIX_COMMAND);
        if (PMIX_SUCCESS != ret) {
            PMIX_ERROR_LOG(ret);
            break;
        }
        PMIX_BFROPS_PACK(ret, scd->peer, relay, &cd->status, 1, PMIX_STATUS);
        if (PMIX_SUCCESS != ret) {
            PMIX_ERROR_LOG(ret);
            break;
        }
        PMIX_BFROPS_PACK(ret, scd->peer, relay, &cd->source, 1, PMIX_PROC);
        if (PMIX_SUCCESS != ret) {
            PMIX_ERROR_LOG(ret);
            break;
        }
        PMIX_BFROPS_PACK(ret, scd->peer, relay, &cd->ninfo, 1, PMIX_SIZE);
        if (PMIX_SUCCESS != ret) {
            PMIX_ERROR_LOG(ret);
            break;
        }
        if (0 < cd->ninfo) {
            PMIX_BFROPS_PACK(ret, scd->peer, relay, cd->info, cd->ninfo, PMIX_INFO);
            if (PMIX_SUCCESS != ret) {
                PMIX_ERROR_LOG(ret);
                break;
            }
        }
        PMIX_SERVER_QUEUE_REPLY(ret, scd->peer, 0, relay);
        if (PMIX_SUCCESS != ret) {
            PMIX_RELEASE(relay);
        }
        if (found) {
            PMIX_RELEASE(cd);
        }
    }
    /* release the caddy */
    if (NULL != scd->codes) {
        free(scd->codes);
    }
    if (NULL != scd->info) {
        PMIX_INFO_FREE(scd->info, scd->ninfo);
    }
    if (NULL != scd->opcbfunc) {
        scd->opcbfunc(ret, scd->cbdata);
    }
    PMIX_RELEASE(scd);
}

/* provide a callback function for the host when it finishes
 * processing the registration */
static void regevopcbfunc(pmix_status_t status, void *cbdata)
{
    pmix_setup_caddy_t *cd = (pmix_setup_caddy_t *) cbdata;

    /* if the registration succeeded, then check local cache */
    if (PMIX_SUCCESS == status) {
        cd->status = status;
        PMIX_THREADSHIFT(cd, _check_cached_events);
        return;
    }

    /* it didn't succeed, so cleanup and execute the callback
     * so we don't hang */
    if (NULL != cd->codes) {
        free(cd->codes);
    }
    if (NULL != cd->info) {
        PMIX_INFO_FREE(cd->info, cd->ninfo);
    }
    if (NULL != cd->opcbfunc) {
        cd->opcbfunc(status, cd->cbdata);
    }
    PMIX_RELEASE(cd);
}

pmix_status_t pmix_server_register_events(pmix_peer_t *peer, pmix_buffer_t *buf,
                                          pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    int32_t cnt;
    pmix_status_t rc;
    pmix_status_t *codes = NULL;
    pmix_info_t *info = NULL;
    size_t ninfo = 0, ncodes, n;
    pmix_regevents_info_t *reginfo, *rptr;
    pmix_peer_events_info_t *prev = NULL;
    pmix_setup_caddy_t *scd;
    bool enviro_events = false;
    bool found;
    pmix_proc_t *affected = NULL;
    size_t naffected = 0;

    pmix_output_verbose(2, pmix_server_globals.event_output,
                        "recvd register events for peer %s:%d",
                        peer->info->pname.nspace, peer->info->pname.rank);

    /* unpack the number of codes */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, peer, buf, &ncodes, &cnt, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    /* unpack the array of codes */
    if (0 < ncodes) {
        codes = (pmix_status_t *) malloc(ncodes * sizeof(pmix_status_t));
        if (NULL == codes) {
            rc = PMIX_ERR_NOMEM;
            goto cleanup;
        }
        cnt = ncodes;
        PMIX_BFROPS_UNPACK(rc, peer, buf, codes, &cnt, PMIX_STATUS);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            goto cleanup;
        }
    }

    /* unpack the number of info objects */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, peer, buf, &ninfo, &cnt, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    /* unpack the array of info objects */
    if (0 < ninfo) {
        PMIX_INFO_CREATE(info, ninfo);
        if (NULL == info) {
            rc = PMIX_ERR_NOMEM;
            goto cleanup;
        }
        cnt = ninfo;
        PMIX_BFROPS_UNPACK(rc, peer, buf, info, &cnt, PMIX_INFO);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            goto cleanup;
        }
    }

    /* check the directives */
    for (n = 0; n < ninfo; n++) {
        if (PMIX_CHECK_KEY(&info[n], PMIX_EVENT_AFFECTED_PROC)) {
            if (NULL != affected) {
                PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
                rc = PMIX_ERR_BAD_PARAM;
                goto cleanup;
            }
            naffected = 1;
            PMIX_PROC_CREATE(affected, naffected);
            memcpy(affected, info[n].value.data.proc, sizeof(pmix_proc_t));
        } else if (PMIX_CHECK_KEY(&info[n], PMIX_EVENT_AFFECTED_PROCS)) {
            if (NULL != affected) {
                PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
                rc = PMIX_ERR_BAD_PARAM;
                goto cleanup;
            }
            naffected = info[n].value.data.darray->size;
            PMIX_PROC_CREATE(affected, naffected);
            memcpy(affected, info[n].value.data.darray->array, naffected * sizeof(pmix_proc_t));
        }
    }

    /* check the codes for system events */
    for (n = 0; n < ncodes; n++) {
        if (PMIX_SYSTEM_EVENT(codes[n])) {
            enviro_events = true;
            break;
        }
    }

    /* if they asked for enviro events, and our host doesn't support
     * register_events, then we cannot meet the request */
    if (enviro_events && NULL == pmix_host_server.register_events) {
        enviro_events = false;
        rc = PMIX_ERR_NOT_SUPPORTED;
        goto cleanup;
    }

    /* if they didn't send us any codes, then they are registering a
     * default event handler. In that case, check only for default
     * handlers and add this request to it, if not already present */
    if (0 == ncodes) {
        PMIX_LIST_FOREACH (reginfo, &pmix_server_globals.events, pmix_regevents_info_t) {
            if (PMIX_MAX_ERR_CONSTANT == reginfo->code) {
                /* both are default handlers */
                prev = PMIX_NEW(pmix_peer_events_info_t);
                if (NULL == prev) {
                    rc = PMIX_ERR_NOMEM;
                    goto cleanup;
                }
                PMIX_RETAIN(peer);
                prev->peer = peer;
                if (NULL != affected) {
                    PMIX_PROC_CREATE(prev->affected, naffected);
                    prev->naffected = naffected;
                    memcpy(prev->affected, affected, naffected * sizeof(pmix_proc_t));
                }
                pmix_list_append(&reginfo->peers, &prev->super);
                break;
            }
        }
        rc = PMIX_OPERATION_SUCCEEDED;
        goto cleanup;
    }

    /* store the event registration info so we can call the registered
     * client when the server notifies the event */
    for (n = 0; n < ncodes; n++) {
        found = false;
        PMIX_LIST_FOREACH (reginfo, &pmix_server_globals.events, pmix_regevents_info_t) {
            if (PMIX_MAX_ERR_CONSTANT == reginfo->code) {
                continue;
            } else if (codes[n] == reginfo->code) {
                found = true;
                break;
            }
        }
        if (found) {
            /* found it - add this request */
            prev = PMIX_NEW(pmix_peer_events_info_t);
            if (NULL == prev) {
                rc = PMIX_ERR_NOMEM;
                goto cleanup;
            }
            PMIX_RETAIN(peer);
            prev->peer = peer;
            if (NULL != affected) {
                PMIX_PROC_CREATE(prev->affected, naffected);
                prev->naffected = naffected;
                memcpy(prev->affected, affected, naffected * sizeof(pmix_proc_t));
            }
            prev->enviro_events = enviro_events;
            pmix_list_append(&reginfo->peers, &prev->super);
        } else {
            /* if we get here, then we didn't find an existing registration for this code */
            rptr = PMIX_NEW(pmix_regevents_info_t);
            if (NULL == rptr) {
                rc = PMIX_ERR_NOMEM;
                goto cleanup;
            }
            rptr->code = codes[n];
            pmix_list_append(&pmix_server_globals.events, &rptr->super);
            prev = PMIX_NEW(pmix_peer_events_info_t);
            if (NULL == prev) {
                rc = PMIX_ERR_NOMEM;
                goto cleanup;
            }
            PMIX_RETAIN(peer);
            prev->peer = peer;
            if (NULL != affected) {
                PMIX_PROC_CREATE(prev->affected, naffected);
                prev->naffected = naffected;
                memcpy(prev->affected, affected, naffected * sizeof(pmix_proc_t));
            }
            prev->enviro_events = enviro_events;
            pmix_list_append(&rptr->peers, &prev->super);
        }
    }

    /* if they asked for enviro events, call the local server */
    if (enviro_events) {
        /* if they don't support this, then we cannot do it */
        if (NULL == pmix_host_server.register_events) {
            rc = PMIX_ERR_NOT_SUPPORTED;
            goto cleanup;
        }
        /* need to ensure the arrays don't go away until after the
         * host RM is done with them */
        scd = PMIX_NEW(pmix_setup_caddy_t);
        if (NULL == scd) {
            rc = PMIX_ERR_NOMEM;
            goto cleanup;
        }
        PMIX_RETAIN(peer);
        scd->peer = peer;
        scd->codes = codes;
        scd->ncodes = ncodes;
        scd->info = info;
        scd->ninfo = ninfo;
        scd->opcbfunc = cbfunc;
        scd->cbdata = cbdata;
        if (PMIX_SUCCESS
            == (rc = pmix_host_server.register_events(scd->codes, scd->ncodes, scd->info,
                                                      scd->ninfo, regevopcbfunc, scd))) {
            /* the host will call us back when completed */
            pmix_output_verbose(
                2, pmix_server_globals.event_output,
                "server register events: host server processing event registration");
            if (NULL != affected) {
                free(affected);
            }
            return rc;
        } else if (PMIX_OPERATION_SUCCEEDED == rc) {
            /* we need to check cached notifications, but we want to ensure
             * that occurs _after_ the client returns from registering the
             * event handler in case the event is flagged for do_not_cache.
             * Setup an event to fire after we return as that means it will
             * occur after we send the registration response back to the client,
             * thus guaranteeing that the client will get their registration
             * callback prior to delivery of an event notification */
            PMIX_RETAIN(peer);
            scd->peer = peer;
            scd->procs = affected;
            scd->nprocs = naffected;
            scd->opcbfunc = NULL;
            scd->cbdata = NULL;
            PMIX_THREADSHIFT(scd, _check_cached_events);
            return rc;
        } else {
            /* host returned a genuine error and won't be calling the callback function */
            pmix_output_verbose(2, pmix_server_globals.event_output,
                                "server register events: host server reg events returned rc =%d",
                                rc);
            PMIX_RELEASE(scd);
            goto cleanup;
        }
    } else {
        rc = PMIX_OPERATION_SUCCEEDED;
        /* we need to check cached notifications, but we want to ensure
         * that occurs _after_ the client returns from registering the
         * event handler in case the event is flagged for do_not_cache.
         * Setup an event to fire after we return as that means it will
         * occur after we send the registration response back to the client,
         * thus guaranteeing that the client will get their registration
         * callback prior to delivery of an event notification */
        scd = PMIX_NEW(pmix_setup_caddy_t);
        PMIX_RETAIN(peer);
        scd->peer = peer;
        scd->codes = codes;
        scd->ncodes = ncodes;
        scd->procs = affected;
        scd->nprocs = naffected;
        scd->opcbfunc = NULL;
        scd->cbdata = NULL;
        PMIX_THREADSHIFT(scd, _check_cached_events);
        if (NULL != info) {
            PMIX_INFO_FREE(info, ninfo);
        }
        return rc;
    }

cleanup:
    pmix_output_verbose(2, pmix_server_globals.event_output,
                        "server register events: ninfo =%lu rc =%d", ninfo, rc);
    if (NULL != info) {
        PMIX_INFO_FREE(info, ninfo);
    }
    if (NULL != codes) {
        free(codes);
    }
    if (NULL != affected) {
        PMIX_PROC_FREE(affected, naffected);
    }
    return rc;
}

void pmix_server_deregister_events(pmix_peer_t *peer, pmix_buffer_t *buf)
{
    int32_t cnt;
    pmix_status_t rc, code;
    pmix_regevents_info_t *reginfo = NULL;
    pmix_regevents_info_t *reginfo_next;
    pmix_peer_events_info_t *prev;

    pmix_output_verbose(2, pmix_server_globals.event_output,
                        "%s recvd deregister events from %s",
                        PMIX_NAME_PRINT(&pmix_globals.myid),
                        PMIX_PEER_PRINT(peer));

    /* unpack codes and process until done */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, peer, buf, &code, &cnt, PMIX_STATUS);
    while (PMIX_SUCCESS == rc) {
        PMIX_LIST_FOREACH_SAFE (reginfo, reginfo_next, &pmix_server_globals.events,
                                pmix_regevents_info_t) {
            if (code == reginfo->code) {
                /* found it - remove this peer from the list */
                PMIX_LIST_FOREACH (prev, &reginfo->peers, pmix_peer_events_info_t) {
                    if (prev->peer == peer) {
                        /* found it */
                        pmix_list_remove_item(&reginfo->peers, &prev->super);
                        PMIX_RELEASE(prev);
                        break;
                    }
                }
                /* if all of the peers for this code are now gone, then remove it */
                if (0 == pmix_list_get_size(&reginfo->peers)) {
                    pmix_list_remove_item(&pmix_server_globals.events, &reginfo->super);
                    /* if this was registered with the host, then deregister it */
                    PMIX_RELEASE(reginfo);
                }
            }
        }
        cnt = 1;
        PMIX_BFROPS_UNPACK(rc, peer, buf, &code, &cnt, PMIX_STATUS);
    }
    if (PMIX_ERR_UNPACK_READ_PAST_END_OF_BUFFER != rc) {
        PMIX_ERROR_LOG(rc);
    }
}

static void local_cbfunc(pmix_status_t status, void *cbdata)
{
    pmix_notify_caddy_t *cd = (pmix_notify_caddy_t *) cbdata;

    if (NULL != cd->cbfunc) {
        cd->cbfunc(status, cd->cbdata);
    }
    PMIX_RELEASE(cd);
}

static void intermed_step(pmix_status_t status, void *cbdata)
{
    pmix_notify_caddy_t *cd = (pmix_notify_caddy_t *) cbdata;
    pmix_status_t rc;

    if (PMIX_SUCCESS != status) {
        rc = status;
        goto complete;
    }

    /* check the range directive - if it is LOCAL, then we are
     * done. Otherwise, it needs to go up to our
     * host for dissemination */
    if (PMIX_RANGE_LOCAL == cd->range) {
        rc = PMIX_SUCCESS;
        goto complete;
    }

    /* pass it to our host RM for distribution */
    if (NULL != pmix_host_server.notify_event) {
        rc = pmix_host_server.notify_event(cd->status, &cd->source, cd->range,
                                           cd->info, cd->ninfo,
                                           local_cbfunc, cd);
        if (PMIX_SUCCESS == rc) {
            /* let the callback function respond for us */
            return;
        }
        if (PMIX_OPERATION_SUCCEEDED == rc ||
            PMIX_ERR_NOT_SUPPORTED == rc) {
            rc = PMIX_SUCCESS; // local_cbfunc will not be called
        }
    } else {
        rc = PMIX_SUCCESS; // local_cbfunc will not be called
    }

complete:
    if (NULL != cd->cbfunc) {
        cd->cbfunc(rc, cd->cbdata);
    }
    PMIX_RELEASE(cd);
}

/* Receive an event sent by the client library. Since it was sent
 * to us by one client, we have to both process it locally to ensure
 * we notify all relevant local clients AND (assuming a range other
 * than LOCAL) deliver to our host, requesting that they send it
 * to all peer servers in the current session */
pmix_status_t pmix_server_event_recvd_from_client(pmix_peer_t *peer, pmix_buffer_t *buf,
                                                  pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    int32_t cnt;
    pmix_status_t rc;
    pmix_notify_caddy_t *cd;
    size_t ninfo, n;

    pmix_output_verbose(2, pmix_server_globals.event_output,
                        "%s:%d recvd event notification from client %s:%d",
                        pmix_globals.myid.nspace, pmix_globals.myid.rank, peer->info->pname.nspace,
                        peer->info->pname.rank);

    cd = PMIX_NEW(pmix_notify_caddy_t);
    if (NULL == cd) {
        return PMIX_ERR_NOMEM;
    }
    cd->cbfunc = cbfunc;
    cd->cbdata = cbdata;
    /* set the source */
    PMIX_LOAD_PROCID(&cd->source, peer->info->pname.nspace, peer->info->pname.rank);

    /* unpack status */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, peer, buf, &cd->status, &cnt, PMIX_STATUS);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto exit;
    }

    /* unpack the range */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, peer, buf, &cd->range, &cnt, PMIX_DATA_RANGE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto exit;
    }

    /* unpack the info keys */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, peer, buf, &ninfo, &cnt, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto exit;
    }
    cd->ninfo = ninfo + 1;
    PMIX_INFO_CREATE(cd->info, cd->ninfo);
    if (NULL == cd->info) {
        rc = PMIX_ERR_NOMEM;
        goto exit;
    }
    if (0 < ninfo) {
        cnt = ninfo;
        PMIX_BFROPS_UNPACK(rc, peer, buf, cd->info, &cnt, PMIX_INFO);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            goto exit;
        }
    }

    /* check to see if we already processed this event - it is possible
     * that a local client "echoed" it back to us and we want to avoid
     * a potential infinite loop */
    for (n = 0; n < ninfo; n++) {
        if (PMIX_CHECK_KEY(&cd->info[n], PMIX_SERVER_INTERNAL_NOTIFY)) {
            /* yep, we did - so don't do it again! */
            rc = PMIX_OPERATION_SUCCEEDED;
            goto exit;
        }
    }

    /* add an info object to mark that we recvd this internally */
    PMIX_INFO_LOAD(&cd->info[cd->ninfo - 1], PMIX_SERVER_INTERNAL_NOTIFY, NULL, PMIX_BOOL);

    /* process it */
    rc = pmix_server_notify_client_of_event(cd->status, &cd->source, cd->range, cd->info,
                                            cd->ninfo, intermed_step, cd);
    if (PMIX_SUCCESS != rc) {
        goto exit;
    }
    return rc;

exit:
    PMIX_RELEASE(cd);
    return rc;
}

pmix_status_t pmix_server_query(pmix_peer_t *peer, pmix_buffer_t *buf,
                                pmix_info_cbfunc_t cbfunc, void *cbdata)
{
    int32_t cnt;
    pmix_status_t rc;
    pmix_query_caddy_t *cd;

    pmix_output_verbose(2, pmix_server_globals.base_output,
                        "recvd query from client");

    cd = PMIX_NEW(pmix_query_caddy_t);
    if (NULL == cd) {
        return PMIX_ERR_NOMEM;
    }
    cd->cbfunc = cbfunc;
    cd->cbdata = cbdata;  // this is the pmix_server_caddy_t we were given
    /* unpack the number of queries */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, peer, buf, &cd->nqueries, &cnt, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(cd);
        return rc;
    }
    /* unpack the queries */
    if (0 < cd->nqueries) {
        PMIX_QUERY_CREATE(cd->queries, cd->nqueries);
        if (NULL == cd->queries) {
            rc = PMIX_ERR_NOMEM;
            PMIX_RELEASE(cd);
            return rc;
        }
        cnt = cd->nqueries;
        PMIX_BFROPS_UNPACK(rc, peer, buf, cd->queries, &cnt, PMIX_QUERY);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_RELEASE(cd);
            return rc;
        }
    }
    PMIX_THREADSHIFT(cd, pmix_parse_localquery);
    return PMIX_SUCCESS;
}

static void logcbfn(pmix_status_t status, void *cbdata)
{
    pmix_shift_caddy_t *cd = (pmix_shift_caddy_t *) cbdata;

    if (NULL != cd->cbfunc.opcbfn) {
        cd->cbfunc.opcbfn(status, cd->cbdata);
    }
    PMIX_RELEASE(cd);
}
pmix_status_t pmix_server_log(pmix_peer_t *peer, pmix_buffer_t *buf,
                              pmix_op_cbfunc_t cbfunc,
                              void *cbdata)
{
    int32_t cnt;
    pmix_status_t rc;
    pmix_shift_caddy_t *cd;
    pmix_proc_t proc;
    time_t timestamp;

    pmix_output_verbose(2, pmix_server_globals.base_output, "recvd log from client");

    /* we need to deliver this to our internal log capability,
     * which may upcall it to our host if it cannot process
     * the request itself */

    /* setup the requesting peer name */
    pmix_strncpy(proc.nspace, peer->info->pname.nspace, PMIX_MAX_NSLEN);
    proc.rank = peer->info->pname.rank;

    cd = PMIX_NEW(pmix_shift_caddy_t);
    if (NULL == cd) {
        return PMIX_ERR_NOMEM;
    }
    cd->cbfunc.opcbfn = cbfunc;
    cd->cbdata = cbdata;
    if (PMIX_PEER_IS_EARLIER(peer, 3, 0, 0)) {
        timestamp = -1;
    } else {
        /* unpack the timestamp */
        cnt = 1;
        PMIX_BFROPS_UNPACK(rc, peer, buf, &timestamp, &cnt, PMIX_TIME);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            goto exit;
        }
    }

    /* unpack the number of data */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, peer, buf, &cd->ninfo, &cnt, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto exit;
    }
    cnt = cd->ninfo;
    PMIX_INFO_CREATE(cd->info, cd->ninfo);
    /* unpack the data */
    if (0 < cnt) {
        PMIX_BFROPS_UNPACK(rc, peer, buf, cd->info, &cnt, PMIX_INFO);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            goto exit;
        }
    }
    /* unpack the number of directives */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, peer, buf, &cd->ndirs, &cnt, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto exit;
    }
    cnt = cd->ndirs;
    /* always add the source to the directives so we
     * can tell downstream if this gets "upcalled" to
     * our host for relay */
    cd->ndirs = cnt + 1;
    /* if a timestamp was sent, then we add it to the directives */
    if (0 < timestamp) {
        cd->ndirs++;
        PMIX_INFO_CREATE(cd->directives, cd->ndirs);
        PMIX_INFO_LOAD(&cd->directives[cnt], PMIX_LOG_SOURCE, &proc, PMIX_PROC);
        PMIX_INFO_LOAD(&cd->directives[cnt + 1], PMIX_LOG_TIMESTAMP, &timestamp, PMIX_TIME);
    } else {
        PMIX_INFO_CREATE(cd->directives, cd->ndirs);
        PMIX_INFO_LOAD(&cd->directives[cnt], PMIX_LOG_SOURCE, &proc, PMIX_PROC);
    }

    /* unpack the directives */
    if (0 < cnt) {
        PMIX_BFROPS_UNPACK(rc, peer, buf, cd->directives, &cnt, PMIX_INFO);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            goto exit;
        }
    }

    /* pass it down */
    rc = pmix_plog.log(&proc, cd->info, cd->ninfo, cd->directives, cd->ndirs, logcbfn, cd);
    return rc;

exit:
    PMIX_RELEASE(cd);
    return rc;
}

pmix_status_t pmix_server_alloc(pmix_peer_t *peer, pmix_buffer_t *buf,
                                pmix_info_cbfunc_t cbfunc,
                                void *cbdata)
{
    int32_t cnt;
    pmix_status_t rc;
    pmix_query_caddy_t *cd;
    pmix_proc_t proc;
    pmix_alloc_directive_t directive;

    pmix_output_verbose(2, pmix_server_globals.base_output,
                        "%s recvd allocate request from client %s",
                        PMIX_NAME_PRINT(&pmix_globals.myid),
                        PMIX_PEER_PRINT(peer));

    if (NULL == pmix_host_server.allocate) {
        return PMIX_ERR_NOT_SUPPORTED;
    }

    cd = PMIX_NEW(pmix_query_caddy_t);
    if (NULL == cd) {
        return PMIX_ERR_NOMEM;
    }
    cd->cbdata = cbdata;

    /* unpack the directive */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, peer, buf, &directive, &cnt, PMIX_ALLOC_DIRECTIVE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto exit;
    }

    /* unpack the number of info objects */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, peer, buf, &cd->ninfo, &cnt, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto exit;
    }
    /* unpack the info */
    if (0 < cd->ninfo) {
        PMIX_INFO_CREATE(cd->info, cd->ninfo);
        cnt = cd->ninfo;
        PMIX_BFROPS_UNPACK(rc, peer, buf, cd->info, &cnt, PMIX_INFO);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            goto exit;
        }
    }

    /* setup the requesting peer name */
    pmix_strncpy(proc.nspace, peer->info->pname.nspace, PMIX_MAX_NSLEN);
    proc.rank = peer->info->pname.rank;

    /* ask the host to execute the request */
    rc = pmix_host_server.allocate(&proc, directive, cd->info, cd->ninfo, cbfunc, cd);
    if (PMIX_SUCCESS != rc) {
        goto exit;
    }
    return PMIX_SUCCESS;

exit:
    PMIX_RELEASE(cd);
    return rc;
}

typedef struct {
    pmix_list_item_t super;
    pmix_epilog_t *epi;
} pmix_srvr_epi_caddy_t;
static PMIX_CLASS_INSTANCE(pmix_srvr_epi_caddy_t, pmix_list_item_t, NULL, NULL);

pmix_status_t pmix_server_job_ctrl(pmix_peer_t *peer, pmix_buffer_t *buf,
                                   pmix_info_cbfunc_t cbfunc,
                                   void *cbdata)
{
    int32_t cnt, m;
    pmix_status_t rc;
    pmix_query_caddy_t *cd;
    pmix_namespace_t *nptr, *tmp;
    pmix_peer_t *pr;
    pmix_proc_t proc;
    size_t n;
    bool recurse = false, leave_topdir = false, duplicate;
    pmix_list_t cachedirs, cachefiles, ignorefiles, epicache;
    pmix_srvr_epi_caddy_t *epicd = NULL;
    pmix_cleanup_file_t *cf, *cf2, *cfptr;
    pmix_cleanup_dir_t *cdir, *cdir2, *cdirptr;

    pmix_output_verbose(2, pmix_server_globals.base_output,
                        "recvd job control request from client");

    if (NULL == pmix_host_server.job_control) {
        return PMIX_ERR_NOT_SUPPORTED;
    }

    cd = PMIX_NEW(pmix_query_caddy_t);
    if (NULL == cd) {
        return PMIX_ERR_NOMEM;
    }
    cd->cbdata = cbdata;

    PMIX_CONSTRUCT(&epicache, pmix_list_t);

    /* unpack the number of targets */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, peer, buf, &cd->ntargets, &cnt, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto exit;
    }
    if (0 < cd->ntargets) {
        PMIX_PROC_CREATE(cd->targets, cd->ntargets);
        cnt = cd->ntargets;
        PMIX_BFROPS_UNPACK(rc, peer, buf, cd->targets, &cnt, PMIX_PROC);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            goto exit;
        }
    }

    /* check targets to find proper place to put any epilog requests */
    if (NULL == cd->targets) {
        epicd = PMIX_NEW(pmix_srvr_epi_caddy_t);
        epicd->epi = &peer->nptr->epilog;
        pmix_list_append(&epicache, &epicd->super);
    } else {
        for (n = 0; n < cd->ntargets; n++) {
            /* find the nspace of this proc */
            nptr = NULL;
            PMIX_LIST_FOREACH (tmp, &pmix_globals.nspaces, pmix_namespace_t) {
                if (0 == strcmp(tmp->nspace, cd->targets[n].nspace)) {
                    nptr = tmp;
                    break;
                }
            }
            if (NULL == nptr) {
                nptr = PMIX_NEW(pmix_namespace_t);
                if (NULL == nptr) {
                    rc = PMIX_ERR_NOMEM;
                    goto exit;
                }
                nptr->nspace = strdup(cd->targets[n].nspace);
                pmix_list_append(&pmix_globals.nspaces, &nptr->super);
            }
            /* if the rank is wildcard, then we use the epilog for the nspace */
            if (PMIX_RANK_WILDCARD == cd->targets[n].rank) {
                epicd = PMIX_NEW(pmix_srvr_epi_caddy_t);
                epicd->epi = &nptr->epilog;
                pmix_list_append(&epicache, &epicd->super);
            } else {
                /* we need to find the precise peer - we can only
                 * do cleanup for a local client */
                for (m = 0; m < pmix_server_globals.clients.size; m++) {
                    pr = (pmix_peer_t *)pmix_pointer_array_get_item(&pmix_server_globals.clients, m);
                    if (NULL == pr) {
                        continue;
                    }
                    if (!PMIX_CHECK_NSPACE(pr->info->pname.nspace, cd->targets[n].nspace)) {
                        continue;
                    }
                    if (pr->info->pname.rank == cd->targets[n].rank) {
                        epicd = PMIX_NEW(pmix_srvr_epi_caddy_t);
                        epicd->epi = &pr->epilog;
                        pmix_list_append(&epicache, &epicd->super);
                        break;
                    }
                }
            }
        }
    }

    /* unpack the number of info objects */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, peer, buf, &cd->ninfo, &cnt, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto exit;
    }
    /* unpack the info */
    if (0 < cd->ninfo) {
        PMIX_INFO_CREATE(cd->info, cd->ninfo);
        cnt = cd->ninfo;
        PMIX_BFROPS_UNPACK(rc, peer, buf, cd->info, &cnt, PMIX_INFO);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            goto exit;
        }
    }

    /* if this includes a request for post-termination cleanup, we handle
     * that request ourselves */
    PMIX_CONSTRUCT(&cachedirs, pmix_list_t);
    PMIX_CONSTRUCT(&cachefiles, pmix_list_t);
    PMIX_CONSTRUCT(&ignorefiles, pmix_list_t);

    cnt = 0; // track how many infos are cleanup related
    for (n = 0; n < cd->ninfo; n++) {
        if (PMIX_CHECK_KEY(&cd->info[n], PMIX_REGISTER_CLEANUP)) {
            ++cnt;
            if (PMIX_STRING != cd->info[n].value.type || NULL == cd->info[n].value.data.string) {
                /* return an error */
                rc = PMIX_ERR_BAD_PARAM;
                goto exit;
            }
            cf = PMIX_NEW(pmix_cleanup_file_t);
            if (NULL == cf) {
                /* return an error */
                rc = PMIX_ERR_NOMEM;
                goto exit;
            }
            cf->path = strdup(cd->info[n].value.data.string);
            pmix_list_append(&cachefiles, &cf->super);
        } else if (PMIX_CHECK_KEY(&cd->info[n], PMIX_REGISTER_CLEANUP_DIR)) {
            ++cnt;
            if (PMIX_STRING != cd->info[n].value.type || NULL == cd->info[n].value.data.string) {
                /* return an error */
                rc = PMIX_ERR_BAD_PARAM;
                goto exit;
            }
            cdir = PMIX_NEW(pmix_cleanup_dir_t);
            if (NULL == cdir) {
                /* return an error */
                rc = PMIX_ERR_NOMEM;
                goto exit;
            }
            cdir->path = strdup(cd->info[n].value.data.string);
            pmix_list_append(&cachedirs, &cdir->super);
        } else if (PMIX_CHECK_KEY(&cd->info[n], PMIX_CLEANUP_RECURSIVE)) {
            recurse = PMIX_INFO_TRUE(&cd->info[n]);
            ++cnt;
        } else if (PMIX_CHECK_KEY(&cd->info[n], PMIX_CLEANUP_IGNORE)) {
            if (PMIX_STRING != cd->info[n].value.type || NULL == cd->info[n].value.data.string) {
                /* return an error */
                rc = PMIX_ERR_BAD_PARAM;
                goto exit;
            }
            cf = PMIX_NEW(pmix_cleanup_file_t);
            if (NULL == cf) {
                /* return an error */
                rc = PMIX_ERR_NOMEM;
                goto exit;
            }
            cf->path = strdup(cd->info[n].value.data.string);
            pmix_list_append(&ignorefiles, &cf->super);
            ++cnt;
        } else if (PMIX_CHECK_KEY(&cd->info[n], PMIX_CLEANUP_LEAVE_TOPDIR)) {
            leave_topdir = PMIX_INFO_TRUE(&cd->info[n]);
            ++cnt;
        }
    }
    if (0 < cnt) {
        /* handle any ignore directives first */
        PMIX_LIST_FOREACH (cf, &ignorefiles, pmix_cleanup_file_t) {
            PMIX_LIST_FOREACH (epicd, &epicache, pmix_srvr_epi_caddy_t) {
                /* scan the existing list of files for any duplicate */
                duplicate = false;
                PMIX_LIST_FOREACH (cf2, &epicd->epi->cleanup_files, pmix_cleanup_file_t) {
                    if (0 == strcmp(cf2->path, cf->path)) {
                        duplicate = true;
                        break;
                    }
                }
                if (!duplicate) {
                    /* append it to the end of the list */
                    cfptr = PMIX_NEW(pmix_cleanup_file_t);
                    cfptr->path = strdup(cf->path);
                    pmix_list_append(&epicd->epi->ignores, &cf->super);
                }
            }
        }
        PMIX_LIST_DESTRUCT(&ignorefiles);
        /* now look at the directories */
        PMIX_LIST_FOREACH (cdir, &cachedirs, pmix_cleanup_dir_t) {
            PMIX_LIST_FOREACH (epicd, &epicache, pmix_srvr_epi_caddy_t) {
                /* scan the existing list of directories for any duplicate */
                duplicate = false;
                PMIX_LIST_FOREACH (cdir2, &epicd->epi->cleanup_dirs, pmix_cleanup_dir_t) {
                    if (0 == strcmp(cdir2->path, cdir->path)) {
                        /* duplicate - check for difference in flags per RFC
                         * precedence rules */
                        if (!cdir->recurse && recurse) {
                            cdir->recurse = recurse;
                        }
                        if (!cdir->leave_topdir && leave_topdir) {
                            cdir->leave_topdir = leave_topdir;
                        }
                        duplicate = true;
                        break;
                    }
                }
                if (!duplicate) {
                    /* check for conflict with ignore */
                    PMIX_LIST_FOREACH (cf, &epicd->epi->ignores, pmix_cleanup_file_t) {
                        if (0 == strcmp(cf->path, cdir->path)) {
                            /* return an error */
                            rc = PMIX_ERR_CONFLICTING_CLEANUP_DIRECTIVES;
                            PMIX_LIST_DESTRUCT(&cachedirs);
                            PMIX_LIST_DESTRUCT(&cachefiles);
                            goto exit;
                        }
                    }
                    /* append it to the end of the list */
                    cdirptr = PMIX_NEW(pmix_cleanup_dir_t);
                    cdirptr->path = strdup(cdir->path);
                    cdirptr->recurse = recurse;
                    cdirptr->leave_topdir = leave_topdir;
                    pmix_list_append(&epicd->epi->cleanup_dirs, &cdirptr->super);
                }
            }
        }
        PMIX_LIST_DESTRUCT(&cachedirs);
        PMIX_LIST_FOREACH (cf, &cachefiles, pmix_cleanup_file_t) {
            PMIX_LIST_FOREACH (epicd, &epicache, pmix_srvr_epi_caddy_t) {
                /* scan the existing list of files for any duplicate */
                duplicate = false;
                PMIX_LIST_FOREACH (cf2, &epicd->epi->cleanup_files, pmix_cleanup_file_t) {
                    if (0 == strcmp(cf2->path, cf->path)) {
                        duplicate = true;
                        break;
                    }
                }
                if (!duplicate) {
                    /* check for conflict with ignore */
                    PMIX_LIST_FOREACH (cf2, &epicd->epi->ignores, pmix_cleanup_file_t) {
                        if (0 == strcmp(cf->path, cf2->path)) {
                            /* return an error */
                            rc = PMIX_ERR_CONFLICTING_CLEANUP_DIRECTIVES;
                            PMIX_LIST_DESTRUCT(&cachedirs);
                            PMIX_LIST_DESTRUCT(&cachefiles);
                            goto exit;
                        }
                    }
                    /* append it to the end of the list */
                    cfptr = PMIX_NEW(pmix_cleanup_file_t);
                    cfptr->path = strdup(cf->path);
                    pmix_list_append(&epicd->epi->cleanup_files, &cfptr->super);
                }
            }
        }
        PMIX_LIST_DESTRUCT(&cachefiles);
        if (cnt == (int) cd->ninfo) {
            /* nothing more to do */
            rc = PMIX_OPERATION_SUCCEEDED;
            goto exit;
        }
    }

    /* setup the requesting peer name */
    pmix_strncpy(proc.nspace, peer->info->pname.nspace, PMIX_MAX_NSLEN);
    proc.rank = peer->info->pname.rank;

    /* ask the host to execute the request */
    if (PMIX_SUCCESS
        != (rc = pmix_host_server.job_control(&proc, cd->targets, cd->ntargets, cd->info, cd->ninfo,
                                              cbfunc, cd))) {
        goto exit;
    }
    PMIX_LIST_DESTRUCT(&epicache);
    return PMIX_SUCCESS;

exit:
    PMIX_RELEASE(cd);
    PMIX_LIST_DESTRUCT(&epicache);
    return rc;
}

pmix_status_t pmix_server_monitor(pmix_peer_t *peer, pmix_buffer_t *buf,
                                  pmix_info_cbfunc_t cbfunc,
                                  void *cbdata)
{
    int32_t cnt;
    pmix_status_t rc;
    pmix_cb_t *cb;

    pmix_output_verbose(2, pmix_server_globals.base_output,
                        "recvd monitor request from client");

    cb = PMIX_NEW(pmix_cb_t);
    if (NULL == cb) {
        return PMIX_ERR_NOMEM;
    }
    cb->cbdata = cbdata;
    cb->cbfunc.infofn = cbfunc;

    /* unpack what is to be monitored */
    cb->info = PMIx_Info_create(1);
    cb->infocopy = true;
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, peer, buf, cb->info, &cnt, PMIX_INFO);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto exit;
    }

    /* unpack the error code */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, peer, buf, &cb->status, &cnt, PMIX_STATUS);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto exit;
    }

    /* unpack the number of directives */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, peer, buf, &cb->ndirs, &cnt, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto exit;
    }
    /* unpack the directives */
    if (0 < cb->ndirs) {
        PMIX_INFO_CREATE(cb->directives, cb->ndirs);
        cnt = cb->ndirs;
        PMIX_BFROPS_UNPACK(rc, peer, buf, cb->directives, &cnt, PMIX_INFO);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            goto exit;
        }
    }

    // pass this over to be processed
    PMIX_THREADSHIFT(cb, pmix_monitor_processing);
    return PMIX_SUCCESS;

exit:
    PMIX_RELEASE(cb);
    return rc;
}

pmix_status_t pmix_server_get_credential(pmix_peer_t *peer, pmix_buffer_t *buf,
                                         pmix_credential_cbfunc_t cbfunc, void *cbdata)
{
    int32_t cnt;
    pmix_status_t rc;
    pmix_query_caddy_t *cd;
    pmix_proc_t proc;

    pmix_output_verbose(2, pmix_globals.debug_output, "recvd get credential request from client");

    if (NULL == pmix_host_server.get_credential) {
        return PMIX_ERR_NOT_SUPPORTED;
    }

    cd = PMIX_NEW(pmix_query_caddy_t);
    if (NULL == cd) {
        return PMIX_ERR_NOMEM;
    }
    cd->cbdata = cbdata;

    /* unpack the number of directives */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, peer, buf, &cd->ninfo, &cnt, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto exit;
    }
    /* unpack the directives */
    if (0 < cd->ninfo) {
        PMIX_INFO_CREATE(cd->info, cd->ninfo);
        cnt = cd->ninfo;
        PMIX_BFROPS_UNPACK(rc, peer, buf, cd->info, &cnt, PMIX_INFO);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            goto exit;
        }
    }

    /* setup the requesting peer name */
    pmix_strncpy(proc.nspace, peer->info->pname.nspace, PMIX_MAX_NSLEN);
    proc.rank = peer->info->pname.rank;

    /* ask the host to execute the request */
    if (PMIX_SUCCESS
        != (rc = pmix_host_server.get_credential(&proc, cd->info, cd->ninfo, cbfunc, cd))) {
        goto exit;
    }
    return PMIX_SUCCESS;

exit:
    PMIX_RELEASE(cd);
    return rc;
}

pmix_status_t pmix_server_validate_credential(pmix_peer_t *peer, pmix_buffer_t *buf,
                                              pmix_validation_cbfunc_t cbfunc, void *cbdata)
{
    int32_t cnt;
    pmix_status_t rc;
    pmix_query_caddy_t *cd;
    pmix_proc_t proc;

    pmix_output_verbose(2, pmix_globals.debug_output,
                        "recvd validate credential request from client");

    if (NULL == pmix_host_server.validate_credential) {
        return PMIX_ERR_NOT_SUPPORTED;
    }

    cd = PMIX_NEW(pmix_query_caddy_t);
    if (NULL == cd) {
        return PMIX_ERR_NOMEM;
    }
    cd->cbdata = cbdata;

    /* unpack the credential */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, peer, buf, &cd->bo, &cnt, PMIX_BYTE_OBJECT);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto exit;
    }

    /* unpack the number of directives */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, peer, buf, &cd->ninfo, &cnt, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto exit;
    }
    /* unpack the directives */
    if (0 < cd->ninfo) {
        PMIX_INFO_CREATE(cd->info, cd->ninfo);
        cnt = cd->ninfo;
        PMIX_BFROPS_UNPACK(rc, peer, buf, cd->info, &cnt, PMIX_INFO);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            goto exit;
        }
    }

    /* setup the requesting peer name */
    pmix_strncpy(proc.nspace, peer->info->pname.nspace, PMIX_MAX_NSLEN);
    proc.rank = peer->info->pname.rank;

    /* ask the host to execute the request */
    if (PMIX_SUCCESS
        != (rc = pmix_host_server.validate_credential(&proc, &cd->bo, cd->info, cd->ninfo, cbfunc,
                                                      cd))) {
        goto exit;
    }
    return PMIX_SUCCESS;

exit:
    PMIX_RELEASE(cd);
    return rc;
}

pmix_status_t pmix_server_iofreg(pmix_peer_t *peer, pmix_buffer_t *buf,
                                 pmix_op_cbfunc_t cbfunc,
                                 void *cbdata)
{
    int32_t cnt;
    pmix_status_t rc;
    pmix_setup_caddy_t *cd;
    pmix_iof_req_t *req;
    size_t refid;

    pmix_output_verbose(2, pmix_server_globals.iof_output, "recvd IOF PULL request from client");

    if (NULL == pmix_host_server.iof_pull) {
        return PMIX_ERR_NOT_SUPPORTED;
    }

    cd = PMIX_NEW(pmix_setup_caddy_t);
    if (NULL == cd) {
        return PMIX_ERR_NOMEM;
    }
    cd->cbdata = cbdata; // this is the pmix_server_caddy_t

    /* unpack the number of procs */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, peer, buf, &cd->nprocs, &cnt, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto exit;
    }
    /* unpack the procs */
    if (0 < cd->nprocs) {
        PMIX_PROC_CREATE(cd->procs, cd->nprocs);
        cnt = cd->nprocs;
        PMIX_BFROPS_UNPACK(rc, peer, buf, cd->procs, &cnt, PMIX_PROC);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            goto exit;
        }
    }

    /* unpack the number of directives */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, peer, buf, &cd->ninfo, &cnt, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto exit;
    }
    /* unpack the directives */
    if (0 < cd->ninfo) {
        PMIX_INFO_CREATE(cd->info, cd->ninfo);
        cnt = cd->ninfo;
        PMIX_BFROPS_UNPACK(rc, peer, buf, cd->info, &cnt, PMIX_INFO);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            goto exit;
        }
    }

    /* unpack the channels */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, peer, buf, &cd->channels, &cnt, PMIX_IOF_CHANNEL);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto exit;
    }

    /* unpack their local reference id */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, peer, buf, &refid, &cnt, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto exit;
    }

    /* add this peer/source/channel combination */
    req = PMIX_NEW(pmix_iof_req_t);
    if (NULL == req) {
        rc = PMIX_ERR_NOMEM;
        goto exit;
    }
    PMIX_RETAIN(peer);
    req->requestor = peer;
    req->nprocs = cd->nprocs;
    if (0 < req->nprocs) {
        PMIX_PROC_CREATE(req->procs, cd->nprocs);
        memcpy(req->procs, cd->procs, req->nprocs * sizeof(pmix_proc_t));
    }
    req->channels = cd->channels;
    req->remote_id = refid;
    req->local_id = pmix_pointer_array_add(&pmix_globals.iof_requests, req);
    cd->ncodes = req->local_id;

    /* ask the host to execute the request */
    rc = pmix_host_server.iof_pull(cd->procs, cd->nprocs,
                                   cd->info, cd->ninfo,
                                   cd->channels, cbfunc, cd);
    if (PMIX_OPERATION_SUCCEEDED == rc) {
        /* the host did it atomically - send the response. In
         * this particular case, we can just use the cbfunc
         * ourselves as it will threadshift and guarantee
         * proper handling (i.e. that the refid will be
         * returned in the response to the client) */
        cbfunc(PMIX_SUCCESS, cd);
        /* returning other than SUCCESS will cause the
         * switchyard to release the cd object */
        return PMIX_SUCCESS;
    }

exit:
    PMIX_RELEASE(cd);
    return rc;
}

pmix_status_t pmix_server_iofdereg(pmix_peer_t *peer, pmix_buffer_t *buf,
                                   pmix_op_cbfunc_t cbfunc,
                                   void *cbdata)
{
    int32_t cnt;
    pmix_status_t rc;
    pmix_setup_caddy_t *cd;
    pmix_iof_req_t *req;
    size_t ninfo, refid;

    pmix_output_verbose(2, pmix_server_globals.iof_output,
                        "recvd IOF DEREGISTER from client");

    if (NULL == pmix_host_server.iof_pull) {
        return PMIX_ERR_NOT_SUPPORTED;
    }

    cd = PMIX_NEW(pmix_setup_caddy_t);
    if (NULL == cd) {
        return PMIX_ERR_NOMEM;
    }
    cd->cbdata = cbdata; // this is the pmix_server_caddy_t

    /* unpack the number of directives */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, peer, buf, &ninfo, &cnt, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto exit;
    }
    /* unpack the directives - note that we have to add one
     * to tell the server to stop forwarding to this channel */
    cd->ninfo = ninfo + 1;
    PMIX_INFO_CREATE(cd->info, cd->ninfo);
    if (0 < ninfo) {
        cnt = ninfo;
        PMIX_BFROPS_UNPACK(rc, peer, buf, cd->info, &cnt, PMIX_INFO);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            goto exit;
        }
    }
    /* add the directive to stop forwarding */
    PMIX_INFO_LOAD(&cd->info[ninfo], PMIX_IOF_STOP, NULL, PMIX_BOOL);

    /* unpack the handler ID */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, peer, buf, &refid, &cnt, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto exit;
    }

    /* get the referenced handler */
    req = (pmix_iof_req_t *) pmix_pointer_array_get_item(&pmix_globals.iof_requests, refid);
    if (NULL == req) {
        /* already gone? */
        rc = PMIX_ERR_NOT_FOUND;
        goto exit;
    }
    pmix_pointer_array_set_item(&pmix_globals.iof_requests, refid, NULL);
    PMIX_RELEASE(req);

    /* tell the server to stop */
    rc = pmix_host_server.iof_pull(cd->procs, cd->nprocs,
                                   cd->info, cd->ninfo,
                                   cd->channels, cbfunc, cd);
    if (PMIX_OPERATION_SUCCEEDED == rc) {
        /* the host did it atomically - send the response. In
         * this particular case, we can just use the cbfunc
         * ourselves as it will threadshift and guarantee
         * proper handling */
        cbfunc(PMIX_SUCCESS, cd);
        /* returning other than SUCCESS will cause the
         * switchyard to release the cd object */
        return PMIX_SUCCESS;
    }

exit:
    return rc;
}

static void stdcbfunc(pmix_status_t status, void *cbdata)
{
    pmix_setup_caddy_t *cd = (pmix_setup_caddy_t *) cbdata;

    if (NULL != cd->opcbfunc) {
        cd->opcbfunc(status, cd->cbdata);
    }
    if (NULL != cd->procs) {
        PMIX_PROC_FREE(cd->procs, cd->nprocs);
    }
    if (NULL != cd->info) {
        PMIX_INFO_FREE(cd->info, cd->ninfo);
    }
    if (NULL != cd->bo) {
        PMIX_BYTE_OBJECT_FREE(cd->bo, 1);
    }
    PMIX_RELEASE(cd);
}

pmix_status_t pmix_server_iofstdin(pmix_peer_t *peer,
                                   pmix_buffer_t *buf,
                                   pmix_op_cbfunc_t cbfunc,
                                   void *cbdata)
{
    int32_t cnt;
    pmix_status_t rc;
    pmix_proc_t source;
    pmix_setup_caddy_t *cd;

    pmix_output_verbose(2, pmix_server_globals.iof_output,
                        "recvd stdin IOF data from tool %s",
                        PMIX_PEER_PRINT(peer));

    if (NULL == pmix_host_server.push_stdin) {
        return PMIX_ERR_NOT_SUPPORTED;
    }

    cd = PMIX_NEW(pmix_setup_caddy_t);
    if (NULL == cd) {
        return PMIX_ERR_NOMEM;
    }
    cd->opcbfunc = cbfunc;
    cd->cbdata = cbdata;

    /* unpack the number of targets */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, peer, buf, &cd->nprocs, &cnt, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto error;
    }
    if (0 < cd->nprocs) {
        PMIX_PROC_CREATE(cd->procs, cd->nprocs);
        if (NULL == cd->procs) {
            rc = PMIX_ERR_NOMEM;
            goto error;
        }
        cnt = cd->nprocs;
        PMIX_BFROPS_UNPACK(rc, peer, buf, cd->procs, &cnt, PMIX_PROC);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            goto error;
        }
    }

    /* unpack the number of directives */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, peer, buf, &cd->ninfo, &cnt, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto error;
    }
    if (0 < cd->ninfo) {
        PMIX_INFO_CREATE(cd->info, cd->ninfo);
        if (NULL == cd->info) {
            rc = PMIX_ERR_NOMEM;
            goto error;
        }
        cnt = cd->ninfo;
        PMIX_BFROPS_UNPACK(rc, peer, buf, cd->info, &cnt, PMIX_INFO);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            goto error;
        }
    }

    /* unpack the data */
    PMIX_BYTE_OBJECT_CREATE(cd->bo, 1);
    if (NULL == cd->bo) {
        rc = PMIX_ERR_NOMEM;
        goto error;
    }

    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, peer, buf, cd->bo, &cnt, PMIX_BYTE_OBJECT);
    if (PMIX_ERR_UNPACK_READ_PAST_END_OF_BUFFER == rc) {
        /* it is okay for them to not send data */
        PMIX_BYTE_OBJECT_FREE(cd->bo, 1);
    } else if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto error;
    }

    /* pass the data to the host */
    pmix_strncpy(source.nspace, peer->nptr->nspace, PMIX_MAX_NSLEN);
    source.rank = peer->info->pname.rank;
    if (PMIX_SUCCESS
        != (rc = pmix_host_server.push_stdin(&source, cd->procs, cd->nprocs, cd->info, cd->ninfo,
                                             cd->bo, stdcbfunc, cd))) {
        if (PMIX_OPERATION_SUCCEEDED != rc) {
            goto error;
        }
    }
    return rc;

error:
    PMIX_RELEASE(cd);
    return rc;
}

static void _fabric_response(int sd, short args, void *cbdata)
{
    pmix_query_caddy_t *qcd = (pmix_query_caddy_t *) cbdata;
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    qcd->cbfunc(PMIX_SUCCESS, qcd->info, qcd->ninfo, qcd->cbdata, NULL, NULL);
    PMIX_RELEASE(qcd);
}

static void frcbfunc(pmix_status_t status, void *cbdata)
{
    pmix_query_caddy_t *qcd = (pmix_query_caddy_t *) cbdata;

    PMIX_ACQUIRE_OBJECT(qcd);
    qcd->status = status;
    PMIX_POST_OBJECT(qcd);

    PMIX_WAKEUP_THREAD(&qcd->lock);
}
/* we are being called from the PMIx server's switchyard function,
 * which means we are in an event and can access global data */
pmix_status_t pmix_server_fabric_register(pmix_server_caddy_t *cd, pmix_buffer_t *buf,
                                          pmix_info_cbfunc_t cbfunc)
{
    int32_t cnt;
    pmix_status_t rc;
    pmix_query_caddy_t *qcd = NULL;
    pmix_proc_t proc;
    pmix_fabric_t fabric;

    pmix_output_verbose(2, pmix_server_globals.base_output,
                        "recvd register_fabric request from client");

    qcd = PMIX_NEW(pmix_query_caddy_t);
    if (NULL == qcd) {
        return PMIX_ERR_NOMEM;
    }
    PMIX_RETAIN(cd);
    qcd->cbfunc = cbfunc;
    qcd->cbdata = cd;

    /* unpack the number of directives */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, cd->peer, buf, &qcd->ninfo, &cnt, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(qcd);
        goto exit;
    }
    /* unpack the directives */
    if (0 < qcd->ninfo) {
        PMIX_INFO_CREATE(cd->info, qcd->ninfo);
        cnt = qcd->ninfo;
        PMIX_BFROPS_UNPACK(rc, cd->peer, buf, qcd->info, &cnt, PMIX_INFO);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_RELEASE(qcd);
            goto exit;
        }
    }

    /* see if we support this request ourselves */
    PMIX_FABRIC_CONSTRUCT(&fabric);
    rc = pmix_pnet.register_fabric(&fabric, qcd->info, qcd->ninfo, frcbfunc, qcd);
    if (PMIX_OPERATION_SUCCEEDED == rc) {
        /* we need to respond, but we want to ensure
         * that occurs _after_ the client returns from its API */
        if (NULL != qcd->info) {
            PMIX_INFO_FREE(qcd->info, qcd->ninfo);
        }
        qcd->info = fabric.info;
        qcd->ninfo = fabric.ninfo;
        PMIX_THREADSHIFT(qcd, _fabric_response);
        return PMIX_SUCCESS;
    } else if (PMIX_SUCCESS == rc) {
        PMIX_WAIT_THREAD(&qcd->lock);
        /* we need to respond, but we want to ensure
         * that occurs _after_ the client returns from its API */
        if (NULL != qcd->info) {
            PMIX_INFO_FREE(qcd->info, qcd->ninfo);
        }
        qcd->info = fabric.info;
        qcd->ninfo = fabric.ninfo;
        PMIX_THREADSHIFT(qcd, _fabric_response);
        return PMIX_SUCCESS;
    }

    /* if we don't internally support it, see if
     * our host does */
    if (NULL == pmix_host_server.fabric) {
        rc = PMIX_ERR_NOT_SUPPORTED;
        goto exit;
    }

    /* setup the requesting peer name */
    PMIX_LOAD_PROCID(&proc, cd->peer->info->pname.nspace, cd->peer->info->pname.rank);

    /* ask the host to execute the request */
    if (PMIX_SUCCESS
        != (rc = pmix_host_server.fabric(&proc, PMIX_FABRIC_REQUEST_INFO, qcd->info, qcd->ninfo,
                                         cbfunc, qcd))) {
        goto exit;
    }
    return PMIX_SUCCESS;

exit:
    if (NULL != qcd) {
        PMIX_RELEASE(qcd);
    }
    return rc;
}

pmix_status_t pmix_server_fabric_update(pmix_server_caddy_t *cd, pmix_buffer_t *buf,
                                        pmix_info_cbfunc_t cbfunc)
{
    int32_t cnt;
    size_t index;
    pmix_status_t rc;
    pmix_query_caddy_t *qcd;
    pmix_proc_t proc;
    pmix_fabric_t fabric;

    pmix_output_verbose(2, pmix_server_globals.base_output,
                        "recvd update_fabric request from client");

    qcd = PMIX_NEW(pmix_query_caddy_t);
    if (NULL == qcd) {
        return PMIX_ERR_NOMEM;
    }
    PMIX_RETAIN(cd);
    qcd->cbdata = cd;

    /* unpack the fabric index */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, cd->peer, buf, &index, &cnt, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto exit;
    }

    /* see if we support this request ourselves */
    PMIX_FABRIC_CONSTRUCT(&fabric);
    fabric.index = index;
    rc = pmix_pnet.update_fabric(&fabric);
    if (PMIX_SUCCESS == rc) {
        /* we need to respond, but we want to ensure
         * that occurs _after_ the client returns from its API */
        if (NULL != qcd->info) {
            PMIX_INFO_FREE(qcd->info, qcd->ninfo);
        }
        qcd->info = fabric.info;
        qcd->ninfo = fabric.ninfo;
        PMIX_THREADSHIFT(qcd, _fabric_response);
        return rc;
    }

    /* if we don't internally support it, see if
     * our host does */
    if (NULL == pmix_host_server.fabric) {
        rc = PMIX_ERR_NOT_SUPPORTED;
        goto exit;
    }

    /* setup the requesting peer name */
    pmix_strncpy(proc.nspace, cd->peer->info->pname.nspace, PMIX_MAX_NSLEN);
    proc.rank = cd->peer->info->pname.rank;
    /* add the index */
    qcd->ninfo = 1;
    PMIX_INFO_CREATE(qcd->info, qcd->ninfo);
    PMIX_INFO_LOAD(&qcd->info[0], PMIX_FABRIC_INDEX, &index, PMIX_SIZE);

    /* ask the host to execute the request */
    if (PMIX_SUCCESS
        != (rc = pmix_host_server.fabric(&proc, PMIX_FABRIC_UPDATE_INFO, qcd->info, qcd->ninfo,
                                         cbfunc, qcd))) {
        goto exit;
    }
    return PMIX_SUCCESS;

exit:
    return rc;
}

pmix_status_t pmix_server_device_dists(pmix_server_caddy_t *cd,
                                       pmix_buffer_t *buf,
                                       pmix_device_dist_cbfunc_t cbfunc)
{
    pmix_topology_t topo = {NULL, NULL};
    pmix_cpuset_t cpuset = {NULL, NULL};
    pmix_status_t rc;
    pmix_device_distance_t *distances;
    size_t ndist;
    int cnt;
    pmix_cb_t cb;
    pmix_kval_t *kv;
    pmix_proc_t proc;

    /* unpack the topology they want us to use */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, cd->peer, buf, &topo, &cnt, PMIX_TOPO);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }

    /* unpack the cpuset */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, cd->peer, buf, &cpuset, &cnt, PMIX_PROC_CPUSET);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto cleanup;
    }

    /* unpack any directives */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, cd->peer, buf, &cd->ninfo, &cnt, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto cleanup;
    }
    if (0 < cd->ninfo) {
        PMIX_INFO_CREATE(cd->info, cd->ninfo);
        cnt = cd->ninfo;
        PMIX_BFROPS_UNPACK(rc, cd->peer, buf, cd->info, &cnt, PMIX_INFO);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            goto cleanup;
        }
    }

    /* if the provided topo is NULL, use my own */
    if (NULL == topo.topology) {
        if (NULL == pmix_globals.topology.topology) {
            /* try to get it */
            rc = pmix_hwloc_load_topology(&pmix_globals.topology);
            if (PMIX_SUCCESS != rc) {
                /* nothing we can do */
                goto cleanup;
            }
        }
        topo.topology = pmix_globals.topology.topology;
    }

    /* if the cpuset is NULL, see if we know the binding of the requesting process */
    if (NULL == cpuset.bitmap) {
        PMIX_CONSTRUCT(&cb, pmix_cb_t);
        cb.key = strdup(PMIX_CPUSET);
        PMIX_LOAD_PROCID(&proc, cd->peer->info->pname.nspace, cd->peer->info->pname.rank);
        cb.proc = &proc;
        cb.scope = PMIX_LOCAL;
        cb.copy = true;
        PMIX_GDS_FETCH_KV(rc, cd->peer, &cb);
        if (PMIX_SUCCESS != rc) {
            PMIX_DESTRUCT(&cb);
            goto cleanup;
        }
        kv = (pmix_kval_t*)pmix_list_get_first(&cb.kvs);
        rc = pmix_hwloc_parse_cpuset_string(kv->value->data.string, &cpuset);
        if (PMIX_SUCCESS != rc) {
            PMIX_DESTRUCT(&cb);
            goto cleanup;
        }
        PMIX_DESTRUCT(&cb);
    }
    /* compute the distances */
    rc = pmix_hwloc_compute_distances(&topo, &cpuset, cd->info, cd->ninfo, &distances, &ndist);
    if (PMIX_SUCCESS == rc) {
        /* send the reply */
        cbfunc(rc, distances, ndist, cd, NULL, NULL);
        PMIX_DEVICE_DIST_FREE(distances, ndist);
    }

cleanup:
    if (NULL != topo.topology &&
        topo.topology != pmix_globals.topology.topology) {
        pmix_hwloc_destruct_topology(&topo);
    }
    if (NULL != cpuset.bitmap) {
        pmix_hwloc_destruct_cpuset(&cpuset);
    }
    if (NULL != cpuset.source) {
        free(cpuset.source);
    }
    return rc;
}

pmix_status_t pmix_server_refresh_cache(pmix_server_caddy_t *cd,
                                        pmix_buffer_t *buf,
                                        pmix_op_cbfunc_t cbfunc)
{
    pmix_proc_t p;
    char *nspace;
    int cnt;
    pmix_status_t rc;
    pmix_cb_t cb;
    pmix_buffer_t *pbkt;
    pmix_kval_t *kv;
    PMIX_HIDE_UNUSED_PARAMS(cbfunc);

    // unpack the ID of the proc being requested
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, cd->peer, buf, &nspace, &cnt, PMIX_STRING);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    PMIX_LOAD_NSPACE(p.nspace, nspace);
    free(nspace);

    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, cd->peer, buf, &p.rank, &cnt, PMIX_PROC_RANK);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }

    /* retrieve the data for the specific rank they are asking about */
    PMIX_CONSTRUCT(&cb, pmix_cb_t);
    cb.proc = &p;
    cb.scope = PMIX_REMOTE;
    cb.copy = false;
    PMIX_GDS_FETCH_KV(rc, pmix_globals.mypeer, &cb);

    // pack it up
    pbkt = PMIX_NEW(pmix_buffer_t);
    // start with the status
    PMIX_BFROPS_PACK(rc, cd->peer, pbkt, &cb.status, 1, PMIX_STATUS);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(pbkt);
        return rc;
    }
    PMIX_LIST_FOREACH(kv, &cb.kvs, pmix_kval_t) {
        PMIX_BFROPS_PACK(rc, cd->peer, pbkt, kv, 1, PMIX_KVAL);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_RELEASE(pbkt);
            return rc;
        }
    }
    PMIX_DESTRUCT(&cb);

    // send it back to the requestor
    PMIX_SERVER_QUEUE_REPLY(rc, cd->peer, cd->hdr.tag, pbkt);
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(pbkt);
    }

    return PMIX_SUCCESS;
}

pmix_status_t pmix_server_resblk(pmix_server_caddy_t *cd,
                                 pmix_buffer_t *buf,
                                 pmix_op_cbfunc_t cbfunc)
{
    int32_t cnt;
    pmix_status_t rc;
    pmix_setup_caddy_t *scd;
    pmix_proc_t proc;
    pmix_resource_block_directive_t directive;

    pmix_output_verbose(2, pmix_server_globals.base_output,
                        "%s recvd resource block request from client %s",
                        PMIX_NAME_PRINT(&pmix_globals.myid),
                        PMIX_PEER_PRINT(cd->peer));

    if (NULL == pmix_host_server.resource_block) {
        return PMIX_ERR_NOT_SUPPORTED;
    }

    scd = PMIX_NEW(pmix_setup_caddy_t);
    if (NULL == cd) {
        return PMIX_ERR_NOMEM;
    }
    scd->cbdata = cd;

    /* unpack the directive */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, cd->peer, buf, &directive, &cnt, PMIX_RESBLOCK_DIRECTIVE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto exit;
    }

    // unpack the block name
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, cd->peer, buf, &scd->nspace, &cnt, PMIX_STRING);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto exit;
    }

    /* unpack the number of units */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, cd->peer, buf, &scd->nunits, &cnt, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto exit;
    }
    /* unpack the resource units */
    if (0 < scd->nunits) {
        PMIX_RESOURCE_UNIT_CREATE(scd->units, scd->nunits);
        cnt = scd->nunits;
        PMIX_BFROPS_UNPACK(rc, cd->peer, buf, scd->units, &cnt, PMIX_RESOURCE_UNIT);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            goto exit;
        }
    }

    /* unpack the number of info */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, cd->peer, buf, &scd->ninfo, &cnt, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto exit;
    }
    /* unpack the info */
    if (0 < scd->ninfo) {
        PMIX_INFO_CREATE(scd->info, scd->ninfo);
        cnt = scd->ninfo;
        PMIX_BFROPS_UNPACK(rc, cd->peer, buf, scd->info, &cnt, PMIX_INFO);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            goto exit;
        }
    }

    /* setup the requesting peer name */
    pmix_strncpy(proc.nspace, cd->peer->info->pname.nspace, PMIX_MAX_NSLEN);
    proc.rank = cd->peer->info->pname.rank;

    /* ask the host to execute the request */
    rc = pmix_host_server.resource_block(&proc, directive, scd->nspace,
                                         scd->units, scd->nunits,
                                         scd->info, scd->ninfo,
                                         cbfunc, scd);
    if (PMIX_SUCCESS != rc) {
        goto exit;
    }
    return PMIX_SUCCESS;

exit:
    PMIX_RELEASE(scd);
    return rc;
}


pmix_status_t pmix_server_session_ctrl(pmix_server_caddy_t *cd,
                                       pmix_buffer_t *buf,
                                       pmix_info_cbfunc_t cbfunc)
{
    int32_t cnt;
    pmix_status_t rc;
    pmix_shift_caddy_t *scd;
    pmix_proc_t proc;

    pmix_output_verbose(2, pmix_server_globals.base_output,
                        "%s recvd session ctrl request from client %s",
                        PMIX_NAME_PRINT(&pmix_globals.myid),
                        PMIX_PEER_PRINT(cd->peer));

    if (NULL == pmix_host_server.session_control) {
        return PMIX_ERR_NOT_SUPPORTED;
    }

    scd = PMIX_NEW(pmix_shift_caddy_t);
    if (NULL == cd) {
        return PMIX_ERR_NOMEM;
    }
    scd->cbdata = cd;
    scd->cbfunc.infocbfunc = cbfunc;

    /* unpack the sessionID */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, cd->peer, buf, &scd->sessionid, &cnt, PMIX_UINT32);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto exit;
    }

    /* unpack the number of info objects */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, cd->peer, buf, &scd->ninfo, &cnt, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto exit;
    }
    /* unpack the info */
    if (0 < scd->ninfo) {
        PMIX_INFO_CREATE(scd->info, scd->ninfo);
        cnt = scd->ninfo;
        PMIX_BFROPS_UNPACK(rc, cd->peer, buf, scd->info, &cnt, PMIX_INFO);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            goto exit;
        }
    }

    /* setup the requesting peer name */
    pmix_strncpy(proc.nspace, cd->peer->info->pname.nspace, PMIX_MAX_NSLEN);
    proc.rank = cd->peer->info->pname.rank;

    /* ask the host to execute the request */
    rc = pmix_host_server.session_control(&proc, scd->sessionid,
                                          scd->info, scd->ninfo, cbfunc, scd);
    if (PMIX_SUCCESS != rc) {
        goto exit;
    }
    return PMIX_SUCCESS;

exit:
    PMIX_RELEASE(scd);
    return rc;
}

/*****    INSTANCE SERVER LIBRARY CLASSES    *****/
static void gcon(pmix_grpinfo_t *p)
{
    PMIX_LOAD_PROCID(&p->proc, NULL, PMIX_RANK_UNDEF);
    PMIX_BYTE_OBJECT_CONSTRUCT(&p->blob);
}
static void gdes(pmix_grpinfo_t *p)
{
    PMIX_BYTE_OBJECT_DESTRUCT(&p->blob);
}
PMIX_CLASS_INSTANCE(pmix_grpinfo_t,
                    pmix_list_item_t,
                    gcon, gdes);

static void tcon(pmix_server_trkr_t *t)
{
    t->event_active = false;
    t->host_called = false;
    t->local = true;
    t->id = NULL;
    memset(t->pname.nspace, 0, PMIX_MAX_NSLEN + 1);
    t->pname.rank = PMIX_RANK_UNDEF;
    t->pcs = NULL;
    t->npcs = 0;
    PMIX_CONSTRUCT(&t->nslist, pmix_list_t);
    PMIX_CONSTRUCT_LOCK(&t->lock);
    t->def_complete = false;
    PMIX_CONSTRUCT(&t->local_cbs, pmix_list_t);
    t->nlocal = 0;
    t->local_cnt = 0;
    t->info = NULL;
    t->ninfo = 0;
    PMIX_CONSTRUCT(&t->grpinfo, pmix_list_t);
    t->grpop = PMIX_GROUP_NONE;
    /* this needs to be set explicitly */
    t->collect_type = PMIX_COLLECT_INVALID;
    t->modexcbfunc = NULL;
    t->op_cbfunc = NULL;
    t->info_cbfunc = NULL;
    t->hybrid = false;
    t->cbdata = NULL;
}
static void tdes(pmix_server_trkr_t *t)
{
    if (NULL != t->id) {
        free(t->id);
    }
    PMIX_DESTRUCT_LOCK(&t->lock);
    if (NULL != t->pcs) {
        free(t->pcs);
    }
    PMIX_LIST_DESTRUCT(&t->local_cbs);
    if (NULL != t->info) {
        PMIX_INFO_FREE(t->info, t->ninfo);
    }
    PMIX_LIST_DESTRUCT(&t->grpinfo);
    PMIX_LIST_DESTRUCT(&t->nslist);
}
PMIX_CLASS_INSTANCE(pmix_server_trkr_t,
                    pmix_list_item_t,
                    tcon, tdes);

static void cdcon(pmix_server_caddy_t *cd)
{
    memset(&cd->ev, 0, sizeof(pmix_event_t));
    cd->event_active = false;
    cd->trk = NULL;
    cd->peer = NULL;
    cd->info = NULL;
    cd->ninfo = 0;
    cd->query = NULL;
}
static void cddes(pmix_server_caddy_t *cd)
{
    if (cd->event_active) {
        pmix_event_del(&cd->ev);
    }
    if (NULL != cd->trk) {
        PMIX_RELEASE(cd->trk);
    }
    if (NULL != cd->peer) {
        PMIX_RELEASE(cd->peer);
    }
    if (NULL != cd->info) {
        PMIX_INFO_FREE(cd->info, cd->ninfo);
    }
    if (NULL != cd->query) {
        PMIX_QUERY_FREE(cd->query, 1);
    }
}
PMIX_CLASS_INSTANCE(pmix_server_caddy_t,
                    pmix_list_item_t,
                    cdcon, cddes);

static void scadcon(pmix_setup_caddy_t *p)
{
    p->peer = NULL;
    memset(&p->proc, 0, sizeof(pmix_proc_t));
    PMIX_CONSTRUCT_LOCK(&p->lock);
    p->nspace = NULL;
    p->codes = NULL;
    p->ncodes = 0;
    p->procs = NULL;
    p->nprocs = 0;
    p->apps = NULL;
    p->napps = 0;
    p->server_object = NULL;
    p->nlocalprocs = 0;
    p->info = NULL;
    p->ninfo = 0;
    p->units = NULL;
    p->nunits = 0;
    p->copied = false;
    p->keys = NULL;
    p->channels = PMIX_FWD_NO_CHANNELS;
    memset(&p->flags, 0, sizeof(pmix_iof_flags_t));
    p->bo = NULL;
    p->nbo = 0;
    p->cbfunc = NULL;
    p->opcbfunc = NULL;
    p->setupcbfunc = NULL;
    p->lkcbfunc = NULL;
    p->spcbfunc = NULL;
    p->cbdata = NULL;
}
static void scaddes(pmix_setup_caddy_t *p)
{
    if (NULL != p->peer) {
        PMIX_RELEASE(p->peer);
    }
    PMIX_PROC_FREE(p->procs, p->nprocs);
    if (p->copied) {
        if (NULL != p->info) {
            PMIX_INFO_FREE(p->info, p->ninfo);
        }
        if (NULL != p->apps) {
            PMIX_APP_FREE(p->apps, p->napps);
        }
    }
    PMIX_RESOURCE_UNIT_FREE(p->units, p->nunits);
    if (NULL != p->bo) {
        PMIX_BYTE_OBJECT_FREE(p->bo, p->nbo);
    }
    PMIX_DESTRUCT_LOCK(&p->lock);
    if (NULL != p->flags.file) {
        free(p->flags.file);
    }
    if (NULL != p->flags.directory) {
        free(p->flags.directory);
    }
}
PMIX_EXPORT PMIX_CLASS_INSTANCE(pmix_setup_caddy_t,
                                pmix_object_t,
                                scadcon, scaddes);

PMIX_CLASS_INSTANCE(pmix_trkr_caddy_t, pmix_object_t, NULL, NULL);

static void dmcon(pmix_dmdx_remote_t *p)
{
    p->cd = NULL;
}
static void dmdes(pmix_dmdx_remote_t *p)
{
    if (NULL != p->cd) {
        PMIX_RELEASE(p->cd);
    }
}
PMIX_CLASS_INSTANCE(pmix_dmdx_remote_t,
                    pmix_list_item_t,
                    dmcon, dmdes);

static void dmrqcon(pmix_dmdx_request_t *p)
{
    memset(&p->ev, 0, sizeof(pmix_event_t));
    p->event_active = false;
    p->lcd = NULL;
    p->key = NULL;
}
static void dmrqdes(pmix_dmdx_request_t *p)
{
    if (p->event_active) {
        pmix_event_del(&p->ev);
    }
    if (NULL != p->lcd) {
        PMIX_RELEASE(p->lcd);
    }
    if (NULL != p->key) {
        free(p->key);
    }
}
PMIX_CLASS_INSTANCE(pmix_dmdx_request_t,
                    pmix_list_item_t,
                    dmrqcon, dmrqdes);

static void lmcon(pmix_dmdx_local_t *p)
{
    memset(&p->proc, 0, sizeof(pmix_proc_t));
    PMIX_CONSTRUCT(&p->loc_reqs, pmix_list_t);
    p->info = NULL;
    p->ninfo = 0;
}
static void lmdes(pmix_dmdx_local_t *p)
{
    if (NULL != p->info) {
        PMIX_INFO_FREE(p->info, p->ninfo);
    }
    PMIX_LIST_DESTRUCT(&p->loc_reqs);
}
PMIX_CLASS_INSTANCE(pmix_dmdx_local_t,
                    pmix_list_item_t,
                    lmcon, lmdes);

static void prevcon(pmix_peer_events_info_t *p)
{
    p->peer = NULL;
    p->affected = NULL;
    p->naffected = 0;
}
static void prevdes(pmix_peer_events_info_t *p)
{
    if (NULL != p->peer) {
        PMIX_RELEASE(p->peer);
    }
    if (NULL != p->affected) {
        PMIX_PROC_FREE(p->affected, p->naffected);
    }
}
PMIX_CLASS_INSTANCE(pmix_peer_events_info_t,
                    pmix_list_item_t,
                    prevcon, prevdes);

static void regcon(pmix_regevents_info_t *p)
{
    PMIX_CONSTRUCT(&p->peers, pmix_list_t);
}
static void regdes(pmix_regevents_info_t *p)
{
    PMIX_LIST_DESTRUCT(&p->peers);
}
PMIX_CLASS_INSTANCE(pmix_regevents_info_t,
                    pmix_list_item_t,
                    regcon, regdes);

static void ilcon(pmix_inventory_rollup_t *p)
{
    PMIX_CONSTRUCT_LOCK(&p->lock);
    p->lock.active = false;
    p->status = PMIX_SUCCESS;
    p->requests = 0;
    p->replies = 0;
    PMIX_CONSTRUCT(&p->payload, pmix_list_t);
    p->info = NULL;
    p->ninfo = 0;
    p->cbfunc = NULL;
    p->infocbfunc = NULL;
    p->opcbfunc = NULL;
    p->cbdata = NULL;
}
static void ildes(pmix_inventory_rollup_t *p)
{
    PMIX_DESTRUCT_LOCK(&p->lock);
    PMIX_LIST_DESTRUCT(&p->payload);
}
PMIX_CLASS_INSTANCE(pmix_inventory_rollup_t,
                    pmix_object_t,
                    ilcon, ildes);

PMIX_CLASS_INSTANCE(pmix_group_caddy_t,
                    pmix_list_item_t,
                    NULL, NULL);

static void iocon(pmix_iof_cache_t *p)
{
    p->bo = NULL;
    p->info = NULL;
    p->ninfo = 0;
}
static void iodes(pmix_iof_cache_t *p)
{
    PMIX_BYTE_OBJECT_FREE(p->bo, 1); // macro protects against NULL
    if (0 < p->ninfo) {
        PMIX_INFO_FREE(p->info, p->ninfo);
    }
}
PMIX_CLASS_INSTANCE(pmix_iof_cache_t,
                    pmix_list_item_t,
                    iocon, iodes);

static void pscon(pmix_pset_t *p)
{
    p->name = NULL;
    p->members = NULL;
    p->nmembers = 0;
}
static void psdes(pmix_pset_t *p)
{
    if (NULL != p->name) {
        free(p->name);
    }
    if (NULL != p->members) {
        free(p->members);
    }
}
PMIX_CLASS_INSTANCE(pmix_pset_t,
                    pmix_list_item_t,
                    pscon, psdes);
