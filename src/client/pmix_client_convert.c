/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2014-2021 Intel, Inc.  All rights reserved.
 * Copyright (c) 2014-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2014      Artem Y. Polyakov <artpol84@gmail.com>.
 *                         All rights reserved.
 * Copyright (c) 2016-2018 Mellanox Technologies, Inc.
 *                         All rights reserved.
 * Copyright (c) 2016-2022 IBM Corporation.  All rights reserved.
 * Copyright (c) 2021-2025 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "src/include/pmix_config.h"

#include "src/include/pmix_prefetch.h"

#include "include/pmix.h"

#include "src/class/pmix_object.h"
#include "src/class/pmix_list.h"
#include "src/client/pmix_client_ops.h"
#include "src/include/pmix_globals.h"
#include "src/mca/gds/base/base.h"

/* Byte-equal namespaces, which is not what PMIX_CHECK_NSPACE answers.
 *
 * That macro is written for "does this proc belong to that job?", so it
 * reports a match whenever *either* side is NULL or empty - an unset
 * namespace means "mine" there. Both questions in this file are the
 * opposite kind: is this the same proc, and does this namespace name a
 * group. An empty namespace is documented shorthand for our own (see
 * process_request() in pmix_client_get.c), so answering "yes" to it
 * silently substitutes something else for what the caller asked for. */
static bool same_nspace(const char *a, const char *b)
{
    if (NULL == a || NULL == b) {
        return false;
    }
    return (0 == strncmp(a, b, PMIX_MAX_NSLEN));
}

/* Returns PMIX_ERR_NOMEM rather than nothing: a participant this silently
 * failed to record is a short participant list, which is the defect the
 * July 2026 review already fixed once here - it surfaces much later as
 * PMIX_ERR_NOT_A_MEMBER or as a collective that never completes, with
 * nothing pointing back at this function. */
static pmix_status_t append_unique(pmix_list_t *list, const pmix_proc_t *proc)
{
    pmix_proclist_t *nm;

    // check for pre-existence
    PMIX_LIST_FOREACH(nm, list, pmix_proclist_t) {
        // require exact match
        if (!same_nspace(nm->proc.nspace, proc->nspace)) {
            continue;
        }
        if (nm->proc.rank != proc->rank) {
            continue;
        }
        // already present
        return PMIX_SUCCESS;
    }
    // wasn't found, so add it
    nm = PMIX_NEW(pmix_proclist_t);
    if (PMIX_UNLIKELY(NULL == nm)) {
        return PMIX_ERR_NOMEM;
    }
    memcpy(&nm->proc, proc, sizeof(pmix_proc_t));
    pmix_list_append(list, &nm->super);
    return PMIX_SUCCESS;
}

/* Does this namespace name a group we belong to?
 *
 * Deliberately not PMIX_CHECK_NSPACE - see same_nspace() above. A caller
 * naming a proc with an empty namespace (its own, by convention) would
 * otherwise be told it had named whichever group happens to sit first on
 * pmix_client_globals.groups, and the collective would go out with that
 * group's membership in place of the process the caller asked for. */
static bool names_group(const char *grpid, const char *nspace)
{
    if (PMIx_Nspace_invalid(nspace)) {
        return false;
    }
    return same_nspace(grpid, nspace);
}

/* Look up a namespace's job size.
 *
 * The store to ask is myserver's, not our own. A client's job-level data
 * is put there - job_data() in pmix_client.c hands the reply to
 * PMIX_GDS_STORE_JOB_INFO(..., pmix_client_globals.myserver, ...) -
 * while pmix_globals.mypeer holds what _putfn wrote. Asking only mypeer
 * happens to work when the server also uses "hash", because both then
 * resolve to the same component-global tables, and fails outright when
 * it does not: measured, a gds/shmem3 client answers PMIX_ERR_NOT_FOUND
 * for its own job's PMIX_JOB_SIZE through mypeer and PMIX_SUCCESS
 * through myserver. Falling back to mypeer when the two modules differ
 * is what PMIx_Connect_nb does for the same lookup.
 *
 * The caller is on its own thread and is no longer holding grouplock -
 * both are what let this be a fetch the progress thread may have to
 * run. */
static pmix_status_t job_size(const pmix_proc_t *proc, uint32_t *jsize)
{
    pmix_cb_t cb2;
    pmix_kval_t *kv;
    pmix_status_t rc;

    PMIX_CONSTRUCT(&cb2, pmix_cb_t);
    cb2.proc = (pmix_proc_t *) proc;
    cb2.key = PMIX_JOB_SIZE;
    rc = pmix_gds_base_fetch_kv_tsafe(pmix_client_globals.myserver, &cb2);
    if (PMIX_SUCCESS != rc && PMIX_OPERATION_SUCCEEDED != rc &&
        !PMIX_GDS_CHECK_COMPONENT(pmix_client_globals.myserver, "hash")) {
        /* a failed fetch can leave entries behind, and we take the first
         * one off the list below - start the retry from an empty one */
        PMIX_DESTRUCT(&cb2);
        PMIX_CONSTRUCT(&cb2, pmix_cb_t);
        cb2.proc = (pmix_proc_t *) proc;
        cb2.key = PMIX_JOB_SIZE;
        rc = pmix_gds_base_fetch_kv_tsafe(pmix_globals.mypeer, &cb2);
    }
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc && PMIX_OPERATION_SUCCEEDED != rc)) {
        PMIX_DESTRUCT(&cb2);
        return rc;
    }
    kv = (pmix_kval_t *) pmix_list_remove_first(&cb2.kvs);
    PMIX_DESTRUCT(&cb2);
    if (PMIX_UNLIKELY(NULL == kv)) { // should never be NULL
        return PMIX_ERR_NOT_FOUND;
    }
    rc = PMIx_Value_get_number(kv->value, jsize, PMIX_UINT32);
    PMIX_RELEASE(kv);
    return rc;
}

/* A copy of one group we belong to, taken under the lock so the expansion
 * below can run without it - see snapshot_groups(). */
typedef struct {
    char *grpid;
    pmix_proc_t *members;
    size_t nmbrs;
} pmix_grpsnap_t;

static void free_snapshot(pmix_grpsnap_t *snap, size_t nsnap)
{
    size_t n;

    if (NULL == snap) {
        return;
    }
    for (n = 0; n < nsnap; n++) {
        if (NULL != snap[n].grpid) {
            free(snap[n].grpid);
        }
        if (NULL != snap[n].members) {
            PMIX_PROC_FREE(snap[n].members, snap[n].nmbrs);
        }
    }
    free(snap);
}

/* Copy the groups we belong to, and their memberships, out from under
 * pmix_client_globals.grouplock.
 *
 * The expansion below cannot be done while holding that lock. It has to
 * look a group member's job size up in the datastore, and reading the
 * datastore from this thread means handing the fetch to the progress
 * thread and waiting for it (pmix_gds_base_fetch_kv_tsafe) - while the
 * progress thread takes this same lock for the group bookkeeping in
 * pmix_invoke_local_event_hdlr. That is a deadlock, and the alternative
 * of reading the store from here anyway is the race the fetch helper
 * exists to close. So: copy, release, then expand.
 *
 * The copy is deep because neither the group nor its membership array is
 * ours: another application thread may leave a group (removing it from
 * the list and releasing it) and the progress thread shifts a departed
 * member out of a live membership. What we expand is therefore a
 * snapshot, which is the same guarantee the caller had before - the
 * membership can change the moment we let go either way. */
static pmix_status_t snapshot_groups(pmix_grpsnap_t **snap, size_t *nsnap)
{
    pmix_group_t *grp;
    pmix_grpsnap_t *sn;
    size_t n = 0, ngrps;

    *snap = NULL;
    *nsnap = 0;

    pmix_mutex_lock(&pmix_client_globals.grouplock);
    ngrps = pmix_list_get_size(&pmix_client_globals.groups);
    if (0 == ngrps) {
        pmix_mutex_unlock(&pmix_client_globals.grouplock);
        return PMIX_SUCCESS;
    }
    /* allocating under the lock is against this directory's usual advice,
     * and is what the sizes force: they are the list's to tell us. It is
     * safe because nothing here re-enters PMIx - the only rule that
     * matters for this lock is that we do not wait on the progress
     * thread, and malloc does not. */
    sn = (pmix_grpsnap_t *) calloc(ngrps, sizeof(pmix_grpsnap_t));
    if (PMIX_UNLIKELY(NULL == sn)) {
        pmix_mutex_unlock(&pmix_client_globals.grouplock);
        return PMIX_ERR_NOMEM;
    }
    PMIX_LIST_FOREACH (grp, &pmix_client_globals.groups, pmix_group_t) {
        sn[n].grpid = strdup(grp->grpid);
        if (PMIX_UNLIKELY(NULL == sn[n].grpid)) {
            pmix_mutex_unlock(&pmix_client_globals.grouplock);
            free_snapshot(sn, n);
            return PMIX_ERR_NOMEM;
        }
        if (0 < grp->nmbrs) {
            PMIX_PROC_CREATE(sn[n].members, grp->nmbrs);
            if (PMIX_UNLIKELY(NULL == sn[n].members)) {
                pmix_mutex_unlock(&pmix_client_globals.grouplock);
                free(sn[n].grpid);
                free_snapshot(sn, n);
                return PMIX_ERR_NOMEM;
            }
            memcpy(sn[n].members, grp->members, grp->nmbrs * sizeof(pmix_proc_t));
            sn[n].nmbrs = grp->nmbrs;
        }
        ++n;
        if (n == ngrps) {
            /* the list cannot grow under us - we hold the lock - but say
             * so rather than trusting it */
            break;
        }
    }
    pmix_mutex_unlock(&pmix_client_globals.grouplock);
    *snap = sn;
    *nsnap = n;
    return PMIX_SUCCESS;
}

pmix_status_t pmix_client_convert_group_procs(const pmix_proc_t *inprocs, size_t insize,
                                              pmix_proc_t **outprocs, size_t *outsize)
{
    pmix_list_t cache;
    pmix_proclist_t *nm;
    pmix_grpsnap_t *snap = NULL, *grp;
    size_t n, i, g, cnt, sz, nsnap = 0;
    bool match, found;
    uint32_t jsize;
    pmix_status_t rc;
    pmix_proc_t *procs, proc;

    PMIX_CONSTRUCT(&cache, pmix_list_t);

    /* The groups list, and the membership arrays hanging off it, can be
     * changed while we walk them - a construct completing appends a group
     * from the progress thread, a PMIX_GROUP_LEFT event shifts a departed
     * member out of an existing one, and another application thread may
     * leave a group outright. Take a copy under the lock and expand that,
     * because the expansion reads the datastore and so cannot hold it. */
    rc = snapshot_groups(&snap, &nsnap);
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        PMIX_LIST_DESTRUCT(&cache);
        return rc;
    }

    /* cycle thru the procs and check to see if any reference
     * a PMIx group */
    for (n = 0; n < insize; n++) {
        match = false;
        found = false;
        for (g = 0; g < nsnap; g++) {
            grp = &snap[g];
            if (names_group(grp->grpid, inprocs[n].nspace)) {
                match = true;
                /* the nspace matches this group ID */

                if (PMIX_RANK_WILDCARD == inprocs[n].rank) {
                    /* we need to replace this proc with the grp members.
                     * A group can be sitting on the list with no members
                     * left - the PMIX_GROUP_LEFT handler decrements nmbrs
                     * as each one departs and does not drop the group when
                     * it reaches zero - and that expands to nothing at all.
                     * Report it the same way as a group rank past the end
                     * of the membership below, rather than contributing
                     * no participant and calling that success. */
                    for (i=0; i < grp->nmbrs; i++) {
                        rc = append_unique(&cache, &grp->members[i]);
                        if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
                            goto done;
                        }
                        found = true;
                    }
                    break;
                }

                /* if the rank isn't wildcard, then we want a specific
                 * proc from within the group. The group might include
                 * members that have rank=wildcard for their nspace,
                 * and so we have to count from the beginning to find
                 * the proc of the specified group rank */
                cnt = 0;
                for (i = 0; i < grp->nmbrs; i++) {
                    /* we are looking for the cnt=inprocs[n].rank proc
                     * within the group. so count our way across */

                    if (PMIX_RANK_WILDCARD == grp->members[i].rank) {
                        /* We must get the number of procs in this nspace so
                         * we can check to see if the specified rank actually
                         * falls within it */
                        rc = job_size(&grp->members[i], &jsize);
                        if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
                            /* couldn't get the job size, so have to abort.
                             * Report what actually went wrong rather than
                             * flattening every reason into BAD_PARAM */
                            goto done;
                        }
                        if (cnt + jsize > inprocs[n].rank) {
                            /* the specified rank is within this job */
                            PMIX_LOAD_NSPACE(proc.nspace, grp->members[i].nspace);
                            proc.rank = inprocs[n].rank - cnt;
                            rc = append_unique(&cache, &proc);
                            if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
                                goto done;
                            }
                            found = true;
                            break;
                        } else {
                            /* increment the count */
                            cnt += jsize;
                            /* continue to the next group member */
                        }
                    } else {
                        /* this is a single proc entry, so just see if
                         * it matches the one they asked for */
                        if (cnt == inprocs[n].rank) {
                            rc = append_unique(&cache, &grp->members[i]);
                            if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
                                goto done;
                            }
                            found = true;
                            break;
                        } else {
                            /* increment the count */
                            ++cnt;
                            /* continue to the next group member */
                        }
                    }
                }
            }
            if (match) {
                break;
            }
        }
        if (!match) {
            /* xfer the incoming proc across to the cache */
            rc = append_unique(&cache, &inprocs[n]);
            if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
                goto done;
            }
        } else if (!found) {
            /* the nspace named a group we belong to, but the requested group
             * rank lies beyond its membership. Say so - dropping the entry
             * silently produced a short participant list that then failed
             * much further downstream (as PMIX_ERR_NOT_A_MEMBER, or as a
             * collective that never completes) with nothing pointing back
             * here. */
            rc = PMIX_ERR_NOT_FOUND;
            goto done;
        }
    }
    free_snapshot(snap, nsnap);

    /* we have to return the cached array because
     * we might have replaced some of the entries */
    sz = pmix_list_get_size(&cache);
    procs = NULL;
    if (0 < sz) {
        /* the count has to be tested first: PMIX_PROC_CREATE(0) returns
         * the same NULL an allocation failure does, and a zero-length
         * expansion is the documented empty answer rather than an error */
        PMIX_PROC_CREATE(procs, sz);
        if (PMIX_UNLIKELY(NULL == procs)) {
            PMIX_LIST_DESTRUCT(&cache);
            return PMIX_ERR_NOMEM;
        }
        n = 0;
        PMIX_LIST_FOREACH(nm, &cache, pmix_proclist_t) {
            memcpy(&procs[n], &nm->proc, sizeof(pmix_proc_t));
            ++n;
        }
    }
    PMIX_LIST_DESTRUCT(&cache);
    *outprocs = procs;
    *outsize = sz;
    return PMIX_SUCCESS;

done:
    free_snapshot(snap, nsnap);
    PMIX_LIST_DESTRUCT(&cache);
    return rc;
}

/* Return true if pmix_globals.myid is covered by the resolved procs array.
 * Should be called after group expansion, so every entry carries a real
 * nspace. PMIX_RANK_WILDCARD, PMIX_RANK_LOCAL_NODE, and PMIX_RANK_LOCAL_PEERS
 * all cover the calling process when the nspace matches. Used by the
 * collective APIs (fence, connect/disconnect, group construct) that require
 * the caller to be among the listed participants. */
bool pmix_client_proc_is_included(const pmix_proc_t *procs, size_t nprocs)
{
    size_t n;

    for (n = 0; n < nprocs; n++) {
        if (!PMIX_CHECK_NSPACE(procs[n].nspace, pmix_globals.myid.nspace)) {
            continue;
        }
        if (PMIX_RANK_WILDCARD == procs[n].rank ||
            PMIX_RANK_LOCAL_NODE == procs[n].rank ||
            PMIX_RANK_LOCAL_PEERS == procs[n].rank ||
            procs[n].rank == pmix_globals.myid.rank) {
            return true;
        }
    }
    return false;
}
