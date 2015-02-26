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

int pmix_server_abort(const char nspace[], int rank, pmix_buffer_t *buf,
                      pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    int32_t cnt;
    int rc, status;
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
        rc = pmix_host_server.abort(nspace, rank, status, msg, cbfunc, cbdata);
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

static pmix_server_trkr_t* get_tracker(pmix_list_t *trks,
                                       pmix_range_t *ranges,
                                       size_t nranges)
{
    pmix_server_trkr_t *trk;
    size_t i, j;
    bool match;
    uint32_t local_cnt = 0;
    pmix_nspace_t *nptr, *tmp;
    pmix_peer_t *pr;

    PMIX_LIST_FOREACH(trk, trks, pmix_server_trkr_t) {
        if (trk->nranges != nranges) {
            continue;
        }
        match = true;
        for (i=0; match && i < nranges; i++) {
            if (0 != strcmp(ranges[i].nspace, trk->ranges[i].nspace)) {
                match = false;
                break;
            }
            if (ranges[i].nranks != trk->ranges[i].nranks) {
                match = false;
                break;
            }
            if (NULL == ranges[i].ranks && NULL == trk->ranges[i].ranks) {
                /* this range matches */
                break;
            }
            for (j=0; j < ranges[i].nranks; j++) {
                if (ranges[i].ranks[j] != trk->ranges[i].ranks[j]) {
                    match = false;
                    break;
                }
            }
        }
        if (match) {
            return trk;
        }
    }
    /* get here if this tracker is new - create it */
    trk = PMIX_NEW(pmix_server_trkr_t);
    trk->active = true;
    /* copy the ranges */
    trk->nranges = nranges;
    trk->ranges = (pmix_range_t*)malloc(nranges * sizeof(pmix_range_t));

    for (i=0; i < nranges; i++) {
        memset(&trk->ranges[i], 0, sizeof(pmix_range_t));
        (void)strncpy(trk->ranges[i].nspace, ranges[i].nspace, PMIX_MAX_NSLEN);
        trk->ranges[i].nranks = ranges[i].nranks;
        trk->ranges[i].ranks = NULL;
        /* find the namespace */
        nptr = NULL;
        PMIX_LIST_FOREACH(tmp, &pmix_server_globals.nspaces, pmix_nspace_t) {
            if (0 == strcmp(ranges[i].nspace, tmp->nspace)) {
                nptr = tmp;
                break;
            }
        }
        if (NULL == nptr) {
            /* not allowed */
            PMIX_ERROR_LOG(PMIX_ERR_NOT_FOUND);
            PMIX_RELEASE(trk);
            return NULL;
        }
        if (NULL != ranges[i].ranks) {
            trk->ranges[i].ranks = (int*)malloc(ranges[i].nranks * sizeof(int));
            for (j=0; j < ranges[i].nranks; j++) {
                trk->ranges[i].ranks[j] = ranges[i].ranks[j];
                // Note if this is local peer
                PMIX_LIST_FOREACH(pr, &nptr->peers, pmix_peer_t) {
                    if (ranges[i].ranks[j] == pr->rank) {
                        local_cnt++;
                        break;
                    }
                }
            }
        } else {
            local_cnt += pmix_list_get_size(&nptr->peers);
        }
    }
    trk->local_cnt = local_cnt;
    trk->trklist = trks;
    pmix_list_append(trks, &trk->super);
    return trk;
}

int pmix_server_fence(pmix_server_caddy_t *cd,
                      pmix_buffer_t *buf,
                      pmix_modex_cbfunc_t modexcbfunc,
                      pmix_op_cbfunc_t opcbfunc)
{
    int32_t cnt;
    int rc;
    size_t i, nranges;
    pmix_range_t *ranges=NULL;
    int collect_data, barrier;
    pmix_modex_data_t mdx;
    pmix_scope_t scope;
    pmix_buffer_t *bptr;
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
    ranges = NULL;
    /* unpack the ranges, if provided */
    if (0 < nranges) {
        /* allocate reqd space */
        ranges = (pmix_range_t*)malloc(nranges * sizeof(pmix_range_t));
        memset(ranges, 0, nranges * sizeof(pmix_range_t));
        /* unpack the ranges */
        cnt = nranges;
        if (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(buf, ranges, &cnt, PMIX_RANGE))) {
            goto cleanup;
        }
    }
    /* unpack the data flag - indicates if the caller wants
     * all modex data returned at end of procedure */
    cnt = 1;
    if (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(buf, &collect_data, &cnt, PMIX_INT))) {
        goto cleanup;
    }
    /* unpack an additional flag indicating if we are to callback
     * once all procs have executed the fence_nb call, or
     * callback immediately */
    cnt = 1;
    if (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(buf, &barrier, &cnt, PMIX_INT))) {
        goto cleanup;
    }
    /* unpack any provided data blobs */
    cnt = 1;
    (void)strncpy(mdx.nspace, cd->hdr.nspace, PMIX_MAX_NSLEN);
    mdx.rank = cd->hdr.rank;
    while (PMIX_SUCCESS == pmix_bfrop.unpack(buf, &scope, &cnt, PMIX_SCOPE)) {
        cnt = 1;
        if (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(buf, &bptr, &cnt, PMIX_BUFFER))) {
            goto cleanup;
        }
        /* let the local host's server store it */
        mdx.blob = (uint8_t*)bptr->base_ptr;
        mdx.size = bptr->bytes_used;
        if (NULL != pmix_host_server.store_modex) {
            pmix_host_server.store_modex(cd->hdr.nspace, cd->hdr.rank, scope, &mdx);
        }
        PMIX_RELEASE(bptr);
        cnt = 1;
    }
    if (0 != barrier) {
        /* find/create the local tracker for this operation */
        trk = get_tracker(&pmix_server_globals.fence_ops, ranges, nranges);
        /* add this contributor to the tracker so they get
         * notified when we are done */
        PMIX_RETAIN(cd);
        pmix_list_append(&trk->locals, &cd->super);
        /* if all local contributions were collected
         * let the local host's server know that we are at the
         * "fence" point - they will callback once the barrier
         * across all participants has been completed */
        if( pmix_list_get_size(&trk->locals) == trk->local_cnt ){
            rc = pmix_host_server.fence_nb(ranges, nranges, barrier,
                                           collect_data, modexcbfunc, trk);
        }
    } else {
        /* tell the caller to send an immediate release */
        if (NULL != opcbfunc) {
            opcbfunc(PMIX_SUCCESS, cd);
        }
        rc = PMIX_SUCCESS;
    }

 cleanup:
    if (NULL != ranges) {
        for (i=0; i < nranges; i++) {
            if (NULL != ranges[i].ranks) {
                free(ranges[i].ranks);
            }
        }
        free(ranges);
    }

    return rc;
}

int pmix_server_get(pmix_buffer_t *buf,
                    pmix_modex_cbfunc_t cbfunc,
                    void *cbdata)
{
    int32_t cnt;
    int rc, rank;
    char *nsp;
    char nspace[PMIX_MAX_NSLEN];
    
    pmix_output_verbose(2, pmix_globals.debug_output,
                        "recvd GET");

    if (NULL == pmix_host_server.get_modex_nb) {
        return PMIX_ERR_NOT_SUPPORTED;
    }
    
    /* setup */
    memset(nspace, 0, PMIX_MAX_NSLEN);
    
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

int pmix_server_publish(pmix_buffer_t *buf,
                        pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    int rc;
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

int pmix_server_lookup(pmix_buffer_t *buf,
                       pmix_lookup_cbfunc_t cbfunc, void *cbdata)
{
    int32_t cnt;
    int rc, wait;
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
        
int pmix_server_unpublish(pmix_buffer_t *buf,
                          pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    int32_t cnt;
    int rc;
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

int pmix_server_spawn(pmix_buffer_t *buf,
                      pmix_spawn_cbfunc_t cbfunc,
                      void *cbdata)
{
    int32_t cnt;
    size_t i, napps;
    pmix_app_t *apps=NULL;
    int rc;
    
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

int pmix_server_connect(pmix_server_caddy_t *cd,
                        pmix_buffer_t *buf, bool disconnect,
                        pmix_op_cbfunc_t cbfunc)
{
    int32_t cnt;
    int rc;
    pmix_range_t *ranges;
    size_t nranges;
    pmix_server_trkr_t *trk;
    
    pmix_output_verbose(2, pmix_globals.debug_output,
                        "recvd CONNECT");

    if (NULL == pmix_host_server.connect) {
        return PMIX_ERR_NOT_SUPPORTED;
    }
    
    /* unpack the number of ranges */
    cnt = 1;
    if (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(buf, &nranges, &cnt, PMIX_SIZE))) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    ranges = NULL;
    /* unpack the ranges, if provided */
    if (0 < nranges) {
        /* allocate reqd space */
        ranges = (pmix_range_t*)malloc(nranges * sizeof(pmix_range_t));
        /* unpack the ranges */
        cnt = nranges;
        if (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(buf, ranges, &cnt, PMIX_RANGE))) {
            PMIX_ERROR_LOG(rc);
            return rc;
        }
    }
    /* find/create the local tracker for this operation */
    trk = get_tracker(&pmix_server_globals.connect_ops, ranges, nranges);
    /* add this contributor to the tracker so they get
     * notified when we are done */
    PMIX_RETAIN(cd);
    pmix_list_append(&trk->locals, &cd->super);
    /* request the connect */
    rc = pmix_host_server.connect(ranges, nranges, cbfunc, trk);
    free(ranges);

    return rc;
}

// instance server library classes
static void tcon(pmix_server_trkr_t *t)
{
    t->ranges = NULL;
    t->nranges = 0;
    PMIX_CONSTRUCT(&t->locals, pmix_list_t);
    t->trklist = NULL;
}
static void tdes(pmix_server_trkr_t *t)
{
    size_t i;
    
    if (NULL != t->ranges) {
        for (i=0; i < t->nranges; i++) {
            if (NULL != t->ranges[i].ranks) {
                free(t->ranges[i].ranks);
            }
        }
        free(t->ranges);
    }
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
