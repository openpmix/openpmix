#include "test_fence.h"

static void release_cb(pmix_status_t status, void *cbdata)
{
    int *ptr = (int*)cbdata;
    *ptr = 0;
}

int test_fence(test_params params, char *my_nspace, int my_rank)
{
    int len;
    int rc;
    size_t i, j;
    fence_desc_t *desc;
    nspace_desc_t *ndesc;
    rank_desc_t *rdesc;
    size_t nranges;
    pmix_range_t *rngs;
    int n = 0, r = 0;
    int participate = 0;
    int fence_num = 0;
    pmix_value_t value;
    char key[50], sval[50];
    pmix_value_t *val = &value;

    PMIX_CONSTRUCT(&test_fences, pmix_list_t);
    parse_fence(params.fences, 1);

    PMIX_LIST_FOREACH(desc, &test_fences, fence_desc_t) {
        nranges = pmix_list_get_size(&(desc->nspaces));
        PMIX_RANGE_CREATE(rngs, nranges);
        char tmp[256] = {0};
        len = sprintf(tmp, "fence %d: block = %d de = %d ", fence_num, desc->blocking, desc->data_exchange);
        n = 0;
        PMIX_LIST_FOREACH(ndesc, &(desc->nspaces), nspace_desc_t) {
            (void)snprintf(rngs[n].nspace, PMIX_MAX_NSLEN, "%s-%d", TEST_NAMESPACE, ndesc->id);
            rngs[n].nranks = pmix_list_get_size(&(ndesc->ranks));
            if (0 < rngs[n].nranks) {
                rngs[n].ranks = (int*)malloc(rngs[n].nranks * sizeof(int));
            }
            len += sprintf(tmp+len, "ns %d ranks: ", ndesc->id);
            r = 0;
            PMIX_LIST_FOREACH(rdesc, &(ndesc->ranks), rank_desc_t) {
                if (!strncmp(my_nspace, rngs[n].nspace, strlen(my_nspace)) && (my_rank == rdesc->rank || -1 == rdesc->rank)) {
                    participate = 1;
                }
                if (-1 == rdesc->rank) {
                    len += sprintf(tmp+len, "all; ");
                    free(rngs[n].ranks);
                    rngs[n].ranks = NULL;
                    rngs[n].nranks = 0;
                    break;
                } else {
                    len += sprintf(tmp+len, "%d,", rdesc->rank);
                    rngs[n].ranks[r] = rdesc->rank;
                }
                r++;
            }
            n++;
        }
        fprintf(stderr,"tmp = %s,  me: %s:%d participate = %d\n",  tmp, my_nspace, my_rank, participate);
        if (1 == participate) {
            /*run fence test on this range */
            /* first put value (my_ns, my_rank) with key based on fence_num to split results of different fences*/
            (void)snprintf(key, 50, "key-f%d", fence_num);
            (void)snprintf(sval, 50, "%s:%d", my_nspace, my_rank);
            fprintf(stderr, "%s:%d put key %s val %s\n", my_nspace, my_rank, key, sval);
            PMIX_VAL_SET(&value, string, sval);
            if (PMIX_SUCCESS != (rc = PMIx_Put(PMIX_GLOBAL, key, &value))) {
                TEST_ERROR(("%s:%d: PMIx_Put failed: %d", my_nspace, my_rank, rc));
                PMIX_RANGE_FREE(rngs, nranges);
                PMIX_LIST_DESTRUCT(&test_fences);
                return rc;
            }
            PMIX_VALUE_DESTRUCT(&value);

            /* Submit the data */
            if (PMIX_SUCCESS != (rc = PMIx_Commit())) {
                TEST_ERROR(("%s:%d: PMIx_Commit failed: %d", my_nspace, my_rank, rc));
                PMIX_RANGE_FREE(rngs, nranges);
                PMIX_LIST_DESTRUCT(&test_fences);
                return rc;
            }

            /* perform fence */
            if( desc->blocking ){
                if (PMIX_SUCCESS != (rc = PMIx_Fence(NULL, 0, desc->data_exchange))) {
                    TEST_ERROR(("%s:%d: PMIx_Fence failed: %d", my_nspace, my_rank, rc));
                    PMIX_RANGE_FREE(rngs, nranges);
                    PMIX_LIST_DESTRUCT(&test_fences);
                    return rc;
                }
            } else {
                int in_progress = 1, count;
                if ( PMIX_SUCCESS != (rc = PMIx_Fence_nb(NULL, 0, desc->data_exchange, release_cb, &in_progress))) {
                    TEST_ERROR(("%s:%d: PMIx_Fence failed: %d", my_nspace, my_rank, rc));
                    PMIX_RANGE_FREE(rngs, nranges);
                    PMIX_LIST_DESTRUCT(&test_fences);
                    return rc;
                }

                count = 0;
                while( in_progress ){
                    struct timespec ts;
                    ts.tv_sec = 0;
                    ts.tv_nsec = 100;
                    nanosleep(&ts,NULL);
                    count++;
                }
                TEST_VERBOSE(("PMIx_Fence_nb(barrier,collect): free time: %lfs", count*100*1E-9));
            }
            TEST_VERBOSE(("%s:%d: Fence successfully completed", my_nspace, my_rank));

            /* get data from all participating in this fence clients */
            for (i = 0; i < nranges; i++ ) {
                /* handle case if all processes from the namespace should participate (ranks == NULL):
                 * parse --ns-dist option and count number of processes and rank offset for the target namespace
                 * to fill ranks array in the current range. This needs to be done for checking purposes.*/
                if (0 == rngs[i].nranks && NULL == rngs[i].ranks) {
                    int base_rank = 0;
                    size_t num_ranks = 0;
                    int num = -1;
                    if (NULL == params.ns_dist) {
                        rngs[i].nranks = params.ns_size;
                        rngs[i].ranks = (int*)malloc(params.ns_size * sizeof(int));
                        for (j = 0; j < (size_t)params.ns_size; j++) {
                            rngs[i].ranks[j] = j;
                        }
                    } else {
                        char *pch = params.ns_dist;
                        int ns_id = (int)strtol(rngs[i].nspace + strlen(TEST_NAMESPACE) + 1, NULL, 10);
                        while (NULL != pch && num != ns_id) {
                            base_rank += num_ranks;
                            pch = strtok((-1 == num ) ? params.ns_dist : NULL, ":");
                            num++;
                            num_ranks = (size_t)strtol(pch, NULL, 10);
                        }
                        if (num == ns_id && 0 != num_ranks) {
                            rngs[i].nranks = num_ranks;
                            rngs[i].ranks = (int*)malloc(num_ranks * sizeof(int));
                            for (j = 0; j < num_ranks; j++) {
                                rngs[i].ranks[j] = base_rank+j;
                            }
                        } else {
                            TEST_ERROR(("%s:%d: Can't parse --ns-dist value in order to get ranks for namespace %s", my_nspace, my_rank, rngs[i].nspace));
                            PMIX_RANGE_FREE(rngs, nranges);
                            PMIX_LIST_DESTRUCT(&test_fences);
                            return PMIX_ERROR;
                        }
                    }
                }
                for (j = 0; j < rngs[i].nranks; j++) {
                    snprintf(sval, 50, "%s:%d", rngs[i].nspace, rngs[i].ranks[j]);
                    fprintf(stderr, "%s:%d want to get from %s:%d key %s val %s\n", my_nspace, my_rank, rngs[i].nspace, rngs[i].ranks[j], key, sval);
                    if (PMIX_SUCCESS != (rc = PMIx_Get(rngs[i].nspace, rngs[i].ranks[j], key, &val))) {//+params.base_rank
                        TEST_ERROR(("%s:%d: PMIx_Get failed (%d) from %s:%d", my_nspace, my_rank, rc, rngs[i].nspace, rngs[i].ranks[j]));
                        PMIX_RANGE_FREE(rngs, nranges);
                        PMIX_LIST_DESTRUCT(&test_fences);
                        return rc;
                    }
                    if (val->type != PMIX_STRING || strcmp(val->data.string, sval)) {
                        TEST_ERROR(("%s:%d:  Key %s value or type mismatch, wait %s(%d) get %s(%d) from %s:%d",
                                    my_nspace, my_rank, key, sval, PMIX_STRING, val->data.string, val->type, rngs[i].nspace, rngs[i].ranks[j]));
                        PMIX_VALUE_RELEASE(val);
                        PMIX_RANGE_FREE(rngs, nranges);
                        PMIX_LIST_DESTRUCT(&test_fences);
                        return PMIX_ERROR;
                    }
                    TEST_VERBOSE(("%s:%d: GET OF %s SUCCEEDED from %s:%d", my_nspace, my_rank, key, rngs[i].nspace, rngs[i].ranks[j]));
                    PMIX_VALUE_RELEASE(val);
                }
            }
        }
        PMIX_RANGE_FREE(rngs, nranges);
        fence_num++;
    }
    PMIX_LIST_DESTRUCT(&test_fences);
    return PMIX_SUCCESS;
}
