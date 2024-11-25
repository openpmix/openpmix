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
 * Copyright (c) 2021-2024 Nanook Consulting  All rights reserved.
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
#include "src/hwloc/pmix_hwloc.h"
#include "src/mca/bfrops/base/base.h"
#include "src/mca/gds/base/base.h"
#include "src/mca/plog/plog.h"
#include "src/mca/pnet/pnet.h"
#include "src/mca/prm/prm.h"
#include "src/mca/psensor/psensor.h"
#include "src/mca/ptl/base/base.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_error.h"
#include "src/util/pmix_name_fns.h"
#include "src/util/pmix_output.h"
#include "src/util/pmix_environ.h"

#include "src/client/pmix_client_ops.h"
#include "pmix_server_ops.h"


/* DEFINE A LOCAL GROUP COLLECTIVE TRACKER AND SHIFTER */
typedef struct {
    pmix_list_item_t super;
    char *id;
    int grpop;              // the group operation being tracked
    pmix_event_t ev;
    bool event_active;
    bool need_cxtid;
    pmix_list_t mbrs;  // list of grp_trk_t
    pmix_list_t nslist;  // list of involved nspaces
} grp_block_t;
static void gbcon(grp_block_t *p)
{
    p->id = NULL;
    p->grpop = PMIX_GROUP_NONE;
    p->event_active = false;
    p->need_cxtid = false;
    PMIX_CONSTRUCT(&p->mbrs, pmix_list_t);
    PMIX_CONSTRUCT(&p->nslist, pmix_list_t);
}
static void gbdes(grp_block_t *p)
{
    if (NULL != p->id) {
        free(p->id);
    }
    PMIX_LIST_DESTRUCT(&p->mbrs);
    PMIX_LIST_DESTRUCT(&p->nslist);
}
static PMIX_CLASS_INSTANCE(grp_block_t,
                           pmix_list_item_t,
                           gbcon, gbdes);

typedef struct {
    pmix_list_item_t super;
    pmix_event_t ev;
    bool event_active;
    pmix_lock_t lock;
    char *id;
    grp_block_t *blk;
    bool host_called;
    bool local;
    bool hybrid;
    pmix_proc_t *pcs;
    size_t npcs;
    pmix_info_t *info;
    size_t ninfo;
    bool def_complete;      // all local procs have been registered and the trk definition is complete
    pmix_list_t local_cbs;  // list of pmix_server_caddy_t for sending result to the local participants
                            //    Note: there may be multiple entries for a given proc if that proc
                            //    has fork/exec'd clones that are also participating
    uint32_t nlocal;        // number of local participants
    uint32_t local_cnt;     // number of local participants who have contributed
    pmix_info_cbfunc_t cbfunc;
    void *cbdata;
} grp_trk_t;
static void gtcon(grp_trk_t *t)
{
    t->event_active = false;
    PMIX_CONSTRUCT_LOCK(&t->lock);
    t->id = NULL;
    t->blk = NULL;
    t->host_called = false;
    t->local = true;
    t->hybrid = false;
    t->pcs = NULL;
    t->npcs = 0;
    t->info = NULL;
    t->ninfo = 0;
    t->def_complete = false;
    PMIX_CONSTRUCT(&t->local_cbs, pmix_list_t);
    t->nlocal = 0;
    t->local_cnt = 0;
    t->cbfunc = NULL;
    t->cbdata = NULL;
}
static void gtdes(grp_trk_t *t)
{
    PMIX_DESTRUCT_LOCK(&t->lock);
    if (NULL != t->blk) {
        PMIX_RELEASE(t->blk);
    }
    PMIX_LIST_DESTRUCT(&t->local_cbs);
}
static PMIX_CLASS_INSTANCE(grp_trk_t,
                           pmix_list_item_t,
                           gtcon, gtdes);


typedef struct {
    pmix_object_t super;
    pmix_event_t ev;
    pmix_lock_t lock;
    pmix_status_t status;
    pmix_info_t *info;
    size_t ninfo;
    grp_block_t *blk;
    pmix_release_cbfunc_t relfn;
    void *cbdata;
} grp_shifter_t;
static void scon(grp_shifter_t *p)
{
    PMIX_CONSTRUCT_LOCK(&p->lock);
    p->info = NULL;
    p->ninfo = 0;
    p->blk = NULL;
    p->relfn = NULL;
    p->cbdata = NULL;
}
static void sdes(grp_shifter_t *p)
{
    PMIX_DESTRUCT_LOCK(&p->lock);
}
PMIX_EXPORT PMIX_CLASS_INSTANCE(grp_shifter_t,
                                pmix_object_t,
                                scon, sdes);


/* DEFINE LOCAL FUNCTIONS */
static pmix_status_t aggregate_info(grp_trk_t *trk,
                                    pmix_info_t *info,
                                    size_t ninfo);

static void check_completion(grp_trk_t *trk,
                             pmix_proc_t *procs, size_t nprocs)
{
    bool all_def, found;
    pmix_nspace_t first;
    pmix_namespace_t *ns, *nptr;
    pmix_nspace_caddy_t *nm;
    pmix_rank_info_t *info;
    size_t i;

    if (trk->def_complete) {
        return;
    }

    all_def = true;
    PMIX_LOAD_NSPACE(first, NULL);
    for (i = 0; i < nprocs; i++) {
        /* is this nspace known to us? */
        nptr = NULL;
        PMIX_LIST_FOREACH (ns, &pmix_globals.nspaces, pmix_namespace_t) {
            if (0 == strcmp(procs[i].nspace, ns->nspace)) {
                nptr = ns;
                break;
            }
        }
        /* check if multiple nspaces are involved in this operation */
        if (0 == strlen(first)) {
            PMIX_LOAD_NSPACE(first, procs[i].nspace);
        } else if (!PMIX_CHECK_NSPACE(first, procs[i].nspace)) {
            trk->hybrid = true;
        }
        if (NULL == nptr) {
            /* we don't know about this nspace. If there is going to
             * be at least one local process participating in a fence,
             * they we require that either at least one process must already
             * have been registered (via "register client") or that the
             * nspace itself have been regisered. So either the nspace
             * wasn't registered because it doesn't include any local
             * procs, or our host has not been told about this nspace
             * because it won't host any local procs. We therefore mark
             * this tracker as including non-local participants.
             *
             * NOTE: It is conceivable that someone might want to review
             * this constraint at a future date. I believe it has to be
             * required (at least for now) as otherwise we wouldn't have
             * a way of knowing when all local procs have participated.
             * It is possible that a new nspace could come along at some
             * later time and add more local participants - but we don't
             * know how long to wait.
             *
             * The only immediately obvious alternative solutions would
             * be to either require that RMs always inform all daemons
             * about the launch of nspaces, regardless of whether or
             * not they will host local procs; or to drop the aggregation
             * of local participants and just pass every fence call
             * directly to the host. Neither of these seems palatable
             * at this time. */
            trk->local = false;
            /* we don't know any more info about this nspace, so
             * there isn't anything more we can do */
            continue;
        }
        /* it is possible we know about this nspace because the host
         * has registered one or more clients via "register_client",
         * but the host has not yet called "register_nspace". There is
         * a very tiny race condition whereby this can happen due
         * to event-driven processing, but account for it here */
        if (SIZE_MAX == nptr->nlocalprocs) {
            /* delay processing until this nspace is registered */
            all_def = false;
            continue;
        }
        if (0 == nptr->nlocalprocs) {
            /* the host has informed us that this nspace has no local procs */
            pmix_output_verbose(5, pmix_server_globals.fence_output,
                                "pmix_server_new_tracker: nspace %s has no local procs", procs[i].nspace);
            trk->local = false;
            continue;
        }

        /* check and add uniq ns into nslist for this group operation block */
        found = false;
        PMIX_LIST_FOREACH (nm, &trk->blk->nslist, pmix_nspace_caddy_t) {
            if (0 == strcmp(nptr->nspace, nm->ns->nspace)) {
                found = true;
                break;
            }
        }
        if (!found) {
            nm = PMIX_NEW(pmix_nspace_caddy_t);
            PMIX_RETAIN(nptr);
            nm->ns = nptr;
            pmix_list_append(&trk->blk->nslist, &nm->super);
        }

        /* if they want all the local members of this nspace, then
         * add them in here. They told us how many procs will be
         * local to us from this nspace, but we don't know their
         * ranks. So as long as they want _all_ of them, we can
         * handle that case regardless of whether the individual
         * clients have been "registered" */
        if (PMIX_RANK_WILDCARD == procs[i].rank) {
            trk->nlocal += nptr->nlocalprocs;
            /* the total number of procs in this nspace was provided
             * in the data blob delivered to register_nspace, so check
             * to see if all the procs are local */
            if (nptr->nprocs != nptr->nlocalprocs) {
                trk->local = false;
            }
            continue;
        }

        /* They don't want all the local clients, or they are at
         * least listing them individually. Check if all the clients
         * for this nspace have been registered via "register_client"
         * so we know the specific ranks on this node */
        if (!nptr->all_registered) {
            /* nope, so no point in going further on this one - we'll
             * process it once all the procs are known */
            all_def = false;
            pmix_output_verbose(5, pmix_server_globals.fence_output,
                                "pmix_server_new_tracker: all clients not registered nspace %s",
                                procs[i].nspace);
            continue;
        }
        /* is this one of my local ranks? */
        found = false;
        PMIX_LIST_FOREACH (info, &nptr->ranks, pmix_rank_info_t) {
            if (procs[i].rank == info->pname.rank) {
                pmix_output_verbose(5, pmix_server_globals.fence_output,
                                    "adding local proc %s.%d to tracker", info->pname.nspace,
                                    info->pname.rank);
                found = true;
                /* track the count */
                trk->nlocal++;
                break;
            }
        }
        if (!found) {
            trk->local = false;
        }
    }

    if (all_def) {
        trk->def_complete = true;
    }
}
static pmix_status_t get_tracker(char *grpid, bool bootstrap,
                                 pmix_proc_t *procs, size_t nprocs,
                                 pmix_info_t *info, size_t ninfo,
                                 grp_block_t **block, grp_trk_t **tracker)
{
    grp_block_t *blk;
    grp_trk_t *trk;
    pmix_status_t rc;

    // see if we already have a block for this ID
    PMIX_LIST_FOREACH(blk, &pmix_server_globals.grp_collectives, grp_block_t) {
        if (0 == strcmp(grpid, blk->id)) {
            // we do - pass it back
            *block = blk;
            if (NULL == procs) {
                // setup a unique tracker for this op
                trk = PMIX_NEW(grp_trk_t);
                PMIX_RETAIN(blk);
                trk->blk = blk;
                trk->info = info;
                trk->ninfo = ninfo;
                trk->def_complete = true;
                pmix_list_append(&blk->mbrs, &trk->super);
                *tracker = trk;
                return PMIX_SUCCESS;
            }
            // if this is a bootstrap, then it always gets its own tracker
            if (bootstrap) {
                trk = PMIX_NEW(grp_trk_t);
                PMIX_RETAIN(blk);
                trk->blk = blk;
                trk->npcs = nprocs;
                PMIX_PROC_CREATE(trk->pcs, trk->npcs);
                memcpy(trk->pcs, procs, nprocs * sizeof(pmix_proc_t));
                trk->info = info;
                trk->ninfo = ninfo;
                // bootstraps are sent as separate participants
                trk->def_complete = true;
                pmix_list_append(&blk->mbrs, &trk->super);
                *tracker = trk;
                return PMIX_SUCCESS;
            }
            // check if this signature is already present
            PMIX_LIST_FOREACH(trk, &blk->mbrs, grp_trk_t) {
                if (trk->npcs != nprocs) {
                    continue;
                }
                if (0 == memcmp(trk->pcs, procs, nprocs * sizeof(pmix_proc_t))) {
                    // matching tracker - pass it back
                    *tracker = trk;
                    // check for completion
                    check_completion(trk, procs, nprocs);
                    // aggregate info
                    rc = aggregate_info(trk, info, ninfo);
                    return rc;
                }
            }
            // new signature, so create a tracker for it
            trk = PMIX_NEW(grp_trk_t);
            PMIX_RETAIN(blk);
            trk->blk = blk;
            trk->npcs = nprocs;
            PMIX_PROC_CREATE(trk->pcs, trk->npcs);
            memcpy(trk->pcs, procs, nprocs * sizeof(pmix_proc_t));
            trk->info = info;
            trk->ninfo = ninfo;
            pmix_list_append(&blk->mbrs, &trk->super);
            *tracker = trk;
            check_completion(trk, procs, nprocs);
            return PMIX_SUCCESS;
        }
    }

    // new block
    blk = PMIX_NEW(grp_block_t);
    blk->id = strdup(grpid);
    pmix_list_append(&pmix_server_globals.grp_collectives, &blk->super);
    *block = blk;
    // setup a tracker for it
    trk = PMIX_NEW(grp_trk_t);
    PMIX_RETAIN(blk);
    trk->blk = blk;
    trk->info = info;
    trk->ninfo = ninfo;
    pmix_list_append(&blk->mbrs, &trk->super);
    if (NULL == procs) {
        trk->def_complete = true;
    } else {
        trk->npcs = nprocs;
        PMIX_PROC_CREATE(trk->pcs, trk->npcs);
        memcpy(trk->pcs, procs, nprocs * sizeof(pmix_proc_t));
        check_completion(trk, procs, nprocs);
    }
    *tracker = trk;
    return PMIX_SUCCESS;
}

static void _grpcbfunc(int sd, short args, void *cbdata)
{
    grp_shifter_t *scd = (grp_shifter_t *) cbdata;
    grp_block_t *blk = scd->blk;
    grp_trk_t *trk;
    pmix_server_caddy_t *cd;
    pmix_buffer_t *reply;
    pmix_status_t ret;
    size_t n, ctxid = SIZE_MAX, nmembers = 0;
    pmix_proc_t *members = NULL;
    bool ctxid_given = false;
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    PMIX_ACQUIRE_OBJECT(scd);

    if (NULL == blk) {
        /* give them a release if they want it - this should
         * never happen, but protect against the possibility */
        if (NULL != scd->relfn) {
            scd->relfn(scd->cbdata);
        }
        PMIX_RELEASE(scd);
        return;
    }

    /* see if this group was assigned a context ID or collected data */
    for (n = 0; n < scd->ninfo; n++) {
        if (PMIX_CHECK_KEY(&scd->info[n], PMIX_GROUP_CONTEXT_ID)) {
            PMIX_VALUE_GET_NUMBER(ret, &scd->info[n].value, ctxid, size_t);
            if (PMIX_SUCCESS != ret) {
                PMIX_ERROR_LOG(ret);
            } else {
                ctxid_given = true;
            }

        } else if (PMIX_CHECK_KEY(&scd->info[n], PMIX_GROUP_MEMBERSHIP)) {
            members = (pmix_proc_t*)scd->info[n].value.data.darray->array;
            nmembers = scd->info[n].value.data.darray->size;
        }
    }

    PMIX_LIST_FOREACH(trk, &blk->mbrs, grp_trk_t) {
        pmix_output_verbose(2, pmix_server_globals.group_output,
                            "server:grpcbfunc processing WITH %d CALLBACKS",
                            (int) pmix_list_get_size(&trk->local_cbs));


        /* loop across all procs in the tracker, sending them the reply */
        PMIX_LIST_FOREACH (cd, &trk->local_cbs, pmix_server_caddy_t) {
            reply = PMIX_NEW(pmix_buffer_t);
            if (NULL == reply) {
                break;
            }
            /* setup the reply, starting with the returned status */
            PMIX_BFROPS_PACK(ret, cd->peer, reply, &scd->status, 1, PMIX_STATUS);
            if (PMIX_SUCCESS != ret) {
                PMIX_ERROR_LOG(ret);
                PMIX_RELEASE(reply);
                break;
            }
            if (PMIX_SUCCESS == scd->status && PMIX_GROUP_CONSTRUCT == blk->grpop) {
                /* add the final membership */
                PMIX_BFROPS_PACK(ret, cd->peer, reply, &nmembers, 1, PMIX_SIZE);
                if (PMIX_SUCCESS != ret) {
                    PMIX_ERROR_LOG(ret);
                    PMIX_RELEASE(reply);
                    break;
                }
                if (0 < nmembers) {
                    PMIX_BFROPS_PACK(ret, cd->peer, reply, members, nmembers, PMIX_PROC);
                    if (PMIX_SUCCESS != ret) {
                        PMIX_ERROR_LOG(ret);
                        PMIX_RELEASE(reply);
                        break;
                    }
                }
                /* if a ctxid was provided, pass it along */
                PMIX_BFROPS_PACK(ret, cd->peer, reply, &ctxid_given, 1, PMIX_BOOL);
                if (PMIX_SUCCESS != ret) {
                    PMIX_ERROR_LOG(ret);
                    PMIX_RELEASE(reply);
                    break;
                }
                if (ctxid_given) {
                    PMIX_BFROPS_PACK(ret, cd->peer, reply, &ctxid, 1, PMIX_SIZE);
                    if (PMIX_SUCCESS != ret) {
                        PMIX_ERROR_LOG(ret);
                        PMIX_RELEASE(reply);
                        break;
                    }
                }
            }

            pmix_output_verbose(2, pmix_server_globals.group_output,
                                "server:grp_cbfunc reply being sent to %s:%u",
                                cd->peer->info->pname.nspace, cd->peer->info->pname.rank);
            PMIX_SERVER_QUEUE_REPLY(ret, cd->peer, cd->hdr.tag, reply);
            if (PMIX_SUCCESS != ret) {
                PMIX_RELEASE(reply);
            }
        }
    }

    /* remove the block from the list */
    pmix_list_remove_item(&pmix_server_globals.grp_collectives, &blk->super);
    PMIX_RELEASE(blk);

    /* we are done */
    if (NULL != scd->relfn) {
        scd->relfn(scd->cbdata);
    }
    PMIX_RELEASE(scd);
}

static void grpcbfunc(pmix_status_t status,
                      pmix_info_t *info, size_t ninfo, void *cbdata,
                      pmix_release_cbfunc_t relfn, void *relcbd)
{
    grp_block_t *blk = (grp_block_t *) cbdata;
    grp_shifter_t *scd;

    pmix_output_verbose(2, pmix_server_globals.group_output,
                        "server:grpcbfunc called with %d info", (int) ninfo);

    if (NULL == blk) {
        /* nothing to do - but be sure to give them
         * a release if they want it */
        if (NULL != relfn) {
            relfn(relcbd);
        }
        return;
    }
    /* need to thread-shift this callback as it accesses global data */
    scd = PMIX_NEW(grp_shifter_t);
    if (NULL == scd) {
        /* nothing we can do */
        if (NULL != relfn) {
            relfn(cbdata);
        }
        return;
    }
    scd->status = status;
    scd->info = info;
    scd->ninfo = ninfo;
    scd->blk = blk;
    scd->relfn = relfn;
    scd->cbdata = relcbd;
    PMIX_THREADSHIFT(scd, _grpcbfunc);
}

static void grp_timeout(int sd, short args, void *cbdata)
{
    grp_block_t *blk = (grp_block_t *) cbdata;
    pmix_server_caddy_t *cd;
    pmix_buffer_t *reply;
    grp_trk_t *trk;
    pmix_status_t ret, rc = PMIX_ERR_TIMEOUT;
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    pmix_output_verbose(2, pmix_server_globals.group_output,
                        "ALERT: grp construct timeout fired");

    PMIX_LIST_FOREACH(trk, &blk->mbrs, grp_trk_t) {
        /* loop across all procs in the tracker, alerting
         * them to the failure */
        PMIX_LIST_FOREACH (cd, &trk->local_cbs, pmix_server_caddy_t) {
            reply = PMIX_NEW(pmix_buffer_t);
            if (NULL == reply) {
                break;
            }
            /* setup the reply, starting with the returned status */
            PMIX_BFROPS_PACK(ret, cd->peer, reply, &rc, 1, PMIX_STATUS);
            if (PMIX_SUCCESS != ret) {
                PMIX_ERROR_LOG(ret);
                PMIX_RELEASE(reply);
                break;
            }
            pmix_output_verbose(2, pmix_server_globals.group_output,
                                "server:grp_cbfunc TIMEOUT being sent to %s:%u",
                                cd->peer->info->pname.nspace, cd->peer->info->pname.rank);
            PMIX_SERVER_QUEUE_REPLY(ret, cd->peer, cd->hdr.tag, reply);
            if (PMIX_SUCCESS != ret) {
                PMIX_RELEASE(reply);
            }
        }
    }

    blk->event_active = false;
    /* record this group as failed */
    PMIx_Argv_append_nosize(&pmix_server_globals.failedgrps, blk->id);
    /* remove the tracker from the list */
    pmix_list_remove_item(&pmix_server_globals.grp_collectives, &blk->super);
    PMIX_RELEASE(blk);
}

static pmix_status_t aggregate_info(grp_trk_t *trk,
                                    pmix_info_t *info,
                                    size_t ninfo)
{
    pmix_list_t ilist, nmlist;
    size_t n, m, j, k, niptr;
    pmix_info_t *iptr;
    bool found, nmfound;
    pmix_info_caddy_t *icd;
    pmix_proclist_t *nm;
    pmix_proc_t *nmarray, *trkarray;
    size_t nmsize, trksize, bt, bt2;
    pmix_status_t rc;

    if (NULL == trk->info) {
        trk->info = info;
        trk->ninfo = ninfo;
        return PMIX_EXISTS;
    }

    // only keep unique entries
    PMIX_CONSTRUCT(&ilist, pmix_list_t);
    for (m=0; m < ninfo; m++) {
        found = false;
        for (n=0; n < trk->ninfo; n++) {
            if (PMIX_CHECK_KEY(&trk->info[n], info[m].key)) {

                // check a few critical keys
                if (PMIX_CHECK_KEY(&info[m], PMIX_GROUP_ADD_MEMBERS)) {
                    // aggregate the members
                    nmarray = (pmix_proc_t*)info[m].value.data.darray->array;
                    nmsize = info[m].value.data.darray->size;
                    trkarray = (pmix_proc_t*)trk->info[n].value.data.darray->array;
                    trksize = trk->info[n].value.data.darray->size;
                    PMIX_CONSTRUCT(&nmlist, pmix_list_t);
                    // sadly, an exhaustive search
                    for (j=0; j < nmsize; j++) {
                        nmfound = false;
                        for (k=0; k < trksize; k++) {
                            if (PMIX_CHECK_PROCID(&nmarray[j], &trkarray[k])) {
                                // if the new one is rank=WILDCARD, then ensure
                                // we keep it as wildcard
                                if (PMIX_RANK_WILDCARD == nmarray[j].rank) {
                                    trkarray[k].rank = PMIX_RANK_WILDCARD;
                                }
                                nmfound = true;
                                break;
                            }
                        }
                        if (!nmfound) {
                            nm = PMIX_NEW(pmix_proclist_t);
                            memcpy(&nm->proc, &nmarray[j], sizeof(pmix_proc_t));
                            pmix_list_append(&nmlist, &nm->super);
                        }
                    }
                    // create the replacement array, if needed
                    if (0 < pmix_list_get_size(&nmlist)) {
                        nmsize = trksize + pmix_list_get_size(&nmlist);
                        PMIX_PROC_CREATE(nmarray, nmsize);
                        memcpy(nmarray, trkarray, trksize * sizeof(pmix_proc_t));
                        j = trksize;
                        PMIX_LIST_FOREACH(nm, &nmlist, pmix_proclist_t) {
                            memcpy(&nmarray[j], &nm->proc, sizeof(pmix_proc_t));
                            ++j;
                        }
                        PMIX_PROC_FREE(trkarray, trksize);
                        trk->info[n].value.data.darray->array = nmarray;
                        trk->info[n].value.data.darray->size = nmsize;
                    }
                    PMIX_LIST_DESTRUCT(&nmlist);

                } else if (PMIX_CHECK_KEY(&info[m], PMIX_GROUP_BOOTSTRAP)) {
                    // the numbers must match
                    PMIX_VALUE_GET_NUMBER(rc, &info[m].value, bt, size_t);
                    if (PMIX_SUCCESS != rc) {
                        PMIX_LIST_DESTRUCT(&ilist);
                        return rc;
                    }
                    PMIX_VALUE_GET_NUMBER(rc, &trk->info[n].value, bt2, size_t);
                    if (PMIX_SUCCESS != rc) {
                        PMIX_LIST_DESTRUCT(&ilist);
                        return rc;
                    }
                    if (bt != bt2) {
                        PMIX_LIST_DESTRUCT(&ilist);
                        return PMIX_ERR_BAD_PARAM;
                    }
                } else if (PMIX_CHECK_KEY(&info[m], PMIX_PROC_DATA) ||
                           PMIX_CHECK_KEY(&info[m], PMIX_GROUP_INFO)) {
                    // keep the duplicates
                    icd = PMIX_NEW(pmix_info_caddy_t);
                    icd->info = &info[m];
                    icd->ninfo = 1;
                    pmix_list_append(&ilist, &icd->super);
                }
                found = true;
                break;
            }
        }
        if (!found) {
            // add this one in
            icd = PMIX_NEW(pmix_info_caddy_t);
            icd->info = &info[m];
            icd->ninfo = 1;
            pmix_list_append(&ilist, &icd->super);
        }
    }
    if (0 < pmix_list_get_size(&ilist)) {
        niptr = trk->ninfo + pmix_list_get_size(&ilist);
        PMIX_INFO_CREATE(iptr, niptr);
        for (n=0; n < trk->ninfo; n++) {
            PMIX_INFO_XFER(&iptr[n], &trk->info[n]);
        }
        n = trk->ninfo;
        PMIX_LIST_FOREACH(icd, &ilist, pmix_info_caddy_t) {
            PMIX_INFO_XFER(&iptr[n], icd->info);
            ++n;
        }
        PMIX_INFO_FREE(trk->info, trk->ninfo);
        trk->info = iptr;
        trk->ninfo = niptr;
        /* cleanup */
    }
    PMIX_LIST_DESTRUCT(&ilist);
    return PMIX_SUCCESS;
}

static void addmembercb(pmix_status_t status,
                        pmix_info_t *info, size_t ninfo, void *cbdata,
                        pmix_release_cbfunc_t relfn, void *relcbd)
{
    pmix_info_caddy_t *ncd = (pmix_info_caddy_t*)cbdata;
    PMIX_HIDE_UNUSED_PARAMS(status, info, ninfo);

    if (NULL != relfn) {
        relfn(relcbd);
    }
    PMIX_RELEASE(ncd);
}

/* we are being called from the PMIx server's switchyard function,
 * which means we are in an event and can access global data */
pmix_status_t pmix_server_group(pmix_server_caddy_t *cd, pmix_buffer_t *buf,
                                pmix_group_operation_t op)
{
    pmix_peer_t *peer = (pmix_peer_t *) cd->peer;
    pmix_peer_t *pr;
    int32_t cnt, m;
    pmix_status_t rc;
    char *grpid;
    pmix_proc_t *procs;
    pmix_info_t *info = NULL;
    size_t n, ninfo, ninf, nprocs;
    grp_block_t *blk;
    grp_trk_t *trk;
    bool need_cxtid = false;
    bool match, force_local = false;
    bool locally_complete = false;
    bool bootstrap = false;
    struct timeval tv = {0, 0};
    pmix_info_caddy_t *ncd;
    pmix_namespace_t *nptr, *ns;

    pmix_output_verbose(2, pmix_server_globals.group_output,
                        "recvd grpconstruct cmd from %s",
                        PMIX_PEER_PRINT(cd->peer));

    /* unpack the group ID */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, peer, buf, &grpid, &cnt, PMIX_STRING);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto error;
    }

    /* is this a failed group? */
    if (NULL != pmix_server_globals.failedgrps) {
        for (m=0; NULL != pmix_server_globals.failedgrps[m]; m++) {
            if (0 == strcmp(grpid, pmix_server_globals.failedgrps[m])) {
                /* yes - reject it */
                free(grpid);
                return PMIX_ERR_TIMEOUT;
            }
        }
    }

    /* unpack the number of procs */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, peer, buf, &nprocs, &cnt, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto error;
    }
    if (0 < nprocs) {
        PMIX_PROC_CREATE(procs, nprocs);
        if (NULL == procs) {
            rc = PMIX_ERR_NOMEM;
            goto error;
        }
        cnt = nprocs;
        PMIX_BFROPS_UNPACK(rc, peer, buf, procs, &cnt, PMIX_PROC);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_PROC_FREE(procs, nprocs);
            goto error;
        }
    } else {
        // this process is participating as an "add-member"
        bootstrap = true;
    }

    /* unpack the number of directives */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, peer, buf, &ninf, &cnt, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto error;
    }
    ninfo = ninf + 1;
    PMIX_INFO_CREATE(info, ninfo);
    /* store default response */
    rc = PMIX_SUCCESS;
    PMIX_INFO_LOAD(&info[ninf], PMIX_LOCAL_COLLECTIVE_STATUS, &rc, PMIX_STATUS);
    if (0 < ninf) {
        cnt = ninf;
        PMIX_BFROPS_UNPACK(rc, peer, buf, info, &cnt, PMIX_INFO);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            goto error;
        }
    }
    /* check directives */
    for (n = 0; n < ninfo; n++) {
        if (PMIX_CHECK_KEY(&info[n], PMIX_GROUP_ASSIGN_CONTEXT_ID)) {
            need_cxtid = PMIX_INFO_TRUE(&info[n]);

        } else if (PMIX_CHECK_KEY(&info[n], PMIX_GROUP_LOCAL_ONLY)) {
            force_local = PMIX_INFO_TRUE(&info[n]);

        } else if (PMIX_CHECK_KEY(&info[n], PMIX_GROUP_BOOTSTRAP)) {
             bootstrap = PMIX_INFO_TRUE(&info[n]);

        } else if (PMIX_CHECK_KEY(&info[n], PMIX_TIMEOUT)) {
            PMIX_VALUE_GET_NUMBER(rc, &info[n].value, tv.tv_sec, uint32_t);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_PROC_FREE(procs, nprocs);
                PMIX_INFO_FREE(info, ninfo);
                goto error;
            }

        }
    }

    if (NULL == procs) {
        // this is a group member participating as an "add-member".
        // check if our host supports group operations
        if (NULL == pmix_host_server.group) {
            /* cannot support it */
            PMIX_INFO_FREE(info, ninfo);
            free(grpid);
            return PMIX_ERR_NOT_SUPPORTED;
        }
        // this is an add-member, so pass it up - we don't
        // create a tracker as the participant doesn't have
        // a callback function (they are waiting on an event)
        ncd = PMIX_NEW(pmix_info_caddy_t);
        ncd->info = info;
        ncd->ninfo = ninfo;
        rc = pmix_host_server.group(PMIX_GROUP_CONSTRUCT, grpid, NULL, 0,
                                    ncd->info, ncd->ninfo, addmembercb, ncd);
        PMIX_ERROR_LOG(rc);
        if (PMIX_SUCCESS != rc && PMIX_OPERATION_SUCCEEDED != rc) {
            PMIX_INFO_FREE(info, ninfo);
            PMIX_RELEASE(ncd);
            return rc;
        }
        return rc;
    }

    /* find/create the local tracker for this operation */
    rc = get_tracker(grpid, bootstrap, procs, nprocs,
                     info, ninfo, &blk, &trk);
    if (PMIX_EXISTS == rc) {
        // we extended the trk's info array
        PMIX_INFO_FREE(info, ninfo);
        info = NULL;
        ninfo = 0;
    }
    // grpid has been copied into the tracker
    free(grpid);
    // mark as a construct op
    blk->grpop = op;
    // track ctx id request
    if (!blk->need_cxtid) {
        blk->need_cxtid = need_cxtid;
    }

    /* see if this constructor only references local processes and isn't
     * requesting a context ID - if both conditions are met, then we
     * can just locally process the request without bothering the host.
     * This is meant to provide an optimized path for a fairly common
     * operation */
    if (force_local) {
        trk->local = true;
    } else if (blk->need_cxtid) {
        trk->local = false;
    } else {
        trk->local = true;
        for (n = 0; n < nprocs; n++) {
            /* if this entry references the local procs, then
             * we can skip it */
            if (PMIX_RANK_LOCAL_PEERS == procs[n].rank ||
                PMIX_RANK_LOCAL_NODE == procs[n].rank) {
                continue;
            }

            /* if this rank is wildcard, then see if #localprocs
             * is equal to nprocs for this namespace- if so, then
             * there are no remote participants for this entry */
            if (PMIX_RANK_WILDCARD == procs[n].rank) {
                // get the namespace object
                nptr = NULL;
                PMIX_LIST_FOREACH (ns, &pmix_globals.nspaces, pmix_namespace_t) {
                    if (0 == strcmp(procs[n].nspace, ns->nspace)) {
                        nptr = ns;
                        break;
                    }
                }
                if (NULL == nptr) {
                    // must be nonlocal
                    trk->local = false;
                    break;
                }
                if (0 == nptr->nlocalprocs) {
                    /* the host has informed us that this nspace has no local procs */
                    pmix_output_verbose(5, pmix_server_globals.group_output,
                                        "nspace %s has no local procs", procs[n].nspace);
                    trk->local = false;
                    break;
                }
                if (nptr->nlocalprocs != nptr->nprocs) {
                    // at least some procs are nonlocal
                    trk->local = false;
                    break;
                }
                continue;
            }

            /* go thru all the local clients and see if this proc
             * is on the list - if not, then it is non-local */
            match = false;
            for (m = 0; m < pmix_server_globals.clients.size; m++) {
                pr = (pmix_peer_t *) pmix_pointer_array_get_item(&pmix_server_globals.clients, m);
                if (NULL == pr) {
                    continue;
                }
                if (PMIX_CHECK_NAMES(&procs[n], &pr->info->pname)) {
                    match = true;
                    break;
                }
            }
            if (!match) {
                /* this requires a non_local operation */
                trk->local = false;
                break;
            }
        }
    }

    /* add this contributor to the tracker so they get
     * notified when we are done */
    pmix_list_append(&trk->local_cbs, &cd->super);

    // if this is a bootstrap, then pass it up
    if (bootstrap) {
        goto proceed;
    }

    /* are we locally complete? */
    if (trk->def_complete && pmix_list_get_size(&trk->local_cbs) == trk->nlocal) {
        locally_complete = true;
    }

    /* if we are not locally complete AND this operation
     * is completely local AND someone specified a timeout,
     * then we will monitor the timeout in this library.
     * Otherwise, any timeout must be done by the host
     * to avoid a race condition whereby we release the
     * tracker object while the host is still using it */
    if (!locally_complete && trk->local &&
        0 < tv.tv_sec && !trk->blk->event_active) {
        PMIX_THREADSHIFT_DELAY(trk, grp_timeout, tv.tv_sec);
        trk->event_active = true;
    }

    /* if we are not locally complete, then we are done */
    if (!locally_complete) {
        return PMIX_SUCCESS;
    }

    /* if all local contributions have been received,
     * shutdown the timeout event if active */
    if (trk->event_active) {
        pmix_event_del(&trk->ev);
    }

    /* let the local host's server know that we are at the
     * "fence" point - they will callback once the barrier
     * across all participants has been completed */

    pmix_output_verbose(2, pmix_server_globals.group_output,
                        "local group op complete with %d procs",
                        (int) trk->npcs);

    if (trk->local) {
        /* we have created the local group, so we technically
         * are done. However, we want to give the host a chance
         * to know about the group to support further operations.
         * For example, a tool might want to query the host to get
         * the IDs of existing groups. So if the host supports
         * group operations, pass this one up to it but indicate
         * it is strictly local */
        if (NULL != pmix_host_server.group) {
            /* need to add an info indicating that this is strictly a local
             * operation, and any group info that was provided */
            if (!force_local) {
                /* add the local op flag and any grp info to the info array */
                ninfo = trk->ninfo + 1;
                PMIX_INFO_CREATE(info, ninfo);
                m = 0;
                for (n=0; n < trk->ninfo; n++) {
                    PMIX_INFO_XFER(&info[m], &trk->info[n]);
                    ++m;
                }
                PMIX_INFO_LOAD(&info[m], PMIX_GROUP_LOCAL_ONLY, NULL, PMIX_BOOL);
                PMIX_INFO_FREE(trk->info, trk->ninfo);
                trk->info = info;
                trk->ninfo = ninfo;
                info = NULL;
                ninfo = 0;
            }
            rc = pmix_host_server.group(op, blk->id, trk->pcs, trk->npcs,
                                        trk->info, trk->ninfo, grpcbfunc, blk);
            PMIX_ERROR_LOG(rc);
            if (PMIX_SUCCESS != rc) {
                if (PMIX_OPERATION_SUCCEEDED == rc) {
                    /* let the grpcbfunc threadshift the result */
                    grpcbfunc(PMIX_SUCCESS, NULL, 0, blk, NULL, NULL);
                    return PMIX_SUCCESS;
                }
                /* remove the tracker from the list */
                pmix_list_remove_item(&pmix_server_globals.grp_collectives, &blk->super);
                PMIX_RELEASE(blk);
                pmix_list_remove_item(&trk->local_cbs, &cd->super);
                return rc;
            }
            /* we will take care of the rest of the process when the
             * host returns our call */
            return PMIX_SUCCESS;
        } else {
            /* let the grpcbfunc threadshift the result */
            grpcbfunc(PMIX_SUCCESS, NULL, 0, blk, NULL, NULL);
            return PMIX_SUCCESS;
        }
    }

    /* we don't have to worry about the timeout event being
     * active in the rest of this code because we only come
     * here if the operation is NOT completely local, and
     * we only activate the timeout if it IS local */

proceed:
    /* check if our host supports group operations */
    if (NULL == pmix_host_server.group) {
        /* cannot support it */
        pmix_list_remove_item(&pmix_server_globals.collectives, &blk->super);
        PMIX_RELEASE(blk);
        return PMIX_ERR_NOT_SUPPORTED;
    }

    rc = pmix_host_server.group(op, blk->id, trk->pcs, trk->npcs,
                                trk->info, trk->ninfo, grpcbfunc, blk);
    if (PMIX_SUCCESS != rc) {
        if (PMIX_OPERATION_SUCCEEDED == rc) {
            /* let the grpcbfunc threadshift the result */
            grpcbfunc(PMIX_SUCCESS, NULL, 0, blk, NULL, NULL);
            return PMIX_SUCCESS;
        }
        /* remove the tracker from the list */
        pmix_list_remove_item(&pmix_server_globals.grp_collectives, &blk->super);
        PMIX_RELEASE(blk);
        return rc;
    }

    return PMIX_SUCCESS;

error:
   if (NULL != info) {
        PMIX_INFO_FREE(info, ninfo);
    }
    return rc;
}
