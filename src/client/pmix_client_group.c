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

#include "src/include/pmix_prefetch.h"

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
    pmix_list_item_t super;
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
     * ACCEPTED - those form the group. Whether a non-accepter is fatal depends
     * on "optional" (the PMIX_GROUP_OPTIONAL directive): when set, participation
     * is optional, so every non-accepter (a decliner, a terminated proc, or, on
     * timeout, a non-responder) is reported to the leader via
     * PMIX_GROUP_INVITE_FAILED and excluded, and the group forms on the reduced
     * membership. When not set (the default), the construct is all-or-nothing:
     * any non-accepter aborts it via PMIX_GROUP_CONSTRUCT_ABORT and no group
     * forms. The timer bounds how long the leader waits, and is armed only when
     * PMIX_TIMEOUT is provided. */
    bool *responded;
    bool *answered;
    size_t nanswered;
    bool optional;
    /* whether PMIX_GROUP_NOTIFY_TERMINATION was requested at construct time;
     * carried into the persistent pmix_group_t so it can be re-applied to the
     * later destruct (see add_group / PMIx_Group_destruct_nb). */
    bool notterm;
    pmix_event_t ev;
    bool timer_active;
    bool completed;
    pmix_info_t *info;
    size_t ninfo;
    pmix_info_t *results;
    size_t nresults;
    /* State of the outcome announcement (see announce_step). The
     * announcement is a chain of non-blocking notifications run on the
     * progress thread, so its position has to live somewhere that survives
     * between them: astate is where the chain is, aidx walks the
     * non-responders while reporting them, and ainfo holds the info array
     * of the notification currently in flight - which must outlive the
     * PMIx_Notify_event call that carries it. */
    int astate;
    size_t aidx;
    pmix_info_t *ainfo;
    size_t nainfo;
    pmix_op_cbfunc_t opcbfunc;
    pmix_info_cbfunc_t cbfunc;
    void *cbdata;
} pmix_group_tracker_t;

/* the announcement chain's states, in the order they are traversed */
#define PMIX_GRP_ANNOUNCE_START    0
#define PMIX_GRP_ANNOUNCE_FAILED   1
#define PMIX_GRP_ANNOUNCE_COMPLETE 2
#define PMIX_GRP_ANNOUNCE_DONE     3

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
    p->optional = false;
    p->notterm = false;
    p->timer_active = false;
    p->completed = false;
    p->info = NULL;
    p->ninfo = 0;
    p->results = NULL;
    p->nresults = 0;
    p->astate = PMIX_GRP_ANNOUNCE_START;
    p->aidx = 0;
    p->ainfo = NULL;
    p->nainfo = 0;
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
    if (NULL != p->ainfo) {
        PMIX_INFO_FREE(p->ainfo, p->nainfo);
    }
    if (NULL != p->results) {
        PMIX_INFO_FREE(p->results, p->nresults);
    }
    if (NULL != p->grpid) {
        free(p->grpid);
        p->grpid = NULL;
    }
}
PMIX_CLASS_INSTANCE(pmix_group_tracker_t, pmix_list_item_t, gtcon, gtdes);

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
static void announce_step(pmix_group_tracker_t *cb);

static void info_cbfunc(pmix_status_t status, pmix_info_t *info, size_t ninfo, void *cbdata,
                        pmix_release_cbfunc_t release_fn, void *release_cbdata);
static pmix_status_t add_group(const char *grpid,
                               size_t ctxid, bool notterm,
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
            if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
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
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }

    /* pack the group ID */
    PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, msg, &grp, 1, PMIX_STRING);
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }

    /* pack the number of procs */
    PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, msg, &nprocs, 1, PMIX_SIZE);
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    if (0 < nprocs) {
        PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, msg, procs, nprocs, PMIX_PROC);
        if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
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
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
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
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        PMIX_ERROR_LOG(rc);
        PMIX_INFO_FREE(icopy, sz);
        return rc;
    }
    if (0 < sz) {
        PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, msg, icopy, sz, PMIX_INFO);
        if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
            PMIX_ERROR_LOG(rc);
            PMIX_INFO_FREE(icopy, sz);
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

    /* both are documented as optional OUT parameters, so honor a NULL for
     * either - and define them before anything can fail, so the caller can
     * read them on an error return too */
    if (NULL != results) {
        *results = NULL;
    }
    if (NULL != nresults) {
        *nresults = 0;
    }

    if (PMIX_UNLIKELY(!pmix_atomic_check_bool(&pmix_globals.initialized))) {
        return PMIX_ERR_INIT;
    }

    /* if we aren't connected, don't attempt to send */
    if (PMIX_UNLIKELY(!pmix_atomic_check_bool(&pmix_globals.connected))) {
        return PMIX_ERR_UNREACH;
    }

    if (PMIX_UNLIKELY(pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped))) {
        return PMIX_ERR_NOT_AVAILABLE;
    }

    /* create a callback object as we need to pass it to the
     * recv routine so we know which callback to use when
     * the return message is recvd */
    cb = PMIX_NEW(pmix_group_tracker_t);

    /* push the message into our event base to send to the server */
    rc = PMIx_Group_construct_nb(grp, procs, nprocs, info, ninfo, info_cbfunc, cb);
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        PMIX_RELEASE(cb);
        return rc;
    }

    /* wait for the group construct to complete */
    PMIX_WAIT_THREAD(&cb->lock);
    rc = cb->status;
    if (PMIX_SUCCESS == rc && NULL != results && NULL != nresults) {
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

    if (PMIX_UNLIKELY(!pmix_atomic_check_bool(&pmix_globals.initialized))) {
        return PMIX_ERR_INIT;
    }

    /* if we aren't connected, don't attempt to send */
    if (PMIX_UNLIKELY(!pmix_atomic_check_bool(&pmix_globals.connected))) {
        return PMIX_ERR_UNREACH;
    }

    if (PMIX_UNLIKELY(pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped))) {
        return PMIX_ERR_NOT_AVAILABLE;
    }

    /* check for bozo input - the group ID names the collective and is
     * strdup'd onto the tracker below */
    if (PMIX_UNLIKELY(NULL == grp)) {
        return PMIX_ERR_BAD_PARAM;
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
        if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
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
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
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
    /* Capture the group's failure policy while we still have the construct
     * directives, so construct_cbfunc can record it in the persistent group
     * and the later destruct can re-apply PMIX_GROUP_NOTIFY_TERMINATION on
     * the caller's behalf (the server keeps no group state between the two
     * collectives).
     *
     * This belongs here, on the tracker construct_cbfunc actually reads.
     * The blocking wrapper used to record it on its own tracker instead -
     * which construct_cbfunc never sees - so add_group() was always told
     * "false", and because add_group() ignores a repeat request for a group
     * it already holds, the correctly-flagged call that followed could not
     * correct it. The policy was therefore never honored by any caller. */
    for (n = 0; n < ninfo; n++) {
        if (PMIX_CHECK_KEY(&info[n], PMIX_GROUP_NOTIFY_TERMINATION)) {
            cb->notterm = PMIX_INFO_TRUE(&info[n]);
            break;
        }
    }

    /* push the message into our event base to send to the server */
    PMIX_PTL_SEND_RECV(rc, pmix_client_globals.myserver, msg, construct_cbfunc, (void*)cb);
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
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

    if (PMIX_UNLIKELY(!pmix_atomic_check_bool(&pmix_globals.initialized))) {
        return PMIX_ERR_INIT;
    }

    /* if we aren't connected, don't attempt to send */
    if (PMIX_UNLIKELY(!pmix_atomic_check_bool(&pmix_globals.connected))) {
        return PMIX_ERR_UNREACH;
    }

    if (PMIX_UNLIKELY(pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped))) {
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
    pmix_info_t *dinfo = (pmix_info_t *) info;
    size_t ndinfo = ninfo, n;
    bool freeinfo = false, notterm = false;
    /* snapshot of the group taken under the lock - see below */
    pmix_proc_t *mbrs = NULL;
    size_t nmbrs = 0;
    bool grpnotterm = false;

    pmix_output_verbose(2, pmix_client_globals.group_output,
                        "pmix:group_destruct_nb called");

    if (PMIX_UNLIKELY(!pmix_atomic_check_bool(&pmix_globals.initialized))) {
        return PMIX_ERR_INIT;
    }

    /* if we aren't connected, don't attempt to send */
    if (PMIX_UNLIKELY(!pmix_atomic_check_bool(&pmix_globals.connected))) {
        return PMIX_ERR_UNREACH;
    }

    if (PMIX_UNLIKELY(pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped))) {
        return PMIX_ERR_NOT_AVAILABLE;
    }

    /* check for bozo input */
    if (PMIX_UNLIKELY(NULL == grpid)) {
        return PMIX_ERR_BAD_PARAM;
    }

    /* Find this group and take a copy of what we need from it. We hold the
     * lock only for the lookup and the copy: the progress thread can append
     * to this list, remove from it, and trim a departed proc out of a
     * group's membership, so neither the list nor grp->members can be
     * relied on once we let go. Packing from a copy also keeps the lock off
     * the bfrops path entirely. */
    pmix_mutex_lock(&pmix_client_globals.grouplock);
    grp = NULL;
    PMIX_LIST_FOREACH(pgrp, &pmix_client_globals.groups, pmix_group_t) {
        if (0 == strcmp(grpid, pgrp->grpid)) {
            grp = pgrp;
            break;
        }
    }
    if (PMIX_UNLIKELY(NULL == grp)) {
        pmix_mutex_unlock(&pmix_client_globals.grouplock);
        return PMIX_ERR_NOT_FOUND;
    }
    grpnotterm = grp->notterm;
    nmbrs = grp->nmbrs;
    if (0 < nmbrs) {
        PMIX_PROC_CREATE(mbrs, nmbrs);
        if (PMIX_UNLIKELY(NULL == mbrs)) {
            pmix_mutex_unlock(&pmix_client_globals.grouplock);
            return PMIX_ERR_NOMEM;
        }
        memcpy(mbrs, grp->members, nmbrs * sizeof(pmix_proc_t));
    }
    pmix_mutex_unlock(&pmix_client_globals.grouplock);

    /* the group's failure policy was chosen at construct time; re-apply
     * PMIX_GROUP_NOTIFY_TERMINATION here so the server (which keeps no group
     * state between the two collectives) honors it for this destruct. If the
     * group was constructed with the flag and the caller did not itself provide
     * it, append it to the info we send. A caller that explicitly passes the
     * attribute overrides the remembered policy. */
    if (grpnotterm) {
        for (n = 0; n < ninfo; n++) {
            if (PMIX_CHECK_KEY(&info[n], PMIX_GROUP_NOTIFY_TERMINATION)) {
                notterm = true;
                break;
            }
        }
        if (!notterm) {
            bool flag = true;
            ndinfo = ninfo + 1;
            PMIX_INFO_CREATE(dinfo, ndinfo);
            for (n = 0; n < ninfo; n++) {
                PMIX_INFO_XFER(&dinfo[n], (pmix_info_t *) &info[n]);
            }
            PMIX_INFO_LOAD(&dinfo[ninfo], PMIX_GROUP_NOTIFY_TERMINATION, &flag, PMIX_BOOL);
            freeinfo = true;
        }
    }

    msg = PMIX_NEW(pmix_buffer_t);
    /* pack the cmd */
    PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, msg, &cmd, 1, PMIX_COMMAND);
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        PMIX_ERROR_LOG(rc);
        goto done;
    }

    /* pack the group ID */
    PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, msg, &grpid, 1, PMIX_STRING);
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        PMIX_ERROR_LOG(rc);
        goto done;
    }

    /* pack the membership - the server isn't storing it,
     * so we have to send it so that the server can
     * track when all local procs have participated */
    PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, msg, &nmbrs, 1, PMIX_SIZE);
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        PMIX_ERROR_LOG(rc);
        goto done;
    }
    PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, msg, mbrs, nmbrs, PMIX_PROC);
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        PMIX_ERROR_LOG(rc);
        goto done;
    }

    /* pack the info structs */
    PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, msg, &ndinfo, 1, PMIX_SIZE);
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(msg);
        goto done;
    }
    if (0 < ndinfo) {
        PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, msg, dinfo, ndinfo, PMIX_INFO);
        if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
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
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        PMIX_RELEASE(cb);
    }

done:
    if (PMIX_SUCCESS != rc && NULL != msg) {
        PMIX_RELEASE(msg);
    }
    if (freeinfo) {
        PMIX_INFO_FREE(dinfo, ndinfo);
    }
    if (NULL != mbrs) {
        PMIX_PROC_FREE(mbrs, nmbrs);
    }
    return rc;
}

/* Count an invitation answer. Registered as an internal observer, so it runs
 * ahead of the event chain and cannot be suppressed by an application handler
 * that ends the chain - which used to hang PMIx_Group_invite outright, since
 * this is the only thing that counts answers and resolves the invitation.
 * See openpmix#4059. Having no return value, it also can no longer swallow
 * the application's own handlers for these codes, which the event handler it
 * replaced did by completing the chain. */
static void invite_observer(pmix_status_t status, const pmix_proc_t *source,
                            const pmix_info_t info[], size_t ninfo,
                            const pmix_proc_t *affected, size_t naffected,
                            void *cbobject)
{
    pmix_group_tracker_t *cb = (pmix_group_tracker_t *) cbobject;
    const pmix_proc_t *responder = source;
    size_t n;

    if (PMIX_UNLIKELY(NULL == cb)) {
        pmix_output(0, "%s: INVITE OBSERVER NULL OBJECT", PMIX_NAME_PRINT(&pmix_globals.myid));
        return;
    }
    /* the invitation has already resolved and is being announced - a late
     * answer must not edit the membership out from under that */
    if (cb->completed) {
        return;
    }

    /* identify the responding proc. An accept/decline names itself as the
     * event source; a termination names the departed proc as the affected
     * proc, which reaches us either as the chain's affected list or as a
     * directive in the info array. */
    if (NULL != affected && 0 < naffected) {
        responder = affected;
    }
    for (n = 0; n < ninfo; n++) {
        if (PMIX_CHECK_KEY(&info[n], PMIX_EVENT_AFFECTED_PROC)) {
            responder = info[n].value.data.proc;
        }
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
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc && PMIX_OPERATION_SUCCEEDED != rc)) {
        PMIX_DESTRUCT(&cb2);
        return PMIX_ERR_BAD_PARAM;
    }
    kv = (pmix_kval_t *) pmix_list_remove_first(&cb2.kvs);
    PMIX_DESTRUCT(&cb2);
    if (PMIX_UNLIKELY(NULL == kv)) { // should never be NULL
        return PMIX_ERR_BAD_PARAM;
    }
    rc = PMIx_Value_get_number(kv->value, jsize, PMIX_UINT32);
    PMIX_RELEASE(kv);
    return rc;
}

/* An invitation has resolved - either every invitee answered, or the timeout
 * fired. Cancel the timer (if still pending) and announce the outcome to the
 * participants; the caller is completed at the end of that chain, not here.
 *
 * This runs on the progress thread (from invite_observer, or from the timer),
 * which is exactly why the announcement is built out of non-blocking
 * notifications - see announce_step. Guarded so a late (post-timeout)
 * acceptance cannot resolve the invitation twice. */
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
    if (PMIX_UNLIKELY(PMIX_SUCCESS != status)) {
        /* nothing resolved, so there is nothing to announce */
        cb->astate = PMIX_GRP_ANNOUNCE_DONE;
    } else {
        cb->astate = PMIX_GRP_ANNOUNCE_START;
    }
    announce_step(cb);
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
    pmix_status_t rc;
    uint32_t jsize, j;
    int timeout = 0;
    struct timeval tv;

    cb->grpid = strdup(grp);

    /* compute the number of proposed members, expanding any wildcard ranks */
    for (n = 0; n < nprocs; n++) {
        if (PMIX_RANK_WILDCARD == procs[n].rank) {
            rc = invite_job_size(&procs[n], &jsize);
            if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
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
    if (PMIX_UNLIKELY(NULL == cb->members)) {
        return PMIX_ERR_NOMEM;
    }
    cb->responded = (bool *) calloc(cb->nmembers, sizeof(bool));
    if (PMIX_UNLIKELY(NULL == cb->responded)) {
        return PMIX_ERR_NOMEM;
    }
    cb->answered = (bool *) calloc(cb->nmembers, sizeof(bool));
    if (PMIX_UNLIKELY(NULL == cb->answered)) {
        return PMIX_ERR_NOMEM;
    }
    m = 0;
    for (n = 0; n < nprocs; n++) {
        if (PMIX_RANK_WILDCARD == procs[n].rank) {
            rc = invite_job_size(&procs[n], &jsize);
            if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
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
    /* If we (the leader) named ourselves among the invitees, we have
     * obviously already answered by accepting - count that answer here, and
     * only here. Seeding nanswered unconditionally was wrong: a leader that
     * invites others without listing itself has no member to match, so the
     * count started one ahead of the flags and the invitation resolved one
     * answer early. Under the default all-or-nothing policy that made the
     * last invitee look like a non-responder and aborted the construct. */
    for (m = 0; m < cb->nmembers; m++) {
        if (PMIX_CHECK_PROCID(&cb->members[m], &pmix_globals.myid)) {
            cb->responded[m] = true;
            cb->answered[m] = true;
            cb->nanswered++;
            break;
        }
    }

    /* Watch for the invitees' answers. This is registered as an internal
     * observer rather than an event handler: invite_observer() is the only
     * thing that counts answers and calls invite_wake(), and as a handler it
     * could be silently pre-empted by an application handler that ended the
     * chain - a PMIX_GROUP_INVITE_ACCEPTED handler that logs who joined, say -
     * leaving this invitation to block forever unless the caller supplied a
     * PMIX_TIMEOUT. See openpmix#4059. Observers run ahead of the chain, so
     * the answers now always reach us.
     *
     * We run on the caller's thread here, so waiting for the registration to
     * complete before notifying the invitees is safe (and keeps an early
     * acceptance from depending on the cached-event replay). The tracker is
     * NOT handed to the registry to own - the invite path releases it in
     * invite_teardown - so no release function is given. */
    ncodes = sizeof(codes) / sizeof(pmix_status_t);
    PMIX_CONSTRUCT(&lock, pmix_group_tracker_t);
    rc = pmix_event_register_observer("pmix-group-invite", codes, ncodes,
                                      invite_observer, cb, NULL,
                                      regcbfunc, &lock);
    /* only wait if the registration was actually accepted - regcbfunc is the
     * only thing that ever wakes this lock, and it does not fire when the
     * call itself failed */
    if (PMIX_SUCCESS == rc) {
        PMIX_WAIT_THREAD(&lock.lock);
        rc = lock.status;
        cb->ref = lock.ref;
    }
    PMIX_DESTRUCT(&lock);
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        return rc;
    }

    /* check for directives - a timeout bounds how long we wait for the
     * invitees to respond, and PMIX_GROUP_OPTIONAL selects reduced membership
     * (form the group on whoever accepts) over the default all-or-nothing
     * construct (abort if any invitee fails to join) */
    if (NULL != info) {
        for (n = 0; n < ninfo; n++) {
            if (PMIX_CHECK_KEY(&info[n], PMIX_TIMEOUT)) {
                rc = PMIx_Value_get_number(&info[n].value, &timeout, PMIX_INT);
                if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
                    timeout = 0;
                }
            } else if (PMIX_CHECK_KEY(&info[n], PMIX_GROUP_OPTIONAL)) {
                cb->optional = PMIX_INFO_TRUE(&info[n]);
            }
        }
    }

    /* limit the range to just the procs we are inviting */
    PMIX_INFO_CREATE(cb->info, 3);
    if (PMIX_UNLIKELY(NULL == cb->info)) {
        return PMIX_ERR_NOMEM;
    }
    cb->ninfo = 3;
    n = 0;
    (void) strncpy(cb->info[n].key, PMIX_EVENT_CUSTOM_RANGE, PMIX_MAX_KEYLEN);
    cb->info[n].value.type = PMIX_DATA_ARRAY;
    PMIX_DATA_ARRAY_CREATE(cb->info[n].value.data.darray, nprocs, PMIX_PROC);
    if (PMIX_UNLIKELY(NULL == cb->info[n].value.data.darray || NULL == cb->info[n].value.data.darray->array)) {
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
    /* as with the registration above, op_cbfunc is the only thing that wakes
     * this lock and it does not run when the notify call itself failed -
     * waiting unconditionally hung the caller on an error */
    if (PMIX_SUCCESS == rc) {
        PMIX_WAIT_THREAD(&lock.lock);
        rc = lock.status;
    }
    PMIX_DESTRUCT(&lock);
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
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

/* ---- announcing the outcome of an invitation ----------------------------
 *
 * Once an invitation has resolved - every invitee answered, or the timeout
 * fired - the leader has to tell the participants what happened. If any
 * invitee failed to join (declined, terminated, or timed out) and
 * participation was not marked PMIX_GROUP_OPTIONAL, the construct is
 * all-or-nothing: abort it by notifying every invited participant with
 * PMIX_GROUP_CONSTRUCT_ABORT, and no group forms. Otherwise report each
 * non-responder to the leader via PMIX_GROUP_INVITE_FAILED and announce the
 * group to the members that accepted via PMIX_GROUP_CONSTRUCT_COMPLETE (only
 * sent to members; servers intercept it to update their membership lists).
 * The membership is the full invited list in the common case, or a reduced
 * list if some invitees declined, terminated, or timed out.
 *
 * This is a chain of *non-blocking* notifications, each step driven by the
 * completion of the one before it, all of it on the progress thread. It used
 * to be a straight-line function built out of blocking PMIx_Notify_event
 * calls, which meant it could only run on the caller's own thread after the
 * blocking PMIx_Group_invite woke - so PMIx_Group_invite_nb, which has no
 * such thread to borrow, never announced anything at all: no
 * PMIX_GROUP_INVITE_FAILED, no PMIX_GROUP_CONSTRUCT_COMPLETE, no
 * PMIX_GROUP_CONSTRUCT_ABORT, and therefore no group. Driving it from the
 * progress thread is what makes the non-blocking form work, and it is also
 * what lets a pending PMIx_Group_join complete at the point its man page
 * documents (see the leader watch below).
 *
 * The chain's position lives on the tracker (astate/aidx/ainfo) because
 * nothing else survives between the steps. Each notification's info array is
 * built on the heap into cb->ainfo and freed by the completion callback,
 * since it has to outlive the call that carries it. */

/* Complete the invitation for the caller, and retire the machinery. */
static void invite_relcb(pmix_status_t status, void *cbdata)
{
    pmix_group_tracker_t *cb = (pmix_group_tracker_t *) cbdata;
    PMIX_HIDE_UNUSED_PARAMS(status);

    PMIX_RELEASE(cb);
}

static void invite_finish(pmix_group_tracker_t *cb)
{
    if (NULL == cb->cbfunc) {
        /* the blocking form is waiting on the tracker's lock; it does the
         * teardown itself, on its own thread, where the deregistration may
         * safely be waited out */
        PMIX_POST_OBJECT(cb);
        PMIX_WAKEUP_THREAD(&cb->lock);
        return;
    }

    /* the non-blocking form: hand the outcome to the caller and then retire
     * the observer and the tracker ourselves - nobody else will. We are on
     * the progress thread, so the deregistration must not be waited out; the
     * tracker is released when it completes. */
    cb->cbfunc(cb->status, NULL, 0, cb->cbdata, NULL, NULL);
    if (cb->timer_active) {
        pmix_event_del(&cb->ev);
        cb->timer_active = false;
    }
    if (SIZE_MAX != cb->ref) {
        pmix_event_deregister_observer(cb->ref, invite_relcb, cb);
        cb->ref = SIZE_MAX;
    } else {
        PMIX_RELEASE(cb);
    }
}

static void announce_step(pmix_group_tracker_t *cb);

/* A notification in the announcement chain has been delivered: free the info
 * it carried and take the next step. */
static void announce_next(pmix_status_t status, void *cbdata)
{
    pmix_group_tracker_t *cb = (pmix_group_tracker_t *) cbdata;
    PMIX_HIDE_UNUSED_PARAMS(status);

    if (NULL != cb->ainfo) {
        PMIX_INFO_FREE(cb->ainfo, cb->nainfo);
        cb->ainfo = NULL;
        cb->nainfo = 0;
    }
    announce_step(cb);
}

/* Advance the announcement. Each iteration either dispatches a notification
 * and returns (the completion callback re-enters us) or falls through to the
 * next state. A notification that fails to dispatch never fires its
 * callback, so those paths loop rather than return - which is also why this
 * is a loop and not recursion: an invitation with many non-responders would
 * otherwise nest one frame per report. */
static void announce_step(pmix_group_tracker_t *cb)
{
    pmix_proc_t *members;
    size_t i, nmembers, nfailed;
    pmix_data_array_t darray;
    pmix_status_t rc;

    while (true) {
        switch (cb->astate) {
        case PMIX_GRP_ANNOUNCE_START:
            /* count the invitees that did not accept */
            nfailed = 0;
            for (i = 0; i < cb->nmembers; i++) {
                if (!cb->responded[i]) {
                    ++nfailed;
                }
            }
            if (0 == nfailed || cb->optional) {
                /* nothing to abort - report any non-accepters, then announce */
                cb->aidx = 0;
                cb->astate = PMIX_GRP_ANNOUNCE_FAILED;
                break;
            }
            /* all-or-nothing, and somebody failed to join: abort the whole
             * construct. Notify every *invited* participant - the full
             * membership, so those that did accept stop waiting for a
             * completion that will never come. That is the outcome of the
             * invitation, whether or not the notification itself succeeds. */
            cb->status = PMIX_GROUP_CONSTRUCT_ABORT;
            cb->astate = PMIX_GRP_ANNOUNCE_DONE;
            PMIX_INFO_CREATE(cb->ainfo, 3);
            if (PMIX_UNLIKELY(NULL == cb->ainfo)) {
                break;
            }
            cb->nainfo = 3;
            darray.type = PMIX_PROC;
            darray.array = cb->members;
            darray.size = cb->nmembers;
            PMIX_INFO_LOAD(&cb->ainfo[0], PMIX_EVENT_CUSTOM_RANGE, &darray, PMIX_DATA_ARRAY);
            /* this only goes to non-default handlers */
            PMIX_INFO_LOAD(&cb->ainfo[1], PMIX_EVENT_NON_DEFAULT, NULL, PMIX_BOOL);
            PMIX_INFO_LOAD(&cb->ainfo[2], PMIX_GROUP_ID, cb->grpid, PMIX_STRING);
            rc = PMIx_Notify_event(PMIX_GROUP_CONSTRUCT_ABORT, &pmix_globals.myid,
                                   PMIX_RANGE_CUSTOM, cb->ainfo, cb->nainfo,
                                   announce_next, (void *) cb);
            if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
                /* no callback is coming - clean up and carry on */
                PMIX_INFO_FREE(cb->ainfo, cb->nainfo);
                cb->ainfo = NULL;
                cb->nainfo = 0;
                break;
            }
            return;

        case PMIX_GRP_ANNOUNCE_FAILED:
            /* report the next proc that did not accept the invitation */
            while (cb->aidx < cb->nmembers && cb->responded[cb->aidx]) {
                ++cb->aidx;
            }
            if (cb->aidx >= cb->nmembers) {
                cb->astate = PMIX_GRP_ANNOUNCE_COMPLETE;
                break;
            }
            i = cb->aidx;
            ++cb->aidx;
            PMIX_INFO_CREATE(cb->ainfo, 1);
            if (PMIX_UNLIKELY(NULL == cb->ainfo)) {
                break;
            }
            cb->nainfo = 1;
            PMIX_INFO_LOAD(&cb->ainfo[0], PMIX_EVENT_AFFECTED_PROC, &cb->members[i], PMIX_PROC);
            rc = PMIx_Notify_event(PMIX_GROUP_INVITE_FAILED, &pmix_globals.myid,
                                   PMIX_RANGE_PROC_LOCAL, cb->ainfo, cb->nainfo,
                                   announce_next, (void *) cb);
            if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
                PMIX_INFO_FREE(cb->ainfo, cb->nainfo);
                cb->ainfo = NULL;
                cb->nainfo = 0;
                break;
            }
            return;

        case PMIX_GRP_ANNOUNCE_COMPLETE:
            /* build the final membership from the members that accepted and
             * announce the group to them */
            cb->astate = PMIX_GRP_ANNOUNCE_DONE;
            PMIX_PROC_CREATE(members, cb->nmembers);
            if (PMIX_UNLIKELY(NULL == members)) {
                cb->status = PMIX_ERR_NOMEM;
                break;
            }
            nmembers = 0;
            for (i = 0; i < cb->nmembers; i++) {
                if (cb->responded[i]) {
                    PMIX_LOAD_PROCID(&members[nmembers], cb->members[i].nspace,
                                     cb->members[i].rank);
                    ++nmembers;
                }
            }
            PMIX_INFO_CREATE(cb->ainfo, 4);
            if (PMIX_UNLIKELY(NULL == cb->ainfo)) {
                PMIX_PROC_FREE(members, cb->nmembers);
                cb->status = PMIX_ERR_NOMEM;
                break;
            }
            cb->nainfo = 4;
            darray.type = PMIX_PROC;
            darray.array = members;
            darray.size = nmembers;
            // limit the range to, and report, the final membership
            PMIX_INFO_LOAD(&cb->ainfo[0], PMIX_EVENT_CUSTOM_RANGE, &darray, PMIX_DATA_ARRAY);
            PMIX_INFO_LOAD(&cb->ainfo[1], PMIX_GROUP_MEMBERSHIP, &darray, PMIX_DATA_ARRAY);
            /* this only goes to non-default handlers */
            PMIX_INFO_LOAD(&cb->ainfo[2], PMIX_EVENT_NON_DEFAULT, NULL, PMIX_BOOL);
            PMIX_INFO_LOAD(&cb->ainfo[3], PMIX_GROUP_ID, cb->grpid, PMIX_STRING);
            /* the loads above copied the array into the info structs */
            PMIX_PROC_FREE(members, cb->nmembers);
            rc = PMIx_Notify_event(PMIX_GROUP_CONSTRUCT_COMPLETE, &pmix_globals.myid,
                                   PMIX_RANGE_CUSTOM, cb->ainfo, cb->nainfo,
                                   announce_next, (void *) cb);
            if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
                PMIX_INFO_FREE(cb->ainfo, cb->nainfo);
                cb->ainfo = NULL;
                cb->nainfo = 0;
                cb->status = rc;
                break;
            }
            return;

        default:
            invite_finish(cb);
            return;
        }
    }
}

/* Deregister the invitation observer (if it was ever registered), cancel any
 * pending timer, and release the tracker. Safe to call whether or not
 * invite_setup got as far as registering, and used both to clean up a failed
 * setup and to tear down after a blocking invite completes. Runs on the
 * caller's thread, so waiting out the deregistration is safe - and necessary,
 * since the observer holds a pointer to the tracker we are about to free. */
static void invite_teardown(pmix_group_tracker_t *cb)
{
    pmix_group_tracker_t lock;

    if (cb->timer_active) {
        pmix_event_del(&cb->ev);
        cb->timer_active = false;
    }
    if (SIZE_MAX != cb->ref) {
        PMIX_CONSTRUCT(&lock, pmix_group_tracker_t);
        if (PMIX_SUCCESS == pmix_event_deregister_observer(cb->ref, op_cbfunc, &lock)) {
            PMIX_WAIT_THREAD(&lock.lock);
        }
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

    /* optional OUT parameters - set them before anything can fail so the
     * caller can read them on an error return, and honor a NULL for either */
    if (NULL != results) {
        *results = NULL;
    }
    if (NULL != nresults) {
        *nresults = 0;
    }

    if (PMIX_UNLIKELY(!pmix_atomic_check_bool(&pmix_globals.initialized))) {
        return PMIX_ERR_INIT;
    }

    /* if we aren't connected, then we cannot notify */
    if (PMIX_UNLIKELY(!pmix_atomic_check_bool(&pmix_globals.connected))) {
        return PMIX_ERR_UNREACH;
    }

    if (PMIX_UNLIKELY(pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped))) {
        return PMIX_ERR_NOT_AVAILABLE;
    }

    /* check for bozo input */
    if (PMIX_UNLIKELY(NULL == grp || NULL == procs || 0 == nprocs)) {
        return PMIX_ERR_BAD_PARAM;
    }

    cb = PMIX_NEW(pmix_group_tracker_t);
    if (PMIX_UNLIKELY(NULL == cb)) {
        return PMIX_ERR_NOMEM;
    }
    /* leave cb->cbfunc NULL: this is the blocking form, so we wait on the
     * tracker's lock rather than being handed the result via a callback */
    rc = invite_setup(cb, grp, procs, nprocs, info, ninfo);
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        invite_teardown(cb);
        return rc;
    }

    /* Wait for the invitation to resolve and be announced. Everything from
     * "every invitee answered (or the timeout fired)" through the
     * PMIX_GROUP_CONSTRUCT_COMPLETE broadcast now happens on the progress
     * thread (invite_wake -> announce_step), which wakes us at the end of
     * that chain with the outcome on the tracker. */
    PMIX_WAIT_THREAD(&cb->lock);
    rc = cb->status;

    /* deregister the invitation observer now that the invite has resolved (so
     * a late response cannot fire it against the released tracker) and release
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

    if (PMIX_UNLIKELY(!pmix_atomic_check_bool(&pmix_globals.initialized))) {
        return PMIX_ERR_INIT;
    }

    /* if we aren't connected, then we cannot notify */
    if (PMIX_UNLIKELY(!pmix_atomic_check_bool(&pmix_globals.connected))) {
        return PMIX_ERR_UNREACH;
    }

    if (PMIX_UNLIKELY(pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped))) {
        return PMIX_ERR_NOT_AVAILABLE;
    }

    /* check for bozo input. A NULL cbfunc is rejected rather than treated as
     * the blocking form: there would be no way to report the outcome, and
     * nothing would ever release the tracker or the observer. */
    if (PMIX_UNLIKELY(NULL == grp || NULL == procs || 0 == nprocs || NULL == cbfunc)) {
        return PMIX_ERR_BAD_PARAM;
    }

    cb = PMIX_NEW(pmix_group_tracker_t);
    if (PMIX_UNLIKELY(NULL == cb)) {
        return PMIX_ERR_NOMEM;
    }
    cb->cbfunc = cbfunc;
    cb->cbdata = cbdata;
    rc = invite_setup(cb, grp, procs, nprocs, info, ninfo);
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        /* setup runs on this thread, so the teardown may be waited out here.
         * The announcement never started, so nothing else holds the tracker. */
        invite_teardown(cb);
    }
    return rc;
}

/* ---- invitee-side construct watch ---------------------------------------
 *
 * A process that accepts an invitation is depending on the leader to drive the
 * construct to completion. This watch is what connects it to that outcome, and
 * it does two jobs.
 *
 * First, it completes the pending PMIx_Group_join. The man page says the call
 * returns "once the group has been completely constructed or its construction
 * has failed (as determined by the leader)", with the construct's results
 * available - and the construct's outcome reaches an acceptor as an event, so
 * this is where the join is completed: PMIX_GROUP_CONSTRUCT_COMPLETE completes
 * it successfully and hands back the group id and membership,
 * PMIX_GROUP_CONSTRUCT_ABORT completes it with PMIX_GROUP_CONSTRUCT_ABORT, and
 * losing the leader completes it with PMIX_GROUP_LEADER_FAILED. Before this,
 * join completed as soon as its accept/decline notification had been handed to
 * the local event system, which is much earlier and carries no group data, so
 * results/nresults always came back empty.
 *
 * Second, if the leader is lost before the construct resolves - the acceptor
 * would otherwise wait forever - it delivers a PMIX_GROUP_LEADER_FAILED event
 * to this process's own handlers. Reselection of a new leader is left to the
 * application (it declares a replacement via PMIX_GROUP_LEADER and announces it
 * with PMIX_GROUP_LEADER_SELECTED); the library's job is only to surface the
 * loss.
 *
 * The watch is a pmix_group_tracker_t carrying the group id, the leader (in
 * members[0]) and the join's completion callback; it is handed to its own
 * callback as the return object, so no global registry is needed. The callback
 * runs on the progress thread.
 *
 * It is registered as an internal *observer* (pmix_event_register_observer),
 * not as an event handler. As a handler it was silently suppressible: any
 * application handler for the construct or termination codes that returned
 * PMIX_EVENT_ACTION_COMPLETE - the normal way to say "handled" - ended the
 * chain before the watch was reached, so the safety net never fired and the
 * watch accumulated until finalize. See openpmix#4059. Observers run ahead
 * of the chain and cannot be pre-empted, so the watch now always sees its
 * teardown event and releases on its own terms.
 *
 * The registry owns the tracker: watch_relcb is handed to the registration
 * as the release function, and is called exactly once when the observer goes
 * away - on deregistration, on a failed registration, or at finalize when
 * the event lists are destructed. That is why there is no longer a private
 * list of surviving watches to drain. */

/* Free the heap info used to carry a locally-injected event; used as the
 * completion callback so the info outlives the non-blocking notification. */
static void leader_failed_relcb(pmix_status_t status, void *cbdata)
{
    pmix_info_t *info = (pmix_info_t *) cbdata;
    PMIX_HIDE_UNUSED_PARAMS(status);
    PMIX_INFO_FREE(info, 2);
}

/* Deliver PMIX_GROUP_LEADER_FAILED to this process's own handlers, naming the
 * failed leader and the group. Runs on the progress thread (from the watch
 * handler), so it must use the non-blocking form of PMIx_Notify_event - the
 * blocking form would deadlock here - and let leader_failed_relcb free the info
 * once the local delivery completes. */
static void emit_leader_failed(const char *grpid, const pmix_proc_t *leader)
{
    pmix_info_t *info;
    pmix_status_t rc;

    PMIX_INFO_CREATE(info, 2);
    if (NULL == info) {
        return;
    }
    PMIX_INFO_LOAD(&info[0], PMIX_EVENT_AFFECTED_PROC, leader, PMIX_PROC);
    PMIX_INFO_LOAD(&info[1], PMIX_GROUP_ID, grpid, PMIX_STRING);
    rc = PMIx_Notify_event(PMIX_GROUP_LEADER_FAILED, &pmix_globals.myid,
                           PMIX_RANGE_PROC_LOCAL, info, 2, leader_failed_relcb, info);
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        PMIX_INFO_FREE(info, 2);
    }
}

/* Release the watch tracker. Handed to the observer registry as the release
 * function, so it is called exactly once when the registration goes away -
 * including at finalize, which is what retires the old survivor list. */
static void watch_relcb(void *cbdata)
{
    pmix_group_tracker_t *cb = (pmix_group_tracker_t *) cbdata;

    PMIX_RELEASE(cb);
}

/* Tear down the watch. Deregistration threadshifts, so it is safe to call
 * from inside the observer callback; the tracker is released by watch_relcb
 * when the registration is actually removed. */
static void watch_teardown(pmix_group_tracker_t *cb)
{
    if (SIZE_MAX != cb->ref) {
        pmix_event_deregister_observer(cb->ref, NULL, NULL);
        cb->ref = SIZE_MAX;
    }
    /* if the registration id is not known yet, the ack has not come back -
     * watch_regcb sees cb->completed and tears down as soon as it can name
     * the observer */
}

/* Complete the PMIx_Group_join this watch was set up for, if it has not been
 * completed already, handing back whatever the construct reported about the
 * group. Only the entries that describe the group are passed on - the event
 * also carries our own plumbing (the custom range that aimed it at the
 * members, and the non-default marker), and presenting those to the caller as
 * "results" of the join would be presenting event directives as group data. */
static void join_complete(pmix_group_tracker_t *cb, pmix_status_t status,
                          const pmix_info_t info[], size_t ninfo)
{
    pmix_info_t *results = NULL;
    size_t n, nresults = 0;

    if (NULL == cb->cbfunc) {
        return;
    }

    if (NULL != info && 0 < ninfo) {
        for (n = 0; n < ninfo; n++) {
            if (PMIX_CHECK_KEY(&info[n], PMIX_GROUP_ID) ||
                PMIX_CHECK_KEY(&info[n], PMIX_GROUP_MEMBERSHIP) ||
                PMIX_CHECK_KEY(&info[n], PMIX_GROUP_CONTEXT_ID)) {
                ++nresults;
            }
        }
    }
    if (0 < nresults) {
        PMIX_INFO_CREATE(results, nresults);
        if (PMIX_UNLIKELY(NULL == results)) {
            nresults = 0;
        }
    }
    if (NULL != results) {
        nresults = 0;
        for (n = 0; n < ninfo; n++) {
            if (PMIX_CHECK_KEY(&info[n], PMIX_GROUP_ID) ||
                PMIX_CHECK_KEY(&info[n], PMIX_GROUP_MEMBERSHIP) ||
                PMIX_CHECK_KEY(&info[n], PMIX_GROUP_CONTEXT_ID)) {
                PMIX_INFO_XFER(&results[nresults], &info[n]);
                ++nresults;
            }
        }
    }

    /* the callback copies what it wants; free our copy once it returns */
    cb->cbfunc(status, results, nresults, cb->cbdata, NULL, NULL);
    cb->cbfunc = NULL;
    if (NULL != results) {
        PMIX_INFO_FREE(results, nresults);
    }
}

/* Watch observer: fires on a termination-type event (to catch the leader's
 * loss) or on the construct resolving (to complete the join and end the
 * watch). Runs ahead of the event chain, so unlike the handler this replaced
 * it cannot be pre-empted - and, having no return value, it cannot suppress
 * the application's own handlers for the same event either. */
static void leader_watch_observer(pmix_status_t status, const pmix_proc_t *source,
                                  const pmix_info_t info[], size_t ninfo,
                                  const pmix_proc_t *affected, size_t naffected,
                                  void *cbobject)
{
    pmix_group_tracker_t *cb = (pmix_group_tracker_t *) cbobject;
    const pmix_proc_t *departed = NULL;
    const char *grpid = NULL;
    size_t n;
    PMIX_HIDE_UNUSED_PARAMS(source);

    if (PMIX_UNLIKELY(NULL == cb || cb->completed)) {
        return;
    }

    /* the affected proc reaches us either as the chain's affected list or as
     * a directive in the info array, depending on how it was generated */
    if (NULL != affected && 0 < naffected) {
        departed = affected;
    }
    for (n = 0; n < ninfo; n++) {
        if (PMIX_CHECK_KEY(&info[n], PMIX_EVENT_AFFECTED_PROC)) {
            departed = info[n].value.data.proc;
        } else if (PMIX_CHECK_KEY(&info[n], PMIX_GROUP_ID)) {
            grpid = info[n].value.data.string;
        }
    }

    /* the construct resolved - this is what the join was waiting for, and the
     * leader is no longer critical, so complete the caller and end the watch.
     * A CONSTRUCT_COMPLETE/ABORT with no group id is treated as ours. */
    if (PMIX_GROUP_CONSTRUCT_COMPLETE == status || PMIX_GROUP_CONSTRUCT_ABORT == status) {
        if (NULL == grpid || 0 == strcmp(grpid, cb->grpid)) {
            cb->completed = true;
            /* the group itself was already recorded in
             * pmix_client_globals.groups by the bookkeeping that runs at the
             * top of pmix_invoke_local_event_hdlr, ahead of this sweep */
            join_complete(cb,
                          (PMIX_GROUP_CONSTRUCT_COMPLETE == status) ? PMIX_SUCCESS
                                                                    : PMIX_GROUP_CONSTRUCT_ABORT,
                          info, ninfo);
            watch_teardown(cb);
        }
        return;
    }

    /* a termination-type event - if the departed proc is our leader, surface
     * the loss as PMIX_GROUP_LEADER_FAILED, complete the join with that same
     * status (the construct will never resolve now, and the man page's "or
     * its construction has failed" is exactly this case), and end the watch */
    if (PMIX_PROC_TERMINATED == status || PMIX_ERR_PROC_ABORTED == status ||
        PMIX_ERR_PROC_TERM_WO_SYNC == status) {
        if (NULL != departed && PMIX_CHECK_PROCID(departed, &cb->members[0])) {
            cb->completed = true;
            emit_leader_failed(cb->grpid, &cb->members[0]);
            join_complete(cb, PMIX_GROUP_LEADER_FAILED, NULL, 0);
            watch_teardown(cb);
        }
    }
}

/* Registration callback for the leader watch. Runs on the progress thread
 * when the observer has been registered; caches the returned id on the
 * tracker so it can later be deregistered. On failure there is nothing to
 * release - the registry has already discharged watch_relcb. */
static void watch_regcb(pmix_status_t status, size_t refid, void *cbdata)
{
    pmix_group_tracker_t *cb = (pmix_group_tracker_t *) cbdata;

    if (PMIX_UNLIKELY(PMIX_SUCCESS != status)) {
        return;
    }
    cb->ref = refid;
    /* the watch can fire in the window between being placed on the observer
     * list and this ack arriving. If it already finished, tear it down now
     * that we can name it - watch_teardown could not do so at the time. */
    if (cb->completed) {
        watch_teardown(cb);
    }
    PMIX_POST_OBJECT(cb);
}

/* Register the construct watch for a process that has accepted an invitation,
 * carrying the join's completion callback so the construct's outcome can be
 * reported to it. This is normally reached from within the application's
 * PMIX_GROUP_INVITED handler (PMIx_Group_join_nb called from an event
 * handler), so it runs on the progress thread - the registration MUST
 * therefore be non-blocking; a blocking wait here would deadlock the progress
 * thread. Ownership of the tracker passes to the observer registry, which
 * discharges watch_relcb on every exit including finalize.
 *
 * Returns PMIX_SUCCESS only if the watch was accepted, because the caller
 * hands it responsibility for completing the join: if this fails the caller
 * must complete the join itself rather than leave it pending forever. */
static pmix_status_t setup_leader_watch(const char *grp, const pmix_proc_t *leader,
                                        pmix_info_cbfunc_t cbfunc, void *cbdata)
{
    pmix_group_tracker_t *cb;
    pmix_status_t codes[] = {
        PMIX_PROC_TERMINATED,
        PMIX_ERR_PROC_ABORTED,
        PMIX_ERR_PROC_TERM_WO_SYNC,
        PMIX_GROUP_CONSTRUCT_COMPLETE,
        PMIX_GROUP_CONSTRUCT_ABORT
    };
    size_t ncodes = sizeof(codes) / sizeof(codes[0]);
    pmix_status_t rc;

    cb = PMIX_NEW(pmix_group_tracker_t);
    if (PMIX_UNLIKELY(NULL == cb)) {
        return PMIX_ERR_NOMEM;
    }
    cb->grpid = strdup(grp);
    PMIX_PROC_CREATE(cb->members, 1);
    if (PMIX_UNLIKELY(NULL == cb->members)) {
        PMIX_RELEASE(cb);
        return PMIX_ERR_NOMEM;
    }
    cb->nmembers = 1;
    PMIX_LOAD_PROCID(&cb->members[0], leader->nspace, leader->rank);
    /* the join is completed from the watch, once the construct resolves */
    cb->cbfunc = cbfunc;
    cb->cbdata = cbdata;

    rc = pmix_event_register_observer("pmix-group-construct-watch", codes, ncodes,
                                      leader_watch_observer, cb, watch_relcb,
                                      watch_regcb, cb);
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        /* the registration was never accepted, so watch_relcb was not
         * discharged and the tracker is still ours */
        PMIX_RELEASE(cb);
    }
    return rc;
}

PMIX_EXPORT pmix_status_t PMIx_Group_join(const char grp[], const pmix_proc_t *leader,
                                          pmix_group_opt_t opt, const pmix_info_t info[],
                                          size_t ninfo, pmix_info_t **results, size_t *nresults)
{
    pmix_status_t rc;
    pmix_group_tracker_t *cb;

    /* Set the default response before anything can fail. These are OUT
     * parameters the caller is entitled to read on return, and both are
     * documented as optional, so honor a NULL for either.
     *
     * An accepted join now returns when the construct resolves, as the man
     * page has always said it should, so on success these carry the group id
     * and membership the leader announced. A declined join, or one that named
     * no leader, still returns as soon as its notification is away - there is
     * no construct outcome for it to wait on - and returns them empty. */
    if (NULL != results) {
        *results = NULL;
    }
    if (NULL != nresults) {
        *nresults = 0;
    }

    if (PMIX_UNLIKELY(!pmix_atomic_check_bool(&pmix_globals.initialized))) {
        return PMIX_ERR_INIT;
    }

    /* if we aren't connected, don't attempt to send */
    if (PMIX_UNLIKELY(!pmix_atomic_check_bool(&pmix_globals.connected))) {
        return PMIX_ERR_UNREACH;
    }

    if (PMIX_UNLIKELY(pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped))) {
        return PMIX_ERR_NOT_AVAILABLE;
    }

    /* check for bozo input - the group ID names the group we are joining */
    if (PMIX_UNLIKELY(NULL == grp)) {
        return PMIX_ERR_BAD_PARAM;
    }

    /* create a callback object as we need to pass it to the
     * recv routine so we know which lock to release when
     * the return message is recvd */
    cb = PMIX_NEW(pmix_group_tracker_t);

    rc = PMIx_Group_join_nb(grp, leader, opt, info, ninfo, info_cbfunc, cb);
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        PMIX_RELEASE(cb);
        return rc;
    }

    /* wait for the group construction to complete */
    PMIX_WAIT_THREAD(&cb->lock);
    rc = cb->status;
    /* hand over whatever the construct reported about the group */
    if (PMIX_SUCCESS == rc && NULL != results && NULL != nresults) {
        *results = cb->results;
        *nresults = cb->nresults;
        cb->results = NULL;
        cb->nresults = 0;
    }
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
    pmix_data_range_t range;
    bool waitsconstruct;
    /* join accepts no directives today - the accept/decline notification it
     * issues carries only the leader's address. A PMIX_TIMEOUT here used to
     * be recognized and then discarded by a loop that did nothing with it;
     * saying so plainly is better than pretending to honor it. */
    PMIX_HIDE_UNUSED_PARAMS(info, ninfo);

    pmix_output_verbose(2, pmix_client_globals.group_output,
                        "[%s:%d] pmix: join nb called",
                        pmix_globals.myid.nspace, pmix_globals.myid.rank);

    if (PMIX_UNLIKELY(!pmix_atomic_check_bool(&pmix_globals.initialized))) {
        return PMIX_ERR_INIT;
    }

    /* if we aren't connected, then we cannot notify */
    if (PMIX_UNLIKELY(!pmix_atomic_check_bool(&pmix_globals.connected))) {
        return PMIX_ERR_UNREACH;
    }

    if (PMIX_UNLIKELY(pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped))) {
        return PMIX_ERR_NOT_AVAILABLE;
    }

    /* check for bozo input - setup_leader_watch() below strdup's this, and
     * every sibling group API screens it */
    if (PMIX_UNLIKELY(NULL == grp)) {
        return PMIX_ERR_BAD_PARAM;
    }

    /* create a callback object as we need to pass it to the
     * recv routine so we know which lock to release when
     * the notification is done */
    cb = PMIX_NEW(pmix_group_tracker_t);
    if (PMIX_UNLIKELY(NULL == cb)) {
        return PMIX_ERR_NOMEM;
    }

    /* set the code according to their request */
    if (PMIX_GROUP_ACCEPT == opt) {
        code = PMIX_GROUP_INVITE_ACCEPTED;
    } else {
        code = PMIX_GROUP_INVITE_DECLINED;
    }

    /* Decide up front who completes the caller, because once the notification
     * below is away its completion can fire at any moment and we can no
     * longer safely change our mind.
     *
     * An acceptor that named a leader depends on that leader to drive the
     * construct, and the man page says this call completes when the construct
     * does - so the *watch* completes it, from the construct event or from
     * the leader's loss, and this tracker is left with nothing to complete.
     * A decline is not waiting for anything, and neither is an acceptance
     * with no leader to watch; those complete when the notification is away,
     * as they always did. */
    waitsconstruct = (PMIX_GROUP_ACCEPT == opt && NULL != leader);
    if (!waitsconstruct) {
        cb->cbfunc = cbfunc;
        cb->cbdata = cbdata;
    }

    /* only notify the leader so we don't hit all procs */
    if (NULL != leader) {
        range = PMIX_RANGE_CUSTOM;
        PMIX_INFO_CREATE(cb->info, 1);
        if (PMIX_UNLIKELY(NULL == cb->info)) {
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
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        /* nothing was sent, so nothing will complete - and an error return
         * from an _nb entry point means no callback is coming */
        PMIX_RELEASE(cb);
        return rc;
    }

    if (waitsconstruct) {
        /* Set up the watch that will complete this join. There is a window
         * here - the acceptance is already away, and the watch is not yet
         * registered - but the leader cannot resolve the construct until it
         * has our answer, so a PMIX_GROUP_CONSTRUCT_COMPLETE would have to
         * make a full round trip through the leader to beat a registration
         * that is already queued on the same server connection. Should it
         * ever lose that race anyway, the registration replays matching
         * cached notifications as it completes. */
        rc = setup_leader_watch(grp, leader, cbfunc, cbdata);
        if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
            /* with no watch nothing would ever complete the caller, so report
             * the failure through the return value instead - which, as above,
             * means no callback is coming */
            return rc;
        }
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

    if (PMIX_UNLIKELY(!pmix_atomic_check_bool(&pmix_globals.initialized))) {
        return PMIX_ERR_INIT;
    }

    /* if we aren't connected, don't attempt to send */
    if (PMIX_UNLIKELY(!pmix_atomic_check_bool(&pmix_globals.connected))) {
        return PMIX_ERR_UNREACH;
    }

    if (PMIX_UNLIKELY(pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped))) {
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
    size_t n, m, nrange, nmbrs;

    pmix_output_verbose(2, pmix_client_globals.group_output,
                        "pmix:group_leave_nb called");

    if (PMIX_UNLIKELY(!pmix_atomic_check_bool(&pmix_globals.initialized))) {
        return PMIX_ERR_INIT;
    }

    /* if we aren't connected, then we cannot notify */
    if (PMIX_UNLIKELY(!pmix_atomic_check_bool(&pmix_globals.connected))) {
        return PMIX_ERR_UNREACH;
    }

    if (PMIX_UNLIKELY(pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped))) {
        return PMIX_ERR_NOT_AVAILABLE;
    }

    /* check for bozo input */
    if (PMIX_UNLIKELY(NULL == grp)) {
        return PMIX_ERR_BAD_PARAM;
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
    if (PMIX_UNLIKELY(NULL == cb->info)) {
        PMIX_RELEASE(cb);
        return PMIX_ERR_NOMEM;
    }

    /* Find this group in our local list - we can only leave a group we
     * belong to, and we need its membership to know who to notify - copy
     * that membership out, and drop the group, all under the lock. The
     * progress thread owns this list too, and the PMIX_GROUP_LEFT handler
     * on that side shifts members out of exactly this array.
     *
     * Everything that can fail and everything that can block is kept
     * outside the lock: the allocations above are already done, and the
     * notification below must not be issued while holding it - the handler
     * it drives needs this same lock. */
    pmix_mutex_lock(&pmix_client_globals.grouplock);
    grpobj = NULL;
    PMIX_LIST_FOREACH(pgrp, &pmix_client_globals.groups, pmix_group_t) {
        if (0 == strcmp(grp, pgrp->grpid)) {
            grpobj = pgrp;
            break;
        }
    }
    if (PMIX_UNLIKELY(NULL == grpobj)) {
        pmix_mutex_unlock(&pmix_client_globals.grouplock);
        PMIX_RELEASE(cb);
        return PMIX_ERR_NOT_FOUND;
    }

    /* the custom range is the membership, excluding ourselves */
    nmbrs = grpobj->nmbrs;
    PMIX_PROC_CREATE(range, nmbrs);
    if (PMIX_UNLIKELY(NULL == range)) {
        pmix_mutex_unlock(&pmix_client_globals.grouplock);
        PMIX_RELEASE(cb);
        return PMIX_ERR_NOMEM;
    }
    nrange = 0;
    for (m = 0; m < nmbrs; m++) {
        if (PMIX_CHECK_PROCID(&grpobj->members[m], &pmix_globals.myid)) {
            continue;
        }
        PMIX_XFER_PROCID(&range[nrange], &grpobj->members[m]);
        ++nrange;
    }

    /* we are no longer a member - drop the group from our local list */
    pmix_list_remove_item(&pmix_client_globals.groups, &grpobj->super);
    PMIX_RELEASE(grpobj);
    pmix_mutex_unlock(&pmix_client_globals.grouplock);

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
    PMIX_PROC_FREE(range, nmbrs);

    /* generate the event - the callback fires once it has been locally
     * generated, which is when the operation is complete */
    rc = PMIx_Notify_event(PMIX_GROUP_LEFT, &pmix_globals.myid, PMIX_RANGE_CUSTOM,
                           cb->info, cb->ninfo, leave_cbfunc, (void *) cb);
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
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
        /* Report no results, not cb->info: the info on this tracker is the
         * custom range we used to aim the accept/decline notification at the
         * leader. That is our own event plumbing, and handing it back as the
         * "results" of the join would present a PMIX_EVENT_CUSTOM_RANGE to
         * the caller as if it were group data. There genuinely are no
         * results at this point - see PMIx_Group_join. */
        cb->cbfunc(status, NULL, 0, cb->cbdata, relfn, cb);
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

    if (PMIX_UNLIKELY(PMIX_SUCCESS != ret)) {
        goto report;
    }

    /* unpack the final membership */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, pmix_client_globals.myserver, buf, &nmembers, &cnt, PMIX_SIZE);
    if (PMIX_ERR_UNPACK_READ_PAST_END_OF_BUFFER == rc) {
        goto report;
    } else if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        PMIX_ERROR_LOG(rc);
        ret = rc;
        goto report;
    }
    if (0 < nmembers) {
        PMIX_PROC_CREATE(members, nmembers);
        cnt = nmembers;
        PMIX_BFROPS_UNPACK(rc, pmix_client_globals.myserver, buf, members, &cnt, PMIX_PROC);
        if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
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
    } else if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        PMIX_ERROR_LOG(rc);
        ret = rc;
        goto report;
    }
    if (gotctxid) {
        cnt = 1;
        PMIX_BFROPS_UNPACK(rc, pmix_client_globals.myserver, buf, &ctxid, &cnt, PMIX_SIZE);
        if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
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
    } else if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        PMIX_ERROR_LOG(rc);
        ret = rc;
        goto report;
    }
    if (0 < ngrpinfo && gotctxid) {
        for (n=0; n < ngrpinfo; n++) {
            PMIX_INFO_CONSTRUCT(&grpinfo);
            cnt = 1;
            PMIX_BFROPS_UNPACK(rc, pmix_client_globals.myserver, buf, &grpinfo, &cnt, PMIX_INFO);
            if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
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
                if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
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
                    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
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
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        PMIx_Info_list_release(ilist);
        goto done;
    }
    if (NULL != members) {
        darray.array = members;
        darray.size = nmembers;
        darray.type = PMIX_PROC;
        rc = PMIx_Info_list_add(ilist, PMIX_GROUP_MEMBERSHIP, &darray, PMIX_DATA_ARRAY);
        // data was copied into the info
        if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
            PMIx_Info_list_release(ilist);
            goto done;
        }
    }
    if (gotctxid) {
        rc = PMIx_Info_list_add(ilist, PMIX_GROUP_CONTEXT_ID, &ctxid, PMIX_SIZE);
        if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
            PMIx_Info_list_release(ilist);
            goto done;
        }
    }
    /* on failure darray is left untouched, so adopting it unchecked handed
     * the tracker an uninitialized pointer that its destructor then freed */
    rc = PMIx_Info_list_convert(ilist, &darray);
    if (PMIX_SUCCESS == rc) {
        cb->info = (pmix_info_t*)darray.array;
        cb->ninfo = darray.size;
    }
    PMIx_Info_list_release(ilist);

    /* record the group locally now that its construction has succeeded.
     * This must happen here, on the completion path shared by both forms
     * of the API, and not in the callback the blocking wrapper installs -
     * a caller of PMIx_Group_construct_nb supplies its own callback, and
     * would otherwise never get the group registered, leaving every
     * subsequent leave/destruct to fail with PMIX_ERR_NOT_FOUND */
    if (PMIX_SUCCESS == ret && NULL != members) {
        add_group(cb->grpid, ctxid, cb->notterm, members, nmembers);
    }

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

    /* Find this group and drop it - the caller's thread reads this list too
     * (see the grouplock note in pmix_client_ops.h). This happens whatever
     * the reply turns out to be: we asked to destruct, so we are done with
     * the group either way, and leaving it registered on one failure path
     * but not the other left the two disagreeing. */
    grp = NULL;
    pmix_mutex_lock(&pmix_client_globals.grouplock);
    PMIX_LIST_FOREACH(grp, &pmix_client_globals.groups, pmix_group_t) {
        if (0 == strcmp(cb->grpid, grp->grpid)) {
            pmix_list_remove_item(&pmix_client_globals.groups, &grp->super);
            PMIX_RELEASE(grp);
            break;
        }
    }
    pmix_mutex_unlock(&pmix_client_globals.grouplock);

    if (PMIX_UNLIKELY(NULL == buf)) {
        ret = PMIX_ERR_BAD_PARAM;
        PMIX_ERROR_LOG(ret);
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
                if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
                    PMIX_ERROR_LOG(rc);
                }
            }
            PMIX_INFO_XFER(&cb->results[n], &info[n]);
        }
    }
    if (NULL != members && NULL != grpid) {
        add_group(grpid, ctxid, cb->notterm, members, nmembers);
    }

    if (NULL != release_fn) {
        release_fn(release_cbdata);
    }
    PMIX_POST_OBJECT(cb);
    PMIX_WAKEUP_THREAD(&cb->lock);
}

static pmix_status_t add_group(const char *grpid,
                               size_t ctxid, bool notterm,
                               pmix_proc_t *members, size_t nmembers)
{
    pmix_group_t *grp;

    /* the caller's thread reads this list, so the lookup and the append have
     * to be one atomic step - two racing completions would otherwise both
     * miss and both append */
    pmix_mutex_lock(&pmix_client_globals.grouplock);

    /* the construct completion path registers the group for every caller,
     * while the callback the blocking wrapper installs still does so for
     * the join path - so tolerate being asked twice for the same group
     * rather than leaving a duplicate on the list */
    PMIX_LIST_FOREACH(grp, &pmix_client_globals.groups, pmix_group_t) {
        if (0 == strcmp(grpid, grp->grpid)) {
            pmix_mutex_unlock(&pmix_client_globals.grouplock);
            return PMIX_SUCCESS;
        }
    }

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
    /* remember the construct-time failure policy so the later destruct can
     * re-apply PMIX_GROUP_NOTIFY_TERMINATION on the caller's behalf */
    grp->notterm = notterm;
    pmix_list_append(&pmix_client_globals.groups, &grp->super);
    pmix_mutex_unlock(&pmix_client_globals.grouplock);

    return PMIX_SUCCESS;
}

