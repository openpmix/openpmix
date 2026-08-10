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
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
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
#include "src/runtime/pmix_rte.h"

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

#include "src/class/pmix_hotel.h"
#include "src/class/pmix_list.h"
#include "src/common/pmix_attributes.h"
#include "src/common/pmix_iof.h"
#include "src/common/pmix_pfexec.h"
#include "src/common/pmix_monitor.h"
#include "src/hwloc/pmix_hwloc.h"
#include "src/mca/bfrops/base/base.h"
#include "src/mca/gds/base/base.h"
#include "src/mca/plog/base/base.h"
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
    .tool_connected2 = NULL,
    .log = NULL,
    .log2 = NULL,
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

static void abcbfn(pmix_status_t status, void *cbdata)
{
    pmix_setup_caddy_t *cd = (pmix_setup_caddy_t*)cbdata;

    if (NULL != cd->opcbfunc) {
        cd->opcbfunc(status, cd->cbdata);
    }
    /* the abort message was unpacked into the caddy, and the caddy's
     * destructor does not free that member - it is borrowed as often
     * as it is owned */
    if (NULL != cd->nspace) {
        free(cd->nspace);
    }
    PMIX_RELEASE(cd);
}

pmix_status_t pmix_server_abort(pmix_peer_t *peer, pmix_buffer_t *buf,
                                pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    pmix_setup_caddy_t *cd;
    int32_t cnt;
    pmix_status_t rc;
    pmix_proc_t proc;

    pmix_output_verbose(2, pmix_server_globals.base_output,
                        "recvd ABORT");

    cd = PMIX_NEW(pmix_setup_caddy_t);
    if (NULL == cd) {
        return PMIX_ERR_NOMEM;
    }
    cd->opcbfunc = cbfunc;
    cd->cbdata = cbdata;

    /* unpack the status */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, peer, buf, &cd->status, &cnt, PMIX_STATUS);
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(cd);
        return rc;
    }
    /* unpack the message - this allocates, and from here on every path
     * that discards the caddy owes it a free */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, peer, buf, &cd->nspace, &cnt, PMIX_STRING);
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(cd);
        return rc;
    }
    /* unpack the number of procs */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, peer, buf, &cd->nprocs, &cnt, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        goto error;
    }

    /* unpack any provided procs - these are the procs the caller
     * wants aborted */
    if (0 < cd->nprocs) {
        PMIX_PROC_CREATE(cd->procs, cd->nprocs);
        if (NULL == cd->procs) {
            rc = PMIX_ERR_NOMEM;
            goto error;
        }
        cnt = cd->nprocs;
        PMIX_BFROPS_UNPACK(rc, peer, buf, cd->procs, &cnt, PMIX_PROC);
        if (PMIX_SUCCESS != rc) {
            goto error;
        }
    }

    /* let the local host's server execute it */
    if (NULL == pmix_host_server.abort) {
        rc = PMIX_ERR_NOT_SUPPORTED;
        goto error;
    }
    pmix_strncpy(proc.nspace, peer->info->pname.nspace, PMIX_MAX_NSLEN);
    proc.rank = peer->info->pname.rank;
    rc = pmix_host_server.abort(&proc, peer->info->server_object, cd->status,
                                cd->nspace, cd->procs, cd->nprocs,
                                abcbfn, cd);
    if (PMIX_SUCCESS != rc) {
        goto error;
    }

    return rc;

error:
    if (NULL != cd->nspace) {
        free(cd->nspace);
    }
    PMIX_RELEASE(cd);
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
    /* screen the count before it sizes an allocation: PMIx_Info_create
     * computes "n * sizeof(pmix_info_t)" with no overflow guard and then
     * constructs every one of the n elements, so a count large enough to
     * wrap that product yields a short allocation whose constructor loop
     * runs straight off the end. This is the round-trip screen the
     * collective and IOF handlers carry for the same reason. */
    cnt = ninfo;
    if (0 > cnt || (size_t) cnt != ninfo) {
        rc = PMIX_ERR_BAD_PARAM;
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
    /* unpack the array of info objects - gate on the count that came off
     * the wire, not on the array size, which carries our own extra slot.
     * With no info objects there is nothing left in the buffer, so the
     * unpack the old gate always ran failed the whole publish. */
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
        rc = PMIx_Argv_append_nosize(&cd->keys, sptr);
        free(sptr);
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
        goto cleanup;
    }
    /* screen the count before it sizes an allocation - see the note in
     * pmix_server_publish */
    cnt = ninfo;
    if (0 > cnt || (size_t) cnt != ninfo) {
        rc = PMIX_ERR_BAD_PARAM;
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
        rc = PMIx_Argv_append_nosize(&cd->keys, sptr);
        free(sptr);
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
        goto cleanup;
    }
    /* screen the count before it sizes an allocation - see the note in
     * pmix_server_publish */
    cnt = ninfo;
    if (0 > cnt || (size_t) cnt != ninfo) {
        rc = PMIX_ERR_BAD_PARAM;
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
    pmix_server_caddy_t *scd;

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        /* the switchyard's caddy is parked on cbdata and scaddes does not
         * reach it, so releasing only this caddy strands it and the
         * retain it holds on the requesting peer. _spcbfunc, the arm that
         * would otherwise hand it to the spawn completion, never runs. */
        scd = (pmix_server_caddy_t *) cd->cbdata;
        if (NULL != scd) {
            PMIX_RELEASE(scd);
        }
        PMIX_RELEASE(cd);
        return;
    }

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

/* Completion callback for a spawn we fork/exec'd ourselves. pfexec calls
 * this with the caddy IT owns (and releases on return), so all it does is
 * hand the result to the normal server completion path, which owns the
 * caddy holding the requester's callback and does the IOF registration
 * the requester asked for. */
static void pfexec_spcbfunc(pmix_status_t status, char nspace[], void *cbdata)
{
    pmix_setup_caddy_t *cd = (pmix_setup_caddy_t *) cbdata;

    pmix_server_spcbfunc(status, nspace, cd);
}

pmix_status_t pmix_server_spawn(pmix_peer_t *peer, pmix_buffer_t *buf,
                                pmix_spawn_cbfunc_t cbfunc,
                                void *cbdata)
{
    pmix_setup_caddy_t *cd, *fcd;
    int32_t cnt;
    pmix_status_t rc;
    pmix_proc_t proc;

    pmix_output_verbose(2, pmix_server_globals.spawn_output,
                        "recvd SPAWN from %s",
                        PMIX_PNAME_PRINT(&peer->info->pname));

    /* Our host is the first choice. Failing that we can still service the
     * request if we are able to fork/exec it ourselves: a launcher that
     * has no server attached is expected to launch locally rather than
     * refuse (the same rule PMIx_Spawn applies to a launcher's own
     * requests). pfexec is opened only by a launcher or scheduler tool,
     * so this flag is exactly the "can I fork/exec" question - a plain
     * PMIx server never has it set and its behavior is unchanged. */
    if (NULL == pmix_host_server.spawn && !pmix_pfexec_globals.initialized) {
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
    /* screen the count before it sizes an allocation - see the note in
     * pmix_server_publish. The parser below also walks this size_t rather
     * than the int32_t the unpack consumed */
    cnt = cd->ninfo;
    if (0 > cnt || (size_t) cnt != cd->ninfo) {
        rc = PMIX_ERR_BAD_PARAM;
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
    /* the same screen: PMIx_App_create multiplies and constructs exactly
     * as PMIx_Info_create does, and scaddes walks this size_t when it
     * frees the array */
    cnt = cd->napps;
    if (0 > cnt || (size_t) cnt != cd->napps) {
        rc = PMIX_ERR_BAD_PARAM;
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

    if (NULL != pmix_host_server.spawn) {
        /* call the local server */
        PMIX_LOAD_PROCID(&proc, peer->info->pname.nspace, peer->info->pname.rank);
        rc = pmix_host_server.spawn(&proc, cd->info, cd->ninfo,
                                    cd->apps, cd->napps,
                                    pmix_server_spcbfunc, cd);
    } else {
        /* No host to ask, so fork/exec it ourselves. pfexec takes a caddy
         * of its own and releases it when the spawn completes, so give it
         * a second one that BORROWS this one's apps and directives -
         * "copied" stays false on it so its destructor leaves them to
         * "cd", which owns them and outlives it. */
        fcd = PMIX_NEW(pmix_setup_caddy_t);
        if (NULL == fcd) {
            rc = PMIX_ERR_NOMEM;
            goto cleanup;
        }
        fcd->info = cd->info;
        fcd->ninfo = cd->ninfo;
        fcd->apps = cd->apps;
        fcd->napps = cd->napps;
        fcd->spcbfunc = pfexec_spcbfunc;
        fcd->cbdata = cd;
        rc = pmix_pfexec_base_spawn_job(fcd);
        if (PMIX_SUCCESS != rc) {
            PMIX_RELEASE(fcd);
        }
    }

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
