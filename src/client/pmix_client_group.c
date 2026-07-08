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
 * Copyright (c) 2024      Triad National Security, LLC. All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "src/include/pmix_config.h"

#include "src/include/pmix_stdint.h"

#include "include/pmix.h"

#include "src/include/pmix_globals.h"
#include "src/mca/gds/base/base.h"

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
#include "src/mca/bfrops/bfrops.h"
#include "src/mca/gds/gds.h"
#include "src/mca/ptl/ptl.h"
#include "src/threads/pmix_threads.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_error.h"
#include "src/util/pmix_output.h"
#include "src/util/pmix_printf.h"

#include "src/server/pmix_server_ops.h"
#include "src/client/pmix_client_ops.h"

/* define a tracking object for group operations */
typedef struct {
    pmix_object_t super;
    pmix_lock_t lock;
    pmix_status_t status;
    size_t ref;
    char *grpid;
    pmix_proc_t *members;
    size_t nmembers;
    /* for the invite/join model: the concrete (wildcard-expanded) list of
     * invited processes and two parallel per-member flags. "answered" marks
     * that a member has responded definitively (accepted OR declined/
     * terminated); the invitation resolves once every member has answered, or
     * the timeout fires. "responded" marks the members that specifically
     * ACCEPTED - those form the group; every other member (a decliner, a
     * terminated proc, or, on timeout, a non-responder) is reported to the
     * leader via PMIX_GROUP_INVITE_FAILED and excluded. The timer bounds how
     * long the leader waits, and is armed only when PMIX_TIMEOUT is provided. */
    bool *responded;
    bool *answered;
    size_t nanswered;
    pmix_event_t ev;
    bool timer_active;
    bool completed;
    pmix_info_t *info;
    size_t ninfo;
    pmix_info_t *results;
    size_t nresults;
    pmix_op_cbfunc_t opcbfunc;
    pmix_info_cbfunc_t cbfunc;
    void *cbdata;
} pmix_group_tracker_t;

static void gtcon(pmix_group_tracker_t *p)
{
    PMIX_CONSTRUCT_LOCK(&p->lock);
    p->status = PMIX_SUCCESS;
    p->ref = SIZE_MAX;
    p->grpid = NULL;
    p->members = NULL;
    p->nmembers = 0;
    p->responded = NULL;
    p->answered = NULL;
    p->nanswered = 0;
    p->timer_active = false;
    p->completed = false;
    p->info = NULL;
    p->ninfo = 0;
    p->results = NULL;
    p->nresults = 0;
    p->cbfunc = NULL;
    p->opcbfunc = NULL;
    p->cbdata = NULL;
}
static void gtdes(pmix_group_tracker_t *p)
{
    PMIX_DESTRUCT_LOCK(&p->lock);
    if (NULL != p->members) {
        PMIX_PROC_FREE(p->members, p->nmembers);
    }
    if (NULL != p->responded) {
        free(p->responded);
        p->responded = NULL;
    }
    if (NULL != p->answered) {
        free(p->answered);
        p->answered = NULL;
    }
    if (NULL != p->info) {
        PMIX_INFO_FREE(p->info, p->ninfo);
    }
    if (NULL != p->grpid)
    {
        free(p->grpid);
        p->grpid = NULL;
    }
}
PMIX_CLASS_INSTANCE(pmix_group_tracker_t, pmix_object_t, gtcon, gtdes);

/* callback for wait completion */
static void construct_cbfunc(struct pmix_peer_t *pr,
                             pmix_ptl_hdr_t *hdr,
                             pmix_buffer_t *buf,
                             void *cbdata);
static void destruct_cbfunc(struct pmix_peer_t *pr,
                            pmix_ptl_hdr_t *hdr,
                            pmix_buffer_t *buf,
                            void *cbdata);
static void op_cbfunc(pmix_status_t status, void *cbdata);
static void op_cbfunc_rel(pmix_status_t status, void *cbdata);
static void invite_timeout(int sd, short args, void *cbdata);
static void invite_wake(pmix_group_tracker_t *cb, pmix_status_t status);

static void info_cbfunc(pmix_status_t status, pmix_info_t *info, size_t ninfo, void *cbdata,
                        pmix_release_cbfunc_t release_fn, void *release_cbdata);
static pmix_status_t add_group(const char *grpid,
                               size_t ctxid,
                               pmix_proc_t *members, size_t nmembers);

static pmix_status_t get_endpts(pmix_info_t *xfer,
                                pmix_scope_t scope,
                                bool *endpts)
{
    pmix_cb_t cb2;
    bool found;
    pmix_kval_t *kv;
    void *ilist;
    pmix_status_t rc;
    pmix_data_array_t darray;

    PMIX_CONSTRUCT(&cb2, pmix_cb_t);
    cb2.proc = &pmix_globals.myid;
    cb2.scope = scope;
    cb2.copy = true;
    PMIX_GDS_FETCH_KV(rc, pmix_globals.mypeer, &cb2);
    if (PMIX_SUCCESS == rc) {
        ilist = PMIx_Info_list_start();
        // start with our procID
        PMIx_Info_list_add(ilist, PMIX_PROCID, &pmix_globals.myid, PMIX_PROC);
        // add the scope
        PMIx_Info_list_add(ilist, PMIX_DATA_SCOPE, &cb2.scope, PMIX_SCOPE);
        // now add the kvals
        found = false;
        PMIX_LIST_FOREACH (kv, &cb2.kvs, pmix_kval_t) {
            if (PMIx_Check_reserved_key(kv->key)) {
                continue;
            }
            PMIx_Info_list_add_value_unique(ilist, kv->key, kv->value, true);
            found = true;
        }
        if (found) {
            // convert to array
            rc = PMIx_Info_list_convert(ilist, &darray);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIx_Info_list_release(ilist);
                return rc;
            }
            // insert into a pmix_info_t for packing
            PMIX_INFO_LOAD(xfer, PMIX_PROC_DATA, &darray, PMIX_DATA_ARRAY);
            PMIX_DATA_ARRAY_DESTRUCT(&darray);
            *endpts = true;
        }
        PMIx_Info_list_release(ilist);
    }
    PMIX_DESTRUCT(&cb2);
    return PMIX_SUCCESS;
}

static pmix_status_t construct_msg(pmix_buffer_t *msg,
                                   const char *grp,
                                   const pmix_proc_t *procs, size_t nprocs,
                                   const pmix_info_t *info, size_t ninfo)
{
    pmix_status_t rc;
    pmix_cmd_t cmd = PMIX_GROUP_CONSTRUCT_CMD;
    pmix_info_t local_endpts, *icopy, *iarray, *iptr;
    size_t sz, n, m, niarray;
    bool lclendpts;

    /* pack the cmd */
    PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, msg, &cmd, 1, PMIX_COMMAND);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }

    /* pack the group ID */
    PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, msg, &grp, 1, PMIX_STRING);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }

    /* pack the number of procs */
    PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, msg, &nprocs, 1, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    if (0 < nprocs) {
        PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, msg, procs, nprocs, PMIX_PROC);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            return rc;
        }
    }

    /* get our endpt info, if some was posted. We use
     * "remote" scope as all local procs have access
     * to info posted by all other local procs, regardless
     * of their namespace */
    sz = ninfo;
    lclendpts = false;
    rc = get_endpts(&local_endpts, PMIX_REMOTE, &lclendpts);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    if (lclendpts) {
        sz = sz + 1;
    }
    PMIX_INFO_CREATE(icopy, sz);

    // check for group info
    for (n=0; n < ninfo; n++) {
        if (PMIX_CHECK_KEY(&info[n], PMIX_GROUP_INFO)) {
            iarray = (pmix_info_t*)info[n].value.data.darray->array;
            niarray = info[n].value.data.darray->size;
            // check if the first entry is our procID
            if (PMIX_PROC != iarray[0].value.type) {
                // we need to add our ID to the beginning of the array
                PMIX_INFO_CREATE(iptr, niarray+1);
                PMIX_INFO_LOAD(&iptr[0], PMIX_PROCID, &pmix_globals.myid, PMIX_PROC);
                for (m=0; m < niarray; m++) {
                    PMIX_INFO_XFER(&iptr[m+1], &iarray[m]);
                }
                PMIx_Load_key(icopy[n].key, PMIX_GROUP_INFO);
                icopy[n].value.type = PMIX_DATA_ARRAY;
                icopy[n].value.data.darray = (pmix_data_array_t*)pmix_malloc(sizeof(pmix_data_array_t));
                icopy[n].value.data.darray->type = PMIX_INFO;
                icopy[n].value.data.darray->array = iptr;
                icopy[n].value.data.darray->size = niarray + 1;
            } else {
                PMIX_INFO_XFER(&icopy[n], &info[n]);
            }
        } else {
            PMIX_INFO_XFER(&icopy[n], &info[n]);
        }
    }
    if (lclendpts) {
        // add the local endpt data
        PMIX_INFO_XFER(&icopy[n], &local_endpts);
        PMIX_INFO_DESTRUCT(&local_endpts);
        ++n;
    }

    /* pack the info structs */
    PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, msg, &sz, 1, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_INFO_FREE(icopy, sz);
        return rc;
    }
    if (0 < sz) {
        PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, msg, icopy, sz, PMIX_INFO);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            return rc;
        }
        PMIX_INFO_FREE(icopy, sz);
    }

    return rc;
}

PMIX_EXPORT pmix_status_t PMIx_Group_construct(const char grp[], const pmix_proc_t procs[],
                                               size_t nprocs, const pmix_info_t info[],
                                               size_t ninfo, pmix_info_t **results,
                                               size_t *nresults)
{
    pmix_status_t rc;
    pmix_group_tracker_t *cb;

    pmix_output_verbose(2, pmix_client_globals.group_output,
                        "pmix: group_construct called");

    if (!pmix_atomic_check_bool(&pmix_globals.initialized)) {
        return PMIX_ERR_INIT;
    }

    /* if we aren't connected, don't attempt to send */
    if (!pmix_atomic_check_bool(&pmix_globals.connected)) {
        return PMIX_ERR_UNREACH;
    }

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        return PMIX_ERR_NOT_AVAILABLE;
    }

    /* create a callback object as we need to pass it to the
     * recv routine so we know which callback to use when
     * the return message is recvd */
    cb = PMIX_NEW(pmix_group_tracker_t);

    /* push the message into our event base to send to the server */
    rc = PMIx_Group_construct_nb(grp, procs, nprocs, info, ninfo, info_cbfunc, cb);
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(cb);
        return rc;
    }

    /* wait for the group construct to complete */
    PMIX_WAIT_THREAD(&cb->lock);
    rc = cb->status;
    if (PMIX_SUCCESS == rc) {
        /* user takes responsibility for releasing any results */
        *results = cb->results;
        *nresults = cb->nresults;
        cb->results = NULL;
        cb->nresults = 0;
    }
    PMIX_RELEASE(cb);

    return rc;
}

PMIX_EXPORT pmix_status_t PMIx_Group_construct_nb(const char grp[], const pmix_proc_t procs[],
                                                  size_t nprocs, const pmix_info_t info[],
                                                  size_t ninfo, pmix_info_cbfunc_t cbfunc,
                                                  void *cbdata)
{
    pmix_buffer_t *msg = NULL;
    pmix_status_t rc;
    pmix_group_tracker_t *cb = NULL;
    pmix_proc_t *rgs = NULL;
    size_t nrg = 0, n;
    bool partial = false;

    pmix_output_verbose(2, pmix_client_globals.group_output,
                        "pmix:group_construct_nb called");

    if (!pmix_atomic_check_bool(&pmix_globals.initialized)) {
        return PMIX_ERR_INIT;
    }

    /* if we aren't connected, don't attempt to send */
    if (!pmix_atomic_check_bool(&pmix_globals.connected)) {
        return PMIX_ERR_UNREACH;
    }

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        return PMIX_ERR_NOT_AVAILABLE;
    }

    /* An "add members" or "bootstrap" construct does not list all
     * participants in the procs array - the remaining members are
     * supplied separately (via PMIX_GROUP_ADD_MEMBERS) or join later,
     * and an individual caller (e.g., a non-bootstrap participant that
     * passes a NULL procs array) may not appear in its own procs array.
     * We therefore cannot validate membership for those operations. */
    for (n = 0; n < ninfo; n++) {
        if (PMIX_CHECK_KEY(&info[n], PMIX_GROUP_ADD_MEMBERS) ||
            PMIX_CHECK_KEY(&info[n], PMIX_GROUP_BOOTSTRAP)) {
            partial = true;
            break;
        }
    }

    if (!partial && NULL != procs && 0 < nprocs) {
        /* every participant must be listed in the procs array, so
         * replace any PMIx group reference with its actual member
         * proc(s) and verify that the caller is among them */
        rc = pmix_client_convert_group_procs(procs, nprocs, &rgs, &nrg);
        if (PMIX_SUCCESS != rc) {
            return rc;
        }
        if (!pmix_client_proc_is_included(rgs, nrg)) {
            PMIX_PROC_FREE(rgs, nrg);
            return PMIX_ERR_NOT_A_MEMBER;
        }
    }

    // send any data to our server
    msg = PMIX_NEW(pmix_buffer_t);
    if (NULL != rgs) {
        rc = construct_msg(msg, grp, rgs, nrg, info, ninfo);
        PMIX_PROC_FREE(rgs, nrg);
    } else {
        rc = construct_msg(msg, grp, procs, nprocs, info, ninfo);
    }
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(msg);
        return rc;
    }

    /* create a callback object as we need to pass it to the
     * recv routine so we know which callback to use when
     * the return message is recvd */
    cb = PMIX_NEW(pmix_group_tracker_t);
    cb->cbfunc = cbfunc;
    cb->cbdata = cbdata;
    cb->grpid = strdup(grp);

    /* push the message into our event base to send to the server */
    PMIX_PTL_SEND_RECV(rc, pmix_client_globals.myserver, msg, construct_cbfunc, (void*)cb);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(cb);
        PMIX_RELEASE(msg);
    }

    return rc;
}

PMIX_EXPORT pmix_status_t PMIx_Group_destruct(const char grp[],
                                              const pmix_info_t info[],
                                              size_t ninfo)
{
    pmix_status_t rc;
    pmix_group_tracker_t cb;

    pmix_output_verbose(2, pmix_client_globals.group_output,
                        "pmix: group_destruct called");

    if (!pmix_atomic_check_bool(&pmix_globals.initialized)) {
        return PMIX_ERR_INIT;
    }

    /* if we aren't connected, don't attempt to send */
    if (!pmix_atomic_check_bool(&pmix_globals.connected)) {
        return PMIX_ERR_UNREACH;
    }

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        return PMIX_ERR_NOT_AVAILABLE;
    }

    /* create a callback object as we need to pass it to the
     * recv routine so we know which callback to use when
     * the return message is recvd */
    PMIX_CONSTRUCT(&cb, pmix_group_tracker_t);

    /* use the non-blocking version */
    if (PMIX_SUCCESS != (rc = PMIx_Group_destruct_nb(grp, info, ninfo, op_cbfunc, (void *) &cb))) {
        PMIX_ERROR_LOG(rc);
        PMIX_DESTRUCT(&cb);
        return rc;
    }

    /* wait for the destruct to complete */
    PMIX_WAIT_THREAD(&cb.lock);
    rc = cb.status;
    PMIX_DESTRUCT(&cb);

    pmix_output_verbose(2, pmix_client_globals.group_output,
                        "pmix: group destruct completed");

    return rc;
}

PMIX_EXPORT pmix_status_t PMIx_Group_destruct_nb(const char grpid[], const pmix_info_t info[],
                                                 size_t ninfo, pmix_op_cbfunc_t cbfunc,
                                                 void *cbdata)
{
    pmix_buffer_t *msg = NULL;
    pmix_cmd_t cmd = PMIX_GROUP_DESTRUCT_CMD;
    pmix_status_t rc;
    pmix_group_tracker_t *cb = NULL;
    pmix_group_t *grp, *pgrp;

    pmix_output_verbose(2, pmix_client_globals.group_output,
                        "pmix:group_destruct_nb called");

    if (!pmix_atomic_check_bool(&pmix_globals.initialized)) {
        return PMIX_ERR_INIT;
    }

    /* if we aren't connected, don't attempt to send */
    if (!pmix_atomic_check_bool(&pmix_globals.connected)) {
        return PMIX_ERR_UNREACH;
    }

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        return PMIX_ERR_NOT_AVAILABLE;
    }

    /* check for bozo input */
    if (NULL == grpid) {
        return PMIX_ERR_BAD_PARAM;
    }

    /* find this group */
    grp = NULL;
    PMIX_LIST_FOREACH(pgrp, &pmix_client_globals.groups, pmix_group_t) {
        if (0 == strcmp(grpid, pgrp->grpid)) {
            grp = pgrp;
            break;
        }
    }
    if (NULL == grp) {
        return PMIX_ERR_NOT_FOUND;
    }

    msg = PMIX_NEW(pmix_buffer_t);
    /* pack the cmd */
    PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, msg, &cmd, 1, PMIX_COMMAND);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto done;
    }

    /* pack the group ID */
    PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, msg, &grpid, 1, PMIX_STRING);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto done;
    }

    /* pack the membership - the server isn't storing it,
     * so we have to send it so that the server can
     * track when all local procs have participated */
    PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, msg, &grp->nmbrs, 1, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto done;
    }
    PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, msg, grp->members, grp->nmbrs, PMIX_PROC);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto done;
    }

    /* pack the info structs */
    PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, msg, &ninfo, 1, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(msg);
        goto done;
    }
    if (0 < ninfo) {
        PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, msg, info, ninfo, PMIX_INFO);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_RELEASE(msg);
            goto done;
        }
    }

    /* create a callback object as we need to pass it to the
     * recv routine so we know which callback to use when
     * the return message is recvd */
    cb = PMIX_NEW(pmix_group_tracker_t);
    cb->opcbfunc = cbfunc;
    cb->cbdata = cbdata;
    cb->grpid  = strdup(grpid);

    /* push the message into our event base to send to the server */
    PMIX_PTL_SEND_RECV(rc, pmix_client_globals.myserver, msg, destruct_cbfunc, (void *) cb);
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(cb);
    }

done:
    if (PMIX_SUCCESS != rc && NULL != msg) {
        PMIX_RELEASE(msg);
    }
    return rc;
}

static void invite_handler(size_t evhdlr_registration_id, pmix_status_t status,
                           const pmix_proc_t *source, pmix_info_t info[], size_t ninfo,
                           pmix_info_t *results, size_t nresults,
                           pmix_event_notification_cbfunc_fn_t cbfunc, void *cbdata)
{
    pmix_group_tracker_t *cb = NULL;
    const pmix_proc_t *responder = source;
    size_t n;
    PMIX_HIDE_UNUSED_PARAMS(evhdlr_registration_id, results, nresults);

    /* find the tracker we asked to be returned with the event, and the identity
     * of the responding proc. An accept/decline names itself as the event
     * source; a termination names the departed proc as the affected proc. */
    for (n = 0; n < ninfo; n++) {
        if (PMIX_CHECK_KEY(&info[n], PMIX_EVENT_RETURN_OBJECT)) {
            cb = (pmix_group_tracker_t *) info[n].value.data.ptr;
        } else if (PMIX_CHECK_KEY(&info[n], PMIX_EVENT_AFFECTED_PROC)) {
            responder = info[n].value.data.proc;
        }
    }
    if (NULL == cb) {
        pmix_output(0, "%s: INVITE HANDLER NULL OBJECT", PMIX_NAME_PRINT(&pmix_globals.myid));
        /* always must continue the chain */
        cbfunc(PMIX_EVENT_ACTION_COMPLETE, NULL, 0, NULL, NULL, cbdata);
        return;
    }

    /* Record this response by identity. A member that ACCEPTS joins the group
     * (responded); one that DECLINES or TERMINATES definitively will not, and
     * is left out of "responded" so it is reported to the leader via
     * PMIX_GROUP_INVITE_FAILED and excluded when the invitation resolves. Either
     * way the member has now "answered", so a decline resolves the construct
     * immediately rather than waiting out the timeout. The notifications and the
     * completion broadcast are issued by PMIx_Group_invite on its own thread -
     * PMIx_Notify_event must not be called from this progress thread, where it
     * would deadlock. */
    for (n = 0; n < cb->nmembers; n++) {
        if (PMIX_CHECK_PROCID(&cb->members[n], responder)) {
            if (!cb->answered[n]) {
                cb->answered[n] = true;
                cb->nanswered++;
                if (PMIX_GROUP_INVITE_ACCEPTED == status) {
                    cb->responded[n] = true;
                }
            }
            break;
        }
    }

    /* once every invited member has answered, the invitation has resolved */
    if (cb->nanswered == cb->nmembers) {
        invite_wake(cb, PMIX_SUCCESS);
    }

    /* always must continue the chain */
    cbfunc(PMIX_EVENT_ACTION_COMPLETE, NULL, 0, NULL, NULL, cbdata);
    return;
}

static void regcbfunc(pmix_status_t status, size_t refid, void *cbdata)
{
    pmix_group_tracker_t *cb = (pmix_group_tracker_t *) cbdata;

    cb->status = status;
    cb->ref = refid;
    PMIX_WAKEUP_THREAD(&cb->lock);
}

/* fetch the number of processes in the given proc's namespace so a
 * PMIX_RANK_WILDCARD entry in an invitation can be expanded into concrete
 * ranks */
static pmix_status_t invite_job_size(const pmix_proc_t *proc, uint32_t *jsize)
{
    pmix_cb_t cb2;
    pmix_info_t optional;
    pmix_kval_t *kv;
    pmix_status_t rc;

    PMIX_CONSTRUCT(&cb2, pmix_cb_t);
    PMIX_INFO_LOAD(&optional, PMIX_OPTIONAL, NULL, PMIX_BOOL);
    cb2.proc = (pmix_proc_t *) proc;
    cb2.key = PMIX_JOB_SIZE;
    cb2.info = &optional;
    cb2.ninfo = 1;
    PMIX_GDS_FETCH_KV(rc, pmix_globals.mypeer, &cb2);
    if (PMIX_SUCCESS != rc && PMIX_OPERATION_SUCCEEDED != rc) {
        PMIX_DESTRUCT(&cb2);
        return PMIX_ERR_BAD_PARAM;
    }
    kv = (pmix_kval_t *) pmix_list_remove_first(&cb2.kvs);
    PMIX_DESTRUCT(&cb2);
    if (NULL == kv) { // should never be NULL
        return PMIX_ERR_BAD_PARAM;
    }
    rc = PMIx_Value_get_number(kv->value, jsize, PMIX_UINT32);
    PMIX_RELEASE(kv);
    return rc;
}

/* An invitation has resolved - either every invitee answered, or the timeout
 * fired. Cancel the timer (if still pending) and wake the operation. The
 * notifications (PMIX_GROUP_INVITE_FAILED for any non-responder and the
 * PMIX_GROUP_CONSTRUCT_COMPLETE broadcast) are deliberately NOT done here:
 * this runs on the progress thread, where PMIx_Notify_event would deadlock.
 * The blocking PMIx_Group_invite issues them on its own thread after it wakes;
 * the non-blocking form hands the result to the caller's callback. Guarded so
 * a late (post-timeout) acceptance cannot resolve the invite twice. */
static void invite_wake(pmix_group_tracker_t *cb, pmix_status_t status)
{
    if (cb->completed) {
        return;
    }
    cb->completed = true;
    if (cb->timer_active) {
        pmix_event_del(&cb->ev);
        cb->timer_active = false;
    }
    cb->status = status;
    if (NULL != cb->cbfunc) {
        /* non-blocking form: deliver the result to the caller's callback */
        cb->cbfunc(status, NULL, 0, cb->cbdata, NULL, NULL);
    }
    /* wake the blocking PMIx_Group_invite (if any) */
    PMIX_WAKEUP_THREAD(&cb->lock);
}

/* Timeout handler: some invitees did not respond within the caller-provided
 * PMIX_TIMEOUT. Resolve the invitation on whoever did accept - PMIx_Group_invite
 * reports the non-responders once it wakes. */
static void invite_timeout(int sd, short args, void *cbdata)
{
    pmix_group_tracker_t *cb = (pmix_group_tracker_t *) cbdata;
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    /* the timer has fired, so it is no longer pending */
    cb->timer_active = false;
    invite_wake(cb, PMIX_SUCCESS);
}

/* Perform the shared setup for an invitation on a caller-provided tracker
 * (with cbfunc/cbdata already set for the non-blocking form): build the
 * concrete membership, register the response handler, notify the invitees, and
 * arm the optional timeout timer. Runs on the caller's thread. On error the
 * caller releases the tracker. */
static pmix_status_t invite_setup(pmix_group_tracker_t *cb, const char *grp,
                                  const pmix_proc_t *procs, size_t nprocs,
                                  const pmix_info_t *info, size_t ninfo)
{
    pmix_group_tracker_t lock;
    pmix_status_t codes[] = {
        PMIX_GROUP_INVITE_ACCEPTED,
        PMIX_GROUP_INVITE_DECLINED,
        PMIX_PROC_TERMINATED
    };
    size_t ncodes, n, m;
    pmix_info_t myinfo[2];
    pmix_status_t rc;
    uint32_t jsize, j;
    int timeout = 0;
    struct timeval tv;

    cb->grpid = strdup(grp);
    cb->nanswered = 1; // we have obviously answered (by accepting)

    /* compute the number of proposed members, expanding any wildcard ranks */
    for (n = 0; n < nprocs; n++) {
        if (PMIX_RANK_WILDCARD == procs[n].rank) {
            rc = invite_job_size(&procs[n], &jsize);
            if (PMIX_SUCCESS != rc) {
                return rc;
            }
            cb->nmembers += jsize;
        } else {
            cb->nmembers++;
        }
    }

    /* build the concrete (wildcard-expanded) list of invited members plus the
     * parallel per-member "answered"/"responded" flags. We track identities -
     * not just a count - so that we can name the non-accepters (decliners,
     * terminated procs, and, on a timeout, non-responders) in the
     * PMIX_GROUP_INVITE_FAILED events and construct the group on the members
     * that did accept. */
    PMIX_PROC_CREATE(cb->members, cb->nmembers);
    if (NULL == cb->members) {
        return PMIX_ERR_NOMEM;
    }
    cb->responded = (bool *) calloc(cb->nmembers, sizeof(bool));
    if (NULL == cb->responded) {
        return PMIX_ERR_NOMEM;
    }
    cb->answered = (bool *) calloc(cb->nmembers, sizeof(bool));
    if (NULL == cb->answered) {
        return PMIX_ERR_NOMEM;
    }
    m = 0;
    for (n = 0; n < nprocs; n++) {
        if (PMIX_RANK_WILDCARD == procs[n].rank) {
            rc = invite_job_size(&procs[n], &jsize);
            if (PMIX_SUCCESS != rc) {
                return rc;
            }
            for (j = 0; j < jsize; j++) {
                PMIX_LOAD_PROCID(&cb->members[m], procs[n].nspace, j);
                m++;
            }
        } else {
            PMIX_LOAD_PROCID(&cb->members[m], procs[n].nspace, procs[n].rank);
            m++;
        }
    }
    /* we (the leader) are a member and have obviously already answered by
     * accepting (this matches the nanswered = 1 seed above) */
    for (m = 0; m < cb->nmembers; m++) {
        if (PMIX_CHECK_PROCID(&cb->members[m], &pmix_globals.myid)) {
            cb->responded[m] = true;
            cb->answered[m] = true;
            break;
        }
    }

    /* register an event handler specifically to respond to accept responses */
    PMIX_INFO_LOAD(&myinfo[0], PMIX_EVENT_RETURN_OBJECT, cb, PMIX_POINTER);
    PMIX_INFO_LOAD(&myinfo[1], PMIX_EVENT_HDLR_PREPEND, NULL, PMIX_BOOL);
    ncodes = sizeof(codes) / sizeof(pmix_status_t);
    PMIX_CONSTRUCT(&lock, pmix_group_tracker_t);
    PMIx_Register_event_handler(codes, ncodes, myinfo, 2, invite_handler, regcbfunc, &lock);
    PMIX_WAIT_THREAD(&lock.lock);
    rc = lock.status;
    cb->ref = lock.ref;
    PMIX_DESTRUCT(&lock);
    PMIX_INFO_DESTRUCT(&myinfo[0]);
    PMIX_INFO_DESTRUCT(&myinfo[1]);
    if (PMIX_SUCCESS != rc) {
        return rc;
    }

    /* check for directives - a timeout bounds how long we wait for the
     * invitees to respond */
    if (NULL != info) {
        for (n = 0; n < ninfo; n++) {
            if (PMIX_CHECK_KEY(&info[n], PMIX_TIMEOUT)) {
                rc = PMIx_Value_get_number(&info[n].value, &timeout, PMIX_INT);
                if (PMIX_SUCCESS != rc) {
                    timeout = 0;
                }
                break;
            }
        }
    }

    /* limit the range to just the procs we are inviting */
    PMIX_INFO_CREATE(cb->info, 3);
    if (NULL == cb->info) {
        return PMIX_ERR_NOMEM;
    }
    cb->ninfo = 3;
    n = 0;
    (void) strncpy(cb->info[n].key, PMIX_EVENT_CUSTOM_RANGE, PMIX_MAX_KEYLEN);
    cb->info[n].value.type = PMIX_DATA_ARRAY;
    PMIX_DATA_ARRAY_CREATE(cb->info[n].value.data.darray, nprocs, PMIX_PROC);
    if (NULL == cb->info[n].value.data.darray || NULL == cb->info[n].value.data.darray->array) {
        return PMIX_ERR_NOMEM;
    }
    memcpy(cb->info[n].value.data.darray->array, procs, nprocs * sizeof(pmix_proc_t));
    ++n;
    /* mark that this only goes to non-default handlers */
    PMIX_INFO_LOAD(&cb->info[n], PMIX_EVENT_NON_DEFAULT, NULL, PMIX_BOOL);
    ++n;
    /* provide the group ID */
    PMIX_INFO_LOAD(&cb->info[n], PMIX_GROUP_ID, grp, PMIX_STRING);

    /* notify everyone of the invitation */
    PMIX_CONSTRUCT(&lock, pmix_group_tracker_t);
    rc = PMIx_Notify_event(PMIX_GROUP_INVITED, &pmix_globals.myid, PMIX_RANGE_CUSTOM,
                           cb->info, cb->ninfo, op_cbfunc, (void *) &lock);
    PMIX_WAIT_THREAD(&lock.lock);
    rc = lock.status;
    PMIX_DESTRUCT(&lock);
    if (PMIX_SUCCESS != rc) {
        return rc;
    }

    /* if a timeout was requested, arm a timer so a non-responding invitee
     * cannot hang the leader indefinitely. The handler runs on the progress
     * thread; it is cancelled by invite_wake() once every invitee answers. */
    if (0 < timeout) {
        pmix_event_assign(&cb->ev, pmix_globals.evbase, -1, 0, invite_timeout, (void *) cb);
        cb->timer_active = true;
        tv.tv_sec = timeout;
        tv.tv_usec = 0;
        PMIX_POST_OBJECT(cb);
        pmix_event_add(&cb->ev, &tv);
    }

    return PMIX_SUCCESS;
}

/* Once an invitation has resolved, announce the outcome. Runs on the caller's
 * (non-progress) thread: report each non-responder to the leader via
 * PMIX_GROUP_INVITE_FAILED, then announce the group to the members that
 * accepted via PMIX_GROUP_CONSTRUCT_COMPLETE (only sent to members; servers
 * intercept it to update their membership lists). The membership is the full
 * invited list in the common case, or a reduced list if some invitees
 * declined, terminated, or timed out. */
static pmix_status_t invite_announce(pmix_group_tracker_t *cb)
{
    pmix_proc_t *members = NULL;
    size_t i, nmembers = 0;
    pmix_data_array_t darray;
    pmix_info_t finfo, cinfo[4];
    pmix_group_tracker_t lock;
    pmix_status_t rc = PMIX_SUCCESS;

    /* report each proc that did not accept the invitation */
    for (i = 0; i < cb->nmembers; i++) {
        if (cb->responded[i]) {
            continue;
        }
        PMIX_INFO_LOAD(&finfo, PMIX_EVENT_AFFECTED_PROC, &cb->members[i], PMIX_PROC);
        PMIX_CONSTRUCT(&lock, pmix_group_tracker_t);
        rc = PMIx_Notify_event(PMIX_GROUP_INVITE_FAILED, &pmix_globals.myid,
                               PMIX_RANGE_PROC_LOCAL, &finfo, 1, op_cbfunc, (void *) &lock);
        if (PMIX_SUCCESS == rc) {
            PMIX_WAIT_THREAD(&lock.lock);
        }
        PMIX_DESTRUCT(&lock);
        PMIX_INFO_DESTRUCT(&finfo);
    }

    /* build the final membership from the members that accepted */
    PMIX_PROC_CREATE(members, cb->nmembers);
    if (NULL == members) {
        return PMIX_ERR_NOMEM;
    }
    for (i = 0; i < cb->nmembers; i++) {
        if (cb->responded[i]) {
            PMIX_LOAD_PROCID(&members[nmembers], cb->members[i].nspace, cb->members[i].rank);
            ++nmembers;
        }
    }
    darray.type = PMIX_PROC;
    darray.array = members;
    darray.size = nmembers;
    // limit the range to, and report, the final membership
    PMIX_INFO_LOAD(&cinfo[0], PMIX_EVENT_CUSTOM_RANGE, &darray, PMIX_DATA_ARRAY);
    PMIX_INFO_LOAD(&cinfo[1], PMIX_GROUP_MEMBERSHIP, &darray, PMIX_DATA_ARRAY);
    /* this only goes to non-default handlers */
    PMIX_INFO_LOAD(&cinfo[2], PMIX_EVENT_NON_DEFAULT, NULL, PMIX_BOOL);
    PMIX_INFO_LOAD(&cinfo[3], PMIX_GROUP_ID, cb->grpid, PMIX_STRING);
    PMIX_CONSTRUCT(&lock, pmix_group_tracker_t);
    rc = PMIx_Notify_event(PMIX_GROUP_CONSTRUCT_COMPLETE, &pmix_globals.myid,
                           PMIX_RANGE_CUSTOM, cinfo, 4, op_cbfunc, (void *) &lock);
    if (PMIX_SUCCESS == rc) {
        PMIX_WAIT_THREAD(&lock.lock);
        rc = lock.status;
    }
    PMIX_DESTRUCT(&lock);
    PMIX_INFO_DESTRUCT(&cinfo[0]);
    PMIX_INFO_DESTRUCT(&cinfo[1]);
    PMIX_INFO_DESTRUCT(&cinfo[2]);
    PMIX_INFO_DESTRUCT(&cinfo[3]);
    PMIX_PROC_FREE(members, nmembers);
    return rc;
}

/* Deregister the invitation handler (if it was ever registered), cancel any
 * pending timer, and release the tracker. Safe to call whether or not
 * invite_setup got as far as registering, and used both to clean up a failed
 * setup and to tear down after a blocking invite completes. */
static void invite_teardown(pmix_group_tracker_t *cb)
{
    pmix_group_tracker_t lock;

    if (cb->timer_active) {
        pmix_event_del(&cb->ev);
        cb->timer_active = false;
    }
    if (SIZE_MAX != cb->ref) {
        PMIX_CONSTRUCT(&lock, pmix_group_tracker_t);
        PMIx_Deregister_event_handler(cb->ref, op_cbfunc, &lock);
        PMIX_WAIT_THREAD(&lock.lock);
        PMIX_DESTRUCT(&lock);
        cb->ref = SIZE_MAX;
    }
    PMIX_RELEASE(cb);
}

PMIX_EXPORT pmix_status_t PMIx_Group_invite(const char grp[], const pmix_proc_t procs[],
                                            size_t nprocs, const pmix_info_t info[], size_t ninfo,
                                            pmix_info_t **results, size_t *nresults)
{
    pmix_group_tracker_t *cb;
    pmix_status_t rc;

    if (!pmix_atomic_check_bool(&pmix_globals.initialized)) {
        return PMIX_ERR_INIT;
    }

    /* if we aren't connected, then we cannot notify */
    if (!pmix_atomic_check_bool(&pmix_globals.connected)) {
        return PMIX_ERR_UNREACH;
    }

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        return PMIX_ERR_NOT_AVAILABLE;
    }

    /* check for bozo input */
    if (NULL == grp || NULL == procs) {
        return PMIX_ERR_BAD_PARAM;
    }

    *results = NULL;
    *nresults = 0;

    cb = PMIX_NEW(pmix_group_tracker_t);
    if (NULL == cb) {
        return PMIX_ERR_NOMEM;
    }
    /* leave cb->cbfunc NULL: this is the blocking form, so we wait on the
     * tracker's lock rather than being handed the result via a callback */
    rc = invite_setup(cb, grp, procs, nprocs, info, ninfo);
    if (PMIX_SUCCESS != rc) {
        invite_teardown(cb);
        return rc;
    }

    /* wait for the invitation to resolve - every invitee answered (accept or
     * decline), or the timeout fired (invite_wake, on the progress thread, only
     * wakes us; it does not notify, as PMIx_Notify_event cannot be called from
     * that thread) */
    PMIX_WAIT_THREAD(&cb->lock);
    rc = cb->status;

    /* now on our own thread, announce the outcome to the group */
    if (PMIX_SUCCESS == rc) {
        rc = invite_announce(cb);
    }

    /* deregister the invitation handler now that the invite has resolved (so a
     * late response cannot fire it against the released tracker) and release
     * the tracker */
    invite_teardown(cb);
    return rc;
}

PMIX_EXPORT pmix_status_t PMIx_Group_invite_nb(const char grp[], const pmix_proc_t procs[],
                                               size_t nprocs, const pmix_info_t info[],
                                               size_t ninfo, pmix_info_cbfunc_t cbfunc,
                                               void *cbdata)
{
    pmix_group_tracker_t *cb;
    pmix_status_t rc;

    if (!pmix_atomic_check_bool(&pmix_globals.initialized)) {
        return PMIX_ERR_INIT;
    }

    /* if we aren't connected, then we cannot notify */
    if (!pmix_atomic_check_bool(&pmix_globals.connected)) {
        return PMIX_ERR_UNREACH;
    }

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        return PMIX_ERR_NOT_AVAILABLE;
    }

    /* check for bozo input */
    if (NULL == grp || NULL == procs) {
        return PMIX_ERR_BAD_PARAM;
    }

    cb = PMIX_NEW(pmix_group_tracker_t);
    if (NULL == cb) {
        return PMIX_ERR_NOMEM;
    }
    cb->cbfunc = cbfunc;
    cb->cbdata = cbdata;
    rc = invite_setup(cb, grp, procs, nprocs, info, ninfo);
    if (PMIX_SUCCESS != rc) {
        invite_teardown(cb);
    }
    return rc;
}

PMIX_EXPORT pmix_status_t PMIx_Group_join(const char grp[], const pmix_proc_t *leader,
                                          pmix_group_opt_t opt, const pmix_info_t info[],
                                          size_t ninfo, pmix_info_t **results, size_t *nresults)
{
    pmix_status_t rc;
    pmix_group_tracker_t *cb;
    PMIX_HIDE_UNUSED_PARAMS(results, nresults);

    if (!pmix_atomic_check_bool(&pmix_globals.initialized)) {
        return PMIX_ERR_INIT;
    }

    /* if we aren't connected, don't attempt to send */
    if (!pmix_atomic_check_bool(&pmix_globals.connected)) {
        return PMIX_ERR_UNREACH;
    }

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        return PMIX_ERR_NOT_AVAILABLE;
    }

    /* create a callback object as we need to pass it to the
     * recv routine so we know which lock to release when
     * the return message is recvd */
    cb = PMIX_NEW(pmix_group_tracker_t);

    rc = PMIx_Group_join_nb(grp, leader, opt, info, ninfo, info_cbfunc, cb);
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(cb);
        return rc;
    }

    /* wait for the group construction to complete */
    PMIX_WAIT_THREAD(&cb->lock);
    rc = cb->status;
    PMIX_RELEASE(cb);

    pmix_output_verbose(2, pmix_client_globals.group_output,
                        "pmix: group construction completed");

    return rc;
}

PMIX_EXPORT pmix_status_t PMIx_Group_join_nb(const char grp[], const pmix_proc_t *leader,
                                             pmix_group_opt_t opt, const pmix_info_t info[],
                                             size_t ninfo, pmix_info_cbfunc_t cbfunc, void *cbdata)
{
    pmix_status_t rc;
    pmix_group_tracker_t *cb;
    pmix_status_t code;
    size_t n;
    pmix_data_range_t range;
    PMIX_HIDE_UNUSED_PARAMS(grp, cbfunc);

    pmix_output_verbose(2, pmix_client_globals.group_output,
                        "[%s:%d] pmix: join nb called",
                        pmix_globals.myid.nspace, pmix_globals.myid.rank);

    if (!pmix_atomic_check_bool(&pmix_globals.initialized)) {
        return PMIX_ERR_INIT;
    }

    /* if we aren't connected, then we cannot notify */
    if (!pmix_atomic_check_bool(&pmix_globals.connected)) {
        return PMIX_ERR_UNREACH;
    }

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        return PMIX_ERR_NOT_AVAILABLE;
    }

    /* create a callback object as we need to pass it to the
     * recv routine so we know which lock to release when
     * the notification is done */
    cb = PMIX_NEW(pmix_group_tracker_t);
    cb->cbfunc = cbfunc;
    cb->cbdata = cbdata;

    /* check for directives */
    if (NULL != info) {
        for (n = 0; n < ninfo; n++) {
            if (PMIX_CHECK_KEY(&info[n], PMIX_TIMEOUT)) {
                /* setup a timer */
                break;
            }
        }
    }

    /* set the code according to their request */
    if (PMIX_GROUP_ACCEPT == opt) {
        code = PMIX_GROUP_INVITE_ACCEPTED;
    } else {
        code = PMIX_GROUP_INVITE_DECLINED;
    }

    /* only notify the leader so we don't hit all procs */
    if (NULL != leader) {
        range = PMIX_RANGE_CUSTOM;
        PMIX_INFO_CREATE(cb->info, 1);
        if (NULL == cb->info) {
            PMIX_RELEASE(cb);
            return PMIX_ERR_NOMEM;
        }
        PMIX_INFO_LOAD(&cb->info[0], PMIX_EVENT_CUSTOM_RANGE, leader, PMIX_PROC);
        cb->ninfo = 1;
    } else {
        range = PMIX_RANGE_SESSION;
    }

    rc = PMIx_Notify_event(code, &pmix_globals.myid, range,
                           cb->info, cb->ninfo, op_cbfunc_rel,
                           (void *) cb);
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(cb);
    }
    pmix_output_verbose(2, pmix_client_globals.group_output,
                        "[%s:%d] pmix: group invite %s",
                        pmix_globals.myid.nspace, pmix_globals.myid.rank,
                        (PMIX_GROUP_INVITE_ACCEPTED == code) ? "ACCEPTED" : "DECLINED");

    return rc;
}

PMIX_EXPORT pmix_status_t PMIx_Group_leave(const char grp[],
                                           const pmix_info_t info[], size_t ninfo)
{
    pmix_status_t rc;
    pmix_group_tracker_t cb;

    pmix_output_verbose(2, pmix_client_globals.group_output, "pmix: group_leave called");

    if (!pmix_atomic_check_bool(&pmix_globals.initialized)) {
        return PMIX_ERR_INIT;
    }

    /* if we aren't connected, don't attempt to send */
    if (!pmix_atomic_check_bool(&pmix_globals.connected)) {
        return PMIX_ERR_UNREACH;
    }

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        return PMIX_ERR_NOT_AVAILABLE;
    }

    /* create a callback object as we need to pass it to the
     * recv routine so we know which callback to use when
     * the return message is recvd */
    PMIX_CONSTRUCT(&cb, pmix_group_tracker_t);

    /* push the message into our event base to send to the server */
    if (PMIX_SUCCESS != (rc = PMIx_Group_leave_nb(grp, info, ninfo, op_cbfunc, (void *) &cb))) {
        PMIX_ERROR_LOG(rc);
        PMIX_DESTRUCT(&cb);
        return rc;
    }

    /* wait for the operation to complete */
    PMIX_WAIT_THREAD(&cb.lock);
    rc = cb.status;
    PMIX_DESTRUCT(&cb);

    pmix_output_verbose(2, pmix_client_globals.group_output,
                        "pmix: group leave completed");

    return rc;
}

/* callback fired once the PMIX_GROUP_LEFT event has been locally
 * generated - per the API contract, that is when the leave operation
 * is considered complete (it is not indicative of remote receipt) */
static void leave_cbfunc(pmix_status_t status, void *cbdata)
{
    pmix_group_tracker_t *cb = (pmix_group_tracker_t *) cbdata;

    if (NULL != cb->opcbfunc) {
        cb->opcbfunc(status, cb->cbdata);
    }
    PMIX_RELEASE(cb);
}

PMIX_EXPORT pmix_status_t PMIx_Group_leave_nb(const char grp[],
                                              const pmix_info_t info[], size_t ninfo,
                                              pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    pmix_status_t rc;
    pmix_group_tracker_t *cb = NULL;
    pmix_group_t *grpobj, *pgrp;
    pmix_proc_t *range = NULL;
    pmix_data_array_t darray;
    size_t n, m, nrange;

    pmix_output_verbose(2, pmix_client_globals.group_output,
                        "pmix:group_leave_nb called");

    if (!pmix_atomic_check_bool(&pmix_globals.initialized)) {
        return PMIX_ERR_INIT;
    }

    /* if we aren't connected, then we cannot notify */
    if (!pmix_atomic_check_bool(&pmix_globals.connected)) {
        return PMIX_ERR_UNREACH;
    }

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        return PMIX_ERR_NOT_AVAILABLE;
    }

    /* check for bozo input */
    if (NULL == grp) {
        return PMIX_ERR_BAD_PARAM;
    }

    /* find this group in our local list - we can only leave a group
     * we belong to, and we need its membership to know who to notify */
    grpobj = NULL;
    PMIX_LIST_FOREACH(pgrp, &pmix_client_globals.groups, pmix_group_t) {
        if (0 == strcmp(grp, pgrp->grpid)) {
            grpobj = pgrp;
            break;
        }
    }
    if (NULL == grpobj) {
        return PMIX_ERR_NOT_FOUND;
    }

    cb = PMIX_NEW(pmix_group_tracker_t);
    cb->opcbfunc = cbfunc;
    cb->cbdata = cbdata;

    /* Per the API contract, leaving generates a PMIX_GROUP_LEFT event
     * that notifies all members of the group of our departure. Build the
     * event's info array: a custom range limited to the other members,
     * the identity of the departing proc, the group ID, and any
     * directives the caller provided. */
    cb->ninfo = 3 + ninfo;
    PMIX_INFO_CREATE(cb->info, cb->ninfo);
    if (NULL == cb->info) {
        PMIX_RELEASE(cb);
        return PMIX_ERR_NOMEM;
    }

    /* the custom range is the membership, excluding ourselves */
    PMIX_PROC_CREATE(range, grpobj->nmbrs);
    if (NULL == range) {
        PMIX_RELEASE(cb);
        return PMIX_ERR_NOMEM;
    }
    nrange = 0;
    for (m = 0; m < grpobj->nmbrs; m++) {
        if (PMIX_CHECK_PROCID(&grpobj->members[m], &pmix_globals.myid)) {
            continue;
        }
        PMIX_XFER_PROCID(&range[nrange], &grpobj->members[m]);
        ++nrange;
    }

    n = 0;
    darray.type = PMIX_PROC;
    darray.array = range;
    darray.size = nrange;
    /* PMIX_INFO_LOAD deep-copies the array into the info */
    PMIX_INFO_LOAD(&cb->info[n], PMIX_EVENT_CUSTOM_RANGE, &darray, PMIX_DATA_ARRAY);
    ++n;
    /* identify the departing process */
    PMIX_INFO_LOAD(&cb->info[n], PMIX_EVENT_AFFECTED_PROC, &pmix_globals.myid, PMIX_PROC);
    ++n;
    /* identify the group being left */
    PMIX_INFO_LOAD(&cb->info[n], PMIX_GROUP_ID, grp, PMIX_STRING);
    ++n;
    /* carry along any caller-provided directives */
    for (m = 0; m < ninfo; m++) {
        PMIX_INFO_XFER(&cb->info[n], &info[m]);
        ++n;
    }
    PMIX_PROC_FREE(range, grpobj->nmbrs);

    /* we are no longer a member - drop the group from our local list */
    pmix_list_remove_item(&pmix_client_globals.groups, &grpobj->super);
    PMIX_RELEASE(grpobj);

    /* generate the event - the callback fires once it has been locally
     * generated, which is when the operation is complete */
    rc = PMIx_Notify_event(PMIX_GROUP_LEFT, &pmix_globals.myid, PMIX_RANGE_CUSTOM,
                           cb->info, cb->ninfo, leave_cbfunc, (void *) cb);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(cb);
    }

    return rc;
}

static void op_cbfunc(pmix_status_t status, void *cbdata)
{
    pmix_group_tracker_t *cb = (pmix_group_tracker_t *) cbdata;

    cb->status = status;
    if (NULL != cb->cbfunc) {
        cb->cbfunc(status, cb->info, cb->ninfo, cb->cbdata, NULL, NULL);
    }
    PMIX_POST_OBJECT(cb);
    PMIX_WAKEUP_THREAD(&cb->lock);
}

static void relfn(void *cbdata)
{
    pmix_group_tracker_t *cb = (pmix_group_tracker_t *) cbdata;
    PMIX_RELEASE(cb);
}

static void op_cbfunc_rel(pmix_status_t status, void *cbdata)
{
    pmix_group_tracker_t *cb = (pmix_group_tracker_t *) cbdata;

    cb->status = status;
    if (NULL != cb->cbfunc) {
        cb->cbfunc(status, cb->info, cb->ninfo, cb->cbdata, relfn, cb);
    } else {
        PMIX_RELEASE(cb);
    }
}

static void construct_cbfunc(struct pmix_peer_t *pr,
                             pmix_ptl_hdr_t *hdr,
                             pmix_buffer_t *buf,
                             void *cbdata)
{
    pmix_group_tracker_t *cb = (pmix_group_tracker_t *) cbdata;
    pmix_status_t rc;
    pmix_status_t ret;
    int32_t cnt;
    size_t ctxid = SIZE_MAX;
    bool gotctxid = false;
    pmix_proc_t *members = NULL;
    size_t nmembers = 0;
    size_t ngrpinfo = 0;
    size_t n, m, ninfo, npinfo;
    pmix_info_t grpinfo, *iptr, *pinfo;
    pmix_data_array_t darray;
    void *ilist;

    PMIX_HIDE_UNUSED_PARAMS(pr, hdr);

    pmix_output_verbose(2, pmix_client_globals.group_output,
                        "%s pmix:client construct callback activated with %d bytes",
                        PMIX_NAME_PRINT(&pmix_globals.myid),
                        (NULL == buf) ? -1 : (int) buf->bytes_used);

    if (NULL == buf) {
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
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        ret = rc;
    }

    if (PMIX_SUCCESS != ret) {
        goto report;
    }

    /* unpack the final membership */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, pmix_client_globals.myserver, buf, &nmembers, &cnt, PMIX_SIZE);
    if (PMIX_ERR_UNPACK_READ_PAST_END_OF_BUFFER == rc) {
        goto report;
    } else if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        ret = rc;
        goto report;
    }
    if (0 < nmembers) {
        PMIX_PROC_CREATE(members, nmembers);
        cnt = nmembers;
        PMIX_BFROPS_UNPACK(rc, pmix_client_globals.myserver, buf, members, &cnt, PMIX_PROC);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            ret = rc;
            goto report;
        }
    }

    /* unpack any ctxid that was provided */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, pmix_client_globals.myserver, buf, &gotctxid, &cnt, PMIX_BOOL);
    if (PMIX_ERR_UNPACK_READ_PAST_END_OF_BUFFER == rc) {
        goto report;
    } else if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        ret = rc;
        goto report;
    }
    if (gotctxid) {
        cnt = 1;
        PMIX_BFROPS_UNPACK(rc, pmix_client_globals.myserver, buf, &ctxid, &cnt, PMIX_SIZE);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            ret = rc;
            goto report;
        }
    }

    // unpack any group info that was provided
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, pmix_client_globals.myserver, buf, &ngrpinfo, &cnt, PMIX_SIZE);
    if (PMIX_ERR_UNPACK_READ_PAST_END_OF_BUFFER == rc) {
        goto report;
    } else if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        ret = rc;
        goto report;
    }
    if (0 < ngrpinfo && gotctxid) {
        for (n=0; n < ngrpinfo; n++) {
            PMIX_INFO_CONSTRUCT(&grpinfo);
            cnt = 1;
            PMIX_BFROPS_UNPACK(rc, pmix_client_globals.myserver, buf, &grpinfo, &cnt, PMIX_INFO);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                ret = rc;
                PMIX_INFO_DESTRUCT(&grpinfo);
                goto report;
            }
            // store the info locally
            iptr = (pmix_info_t*)grpinfo.value.data.darray->array;
            ninfo = grpinfo.value.data.darray->size;

            if (PMIX_CHECK_KEY(&grpinfo, PMIX_GROUP_INFO)) {
                // this is just a single array of group info
                rc = pmix_server_process_grpinfo(ctxid, iptr, ninfo);
                if (PMIX_SUCCESS != rc) {
                    PMIX_ERROR_LOG(rc);
                    ret = rc;
                    PMIX_INFO_DESTRUCT(&grpinfo);
                    goto report;
                }

            } else {
                // contains an array of group info arrays
                for (m=0; m < ninfo; m++) {
                    pinfo = (pmix_info_t*)iptr[m].value.data.darray->array;
                    npinfo = iptr[m].value.data.darray->size;
                    rc = pmix_server_process_grpinfo(ctxid, pinfo, npinfo);
                    if (PMIX_SUCCESS != rc) {
                        PMIX_ERROR_LOG(rc);
                        ret = rc;
                        PMIX_INFO_DESTRUCT(&grpinfo);
                        goto report;
                    }
                }
            }
            PMIX_INFO_DESTRUCT(&grpinfo);
        }
    }

report:
    ilist = PMIx_Info_list_start();

    rc = PMIx_Info_list_add(ilist, PMIX_GROUP_ID, cb->grpid, PMIX_STRING);
    if (PMIX_SUCCESS != rc) {
        PMIx_Info_list_release(ilist);
        goto done;
    }
    if (NULL != members) {
        darray.array = members;
        darray.size = nmembers;
        darray.type = PMIX_PROC;
        rc = PMIx_Info_list_add(ilist, PMIX_GROUP_MEMBERSHIP, &darray, PMIX_DATA_ARRAY);
        // data was copied into the info
        if (PMIX_SUCCESS != rc) {
            PMIx_Info_list_release(ilist);
            goto done;
        }
    }
    if (gotctxid) {
        rc = PMIx_Info_list_add(ilist, PMIX_GROUP_CONTEXT_ID, &ctxid, PMIX_SIZE);
        if (PMIX_SUCCESS != rc) {
            PMIx_Info_list_release(ilist);
            goto done;
        }
    }
    PMIx_Info_list_convert(ilist, &darray);
    cb->info = (pmix_info_t*)darray.array;
    cb->ninfo = darray.size;
    PMIx_Info_list_release(ilist);

done:
    if (NULL != members) {
        PMIX_PROC_FREE(members, nmembers);
    }
    if (NULL != cb->cbfunc) {
        cb->cbfunc(ret, cb->info, cb->ninfo, cb->cbdata, relfn, cb);
        return;
    }
    PMIX_RELEASE(cb);
}

static void destruct_cbfunc(struct pmix_peer_t *pr,
                            pmix_ptl_hdr_t *hdr,
                            pmix_buffer_t *buf,
                            void *cbdata)
{
    pmix_group_tracker_t *cb = (pmix_group_tracker_t *) cbdata;
    pmix_status_t rc;
    pmix_status_t ret;
    int32_t cnt;
    pmix_group_t *grp;

    PMIX_HIDE_UNUSED_PARAMS(pr, hdr);

    pmix_output_verbose(2, pmix_client_globals.group_output,
                        "pmix:client recv callback activated with %d bytes",
                        (NULL == buf) ? -1 : (int) buf->bytes_used);

    if (NULL == buf) {
        ret = PMIX_ERR_BAD_PARAM;
        PMIX_ERROR_LOG(ret);
        goto report;
    }

    /* find this group */
    grp = NULL;
    PMIX_LIST_FOREACH(grp, &pmix_client_globals.groups, pmix_group_t) {
        if (0 == strcmp(cb->grpid, grp->grpid)) {
            pmix_list_remove_item(&pmix_client_globals.groups, &grp->super);
            PMIX_RELEASE(grp);
            break;
        }
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
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        ret = rc;
    }

report:
    if (NULL != cb->opcbfunc) {
        cb->opcbfunc(ret, cb->cbdata);
    }
    PMIX_RELEASE(cb);
}

static void info_cbfunc(pmix_status_t status, pmix_info_t *info, size_t ninfo, void *cbdata,
                        pmix_release_cbfunc_t release_fn, void *release_cbdata)
{
    pmix_group_tracker_t *cb = (pmix_group_tracker_t *) cbdata;
    size_t n, nmembers = 0;
    pmix_proc_t *members = NULL;
    char *grpid = NULL;
    size_t ctxid = SIZE_MAX;
    pmix_status_t rc;

    /* see if anything was returned - e.g., a context id */
    cb->status = status;
    /* copy/save any returned info */
    if (NULL != info) {
        cb->nresults = ninfo;
        PMIX_INFO_CREATE(cb->results, cb->nresults);
        for (n = 0; n < ninfo; n++) {
            if (PMIX_CHECK_KEY(&info[n], PMIX_GROUP_MEMBERSHIP)) {
                members = (pmix_proc_t*)info[n].value.data.darray->array;
                nmembers = info[n].value.data.darray->size;

            } else if (PMIX_CHECK_KEY(&info[n], PMIX_GROUP_ID)) {
                grpid = info[n].value.data.string;

            } else if (PMIX_CHECK_KEY(&info[n], PMIX_GROUP_CONTEXT_ID)) {
                rc = PMIx_Value_get_number(&info[n].value, &ctxid, PMIX_SIZE);
                if (PMIX_SUCCESS != rc) {
                    PMIX_ERROR_LOG(rc);
                }
            }
            PMIX_INFO_XFER(&cb->results[n], &info[n]);
        }
    }
    if (NULL != members && NULL != grpid) {
        add_group(grpid, ctxid, members, nmembers);
    }

    if (NULL != release_fn) {
        release_fn(release_cbdata);
    }
    PMIX_POST_OBJECT(cb);
    PMIX_WAKEUP_THREAD(&cb->lock);
}

static pmix_status_t add_group(const char *grpid,
                               size_t ctxid,
                               pmix_proc_t *members, size_t nmembers)
{
    pmix_group_t *grp;

    /* since the group construction has finished, we can add
     * the group to out list of groups. Always sort the
     * the array to maintain the same view across participants.*/
    grp = PMIX_NEW(pmix_group_t);
    PMIX_PROC_CREATE(grp->members, nmembers);
    memcpy(grp->members, members, nmembers * sizeof(pmix_proc_t));
    qsort(grp->members, nmembers, sizeof(pmix_proc_t), pmix_util_compare_proc);
    grp->nmbrs = nmembers;
    grp->grpid = strdup(grpid);
    grp->ctxid = ctxid;
    pmix_list_append(&pmix_client_globals.groups, &grp->super);

    return PMIX_SUCCESS;
}

