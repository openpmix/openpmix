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
    pmix_proc_t *pcs;
    size_t npcs;
    pmix_info_t *info;
    size_t ninfo;
    pmix_list_t mbrs;  // list of grp_trk_t - one per participant call; its
                       //    size is the number of local participants that have
                       //    contributed
    pmix_list_t departed;   // ranks lost (connection dropped) BEFORE contributing;
                            //    list of pmix_proclist_t, deduplicated by name.
                            //    Counted alongside mbrs so the construct can
                            //    complete on the survivors rather than hang.
                            //    See docs/how-things-work/collectives.
    bool host_called;       // the block has been forwarded to the host - the
                            //    local phase is frozen
    bool def_complete;      // all local procs have been registered and the trk definition is complete
    uint32_t nlocal;        // number of local participants
} grp_block_t;
static void gbcon(grp_block_t *p)
{
    p->id = NULL;
    p->grpop = PMIX_GROUP_NONE;
    p->event_active = false;
    p->need_cxtid = false;
    p->pcs = NULL;
    p->npcs = 0;
    p->info = NULL;
    p->ninfo = 0;
    PMIX_CONSTRUCT(&p->mbrs, pmix_list_t);
    PMIX_CONSTRUCT(&p->departed, pmix_list_t);
    p->host_called = false;
    p->def_complete = false;
    p->nlocal = 0;
}
static void gbdes(grp_block_t *p)
{
    /* safety net: stop any still-armed local-phase timeout so it cannot fire
     * on freed memory (the normal paths delete it before releasing the block) */
    if (p->event_active) {
        pmix_event_del(&p->ev);
        p->event_active = false;
    }
    if (NULL != p->id) {
        free(p->id);
    }
    if (NULL != p->pcs) {
        PMIX_PROC_FREE(p->pcs, p->npcs);
    }
    if (NULL != p->info) {
        PMIX_INFO_FREE(p->info, p->ninfo);
    }
    PMIX_LIST_DESTRUCT(&p->mbrs);
    PMIX_LIST_DESTRUCT(&p->departed);
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
    bool hybrid;
    pmix_proc_t *pcs;
    size_t npcs;
    pmix_info_t *info;
    size_t ninfo;
    pmix_list_t local_cbs;  // list of pmix_server_caddy_t for sending result to the local participants
} grp_trk_t;
static void gtcon(grp_trk_t *t)
{
    t->event_active = false;
    PMIX_CONSTRUCT_LOCK(&t->lock);
    t->id = NULL;
    t->blk = NULL;
    t->host_called = false;
    t->hybrid = false;
    t->pcs = NULL;
    t->npcs = 0;
    t->info = NULL;
    t->ninfo = 0;
    PMIX_CONSTRUCT(&t->local_cbs, pmix_list_t);
}
static void gtdes(grp_trk_t *t)
{
    PMIX_DESTRUCT_LOCK(&t->lock);
    /* t->blk is a non-owning back-reference: the block owns its trackers
     * (they live on blk->mbrs and are freed by this destructor when the
     * block is released), so the tracker must NOT hold a reference on the
     * block - doing so forms a refcount cycle that leaks the entire block,
     * its trackers, and their queued participant caddies. */
    /* the tracker owns its copy of the participant array and the
     * info array handed to it by get_tracker */
    if (NULL != t->pcs) {
        PMIX_PROC_FREE(t->pcs, t->npcs);
    }
    if (NULL != t->info) {
        PMIX_INFO_FREE(t->info, t->ninfo);
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
static pmix_status_t aggregate_info(grp_block_t *blk);
static bool grp_notify_termination(grp_block_t *blk);

static void check_definition_complete(grp_block_t *blk)
{
    pmix_namespace_t *ns, *nptr;
    pmix_rank_info_t *info;
    size_t i;
    uint32_t nlocal = 0;
    grp_trk_t *trk;

    if (blk->def_complete) {
        return;
    }

    PMIX_LIST_FOREACH(trk, &blk->mbrs, grp_trk_t) {
        for (i = 0; i < trk->npcs; i++) {
            /* is this nspace known to us? */
            nptr = NULL;
            PMIX_LIST_FOREACH (ns, &pmix_globals.nspaces, pmix_namespace_t) {
                if (0 == strcmp(trk->pcs[i].nspace, ns->nspace)) {
                    nptr = ns;
                    break;
                }
            }
            if (NULL == nptr) {
                /* we don't know about this nspace - we need to
                 * wait until it has been registered */
                return;
            }
            /* it is possible we know about this nspace because the host
             * has registered one or more clients via "register_client",
             * but the host has not yet called "register_nspace". There is
             * a very tiny race condition whereby this can happen due
             * to event-driven processing, but account for it here */
            if (SIZE_MAX == nptr->nlocalprocs) {
                /* delay processing until this nspace is registered */
                return;
            }
            if (0 == nptr->nlocalprocs) {
                /* the host has informed us that this nspace has no local procs */
                pmix_output_verbose(5, pmix_server_globals.fence_output,
                                    "pmix_server_new_tracker: nspace %s has no local procs",
                                    trk->pcs[i].nspace);
                continue;
            }

            /* if they want all the local members of this nspace, then
             * add them in here. They told us how many procs will be
             * local to us from this nspace, but we don't know their
             * ranks. So as long as they want _all_ of them, we can
             * handle that case regardless of whether the individual
             * clients have been "registered" */
            if (PMIX_RANK_WILDCARD == trk->pcs[i].rank) {
                nlocal += nptr->nlocalprocs;
                continue;
            }

            /* They don't want all the local clients, or they are at
             * least listing them individually. Check if all the clients
             * for this nspace have been registered via "register_client"
             * so we know the specific ranks on this node */
            if (!nptr->all_registered) {
                /* nope, so no point in going further on this one - we'll
                 * process it once all the procs are known */
                pmix_output_verbose(5, pmix_server_globals.fence_output,
                                    "pmix_server_new_tracker: all clients not registered nspace %s",
                                    trk->pcs[i].nspace);
                return;
            }
            /* is this one of my local ranks? */
            PMIX_LIST_FOREACH (info, &nptr->ranks, pmix_rank_info_t) {
                if (trk->pcs[i].rank == info->pname.rank) {
                    pmix_output_verbose(5, pmix_server_globals.fence_output,
                                        "adding local proc %s.%d to tracker", info->pname.nspace,
                                        info->pname.rank);
                    /* track the count */
                    nlocal++;
                    break;
                }
            }
        }
    }

    // if we get here, then we have completed definition of the block
    blk->def_complete = true;
    blk->nlocal = nlocal;
}

/* The group analog of pmix_server_trk_complete: the block's definition must be
 * complete and every expected local participant must be accounted for - either
 * by having contributed (a member tracker on mbrs) or by having departed before
 * contributing (an entry on departed). Kept in the same shape as the
 * fence-family predicate. See docs/how-things-work/collectives. */
static bool grp_blk_locally_complete(grp_block_t *blk)
{
    if (!blk->def_complete) {
        return false;
    }
    return (pmix_list_get_size(&blk->mbrs) +
            pmix_list_get_size(&blk->departed)) >= blk->nlocal;
}

static pmix_status_t get_tracker(char *grpid, bool bootstrap, bool follower,
                                 pmix_proc_t *procs, size_t nprocs,
                                 pmix_info_t *info, size_t ninfo,
                                 grp_block_t **block, grp_trk_t **tracker)
{
    grp_block_t *blk;
    grp_trk_t *trk;


    if (follower || bootstrap) {
        // bootstrap operations maintain independent trackers
        // since we don't know who is going to participate as
        // a leader
        goto newblock;
    }

    // see if we already have a block for this ID
    PMIX_LIST_FOREACH(blk, &pmix_server_globals.grp_collectives, grp_block_t) {
        if (0 == strcmp(grpid, blk->id)) {
            // we do - pass it back
            *block = blk;
            // new signature, so create a tracker for it
            trk = PMIX_NEW(grp_trk_t);
            // non-owning back-reference - see gtdes
            trk->blk = blk;
            trk->npcs = nprocs;
            PMIX_PROC_CREATE(trk->pcs, trk->npcs);
            memcpy(trk->pcs, procs, nprocs * sizeof(pmix_proc_t));
            trk->info = info;
            trk->ninfo = ninfo;
            pmix_list_append(&blk->mbrs, &trk->super);
            *tracker = trk;
            check_definition_complete(blk);
            return PMIX_SUCCESS;
        }
    }

newblock:
    // new block
    blk = PMIX_NEW(grp_block_t);
    blk->id = strdup(grpid);
    pmix_list_append(&pmix_server_globals.grp_collectives, &blk->super);
    *block = blk;
    // setup a tracker for it
    trk = PMIX_NEW(grp_trk_t);
    // non-owning back-reference - see gtdes
    trk->npcs = nprocs;
    if (NULL != procs) {
        PMIX_PROC_CREATE(trk->pcs, trk->npcs);
        memcpy(trk->pcs, procs, nprocs * sizeof(pmix_proc_t));
    }
    trk->blk = blk;
    trk->info = info;
    trk->ninfo = ninfo;
    pmix_list_append(&blk->mbrs, &trk->super);
    if (follower || bootstrap) {
        blk->def_complete = true;
    } else {
        check_definition_complete(blk);
    }
    *tracker = trk;
    return PMIX_SUCCESS;
}

pmix_status_t pmix_server_process_grpinfo(size_t ctxid,
                                          pmix_info_t *pinfo,
                                          size_t npinfo)
{
    size_t m;
    pmix_kval_t kp;
    pmix_value_t val;
    pmix_data_array_t darray;
    pmix_info_t *piptr;
    pmix_status_t rc;
    pmix_proc_t procid;

    // the first element in the array must be the procID
    // of the process that contributed this info
    if (!PMIX_CHECK_KEY(&pinfo[0], PMIX_PROCID)) {
        PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
        return PMIX_ERR_BAD_PARAM;
    }
    memcpy(&procid, pinfo[0].value.data.proc, sizeof(pmix_proc_t));
    /* reconstruct each value as a qualified one based
     * on the ctxid */
    PMIX_CONSTRUCT(&kp, pmix_kval_t);
    kp.value = &val;
    kp.key = PMIX_QUALIFIED_VALUE;
    val.type = PMIX_DATA_ARRAY;
    for (m=1; m < npinfo; m++) {
        PMIX_DATA_ARRAY_CONSTRUCT(&darray, 2, PMIX_INFO);
        piptr = (pmix_info_t*)darray.array;
        /* the primary value is in the first position */
        PMIX_INFO_XFER(&piptr[0], &pinfo[m]);
        /* add the context ID qualifier */
        PMIX_INFO_LOAD(&piptr[1], PMIX_GROUP_CONTEXT_ID, &ctxid, PMIX_SIZE);
        PMIX_INFO_SET_QUALIFIER(&piptr[1]);
        /* add it to the kval */
        val.data.darray = &darray;
        /* store it */
        PMIX_GDS_STORE_KV(rc, pmix_globals.mypeer, &procid, PMIX_GLOBAL, &kp);
        PMIX_DATA_ARRAY_DESTRUCT(&darray);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            return rc;
        }
    }
    return PMIX_SUCCESS;
}

/* Synthesize a PMIX_GROUP_MEMBER_FAILED event for each member that was lost
 * before contributing to this group operation (a surviving construct or a
 * destruct that requested PMIX_GROUP_NOTIFY_TERMINATION). This is done entirely
 * within the PMIx server so it works with any host environment, not just one
 * that knows how to generate the event: the loss is recorded in blk->departed by
 * the server's own lost-connection / voluntary-leave accounting (see
 * account_departed), so the event is produced the same way regardless of which
 * host runs the collective. It is delivered to the surviving local group
 * members registered for the event; members on other nodes are notified by
 * their own local server, which performs the identical accounting on its half
 * of the collective. Runs on the progress thread, so it uses the internal
 * notification path (pmix_server_notify_client_of_event thread-shifts) and
 * never the public PMIx_Notify_event, which cannot be called from the progress
 * thread. */
static void notify_local_members_of_loss(grp_block_t *blk)
{
    pmix_proclist_t *dp;
    pmix_info_t info[2];
    pmix_status_t rc;

    PMIX_LIST_FOREACH (dp, &blk->departed, pmix_proclist_t) {
        PMIX_INFO_LOAD(&info[0], PMIX_EVENT_AFFECTED_PROC, &dp->proc, PMIX_PROC);
        PMIX_INFO_LOAD(&info[1], PMIX_GROUP_ID, blk->id, PMIX_STRING);
        rc = pmix_server_notify_client_of_event(PMIX_GROUP_MEMBER_FAILED,
                                                &pmix_globals.myid, PMIX_RANGE_LOCAL,
                                                info, 2, NULL, NULL);
        PMIX_INFO_DESTRUCT(&info[0]);
        PMIX_INFO_DESTRUCT(&info[1]);
        if (PMIX_SUCCESS != rc && PMIX_OPERATION_SUCCEEDED != rc) {
            PMIX_ERROR_LOG(rc);
        }
    }
}

static void _grpcbfunc(int sd, short args, void *cbdata)
{
    grp_shifter_t *scd = (grp_shifter_t *) cbdata;
    grp_block_t *blk = scd->blk, *bk, *nxt_bk;
    char *id;
    pmix_group_operation_t op;
    grp_trk_t *trk;
    pmix_server_caddy_t *cd;
    pmix_buffer_t *reply;
    pmix_status_t ret, rc;
    size_t n, ctxid = SIZE_MAX, nmembers = 0;
    pmix_proc_t *members = NULL;
    bool ctxid_given = false;
    pmix_info_t *iptr, *pinfo;
    size_t ninfo, npinfo;
    pmix_list_t grpinfo;
    pmix_info_caddy_t *g=NULL;
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

    PMIX_CONSTRUCT(&grpinfo, pmix_list_t);
    /* see if this group was assigned a context ID or collected data */
    for (n = 0; n < scd->ninfo; n++) {
        if (PMIX_CHECK_KEY(&scd->info[n], PMIX_GROUP_CONTEXT_ID)) {
            ret = PMIx_Value_get_number(&scd->info[n].value, &ctxid, PMIX_SIZE);
            if (PMIX_SUCCESS != ret) {
                PMIX_ERROR_LOG(ret);
            } else {
                ctxid_given = true;
            }

        } else if (PMIX_CHECK_KEY(&scd->info[n], PMIX_GROUP_MEMBERSHIP)) {
            members = (pmix_proc_t*)scd->info[n].value.data.darray->array;
            nmembers = scd->info[n].value.data.darray->size;

        } else if (PMIX_CHECK_KEY(&scd->info[n], PMIX_GROUP_INFO_ARRAY) ||
                   PMIX_CHECK_KEY(&scd->info[n], PMIX_GROUP_INFO)) {
            g = PMIX_NEW(pmix_info_caddy_t);
            g->info = &scd->info[n];
            pmix_list_append(&grpinfo, &g->super);

        }
    }

    // preserve the group id
    id = strdup(blk->id);
    op = blk->grpop;

    // if group info was provided, then we need to store it
    // in our hash table too
    if (0 < pmix_list_get_size(&grpinfo)) {
        // must have been given a context ID
        if (!ctxid_given) {
            if (NULL != scd->relfn) {
                scd->relfn(scd->cbdata);
            }
            PMIX_RELEASE(scd);
            PMIX_LIST_DESTRUCT(&grpinfo);
            return;
        }
        /* Each list member points to a pmix_info_t that contains
         * a data array of info from a given proc */
        PMIX_LIST_FOREACH(g, &grpinfo, pmix_info_caddy_t) {
            iptr = (pmix_info_t*)g->info->value.data.darray->array;
            ninfo = g->info->value.data.darray->size;

            if (PMIX_CHECK_KEY(g->info, PMIX_GROUP_INFO)) {
                // this is just a single array of group info
                rc = pmix_server_process_grpinfo(ctxid, iptr, ninfo);
                if (PMIX_SUCCESS != rc) {
                    PMIX_ERROR_LOG(rc);
                    if (NULL != scd->relfn) {
                        scd->relfn(scd->cbdata);
                    }
                    PMIX_RELEASE(scd);
                    PMIX_LIST_DESTRUCT(&grpinfo);
                    return;
                }
            } else {
                // contains an array of group info arrays
                for (n=0; n < ninfo; n++) {
                    pinfo = (pmix_info_t*)iptr[n].value.data.darray->array;
                    npinfo = iptr[n].value.data.darray->size;
                    rc = pmix_server_process_grpinfo(ctxid, pinfo, npinfo);
                    if (PMIX_SUCCESS != rc) {
                        PMIX_ERROR_LOG(rc);
                        if (NULL != scd->relfn) {
                            scd->relfn(scd->cbdata);
                        }
                        PMIX_RELEASE(scd);
                        PMIX_LIST_DESTRUCT(&grpinfo);
                        return;
                    }
                }
            }
        }
    }

    // because bootstrap will have added multiple blocks to the collectives
    // for each bootstrap operation, cycle across the list to find them all.
    // Use the SAFE variant: matching blocks are removed and released below,
    // so we must cache the next pointer before freeing the current block.
    PMIX_LIST_FOREACH_SAFE(bk, nxt_bk, &pmix_server_globals.grp_collectives, grp_block_t) {
        if (0 != strcmp(id, bk->id)) {
            continue;
        }
        PMIX_LIST_FOREACH(trk, &bk->mbrs, grp_trk_t) {
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
                if (PMIX_SUCCESS == scd->status && PMIX_GROUP_CONSTRUCT == op) {
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
                    /* add any group info */
                    ninfo = pmix_list_get_size(&grpinfo);
                    PMIX_BFROPS_PACK(ret, cd->peer, reply, &ninfo, 1, PMIX_SIZE);
                    if (PMIX_SUCCESS != ret) {
                        PMIX_ERROR_LOG(ret);
                        PMIX_RELEASE(reply);
                        break;
                    }
                    if (0 < ninfo) {
                        PMIX_LIST_FOREACH(g, &grpinfo, pmix_info_caddy_t) {
                            PMIX_BFROPS_PACK(ret, cd->peer, reply, g->info, 1, PMIX_INFO);
                            if (PMIX_SUCCESS != ret) {
                                PMIX_ERROR_LOG(ret);
                                PMIX_RELEASE(reply);
                                break;
                            }
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
        /* if any expected member was lost before contributing but the operation
         * still completed successfully on the survivors, tell the survivors
         * which members failed so fault-aware applications can react. This
         * applies to a construct that was allowed to survive the loss
         * (fault-tolerant collective tracking was requested) and to a destruct
         * for which termination notification was requested
         * (PMIX_GROUP_NOTIFY_TERMINATION). Skip it when the operation did not
         * complete successfully (status is not SUCCESS - e.g., a construct that
         * aborted): there is no surviving group to inform, and the participants
         * already learn of the outcome through the operation status. */
        if (PMIX_SUCCESS == scd->status &&
            0 < pmix_list_get_size(&bk->departed) &&
            (PMIX_GROUP_CONSTRUCT == bk->grpop ||
             (PMIX_GROUP_DESTRUCT == bk->grpop && grp_notify_termination(bk)))) {
            notify_local_members_of_loss(bk);
        }
        /* remove the block from the list */
        pmix_list_remove_item(&pmix_server_globals.grp_collectives, &bk->super);
        PMIX_RELEASE(bk);
    }

    /* we are done */
    free(id);
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

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        /* we are shutting down and cannot thread-shift. The block is still
         * on the collectives list (it is removed only in _grpcbfunc), so
         * leave it there for finalize to reclaim via PMIX_LIST_DESTRUCT
         * rather than releasing the list's reference out from under it. */
        return;
    }

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

/* Did any participant request fault-tolerant collective tracking via
 * PMIX_GROUP_FT_COLLECTIVE? When true, a construct completes on the surviving
 * membership if an expected member is lost before contributing. When false (the
 * default), the loss of a required member means the group cannot form as
 * requested and the construct is aborted instead. Only meaningful once the
 * block's info has been aggregated. */
static bool grp_ft_collective(grp_block_t *blk)
{
    size_t n;

    for (n = 0; n < blk->ninfo; n++) {
        if (PMIX_CHECK_KEY(&blk->info[n], PMIX_GROUP_FT_COLLECTIVE)) {
            return PMIX_INFO_TRUE(&blk->info[n]);
        }
    }
    return false;
}

/* Did any participant request termination notification via
 * PMIX_GROUP_NOTIFY_TERMINATION? The group's failure policy is chosen at
 * construct time, but the server holds no group state between operations, so
 * the client re-attaches the flag to the destruct request (see
 * PMIx_Group_destruct_nb). When true, a member lost before contributing to a
 * destruct is reported to the survivors via a synthesized
 * PMIX_GROUP_MEMBER_FAILED event and the destruct completes on the survivors
 * with success - the event serving in place of an error return. When false (the
 * default), the loss is recorded as a degraded local-collective status so the
 * teardown is reported as an error. Only meaningful once the block's info has
 * been aggregated. */
static bool grp_notify_termination(grp_block_t *blk)
{
    size_t n;

    for (n = 0; n < blk->ninfo; n++) {
        if (PMIX_CHECK_KEY(&blk->info[n], PMIX_GROUP_NOTIFY_TERMINATION)) {
            return PMIX_INFO_TRUE(&blk->info[n]);
        }
    }
    return false;
}

/* Completion callback for a server-issued PMIX_GROUP_CANCEL to the host. The
 * cancel is fire-and-forget from our side - our own local participants are
 * completed directly (see abort_construct), and the host's resulting abort of
 * the cross-server collective reaches the other servers through the normal
 * group release path - so here we need only honor the release contract. */
static void cancel_cbfunc(pmix_status_t status, pmix_info_t *info, size_t ninfo,
                          void *cbdata, pmix_release_cbfunc_t release_fn,
                          void *release_cbdata)
{
    PMIX_HIDE_UNUSED_PARAMS(status, info, ninfo, cbdata);
    if (NULL != release_fn) {
        release_fn(release_cbdata);
    }
}

/* Abort an in-flight group construct during its local phase, completing our own
 * local participants with the given status (PMIX_GROUP_CONSTRUCT_ABORT when a
 * required member was lost and fault-tolerant collective tracking was not
 * requested, PMIX_ERR_TIMEOUT when the local-phase timer expired). Each
 * participant's PMIx_Group_construct returns that status rather than silently
 * forming a reduced group; grpcbfunc consumes (releases) the block via its
 * threadshift, matching the survive path.
 *
 * If a host is present, also ask it to cancel the cross-server collective:
 * other servers may already have forwarded their local phase, leaving the
 * host's collective waiting for a contribution this server will never send. The
 * cancel unsticks the host, which aborts the collective on the remaining
 * servers through their own group releases. We deliberately complete our local
 * participants directly here rather than through that release - this block is
 * removed below, so the host's abort release is a no-op back on this server.
 *
 * Called only before the block is forwarded to the host (host_called is still
 * false), so no host completion can be in flight for it. */
static void abort_construct(grp_block_t *blk, pmix_status_t st)
{
    /* the local phase is ending - stop any pending local-phase timeout */
    if (blk->event_active) {
        pmix_event_del(&blk->ev);
        blk->event_active = false;
    }
    if (NULL != pmix_host_server.group) {
        pmix_host_server.group(PMIX_GROUP_CANCEL, blk->id, NULL, 0, NULL, 0,
                               cancel_cbfunc, NULL);
    }
    blk->host_called = true;
    grpcbfunc(st, NULL, 0, blk, NULL, NULL);
}

/* Local-phase timeout handler for a group construct. Armed (only when the
 * caller supplied PMIX_TIMEOUT) while we wait for all local participants to
 * contribute, and deleted the moment the local phase completes and the block is
 * forwarded to the host - so if it fires, the block has not been forwarded and
 * aborting it here cannot race a host completion. */
static void group_timeout(int sd, short args, void *cbdata)
{
    grp_block_t *blk = (grp_block_t *) cbdata;
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    pmix_output_verbose(2, pmix_server_globals.group_output,
                        "group construct timed out for %s", blk->id);
    blk->event_active = false;
    abort_construct(blk, PMIX_ERR_TIMEOUT);
}

static pmix_status_t aggregate_info(grp_block_t *blk)
{
    grp_trk_t *trk;
    pmix_list_t ilist, nmlist, plist;
    size_t n, m, j, k, niptr;
    pmix_info_t *iptr;
    bool found, nmfound;
    pmix_info_caddy_t *icd;
    pmix_proclist_t *nm;
    pmix_proc_t *nmarray, *trkarray, *tmp;
    size_t nmsize, trksize, bt, bt2;
    pmix_status_t rc;

    // only keep unique entries
    PMIX_CONSTRUCT(&ilist, pmix_list_t);
    PMIX_CONSTRUCT(&plist, pmix_list_t);

    PMIX_LIST_FOREACH(trk, &blk->mbrs, grp_trk_t) {
        // aggregate procs
        for (n=0; n < trk->npcs; n++) {
            found = false;
            for (m=0; m < blk->npcs; m++) {
                if (PMIX_CHECK_PROCID(&trk->pcs[n], &blk->pcs[m])) {
                    found = true;
                    // preserve WILDCARD rank
                    if (PMIX_RANK_WILDCARD == trk->pcs[n].rank) {
                        blk->pcs[m].rank = PMIX_RANK_WILDCARD;
                    }
                    break;
                }
            }
            if (!found) {
                nm = PMIX_NEW(pmix_proclist_t);
                memcpy(&nm->proc, &trk->pcs[n], sizeof(pmix_proc_t));
                pmix_list_append(&plist, &nm->super);
            }
        }
        // aggregate info structs
        for (n=0; n < trk->ninfo; n++) {
            found = false;
            for (m=0; m < blk->ninfo; m++) {
                // if the keys match, then aggregate the values
                if (PMIX_CHECK_KEY(&trk->info[n], blk->info[m].key)) {
                    // check a few critical keys
                    if (PMIX_CHECK_KEY(&blk->info[m], PMIX_GROUP_ADD_MEMBERS)) {
                        // aggregate the members
                        nmarray = (pmix_proc_t*)blk->info[m].value.data.darray->array;
                        nmsize = blk->info[m].value.data.darray->size;
                        trkarray = (pmix_proc_t*)trk->info[n].value.data.darray->array;
                        trksize = trk->info[n].value.data.darray->size;
                        PMIX_CONSTRUCT(&nmlist, pmix_list_t);
                        // sadly, an exhaustive search
                        for (k=0; k < trksize; k++) {
                            nmfound = false;
                            for (j=0; j < nmsize; j++) {
                                if (PMIX_CHECK_PROCID(&nmarray[j], &trkarray[k])) {
                                    // if the new one is rank=WILDCARD, then ensure
                                    // we keep it as wildcard
                                    if (PMIX_RANK_WILDCARD == trkarray[k].rank) {
                                        nmarray[j].rank = PMIX_RANK_WILDCARD;
                                    }
                                    nmfound = true;
                                    break;
                                }
                            }
                            if (!nmfound) {
                                nm = PMIX_NEW(pmix_proclist_t);
                                memcpy(&nm->proc, &trkarray[k], sizeof(pmix_proc_t));
                                pmix_list_append(&nmlist, &nm->super);
                            }
                        }
                        // create the replacement array, if needed
                        if (0 < pmix_list_get_size(&nmlist)) {
                            bt = nmsize + pmix_list_get_size(&nmlist);
                            PMIX_PROC_CREATE(tmp, bt);
                            memcpy(tmp, nmarray, nmsize * sizeof(pmix_proc_t));
                            j = nmsize;
                            PMIX_LIST_FOREACH(nm, &nmlist, pmix_proclist_t) {
                                memcpy(&tmp[j], &nm->proc, sizeof(pmix_proc_t));
                                ++j;
                            }
                            PMIX_PROC_FREE(nmarray, nmsize);
                            blk->info[m].value.data.darray->array = tmp;
                            blk->info[m].value.data.darray->size = bt;
                        }
                        PMIX_LIST_DESTRUCT(&nmlist);

                    } else if (PMIX_CHECK_KEY(&blk->info[m], PMIX_GROUP_BOOTSTRAP)) {
                        // the numbers must match
                        rc = PMIx_Value_get_number(&blk->info[m].value, &bt, PMIX_SIZE);
                        if (PMIX_SUCCESS != rc) {
                            PMIX_LIST_DESTRUCT(&ilist);
                            return rc;
                        }
                        rc = PMIx_Value_get_number(&trk->info[n].value, &bt2, PMIX_SIZE);
                        if (PMIX_SUCCESS != rc) {
                            PMIX_LIST_DESTRUCT(&ilist);
                            return rc;
                        }
                        if (bt != bt2) {
                            PMIX_LIST_DESTRUCT(&ilist);
                            return PMIX_ERR_BAD_PARAM;
                        }
                    } else if (PMIX_CHECK_KEY(&blk->info[m], PMIX_PROC_DATA) ||
                               PMIX_CHECK_KEY(&blk->info[m], PMIX_GROUP_INFO)) {
                        // keep the duplicates
                        icd = PMIX_NEW(pmix_info_caddy_t);
                        icd->info = &trk->info[n];
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
                icd->info = &trk->info[n];
                icd->ninfo = 1;
                pmix_list_append(&ilist, &icd->super);
            }
        }
    }
    if (0 < pmix_list_get_size(&plist)) {
        m = blk->npcs + pmix_list_get_size(&plist);
        PMIX_PROC_CREATE(tmp, m);
        if (NULL != blk->pcs) {
            memcpy(tmp, blk->pcs, blk->npcs * sizeof(pmix_proc_t));
        }
        n = blk->npcs;
        PMIX_LIST_FOREACH(nm, &plist, pmix_proclist_t) {
            memcpy(&tmp[n], &nm->proc, sizeof(pmix_proc_t));
            n++;
        }
        if (NULL != blk->pcs) {
            PMIX_PROC_FREE(blk->pcs, blk->npcs);
        }
        blk->pcs = tmp;
        blk->npcs = m;
    }
    if (0 < pmix_list_get_size(&ilist)) {
        niptr = blk->ninfo + pmix_list_get_size(&ilist);
        PMIX_INFO_CREATE(iptr, niptr);
        for (n=0; n < blk->ninfo; n++) {
            PMIX_INFO_XFER(&iptr[n], &blk->info[n]);
        }
        n = blk->ninfo;
        PMIX_LIST_FOREACH(icd, &ilist, pmix_info_caddy_t) {
            PMIX_INFO_XFER(&iptr[n], icd->info);
            ++n;
        }
        PMIX_INFO_FREE(blk->info, blk->ninfo);
        blk->info = iptr;
        blk->ninfo = niptr;
        /* cleanup */
    }
    PMIX_LIST_DESTRUCT(&ilist);
    PMIX_LIST_DESTRUCT(&plist);
    return PMIX_SUCCESS;
}

/* we are being called from the PMIx server's switchyard function,
 * which means we are in an event and can access global data */
pmix_status_t pmix_server_group(pmix_server_caddy_t *cd, pmix_buffer_t *buf,
                                pmix_group_operation_t op)
{
    pmix_peer_t *peer = (pmix_peer_t *) cd->peer;
    int32_t cnt;
    pmix_status_t rc;
    char *grpid;
    pmix_proc_t *procs = NULL;
    pmix_info_t *info = NULL;
    size_t n, ninfo, ninf, nprocs;
    grp_block_t *blk;
    grp_trk_t *trk;
    bool need_cxtid = false;
    bool bootstrap = false;
    bool follower = false;
    uint32_t tmo = 0;

    pmix_output_verbose(2, pmix_server_globals.group_output,
                        "recvd grpconstruct cmd from %s",
                        PMIX_PEER_PRINT(cd->peer));

    /* check if our host supports group operations */
    if (NULL == pmix_host_server.group) {
        /* cannot support it */
        return PMIX_ERR_NOT_SUPPORTED;
    }

    /* unpack the group ID */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, peer, buf, &grpid, &cnt, PMIX_STRING);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto error;
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
        // if no procs were given, then this must be a follower
        follower = true;
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

        } else if (PMIX_CHECK_KEY(&info[n], PMIX_GROUP_BOOTSTRAP)) {
            // we don't care what the number is, we only care that
            // bootstrap is being given
            bootstrap = true;

        } else if (PMIX_CHECK_KEY(&info[n], PMIX_TIMEOUT)) {
            // bound how long we wait for all local participants to contribute
            PMIx_Value_get_number(&info[n].value, &tmo, PMIX_UINT32);
        }
    }

    /* find/create the local tracker for this operation */
    rc = get_tracker(grpid, bootstrap, follower, procs, nprocs,
                     info, ninfo, &blk, &trk);
    if (PMIX_EXISTS == rc) {
        // we extended the trk's info array
        PMIX_INFO_FREE(info, ninfo);
        info = NULL;
        ninfo = 0;
    }
    // grpid has been copied into the tracker
    free(grpid);
    // get_tracker copied the participant array into the tracker, so
    // release our unpacked copy - the tracker's trk->pcs is used from
    // here on
    if (NULL != procs) {
        PMIX_PROC_FREE(procs, nprocs);
        procs = NULL;
    }
    // mark as a construct op
    blk->grpop = op;
    // track ctx id request
    if (!blk->need_cxtid) {
        blk->need_cxtid = need_cxtid;
    }
    // track the callback
    pmix_list_append(&trk->local_cbs, &cd->super);

    if (follower || bootstrap) {
        /* this is an add-member, so pass it up by itself. We cannot
         * know if there will be a group leader calling from this node,
         * so we cannot aggregate info from this proc. We therefore pass
         * everything up separately and rely on the host to properly
         * deal with it */
        rc = pmix_host_server.group(PMIX_GROUP_CONSTRUCT, blk->id, trk->pcs, trk->npcs,
                                    trk->info, trk->ninfo, grpcbfunc, blk);
        if (PMIX_OPERATION_SUCCEEDED == rc) {
            // the host will not be calling back
            /* let the grpcbfunc threadshift the result */
            grpcbfunc(PMIX_SUCCESS, NULL, 0, blk, NULL, NULL);
            return PMIX_SUCCESS;
        }
        if (PMIX_SUCCESS != rc) {
            pmix_list_remove_item(&trk->local_cbs, &cd->super);
            // the trk will be released when we release the blk
            pmix_list_remove_item(&pmix_server_globals.grp_collectives, &blk->super);
            PMIX_RELEASE(blk);
            return rc;
        }
        return PMIX_SUCCESS;
    }

    /* arm the local-phase timeout if one was requested and not already set. It
     * bounds how long we wait for all local participants to contribute; once the
     * local phase completes and we forward to the host, the timer is deleted and
     * the host owns any further timeout (mirroring the fence family). */
    if (0 < tmo && !blk->event_active) {
        PMIX_THREADSHIFT_DELAY(blk, group_timeout, tmo);
        blk->event_active = true;
    }

    /* are we locally complete? */
    if (blk->host_called || !grp_blk_locally_complete(blk)) {
        /* if we are not locally complete, then we are done */
        return PMIX_SUCCESS;
    }

    pmix_output_verbose(2, pmix_server_globals.group_output,
                        "local group op complete with %d procs",
                        (int) trk->npcs);

    rc = aggregate_info(blk);
    if (PMIX_SUCCESS != rc) {
        pmix_list_remove_item(&trk->local_cbs, &cd->super);
        // the trk will be released when we release the blk
        pmix_list_remove_item(&pmix_server_globals.grp_collectives, &blk->super);
        PMIX_RELEASE(blk);
        return rc;
    }
    /* if an expected member was lost before contributing, how we proceed
     * depends on the operation and its directives. For a construct without
     * PMIX_GROUP_FT_COLLECTIVE (the default), the group cannot form as
     * requested, so abort it rather than silently reduce the membership.
     * Otherwise the operation completes on the survivors. A destruct for which
     * PMIX_GROUP_NOTIFY_TERMINATION was requested completes cleanly (success),
     * with the survivors told which member was lost via a synthesized
     * PMIX_GROUP_MEMBER_FAILED event (see _grpcbfunc) - the event serving in
     * place of an error return. Every other survive path (a fault-tolerant
     * construct, or a destruct without notification) records the degraded status
     * in the block's info so the host sees it - matching the
     * fence/connect/disconnect families. The status slot is located by key (see
     * pmix_server_set_collective_status). */
    if (0 < pmix_list_get_size(&blk->departed)) {
        if (PMIX_GROUP_CONSTRUCT == op && !grp_ft_collective(blk)) {
            abort_construct(blk, PMIX_GROUP_CONSTRUCT_ABORT);
            return PMIX_SUCCESS;
        }
        if (!(PMIX_GROUP_DESTRUCT == op && grp_notify_termination(blk))) {
            pmix_server_set_collective_status(blk->info, blk->ninfo,
                                              PMIX_ERR_LOST_CONNECTION);
        }
    }
    /* local phase complete - delete the local-phase timer before handing the
     * block to the host, so a late internal timeout cannot race the host's
     * completion (which could return the block after we released it) */
    if (blk->event_active) {
        pmix_event_del(&blk->ev);
        blk->event_active = false;
    }
    // the local phase is complete - freeze it and pass up to the host
    blk->host_called = true;
    rc = pmix_host_server.group(op, blk->id, trk->pcs, trk->npcs,
                                blk->info, blk->ninfo, grpcbfunc, blk);
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

/* Account for a single departed participant in one in-flight group block, where
 * "departed" means the named rank will never contribute to this block - either
 * because its connection was lost (pmix_server_grp_peer_lost) or because it
 * voluntarily left the group (pmix_server_grp_member_left). Participation is
 * tracked by identity: if the rank has already contributed (a member tracker
 * holds a caddy with its name) its contribution stands and the departure is
 * ignored (Case A); otherwise the rank is recorded as departed so the block can
 * complete on the surviving participants instead of hanging (Case B), and if
 * that completes the local phase the block is forwarded to the host. The block
 * may be released (on error) - callers must iterate with FOREACH_SAFE. See
 * docs/how-things-work/collectives. */
static void account_departed(grp_block_t *blk, const pmix_proc_t *proc)
{
    grp_trk_t *trk;
    pmix_server_caddy_t *scd;
    pmix_proclist_t *dp;
    pmix_status_t rc;
    bool ismbr, contributed, known;
    size_t n;

    /* bootstrap/follower blocks are passed up per-call and never wait on a
     * local count (nlocal == 0), so nothing here can hang; and a block that
     * has already been forwarded to the host has a frozen local phase.
     * Skip both. */
    if (0 == blk->nlocal || blk->host_called) {
        return;
    }
    /* is this proc an expected member of this group, and has its rank
     * already contributed? */
    ismbr = false;
    contributed = false;
    PMIX_LIST_FOREACH (trk, &blk->mbrs, grp_trk_t) {
        for (n = 0; n < trk->npcs; n++) {
            if (PMIX_CHECK_NAMES(&trk->pcs[n], proc)) {
                ismbr = true;
                break;
            }
        }
        PMIX_LIST_FOREACH (scd, &trk->local_cbs, pmix_server_caddy_t) {
            if (PMIX_CHECK_NAMES(&scd->peer->info->pname, proc)) {
                contributed = true;
                break;
            }
        }
        if (contributed) {
            break;
        }
    }
    if (!ismbr || contributed) {
        /* not a participant, or Case A (already contributed - its
         * contribution and data stand, so ignore the departure) */
        return;
    }
    /* Case B: the rank had not yet contributed and never will. Record it as
     * departed (once) so the block can complete on the survivors. */
    known = false;
    PMIX_LIST_FOREACH (dp, &blk->departed, pmix_proclist_t) {
        if (PMIX_CHECK_NAMES(&dp->proc, proc)) {
            known = true;
            break;
        }
    }
    if (!known) {
        dp = PMIX_NEW(pmix_proclist_t);
        PMIX_LOAD_PROCID(&dp->proc, proc->nspace, proc->rank);
        pmix_list_append(&blk->departed, &dp->super);
    }
    /* did that complete the local phase? if so we must forward to the host
     * now - no further participant call will arrive to do it for us. All
     * non-bootstrap members carry the same membership, so use the first. */
    if (grp_blk_locally_complete(blk)) {
        trk = (grp_trk_t *) pmix_list_get_first(&blk->mbrs);
        rc = aggregate_info(blk);
        if (PMIX_SUCCESS != rc) {
            pmix_list_remove_item(&pmix_server_globals.grp_collectives, &blk->super);
            PMIX_RELEASE(blk);
            return;
        }
        /* a member departed before contributing (that is why we are here).
         * For a construct without PMIX_GROUP_FT_COLLECTIVE (the default), the
         * group cannot form as requested, so abort it rather than silently
         * reduce the membership. Otherwise complete on the survivors. A destruct
         * for which PMIX_GROUP_NOTIFY_TERMINATION was requested completes cleanly
         * (the survivors are told which member was lost via a synthesized
         * PMIX_GROUP_MEMBER_FAILED event in _grpcbfunc); every other survive path
         * records the degraded status in the block's info so the host sees it,
         * matching the fence/connect/disconnect families. */
        if (PMIX_GROUP_CONSTRUCT == blk->grpop && !grp_ft_collective(blk)) {
            abort_construct(blk, PMIX_GROUP_CONSTRUCT_ABORT);
            return;
        }
        if (!(PMIX_GROUP_DESTRUCT == blk->grpop && grp_notify_termination(blk))) {
            pmix_server_set_collective_status(blk->info, blk->ninfo,
                                              PMIX_ERR_LOST_CONNECTION);
        }
        /* delete the local-phase timer before handing the block to the host,
         * so a late internal timeout cannot race the host's completion */
        if (blk->event_active) {
            pmix_event_del(&blk->ev);
            blk->event_active = false;
        }
        blk->host_called = true;
        rc = pmix_host_server.group(blk->grpop, blk->id, trk->pcs, trk->npcs,
                                    blk->info, blk->ninfo, grpcbfunc, blk);
        if (PMIX_OPERATION_SUCCEEDED == rc) {
            /* the host will not call back - drive completion ourselves */
            grpcbfunc(PMIX_SUCCESS, NULL, 0, blk, NULL, NULL);
        } else if (PMIX_SUCCESS != rc) {
            pmix_list_remove_item(&pmix_server_globals.grp_collectives, &blk->super);
            PMIX_RELEASE(blk);
        }
    }
}

/* Account for a lost peer across all in-flight group construct/destruct blocks.
 * This is the group-family analog of the fence/connect/disconnect accounting in
 * lost_connection(), expressed in the group's two-level structure, and is
 * called from that same handler. See account_departed(). */
void pmix_server_grp_peer_lost(pmix_peer_t *peer)
{
    grp_block_t *blk, *bnext;
    pmix_proc_t proc;

    /* the peer's identity is carried as a pmix_name_t; account_departed works
     * in terms of pmix_proc_t (as the group membership arrays do), so convert */
    PMIX_LOAD_PROCID(&proc, peer->info->pname.nspace, peer->info->pname.rank);
    PMIX_LIST_FOREACH_SAFE (blk, bnext, &pmix_server_globals.grp_collectives, grp_block_t) {
        account_departed(blk, &proc);
    }
}

/* Account for a member voluntarily leaving a group (via PMIx_Group_leave, which
 * generates a PMIX_GROUP_LEFT event that the server intercepts). A voluntary
 * leave is the deliberate cousin of a lost participant: any in-flight
 * construct/destruct block for that group that was still waiting on the departed
 * member must complete on the survivors rather than hang. Unlike a lost
 * connection, this is scoped to the one named group. See account_departed(). */
void pmix_server_grp_member_left(const char *grpid, const pmix_proc_t *proc)
{
    grp_block_t *blk, *bnext;

    PMIX_LIST_FOREACH_SAFE (blk, bnext, &pmix_server_globals.grp_collectives, grp_block_t) {
        if (0 != strcmp(grpid, blk->id)) {
            continue;
        }
        account_departed(blk, proc);
    }
}
