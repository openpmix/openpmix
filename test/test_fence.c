#include "test_fence.h"

int test_fence(test_params params, int my_nspace, int my_rank)
{
    int i = 0;
    int len;
    fence_desc_t *desc;
    nspace_desc_t *ndesc;
    rank_desc_t *rdesc;
    PMIX_CONSTRUCT(&test_fences, pmix_list_t);
    parse_fence(params.fences, 1);
    PMIX_LIST_FOREACH(desc, &test_fences, fence_desc_t) {
        char tmp[256] = {0};
        len = sprintf(tmp, "fence %d: block = %d de = %d ", i, desc->blocking, desc->data_exchange);
        i++;
        PMIX_LIST_FOREACH(ndesc, &(desc->nspaces), nspace_desc_t) {
            len += sprintf(tmp+len, "ns %d ranks: ", ndesc->id);
            PMIX_LIST_FOREACH(rdesc, &(ndesc->ranks), rank_desc_t) {
                len += sprintf(tmp+len, "%d,", rdesc->rank);
            }
        }
        fprintf(stderr,"tmp = %s\n",  tmp);
    }
    PMIX_LIST_DESTRUCT(&test_fences);
    return 0;
}
