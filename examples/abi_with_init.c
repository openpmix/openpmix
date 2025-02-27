/*
 * Copyright (c) 2022      IBM Corporation.  All rights reserved.
 * Copyright (c) 2025      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Test PMIx_Query_info with a locally resolved key (PMIX_QUERY_ABI_VERSION)
 * and a key that the server will need to resolve.
 */

#include <stdio.h>
#include <stdlib.h>

#include <pmix.h>
#include "examples.h"

static void cbfunc(pmix_status_t status, pmix_info_t info[], size_t ninfo, void *cbdata,
                   pmix_release_cbfunc_t release_fn, void *release_cbdata)
{
    myquery_data_t *q = (myquery_data_t*)cbdata;
    size_t n;

    q->status = status;
    if (0 < ninfo) {
        q->ninfo = ninfo;
        PMIX_INFO_CREATE(q->info, q->ninfo);
        for (n=0; n < ninfo; n++) {
            PMIX_INFO_XFER(&q->info[n], &info[n]);
        }
    }
    if (NULL != release_fn) {
        release_fn(release_cbdata);
    }
    DEBUG_WAKEUP_THREAD(&q->lock);
}

int main(int argc, char **argv) {
    int rc;
    size_t i;
    size_t ninfo, nqueries;
    pmix_info_t *info = NULL;
    pmix_query_t *query = NULL;
    static pmix_proc_t myproc;
    myquery_data_t q;
    EXAMPLES_HIDE_UNUSED_PARAMS(argc, argv);

    if (PMIX_SUCCESS != (rc = PMIx_Init(&myproc, NULL, 0))) {
        if (PMIX_ERR_UNREACH != rc) {
            fprintf(stderr, "PMIx_Init failed: %d\n", rc);
            exit(rc);
        }
    }

    nqueries = 3;

    PMIX_QUERY_CREATE(query, nqueries);
    PMIX_ARGV_APPEND(rc, query[0].keys, PMIX_QUERY_STABLE_ABI_VERSION);
    PMIX_ARGV_APPEND(rc, query[1].keys, PMIX_QUERY_PROVISIONAL_ABI_VERSION);
    PMIX_ARGV_APPEND(rc, query[2].keys, PMIX_QUERY_NAMESPACES);

    rc = PMIx_Query_info(query, nqueries, &info, &ninfo);
    if (PMIX_SUCCESS != rc ) {
        fprintf(stderr, "Error: PMIx_Query_info failed: %d (%s)\n", rc, PMIx_Error_string(rc));
        return rc;
    }

    printf("--> Query returned (ninfo %d)\n", (int)ninfo);
    for(i = 0; i < ninfo; ++i) {
        printf("--> KEY: %s\n", info[i].key);
        if (PMIX_CHECK_KEY(&info[i], PMIX_QUERY_STABLE_ABI_VERSION)) {
            printf("----> ABI (Stable): String: %s\n",
                   info[i].value.data.string);
        }
        else if (PMIX_CHECK_KEY(&info[i], PMIX_QUERY_PROVISIONAL_ABI_VERSION)) {
            printf("----> ABI (Provisional): String: %s\n",
                   info[i].value.data.string);
        }
        else if (PMIX_CHECK_KEY(&info[i], PMIX_QUERY_NAMESPACES)) {
            printf("----> Namespaces: String: %s\n",
                   info[i].value.data.string);
        }
    }

    // now do it with the non-blocking form
    DEBUG_CONSTRUCT_MYQUERY(&q);
    rc = PMIx_Query_info_nb(query, nqueries, cbfunc, &q);
    if (PMIX_SUCCESS != rc ) {
        fprintf(stderr, "Error: PMIx_Query_info_nb failed: %d (%s)\n", rc, PMIx_Error_string(rc));
        return rc;
    }
    DEBUG_WAIT_THREAD(&q.lock);

    printf("--> Query_nb returned %s (ninfo %d)\n", PMIx_Error_string(q.status), (int)q.ninfo);
    for(i = 0; i < q.ninfo; ++i) {
        printf("--> KEY: %s\n", q.info[i].key);
        if (PMIX_CHECK_KEY(&q.info[i], PMIX_QUERY_STABLE_ABI_VERSION)) {
            printf("----> ABI (Stable): String: %s\n",
                   q.info[i].value.data.string);
        }
        else if (PMIX_CHECK_KEY(&q.info[i], PMIX_QUERY_PROVISIONAL_ABI_VERSION)) {
            printf("----> ABI (Provisional): String: %s\n",
                   q.info[i].value.data.string);
        }
        else if (PMIX_CHECK_KEY(&q.info[i], PMIX_QUERY_NAMESPACES)) {
            printf("----> Namespaces: String: %s\n",
                   q.info[i].value.data.string);
        }
    }


    /*
     * Cleanup
     */
    PMIX_INFO_FREE(info, ninfo);
    PMIX_QUERY_FREE(query, nqueries);

    rc = PMIx_Finalize(NULL, 0);

    return rc;
}
