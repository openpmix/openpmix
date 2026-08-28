/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2014-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2014-2015 Artem Y. Polyakov <artpol84@gmail.com>.
 *                         All rights reserved.
 * Copyright (c) 2016      IBM Corporation.  All rights reserved.
 * Copyright (c) 2019      Mellanox Technologies, Inc.
 *                         All rights reserved.
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
 * Copyright (c) 2023      Triad National Security, LLC. All rights reserved.
 * Copyright (c) 2026      Jeff Squyres  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/* THIS FILE IS INCLUDED SOLELY TO INSTANTIATE AND INIT/FINALIZE THE GLOBAL CLASSES */

#include "src/include/pmix_config.h"

#include "pmix_common.h"
#include "src/include/pmix_socket_errno.h"
#include "src/include/pmix_stdint.h"
#include "src/include/pmix_types.h"
#include "src/include/pmix_dictionary.h"

#include "src/include/pmix_globals.h"

#ifdef HAVE_STRING_H
#    include <string.h>
#endif
#include <fcntl.h>
#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif
#ifdef HAVE_SYS_TYPES_H
#    include <sys/types.h>
#endif
#include <ctype.h>
#include <event.h>
#if HAVE_SYS_STAT_H
#    include <sys/stat.h>
#endif /* HAVE_SYS_STAT_H */
#ifdef HAVE_DIRENT_H
#    include <dirent.h>
#endif /* HAVE_DIRENT_H */

#include "pmix_common.h"

#include "src/class/pmix_hash_table.h"
#include "src/class/pmix_list.h"
#include "src/common/pmix_iof.h"
#include "src/mca/bfrops/bfrops_types.h"
#include "src/mca/bfrops/base/bfrop_base_tma.h"
#include "src/threads/pmix_threads.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_os_path.h"

const char* PMIX_PROXY_VERSION = PMIX_PROXY_VERSION_STRING;
const char* PMIX_PROXY_BUGREPORT = PMIX_PROXY_BUGREPORT_STRING;

static void dirpath_destroy(char *path, pmix_cleanup_dir_t *cd,
                            pmix_epilog_t *epi);
static bool dirpath_is_empty(const char *path);

static void nsenvcon(pmix_nspace_env_cache_t *p)
{
    /* PMIX_NEW mallocs rather than callocs, so this is not redundant: the
     * destructor below releases this reference, and it has to be able to
     * tell "never assigned" from "assigned" */
    p->ns = NULL;
    PMIX_CONSTRUCT(&p->envars, pmix_list_t);
}
static void nsenvdes(pmix_nspace_env_cache_t *p)
{
    PMIX_LIST_DESTRUCT(&p->envars);
    /* whoever hands this object a namespace retains it for us (pnet and
     * pgpu both do), so the reference is ours to give back - without this
     * the namespace object outlived the server's own release of it, and
     * every job leaked one */
    if (NULL != p->ns) {
        PMIX_RELEASE(p->ns);
    }
}
PMIX_EXPORT PMIX_CLASS_INSTANCE(pmix_nspace_env_cache_t,
                                pmix_list_item_t,
                                nsenvcon, nsenvdes);

static void encon(pmix_envar_list_item_t *p)
{
    PMIX_ENVAR_CONSTRUCT(&p->envar);
}
static void endes(pmix_envar_list_item_t *p)
{
    PMIX_ENVAR_DESTRUCT(&p->envar);
}
PMIX_EXPORT PMIX_CLASS_INSTANCE(pmix_envar_list_item_t,
                                pmix_list_item_t,
                                encon, endes);


PMIX_EXPORT PMIX_CLASS_INSTANCE(pmix_namelist_t,
                                pmix_list_item_t,
                                NULL, NULL);

PMIX_EXPORT PMIX_CLASS_INSTANCE(pmix_proclist_t,
                                pmix_list_item_t,
                                NULL, NULL);

PMIX_EXPORT PMIX_CLASS_INSTANCE(pmix_topo_obj_t,
                                pmix_object_t,
                                NULL, NULL);

static void cfcon(pmix_cleanup_file_t *p)
{
    p->path = NULL;
}
static void cfdes(pmix_cleanup_file_t *p)
{
    if (NULL != p->path) {
        free(p->path);
    }
}
PMIX_EXPORT PMIX_CLASS_INSTANCE(pmix_cleanup_file_t, pmix_list_item_t, cfcon, cfdes);

static void cdcon(pmix_cleanup_dir_t *p)
{
    p->path = NULL;
    p->recurse = false;
    p->leave_topdir = false;
}
static void cddes(pmix_cleanup_dir_t *p)
{
    if (NULL != p->path) {
        free(p->path);
    }
}
PMIX_EXPORT PMIX_CLASS_INSTANCE(pmix_cleanup_dir_t, pmix_list_item_t, cdcon, cddes);

static void nscon(pmix_namespace_t *p)
{
    p->nspace = NULL;
    memset(&p->version, 0, sizeof(p->version));
    p->nprocs = 0;
    p->nlocalprocs = SIZE_MAX;
    p->all_registered = false;
    p->version_stored = false;
    p->job_info_recvd = false;
    p->jobbkt = NULL;
    p->ndelivered = 0;
    p->nfinalized = 0;
    p->local_app_fini_fired = false;
    PMIX_CONSTRUCT(&p->ranks, pmix_list_t);
    memset(&p->compat, 0, sizeof(p->compat));
    PMIX_CONSTRUCT(&p->epilog.cleanup_dirs, pmix_list_t);
    PMIX_CONSTRUCT(&p->epilog.cleanup_files, pmix_list_t);
    PMIX_CONSTRUCT(&p->epilog.ignores, pmix_list_t);
    PMIX_CONSTRUCT(&p->setup_data, pmix_list_t);
    PMIX_CONSTRUCT(&p->departed, pmix_list_t);
    pmix_iof_init_flags(&p->iof_flags);
    PMIX_CONSTRUCT(&p->sinks, pmix_list_t);

}
static void nsdes(pmix_namespace_t *p)
{
    if (NULL != p->nspace) {
        free(p->nspace);
    }
    if (NULL != p->jobbkt) {
        PMIX_RELEASE(p->jobbkt);
    }
    PMIX_LIST_DESTRUCT(&p->ranks);
    PMIX_LIST_DESTRUCT(&p->departed);
    /* perform any epilog */
    pmix_execute_epilog(&p->epilog);
    /* cleanup the epilog */
    PMIX_LIST_DESTRUCT(&p->epilog.cleanup_dirs);
    PMIX_LIST_DESTRUCT(&p->epilog.cleanup_files);
    PMIX_LIST_DESTRUCT(&p->epilog.ignores);
    PMIX_LIST_DESTRUCT(&p->setup_data);
    if (NULL != p->iof_flags.file) {
        free(p->iof_flags.file);
    }
    if (NULL != p->iof_flags.directory) {
        free(p->iof_flags.directory);
    }
    PMIX_LIST_DESTRUCT(&p->sinks);
}
PMIX_EXPORT PMIX_CLASS_INSTANCE(pmix_namespace_t,
                                pmix_list_item_t,
                                nscon, nsdes);

static void ncdcon(pmix_nspace_caddy_t *p)
{
    p->ns = NULL;
}
static void ncddes(pmix_nspace_caddy_t *p)
{
    if (NULL != p->ns) {
        PMIX_RELEASE(p->ns);
    }
}
PMIX_EXPORT PMIX_CLASS_INSTANCE(pmix_nspace_caddy_t,
                                pmix_list_item_t,
                                ncdcon, ncddes);

static void keyindex_construct(pmix_keyindex_t *ki)
{
    pmix_tma_t *const tma = pmix_obj_get_tma(&ki->super);

    ki->table = PMIX_NEW(pmix_pointer_array_t, tma);
    pmix_pointer_array_init(ki->table, PMIX_KEYINDEX_TABLE_SIZE, INT_MAX,
                            PMIX_KEYINDEX_TABLE_BLOCK);

    /* the string -> entry side. Sized past the reserved dictionary so
     * the common case never rehashes. */
    ki->lookup = PMIX_NEW(pmix_hash_table_t, tma);
    pmix_hash_table_init(ki->lookup, PMIX_KEYINDEX_LOOKUP_SIZE);

    /* Non-reserved keys are numbered from the top of the reserved range
     * upward, so an id says which half it came from. That matters for a
     * key index living in a gds/shmem3 segment: it carries only the
     * non-reserved keys, because the reserved ones already have ids
     * every process agrees on, and lookup_key() tells the two apart by
     * comparing against PMIX_INDEX_BOUNDARY. Matches
     * PMIX_KEYINDEX_STATIC_INIT. */
    ki->next_id = PMIX_INDEX_BOUNDARY;
}

size_t pmix_keyindex_sizeof_fixed_storage(void)
{
    /* Mirror keyindex_construct() above: the object, its pointer array and
     * that array's slots and free-bit map, its lookup table and that
     * table's elements. Both children are built at a fixed capacity
     * whatever the key count turns out to be, so this is the whole cost of
     * an empty key index - and it is not small, which is exactly why a
     * caller reserving space for one has to be told rather than guess. */
    const size_t bits_per_word = 8 * sizeof(uint64_t);

    return sizeof(pmix_keyindex_t)
           + sizeof(pmix_pointer_array_t)
           + PMIX_KEYINDEX_TABLE_SIZE * sizeof(void *)
           + ((PMIX_KEYINDEX_TABLE_SIZE + bits_per_word - 1) / bits_per_word)
                 * sizeof(uint64_t)
           + sizeof(pmix_hash_table_t)
           + pmix_hash_table_sizeof_storage(PMIX_KEYINDEX_LOOKUP_SIZE);
}

static void keyindex_destruct(pmix_keyindex_t *ki)
{
    pmix_tma_t *const tma = pmix_obj_get_tma(&ki->super);

    for (int i = 0; i < ki->table->size; i++) {
        pmix_regattr_input_t *p = (pmix_regattr_input_t *)pmix_pointer_array_get_item(ki->table, i);
        if (NULL != p) {
            if (NULL != p->name) {
                pmix_tma_free(tma, p->name);
            }
            if (NULL != p->string) {
                pmix_tma_free(tma, p->string);
            }
            if (NULL != p->description) {
                pmix_bfrops_base_tma_argv_free(p->description, tma);
            }
            pmix_tma_free(tma, p);
        }
    }
    /* the lookup table holds borrowed pointers to the entries just
     * freed above, so it only needs its own storage released */
    if (NULL != ki->lookup) {
        PMIX_RELEASE(ki->lookup);
    }
    PMIX_RELEASE(ki->table);
}

PMIX_EXPORT PMIX_CLASS_INSTANCE(pmix_keyindex_t,
                                pmix_object_t,
                                keyindex_construct, keyindex_destruct);

static void info_con(pmix_rank_info_t *info)
{
    info->peerid = -1;
    info->pid = -1;
    info->realuid = 0;
    info->uid = 0;
    info->realgid = 0;
    info->gid = 0;
    info->pname.nspace = NULL;
    info->pname.rank = PMIX_RANK_UNDEF;
    info->modex_recvd = false;
    info->proc_cnt = 0;
    info->server_object = NULL;
    PMIX_CONSTRUCT(&info->pending_modex, pmix_list_t);
    PMIX_CONSTRUCT(&info->pending_deletes, pmix_list_t);
    info->modex_sig = 0;
    info->modex_contributed = false;
}
static void info_des(pmix_rank_info_t *info)
{
    if (NULL != info->pname.nspace) {
        free(info->pname.nspace);
    }
    PMIX_LIST_DESTRUCT(&info->pending_modex);
    PMIX_LIST_DESTRUCT(&info->pending_deletes);
}
PMIX_EXPORT PMIX_CLASS_INSTANCE(pmix_rank_info_t,
                                pmix_list_item_t,
                                info_con, info_des);

static void pcon(pmix_peer_t *p)
{
    p->nptr = NULL;
    p->gds = NULL;
    p->proc_type.type = PMIX_PROC_UNDEF;
    p->proc_type.major = PMIX_MAJOR_WILDCARD;
    p->proc_type.minor = PMIX_MINOR_WILDCARD;
    p->proc_type.release = PMIX_RELEASE_WILDCARD;
    p->proc_type.flag = 0;
    p->protocol = PMIX_PROTOCOL_UNDEF;
    p->finalized = false;
    p->stdin_producer = false;
    p->info = NULL;
    p->proc_cnt = 0;
    p->index = 0;
    p->sd = -1;
    p->send_ev_active = false;
    p->recv_ev_active = false;
    PMIX_CONSTRUCT(&p->send_queue, pmix_list_t);
    p->send_msg = NULL;
    p->recv_msg = NULL;
    p->commit_cnt = 0;
    PMIX_CONSTRUCT(&p->epilog.cleanup_dirs, pmix_list_t);
    PMIX_CONSTRUCT(&p->epilog.cleanup_files, pmix_list_t);
    PMIX_CONSTRUCT(&p->epilog.ignores, pmix_list_t);
    p->dyn_tags_start = PMIX_PTL_TAG_DYNAMIC;
    p->dyn_tags_current = PMIX_PTL_TAG_DYNAMIC;
    p->dyn_tags_end = UINT32_MAX;
}

static void pdes(pmix_peer_t *p)
{
    if (NULL != p->nptr) {
        PMIX_RELEASE(p->nptr);
    }
    if (0 <= p->sd) {
        CLOSE_THE_SOCKET(p->sd);
    }
    /* This is the destructor that runs with no event base left - a role
     * releases its peers after pmix_rte_finalize() has freed it - so the
     * deletes below are the ones pmix_event_del_checked() exists for.
     * The flags are cleared either way, so the state a released peer
     * leaves behind says what is true of it. */
    if (p->send_ev_active) {
        pmix_event_del(&p->send_event);
    }
    if (p->recv_ev_active) {
        pmix_event_del(&p->recv_event);
    }
    p->send_ev_active = false;
    p->recv_ev_active = false;

    if (NULL != p->info) {
        PMIX_RELEASE(p->info);
    }

    PMIX_LIST_DESTRUCT(&p->send_queue);
    if (NULL != p->send_msg) {
        PMIX_RELEASE(p->send_msg);
    }
    if (NULL != p->recv_msg) {
        PMIX_RELEASE(p->recv_msg);
    }
    /* perform any epilog */
    pmix_execute_epilog(&p->epilog);
    /* cleanup the epilog */
    PMIX_LIST_DESTRUCT(&p->epilog.cleanup_dirs);
    PMIX_LIST_DESTRUCT(&p->epilog.cleanup_files);
    PMIX_LIST_DESTRUCT(&p->epilog.ignores);
}
PMIX_EXPORT PMIX_CLASS_INSTANCE(pmix_peer_t,
                                pmix_object_t,
                                pcon, pdes);

static void iofreqcon(pmix_iof_req_t *p)
{
    PMIX_CONSTRUCT_LOCK(&p->lock);
    p->status = PMIX_SUCCESS;
    p->requestor = NULL;
    p->local_id = 0;
    p->remote_id = 0;
    p->procs = NULL;
    p->nprocs = 0;
    pmix_iof_init_flags(&p->flags);
    p->channels = PMIX_FWD_NO_CHANNELS;
    p->directives = NULL;
    p->ndirs = 0;
    p->cbfunc = NULL;
    p->regcbfunc = NULL;
    p->cbdata = NULL;
}
static void iofreqdes(pmix_iof_req_t *p)
{
    PMIX_DESTRUCT_LOCK(&p->lock);
    if (NULL != p->requestor) {
        PMIX_RELEASE(p->requestor);
    }
    if (0 < p->nprocs) {
        PMIX_PROC_FREE(p->procs, p->nprocs);
    }
}
PMIX_EXPORT PMIX_CLASS_INSTANCE(pmix_iof_req_t, pmix_object_t, iofreqcon, iofreqdes);

static void scon(pmix_shift_caddy_t *p)
{
    PMIX_CONSTRUCT_LOCK(&p->lock);
    /* objects are malloc'd, not calloc'd, so every field must be
     * given a value here - a caddy whose status is never explicitly
     * set is otherwise reported to the caller as random garbage */
    p->status = PMIX_SUCCESS;
    p->codes = NULL;
    p->ncodes = 0;
    p->sessionid = UINT32_MAX;
    p->peer = NULL;
    p->proc = NULL;
    p->pname.nspace = NULL;
    p->pname.rank = PMIX_RANK_UNDEF;
    p->data = NULL;
    p->ndata = 0;
    p->key = NULL;
    p->infocopy = false;
    p->info = NULL;
    p->ninfo = 0;
    p->dircopy = false;
    p->directives = NULL;
    p->ndirs = 0;
    p->pdata = NULL;
    p->npdata = 0;
    p->range = PMIX_RANGE_UNDEF;
    p->bo = NULL;
    p->dist = NULL;
    p->ndist = 0;
    p->evhdlr = NULL;
    p->iofreq = NULL;
    p->kv = NULL;
    p->vptr = NULL;
    p->cd = NULL;
    p->tracker = NULL;
    p->enviro = false;
    p->cbfunc.relfn = NULL;
    p->cbdata = NULL;
    p->relcbdata = NULL;
    p->ref = 0;
}
static void scdes(pmix_shift_caddy_t *p)
{
    PMIX_DESTRUCT_LOCK(&p->lock);
    if (NULL != p->key) {
        free((char *) p->key);
    }
    if (NULL != p->peer) {
        PMIX_RELEASE(p->peer);
    }
    if (NULL != p->pname.nspace) {
        free(p->pname.nspace);
    }
    if (p->infocopy) {
        PMIX_INFO_FREE(p->info, p->ninfo);
    }
    if (p->dircopy) {
        PMIX_INFO_FREE(p->directives, p->ndirs);
    }
    if (NULL != p->kv) {
        PMIX_RELEASE(p->kv);
    }
}
PMIX_EXPORT PMIX_CLASS_INSTANCE(pmix_shift_caddy_t,
                                pmix_object_t,
                                scon, scdes);

static void lgcon(pmix_get_logic_t *p)
{
    memset(&p->p, 0, sizeof(pmix_proc_t));
    p->pntrval = false;
    p->stval = false;
    p->optional = false;
    p->immediate = false;
    p->refresh_cache = false;
    p->scope = PMIX_SCOPE_UNDEF;
    p->sessioninfo = false;
    p->sessiondirective = false;
    p->sessionid = UINT32_MAX;
    p->nodeinfo = false;
    p->nodedirective = false;
    p->hostname = NULL;
    p->nodeid = UINT32_MAX;
    p->appinfo = false;
    p->appdirective = false;
    p->appnum = UINT32_MAX;
}
static void lgdes(pmix_get_logic_t *p)
{
    if (NULL != p->hostname) {
        free(p->hostname);
        p->hostname = NULL;
    }
}
PMIX_EXPORT PMIX_CLASS_INSTANCE(pmix_get_logic_t,
                                pmix_object_t,
                                lgcon, lgdes);

static void cbcon(pmix_cb_t *p)
{
    PMIX_CONSTRUCT_LOCK(&p->lock);
    p->status = PMIX_SUCCESS;
    p->checked = false;
    PMIX_CONSTRUCT(&p->data, pmix_buffer_t);
    p->cbfunc.ptlfn = NULL;
    p->cbdata = NULL;
    p->pname.nspace = NULL;
    p->pname.rank = PMIX_RANK_UNDEF;
    p->scope = PMIX_SCOPE_UNDEF;
    p->key = NULL;
    p->value = NULL;
    p->proc = NULL;
    p->procs = NULL;
    p->nprocs = 0;
    p->info = NULL;
    p->ninfo = 0;
    p->directives = NULL;
    p->ndirs = 0;
    p->dist = NULL;
    p->bo = NULL;
    p->infocopy = false;
    p->dircopy = false;
    p->nvals = 0;
    PMIX_CONSTRUCT(&p->kvs, pmix_list_t);
    p->copy = false;
    p->lg = NULL;
    p->timer_running = false;
    p->fabric = NULL;
    p->topo = NULL;
}
static void cbdes(pmix_cb_t *p)
{
    /* cbcon() constructs this lock unconditionally, so the destructor
     * has to take it down unconditionally - every sibling caddy class
     * in this file does. pthread_mutex_init/pthread_cond_init are
     * allowed to allocate, and on the implementations that do, a
     * pmix_cb_t is the worst possible place to leak: there is one per
     * PMIx_Get, PMIx_Put, PMIx_Commit, PMIx_Fence and query. */
    PMIX_DESTRUCT_LOCK(&p->lock);
    if (p->timer_running) {
        pmix_event_del(&p->ev);
    }
    if (NULL != p->pname.nspace) {
        free(p->pname.nspace);
    }
    PMIX_DESTRUCT(&p->data);
    if (p->infocopy) {
        PMIX_INFO_FREE(p->info, p->ninfo);
    }
    /* the directives are borrowed from the caller in every path that
     * simply passes an API's argument down, so only an owner that says
     * so gets them freed - see pmix_server_monitor, which unpacks them
     * off the wire and therefore owns them */
    if (p->dircopy) {
        PMIX_INFO_FREE(p->directives, p->ndirs);
    }
    PMIX_LIST_DESTRUCT(&p->kvs);
}
PMIX_EXPORT PMIX_CLASS_INSTANCE(pmix_cb_t,
                                pmix_list_item_t,
                                cbcon, cbdes);

PMIX_EXPORT PMIX_CLASS_INSTANCE(pmix_info_caddy_t,
                                pmix_list_item_t,
                                NULL, NULL);

PMIX_EXPORT PMIX_CLASS_INSTANCE(pmix_peerlist_t,
                                pmix_list_item_t,
                                NULL, NULL);

static void ifcon(pmix_infolist_t *p)
{
    PMIX_INFO_CONSTRUCT(&p->info);
}
static void ifdes(pmix_infolist_t *p)
{
    PMIX_INFO_DESTRUCT(&p->info);
}
PMIX_EXPORT PMIX_CLASS_INSTANCE(pmix_infolist_t,
                                pmix_list_item_t,
                                ifcon, ifdes);

static void qlcon(pmix_querylist_t *p)
{
    PMIX_QUERY_CONSTRUCT(&p->query);
}
static void qldes(pmix_querylist_t *p)
{
    PMIX_QUERY_DESTRUCT(&p->query);
}
PMIX_EXPORT PMIX_CLASS_INSTANCE(pmix_querylist_t,
                                pmix_list_item_t,
                                qlcon, qldes);

static void qcon(pmix_query_caddy_t *p)
{
    PMIX_CONSTRUCT_LOCK(&p->lock);
    /* objects are malloc'd, not calloc'd, so every field must be
     * given a value here - see the note in scon() */
    p->status = PMIX_SUCCESS;
    p->host_called = false;
    p->queries = NULL;
    p->nqueries = 0;
    p->targets = NULL;
    p->ntargets = 0;
    p->info = NULL;
    p->ninfo = 0;
    p->dirs = NULL;
    p->ndirs = 0;
    PMIX_BYTE_OBJECT_CONSTRUCT(&p->bo);
    PMIX_CONSTRUCT(&p->results, pmix_list_t);
    p->nreplies = 0;
    p->nrequests = 0;
    p->cbfunc = NULL;
    p->valcbfunc = NULL;
    p->cbdata = NULL;
    p->relcbfunc = NULL;
    p->credcbfunc = NULL;
    p->validcbfunc = NULL;
}
static void qdes(pmix_query_caddy_t *p)
{
    PMIX_DESTRUCT_LOCK(&p->lock);
    PMIX_BYTE_OBJECT_DESTRUCT(&p->bo);
    PMIX_QUERY_FREE(p->queries, p->nqueries);
    PMIX_PROC_FREE(p->targets, p->ntargets);
    PMIX_INFO_FREE(p->info, p->ninfo);
    PMIX_LIST_DESTRUCT(&p->results);
}
PMIX_EXPORT PMIX_CLASS_INSTANCE(pmix_query_caddy_t,
                                pmix_object_t,
                                qcon, qdes);

static void ncon(pmix_notify_caddy_t *p)
{
    PMIX_CONSTRUCT_LOCK(&p->lock);
#if defined(__linux__) && PMIX_HAVE_CLOCK_GETTIME
    struct timespec tp;
    (void) clock_gettime(CLOCK_MONOTONIC, &tp);
    p->ts = tp.tv_sec;
#else
    /* Fall back to gettimeofday() if we have nothing else */
    struct timeval tv;
    gettimeofday(&tv, NULL);
    p->ts = tv.tv_sec;
#endif
    p->room = -1;
    memset(p->source.nspace, 0, PMIX_MAX_NSLEN + 1);
    p->source.rank = PMIX_RANK_UNDEF;
    p->range = PMIX_RANGE_UNDEF;
    p->staylocal = false;
    p->targets = NULL;
    p->ntargets = 0;
    p->nleft = SIZE_MAX;
    p->notified = NULL;
    p->nnotified = 0;
    p->affected = NULL;
    p->naffected = 0;
    p->nondefault = false;
    p->info = NULL;
    p->ninfo = 0;
}
static void ndes(pmix_notify_caddy_t *p)
{
    PMIX_DESTRUCT_LOCK(&p->lock);
    if (NULL != p->info) {
        PMIX_INFO_FREE(p->info, p->ninfo);
    }
    PMIX_PROC_FREE(p->affected, p->naffected);
    if (NULL != p->targets) {
        free(p->targets);
    }
    if (NULL != p->notified) {
        free(p->notified);
    }
}
PMIX_CLASS_INSTANCE(pmix_notify_caddy_t,
                    pmix_object_t,
                    ncon, ndes);

pmix_dstor_t *pmix_dstor_new_tma(uint32_t index,
                                 pmix_tma_t *tma)
{
    pmix_dstor_t *d = (pmix_dstor_t *)pmix_tma_malloc(tma, sizeof(pmix_dstor_t));
    if (PMIX_LIKELY(NULL != d)) {
        d->index = index;
        d->qualindex = UINT32_MAX;
        d->value = NULL;
    }
    return d;
}

void pmix_dstor_release_tma(pmix_dstor_t *d,
                            pmix_tma_t *tma)
{
    if (NULL != d->value) {
        pmix_bfrops_base_tma_value_destruct(d->value, tma);
        pmix_tma_free(tma, d->value);
    }
    pmix_tma_free(tma, d);
}

static void grcon(pmix_group_t *p)
{
    p->grpid = NULL;
    p->ctxid = SIZE_MAX;
    p->members = NULL;
    p->nmbrs = 0;
    p->notterm = false;
}
static void grdes(pmix_group_t *p)
{
    if (NULL != p->grpid) {
        free(p->grpid);
        p->grpid = NULL;
    }
    p->ctxid = SIZE_MAX;
    if (NULL != p->members) {
        PMIX_PROC_FREE(p->members, p->nmbrs);
        p->members = NULL;
    }
}
PMIX_CLASS_INSTANCE(pmix_group_t,
                    pmix_list_item_t,
                    grcon, grdes);

/* Is this path one the client asked us to leave alone? dirpath_destroy
 * asks the same question of the directory it is descending and of every
 * file inside it; the cleanup_files loop below did not ask it at all,
 * so an ignore naming a path already registered for cleanup was
 * recorded and then had no effect - the epilog went on unlinking a file
 * the client had asked it to keep. pmix_server_job_ctrl refuses the
 * reverse order with PMIX_ERR_CONFLICTING_CLEANUP_DIRECTIVES, which is
 * what says the two lists are meant to be exclusive; an ignore arriving
 * second is the one way a path can reach both, and it is the ignore
 * that has to win. */
static bool epilog_ignored(pmix_epilog_t *epi, const char *path)
{
    pmix_cleanup_file_t *cf;

    PMIX_LIST_FOREACH (cf, &epi->ignores, pmix_cleanup_file_t) {
        if (0 == strcmp(cf->path, path)) {
            return true;
        }
    }
    return false;
}

void pmix_execute_epilog(pmix_epilog_t *epi)
{
    pmix_cleanup_file_t *cf, *cfnext;
    pmix_cleanup_dir_t *cd, *cdnext;
    DIR *tst;
    int rc;

    /* Every entry on these two lists carries exactly one path.
     * pmix_server_job_ctrl is the only thing in the tree that builds
     * one, and it expands the comma-delimited value the client sent
     * before anything is compared against it - which is what makes the
     * ignore test here, and the top-directory test in dirpath_destroy,
     * whole-path comparisons that can actually match. Do not re-split
     * here: a second expansion point is how those comparisons came to be
     * made against a string no constructed path ever equals. */

    /* start with any specified files */
    PMIX_LIST_FOREACH_SAFE (cf, cfnext, &epi->cleanup_files, pmix_cleanup_file_t) {
        if (!epilog_ignored(epi, cf->path)) {
            rc = unlink(cf->path);
            if (0 > rc) {
                pmix_output_verbose(10, pmix_globals.debug_output, "File %s failed to unlink: %s",
                                    cf->path, strerror(errno));
            }
        }
        pmix_list_remove_item(&epi->cleanup_files, &cf->super);
        PMIX_RELEASE(cf);
    }

    /* now cleanup the directories */
    PMIX_LIST_FOREACH_SAFE (cd, cdnext, &epi->cleanup_dirs, pmix_cleanup_dir_t) {
        tst = opendir(cd->path);
        if (NULL != tst) {
            closedir(tst);
            dirpath_destroy(cd->path, cd, epi);
        }
        pmix_list_remove_item(&epi->cleanup_dirs, &cd->super);
        PMIX_RELEASE(cd);
    }
}

static void dirpath_destroy(char *path, pmix_cleanup_dir_t *cd, pmix_epilog_t *epi)
{
    DIR *dp, *tst;
    struct dirent *ep;
    char *filenm;

    if (NULL == path) { /* protect against error */
        return;
    }

    /* if this path is to be ignored, then do so */
    if (epilog_ignored(epi, path)) {
        return;
    }

    /* Open up the directory */
    dp = opendir(path);
    if (NULL == dp) {
        return;
    }

    while (NULL != (ep = readdir(dp))) {
        /* skip:
         *  - . and ..
         */
        if ((0 == strcmp(ep->d_name, ".")) || (0 == strcmp(ep->d_name, ".."))) {
            continue;
        }

        /* Create a pathname.  This is not always needed, but it makes
         * for cleaner code just to create it here.  Note that we are
         * allocating memory here, so we need to free it later on.
         */
        filenm = pmix_os_path(false, path, ep->d_name, NULL);
        if (NULL == filenm) {
            /* the assembled name is longer than PMIX_PATH_MAX, or we are
             * out of memory - either way we cannot name this entry, and
             * the ignore-list comparison below would hand the NULL to
             * strcmp() */
            continue;
        }

        /* if this path is to be ignored, then do so */
        if (epilog_ignored(epi, filenm)) {
            free(filenm);
            continue;
        }

        /* Check to see if it is a directory */
        tst = opendir(filenm);
        if (NULL != tst) {
            closedir(tst);
            /*
             * If not recursively descending, then if we find a directory then fail
             * since we were not told to remove it.
             */
            if (!cd->recurse) {
                /* continue removing files */
                free(filenm);
                continue;
            } else {
                /* Directories are recursively destroyed */
                dirpath_destroy(filenm, cd, epi);
                free(filenm);
            }
        } else {
            /* Files are removed right here */
            unlink(filenm);
            free(filenm);
        }
    }

    /* Done with this directory */
    closedir(dp);

    /* If the directory is empty, then remove it unless we
     * were told to leave it */
    if (0 == strcmp(path, cd->path) && cd->leave_topdir) {
        return;
    }
    if (dirpath_is_empty(path)) {
        rmdir(path);
    }
}

static bool dirpath_is_empty(const char *path)
{
    DIR *dp;
    struct dirent *ep;

    if (NULL != path) { /* protect against error */
        dp = opendir(path);
        if (NULL != dp) {
            while ((ep = readdir(dp))) {
                if ((0 != strcmp(ep->d_name, ".")) && (0 != strcmp(ep->d_name, ".."))) {
                    closedir(dp);
                    return false;
                }
            }
            closedir(dp);
            return true;
        }
        return false;
    }

    return true;
}

int pmix_event_assign(struct event *ev, pmix_event_base_t *evbase, int fd, short arg,
                      event_callback_fn cbfn, void *cbd)
{
    event_assign(ev, evbase, fd, arg, cbfn, cbd);
    return 0;
}

pmix_event_t *pmix_event_new(pmix_event_base_t *b, int fd, short fg, event_callback_fn cbfn,
                             void *cbd)
{
    pmix_event_t *ev = NULL;

    ev = event_new(b, fd, fg, (event_callback_fn) cbfn, cbd);
    return ev;
}

/* An object that owns an event normally dies before the base that event
 * is registered on, and then this is just event_del(). Some cannot: each
 * role releases its peers *after* pmix_rte_finalize(), because the
 * two-reference arrangement on mypeer described in
 * src/runtime/pmix_finalize.c requires it - and pmix_rte_finalize() is
 * what stops the progress thread and frees the base. So the peer
 * destructor below, and everything it reaches (its namespace, that
 * namespace's IOF sinks, and each sink's write event), can run with no
 * base left.
 *
 * There is nothing for them to delete by then: libevent tore every event
 * off the base when it freed it. What event_del() does instead is take
 * the base's lock, which is memory that has been handed back. Usually
 * the bytes there still read as an unheld mutex and nobody notices; when
 * they do not, the process blocks on a mutex no thread will ever release
 * and the only symptom is a hang with no output. That is a defect whose
 * appearance depends on what the allocator happens to reuse, so it comes
 * and goes with unrelated changes elsewhere.
 *
 * Rather than leave each such destructor to remember, decline here for
 * all of them: no base, nothing registered, nothing to delete.
 * pmix_rte_finalize() NULLs both pointers once the base is gone, and
 * they are NULL before the first init as well - a window in which no
 * event can have been added either. */
int pmix_event_del_checked(struct event *ev)
{
    if (NULL == pmix_globals.evbase && NULL == pmix_globals.evauxbase) {
        return 0;
    }
    return event_del(ev);
}

static void tcon(pmix_timer_t *p)
{
    p->active = false;
    p->payload = NULL;
}
static void tdes(pmix_timer_t *p)
{
    if (p->active) {
        pmix_event_del(&p->ev);
        p->active = false;
    }
}
PMIX_CLASS_INSTANCE(pmix_timer_t,
                    pmix_object_t,
                    tcon, tdes);

#if PMIX_PICKY_COMPILERS
void pmix_hide_unused_params(int x, ...)
{
    va_list ap;
    (void)x;
    va_start(ap, x);
    va_end(ap);
}
#endif
