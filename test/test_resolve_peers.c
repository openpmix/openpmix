#include "test_resolve_peers.h"

int test_resolve_peers(char *my_nspace, int my_rank, test_params params)
{
    int rc;
    pmix_proc_t *procs;
    size_t nprocs, nranks, i;
    int *ranks;
    rc = PMIx_Resolve_peers(NODE_NAME, my_nspace, &procs, &nprocs);
    if (PMIX_SUCCESS != rc) {
        TEST_ERROR(("%s:%d: Resolve peers test failed: rc = %d", my_nspace, my_rank, rc));
        return rc;
    }
    rc = get_all_ranks_from_namespace(params, my_nspace, &ranks, &nranks);
    if (PMIX_SUCCESS != rc) {
        TEST_ERROR(("%s:%d: get_all_ranks_from_namespace function failed", my_nspace, my_rank));
        PMIX_PROC_FREE(procs, nprocs);
        return rc;
    }
    if (nprocs != nranks) {
        TEST_ERROR(("%s:%d: Resolve peers returned incorect result: returned %lu processes, expected %lu", my_nspace, my_rank, nprocs, nranks));
        PMIX_PROC_FREE(procs, nprocs);
        free(ranks);
        return PMIX_ERROR;
    }
    if (NULL == procs && 0 == nprocs) {
        TEST_ERROR(("%s:%d: Resolve peers didn't find any process from ns %s at this node\n", my_nspace, my_rank,my_nspace));
        PMIX_PROC_FREE(procs, nprocs);
        free(ranks);
        return PMIX_ERROR;
    }
    for (i = 0; i < nprocs; i++) {
        if (strncmp(procs[i].nspace, my_nspace, strlen(my_nspace)+1) || procs[i].rank != ranks[i]) {
            TEST_ERROR(("%s:%d: Resolve peers returned incorect result: returned value %s:%d, expected %s:%d", my_nspace, my_rank, procs[i].nspace, procs[i].rank, my_nspace, ranks[i]));
            rc = PMIX_ERROR;
            break;
        }
    }
    TEST_VERBOSE(("%s:%d: Resolve peers test succeeded.", my_nspace, my_rank));
    PMIX_PROC_FREE(procs, nprocs);
    free(ranks);
    return PMIX_SUCCESS;
}
