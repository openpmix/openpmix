#include <pmix.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void fence_collect(const char *nspace)
{
    pmix_proc_t wildcard;
    PMIx_Proc_load(&wildcard, nspace, PMIX_RANK_WILDCARD);

    pmix_info_t info;
    bool collect = true;
    PMIx_Info_load(&info, PMIX_COLLECT_DATA, &collect, PMIX_BOOL);

    PMIx_Fence(&wildcard, 1, &info, 1);
    PMIx_Info_destruct(&info);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    pmix_proc_t me;
    if (PMIx_Init(&me, NULL, 0) != PMIX_SUCCESS) {
        fprintf(stderr, "PMIx_Init failed\n");
        return 1;
    }

    printf("PMIx client initialized: nspace=%s rank=%u\n", me.nspace, me.rank);

    const char *key = "mykey";
    pmix_proc_t p0;
    PMIx_Proc_load(&p0, me.nspace, 0);

    // Round 0: Rank 0 puts "A"
    if (me.rank == 0) {
        pmix_value_t v;
        PMIx_Value_load(&v, "A", PMIX_STRING);
        PMIx_Put(PMIX_GLOBAL, key, &v);
        PMIx_Commit();
        PMIx_Value_destruct(&v);
        printf("rank0: PUT 'A'\n");
    }
    fence_collect(me.nspace);

    // Rank 1 reads and caches 'A'
    if (me.rank == 1) {
        pmix_value_t *out = NULL;
        pmix_status_t rc = PMIx_Get(&p0, key, NULL, 0, &out);
        printf("rank1: read1 -> rc=%d val=%s (now cached)\n",
               rc, (out && out->type == PMIX_STRING) ? out->data.string : "NULL");
        if (out) PMIx_Value_free(out, 1);
    }
    fence_collect(me.nspace);

    // Round 1: Rank 0 overwrites with "B"
    if (me.rank == 0) {
        pmix_value_t v;
        PMIx_Value_load(&v, "B", PMIX_STRING);
        PMIx_Put(PMIX_GLOBAL, key, &v);
        PMIx_Commit();
        PMIx_Value_destruct(&v);
        printf("rank0: PUT 'B' (overwrite)\n");
    }
    fence_collect(me.nspace);

    // Rank 1: Try to get fresh value with REFRESH
    if (me.rank == 1) {
        pmix_info_t info[2];
        bool refresh = true;
        bool immediate = true;

        PMIx_Info_load(&info[0], PMIX_GET_REFRESH_CACHE, &refresh, PMIX_BOOL);
        PMIx_Info_load(&info[1], PMIX_IMMEDIATE, &immediate, PMIX_BOOL);

        pmix_value_t *out = NULL;
        pmix_status_t rc = PMIx_Get(&p0, key, info, 2, &out);

        printf("rank1: read2 (REFRESH + IMMEDIATE) -> rc=%d val=%s (expect 'B')\n",
               rc, (out && out->type == PMIX_STRING) ? out->data.string : "NULL");
        if (out) PMIx_Value_free(out, 1);

        PMIx_Info_destruct(&info[0]);
        PMIx_Info_destruct(&info[1]);
    }

    PMIx_Finalize(NULL, 0);
    return 0;
}

