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
#include "src/runtime/pmix_progress_threads.h"
#include "src/util/pmix_output.h"

#include "pmix_client_ops.h"

/* callback for wait completion */
static void wait_cbfunc(struct pmix_peer_t *pr, pmix_ptl_hdr_t *hdr, pmix_buffer_t *buf,
                        void *cbdata);
static void disconnect_cbfunc(struct pmix_peer_t *pr, pmix_ptl_hdr_t *hdr, pmix_buffer_t *buf,
                              void *cbdata);
static void op_cbfunc(pmix_status_t status, void *cbdata);

/* Append one of the connect message's OPTIONAL trailing info blobs - the
 * caller's endpoint data, or the job-level data its namespace contributes.
 *
 * Optional is meant literally, and the server relies on it: it reads each of
 * these with a trailing unpack and treats
 * PMIX_ERR_UNPACK_READ_PAST_END_OF_BUFFER as "the client sent none" (see
 * pmix_server_connect). A peer whose wire format cannot represent the blob
 * must therefore still be able to connect - and such peers exist. Both blobs
 * are a PMIX_DATA_ARRAY of PMIX_INFO, and a pre-v4 peer's bfrops has no
 * registered array-element handler for PMIX_INFO, so the array cannot be
 * packed for it at all.
 *
 * So pack through a scratch buffer and append only what packed cleanly.
 * Packing straight into the message could not do that: the element type and
 * count are written before the elements themselves fail, leaving a
 * half-encoded info on the wire for the server to trip over and skip.
 *
 * Returns PMIX_SUCCESS whether or not the blob was appended - being unable
 * to express it for this peer is not an error. Only failing to append
 * something we did manage to pack is. */
static pmix_status_t append_optional(pmix_buffer_t *msg, pmix_info_t *xfer)
{
    pmix_buffer_t pbkt;
    pmix_status_t rc;

    PMIX_CONSTRUCT(&pbkt, pmix_buffer_t);
    PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, &pbkt, xfer, 1, PMIX_INFO);
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        pmix_output_verbose(2, pmix_client_globals.connect_output,
                            "pmix:connect omitting %s: this peer cannot represent it (%s)",
                            xfer->key, PMIx_Error_string(rc));
        PMIX_DESTRUCT(&pbkt);
        return PMIX_SUCCESS;
    }
    PMIX_BFROPS_COPY_PAYLOAD(rc, pmix_client_globals.myserver, msg, &pbkt);
    PMIX_DESTRUCT(&pbkt);
    return rc;
}

PMIX_EXPORT pmix_status_t PMIx_Connect(const pmix_proc_t procs[], size_t nprocs,
                                       const pmix_info_t info[], size_t ninfo)
{
    pmix_status_t rc;
    pmix_cb_t *cb;

    pmix_output_verbose(2, pmix_client_globals.connect_output,
                        "pmix: connect called");

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

    /* what would release us runs on the progress thread, so waiting
     * for it from that thread waits for ourselves */
    if (PMIX_UNLIKELY(pmix_progress_thread_check_blocking("PMIx_Connect"))) {
        return PMIX_ERR_WOULD_BLOCK;
    }

    /* create a callback object as we need to pass it to the
     * recv routine so we know which callback to use when
     * the return message is recvd */
    cb = PMIX_NEW(pmix_cb_t);
    if (PMIX_UNLIKELY(NULL == cb)) {
        return PMIX_ERR_NOMEM;
    }

    /* push the message into our event base to send to the server */
    if (PMIX_UNLIKELY(PMIX_SUCCESS != (rc = PMIx_Connect_nb(procs, nprocs, info, ninfo,
                                                             op_cbfunc, cb)))) {
        PMIX_RELEASE(cb);
        return rc;
    }

    /* wait for the connect to complete */
    PMIX_WAIT_THREAD(&cb->lock);
    rc = cb->status;
    PMIX_RELEASE(cb);

    pmix_output_verbose(2, pmix_globals.debug_output, "pmix: connect completed");

    return rc;
}

PMIX_EXPORT pmix_status_t PMIx_Connect_nb(const pmix_proc_t procs[], size_t nprocs,
                                          const pmix_info_t info[], size_t ninfo,
                                          pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    pmix_buffer_t *msg;
    pmix_cmd_t cmd = PMIX_CONNECTNB_CMD;
    pmix_status_t rc;
    pmix_cb_t *cb, cb2;
    pmix_info_t xfer;
    pmix_kval_t *kv;
    void *ilist;
    pmix_data_array_t darray;
    bool found;
    size_t n;
    pmix_nspace_t nspace;
    pmix_rank_t minrank;
    pmix_proc_t proc;
    pmix_proc_t *rgs;
    size_t nrg;

    pmix_output_verbose(2, pmix_client_globals.connect_output,
                        "pmix:connect_nb called");

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
    if (PMIX_UNLIKELY(NULL == procs || 0 >= nprocs)) {
        return PMIX_ERR_BAD_PARAM;
    }

    /* if any of the participants are referencing a PMIx group, then
     * replace that group with the actual member proc(s) */
    rc = pmix_client_convert_group_procs(procs, nprocs, &rgs, &nrg);
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        return rc;
    }

    /* PMIx_Connect requires that all participants be listed in the
     * input array, so verify that the calling process is among them */
    if (!pmix_client_proc_is_included(rgs, nrg)) {
        PMIX_PROC_FREE(rgs, nrg);
        return PMIX_ERR_NOT_A_MEMBER;
    }

    msg = PMIX_NEW(pmix_buffer_t);
    if (PMIX_UNLIKELY(NULL == msg)) {
        /* PMIX_BFROPS_PACK reads the buffer's type before it does
         * anything else, so an unchecked failure here is a segfault */
        PMIX_PROC_FREE(rgs, nrg);
        return PMIX_ERR_NOMEM;
    }
    /* pack the cmd */
    PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, msg, &cmd, 1, PMIX_COMMAND);
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(msg);
        PMIX_PROC_FREE(rgs, nrg);
        return rc;
    }

    /* pack the number of procs */
    PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, msg, &nrg, 1, PMIX_SIZE);
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(msg);
        PMIX_PROC_FREE(rgs, nrg);
        return rc;
    }
    PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, msg, rgs, nrg, PMIX_PROC);
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(msg);
        PMIX_PROC_FREE(rgs, nrg);
        return rc;
    }

    /* pack the info structs */
    PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, msg, &ninfo, 1, PMIX_SIZE);
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(msg);
        PMIX_PROC_FREE(rgs, nrg);
        return rc;
    }
    if (0 < ninfo) {
        PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, msg, info, ninfo, PMIX_INFO);
        if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
            PMIX_ERROR_LOG(rc);
            PMIX_RELEASE(msg);
            PMIX_PROC_FREE(rgs, nrg);
            return rc;
        }
    }

    /* get our endpt info, if some was posted. We use
     * "remote" scope as all local procs have access
     * to info posted by all other local procs, regardless
     * of their namespace */
    PMIX_CONSTRUCT(&cb2, pmix_cb_t);
    cb2.proc = &pmix_globals.myid;
    cb2.scope = PMIX_REMOTE;
    cb2.copy = true;
    /* we are on the caller's thread, and this is the table _putfn writes
     * on the progress thread */
    rc = pmix_gds_base_fetch_kv_tsafe(pmix_globals.mypeer, &cb2);
    if (PMIX_SUCCESS == rc) {
        ilist = PMIx_Info_list_start();
        // start with our procID
        rc = PMIx_Info_list_add(ilist, PMIX_PROCID, &pmix_globals.myid, PMIX_PROC);
        // now add the kvals
        found = false;
        if (PMIX_SUCCESS == rc) {
            PMIX_LIST_FOREACH (kv, &cb2.kvs, pmix_kval_t) {
                if (PMIx_Check_reserved_key(kv->key)) {
                    continue;
                }
                rc = PMIx_Info_list_add_value_unique(ilist, kv->key, kv->value, true);
                if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
                    break;
                }
                found = true;
            }
        }
        /* a key dropped here is one this peer never publishes to the
         * processes it is connecting to, and nothing downstream can tell
         * that from our having posted nothing at all - so stop rather
         * than send a blob that is quietly short */
        if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
            PMIX_ERROR_LOG(rc);
            PMIX_RELEASE(msg);
            PMIx_Info_list_release(ilist);
            PMIX_DESTRUCT(&cb2);
            PMIX_PROC_FREE(rgs, nrg);
            return rc;
        }
        if (found) {
            // convert to array
            rc = PMIx_Info_list_convert(ilist, &darray);
            if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
                PMIX_ERROR_LOG(rc);
                PMIX_RELEASE(msg);
                PMIx_Info_list_release(ilist);
                PMIX_DESTRUCT(&cb2);
                PMIX_PROC_FREE(rgs, nrg);
                return rc;
            }
            // insert into a pmix_info_t for packing
            rc = PMIx_Info_load(&xfer, PMIX_PROC_INFO_ARRAY, &darray, PMIX_DATA_ARRAY);
            PMIX_DATA_ARRAY_DESTRUCT(&darray);
            if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
                /* the load copies the array, and it is the copy that goes
                 * on the wire. PMIX_INFO_LOAD discards this status, which
                 * left a failed copy packing an info whose array is NULL -
                 * indistinguishable, to the server, from our having posted
                 * nothing */
                PMIX_ERROR_LOG(rc);
                PMIX_INFO_DESTRUCT(&xfer);
                PMIX_RELEASE(msg);
                PMIx_Info_list_release(ilist);
                PMIX_DESTRUCT(&cb2);
                PMIX_PROC_FREE(rgs, nrg);
                return rc;
            }
            // append it if this peer can carry it
            rc = append_optional(msg, &xfer);
            PMIX_INFO_DESTRUCT(&xfer);
            if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
                PMIX_ERROR_LOG(rc);
                PMIX_RELEASE(msg);
                PMIx_Info_list_release(ilist);
                PMIX_DESTRUCT(&cb2);
                PMIX_PROC_FREE(rgs, nrg);
                return rc;
            }
        }
        PMIx_Info_list_release(ilist);
    }
    PMIX_DESTRUCT(&cb2);

    /* if this operation involves multiple namespaces, then we need to
     * share job-level info between the participants. We only need to
     * add it once per namespace, so have the lowest participating rank
     * in each namespace add the info */
    PMIX_LOAD_NSPACE(nspace, rgs[0].nspace);
    found = false;
    for (n=1; n < nrg; n++) {
        if (!PMIX_CHECK_NSPACE(nspace, rgs[n].nspace)) {
            found = true;
            break;
        }
    }
    if (found) {
        // see if I am the lowest participating rank from my namespace
        minrank = UINT32_MAX;
        for (n=0; n < nrg; n++) {
            if (PMIX_CHECK_NSPACE(pmix_globals.myid.nspace, rgs[n].nspace)) {
                // this is my nspace - check the rank
                if (PMIX_RANK_WILDCARD == rgs[n].rank) {
                    // all ranks included, so see if I am rank 0
                    if (0 == pmix_globals.myid.rank) {
                        minrank = 0;
                        break;
                    }
                } else {
                    // see if I am the lowest
                    if (rgs[n].rank < minrank) {
                        minrank = rgs[n].rank;
                    }
                }
            }
        }
        if (minrank == pmix_globals.myid.rank) {
            // we will provide the job-level info for our nspace
            PMIX_CONSTRUCT(&cb2, pmix_cb_t);
            PMIX_LOAD_PROCID(&proc, pmix_globals.myid.nspace, PMIX_RANK_WILDCARD);
            cb2.proc = &proc;
            cb2.scope = PMIX_SCOPE_UNDEF;
            cb2.copy = false;
            rc = pmix_gds_base_fetch_kv_tsafe(pmix_client_globals.myserver, &cb2);
            if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
                if (!PMIX_GDS_CHECK_COMPONENT(pmix_client_globals.myserver, "hash")) {
                    /* check the data in my hash module. Hand it an empty
                     * list: a gds fetch may append and then fail, and
                     * everything left on this one is packed below as our
                     * namespace's job-level contribution - so the failed
                     * attempt would put whatever it got as far as onto the
                     * wire alongside the data that actually answered. Same
                     * rule as try_local_fetch() in pmix_client_get.c: a
                     * short-circuit is never a partial one */
                    PMIX_LIST_DESTRUCT(&cb2.kvs);
                    PMIX_CONSTRUCT(&cb2.kvs, pmix_list_t);
                    rc = pmix_gds_base_fetch_kv_tsafe(pmix_globals.mypeer, &cb2);
                    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
                        PMIX_ERROR_LOG(rc);
                        PMIX_DESTRUCT(&cb2);
                        goto moveon;
                    }
                } else {
                    PMIX_ERROR_LOG(rc);
                    PMIX_DESTRUCT(&cb2);
                    goto moveon;
                }
            }
            if (0 < pmix_list_get_size(&cb2.kvs)) {
                // pack to send it along
                ilist = PMIx_Info_list_start();
                // start with our namespace
                rc = PMIx_Info_list_add(ilist, PMIX_NSPACE, pmix_globals.myid.nspace,
                                        PMIX_PROC_NSPACE);
                // now add the kvals
                if (PMIX_SUCCESS == rc) {
                    PMIX_LIST_FOREACH (kv, &cb2.kvs, pmix_kval_t) {
                        rc = PMIx_Info_list_add_value_unique(ilist, kv->key, kv->value, true);
                        if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
                            break;
                        }
                    }
                }
                /* this is the job-level data every other namespace in the
                 * operation receives, and we are the only participant of
                 * ours sending it - a key silently dropped here is one
                 * they never get */
                if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
                    PMIX_ERROR_LOG(rc);
                    PMIX_RELEASE(msg);
                    PMIx_Info_list_release(ilist);
                    PMIX_DESTRUCT(&cb2);
                    PMIX_PROC_FREE(rgs, nrg);
                    return rc;
                }
                // convert to array
                rc = PMIx_Info_list_convert(ilist, &darray);
                if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
                    PMIX_ERROR_LOG(rc);
                    PMIX_RELEASE(msg);
                    PMIx_Info_list_release(ilist);
                    PMIX_DESTRUCT(&cb2);
                    PMIX_PROC_FREE(rgs, nrg);
                    return rc;
                }
                // insert into a pmix_info_t for packing
                rc = PMIx_Info_load(&xfer, PMIX_JOB_INFO_ARRAY, &darray, PMIX_DATA_ARRAY);
                if (PMIX_SUCCESS == rc) {
                    // append it if this peer can carry it
                    rc = append_optional(msg, &xfer);
                }
                PMIX_DATA_ARRAY_DESTRUCT(&darray);
                PMIX_INFO_DESTRUCT(&xfer);
                PMIx_Info_list_release(ilist);
                /* every other pack in this function is checked, and this one
                 * used to go unexamined - so a genuine failure to append data
                 * the server is waiting for passed silently. Note that
                 * "this peer cannot represent it" is NOT such a failure;
                 * append_optional() reports success for that case. */
                if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
                    PMIX_ERROR_LOG(rc);
                    PMIX_RELEASE(msg);
                    PMIX_DESTRUCT(&cb2);
                    PMIX_PROC_FREE(rgs, nrg);
                    return rc;
                }
            }
            /* release the job-info fetch results (the error paths above
             * destruct cb2 before jumping to moveon, so this only runs on
             * the normal path) */
            PMIX_DESTRUCT(&cb2);
        }
    }

moveon:
    /* done with the (possibly group-expanded) participant array */
    PMIX_PROC_FREE(rgs, nrg);

    /* create a callback object as we need to pass it to the
     * recv routine so we know which callback to use when
     * the return message is recvd */
    cb = PMIX_NEW(pmix_cb_t);
    cb->cbfunc.opfn = cbfunc;
    cb->cbdata = cbdata;

    /* push the message into our event base to send to the server */
    PMIX_PTL_SEND_RECV(rc, pmix_client_globals.myserver, msg, wait_cbfunc, (void *) cb);
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        PMIX_RELEASE(msg);
        PMIX_RELEASE(cb);
    }

    return rc;
}

PMIX_EXPORT pmix_status_t PMIx_Disconnect(const pmix_proc_t procs[], size_t nprocs,
                                          const pmix_info_t info[], size_t ninfo)
{
    pmix_status_t rc;
    pmix_cb_t *cb;

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

    /* what would release us runs on the progress thread, so waiting
     * for it from that thread waits for ourselves */
    if (PMIX_UNLIKELY(pmix_progress_thread_check_blocking("PMIx_Disconnect"))) {
        return PMIX_ERR_WOULD_BLOCK;
    }

    /* create a callback object as we need to pass it to the
     * recv routine so we know which callback to use when
     * the return message is recvd */
    cb = PMIX_NEW(pmix_cb_t);
    if (PMIX_UNLIKELY(NULL == cb)) {
        return PMIX_ERR_NOMEM;
    }

    if (PMIX_UNLIKELY(PMIX_SUCCESS != (rc = PMIx_Disconnect_nb(procs, nprocs, info, ninfo,
                                                                op_cbfunc, cb)))) {
        PMIX_RELEASE(cb);
        return rc;
    }

    /* wait for the disconnect to complete */
    PMIX_WAIT_THREAD(&cb->lock);
    rc = cb->status;
    PMIX_RELEASE(cb);

    pmix_output_verbose(2, pmix_globals.debug_output, "pmix: disconnect completed");

    return rc;
}

PMIX_EXPORT pmix_status_t PMIx_Disconnect_nb(const pmix_proc_t procs[], size_t nprocs,
                                             const pmix_info_t info[], size_t ninfo,
                                             pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    pmix_buffer_t *msg;
    pmix_cmd_t cmd = PMIX_DISCONNECTNB_CMD;
    pmix_status_t rc;
    pmix_cb_t *cb;
    pmix_proc_t *rgs;
    size_t nrg;

    pmix_output_verbose(2, pmix_globals.debug_output,
                        "pmix: disconnect called");

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
    if (PMIX_UNLIKELY(NULL == procs || 0 >= nprocs)) {
        return PMIX_ERR_BAD_PARAM;
    }

    /* if any of the participants are referencing a PMIx group, then
     * replace that group with the actual member proc(s) */
    rc = pmix_client_convert_group_procs(procs, nprocs, &rgs, &nrg);
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        return rc;
    }

    /* PMIx_Disconnect requires that all participants be listed in the
     * input array, so verify that the calling process is among them */
    if (!pmix_client_proc_is_included(rgs, nrg)) {
        PMIX_PROC_FREE(rgs, nrg);
        return PMIX_ERR_NOT_A_MEMBER;
    }

    /* Note what does NOT happen here: dropping the locally-stored data
     * for the other namespaces in the operation. That used to run at this
     * point, and it was wrong twice over - see disconnect_cbfunc(), which
     * does it now. */

    msg = PMIX_NEW(pmix_buffer_t);
    if (PMIX_UNLIKELY(NULL == msg)) {
        /* PMIX_BFROPS_PACK reads the buffer's type before it does
         * anything else, so an unchecked failure here is a segfault */
        PMIX_PROC_FREE(rgs, nrg);
        return PMIX_ERR_NOMEM;
    }
    /* pack the cmd */
    PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, msg, &cmd, 1, PMIX_COMMAND);
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(msg);
        PMIX_PROC_FREE(rgs, nrg);
        return rc;
    }

    /* pack the number of procs */
    PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, msg, &nrg, 1, PMIX_SIZE);
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(msg);
        PMIX_PROC_FREE(rgs, nrg);
        return rc;
    }
    PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, msg, rgs, nrg, PMIX_PROC);
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(msg);
        PMIX_PROC_FREE(rgs, nrg);
        return rc;
    }

    /* pack the info structs */
    PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, msg, &ninfo, 1, PMIX_SIZE);
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(msg);
        PMIX_PROC_FREE(rgs, nrg);
        return rc;
    }
    if (0 < ninfo) {
        PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, msg, info, ninfo, PMIX_INFO);
        if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
            PMIX_ERROR_LOG(rc);
            PMIX_RELEASE(msg);
            PMIX_PROC_FREE(rgs, nrg);
            return rc;
        }
    }

    /* create a callback object as we need to pass it to the
     * recv routine so we know which callback to use when
     * the return message is recvd */
    cb = PMIX_NEW(pmix_cb_t);
    if (PMIX_UNLIKELY(NULL == cb)) {
        PMIX_RELEASE(msg);
        PMIX_PROC_FREE(rgs, nrg);
        return PMIX_ERR_NOMEM;
    }
    cb->cbfunc.opfn = cbfunc;
    cb->cbdata = cbdata;
    /* the participant array outlives this call now: disconnect_cbfunc()
     * needs it to know whose data to drop. Nothing in pmix_cb_t's
     * destructor frees procs - it is a borrowed pointer everywhere else -
     * so the callback frees it explicitly. */
    cb->procs = rgs;
    cb->nprocs = nrg;

    /* push the message into our event base to send to the server */
    PMIX_PTL_SEND_RECV(rc, pmix_client_globals.myserver, msg, disconnect_cbfunc, (void *) cb);
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        PMIX_RELEASE(msg);
        PMIX_PROC_FREE(cb->procs, cb->nprocs);
        cb->procs = NULL;
        cb->nprocs = 0;
        PMIX_RELEASE(cb);
    }

    return rc;
}

/* Extract the status the caller is owed from a connect or disconnect
 * reply, storing any job-level data the reply carried.
 *
 * Runs on the progress thread - it is a PTL recv callback - which is what
 * makes the gds stores below legal. Both commands come through here; a
 * disconnect reply simply carries no byte objects, so the loop ends on
 * its first unpack. */
static pmix_status_t process_reply(pmix_buffer_t *buf)
{
    pmix_status_t rc;
    pmix_status_t ret;
    int32_t cnt;
    char *nspace;
    pmix_buffer_t bkt;
    pmix_byte_object_t bo;

    if (PMIX_UNLIKELY(NULL == buf)) {
        return PMIX_ERR_BAD_PARAM;
    }

    /* a zero-byte buffer indicates that this recv is being
     * completed due to a lost connection */
    if (PMIX_BUFFER_IS_EMPTY(buf)) {
        return PMIX_ERR_UNREACH;
    }

    /* unpack the returned status */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, pmix_client_globals.myserver, buf, &ret, &cnt, PMIX_STATUS);
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        PMIX_ERROR_LOG(rc);
        ret = rc;
    }
    /* connect has to also pass back data from all nspace's involved in
     * the operation, including our own. Each will come as a byte object */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, pmix_client_globals.myserver, buf, &bo, &cnt, PMIX_BYTE_OBJECT);
    while (PMIX_SUCCESS == rc) {
        /* load it for unpacking */
        PMIX_CONSTRUCT(&bkt, pmix_buffer_t);
        PMIX_LOAD_BUFFER(pmix_client_globals.myserver, &bkt, bo.bytes, bo.size);

        /* unpack the nspace for this blob */
        cnt = 1;
        PMIX_BFROPS_UNPACK(rc, pmix_client_globals.myserver, &bkt, &nspace, &cnt, PMIX_STRING);
        if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
            /* give up on the remaining blobs - the check below this loop
             * reports rc to the caller. This used to "continue", which
             * reads as "skip this blob and take the next one" but does
             * the same thing, the loop condition being on rc */
            PMIX_DESTRUCT(&bkt);
            break;
        }
        /* extract and process any proc-related info for this nspace */
        PMIX_GDS_STORE_JOB_INFO(rc, pmix_globals.mypeer, nspace, &bkt);
        if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
            PMIX_ERROR_LOG(rc);
        }
        free(nspace);
        PMIX_DESTRUCT(&bkt);
        /* get the next one */
        cnt = 1;
        PMIX_BFROPS_UNPACK(rc, pmix_client_globals.myserver, buf, &bo, &cnt, PMIX_BYTE_OBJECT);
    }
    if (PMIX_ERR_UNPACK_READ_PAST_END_OF_BUFFER != rc) {
        PMIX_ERROR_LOG(rc);
        ret = rc;
    }

    return ret;
}

static void wait_cbfunc(struct pmix_peer_t *pr, pmix_ptl_hdr_t *hdr, pmix_buffer_t *buf,
                        void *cbdata)
{
    pmix_cb_t *cb = (pmix_cb_t *) cbdata;
    pmix_status_t ret;

    pmix_output_verbose(2, pmix_globals.debug_output,
                        "pmix:client recv callback activated with %d bytes",
                        (NULL == buf) ? -1 : (int) buf->bytes_used);
    PMIX_HIDE_UNUSED_PARAMS(pr, hdr);

    ret = process_reply(buf);

    if (NULL != cb->cbfunc.opfn) {
        cb->cbfunc.opfn(ret, cb->cbdata);
    }
    PMIX_RELEASE(cb);
}

/* The disconnect reply, plus the local cleanup that goes with it.
 *
 * Dropping the other namespaces' cached data used to happen up in
 * PMIx_Disconnect_nb, before the message was even packed. Two things were
 * wrong with that, and this is the one place that fixes both.
 *
 * It ran on the **caller's** thread. Every store into a datastore happens
 * on the progress thread, and PMIX_GDS_DEL_NSPACE tears down the very
 * trackers a concurrent store or fetch is walking - gds/hash takes no
 * locks, which is precisely what its is_tsafe = false says. A reply
 * arriving for an outstanding PMIx_Get on a proc in one of those
 * namespaces, at the moment the application called PMIx_Disconnect, is a
 * use-after-free. Here we are the progress thread by construction.
 *
 * And it ran **before the operation had happened**, so a pack failure, a
 * dead transport or a server that answered with an error left this
 * process still connected and its copy of the peers' job data gone.
 *
 * Note the deletion is deliberately not done on the PMIX_ERR_UNREACH
 * path: a lost connection is not a completed disconnect. */
static void disconnect_cbfunc(struct pmix_peer_t *pr, pmix_ptl_hdr_t *hdr, pmix_buffer_t *buf,
                              void *cbdata)
{
    pmix_cb_t *cb = (pmix_cb_t *) cbdata;
    pmix_status_t ret, rc;
    size_t n;

    pmix_output_verbose(2, pmix_globals.debug_output,
                        "pmix:client disconnect recv callback activated with %d bytes",
                        (NULL == buf) ? -1 : (int) buf->bytes_used);
    PMIX_HIDE_UNUSED_PARAMS(pr, hdr);

    ret = process_reply(buf);

    if (PMIX_SUCCESS == ret) {
        /* remove the locally-stored data for any nspace other than our
         * own that took part in this disconnect */
        for (n = 0; n < cb->nprocs; n++) {
            if (0 == strcmp(pmix_globals.myid.nspace, cb->procs[n].nspace)) {
                continue;
            }
            PMIX_GDS_DEL_NSPACE(rc, cb->procs[n].nspace);
            if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
                PMIX_ERROR_LOG(rc);
            }
        }
    }
    PMIX_PROC_FREE(cb->procs, cb->nprocs);
    cb->procs = NULL;
    cb->nprocs = 0;

    if (NULL != cb->cbfunc.opfn) {
        cb->cbfunc.opfn(ret, cb->cbdata);
    }
    PMIX_RELEASE(cb);
}

static void op_cbfunc(pmix_status_t status, void *cbdata)
{
    pmix_cb_t *cb = (pmix_cb_t *) cbdata;

    cb->status = status;
    PMIX_POST_OBJECT(cb);
    PMIX_WAKEUP_THREAD(&cb->lock);
}
