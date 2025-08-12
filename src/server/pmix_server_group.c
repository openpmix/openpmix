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
 * Copyright (c) 2021-2025 Nanook Consulting  All rights reserved.
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
    pmix_list_t mbrs;  // list of grp_trk_t
    bool def_complete;      // all local procs have been registered and the trk definition is complete
    uint32_t nlocal;        // number of local participants
    uint32_t local_cnt;     // number of local participants who have contributed
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
    p->def_complete = false;
    p->nlocal = 0;
    p->local_cnt = 0;
}
static void gbdes(grp_block_t *p)
{
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
static pmix_status_t aggregate_info(grp_block_t *blk);

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
            PMIX_RETAIN(blk);
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
    PMIX_RETAIN(blk);
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

static void _grpcbfunc(int sd, short args, void *cbdata)
{
    grp_shifter_t *scd = (grp_shifter_t *) cbdata;
    grp_block_t *blk = scd->blk, *bk;
    char *id;
    pmix_group_operation_t op;
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
            ret = PMIx_Value_get_number(&scd->info[n].value, &ctxid, PMIX_SIZE);
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

    // preserve the group id
    id = strdup(blk->id);
    op = blk->grpop;

    // because bootstrap will have added multiple blocks to the collectives
    // for each bootstrap operation, cycle across the list to find them all
    PMIX_LIST_FOREACH(bk, &pmix_server_globals.grp_collectives, grp_block_t) {
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
        pmix_list_remove_item(&pmix_server_globals.grp_collectives, &bk->super);
        PMIX_RELEASE(bk);
    }

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
        rc = pmix_host_server.group(PMIX_GROUP_CONSTRUCT, blk->id, procs, nprocs,
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

    /* are we locally complete? */
    if (!blk->def_complete || pmix_list_get_size(&blk->mbrs) != blk->nlocal) {
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
