/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/* The members a host says never took part in a group construct
 * (PMIX_COLLECTIVE_NONPARTICIPANTS) reach the participants in the
 * construct's results - when it succeeds, and when it fails, through both
 * the non-blocking and the blocking form.
 *
 * Run under test/simple/simptest, whose group upcall answers a construct
 * carrying "pmix.test.nonpart" with the procs that directive names as the
 * non-participants, and one carrying "pmix.test.nonpart.fail" the same way
 * with PMIX_ERR_TIMEOUT and no group. It does so only once it has checked
 * the upcall's PMIX_LOCAL_PARTICIPANTS - otherwise it answers
 * PMIX_ERR_BAD_PARAM, which fails the construct here. Driven by
 * test/unit/run_grpnonpart.pl. */

#include "src/include/pmix_config.h"
#include "include/pmix.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "src/include/pmix_globals.h"

static pmix_proc_t myproc;

typedef struct {
    pmix_lock_t lock;
    pmix_status_t status;
    pmix_proc_t nonpart;
    size_t nnonpart;
} nb_result_t;

static int fail(const char *what, pmix_status_t rc)
{
    fprintf(stderr, "Client %s:%u %s: %s\n", myproc.nspace, myproc.rank, what,
            PMIx_Error_string(rc));
    PMIx_Finalize(NULL, 0);
    return 1;
}

/* the non-participants among results, if exactly one is named */
static size_t find_nonpart(const pmix_info_t *results, size_t nresults, pmix_proc_t *out)
{
    size_t n;

    for (n = 0; NULL != results && n < nresults; n++) {
        if (PMIX_CHECK_KEY(&results[n], PMIX_COLLECTIVE_NONPARTICIPANTS) &&
            PMIX_DATA_ARRAY == results[n].value.type &&
            NULL != results[n].value.data.darray &&
            PMIX_PROC == results[n].value.data.darray->type) {
            if (1 == results[n].value.data.darray->size) {
                memcpy(out, results[n].value.data.darray->array, sizeof(pmix_proc_t));
            }
            return results[n].value.data.darray->size;
        }
    }
    return 0;
}

static void construct_cb(pmix_status_t status, pmix_info_t *info, size_t ninfo, void *cbdata,
                         pmix_release_cbfunc_t release_fn, void *release_cbdata)
{
    nb_result_t *r = (nb_result_t *) cbdata;

    r->status = status;
    r->nnonpart = find_nonpart(info, ninfo, &r->nonpart);
    if (NULL != release_fn) {
        release_fn(release_cbdata);
    }
    PMIX_WAKEUP_THREAD(&r->lock);
}

int main(int argc, char **argv)
{
    pmix_status_t rc;
    pmix_proc_t proc, *procs = NULL, absent, got;
    pmix_value_t *val = NULL;
    pmix_info_t *results = NULL, info;
    pmix_data_array_t darray;
    size_t nresults = 0;
    uint32_t nprocs, r;
    nb_result_t nb;
    PMIX_HIDE_UNUSED_PARAMS(argc, argv);

    rc = PMIx_Init(&myproc, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "Client PMIx_Init failed: %s\n", PMIx_Error_string(rc));
        return 1;
    }
    PMIX_LOAD_PROCID(&proc, myproc.nspace, PMIX_RANK_WILDCARD);
    rc = PMIx_Get(&proc, PMIX_JOB_SIZE, NULL, 0, &val);
    if (PMIX_SUCCESS != rc) {
        return fail("get job size", rc);
    }
    nprocs = val->data.uint32;
    PMIX_VALUE_RELEASE(val);
    PMIX_PROC_CREATE(procs, nprocs);
    for (r = 0; r < nprocs; r++) {
        PMIX_LOAD_PROCID(&procs[r], myproc.nspace, r);
    }

    /* the proc the host is to name - any proc will do */
    PMIX_LOAD_PROCID(&absent, "simpgrpnonpart.absent", 7);
    darray.type = PMIX_PROC;
    darray.array = &absent;
    darray.size = 1;

    /* a construct that succeeds, with a non-participant named */
    PMIX_INFO_LOAD(&info, "pmix.test.nonpart", &darray, PMIX_DATA_ARRAY);
    rc = PMIx_Group_construct("nonpart-ok", procs, nprocs, &info, 1, &results, &nresults);
    PMIX_INFO_DESTRUCT(&info);
    if (PMIX_SUCCESS != rc) {
        return fail("construct that succeeds", rc);
    }
    if (1 != find_nonpart(results, nresults, &got) || !PMIX_CHECK_PROCID(&got, &absent)) {
        return fail("construct that succeeds did not return the non-participant",
                    PMIX_ERR_NOT_FOUND);
    }
    PMIX_INFO_FREE(results, nresults);
    results = NULL;
    rc = PMIx_Group_destruct("nonpart-ok", NULL, 0);
    if (PMIX_SUCCESS != rc) {
        return fail("destruct", rc);
    }

    /* a construct that fails: the non-participant still reaches us */
    PMIX_CONSTRUCT_LOCK(&nb.lock);
    nb.status = PMIX_SUCCESS;
    nb.nnonpart = 0;
    PMIX_INFO_LOAD(&info, "pmix.test.nonpart.fail", &darray, PMIX_DATA_ARRAY);
    rc = PMIx_Group_construct_nb("nonpart-fail", procs, nprocs, &info, 1, construct_cb, &nb);
    if (PMIX_SUCCESS != rc) {
        PMIX_INFO_DESTRUCT(&info);
        return fail("construct that fails was not started", rc);
    }
    PMIX_WAIT_THREAD(&nb.lock);
    PMIX_DESTRUCT_LOCK(&nb.lock);
    PMIX_INFO_DESTRUCT(&info);
    if (PMIX_ERR_TIMEOUT != nb.status) {
        return fail("construct that fails did not fail as the host said", nb.status);
    }
    if (1 != nb.nnonpart || !PMIX_CHECK_PROCID(&nb.nonpart, &absent)) {
        return fail("construct that fails did not return the non-participant",
                    PMIX_ERR_NOT_FOUND);
    }

    /* ...and through the blocking form, which returns results on a failure
     * only when they name the members that never took part */
    PMIX_INFO_LOAD(&info, "pmix.test.nonpart.fail", &darray, PMIX_DATA_ARRAY);
    rc = PMIx_Group_construct("nonpart-fail-blocking", procs, nprocs, &info, 1, &results,
                              &nresults);
    PMIX_INFO_DESTRUCT(&info);
    if (PMIX_ERR_TIMEOUT != rc) {
        return fail("blocking construct that fails did not fail as the host said", rc);
    }
    if (1 != find_nonpart(results, nresults, &got) || !PMIX_CHECK_PROCID(&got, &absent)) {
        return fail("blocking construct that fails did not return the non-participant",
                    PMIX_ERR_NOT_FOUND);
    }
    PMIX_INFO_FREE(results, nresults);
    results = NULL;

    PMIX_PROC_FREE(procs, nprocs);
    fprintf(stdout, "Client %s:%u non-participants returned OK\n", myproc.nspace, myproc.rank);
    rc = PMIx_Finalize(NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "Client finalize failed: %s\n", PMIx_Error_string(rc));
        return 1;
    }
    return 0;
}
