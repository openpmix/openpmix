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

/* Construction and destruction of the server library's classes. The
 * PMIX_CLASS_INSTANCE for every type declared in pmix_server_ops.h, or
 * declared in pmix_globals.h and used only by the server role, lives
 * here so the ownership rules a destructor encodes can be read in one
 * place. A type private to a single .c file keeps its class beside it
 * instead - see pmix_server_group.c, pmix_server_fence.c,
 * pmix_server_get.c and pmix_server_control.c.
 *
 * Note that PMIX_NEW does not zero the allocation, so a member a
 * constructor here skips starts out as whatever was on the heap. */

#include "src/include/pmix_config.h"

#include "src/include/pmix_stdint.h"

#include "include/pmix_server.h"
#include "src/include/pmix_globals.h"

#ifdef HAVE_STRING_H
#    include <string.h>
#endif
#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif
#ifdef HAVE_TIME_H
#    include <time.h>
#endif

#include "src/class/pmix_list.h"
#include "src/common/pmix_iof.h"
#include "src/mca/bfrops/base/base.h"
#include "src/mca/ptl/base/base.h"
#include "src/util/pmix_error.h"
#include "src/util/pmix_output.h"

#include "pmix_server_ops.h"

/*****    INSTANCE SERVER LIBRARY CLASSES    *****/
static void gcon(pmix_grpinfo_t *p)
{
    PMIX_LOAD_PROCID(&p->proc, NULL, PMIX_RANK_UNDEF);
    PMIX_BYTE_OBJECT_CONSTRUCT(&p->blob);
}
static void gdes(pmix_grpinfo_t *p)
{
    PMIX_BYTE_OBJECT_DESTRUCT(&p->blob);
}
PMIX_CLASS_INSTANCE(pmix_grpinfo_t,
                    pmix_list_item_t,
                    gcon, gdes);

static void tcon(pmix_server_trkr_t *t)
{
    t->event_active = false;
    t->host_called = false;
    t->local = true;
    t->id = NULL;
    memset(t->pname.nspace, 0, PMIX_MAX_NSLEN + 1);
    t->pname.rank = PMIX_RANK_UNDEF;
    t->pcs = NULL;
    t->npcs = 0;
    PMIX_CONSTRUCT(&t->nslist, pmix_list_t);
    PMIX_CONSTRUCT_LOCK(&t->lock);
    t->def_complete = false;
    PMIX_CONSTRUCT(&t->local_cbs, pmix_list_t);
    PMIX_CONSTRUCT(&t->departed, pmix_list_t);
    t->nlocal = 0;
    t->info = NULL;
    t->ninfo = 0;
    PMIX_CONSTRUCT(&t->grpinfo, pmix_list_t);
    t->grpop = PMIX_GROUP_NONE;
    /* this needs to be set explicitly */
    t->collect_type = PMIX_COLLECT_INVALID;
    t->modexcbfunc = NULL;
    t->op_cbfunc = NULL;
    t->info_cbfunc = NULL;
    t->hybrid = false;
    t->cbdata = NULL;
}
static void tdes(pmix_server_trkr_t *t)
{
    if (NULL != t->id) {
        free(t->id);
    }
    PMIX_DESTRUCT_LOCK(&t->lock);
    if (NULL != t->pcs) {
        free(t->pcs);
    }
    PMIX_LIST_DESTRUCT(&t->local_cbs);
    PMIX_LIST_DESTRUCT(&t->departed);
    if (NULL != t->info) {
        PMIX_INFO_FREE(t->info, t->ninfo);
    }
    PMIX_LIST_DESTRUCT(&t->grpinfo);
    PMIX_LIST_DESTRUCT(&t->nslist);
}
PMIX_CLASS_INSTANCE(pmix_server_trkr_t,
                    pmix_list_item_t,
                    tcon, tdes);

static void cdcon(pmix_server_caddy_t *cd)
{
    memset(&cd->ev, 0, sizeof(pmix_event_t));
    cd->event_active = false;
    cd->trk = NULL;
    cd->peer = NULL;
    cd->info = NULL;
    cd->ninfo = 0;
    cd->query = NULL;
    cd->key = NULL;
}
static void cddes(pmix_server_caddy_t *cd)
{
    if (cd->event_active) {
        pmix_event_del(&cd->ev);
    }
    if (NULL != cd->trk) {
        PMIX_RELEASE(cd->trk);
    }
    if (NULL != cd->peer) {
        PMIX_RELEASE(cd->peer);
    }
    if (NULL != cd->info) {
        PMIX_INFO_FREE(cd->info, cd->ninfo);
    }
    if (NULL != cd->query) {
        PMIX_QUERY_FREE(cd->query, 1);
    }
    if (NULL != cd->key) {
        free(cd->key);
    }
}
PMIX_CLASS_INSTANCE(pmix_server_caddy_t,
                    pmix_list_item_t,
                    cdcon, cddes);

static void scadcon(pmix_setup_caddy_t *p)
{
    p->peer = NULL;
    memset(&p->proc, 0, sizeof(pmix_proc_t));
    PMIX_CONSTRUCT_LOCK(&p->lock);
    p->nspace = NULL;
    p->status = PMIX_SUCCESS;
    p->uid = 0;
    p->gid = 0;
    p->codes = NULL;
    p->ncodes = 0;
    p->nactive = 0;
    p->procs = NULL;
    p->nprocs = 0;
    p->apps = NULL;
    p->napps = 0;
    p->server_object = NULL;
    p->nlocalprocs = 0;
    p->info = NULL;
    p->ninfo = 0;
    p->units = NULL;
    p->nunits = 0;
    p->copied = false;
    p->keys = NULL;
    p->channels = PMIX_FWD_NO_CHANNELS;
    pmix_iof_init_flags(&p->flags);
    p->xoff = false;
    p->bo = NULL;
    p->nbo = 0;
    p->cbfunc = NULL;
    p->opcbfunc = NULL;
    p->setupcbfunc = NULL;
    p->lkcbfunc = NULL;
    p->spcbfunc = NULL;
    p->cbdata = NULL;
}
static void scaddes(pmix_setup_caddy_t *p)
{
    if (NULL != p->peer) {
        PMIX_RELEASE(p->peer);
    }
    PMIX_PROC_FREE(p->procs, p->nprocs);
    if (p->copied) {
        if (NULL != p->info) {
            PMIX_INFO_FREE(p->info, p->ninfo);
        }
        if (NULL != p->apps) {
            PMIX_APP_FREE(p->apps, p->napps);
        }
    }
    PMIX_RESOURCE_UNIT_FREE(p->units, p->nunits);
    if (NULL != p->bo) {
        PMIX_BYTE_OBJECT_FREE(p->bo, p->nbo);
    }
    PMIX_DESTRUCT_LOCK(&p->lock);
    if (NULL != p->flags.file) {
        free(p->flags.file);
    }
    if (NULL != p->flags.directory) {
        free(p->flags.directory);
    }
}
PMIX_EXPORT PMIX_CLASS_INSTANCE(pmix_setup_caddy_t,
                                pmix_object_t,
                                scadcon, scaddes);

static void tkcon(pmix_trkr_caddy_t *p)
{
    p->trk = NULL;
}
static void tkdes(pmix_trkr_caddy_t *p)
{
    /* PMIX_SETUP_COLLECTIVE retained the tracker so it could not be
     * released while the caddy was in flight - give that back */
    if (NULL != p->trk) {
        PMIX_RELEASE(p->trk);
    }
}
PMIX_CLASS_INSTANCE(pmix_trkr_caddy_t, pmix_object_t, tkcon, tkdes);

static void dmcon(pmix_dmdx_remote_t *p)
{
    p->cd = NULL;
}
static void dmdes(pmix_dmdx_remote_t *p)
{
    if (NULL != p->cd) {
        PMIX_RELEASE(p->cd);
    }
}
PMIX_CLASS_INSTANCE(pmix_dmdx_remote_t,
                    pmix_list_item_t,
                    dmcon, dmdes);

static void dmrqcon(pmix_dmdx_request_t *p)
{
    memset(&p->ev, 0, sizeof(pmix_event_t));
    p->event_active = false;
    p->lcd = NULL;
    p->key = NULL;
}
static void dmrqdes(pmix_dmdx_request_t *p)
{
    if (p->event_active) {
        pmix_event_del(&p->ev);
    }
    if (NULL != p->lcd) {
        PMIX_RELEASE(p->lcd);
    }
    if (NULL != p->key) {
        free(p->key);
    }
}
PMIX_CLASS_INSTANCE(pmix_dmdx_request_t,
                    pmix_list_item_t,
                    dmrqcon, dmrqdes);

static void lmcon(pmix_dmdx_local_t *p)
{
    memset(&p->proc, 0, sizeof(pmix_proc_t));
    PMIX_CONSTRUCT(&p->loc_reqs, pmix_list_t);
    p->info = NULL;
    p->ninfo = 0;
}
static void lmdes(pmix_dmdx_local_t *p)
{
    if (NULL != p->info) {
        PMIX_INFO_FREE(p->info, p->ninfo);
    }
    PMIX_LIST_DESTRUCT(&p->loc_reqs);
}
PMIX_CLASS_INSTANCE(pmix_dmdx_local_t,
                    pmix_list_item_t,
                    lmcon, lmdes);

static void prevcon(pmix_peer_events_info_t *p)
{
    p->peer = NULL;
    /* PMIX_NEW does not zero the allocation, and the default-handler
     * registration path never assigns this one */
    p->enviro_events = false;
    p->affected = NULL;
    p->naffected = 0;
}
static void prevdes(pmix_peer_events_info_t *p)
{
    if (NULL != p->peer) {
        PMIX_RELEASE(p->peer);
    }
    if (NULL != p->affected) {
        PMIX_PROC_FREE(p->affected, p->naffected);
    }
}
PMIX_CLASS_INSTANCE(pmix_peer_events_info_t,
                    pmix_list_item_t,
                    prevcon, prevdes);

static void regcon(pmix_regevents_info_t *p)
{
    PMIX_CONSTRUCT(&p->peers, pmix_list_t);
    p->code = PMIX_MAX_ERR_CONSTANT;
    p->nmine = 0;
    p->active = false;
}
static void regdes(pmix_regevents_info_t *p)
{
    PMIX_LIST_DESTRUCT(&p->peers);
}
PMIX_CLASS_INSTANCE(pmix_regevents_info_t,
                    pmix_list_item_t,
                    regcon, regdes);

static void iocon(pmix_iof_cache_t *p)
{
    p->bo = NULL;
    p->info = NULL;
    p->ninfo = 0;
}
static void iodes(pmix_iof_cache_t *p)
{
    PMIX_BYTE_OBJECT_FREE(p->bo, 1); // macro protects against NULL
    if (NULL != p->info) {
        PMIX_INFO_FREE(p->info, p->ninfo);
    }
}
PMIX_CLASS_INSTANCE(pmix_iof_cache_t,
                    pmix_list_item_t,
                    iocon, iodes);

static void pscon(pmix_pset_t *p)
{
    p->name = NULL;
    p->members = NULL;
    p->nmembers = 0;
}
static void psdes(pmix_pset_t *p)
{
    if (NULL != p->name) {
        free(p->name);
    }
    if (NULL != p->members) {
        free(p->members);
    }
}
PMIX_CLASS_INSTANCE(pmix_pset_t,
                    pmix_list_item_t,
                    pscon, psdes);
