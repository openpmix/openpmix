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

/* Standard I/O forwarding: the inbound handler for output arriving from
 * our host or another server, and the public APIs by which a host pushes
 * output at us and throttles it. */

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

#include "src/common/pmix_iof.h"
#include "src/mca/bfrops/base/base.h"
#include "src/mca/ptl/base/base.h"
#include "src/runtime/pmix_progress_threads.h"
#include "src/util/pmix_error.h"
#include "src/util/pmix_name_fns.h"
#include "src/util/pmix_output.h"

#include "pmix_server_ops.h"

/* defined below, beside the spawn-time inheritance it is the other half
 * of - _iofdeliver reaches for it on its miss path */
static size_t inherit_from_ancestry(const pmix_proc_t *source);

void pmix_server_iof_handler(struct pmix_peer_t *pr, pmix_ptl_hdr_t *hdr,
                            pmix_buffer_t *buf, void *cbdata)
{
    pmix_peer_t *peer = (pmix_peer_t *) pr;
    pmix_proc_t source;
    pmix_iof_channel_t channel;
    pmix_byte_object_t bo;
    int32_t cnt;
    pmix_status_t rc;
    size_t refid, ninfo = 0;
    pmix_iof_req_t *req;
    pmix_info_t *info = NULL;

    PMIX_HIDE_UNUSED_PARAMS(hdr, cbdata);

    pmix_output_verbose(2, pmix_server_globals.iof_output,
                        "recvd IOF with %d bytes from %s",
                        (int) buf->bytes_used,
                        PMIX_PNAME_PRINT(&peer->info->pname));

    /* if the buffer is empty, they are simply closing the socket */
    if (0 == buf->bytes_used) {
        return;
    }
    PMIX_BYTE_OBJECT_CONSTRUCT(&bo);

    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, peer, buf, &source, &cnt, PMIX_PROC);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return;
    }
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, peer, buf, &channel, &cnt, PMIX_IOF_CHANNEL);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return;
    }
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, peer, buf, &refid, &cnt, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return;
    }
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, peer, buf, &ninfo, &cnt, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return;
    }
    /* this count came off the wire and is about to be multiplied by
     * sizeof(pmix_info_t) to size an allocation whose element
     * constructors then walk it, so a value large enough to wrap that
     * product runs off the end of a short block before any NULL check
     * gets a say. Require it to survive the round trip through the
     * int32_t the unpack consumes it as - the screen the fence and
     * event handlers use */
    cnt = ninfo;
    if (0 > cnt || (size_t) cnt != ninfo) {
        PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
        return;
    }
    if (0 < ninfo) {
        PMIX_INFO_CREATE(info, ninfo);
        cnt = ninfo;
        PMIX_BFROPS_UNPACK(rc, peer, buf, info, &cnt, PMIX_INFO);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            goto cleanup;
        }
    }
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, peer, buf, &bo, &cnt, PMIX_BYTE_OBJECT);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto cleanup;
    }
    /* lookup the handler for this IOF package */
    req = (pmix_iof_req_t *) pmix_pointer_array_get_item(&pmix_globals.iof_requests, refid);
    if (NULL != req && NULL != req->cbfunc) {
        req->cbfunc(refid, channel, &source, &bo, info, ninfo);
    } else {
        /* otherwise, simply write it out to the specified std IO channel */
        if (NULL != bo.bytes && 0 < bo.size) {
            pmix_iof_write_output(&source, channel, &bo);
        }
    }

cleanup:
    /* cleanup the memory */
    if (0 < ninfo) {
        PMIX_INFO_FREE(info, ninfo);
    }
    PMIX_BYTE_OBJECT_DESTRUCT(&bo);
}

/* the progress-thread half of PMIx_server_IOF_deliver: write the output
 * locally if we are meant to, hand it to every matching registration,
 * and cache it if nobody has registered for it yet */
static void _iofdeliver(int sd, short args, void *cbdata)
{
    pmix_setup_caddy_t *cd = (pmix_setup_caddy_t *) cbdata;
    pmix_iof_req_t *req;
    bool found = false;
    bool outputlocal;
    pmix_iof_cache_t *iof;
    int i;
    size_t n;
    /* the local write used to be unconditional and was what first set this;
     * skipping it must not leave the completion status undefined */
    pmix_status_t rc = PMIX_SUCCESS;

    PMIX_ACQUIRE_OBJECT(cd);
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    pmix_output_verbose(2, pmix_server_globals.iof_output,
                        "PMIX:SERVER delivering IOF from %s on channel %s with %d bytes",
                        PMIX_NAME_PRINT(cd->procs),
                        PMIx_IOF_channel_string(cd->channels),
                        (int)cd->bo->size);

    /* The host may be handing us output that is not ours to emit - a runtime
     * relaying another node's output to us solely because a tool attached
     * here asked for it is the case this exists for. Writing it out again
     * here would duplicate whatever the server that owns those processes
     * already wrote, and with output-to-file directives in play that means
     * two servers writing the same file. So let the host say so per
     * delivery, using the same attribute that says it at registration. */
    outputlocal = true;
    for (n = 0; n < cd->ninfo; n++) {
        if (PMIX_CHECK_KEY(&cd->info[n], PMIX_IOF_LOCAL_OUTPUT)) {
            outputlocal = PMIX_INFO_TRUE(&cd->info[n]);
            break;
        }
    }

    /* output it locally if requested */
    if (outputlocal) {
        rc = pmix_iof_write_output(cd->procs, cd->channels, cd->bo);
        if (0 > rc) {
            goto done;
        }
    }

    /* cycle across our list of IOF requests and see who wants
     * this channel from this source */
    for (i = 0; i < pmix_globals.iof_requests.size; i++) {
        req = (pmix_iof_req_t *) pmix_pointer_array_get_item(&pmix_globals.iof_requests, i);
        if (NULL == req) {
            continue;
        }
        rc = pmix_iof_process_iof(cd->channels, cd->procs, cd->bo, cd->info, cd->ninfo, req);
        if (PMIX_OPERATION_SUCCEEDED == rc) {
            /* flag that we do have at least one registrant for this info,
             * so there is no need to cache it */
            found = true;
            rc = PMIX_SUCCESS;
        }
    }

    /* Nobody here is subscribed to this namespace - but it may be the
     * child of one they are. Ask before the bytes go into the cache to
     * age out: this is the only point at which the server that RECEIVES
     * a spawned job's output gets to make the inheritance decision, and
     * on a multi-node DVM it is not the server that processed the spawn.
     * A match leaves clones behind, so the walk above answers every
     * later chunk and this runs once per namespace rather than once per
     * chunk. */
    if (!found && 0 < inherit_from_ancestry(cd->procs)) {
        for (i = 0; i < pmix_globals.iof_requests.size; i++) {
            req = (pmix_iof_req_t *) pmix_pointer_array_get_item(&pmix_globals.iof_requests, i);
            if (NULL == req) {
                continue;
            }
            rc = pmix_iof_process_iof(cd->channels, cd->procs, cd->bo, cd->info, cd->ninfo, req);
            if (PMIX_OPERATION_SUCCEEDED == rc) {
                found = true;
                rc = PMIX_SUCCESS;
            }
        }
    }

    /* if nobody has registered for this yet, then cache it */
    if (!found) {
        pmix_output_verbose(2, pmix_server_globals.iof_output,
                            "PMIx:SERVER caching IOF %d",
                            (int)cd->bo->size);
        if (pmix_server_globals.max_iof_cache == pmix_list_get_size(&pmix_server_globals.iof)) {
            /* remove the oldest cached message */
            iof = (pmix_iof_cache_t *) pmix_list_remove_first(&pmix_server_globals.iof);
            if (NULL != iof) {
                PMIX_RELEASE(iof);
            }
        }
        /* add this output to our cache so it is cached until someone
         * registers to receive it */
        iof = PMIX_NEW(pmix_iof_cache_t);
        memcpy(&iof->source, cd->procs, sizeof(pmix_proc_t));
        iof->channel = cd->channels;
        /* copy the data */
        PMIX_BYTE_OBJECT_CREATE(iof->bo, 1);
        if (0 < cd->bo->size) {
            iof->bo->bytes = (char *) malloc(cd->bo->size);
            memcpy(iof->bo->bytes, cd->bo->bytes, cd->bo->size);
        }
        iof->bo->size = cd->bo->size;
        if (0 < cd->ninfo) {
            PMIX_INFO_CREATE(iof->info, cd->ninfo);
            iof->ninfo = cd->ninfo;
            for (n = 0; n < iof->ninfo; n++) {
                PMIX_INFO_XFER(&iof->info[n], &cd->info[n]);
            }
        }
        pmix_list_append(&pmix_server_globals.iof, &iof->super);
        rc = PMIX_SUCCESS;
    }

done:
    if (NULL != cd->opcbfunc) {
        cd->opcbfunc(rc, cd->cbdata);
    }

    /* release the caddy */
    cd->procs = NULL;
    cd->nprocs = 0;
    cd->info = NULL;
    cd->ninfo = 0;
    cd->bo = NULL;
    PMIX_RELEASE(cd);
}

pmix_status_t PMIx_server_IOF_deliver(const pmix_proc_t *source, pmix_iof_channel_t channel,
                                      const pmix_byte_object_t *bo, const pmix_info_t info[],
                                      size_t ninfo, pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    pmix_setup_caddy_t *cd;
    pmix_lock_t mylock;
    pmix_status_t rc;

    if (!pmix_atomic_check_bool(&pmix_globals.initialized)) {
        return PMIX_ERR_INIT;
    }

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        return PMIX_ERR_NOT_AVAILABLE;
    }

    /* need to threadshift this request */
    cd = PMIX_NEW(pmix_setup_caddy_t);
    if (NULL == cd) {
        return PMIX_ERR_NOMEM;
    }
    cd->procs = (pmix_proc_t *) source;
    cd->nprocs = 1;
    cd->channels = channel;
    cd->bo = (pmix_byte_object_t *) bo;
    cd->info = (pmix_info_t *) info;
    cd->ninfo = ninfo;
    cd->opcbfunc = cbfunc;
    cd->cbdata = cbdata;

    /* if the provided callback is NULL, then substitute
     * our own internal cbfunc and block here */
    if (NULL == cbfunc) {
        if (pmix_progress_thread_check_blocking("PMIx_server_IOF_deliver")) {
            /* we are ON the progress thread, so waiting for the event we
             * would post is waiting for ourselves - answer rather than
             * hang. The caller wanted the blocking form; the non-blocking
             * one works fine from here. The caddy is only borrowing the
             * caller's proc and byte object, and its destructor frees both
             * unconditionally - detach them as _iofdeliver does before
             * handing the caddy back */
            cd->procs = NULL;
            cd->nprocs = 0;
            cd->bo = NULL;
            PMIX_RELEASE(cd);
            return PMIX_ERR_WOULD_BLOCK;
        }
        PMIX_CONSTRUCT_LOCK(&mylock);
        cd->opcbfunc = pmix_server_lock_opcbfunc;
        cd->cbdata = &mylock;
        PMIX_THREADSHIFT(cd, _iofdeliver);
        PMIX_WAIT_THREAD(&mylock);
        rc = mylock.status;
        PMIX_DESTRUCT_LOCK(&mylock);
        if (PMIX_SUCCESS == rc) {
            rc = PMIX_OPERATION_SUCCEEDED;
        }
        return rc;
    }

    PMIX_THREADSHIFT(cd, _iofdeliver);
    return PMIX_SUCCESS;
}

static void _iofflowcontrol(int sd, short args, void *cbdata)
{
    pmix_setup_caddy_t *cd = (pmix_setup_caddy_t *) cbdata;
    pmix_status_t rc;
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    PMIX_ACQUIRE_OBJECT(cd);

    rc = pmix_iof_flow_control(cd->procs, cd->channels, cd->xoff,
                               cd->info, cd->ninfo);

    if (NULL != cd->opcbfunc) {
        cd->opcbfunc(rc, cd->cbdata);
    }

    /* the caddy is only borrowing the caller's arrays */
    cd->procs = NULL;
    cd->nprocs = 0;
    cd->info = NULL;
    cd->ninfo = 0;
    PMIX_RELEASE(cd);
}

pmix_status_t PMIx_server_IOF_flow_control(const pmix_proc_t *source,
                                           pmix_iof_channel_t channel,
                                           bool xoff,
                                           const pmix_info_t directives[], size_t ndirs,
                                           pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    pmix_setup_caddy_t *cd;
    pmix_lock_t mylock;
    pmix_status_t rc;

    if (!pmix_atomic_check_bool(&pmix_globals.initialized)) {
        return PMIX_ERR_INIT;
    }

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        return PMIX_ERR_NOT_AVAILABLE;
    }

    /* only stdin can be flow controlled - reject anything else here so
     * the caller finds out before we go anywhere near the event base */
    if (!(PMIX_FWD_STDIN_CHANNEL & channel)) {
        return PMIX_ERR_NOT_SUPPORTED;
    }

    /* need to threadshift this request - it walks the client array and
     * the stdin read event, both of which belong to the progress thread */
    cd = PMIX_NEW(pmix_setup_caddy_t);
    if (NULL == cd) {
        return PMIX_ERR_NOMEM;
    }
    cd->procs = (pmix_proc_t *) source;
    cd->nprocs = (NULL == source) ? 0 : 1;
    cd->channels = channel;
    cd->xoff = xoff;
    cd->info = (pmix_info_t *) directives;
    cd->ninfo = ndirs;
    cd->opcbfunc = cbfunc;
    cd->cbdata = cbdata;

    /* if the provided callback is NULL, then substitute
     * our own internal cbfunc and block here */
    if (NULL == cbfunc) {
        if (pmix_progress_thread_check_blocking("PMIx_server_IOF_flow_control")) {
            /* we are ON the progress thread, so waiting for the event we
             * would post is waiting for ourselves - answer rather than
             * hang. The caller wanted the blocking form; the non-blocking
             * one works fine from here. Detach the caller's proc first -
             * the caddy is only borrowing it, but its destructor frees it
             * unconditionally */
            cd->procs = NULL;
            cd->nprocs = 0;
            PMIX_RELEASE(cd);
            return PMIX_ERR_WOULD_BLOCK;
        }
        PMIX_CONSTRUCT_LOCK(&mylock);
        cd->opcbfunc = pmix_server_lock_opcbfunc;
        cd->cbdata = &mylock;
        PMIX_THREADSHIFT(cd, _iofflowcontrol);
        PMIX_WAIT_THREAD(&mylock);
        rc = mylock.status;
        PMIX_DESTRUCT_LOCK(&mylock);
        if (PMIX_SUCCESS == rc) {
            rc = PMIX_OPERATION_SUCCEEDED;
        }
        return rc;
    }

    PMIX_THREADSHIFT(cd, _iofflowcontrol);
    return PMIX_SUCCESS;
}

/* Hand the output already in the cache to the subscriptions just built
 * for a job. A job's first bytes routinely beat its own spawn reply back
 * to us, so without this the opening lines of every spawned job are lost.
 *
 * This is driven by namespace rather than by one request because a job
 * can inherit more than one watcher, and a cache entry is consumed the
 * moment it is forwarded: draining per-request would give the cached
 * head of the stream to whichever watcher happened to be cloned first
 * and the rest of it to all of them. Every request naming the job is
 * offered each entry, and the entry is dropped once anyone took it. */
static void drain_cache(const char *nspace)
{
    pmix_iof_cache_t *iof, *ionext;
    pmix_iof_req_t *req;
    pmix_status_t rc;
    bool taken;
    int i;

    PMIX_LIST_FOREACH_SAFE (iof, ionext, &pmix_server_globals.iof, pmix_iof_cache_t) {
        taken = false;
        for (i = 0; i < pmix_globals.iof_requests.size; i++) {
            req = (pmix_iof_req_t *) pmix_pointer_array_get_item(&pmix_globals.iof_requests, i);
            if (NULL == req || 0 == req->nprocs) {
                continue;
            }
            if (!PMIX_CHECK_NSPACE(req->procs[0].nspace, nspace)) {
                continue;
            }
            /* the source still has to match, so an entry from some other
             * job is simply declined here */
            rc = pmix_iof_process_iof(iof->channel, &iof->source, iof->bo,
                                      iof->info, iof->ninfo, req);
            if (PMIX_OPERATION_SUCCEEDED == rc) {
                taken = true;
            }
        }
        if (taken) {
            /* remove it from the list since it has now been forwarded */
            pmix_list_remove_item(&pmix_server_globals.iof, &iof->super);
            PMIX_RELEASE(iof);
        }
    }
}

/* Clone every subscription covering "parent" onto "nspace".
 *
 * "The parent's settings" are not recorded anywhere as settings - what
 * exists is the set of live subscriptions covering the spawning process's
 * own job, which is the same thing seen from the other end: whoever is
 * receiving the parent job's output is who should receive the child's.
 * So each matching request is cloned onto the child's namespace with the
 * same requestor, channels and formatting.
 *
 * The clone keeps the original's remote_id deliberately, so the child's
 * output arrives at the requestor under the same handler id the parent's
 * does and is treated identically there.
 *
 * Returns the number of subscriptions cloned - zero means nobody is
 * watching that parent here.
 */
static size_t clone_iof_reqs(const pmix_proc_t *parent, const char *nspace)
{
    pmix_iof_req_t *req, *nreq;
    size_t m, ninherited = 0;
    int i, limit;

    /* the array grows as we add to it, so bound the walk to what was
     * there when we started - a clone can never be its own parent */
    limit = pmix_globals.iof_requests.size;
    for (i = 0; i < limit; i++) {
        req = (pmix_iof_req_t *) pmix_pointer_array_get_item(&pmix_globals.iof_requests, i);
        if (NULL == req) {
            continue;
        }
        /* a request with no requestor peer was registered by this process
         * for itself and is delivered through its own callback; there is
         * nothing about it to give the child. The same screens
         * pmix_iof_process_iof applies before forwarding apply here
         * before cloning - a request it would refuse is not worth
         * inheriting - and the info read is load-bearing rather than
         * defensive, because the verbose line below dereferences it */
        if (NULL == req->requestor || NULL == req->requestor->info ||
            req->requestor->finalized) {
            continue;
        }
        if (PMIX_FWD_NO_CHANNELS == req->channels) {
            continue;
        }
        /* does it cover the job the spawning process belongs to? A
         * request registered for the job as a whole carries
         * PMIX_RANK_WILDCARD, which PMIX_CHECK_PROCID matches */
        for (m = 0; m < req->nprocs; m++) {
            if (PMIX_CHECK_PROCID(parent, &req->procs[m])) {
                break;
            }
        }
        if (m >= req->nprocs) {
            continue;
        }

        nreq = PMIX_NEW(pmix_iof_req_t);
        if (NULL == nreq) {
            return ninherited;
        }
        PMIX_PROC_CREATE(nreq->procs, 1);
        if (NULL == nreq->procs) {
            PMIX_RELEASE(nreq);
            return ninherited;
        }
        nreq->nprocs = 1;
        PMIX_LOAD_PROCID(&nreq->procs[0], nspace, PMIX_RANK_WILDCARD);
        PMIX_RETAIN(req->requestor);
        nreq->requestor = req->requestor;
        nreq->channels = req->channels;
        nreq->remote_id = req->remote_id;
        /* a shallow copy of the flags owns nothing - see the note in
         * pmix_server_process_iof below */
        nreq->flags = req->flags;
        nreq->flags.file = NULL;
        nreq->flags.directory = NULL;
        nreq->local_id = pmix_pointer_array_add(&pmix_globals.iof_requests, nreq);
        ++ninherited;

        pmix_output_verbose(2, pmix_server_globals.iof_output,
                            "PMIx:SERVER job %s inheriting %s forwarding to %s from %s",
                            nspace, PMIx_IOF_channel_string(nreq->channels),
                            PMIX_PNAME_PRINT(&req->requestor->info->pname),
                            PMIX_NAME_PRINT(parent));
    }
    /* every clone exists now, so the cached head of the stream can be
     * shared out among all of them */
    if (0 < ninherited) {
        drain_cache(nspace);
    }
    return ninherited;
}

/* Give the child job the output forwarding its parent has, at the moment
 * the spawn completes. The spawning process is the parent, and it is a
 * local client of ours - so this only ever finds anything on the server
 * that processed the spawn. See inherit_from_ancestry() for the other
 * half, which runs where the output arrives.
 */
static size_t inherit_parent_iof(pmix_setup_caddy_t *cd, char nspace[])
{
    pmix_proc_t parent;

    if (NULL == cd->peer || NULL == cd->peer->info) {
        return 0;
    }
    PMIX_LOAD_PROCID(&parent, cd->peer->info->pname.nspace,
                     cd->peer->info->pname.rank);

    return clone_iof_reqs(&parent, nspace);
}

/* How far the ancestry walk below will climb. The chain is data - each
 * link is a PMIX_PARENT_ID the host stored - so a bound is what keeps a
 * malformed or circular one from spinning the progress thread. Nothing
 * legitimate is anywhere near this deep.
 */
#define PMIX_IOF_MAX_ANCESTRY 16

/* Who spawned the job this process belongs to?
 *
 * PMIX_PARENT_ID is per-process job data supplied by the host, so it is
 * available on any server the namespace was registered with - including
 * ones that had nothing to do with the spawn, which is the entire point
 * of the caller. A host that records it for the job as a whole rather
 * than per rank (pmix_pfexec does) is answered by the second fetch.
 */
static bool get_parent_id(const pmix_proc_t *proc, pmix_proc_t *parent)
{
    pmix_cb_t cb;
    pmix_info_t optional;
    pmix_kval_t *kv;
    pmix_proc_t wild;
    pmix_status_t rc;
    bool found = false;
    int pass;

    PMIX_INFO_LOAD(&optional, PMIX_OPTIONAL, NULL, PMIX_BOOL);
    PMIX_LOAD_PROCID(&wild, proc->nspace, PMIX_RANK_WILDCARD);

    for (pass = 0; pass < 2 && !found; pass++) {
        if (1 == pass && PMIX_RANK_WILDCARD == proc->rank) {
            break;      // the second fetch would repeat the first
        }
        PMIX_CONSTRUCT(&cb, pmix_cb_t);
        cb.proc = (0 == pass) ? (pmix_proc_t *) proc : &wild;
        cb.key = PMIX_PARENT_ID;
        cb.info = &optional;
        cb.ninfo = 1;
        PMIX_GDS_FETCH_KV(rc, pmix_globals.mypeer, &cb);
        if (PMIX_SUCCESS == rc || PMIX_OPERATION_SUCCEEDED == rc) {
            kv = (pmix_kval_t *) pmix_list_remove_first(&cb.kvs);
            /* the union must not be read until the type says which member
             * is live, and PMIX_PROC carries a pointer that a mistyped or
             * truncated store can leave NULL */
            if (NULL != kv && NULL != kv->value && PMIX_PROC == kv->value->type &&
                NULL != kv->value->data.proc) {
                PMIX_LOAD_PROCID(parent, kv->value->data.proc->nspace,
                                 kv->value->data.proc->rank);
                found = true;
            }
            if (NULL != kv) {
                PMIX_RELEASE(kv);
            }
        }
        PMIX_DESTRUCT(&cb);
    }
    PMIX_INFO_DESTRUCT(&optional);

    return found;
}

/* The delivery-time half of inheritance: output has arrived for a
 * namespace nobody here is subscribed to. Before it is cached and lost,
 * ask whether it belongs to a job that was SPAWNED by one somebody here
 * IS subscribed to - a subscription covering a parent job covers its
 * children.
 *
 * This is what makes inheritance work when the spawn was processed
 * somewhere else. pmix_server_process_iof() runs on the server hosting
 * the spawning process, which on a multi-node DVM is not the server
 * holding the watching tool's subscription; this runs on the server the
 * output actually reaches, which is that one. Neither needs the other to
 * have happened, and no subscription crosses the wire.
 *
 * The walk is transitive because "treated the way its parent is treated"
 * says nothing about depth - a grandchild of a watched job is watched.
 * It stops at the first ancestor anybody here is subscribed to: once the
 * clones exist, that generation's watchers are this namespace's watchers.
 *
 * A match CLONES rather than delivering once, so every later chunk from
 * this namespace takes the ordinary path above. That costs one entry per
 * watcher and gives the fallback the same lifetime the spawn-time clones
 * have; re-deriving it per chunk would put a datastore fetch in front of
 * every line of a watched job's output.
 */
static size_t inherit_from_ancestry(const pmix_proc_t *source)
{
    pmix_proc_t anc, parent;
    size_t ninherited;
    int depth;

    PMIX_LOAD_PROCID(&anc, source->nspace, source->rank);

    for (depth = 0; depth < PMIX_IOF_MAX_ANCESTRY; depth++) {
        if (!get_parent_id(&anc, &parent)) {
            return 0;       // no parent recorded - this job is a root
        }
        if (PMIX_CHECK_NSPACE(parent.nspace, anc.nspace)) {
            return 0;       // a job cannot have spawned itself
        }
        ninherited = clone_iof_reqs(&parent, source->nspace);
        if (0 < ninherited) {
            pmix_output_verbose(2, pmix_server_globals.iof_output,
                                "PMIx:SERVER job %s inherited %d subscription(s) "
                                "from ancestor %s at delivery",
                                source->nspace, (int) ninherited,
                                PMIX_NAME_PRINT(&parent));
            return ninherited;
        }
        PMIX_LOAD_PROCID(&anc, parent.nspace, parent.rank);
    }

    return 0;
}

pmix_status_t pmix_server_process_iof(pmix_setup_caddy_t *cd,
                                      char nspace[])
{
    pmix_iof_req_t *req;

    /* Nothing in the spawn request said which channels to forward, so
     * the child job takes its parent's - and if the parent has nobody
     * watching it, there is nowhere for the child's output to go
     * either. We deliberately do NOT fall back to forwarding it to the
     * spawner: output is never forwarded to an application process,
     * which would only emit it on its own stdout for the runtime to
     * pick up and forward again. */
    if (cd->inherit_iof) {
        if (0 == inherit_parent_iof(cd, nspace)) {
            pmix_output_verbose(2, pmix_server_globals.iof_output,
                                "PMIx:SERVER job %s has nothing to inherit - "
                                "its parent's output is not being forwarded",
                                nspace);
        }
        return PMIX_SUCCESS;
    }

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
    if (NULL == req->procs) {
        /* what follows is a load, not an unpack, so nothing downstream
         * would screen a NULL here - the same reason pmix_server_iofreg
         * checks its own proc allocation */
        PMIX_RELEASE(req);
        return PMIX_ERR_NOMEM;
    }
    PMIX_LOAD_PROCID(&req->procs[0], nspace, PMIX_RANK_WILDCARD);
    req->channels = cd->channels;
    /* a shallow copy of the flags owns nothing: file and directory are
     * strdup'ed by the flag parser and freed with the caddy, which goes
     * away long before this request does. The client and tool sides of
     * the same copy say so explicitly - do the same here */
    req->flags = cd->flags;
    req->flags.file = NULL;
    req->flags.directory = NULL;
    req->local_id = pmix_pointer_array_add(&pmix_globals.iof_requests, req);
    /* process any cached IO */
    drain_cache(nspace);
    return PMIX_SUCCESS;
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
    /* screen the count before it sizes an allocation - see the note in
     * pmix_server_iof_handler. This one is copied again below, walking
     * the size_t rather than the int32_t the unpack consumed */
    cnt = cd->nprocs;
    if (0 > cnt || (size_t) cnt != cd->nprocs) {
        rc = PMIX_ERR_BAD_PARAM;
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
    cnt = cd->ninfo;
    if (0 > cnt || (size_t) cnt != cd->ninfo) {
        rc = PMIX_ERR_BAD_PARAM;
        PMIX_ERROR_LOG(rc);
        goto exit;
    }
    /* unpack the directives */
    if (0 < cd->ninfo) {
        PMIX_INFO_CREATE(cd->info, cd->ninfo);
        /* we unpacked this array, so the caddy owns it - without the flag
         * the destructor leaves it behind on every path that does not
         * reach _iofreg's explicit free, which includes every error
         * return below and _iofreg's own finalize-race early-out */
        cd->copied = true;
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
        if (NULL == req->procs) {
            /* nothing screens this one: what follows is a memcpy, not an
             * unpack, so a failed allocation is a NULL destination that
             * nobody would catch */
            PMIX_RELEASE(req);
            rc = PMIX_ERR_NOMEM;
            goto exit;
        }
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
    if (PMIX_SUCCESS == rc) {
        /* the host accepted the request and now owns cd - it will release
         * it when it invokes the callback. Falling through to the exit
         * label here would free the caddy out from under the host and
         * leave the callback operating on freed memory. */
        return PMIX_SUCCESS;
    }

    /* the host refused, so take the registration back out of the array.
     * _iofreg does exactly this when the host fails the request
     * asynchronously; the synchronous refusal left the entry behind,
     * where it pinned the requestor's peer with its retain for the life
     * of the server and went on matching output for a pull that was
     * never granted */
    pmix_pointer_array_set_item(&pmix_globals.iof_requests, req->local_id, NULL);
    PMIX_RELEASE(req);

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
    /* screen the count before the "+ 1" below: a wire value near
     * SIZE_MAX wraps that sum down to zero, PMIx_Info_create answers
     * NULL for a zero-element array, and the stop directive would then
     * be seeded at info[SIZE_MAX]. The same wrap can happen inside the
     * allocator's "ninfo * sizeof(pmix_info_t)". This is the screen the
     * group and collective handlers carry for the same shape */
    cnt = ninfo;
    if (0 > cnt || (size_t) cnt != ninfo) {
        rc = PMIX_ERR_BAD_PARAM;
        PMIX_ERROR_LOG(rc);
        goto exit;
    }
    /* unpack the directives - note that we have to add one
     * to tell the server to stop forwarding to this channel */
    cd->ninfo = ninfo + 1;
    PMIX_INFO_CREATE(cd->info, cd->ninfo);
    if (NULL == cd->info) {
        rc = PMIX_ERR_NOMEM;
        goto exit;
    }
    /* we built this array, so the caddy owns it. Nothing else frees it:
     * unlike _iofreg, the deregistration completion has no explicit
     * free, so without the flag every deregistration leaked its
     * directives - and so did every error return below */
    cd->copied = true;
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
    if (PMIX_SUCCESS == rc) {
        /* the host accepted the request and now owns cd - see the
         * matching note in pmix_server_iofreg */
        return PMIX_SUCCESS;
    }

exit:
    /* mirror pmix_server_iofreg: on an async SUCCESS the host owns cd and
     * releases it via the callback; on any error path here it was never
     * handed off, so release it (previously it leaked on every error). */
    PMIX_RELEASE(cd);
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

    /* remember that this peer feeds us stdin - it is who IOF flow
     * control has to reach if our host tells us to stop taking it */
    peer->stdin_producer = true;

    /* unpack the number of targets */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, peer, buf, &cd->nprocs, &cnt, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto error;
    }
    /* screen the count before it sizes an allocation - see the note in
     * pmix_server_iof_handler */
    cnt = cd->nprocs;
    if (0 > cnt || (size_t) cnt != cd->nprocs) {
        rc = PMIX_ERR_BAD_PARAM;
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
    cnt = cd->ninfo;
    if (0 > cnt || (size_t) cnt != cd->ninfo) {
        rc = PMIX_ERR_BAD_PARAM;
        PMIX_ERROR_LOG(rc);
        goto error;
    }
    if (0 < cd->ninfo) {
        PMIX_INFO_CREATE(cd->info, cd->ninfo);
        if (NULL == cd->info) {
            rc = PMIX_ERR_NOMEM;
            goto error;
        }
        /* we unpacked this array, so the caddy owns it - without the
         * flag the destructor leaves it behind on every error return
         * below, which is the only thing that frees it there */
        cd->copied = true;
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
    /* the destructor frees the *bytes* only for the nbo objects it is
     * told about, so leaving this at zero would free the container and
     * leak the payload on every error return below. Note this is safe
     * only because the object is ours: PMIx_server_IOF_deliver parks a
     * borrowed one on this same member and detaches it instead */
    cd->nbo = 1;

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
        if (PMIX_OPERATION_SUCCEEDED == rc) {
            /* the host completed atomically and will not call back - invoke
             * the callback ourselves (it sends the reply and releases cd)
             * and tell the switchyard we are done. Previously this path
             * leaked cd (its procs, info, and byte object). */
            stdcbfunc(PMIX_SUCCESS, cd);
            return PMIX_SUCCESS;
        }
        goto error;
    }
    return rc;

error:
    PMIX_RELEASE(cd);
    return rc;
}
