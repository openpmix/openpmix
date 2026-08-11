/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2014-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2014-2015 Artem Y. Polyakov <artpol84@gmail.com>.
 *                         All rights reserved.
 * Copyright (c) 2016-2020 Mellanox Technologies, Inc.
 *                         All rights reserved.
 * Copyright (c) 2016-2018 IBM Corporation.  All rights reserved.
 * Copyright (c) 2018      Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
 * Copyright (c) 2022-2023 Triad National Security, LLC. All rights reserved.
 * Copyright (c) 2026      Jeff Squyres  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/* Replies that complete a specific client operation and carry that
 * operation's own payload: the fence modex, a direct-modex get, connect
 * and disconnect, spawn, lookup, event registration, and IOF register
 * and deregister.
 *
 * These are callbacks the host server invokes, so they can run in either
 * the host's thread context or our own if the host answers immediately.
 * Anything touching a global entity is therefore pushed into an event
 * before it is used. The switchyard in pmix_server_switchyard.c is the
 * only caller that hands these to a host up-call. */

#include "src/include/pmix_config.h"

#include "src/include/pmix_stdint.h"

#include "pmix_common.h"
#include "include/pmix_server.h"

#include "src/include/pmix_globals.h"

#ifdef HAVE_STRING_H
#    include <string.h>
#endif
#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif

#include "src/mca/bfrops/base/base.h"
#include "src/mca/gds/base/base.h"
#include "src/mca/ptl/base/base.h"
#include "src/util/pmix_error.h"
#include "src/util/pmix_name_fns.h"
#include "src/util/pmix_output.h"

#include "pmix_server_ops.h"
#include "src/client/pmix_client_ops.h"

/* fence modex calls return here when the host RM has completed
 * the operation - any enclosed data is provided to us as a blob
 * which contains byte objects, one for each set of data. Our
 * peer servers will have packed the blobs using our common
 * GDS module, so use the mypeer one to unpack them */
static void _mdxcbfunc(int sd, short args, void *cbdata)
{
    pmix_shift_caddy_t *scd = (pmix_shift_caddy_t *) cbdata;
    pmix_server_trkr_t *tracker = scd->tracker;
    pmix_buffer_t xfer, *reply;
    pmix_server_caddy_t *cd, *nxt;
    pmix_status_t rc = PMIX_SUCCESS, ret;
    pmix_nspace_caddy_t *nptr;
    pmix_list_t nslist;
    bool found;
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    PMIX_ACQUIRE_OBJECT(scd);

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        /* the host's data is owed a release even here: the flag goes up
         * while PMIx_server_finalize is running, which is exactly when an
         * in-flight host completion lands, and the host outlives us */
        if (NULL != scd->cbfunc.relfn) {
            scd->cbfunc.relfn(scd->relcbdata);
        }
        PMIX_RELEASE(scd);
        return;
    }

    if (NULL == tracker) {
        /* give them a release if they want it - this should
         * never happen, but protect against the possibility.
         * pmix_server_modex_cbfunc parks the host's release data in relcbdata; cbdata
         * is not set on this caddy at all, so handing that to the host's
         * release function passes it something that is not its own. */
        if (NULL != scd->cbfunc.relfn) {
            scd->cbfunc.relfn(scd->relcbdata);
        }
        PMIX_RELEASE(scd);
        return;
    }

    /* if we get here, then there are processes waiting
     * for a response */

    /* if the timer is active, clear it */
    if (tracker->event_active) {
        pmix_event_del(&tracker->ev);
    }

    /* pass the blobs being returned */
    PMIX_CONSTRUCT(&xfer, pmix_buffer_t);
    PMIX_CONSTRUCT(&nslist, pmix_list_t);

    if (PMIX_SUCCESS != scd->status) {
        rc = scd->status;
        goto finish_collective;
    }

    if (PMIX_COLLECT_INVALID == tracker->collect_type) {
        rc = PMIX_ERR_INVALID_ARG;
        goto finish_collective;
    }

    /* Collect the list of unique nspaces so we store the
     * data so each namespace can access it */
    PMIX_LIST_FOREACH (cd, &tracker->local_cbs, pmix_server_caddy_t) {
        // see if we already have this nspace
        found = false;
        PMIX_LIST_FOREACH (nptr, &nslist, pmix_nspace_caddy_t) {
            if (0 == strcmp(nptr->ns->nspace, cd->peer->nptr->nspace)) {
                found = true;
                break;
            }
        }
        if (!found) {
            // add it
            nptr = PMIX_NEW(pmix_nspace_caddy_t);
            if (NULL == nptr) {
                PMIX_ERROR_LOG(PMIX_ERR_NOMEM);
                rc = PMIX_ERR_NOMEM;
                goto finish_collective;
            }
            PMIX_RETAIN(cd->peer->nptr);
            nptr->ns = cd->peer->nptr;
            pmix_list_append(&nslist, &nptr->super);
        }
    }

    // Skip storing the data if we didn't collect it
    if (NULL == scd->data) {
        rc = PMIX_SUCCESS;
        goto finish_collective;
    }

    /* Store the returned blobs, once per participating nspace.
     *
     * All local clients of one nspace share a gds module, but clients of
     * *different* nspaces need not - so there is no single module that
     * can be handed the whole payload. Walk it once for each nspace,
     * through a peer of that nspace, storing only that nspace's blobs.
     * The walk is cheap relative to the collective, and a real job has
     * only a couple of local nspaces.
     *
     * Note the buffer is reloaded each pass: the walker consumes it, and
     * PMIX_LOAD_BUFFER_NON_DESTRUCT simply re-points at the payload the
     * host handed us without copying or taking ownership of it. */
    PMIX_LIST_FOREACH (nptr, &nslist, pmix_nspace_caddy_t) {
        pmix_peer_t *nspeer = NULL;

        /* find a local peer of this nspace to resolve its module */
        PMIX_LIST_FOREACH (cd, &tracker->local_cbs, pmix_server_caddy_t) {
            if (0 == strcmp(nptr->ns->nspace, cd->peer->nptr->nspace)) {
                nspeer = cd->peer;
                break;
            }
        }
        if (NULL == nspeer) {
            /* cannot happen - the nspace came from this very list */
            continue;
        }
        PMIX_LOAD_BUFFER_NON_DESTRUCT(pmix_globals.mypeer, &xfer, scd->data, scd->ndata);
        PMIX_GDS_STORE_MODEX(rc, nspeer, nptr->ns->nspace, &xfer, tracker);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            break;
        }
    }
    /* do NOT destruct the xfer buffer as that would release the payload! */

finish_collective:
    /* Loop across all procs in the tracker, sending them the reply.
     *
     * A failure serving one participant must not abandon the others.
     * Nothing else in the tree will ever answer them - the timer was
     * cancelled above and the tracker release below frees their caddies
     * without a reply - so they would sit in PMIx_Fence forever. Skip to
     * the next participant instead of leaving the loop.
     *
     * Note also that the status being packed is rc, the collective's
     * status, and PMIX_GDS_MARK_MODEX_COMPLETE assigns whatever it is
     * given: writing it into rc would report a failed fence as a success
     * to every participant after the first. */
    PMIX_LIST_FOREACH_SAFE (cd, nxt, &tracker->local_cbs, pmix_server_caddy_t) {
        reply = PMIX_NEW(pmix_buffer_t);
        if (NULL == reply) {
            PMIX_ERROR_LOG(PMIX_ERR_NOMEM);
            continue;
        }
        /* setup the reply, starting with the returned status */
        PMIX_BFROPS_PACK(ret, cd->peer, reply, &rc, 1, PMIX_STATUS);
        if (PMIX_SUCCESS != ret) {
            PMIX_ERROR_LOG(ret);
            PMIX_RELEASE(reply);
            continue;
        }
        /* let the gds have a chance to add any data it needs
         * for providing access to any collected data */
        PMIX_GDS_MARK_MODEX_COMPLETE(ret, cd->peer, &nslist, reply);
        if (PMIX_SUCCESS != ret) {
            PMIX_RELEASE(reply);
            PMIX_ERROR_LOG(ret);
            continue;
        }
        pmix_output_verbose(2, pmix_server_globals.base_output,
                            "server:modex_cbfunc reply being sent to %s:%u",
                            cd->peer->info->pname.nspace, cd->peer->info->pname.rank);
        PMIX_SERVER_QUEUE_REPLY(ret, cd->peer, cd->hdr.tag, reply);
        if (PMIX_SUCCESS != ret) {
            PMIX_RELEASE(reply);
        }
        /* remove this entry */
        pmix_list_remove_item(&tracker->local_cbs, &cd->super);
        PMIX_RELEASE(cd);
    }

    /* Protect data from being free'd because RM pass
     * the pointer that is set to the middle of some
     * buffer (the case with SLURM).
     * RM is responsible on the release of the buffer
     */
    xfer.base_ptr = NULL;
    xfer.bytes_used = 0;
    PMIX_DESTRUCT(&xfer);

    pmix_list_remove_item(&pmix_server_globals.collectives, &tracker->super);
    PMIX_RELEASE(tracker);
    PMIX_LIST_DESTRUCT(&nslist);

    /* we are done */
    if (NULL != scd->cbfunc.relfn) {
        scd->cbfunc.relfn(scd->relcbdata);
    }
    PMIX_RELEASE(scd);
}

void pmix_server_modex_cbfunc(pmix_status_t status, const char *data, size_t ndata, void *cbdata,
                              pmix_release_cbfunc_t relfn, void *relcbd)
{
    pmix_server_trkr_t *tracker = (pmix_server_trkr_t *) cbdata;
    pmix_shift_caddy_t *scd;

    pmix_output_verbose(2, pmix_server_globals.base_output,
                        "server:modex_cbfunc called with %d bytes", (int) ndata);

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        /* honor the release contract even here - see _mdxcbfunc */
        if (NULL != relfn) {
            relfn(relcbd);
        }
        return;
    }

    /* need to thread-shift this callback as it accesses global data */
    scd = PMIX_NEW(pmix_shift_caddy_t);
    if (NULL == scd) {
        PMIX_ERROR_LOG(PMIX_ERR_NOMEM);
        /* nothing we can do beyond honoring the release contract - with
         * the host's own release data, not the tracker we were handed */
        if (NULL != relfn) {
            relfn(relcbd);
        }
        return;
    }
    scd->status = status;
    scd->data = data;
    scd->ndata = ndata;
    scd->tracker = tracker;
    scd->cbfunc.relfn = relfn;
    scd->relcbdata = relcbd;
    /* the tracker is now spoken for - _mdxcbfunc will unlink and release
     * it, so nothing walking the collectives list in the meantime may
     * complete it again. Marked here rather than at the dozen sites that
     * drive a completion, so a new one cannot forget to. Only once we know
     * we are actually going to shift: the NOMEM return above leaves the
     * tracker unclaimed, which is what lets a later sweep rescue it. */
    if (NULL != tracker) {
        tracker->completion_fired = true;
    }
    PMIX_THREADSHIFT(scd, _mdxcbfunc);
}

static void _getcbfunc(int sd, short args, void *cbdata)
{
    pmix_shift_caddy_t *scd = (pmix_shift_caddy_t *) cbdata;
    pmix_server_caddy_t *cd = (pmix_server_caddy_t *) scd->cbdata;
    pmix_buffer_t *reply, buf;
    pmix_status_t rc;
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    PMIX_ACQUIRE_OBJECT(scd);

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        /* the host is still owed its release - see _mdxcbfunc */
        if (NULL != scd->cbfunc.relfn) {
            scd->cbfunc.relfn(scd->relcbdata);
        }
        if (NULL != cd) {
            PMIX_RELEASE(cd);
        }
        PMIX_RELEASE(scd);
        return;
    }

    if (NULL == cd) {
        /* nothing to do - but be sure to give them
         * a release if they want it */
        goto cleanup;
    }

    /* setup the reply, starting with the returned status */
    reply = PMIX_NEW(pmix_buffer_t);
    if (NULL == reply) {
        PMIX_ERROR_LOG(PMIX_ERR_NOMEM);
        goto cleanup;
    }
    PMIX_BFROPS_PACK(rc, cd->peer, reply, &scd->status, 1, PMIX_STATUS);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(reply);
        goto cleanup;
    }
    /* If there are data, pack the blob being returned.
     *
     * PMIX_LOAD_BUFFER points the buffer straight at the host's payload -
     * it neither copies it nor takes ownership of it, and it NULLs the
     * source it was handed. So the buffer has to be disowned before it is
     * destructed, on *every* path: the payload belongs to whoever gave us
     * the release function, and that function is called below. The error
     * arm destructed the buffer with the pointer still in it, so a failed
     * copy freed the host's blob and the release function then freed it a
     * second time. */
    if (NULL != scd->data) {
        PMIX_CONSTRUCT(&buf, pmix_buffer_t);
        PMIX_LOAD_BUFFER(cd->peer, &buf, scd->data, scd->ndata);
        PMIX_BFROPS_COPY_PAYLOAD(rc, cd->peer, reply, &buf);
        buf.base_ptr = NULL;
        buf.bytes_used = 0;
        PMIX_DESTRUCT(&buf);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_RELEASE(reply);
            goto cleanup;
        }
    }

    /* send the data to the requestor */
    pmix_output_verbose(2, pmix_server_globals.base_output,
                        "server:get_cbfunc reply being sent to %s:%u with status %s",
                        cd->peer->info->pname.nspace,
                        cd->peer->info->pname.rank,
                        PMIx_Error_string(scd->status));
    pmix_output_hexdump(10, pmix_server_globals.base_output, reply->base_ptr,
                        (reply->bytes_used < 256 ? reply->bytes_used : 256));

    PMIX_SERVER_QUEUE_REPLY(rc, cd->peer, cd->hdr.tag, reply);
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(reply);
    }

cleanup:
    /* if someone wants a release, give it to them */
    if (NULL != scd->cbfunc.relfn) {
        scd->cbfunc.relfn(scd->relcbdata);
    }
    if (NULL != cd) {
        PMIX_RELEASE(cd);
    }
    PMIX_RELEASE(scd);
}

void pmix_server_get_cbfunc(pmix_status_t status, const char *data, size_t ndata, void *cbdata,
                            pmix_release_cbfunc_t relfn, void *relcbd)
{
    pmix_shift_caddy_t *scd;
    pmix_server_caddy_t *cd = (pmix_server_caddy_t *) cbdata;

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        /* honor the release contract even here - see _mdxcbfunc */
        if (NULL != relfn) {
            relfn(relcbd);
        }
        if (NULL != cd) {
            PMIX_RELEASE(cd);
        }
        return;
    }

    /* need to thread-shift this callback as it accesses global data */
    scd = PMIX_NEW(pmix_shift_caddy_t);
    if (NULL == scd) {
        PMIX_ERROR_LOG(PMIX_ERR_NOMEM);
        /* honor the release contract - with the host's own release data,
         * not the caddy we were handed - and let go of that caddy, which
         * nothing else will now release */
        if (NULL != relfn) {
            relfn(relcbd);
        }
        if (NULL != cd) {
            PMIX_RELEASE(cd);
        }
        return;
    }
    scd->status = status;
    scd->data = data;
    scd->ndata = ndata;
    scd->cbdata = cbdata;
    scd->cbfunc.relfn = relfn;
    scd->relcbdata = relcbd;
    PMIX_THREADSHIFT(scd, _getcbfunc);
}

static void _cnct(int sd, short args, void *cbdata)
{
    pmix_shift_caddy_t *scd = (pmix_shift_caddy_t *) cbdata;
    pmix_server_trkr_t *tracker = scd->tracker;
    pmix_buffer_t *reply, pbkt;
    pmix_byte_object_t bo;
    pmix_status_t rc, ret;
    int i;
    pmix_server_caddy_t *cd;
    char **nspaces = NULL;
    bool found;
    pmix_proc_t proc;
    pmix_cb_t cb;
    pmix_kval_t *kptr;
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    PMIX_ACQUIRE_OBJECT(scd);

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        PMIX_RELEASE(scd);
        return;
    }

    if (NULL == tracker) {
        /* nothing to do - but the shifter is still ours to free */
        PMIX_RELEASE(scd);
        return;
    }

    /* if we get here, then there are processes waiting
     * for a response */

    /* if the timer is active, clear it */
    if (tracker->event_active) {
        pmix_event_del(&tracker->ev);
    }

    /* find the unique nspaces that are participating */
    PMIX_LIST_FOREACH (cd, &tracker->local_cbs, pmix_server_caddy_t) {
        if (NULL == nspaces) {
            PMIx_Argv_append_nosize(&nspaces, cd->peer->info->pname.nspace);
        } else {
            found = false;
            for (i = 0; NULL != nspaces[i]; i++) {
                if (0 == strcmp(nspaces[i], cd->peer->info->pname.nspace)) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                PMIx_Argv_append_nosize(&nspaces, cd->peer->info->pname.nspace);
            }
        }
    }

    /* Loop across all local procs in the tracker, sending them the reply.
     *
     * A failure serving one participant must not abandon the others:
     * nothing else will ever answer them, and the tracker release below
     * frees their caddies without a reply, so they would sit in
     * PMIx_Connect forever. Failures that leave this participant's reply
     * unsendable fall to the participant_error arm at the foot of the
     * loop, which hands that one client the error and carries on. */
    PMIX_LIST_FOREACH (cd, &tracker->local_cbs, pmix_server_caddy_t) {
        /* setup the reply, starting with the returned status */
        reply = PMIX_NEW(pmix_buffer_t);
        if (NULL == reply) {
            PMIX_ERROR_LOG(PMIX_ERR_NOMEM);
            continue;
        }
        /* start with the status */
        PMIX_BFROPS_PACK(rc, cd->peer, reply, &scd->status, 1, PMIX_STATUS);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_RELEASE(reply);
            continue;
        }
        if (PMIX_SUCCESS == scd->status && NULL != nspaces) {
            /* loop across all participating nspaces and include their
             * job-related info. nspaces can be NULL only if every append
             * above failed to allocate */
            for (i = 0; NULL != nspaces[i]; i++) {
                /* if this is the local proc's own nspace, then
                 * ignore it - it already has this info */
                if (0 == strncmp(nspaces[i], cd->peer->info->pname.nspace, PMIX_MAX_NSLEN)) {
                    continue;
                }

                /* this is a local request, so give the gds the option
                 * of returning a copy of the data, or a pointer to
                 * local storage */
                /* add the job-level info, if necessary */
                PMIX_LOAD_PROCID(&proc, nspaces[i], PMIX_RANK_WILDCARD);
                PMIX_CONSTRUCT(&cb, pmix_cb_t);
                /* this is for a local client, so give the gds the
                 * option of returning a complete copy of the data,
                 * or returning a pointer to local storage */
                cb.proc = &proc;
                cb.scope = PMIX_SCOPE_UNDEF;
                cb.copy = false;
                PMIX_GDS_FETCH_KV(rc, cd->peer, &cb);
                if (PMIX_SUCCESS != rc) {
                    /* try getting it from our storage */
                    PMIX_GDS_FETCH_KV(rc, pmix_globals.mypeer, &cb);
                    if (PMIX_SUCCESS != rc) {
                        PMIX_ERROR_LOG(rc);
                        PMIX_RELEASE(reply);
                        PMIX_DESTRUCT(&cb);
                        goto participant_error;
                    }
                }
                PMIX_CONSTRUCT(&pbkt, pmix_buffer_t);
                /* pack the nspace name */
                PMIX_BFROPS_PACK(rc, cd->peer, &pbkt, &nspaces[i], 1, PMIX_STRING);
                if (PMIX_SUCCESS != rc) {
                    PMIX_ERROR_LOG(rc);
                    PMIX_RELEASE(reply);
                    PMIX_DESTRUCT(&pbkt);
                    PMIX_DESTRUCT(&cb);
                    goto participant_error;
                }
                PMIX_LIST_FOREACH (kptr, &cb.kvs, pmix_kval_t) {
                    PMIX_BFROPS_PACK(rc, cd->peer, &pbkt, kptr, 1, PMIX_KVAL);
                    if (PMIX_SUCCESS != rc) {
                        PMIX_ERROR_LOG(rc);
                        PMIX_RELEASE(reply);
                        PMIX_DESTRUCT(&pbkt);
                        PMIX_DESTRUCT(&cb);
                        goto participant_error;
                    }
                }
                PMIX_DESTRUCT(&cb);

                if (PMIX_PEER_IS_V1(cd->peer) || PMIX_PEER_IS_V20(cd->peer)) {
                    PMIX_BFROPS_PACK(rc, cd->peer, reply, &pbkt, 1, PMIX_BUFFER);
                    if (PMIX_SUCCESS != rc) {
                        PMIX_ERROR_LOG(rc);
                        PMIX_RELEASE(reply);
                        PMIX_DESTRUCT(&pbkt);
                        goto participant_error;
                    }
                } else {
                    PMIX_UNLOAD_BUFFER(&pbkt, bo.bytes, bo.size);
                    PMIX_BFROPS_PACK(rc, cd->peer, reply, &bo, 1, PMIX_BYTE_OBJECT);
                    PMIX_BYTE_OBJECT_DESTRUCT(&bo); // data has been copied
                    if (PMIX_SUCCESS != rc) {
                        PMIX_ERROR_LOG(rc);
                        PMIX_RELEASE(reply);
                        PMIX_DESTRUCT(&pbkt);
                        goto participant_error;
                    }
                }

                PMIX_DESTRUCT(&pbkt);
            }
        }
        pmix_output_verbose(2, pmix_server_globals.connect_output,
                            "server:cnct_cbfunc reply being sent to %s",
                            PMIX_PEER_PRINT(cd->peer));
        PMIX_SERVER_QUEUE_REPLY(rc, cd->peer, cd->hdr.tag, reply);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_RELEASE(reply);
        }
        continue;

participant_error:
        /* we could not assemble this participant's job-level info - give
         * it the error status so it does not hang, and go on to the next
         * one. Do not record the failure in scd->status: it belongs to
         * this participant, and the ones behind it may yet be served. */
        reply = PMIX_NEW(pmix_buffer_t);
        if (NULL == reply) {
            PMIX_ERROR_LOG(PMIX_ERR_NOMEM);
            continue;
        }
        PMIX_BFROPS_PACK(ret, cd->peer, reply, &rc, 1, PMIX_STATUS);
        if (PMIX_SUCCESS != ret) {
            PMIX_ERROR_LOG(ret);
            PMIX_RELEASE(reply);
            continue;
        }
        PMIX_SERVER_QUEUE_REPLY(ret, cd->peer, cd->hdr.tag, reply);
        if (PMIX_SUCCESS != ret) {
            PMIX_RELEASE(reply);
        }
    }

    if (NULL != nspaces) {
        PMIx_Argv_free(nspaces);
    }
    pmix_list_remove_item(&pmix_server_globals.collectives, &tracker->super);
    PMIX_RELEASE(tracker);

    /* we are done */
    PMIX_RELEASE(scd);
}

void pmix_server_cnct_cbfunc(pmix_status_t status, void *cbdata)
{
    pmix_server_trkr_t *tracker = (pmix_server_trkr_t *) cbdata;
    pmix_shift_caddy_t *scd;

    pmix_output_verbose(2, pmix_server_globals.connect_output,
                        "server:cnct_cbfunc called");

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        return;
    }

    /* need to thread-shift this callback as it accesses global data */
    scd = PMIX_NEW(pmix_shift_caddy_t);
    if (NULL == scd) {
        /* nothing we can do - the tracker stays on the collectives list
         * with its participants waiting, and touching it from here would
         * be off the progress thread. Say so rather than failing mute */
        PMIX_ERROR_LOG(PMIX_ERR_NOMEM);
        return;
    }
    scd->status = status;
    scd->tracker = tracker;
    /* the tracker is now spoken for - see pmix_server_modex_cbfunc */
    if (NULL != tracker) {
        tracker->completion_fired = true;
    }
    PMIX_THREADSHIFT(scd, _cnct);
}

static void _discnct(int sd, short args, void *cbdata)
{
    pmix_shift_caddy_t *scd = (pmix_shift_caddy_t *) cbdata;
    pmix_server_trkr_t *tracker = scd->tracker;
    pmix_buffer_t *reply;
    pmix_status_t rc;
    pmix_server_caddy_t *cd;
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    PMIX_ACQUIRE_OBJECT(scd);

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        PMIX_RELEASE(scd);
        return;
    }

    if (NULL == tracker) {
        /* nothing to do - but the shifter is still ours to free */
        PMIX_RELEASE(scd);
        return;
    }

    /* if we get here, then there are processes waiting
     * for a response */

    /* if the timer is active, clear it */
    if (tracker->event_active) {
        pmix_event_del(&tracker->ev);
    }

    /* Loop across all local procs in the tracker, sending them the reply.
     * A failure serving one participant must not abandon the others - see
     * the matching note in _cnct. */
    PMIX_LIST_FOREACH (cd, &tracker->local_cbs, pmix_server_caddy_t) {
        /* setup the reply */
        reply = PMIX_NEW(pmix_buffer_t);
        if (NULL == reply) {
            PMIX_ERROR_LOG(PMIX_ERR_NOMEM);
            continue;
        }
        /* return the status */
        PMIX_BFROPS_PACK(rc, cd->peer, reply, &scd->status, 1, PMIX_STATUS);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_RELEASE(reply);
            continue;
        }
        pmix_output_verbose(2, pmix_server_globals.connect_output,
                            "server:cnct_cbfunc reply being sent to %s:%u",
                            cd->peer->info->pname.nspace, cd->peer->info->pname.rank);
        PMIX_SERVER_QUEUE_REPLY(rc, cd->peer, cd->hdr.tag, reply);
        if (PMIX_SUCCESS != rc) {
            PMIX_RELEASE(reply);
        }
    }

    /* cleanup the tracker -- the host RM is responsible for
     * telling us when to remove the nspace from our data */
    pmix_list_remove_item(&pmix_server_globals.collectives, &tracker->super);
    PMIX_RELEASE(tracker);

    /* we are done */
    PMIX_RELEASE(scd);
}

void pmix_server_discnct_cbfunc(pmix_status_t status, void *cbdata)
{
    pmix_server_trkr_t *tracker = (pmix_server_trkr_t *) cbdata;
    pmix_shift_caddy_t *scd;

    pmix_output_verbose(2, pmix_server_globals.base_output,
                        "server:discnct_cbfunc called on nspace %s",
                        (NULL == tracker) ? "NULL" : tracker->pname.nspace);

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        return;
    }

    /* need to thread-shift this callback as it accesses global data */
    scd = PMIX_NEW(pmix_shift_caddy_t);
    if (NULL == scd) {
        /* nothing we can do - the tracker stays on the collectives list
         * with its participants waiting, and touching it from here would
         * be off the progress thread. Say so rather than failing mute */
        PMIX_ERROR_LOG(PMIX_ERR_NOMEM);
        return;
    }
    scd->status = status;
    scd->tracker = tracker;
    /* the tracker is now spoken for - see pmix_server_modex_cbfunc */
    if (NULL != tracker) {
        tracker->completion_fired = true;
    }
    PMIX_THREADSHIFT(scd, _discnct);
}

static void _spcb(int sd, short args, void *cbdata)
{
    pmix_shift_caddy_t *cd = (pmix_shift_caddy_t *) cbdata;
    pmix_buffer_t *reply;
    pmix_status_t rc;
    pmix_proc_t proc;
    pmix_cb_t cb;
    pmix_kval_t *kv;
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    PMIX_ACQUIRE_OBJECT(cd);

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        goto cleanup;
    }

    /* setup the reply with the returned status */
    if (NULL == (reply = PMIX_NEW(pmix_buffer_t))) {
        PMIX_ERROR_LOG(PMIX_ERR_OUT_OF_RESOURCE);
        goto cleanup;
    }
    PMIX_BFROPS_PACK(rc, cd->cd->peer, reply, &cd->status, 1, PMIX_STATUS);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(reply);
        goto cleanup;
    }
    /* pass back the name of the nspace */
    PMIX_BFROPS_PACK(rc, cd->cd->peer, reply, &cd->pname.nspace, 1, PMIX_STRING);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(reply);
        goto cleanup;
    }
    if (PMIX_SUCCESS == cd->status) {
        /* add the job-level info, if we have it */
        PMIX_LOAD_PROCID(&proc, cd->pname.nspace, PMIX_RANK_WILDCARD);
        /* this is going to a local client, so let the gds
         * have the option of returning a copy of the data,
         * or a pointer to local storage */
        PMIX_CONSTRUCT(&cb, pmix_cb_t);
        cb.proc = &proc;
        cb.scope = PMIX_SCOPE_UNDEF;
        cb.copy = false;
        PMIX_GDS_FETCH_KV(rc, pmix_globals.mypeer, &cb);
        if (PMIX_SUCCESS == rc) {
            PMIX_LIST_FOREACH (kv, &cb.kvs, pmix_kval_t) {
                PMIX_BFROPS_PACK(rc, cd->cd->peer, reply, kv, 1, PMIX_KVAL);
                if (PMIX_SUCCESS != rc) {
                    PMIX_ERROR_LOG(rc);
                    PMIX_RELEASE(reply);
                    PMIX_DESTRUCT(&cb);
                    goto cleanup;
                }
            }
        }
        /* the destruct belongs outside the success arm: a fetch that finds
         * nothing is the ordinary case for a job we have not been told
         * about yet, and cbcon constructs a lock unconditionally */
        PMIX_DESTRUCT(&cb);
    }

    /* the function that created the server_caddy did a
     * retain on the peer, so we don't have to worry about
     * it still being present - tell the originator the result */
    PMIX_SERVER_QUEUE_REPLY(rc, cd->cd->peer, cd->cd->hdr.tag, reply);
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(reply);
    }

cleanup:
    /* cleanup */
    PMIX_RELEASE(cd->cd);
    PMIX_RELEASE(cd);
}

void pmix_server_spawn_cbfunc(pmix_status_t status, char *nspace, void *cbdata)
{
    pmix_shift_caddy_t *cd;
    pmix_server_caddy_t *scd = (pmix_server_caddy_t *) cbdata;

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        PMIX_RELEASE(scd);
        return;
    }

    /* need to thread-shift this request */
    cd = PMIX_NEW(pmix_shift_caddy_t);
    if (NULL == cd) {
        PMIX_ERROR_LOG(PMIX_ERR_NOMEM);
        /* we cannot answer the requestor, but we can at least let go of
         * the peer this caddy is holding */
        PMIX_RELEASE(scd);
        return;
    }
    cd->status = status;
    if (NULL != nspace) {
        cd->pname.nspace = strdup(nspace);
    }
    cd->cd = scd;

    PMIX_THREADSHIFT(cd, _spcb);
}

static void _lkupcbfunc(int sd, short args, void *cbdata)
{
    pmix_shift_caddy_t *scd = (pmix_shift_caddy_t *) cbdata;
    pmix_server_caddy_t *cd = (pmix_server_caddy_t *) scd->cbdata;
    pmix_buffer_t *reply;
    pmix_status_t rc;
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    PMIX_ACQUIRE_OBJECT(scd);

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        goto cleanup;
    }
    /* no need to thread-shift as no global data is accessed */
    /* setup the reply with the returned status */
    if (NULL == (reply = PMIX_NEW(pmix_buffer_t))) {
        PMIX_ERROR_LOG(PMIX_ERR_OUT_OF_RESOURCE);
        goto cleanup;
    }
    PMIX_BFROPS_PACK(rc, cd->peer, reply, &scd->status, 1, PMIX_STATUS);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(reply);
        goto cleanup;
    }
    if (NULL != scd->pdata) {
        /* pack the returned data objects */
        PMIX_BFROPS_PACK(rc, cd->peer, reply, &scd->ndata, 1, PMIX_SIZE);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_RELEASE(reply);
            goto cleanup;
        }
        PMIX_BFROPS_PACK(rc, cd->peer, reply, scd->pdata, scd->ndata, PMIX_PDATA);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_RELEASE(reply);
            goto cleanup;
        }
    }

    /* the function that created the server_caddy did a
     * retain on the peer, so we don't have to worry about
     * it still being present - tell the originator the result */
    PMIX_SERVER_QUEUE_REPLY(rc, cd->peer, cd->hdr.tag, reply);
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(reply);
    }

cleanup:
    /* The array was created with ndata, and npdata is never assigned
     * anywhere - it is zero from the constructor, and PMIx_Pdata_free()
     * does nothing at all for a count of zero, so this leaked the whole
     * array and every value in it on every lookup that returned data.
     * scdes does not free pdata either, so the free belongs here rather
     * than on the success arm alone: the finalize-race early-out and
     * every pack failure above reach only this label. */
    PMIX_PDATA_FREE(scd->pdata, scd->ndata);
    PMIX_RELEASE(cd);
    PMIX_RELEASE(scd);
}

void pmix_server_lookup_cbfunc(pmix_status_t status, pmix_pdata_t pdata[], size_t ndata,
                               void *cbdata)
{
    pmix_server_caddy_t *cd = (pmix_server_caddy_t *)cbdata;
    pmix_shift_caddy_t *scd;
    size_t n;
    pmix_status_t rc;

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        PMIX_RELEASE(cd);
        return;
    }

    /* need to thread-shift this request */
    scd = PMIX_NEW(pmix_shift_caddy_t);
    if (NULL == scd) {
        PMIX_ERROR_LOG(PMIX_ERR_NOMEM);
        /* we cannot answer the requestor, but we can at least let go of
         * the peer this caddy is holding */
        PMIX_RELEASE(cd);
        return;
    }
    scd->status = status;
    if (NULL != pdata && 0 < ndata) {
        scd->ndata = ndata;
        PMIX_PDATA_CREATE(scd->pdata, scd->ndata);
        for (n=0; NULL != scd->pdata && n < scd->ndata; n++) {
            memcpy(&scd->pdata[n].proc, &pdata[n].proc, sizeof(pmix_proc_t));
            memcpy(scd->pdata[n].key, pdata[n].key, sizeof(pmix_key_t));
            PMIX_BFROPS_VALUE_XFER(rc, cd->peer, &scd->pdata[n].value, &pdata[n].value);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
            }
        }
    }
    scd->cbdata = cbdata;

    PMIX_THREADSHIFT(scd, _lkupcbfunc);
}

static void _evcbfunc(int sd, short args, void *cbdata)
{
    pmix_shift_caddy_t *scd = (pmix_shift_caddy_t*)cbdata;
    pmix_server_caddy_t *cd = (pmix_server_caddy_t *)scd->cbdata;
    pmix_buffer_t *reply;
    pmix_status_t rc;
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    PMIX_ACQUIRE_OBJECT(scd);

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        PMIX_RELEASE(cd);
        PMIX_RELEASE(scd);
        return;
    }

    pmix_output_verbose(2, pmix_server_globals.base_output,
                        "server:events_cbfunc called status = %s",
                        PMIx_Error_string(scd->status));

    reply = PMIX_NEW(pmix_buffer_t);
    if (NULL == reply) {
        PMIX_ERROR_LOG(PMIX_ERR_NOMEM);
        goto cleanup;
    }
    PMIX_BFROPS_PACK(rc, cd->peer, reply, &scd->status, 1, PMIX_STATUS);
    if (PMIX_SUCCESS != rc) {
        /* the reply carries nothing but this status, so there is no
         * point queueing an empty buffer - every sibling handler here
         * abandons the reply when its status will not pack */
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(reply);
        goto cleanup;
    }
    // send reply
    PMIX_SERVER_QUEUE_REPLY(rc, cd->peer, cd->hdr.tag, reply);
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(reply);
    }

cleanup:
    PMIX_RELEASE(cd);
    PMIX_RELEASE(scd);
}

void pmix_server_events_cbfunc(pmix_status_t status, void *cbdata)
{
    pmix_shift_caddy_t *scd;
    pmix_server_caddy_t *cd = (pmix_server_caddy_t *)cbdata;

    pmix_output_verbose(2, pmix_server_globals.base_output,
                        "server:events_cbfunc called");

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        PMIX_RELEASE(cd);
        return;
    }

    /* need to thread-shift this callback as it accesses global data */
    scd = PMIX_NEW(pmix_shift_caddy_t);
    if (NULL == scd) {
        PMIX_ERROR_LOG(PMIX_ERR_NOMEM);
        /* we cannot answer the requestor, but we can at least let go of
         * the peer this caddy is holding */
        PMIX_RELEASE(cd);
        return;
    }
    scd->status = status;
    scd->cbdata = cbdata;
    PMIX_THREADSHIFT(scd, _evcbfunc);
}

static void _iofreg(int sd, short args, void *cbdata)
{
    pmix_setup_caddy_t *cd = (pmix_setup_caddy_t *) cbdata;
    pmix_server_caddy_t *scd = (pmix_server_caddy_t *) cd->cbdata;
    pmix_buffer_t *reply;
    pmix_status_t rc;
    pmix_iof_req_t *req;
    pmix_iof_cache_t *iof, *inxt;
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    PMIX_ACQUIRE_OBJECT(cd);

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        PMIX_RELEASE(scd);
        PMIX_RELEASE(cd);
        return;
    }

    /* setup the reply to the requestor */
    reply = PMIX_NEW(pmix_buffer_t);
    if (NULL == reply) {
        PMIX_ERROR_LOG(PMIX_ERR_NOMEM);
        goto cleanup;
    }
    /* start with the status */
    PMIX_BFROPS_PACK(rc, scd->peer, reply, &cd->status, 1, PMIX_STATUS);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(reply);
        goto cleanup;
    }

    /* was the request a success? */
    if (PMIX_SUCCESS != cd->status) {
        /* find and remove the tracker */
        req = (pmix_iof_req_t *) pmix_pointer_array_get_item(&pmix_globals.iof_requests,
                                                             cd->ncodes);
        if (NULL != req) {
            PMIX_RELEASE(req);
        }
        pmix_pointer_array_set_item(&pmix_globals.iof_requests, cd->ncodes, NULL);
    } else {
        /* return our reference ID for this handler */
        PMIX_BFROPS_PACK(rc, scd->peer, reply, &cd->ncodes, 1, PMIX_SIZE);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_RELEASE(reply);
            goto cleanup;
        }
    }

    pmix_output_verbose(2, pmix_server_globals.iof_output,
                        "server:_iofreg reply being sent to %s:%u", scd->peer->info->pname.nspace,
                        scd->peer->info->pname.rank);
    PMIX_SERVER_QUEUE_REPLY(rc, scd->peer, scd->hdr.tag, reply);
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(reply);
    }

    /* if the request succeeded, then process any cached IO - doing it here
     * guarantees that the IO will be received AFTER the client gets the
     * refid response */
    if (PMIX_SUCCESS == cd->status) {
        /* get the request */
        req = (pmix_iof_req_t *) pmix_pointer_array_get_item(&pmix_globals.iof_requests,
                                                             cd->ncodes);
        if (NULL != req) {
            PMIX_LIST_FOREACH_SAFE (iof, inxt, &pmix_server_globals.iof, pmix_iof_cache_t) {
                rc = pmix_iof_process_iof(iof->channel, &iof->source, iof->bo, iof->info,
                                            iof->ninfo, req);
                if (PMIX_OPERATION_SUCCEEDED == rc) {
                    pmix_list_remove_item(&pmix_server_globals.iof, &iof->super);
                    PMIX_RELEASE(iof);
                }
            }
        }
    }

cleanup:
    /* release the cached info */
    if (NULL != cd->procs) {
        PMIX_PROC_FREE(cd->procs, cd->nprocs);
    }
    PMIX_INFO_FREE(cd->info, cd->ninfo);
    /* We are done - and the switchyard's caddy is ours to release. The
     * handler returned PMIX_SUCCESS, so the switchyard did not touch it,
     * and scaddes does not reach cbdata. Without this the caddy - and the
     * retain it holds on the requesting peer - leaked on every IOF pull
     * registration, pinning that peer for the life of the server. See
     * _iofdreg, which does the same three releases. */
    PMIX_RELEASE(scd);
    PMIX_RELEASE(cd);
}

void pmix_server_iofreg_cbfunc(pmix_status_t status, void *cbdata)
{
    pmix_setup_caddy_t *cd = (pmix_setup_caddy_t *) cbdata;
    pmix_server_caddy_t *scd;

    if (NULL == cd) {
        /* nothing to do */
        return;
    }
    scd = (pmix_server_caddy_t *) cd->cbdata;

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        /* both caddies are ours here - the switchyard let go of the
         * server caddy when the handler returned PMIX_SUCCESS, and
         * _iofreg, the arm that would otherwise release it, will never
         * run. Releasing only the setup caddy stranded the peer retain */
        PMIX_RELEASE(scd);
        PMIX_RELEASE(cd);
        return;
    }
    pmix_output_verbose(2, pmix_server_globals.iof_output,
                        "server:iof_cbfunc called with status %d", status);

    cd->status = status;

    /* need to thread-shift this callback as it accesses global data */
    PMIX_THREADSHIFT(cd, _iofreg);
}

static void _iofdreg(int sd, short args, void *cbdata)
{
    pmix_shift_caddy_t *scdwrapper = (pmix_shift_caddy_t*)cbdata;
    pmix_setup_caddy_t *cd = (pmix_setup_caddy_t *) scdwrapper->cbdata;
    pmix_server_caddy_t *scd = (pmix_server_caddy_t *) cd->cbdata;
    pmix_buffer_t *reply;
    pmix_status_t rc;
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    /* the shifter is the object that was posted, so it is the one to
     * acquire - the setup caddy simply hangs off it */
    PMIX_ACQUIRE_OBJECT(scdwrapper);

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        PMIX_RELEASE(scd);
        PMIX_RELEASE(cd);
        PMIX_RELEASE(scdwrapper);
        return;
    }

    /* setup the reply to the requestor */
    reply = PMIX_NEW(pmix_buffer_t);
    if (NULL == reply) {
        PMIX_ERROR_LOG(PMIX_ERR_NOMEM);
        goto cleanup;
    }
    /* its just the status */
    PMIX_BFROPS_PACK(rc, scd->peer, reply, &scdwrapper->status, 1, PMIX_STATUS);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(reply);
        goto cleanup;
    }

    pmix_output_verbose(2, pmix_server_globals.iof_output,
                        "server:_iofreg reply being sent to %s:%u",
                        scd->peer->info->pname.nspace,
                        scd->peer->info->pname.rank);
    PMIX_SERVER_QUEUE_REPLY(rc, scd->peer, scd->hdr.tag, reply);
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(reply);
    }

cleanup:
    PMIX_RELEASE(scd);
    PMIX_RELEASE(cd);
    PMIX_RELEASE(scdwrapper);
}

void pmix_server_iofdereg_cbfunc(pmix_status_t status, void *cbdata)
{
    pmix_shift_caddy_t *scdwrapper;
    pmix_setup_caddy_t *cd = (pmix_setup_caddy_t *) cbdata;
    pmix_server_caddy_t *scd = (pmix_server_caddy_t *) cd->cbdata;

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        PMIX_RELEASE(scd);
        PMIX_RELEASE(cd);
        return;
    }

    pmix_output_verbose(2, pmix_server_globals.base_output,
                        "server:_iofdereg called with %s status",
                        PMIx_Error_string(status));

    /* need to thread-shift this callback */
    scdwrapper = PMIX_NEW(pmix_shift_caddy_t);
    if (NULL == scdwrapper) {
        /* we cannot answer the requestor, but we can at least let go of
         * the caddies - and the peer retain one of them holds */
        PMIX_ERROR_LOG(PMIX_ERR_NOMEM);
        PMIX_RELEASE(scd);
        PMIX_RELEASE(cd);
        return;
    }
    scdwrapper->status = status;
    scdwrapper->cbdata = cbdata;
    PMIX_THREADSHIFT(scdwrapper, _iofdreg);
}
