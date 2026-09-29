/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/* The membership of the groups this server's clients belong to.
 *
 * A group is recorded when it forms: a synchronous construct when the host
 * answers it, an invited group when it completes - by the leader's server
 * as it announces the group, and by any other server with a member among
 * its clients when the host delivers the announcement. A member that leaves is
 * removed, a destruct drops the group, and so does the departure of the
 * last member that is a client here - noticed when that member leaves, or
 * when its job is deregistered.
 *
 * The record is what lets a client name a group - by its ID, with
 * PMIX_RANK_WILDCARD for every member or a group rank for one - in a
 * fence, connect, disconnect, group construct or destruct, or a get, and
 * have this server expand it. A client from v7.0 on keeps no membership of
 * its own when its server does this; see pmix_client_server_has_groups().
 *
 * Everything here runs on the progress thread. */

#include "src/include/pmix_config.h"

#include "src/include/pmix_stdint.h"

#include "pmix_common.h"

#include <string.h>

#include "src/class/pmix_list.h"
#include "src/include/pmix_globals.h"
#include "src/util/pmix_name_fns.h"

#include "pmix_server_ops.h"

static pmix_list_t groups;
static bool initialized = false;

static void grpmbr_init(void)
{
    if (!initialized) {
        PMIX_CONSTRUCT(&groups, pmix_list_t);
        initialized = true;
    }
}

/* PMIx_server_finalize: a later PMIx_server_init in the same process
 * starts with nothing recorded */
void pmix_server_grpmbr_finalize(void)
{
    if (!initialized) {
        return;
    }
    PMIX_LIST_DESTRUCT(&groups);
    initialized = false;
}

/* Byte-equal namespaces. PMIX_CHECK_NSPACE reads an empty name as a
 * wildcard, and an empty name never names a group */
static bool same_nspace(const char *a, const char *b)
{
    if (NULL == a || NULL == b) {
        return false;
    }
    return (0 == strncmp(a, b, PMIX_MAX_NSLEN));
}

static bool same_proc(const pmix_proc_t *a, const pmix_proc_t *b)
{
    return same_nspace(a->nspace, b->nspace) && a->rank == b->rank;
}

pmix_group_t *pmix_server_grp_find(const char *grpid)
{
    pmix_group_t *grp;

    if (!initialized || PMIx_Nspace_invalid(grpid)) {
        return NULL;
    }
    PMIX_LIST_FOREACH (grp, &groups, pmix_group_t) {
        if (same_nspace(grp->grpid, grpid)) {
            return grp;
        }
    }
    return NULL;
}

pmix_status_t pmix_server_grp_record(const char *grpid, const pmix_proc_t *members,
                                     size_t nmembers, size_t ctxid, bool notterm, bool sort)
{
    pmix_group_t *grp;
    pmix_proc_t *mbrs = NULL;

    if (PMIx_Nspace_invalid(grpid) || NULL == members || 0 == nmembers) {
        return PMIX_ERR_BAD_PARAM;
    }
    grpmbr_init();
    PMIX_PROC_CREATE(mbrs, nmembers);
    if (NULL == mbrs) {
        return PMIX_ERR_NOMEM;
    }
    memcpy(mbrs, members, nmembers * sizeof(pmix_proc_t));
    /* a synchronous construct keeps the host's order - every member is
     * handed the same array, and group ranks count across it in the order
     * PMIX_GROUP_MEMBERSHIP reports. An invited group has always been
     * recorded sorted by its members; keep that, so a group rank names the
     * same process for a client of any version */
    if (sort) {
        qsort(mbrs, nmembers, sizeof(pmix_proc_t), pmix_util_compare_proc);
    }

    grp = pmix_server_grp_find(grpid);
    if (NULL == grp) {
        grp = PMIX_NEW(pmix_group_t);
        if (NULL == grp) {
            PMIX_PROC_FREE(mbrs, nmembers);
            return PMIX_ERR_NOMEM;
        }
        grp->grpid = strdup(grpid);
        if (NULL == grp->grpid) {
            PMIX_RELEASE(grp);
            PMIX_PROC_FREE(mbrs, nmembers);
            return PMIX_ERR_NOMEM;
        }
        pmix_list_append(&groups, &grp->super);
    } else if (NULL != grp->members) {
        /* the same ID formed again - the new membership is the group */
        PMIX_PROC_FREE(grp->members, grp->nmbrs);
    }
    grp->members = mbrs;
    grp->nmbrs = nmembers;
    grp->ctxid = ctxid;
    grp->notterm = notterm;
    return PMIX_SUCCESS;
}

void pmix_server_grp_drop(const char *grpid)
{
    pmix_group_t *grp;

    grp = pmix_server_grp_find(grpid);
    if (NULL != grp) {
        pmix_list_remove_item(&groups, &grp->super);
        PMIX_RELEASE(grp);
    }
}

/* Is proc - or, for a wildcard rank, any process of its job - this server
 * itself or one of its connected clients? */
static bool is_local(const pmix_proc_t *proc)
{
    pmix_peer_t *peer;
    int n;

    if (same_nspace(proc->nspace, pmix_globals.myid.nspace) &&
        (PMIX_RANK_WILDCARD == proc->rank || proc->rank == pmix_globals.myid.rank)) {
        return true;
    }
    for (n = 0; n < pmix_server_globals.clients.size; n++) {
        peer = (pmix_peer_t *) pmix_pointer_array_get_item(&pmix_server_globals.clients, n);
        if (NULL == peer || NULL == peer->info || NULL == peer->info->pname.nspace) {
            continue;
        }
        if (same_nspace(peer->info->pname.nspace, proc->nspace) &&
            (PMIX_RANK_WILDCARD == proc->rank || peer->info->pname.rank == proc->rank)) {
            return true;
        }
    }
    return false;
}

static bool has_local_member(const pmix_group_t *grp)
{
    size_t n;

    for (n = 0; n < grp->nmbrs; n++) {
        if (is_local(&grp->members[n])) {
            return true;
        }
    }
    return false;
}

void pmix_server_grp_remove_member(const char *grpid, const pmix_proc_t *proc)
{
    pmix_group_t *grp;
    size_t m, k;

    grp = pmix_server_grp_find(grpid);
    if (NULL == grp || NULL == proc) {
        return;
    }
    for (m = 0; m < grp->nmbrs; m++) {
        if (same_proc(&grp->members[m], proc)) {
            for (k = m + 1; k < grp->nmbrs; k++) {
                memcpy(&grp->members[k - 1], &grp->members[k], sizeof(pmix_proc_t));
            }
            --grp->nmbrs;
            break;
        }
    }
    if (!has_local_member(grp)) {
        pmix_list_remove_item(&groups, &grp->super);
        PMIX_RELEASE(grp);
    }
}

void pmix_server_grp_sweep(void)
{
    pmix_group_t *grp, *gnext;

    if (!initialized) {
        return;
    }
    PMIX_LIST_FOREACH_SAFE (grp, gnext, &groups, pmix_group_t) {
        if (!has_local_member(grp)) {
            pmix_list_remove_item(&groups, &grp->super);
            PMIX_RELEASE(grp);
        }
    }
}

static pmix_status_t append_unique(pmix_list_t *list, const pmix_proc_t *proc)
{
    pmix_proclist_t *nm;

    PMIX_LIST_FOREACH (nm, list, pmix_proclist_t) {
        if (same_proc(&nm->proc, proc)) {
            return PMIX_SUCCESS;
        }
    }
    nm = PMIX_NEW(pmix_proclist_t);
    if (NULL == nm) {
        return PMIX_ERR_NOMEM;
    }
    memcpy(&nm->proc, proc, sizeof(pmix_proc_t));
    pmix_list_append(list, &nm->super);
    return PMIX_SUCCESS;
}

/* The member at group rank "rank". A member with a wildcard rank stands
 * for every process of its job, counted in rank order */
static pmix_status_t group_rank(const pmix_group_t *grp, pmix_rank_t rank, pmix_proc_t *proc)
{
    pmix_namespace_t *ns, *nptr;
    size_t i, cnt = 0;

    for (i = 0; i < grp->nmbrs; i++) {
        if (PMIX_RANK_WILDCARD != grp->members[i].rank) {
            if (cnt == rank) {
                memcpy(proc, &grp->members[i], sizeof(pmix_proc_t));
                return PMIX_SUCCESS;
            }
            ++cnt;
            continue;
        }
        nptr = NULL;
        PMIX_LIST_FOREACH (ns, &pmix_globals.nspaces, pmix_namespace_t) {
            if (same_nspace(ns->nspace, grp->members[i].nspace)) {
                nptr = ns;
                break;
            }
        }
        if (NULL == nptr || 0 == nptr->nprocs) {
            /* without the job's size there is no counting past it */
            return PMIX_ERR_NOT_FOUND;
        }
        if (cnt + nptr->nprocs > rank) {
            PMIX_LOAD_PROCID(proc, grp->members[i].nspace, rank - cnt);
            return PMIX_SUCCESS;
        }
        cnt += nptr->nprocs;
    }
    /* the group rank lies beyond the membership */
    return PMIX_ERR_NOT_FOUND;
}

pmix_status_t pmix_server_grp_expand(const pmix_proc_t *in, size_t nin, pmix_proc_t **out,
                                     size_t *nout)
{
    pmix_list_t cache;
    pmix_proclist_t *nm;
    pmix_group_t *grp;
    pmix_proc_t *procs = NULL, proc;
    pmix_status_t rc = PMIX_SUCCESS;
    size_t n, i, sz;

    *out = NULL;
    *nout = 0;
    PMIX_CONSTRUCT(&cache, pmix_list_t);
    for (n = 0; PMIX_SUCCESS == rc && n < nin; n++) {
        grp = pmix_server_grp_find(in[n].nspace);
        if (NULL == grp) {
            rc = append_unique(&cache, &in[n]);
        } else if (PMIX_RANK_WILDCARD == in[n].rank) {
            /* every member - and a group with none left names nobody,
             * which is reported rather than contributing no participant */
            if (0 == grp->nmbrs) {
                rc = PMIX_ERR_NOT_FOUND;
            }
            for (i = 0; PMIX_SUCCESS == rc && i < grp->nmbrs; i++) {
                rc = append_unique(&cache, &grp->members[i]);
            }
        } else {
            rc = group_rank(grp, in[n].rank, &proc);
            if (PMIX_SUCCESS == rc) {
                rc = append_unique(&cache, &proc);
            }
        }
    }
    if (PMIX_SUCCESS != rc) {
        PMIX_LIST_DESTRUCT(&cache);
        return rc;
    }
    sz = pmix_list_get_size(&cache);
    if (0 < sz) {
        PMIX_PROC_CREATE(procs, sz);
        if (NULL == procs) {
            PMIX_LIST_DESTRUCT(&cache);
            return PMIX_ERR_NOMEM;
        }
        n = 0;
        PMIX_LIST_FOREACH (nm, &cache, pmix_proclist_t) {
            memcpy(&procs[n], &nm->proc, sizeof(pmix_proc_t));
            ++n;
        }
    }
    PMIX_LIST_DESTRUCT(&cache);
    *out = procs;
    *nout = sz;
    return PMIX_SUCCESS;
}

pmix_status_t pmix_server_grp_expand_procs(pmix_proc_t **procs, size_t *nprocs)
{
    pmix_proc_t *out = NULL;
    size_t nout = 0, n;
    pmix_status_t rc;

    if (NULL == *procs || 0 == *nprocs || !initialized || 0 == pmix_list_get_size(&groups)) {
        return PMIX_SUCCESS;
    }
    /* This is on the path of every fence and connect, and the expansion
     * rebuilds the array - leave one that names no group as it came */
    for (n = 0; n < *nprocs; n++) {
        if (NULL != pmix_server_grp_find((*procs)[n].nspace)) {
            break;
        }
    }
    if (n == *nprocs) {
        return PMIX_SUCCESS;
    }
    rc = pmix_server_grp_expand(*procs, *nprocs, &out, &nout);
    if (PMIX_SUCCESS != rc) {
        return rc;
    }
    PMIX_PROC_FREE(*procs, *nprocs);
    *procs = out;
    *nprocs = nout;
    return PMIX_SUCCESS;
}

bool pmix_server_grp_is_participant(const pmix_peer_t *peer, const pmix_proc_t *procs,
                                    size_t nprocs)
{
    pmix_proc_t me;
    size_t n;

    if (NULL == peer || NULL == peer->info || NULL == peer->info->pname.nspace) {
        return false;
    }
    PMIX_LOAD_PROCID(&me, peer->info->pname.nspace, peer->info->pname.rank);
    /* a request the server makes itself is its host's */
    if (same_proc(&me, &pmix_globals.myid)) {
        return true;
    }
    for (n = 0; n < nprocs; n++) {
        /* the ranks that cover the caller are the ones the client library
         * accepts - see pmix_client_proc_is_included */
        if (same_nspace(procs[n].nspace, me.nspace) &&
            (PMIX_RANK_WILDCARD == procs[n].rank || PMIX_RANK_LOCAL_NODE == procs[n].rank ||
             PMIX_RANK_LOCAL_PEERS == procs[n].rank || procs[n].rank == me.rank)) {
            return true;
        }
    }
    return false;
}

pmix_status_t pmix_server_grp_others(const char *grpid, const pmix_proc_t *self,
                                     pmix_proc_t **out, size_t *nout)
{
    pmix_group_t *grp;
    pmix_proc_t *procs = NULL;
    size_t m, n = 0;

    *out = NULL;
    *nout = 0;
    grp = pmix_server_grp_find(grpid);
    if (NULL == grp) {
        return PMIX_ERR_NOT_FOUND;
    }
    if (0 < grp->nmbrs) {
        PMIX_PROC_CREATE(procs, grp->nmbrs);
        if (NULL == procs) {
            return PMIX_ERR_NOMEM;
        }
    }
    for (m = 0; m < grp->nmbrs; m++) {
        if (same_proc(&grp->members[m], self)) {
            continue;
        }
        memcpy(&procs[n], &grp->members[m], sizeof(pmix_proc_t));
        ++n;
    }
    if (0 == n && NULL != procs) {
        PMIX_PROC_FREE(procs, grp->nmbrs);
        procs = NULL;
    }
    *out = procs;
    *nout = n;
    return PMIX_SUCCESS;
}

void pmix_server_grp_host_event(pmix_status_t status, const pmix_proc_t *source,
                                const pmix_info_t *info, size_t ninfo)
{
    const char *grpid = NULL;
    const pmix_proc_t *members = NULL, *affected = NULL;
    size_t n, nmembers = 0, ctxid = SIZE_MAX;

    if (PMIX_GROUP_CONSTRUCT_COMPLETE != status && PMIX_GROUP_LEFT != status) {
        return;
    }
    /* check each type before reading the union - an event can be raised
     * by anyone */
    for (n = 0; n < ninfo; n++) {
        if (PMIX_CHECK_KEY(&info[n], PMIX_GROUP_ID)) {
            if (PMIX_STRING == info[n].value.type) {
                grpid = info[n].value.data.string;
            }
        } else if (PMIX_CHECK_KEY(&info[n], PMIX_GROUP_MEMBERSHIP)) {
            if (PMIX_DATA_ARRAY == info[n].value.type && NULL != info[n].value.data.darray &&
                PMIX_PROC == info[n].value.data.darray->type &&
                NULL != info[n].value.data.darray->array) {
                members = (const pmix_proc_t *) info[n].value.data.darray->array;
                nmembers = info[n].value.data.darray->size;
            }
        } else if (PMIX_CHECK_KEY(&info[n], PMIX_GROUP_CONTEXT_ID)) {
            if (PMIX_SUCCESS != PMIx_Value_get_number(&info[n].value, &ctxid, PMIX_SIZE)) {
                ctxid = SIZE_MAX;
            }
        } else if (PMIX_CHECK_KEY(&info[n], PMIX_EVENT_AFFECTED_PROC)) {
            if (PMIX_PROC == info[n].value.type) {
                affected = info[n].value.data.proc;
            }
        }
    }
    if (PMIx_Nspace_invalid(grpid) || NULL == source) {
        return;
    }

    if (PMIX_GROUP_LEFT == status) {
        /* a process leaves a group only for itself */
        if (NULL == affected) {
            affected = source;
        } else if (!same_proc(affected, source)) {
            return;
        }
        pmix_server_grp_remove_member(grpid, affected);
        return;
    }

    /* PMIX_GROUP_CONSTRUCT_COMPLETE: an invited group, announced by the
     * leader's server through the host - clients cannot raise it (see
     * server_only_group_event). Record it if any of its members is ours;
     * the latest announcement of an ID is the group */
    if (PMIX_GROUP_CONSTRUCT_COMPLETE != status || NULL == members) {
        return;
    }
    for (n = 0; n < nmembers; n++) {
        if (is_local(&members[n])) {
            (void) pmix_server_grp_record(grpid, members, nmembers, ctxid, false, true);
            return;
        }
    }
}
