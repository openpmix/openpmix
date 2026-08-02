
#include "include/pmix.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <ctype.h>

static void hide_unused_params(int x, ...)
{
    va_list ap;
    (void)x;
    va_start(ap, x);
    va_end(ap);
}

/* these were assert()s, which a build that defines NDEBUG compiles out
 * entirely - the calls then had their status silently discarded */
#define CHECK_RC(fn)                                                 \
    do {                                                             \
        if (PMIX_SUCCESS != rc) {                                    \
            fprintf(stderr, "%s failed: %s\n", (fn),                 \
                    PMIx_Error_string(rc));                          \
            exit(1);                                                 \
        }                                                            \
    } while (0)

int main(int argc, char **argv)
{
    pmix_proc_t myproc;
    pmix_status_t rc=0;
    int rank;
    hide_unused_params(rc, argc, argv);

    rc = PMIx_Init(&myproc, NULL, 0);
    CHECK_RC("PMIx_Init");

    /* PMIx_Init already handed us our identity. This used to ask for
     * it back with PMIx_Get(&myproc, PMIX_RANK), which cannot work and
     * is not meant to: naming the proc to ask about requires supplying
     * the very rank being asked for. PMIX_RANK inside a proc-data array
     * identifies which process the array describes, so it is consumed
     * as the index rather than stored as a retrievable key. The library
     * does serve the one form of the question that carries information
     * - "what is my own rank", asked with PMIX_RANK_INVALID in the rank
     * field - but there is nothing here that needs it. */
    rank = myproc.rank;

    if (rank == 0) {
        pmix_info_t *info;
        PMIX_INFO_CREATE(info, 1);
        snprintf(info[0].key, PMIX_MAX_KEYLEN, "magic-found");
        info[0].value.type = PMIX_STRING;
        info[0].value.data.string = "yes";
        rc = PMIx_Publish(info, 1);
        CHECK_RC("PMIx_Publish");
    }

    printf("I am rank %d\n", rank);

    {
        bool flag;
        pmix_info_t *info;
        PMIX_INFO_CREATE(info, 1);
        flag = true;
        PMIX_INFO_LOAD(info, PMIX_COLLECT_DATA, &flag, PMIX_BOOL);
        rc = PMIx_Fence(&myproc, 1, info, 1);
        CHECK_RC("PMIx_Fence");
        PMIX_INFO_FREE(info, 1);
    }

    if (rank == 1) {
        int i;
        pmix_pdata_t *pdata;
        PMIX_PDATA_CREATE(pdata, 2);
        snprintf(pdata[0].key, PMIX_MAX_KEYLEN, "magic-found");
        snprintf(pdata[1].key, PMIX_MAX_KEYLEN, "magic-not-found");
        rc = PMIx_Lookup(&pdata[0], 2, NULL, 0);
        if (PMIX_SUCCESS != rc && PMIX_ERR_NOT_FOUND != rc) {
            fprintf(stderr, "PMIx_Lookup failed: %s\n", PMIx_Error_string(rc));
            exit(1);
        }
        for (i = 0; i < 2; i++)
            if (pdata[i].value.type == PMIX_STRING)
                printf("Found[%d] %d %s\n", i, pdata[i].value.type, pdata[i].value.data.string);
            else
                printf("Found[%d] %d\n", i, pdata[i].value.type);
        /* two were created, so two have to be released */
        PMIX_PDATA_FREE(pdata, 2);
    }

    rc = PMIx_Finalize(NULL, 0);
    CHECK_RC("PMIx_Finalize");
    return 0;
}
