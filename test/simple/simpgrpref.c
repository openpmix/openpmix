/*
 * Copyright (c) 2026      Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 */

/* A group named by its ID, as the PMIx APIs allow: PMIX_RANK_WILDCARD for
 * every member, or a group rank for one. From v7.0 the server holds each
 * group's membership and expands the reference; the client keeps none.
 * Run under simptest with at least three processes. Each one:
 *
 *   constructs "refgrp" from every process of the job, listed in reverse
 *   fences with { "refgrp", PMIX_RANK_WILDCARD }
 *   gets each other member's value by group rank - the members, in group
 *     rank order, are the PMIX_GROUP_MEMBERSHIP the construct returned
 *   asks for the whole group, and for a group rank past the membership,
 *     in a get - each refused
 *   fences naming a group that does not exist - refused, as the caller
 *     is not among the participants
 *
 * then the last process leaves the group, and the rest fence with the group
 * again - now without it - and destruct it naming only the group. */

#include "src/include/pmix_config.h"
#include "include/pmix.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "src/include/pmix_globals.h"

#define REF_GROUP "refgrp"
#define REF_KEY   "simpgrpref.rank"

static pmix_proc_t myproc;

static int fail(const char *what, pmix_status_t rc)
{
    fprintf(stderr, "Client %s:%u %s: %s\n", myproc.nspace, myproc.rank, what,
            PMIx_Error_string(rc));
    PMIx_Finalize(NULL, 0);
    return 1;
}

int main(int argc, char **argv)
{
    pmix_status_t rc;
    pmix_proc_t proc, *procs = NULL, *members = NULL;
    pmix_value_t value, *val = NULL;
    pmix_info_t *results = NULL, info;
    size_t nresults = 0, nmembers = 0, n, grank = 0;
    uint32_t nprocs, r;
    bool collect = true;
    PMIX_HIDE_UNUSED_PARAMS(argc, argv);

    if (PMIX_SUCCESS != (rc = PMIx_Init(&myproc, NULL, 0))) {
        fprintf(stderr, "Client PMIx_Init failed: %s\n", PMIx_Error_string(rc));
        exit(1);
    }
    PMIX_LOAD_PROCID(&proc, myproc.nspace, PMIX_RANK_WILDCARD);
    if (PMIX_SUCCESS != (rc = PMIx_Get(&proc, PMIX_JOB_SIZE, NULL, 0, &val))) {
        return fail("get of the job size failed", rc);
    }
    nprocs = val->data.uint32;
    PMIX_VALUE_RELEASE(val);
    if (nprocs < 3) {
        return fail("needs at least three processes", PMIX_ERR_BAD_PARAM);
    }

    /* something for each member to find by group rank */
    PMIX_VALUE_LOAD(&value, &myproc.rank, PMIX_UINT32);
    rc = PMIx_Put(PMIX_GLOBAL, REF_KEY, &value);
    PMIX_VALUE_DESTRUCT(&value);
    if (PMIX_SUCCESS != rc || PMIX_SUCCESS != (rc = PMIx_Commit())) {
        return fail("put/commit failed", rc);
    }

    /* every process of the job, in reverse */
    PMIX_PROC_CREATE(procs, nprocs);
    for (r = 0; r < nprocs; r++) {
        PMIX_LOAD_PROCID(&procs[r], myproc.nspace, nprocs - 1 - r);
    }
    rc = PMIx_Group_construct(REF_GROUP, procs, nprocs, NULL, 0, &results, &nresults);
    PMIX_PROC_FREE(procs, nprocs);
    if (PMIX_SUCCESS != rc) {
        return fail("group construct failed", rc);
    }
    for (n = 0; n < nresults; n++) {
        if (PMIX_CHECK_KEY(&results[n], PMIX_GROUP_MEMBERSHIP) &&
            PMIX_DATA_ARRAY == results[n].value.type) {
            nmembers = results[n].value.data.darray->size;
            PMIX_PROC_CREATE(members, nmembers);
            memcpy(members, results[n].value.data.darray->array, nmembers * sizeof(pmix_proc_t));
        }
    }
    PMIX_INFO_FREE(results, nresults);
    if (nprocs != nmembers) {
        return fail("the construct did not return the membership", PMIX_ERR_BAD_PARAM);
    }

    /* the whole group, by its ID */
    PMIX_LOAD_PROCID(&proc, REF_GROUP, PMIX_RANK_WILDCARD);
    PMIX_INFO_LOAD(&info, PMIX_COLLECT_DATA, &collect, PMIX_BOOL);
    rc = PMIx_Fence(&proc, 1, &info, 1);
    PMIX_INFO_DESTRUCT(&info);
    if (PMIX_SUCCESS != rc) {
        return fail("fence with the group failed", rc);
    }

    /* each other member, by group rank */
    for (n = 0; n < nmembers; n++) {
        if (PMIX_CHECK_PROCID(&members[n], &myproc)) {
            grank = n;
            continue;
        }
        PMIX_LOAD_PROCID(&proc, REF_GROUP, (pmix_rank_t) n);
        rc = PMIx_Get(&proc, REF_KEY, NULL, 0, &val);
        if (PMIX_SUCCESS != rc) {
            return fail("get by group rank failed", rc);
        }
        if (PMIX_UINT32 != val->type || val->data.uint32 != members[n].rank) {
            fprintf(stderr, "Client %s:%u group rank %u answered %u, expected %u\n",
                    myproc.nspace, myproc.rank, (unsigned) n, (unsigned) val->data.uint32,
                    (unsigned) members[n].rank);
            PMIX_VALUE_RELEASE(val);
            PMIx_Finalize(NULL, 0);
            return 1;
        }
        PMIX_VALUE_RELEASE(val);
    }

    /* a get is for one process */
    PMIX_LOAD_PROCID(&proc, REF_GROUP, PMIX_RANK_WILDCARD);
    rc = PMIx_Get(&proc, REF_KEY, NULL, 0, &val);
    if (PMIX_ERR_BAD_PARAM != rc) {
        return fail("get of the whole group was not refused", rc);
    }
    PMIX_LOAD_PROCID(&proc, REF_GROUP, (pmix_rank_t) nmembers);
    rc = PMIx_Get(&proc, REF_KEY, NULL, 0, &val);
    if (PMIX_ERR_NOT_FOUND != rc) {
        return fail("get past the membership was not refused", rc);
    }

    /* a group that does not exist names nobody we know - and not us */
    PMIX_LOAD_PROCID(&proc, "simpgrpref-no-such-group", PMIX_RANK_WILDCARD);
    rc = PMIx_Fence(&proc, 1, NULL, 0);
    if (PMIX_ERR_NOT_A_MEMBER != rc) {
        return fail("fence with an unknown group was not refused", rc);
    }

    /* everyone is done reading the group by rank before anyone leaves it:
     * once the last member has left, its group rank names nobody */
    PMIX_LOAD_PROCID(&proc, myproc.nspace, PMIX_RANK_WILDCARD);
    if (PMIX_SUCCESS != (rc = PMIx_Fence(&proc, 1, NULL, 0))) {
        return fail("job fence before the leave failed", rc);
    }

    /* the last member leaves, and says so before the job-wide fence - so
     * the rest name the group again only once it is gone */
    if (grank == nmembers - 1) {
        rc = PMIx_Group_leave(REF_GROUP, NULL, 0);
        if (PMIX_SUCCESS != rc) {
            return fail("group leave failed", rc);
        }
    }
    PMIX_LOAD_PROCID(&proc, myproc.nspace, PMIX_RANK_WILDCARD);
    if (PMIX_SUCCESS != (rc = PMIx_Fence(&proc, 1, NULL, 0))) {
        return fail("job fence failed", rc);
    }
    if (grank != nmembers - 1) {
        PMIX_LOAD_PROCID(&proc, REF_GROUP, PMIX_RANK_WILDCARD);
        if (PMIX_SUCCESS != (rc = PMIx_Fence(&proc, 1, NULL, 0))) {
            return fail("fence with the group after a member left failed", rc);
        }
        if (PMIX_SUCCESS != (rc = PMIx_Group_destruct(REF_GROUP, NULL, 0))) {
            return fail("group destruct failed", rc);
        }
    }
    PMIX_PROC_FREE(members, nmembers);

    if (PMIX_SUCCESS != (rc = PMIx_Finalize(NULL, 0))) {
        fprintf(stderr, "Client %s:%u PMIx_Finalize failed: %s\n", myproc.nspace,
                myproc.rank, PMIx_Error_string(rc));
        return 1;
    }
    fprintf(stderr, "Client %s:%u group references resolved OK\n", myproc.nspace, myproc.rank);
    return 0;
}
