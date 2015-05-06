#include "test_fence.h"

static void release_cb(pmix_status_t status, void *cbdata)
{
    int *ptr = (int*)cbdata;
    *ptr = 0;
}

static void add_noise(char *noise_param, char *my_nspace, int my_rank)
{
    int participate = 0;
    nspace_desc_t *ndesc;
    rank_desc_t *rdesc;
    char nspace[PMIX_MAX_NSLEN];
    parse_noise(noise_param, 1);
    if (NULL != noise_range) {
        PMIX_LIST_FOREACH(ndesc, &(noise_range->nspaces), nspace_desc_t) {
            (void)snprintf(nspace, PMIX_MAX_NSLEN, "%s-%d", TEST_NAMESPACE, ndesc->id);
            PMIX_LIST_FOREACH(rdesc, &(ndesc->ranks), rank_desc_t) {
                if (!strncmp(my_nspace, nspace, strlen(my_nspace)) && (my_rank == rdesc->rank || -1 == rdesc->rank)) {
                    participate = 1;
                    break;
                }
            }
        }
        if (1 == participate) {
            sleep(2);
            fprintf(stderr, "I'm %s:%d sleeping\n", my_nspace, my_rank);
        }
        PMIX_RELEASE(noise_range);
        noise_range = NULL;
    }
}

static int get_all_ranks_from_namespace(test_params params, char *nspace, int **ranks, size_t *nranks)
{
    int base_rank = 0;
    size_t num_ranks = 0;
    int num = -1;
    size_t j;
    if (NULL == params.ns_dist) {
        *nranks = params.ns_size;
        *ranks = (int*)malloc(params.ns_size * sizeof(int));
        for (j = 0; j < (size_t)params.ns_size; j++) {
            (*ranks)[j] = j;
        }
    } else {
        char *tmp = strdup(params.ns_dist);
        char *pch = tmp;
        int ns_id = (int)strtol(nspace + strlen(TEST_NAMESPACE) + 1, NULL, 10);
        while (NULL != pch && num != ns_id) {
            base_rank += num_ranks;
            pch = strtok((-1 == num ) ? tmp : NULL, ":");
            num++;
            num_ranks = (size_t)strtol(pch, NULL, 10);
        }
        if (num == ns_id && 0 != num_ranks) {
            *nranks = num_ranks;
            *ranks = (int*)malloc(num_ranks * sizeof(int));
            for (j = 0; j < num_ranks; j++) {
                (*ranks)[j] = base_rank+j;
            }
        } else {
            free(tmp);
            return PMIX_ERROR;
        }
        free(tmp);
    }
    return PMIX_SUCCESS;
}

#define PUT(dtype, data, flag, fence_num, ind) do {                                                                 \
    char key[50];                                                                                                   \
    pmix_value_t value;                                                                                             \
    (void)snprintf(key, 50, "key-f%d:%d", fence_num, ind);                                                          \
    PMIX_VAL_SET(&value, dtype, data);                                                                              \
    fprintf(stderr, "%s:%d put key %s\n", my_nspace, my_rank, key);                                                 \
    if (PMIX_SUCCESS != (rc = PMIx_Put(flag, key, &value))) {                                                       \
        TEST_ERROR(("%s:%d: PMIx_Put key %s failed: %d", my_nspace, my_rank, key, rc));                             \
        rc = PMIX_ERROR;                                                                                            \
    }                                                                                                               \
    PMIX_VALUE_DESTRUCT(&value);                                                                                    \
} while (0);

#define GET(dtype, data, ns, rank, fence_num, ind) do {                                                             \
    char key[50];                                                                                                   \
    pmix_value_t value;                                                                                             \
    pmix_value_t *val = &value;                                                                                     \
    sprintf(key,"key-f%d:%d", fence_num, ind);                                                                      \
    fprintf(stderr, "%s:%d want to get from %s:%d key %s\n", my_nspace, my_rank, ns, rank, key);                    \
    if (PMIX_SUCCESS != (rc = PMIx_Get(ns, rank, key, &val))) {                                                     \
        TEST_ERROR(("%s:%d: PMIx_Get failed: %d from %s:%d", my_nspace, my_rank, rc, ns, rank));                    \
        rc = PMIX_ERROR;                                                                                            \
    }                                                                                                               \
    else if (NULL == val) {                                                                                         \
        TEST_ERROR(("%s:%d: PMIx_Get returned NULL value", my_nspace, my_rank));                                    \
        rc = PMIX_ERROR;                                                                                            \
    }                                                                                                               \
    else if (val->type != PMIX_VAL_TYPE_ ## dtype || PMIX_VAL_CMP(dtype, PMIX_VAL_FIELD_ ## dtype((val)), data)) {  \
        TEST_ERROR(("%s:%d: Key %s value or type mismatch,"                                                         \
                    " want type %d get type %d",                                                                    \
                    my_nspace, my_rank, key, PMIX_VAL_TYPE_ ## dtype, val->type));                                  \
        rc = PMIX_ERROR;                                                                                            \
    }                                                                                                               \
    if (PMIX_SUCCESS == rc) {                                                                                       \
        TEST_VERBOSE(("%s:%d: GET OF %s from %s:%d SUCCEEDED", my_nspace, my_rank, key, ns, rank));                 \
    }                                                                                                               \
    PMIX_VALUE_RELEASE(val);                                                                                        \
} while(0);

#define FENCE(blocking, data_ex, rngs, nranges) do {                                                                \
    if( blocking ){                                                                                                 \
        rc = PMIx_Fence(rngs, nranges, data_ex);                                                                    \
    } else {                                                                                                        \
        int in_progress = 1, count;                                                                                 \
        if ( PMIX_SUCCESS == (rc = PMIx_Fence_nb(rngs, nranges, data_ex, release_cb, &in_progress))) {              \
            count = 0;                                                                                              \
            while( in_progress ){                                                                                   \
                struct timespec ts;                                                                                 \
                ts.tv_sec = 0;                                                                                      \
                ts.tv_nsec = 100;                                                                                   \
                nanosleep(&ts,NULL);                                                                                \
                count++;                                                                                            \
            }                                                                                                       \
            TEST_VERBOSE(("PMIx_Fence_nb(barrier,collect): free time: %lfs", count*100*1E-9));                      \
        }                                                                                                           \
    }                                                                                                               \
    if (PMIX_SUCCESS == rc) {                                                                                       \
        TEST_VERBOSE(("%s:%d: Fence successfully completed", my_nspace, my_rank));                                  \
    }                                                                                                               \
} while (0);

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
    char sval[50];
    int put_ind;

    if (NULL != params.noise) {
        add_noise(params.noise, my_nspace, my_rank);
    }

    PMIX_CONSTRUCT(&test_fences, pmix_list_t);
    parse_fence(params.fences, 1);

    PMIX_LIST_FOREACH(desc, &test_fences, fence_desc_t) {
        nranges = pmix_list_get_size(&(desc->range->nspaces));
        PMIX_RANGE_CREATE(rngs, nranges);
        char tmp[256] = {0};
        len = sprintf(tmp, "fence %d: block = %d de = %d ", fence_num, desc->blocking, desc->data_exchange);
        n = 0;
        participate = 0;
        PMIX_LIST_FOREACH(ndesc, &(desc->range->nspaces), nspace_desc_t) {
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
            put_ind = 0;
            (void)snprintf(sval, 50, "%s:%d", my_nspace, my_rank);
            PUT(string, sval, PMIX_GLOBAL, fence_num, put_ind++);
            if (PMIX_SUCCESS != rc) {
                TEST_ERROR(("%s:%d: PMIx_Put failed: %d", my_nspace, my_rank, rc));
                PMIX_RANGE_FREE(rngs, nranges);
                PMIX_LIST_DESTRUCT(&test_fences);
                return rc;
            }

            PUT(int, my_rank, PMIX_GLOBAL, fence_num, put_ind++);
            if (PMIX_SUCCESS != rc) {
                TEST_ERROR(("%s:%d: PMIx_Put failed: %d", my_nspace, my_rank, rc));
                PMIX_RANGE_FREE(rngs, nranges);
                PMIX_LIST_DESTRUCT(&test_fences);
                return rc;
            }

            PUT(float, 1.1, PMIX_GLOBAL, fence_num, put_ind++);
            if (PMIX_SUCCESS != rc) {
                TEST_ERROR(("%s:%d: PMIx_Put failed: %d", my_nspace, my_rank, rc));
                PMIX_RANGE_FREE(rngs, nranges);
                PMIX_LIST_DESTRUCT(&test_fences);
                return rc;
            }

            PUT(uint32_t, 14, PMIX_GLOBAL, fence_num, put_ind++);
            if (PMIX_SUCCESS != rc) {
                TEST_ERROR(("%s:%d: PMIx_Put failed: %d", my_nspace, my_rank, rc));
                PMIX_RANGE_FREE(rngs, nranges);
                PMIX_LIST_DESTRUCT(&test_fences);
                return rc;
            }

            PUT(uint16_t, 15, PMIX_GLOBAL, fence_num, put_ind++);
            if (PMIX_SUCCESS != rc) {
                TEST_ERROR(("%s:%d: PMIx_Put failed: %d", my_nspace, my_rank, rc));
                PMIX_RANGE_FREE(rngs, nranges);
                PMIX_LIST_DESTRUCT(&test_fences);
                return rc;
            }

            /* Submit the data */
            if (PMIX_SUCCESS != (rc = PMIx_Commit())) {
                TEST_ERROR(("%s:%d: PMIx_Commit failed: %d", my_nspace, my_rank, rc));
                PMIX_RANGE_FREE(rngs, nranges);
                PMIX_LIST_DESTRUCT(&test_fences);
                return rc;
            }

            /* perform fence */
            FENCE(desc->blocking, desc->data_exchange, rngs, nranges);
            if (PMIX_SUCCESS != rc) {
                TEST_ERROR(("%s:%d: PMIx_Fence failed: %d", my_nspace, my_rank, rc));
                PMIX_RANGE_FREE(rngs, nranges);
                PMIX_LIST_DESTRUCT(&test_fences);
                return rc;
            }

            /* get data from all participating in this fence clients */
            for (i = 0; i < nranges; i++ ) {
                /* handle case if all processes from the namespace should participate (ranks == NULL):
                 * parse --ns-dist option and count number of processes and rank offset for the target namespace
                 * to fill ranks array in the current range. This needs to be done for checking purposes.*/
                if (0 == rngs[i].nranks && NULL == rngs[i].ranks) {
                    rc = get_all_ranks_from_namespace(params, rngs[i].nspace, &rngs[i].ranks, &rngs[i].nranks);
                    if (PMIX_SUCCESS != rc) {
                        TEST_ERROR(("%s:%d: Can't parse --ns-dist value in order to get ranks for namespace %s", my_nspace, my_rank, rngs[i].nspace));
                        PMIX_RANGE_FREE(rngs, nranges);
                        PMIX_LIST_DESTRUCT(&test_fences);
                        return PMIX_ERROR;
                    }
                }
                for (j = 0; j < rngs[i].nranks; j++) {
                    put_ind = 0;
                    snprintf(sval, 50, "%s:%d", rngs[i].nspace, rngs[i].ranks[j]);
                    GET(string, sval, rngs[i].nspace, rngs[i].ranks[j], fence_num, put_ind++);
                    if (PMIX_SUCCESS != rc) {
                        TEST_ERROR(("%s:%d: PMIx_Get failed (%d) from %s:%d", my_nspace, my_rank, rc, rngs[i].nspace, rngs[i].ranks[j]));
                        PMIX_RANGE_FREE(rngs, nranges);
                        PMIX_LIST_DESTRUCT(&test_fences);
                        return rc;
                    }
                    GET(int, rngs[i].ranks[j], rngs[i].nspace, rngs[i].ranks[j], fence_num, put_ind++);
                    if (PMIX_SUCCESS != rc) {
                        TEST_ERROR(("%s:%d: PMIx_Get failed (%d) from %s:%d", my_nspace, my_rank, rc, rngs[i].nspace, rngs[i].ranks[j]));
                        PMIX_RANGE_FREE(rngs, nranges);
                        PMIX_LIST_DESTRUCT(&test_fences);
                        return rc;
                    }
                    GET(float, 1.1, rngs[i].nspace, rngs[i].ranks[j], fence_num, put_ind++);
                    if (PMIX_SUCCESS != rc) {
                        TEST_ERROR(("%s:%d: PMIx_Get failed (%d) from %s:%d", my_nspace, my_rank, rc, rngs[i].nspace, rngs[i].ranks[j]));
                        PMIX_RANGE_FREE(rngs, nranges);
                        PMIX_LIST_DESTRUCT(&test_fences);
                        return rc;
                    }
                    GET(uint32_t, 14, rngs[i].nspace, rngs[i].ranks[j], fence_num, put_ind++);
                    if (PMIX_SUCCESS != rc) {
                        TEST_ERROR(("%s:%d: PMIx_Get failed (%d) from %s:%d", my_nspace, my_rank, rc, rngs[i].nspace, rngs[i].ranks[j]));
                        PMIX_RANGE_FREE(rngs, nranges);
                        PMIX_LIST_DESTRUCT(&test_fences);
                        return rc;
                    }
                    GET(uint16_t, 15, rngs[i].nspace, rngs[i].ranks[j], fence_num, put_ind++);
                    if (PMIX_SUCCESS != rc) {
                        TEST_ERROR(("%s:%d: PMIx_Get failed (%d) from %s:%d", my_nspace, my_rank, rc, rngs[i].nspace, rngs[i].ranks[j]));
                        PMIX_RANGE_FREE(rngs, nranges);
                        PMIX_LIST_DESTRUCT(&test_fences);
                        return rc;
                    }
                }
            }
        }
        PMIX_RANGE_FREE(rngs, nranges);
        fence_num++;
    }
    PMIX_LIST_DESTRUCT(&test_fences);
    return PMIX_SUCCESS;
}

static int get_local_peers(char *my_nspace, int my_rank, int **_peers, int *count)
{
    pmix_value_t *val;
    int *peers = NULL;
    char *sptr, *token, *eptr, *str;
    int npeers;
    int rc;

    /* get number of neighbours on this node */
    if (PMIX_SUCCESS != (rc = PMIx_Get(my_nspace, my_rank, PMIX_LOCAL_SIZE, &val))) {
        TEST_ERROR(("%s:%d: PMIx_Get local peer # failed: %d", my_nspace, my_rank, rc));
        return rc;
    }
    if (NULL == val) {
        TEST_ERROR(("%s:%d: PMIx_Get local peer # returned NULL value", my_nspace, my_rank));
        return PMIX_ERROR;
    }

    if (val->type != PMIX_UINT32  ) {
        TEST_ERROR(("%s:%d: local peer # attribute value type mismatch,"
                " want %d get %d(%d)",
                my_nspace, my_rank, PMIX_UINT32, val->type));
        return PMIX_ERROR;
    }
    npeers = val->data.uint32;
    peers = malloc(sizeof(int) * npeers);

    /* get ranks of neighbours on this node */
    if (PMIX_SUCCESS != (rc = PMIx_Get(my_nspace, my_rank, PMIX_LOCAL_PEERS, &val))) {
        TEST_ERROR(("%s:%d: PMIx_Get local peers failed: %d", my_nspace, my_rank, rc));
        return rc;
    }
    if (NULL == val) {
        TEST_ERROR(("%s:%d: PMIx_Get local peers returned NULL value", my_nspace, my_rank));
        return PMIX_ERROR;
    }

    if (val->type != PMIX_STRING  ) {
        TEST_ERROR(("%s:%d: local peers attribute value type mismatch,"
                " want %d get %d(%d)",
                my_nspace, my_rank, PMIX_UINT32, val->type));
        return PMIX_ERROR;
    }

    *count = 0;
    sptr = NULL;
    str = val->data.string;
    do{
        if( *count > npeers ){
            TEST_ERROR(("%s:%d: Bad peer ranks number: should be %d, actual %d (%s)",
                my_nspace, my_rank, npeers, *count, val->data.string));
            return PMIX_ERROR;
        }
        token = strtok_r(str, ",", &sptr);
        str = NULL;
        if( NULL != token ){
            peers[(*count)++] = strtol(token,&eptr,10);
            if( *eptr != '\0' ){
                TEST_ERROR(("%s:%d: Bad peer ranks string", my_nspace, my_rank));
                return PMIX_ERROR;
            }
        }

    } while( NULL != token );

    if( *count != npeers ){
        TEST_ERROR(("%s:%d: Bad peer ranks number: should be %d, actual %d (%s)",
                my_nspace, my_rank, npeers, *count, val->data.string));
        return PMIX_ERROR;
    }
    *_peers = peers;
    return PMIX_SUCCESS;
}

int test_job_fence(test_params params, char *my_nspace, int my_rank)
{
    int rc;
    int i, j;
    char sval[50];
    int *peers, npeers;
    pmix_value_t value;
    pmix_value_t *val = &value;
    for (i=0; i < 3; i++) {
        PUT(int, 12340 + i, PMIX_LOCAL, 0, i);
        if (PMIX_SUCCESS != rc) {
            TEST_ERROR(("%s:%d: PMIx_Put failed: %d", my_nspace, my_rank, rc));
            return rc;
        }

        (void)snprintf(sval, 50, "%s:%d", my_nspace, my_rank);
        PUT(string, sval, PMIX_REMOTE, 1, i);
        if (PMIX_SUCCESS != rc) {
            TEST_ERROR(("%s:%d: PMIx_Put failed: %d", my_nspace, my_rank, rc));
            return PMIX_ERROR;
        }

        PUT(float, (float)12.15 + i, PMIX_GLOBAL, 2, i);
        if (PMIX_SUCCESS != rc) {
            TEST_ERROR(("%s:%d: PMIx_Put failed: %d", my_nspace, my_rank, rc));
            return PMIX_ERROR;
        }
    }

    /* Submit the data */
    if (PMIX_SUCCESS != (rc = PMIx_Commit())) {
        TEST_ERROR(("%s:%d: PMIx_Commit failed: %d", my_nspace, my_rank, rc));
        return PMIX_ERROR;
    }

    /* Perform a fence if was requested */
    FENCE(!params.nonblocking, params.collect, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        TEST_ERROR(("%s:%d: PMIx_Fence failed: %d", my_nspace, my_rank, rc));
        return rc;
    }

    if (PMIX_SUCCESS != (rc = get_local_peers(my_nspace, my_rank, &peers, &npeers))) {
        return PMIX_ERROR;
    }

    /* Check the predefined output */
    for (i=0; i < (int)params.ns_size; i++) {

        for (j=0; j < 3; j++) {

            int local = 0, k;
            for(k=0; k<npeers; k++){
                if( peers[k] == i+params.base_rank){
                    local = 1;
                }
            }
            if( local ){
                GET(int, (12340+j), my_nspace, i+params.base_rank, 0, j);
                if (PMIX_SUCCESS != rc) {
                    TEST_ERROR(("%s:%d: PMIx_Get failed: %d", my_nspace, my_rank, rc));
                    return PMIX_ERROR;
                }
            }

            snprintf(sval, 50, "%s:%d", my_nspace, i+params.base_rank);
            GET(string, sval, my_nspace, i+params.base_rank, 1, j);
            if (PMIX_SUCCESS != rc) {
                TEST_ERROR(("%s:%d: PMIx_Get failed (%d)", my_nspace, my_rank, rc));
                return PMIX_ERROR;
            }

            GET(float, (float)12.15 + j, my_nspace, i+params.base_rank, 2, j);
            if (PMIX_SUCCESS != rc) {
                TEST_ERROR(("%s:%d: PMIx_Get failed (%d)", my_nspace, my_rank, rc));
                return PMIX_ERROR;
            }
        }

        /* ask for a non-existent key */
        if (PMIX_SUCCESS == (rc = PMIx_Get(my_nspace, i+params.base_rank, "foobar", &val))) {
            TEST_ERROR(("%s:%d: PMIx_Get returned success instead of failure",
                        my_nspace, my_rank));
            return PMIX_ERROR;
        }
        if (PMIX_ERR_NOT_FOUND != rc) {
            TEST_ERROR(("rank %d [ERROR]: PMIx_Get returned %d instead of not_found",
                        my_nspace, my_rank, rc));
        }
        if (NULL != val) {
            TEST_ERROR(("rank %d [ERROR]: PMIx_Get did not return NULL value", my_nspace, my_rank));
            return PMIX_ERROR;
        }
        TEST_VERBOSE(("%s:%d: rank %d is OK", my_nspace, my_rank, i+params.base_rank));
    }
    return PMIX_SUCCESS;
}

