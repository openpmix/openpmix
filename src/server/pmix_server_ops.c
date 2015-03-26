/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2014-2015 Intel, Inc.  All rights reserved.
 * Copyright (c) 2014      Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2014-2015 Artem Y. Polyakov <artpol84@gmail.com>.
 *                         All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "pmix_config.h"
#include "src/api/pmix_server.h"
#include "src/include/types.h"
#include "src/include/pmix_globals.h"
#include "src/include/pmix_stdint.h"
#include "src/include/pmix_socket_errno.h"

#ifdef HAVE_STRING_H
#include <string.h>
#endif
#include <fcntl.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#ifdef HAVE_SYS_SOCKET_H
#include <sys/socket.h>
#endif
#ifdef HAVE_SYS_UN_H
#include <sys/un.h>
#endif
#ifdef HAVE_SYS_UIO_H
#include <sys/uio.h>
#endif
#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif
#include <event.h>

#include "src/class/pmix_list.h"
#include "src/buffer_ops/buffer_ops.h"
#include "src/util/argv.h"
#include "src/util/error.h"
#include "src/util/output.h"
#include "src/util/pmix_environ.h"
#include "src/util/progress_threads.h"
#include "src/usock/usock.h"
#include "src/sec/pmix_sec.h"

#include "pmix_server_ops.h"

pmix_server_module_t pmix_host_server;

pmix_status_t pmix_server_abort(pmix_peer_t *peer, pmix_buffer_t *buf,
                                pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    int32_t cnt;
    pmix_status_t rc;
    int status;
    char *msg;
    
    pmix_output_verbose(2, pmix_globals.debug_output, "recvd ABORT");
    
    /* unpack the status */
    cnt = 1;
    if (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(buf, &status, &cnt, PMIX_INT))) {
        return rc;
    }
    /* unpack the message */
    cnt = 1;
    if (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(buf, &msg, &cnt, PMIX_STRING))) {
        return rc;
    }
    /* let the local host's server execute it */
    if (NULL != pmix_host_server.abort) {
        rc = pmix_host_server.abort(peer->info->nptr->nspace, peer->info->rank,
                                    peer->info->server_object, status, msg, cbfunc, cbdata);
    } else {
        rc = PMIX_ERR_NOT_SUPPORTED;
    }

    /* the client passed this msg to us so we could give
     * it to the host server - we are done with it now */
    if (NULL != msg) {
        free(msg);
    }

    return rc;
}

pmix_status_t pmix_server_commit(pmix_peer_t *peer, pmix_buffer_t *buf)
{
    int32_t cnt;
    pmix_status_t rc=PMIX_SUCCESS;
    pmix_modex_data_t mdx;
    pmix_scope_t scope;
    pmix_buffer_t *bptr;

    /* unpack any provided data blobs */
    cnt = 1;
    (void)strncpy(mdx.nspace, peer->info->nptr->nspace, PMIX_MAX_NSLEN);
    mdx.rank = peer->info->rank;
    while (PMIX_SUCCESS == pmix_bfrop.unpack(buf, &scope, &cnt, PMIX_SCOPE)) {
        cnt = 1;
        if (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(buf, &bptr, &cnt, PMIX_BUFFER))) {
            goto cleanup;
        }
        /* let the local host's server store it */
        mdx.blob = (uint8_t*)bptr->base_ptr;
        mdx.size = bptr->bytes_used;
        if (NULL != pmix_host_server.store_modex) {
            pmix_host_server.store_modex(peer->info->nptr->nspace,
                                         peer->info->rank,
                                         peer->info->server_object,
                                         scope, &mdx);
        }
        PMIX_RELEASE(bptr);
        cnt = 1;
    }
 cleanup:
    return rc;
}


bool pmix_server_trk_update(pmix_server_trkr_t *trk)
{
    size_t j;
    pmix_range_trkr_t *rtrk;
    pmix_nspace_t *nptr;
    pmix_rank_info_t *info;
    bool complete=true;
    
    pmix_output_verbose(5, pmix_globals.debug_output,
                        "trk_update");
    
    /* if the definition for this tracker is complete, then
     * there is nothing we need do */
    if (trk->def_complete) {
        pmix_output_verbose(5, pmix_globals.debug_output,
                            "trk_update: trk complete");
        return true;
    }
    
    /* no simple way to do this - just have to perform an
     * exhaustive search across the ranges in this tracker.
     * Fortunately, there typically is only one */
    PMIX_LIST_FOREACH(rtrk, &trk->ranges, pmix_range_trkr_t) {
        pmix_output_verbose(5, pmix_globals.debug_output,
                            "trk_update: checking range");
        /* see if the nspace for this tracker is known */
        if (NULL == rtrk->nptr) {
            pmix_output_verbose(5, pmix_globals.debug_output,
                                "trk_update: looking for nspace %s",
                                rtrk->range->nspace);
            /* okay, see if we can find it - the nspace may have
             * been defined since the last time we looked */
            PMIX_LIST_FOREACH(nptr, &pmix_server_globals.nspaces, pmix_nspace_t) {
                if (0 == strcmp(rtrk->range->nspace, nptr->nspace)) {
                    PMIX_RETAIN(nptr);
                    rtrk->nptr = nptr;
                    break;
                }
            }
        }
        if (NULL != rtrk->nptr) {
            if (!rtrk->contribution_added) {
                /* if the ranks for this range is NULL, then all procs
                 * participate - we know the number of local procs we
                 * will have, so we can take care of it now */
                if (NULL == rtrk->range->ranks) {
                    pmix_output_verbose(5, pmix_globals.debug_output,
                                        "trk_update: all %d local procs in nspace %s participating",
                                        (int)rtrk->nptr->nlocalprocs, rtrk->range->nspace);
                    trk->local_cnt += rtrk->nptr->nlocalprocs;
                    rtrk->contribution_added = true;
                } else {
                    /* we have to look for the specific ranks to see if
                     * we have them. First, we have to check to see if we
                     * already know about them all - otherwise, we cannot
                     * perform the check */
                    if (rtrk->nptr->all_registered) {
                        for (j=0; j < rtrk->range->nranks; j++) {
                            PMIX_LIST_FOREACH(info, &rtrk->nptr->ranks, pmix_rank_info_t) {
                                if (rtrk->range->ranks[j] == info->rank ) {
                                    pmix_output_verbose(5, pmix_globals.debug_output,
                                                        "trk_update: proc %d in nspace %s participating",
                                                        info->rank, rtrk->range->nspace);
                                    /* we can only count the primary client as
                                     * we cannot know how many clones might
                                     * eventually connect. Thus, we restrict
                                     * collective participation to one representative
                                     * from each rank */
                                    trk->local_cnt++;
                                    break;
                                }
                            }
                        }
                        rtrk->contribution_added = true;
                    }
                }
            }
        } else {
            complete = false;
        }
    }
    
    /* if all of the range contributions have been added, then
     * the definition is complete */
    if (complete) {
        trk->def_complete = true;
    }

    return trk->def_complete;
}

static bool trk_complete(pmix_server_trkr_t *trk)
{
    /* see if we know everything we need */
    if (!pmix_server_trk_update(trk)) {
        /* still missing some info */
        return false;
    }
    
    if (trk->local_cnt == pmix_list_get_size(&trk->locals)) {
        return true;
    }
    return false;
}

/* get an object for tracking LOCAL participation in a collective
 * operation such as "fence". The only way this function can be
 * called is if at least one local client process is participating
 * in the operation. Thus, we know that at least one process is
 * involved AND has called the collective operation. */
static pmix_server_trkr_t* get_tracker(pmix_range_t *ranges,
                                       size_t nranges)
{
    pmix_server_trkr_t *trk;
    pmix_range_trkr_t *rtrk;
    size_t i;
    size_t match;
    pmix_nspace_t *nptr;

    pmix_output_verbose(5, pmix_globals.debug_output,
                        "get_tracker called with %d ranges", (int)nranges);

    /* bozo check - should never happen outside of programmer error */
    if (NULL == ranges) {
        PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
        return NULL;
    }
    
    /* there is no shortcut way to search the trackers - all
     * we can do is perform a brute-force search. Fortunately,
     * it is highly unlikely that there will be more than one
     * or two active at a time, and they are most likely to
     * involve only a single range - so this shouldn't take long */
    PMIX_LIST_FOREACH(trk, &pmix_server_globals.collectives, pmix_server_trkr_t) {
        if (nranges != pmix_list_get_size(&trk->ranges)) {
            continue;
        }
        match = 0;
        for (i=0; i < nranges; i++) {
            PMIX_LIST_FOREACH(rtrk, &trk->ranges, pmix_range_trkr_t) {
                if (0 != strcmp(ranges[i].nspace, rtrk->range->nspace)) {
                    continue;
                }
                /* if we haven't yet connected the nspace to its
                 * definition, then try to do so now */
                if (NULL == rtrk->nptr) {
                    PMIX_LIST_FOREACH(nptr, &pmix_server_globals.nspaces, pmix_nspace_t) {
                        if (0 == strcmp(rtrk->range->nspace, nptr->nspace)) {
                            PMIX_RETAIN(nptr);
                            rtrk->nptr = nptr;
                            break;
                        }
                    }
                }
                if (ranges[i].nranks != rtrk->range->nranks) {
                    continue;
                }
                if (NULL == ranges[i].ranks && NULL == rtrk->range->ranks) {
                    /* this range matches */
                    match++;
                    break;
                }
                if (0 == memcmp(ranges[i].ranks, rtrk->range->ranks, rtrk->range->nranks*sizeof(int))) {
                    /* this range matches */
                    match++;
                    break;
                }
            }
            if (match != (i+1)) {
                /* we didn't find a match for this range */
                break;
            }
        }
        if (match == nranges) {
            return trk;
        }
    }

    pmix_output_verbose(5, pmix_globals.debug_output,
                        "adding new tracker with %d ranges", (int)nranges);
        
    /* get here if this tracker is new - create it */
    trk = PMIX_NEW(pmix_server_trkr_t);
    
    /* copy the ranges */
    PMIX_RANGE_CREATE(trk->rngs, nranges);
    
    for (i=0; i < nranges; i++) {
        pmix_output_verbose(5, pmix_globals.debug_output,
                            "adding range for nspace %s", ranges[i].nspace);
        (void)strncpy(trk->rngs[i].nspace, ranges[i].nspace, PMIX_MAX_NSLEN);
        trk->rngs[i].nranks = ranges[i].nranks;
        if (0 < ranges[i].nranks) {
            trk->rngs[i].ranks = (int*)malloc(ranges[i].nranks * sizeof(int));
            memcpy(trk->rngs[i].ranks, ranges[i].ranks, ranges[i].nranks * sizeof(int));
        }
        /* add a tracker for this range */
        rtrk = PMIX_NEW(pmix_range_trkr_t);
        rtrk->range = &trk->rngs[i];
        /* find the nspace - it is okay if we don't have it. It just
         * means that we don't know about it yet */
        PMIX_LIST_FOREACH(nptr, &pmix_server_globals.nspaces, pmix_nspace_t) {
            pmix_output_verbose(5, pmix_globals.debug_output,
                                "comparing %s to %s", rtrk->range->nspace, nptr->nspace);
            if (0 == strcmp(rtrk->range->nspace, nptr->nspace)) {
                PMIX_RETAIN(nptr);
                rtrk->nptr = nptr;
                break;
            }
        }
        pmix_list_append(&trk->ranges, &rtrk->super);
    }
    pmix_list_append(&pmix_server_globals.collectives, &trk->super);
    return trk;
}

pmix_status_t pmix_server_fence(pmix_server_caddy_t *cd,
                                pmix_buffer_t *buf,
                                pmix_modex_cbfunc_t modexcbfunc,
                                pmix_op_cbfunc_t opcbfunc)
{
    int32_t cnt;
    pmix_status_t rc;
    size_t nranges;
    pmix_range_t *ranges=NULL;
    int collect_data;
    pmix_server_trkr_t *trk;
    
    pmix_output_verbose(2, pmix_globals.debug_output,
                        "recvd FENCE");

    if (NULL == pmix_host_server.fence_nb) {
        return PMIX_ERR_NOT_SUPPORTED;
    }
    
    /* unpack the number of ranges */
    cnt = 1;
    if (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(buf, &nranges, &cnt, PMIX_SIZE))) {
        return rc;
    }
    pmix_output_verbose(2, pmix_globals.debug_output,
                        "recvd fence with %d ranges", (int)nranges);
    /* there must be at least one as the client has to at least provide
     * their own namespace */
    if (nranges < 1) {
        return PMIX_ERR_BAD_PARAM;
    }
    
    /* unpack the ranges */
    ranges = (pmix_range_t*)malloc(nranges * sizeof(pmix_range_t));
    memset(ranges, 0, nranges * sizeof(pmix_range_t));
    /* unpack the ranges */
    cnt = nranges;
    if (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(buf, ranges, &cnt, PMIX_RANGE))) {
        goto cleanup;
    }
    
    /* unpack the data flag - indicates if the caller wants
     * all modex data returned at end of procedure */
    cnt = 1;
    if (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(buf, &collect_data, &cnt, PMIX_INT))) {
        goto cleanup;
    }
    /* find/create the local tracker for this operation */
    if (NULL == (trk = get_tracker(ranges, nranges))) {
        /* only if a bozo error occurs */
        PMIX_ERROR_LOG(PMIX_ERROR);
        /* DO NOT HANG */
        if (NULL != opcbfunc) {
            opcbfunc(PMIX_ERROR, cd);
        }
        rc = PMIX_ERROR;
        goto cleanup;
    }
    trk->type = PMIX_FENCENB_CMD;
    trk->modexcbfunc = modexcbfunc;
    trk->collect_data = collect_data;
    /* add this contributor to the tracker so they get
     * notified when we are done */
    PMIX_RETAIN(cd);
    pmix_list_append(&trk->locals, &cd->super);
    /* if all local contributions were collected
     * let the local host's server know that we are at the
     * "fence" point - they will callback once the barrier
     * across all participants has been completed */
    if (trk_complete(trk)) {
        rc = pmix_host_server.fence_nb(ranges, nranges,
                                       collect_data, modexcbfunc, trk);
    }

 cleanup:
    PMIX_RANGE_FREE(ranges, nranges);

    return rc;
}

pmix_status_t pmix_server_get(pmix_buffer_t *buf,
                              pmix_modex_cbfunc_t cbfunc,
                              void *cbdata)
{
    int32_t cnt;
    pmix_status_t rc;
    int rank;
    char *nsp;
    char nspace[PMIX_MAX_NSLEN+1];
    
    pmix_output_verbose(2, pmix_globals.debug_output,
                        "recvd GET");

    if (NULL == pmix_host_server.get_modex_nb) {
        return PMIX_ERR_NOT_SUPPORTED;
    }
    
    /* setup */
    memset(nspace, 0, sizeof(nspace));
    
    /* retrieve the nspace and rank of the requested proc */
    cnt = 1;
    if (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(buf, &nsp, &cnt, PMIX_STRING))) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    (void)strncpy(nspace, nsp, PMIX_MAX_NSLEN);
    free(nsp);
    cnt = 1;
    if (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(buf, &rank, &cnt, PMIX_INT))) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    /* request the data */
    rc = pmix_host_server.get_modex_nb(nspace, rank, cbfunc, cbdata);
    return rc;
}

pmix_status_t pmix_server_publish(pmix_buffer_t *buf,
                                  pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    pmix_status_t rc;
    int32_t cnt;
    pmix_scope_t scope;
    pmix_persistence_t persist;
    size_t i, ninfo;
    pmix_info_t *info = NULL;
    
    pmix_output_verbose(2, pmix_globals.debug_output,
                        "recvd PUBLISH");

    if (NULL == pmix_host_server.publish) {
        return PMIX_ERR_NOT_SUPPORTED;
    }
    
    /* unpack the scope */
    cnt=1;
    if (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(buf, &scope, &cnt, PMIX_SCOPE))) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    /* unpack the persistence */
    cnt=1;
    if (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(buf, &persist, &cnt, PMIX_PERSIST))) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    /* unpack the number of info objects */
    cnt=1;
    if (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(buf, &ninfo, &cnt, PMIX_SIZE))) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    /* unpack the array of info objects */
    if (0 < ninfo) {
        info = (pmix_info_t*)malloc(ninfo * sizeof(pmix_info_t));
        cnt=ninfo;
        if (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(buf, info, &cnt, PMIX_INFO))) {
            PMIX_ERROR_LOG(rc);
            goto cleanup;
        }
    }
    
    /* call the local server */
    rc = pmix_host_server.publish(scope, persist, info, ninfo, cbfunc, cbdata);

 cleanup:
    if (NULL != info) {
        for (i=0; i < ninfo; i++) {
            PMIX_INFO_DESTRUCT(&info[i]);
        }
        free(info);
    }
    return rc;
}

pmix_status_t pmix_server_lookup(pmix_buffer_t *buf,
                                 pmix_lookup_cbfunc_t cbfunc, void *cbdata)
{
    int32_t cnt;
    pmix_status_t rc;
    int wait;
    pmix_scope_t scope;
    size_t nkeys, i;
    char **keys=NULL, *sptr;
    
    pmix_output_verbose(2, pmix_globals.debug_output,
                        "recvd LOOKUP");
    
    if (NULL == pmix_host_server.lookup) {
        return PMIX_ERR_NOT_SUPPORTED;
    }
    
    /* unpack the scope */
    cnt=1;
    if  (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(buf, &scope, &cnt, PMIX_SCOPE))) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    /* unpack the wait flag */
    cnt=1;
    if  (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(buf, &wait, &cnt, PMIX_INT))) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    /* unpack the number of keys */
    cnt=1;
    if  (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(buf, &nkeys, &cnt, PMIX_SIZE))) {
         PMIX_ERROR_LOG(rc);
        return rc;
    }
    /* unpack the array of keys */
    for (i=0; i < nkeys; i++) {
        cnt=1;
        if  (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(buf, &sptr, &cnt, PMIX_STRING))) {
            PMIX_ERROR_LOG(rc);
            goto cleanup;
        }
        pmix_argv_append_nosize(&keys, sptr);
        free(sptr);
    }
    /* call the local server */
    rc = pmix_host_server.lookup(scope, wait, keys, cbfunc, cbdata);

 cleanup:
    pmix_argv_free(keys);
    return rc;
}
        
pmix_status_t pmix_server_unpublish(pmix_buffer_t *buf,
                                    pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    int32_t cnt;
    pmix_status_t rc;
    pmix_scope_t scope;
    size_t i, nkeys;
    char **keys=NULL, *sptr;
    
    pmix_output_verbose(2, pmix_globals.debug_output,
                        "recvd UNPUBLISH");
    
    if (NULL == pmix_host_server.unpublish) {
        return PMIX_ERR_NOT_SUPPORTED;
    }
    
    /* unpack the scope */
    cnt=1;
    if  (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(buf, &scope, &cnt, PMIX_SCOPE))) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    /* unpack the number of keys */
    cnt=1;
    if  (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(buf, &nkeys, &cnt, PMIX_SIZE))) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    /* unpack the keys */
    for (i=0; i < nkeys; i++) {
        cnt=1;
        if  (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(buf, &sptr, &cnt, PMIX_STRING))) {
            PMIX_ERROR_LOG(rc);
            goto cleanup;
        }
        pmix_argv_append_nosize(&keys, sptr);
        free(sptr);
    }
    /* call the local server */
    rc = pmix_host_server.unpublish(scope, keys, cbfunc, cbdata);

 cleanup:
    pmix_argv_free(keys);
    return rc;
}

pmix_status_t pmix_server_spawn(pmix_buffer_t *buf,
                                pmix_spawn_cbfunc_t cbfunc,
                                void *cbdata)
{
    int32_t cnt;
    size_t i, napps;
    pmix_app_t *apps=NULL;
    pmix_status_t rc;
    
    pmix_output_verbose(2, pmix_globals.debug_output,
                        "recvd SPAWN");

    if (NULL == pmix_host_server.spawn) {
        return PMIX_ERR_NOT_SUPPORTED;
    }
    
    /* unpack the number of apps */
    cnt=1;
    if  (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(buf, &napps, &cnt, PMIX_SIZE))) {
        return rc;
    }
    /* unpack the array of apps */
    if (0 < napps) {
        PMIX_APP_CREATE(apps, napps);
        cnt=napps;
        if (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(buf, apps, &cnt, PMIX_APP))) {
            PMIX_ERROR_LOG(rc);
            goto cleanup;
        }
    }
    /* call the local server */
    rc = pmix_host_server.spawn(apps, napps, cbfunc, cbdata);

 cleanup:
    if (NULL != apps) {
        /* free the apps array */
        for (i=0; i < napps; i++) {
            PMIX_APP_DESTRUCT(&apps[i]);
        }
        free(apps);
    }
    return rc;
}

pmix_status_t pmix_server_connect(pmix_server_caddy_t *cd,
                                  pmix_buffer_t *buf, bool disconnect,
                                  pmix_op_cbfunc_t cbfunc)
{
    int32_t cnt;
    pmix_status_t rc;
    pmix_range_t *ranges;
    size_t nranges;
    pmix_server_trkr_t *trk;
    
    pmix_output_verbose(2, pmix_globals.debug_output,
                        "recvd CONNECT");

    if ((disconnect && NULL == pmix_host_server.disconnect) ||
        (!disconnect && NULL == pmix_host_server.connect)) {
        return PMIX_ERR_NOT_SUPPORTED;
    }
    
    /* unpack the number of ranges */
    cnt = 1;
    if (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(buf, &nranges, &cnt, PMIX_SIZE))) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    /* there must be at least one range - we do not allow the client
     * to send us NULL range as the server has no idea what to do
     * with that situation. Instead, the client should at least send
     * us their own namespace for the use-case where the connection
     * spans all procs in that namespace */
    if (nranges < 1) {
        PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
        return PMIX_ERR_BAD_PARAM;
    }
    
    /* unpack the ranges */
    ranges = (pmix_range_t*)malloc(nranges * sizeof(pmix_range_t));
    cnt = nranges;
    if (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(buf, ranges, &cnt, PMIX_RANGE))) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }

    /* find/create the local tracker for this operation */
    if (NULL == (trk = get_tracker(ranges, nranges))) {
        /* only if a bozo error occurs */
        PMIX_ERROR_LOG(PMIX_ERROR);
        /* DO NOT HANG */
        if (NULL != cbfunc) {
            cbfunc(PMIX_ERROR, cd);
        }
        rc = PMIX_ERROR;
        goto cleanup;
    }
    if (disconnect) {
        trk->type = PMIX_DISCONNECTNB_CMD;
    } else {
        trk->type = PMIX_CONNECTNB_CMD;
    }
    trk->op_cbfunc = cbfunc;
    /* add this contributor to the tracker so they get
     * notified when we are done */
    PMIX_RETAIN(cd);
    pmix_list_append(&trk->locals, &cd->super);
    /* if all local contributions were collected
     * let the local host's server know that we are at the
     * "connect" point - they will callback once the connect
     * across all participants has been completed */
    if (trk_complete(trk)) {
        if (disconnect) {
            rc = pmix_host_server.disconnect(ranges, nranges, cbfunc, trk);
        } else {
            rc = pmix_host_server.connect(ranges, nranges, cbfunc, trk);
        }
    } else {
        rc = PMIX_SUCCESS;
    }
    
 cleanup:
    PMIX_RANGE_FREE(ranges, nranges);

    return rc;
}

// instance server library classes
static void rtcon(pmix_range_trkr_t *t)
{
    t->nptr = NULL;
    t->contribution_added = false;
    t->range = NULL;
}
static void rtdes(pmix_range_trkr_t *t)
{
    if (NULL != t->nptr) {
        PMIX_RELEASE(t->nptr);
    }
    PMIX_RANGE_DESTRUCT(t->range);
}
PMIX_CLASS_INSTANCE(pmix_range_trkr_t,
                    pmix_list_item_t,
                    rtcon, rtdes);

static void tcon(pmix_server_trkr_t *t)
{
    t->rngs = NULL;
    t->active = true;
    t->def_complete = false;
    PMIX_CONSTRUCT(&t->ranges, pmix_list_t);
    PMIX_CONSTRUCT(&t->locals, pmix_list_t);
    t->local_cnt = 0;
    t->collect_data = false;
    t->modexcbfunc = NULL;
    t->op_cbfunc = NULL;
}
static void tdes(pmix_server_trkr_t *t)
{
    if (NULL != t->rngs) {
        free(t->rngs);
    }
    PMIX_LIST_DESTRUCT(&t->ranges);
    PMIX_LIST_DESTRUCT(&t->locals);
}
PMIX_CLASS_INSTANCE(pmix_server_trkr_t,
                   pmix_list_item_t,
                   tcon, tdes);

static void cdcon(pmix_server_caddy_t *cd)
{
    cd->peer = NULL;
    PMIX_CONSTRUCT(&cd->snd, pmix_snd_caddy_t);
}
static void cddes(pmix_server_caddy_t *cd)
{
    if (NULL != cd->peer) {
        PMIX_RELEASE(cd->peer);
    }
    PMIX_DESTRUCT(&cd->snd);
}
PMIX_CLASS_INSTANCE(pmix_server_caddy_t,
                   pmix_list_item_t,
                   cdcon, cddes);


static void pscon(pmix_snd_caddy_t *p)
{
    p->cbfunc = NULL;
}
PMIX_CLASS_INSTANCE(pmix_snd_caddy_t,
                   pmix_object_t,
                   pscon, NULL);

static void scadcon(pmix_setup_caddy_t *p)
{
    memset(p->nspace, 0, sizeof(p->nspace));
    p->active = true;
    p->server_object = NULL;
    p->nlocalprocs = 0;
    p->info = NULL;
    p->ninfo = 0;
}
static void scaddes(pmix_setup_caddy_t *p)
{
    if (NULL != p->info) {
        PMIX_INFO_FREE(p->info, p->ninfo);
    }
}
PMIX_CLASS_INSTANCE(pmix_setup_caddy_t,
                    pmix_object_t,
                    scadcon, scaddes);

static void ncon(pmix_notify_caddy_t *p)
{
    p->active = true;
    p->ranges = NULL;
    p->nranges = 0;
    p->error_ranges = NULL;
    p->error_nranges = 0;
    p->info = NULL;
    p->ninfo = 0;
    PMIX_CONSTRUCT(&p->buf, pmix_buffer_t);
}
static void ndes(pmix_notify_caddy_t *p)
{
    if (NULL != p->ranges) {
        PMIX_RANGE_FREE(p->ranges, p->nranges);
    }
    if (NULL != p->error_ranges) {
        PMIX_RANGE_FREE(p->error_ranges, p->error_nranges);
    }
    if (NULL != p->info) {
        PMIX_INFO_FREE(p->info, p->ninfo);
    }
    PMIX_DESTRUCT(&p->buf);
}
PMIX_CLASS_INSTANCE(pmix_notify_caddy_t,
                    pmix_object_t,
                    ncon, ndes);

PMIX_CLASS_INSTANCE(pmix_trkr_caddy_t,
                    pmix_object_t,
                    NULL, NULL);
